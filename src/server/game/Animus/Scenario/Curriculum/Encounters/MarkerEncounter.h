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

#ifndef ANIMUS_LIB_CURRICULUM_MARKER_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_MARKER_ENCOUNTER_H

#include "DifficultyLadder.h"
#include "Encounter.h"
#include "Position.h"
#include "RoutePlanner.h"
#include "StageDefinition.h"
#include <unordered_map>
#include <vector>

namespace Animus::Curriculum
{
    /// The rung's task (MarkerEncounter::RungTask): how far, how far round and how tight.
    struct MarkerRung
    {
        float Nearest = 5.0f;       // yards
        float Furthest = 10.0f;
        float BearingHalf = 0.0f;   // radians either side of the seat's facing
        float Radius = 4.0f;        // yards to stop within
        float DetourMin = 0.0f;     // the walking way over the straight line, at least (0: none asked) ...
        float DetourMax = 0.0f;     // ... and at most (0: the course's own ceiling)
        float HeightMin = 0.0f;     // the vertical course: the height window, yards (up for a climb, the drop for a
        float HeightMax = 0.0f;     // ledge, either way indoors); both 0 for no window
    };

    /// **The movement stages' markers** (Opposition::Markers; movement-curriculum plan §2, M1): a place on the ground
    /// to stop on, then the next from where the seat stands, MarkersMin to MarkersMax an episode. Arriving is
    /// stopping inside the rung's radius (MarkerEncounter::Stopped: no movement key held, no jump, on the ground, the
    /// feet still) -- running through a marker reaches nothing, which is the lesson. The episode ends when the last
    /// marker is stopped on, or on the seat's death; the clock otherwise.
    ///
    /// The ladder (per class and build) widens the distance, swings the marker from ahead to behind and tightens the
    /// radius (Markers.*): its last rung is the stage's real task, a marker anywhere around up to sixty yards off and
    /// a half-yard circle to stop in.
    ///
    /// Paid: Arrive per marker (Outcome), StepCost and Death (Cost), and two potentials that fade with the stage's
    /// shaping -- Progress on the straight-line distance, and Facing on the cosine of the marker's bearing.
    ///
    /// **Courses** (ArenaDefinition::Course): each movement stage's ground. Open (M1) is the above. Ground (M2) is
    /// broken ground with something in the way: its ladder (MarkerGround.*) widens the distance and the detour, the
    /// radius is fixed, Progress is shaped on the route planner's distance (re-planned when the seat strays, a
    /// re-plan paying nothing), there is no Facing term, and the seat pays Stuck and Wall -- noise prices on the cost
    /// ladder -- per second the controller counts it stuck or pressing into a wall. Vertical (M3) is up and down: by
    /// the arena's ground a marker above (a climb), below a ledge (ArenaDefinition::Ledges) or on another floor
    /// (Indoors), within the rung's height window (MarkerVertical.*); the costs are the ground course's and
    /// FallDamage. Every course's markers are placed only where the player controller can walk to them
    /// (TravelPlaceRules::ControllerReach, MarkerReach). Water (M4): across water whose dry way round is the longer one
    /// (both walkable or swimmable), on a lakebed (Underwater), or a chain of lakebeds (Checkpoints) within the rung's
    /// depth window (MarkerWater.*); shaped on the straight distance in three dimensions, stopped as a swimmer, and
    /// Drowning paid besides Stuck and Wall. Routes (M5): one long trip, planned whole by the RoutePlanner, in the
    /// rung's distance band and detour window (MarkerRoutes.*); shaped on the route, priced as the ground and
    /// vertical courses are.
    class MarkerEncounter final : public Encounter
    {
    public:
        MarkerEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

        /// Rung `rung` of a ground course's ladder of `rungs`: distance and detour window widen, the radius is fixed.
        [[nodiscard]] static MarkerRung GroundRungTask(uint32 rung, uint32 rungs, float distanceMin,
            float distanceFirst, float distanceLast, float detourFirst, float detourLast, float detourSpan,
            float radius);

        /// What pressing into a wall costs over a decision: `wallSeconds` of it (the controller's count, slides
        /// included), the unit having moved `moved` yards against the `asked` its held keys ask over the decision.
        /// Nothing while moved / asked is at least `slideShare` (a slide that makes good progress is the right way
        /// round a corner); in proportion to the shortfall below it, the full `price` a second at no movement.
        [[nodiscard]] static float WallCharge(float wallSeconds, float moved, float asked, float price,
            float slideShare);

        /// Rung `rung` of the vertical course's ladder: the height window and the distance widen, the radius is fixed.
        [[nodiscard]] static MarkerRung VerticalRungTask(uint32 rung, uint32 rungs, float distanceMin,
            float distanceFirst, float distanceLast, float heightMinFirst, float heightMaxFirst, float heightMinLast,
            float heightMaxLast, float radius);

        /// Rung `rung` of the routes course's ladder: the straight distance band and the detour window move up; the
        /// detour's ceiling is `detourCap`.
        [[nodiscard]] static MarkerRung RoutesRungTask(uint32 rung, uint32 rungs, float nearestFirst,
            float furthestFirst, float nearestLast, float furthestLast, float detourFirst, float detourLast,
            float detourSpan, float detourCap, float radius);

