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

#include "CombatEncounter.h"
#include "BotFactory.h"
#include "CombatBlock.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Opponents.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "StageState.h"
#include "Supplies.h"
#include <algorithm>
#include <cmath>

namespace
{
    namespace Draw = Animus::Curriculum::CombatDraw;
    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// How far the next pack may swing off the line from the seat through this one, either way (radians).
    constexpr float NEXT_SPREAD = 1.0f;
    /// A corridor point's floor: the body's height for the liquid test.
    constexpr float BODY_HEIGHT = 2.0f;

    Draw::Point PointOf(Position const& at)
    {
        return { at.GetPositionX(), at.GetPositionY(), at.GetPositionZ() };
    }
}

Animus::Curriculum::CombatEncounter::CombatEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "combat rung")
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::CombatEncounter::RewardTerms() const
{
    return { RewardTerm::Kill, RewardTerm::Clear, RewardTerm::Survived, RewardTerm::InterruptLanded,
        RewardTerm::Rejoin, RewardTerm::Death, RewardTerm::TeammateDeath, RewardTerm::Hurt, RewardTerm::FireHurt,
        RewardTerm::PullExtra, RewardTerm::StepCost, RewardTerm::DamageDealt };
}

Animus::Curriculum::CombatDrill Animus::Curriculum::CombatEncounter::Drill(Env const& env) const
{
    return _scenario.Arena(env).Combat;
}

void Animus::Curriculum::CombatEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    auto const of = [this](auto read)
    {
        return [this, read](Env const& env, uint32) { return float(read(_envs[env.Index])); };
    };
    // The stage's measures. `won` is the rung's window's: something taken down and no death (CombatDraw::Won); the
    // evaluation videos call an episode a success by it. `survived`: no death at all.
    table.Add("won", of([](EnvCombat const& c) { return Draw::Won(c.Kills + c.Clears, c.Deaths) ? 1 : 0; }));
    table.Add("survived", of([](EnvCombat const& c) { return c.Deaths == 0 ? 1 : 0; }));
    table.Add("kills", of([](EnvCombat const& c) { return c.Kills; }));
    table.Add("packs_cleared", of([](EnvCombat const& c) { return c.Clears; }));
    table.Add("pulls", of([](EnvCombat const& c) { return c.Pulls; }));
    table.Add("extra_pulls", of([](EnvCombat const& c) { return c.ExtraPulls; }));
    table.Add("interrupts", of([](EnvCombat const& c) { return c.Interrupts; }));
    table.Add("deaths", of([](EnvCombat const& c) { return c.Deaths; }));
    table.Add("respawns", of([](EnvCombat const& c) { return c.Respawns; }));
    table.Add("rejoins", of([](EnvCombat const& c) { return c.Rejoins; }));
    // Per rejoin (PER_EVENT on rejoins): seconds from the respawn at the entrance to back at the fight.
    table.Add("rejoin_seconds", of([](EnvCombat const& c) { return c.RejoinSeconds; }));
    // Per kill (PER_EVENT on kills): seconds from a creature's engage to its death, C1's.
    table.Add("kill_seconds", of([](EnvCombat const& c) { return c.KillSeconds; }));
    table.Add("ally_deaths", of([](EnvCombat const& c) { return c.AllyDeaths; }));
    // Health taken, in the seat's maximum healths: all of it, and what ground fire took.
    table.Add("hurt_share", of([](EnvCombat const& c) { return c.HurtShare; }));
    table.Add("fire_share", of([](EnvCombat const& c) { return c.FireShare; }));
    table.Add("hazard_pulls", of([](EnvCombat const& c) { return c.HazardPulls; }));
    table.Add("linked_pulls", of([](EnvCombat const& c) { return c.LinkedPulls; }));
    table.Add("caster_pulls", of([](EnvCombat const& c) { return c.CasterPulls; }));
    // Seconds spent eating or drinking (C3's rest).
    table.Add("rest_seconds", of([](EnvCombat const& c) { return float(c.RestMs) / 1000.0f; }));
    // The share of decisions with a selection, and with it in the camera's frame: choosing a target by sight.
    table.Add("selected_share", of([](EnvCombat const& c)
    {
        return c.Decisions ? float(c.SelectedDecisions) / float(c.Decisions) : 0.0f;
    }));
    table.Add("target_in_view", of([](EnvCombat const& c)
    {
        return c.SelectedDecisions ? float(c.InViewDecisions) / float(c.SelectedDecisions) : 0.0f;
    }));
    table.Add("start_walk", of([](EnvCombat const& c) { return c.StartWalk; }));
    // The rung: `combat_rung` for the evaluation videos (Vision::EvalVideoRungColumn) and the status, `difficulty`
    // for the learner's per-class ladder tracking, `at_top_rung` its top.
    table.Add("combat_rung", of([](EnvCombat const& c) { return c.Tier; }));
    table.Add("difficulty", of([](EnvCombat const& c) { return c.Tier; }));
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Tier >= _scenario.Tuning().Combat.MaxTier ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::CombatEncounter::ResetEpisode(Env& env)
{
    // The creatures stay in the world until the next Build clears them (it has the map); the episode's measures go.
    EnvCombat& combat = _envs[env.Index];
    std::vector<Pack> packs = std::move(combat.Packs);
    ObjectGuid const ally = combat.Ally;
    combat = EnvCombat();
    combat.Packs = std::move(packs);
    combat.Ally = ally;
}

