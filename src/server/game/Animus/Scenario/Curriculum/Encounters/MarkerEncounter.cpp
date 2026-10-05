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
#include "UnitDefines.h"
#include "MoveBlock.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// The fastest turn the move block offers (MoveControls::TURN_RATES_DEG's 360), for the leg's optimum.
    constexpr float FASTEST_TURN = TWO_PI;
    /// A storey's height: a marker this far up or down is on another floor (storey_legs).
    constexpr float STOREY = 3.0f;
    /// Indoors, a marker on the seat's own floor may sit a stair or two up from its feet.
    constexpr float INDOOR_LEVEL = 2.5f;

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

Animus::Curriculum::MarkerRung Animus::Curriculum::MarkerEncounter::GroundRungTask(uint32 rung, uint32 rungs,
    float distanceMin, float distanceFirst, float distanceLast, float detourFirst, float detourLast, float detourSpan,
    float radius)
{
    float const t = rungs > 1 ? float(std::min(rung, rungs - 1)) / float(rungs - 1) : 1.0f;
    MarkerRung task;
    task.Nearest = distanceMin;
    task.Furthest = std::max(distanceMin, Lerp(distanceFirst, distanceLast, t));
    task.BearingHalf = float(M_PI);
    task.Radius = std::max(0.1f, radius);
    task.DetourMin = std::max(1.0f, Lerp(detourFirst, detourLast, t));
    task.DetourMax = task.DetourMin + std::max(0.05f, detourSpan);
    return task;
}

float Animus::Curriculum::MarkerEncounter::WallCharge(float wallSeconds, float moved, float asked, float price,
    float slideShare)
{
    if (wallSeconds <= 0.0f || price <= 0.0f)
        return 0.0f;
    float const share = std::clamp(slideShare, 0.0f, 1.0f);
    float const ratio = asked > 1e-4f ? std::clamp(moved / asked, 0.0f, 1.0f) : 0.0f;
    float const blocked = share > 0.0f ? std::clamp((share - ratio) / share, 0.0f, 1.0f) : 0.0f;
    return price * wallSeconds * blocked;
}

Animus::Curriculum::MarkerRung Animus::Curriculum::MarkerEncounter::VerticalRungTask(uint32 rung, uint32 rungs,
    float distanceMin, float distanceFirst, float distanceLast, float heightMinFirst, float heightMaxFirst,
    float heightMinLast, float heightMaxLast, float radius)
{
    float const t = rungs > 1 ? float(std::min(rung, rungs - 1)) / float(rungs - 1) : 1.0f;
    MarkerRung task;
    task.Nearest = distanceMin;
    task.Furthest = std::max(distanceMin, Lerp(distanceFirst, distanceLast, t));
    task.BearingHalf = float(M_PI);
    task.Radius = std::max(0.1f, radius);
    task.HeightMin = std::max(0.0f, Lerp(heightMinFirst, heightMinLast, t));
    task.HeightMax = std::max(task.HeightMin + 1.0f, Lerp(heightMaxFirst, heightMaxLast, t));
    return task;
}

