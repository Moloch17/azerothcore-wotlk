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
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "EntranceRespawn.h"
#include "Env.h"
#include "GameObject.h"
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
#include "Player.h"
#include "StageScenario.h"
#include "Supplies.h"
#include "MentalMap.h"
#include "EntityMemory.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

Animus::Curriculum::EnemyRank Animus::Curriculum::RankEnemy(Unit const* enemy, Unit const* tank)
{
    if (!enemy || !enemy->IsAlive())
        return EnemyRank::Gone;
    if (!enemy->IsInCombat())
        return EnemyRank::Standing;
    if (tank && tank->IsAlive() && tank->GetVictim() == enemy)
        return EnemyRank::TankTarget;
    Unit const* victim = enemy->GetVictim();
    return victim && victim->IsPlayer() ? EnemyRank::OnPlayer : EnemyRank::Fighting;
}

namespace
{
    constexpr float OBJECT_SIGHT = 40.0f;      // the party sees what it can use this far

    /// The item a lock is opened with (LOCK_KEY_ITEM), or 0: the Deadmines' cannon takes the Defias Gunpowder.
    uint32 KeyOf(GameObject const* object)
    {
        LockEntry const* lock = sLockStore.LookupEntry(object->GetGOInfo()->GetLockId());
        if (!lock)
            return 0;
        for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i])
                return lock->Index[i];
        return 0;
    }

    /// Whether `bot` can use `object` as it stands: a key it needs is carried.
    bool CanUse(Player const* bot, GameObject const* object)
    {
        uint32 const key = KeyOf(object);
        return !key || bot->HasItemCount(key, 1);
    }

    /// Every gameobject within `range` of a point, flat: UpdateWingEnemies' one visit from the party's middle.
    class GameObjectsNearPoint
    {
    public:
        GameObjectsNearPoint(float x, float y, float range) : _x(x), _y(y), _range(range) { }
        bool operator()(GameObject* object) const { return object->GetExactDist2d(_x, _y) <= _range; }

    private:
        float _x;
        float _y;
        float _range;
    };
    /// How far from its spawn a boss is looked for by entry.
    constexpr float BOSS_SEARCH_YARDS = 100.0f;
    /// A boss back at full health out of combat after having been engaged has evaded: the fight is lost.
    constexpr float EVADED_HEALTH_PCT = 99.0f;
    /// The fights started ready (ReadyEngages) the party is paid for: once a run (ReadyPull).
    constexpr uint32 READY_PULLS_PAID = 1;
}

Animus::Curriculum::InstanceEncounter::InstanceEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
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
    return { RewardTerm::StepCost, RewardTerm::Kill, RewardTerm::Death, RewardTerm::Timeout, RewardTerm::Threat,
        RewardTerm::Clear, RewardTerm::ReadyPull, RewardTerm::Idle, RewardTerm::Lost, RewardTerm::Away };
}