std::vector<Animus::Curriculum::CombatDraw::Point> const& Animus::Curriculum::CombatEncounter::Corridors(Player* bot,
    Map* map) const
{
    std::lock_guard<std::mutex> guard(_corridorLock);
    auto const known = _corridors.find(map->GetId());
    if (known != _corridors.end())
        return known->second;

    // The dungeon's own creature spawns, by spawn id (a fixed order, so an evaluation's seed picks the same point).
    std::vector<std::pair<ObjectGuid::LowType, Draw::Point>> spawns;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        if (data.mapid == map->GetId())
            spawns.emplace_back(spawnId, Draw::Point{ data.posX, data.posY, data.posZ });
    std::sort(spawns.begin(), spawns.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<Draw::Point> candidates;
    for (auto const& [spawnId, point] : spawns)
    {
        // Dry ground only: a creature of the lava stands where a seat cannot.
        LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), point.X, point.Y, point.Z, BODY_HEIGHT, {});
        if (liquid.Status == LIQUID_MAP_NO_WATER)
            candidates.push_back(point);
    }

    // Reached from the entrance (where the seat stands now) by a whole path, within the walk.
    CurriculumTuning::CombatTuning const& tuning = _scenario.Tuning().Combat;
    std::vector<Draw::Point> kept = Draw::CorridorPoints(candidates, [bot](Draw::Point const& point)
    {
        PathGenerator path(bot);
        if (!path.CalculatePath(point.X, point.Y, point.Z) || (path.GetPathType() & PATHFIND_NOPATH)
            || !(path.GetPathType() & PATHFIND_NORMAL))
            return -1.0f;
        return path.getPathLength();
    }, tuning.CorridorWalk, tuning.CorridorSpacing);
    LOG_INFO("module.animus", "{}: map {} has {} corridor points of {} creature spawns (within {:.0f} yd of its "
        "entrance)", _scenario.Name(), map->GetId(), kept.size(), spawns.size(), tuning.CorridorWalk);
    return _corridors.emplace(map->GetId(), std::move(kept)).first->second;
}