bool Animus::Curriculum::MarkerEncounter::Stopped(uint32 movementFlags, float movedYards, float stopMoved)
{
    constexpr uint32 MOVING = MOVEMENTFLAG_MASK_MOVING | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING;
    return (movementFlags & MOVING) == 0 && movedYards < stopMoved;
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::MarkerEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::StepCost, RewardTerm::Death, RewardTerm::Progress, RewardTerm::Facing,
        RewardTerm::Stuck, RewardTerm::Wall, RewardTerm::FallDamage, RewardTerm::Drowning };
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
    // Whether the markers are reached by walking or by the class's movement spells: a blink is the server moving the
    // body (imposed motion, which the controller yields to and resyncs from), so a stage learned by blinking reads
    // here and not in the walking columns.
    // The ground course: the walking way over the straight line of the markers placed (mean), what the detour ladder
    // actually got.
    table.Add("detour", [this](Env const& env, uint32)
    {
        EnvMarkers const& markers = _envs[env.Index];
        return markers.Legs ? markers.DetourSum / float(markers.Legs) : 0.0f;
    });
    // The vertical course: how far up or down the markers were (mean |height over the seat| at each leg's start),
    // and how many legs changed a storey (3 yd or more).
    table.Add("marker_rise", [this](Env const& env, uint32)
    {
        EnvMarkers const& markers = _envs[env.Index];
        return markers.Legs ? markers.RiseSum / float(markers.Legs) : 0.0f;
    });
    table.Add("storey_legs", [this](Env const& env, uint32) { return float(_envs[env.Index].StoreyLegs); });
    table.Add("storey_up_legs", [this](Env const& env, uint32) { return float(_envs[env.Index].StoreyUpLegs); });
    table.Add("storey_down_legs", [this](Env const& env, uint32) { return float(_envs[env.Index].StoreyDownLegs); });
    // The water course: crossings placed (a dry way round existed and was the longer), and the seat's climbs out of
    // the water onto land. swim_seconds, breaths and drowned are the scenario's own columns.
    table.Add("crossings", [this](Env const& env, uint32) { return float(_envs[env.Index].Crossings); });
    // What Drowning is paid on: the share of maximum health the seat's own damage (no attacker: drowning, mostly,
    // and a fall that ends under the surface) took while it was under the water.
    table.Add("underwater_self_damage", [this](Env const& env, uint32)
    {
        return _envs[env.Index].UnderwaterSelfDamage;
    });
    table.Add("banks_climbed", [this](Env const& env, uint32) { return float(_envs[env.Index].BanksClimbed); });
    table.Add("movement_casts", [this](Env const& env, uint32) { return float(_envs[env.Index].MovementCasts); });
    table.Add("speed_casts", [this](Env const& env, uint32) { return float(_envs[env.Index].SpeedCasts); });
}

void Animus::Curriculum::MarkerEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvMarkers();
}