void Animus::Curriculum::InstanceEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // The rung (the ladder's tier, which the evaluation spreads its seeds over) and the fight's outcome. The rung
    // is `difficulty` -- the column the convergence rule's ladder signal reads; `boss_rung` is the same.
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    // The difficulty ladder's rung comes first of the `_rung` columns (the evaluation videos read the first: Vision::
    // EvalVideoRungColumn) -- the row a stage pins (boss_rung) is the same every run.
    table.Add("wing_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    // At the top of the difficulty ladder (the learner's convergence.top_rung: a stage converges on its real task,
    // never on an easier rung) -- the evaluation's conditions. A stage's pinned row (difficulty) is the same every run
    // and says nothing.
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Rung + 1 >= StageScenario::WING_RUNGS.size() ? 1.0f : 0.0f;
    });
    table.Add("boss_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add("instance_map", [this](Env const& env, uint32) { return float(_envs[env.Index].MapId); });
    table.Add("boss_entry", [this](Env const& env, uint32) { return float(_envs[env.Index].Entry); });
    table.Add("boss_killed", [this](Env const& env, uint32) { return _envs[env.Index].BossDead ? 1.0f : 0.0f; });
    table.Add("boss_health_left", [this](Env const& env, uint32) { return _envs[env.Index].HealthLeft; });
    table.Add("engaged", [this](Env const& env, uint32) { return _envs[env.Index].Engaged ? 1.0f : 0.0f; });
    table.Add("wiped", [this](Env const& env, uint32) { return _envs[env.Index].Wiped ? 1.0f : 0.0f; });
    table.Add("evaded", [this](Env const& env, uint32) { return _envs[env.Index].Evaded ? 1.0f : 0.0f; });
    table.Add("wing_trash_kills", [this](Env const& env, uint32) { return float(_envs[env.Index].TrashKills); });
    table.Add("wing_boss_kills", [this](Env const& env, uint32) { return float(_envs[env.Index].BossKills); });
    table.Add("wing_wipes", [this](Env const& env, uint32) { return float(_envs[env.Index].Wipes); });
    table.Add("wing_cleared_share", [this](Env const& env, uint32) { return ClearedShare(_envs[env.Index]); });
    table.Add("wing_crowd_seconds", [this](Env const& env, uint32) { return _envs[env.Index].CrowdSeconds; });
    table.Add("wing_probe", [this](Env const& env, uint32) { return _envs[env.Index].Probe ? 1.0f : 0.0f; });
    table.Add("wing_rises", [this](Env const& env, uint32) { return float(_envs[env.Index].Rises); });
    table.Add("wing_rejoins", [this](Env const& env, uint32) { return float(_envs[env.Index].Rejoins); });
    table.Add("wing_rejoin_seconds", [this](Env const& env, uint32)
    {
        EnvInstance const& fight = _envs[env.Index];
        return fight.Rejoins ? float(fight.RejoinMsTotal) / float(fight.Rejoins) / 1000.0f : 0.0f;
    });
    table.Add("wing_level", [this](Env const& env, uint32) { return float(_scenario.Data(env).EpisodeLevel); });
    // **The party stages' measures** (dungeon-curriculum D2, D3, 2026-10-07). The run's success as the stage counts
    // it (`cleared`: the last boss dead), the full clear (every creature a clear counts, and the last boss) and the
    // Deadmines' bar (a clear with at most one wipe); the pulls started ready.
    table.Add("cleared", [this](Env const& env, uint32) { return Succeeded(_envs[env.Index]) ? 1.0f : 0.0f; });
    table.Add("full_clear", [this](Env const& env, uint32) { return FullClear(_envs[env.Index]) ? 1.0f : 0.0f; });
    table.Add("bar_clear", [this](Env const& env, uint32)
    {
        EnvInstance const& fight = _envs[env.Index];
        return Succeeded(fight) && fight.Wipes <= 1 ? 1.0f : 0.0f;
    });
    table.Add("ready_pulls", [this](Env const& env, uint32)
    {
        EnvInstance const& fight = _envs[env.Index];
        return float(std::min(fight.ReadyEngages, READY_PULLS_PAID));
    });
    // **H, the stand-in split**: the run's success beside the "human" stand-in, and without it -- each a per-event
    // column over the episodes that had (with_stand_in) or had not (without_stand_in) the stand-in in a seat, so
    // their means are the two clear rates (episode_means.PER_EVENT); the plan's measure is the gap between them.
    table.Add("without_stand_in", [this](Env const& env, uint32)
    {
        return _scenario.Data(env).StandInPlay.Seat >= 0 ? 0.0f : 1.0f;
    });
    table.Add("clear_standin", [this](Env const& env, uint32)
    {
        return _scenario.Data(env).StandInPlay.Seat >= 0 && Succeeded(_envs[env.Index]) ? 1.0f : 0.0f;
    });
    table.Add("clear_allbot", [this](Env const& env, uint32)
    {
        return _scenario.Data(env).StandInPlay.Seat < 0 && Succeeded(_envs[env.Index]) ? 1.0f : 0.0f;
    });
    // **By role** (D3: "by role", the seat's place in the party read off its build:
    // StageScenario::FitsDungeonRole):
    // which place the row's seat had, and its deaths -- per-event columns over the rows of each place.
    static constexpr std::array<std::pair<char const*, uint8>, 3> ROLES = { {
        { "tank", DUNGEON_TANK }, { "healer", DUNGEON_HEALER }, { "damage", DUNGEON_DAMAGE } } };
    for (auto const& [name, role] : ROLES)
    {
        uint8 const place = role;
        table.Add(Acore::StringFormat("role_{}", name), [this, place](Env const& env, uint32 seat)
        {
            return seat < MAX_SEATS && _scenario.Data(env).Seats[seat].DungeonRole == place ? 1.0f : 0.0f;
        });
        table.Add(Acore::StringFormat("deaths_{}", name), [this, place](Env const& env, uint32 seat)
        {
            return seat < MAX_SEATS && _scenario.Data(env).Seats[seat].DungeonRole == place
                ? float(_envs[env.Index].Seats[seat].Deaths) : 0.0f;
        });
    }
    table.Add("seat_deaths", [this](Env const& env, uint32 seat)
    {
        return seat < MAX_SEATS ? float(_envs[env.Index].Seats[seat].Deaths) : 0.0f;
    });
    // **Per boss** (D2, D3: "per boss"): each boss of the stage's dungeons (WingBosses), killed this run or not.
    std::vector<uint32> maps;
    for (ArenaDefinition const& arena : _scenario.Stage().Arenas)
        if (arena.Instance == InstanceLadder::Wing && arena.InstanceRow >= 0
            && std::size_t(arena.InstanceRow) < InstanceLadderRows(InstanceLadder::Wing).size())
        {
            uint32 const map = InstanceLadderRows(InstanceLadder::Wing)[std::size_t(arena.InstanceRow)].MapId;
            if (std::find(maps.begin(), maps.end(), map) == maps.end())
                maps.push_back(map);
        }
    for (WingBoss const& boss : WingBosses())
    {
        if (std::find(maps.begin(), maps.end(), boss.MapId) == maps.end())
            continue;
        uint32 const entry = boss.Entry;
        table.Add(Acore::StringFormat("boss_{}", boss.Name), [this, entry](Env const& env, uint32)
        {
            EnvInstance const& fight = _envs[env.Index];
            std::vector<uint32> const& kills = fight.BossesKilled;
            bool const killed = (fight.BossDead && fight.Entry == entry)
                || std::find(kills.begin(), kills.end(), entry) != kills.end();
            return killed ? 1.0f : 0.0f;
        });
    }
}

void Animus::Curriculum::InstanceEncounter::ResetEpisode(Env& env)
{
    EnvInstance& fight = _envs[env.Index];
    // One line a finished run (Instance.WingTrace): how far it got, what it killed and how it ended.
    bool const built = !fight.Boss.IsEmpty();
    if (built && _scenario.Tuning().Instance.WingTrace)
        LOG_INFO("module.animus", "Wing run: env {} {}{} rung {} level {} | {} of {} creatures killed, {} bosses, last "
            "boss {} | {} wipes, {} rises, {} rejoined | {:.0f}s with no progress at the end | {}",
            env.Index, fight.Evaluating ? "eval" : "train", fight.Probe ? " probe" : "", fight.Rung, fight.Level,
            fight.TrashKills, fight.HostileTotal, fight.BossKills, fight.BossDead ? "killed" : "alive", fight.Wipes,
            fight.Rises, fight.Rejoins, float(fight.LastMs - std::min(fight.LastMs, fight.ProgressMs)) / 1000.0f,
            fight.BossDead ? "cleared" : fight.Wiped ? "wiped" : "out of time");
    // The run just ended counts toward the support's running share (the training runs of a whole dungeon only; the
    // probes are the policy's own).
    if (built && !fight.Evaluating)
        _scenario.NoteWingRun(fight.Rung, fight.Probe, fight.BossDead ? 1.0f : ClearedShare(fight));
    fight = EnvInstance();
}

