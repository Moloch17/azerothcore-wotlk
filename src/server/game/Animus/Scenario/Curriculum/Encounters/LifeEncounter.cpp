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

#include "LifeEncounter.h"
#include "CellImpl.h"
#include "CombatReward.h"
#include "Creature.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "WorldActions.h"
#include <algorithm>
#include <list>

namespace
{
    using namespace Animus::Curriculum;

    /// How far a hostile of the place is offered as a target before it has attacked: the pack block needs a slot
    /// to aim at to open a fight.
    constexpr float HOSTILE_REACH = 40.0f;
    /// Salts for the seeded draws, so the rung, the side and the place of one seed differ.
    constexpr uint32 SALT_SIDE = 11;
    constexpr uint32 SALT_LEVEL = 13;
}

Animus::Curriculum::LifeEncounter::LifeEncounter(StageScenario& scenario, uint32 envs, std::string what)
    : Encounter(scenario), _envs(envs), _ladder(scenario, what + " band"), _what(std::move(what))
{
    // The indexes are built at startup rather than in the first episode: reading the spawn tables is seconds.
    LifeWorld::SpawnIndex::Instance();
}

void Animus::Curriculum::LifeEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // The rung is `difficulty` -- the column the convergence rule's ladder signal reads -- unless another
    // encounter of the stage reports its own under that name (a creature duel, pulls, an instance: the crossroads).
    auto const reportsDifficulty = [](ArenaDefinition const& arena)
    {
        return arena.Against == Opposition::Creature || arena.Against == Opposition::Pulls
            || arena.Against == Opposition::Instance;
    };
    if (!_scenario.Stage().AnyArena(reportsDifficulty))
        table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add(_what + "_band", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add(_what + "_side", [this](Env const& env, uint32) { return float(uint8(_envs[env.Index].Side)); });
    table.Add(_what + "_won", [this](Env const& env, uint32) { return _envs[env.Index].Won ? 1.0f : 0.0f; });
    table.Add("interactions", [this](Env const& env, uint32) { return float(_envs[env.Index].Interactions); });
    table.Add("wasted_presses", [this](Env const& env, uint32) { return float(_envs[env.Index].Wasted); });
    table.Add("corpses_looted", [this](Env const& env, uint32) { return float(_envs[env.Index].CorpsesLooted); });
    table.Add("items_looted", [this](Env const& env, uint32) { return float(_envs[env.Index].ItemsLooted); });
    table.Add("copper_looted", [this](Env const& env, uint32) { return float(_envs[env.Index].CopperLooted); });
    table.Add("spawned", [this](Env const& env, uint32) { return float(_envs[env.Index].Spawned.size()); });
    table.Add("progressed", [this](Env const& env, uint32) { return _envs[env.Index].Progressed; });
    AddMoreEpisodeInfo(table);
}

void Animus::Curriculum::LifeEncounter::ResetEpisode(Env& env)
{
    // The spawns go with the rebuild (BeforeRebuild); the rest starts over.
    std::vector<ObjectGuid> spawned = std::move(_envs[env.Index].Spawned);
    _envs[env.Index] = EnvLife();
    _envs[env.Index].Spawned = std::move(spawned);
}

void Animus::Curriculum::LifeEncounter::BeforeRebuild(Env& env)
{
    LifeWorld::Despawn(env.FindMap(), _envs[env.Index].Spawned);
}

void Animus::Curriculum::LifeEncounter::Teardown(Env& env)
{
    LifeWorld::Despawn(env.FindMap(), _envs[env.Index].Spawned);
}