uint32 Animus::Curriculum::MarkerEncounter::TopRung(Env const& env) const
{
    MarkerCourse const course = _scenario.Arena(env).Course;
    uint32 const rungs = std::max<uint32>(1, course == MarkerCourse::Ground ? _scenario.Tuning().MarkerGround.Rungs
        : course == MarkerCourse::Vertical ? _scenario.Tuning().MarkerVertical.Rungs
        : course == MarkerCourse::Water ? _scenario.Tuning().MarkerWater.Rungs
        : _scenario.Tuning().Markers.Rungs);
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
    rules.MaxDetour = markers.Task.DetourMax > 0.0f ? markers.Task.DetourMax : tuning.MaxDetour;
    rules.MinDetour = markers.Task.DetourMin > 1.0f ? markers.Task.DetourMin : 0.0f;
    // No swimming before the water stage: the walking way is planned on dry ground alone, and it has to be one the
    // player controller can walk (every rise a step or a jump): no marker is placed where the seat cannot get.
    rules.DryOnly = markers.Course != MarkerCourse::Water;
    rules.ControllerReach = true;
    // Drops are allowed on the way, never a near-fatal one: 20 yd takes 12% of maximum health (Markers.RouteMaxDrop).
    rules.RouteMaxDrop = tuning.RouteMaxDrop;
    float nearest = markers.Task.Nearest;
    float furthest = markers.Task.Furthest;
    bool upstairs = false;
    bool across = false;

    // The vertical course: above (a climb), below a ledge, or on another floor, within the rung's height window.
    ArenaDefinition const& arena = _scenario.Arena(env);
    bool const indoors = arena.Indoors;
    if (markers.Course == MarkerCourse::Water)
    {
        // Across water whose dry way round is the longer one (both ways walkable or swimmable by the controller),
        // or on a lakebed within the rung's depth window, swum to straight.
        rules.MaxDetour = 0.0f;
        rules.MinDetour = 0.0f;
        if (arena.Underwater)
        {
            rules.Underwater = true;
            rules.DepthMin = markers.Task.HeightMin;
            rules.DepthMax = markers.Task.HeightMax;
        }
        else
            across = true;
    }
    else if (markers.Course == MarkerCourse::Vertical)
    {
        CurriculumTuning::TravelTuning const& travel = _scenario.Tuning().Travel;
        CurriculumTuning::MarkerVerticalTuning const& vertical = _scenario.Tuning().MarkerVertical;
        // Up a ramp, a stair or a slope may be the long way round: the route's detour is the ground's to decide.
        rules.MaxDetour = 0.0f;
        rules.MinDetour = 0.0f;
        if (arena.Ledges)
        {
            // Below a ledge on the straight line: the drop is the shortcut, the way round (at least LedgeDetour times
            // the line) takes no drop past SafeDrop.
            rules.Ledge = true;
            rules.LedgeDetour = travel.LedgeDetour;
            rules.DropMin = std::max(markers.Task.HeightMin, MoveBlock::MAX_STEP);
            rules.DropMax = markers.Task.HeightMax;
            rules.RouteMaxDrop = vertical.SafeDrop;
        }
        else if (indoors)
        {
            // A room's markers: a share of them a storey or two up the stairs, the rest down or on the seat's floor
            // (the first curriculum's indoor trip lengths: a building is twenty to forty yards across).
            nearest = travel.IndoorMin;
            furthest = std::max(travel.IndoorMin, std::min(markers.Task.Furthest, travel.IndoorMax));
            upstairs = frand(0.0f, 1.0f) < vertical.RoomUpShare;
            rules.HasRise = true;
            rules.RiseMin = -markers.Task.HeightMax;
            rules.RiseMax = INDOOR_LEVEL;
        }
        else
        {
            rules.HasRise = true;
            rules.RiseMin = markers.Task.HeightMin;
            rules.RiseMax = markers.Task.HeightMax;
        }
    }

    // Within what is left of the clock: TravelEncounter::FindPlace holds the walk to the share of it a trip may need.
    uint32 const leftMs = env.EpisodeLengthMs > env.EpisodeElapsedMs ? env.EpisodeLengthMs - env.EpisodeElapsedMs : 0;
    float const budget = env.EpisodeLengthMs ? float(leftMs) / 1000.0f : 0.0f;
    if (env.EpisodeLengthMs && budget <= 0.0f)
        return false;

    Position place;
    float walk = 0.0f;
    float dry = 0.0f;
    bool placed = false;
    if (upstairs)
    {
        // Up the stairs: the lowest floor with headroom a storey or two over the seat's, and the stairs walkable.
        // A building with no such floor in reach (a single-storey inn) takes a down-or-level marker instead.
        TravelPlaceRules up = rules;
        up.Upstairs = true;
        up.RiseMin = STOREY;
        up.RiseMax = std::max(STOREY + 1.0f, _scenario.Tuning().MarkerVertical.UpstairsRise);
        placed = TravelEncounter::FindPlace(bot, map, nearest, furthest, false, place, budget, &walk, false, nullptr,
            indoors, nullptr, up);
    }
    if (!placed && !TravelEncounter::FindPlace(bot, map, nearest, furthest, false, place, budget, &walk, across,
        &dry, indoors, nullptr, rules))
        return false;

    markers.HasMarker = true;
    markers.Marker.Relocate(place);
    ++markers.Placed;
    markers.LegStartMs = env.EpisodeElapsedMs;
    markers.LegStraight = bot->GetExactDist2d(&place);
    markers.LegWalk = walk > 0.0f ? walk : markers.LegStraight;
    markers.LegDry = dry;
    markers.LegUnder = arena.Underwater;
    markers.Crossings += across && dry > 0.0f ? 1 : 0;
    markers.DetourSum += markers.LegWalk / std::max(1.0f, markers.LegStraight);
    ++markers.Legs;
    float const rise = std::fabs(place.GetPositionZ() - bot->GetPositionZ());
    markers.RiseSum += rise;
    markers.StoreyLegs += rise >= STOREY ? 1 : 0;
    markers.StoreyUpLegs += place.GetPositionZ() - bot->GetPositionZ() >= STOREY ? 1 : 0;
    markers.StoreyDownLegs += bot->GetPositionZ() - place.GetPositionZ() >= STOREY ? 1 : 0;
    markers.Way.Clear();
    markers.WayFailed = false;
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
    markers.Course = _scenario.Arena(env).Course;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, markers.Layout, markers.Spec, TopRung(env));
    markers.Rung = pick.Tier;
    markers.Counts = pick.Counts;
    if (markers.Course == MarkerCourse::Water)
    {
        CurriculumTuning::MarkerWaterTuning const& water = _scenario.Tuning().MarkerWater;
        markers.Task = VerticalRungTask(markers.Rung, std::max<uint32>(1, water.Rungs), water.DistanceMin,
            water.DistanceFirst, water.DistanceLast, water.DepthMinFirst, water.DepthMaxFirst, water.DepthMinLast,
            water.DepthMaxLast, water.Radius);
        bool const chain = _scenario.Arena(env).Checkpoints;
        uint32 const least = chain ? water.ChainMin : water.MarkersMin;
        uint32 const most = chain ? water.ChainMax : water.MarkersMax;
        markers.Wanted = urand(std::min(least, most), std::max(least, most));
    }
    else if (markers.Course == MarkerCourse::Vertical)
    {
        CurriculumTuning::MarkerVerticalTuning const& vertical = _scenario.Tuning().MarkerVertical;
        markers.Task = VerticalRungTask(markers.Rung, std::max<uint32>(1, vertical.Rungs), vertical.DistanceMin,
            vertical.DistanceFirst, vertical.DistanceLast, vertical.HeightMinFirst, vertical.HeightMaxFirst,
            vertical.HeightMinLast, vertical.HeightMaxLast, vertical.Radius);
        markers.Wanted = urand(std::min(vertical.MarkersMin, vertical.MarkersMax), std::max(vertical.MarkersMin,
            vertical.MarkersMax));
    }
    else if (markers.Course == MarkerCourse::Ground)
    {
        CurriculumTuning::MarkerGroundTuning const& ground = _scenario.Tuning().MarkerGround;
        markers.Task = GroundRungTask(markers.Rung, std::max<uint32>(1, ground.Rungs), ground.DistanceMin,
            ground.DistanceFirst, ground.DistanceLast, ground.DetourFirst, ground.DetourLast, ground.DetourSpan,
            ground.Radius);
        markers.Wanted = urand(std::min(ground.MarkersMin, ground.MarkersMax), std::max(ground.MarkersMin,
            ground.MarkersMax));
    }
    else
    {
        markers.Task = RungTask(markers.Rung, std::max<uint32>(1, tuning.Rungs), tuning.DistanceMin,
            tuning.DistanceFirst, tuning.DistanceLast, tuning.BearingFirst, tuning.BearingLast, tuning.RadiusFirst,
            tuning.RadiusLast);
        markers.Wanted = urand(std::min(tuning.MarkersMin, tuning.MarkersMax), std::max(tuning.MarkersMin,
            tuning.MarkersMax));
    }
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

    markers.MovementCasts += env.StepStats[seatIndex].MovementCasts;
    markers.SpeedCasts += env.StepStats[seatIndex].SpeedCasts;

    // How far the unit went since the last decision, on the server's applied position (§5A.1): arrival, the marker's
    // distance and "still" are judged on what the server holds. Only the seat's own view of the marker (the move
    // block's bearing and distance, and the Facing shaping) reads its body and facing.
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

    // The ground course's costs, read off the controller's own counts (StageScenario::TrackController): the
    // seconds since the last decision it was stuck with a key held, or pressing into a wall.
    // The vertical course climbs on the ground course's route and pays its costs, and its falls besides.
    // The water course pays Stuck and Wall too, and what the water took; its distance is the straight one, in three
    // dimensions (a lakebed is a point under the surface).
    bool const routed = markers.Course == MarkerCourse::Ground || markers.Course == MarkerCourse::Vertical;
    bool const water = markers.Course == MarkerCourse::Water;
    bool const ground = routed || water;
    if (water)
    {
        // What the water took: drowning is the agent's damage with no attacker behind it (AgentStats::SelfDamage),
        // charged while under the surface.
        uint64 const self = env.StepStats[seatIndex].SelfDamage;
        if (self && bot->IsUnderWater())
        {
            float const share = float(self) / float(std::max<uint32>(1, bot->GetMaxHealth()));
            markers.UnderwaterSelfDamage += share;
            ledger.Add(RewardTerm::Drowning, -_scenario.Tuning().MarkerWater.Drowning * share);
        }
        bool const wet = bot->IsInWater();
        markers.BanksClimbed += markers.WasInWater && !wet ? 1 : 0;
        markers.WasInWater = wet;
    }
    if (markers.Course == MarkerCourse::Vertical)
    {
        float const took = seat.FallDamage - std::min(seat.FallDamage, markers.LastFallDamage);
        markers.LastFallDamage = seat.FallDamage;
        if (took > 0.0f)
            ledger.Add(RewardTerm::FallDamage, -_scenario.Tuning().MarkerVertical.FallDamage * took);
    }
    if (ground)
    {
        CurriculumTuning::MarkerGroundTuning const& costs = _scenario.Tuning().MarkerGround;
        uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, markers.LastStuckMs);
        uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, markers.LastWallMs);
        markers.LastStuckMs = seat.StuckMs;
        markers.LastWallMs = seat.WallMs;
        if (stuckMs)
            ledger.Add(RewardTerm::Stuck, -costs.Stuck * float(stuckMs) / 1000.0f);
        if (wallMs)
        {
            // What the held keys ask over the decision, at the speed they move at: a slide along the wall that keeps
            // most of it is the way round, not pressing into the wall.
            Movement::ControlState const& held = seat.Controls.Held;
            UnitMoveType const kind = held.Walk ? MOVE_WALK
                : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
            float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
            float const charge = WallCharge(float(wallMs) / 1000.0f, moved, asked, costs.Wall, costs.WallSlide);
            if (charge > 0.0f)
                ledger.Add(RewardTerm::Wall, -charge);
        }
    }

    // The potentials, each started over with every new marker (a new potential function: the jump in distance to
    // the next one is not ground given back). On the ground course the distance shaped on is the route's -- a
    // re-plan is a new potential too, and pays nothing -- and arrival is still judged on the straight distance.
    float const distance = bot->GetExactDist2d(&markers.Marker);
    bool const replanned = routed && RefreshWay(markers, bot, env.EpisodeElapsedMs);
    float const shaped = routed ? WayDistance(markers, bot) : water ? bot->GetExactDist(&markers.Marker) : distance;
    float const leg = std::max(routed ? markers.LegWalk : markers.LegStraight, markers.Task.Radius);
    if (markers.LastDistance >= 0.0f && !replanned)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (markers.LastDistance - shaped) / leg);
    markers.LastDistance = shaped;

    if (!ground)
    {
        float const facingCos = std::cos(BearingFrom(bot, seat.Facing, markers.Marker));
        if (markers.LastFacingCos >= -1.0f)
            ledger.Add(RewardTerm::Facing, tuning.Facing * 0.5f * (facingCos - markers.LastFacingCos));
        markers.LastFacingCos = facingCos;
    }

    // Running through: how far past the radius the seat went after first being inside it.
    float const rise = water ? _scenario.Tuning().MarkerWater.ArriveRise : tuning.ArriveRise;
    bool const inside = distance <= markers.Task.Radius
        && std::fabs(bot->GetPositionZ() - markers.Marker.GetPositionZ()) <= rise;
    if (inside)
        markers.Entered = true;
    else if (markers.Entered)
        markers.LegOvershoot = std::max(markers.LegOvershoot, distance - markers.Task.Radius);

    // A stop is the decision the seat came to rest on after moving; one near the marker is measured whether or not it
    // was in. The first look has no last position to measure "still" from: the seat starts at rest, so it is read as
    // stopped there, and only a stop after moving counts.
    // In the water a stop is a swimmer holding no key and still: SWIMMING is where it is, not a key it holds.
    uint32 const flags = bot->GetUnitMovementFlags() & ~(water ? uint32(MOVEMENTFLAG_SWIMMING) : 0u);
    bool const stopped = firstLook || Stopped(flags, moved, tuning.StopMoved);
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
    // The optimum: the walking way (the straight line on open ground) at run speed, and the first turn. Across water,
    // the better of swimming straight and running round; to a lakebed, the swim (FindPlace's run-equivalent).
    float const way = routed || (water && markers.LegUnder) ? markers.LegWalk : markers.LegStraight;
    float travel = std::max(0.0f, way - markers.Task.Radius) / run;
    if (water && !markers.LegUnder)
    {
        float const swim = std::max(1.0f, bot->GetSpeed(MOVE_SWIM));
        travel = std::max(0.0f, markers.LegStraight - markers.Task.Radius) / swim;
        if (markers.LegDry > 0.0f)
            travel = std::min(travel, std::max(0.0f, markers.LegDry - markers.Task.Radius) / run);
    }
    float const optimum = std::max(0.25f, travel + markers.LegBearing / FASTEST_TURN);
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

