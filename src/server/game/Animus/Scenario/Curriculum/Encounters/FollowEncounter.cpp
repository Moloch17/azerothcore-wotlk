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

#include "FollowEncounter.h"
#include "Creature.h"
#include "Encounters.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Map.h"
#include "MarkerEncounter.h"
#include "Player.h"
#include "Random.h"
#include "Seek.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "StageState.h"
#include <algorithm>
#include <cmath>

namespace
{
    /// The leader has reached its trip's end, or a corner of its way, this close.
    constexpr float LEADER_ARRIVE = 3.0f;
    /// A scripted leader stuck this long on one trip (the controller's own count) gives it up for another.
    constexpr uint32 LEADER_GIVE_UP_MS = 4000;
    /// Where the leader starts beside the spawn point.
    constexpr float LEADER_START_OFFSET = 3.0f;
    /// A route re-planned when the leader strays this far from the corner it walks to.
    constexpr float LEADER_STRAY = 15.0f;
}

Animus::Curriculum::FollowEncounter::FollowEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "follow rung")
{
}

float Animus::Curriculum::FollowEncounter::TripFurthest(uint32 rung, uint32 rungs, float first, float last)
{
    float const t = rungs > 1 ? float(std::min(rung, rungs - 1)) / float(rungs - 1) : 1.0f;
    return first + (last - first) * t;
}

uint32 Animus::Curriculum::FollowEncounter::Band(float distance, float bandMin, float bandMax, float lostYards)
{
    if (distance < bandMin)
        return 0;
    if (distance <= bandMax)
        return 1;
    return distance <= lostYards ? 2 : 3;
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::FollowEncounter::RewardTerms() const
{
    return { RewardTerm::FollowKept, RewardTerm::Lost, RewardTerm::Aggro, RewardTerm::Progress, RewardTerm::Death,
        RewardTerm::Stuck, RewardTerm::Wall, RewardTerm::FallDamage };
}

void Animus::Curriculum::FollowEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // The stage's measure: the share of the episode within the band of the leader.
    table.Add("follow_in_band_share", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].InBandMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    table.Add("follow_distance_mean", [this](Env const& env, uint32)
    {
        EnvFollow const& follow = _envs[env.Index];
        return follow.Samples ? float(follow.DistanceSum / follow.Samples) : 0.0f;
    });
    table.Add("follow_distance_sd", [this](Env const& env, uint32)
    {
        EnvFollow const& follow = _envs[env.Index];
        if (follow.Samples < 2)
            return 0.0f;
        double const mean = follow.DistanceSum / follow.Samples;
        return float(std::sqrt(std::max(0.0, follow.DistanceSquares / follow.Samples - mean * mean)));
    });
    table.Add("catch_ups", [this](Env const& env, uint32) { return float(_envs[env.Index].CatchUps); });
    table.Add("lost_seconds", [this](Env const& env, uint32) { return float(_envs[env.Index].LostMs) / 1000.0f; });
    table.Add("aggro_pulled", [this](Env const& env, uint32) { return float(_envs[env.Index].AggroPulled); });
    table.Add("leader_cast", [this](Env const& env, uint32) { return _envs[env.Index].Cast ? 1.0f : 0.0f; });
    // How the leader went: into the water, and jumps (the controller's count). A scripted leader does neither; a cast
    // one may, and the follower has to follow it there.
    table.Add("leader_swims", [this](Env const& env, uint32) { return float(_envs[env.Index].LeaderSwims); });
    table.Add("leader_jumps", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Built ? float(_scenario.Data(env).Seats[_scenario.OwnerAgent()].Jumps) : 0.0f;
    });
    table.Add("leader_trips", [this](Env const& env, uint32) { return float(_envs[env.Index].Trips); });
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Rung + 1 >= std::max<uint32>(1, _scenario.Tuning().Follow.Rungs) ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::FollowEncounter::ResetEpisode(Env& env)
{
    // The leader's character stays until Build replaces it (Deactivate and Teardown send it away).
    EnvFollow& follow = _envs[env.Index];
    bool const built = follow.Built;
    follow = EnvFollow();
    follow.Built = built;
}

