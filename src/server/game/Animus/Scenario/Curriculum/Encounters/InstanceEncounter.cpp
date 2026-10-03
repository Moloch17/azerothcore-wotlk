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
#include "FieldRoute.h"
#include "CellImpl.h"
#include "CombatReward.h"
#include "CombatRewardScenario.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CrowdBlock.h"
#include "DBCStores.h"
#include "Env.h"
#include "GameObject.h"
#include "EpisodeInfoTable.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "InstanceBosses.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "EncoderSupport.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "StageScenario.h"
#include "Supplies.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>

namespace
{
    /// A closed door this near a living seat opens (InstanceEncounter::UpdateWingEnemies).
    constexpr float DOOR_REACH = 15.0f;
    constexpr float OBJECT_SIGHT = 40.0f;      // the party sees what it can use this far (CrowdView::Object)
    /// Where a dungeon's navmesh does not join two stops, the route steps from creature to creature: to the nearest
    /// within BREADCRUMB_REACH that is at least BREADCRUMB_MIN_GAIN closer to the far side.
    constexpr float BREADCRUMB_REACH = 45.0f;
    constexpr float BREADCRUMB_MIN_GAIN = 3.0f;
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
    /// The pull drill's rungs: a pack is drilled once the nearest other creature stands at least this far from it
    /// (InstanceEncounter::StartDrill), the last rung any pack.
    constexpr std::array<float, 4> PULL_GAPS = { 30.0f, 22.0f, 14.0f, 0.0f };
    /// The drill's party stands at least this far from anything left alive, looking no further back than
    /// PULL_START_MAX_YARDS along the route.
    constexpr float PULL_START_CLEARANCE = 25.0f;
    constexpr float PULL_START_MAX_YARDS = 120.0f;

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
    _drillRung = std::min<uint32>(scenario.Tuning().Instance.PullRungStart, uint32(PULL_GAPS.size()) - 1);
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
        RewardTerm::Readiness, RewardTerm::Threat };
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
        table.Add("wing_cleared_share", [this](Env const& env, uint32)
        {
            EnvInstance const& fight = _envs[env.Index];
            return fight.HostileTotal ? std::min(1.0f, float(fight.TrashKills + (fight.BossDead ? 1 : 0))
                / float(fight.HostileTotal + 1)) : 0.0f;
        });
        table.Add("wing_crowd_seconds", [this](Env const& env, uint32) { return _envs[env.Index].CrowdSeconds; });
        table.Add("wing_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
        table.Add("wing_probe", [this](Env const& env, uint32) { return _envs[env.Index].Probe ? 1.0f : 0.0f; });
        table.Add("wing_rises", [this](Env const& env, uint32) { return float(_envs[env.Index].Rises); });
        table.Add("wing_scripted", [this](Env const& env, uint32) { return _envs[env.Index].Scripted ? 1.0f : 0.0f; });
        table.Add("wing_level", [this](Env const& env, uint32) { return float(_scenario.Data(env).EpisodeLevel); });
    }
}