void Animus::Curriculum::LifeEncounter::BeforeLevel(Env& env)
{
    // The rung is the band: seat 0's class and build climb it as they win; an evaluation spreads its seeds over
    // the three. The band fixes the level (a draw within it), and the side is drawn too, since a quest and a town
    // belong to one; the race draw honours it (EnvState::EpisodeTeam).
    EnvState& data = _scenario.Data(env);
    EnvLife& life = _envs[env.Index];
    SeatState const& seat = data.Seats[0];
    life.Layout = seat.L ? seat.L->Index : 0;
    life.Spec = seat.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, life.Layout, life.Spec, LifeWorld::BAND_COUNT - 1);
    life.Tier = std::min<uint32>(pick.Tier, LifeWorld::BAND_COUNT - 1);
    life.Counts = pick.Counts;
    life.Side = LifeWorld::Draw(env, 2, SALT_SIDE) ? LifeWorld::Side::Horde : LifeWorld::Side::Alliance;

    LifeWorld::Band const& band = LifeWorld::BandAt(life.Tier);
    data.EpisodeLevel = uint8(band.Min + LifeWorld::Draw(env, band.Max - band.Min + 1, SALT_LEVEL));
    data.EpisodeTeam = uint8(life.Side == LifeWorld::Side::Horde ? TEAM_HORDE : TEAM_ALLIANCE) + 1;

    if (!Place(env, life))
    {
        LOG_ERROR("module.animus", "{}: env {} has nowhere to go for band {} ({})", _scenario.Name(), env.Index,
            life.Tier, LifeWorld::SideName(life.Side));
        data.EpisodeLevel = 0;
        data.EpisodeTeam = 0;
    }
}

Animus::Curriculum::LifeWorld::Side Animus::Curriculum::LifeEncounter::SideOfSeat(Env const& env) const
{
    Player const* bot = _scenario.SeatBot(env, 0);
    return bot ? LifeWorld::SideOf(bot->GetTeamId()) : _envs[env.Index].Side;
}

void Animus::Curriculum::LifeEncounter::SetWaypoint(EnvLife& life, uint8 kind, Position const& where)
{
    if (!life.HasWaypoint || life.WaypointKind != kind)
        life.LastDistance = -1.0f;
    life.HasWaypoint = true;
    life.WaypointKind = kind;
    life.Waypoint = where;
}

void Animus::Curriculum::LifeEncounter::ClearWaypoint(EnvLife& life)
{
    life.HasWaypoint = false;
    life.LastDistance = -1.0f;
}

Creature* Animus::Curriculum::LifeEncounter::Summon(Env& env, EnvLife& life, Map* map,
    LifeWorld::Spawn const& spawn)
{
    Creature* creature = LifeWorld::Summon(map, StageScenario::EnvPhase(env), spawn);
    if (creature)
        life.Spawned.push_back(creature->GetGUID());
    return creature;
}

GameObject* Animus::Curriculum::LifeEncounter::SummonObject(Env& env, EnvLife& life, Map* map,
    LifeWorld::Spawn const& spawn)
{
    GameObject* object = LifeWorld::SummonObject(map, StageScenario::EnvPhase(env), spawn);
    if (object)
        life.Spawned.push_back(object->GetGUID());
    return object;
}

uint32 Animus::Curriculum::LifeEncounter::SummonAround(Env& env, EnvLife& life, Map* map, Position const& where,
    float radius, uint32 cap, bool npcs)
{
    std::vector<LifeWorld::Spawn const*> spawns;
    LifeWorld::SpawnIndex::Instance().CreaturesNear(map->GetId(), where.GetPositionX(), where.GetPositionY(),
        radius, spawns);
    uint32 count = 0;
    for (LifeWorld::Spawn const* spawn : spawns)
    {
        if (count >= cap)
            break;
        if (!LifeWorld::IsWorldCreature(*spawn, npcs))
            continue;
        if (Summon(env, life, map, *spawn))
            ++count;
    }
    return count;
}

float Animus::Curriculum::LifeEncounter::TierScale(Env const& env) const
{
    return CombatReward::TierScale(_scenario.Tuning().Difficulty.TierScale, _envs[env.Index].Tier);
}

bool Animus::Curriculum::LifeEncounter::TimeIsUp(Env const& env) const
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

