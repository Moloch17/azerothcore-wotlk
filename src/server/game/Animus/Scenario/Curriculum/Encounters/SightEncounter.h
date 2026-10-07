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

#ifndef ANIMUS_LIB_CURRICULUM_SIGHT_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_SIGHT_ENCOUNTER_H

#include "Encounter.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    struct ArenaDefinition;

    /// **M1 controls, redesigned** (Opposition::Sight; perception-goals REDESIGN §1): the controls -- straight lines,
    /// stopping on a point -- taught on what the camera sees, so M2 starts with a camera that works.
    ///
    /// Each episode the seat stands at a random point of the dungeon's hallways (the arena's SpawnPoints: the
    /// Stockades' hallway table, the entrance among them), facing a random way, and one real object of M2's pool
    /// (ArenaDefinition::Objects: the same class and flag M2 shows) stands at another hallway point Controls.Nearest
    /// to Controls.Furthest yards off, in sight of the seat's eye (SightDraw::Place: one ray, cast as the camera casts
    /// its pixels, from the eye to the object's centre). From Controls.CornerFrom on the fade's ladder a share of them
    /// stand just round a corner instead: out of sight of the spawn, in sight a few yards' walk away.
    ///
    /// **The compass fades out**: each episode it is withheld with the chance its rung asks (Controls.Withhold0..3 at
    /// the shaping fade's scales x1, x0.5, x0.25, x0): the CompassBlock reads as absent (SeatView::CompassWithheld:
    /// presence and values 0), as a player with no quest arrow. Only that input goes -- no action is masked, the
    /// camera still flags the object, and the critic's state still has it (WriteState). By the top rung the seat
    /// goes to what it sees.
    ///
    /// Arriving is M1's: stopped (MarkerEncounter::Stopped) with the feet within the object's bounding radius plus
    /// Controls.ArriveTolerance of its centre, on its floor. Paid: Arrive once (Outcome); StepCost and Death (Cost);
    /// Wall and Stuck at their own fixed price from the first step, off the cost ladder (RewardLedger::AddFixed);
    /// Progress on the straight distance and Facing on the object's bearing (Shaping, faded). Measured as M1 was:
    /// the time, the time ratio, the overshoot and the stops' precision; and by compass: arrived_no_compass and
    /// arrived_with_compass.
    ///
    /// An evaluation plays the arena's fixed SightPairs, each with and without the compass (SightDraw::
    /// EvaluationPick); a replayed evaluation episode keeps its pair, and draws its compass from the rung.
    class SightEncounter final : public Encounter
    {
    public:
        SightEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

        /// The object pool's names, in the order the episode info column sight_object indexes them (stage.json
        /// episode_categories).
        [[nodiscard]] static std::vector<std::string> ObjectNames(ArenaDefinition const& arena);

        /// The air between a body of radius `body` and an object of bounding radius `bound`, its feet `distance`
        /// yards from the object's centre: what stop_distance measures (0 touching its widest side).
        [[nodiscard]] static float StopGap(float distance, float bound, float body);

    private:
        struct EnvSight
        {
            bool Placed = false;
            ObjectGuid Object;
            Position Spot;                  // the object's base
            Position Centre;                // the objective point: its centre, which the flag and the compass mark
            uint32 ObjectIndex = 0;
            float Bound = 0.5f;             // its bounding radius
            float Radius = 1.5f;            // arriving: Bound + Controls.ArriveTolerance
            int32 Pair = -1;                // the evaluation pair, -1 none
            uint32 Rung = 0;                // the withholding ladder's rung (SightDraw::Rung)
            float WithholdChance = 0.0f;
            bool Withheld = false;
            bool CornerAsked = false;
            bool Corner = false;            // placed round a corner (not in sight of the spawn)
            bool Reached = false;

            uint32 LegStartMs = 0;
            float Straight = 0.0f;          // yards from the spawn to the object, straight
            float Bearing = 0.0f;           // radians off the spawn's facing, absolute
            float LastDistance = -1.0f;     // shaping: at the last reward; < 0 none yet
            float LastFacingCos = -2.0f;
            bool Entered = false;
            float Overshoot = 0.0f;

            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            bool WasStopped = true;

            float ArriveSeconds = 0.0f;
            float TimeRatio = 0.0f;
            float StopDistanceSum = 0.0f;
            uint32 Stops = 0;
            float Travelled = 0.0f;
            uint32 MovementCasts = 0;
            uint32 SpeedCasts = 0;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
            uint32 Decisions = 0;
            uint32 VisibleDecisions = 0;
        };

        std::vector<EnvSight> _envs;
    };
}

#endif