        /// Rung `rung` of a ladder of `rungs` (0 the first), from the tuning's First and Last values.
        [[nodiscard]] static MarkerRung RungTask(uint32 rung, uint32 rungs, float distanceMin, float distanceFirst,
            float distanceLast, float bearingFirstDeg, float bearingLastDeg, float radiusFirst, float radiusLast);

        /// Whether a seat is stopped, as arriving means it, read from the server's applied state (player-controller
        /// §5A.1: what is judged is what the server holds; the client sends MOVE_STOP at once, so at a stop the two
        /// agree): `movementFlags` is the unit's MovementInfo flags -- no forward, back, strafe, pitch, ascend or
        /// descend, not falling (a jump is), not swimming or flying -- and the unit's position moved under
        /// `stopMoved` yards since the last decision. A turn (MOVEMENTFLAG_LEFT/RIGHT, or the mouse's facing alone)
        /// does not count against it: turning on the spot is standing still (ratified 2026-10-05).
        [[nodiscard]] static bool Stopped(uint32 movementFlags, float movedYards, float stopMoved);

    private:
        struct EnvMarkers
        {
            bool HasMarker = false;
            Position Marker;
            uint32 Rung = 0;
            bool Counts = false;            // a training episode at its class and build's own rung: it moves the rung
            bool Recorded = false;
            uint16 Layout = 0;
            uint8 Spec = 0;
            MarkerRung Task;
            uint32 Wanted = 0;              // markers this episode
            uint32 Placed = 0;              // markers set so far (the current one included)
            uint32 Reached = 0;
            bool Broken = false;            // a next marker was wanted and none could be placed

            // The current leg.
            uint32 LegStartMs = 0;
            float LegStraight = 0.0f;       // yards from where the leg began
            float LegBearing = 0.0f;        // radians off the facing it began at, absolute
            float LastDistance = -1.0f;     // shaping: at the last reward; < 0 none yet
            float LastFacingCos = -2.0f;    // shaping: likewise; < -1 none yet
            bool Entered = false;           // inside the radius at some point this leg
            float LegOvershoot = 0.0f;      // the furthest past the radius after first entering it

            // Where the feet were at the last reward, for "still".
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            bool WasStopped = true;

            // The episode's measures.
            float TimeRatioSum = 0.0f;      // reached legs: time taken over the straight-line optimum
            float OvershootSum = 0.0f;      // legs that entered the radius
            uint32 OvershootLegs = 0;
            float StopDistanceSum = 0.0f;   // stops within StopNear of the marker
            uint32 Stops = 0;
            float Travelled = 0.0f;
            uint32 MovementCasts = 0;       // the seat's blinks, leaps, charges and jumps (AgentStats::MovementCasts)
            uint32 SpeedCasts = 0;          // ... and run-speed buffs (Sprint, Dash, Aspect of the Cheetah, ...)

            MarkerCourse Course = MarkerCourse::Open;
            float LegWalk = 0.0f;           // yards of the walking way when the leg began (the leg's optimum)
            float DetourSum = 0.0f;         // legs placed: walking way over straight line
            uint32 Legs = 0;
            // The ground course's route, shaped on (never seen by the actor), and the costs' last readings.
            Route Way;
            uint32 WayMs = 0;
            bool WayFailed = false;
            uint32 LastWallMs = 0;
            uint32 LastStuckMs = 0;
            // The vertical course: the falls' cost read off the controller's count, and what the markers asked.
            float LastFallDamage = 0.0f;
            float RiseSum = 0.0f;           // legs placed: |the marker's height over the seat's| at the leg's start
            uint32 StoreyLegs = 0;          // ... of which a storey or more (3 yd) up or down
            uint32 StoreyUpLegs = 0;        // ... up
            uint32 StoreyDownLegs = 0;      // ... down
            // The water course.
            float LegDry = 0.0f;            // the dry way round of a crossing (0: none or not a crossing)
            bool LegUnder = false;          // the leg's marker is on a lakebed
            uint32 Crossings = 0;
            uint32 BanksClimbed = 0;
            bool WasInWater = false;
            float UnderwaterSelfDamage = 0.0f;  // share of maximum health, what Drowning is paid on
            // The routes course: cells of the ground visited (cell -> when last there), and returns to one after a
            // while away.
            std::unordered_map<uint64, uint32> Cells;
            uint64 LastCell = ~uint64(0);
            uint32 Revisits = 0;
        };

        /// Place the next marker from where the seat stands, on the episode's rung; false when none could be found.
        bool PlaceMarker(Env const& env, EnvMarkers& markers, Player* bot, float facing) const;
        /// The ground course's route to the marker, re-planned when the seat has strayed or the refresh is due; true
        /// when it was re-planned (a new potential: nothing is paid for the change).
        bool RefreshWay(EnvMarkers& markers, Player* bot, uint32 nowMs) const;
        /// Yards left to the marker: along the route on a ground course (with the gap at a partial route's end), the
        /// straight line otherwise or without a route.
        [[nodiscard]] static float WayDistance(EnvMarkers const& markers, Player const* bot);
        /// Tell the ladder how a counting episode went, once: won when every marker was stopped on.
        void RecordRung(Env const& env, EnvMarkers& markers);
        /// The ladder's top rung index: the arena's pinned rung when it has one, else Markers.Rungs - 1.
        [[nodiscard]] uint32 TopRung(Env const& env) const;

        std::vector<EnvMarkers> _envs;
        DifficultyLadder _ladder;
    };
}

#endif
