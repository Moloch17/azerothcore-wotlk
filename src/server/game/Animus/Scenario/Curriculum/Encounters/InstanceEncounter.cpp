/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Encounters.h"
#include "BotFactory.h"
#include "CellImpl.h"
#include "CombatReward.h"
#include "CombatRewardScenario.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "InstanceBosses.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "EncoderSupport.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "StageScenario.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace
{
    /// How far from its spawn a boss is looked for by entry.
    constexpr float BOSS_SEARCH_YARDS = 100.0f;
    /// Legs of the entrance-to-boss path: PathGenerator stops at MAX_POINT_PATH_LENGTH points (~296 yd), and the
    /// deepest boss of a raid is a few of those from the door.
    constexpr uint32 PATH_LEGS = 16;
    /// The party's spawn rows, as StageScenario lays them out.
    constexpr float ROW_SPACING = 3.0f;
    /// How far from the boss a creature has to be to count as in the fight (UpdateEnemies).
    constexpr float FIGHT_RADIUS = 80.0f;
    /// A boss back at full health out of combat after having been engaged has evaded: the fight is lost.
    constexpr float EVADED_HEALTH_PCT = 99.0f;
    /// Trash cleared for the episode stays away this long (it respawns for the next instance anyway).
    constexpr Seconds TRASH_RESPAWN = Seconds(3600);

    struct EngageKey
    {
        uint32 Map;
        uint32 Entry;
        bool operator<(EngageKey const& other) const { return std::tie(Map, Entry) < std::tie(other.Map, other.Entry); }
    };

    /// Where each boss's raid stands, once per boss per run: the path is the same for every env.
    std::map<EngageKey, Position> engagePoints;

    float Distance2d(Position const& a, Position const& b)
    {
        float const dx = a.GetPositionX() - b.GetPositionX();
        float const dy = a.GetPositionY() - b.GetPositionY();
        return std::sqrt(dx * dx + dy * dy);
    }
}

Animus::Curriculum::InstanceEncounter::InstanceEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "boss")
{
    // The rows the world database can actually field, per ladder: a wrong entry or a boss with no spawn is a log
    // line at startup, not a crash in the first episode.
    for (ArenaDefinition const& arena : scenario.Stage().Arenas)
    {
        if (arena.Instance == InstanceLadder::None || _rows.contains(arena.Instance))
            continue;

        std::vector<BossRow const*>& rows = _rows[arena.Instance];
        for (BossRow const& row : InstanceLadderRows(arena.Instance))
        {
            if (!sObjectMgr->GetCreatureTemplate(row.Entry))
            {
                LOG_ERROR("module.animus", "{}: no creature template {} for {} (map {}); rung dropped",
                    scenario.Name(), row.Entry, row.Name, row.MapId);
                continue;
            }
            if (!FindSpawn(row))
            {
                LOG_ERROR("module.animus", "{}: {} ({}) has no spawn on map {}; rung dropped", scenario.Name(),
                    row.Name, row.Entry, row.MapId);
                continue;
            }
            if (!sObjectMgr->GetMapEntranceTrigger(row.MapId))
            {
                LOG_ERROR("module.animus", "{}: map {} has no entrance trigger; {} dropped", scenario.Name(),
                    row.MapId, row.Name);
                continue;
            }
            rows.push_back(&row);
        }

        if (rows.empty())
            LOG_ERROR("module.animus", "{}: arena {} has no boss its world database can field", scenario.Name(),
                arena.Name);
    }
}

CreatureData const* Animus::Curriculum::InstanceEncounter::FindSpawn(BossRow const& row)
{
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        if (data.mapid == row.MapId && data.id == row.Entry)
            return &data;

    return nullptr;
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::InstanceEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::StealthUtility, RewardTerm::Kill,
        RewardTerm::HealthKept, RewardTerm::Death, RewardTerm::BossProgress, RewardTerm::Timeout, RewardTerm::Stall,
        RewardTerm::Readiness };
}