void Animus::Curriculum::InstanceEncounter::ResetEpisode(Env& env)
{
    EnvInstance& fight = _envs[env.Index];
    // A pull drill: one line (Instance.WingTrace), and its outcome on the drill's own ladder.
    if (fight.Drill)
    {
        if (_scenario.Tuning().Instance.WingTrace)
            LOG_INFO("module.animus", "Pull drill: env {} rung {} level {} gap {:.0f} yd | {} | peak {} on the party, "
                "{} wipes, {:.0f}s", env.Index, fight.DrillRung, fight.Level, fight.DrillGap,
                fight.DrillExtra ? Acore::StringFormat("second pack ({})", fight.DrillExtraEntry)
                    : fight.DrillCleared ? std::string(fight.DrillOther ? "clean (another pack)" : "clean")
                    : fight.Wiped ? std::string("wiped")
                    : fight.DrillEngaged ? std::string("pack alive at the end") : std::string("never pulled"),
                fight.DrillPeak, fight.Wipes, float(fight.LastMs) / 1000.0f);
        if (fight.Row)
            NoteDrill(fight.DrillRung, fight.DrillCleared && !fight.DrillExtra);
        fight = EnvInstance();
        return;
    }
    // The run just ended counts toward the support's running share (training runs of a whole dungeon only).
    // One line a finished run (Instance.WingTrace): how far it got, what it killed and how it ended.
    if (!fight.Route.empty() && _scenario.Tuning().Instance.WingTrace)
        LOG_INFO("module.animus", "Wing run: env {} {}{}{} rung {} level {} | point {}/{} at the end, {} of {} creatures killed, "
            "{} bosses, last boss {} | {} wipes, {:.0f}s with no progress at the end | {}",
            env.Index, fight.Evaluating ? "eval" : "train", fight.Scripted ? " scripted" : "",
            fight.Probe ? " probe" : "", fight.Rung, fight.Level,
            fight.RouteNext, fight.Route.size(), fight.TrashKills, fight.HostileTotal, fight.BossKills,
            fight.BossDead ? "killed" : "alive", fight.Wipes,
            float(fight.LastMs - std::min(fight.LastMs, fight.ProgressMs)) / 1000.0f,
            fight.BossDead ? "cleared" : fight.Wiped ? "wiped" : "out of time");
    // Every training run counts for its rung; the probes are the policy's own.
    if (!fight.Route.empty() && !fight.Evaluating)
    {
        float const cleared = fight.HostileTotal
            ? float(fight.TrashKills + (fight.BossDead ? 1 : 0)) / float(fight.HostileTotal + 1) : 0.0f;
        _scenario.NoteWingRun(fight.Rung, fight.Probe, fight.BossDead ? 1.0f : std::min(1.0f, cleared));
    }
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
    // An arena pinned to one row (ArenaDefinition::InstanceRow) runs it every time, and its outcome moves no rung.
    int8 const pinned = _scenario.Arena(env).InstanceRow;
    DifficultyLadder::Pick const pick = pinned >= 0 ? DifficultyLadder::Pick{ uint32(pinned), false }
        : _ladder.Draw(env, fight.Layout, fight.Spec, uint32(rows.size()) - 1);
    fight.Tier = std::min<uint32>(pick.Tier, uint32(rows.size()) - 1);
    fight.Counts = pick.Counts;
    fight.Row = rows[fight.Tier];
    fight.MapId = fight.Row->MapId;
    fight.Entry = fight.Row->Entry;

    data.EpisodeMapId = fight.Row->MapId;
    data.HasEpisodeMap = true;
    data.EpisodeLevel = fight.Row->Level;
    // A whole dungeon is run by characters of its own level range: the dungeon finder's target range for the map
    // and difficulty (LFGDungeons.dbc), a level drawn in it every run. The row's level is the fallback.
    // Training runs it at the support ladder's rung (StageScenario::WING_RUNGS): above that range, with wipes to
    // spare, the script playing some seats and every seat shown its hints; a probe at the rung's level and wipes with
    // neither; an evaluation as it is.
    if (Wing(env))
    {
        CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
        fight.Evaluating = env.Evaluating;
        fight.Rung = env.Evaluating ? uint32(StageScenario::WING_RUNGS.size()) - 1 : _scenario.WingRungNow();
        StageScenario::WingRung const& rung = StageScenario::WING_RUNGS[fight.Rung];
        // A pull drill is one pull by the learned seats, at about the dungeon's own levels: a party far above them
        // walks past what a party of the level would pull, and the drill is about what it would pull. No probes, no
        // script, one wipe; the hints as the support has them.
        fight.Drill = _scenario.Arena(env).PullDrill && !env.Evaluating;
        fight.Probe = !env.Evaluating && !fight.Drill && frand(0.0f, 1.0f) < tuning.WingProbe;
        fight.WipesAllowed = fight.Drill ? 1 : tuning.WingWipes + rung.ExtraWipes;
        // The script's seats and hints are a support switched on by hand (Instance.WingSupport): off, the rungs are
        // the levels and the wipes alone.
        bool const supported = tuning.WingSupport && !env.Evaluating && !fight.Probe;
        data.WingScript = supported && !fight.Drill ? rung.Script : 0.0f;
        data.WingHint = supported ? rung.Hint : 0.0f;
        auto const [low, high] = DungeonLevels(*fight.Row);
        uint32 const lift = fight.Drill ? std::min(rung.Lift, tuning.PullLift) : rung.Lift;
        data.EpisodeLevel = uint8(std::min<uint32>(urand(low, high) + lift, DEFAULT_MAX_LEVEL));
    }
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
        WingPlan const plan = WingRoute(env, map, seat, boss);
        fight.Route = plan.Route;
        fight.Dense = plan.Dense;
        fight.RouteDense = plan.RouteDense;
        for (SeatInstance& seatState : fight.Seats)
        {
            seatState.DenseAt = 0;
            seatState.Detour.clear();
            seatState.DetourMs = 0;
        }
        // The creatures a full clear kills: the ones a seat can walk to, with a field route (WingPlan::Reachable),
        // and the bosses; every hostile one with the navmesh's.
        fight.HostileTotal = 0;
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (Hostile(seat, creature) && (!plan.Field || creature->IsDungeonBoss() || creature->isWorldBoss()
                || std::binary_search(plan.Reachable.begin(), plan.Reachable.end(), spawnId)))
                ++fight.HostileTotal;
        fight.RouteRemain.assign(fight.Route.size(), 0.0f);
        for (std::size_t i = fight.Route.size(); i-- > 1;)
            fight.RouteRemain[i - 1] = fight.RouteRemain[i] + fight.Route[i - 1].GetExactDist(&fight.Route[i]);
        env.Targets.clear();
        if (fight.Drill && !StartDrill(env, map, plan))
        {
            LOG_WARN("module.animus", "{}: env {}: {} has no field route packs to drill; the whole dungeon instead",
                _scenario.Name(), env.Index, fight.Row->Name);
            fight.Drill = false;
        }
        // Food and water for the whole dungeon, as a party brings: without them nobody could eat or drink between
        // pulls (the gauntlet's encounter was the only one that gave them), and a healer waiting for its mana on
        // natural regeneration held the party for the hour (2026-10-01).
        ConsumablePool const& consumables = ConsumablePool::Instance();
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index))
            {
                SeatState& seatState = _scenario.Data(env).Seats[index];
                _scenario.PrepareFighter(bot, seatState);
                SeatInstance& supplies = fight.Seats[index];
                supplies.FoodItem = consumables.Food(seatState.Level);
                supplies.DrinkItem = bot->GetMaxPower(POWER_MANA) ? consumables.Drink(seatState.Level) : 0;
                StockConsumables(bot, supplies.FoodItem, supplies.DrinkItem, _scenario.Tuning().Instance.WingSupplies);
            }
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

    // Whether the party is in a fight, and how many are on it (Instance.WingCrowd), followed for the wipe's line too.
    bool const wasFighting = fight.Fighting;
    fight.Fighting = false;
    for (uint32 slot = 0; slot < env.Targets.size() && !fight.Fighting; ++slot)
        if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
            fight.Fighting = true;
    // A fight started with the party ready (Instance.WingEngage, paid to the tank): the parties stood in front of the
    // next pack, rested and whole, for minutes on end (2026-10-03) -- pulling it is the tank's call to make.
    if (fight.Fighting && !wasFighting)
    {
        float const ready = _scenario.Tuning().Instance.WingReadyShare;
        bool allReady = true;
        for (uint32 index = 0; index < data.ActiveSeats && allReady; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive())
            {
                uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
                allReady = bot->GetHealthPct() >= ready * 100.0f
                    && (!maxMana || float(bot->GetPower(POWER_MANA)) >= ready * float(maxMana));
            }
        if (allReady)
            ++fight.ReadyEngages;
    }
    TraceWing(env, fight, fight.Fighting || !anyoneAlive);
    if (fight.Drill)
        UpdateDrill(env, fight);
    fight.LastMs = env.EpisodeElapsedMs;
    fight.Level = uint32(data.EpisodeLevel);

    // The clock ran out (Instance.WingTrace): what the party was doing -- what is in combat around it, where, on whom.
    if (_scenario.Tuning().Instance.WingTrace && !fight.Drill && !fight.EndLogged && TimeIsUp(env))
    {
        fight.EndLogged = true;
        Player* anchor = nullptr;
        for (uint32 index = 0; index < data.ActiveSeats && !anchor; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive())
                anchor = bot;
        std::string around;
        if (anchor)
        {
            std::list<Unit*> units;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(anchor, anchor, 80.0f);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(anchor, units, check);
            Cell::VisitObjects(anchor, searcher, 80.0f);
            for (Unit* unit : units)
                if (unit->IsAlive() && !unit->IsPlayer() && unit->IsInCombat())
                    around += Acore::StringFormat(" {}({:.0f}yd dz {:.0f} on {}{})", unit->GetEntry(),
                        anchor->GetExactDist(unit), unit->GetPositionZ() - anchor->GetPositionZ(),
                        unit->GetVictim() ? unit->GetVictim()->GetName() : "nobody",
                        unit->HasUnitState(UNIT_STATE_EVADE) ? " evading" : "");
        }
        LOG_INFO("module.animus", "Wing time: env {} at point {}/{}, fighting {}, {} on the party, last kill {:.0f}s "
            "ago, in combat around:{}", env.Index, fight.RouteNext, fight.Route.size(), fight.Fighting ? 1 : 0,
            fight.OnParty,
            float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, fight.LastKillMs)) / 1000.0f,
            around.empty() ? " nothing" : around);
    }

    // The dead rejoin: one nobody has raised by Instance.WingRiseMs after the fight is over rises at the door, as a
    // player who released and ran back would, and walks back to the group (the pull waits for it).
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        SeatInstance& seatState = fight.Seats[index];
        if (!bot || bot->IsAlive())
        {
            seatState.DeadSinceMs = 0;
            continue;
        }
        if (fight.Fighting || !anyoneAlive)
        {
            seatState.DeadSinceMs = 0;
            continue;
        }
        if (!seatState.DeadSinceMs)
            seatState.DeadSinceMs = std::max<uint32>(1, env.EpisodeElapsedMs);
        if (env.EpisodeElapsedMs >= seatState.DeadSinceMs + _scenario.Tuning().Instance.WingRiseMs)
        {
            bot->ResurrectPlayer(0.5f);
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA) / 2);
            BotFactory::TeleportWithinMap(bot, data.EpisodeSpawn);
            bot->CombatStopWithPets(true);
            seatState.DeathPaid = false;
            seatState.Walk = 0;
            seatState.DeadSinceMs = 0;
            ++fight.Rises;
        }
    }

    // A thing the tank has had within reach of the script (25 yd), out of a fight, unused for 45 s is out of its
    // reach (on a ledge, in the wall): it is passed by for the rest of the run, as used.
    constexpr float NEAR_OBJECT_YARDS = 25.0f;
    constexpr uint32 GIVE_UP_MS = 45000;
    {
        Player* tank = nullptr;
        for (uint32 index = 0; index < data.ActiveSeats && !tank; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot->GetGUID() == fight.Tank && bot->IsAlive())
                tank = bot;
        GameObject* nearest = nullptr;
        if (tank && !fight.Fighting)
            for (ObjectGuid const& guid : fight.Objects)
                if (GameObject* object = ObjectAccessor::GetGameObject(*tank, guid); object && Usable(object)
                    && CrowdBlock::CanUse(tank, object) && tank->GetExactDist(object) <= NEAR_OBJECT_YARDS
                    && std::find(fight.Used.begin(), fight.Used.end(), guid) == fight.Used.end()
                    && (!nearest || tank->GetExactDist(object) < tank->GetExactDist(nearest)))
                    nearest = object;
        if (!nearest)
            fight.Approached = ObjectGuid::Empty;
        else if (nearest->GetGUID() != fight.Approached)
        {
            fight.Approached = nearest->GetGUID();
            fight.ApproachedMs = env.EpisodeElapsedMs;
        }
        else if (env.EpisodeElapsedMs > fight.ApproachedMs + GIVE_UP_MS)
        {
            fight.Used.push_back(fight.Approached);
            fight.Approached = ObjectGuid::Empty;
        }
    }

    // Each seat's place on the route: on to the furthest of the next few points it is near, up to the route's next
    // point for the tank and up to the tank's place for the others.
    constexpr float WALK_REACH = 12.0f;
    constexpr std::size_t WALK_LOOK = 6;
    uint32 tankWalk = fight.RouteNext;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->GetGUID() == fight.Tank)
            tankWalk = fight.Seats[index].Walk;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        if (!bot || !bot->IsAlive() || fight.Route.empty())
            continue;
        SeatInstance& seatState = fight.Seats[index];
        uint32 const cap = std::min<uint32>(bot->GetGUID() == fight.Tank ? fight.RouteNext : tankWalk,
            uint32(fight.Route.size()) - 1);
        std::size_t reached = seatState.Walk;
        for (std::size_t i = seatState.Walk; i <= cap && i < seatState.Walk + WALK_LOOK; ++i)
            if (bot->GetExactDist(&fight.Route[i]) <= WALK_REACH)
                reached = i + 1;
        seatState.Walk = uint32(std::min<std::size_t>(reached, cap));
    }

    // A run stuck for two minutes, and every ten after (Instance.WingTrace): each seat's state against the tank's, and
    // the tank's way on, so a script or a party that stands still says why.
    constexpr uint32 STUCK_FIRST_MS = 120000;
    constexpr uint32 STUCK_EVERY_MS = 600000;
    uint32 const still = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, fight.ProgressMs);
    if (_scenario.Tuning().Instance.WingTrace && !fight.Drill && still >= STUCK_FIRST_MS && env.EpisodeElapsedMs >= fight.StuckLoggedMs)
    {
        fight.StuckLoggedMs = env.EpisodeElapsedMs + STUCK_EVERY_MS;
        Unit* tank = nullptr;
        if (Player* any = _scenario.SeatBot(env, 0); any && !fight.Tank.IsEmpty())
            tank = ObjectAccessor::GetUnit(*any, fight.Tank);
        std::string seats;
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index))
            {
                uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
                SeatState const& state = data.Seats[index];
                std::string const& reason = state.ScriptReason;
                // What it pressed against what the script suggested, by name.
                std::string presses;
                if (state.L)
                {
                    std::vector<std::string> const names = state.L->ActionNames();
                    auto const name = [&names](int32 action)
                    {
                        return action >= 0 && std::size_t(action) < names.size() ? names[std::size_t(action)]
                            : std::string("-");
                    };
                    presses = Acore::StringFormat(" pressed {} hint {}", name(state.Pressed), name(state.HintAction));
                }
                seats += Acore::StringFormat("{}[{}{} hp {:.0f}% mana {} {:.0f}yd at ({:.0f} {:.0f} {:.0f}){}{}{}{}]",
                    seats.empty() ? "" : " ", index, bot == tank ? " tank" : "", bot->GetHealthPct(),
                    maxMana ? std::to_string(bot->GetPower(POWER_MANA) * 100 / maxMana) + "%" : "-",
                    tank && tank->IsInMap(bot) ? bot->GetExactDist(tank) : -1.0f, bot->GetPositionX(),
                    bot->GetPositionY(), bot->GetPositionZ(), bot->IsAlive() ? "" : " dead",
                    bot->IsInCombat() ? " combat" : "", presses,
                    reason.empty() ? std::string() : " {" + reason + "}");
            }
        float const toNext = tank && fight.RouteNext < fight.Route.size()
            ? tank->GetExactDist(&fight.Route[fight.RouteNext]) : -1.0f;
        std::string objects;
        if (tank)
            for (ObjectGuid const& guid : fight.Objects)
                if (GameObject* object = ObjectAccessor::GetGameObject(*tank, guid))
                    objects += Acore::StringFormat("{}{} type {} state {} {:.0f}yd", objects.empty() ? "" : ", ",
                        object->GetEntry(), uint32(object->GetGoType()), uint32(object->GetGoState()),
                        tank->GetExactDist(object));
        seats += " | objects: " + (objects.empty() ? std::string("none") : objects);
        for (uint32 index = 0; index < data.ActiveSeats && tank; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot != tank && bot->IsAlive() && tank->IsInMap(bot)
                && bot->GetExactDist(tank) > 20.0f)
            {
                PathGenerator path(bot);
                path.CalculatePath(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), false);
                G3D::Vector3 const end = path.GetPath().empty() ? G3D::Vector3(0, 0, 0) : path.GetPath().back();
                seats += Acore::StringFormat(" | seat {} to the tank: path type {}, ends {:.0f} yd from it, walk {}, "
                    "motion {}, moving {}", index, uint32(path.GetPathType()), tank->GetExactDist(end.x, end.y, end.z),
                    fight.Seats[index].Walk, uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                    bot->isMoving() ? 1 : 0);
            }
        if (Player* tankPlayer = tank ? tank->ToPlayer() : nullptr; tankPlayer && fight.RouteNext < fight.Route.size())
        {
            Position const& next = fight.Route[fight.RouteNext];
            PathGenerator path(tankPlayer);
            path.CalculatePath(next.GetPositionX(), next.GetPositionY(), next.GetPositionZ(), false);
            G3D::Vector3 const end = path.GetPath().empty() ? G3D::Vector3(0, 0, 0) : path.GetPath().back();
            seats += Acore::StringFormat(" | path to the point: type {} {} points, ends {:.0f} yd from it; tank z "
                "{:.1f}, point z {:.1f}, motion {}", uint32(path.GetPathType()), path.GetPath().size(),
                next.GetExactDist(end.x, end.y, end.z), tankPlayer->GetPositionZ(), next.GetPositionZ(),
                uint32(tankPlayer->GetMotionMaster()->GetCurrentMovementGeneratorType()));
            std::string fighters;
            for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
                if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
                    fighters += Acore::StringFormat(" {}({} on {})", enemy->GetEntry(),
                        tankPlayer->GetExactDist(enemy), enemy->GetVictim() ? enemy->GetVictim()->GetName() : "nobody");
            uint32 tankSeat = 0;
            for (uint32 index = 0; index < data.ActiveSeats; ++index)
                if (_scenario.SeatBot(env, index) == tankPlayer)
                    tankSeat = index;
            SeatView probe;
            probe.Bot = tankPlayer;
            View(env, tankSeat, probe);
            SeatInstance const& own = fight.Seats[tankSeat];
            seats += Acore::StringFormat(" | fighting {}{}, moving {}, objective {:.0f} yd at ({:.0f} {:.0f} {:.0f}), "
                "point ({:.0f} {:.0f} {:.0f}), route yard {} of {} ({:.1f} yd off it), step {}, detour {} points",
                fight.Fighting ? 1 : 0, fighters, tankPlayer->isMoving() ? 1 : 0,
                tankPlayer->GetExactDist(&probe.Objective), probe.Objective.GetPositionX(),
                probe.Objective.GetPositionY(), probe.Objective.GetPositionZ(), next.GetPositionX(), next.GetPositionY(),
                next.GetPositionZ(), own.DenseAt, fight.Dense.size(),
                own.DenseAt < fight.Dense.size() ? tankPlayer->GetExactDist(&fight.Dense[own.DenseAt]) : -1.0f,
                probe.Crowd.HasStep ? Acore::StringFormat("({:.0f} {:.0f} {:.0f})", probe.Crowd.Step.GetPositionX(),
                    probe.Crowd.Step.GetPositionY(), probe.Crowd.Step.GetPositionZ()) : std::string("none"),
                own.Detour.size());
        }
        LOG_INFO("module.animus", "Wing stuck: env {} {:.0f}s still at point {}/{} (tank {:.0f} yd from it), {} on the "
            "party, pack ahead {} | {}", env.Index, float(still) / 1000.0f, fight.RouteNext, fight.Route.size(), toNext,
            fight.OnParty, fight.HasAhead && tank
                ? Acore::StringFormat("{:.0f} yd ({})", tank->GetExactDist(&fight.Ahead), fight.AheadSize)
                : std::string("none"), seats);
    }
    for (uint32 index = 0; index < data.ActiveSeats && !fight.Scripted; ++index)
        fight.Scripted = data.Seats[index].Scripted;

    // A wing: the next point of the route reached by any seat out of a fight -- ground is taken by clearing it, not
    // by running past what is still fighting (the Deadmines' parties ran into the next pack mid-fight and had eight on
    // them at once, 2026-10-01); a wipe stands the party up at the door (until
    // the run's allowance, Instance.WingWipes and more while the support lasts, which ends it), with the trash that
    // killed it still where it was.
    if (fight.RouteNext < fight.Route.size() && !fight.Fighting && (!fight.Drill || fight.RouteNext < fight.DrillPoint))
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive()
                && bot->GetExactDist(&fight.Route[fight.RouteNext]) <= 15.0f)
            {
                ++fight.RouteNext;
                break;
            }
    if (!anyoneAlive && !fight.BossDead)
    {
        ++fight.Wipes;
        if (_scenario.Tuning().Instance.WingTrace)
            LogWipe(env, fight);
        fight.Trace = EnvInstance::FightTrace();
        if (fight.Wipes >= fight.WipesAllowed)
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
                // Out of the fight it lost: what killed it stays where it was, and a party still flagged in combat
                // with it could not eat or drink at the door, and waited there for the hour (2026-10-01).
                bot->CombatStopWithPets(true);
                fight.Seats[index].DeathPaid = false;
                fight.Seats[index].Walk = 0;
            }
        if (Player* owner = _scenario.Owner(env); owner && !owner->IsAlive())
        {
            owner->ResurrectPlayer(0.5f);
            BotFactory::TeleportWithinMap(owner, data.EpisodeSpawn);
        }
    }
}

