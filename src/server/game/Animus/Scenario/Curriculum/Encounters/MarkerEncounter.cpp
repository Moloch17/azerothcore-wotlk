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

#include "MarkerEncounter.h"
#include "Encounters.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "StageState.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// The fastest turn the move block offers (MoveControls::TURN_RATES_DEG's 360), for the leg's optimum.
    constexpr float FASTEST_TURN = TWO_PI;

    float Lerp(float first, float last, float t)
    {
        return first + (last - first) * t;
    }

    /// The marker's bearing from `facing`, in (-pi, pi].
    float BearingFrom(Player const* bot, float facing, Position const& marker)
    {
        float const relative = bot->GetAngle(marker.GetPositionX(), marker.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }
}

Animus::Curriculum::MarkerEncounter::MarkerEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "marker rung")
{
}

Animus::Curriculum::MarkerRung Animus::Curriculum::MarkerEncounter::RungTask(uint32 rung, uint32 rungs,
    float distanceMin, float distanceFirst, float distanceLast, float bearingFirstDeg, float bearingLastDeg,
    float radiusFirst, float radiusLast)
{
    float const t = rungs > 1 ? float(std::min(rung, rungs - 1)) / float(rungs - 1) : 1.0f;
    MarkerRung task;
    task.Nearest = distanceMin;
    task.Furthest = std::max(distanceMin, Lerp(distanceFirst, distanceLast, t));
    task.BearingHalf = std::clamp(Lerp(bearingFirstDeg, bearingLastDeg, t), 0.0f, 180.0f) * float(M_PI) / 180.0f;
    task.Radius = std::max(0.1f, Lerp(radiusFirst, radiusLast, t));
    return task;
}

bool Animus::Curriculum::MarkerEncounter::Stopped(SeatState const& seat, float movedYards, float stopMoved)
{
    Movement::ControlState const& held = seat.Controls.Held;
    return held.Forward == 0 && held.Strafe == 0 && held.Vertical == 0 && !held.Jump
        && seat.Body.Kind == Movement::Mode::Ground && movedYards < stopMoved;
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::MarkerEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::StepCost, RewardTerm::Death, RewardTerm::Progress, RewardTerm::Facing };
}

void Animus::Curriculum::MarkerEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // `arrived` is the share of the episode's markers stopped on: the stage's measure, which the cost ladder and the
    // shaping fade are gated on (configs/move1_controls.yaml). `markers` is the count behind it.
    table.Add("arrived", [this](Env const& env, uint32)
    {
        EnvMarkers const& markers = _envs[env.Index];
        return markers.Wanted ? float(markers.Reached) / float(markers.Wanted) : 0.0f;
    });
    table.Add("markers", [this](Env const& env, uint32) { return float(_envs[env.Index].Reached); });
    table.Add("markers_wanted", [this](Env const& env, uint32) { return float(_envs[env.Index].Wanted); });
    table.Add("markers_broken", [this](Env const& env, uint32) { return _envs[env.Index].Broken ? 1.0f : 0.0f; });
    // The rung, and whether it is the top one: the stage is about the top rung (user, 2026-10-05), and the
    // evaluation spreads its seeds over every rung, so `arrived` at the top is read with this column.
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Rung >= TopRung(env) ? 1.0f : 0.0f;
    });
    table.Add("marker_radius", [this](Env const& env, uint32) { return _envs[env.Index].Task.Radius; });
    // Time taken per marker reached over the straight-line optimum (the distance at run speed, plus the turn to face
    // it at the fastest rate): 1 is a perfect leg. Mean over the legs reached; 0 when none was.
    table.Add("time_ratio", [this](Env const& env, uint32)
    {
        EnvMarkers const& markers = _envs[env.Index];
        return markers.Reached ? markers.TimeRatioSum / float(markers.Reached) : 0.0f;
    });
    // How far past the radius the seat ran after first reaching it, yards, mean over the legs that reached it.
    table.Add("overshoot", [this](Env const& env, uint32)
    {
        EnvMarkers const& markers = _envs[env.Index];
        return markers.OvershootLegs ? markers.OvershootSum / float(markers.OvershootLegs) : 0.0f;
    });
    // The distance from the marker at each stop near it (within Markers.StopNear), yards, mean: how precise the stops
    // are, whether or not they were inside the radius.
    table.Add("stop_distance", [this](Env const& env, uint32)
    {
        EnvMarkers const& markers = _envs[env.Index];
        return markers.Stops ? markers.StopDistanceSum / float(markers.Stops) : 0.0f;
    });
    table.Add("stops_near", [this](Env const& env, uint32) { return float(_envs[env.Index].Stops); });
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });
}