void Animus::Curriculum::InstanceEncounter::BeforeLevel(Env& env)
{
    // The arena's row fixes the map, the level and the difficulty the seats are built for, which is why this runs
    // before the level is drawn.
    EnvState& data = _scenario.Data(env);
    EnvInstance& fight = _envs[env.Index];
    std::vector<BossRow const*> const& rows = Rows(env);
    if (rows.empty())
        return;

    // An arena runs its pinned row (ArenaDefinition::InstanceRow) every time, from the door.
    ArenaDefinition const& arena = _scenario.Arena(env);
    uint32 const pinned = uint32(arena.InstanceRow);
    fight.Tier = std::min<uint32>(pinned, uint32(rows.size()) - 1);
    fight.Row = rows[fight.Tier];
    fight.MapId = fight.Row->MapId;
    fight.Entry = fight.Row->Entry;

    data.EpisodeMapId = fight.Row->MapId;
    data.HasEpisodeMap = true;
    data.EpisodeLevel = fight.Row->Level;
    // A whole dungeon is run by characters of its own level range: the dungeon finder's target range for the map
    // and difficulty (LFGDungeons.dbc), a level drawn in it every run. The row's level is the fallback.
    // Training runs it at the difficulty ladder's rung (StageScenario::WING_RUNGS): above that range, with wipes to
    // spare; an evaluation as it is.
    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    fight.Evaluating = env.Evaluating;
    fight.Rung = env.Evaluating ? uint32(StageScenario::WING_RUNGS.size()) - 1 : _scenario.WingRungNow();
    StageScenario::WingRung const& rung = StageScenario::WING_RUNGS[fight.Rung];
    fight.Probe = !env.Evaluating && frand(0.0f, 1.0f) < tuning.WingProbe;
    fight.WipesAllowed = tuning.WingWipes + rung.ExtraWipes;
    // The arena's own band (the Deadmines' bar: 17-20) where it names one, else the dungeon finder's range.
    auto const [low, high] = arena.LevelFirst ? std::pair<uint32, uint32>(arena.LevelFirst, arena.LevelLast)
        : DungeonLevels(*fight.Row);
    data.EpisodeLevel = uint8(std::min<uint32>(urand(low, high) + rung.Lift, DEFAULT_MAX_LEVEL));

    MapEntry const* mapEntry = sMapStore.LookupEntry(fight.Row->MapId);
    bool const raid = mapEntry && mapEntry->IsRaid();
    data.DungeonDifficulty = raid ? 0 : fight.Row->Difficulty;
    data.RaidDifficulty = raid ? fight.Row->Difficulty : 0;

    // The seats spawn at the instance's front door, as a group that walked in would.
    AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(fight.Row->MapId);
    data.EpisodeSpawn.Relocate(entrance->target_X, entrance->target_Y, entrance->target_Z, entrance->target_Orientation);
    data.HasEpisodeSpawn = true;
    fight.Entrance = data.EpisodeSpawn;
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
    fight.Boss = boss->GetGUID();
    fight.BossHealth = std::max<uint32>(1, boss->GetMaxHealth());

    EnvState& data = _scenario.Data(env);
    // The party stays at the door with the trash alive. The creatures a full clear kills: every hostile one the
    // instance holds, bosses among them (an instance loads all its grids when it is created).
    fight.HostileTotal = 0;
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
        if (Hostile(seat, creature))
            ++fight.HostileTotal;
    env.Targets.clear();
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
            // What a lock on the way takes -- the Deadmines' cannon its gunpowder -- carried from the door: nothing
            // is looted (the user, 2026-10-06), and the key is used on the lock with a real press (CMSG_USE_ITEM).
            for (uint32 key : KeyItems(fight.MapId))
                if (!bot->HasItemCount(key, 1))
                    bot->AddItem(key, 1);
        }
    return true;
}

void Animus::Curriculum::InstanceEncounter::UpdateEnemies(Env& env)
{
    UpdateWingEnemies(env, _envs[env.Index]);
}