bool Animus::Curriculum::MarkerEncounter::RefreshWay(EnvMarkers& markers, Player* bot, uint32 nowMs) const
{
    Map* map = bot->GetMap();
    if (!map)
        return false;

    CurriculumTuning::TravelTuning const& route = _scenario.Tuning().Travel;
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    float const sinceSeconds = float(nowMs - std::min(nowMs, markers.WayMs)) / 1000.0f;

    // A way that could not be planned is tried again on the refresh clock, not every decision: a failing search
    // searches everything it can reach first.
    if (!markers.Way.Valid && markers.WayFailed && sinceSeconds < route.RouteRefresh)
        return false;

    if (markers.Way.Valid)
    {
        markers.Way.Advance(x, y, z, route.RouteCorner);
        bool const strayed = markers.Way.Next < markers.Way.Count
            && bot->GetExactDist2d(markers.Way.X[markers.Way.Next], markers.Way.Y[markers.Way.Next]) > route.RouteStray;
        if (!strayed && sinceSeconds < route.RouteRefresh)
            return false;
    }

    Position const from(x, y, z, bot->GetOrientation());
    bool const planned = RoutePlanner::Instance().Plan(map, from, markers.Marker, markers.Way);
    markers.WayMs = nowMs;
    markers.WayFailed = !planned;
    return true;
}

float Animus::Curriculum::MarkerEncounter::WayDistance(EnvMarkers const& markers, Player const* bot)
{
    float const straight = bot->GetExactDist2d(&markers.Marker);
    if (!markers.Way.Valid)
        return straight;
    float const along = markers.Way.RemainingFrom(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    if (along < 0.0f)
        return straight;
    if (!markers.Way.Complete && markers.Way.Count > 0)
    {
        uint32 const last = markers.Way.Count - 1;
        float const dx = markers.Way.X[last] - markers.Marker.GetPositionX();
        float const dy = markers.Way.Y[last] - markers.Marker.GetPositionY();
        return along + std::sqrt(dx * dx + dy * dy);
    }
    return along;
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