void Animus::Curriculum::MarkerEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvMarkers();
}

uint32 Animus::Curriculum::MarkerEncounter::TopRung(Env const& env) const
{
    uint32 const rungs = std::max<uint32>(1, _scenario.Tuning().Markers.Rungs);
    int32 const pinned = _scenario.ArenaMaxRung(env);
    return pinned >= 0 ? std::min<uint32>(uint32(pinned), rungs - 1) : rungs - 1;
}

bool Animus::Curriculum::MarkerEncounter::PlaceMarker(Env const& env, EnvMarkers& markers, Player* bot,
    float facing) const
{
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map)
        return false;

    CurriculumTuning::MarkerTuning const& tuning = _scenario.Tuning().Markers;
    TravelPlaceRules rules;
    rules.ArcCentre = facing;
    rules.ArcHalf = markers.Task.BearingHalf;
    rules.MaxDetour = tuning.MaxDetour;

    // Within what is left of the clock: TravelEncounter::FindPlace holds the walk to the share of it a trip may need.
    uint32 const leftMs = env.EpisodeLengthMs > env.EpisodeElapsedMs ? env.EpisodeLengthMs - env.EpisodeElapsedMs : 0;
    float const budget = env.EpisodeLengthMs ? float(leftMs) / 1000.0f : 0.0f;
    if (env.EpisodeLengthMs && budget <= 0.0f)
        return false;

    Position place;
    float walk = 0.0f;
    if (!TravelEncounter::FindPlace(bot, map, markers.Task.Nearest, markers.Task.Furthest, false, place, budget,
        &walk, false, nullptr, false, nullptr, rules))
        return false;

    markers.HasMarker = true;
    markers.Marker.Relocate(place);
    ++markers.Placed;
    markers.LegStartMs = env.EpisodeElapsedMs;
    markers.LegStraight = bot->GetExactDist2d(&place);
    markers.LegBearing = std::fabs(BearingFrom(bot, facing, place));
    markers.LastDistance = -1.0f;
    markers.LastFacingCos = -2.0f;
    markers.Entered = false;
    markers.LegOvershoot = 0.0f;
    return true;
}

bool Animus::Curriculum::MarkerEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;

    CurriculumTuning::MarkerTuning const& tuning = _scenario.Tuning().Markers;
    EnvMarkers& markers = _envs[env.Index];
    markers = EnvMarkers();

    // The rung: spread over the seeds in an evaluation, the class and build's own in training (now and then one
    // below it or one above, which do not count).
    SeatState const& seat = data.Seats[0];
    markers.Layout = seat.L ? seat.L->Index : 0;
    markers.Spec = seat.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, markers.Layout, markers.Spec, TopRung(env));
    markers.Rung = pick.Tier;
    markers.Counts = pick.Counts;
    markers.Task = RungTask(markers.Rung, std::max<uint32>(1, tuning.Rungs), tuning.DistanceMin, tuning.DistanceFirst,
        tuning.DistanceLast, tuning.BearingFirst, tuning.BearingLast, tuning.RadiusFirst, tuning.RadiusLast);
    markers.Wanted = urand(std::min(tuning.MarkersMin, tuning.MarkersMax), std::max(tuning.MarkersMin,
        tuning.MarkersMax));
    markers.Wanted = std::max<uint32>(1, markers.Wanted);

    // The scenario seeds the seat's facing from the bot after the encounters are built, so the bot's own is the one
    // the first marker's bearing is measured from.
    return PlaceMarker(env, markers, bot, bot->GetOrientation());
}

bool Animus::Curriculum::MarkerEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seats act without a target (SeatEncoder::ActsWithoutTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::MarkerEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    EnvMarkers const& markers = _envs[env.Index];
    view.HasObjective = markers.HasMarker;
    view.Objective = markers.Marker;
    view.MountsAllowed = false;
    view.GroundMountAllowed = false;
    // The rung's radius, for any block that reads it (TravelBlock's OBS_AT_OBJECTIVE). The move block, which is what
    // M1 carries, sees the marker's bearing and distance but not the radius: the rung is learned from the reward.
    view.ArriveWithin = markers.Task.Radius;
}