void Animus::Curriculum::InstanceEncounter::TraceWing(Env& env, EnvInstance& fight, bool fighting)
{
    EnvInstance::FightTrace& trace = fight.Trace;
    EnvState const& data = _scenario.Data(env);
    fight.OnParty = 0;
    fight.OnTank = 0;
    fight.Elites = 0;
    // The tank: the living seat with the most mitigation among those that can hold a pull, else among all -- the
    // party block's rule (PartyEncounter::Tank), so the seats follow the one the crowd is counted against. A party
    // of level-17 builds none of which could hold one had no tank and stood at the door (2026-10-01).
    fight.Tank = ObjectGuid::Empty;
    {
        float most = -1.0f;
        bool holds = false;
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive() && data.Seats[index].L)
            {
                // The seat drawn as the party's tank, while it lives (StageScenario::FitsDungeonRole).
                Aptitude const& apt = data.Seats[index].Apt;
                bool const can = data.Seats[index].DungeonRole == DUNGEON_TANK
                    || (data.Seats[index].DungeonRole == DUNGEON_ANY
                        && AptitudeDemand::HoldsThePull().MetBy(apt));
                if ((can && !holds) || (can == holds && apt[Aptitude::MITIGATION] > most))
                {
                    holds = holds || can;
                    most = apt[Aptitude::MITIGATION];
                    fight.Tank = bot->GetGUID();
                }
            }
    }
    if (!fighting)
    {
        trace.InFight = false;
        return;
    }
    if (!trace.InFight)
    {
        trace = EnvInstance::FightTrace();
        trace.InFight = true;
        trace.StartMs = env.EpisodeElapsedMs;
        trace.KillsAtStart = fight.TrashKills;
        trace.PointAtStart = fight.RouteNext;
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* bot = _scenario.SeatBot(env, index))
                trace.Dead[index] = !bot->IsAlive();
    }

    // Who is on the party now: every hostile creature within reach of a living seat whose victim is a player.
    Player* anchor = nullptr;
    Player* tank = nullptr;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive() && bot->IsInWorld())
        {
            anchor = anchor ? anchor : bot;
            if (!tank && AptitudeDemand::HoldsThePull().MetBy(data.Seats[index].Apt))
                tank = bot;
        }
    if (anchor)
    {
        constexpr float TRACE_REACH = 60.0f;
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(anchor, anchor, TRACE_REACH);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(anchor, units, check);
        Cell::VisitObjects(anchor, searcher, TRACE_REACH);
        uint32 engaged = 0;
        uint32 elites = 0;
        uint32 onTank = 0;
        std::map<uint32, uint32> entries;
        for (Unit* unit : units)
        {
            Unit* victim = unit->IsAlive() && !unit->IsPlayer() ? unit->GetVictim() : nullptr;
            if (!victim || !victim->IsPlayer())
                continue;
            ++engaged;
            onTank += victim == tank ? 1 : 0;
            if (Creature* creature = unit->ToCreature())
            {
                elites += creature->isElite() ? 1 : 0;
                ++entries[creature->GetEntry()];
            }
        }
        fight.OnParty = engaged;
        fight.OnTank = onTank;
        fight.Elites = elites;
        if (engaged > _scenario.Tuning().Instance.WingCrowdFree)
            fight.CrowdSeconds += float(_scenario.DecisionMs()) / 1000.0f;
        if (engaged > trace.PeakEngaged)
        {
            trace.PeakEngaged = engaged;
            trace.PeakElites = elites;
            trace.PeakOnTank = onTank;
            trace.PeakEntries.clear();
            for (auto const& [entry, count] : entries)
            {
                CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
                trace.PeakEntries += Acore::StringFormat("{}{}x{}", trace.PeakEntries.empty() ? "" : ", ", count,
                    info ? info->Name : std::to_string(entry));
            }
        }
    }

    // Each death in order: role, class, seconds into the fight, its mana then.
    static char const* const CLASS_NAMES[] = { "?", "warrior", "paladin", "hunter", "rogue", "priest", "dk", "shaman",
        "mage", "warlock", "?", "druid" };
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        if (bot && bot->IsAlive() && bot->GetMaxPower(POWER_MANA))
            trace.Mana[index] = uint8(bot->GetPower(POWER_MANA) * 100 / bot->GetMaxPower(POWER_MANA));
        if (!bot || bot->IsAlive() || trace.Dead[index])
            continue;
        trace.Dead[index] = true;
        Aptitude const& apt = data.Seats[index].Apt;
        char const* role = AptitudeDemand::HoldsThePull().MetBy(apt) ? "tank"
            : AptitudeDemand::KeepsThemUp().MetBy(apt) ? "healer" : "dps";
        // The mana it had last alive: a corpse has none.
        trace.Deaths += Acore::StringFormat("{}{} {} {:.0f}s{}", trace.Deaths.empty() ? "" : ", ", role,
            bot->getClass() < std::size(CLASS_NAMES) ? CLASS_NAMES[bot->getClass()] : "?",
            float(env.EpisodeElapsedMs - trace.StartMs) / 1000.0f,
            bot->GetMaxPower(POWER_MANA) ? Acore::StringFormat(" mana {}%", trace.Mana[index]) : "");
    }
}