bool Animus::Curriculum::CombatEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;
    Despawn(env);

    EnvState& data = _scenario.Data(env);
    SeatState& seat = data.Seats[0];
    EnvCombat& combat = _envs[env.Index];
    combat = EnvCombat();
    CurriculumTuning::CombatTuning const& tuning = _scenario.Tuning().Combat;

    // The rung, the class and build's own (an evaluation's spread over the seeds).
    combat.Layout = seat.L ? seat.L->Index : 0;
    combat.Spec = seat.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, combat.Layout, combat.Spec, tuning.MaxTier);
    combat.Tier = pick.Tier;
    combat.Counts = pick.Counts;
    combat.Level = seat.Level;

    // Where it starts: one of the dungeon's corridor points, from the entrance it stands at now.
    std::vector<Draw::Point> const& corridors = Corridors(bot, map);
    Position const& entrance = _scenario.SpawnPointFor(env);
    if (!corridors.empty())
    {
        bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED;
        uint32 const at = seeded ? env.EpisodeSeedIndex % uint32(corridors.size())
            : urand(0, uint32(corridors.size()) - 1);
        Draw::Point const& point = corridors[at];
        combat.Corridor = int32(at);
        combat.StartWalk = std::sqrt((point.X - entrance.GetPositionX()) * (point.X - entrance.GetPositionX())
            + (point.Y - entrance.GetPositionY()) * (point.Y - entrance.GetPositionY()));
        BotFactory::TeleportWithinMap(bot, Position(point.X, point.Y, point.Z, frand(0.0f, TWO_PI)));
    }

    _scenario.PrepareFighter(bot, seat);
    // Food and drink for the rest between pulls (C3): the gauntlet block eats and drinks them.
    if (Drill(env) == CombatDrill::Survive)
    {
        ConsumablePool const& consumables = ConsumablePool::Instance();
        uint32 const food = consumables.Food(seat.Level);
        uint32 const drink = bot->GetMaxPower(POWER_MANA) ? consumables.Drink(seat.Level) : 0;
        StockConsumables(bot, food, drink);
    }

    if (_scenario.Arena(env).Ally)
        SpawnAlly(env, map, bot);
    if (!SpawnPull(env, map, bot, nullptr))
        return false;
    if (Drill(env) != CombatDrill::Fight)
        SpawnPull(env, map, bot, &combat.Packs.front().Spot);
    ListTargets(env);
    return true;
}

bool Animus::Curriculum::CombatEncounter::SpawnPull(Env& env, Map* map, Player* bot, Position const* from)
{
    EnvCombat& combat = _envs[env.Index];
    ArenaDefinition const& arena = _scenario.Arena(env);
    CurriculumTuning::CombatTuning const& tuning = _scenario.Tuning().Combat;
    CombatDrill const drill = arena.Combat;

    bool const fire = drill != CombatDrill::Fight && (arena.Hazards || roll_chance_i(tuning.HazardChance));
    bool const caster = roll_chance_i(int32(_scenario.Tuning().Difficulty.CasterChance));
    Draw::Pull const plan = Draw::PlanPull(drill, combat.Tier, tuning, fire, caster);
    uint8 const level = Draw::CreatureLevel(combat.Level, plan);

    // Who is in it: an elite, a caster and a fire-caster first where the rung asks, the rest from the pack pool.
    Opponents::OpponentPool const& pool = Opponents::OpponentPool::Instance();
    std::vector<uint32> entries;
    if (plan.Elite)
        if (uint32 const elite = pool.RandomElite(level))
            entries.push_back(elite);
    if (plan.Hazard && entries.size() < plan.Size)
        if (uint32 const burner = pool.RandomHazardCaster(level))
            entries.push_back(burner);
    if (plan.Caster && entries.size() < plan.Size)
        if (uint32 const casting = pool.RandomCaster(level))
            entries.push_back(casting);
    while (entries.size() < plan.Size)
    {
        uint32 const entry = drill == CombatDrill::Fight ? pool.Random(level) : pool.RandomPackMember(level);
        if (!entry)
            break;
        entries.push_back(entry);
    }
    if (entries.empty())
        return false;

    // Where: in the seat's sight, or further on from the pack in front, away from the seat.
    Position spot;
    std::optional<Position> onward;
    if (from)
    {
        float const bearing = std::atan2(from->GetPositionY() - bot->GetPositionY(),
            from->GetPositionX() - bot->GetPositionX());
        onward = Opponents::FindSpawnPointFrom(bot, map, *from, bearing, NEXT_SPREAD, tuning.NextNearest,
            tuning.NextFurthest);
    }
    spot = onward ? *onward : Opponents::FindSpawnPoint(bot, map, tuning.FightNearest, tuning.FightFurthest);
    std::vector<Creature*> members = Opponents::SpawnPack(bot, map, entries, level, spot);
    if (members.empty())
        return false;

    Pack pack;
    pack.Spot = spot;
    pack.Linked = plan.Linked;
    pack.Hazard = plan.Hazard;
    pack.Caster = plan.Caster;
    pack.Elite = plan.Elite;
    for (Creature* member : members)
    {
        pack.Members.push_back(member->GetGUID());
        pack.Counted.push_back(false);
    }
    combat.Packs.push_back(std::move(pack));
    ++combat.Pulls;
    combat.HazardPulls += plan.Hazard ? 1 : 0;
    combat.LinkedPulls += plan.Linked ? 1 : 0;
    combat.CasterPulls += plan.Caster ? 1 : 0;

    // The guard arena: the creature goes for the friend first.
    if (Creature* ally = combat.Ally.IsEmpty() ? nullptr : map->GetCreature(combat.Ally); ally && ally->IsAlive()
        && combat.Packs.size() == 1)
        for (Creature* member : members)
            if (member->IsAIEnabled && member->CanCreatureAttack(ally))
            {
                member->GetThreatMgr().AddThreat(ally, 10.0f);
                member->AI()->AttackStart(ally);
            }
    return true;
}