bool Animus::Curriculum::FollowEncounter::Build(Env& env, Map* map, uint8 level)
{
    EnvFollow& follow = _envs[env.Index];
    CurriculumTuning::FollowTuning const& tuning = _scenario.Tuning().Follow;
    Player* seat = _scenario.SeatBot(env, 0);
    if (!seat || !map)
        return false;

    // Last episode's leader goes; this one's is built fresh in the owner's slot.
    if (follow.Built)
        _scenario.ReleaseOwnerSeat(env);
    follow = EnvFollow();

    SeatState const& state = _scenario.Data(env).Seats[0];
    follow.Layout = state.L ? state.L->Index : 0;
    follow.Spec = state.Spec;
    uint32 const rungs = std::max<uint32>(1, tuning.Rungs);
    DifficultyLadder::Pick const pick = _ladder.Draw(env, follow.Layout, follow.Spec, rungs - 1);
    follow.Rung = pick.Tier;
    follow.Counts = pick.Counts;
    follow.Cast = !env.Evaluating && follow.Rung >= tuning.CastFromRung && roll_chance_i(tuning.CastShare);

    Position start = _scenario.SpawnPointFor(env);
    start.m_positionX += LEADER_START_OFFSET;
    Map* leaderMap = map;
    Player* leader = _scenario.BuildOwnerSeat(env, leaderMap, level, start, AptitudeDemand::Anything());
    if (!leader)
        return false;
    follow.Built = true;
    // The seat's faction, so the two are friends.
    leader->SetFaction(seat->GetFaction());
    env.Allies = { leader->GetGUID() };
    return NextTrip(env, follow, leader);
}

bool Animus::Curriculum::FollowEncounter::NextTrip(Env const& env, EnvFollow& follow, Player* leader) const
{
    Map* map = leader ? leader->GetMap() : nullptr;
    if (!map)
        return false;

    CurriculumTuning::FollowTuning const& tuning = _scenario.Tuning().Follow;
    float const furthest = std::max(tuning.TripNearest, TripFurthest(follow.Rung, std::max<uint32>(1, tuning.Rungs),
        tuning.TripFurthestFirst, tuning.TripFurthestLast));
    // The whole way planned by the RoutePlanner (PathGenerator stops short at the long trips), walkable by the
    // controller with no drop past RouteMaxDrop. A scripted leader's keys never jump or climb out of water, so its
    // trips are dry and jump-free; a cast leader (an M6 policy) gets M5/M6's ground -- swims and jumps allowed -- and
    // the follower learns to follow it there.
    TravelPlaceRules rules;
    rules.LongRoute = true;
    rules.ControllerReach = true;
    rules.RouteMaxDrop = _scenario.Tuning().Markers.RouteMaxDrop;
    rules.DryOnly = !follow.Cast;
    rules.NoJump = !follow.Cast;
    Position place;
    if (!TravelEncounter::FindPlace(leader, map, tuning.TripNearest, furthest, false, place, 0.0f, nullptr, false,
        nullptr, false, nullptr, rules))
        return false;

    follow.HasTrip = true;
    follow.Trip.Relocate(place);
    follow.Way.Clear();
    follow.TripStartMs = env.EpisodeElapsedMs;
    follow.TripStuckMs = _scenario.Data(env).Seats[_scenario.OwnerAgent()].StuckMs;
    ++follow.Trips;
    return true;
}