void Animus::Curriculum::InstanceEncounter::LogWipe(Env const& env, EnvInstance const& fight) const
{
    EnvInstance::FightTrace const& trace = fight.Trace;
    // The healers' mana when the party went down is in its deaths (each seat's mana as it died).
    LOG_INFO("module.animus", "Wing wipe: env {} {} rung {}{} level {} wipe {}/{} at {:.0f}s, point {}/{} "
        "(fight began at point {}, lasted {:.0f}s, {} kills in it, {} before) | peak {} on the party ({} elite, {} on "
        "the tank): {} | deaths: {}",
        env.Index, fight.Evaluating ? "eval" : fight.Drill ? "drill" : "train", fight.Rung, fight.Probe ? " probe" : "",
        uint32(_scenario.Data(env).EpisodeLevel),
        fight.Wipes, fight.WipesAllowed, float(env.EpisodeElapsedMs) / 1000.0f, fight.RouteNext, fight.Route.size(),
        trace.PointAtStart, float(env.EpisodeElapsedMs - trace.StartMs) / 1000.0f,
        fight.TrashKills - trace.KillsAtStart, trace.KillsAtStart, trace.PeakEngaged, trace.PeakElites,
        trace.PeakOnTank, trace.PeakEntries.empty() ? "none seen" : trace.PeakEntries,
        trace.Deaths.empty() ? "none seen" : trace.Deaths);
}

bool Animus::Curriculum::InstanceEncounter::Hostile(Player const* seat, Creature const* creature)
{
    return creature && seat && creature->IsAlive() && !creature->IsCritter() && !creature->IsCivilian()
        && !creature->IsTotem() && !creature->IsPet() && !creature->IsSummon()
        && !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE)
        && creature->IsHostileTo(seat);
}

bool Animus::Curriculum::InstanceEncounter::Usable(GameObject const* object)
{
    if (!object || !object->isSpawned() || object->GetGoState() != GO_STATE_READY || object->getLootState() != GO_READY
        || object->HasGameObjectFlag(GameObjectFlags(GO_FLAG_NOT_SELECTABLE | GO_FLAG_LOCKED | GO_FLAG_INTERACT_COND
            | GO_FLAG_IN_USE)))
        return false;
    switch (object->GetGoType())
    {
        case GAMEOBJECT_TYPE_BUTTON:
        case GAMEOBJECT_TYPE_GOOBER:
        case GAMEOBJECT_TYPE_CHEST:
        {
            // Only a chest any hand opens (the Deadmines' gunpowder): a vein, a herb or a locked chest wants a skill
            // the party may not have, and the tank stood by a Tin Vein in the wall for the hour (2026-10-01).
            LockEntry const* lock = sLockStore.LookupEntry(object->GetGOInfo()->GetLockId());
            for (uint32 i = 0; lock && i < MAX_LOCK_CASE; ++i)
                if (lock->Type[i] == LOCK_KEY_SKILL && lock->Skill[i])
                    return false;
            return true;
        }
        case GAMEOBJECT_TYPE_DOOR:
            // A door with a lock is opened by what its lock names (a lever, the cannon), not by a hand on it.
            return !object->GetGOInfo()->GetLockId();
        default:
            return false;
    }
}

bool Animus::Curriculum::InstanceEncounter::Wing(Env const& env) const
{
    return _scenario.Arena(env).Instance == InstanceLadder::Wing;
}

std::pair<uint32, uint32> Animus::Curriculum::InstanceEncounter::DungeonLevels(BossRow const& row)
{
    for (LFGDungeonEntry const* dungeon : sLFGDungeonStore)
    {
        if (!dungeon || dungeon->MapID != row.MapId || dungeon->Difficulty != row.Difficulty)
            continue;
        uint32 const low = dungeon->TargetLevelMin ? dungeon->TargetLevelMin : dungeon->MinLevel;
        uint32 const high = dungeon->TargetLevelMax ? dungeon->TargetLevelMax : dungeon->MaxLevel;
        if (low && high >= low)
            return { low, std::min<uint32>(high, DEFAULT_MAX_LEVEL) };
    }
    return { row.Level, row.Level };
}

Animus::Curriculum::InstanceEncounter::WingPlan Animus::Curriculum::InstanceEncounter::WingRoute(Env const& env,
    Map* map, Player* seat, Creature* boss) const
{
    static std::map<EngageKey, WingPlan> routes;
    static std::mutex routesLock;
    BossRow const& row = *_envs[env.Index].Row;
    EngageKey const key{ row.MapId, row.Entry };
    {
        std::lock_guard<std::mutex> guard(routesLock);
        if (auto const known = routes.find(key); known != routes.end())
            return known->second;
    }

    // The whole dungeon, end to end. First the bosses, nearest next from the door, then the last boss: the order the
    // dungeon opens up in (a boss's death opens the door behind it). Straight to the last boss, the path gave out a
    // third of the way in the Deadmines (a door's tunnel the navmesh does not join).
    std::vector<Position> stops;
    Position const last(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
    {
        std::vector<Creature const*> bosses;
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature && creature != boss && creature->IsAlive()
                && (creature->IsDungeonBoss() || creature->isWorldBoss()))
                bosses.push_back(creature);
        Position at(seat->GetPositionX(), seat->GetPositionY(), seat->GetPositionZ());
        while (!bosses.empty())
        {
            auto const nearest = std::min_element(bosses.begin(), bosses.end(),
                [&at](Creature const* a, Creature const* b)
            {
                return a->GetExactDist(&at) < b->GetExactDist(&at);
            });
            at.Relocate((*nearest)->GetPositionX(), (*nearest)->GetPositionY(), (*nearest)->GetPositionZ());
            stops.push_back(at);
            bosses.erase(nearest);
        }
        stops.push_back(last);
    }

    // Over the layered field where it holds the dungeon: the way the seats themselves can walk (FieldRoute).
    if (FieldRoute::Covers(row.MapId, seat->GetPositionX(), seat->GetPositionY()))
    {
        WingPlan plan = FieldWingRoute(env, map, seat, boss, stops);
        if (plan.Field)
        {
            std::lock_guard<std::mutex> guard(routesLock);
            routes[key] = plan;
            return plan;
        }
        LOG_WARN("module.animus", "{}: {}: the layered field has no way from the door through the bosses; the "
            "navmesh's route instead", _scenario.Name(), row.Name);
    }

    // The server's path stop to stop, leg by leg as EngagePoint walks it; where the navmesh does not join two
    // stops, the straight line between them (a door's tunnel is walked, the navmesh just does not cross it).
    auto const pathThrough = [&](std::vector<Position> const& stops, bool log)
    {
        std::vector<G3D::Vector3> points;
        Position cursor(seat->GetPositionX(), seat->GetPositionY(), seat->GetPositionZ());
        points.emplace_back(cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ());
        for (Position const& stop : stops)
        {
            for (uint32 leg = 0; leg < PATH_LEGS; ++leg)
            {
                map->LoadGrid(cursor.GetPositionX(), cursor.GetPositionY());
                PathGenerator path(seat);
                path.CalculatePath(cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ(),
                    stop.GetPositionX(), stop.GetPositionY(), stop.GetPositionZ(), false);
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
            // Short of the stop: a closed door's gap in the navmesh, most likely. Path back from the stop as well;
            // where the two halves come within a door's width of each other, only that gap is walked straight.
            // Otherwise the straight line from the path's end, which went through the rock (the Deadmines' foundry
            // and ship legs).
            float const missed = cursor.GetExactDist(&stop);
            float bridged = missed;
            if (missed > 5.0f)
            {
                std::vector<G3D::Vector3> back;
                Position from(stop);
                for (uint32 leg = 0; leg < PATH_LEGS; ++leg)
                {
                    map->LoadGrid(from.GetPositionX(), from.GetPositionY());
                    PathGenerator path(seat);
                    path.CalculatePath(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(),
                        cursor.GetPositionX(), cursor.GetPositionY(), cursor.GetPositionZ(), false);
                    if (path.GetPathType() & PATHFIND_NOPATH || path.GetPath().size() < 2)
                        break;
                    for (std::size_t i = 1; i < path.GetPath().size(); ++i)
                        back.push_back(path.GetPath()[i]);
                    G3D::Vector3 const& end = path.GetPath().back();
                    if (Distance2d(from, Position(end.x, end.y, end.z)) < 1.0f)
                        break;
                    from.Relocate(end.x, end.y, end.z);
                    if (!(path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORT)))
                        break;
                }
                // Across the gap, from creature to creature: the dungeon's trash stands in its corridors and rooms, so
                // stepping to the nearest one that is closer to where the path back begins walks the way the dungeon
                // goes where the navmesh does not join it (the Deadmines' foundry and ship legs, 2026-09-30).
                Position const target = back.empty() ? stop : Position(back.back().x, back.back().y, back.back().z);
                std::vector<Position> spawns;
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature)
                        spawns.emplace_back(creature->GetHomePosition());
                Position at(cursor);
                for (uint32 step = 0; step < 64 && at.GetExactDist(&target) > BREADCRUMB_REACH; ++step)
                {
                    float const left = at.GetExactDist(&target);
                    Position const* next = nullptr;
                    for (Position const& spawn : spawns)
                        if (spawn.GetExactDist(&target) < left - BREADCRUMB_MIN_GAIN
                            && at.GetExactDist(&spawn) <= BREADCRUMB_REACH
                            && (!next || at.GetExactDist(&spawn) < at.GetExactDist(next)))
                            next = &spawn;
                    if (!next)
                        break;
                    points.emplace_back(next->GetPositionX(), next->GetPositionY(), next->GetPositionZ());
                    at = *next;
                }
                bridged = at.GetExactDist(&target);
                for (auto point = back.rbegin(); point != back.rend(); ++point)
                    points.push_back(*point);
                points.emplace_back(stop.GetPositionX(), stop.GetPositionY(), stop.GetPositionZ());
            }
            if (log)
            LOG_INFO("module.animus", "{}: {} route leg to ({:.0f} {:.0f} {:.0f}): the path ends {:.0f} yd short{}",
                _scenario.Name(), row.Name, stop.GetPositionX(), stop.GetPositionY(), stop.GetPositionZ(), missed,
                missed > 5.0f
                    ? Acore::StringFormat("; stepped across by the creatures, {:.0f} yd left straight", bridged)
                    : "");
            cursor = stop;
        }
        return points;
    };


    // Then every pack in the instance -- trash, side bosses and all (Instance.WingFullClear) -- each where the boss
    // route passes nearest it, so the clear goes the way the dungeon opens up. Only the bosses, the route passed the
    // packs by and the parties never cleared a room (2026-10-01: "clear every pull and every boss, even side ones");
    // the packs nearest next from the door led through doors that open later, and the parties stood at them.
    if (_scenario.Tuning().Instance.WingFullClear)
    {
        std::vector<G3D::Vector3> const spine = pathThrough(stops, false);
        constexpr float PACK_REACH = 15.0f;
        std::vector<std::pair<std::pair<std::size_t, float>, Position>> packs;
        std::vector<Creature const*> left;
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature && creature != boss && Hostile(seat, creature))
                left.push_back(creature);
        // A pack: a creature and every other within PACK_REACH of it, stood at their middle.
        while (!left.empty())
        {
            Creature const* first = left.front();
            float x = 0.0f, y = 0.0f, z = 0.0f;
            uint32 count = 0;
            for (auto it = left.begin(); it != left.end();)
                if ((*it)->GetHomePosition().GetExactDist(&first->GetHomePosition()) <= PACK_REACH)
                {
                    x += (*it)->GetHomePosition().GetPositionX();
                    y += (*it)->GetHomePosition().GetPositionY();
                    z += (*it)->GetHomePosition().GetPositionZ();
                    ++count;
                    it = left.erase(it);
                }
                else
                    ++it;
            Position const middle(x / float(count), y / float(count), z / float(count));
            std::size_t along = 0;
            float nearest = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < spine.size(); ++i)
                if (float const d = middle.GetExactDist(spine[i].x, spine[i].y, spine[i].z); d < nearest)
                {
                    nearest = d;
                    along = i;
                }
            packs.push_back({ { along, nearest }, middle });
        }
        std::sort(packs.begin(), packs.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        stops.clear();
        for (auto const& [key, pack] : packs)
            stops.push_back(pack);
        stops.push_back(last);
    }
    LOG_INFO("module.animus", "{}: {} route stops ({})", _scenario.Name(), stops.size(),
        _scenario.Tuning().Instance.WingFullClear ? "every pack, in the bosses' order" : "the bosses");
    std::vector<G3D::Vector3> const points = pathThrough(stops, true);

    // A point every WingWaypointYards along it, between corners too; the last boss's own position last.
    float const spacing = float(std::max<uint32>(5, _scenario.Tuning().Instance.WingWaypointYards));
    std::vector<Position> route;
    float carried = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const& from = points[i - 1];
        G3D::Vector3 const segment = points[i] - from;
        float const length = segment.length();
        float along = spacing - carried;
        while (along <= length)
        {
            G3D::Vector3 const at = from + segment * (along / std::max(0.001f, length));
            route.emplace_back(at.x, at.y, at.z);
            along += spacing;
        }
        carried = length - (along - spacing);
    }
    route.emplace_back(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
    LOG_INFO("module.animus", "{}: the route to {} ({}) is {} points from the door", _scenario.Name(), row.Name,
        row.Entry, route.size());

    WingPlan plan;
    plan.Route = route;
    std::lock_guard<std::mutex> guard(routesLock);
    routes[key] = plan;
    return plan;
}

