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

#include "SightEncounter.h"
#include "BotFactory.h"
#include "Camera.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapVisionWorld.h"
#include "Standing.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SeekDraw.h"
#include "ObjectPool.h"
#include "SightDraw.h"
#include "StageScenario.h"
#include "StageState.h"
#include "UnitBody.h"
#include "UnitDefines.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// The salts of an evaluation pair's seeded draws (SeekDraw::SeedUniform on the pair's index).
    enum Salt : uint32 { SALT_FACING = 1, SALT_OBJECT_FACING };
    /// A corner's stepping point is on the spawn's own floor: within this many yards of its height.
    constexpr float STOREY_RISE = 4.0f;
}

Animus::Curriculum::SightEncounter::SightEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::SightEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::StepCost, RewardTerm::Death, RewardTerm::Progress, RewardTerm::Facing,
        RewardTerm::Stuck, RewardTerm::Wall };
}

std::vector<std::string> Animus::Curriculum::SightEncounter::ObjectNames(ArenaDefinition const& arena)
{
    std::vector<std::string> names;
    for (SeekObject const& object : arena.Objects)
        names.push_back(object.Kind);
    return names;
}

float Animus::Curriculum::SightEncounter::StopGap(float distance, float bound, float body)
{
    return std::max(0.0f, distance - bound - body);
}