void Animus::Curriculum::InstanceEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // The rung (the ladder's tier, which the evaluation spreads its seeds over) and the fight's outcome. The rung
    // is `difficulty` -- the column the convergence rule's ladder signal reads -- unless the stage has a creature
    // duel, which reports its own tier under that name (the crossroads); `boss_rung` is always there.
    auto const creature = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Creature; };
    if (!_scenario.Stage().AnyArena(creature))
        table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add("boss_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add("instance_map", [this](Env const& env, uint32) { return float(_envs[env.Index].MapId); });
    table.Add("boss_entry", [this](Env const& env, uint32) { return float(_envs[env.Index].Entry); });
    table.Add("boss_killed", [this](Env const& env, uint32) { return _envs[env.Index].BossDead ? 1.0f : 0.0f; });
    table.Add("boss_health_left", [this](Env const& env, uint32) { return _envs[env.Index].HealthLeft; });
    table.Add("engaged", [this](Env const& env, uint32) { return _envs[env.Index].Engaged ? 1.0f : 0.0f; });
    table.Add("wiped", [this](Env const& env, uint32) { return _envs[env.Index].Wiped ? 1.0f : 0.0f; });
    table.Add("evaded", [this](Env const& env, uint32) { return _envs[env.Index].Evaded ? 1.0f : 0.0f; });
    if (_scenario.Stage().AnyArena([](ArenaDefinition const& arena) { return arena.Instance == InstanceLadder::Wing; }))
    {
        table.Add("wing_trash_kills", [this](Env const& env, uint32) { return float(_envs[env.Index].TrashKills); });
        table.Add("wing_boss_kills", [this](Env const& env, uint32) { return float(_envs[env.Index].BossKills); });
        table.Add("wing_route_share", [this](Env const& env, uint32)
        {
            EnvInstance const& fight = _envs[env.Index];
            return fight.Route.empty() ? 0.0f : float(std::min<std::size_t>(fight.RouteNext, fight.Route.size()))
                / float(fight.Route.size());
        });
        table.Add("wing_wipes", [this](Env const& env, uint32) { return float(_envs[env.Index].Wipes); });
    }
}

void Animus::Curriculum::InstanceEncounter::ResetEpisode(Env& env)
{
    EnvInstance& fight = _envs[env.Index];
    fight = EnvInstance();
}

void Animus::Curriculum::InstanceEncounter::BeforeLevel(Env& env)
{
    // The rung: the class and build of seat 0 climb the ladder as they win (every seat of a per-class run is that
    // class); an evaluation spreads its seeds over every rung. The rung fixes the map, the level and the difficulty
    // the seats are built for, which is why this runs before the level is drawn.
    EnvState& data = _scenario.Data(env);
    EnvInstance& fight = _envs[env.Index];
    std::vector<BossRow const*> const& rows = Rows(env);
    if (rows.empty())
        return;

    SeatState const& seat = data.Seats[0];
    fight.Layout = seat.L ? seat.L->Index : 0;
    fight.Spec = seat.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, fight.Layout, fight.Spec, uint32(rows.size()) - 1);
    fight.Tier = std::min<uint32>(pick.Tier, uint32(rows.size()) - 1);
    fight.Counts = pick.Counts;
    fight.Row = rows[fight.Tier];
    fight.MapId = fight.Row->MapId;
    fight.Entry = fight.Row->Entry;

    data.EpisodeMapId = fight.Row->MapId;
    data.HasEpisodeMap = true;
    data.EpisodeLevel = fight.Row->Level;
    MapEntry const* mapEntry = sMapStore.LookupEntry(fight.Row->MapId);
    bool const raid = mapEntry && mapEntry->IsRaid();
    data.DungeonDifficulty = raid ? 0 : fight.Row->Difficulty;
    data.RaidDifficulty = raid ? fight.Row->Difficulty : 0;

    // The seats spawn at the instance's front door, as a group that walked in would; Build then takes them to the
    // boss along the server's own path.
    // A map with several ways in (Scarlet Monastery's wings) names the one for this boss: the map's first trigger is
    // one wing's door for all of them, and the path from it to a boss in another wing does not exist.
    AreaTriggerTeleport const* entrance = fight.Row->Entrance
        ? sObjectMgr->GetAreaTriggerTeleport(fight.Row->Entrance) : nullptr;
    if (!entrance || entrance->target_mapId != fight.Row->MapId)
        entrance = sObjectMgr->GetMapEntranceTrigger(fight.Row->MapId);
    data.EpisodeSpawn.Relocate(entrance->target_X, entrance->target_Y, entrance->target_Z, entrance->target_Orientation);
    data.HasEpisodeSpawn = true;
}

std::vector<Animus::Curriculum::BossRow const*> const& Animus::Curriculum::InstanceEncounter::Rows(
    Env const& env) const
{
    static std::vector<BossRow const*> const none;
    auto const rows = _rows.find(_scenario.Arena(env).Instance);
    return rows == _rows.end() ? none : rows->second;
}