void Animus::Curriculum::CombatEncounter::SpawnAlly(Env& env, Map* map, Player* bot)
{
    EnvCombat& combat = _envs[env.Index];
    if (Creature* old = combat.Ally.IsEmpty() ? nullptr : map->GetCreature(combat.Ally))
        old->DespawnOrUnsummon();
    combat.Ally.Clear();
    combat.AllyDead = false;

    uint32 const entry = Opponents::OpponentPool::Instance().Random(combat.Level);
    Position const at = Opponents::FindSpawnPoint(bot, map, 2.0f, 5.0f);
    Creature* ally = entry ? Opponents::SummonOpponent(bot, map, entry, at, combat.Level) : nullptr;
    if (!ally)
        return;
    // A friend to keep alive: of the seat's faction (so its heals take), passive (it never strikes back, so the seat's
    // taunt and its damage are what take the creature off it), and healing only by the seat's hand.
    ally->SetFaction(bot->GetFaction());
    ally->SetReactState(REACT_PASSIVE);
    combat.Ally = ally->GetGUID();
}

void Animus::Curriculum::CombatEncounter::ListTargets(Env& env) const
{
    env.Targets.clear();
    for (Pack const& pack : _envs[env.Index].Packs)
        for (ObjectGuid const& guid : pack.Members)
            if (env.Targets.size() < MAX_TARGETS)
                env.Targets.push_back(guid);
}

Creature* Animus::Curriculum::CombatEncounter::Member(Env const& env, ObjectGuid guid) const
{
    Map* map = env.FindMap();
    return map && !guid.IsEmpty() ? map->GetCreature(guid) : nullptr;
}

void Animus::Curriculum::CombatEncounter::Despawn(Env& env)
{
    EnvCombat& combat = _envs[env.Index];
    for (Pack const& pack : combat.Packs)
        for (ObjectGuid const& guid : pack.Members)
            if (Creature* creature = Member(env, guid))
                creature->DespawnOrUnsummon();
    combat.Packs.clear();
    if (Creature* ally = Member(env, combat.Ally))
        ally->DespawnOrUnsummon();
    combat.Ally.Clear();
    env.Targets.clear();
}