void Animus::Curriculum::InstanceEncounter::Update(Env& env)
{
    EnvInstance& fight = _envs[env.Index];
    Player* seat = _scenario.SeatBot(env, 0);
    Creature* boss = seat && !fight.Boss.IsEmpty() ? Encoding::CreatureThrough(*seat, fight.Boss) : nullptr;
    if (!boss)
        return;

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
        && env.EpisodeElapsedMs > fight.EngageMs + _scenario.StepMs(env))
        fight.Evaded = true;

    // A wipe: no seat left standing. Nobody stands up in an instance; the dead wait for the episode to end.
    EnvState const& data = _scenario.Data(env);
    bool anyoneAlive = false;
    for (uint32 index = 0; index < data.ActiveSeats && !anyoneAlive; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive())
            anyoneAlive = true;
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
    fight.LastMs = env.EpisodeElapsedMs;
    fight.Level = uint32(data.EpisodeLevel);

    // The clock ran out (Instance.WingTrace): what the party was doing -- what is in combat around it, where, on whom.
    if (_scenario.Tuning().Instance.WingTrace && !fight.EndLogged && TimeIsUp(env))
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
        LOG_INFO("module.animus", "Wing time: env {}, fighting {}, {} on the party, last kill {:.0f}s "
            "ago, in combat around:{}", env.Index, fight.Fighting ? 1 : 0, fight.OnParty,
            float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, fight.LastKillMs)) / 1000.0f,
            around.empty() ? " nothing" : around);
    }

    // A wipe -- nobody standing -- counts once (Instance.WingWipes and more while the support lasts); past the run's
    // allowance it ends the run. Short of it, the party rises at the entrance as any death does, below.
    if (fight.Wipe.Note(anyoneAlive, fight.BossDead))
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
    }
    RiseDead(env, fight);

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
                    && CanUse(tank, object) && tank->GetExactDist(object) <= NEAR_OBJECT_YARDS
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

    // A run stuck for two minutes, and every ten after (Instance.WingTrace): each seat's state against the tank's, so a
    // party that stands still says why.
    constexpr uint32 STUCK_FIRST_MS = 120000;
    constexpr uint32 STUCK_EVERY_MS = 600000;
    uint32 const still = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, fight.ProgressMs);
    if (_scenario.Tuning().Instance.WingTrace && still >= STUCK_FIRST_MS && env.EpisodeElapsedMs >= fight.StuckLoggedMs)
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
                // What it pressed, by name.
                std::string presses;
                if (state.L)
                {
                    std::vector<std::string> const names = state.L->ActionNames();
                    auto const name = [&names](int32 action)
                    {
                        return action >= 0 && std::size_t(action) < names.size() ? names[std::size_t(action)]
                            : std::string("-");
                    };
                    presses = Acore::StringFormat(" pressed {}", name(state.Pressed));
                }
                seats += Acore::StringFormat("{}[{}{} hp {:.0f}% mana {} {:.0f}yd at ({:.0f} {:.0f} {:.0f}){}{}{}]",
                    seats.empty() ? "" : " ", index, bot == tank ? " tank" : "", bot->GetHealthPct(),
                    maxMana ? std::to_string(bot->GetPower(POWER_MANA) * 100 / maxMana) + "%" : "-",
                    tank && tank->IsInMap(bot) ? bot->GetExactDist(tank) : -1.0f, bot->GetPositionX(),
                    bot->GetPositionY(), bot->GetPositionZ(), bot->IsAlive() ? "" : " dead",
                    bot->IsInCombat() ? " combat" : "", presses);
            }
        std::string objects;
        if (tank)
            for (ObjectGuid const& guid : fight.Objects)
                if (GameObject* object = ObjectAccessor::GetGameObject(*tank, guid))
                    objects += Acore::StringFormat("{}{} type {} state {} {:.0f}yd", objects.empty() ? "" : ", ",
                        object->GetEntry(), uint32(object->GetGoType()), uint32(object->GetGoState()),
                        tank->GetExactDist(object));
        seats += " | objects: " + (objects.empty() ? std::string("none") : objects);
        if (Player* tankPlayer = tank ? tank->ToPlayer() : nullptr)
        {
            std::string fighters;
            for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
                if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
                    fighters += Acore::StringFormat(" {}({} on {})", enemy->GetEntry(),
                        tankPlayer->GetExactDist(enemy), enemy->GetVictim() ? enemy->GetVictim()->GetName() : "nobody");
            seats += Acore::StringFormat(" | fighting {}{}, moving {}", fight.Fighting ? 1 : 0, fighters,
                tankPlayer->isMoving() ? 1 : 0);
        }
        LOG_INFO("module.animus", "Wing stuck: env {} {:.0f}s still, {} on the party, pack ahead {} | {}", env.Index,
            float(still) / 1000.0f, fight.OnParty, fight.HasAhead && tank
                ? Acore::StringFormat("{:.0f} yd ({})", tank->GetExactDist(&fight.Ahead), fight.AheadSize)
                : std::string("none"), seats);
    }
}

void Animus::Curriculum::InstanceEncounter::RiseDead(Env& env, EnvInstance& fight)
{
    // The dead rejoin (dungeon-curriculum I4; the user, 2026-10-06: no graveyard): a seat that dies is out for
    // Respawn.DelayMs, then is alive at the instance's entrance and walks back to the party on the controller --
    // its own walk, never a teleport to the party. It has rejoined
    // within Respawn.RejoinYards of the party: the tank, or the living party's middle while the tank is down.
    CurriculumTuning::RespawnTuning const& tuning = _scenario.Tuning().Respawn;
    EnvState& data = _scenario.Data(env);
    Player* tank = nullptr;
    for (uint32 index = 0; index < data.ActiveSeats && !tank; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->GetGUID() == fight.Tank && bot->IsAlive()
            && bot->IsInWorld())
            tank = bot;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* bot = _scenario.SeatBot(env, index);
        if (!bot || !bot->IsInWorld())
            continue;
        SeatInstance& seatState = fight.Seats[index];
        float partyYards = -1.0f;
        if (bot->IsAlive())
        {
            if (tank && tank != bot && tank->IsInMap(bot))
                partyYards = bot->GetExactDist(tank);
            else
            {
                float x = 0.0f;
                float y = 0.0f;
                uint32 others = 0;
                for (uint32 other = 0; other < data.ActiveSeats; ++other)
                    if (Player* member = _scenario.SeatBot(env, other); member && member != bot && member->IsAlive()
                        && member->IsInMap(bot))
                    {
                        x += member->GetPositionX();
                        y += member->GetPositionY();
                        ++others;
                    }
                if (others)
                    partyYards = bot->GetExactDist2d(x / float(others), y / float(others));
            }
        }
        RespawnClock::Step const step = seatState.Clock.Note(env.EpisodeElapsedMs, bot->IsAlive(), partyYards,
            tuning.DelayMs, tuning.RejoinYards);
        if (step == RespawnClock::Step::Rise)
        {
            RiseAtEntrance(bot, data.Seats[index], fight.Entrance, env.EpisodeElapsedMs);
            seatState.Clock.Risen(env.EpisodeElapsedMs);
            seatState.DeathPaid = false;
            ++fight.Rises;
        }
        else if (step == RespawnClock::Step::Rejoined)
        {
            ++fight.Rejoins;
            fight.RejoinMsTotal = 0;
            for (SeatInstance const& any : fight.Seats)
                fight.RejoinMsTotal += any.Clock.RejoinMsTotal;
        }
    }
}