Animus::Curriculum::InstanceEncounter::WingPlan Animus::Curriculum::InstanceEncounter::FieldWingRoute(Env const& env,
    Map* map, Player* seat, Creature* boss, std::vector<Position> const& bosses) const
{
    BossRow const& row = *_envs[env.Index].Row;
    uint32 const mapId = row.MapId;
    WingPlan plan;
    Position const door(seat->GetPositionX(), seat->GetPositionY(), seat->GetPositionZ());

    // The yards from `from` through each of `stops`, appended to `dense`; false at the first leg the field cannot
    // walk.
    auto const walk = [&](Position from, std::vector<Position> const& stops, std::vector<Position>& dense,
        bool log) -> bool
    {
        if (dense.empty())
            dense.push_back(from);
        for (Position const& stop : stops)
        {
            std::vector<Position> leg;
            if (!FieldRoute::Plan(mapId, from, stop, leg))
            {
                if (log)
                    LOG_WARN("module.animus", "{}: {} field route: no way from ({:.0f} {:.0f} {:.0f}) to "
                        "({:.0f} {:.0f} {:.0f})", _scenario.Name(), row.Name, from.GetPositionX(), from.GetPositionY(),
                        from.GetPositionZ(), stop.GetPositionX(), stop.GetPositionY(), stop.GetPositionZ());
                return false;
            }
            dense.insert(dense.end(), leg.begin() + 1, leg.end());
            from = leg.back();
        }
        return true;
    };

    // The spine: the door, then the bosses in the order the dungeon opens up, the way the field walks it.
    std::vector<Position> spine;
    if (!walk(door, bosses, spine, true))
        return plan;

    // Every pack a seat can walk to from the spine (Instance.WingFullClear), at the spine's yard nearest it, in that
    // order. A pack the field cannot reach -- down a drop too far to take, in lava, behind rock -- is not on the
    // route and not in the dungeon's count: Ragefire's lower cavern and its molten elementals put the old route's
    // targets where no path went (2026-10-02).
    std::vector<Position> stops;
    std::vector<std::pair<ObjectGuid::LowType, Position>> homes;
    std::vector<int32> stopPack;                // per stop, the pack it is (WingPlan::Packs) or -1
    uint32 left = 0;
    uint32 packsReached = 0;
    if (_scenario.Tuning().Instance.WingFullClear)
    {
        constexpr float PACK_REACH = 15.0f;
        std::vector<Creature const*> hostiles;
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
            if (creature && creature != boss && Hostile(seat, creature))
                hostiles.push_back(creature);
        // Every hostile creature's home, the last boss's too: what a pack's gap is measured to (WingPack::Gap).
        for (Creature const* creature : hostiles)
            homes.emplace_back(creature->GetSpawnId(), creature->GetHomePosition());
        homes.emplace_back(boss->GetSpawnId(), boss->GetHomePosition());
        std::vector<std::pair<std::size_t, WingPack>> packs;
        while (!hostiles.empty())
        {
            Creature const* first = hostiles.front();
            std::vector<Creature const*> members;
            for (auto it = hostiles.begin(); it != hostiles.end();)
                if ((*it)->GetHomePosition().GetExactDist(&first->GetHomePosition()) <= PACK_REACH)
                {
                    members.push_back(*it);
                    it = hostiles.erase(it);
                }
                else
                    ++it;
            // The member nearest the spine stands where the pack is fought from, on its own floor.
            std::size_t along = 0;
            float nearest = std::numeric_limits<float>::max();
            Creature const* closest = members.front();
            for (Creature const* member : members)
                for (std::size_t i = 0; i < spine.size(); ++i)
                    if (float const d = member->GetHomePosition().GetExactDist(&spine[i]); d < nearest)
                    {
                        nearest = d;
                        along = i;
                        closest = member;
                    }
            Position const at = closest->GetHomePosition();
            // There and back: a pit a seat can drop into but not climb out of (Ragefire's lower cavern) is no
            // place for the route to go -- the party could never walk on from it.
            std::vector<Position> leg;
            if (!FieldRoute::Plan(mapId, spine[along], at, leg, 400000)
                || !FieldRoute::Plan(mapId, at, spine[along], leg, 400000))
            {
                left += uint32(members.size());
                continue;
            }
            ++packsReached;
            WingPack pack;
            pack.At = Position(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ());
            for (Creature const* member : members)
            {
                plan.Reachable.push_back(member->GetSpawnId());
                pack.Members.push_back(member->GetSpawnId());
            }
            packs.emplace_back(along, std::move(pack));
        }
        std::stable_sort(packs.begin(), packs.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto& [along, pack] : packs)
        {
            stops.push_back(pack.At);
            plan.Packs.push_back(std::move(pack));
        }
    }
    {
        // The bosses in their place among the packs, by where the spine passes them, and the last one last.
        // (spine yard, stop, its pack or -1)
        std::vector<std::tuple<std::size_t, Position, int32>> ordered;
        auto const alongSpine = [&spine](Position const& p)
        {
            std::size_t at = 0;
            float nearest = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < spine.size(); ++i)
                if (float const d = p.GetExactDist(&spine[i]); d < nearest)
                {
                    nearest = d;
                    at = i;
                }
            return at;
        };
        for (std::size_t i = 0; i < stops.size(); ++i)
            ordered.emplace_back(alongSpine(stops[i]), stops[i], int32(i));
        for (std::size_t i = 0; i + 1 < bosses.size(); ++i)
            ordered.emplace_back(alongSpine(bosses[i]), bosses[i], -1);
        std::stable_sort(ordered.begin(), ordered.end(),
            [](auto const& a, auto const& b) { return std::get<0>(a) < std::get<0>(b); });
        stops.clear();
        stopPack.clear();
        for (auto const& [along, stop, pack] : ordered)
        {
            stops.push_back(stop);
            stopPack.push_back(pack);
        }
        stops.push_back(bosses.back());
        stopPack.push_back(-1);
    }
    std::sort(plan.Reachable.begin(), plan.Reachable.end());

    // Each pack's gap: the nearest creature that is not of it or of a pack the route reaches before it -- the one a
    // pull of it may bring, once the party has cleared its way there.
    for (std::size_t k = 0; k < plan.Packs.size(); ++k)
    {
        std::vector<ObjectGuid::LowType> cleared;
        for (std::size_t j = 0; j <= k; ++j)
            cleared.insert(cleared.end(), plan.Packs[j].Members.begin(), plan.Packs[j].Members.end());
        std::sort(cleared.begin(), cleared.end());
        float gap = std::numeric_limits<float>::max();
        for (ObjectGuid::LowType member : plan.Packs[k].Members)
        {
            auto const own = std::find_if(homes.begin(), homes.end(), [member](auto const& home)
            {
                return home.first == member;
            });
            if (own == homes.end())
                continue;
            for (auto const& [spawnId, home] : homes)
                if (!std::binary_search(cleared.begin(), cleared.end(), spawnId))
                    gap = std::min(gap, own->second.GetExactDist(&home));
        }
        plan.Packs[k].Gap = gap;
    }

    // Stop by stop; one the field cannot walk to from the last is passed over rather than giving up on the rest.
    std::vector<Position> dense{ door };
    Position cursor = door;
    uint32 skipped = 0;
    // A pack the route could not walk to keeps no yard (0) and is never drilled.
    for (std::size_t i = 0; i < stops.size(); ++i)
    {
        std::vector<Position> leg;
        if (!FieldRoute::Plan(mapId, cursor, stops[i], leg))
        {
            if (i + 1 == stops.size())
            {
                // The last boss itself out of reach from here: the spine, which the field walks.
                LOG_WARN("module.animus", "{}: {} field route: the last boss out of reach after the packs; the bosses' "
                    "way alone", _scenario.Name(), row.Name);
                dense = spine;
                for (WingPack& pack : plan.Packs)
                    pack.Yard = 0;
                break;
            }
            ++skipped;
            continue;
        }
        dense.insert(dense.end(), leg.begin() + 1, leg.end());
        cursor = leg.back();
        if (i < stopPack.size() && stopPack[i] >= 0 && std::size_t(stopPack[i]) < plan.Packs.size())
            plan.Packs[std::size_t(stopPack[i])].Yard = uint32(dense.size() - 1);
    }
    if (skipped)
        LOG_WARN("module.animus", "{}: {} field route: {} stops could not be walked from the one before and were "
            "passed over", _scenario.Name(), row.Name, skipped);

    // A route point every WingWaypointYards along it, the last boss's own position last; each point's yard.
    float const spacing = float(std::max<uint32>(5, _scenario.Tuning().Instance.WingWaypointYards));
    float walked = 0.0f;
    float next = spacing;
    for (std::size_t i = 1; i < dense.size(); ++i)
    {
        walked += dense[i - 1].GetExactDist(&dense[i]);
        if (walked >= next)
        {
            plan.Route.push_back(dense[i]);
            plan.RouteDense.push_back(uint32(i));
            next += spacing;
        }
    }
    plan.Route.emplace_back(boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
    plan.RouteDense.push_back(uint32(dense.size() - 1));
    plan.Dense = std::move(dense);
    plan.Field = true;
    {
        // The pull drill's rungs as this dungeon fills them (InstanceEncounter::StartDrill): packs walked to, by the
        // gap to the nearest creature not cleared before them.
        std::array<uint32, PULL_GAPS.size()> bands{};
        for (WingPack const& pack : plan.Packs)
            for (std::size_t rung = 0; rung < PULL_GAPS.size(); ++rung)
                if (pack.Yard && pack.Gap >= PULL_GAPS[rung])
                {
                    ++bands[rung];
                    break;
                }
        std::string list;
        for (std::size_t rung = 0; rung < PULL_GAPS.size(); ++rung)
            list += Acore::StringFormat("{}{} at {:.0f}+ yd", list.empty() ? "" : ", ", bands[rung], PULL_GAPS[rung]);
        LOG_INFO("module.animus", "{}: {} pull drill packs by gap: {}", _scenario.Name(), row.Name, list);
    }
    LOG_INFO("module.animus", "{}: the field route to {} ({}) is {} points ({:.0f} yd) from the door, through {} "
        "packs; {} creatures no seat can walk to are left out", _scenario.Name(), row.Name, row.Entry,
        plan.Route.size(),
        walked, packsReached, left);
    return plan;
}

