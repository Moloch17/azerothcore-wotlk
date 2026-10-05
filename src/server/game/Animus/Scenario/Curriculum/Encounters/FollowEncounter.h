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

#ifndef ANIMUS_LIB_CURRICULUM_FOLLOW_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_FOLLOW_ENCOUNTER_H

#include "DifficultyLadder.h"
#include "Encounter.h"
#include "Env.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "RoutePlanner.h"
#include <unordered_set>
#include <vector>

namespace Animus::Curriculum
{
    /// **The follow stage** (Opposition::Follow; movement-curriculum plan §2, M7): moving with someone. A leader walks
    /// trips across the ground and the seat keeps within [Follow.BandMin, Follow.BandMax] yards of it: falling
    /// behind, catching up, not crowding it, going where it went. No combat.
    ///
    /// **The leader** is an agent in the owner's slot (StageScenario::OwnerAgent, built by BuildOwnerSeat), moved by
    /// the player controller and reported to the server as a client, every world tick -- never a spline. On a
    /// scripted episode its keys are the seek helper's (Movement::Seek) toward the route planner's next corner of its
    /// trip, at a walk on the first rungs; from Follow.CastFromRung a share of training episodes hand it to a frozen
    /// checkpoint (stage.json cast "leader": an M6 policy that rides, swims and jumps as it likes), whose objective is
    /// the trip's end. Evaluations always keep the script. The scripted leader's trips are dry and need no jump
    /// (TravelPlaceRules::NoJump): its keys never jump. A cast leader's may swim and jump (M5/M6's rules).
    ///
    /// The follower sees the leader as its objective (SeatView's objective bearing and distance). It is paid
    /// FollowKept per second in the band (Outcome), Lost per second past Follow.LostYards and Aggro per hostile
    /// creature newly on it (Costs), Progress on closing to the band (Shaping), and the ground courses' Stuck, Wall
    /// and FallDamage.
    ///
    /// **Aggro is dormant as built:** each env's phase hides the world's creatures, so nothing attacks the follower
    /// and aggro_pulled reads 0. Whether hostile camps are spawned into the env's phase on M7's roads, or the term
    /// waits for a later stage, is the user's to decide (2026-10-05); the term stays wired and audited until then.
    class FollowEncounter final : public Encounter
    {
    public:
        FollowEncounter(StageScenario& scenario, uint32 envs);

        /// Whether the episode's leader is a frozen checkpoint's (its row is played); else the script's keys.
        [[nodiscard]] bool IsCast(Env const& env) const { return _envs[env.Index].Cast; }
        /// Whether the episode has a leader in the owner's slot for the controller to move.
        [[nodiscard]] bool HasLeader(Env const& env) const { return _envs[env.Index].Built; }

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Deactivate(Env& env) override;
        void Teardown(Env& env) override;

        /// The leader's trip length on rung `rung` of `rungs`: the furthest moves from `first` to `last`.
        [[nodiscard]] static float TripFurthest(uint32 rung, uint32 rungs, float first, float last);
        /// Where a distance to the leader falls: 0 too close, 1 in the band, 2 behind, 3 lost.
        [[nodiscard]] static uint32 Band(float distance, float bandMin, float bandMax, float lostYards);

    private:
        struct EnvFollow
        {
            bool Built = false;
            bool Cast = false;
            uint32 Rung = 0;
            bool Counts = false;
            bool Recorded = false;
            uint16 Layout = 0;
            uint8 Spec = 0;

            // The leader's trip and its way.
            bool HasTrip = false;
            Position Trip;
            Route Way;
            uint32 TripStartMs = 0;
            uint32 TripStuckMs = 0;         // the leader's controller-stuck time when the trip began
            uint32 Trips = 0;
            uint32 LeaderSwims = 0;         // the leader's entries into the water
            bool LeaderWasWet = false;

            // The follower.
            uint32 InBandMs = 0;
            uint32 LostMs = 0;
            uint32 OutSinceMs = 0;
            bool InBand = false;
            uint32 CatchUps = 0;
            float LastExcess = -1.0f;       // shaping: yards past the band at the last reward; < 0 none yet
            double DistanceSum = 0.0;
            double DistanceSquares = 0.0;
            uint32 Samples = 0;
            uint32 LastRewardMs = 0;
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
            float LastFallDamage = 0.0f;
            std::unordered_set<ObjectGuid> Attackers;
            uint32 AggroPulled = 0;
        };

        /// A new trip for the leader from where it stands; false when none could be placed.
        bool NextTrip(Env const& env, EnvFollow& follow, Player* leader) const;
        /// Tell the ladder how a counting episode went, once.
        void RecordRung(Env const& env, EnvFollow& follow);

        std::vector<EnvFollow> _envs;
        DifficultyLadder _ladder;
    };
}

#endif