void Animus::Curriculum::SightEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // `arrived` is the stage's measure, M1's as before: stopped beside the object before the clock ran out (0 or 1:
    // one object an episode). The convergence reads it; the shaping fade and the cost ladder read arrived_at_rung,
    // the evaluation's arrival at the rung's own withholding mix (animus.evaluation). `markers` is the count the
    // per-arrival means are weighted by (episode_means.PER_EVENT), as M1's marker columns were.
    table.Add("arrived", [this](Env const& env, uint32) { return _envs[env.Index].Reached ? 1.0f : 0.0f; });
    table.Add("markers", [this](Env const& env, uint32) { return _envs[env.Index].Reached ? 1.0f : 0.0f; });
    // By compass: whether this episode withheld it (and its complement, the count arrived_with_compass is weighted
    // by), the chance the training rung withholds it with -- an evaluation's episodes too, which take the compass from
    // their seed -- and the arrival in each kind of episode, each a per-event column (PER_EVENT) so its mean is the
    // arrival rate over the episodes of its kind. arrived_no_compass near 0 at the 0.6 rung means the camera is not
    // being learned (REDESIGN amendment 5).
    table.Add("compass_withheld", [this](Env const& env, uint32) { return _envs[env.Index].Withheld ? 1.0f : 0.0f; });
    table.Add("compass_present", [this](Env const& env, uint32) { return _envs[env.Index].Withheld ? 0.0f : 1.0f; });
    table.Add("compass_withhold_chance", [this](Env const& env, uint32) { return _envs[env.Index].WithholdChance; });
    table.Add("arrived_no_compass", [this](Env const& env, uint32)
    {
        EnvSight const& sight = _envs[env.Index];
        return sight.Withheld && sight.Reached ? 1.0f : 0.0f;
    });
    table.Add("arrived_with_compass", [this](Env const& env, uint32)
    {
        EnvSight const& sight = _envs[env.Index];
        return !sight.Withheld && sight.Reached ? 1.0f : 0.0f;
    });
    // The withholding ladder's rung (the shaping fade's: 0 at x1, 3 at x0). Not `difficulty` or `at_top_rung`: those
    // are a class's own difficulty ladder (DifficultyLadder), which the learner waits on per class before the fade
    // steps and before a class converges; this ladder is the fade itself, one for every class, whose settling is
    // already a convergence signal.
    table.Add("compass_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    table.Add("marker_radius", [this](Env const& env, uint32) { return _envs[env.Index].Radius; });
    // M1's measures, per arrival (PER_EVENT on markers): seconds from the spawn to the stop, that over the optimum
    // (the straight line at run speed and the first turn at the fastest rate), and how far past the radius the seat
    // ran after first reaching it.
    table.Add("arrive_seconds", [this](Env const& env, uint32) { return _envs[env.Index].ArriveSeconds; });
    table.Add("time_ratio", [this](Env const& env, uint32) { return _envs[env.Index].TimeRatio; });
    // The same, split by where the object stood: in sight of the spawn, or round a corner -- whose optimum is still
    // the straight line through the wall, so its ratio reads high by the corner, not by slowness. Each per arrival of
    // its kind (PER_EVENT on markers_sight and markers_corner).
    auto const kind = [this](bool corner, auto value)
    {
        return [this, corner, value](Env const& env, uint32)
        {
            EnvSight const& sight = _envs[env.Index];
            return sight.Reached && sight.Corner == corner ? value(sight) : 0.0f;
        };
    };
    auto const one = [](EnvSight const&) { return 1.0f; };
    auto const seconds = [](EnvSight const& sight) { return sight.ArriveSeconds; };
    auto const ratio = [](EnvSight const& sight) { return sight.TimeRatio; };
    table.Add("markers_sight", kind(false, one));
    table.Add("markers_corner", kind(true, one));
    table.Add("arrive_seconds_sight", kind(false, seconds));
    table.Add("arrive_seconds_corner", kind(true, seconds));
    table.Add("time_ratio_sight", kind(false, ratio));
    table.Add("time_ratio_corner", kind(true, ratio));
    table.Add("overshoot", [this](Env const& env, uint32)
    {
        EnvSight const& sight = _envs[env.Index];
        return sight.Reached ? sight.Overshoot : 0.0f;
    });
    // The stops near the object (within Markers.StopNear of its centre) and their precision: the air between the body
    // and the object's bounding circle (StopGap), mean, so 0 is touching its widest side whatever the object.
    table.Add("stop_distance", [this](Env const& env, uint32)
    {
        EnvSight const& sight = _envs[env.Index];
        return sight.Stops ? sight.StopDistanceSum / float(sight.Stops) : 0.0f;
    });
    table.Add("stops_near", [this](Env const& env, uint32) { return float(_envs[env.Index].Stops); });
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });
    table.Add("movement_casts", [this](Env const& env, uint32) { return float(_envs[env.Index].MovementCasts); });
    table.Add("speed_casts", [this](Env const& env, uint32) { return float(_envs[env.Index].SpeedCasts); });
    // Where it was: the straight distance from the spawn, round a corner or in sight (and a corner asked for that fell
    // back to a point in sight), the object (an index into stage.json episode_categories) and the evaluation pair.
    table.Add("objective_distance", [this](Env const& env, uint32) { return _envs[env.Index].Straight; });
    table.Add("objective_corner", [this](Env const& env, uint32) { return _envs[env.Index].Corner ? 1.0f : 0.0f; });
    table.Add("corner_fallback", [this](Env const& env, uint32)
    {
        EnvSight const& sight = _envs[env.Index];
        return sight.CornerAsked && !sight.Corner ? 1.0f : 0.0f;
    });
    table.Add("sight_object", [this](Env const& env, uint32) { return float(_envs[env.Index].ObjectIndex); });
    table.Add("sight_pair", [this](Env const& env, uint32) { return float(_envs[env.Index].Pair); });
    // The share of decisions whose frame showed the object's flag: the camera's side of the lesson.
    table.Add("objective_visible", [this](Env const& env, uint32)
    {
        EnvSight const& sight = _envs[env.Index];
        return sight.Decisions ? float(sight.VisibleDecisions) / float(sight.Decisions) : 0.0f;
    });
}

void Animus::Curriculum::SightEncounter::ResetEpisode(Env& env)
{
    // The object stays in the world until the next Build removes it (it has the map); the episode's measures go.
    EnvSight& sight = _envs[env.Index];
    ObjectGuid const object = sight.Object;
    bool const placed = sight.Placed;
    sight = EnvSight();
    sight.Object = object;
    sight.Placed = placed;
}