void Animus::Curriculum::InstanceEncounter::UpdateWingEnemies(Env& env, EnvInstance& fight)
{
    Player* seat = _scenario.SeatBot(env, 0);
    Creature* boss = seat && !fight.Boss.IsEmpty() ? Encoding::CreatureThrough(*seat, fight.Boss) : nullptr;
    if (!seat || !seat->IsInWorld())
        return;

    // What the party can use near it -- a lever, a button, the Deadmines' cannon, a closed door it may open -- for the
    // crowd block's use action (CrowdBlock::ACTION_USE_OBJECT): a dungeon's way on is opened the way a player opens
    // it (2026-10-01: "they need to be able to use the proper actions to activate doors and cannons"). With
    // Instance.WingAutoDoors a closed door also opens by itself when a seat reaches it out of a fight, as it did
    // before the action existed.
    bool fighting = false;
    for (uint32 slot = 0; slot < env.Targets.size() && !fighting; ++slot)
        if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
            fighting = true;
    fight.Objects.clear();
    for (uint32 index = 0; index < _scenario.Data(env).ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        if (!bot || !bot->IsAlive() || !bot->IsInWorld())
            continue;
        std::list<GameObject*> objects;
        Acore::AllWorldObjectsInRange check(bot, OBJECT_SIGHT);
        Acore::GameObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, OBJECT_SIGHT);
        for (GameObject* object : objects)
        {
            if (!Usable(object)
                || std::find(fight.Used.begin(), fight.Used.end(), object->GetGUID()) != fight.Used.end())
                continue;
            if (std::find(fight.Objects.begin(), fight.Objects.end(), object->GetGUID()) == fight.Objects.end())
                fight.Objects.push_back(object->GetGUID());
            if (!fighting && _scenario.Tuning().Instance.WingAutoDoors && object->GetGoType() == GAMEOBJECT_TYPE_DOOR
                && bot->GetExactDist(object) <= DOOR_REACH)
                object->SetGoState(GO_STATE_ACTIVE);
        }
    }

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
            fight.LastKillMs = env.EpisodeElapsedMs;
            if (creature->IsDungeonBoss() || creature->isWorldBoss())
                ++fight.BossKills;
        }
    }

    // In the slots: what is fighting the party first, then the nearest of what stands ahead of it -- the next pack.
    // Seen from the tank, who leads and pulls, while it is up; seat 0 otherwise. From seat 0 the pack the tank was
    // about to pull was often not in the slots at all (2026-10-01).
    constexpr float WING_SIGHT = 45.0f;
    if (Player* tank = fight.Tank.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*seat, fight.Tank);
        tank && tank->IsAlive() && tank->IsInWorld() && tank->IsInMap(seat))
        seat = tank;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(seat, seat, WING_SIGHT);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(seat, units, check);
    Cell::VisitObjects(seat, searcher, WING_SIGHT);
    // A creature that cannot reach the party (below a ledge, across lava) or is running home is no fight: in and out
    // of combat with the party above Ragefire's drop at route point 30, it held the script's "fight" -- and the
    // party, imitating it -- for the rest of the run (2026-10-03).
    units.remove_if([](Unit* unit)
    {
        Creature const* creature = unit->ToCreature();
        return !unit->IsAlive() || unit->IsPlayer() || unit->IsTotem() || (creature && creature->IsEvadingAttacks());
    });
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

    // Past the slots (CrowdBlock): the next of them, fight first, and the nearest pack not in the fight with the
    // creatures standing within a pack's reach of it.
    constexpr float PACK_REACH = 12.0f;
    fight.Overflow.clear();
    fight.HasAhead = false;
    fight.AheadSize = 0;
    for (Unit* unit : units)
    {
        if (std::find(env.Targets.begin(), env.Targets.end(), unit->GetGUID()) != env.Targets.end())
            continue;
        if (fight.Overflow.size() < CROWD_SLOTS)
        {
            fight.Overflow.push_back(unit->GetGUID());
            if (std::find(fight.Watched.begin(), fight.Watched.end(), unit->GetGUID()) == fight.Watched.end())
                fight.Watched.push_back(unit->GetGUID());
        }
        if (!fight.HasAhead && !unit->IsInCombat())
        {
            fight.HasAhead = true;
            fight.Ahead.Relocate(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
        }
    }
    fight.HasSecond = false;
    if (fight.HasAhead)
        for (Unit* unit : units)
        {
            if (unit->IsInCombat())
                continue;
            if (unit->GetExactDist(&fight.Ahead) <= PACK_REACH)
                ++fight.AheadSize;
            else if (!fight.HasSecond)
            {
                // The units are nearest first: the first idle one past the pack ahead's reach.
                fight.HasSecond = true;
                fight.Second.Relocate(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
            }
        }
}

bool Animus::Curriculum::InstanceEncounter::StartDrill(Env& env, Map* map, WingPlan const& plan)
{
    EnvInstance& fight = _envs[env.Index];
    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    if (plan.Packs.empty() || fight.Dense.empty() || fight.RouteDense.size() != fight.Route.size())
        return false;

    // A pack of the rung: its nearest other creature at least the rung's gap away; any pack the route reaches when
    // none is.
    uint32 rung = 0;
    {
        std::lock_guard<std::mutex> guard(_drillLock);
        rung = _drillRung;
    }
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < plan.Packs.size(); ++i)
        if (plan.Packs[i].Yard && plan.Packs[i].Yard < fight.Dense.size() && plan.Packs[i].Gap >= PULL_GAPS[rung])
            open.push_back(i);
    if (open.empty())
        for (std::size_t i = 0; i < plan.Packs.size(); ++i)
            if (plan.Packs[i].Yard && plan.Packs[i].Yard < fight.Dense.size())
                open.push_back(i);
    if (open.empty())
        return false;
    std::size_t const chosen = open[urand(0, uint32(open.size()) - 1)];
    WingPack const& pack = plan.Packs[chosen];
    fight.DrillRung = rung;
    fight.DrillGap = std::min(pack.Gap, 999.0f);

    // The way there cleared, as a party that came from the door would have left it: every pack the route reaches
    // first. Their grids are loaded on purpose -- a creature on a grid nothing has walked yet is not in the store,
    // and would stand up alive behind the party.
    std::vector<ObjectGuid::LowType> cleared;
    for (std::size_t j = 0; j < chosen; ++j)
    {
        map->LoadGrid(plan.Packs[j].At.GetPositionX(), plan.Packs[j].At.GetPositionY());
        cleared.insert(cleared.end(), plan.Packs[j].Members.begin(), plan.Packs[j].Members.end());
    }
    map->LoadGrid(pack.At.GetPositionX(), pack.At.GetPositionY());
    std::sort(cleared.begin(), cleared.end());
    std::vector<ObjectGuid::LowType> own = pack.Members;
    std::sort(own.begin(), own.end());
    std::vector<Creature*> clearing;
    std::vector<Position> standing;
    Player* seat = _scenario.SeatBot(env, 0);
    uint32 alone = uint32(plan.Packs.size());
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
    {
        if (!creature || !creature->IsAlive())
            continue;
        if (std::binary_search(cleared.begin(), cleared.end(), spawnId))
            clearing.push_back(creature);
        else
        {
            if (std::binary_search(own.begin(), own.end(), spawnId))
                fight.DrillPack.push_back(creature->GetGUID());
            if (Hostile(seat, creature))
                standing.push_back(creature->GetPosition());
            uint32 group = alone;
            for (std::size_t j = chosen; j < plan.Packs.size() && group == alone; ++j)
                if (std::find(plan.Packs[j].Members.begin(), plan.Packs[j].Members.end(), spawnId)
                    != plan.Packs[j].Members.end())
                    group = uint32(j);
            if (group == alone)
                ++alone;
            fight.DrillGroups.emplace_back(creature->GetGUID(), group);
        }
    }
    for (Creature* creature : clearing)
        creature->DespawnOrUnsummon(0ms, TRASH_RESPAWN);
    if (fight.DrillPack.empty())
        return false;

    // Where the party stands: PullStartYards back along the route from the pack, and further back (to
    // PULL_START_MAX_YARDS) until nothing left standing is within PULL_START_CLEARANCE of it.
    auto const clear = [&standing](Position const& at)
    {
        return std::none_of(standing.begin(), standing.end(),
            [&at](Position const& creature) { return at.GetExactDist(&creature) < PULL_START_CLEARANCE; });
    };
    std::size_t yard = pack.Yard;
    float walked = 0.0f;
    while (yard > 0 && (walked < tuning.PullStartYards
        || (walked < PULL_START_MAX_YARDS && !clear(fight.Dense[yard]))))
    {
        walked += fight.Dense[yard].GetExactDist(&fight.Dense[yard - 1]);
        --yard;
    }
    Position start = fight.Dense[yard];
    start.SetOrientation(start.GetAngle(&pack.At));

    // The route from there: its next point the first past the start, and none past the pack's.
    fight.DrillPoint = uint32(fight.Route.size()) - 1;
    for (std::size_t i = 0; i < fight.RouteDense.size(); ++i)
        if (fight.RouteDense[i] >= pack.Yard)
        {
            fight.DrillPoint = uint32(i);
            break;
        }
    fight.RouteNext = fight.DrillPoint;
    for (std::size_t i = 0; i < fight.RouteDense.size(); ++i)
        if (fight.RouteDense[i] > yard)
        {
            fight.RouteNext = std::min(uint32(i), fight.DrillPoint);
            break;
        }

    EnvState& data = _scenario.Data(env);
    data.EpisodeSpawn = start;
    data.HasEpisodeSpawn = true;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        fight.Seats[index].Walk = fight.RouteNext;
        fight.Seats[index].DenseAt = uint32(yard);
        if (Player* bot = _scenario.SeatBot(env, index))
        {
            // Close together: the route may be a tunnel no wider than a few yards.
            Position at = start;
            at.m_positionX += float(int32(index % 3) - 1) * 1.0f;
            at.m_positionY += float(int32(index / 3) - 1) * 1.0f;
            BotFactory::TeleportWithinMap(bot, at);
        }
    }
    fight.ProgressMs = env.EpisodeElapsedMs;
    if (tuning.WingTrace)
        LOG_INFO("module.animus", "Pull drill start: env {} rung {} pack {} of {} ({} creatures, gap {:.0f} yd), {:.0f} yd "
            "back along the route, {} packs cleared before it", env.Index, rung, chosen + 1, plan.Packs.size(),
            fight.DrillPack.size(), fight.DrillGap, walked, chosen);
    return true;
}