void Animus::Curriculum::InstanceEncounter::TraceWing(Env& env, EnvInstance& fight, bool fighting)
{
    EnvInstance::FightTrace& trace = fight.Trace;
    EnvState const& data = _scenario.Data(env);
    fight.OnParty = 0;
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
        auto const onParty = [](Unit* unit) -> Unit*
        {
            Unit* victim = unit->IsAlive() && !unit->IsPlayer() ? unit->GetVictim() : nullptr;
            return victim && victim->IsPlayer() ? victim : nullptr;
        };
        for (Unit* unit : units)
        {
            Unit* victim = onParty(unit);
            if (!victim)
                continue;
            ++engaged;
            onTank += victim == tank ? 1 : 0;
            if (Creature* creature = unit->ToCreature())
                elites += creature->isElite() ? 1 : 0;
        }
        fight.OnParty = engaged;
        if (engaged > _scenario.Tuning().Instance.WingCrowdFree)
            fight.CrowdSeconds += float(_scenario.StepMs(env)) / 1000.0f;
        if (engaged > trace.PeakEngaged)
        {
            // Who they are, by entry: counted only on a new peak, the one time the stuck log is given them.
            std::map<uint32, uint32> entries;
            for (Unit* unit : units)
                if (Creature* creature = onParty(unit) ? unit->ToCreature() : nullptr)
                    ++entries[creature->GetEntry()];
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
    LOG_INFO("module.animus", "Wing wipe: env {} {} rung {}{} level {} wipe {}/{} at {:.0f}s (the fight lasted "
        "{:.0f}s, {} kills in it, {} before) | peak {} on the party ({} elite, {} on the tank): {} | deaths: {}",
        env.Index, fight.Evaluating ? "eval" : "train", fight.Rung, fight.Probe ? " probe" : "",
        uint32(_scenario.Data(env).EpisodeLevel),
        fight.Wipes, fight.WipesAllowed, float(env.EpisodeElapsedMs) / 1000.0f,
        float(env.EpisodeElapsedMs - trace.StartMs) / 1000.0f,
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
        {
            // Not one that wants a skill the party may not have: the tank stood by a Tin Vein in the wall for the
            // hour (2026-10-01). No chest at all: nothing is looted (the user, 2026-10-06) -- what a lock takes is
            // carried from the door (KeyItems).
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

std::vector<uint32> const& Animus::Curriculum::InstanceEncounter::KeyItems(uint32 mapId)
{
    // Once per map: the key items (LOCK_KEY_ITEM) of the map's spawned game objects' locks -- the Deadmines' cannon
    // takes the Defias Gunpowder.
    static std::mutex lock;
    static std::map<uint32, std::vector<uint32>> keys;
    std::lock_guard<std::mutex> guard(lock);
    auto const known = keys.find(mapId);
    if (known != keys.end())
        return known->second;
    std::vector<uint32>& found = keys[mapId];
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (data.mapid != mapId)
            continue;
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(data.id);
        LockEntry const* entry = info ? sLockStore.LookupEntry(info->GetLockId()) : nullptr;
        for (uint32 i = 0; entry && i < MAX_LOCK_CASE; ++i)
            if (entry->Type[i] == LOCK_KEY_ITEM && entry->Index[i] && sObjectMgr->GetItemTemplate(entry->Index[i])
                && std::find(found.begin(), found.end(), entry->Index[i]) == found.end())
                found.push_back(entry->Index[i]);
    }
    return found;
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

void Animus::Curriculum::InstanceEncounter::UpdateWingEnemies(Env& env, EnvInstance& fight)
{
    Player* seat = _scenario.SeatBot(env, 0);
    Creature* boss = seat && !fight.Boss.IsEmpty() ? Encoding::CreatureThrough(*seat, fight.Boss) : nullptr;
    if (!seat || !seat->IsInWorld())
        return;

    // What the party can use near it -- a lever, a button, the Deadmines' cannon, a closed door it may open: a
    // dungeon's way on is opened the way a player opens it (2026-10-01: "they need to be able to use the proper
    // actions to activate doors and cannons"), with a press on it (the sight block's interact or use-item). Nothing
    // opens by itself.
    fight.Objects.clear();
    // One visit from the party's middle, wide enough to reach OBJECT_SIGHT past its farthest living seat, rather than
    // one per seat: what each seat can see is then picked out of it, as before.
    std::array<Player*, MAX_SEATS> living{};
    uint32 livingCount = 0;
    float centreX = 0.0f;
    float centreY = 0.0f;
    for (uint32 index = 0; index < _scenario.Data(env).ActiveSeats; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive() && bot->IsInWorld())
        {
            living[livingCount++] = bot;
            centreX += bot->GetPositionX();
            centreY += bot->GetPositionY();
        }
    if (livingCount)
    {
        centreX /= float(livingCount);
        centreY /= float(livingCount);
        float spread = 0.0f;
        for (uint32 index = 0; index < livingCount; ++index)
            spread = std::max(spread, living[index]->GetExactDist2d(centreX, centreY));
        std::list<GameObject*> objects;
        GameObjectsNearPoint check(centreX, centreY, OBJECT_SIGHT + spread);
        Acore::GameObjectListSearcher<GameObjectsNearPoint> searcher(living[0], objects, check);
        Cell::VisitObjects(centreX, centreY, living[0]->GetMap(), searcher, OBJECT_SIGHT + spread);
        for (GameObject* object : objects)
        {
            if (!Usable(object)
                || std::find(fight.Used.begin(), fight.Used.end(), object->GetGUID()) != fight.Used.end())
                continue;
            bool seen = false;
            for (uint32 index = 0; index < livingCount; ++index)
                seen = seen || living[index]->GetExactDist(object) <= OBJECT_SIGHT;
            if (!seen)
                continue;
            fight.Objects.push_back(object->GetGUID());
        }
    }

    // The creatures watched last decision that have died since: the party's kills (the boss's is its own term).
    for (auto watched = fight.Watched.begin(); watched != fight.Watched.end();)
    {
        ObjectGuid const guid = *watched;
        Creature const* creature = guid == fight.Boss ? nullptr : Encoding::CreatureThrough(*seat, guid);
        if (creature && !creature->IsAlive())
        {
            fight.Counted.insert(guid);
            watched = fight.Watched.erase(watched);
            ++fight.TrashKills;
            fight.LastKillMs = env.EpisodeElapsedMs;
            if (creature->IsDungeonBoss() || creature->isWorldBoss())
            {
                ++fight.BossKills;
                fight.BossesKilled.push_back(creature->GetEntry());
            }
        }
        else
            ++watched;
    }

    // In the slots, by what matters to the party (RankEnemy): the tank's target, what is on a player, what else
    // fights -- each keeping the slot it had, so the slots do not reshuffle as the fight moves -- then the nearest of
    // what stands ahead, the next pack. Seen from the tank, who leads and pulls, while it is up; seat 0 otherwise.
    // From seat 0 the pack the tank was about to pull was often not in the slots at all (2026-10-01).
    constexpr float WING_SIGHT = 45.0f;
    Player* tank = fight.Tank.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*seat, fight.Tank);
    if (tank && tank->IsAlive() && tank->IsInWorld() && tank->IsInMap(seat))
        seat = tank;
    else
        tank = nullptr;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(seat, seat, WING_SIGHT);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(seat, units, check);
    Cell::VisitObjects(seat, searcher, WING_SIGHT);
    // A creature that cannot reach the party (below a ledge, across lava) or is running home is no fight: in and out
    // of combat with the party above Ragefire's drop, it held the script's "fight" -- and the
    // party, imitating it -- for the rest of the run (2026-10-03).
    units.remove_if([](Unit* unit)
    {
        Creature const* creature = unit->ToCreature();
        return !unit->IsAlive() || unit->IsPlayer() || unit->IsTotem() || (creature && creature->IsEvadingAttacks());
    });
    // Each unit's key taken once, not in every comparison of the sort: its rank, then the slot it had (a fighting
    // rank) or its distance (standing; a unit new to the slots after those that were in them).
    struct Ranked
    {
        Unit* U;
        EnemyRank Rank;
        float Order;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(units.size());
    for (Unit* unit : units)
    {
        EnemyRank const rank = RankEnemy(unit, tank);
        float order = seat->GetDistance(unit);
        if (rank != EnemyRank::Standing)
        {
            auto const had = std::find(env.Targets.begin(), env.Targets.end(), unit->GetGUID());
            order = had != env.Targets.end() ? float(had - env.Targets.begin()) : float(PACK_SLOTS) + order;
        }
        ranked.push_back({ unit, rank, order });
    }
    std::sort(ranked.begin(), ranked.end(), [](Ranked const& a, Ranked const& b)
    {
        return a.Rank != b.Rank ? a.Rank < b.Rank : a.Order < b.Order;
    });
    units.clear();
    for (Ranked const& entry : ranked)
        units.push_back(entry.U);
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
        if (!fight.Counted.count(guid))
            fight.Watched.insert(guid);

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
            if (!fight.Counted.count(unit->GetGUID()))
                fight.Watched.insert(unit->GetGUID());
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

bool Animus::Curriculum::InstanceEncounter::FullClear(EnvInstance const& fight)
{
    if (!fight.BossDead)
        return false;
    // Every creature the clear counts (side bosses among them) and the last boss.
    return fight.HostileTotal && fight.TrashKills + 1 >= fight.HostileTotal;
}

bool Animus::Curriculum::InstanceEncounter::Succeeded(EnvInstance const& fight)
{
    return fight.BossDead;
}

float Animus::Curriculum::InstanceEncounter::ClearedShare(EnvInstance const& fight)
{
    return fight.HostileTotal ? std::min(1.0f, float(fight.TrashKills + (fight.BossDead ? 1 : 0))
        / float(fight.HostileTotal + 1)) : 0.0f;
}

void Animus::Curriculum::InstanceEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    // A wing's seats perceive what a player does (the coordinator's ruling, 2026-10-07: every dungeon stage carries the
    // sight block): the goal head's places and assignment are what the seat saw, its map's frontier and its leader
    // within the minimap's range (SeenWorld) -- never a route or the packs in order.
    EnvInstance const& fight = _envs[env.Index];
    if (fight.Boss.IsEmpty())
        return;
    if (seat < fight.Seats.size())
    {
        view.FoodItem = fight.Seats[seat].FoodItem;
        view.DrinkItem = fight.Seats[seat].DrinkItem;
    }
    SeenWorld(env, seat, view);
}

void Animus::Curriculum::InstanceEncounter::SeenWorld(Env const& env, uint32 seat, SeatView& view) const
{
    EnvInstance const& fight = _envs[env.Index];
    WorldView& world = view.World;
    world.Places = {};
    world.HasAssignment = false;
    world.HasSeenPlaces = true;
    if (!view.Bot || seat >= MAX_SEATS)
        return;
    SeatState const& state = _scenario.Data(env).Seats[seat];
    Player const* bot = view.Bot;

    SeenPlaces::Input in;
    in.Seat = { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    // What its entity memory holds: the hostiles it saw, where it last saw them (I2) -- nothing it never saw.
    for (Vision::Remembered const& entry : state.Recall.Entries())
        if (entry.Guid)
            in.Memory.push_back({ { entry.Position.X, entry.Position.Y, entry.Position.Z }, entry.Reaction < 0,
                entry.Dead, entry.GameObject });
    // Its own mental map's frontier, refreshed every FRONTIER_MS.
    constexpr uint32 FRONTIER_MS = 2000;
    constexpr float FRONTIER_RADIUS = 40.0f;
    constexpr float FRONTIER_STEP = 2.0f;
    constexpr uint32 FRONTIER_POINTS = SeenPlaces::ROAM_PLACES;
    SeatInstance const& own = fight.Seats[seat];
    if (!own.FrontierReady || env.EpisodeElapsedMs >= own.FrontierMs + FRONTIER_MS)
    {
        Vision::MentalMap const& map = state.Map;
        own.Frontier = SeenPlaces::Frontier(in.Seat, FRONTIER_RADIUS, FRONTIER_STEP, FRONTIER_POINTS,
            [&map](float x, float y)
            {
                Vision::MapCell const* cell = map.Find(x, y);
                if (!cell || !Vision::Known(*cell))
                    return SeenPlaces::Ground::Unknown;
                if (cell->Flags & (Vision::MAP_WALL_LOW | Vision::MAP_WALL_HIGH | Vision::MAP_HAZARD))
                    return SeenPlaces::Ground::Shut;
                return cell->Floor[0] != Vision::NO_FLOOR || (cell->Flags & (Vision::MAP_FREE | Vision::MAP_VISITED))
                    ? SeenPlaces::Ground::Open : SeenPlaces::Ground::Unknown;
            });
        own.FrontierMs = env.EpisodeElapsedMs;
        own.FrontierReady = true;
    }
    in.Frontier = own.Frontier;
    // The party's leader -- the stand-in when it leads, else the tank -- as its frame and its map dot show it: the
    // place only within the minimap's range (PartyFollow.MinimapYards, the dot's own, 2D), as a player's map shows it.
    int32 tankSeat = -1;
    for (uint32 index = 0; index < _scenario.Data(env).ActiveSeats && tankSeat < 0; ++index)
        if (Player* member = _scenario.SeatBot(env, index); member && !fight.Tank.IsEmpty()
            && member->GetGUID() == fight.Tank)
            tankSeat = int32(index);
    int32 const leaderSeat = WingRun::LeaderSeat(_scenario.StandInSeat(env), _scenario.StandInLeads(env), tankSeat);
    if (Player* leader = leaderSeat >= 0 && uint32(leaderSeat) != seat ? _scenario.SeatBot(env, uint32(leaderSeat))
        : nullptr; leader && leader->IsAlive() && leader->IsInMap(bot)
        && bot->GetExactDist2d(leader) <= _scenario.Tuning().PartyFollow.MinimapYards)
    {
        in.HasLeader = true;
        in.Leader = { leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ() };
    }

    SeenPlaces::Choice const choice = SeenPlaces::Choose(in);
    for (uint32 place = 0; place < SeenPlaces::PLACES && place < WorldView::JOURNAL_PLACES; ++place)
    {
        world.Places[place].Present = choice.Present[place];
        if (choice.Present[place])
            world.Places[place].Where.Relocate(choice.Where[place].X, choice.Where[place].Y, choice.Where[place].Z);
    }
    world.HasAssignment = choice.HasAssignment;
    if (choice.HasAssignment)
        world.Assignment.Relocate(choice.Assignment.X, choice.Assignment.Y, choice.Assignment.Z);
}

void Animus::Curriculum::InstanceEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    EnvInstance& fight = _envs[env.Index];
    SeatInstance& paid = fight.Seats[seatIndex];
    CurriculumTuning::InstanceTuning const& tuning = _scenario.Tuning().Instance;
    float const tierScale = TierScale(env);

    // Standing still in a dungeon costs (Instance.WingStall): once WingStallGraceMs pass with nothing killed and
    // nothing fighting the party, every seat pays by the second. Without it a party at the door paid only the
    // timeout, and early in training that beat fighting through half the dungeon.
    if (seatIndex == 0)
    {
        uint32 const seen = fight.TrashKills;
        if (seen != fight.ProgressSeen || _scenario.Data(env).StepEngaged)
        {
            fight.ProgressSeen = seen;
            fight.ProgressMs = env.EpisodeElapsedMs;
        }
    }
    // The tank decides when the party moves on, so standing about is its to pay, the others' at WingStallOthers of it:
    // charged alike, -177 a run swamped every seat's own role terms (a healer's healing paid 8.5), 2026-10-03.
    // Idle, a Cost at its full price from the first step (2026-10-07; it was Stall's Shaping, which the fade took).
    bool const tankSeat = _scenario.Data(env).Seats[seatIndex].DungeonRole == DUNGEON_TANK;
    float const seconds = float(_scenario.StepMs(env)) / 1000.0f;
    if (env.EpisodeElapsedMs > fight.ProgressMs + tuning.WingStallGraceMs)
        ledger.Add(RewardTerm::Idle, -tuning.WingStall * (tankSeat ? 1.0f : tuning.WingStallOthers) * seconds);
    // The clock (Instance.WingClock): a run that clears sooner is better, at its full price from the first step.
    ledger.Add(RewardTerm::StepCost, -tuning.WingClock * seconds);
    // The party's rest discipline -- pull, fight, rest, ready, next: every seat paid for a fight started with every
    // living member ready, at most once a run (ReadyPull, an Outcome; it was the tank's Threat, Shaping).
    uint32 const ready = std::min(fight.ReadyEngages, READY_PULLS_PAID);
    if (ready > paid.EngagesPaid)
    {
        ledger.Add(RewardTerm::ReadyPull, tuning.WingEngage * float(ready - paid.EngagesPaid), tierScale);
        paid.EngagesPaid = ready;
    }
    // More on the party than a pack (Instance.WingCrowd, past WingCrowdFree): a pull that ran into the next.
    if (fight.OnParty > tuning.WingCrowdFree)
        ledger.Add(RewardTerm::Threat, -tuning.WingCrowd * float(fight.OnParty - tuning.WingCrowdFree)
            * float(_scenario.StepMs(env)) / 1000.0f);
    // Away from the leader (Instance.WingStray): the party's leader -- the stand-in when it leads, else the tank -- and
    // a seat further than WingStrayYards from it. Lost, a Cost (2026-10-07: "they have to stay with the leader"; as
    // Approach it was Shaping and faded). Never while a risen seat walks back from the entrance: Away prices that.
    bool const walkingBack = paid.Clock.Rejoining;
    if (bot)
    {
        int32 tankSeat = -1;
        for (uint32 index = 0; index < _scenario.Data(env).ActiveSeats && tankSeat < 0; ++index)
            if (Player* member = _scenario.SeatBot(env, index); member && !fight.Tank.IsEmpty()
                && member->GetGUID() == fight.Tank)
                tankSeat = int32(index);
        int32 const leaderSeat = WingRun::LeaderSeat(_scenario.StandInSeat(env), _scenario.StandInLeads(env), tankSeat);
        Player* leader = leaderSeat >= 0 ? _scenario.SeatBot(env, uint32(leaderSeat)) : nullptr;
        bool const leaderHere = leader && leader->IsAlive() && leader->IsInWorld() && leader->IsInMap(bot);
        if (WingRun::Strays(bot->IsAlive(), leaderSeat == int32(seatIndex), walkingBack, leaderHere,
            leaderHere ? bot->GetExactDist(leader) : 0.0f, tuning.WingStrayYards))
            ledger.Add(RewardTerm::Lost, -tuning.WingStray * seconds);
        // Dead, or walking back from the entrance: every second of it a Cost (Instance.WingAway, as C3's Away).
        if (WingRun::Away(bot->IsAlive(), walkingBack))
            ledger.Add(RewardTerm::Away, -tuning.WingAway * seconds);
    }
    if (bot && bot->IsAlive())
    {
        ledger.Add(RewardTerm::Kill, tuning.WingTrashKill * float(fight.TrashKills - paid.KillsPaid), tierScale);
        ledger.Add(RewardTerm::Kill, tuning.WingMidBoss * float(fight.BossKills - paid.BossKillsPaid), tierScale);
    }
    paid.KillsPaid = fight.TrashKills;
    paid.BossKillsPaid = fight.BossKills;
    if (bot && !bot->IsAlive() && !paid.DeathPaid)
    {
        paid.DeathPaid = true;
        ++paid.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.WingDeath, 1.0f / tierScale);
    }
    if (fight.Wipes > paid.WipesPaid)
    {
        ledger.Add(RewardTerm::Death, -tuning.WingWipe * float(fight.Wipes - paid.WipesPaid), 1.0f / tierScale);
        paid.WipesPaid = fight.Wipes;
    }

    bool const over = fight.BossDead || fight.Wiped || TimeIsUp(env);
    if (!over || paid.OutcomePaid)
        return;
    paid.OutcomePaid = true;
    if (fight.BossDead)
    {
        ledger.Add(RewardTerm::Kill, tuning.WingBoss, tierScale);
        // The full clear (D2, D3: every pull and side boss on the way), times the tier scale.
        if (FullClear(fight) && !paid.FullClearPaid)
        {
            paid.FullClearPaid = true;
            ledger.Add(RewardTerm::Clear, tuning.WingClear, tierScale);
        }
    }
    // The clock out costs the share of the dungeon left: the creatures not yet killed (no route, no navmesh).
    else if (TimeIsUp(env) && !fight.Boss.IsEmpty())
        ledger.Add(RewardTerm::Timeout, -tuning.WingTimeout * (1.0f - ClearedShare(fight)), 1.0f / tierScale);
}

float Animus::Curriculum::InstanceEncounter::TierScale(Env const& env) const
{
    // A wing's difficulty is its support ladder's rung, not its pinned row (a stage's row is the same every run): the
    // tier rises as the rung steps down to the dungeon's own levels with no help (WingRun::TierOfRung).
    CurriculumTuning const& tuning = _scenario.Tuning();
    EnvInstance const& fight = _envs[env.Index];
    uint32 const tier = WingRun::TierOfRung(fight.Rung);
    return CombatReward::TierScale(tuning.Difficulty.TierScale, std::min<uint32>(tier, tuning.Instance.MaxTierScale));
}

bool Animus::Curriculum::InstanceEncounter::TimeIsUp(Env const& env)
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
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
    return fight.BossDead || fight.Wiped || TimeIsUp(env);
}