void Animus::Curriculum::SightEncounter::BeforeLevel(Env& env)
{
    // A seeded episode (an evaluation, or the replay of one) stands at its pair's spawn: the scenario's own draw is
    // replaced before the seat is placed, so SpawnPointFor and the critic's origin are the pair's.
    if (env.EpisodeSeedIndex == NO_EPISODE_SEED)
        return;
    ArenaDefinition const& arena = _scenario.Arena(env);
    if (arena.Against != Opposition::Sight || arena.SightPairs.empty())
        return;
    SightDraw::EvaluationEpisode const pick = SightDraw::EvaluationPick(env.EpisodeSeedIndex,
        _scenario.CastingCount(), uint32(arena.SightPairs.size()));
    uint32 const spawn = arena.SightPairs[pick.Pair].Spawn;
    if (spawn < arena.SpawnPoints.size())
        _scenario.Data(env).Spawn = spawn;
}

bool Animus::Curriculum::SightEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;

    ArenaDefinition const& arena = _scenario.Arena(env);
    std::vector<Position> const& points = arena.SpawnPoints;
    if (points.size() < 2 || arena.Objects.empty())
        return false;

    EnvSight& sight = _envs[env.Index];
    ObjectPool::Remove(map, sight.Object);
    sight = EnvSight();
    ObjectPool::ClearOwn(map);

    CurriculumTuning::ControlsTuning const& tuning = _scenario.Tuning().Controls;
    bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED && !arena.SightPairs.empty();

    // The withholding ladder: the rung and its chance from the shaping fade's scale, every episode (an evaluation
    // reports the training rung's chance, beside the compass its seed gives it).
    float const shaping = _scenario.ShapingScale();
    sight.Rung = SightDraw::Rung(shaping);
    sight.WithholdChance = SightDraw::WithholdChance(shaping, { tuning.Withhold0, tuning.Withhold1, tuning.Withhold2,
        tuning.Withhold3 });
    SightDraw::EvaluationEpisode pick;
    if (seeded)
    {
        pick = SightDraw::EvaluationPick(env.EpisodeSeedIndex, _scenario.CastingCount(),
            uint32(arena.SightPairs.size()));
        sight.Pair = int32(pick.Pair);
    }
    sight.Withheld = seeded && env.Evaluating ? pick.Withheld : frand(0.0f, 1.0f) < sight.WithholdChance;

    // Facing a random way, where the scenario put the seat (a pair's spawn: BeforeLevel). The scenario reads the
    // seat's facing from the bot after the encounters are built.
    Position const& spawn = _scenario.SpawnPointFor(env);
    float const facing = seeded ? SeekDraw::SeedUniform(uint32(sight.Pair), SALT_FACING) * TWO_PI
        : frand(0.0f, TWO_PI);
    Position const start(spawn.GetPositionX(), spawn.GetPositionY(), spawn.GetPositionZ(), facing);
    BotFactory::TeleportWithinMap(bot, start);

    // The object and where it stands: a pair's, or drawn in sight of the seat's eye (round a corner from the
    // ladder's CornerFrom on, CornerShare of the time).
    SightDraw::Placement placement;
    if (seeded)
    {
        SightPair const& pair = arena.SightPairs[pick.Pair];
        sight.ObjectIndex = pick.Pair % uint32(arena.Objects.size());
        placement.Point = pair.Object < points.size() ? int32(pair.Object) : -1;
        placement.Corner = pair.Corner;
        sight.CornerAsked = pair.Corner;
    }
    else
    {
        sight.ObjectIndex = urand(0, uint32(arena.Objects.size()) - 1);
        SeekObject const& kind = arena.Objects[sight.ObjectIndex];
        SightDraw::Viewing viewing;
        viewing.EyeRise = Vision::PIVOT_SHARE * Movement::ShapeOf(bot).Height;
        viewing.CentreRise = kind.Height * 0.5f;
        viewing.Radius = kind.Radius;
        viewing.Nearest = tuning.Nearest;
        viewing.Furthest = tuning.Furthest;
        viewing.StoreyRise = STOREY_RISE;
        viewing.CornerStep = tuning.CornerStep;
        viewing.Attempts = std::max<uint32>(1, tuning.Attempts);
        sight.CornerAsked = 1.0f - shaping >= tuning.CornerFrom - 1e-4f && frand(0.0f, 1.0f) < tuning.CornerShare;
        Vision::MapVisionWorld const world(map, bot->GetPhaseMask());
        placement = SightDraw::Place(points, start, sight.CornerAsked, viewing, world,
            []() { return frand(0.0f, 1.0f); });
    }
    if (placement.Point < 0)
        return false;
    sight.Corner = placement.Corner;

    SeekObject const& kind = arena.Objects[sight.ObjectIndex];
    Position const& at = points[uint32(placement.Point)];
    float const turned = seeded ? SeekDraw::SeedUniform(uint32(sight.Pair), SALT_OBJECT_FACING) * TWO_PI
        : frand(0.0f, TWO_PI);
    Position const spot(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), turned);
    GameObject* object = ObjectPool::Summon(map, kind, spot, bot->GetPhaseMask());
    if (!object)
    {
        LOG_ERROR("module.animus", "{}: the sight object {} ({}) could not be spawned at ({:.1f} {:.1f} {:.1f})",
            _scenario.Name(), kind.Entry, kind.Kind, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
        return false;
    }

    sight.Object = object->GetGUID();
    sight.Placed = true;
    sight.Spot = spot;
    sight.Centre = Position(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + kind.Height * 0.5f,
        turned);
    sight.Bound = kind.Radius;
    sight.Radius = kind.Radius + std::max(0.1f, tuning.ArriveTolerance);
    sight.LegStartMs = env.EpisodeElapsedMs;
    sight.Straight = start.GetExactDist2d(&spot);
    float const bearing = start.GetAngle(spot.GetPositionX(), spot.GetPositionY()) - facing;
    sight.Bearing = std::fabs(std::atan2(std::sin(bearing), std::cos(bearing)));
    return true;
}