void Animus::Curriculum::CombatEncounter::UpdateEnemies(Env& env)
{
    // A linked pack: one member in combat brings the rest, on whoever it is fighting.
    for (Pack& pack : _envs[env.Index].Packs)
    {
        Unit* victim = nullptr;
        std::vector<Creature*> idle;
        for (ObjectGuid const& guid : pack.Members)
            if (Creature* member = Member(env, guid); member && member->IsAlive())
            {
                if (member->IsInCombat() && member->GetVictim() && !victim)
                    victim = member->GetVictim();
                else if (!member->IsInCombat())
                    idle.push_back(member);
            }
        if (!pack.Engaged && victim)
        {
            pack.Engaged = true;
            pack.EngageMs = env.EpisodeElapsedMs;
        }
        if (pack.Linked && victim && victim->IsAlive())
            for (Creature* member : idle)
                if (member->IsAIEnabled && member->CanCreatureAttack(victim))
                    member->AI()->AttackStart(victim);
    }
}

void Animus::Curriculum::CombatEncounter::Update(Env& env)
{
    EnvCombat& combat = _envs[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    Map* map = env.FindMap();
    if (!bot || !map)
        return;

    // A death (I4): out for the delay, then alive at the entrance to walk back.
    if (!bot->IsAlive())
    {
        if (!combat.DeadSinceMs)
        {
            combat.DeadSinceMs = std::max<uint32>(1, env.EpisodeElapsedMs);
            combat.FellAt = PointOf(*bot);
        }
        if (_scenario.Arena(env).RespawnAtEntrance
            && Draw::RespawnDue(combat.DeadSinceMs, env.EpisodeElapsedMs, _scenario.Tuning().Combat.RespawnDelayMs)
            && _scenario.RespawnAtEntrance(env, 0))
        {
            combat.DeadSinceMs = 0;
            combat.DeathPaid = false;
            combat.RejoinPending = true;
            combat.RespawnedAtMs = env.EpisodeElapsedMs;
            ++combat.Respawns;
        }
        return;
    }

    // C1: the next creature, once the last is down and its moment has come.
    if (combat.NextFightMs && env.EpisodeElapsedMs >= combat.NextFightMs)
    {
        combat.NextFightMs = 0;
        if (_scenario.Arena(env).Ally && (combat.AllyDead || combat.Ally.IsEmpty()))
            SpawnAlly(env, map, bot);
        if (SpawnPull(env, map, bot, nullptr))
            ListTargets(env);
    }

    // C2, C3: a pull in front and the next behind it, whatever happened while the seat was away (a spawn that found
    // no ground, a pack cleared while it lay dead).
    if (Drill(env) != CombatDrill::Fight && combat.Packs.size() < 2)
    {
        bool const spawned = SpawnPull(env, map, bot, combat.Packs.empty() ? nullptr : &combat.Packs.back().Spot);
        if (spawned)
            ListTargets(env);
    }
}

void Animus::Curriculum::CombatEncounter::OnSeatAction(Env& env, uint32 /*seat*/, SeatActionResult const& result)
{
    if (!result.PendingInterrupt.IsEmpty())
        _envs[env.Index].PendingInterrupt = result.PendingInterrupt;
}

void Animus::Curriculum::CombatEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    // The gauntlet block's rest (C3): its food and drink, the pulls cleared, how long it has been quiet. The next
    // pull's timers stay 0: nothing in the world tells a player when a pack will come, and none here comes on its
    // own -- the seat goes to it.
    EnvCombat const& combat = _envs[env.Index];
    view.PullsCleared = combat.Clears + combat.Kills;
    if (Drill(env) == CombatDrill::Survive)
    {
        ConsumablePool const& consumables = ConsumablePool::Instance();
        view.FoodItem = consumables.Food(combat.Level);
        view.DrinkItem = view.Bot && view.Bot->GetMaxPower(POWER_MANA) ? consumables.Drink(combat.Level) : 0;
        view.GauntletSupplies = CONSUMABLE_COUNT;
    }
    if (!combat.Packs.empty())
    {
        Pack const& front = combat.Packs.front();
        view.ElitePull = front.Elite && front.Engaged;
        if (front.Engaged)
            view.PullTime = std::min(1.0f, float(env.EpisodeElapsedMs - front.EngageMs) / 60000.0f);
    }
}