void Animus::Curriculum::FollowEncounter::Update(Env& env)
{
    EnvFollow& follow = _envs[env.Index];
    uint32 const agent = _scenario.OwnerAgent();
    Player* leader = follow.Built ? _scenario.SeatBot(env, agent) : nullptr;
    if (!leader || !leader->IsInWorld() || !leader->IsAlive())
        return;

    SeatState& seat = _scenario.Data(env).Seats[agent];
    bool const wet = leader->IsInWater();
    follow.LeaderSwims += wet && !follow.LeaderWasWet ? 1 : 0;
    follow.LeaderWasWet = wet;
    bool const arrived = follow.HasTrip && leader->GetExactDist2d(&follow.Trip) <= LEADER_ARRIVE;
    bool const stuck = follow.HasTrip && seat.StuckMs - std::min(seat.StuckMs, follow.TripStuckMs)
        >= LEADER_GIVE_UP_MS;
    if ((!follow.HasTrip || arrived || stuck) && !NextTrip(env, follow, leader))
        follow.HasTrip = false;

    // A cast leader plays its own row (its objective is the trip's end); the script's leader holds the seek
    // helper's keys toward the next corner of its way.
    if (follow.Cast)
        return;

    // Not before the controller has started this episode: its body is the world's from then on, and seeking from a
    // default one would turn toward a bearing measured from nowhere.
    Movement::ControlState held;
    if (follow.HasTrip && seat.Mover.Started())
    {
        if (!follow.Way.Valid || follow.Way.To.GetExactDist2d(&follow.Trip) > 1.0f)
        {
            Position const from(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), 0.0f);
            RoutePlanner::Instance().Plan(leader->GetMap(), from, follow.Trip, follow.Way);
        }
        float x = follow.Trip.GetPositionX();
        float y = follow.Trip.GetPositionY();
        if (follow.Way.Valid)
        {
            follow.Way.Advance(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                LEADER_ARRIVE);
            if (follow.Way.Next < follow.Way.Count)
            {
                x = follow.Way.X[follow.Way.Next];
                y = follow.Way.Y[follow.Way.Next];
                if (leader->GetExactDist2d(x, y) > LEADER_STRAY)
                    follow.Way.Valid = false;       // strayed: planned again next decision
            }
        }
        held = Movement::Seek(seat.Mover.Body, x, y);
        held.Walk = follow.Rung < _scenario.Tuning().Follow.WalkRungs;
    }
    seat.Controls.Held = held;
}

bool Animus::Curriculum::FollowEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight.
    target = nullptr;
    return true;
}

void Animus::Curriculum::FollowEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvFollow const& follow = _envs[env.Index];
    view.MountsAllowed = true;
    view.GroundMountAllowed = true;
    if (seat == _scenario.OwnerAgent())
    {
        // The cast leader: its trip's end is its objective, as a marker was in the stage it was trained on.
        view.HasObjective = follow.HasTrip;
        view.Objective = follow.Trip;
        view.ArriveWithin = LEADER_ARRIVE;
        return;
    }

    // The follower: the leader is its objective.
    Player const* leader = follow.Built ? _scenario.SeatBot(env, _scenario.OwnerAgent()) : nullptr;
    view.HasObjective = leader != nullptr;
    if (leader)
        view.Objective.Relocate(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
    view.ArriveWithin = _scenario.Tuning().Follow.BandMax;
}