Creature* Animus::Curriculum::InstanceEncounter::FindBoss(Map* map, BossRow const& row, WorldObject const* anchor) const
{
    CreatureData const* spawn = FindSpawn(row);
    if (!spawn || !map)
        return nullptr;

    // The boss's grid is loaded on purpose: nothing has walked there yet, and an unloaded grid holds no creature.
    map->LoadGrid(spawn->posX, spawn->posY);
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != row.MapId || data.id != row.Entry)
            continue;
        auto const range = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second)
                return it->second;
    }

    // Not in the spawn-id store: the cells around its spawn, by entry (a boss its script re-summons, or one the
    // grid holds under another spawn).
    Position const at(spawn->posX, spawn->posY, spawn->posZ);
    std::list<Creature*> found;
    auto const check = [&row](Creature* creature) { return creature->GetEntry() == row.Entry; };
    Acore::CreatureListSearcher<decltype(check)> searcher(anchor, found, check);
    Cell::VisitObjects(spawn->posX, spawn->posY, map, searcher, BOSS_SEARCH_YARDS);
    for (Creature* creature : found)
        if (creature && creature->GetExactDist2d(&at) <= BOSS_SEARCH_YARDS)
            return creature;

    // Gone: the core's dynamic respawn removes a dead creature outright and brings it back on its own clock, which
    // the sim does not wait for. Its row is loaded again, alive, the way the grid loaded it the first time.
    ObjectGuid::LowType spawnId = 0;
    for (auto const& [id, data] : sObjectMgr->GetAllCreatureData())
        if (&data == spawn)
        {
            spawnId = id;
            break;
        }
    if (spawnId)
    {
        map->RemoveRespawnTime(SPAWN_TYPE_CREATURE, spawnId);
        Creature* creature = new Creature();
        if (creature->LoadCreatureFromDB(spawnId, map, true, false))
        {
            LOG_INFO("module.animus", "{}: {} ({}) reloaded into instance {} of map {}", _scenario.Name(), row.Name,
                row.Entry, map->GetInstanceId(), map->GetId());
            return creature;
        }
        delete creature;
    }

    return nullptr;
}

Position Animus::Curriculum::InstanceEncounter::EngagePoint(Env const& env, Map* map, Player* seat,
    Creature* boss) const
{
    BossRow const& row = *_envs[env.Index].Row;
    EngageKey const key{ row.MapId, row.Entry };
    if (auto const known = engagePoints.find(key); known != engagePoints.end())
        return known->second;

    float const engageYards = float(_scenario.Tuning().Instance.EngageYards);
    Position const goal(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ(), boss->GetOrientation());

    // The server's own path from the door to the boss, in as many legs as PathGenerator's point cap needs, with
    // the grids along it loaded so the navmesh is there to walk. Then back up the path from the boss by
    // EngageYards: that is where a raid that came in the front stands, on its side of the trash it never pulled.
    std::vector<G3D::Vector3> points;
    Position cursor(seat->GetPositionX(), seat->GetPositionY(), seat->GetPositionZ());
    // On the ground: an entrance trigger's arrival point can hang above the floor by more than the navmesh
    // query tolerates.
    map->LoadGrid(cursor.GetPositionX(), cursor.GetPositionY());
    if (float const floor = map->GetHeight(seat->GetPhaseMask(), cursor.GetPositionX(), cursor.GetPositionY(),
        cursor.GetPositionZ() + 5.0f, true, 50.0f); floor > INVALID_HEIGHT)
        cursor.m_positionZ = floor;
    points.emplace_back(cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ());
    bool complete = false;
    PathType lastType = PATHFIND_BLANK;
    for (uint32 leg = 0; leg < PATH_LEGS && !complete; ++leg)
    {
        map->LoadGrid(cursor.GetPositionX(), cursor.GetPositionY());
        PathGenerator path(seat);
        path.CalculatePath(cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ(), goal.GetPositionX(),
            goal.GetPositionY(), goal.GetPositionZ(), false);
        PathType const type = path.GetPathType();
        lastType = type;
        if (type & PATHFIND_NOPATH || path.GetPath().size() < 2)
            break;

        for (std::size_t i = 1; i < path.GetPath().size(); ++i)
            points.push_back(path.GetPath()[i]);
        G3D::Vector3 const& end = path.GetPath().back();
        // No progress: the navmesh ends here (a door, a jump, a different floor).
        if (Distance2d(cursor, Position(end.x, end.y, end.z)) < 1.0f)
            break;
        cursor.Relocate(end.x, end.y, end.z);
        complete = !(type & (PATHFIND_INCOMPLETE | PATHFIND_SHORT));
    }

    Position engage;
    if (complete && points.size() >= 2)
    {
        float left = engageYards;
        std::size_t i = points.size() - 1;
        while (i > 0)
        {
            G3D::Vector3 const& a = points[i - 1];
            G3D::Vector3 const& b = points[i];
            float const segment = (b - a).length();
            if (segment >= left)
            {
                float const t = segment > 0.0f ? left / segment : 0.0f;
                G3D::Vector3 const at = b + (a - b) * t;
                engage.Relocate(at.x, at.y, at.z);
                break;
            }
            left -= segment;
            --i;
        }
        if (i == 0)
            engage.Relocate(points.front().x, points.front().y, points.front().z);
        LOG_INFO("module.animus", "{}: {} ({}) engaged from ({:.0f} {:.0f} {:.0f}), {} path points from the door",
            _scenario.Name(), row.Name, row.Entry, engage.GetPositionX(), engage.GetPositionY(),
            engage.GetPositionZ(), points.size());
    }
    else
    {
        // No path from the door (a boss reached through a teleporter or a locked door): stand EngageYards in front
        // of the boss along its spawn facing, which is the way bosses face their room's entrance.
        engage.Relocate(goal.GetPositionX() + std::cos(goal.GetOrientation()) * engageYards,
            goal.GetPositionY() + std::sin(goal.GetOrientation()) * engageYards, goal.GetPositionZ());
        map->LoadGrid(engage.GetPositionX(), engage.GetPositionY());
        engage.m_positionZ = map->GetHeight(seat->GetPhaseMask(), engage.GetPositionX(), engage.GetPositionY(),
            goal.GetPositionZ() + 5.0f, true, 50.0f);
        LOG_WARN("module.animus", "{}: no path from the door to {} ({}) on map {} (from ({:.0f} {:.0f} {:.0f}), {} "
            "points, last leg type {}); standing in front of it", _scenario.Name(), row.Name, row.Entry, row.MapId,
            seat->GetPositionX(), seat->GetPositionY(), seat->GetPositionZ(), points.size(), uint32(lastType));
    }
    engage.SetOrientation(engage.GetAngle(&goal));
    engagePoints[key] = engage;
    return engage;
}