bool Animus::Curriculum::SightEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seat acts without a target (SeatEncoder::ActsWithoutTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::SightEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    EnvSight const& sight = _envs[env.Index];
    view.HasObjective = sight.Placed && !sight.Reached;
    view.Objective = sight.Centre;
    // The flag on the object itself, as M2 shows it: its own radius and a quarter yard.
    view.ObjectiveRadius = Vision::ObjectiveRadiusFor(sight.Bound);
    view.CompassWithheld = sight.Withheld;
    view.MountsAllowed = false;
    view.GroundMountAllowed = false;
    view.ArriveWithin = sight.Radius;
}

void Animus::Curriculum::SightEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::MarkerTuning const& tuning = _scenario.Tuning().Markers;
    CurriculumTuning::ControlsTuning const& controls = _scenario.Tuning().Controls;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvSight& sight = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (!bot)
        return;

    sight.MovementCasts += env.StepStats[seatIndex].MovementCasts;
    sight.SpeedCasts += env.StepStats[seatIndex].SpeedCasts;

    // How far the unit went since the last decision, on the server's applied position (player-controller §5A.1).
    float moved = 0.0f;
    if (sight.HasLastPos)
    {
        float const dx = bot->GetPositionX() - sight.LastX;
        float const dy = bot->GetPositionY() - sight.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
        sight.Travelled += moved;
    }
    sight.LastX = bot->GetPositionX();
    sight.LastY = bot->GetPositionY();
    bool const firstLook = !sight.HasLastPos;
    sight.HasLastPos = true;

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
    if (sight.Placed && !sight.Reached && timeIsUp)
        tally.TimedOut = true;
    if (!sight.Placed || sight.Reached || !bot->IsAlive())
        return;

    // Wall and Stuck, read off the controller's counts as the ground course reads them, at their fixed price from
    // the first step: off the cost ladder, so pressing into a wall is never free.
    uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, sight.LastStuckMs);
    uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, sight.LastWallMs);
    sight.LastStuckMs = seat.StuckMs;
    sight.LastWallMs = seat.WallMs;
    if (stuckMs)
        ledger.AddFixed(RewardTerm::Stuck, -controls.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
        float const charge = Standing::WallCharge(float(wallMs) / 1000.0f, moved, asked, controls.Wall,
            controls.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
    }

    // What the camera showed, the frame the seat decided on (StageScenario::ObserveSeat counts the flag's pixels).
    ++sight.Decisions;
    sight.VisibleDecisions += seat.ObjectivePixels ? 1 : 0;

    // The potentials (Shaping, faded): the straight distance, spread over the leg so closing it pays Progress once,
    // and the cosine of the object's bearing off the seat's facing. Arrival is judged on the server's position.
    float const distance = bot->GetExactDist2d(&sight.Spot);
    float const leg = std::max(sight.Straight, sight.Radius);
    if (sight.LastDistance >= 0.0f)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (sight.LastDistance - distance) / leg);
    sight.LastDistance = distance;
    float const angle = bot->GetAngle(sight.Spot.GetPositionX(), sight.Spot.GetPositionY()) - seat.Facing;
    float const facingCos = std::cos(std::atan2(std::sin(angle), std::cos(angle)));
    if (sight.LastFacingCos >= -1.0f)
        ledger.Add(RewardTerm::Facing, tuning.Facing * 0.5f * (facingCos - sight.LastFacingCos));
    sight.LastFacingCos = facingCos;

    // Running through: how far past the radius the seat went after first being inside it.
    bool const inside = distance <= sight.Radius
        && std::fabs(bot->GetPositionZ() - sight.Spot.GetPositionZ()) <= tuning.ArriveRise;
    if (inside)
        sight.Entered = true;
    else if (sight.Entered)
        sight.Overshoot = std::max(sight.Overshoot, distance - sight.Radius);

    // A stop is the decision the seat came to rest on after moving; one near the object is measured whether or not
    // it was in. The first look is at rest and is no stop.
    bool const stopped = firstLook || Standing::Stopped(bot->GetUnitMovementFlags(), moved, tuning.StopMoved);
    if (!firstLook && stopped && !sight.WasStopped && distance <= tuning.StopNear)
    {
        sight.StopDistanceSum += StopGap(distance, sight.Bound, Movement::ShapeOf(bot).Radius);
        ++sight.Stops;
    }
    sight.WasStopped = stopped;
    if (firstLook || !(stopped && inside))
        return;

    // Stopped beside it: paid and measured, and the episode is done.
    sight.Reached = true;
    ledger.Add(RewardTerm::Arrive, tuning.Arrive);
    float const seconds = float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, sight.LegStartMs)) / 1000.0f;
    sight.ArriveSeconds = seconds;
    // The optimum: the straight line at run speed (round a corner the walk is longer, so its ratio reads high) and
    // the first turn at the fastest rate the move block offers (360 degrees a second).
    float const run = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
    float const optimum = std::max(0.25f, std::max(0.0f, sight.Straight - sight.Radius) / run + sight.Bearing / TWO_PI);
    sight.TimeRatio = seconds / optimum;
}