void Animus::Curriculum::InstanceEncounter::UpdateDrill(Env& env, EnvInstance& fight)
{
    EnvState const& data = _scenario.Data(env);
    Player* anchor = nullptr;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive() && bot->IsInWorld()
            && (!anchor || bot->GetGUID() == fight.Tank))
            anchor = bot;
    if (!anchor)
        return;

    bool packAlive = false;
    for (ObjectGuid const& guid : fight.DrillPack)
        if (Creature const* creature = ObjectAccessor::GetCreature(*anchor, guid); creature && creature->IsAlive())
        {
            packAlive = true;
            fight.DrillEngaged = fight.DrillEngaged || creature->IsInCombat();
        }

    // What fights the party, from every living seat's side: a creature of the instance's own (not a summon) that is
    // not of the drill's pack is a second pack.
    constexpr float DRILL_SIGHT = 50.0f;
    std::vector<ObjectGuid> onParty;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        if (!bot || !bot->IsAlive() || !bot->IsInWorld())
            continue;
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, DRILL_SIGHT);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, DRILL_SIGHT);
        for (Unit* unit : units)
        {
            Creature const* creature = unit->ToCreature();
            if (!creature || !creature->IsAlive() || creature->IsSummon() || creature->IsTotem() || creature->IsPet())
                continue;
            Unit const* victim = creature->GetVictim();
            if (!victim || !victim->GetCharmerOrOwnerPlayerOrPlayerItself())
                continue;
            if (std::find(onParty.begin(), onParty.end(), creature->GetGUID()) != onParty.end())
                continue;
            onParty.push_back(creature->GetGUID());
            // The first creature on the party names the pack: the route's next or not, its whole pack is the pull.
            if (!fight.DrillLocked)
            {
                fight.DrillLocked = true;
                auto const of = std::find_if(fight.DrillGroups.begin(), fight.DrillGroups.end(),
                    [&creature](auto const& entry) { return entry.first == creature->GetGUID(); });
                if (std::find(fight.DrillPack.begin(), fight.DrillPack.end(), creature->GetGUID())
                    == fight.DrillPack.end())
                {
                    // Another pack; one the drill never saw (a grid loaded since) is a pack of its own.
                    fight.DrillOther = true;
                    fight.DrillPack.assign(1, creature->GetGUID());
                    if (of != fight.DrillGroups.end())
                        for (auto const& [guid, group] : fight.DrillGroups)
                            if (group == of->second && guid != creature->GetGUID())
                                fight.DrillPack.push_back(guid);
                }
            }
            if (!fight.DrillExtra
                && std::find(fight.DrillPack.begin(), fight.DrillPack.end(), creature->GetGUID()) == fight.DrillPack.end())
            {
                fight.DrillExtra = true;
                fight.DrillExtraEntry = creature->GetEntry();
            }
        }
    }
    fight.DrillPeak = std::max<uint32>(fight.DrillPeak, uint32(onParty.size()));
    if (!packAlive && !fight.Fighting && onParty.empty())
        fight.DrillCleared = true;
}

void Animus::Curriculum::InstanceEncounter::NoteDrill(uint32 rung, bool clean)
{
    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    std::lock_guard<std::mutex> guard(_drillLock);
    if (rung != _drillRung)
        return;
    std::size_t const window = std::max<uint32>(1, tuning.PullRungRuns);
    _drillRuns.push_back(clean);
    while (_drillRuns.size() > window)
        _drillRuns.pop_front();
    if (_drillRuns.size() < window || _drillRung + 1 >= PULL_GAPS.size())
        return;
    float const share = float(std::count(_drillRuns.begin(), _drillRuns.end(), true)) / float(_drillRuns.size());
    if (share < tuning.PullRungTarget)
        return;
    ++_drillRung;
    _drillRuns.clear();
    LOG_INFO("module.animus", "{}: the pull drill steps to rung {} ({:.0f}% clean on the last {}): packs with another "
        "{:.0f} yd or more away", _scenario.Name(), _drillRung, share * 100.0f, window, PULL_GAPS[_drillRung]);
}