void Animus::Curriculum::FollowEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    if (seatIndex != 0 || !bot)
        return;

    EnvFollow& follow = _envs[env.Index];
    CurriculumTuning::FollowTuning const& tuning = _scenario.Tuning().Follow;
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    uint32 const stepMs = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, follow.LastRewardMs);
    follow.LastRewardMs = env.EpisodeElapsedMs;
    float const seconds = float(stepMs) / 1000.0f;

    CombatTally& tally = seat.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -_scenario.Tuning().Markers.Death);
    }
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (!bot->IsAlive() || timeIsUp)
        RecordRung(env, follow);

    // Hostile creatures newly on the seat: keeping clear of aggro while travelling.
    for (Unit* attacker : bot->getAttackers())
        if (attacker && attacker->IsCreature() && follow.Attackers.insert(attacker->GetGUID()).second)
        {
            ++follow.AggroPulled;
            ledger.Add(RewardTerm::Aggro, -tuning.Aggro);
        }

    // The ground courses' costs, off the controller's own counts.
    CurriculumTuning::MarkerGroundTuning const& costs = _scenario.Tuning().MarkerGround;
    uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, follow.LastStuckMs);
    uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, follow.LastWallMs);
    follow.LastStuckMs = seat.StuckMs;
    follow.LastWallMs = seat.WallMs;
    if (stuckMs)
        ledger.Add(RewardTerm::Stuck, -costs.Stuck * float(stuckMs) / 1000.0f);
    // How far the unit went since the last decision (the server's position), for the wall charge.
    float moved = 0.0f;
    if (follow.HasLastPos)
    {
        float const dx = bot->GetPositionX() - follow.LastX;
        float const dy = bot->GetPositionY() - follow.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
    }
    follow.LastX = bot->GetPositionX();
    follow.LastY = bot->GetPositionY();
    follow.HasLastPos = true;
    if (wallMs)
    {
        // As the marker stages: a slide along the wall that keeps most of the ground asked for is free.
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
        float const charge = MarkerEncounter::WallCharge(float(wallMs) / 1000.0f, moved, asked, costs.Wall,
            costs.WallSlide);
        if (charge > 0.0f)
            ledger.Add(RewardTerm::Wall, -charge);
    }
    float const took = seat.FallDamage - std::min(seat.FallDamage, follow.LastFallDamage);
    follow.LastFallDamage = seat.FallDamage;
    if (took > 0.0f)
        ledger.Add(RewardTerm::FallDamage, -_scenario.Tuning().MarkerVertical.FallDamage * took);

    Player* leader = follow.Built ? _scenario.SeatBot(env, _scenario.OwnerAgent()) : nullptr;
    if (!leader || !leader->IsInWorld() || !bot->IsAlive())
        return;

    float const distance = bot->GetExactDist(leader);
    follow.DistanceSum += distance;
    follow.DistanceSquares += double(distance) * distance;
    ++follow.Samples;

    uint32 const band = Band(distance, tuning.BandMin, tuning.BandMax, tuning.LostYards);
    if (band == 1)
    {
        follow.InBandMs += stepMs;
        ledger.Add(RewardTerm::FollowKept, tuning.Kept * seconds);
        if (!follow.InBand && follow.OutSinceMs
            && env.EpisodeElapsedMs - follow.OutSinceMs >= uint32(tuning.CatchUpSeconds * 1000.0f))
            ++follow.CatchUps;
        follow.InBand = true;
        follow.OutSinceMs = 0;
    }
    else
    {
        if (follow.InBand || !follow.OutSinceMs)
            follow.OutSinceMs = std::max<uint32>(1, env.EpisodeElapsedMs);
        follow.InBand = false;
    }
    if (band == 3)
    {
        follow.LostMs += stepMs;
        ledger.Add(RewardTerm::Lost, -tuning.Lost * seconds);
    }

    // Closing to the band: a potential on the yards past it, capped at LostYards.
    float const excess = std::clamp(distance - tuning.BandMax, 0.0f, tuning.LostYards);
    if (follow.LastExcess >= 0.0f)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (follow.LastExcess - excess)
            / std::max(1.0f, tuning.LostYards));
    follow.LastExcess = excess;
}

bool Animus::Curriculum::FollowEncounter::IsTerminal(Env const& env) const
{
    // The episode runs to its clock: following has no end but the follower's death.
    return _scenario.DeadForGood(env, 0);
}

void Animus::Curriculum::FollowEncounter::RecordRung(Env const& env, EnvFollow& follow)
{
    if (!follow.Counts || follow.Recorded)
        return;
    follow.Recorded = true;
    CurriculumTuning::FollowTuning const& tuning = _scenario.Tuning().Follow;
    bool const won = env.EpisodeElapsedMs
        && float(follow.InBandMs) >= tuning.WinShare * float(env.EpisodeElapsedMs);
    _ladder.Record(follow.Layout, follow.Spec, follow.Rung, won, std::max<uint32>(1, tuning.Rungs) - 1);
}

void Animus::Curriculum::FollowEncounter::Deactivate(Env& env)
{
    Teardown(env);
}

void Animus::Curriculum::FollowEncounter::Teardown(Env& env)
{
    EnvFollow& follow = _envs[env.Index];
    if (follow.Built)
        _scenario.ReleaseOwnerSeat(env);
    follow.Built = false;
    env.Allies.clear();
}