bool Animus::Curriculum::InstanceEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvInstance& fight = _envs[env.Index];
    if (!fight.Row || !map)
        return false;

    Player* seat = _scenario.SeatBot(env, 0);
    Creature* boss = seat ? FindBoss(map, *fight.Row, seat) : nullptr;
    if (!seat || !boss)
    {
        LOG_ERROR("module.animus", "{}: env {}: {} ({}) is not in instance {} of map {}", _scenario.Name(), env.Index,
            fight.Row->Name, fight.Row->Entry, map->GetInstanceId(), map->GetId());
        return false;
    }

    // The boss as the raid should find it: alive, at home, its script's state cleared (doors and minions with it).
    if (!boss->IsAlive())
        boss->Respawn(true);
    if (boss->IsInCombat() && boss->IsAIEnabled)
        boss->AI()->EnterEvadeMode();
    boss->SetFullHealth();
    if (fight.Row->DataId >= 0)
        if (InstanceMap* instance = map->ToInstanceMap())
            if (InstanceScript* script = instance->GetInstanceScript())
                script->SetBossState(uint32(fight.Row->DataId), NOT_STARTED);
    fight.Boss = boss->GetGUID();
    fight.BossHealth = std::max<uint32>(1, boss->GetMaxHealth());

    EnvState& data = _scenario.Data(env);
    // A whole wing: the party stays at the door with the trash alive, and the route to the boss is its objective.
    if (Wing(env))
    {
        fight.Route = WingRoute(env, map, seat, boss);
        env.Targets.clear();
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index))
                _scenario.PrepareFighter(bot, _scenario.Data(env).Seats[index]);
        return true;
    }

    // Where the raid stands, then the raid: the seats in the rows StageScenario laid them out in at the door, and
    // the owner with them.
    Position const engage = EngagePoint(env, map, seat, boss);
    // The episode's home is the boss room from here on: the scripted owner holds it rather than walking back to the
    // door through the trash that was never pulled.
    data.EpisodeSpawn = engage;
    data.HasEpisodeSpawn = true;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        if (!bot)
            continue;
        Position at = engage;
        uint32 const inGroup = index % GROUP_SEATS;
        uint32 const group = index / GROUP_SEATS;
        at.m_positionX += (inGroup % 2 ? -ROW_SPACING : ROW_SPACING) * float(1 + inGroup / 2);
        at.m_positionY += (inGroup % 2 ? ROW_SPACING : -ROW_SPACING) - ROW_SPACING * 2.0f * float(group);
        BotFactory::TeleportWithinMap(bot, at);
    }
    if (Player* owner = _scenario.Owner(env))
    {
        Position at = engage;
        at.m_positionX += ROW_SPACING;
        BotFactory::TeleportWithinMap(owner, at);
    }

    // The trash between the door and the boss was never pulled; what stands around the boss goes too, except the
    // creatures that are the encounter (BossRow::Keep) and the boss itself. A Trash rung keeps them all.
    if (!fight.Row->Trash)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(boss, seat, float(_scenario.Tuning().Instance.TrashRadius));
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(boss, units, check);
        Cell::VisitObjects(boss, searcher, float(_scenario.Tuning().Instance.TrashRadius));
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || creature == boss || creature->IsSummon() || creature->IsPet())
                continue;
            if (std::find(fight.Row->Keep.begin(), fight.Row->Keep.end(), creature->GetEntry())
                != fight.Row->Keep.end())
                continue;
            creature->DespawnOrUnsummon(0ms, TRASH_RESPAWN);
            ++fight.TrashCleared;
        }
    }

    env.Targets = { fight.Boss };
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
        if (Player* bot = _scenario.SeatBot(env, index))
            _scenario.PrepareFighter(bot, _scenario.Data(env).Seats[index]);
    return true;
}