void Animus::Curriculum::MarkerEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::MarkerTuning const& tuning = _scenario.Tuning().Markers;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvMarkers& markers = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (!bot)
        return;

    // How far the feet went since the last decision: "still" is read from it, since the body's own velocity is the
    // controller's and may be reset by a resync.
    float moved = 0.0f;
    if (markers.HasLastPos)
    {
        float const dx = bot->GetPositionX() - markers.LastX;
        float const dy = bot->GetPositionY() - markers.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
        markers.Travelled += moved;
    }
    markers.LastX = bot->GetPositionX();
    markers.LastY = bot->GetPositionY();
    bool const firstLook = !markers.HasLastPos;
    markers.HasLastPos = true;

    CombatTally& tally = seat.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }

    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (markers.HasMarker && markers.Reached < markers.Wanted && !tally.TimedOut && timeIsUp)
        tally.TimedOut = true;

    // A death ends the episode short of its last marker: a loss on the ladder. (The clock's is recorded after the
    // arrival check, so a stop on the last marker in the last decision still counts as the win it is.)
    if (!bot->IsAlive())
        RecordRung(env, markers);

    if (!markers.HasMarker || !bot->IsAlive())
    {
        if (timeIsUp)
            RecordRung(env, markers);
        return;
    }

    // The two potentials, each started over with every new marker (a new potential function: the jump in distance
    // to the next one is not ground given back).
    float const distance = bot->GetExactDist2d(&markers.Marker);
    float const leg = std::max(markers.LegStraight, markers.Task.Radius);
    if (markers.LastDistance >= 0.0f)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (markers.LastDistance - distance) / leg);
    markers.LastDistance = distance;

    float const facingCos = std::cos(BearingFrom(bot, seat.Facing, markers.Marker));
    if (markers.LastFacingCos >= -1.0f)
        ledger.Add(RewardTerm::Facing, tuning.Facing * 0.5f * (facingCos - markers.LastFacingCos));
    markers.LastFacingCos = facingCos;

    // Running through: how far past the radius the seat went after first being inside it.
    bool const inside = distance <= markers.Task.Radius;
    if (inside)
        markers.Entered = true;
    else if (markers.Entered)
        markers.LegOvershoot = std::max(markers.LegOvershoot, distance - markers.Task.Radius);

    // A stop is the decision the seat came to rest on after moving; one near the marker is measured whether or not it
    // was in. The first look has no last position to measure "still" from: the seat starts at rest, so it is read as
    // stopped there, and only a stop after moving counts.
    bool const stopped = firstLook || Stopped(seat, moved, tuning.StopMoved);
    if (!firstLook && stopped && !markers.WasStopped && distance <= tuning.StopNear)
    {
        markers.StopDistanceSum += distance;
        ++markers.Stops;
    }
    markers.WasStopped = stopped;

    if (firstLook || !(stopped && inside))
    {
        if (timeIsUp)
            RecordRung(env, markers);
        return;
    }

    // Stopped on it: paid, measured, and the next one placed from here -- or the episode is done.
    ++markers.Reached;
    ledger.Add(RewardTerm::Arrive, tuning.Arrive);
    float const seconds = float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, markers.LegStartMs)) / 1000.0f;
    float const run = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
    float const optimum = std::max(0.25f, std::max(0.0f, markers.LegStraight - markers.Task.Radius) / run
        + markers.LegBearing / FASTEST_TURN);
    markers.TimeRatioSum += seconds / optimum;
    if (markers.Entered)
    {
        markers.OvershootSum += markers.LegOvershoot;
        ++markers.OvershootLegs;
    }

    if (markers.Reached >= markers.Wanted)
    {
        markers.HasMarker = false;
        RecordRung(env, markers);
        return;
    }

    // No next marker in reach is the ground's shortfall, not the seat's: the episode ends and the ladder hears
    // nothing of it.
    if (!PlaceMarker(env, markers, bot, seat.Facing))
    {
        markers.HasMarker = false;
        markers.Broken = true;
        markers.Recorded = true;
    }
    else if (timeIsUp)
        RecordRung(env, markers);
}

void Animus::Curriculum::MarkerEncounter::RecordRung(Env const& env, EnvMarkers& markers)
{
    if (!markers.Counts || markers.Recorded)
        return;
    markers.Recorded = true;
    _ladder.Record(markers.Layout, markers.Spec, markers.Rung, markers.Reached >= markers.Wanted, TopRung(env));
}

bool Animus::Curriculum::MarkerEncounter::IsTerminal(Env const& env) const
{
    EnvMarkers const& markers = _envs[env.Index];
    return (markers.Wanted && (markers.Reached >= markers.Wanted || markers.Broken)) || _scenario.DeadForGood(env, 0);
}