void Animus::Curriculum::CombatEncounter::BeforeRewards(Env& env)
{
    EnvCombat& combat = _envs[env.Index];
    combat.NewKills = 0;
    combat.NewClears = 0;
    combat.NewExtra = 0;
    if (combat.Packs.empty())
        return;

    Map* map = env.FindMap();
    Player* bot = _scenario.SeatBot(env, 0);
    CombatDrill const drill = Drill(env);

    // The friend of the guard arena.
    combat.NewAllyDeath = false;
    if (!combat.Ally.IsEmpty() && !combat.AllyDead)
        if (Creature* ally = Member(env, combat.Ally); !ally || !ally->IsAlive())
        {
            combat.AllyDead = true;
            combat.NewAllyDeath = true;
            ++combat.AllyDeaths;
        }

    // The pack in front: each death counted once, and cleared when none is left.
    Pack& front = combat.Packs.front();
    bool alive = false;
    for (std::size_t i = 0; i < front.Members.size(); ++i)
    {
        Creature* member = Member(env, front.Members[i]);
        if (member && member->IsAlive())
        {
            alive = true;
            continue;
        }
        if (front.Counted[i])
            continue;
        front.Counted[i] = true;
        if (drill == CombatDrill::Fight)
        {
            ++combat.NewKills;
            ++combat.Kills;
            if (front.Engaged)
                combat.KillSeconds += float(env.EpisodeElapsedMs - front.EngageMs) / 1000.0f;
        }
    }

    // The next pack drawn in while the front one still stands: an extra pull, charged once.
    if (alive && combat.Packs.size() > 1 && !combat.Packs[1].ExtraCounted && combat.Packs[1].Engaged)
    {
        combat.Packs[1].ExtraCounted = true;
        ++combat.NewExtra;
        ++combat.ExtraPulls;
    }

    if (alive)
        return;

    // Cleared: the next pull comes forward, and a new one stands further on (C2, C3) or comes after a breath (C1).
    if (drill != CombatDrill::Fight)
    {
        ++combat.NewClears;
        ++combat.Clears;
    }
    Position const cleared = front.Spot;
    combat.Packs.erase(combat.Packs.begin());
    if (drill == CombatDrill::Fight)
        combat.NextFightMs = env.EpisodeElapsedMs + _scenario.Tuning().Combat.NextFightMs;
    else if (map && bot && bot->IsAlive())
        SpawnPull(env, map, bot, combat.Packs.empty() ? &cleared : &combat.Packs.back().Spot);
    ListTargets(env);
}