void Animus::Curriculum::LifeEncounter::UpdateEnemies(Env& env)
{
    // Whatever is fighting the seat, nearest first, then the nearest hostile within reach that is not yet: the
    // pack block's slots, so a fight can be opened and finished with the combat blocks as they were trained.
    Player* seat = _scenario.SeatBot(env, 0);
    env.Targets.clear();
    if (!seat || !seat->IsAlive())
        return;

    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(seat, seat, HOSTILE_REACH);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(seat, units, check);
    Cell::VisitObjects(seat, searcher, HOSTILE_REACH);
    units.remove_if([seat](Unit* unit) { return unit->IsPlayer() || !unit->IsAlive() || !seat->IsValidAttackTarget(unit); });
    units.sort([seat](Unit* a, Unit* b)
    {
        bool const fightingA = a->IsInCombat();
        bool const fightingB = b->IsInCombat();
        if (fightingA != fightingB)
            return fightingA;
        return seat->GetDistance(a) < seat->GetDistance(b);
    });
    for (Unit* unit : units)
    {
        if (env.Targets.size() >= PACK_SLOTS)
            break;
        env.Targets.push_back(unit->GetGUID());
    }
}

void Animus::Curriculum::LifeEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvLife const& life = _envs[env.Index];
    if (seat != 0 || !view.Bot)
        return;

    WorldActions::Sense(view.Bot, _scenario.Tuning().Life.SenseRange, view.World);
    Sensed(env, life, view);
    // The waypoint is the travel block's objective: the same features the trips were learned on.
    if (life.HasWaypoint)
    {
        view.HasObjective = true;
        view.Objective = life.Waypoint;
    }
}

void Animus::Curriculum::LifeEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    if (seat != 0)
        return;
    EnvLife& life = _envs[env.Index];
    life.Interactions += result.Interactions;
    life.Wasted += result.Wasted;
    life.CorpsesLooted += result.CorpsesLooted;
    life.ItemsLooted += result.ItemsLooted;
    life.CopperLooted += result.CopperLooted;
    life.GatherCasts += result.GatherCasts;
    Account(env, life, result);
}

void Animus::Curriculum::LifeEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    EnvLife& life = _envs[env.Index];
    if (seatIndex != 0)
        return;

    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    if (bot && !bot->IsAlive() && !life.Died)
        life.Died = true;
    if (life.Died && !life.DeathPaid)
    {
        life.DeathPaid = true;
        ledger.Add(RewardTerm::Death, -tuning.Death / TierScale(env));
    }

    // Potential-based shaping on the straight distance to the waypoint: what is closed pays, what is given back
    // costs. Spread over the trip's length (at least 100 yd) so a whole approach pays Life.Progress once, and
    // never paid across a change of waypoint (SetWaypoint resets the potential).
    if (bot && bot->IsAlive() && life.HasWaypoint)
    {
        float const distance = bot->GetExactDist2d(&life.Waypoint);
        if (life.LastDistance >= 0.0f)
        {
            float const trip = std::max(100.0f, life.LastDistance);
            float const closed = life.LastDistance - distance;
            ledger.Add(RewardTerm::Progress, tuning.Progress * closed / trip);
            life.Progressed += closed;
        }
        life.LastDistance = distance;
    }

    // Each wasted press (an interact with nothing to interact with, a buy that bought nothing), once.
    if (life.Wasted > life.WastedPaid)
    {
        ledger.Add(RewardTerm::Wasted, -tuning.Wasted * float(life.Wasted - life.WastedPaid));
        life.WastedPaid = life.Wasted;
    }

    RewardMore(env, life, bot, ledger);

    bool const finished = Finished(env, life);
    bool const over = finished || life.Died || TimeIsUp(env);
    if (!over || life.OutcomePaid)
        return;
    life.OutcomePaid = true;
    life.Won = finished;
    if (!life.Recorded)
    {
        life.Recorded = true;
        if (life.Counts)
            _ladder.Record(life.Layout, life.Spec, life.Tier, finished, LifeWorld::BAND_COUNT - 1);
    }
}

void Animus::Curriculum::LifeEncounter::WriteState(Env const& env, float* state) const
{
    state[StageScenario::STATE_TIER] = float(_envs[env.Index].Tier) / float(LifeWorld::BAND_COUNT - 1);
}

bool Animus::Curriculum::LifeEncounter::IsTerminal(Env const& env) const
{
    EnvLife const& life = _envs[env.Index];
    return life.Died || life.Done || Finished(env, life) || TimeIsUp(env);
}