void Animus::Curriculum::InstanceEncounter::UpdateEnemies(Env& env)
{
    // The boss in slot 0, then the creatures in the fight nearest the seats: its adds and summons reach the pack
    // block's slots the way a pull's members do.
    EnvInstance& fight = _envs[env.Index];
    if (Wing(env))
    {
        UpdateWingEnemies(env, fight);
        return;
    }
    Player* seat = _scenario.SeatBot(env, 0);
    if (!seat || fight.Boss.IsEmpty())
        return;

    Creature* boss = Encoding::CreatureThrough(*seat, fight.Boss);
    env.Targets.assign(1, fight.Boss);
    if (!boss)
        return;

    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(boss, seat, FIGHT_RADIUS);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(boss, units, check);
    Cell::VisitObjects(boss, searcher, FIGHT_RADIUS);
    units.remove_if([boss](Unit* unit) { return unit == boss || !unit->IsInCombat() || unit->IsPlayer(); });
    units.sort([seat](Unit* a, Unit* b) { return seat->GetDistance(a) < seat->GetDistance(b); });
    for (Unit* unit : units)
    {
        if (env.Targets.size() >= PACK_SLOTS)
            break;
        env.Targets.push_back(unit->GetGUID());
    }
}

void Animus::Curriculum::InstanceEncounter::Update(Env& env)
{
    EnvInstance& fight = _envs[env.Index];
    Player* seat = _scenario.SeatBot(env, 0);
    Creature* boss = seat && !fight.Boss.IsEmpty() ? Encoding::CreatureThrough(*seat, fight.Boss) : nullptr;
    if (!boss)
        return;

    // The boss is the episode's pull. Announced on the first update, once the owner is configured (which clears its
    // engage timer): the scripted owner's timer is only ever set by a pull starting, so in an instance it attacked
    // the boss on its first update, before the party had moved. Now a tank owner pulls after a moment and any other
    // waits for the tank, as it does for a pull.
    if (!fight.Announced)
    {
        fight.Announced = true;
        _scenario.NotifyPullStarting(env);
    }

    fight.HealthLeft = boss->IsAlive() ? float(boss->GetHealth()) / float(fight.BossHealth) : 0.0f;
    if (!fight.BossDead && !boss->IsAlive())
        fight.BossDead = true;
    if (!fight.Engaged && boss->IsInCombat())
    {
        fight.Engaged = true;
        fight.EngageMs = env.EpisodeElapsedMs;
    }
    // Back at full health and out of combat after having been fought: the script evaded, and the fight is lost.
    if (fight.Engaged && !fight.BossDead && !boss->IsInCombat() && boss->GetHealthPct() >= EVADED_HEALTH_PCT
        && env.EpisodeElapsedMs > fight.EngageMs + _scenario.DecisionMs())
        fight.Evaded = true;

    // A wipe: no seat left standing. Nobody stands up in an instance; the dead wait for the episode to end.
    EnvState const& data = _scenario.Data(env);
    bool anyoneAlive = false;
    for (uint32 index = 0; index < data.ActiveSeats && !anyoneAlive; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive())
            anyoneAlive = true;
    if (!Wing(env))
    {
        if (!anyoneAlive && !fight.BossDead)
            fight.Wiped = true;
        return;
    }

    // A wing: the next point of the route reached by any seat; a wipe stands the party up at the door (until
    // Instance.WingWipes, which end it), with the trash that killed it still where it was.
    if (fight.RouteNext < fight.Route.size())
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive()
                && bot->GetExactDist2d(&fight.Route[fight.RouteNext]) <= 15.0f)
            {
                ++fight.RouteNext;
                break;
            }
    if (!anyoneAlive && !fight.BossDead)
    {
        ++fight.Wipes;
        if (fight.Wipes >= _scenario.Tuning().Instance.WingWipes)
        {
            fight.Wiped = true;
            return;
        }
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index))
            {
                bot->ResurrectPlayer(0.5f);
                bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA) / 2);
                BotFactory::TeleportWithinMap(bot, data.EpisodeSpawn);
                fight.Seats[index].DeathPaid = false;
            }
        if (Player* owner = _scenario.Owner(env); owner && !owner->IsAlive())
        {
            owner->ResurrectPlayer(0.5f);
            BotFactory::TeleportWithinMap(owner, data.EpisodeSpawn);
        }
    }
}

