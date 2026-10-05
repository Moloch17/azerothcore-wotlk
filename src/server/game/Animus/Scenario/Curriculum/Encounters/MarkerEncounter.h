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
        };

        /// Place the next marker from where the seat stands, on the episode's rung; false when none could be found.
        bool PlaceMarker(Env const& env, EnvMarkers& markers, Player* bot, float facing) const;
        /// Tell the ladder how a counting episode went, once: won when every marker was stopped on.
        void RecordRung(Env const& env, EnvMarkers& markers);
        /// The ladder's top rung index: the arena's pinned rung when it has one, else Markers.Rungs - 1.
        [[nodiscard]] uint32 TopRung(Env const& env) const;

        std::vector<EnvMarkers> _envs;
        DifficultyLadder _ladder;
    };
}

#endif