void Animus::Curriculum::CombatEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    if (seatIndex != 0 || !bot)
        return;
    EnvCombat& combat = _envs[env.Index];
    CurriculumTuning::CombatTuning const& tuning = _scenario.Tuning().Combat;
    CombatDrill const drill = Drill(env);
    float const w = Draw::TierWeight(_scenario.Tuning().Difficulty.TierScale, combat.Tier);
    AgentStats const& step = env.StepStats[seatIndex];
    float const maxHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));
    float const decision = float(_scenario.DecisionMs()) / 1000.0f;

    // Outcome: what was taken down.
    if (combat.NewKills)
        ledger.Add(RewardTerm::Kill, tuning.Kill * w * float(combat.NewKills));
    if (combat.NewClears)
        ledger.Add(RewardTerm::Clear, tuning.Clear * w * float(combat.NewClears));

    // An interrupt that stopped the cast it was aimed at.
    if (!combat.PendingInterrupt.IsEmpty())
    {
        auto const stopped = std::find_if(env.StepInterruptedTargets.begin(), env.StepInterruptedTargets.end(),
            [&combat](Env::InterruptedCast const& cast) { return cast.Caster == combat.PendingInterrupt; });
        if (stopped != env.StepInterruptedTargets.end())
        {
            ++combat.Interrupts;
            if (drill != CombatDrill::Fight)
                ledger.Add(RewardTerm::InterruptLanded, tuning.InterruptLanded);
        }
        combat.PendingInterrupt.Clear();
    }

    // Cost: an extra pull, an ally lost, health taken (ground fire at its own price), the time a kill takes.
    if (combat.NewExtra)
        ledger.Add(RewardTerm::PullExtra, -tuning.ExtraPull * float(combat.NewExtra));
    if (combat.NewAllyDeath)
        ledger.Add(RewardTerm::TeammateDeath, -tuning.AllyDeath / w);
    float const fire = float(step.HazardDamage) / maxHealth;
    float const hurt = float(step.DamageTaken - std::min(step.DamageTaken, step.HazardDamage)) / maxHealth;
    combat.HurtShare += hurt + fire;
    combat.FireShare += fire;
    if (hurt > 0.0f)
        ledger.Add(RewardTerm::Hurt, -tuning.Hurt * hurt);
    if (fire > 0.0f)
        ledger.Add(RewardTerm::FireHurt, -tuning.FireHurt * fire);
    if (!combat.Packs.empty() && combat.Packs.front().Engaged)
        ledger.Add(RewardTerm::StepCost, -tuning.Clock * decision);

    // Shaping: damage dealt, in the front pull's creature healths.
    if (step.Damage && !combat.Packs.empty())
    {
        float health = 0.0f;
        uint32 members = 0;
        for (ObjectGuid const& guid : combat.Packs.front().Members)
            if (Creature* member = Member(env, guid))
            {
                health += float(member->GetMaxHealth());
                ++members;
            }
        if (members && health > 0.0f)
            ledger.Add(RewardTerm::DamageDealt, tuning.Damage * float(step.Damage) / (health / float(members)));
    }

    // Death: charged once a death; the episode goes on (the respawn is Update's).
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    CombatTally& tally = seat.Combat;
    if (!bot->IsAlive() && !combat.DeathPaid)
    {
        combat.DeathPaid = true;
        ++combat.Deaths;
        tally.Died = true;
        tally.DeathCounted = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death / w);
    }

    // Back at the fight after a respawn.
    if (combat.RejoinPending && bot->IsAlive()
        && Draw::Rejoined(PointOf(*bot), combat.FellAt, tuning.RejoinYards))
    {
        combat.RejoinPending = false;
        ++combat.Rejoins;
        combat.RejoinSeconds += float(env.EpisodeElapsedMs - combat.RespawnedAtMs) / 1000.0f;
        if (drill == CombatDrill::Survive)
            ledger.Add(RewardTerm::Rejoin, tuning.Rejoin);
    }

    // What the seat did with its view, measured.
    if (bot->IsAlive())
    {
        ++combat.Decisions;
        if (!bot->GetTarget().IsEmpty())
        {
            ++combat.SelectedDecisions;
            if (CombatBlock::InView(seat.Seen, bot->GetTarget().GetRawValue()))
                ++combat.InViewDecisions;
        }
        if (bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
            combat.RestMs += _scenario.DecisionMs();
    }

    // The episode's end: survived it, and the rung's window.
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (timeIsUp && !combat.Recorded)
    {
        combat.Recorded = true;
        if (combat.Deaths == 0)
            ledger.Add(RewardTerm::Survived,
                (drill == CombatDrill::Survive ? tuning.SurviveSurvived : tuning.Survived) * w);
        if (combat.Counts)
            _ladder.Record(combat.Layout, combat.Spec, combat.Tier, Draw::Won(combat.Kills + combat.Clears,
                combat.Deaths), tuning.MaxTier);
    }
}

void Animus::Curriculum::CombatEncounter::WriteState(Env const& env, float* state) const
{
    // The rung, so the critic can predict a tier-scaled return.
    uint32 const top = std::max<uint32>(1, _scenario.Tuning().Combat.MaxTier);
    state[StageScenario::STATE_TIER] = float(_envs[env.Index].Tier) / float(top);
}

bool Animus::Curriculum::CombatEncounter::IsTerminal(Env const& env) const
{
    // Never a death: the episode runs to its clock.
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

void Animus::Curriculum::CombatEncounter::Teardown(Env& env)
{
    Despawn(env);
}