bool Animus::Curriculum::InstanceEncounter::Wing(Env const& env) const
{
    return _scenario.Arena(env).Instance == InstanceLadder::Wing;
}

std::vector<Position> Animus::Curriculum::InstanceEncounter::WingRoute(Env const& env, Map* map, Player* seat,
    Creature* boss) const
{
    static std::map<EngageKey, std::vector<Position>> routes;
    static std::mutex routesLock;
    BossRow const& row = *_envs[env.Index].Row;
    EngageKey const key{ row.MapId, row.Entry };
    {
        std::lock_guard<std::mutex> guard(routesLock);
        if (auto const known = routes.find(key); known != routes.end())
            return known->second;
    }

    // The server's path from the door to the boss, leg by leg as EngagePoint walks it, as points every
    // WingWaypointYards; the boss's own position last.
    std::vector<G3D::Vector3> points;
    Position cursor(seat->GetPositionX(), seat->GetPositionY(), seat->GetPositionZ());
    points.emplace_back(cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ());
    for (uint32 leg = 0; leg < PATH_LEGS; ++leg)
    {
        map->LoadGrid(cursor.GetPositionX(), cursor.GetPositionY());
        PathGenerator path(seat);
        path.CalculatePath(cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ(), boss->GetPositionX(),
            boss->GetPositionY(), boss->GetPositionZ(), false);
        if (path.GetPathType() & PATHFIND_NOPATH || path.GetPath().size() < 2)
            break;
        for (std::size_t i = 1; i < path.GetPath().size(); ++i)
            points.push_back(path.GetPath()[i]);
        G3D::Vector3 const& end = path.GetPath().back();
        if (Distance2d(cursor, Position(end.x, end.y, end.z)) < 1.0f)
            break;
        cursor.Relocate(end.x, end.y, end.z);
        if (!(path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORT)))
            break;
    }

    float const spacing = float(std::max<uint32>(5, _scenario.Tuning().Instance.WingWaypointYards));
    std::vector<Position> route;
    float walked = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i)
    {
        walked += (points[i] - points[i - 1]).length();
        if (walked >= spacing)
        {
            route.emplace_back(points[i].x, points[i].y, points[i].z);
            walked = 0.0f;
        }
    }
    route.emplace_back(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
    LOG_INFO("module.animus", "{}: the route to {} ({}) is {} points from the door", _scenario.Name(), row.Name,
        row.Entry, route.size());

    std::lock_guard<std::mutex> guard(routesLock);
    routes[key] = route;
    return route;
}