void Animus::Curriculum::SightEncounter::WriteState(Env const& env, float* state) const
{
    // The critic sees the objective whether or not the actor's compass does (REDESIGN amendment 1): where the object
    // is, in the first enemy slot (the stage has no enemies), relative to the spawn as every position in the state
    // is; and whether this episode withheld the compass, in the tier column.
    EnvSight const& sight = _envs[env.Index];
    if (!sight.Placed)
        return;
    Position const& origin = _scenario.SpawnPointFor(env);
    float* slot = state + StageScenario::STATE_GLOBAL_COUNT + MAX_SEATS * StageScenario::STATE_SEAT_FEATURES;
    slot[StageScenario::STATE_ENEMY_PRESENT] = 1.0f;
    slot[StageScenario::STATE_ENEMY_ALIVE] = sight.Reached ? 0.0f : 1.0f;
    slot[StageScenario::STATE_ENEMY_X] = Encoding::RelativePosition(sight.Spot.GetPositionX(), origin.GetPositionX());
    slot[StageScenario::STATE_ENEMY_Y] = Encoding::RelativePosition(sight.Spot.GetPositionY(), origin.GetPositionY());
    state[StageScenario::STATE_TIER] = sight.Withheld ? 1.0f : 0.0f;
}

bool Animus::Curriculum::SightEncounter::IsTerminal(Env const& env) const
{
    return _envs[env.Index].Reached || _scenario.DeadForGood(env, 0);
}

void Animus::Curriculum::SightEncounter::Teardown(Env& env)
{
    EnvSight& sight = _envs[env.Index];
    ObjectPool::Remove(env.FindMap(), sight.Object);
    sight.Placed = false;
}