void Animus::Curriculum::InstanceEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    // A wing's route: the next point is where the party is going, a TravelTo target (GoalBlock's assignment slot).
    EnvInstance const& fight = _envs[env.Index];
    if (!Wing(env) || fight.Route.empty())
        return;
    if (seat < fight.Seats.size())
    {
        view.FoodItem = fight.Seats[seat].FoodItem;
        view.DrinkItem = fight.Seats[seat].DrinkItem;
    }
    CrowdView& crowd = view.Crowd;
    crowd.Present = true;
    crowd.OnParty = fight.OnParty;
    crowd.OnTank = fight.OnTank;
    crowd.Elites = fight.Elites;
    if (view.Bot)
    {
        crowd.Tank = fight.Tank.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*view.Bot, fight.Tank);
        for (ObjectGuid const& guid : fight.Overflow)
            if (Unit* unit = ObjectAccessor::GetUnit(*view.Bot, guid); unit && crowd.Count < CROWD_SLOTS)
                crowd.Units[crowd.Count++] = unit;
    }
    crowd.Used = &fight.Used;
    if (view.Bot)
        for (ObjectGuid const& guid : fight.Objects)
            if (GameObject* object = ObjectAccessor::GetGameObject(*view.Bot, guid); object && Usable(object)
                && CrowdBlock::CanUse(view.Bot, object)
                && (!crowd.Object || view.Bot->GetExactDist(object) < view.Bot->GetExactDist(crowd.Object)))
                crowd.Object = object;
    crowd.HasAhead = fight.HasAhead;
    crowd.Ahead = fight.Ahead;
    crowd.AheadSize = fight.AheadSize;
    crowd.HasSecond = fight.HasSecond;
    crowd.Second = fight.Second;
    crowd.Still = float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, fight.ProgressMs)) / 120000.0f;
    view.HasObjective = true;
    // The seat's own place on the route (SeatInstance::Walk): the tank walks it up to the route's next point, the
    // others up to the tank's place, point by point from wherever they stood up -- after a wipe stood the party up at
    // the door the next point was hundreds of yards on through the rock, and the seats walked into walls towards it.
    // A seat that has caught up with the tank's place goes to the tank itself: the tank waits short of its next
    // point for the party, and a seat sent to that point waited there for the tank (2026-10-01).
    std::size_t const last = std::min<std::size_t>(fight.RouteNext, fight.Route.size() - 1);
    std::size_t walk = last;
    std::size_t tankWalk = last;
    for (uint32 index = 0; index < MAX_SEATS; ++index)
        if (Player* bot = _scenario.SeatBot(env, index))
        {
            if (bot == view.Bot)
                walk = std::min<std::size_t>(fight.Seats[index].Walk, last);
            if (bot->GetGUID() == fight.Tank)
                tankWalk = std::min<std::size_t>(fight.Seats[index].Walk, last);
        }
    view.Objective = fight.Route[walk];
    // A pull coming in: the tank's objective is a route point behind it, back where the party waits, so what it pulled
    // is fought there and not beside the next pack (2026-10-01: eight enemies at once at the mine's entrance, the
    // packs pulled where they stood). Only while something attacking the tank is still more than PULL_BACK_YARDS out.
    constexpr float PULL_BACK_YARDS = 10.0f;
    if (view.Bot && view.Crowd.Tank == view.Bot && fight.Fighting)
    {
        bool coming = false;
        for (uint32 slot = 0; slot < env.Targets.size() && !coming; ++slot)
            if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->GetVictim() == view.Bot
                && enemy->IsInMap(view.Bot) && view.Bot->GetExactDist(enemy) > PULL_BACK_YARDS)
                coming = true;
        if (coming)
            view.Objective = fight.Route[walk > 0 ? walk - 1 : 0];
    }
    // Behind the party: the route's next point (the party's) is well ahead of the seat's own place -- the tank that
    // rose at the door, walking back to a party that is still deep in.
    view.Crowd.Behind = walk + 2 < last;
    Unit const* tank = view.Crowd.Tank;
    if (view.Bot && tank && tank != view.Bot && tank->IsAlive() && tank->IsInMap(view.Bot) && walk >= tankWalk)
        view.Objective.Relocate(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());

    // Where the advance action steps: a few yards on along the field route towards the objective, from the yard
    // of it the seat stands on. Off the route, nothing: the server's path to the objective is the way back to it.
    if (view.Bot && seat < fight.Seats.size() && !fight.Dense.empty() && fight.RouteDense.size() == fight.Route.size())
    {
        constexpr int32 STEP_YARDS = 6;
        constexpr float ON_ROUTE = 6.0f;
        auto const nearestYard = [&fight](Position const& at, uint32 hint)
        {
            std::size_t const size = fight.Dense.size();
            auto const search = [&](std::size_t first, std::size_t last)
            {
                std::pair<uint32, float> best{ uint32(first), std::numeric_limits<float>::max() };
                for (std::size_t i = first; i < last; ++i)
                    if (float const d = at.GetExactDist(&fight.Dense[i]); d < best.second)
                        best = { uint32(i), d };
                return best;
            };
            std::size_t const from = hint > 60 ? hint - 60 : 0;
            std::pair<uint32, float> best = search(from, std::min(size, std::size_t(hint) + 120));
            if (best.second > ON_ROUTE * 2.0f)
                best = search(0, size);
            return best;
        };
        SeatInstance const& own = fight.Seats[seat];
        auto const [yard, off] = nearestYard(Position(view.Bot->GetPositionX(), view.Bot->GetPositionY(),
            view.Bot->GetPositionZ()), own.DenseAt);
        own.DenseAt = yard;
        uint32 const hint = fight.RouteDense[std::min<std::size_t>(walk, fight.RouteDense.size() - 1)];
        uint32 const target = nearestYard(view.Objective, hint).first;
        if (off <= ON_ROUTE)
        {
            own.Detour.clear();
            if (target != yard)
            {
                int32 const delta = std::clamp(int32(target) - int32(yard), -STEP_YARDS, STEP_YARDS);
                view.Crowd.HasStep = true;
                view.Crowd.Step = fight.Dense[std::size_t(int32(yard) + delta)];
            }
        }
        else if (!view.Bot->IsInCombat())
        {
            // Off the route and out of a fight: the seat's own way back over the field, lava and all, replanned
            // every few seconds as it walks. The server's path cannot leave a cavern its navmesh does not join.
            constexpr uint32 DETOUR_REPLAN_MS = 5000;
            constexpr uint32 DETOUR_NODES = 200000;
            Position const at(view.Bot->GetPositionX(), view.Bot->GetPositionY(), view.Bot->GetPositionZ());
            if (own.Detour.empty() || env.EpisodeElapsedMs >= own.DetourMs + DETOUR_REPLAN_MS)
            {
                own.DetourMs = env.EpisodeElapsedMs;
                if (!FieldRoute::Plan(fight.MapId, at, fight.Dense[target], own.Detour, DETOUR_NODES))
                    own.Detour.clear();
            }
            if (!own.Detour.empty())
            {
                std::size_t nearest = 0;
                float best = std::numeric_limits<float>::max();
                for (std::size_t i = 0; i < own.Detour.size(); ++i)
                    if (float const d = at.GetExactDist(&own.Detour[i]); d < best)
                    {
                        best = d;
                        nearest = i;
                    }
                view.Crowd.HasStep = true;
                view.Crowd.Step = own.Detour[std::min(own.Detour.size() - 1, nearest + std::size_t(STEP_YARDS))];
            }
        }
    }
}

void Animus::Curriculum::InstanceEncounter::RewardWing(Env& env, uint32 seatIndex, Player* bot,
    RewardLedger& ledger)
{
    EnvInstance& fight = _envs[env.Index];
    SeatInstance& paid = fight.Seats[seatIndex];
    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    float const tierScale = TierScale(env);
    uint32 const waypoints = std::min<uint32>(fight.RouteNext, uint32(fight.Route.size()));

    // Standing still in a dungeon costs (Instance.WingStall): once WingStallGraceMs pass with nothing killed, no
    // step along the route and nothing fighting the party, every seat pays by the second. Without it a party at the
    // door paid only the timeout, and early in training that beat fighting through half the dungeon.
    if (seatIndex == 0)
    {
        uint32 const seen = fight.TrashKills + waypoints;
        bool engaged = false;
        for (uint32 slot = 0; slot < env.Targets.size() && !engaged; ++slot)
            if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
                engaged = true;
        if (seen != fight.ProgressSeen || engaged)
        {
            fight.ProgressSeen = seen;
            fight.ProgressMs = env.EpisodeElapsedMs;
        }
    }
    // The tank decides when the party moves on, so standing about is its to pay, the others' at WingStallOthers of it:
    // charged alike, -177 a run swamped every seat's own role terms (a healer's healing paid 8.5), 2026-10-03.
    bool const tankSeat = _scenario.Data(env).Seats[seatIndex].DungeonRole == DUNGEON_TANK;
    if (env.EpisodeElapsedMs > fight.ProgressMs + (fight.Drill ? tuning.PullGraceMs : tuning.WingStallGraceMs))
        ledger.Add(RewardTerm::Stall, -tuning.WingStall * (tankSeat ? 1.0f : tuning.WingStallOthers)
            * float(_scenario.DecisionMs()) / 1000.0f);
    if (tankSeat && fight.ReadyEngages > paid.EngagesPaid)
    {
        ledger.Add(RewardTerm::Threat, tuning.WingEngage * tierScale * float(fight.ReadyEngages - paid.EngagesPaid));
        paid.EngagesPaid = fight.ReadyEngages;
    }
    // More on the party than a pack (Instance.WingCrowd, past WingCrowdFree): a pull that ran into the next.
    if (fight.OnParty > tuning.WingCrowdFree)
        ledger.Add(RewardTerm::Threat, -tuning.WingCrowd * float(fight.OnParty - tuning.WingCrowdFree)
            * float(_scenario.DecisionMs()) / 1000.0f);
    // Away from the leader (Instance.WingStray): a seat other than the tank further than WingStrayYards from it.
    if (bot && bot->IsAlive() && !fight.Tank.IsEmpty() && bot->GetGUID() != fight.Tank)
        if (Unit* tank = ObjectAccessor::GetUnit(*bot, fight.Tank); tank && tank->IsAlive() && tank->IsInMap(bot)
            && bot->GetExactDist(tank) > tuning.WingStrayYards)
            ledger.Add(RewardTerm::Approach, -tuning.WingStray * float(_scenario.DecisionMs()) / 1000.0f);
    if (bot && bot->IsAlive())
    {
        ledger.Add(RewardTerm::Kill, tuning.WingTrashKill * tierScale * float(fight.TrashKills - paid.KillsPaid));
        ledger.Add(RewardTerm::Approach, tuning.WingWaypoint * tierScale * float(waypoints - paid.WaypointsPaid));
        ledger.Add(RewardTerm::Kill, tuning.WingMidBoss * tierScale * float(fight.BossKills - paid.BossKillsPaid));

        // Forward through the dungeon, paid as it is walked (Instance.WingProgress over the whole route): the
        // potential is the route still ahead -- to the next point, then along the route from it. The route points
        // alone were a coarse signal, and the parties stood at the door. Paid once, for ground the seat had not
        // reached before, and only out of a fight: walking past a pack still fighting paid it to pull the next one.
        if (!fight.Route.empty() && fight.RouteNext < fight.Route.size() && !fight.RouteRemain.empty()
            && !fight.Fighting)
        {
            std::size_t const next = fight.RouteNext;
            float const total = std::max(1.0f, fight.RouteRemain.front());
            float const ahead = bot->GetExactDist(&fight.Route[next]) + fight.RouteRemain[next];
            float const potential = -ahead / total;
            if (paid.PotentialReady && potential > paid.Potential)
                ledger.Add(RewardTerm::Approach, tuning.WingProgress * tierScale * (potential - paid.Potential));
            if (!paid.PotentialReady || potential > paid.Potential)
                paid.Potential = potential;
            paid.PotentialReady = true;
        }
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

    bool const over = fight.BossDead || fight.Wiped || TimeIsUp(env)
        || (fight.Drill && (fight.DrillCleared || fight.DrillExtra));
    if (!over || paid.OutcomePaid)
        return;
    paid.OutcomePaid = true;
    // A pull drill's outcome, the tank's in full and the others' at PullOthers: the pack alone and dead, a second
    // pack in the fight, or the clock out with the pack still standing. A wipe is paid above.
    if (fight.Drill)
    {
        float const share = tankSeat ? 1.0f : tuning.PullOthers;
        if (fight.DrillExtra)
            ledger.Add(RewardTerm::Threat, -tuning.PullExtra * share / tierScale);
        else if (fight.DrillCleared)
            ledger.Add(RewardTerm::Kill, tuning.PullClean * share * tierScale);
        else if (TimeIsUp(env))
            ledger.Add(RewardTerm::Timeout, -tuning.PullTimeout * share / tierScale);
        return;
    }
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
        ledger.Add(RewardTerm::Timeout, -tuning.WingTimeout * (1.0f - float(waypoints) / float(fight.Route.size()))
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
        return fight.BossDead || fight.Wiped || TimeIsUp(env)
            || (fight.Drill && (fight.DrillCleared || fight.DrillExtra));
    return fight.BossDead || fight.Wiped || fight.Evaded || TimeIsUp(env);
}