void Animus::Curriculum::InstanceEncounter::UpdateWingEnemies(Env& env, EnvInstance& fight)
{
    Player* seat = _scenario.SeatBot(env, 0);
    Creature* boss = seat && !fight.Boss.IsEmpty() ? Encoding::CreatureThrough(*seat, fight.Boss) : nullptr;
    if (!seat || !seat->IsInWorld())
        return;

    // The creatures watched last decision that have died since: the party's kills (the boss's is its own term).
    for (ObjectGuid const& guid : fight.Watched)
    {
        if (guid == fight.Boss || std::find(fight.Counted.begin(), fight.Counted.end(), guid) != fight.Counted.end())
            continue;
        Creature const* creature = Encoding::CreatureThrough(*seat, guid);
        if (creature && !creature->IsAlive())
        {
            fight.Counted.push_back(guid);
            ++fight.TrashKills;
            if (creature->IsDungeonBoss() || creature->isWorldBoss())
                ++fight.BossKills;
        }
    }

    // In the slots: what is fighting the party first, then the nearest of what stands ahead of it -- the next pack.
    constexpr float WING_SIGHT = 45.0f;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(seat, seat, WING_SIGHT);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(seat, units, check);
    Cell::VisitObjects(seat, searcher, WING_SIGHT);
    units.remove_if([](Unit* unit) { return !unit->IsAlive() || unit->IsPlayer() || unit->IsTotem(); });
    units.sort([seat](Unit* a, Unit* b)
    {
        if (a->IsInCombat() != b->IsInCombat())
            return a->IsInCombat();
        return seat->GetDistance(a) < seat->GetDistance(b);
    });
    env.Targets.clear();
    for (Unit* unit : units)
    {
        if (env.Targets.size() >= PACK_SLOTS)
            break;
        env.Targets.push_back(unit->GetGUID());
    }
    // The boss once it is in the fight, whatever else is.
    if (boss && boss->IsAlive() && boss->IsInCombat()
        && std::find(env.Targets.begin(), env.Targets.end(), fight.Boss) == env.Targets.end())
    {
        if (env.Targets.size() >= PACK_SLOTS)
            env.Targets.back() = fight.Boss;
        else
            env.Targets.push_back(fight.Boss);
    }
    for (ObjectGuid const& guid : env.Targets)
        if (std::find(fight.Watched.begin(), fight.Watched.end(), guid) == fight.Watched.end())
            fight.Watched.push_back(guid);
}

void Animus::Curriculum::InstanceEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    // A wing's route: the next point is where the party is going, a TravelTo target (GoalBlock's assignment slot).
    EnvInstance const& fight = _envs[env.Index];
    if (!Wing(env) || fight.Route.empty())
        return;
    view.HasObjective = true;
    view.Objective = fight.Route[std::min<std::size_t>(fight.RouteNext, fight.Route.size() - 1)];
}

void Animus::Curriculum::InstanceEncounter::RewardWing(Env& env, uint32 seatIndex, Player* bot,
    RewardLedger& ledger)
{
    EnvInstance& fight = _envs[env.Index];
    SeatInstance& paid = fight.Seats[seatIndex];
    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    float const tierScale = TierScale(env);
    uint32 const waypoints = std::min<uint32>(fight.RouteNext, uint32(fight.Route.size()));
    if (bot && bot->IsAlive())
    {
        ledger.Add(RewardTerm::Kill, tuning.WingTrashKill * tierScale * float(fight.TrashKills - paid.KillsPaid));
        ledger.Add(RewardTerm::Approach, tuning.WingWaypoint * tierScale * float(waypoints - paid.WaypointsPaid));
        ledger.Add(RewardTerm::Kill, tuning.WingMidBoss * tierScale * float(fight.BossKills - paid.BossKillsPaid));
    }
    paid.KillsPaid = fight.TrashKills;
    paid.BossKillsPaid = fight.BossKills;
    paid.WaypointsPaid = waypoints;
    if (bot && !bot->IsAlive() && !paid.DeathPaid)
    {
        paid.DeathPaid = true;
        ledger.Add(RewardTerm::Death, -tuning.WingDeath / tierScale);
    }
    if (fight.Wipes > paid.WipesPaid)
    {
        ledger.Add(RewardTerm::Death, -tuning.WingWipe * float(fight.Wipes - paid.WipesPaid) / tierScale);
        paid.WipesPaid = fight.Wipes;
    }

    bool const over = fight.BossDead || fight.Wiped || TimeIsUp(env);
    if (!over || paid.OutcomePaid)
        return;
    paid.OutcomePaid = true;
    if (seatIndex == 0 && !fight.Recorded)
    {
        fight.Recorded = true;
        if (fight.Counts)
            _ladder.Record(fight.Layout, fight.Spec, fight.Tier, fight.BossDead,
                uint32(std::max<std::size_t>(1, Rows(env).size())) - 1);
    }
    if (fight.BossDead)
        ledger.Add(RewardTerm::Kill, tuning.WingBoss * tierScale);
    else if (TimeIsUp(env) && !fight.Route.empty())
        ledger.Add(RewardTerm::Timeout, -tuning.Timeout * (1.0f - float(waypoints) / float(fight.Route.size()))
            / tierScale);
}

bool Animus::Curriculum::InstanceEncounter::SelectTarget(Env const& env, uint32 seatIndex, Unit*& target)
{
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (Unit* selected = env.FindTargetUnit(seat.TargetSlot); selected && selected->IsAlive())
    {
        target = selected;
        return true;
    }

    // The selection died or despawned: the boss while it lives, else the nearest living enemy.
    target = nullptr;
    Player* bot = env.FindBot(seatIndex);
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive())
            continue;
        if (!target || (bot && slot != 0 && bot->GetDistance(enemy) < bot->GetDistance(target)))
        {
            target = enemy;
            seat.TargetSlot = slot;
        }
        if (slot == 0)
            break;
    }
    return true;
}

float Animus::Curriculum::InstanceEncounter::TierScale(Env const& env) const
{
    CurriculumTuning const& tuning = _scenario.Tuning();
    return CombatReward::TierScale(tuning.Difficulty.TierScale,
        std::min<uint32>(_envs[env.Index].Tier, tuning.Instance.MaxTierScale));
}

bool Animus::Curriculum::InstanceEncounter::TimeIsUp(Env const& env)
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

void Animus::Curriculum::InstanceEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    if (Wing(env))
    {
        RewardWing(env, seatIndex, bot, ledger);
        return;
    }
    EnvInstance& fight = _envs[env.Index];
    Unit* boss = env.FindTargetUnit(0);
    float const tierScale = TierScale(env);

    // The one-on-one terms against the boss: damage as a share of its health (adds count, at the boss's scale),
    // damage taken, the approach, and the outcome once -- Kill and HealthKept to every seat alive when it dies,
    // Death once per seat, all scaled by the rung.
    CombatReward::OneOnOne(_scenario, env, seatIndex, bot, boss, ledger, tierScale);

    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    // Not engaged once the grace is gone: the clock costs by the second, as the single pack's stall does.
    if (!fight.Engaged && env.EpisodeElapsedMs > tuning.StallGraceMs)
        ledger.Add(RewardTerm::Stall, -tuning.Stall * float(_scenario.DecisionMs()) / 1000.0f);

    bool const over = fight.BossDead || fight.Wiped || fight.Evaded || TimeIsUp(env);
    if (!over || fight.Seats[seatIndex].OutcomePaid)
        return;
    fight.Seats[seatIndex].OutcomePaid = true;

    if (seatIndex == 0 && !fight.Recorded)
    {
        fight.Recorded = true;
        if (fight.Counts)
            _ladder.Record(fight.Layout, fight.Spec, fight.Tier, fight.BossDead,
                uint32(std::max<std::size_t>(1, Rows(env).size())) - 1);
    }

    if (fight.BossDead)
        return;

    // A lost fight is not one bit: what the raid took off the boss before it wiped or the script reset is paid as
    // progress, so a forty-seat fight has a gradient before its first kill. The clock costs what a duel's does.
    float const progress = std::clamp(1.0f - fight.HealthLeft, 0.0f, 1.0f);
    if (progress > 0.0f)
        ledger.Add(RewardTerm::BossProgress, tuning.BossProgress * progress * tierScale);
    if (!fight.Wiped && !fight.Evaded && TimeIsUp(env))
        ledger.Add(RewardTerm::Timeout, -tuning.Timeout
            * CombatReward::TimeoutScale(_scenario.Tuning().Duel.TimeoutFloor, fight.HealthLeft) / tierScale);
}

void Animus::Curriculum::InstanceEncounter::WriteState(Env const& env, float* state) const
{
    uint32 const top = uint32(std::max<std::size_t>(2, Rows(env).size())) - 1;
    state[StageScenario::STATE_TIER] = float(_envs[env.Index].Tier) / float(top);
}

bool Animus::Curriculum::InstanceEncounter::IsTerminal(Env const& env) const
{
    EnvInstance const& fight = _envs[env.Index];
    // A wing goes on past an evade (the party can pull the boss again) and ends on the kill, the last wipe or time.
    if (Wing(env))
        return fight.BossDead || fight.Wiped || TimeIsUp(env);
    return fight.BossDead || fight.Wiped || fight.Evaded || TimeIsUp(env);
}
