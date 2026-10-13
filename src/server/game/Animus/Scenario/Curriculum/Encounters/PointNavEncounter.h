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

#ifndef ANIMUS_LIB_CURRICULUM_POINT_NAV_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_POINT_NAV_ENCOUNTER_H

#include "Block.h"
#include "Encounter.h"
#include "EntranceRespawn.h"
#include "Position.h"
#include "TravBake.h"
#include <array>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    class PartyFollowEncounter;

    /// **PointNav** (Opposition::PointNav; vision movement curriculum, decision 0036, CONTRACT.md secs 1-3): a point
    /// goal over a cleared dungeon, shown to the seat as the compass (the minimap marker: bearing and distance, exact,
    /// never withheld), the route unknown. The arena's traversability bake (Vision::TravRegistry, Runtime/Vision/
    /// TravBake) is baked on the arena's first episode and gives every episode its pair: the goal first, one Dijkstra
    /// from it, then a start whose route length and detour (route over the straight line) fall in bins the rung opens
    /// (PointNav.DetourBins, LengthBins, RungBins0..3; the rung is the shaping fade's, SightDraw::Rung). ReplayShare
    /// percent of training episodes start where a seat of this arena failed -- pinned PinSeconds, fell for damage,
    /// drowned or timed out -- with that goal (a ring of FailurePool); an evaluation's pairs come from its seed. A
    /// SightOnly arena (nav1) keeps only starts whose eye sees the goal; a goal on a water cell is at the surface, at
    /// the bed BedShare percent of the time.
    ///
    /// Paid (PointNav.*): Arrive once (Outcome) on stopping (Standing::Stopped) within ArriveYards of the goal and
    /// ArriveRise of its height; StepCost per decision, Death, FallHurt per share of maximum health a fall took,
    /// Drowned on a death under water, Lost per second while the route distance to the goal exceeds the start's by
    /// LostYards, Revisit per second on bake cells stood on before (arenas with Revisit) -- Costs; Stuck and Wall at
    /// their fixed price (Controls.*, as M1). No shaping from the sim: the learner pays its potential on the route
    /// distance the STEP's labels carry. The episode runs its clock: a death rises at the start point after
    /// Respawn.DelayMs (EntranceRespawn) and arriving ends nothing.
    ///
    /// **Labels** (sec 3.1; WriteLabels): per seat, the bake's classes and the route cost-to-go in the fine and
    /// coarse crop frames of the seat's last observation (SeatState::Crop), then C(body), C(start) and the goal in the
    /// body frame. **LabelsOnly** (nav6, beside PartyFollowEncounter): nothing placed or paid; the goal is the
    /// leader's position as the server last had it, and the cost map is recomputed when its cell changes.
    class PointNavEncounter final : public Encounter
    {
    public:
        PointNavEncounter(StageScenario& scenario, uint32 envs);

        /// Bytes per agent of the STEP's labels (ScenarioSpec::LabelBytes).
        static constexpr uint32 LABEL_BYTES = Vision::TRAV_LABEL_BYTES;
        /// The failure pool's and the draw's bins are the tuning's lists; the ladder is the fade's four rungs.
        static constexpr uint32 RUNGS = 4;

        /// The leader whose reported position is nav6's goal (set by the scenario after construction).
        void FollowLeader(PartyFollowEncounter const* follow) { _follow = follow; }

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        /// The episode runs its clock: arriving ends nothing, and a death rises at the start (Update).
        [[nodiscard]] bool IsTerminal(Env const& /*env*/) const override { return false; }

        /// Seat `seat`'s LABEL_BYTES of labels for the observation just taken (its SeatState::Crop pose), into `out`.
        /// Zeros, with every cost unreachable, for a seat with no bake or no body.
        void WriteLabels(Env const& env, uint32 seat, uint8* out) const;

        /// A comma list of numbers ("1.0,1.2,1.6") parsed; empty on garbage.
        [[nodiscard]] static std::vector<float> ParseList(std::string const& list);

    private:
        struct SeatNav
        {
            RespawnClock Clock;
            bool Reached = false;
            uint32 ArriveMs = 0;
            float StopError = 0.0f;
            float Travelled = 0.0f;
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            uint32 Decisions = 0;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
            uint32 StuckMs = 0;
            uint32 WallMs = 0;
            uint32 LostMs = 0;
            uint32 RevisitMs = 0;
            float LastFallDamage = 0.0f;    // SeatState::FallDamage at the last reward
            uint32 LastFalls = 0;
            float FallShare = 0.0f;         // health lost to falls this episode, in health bars
            bool DrownedPaid = false;
            bool PinNoted = false;          // the current pin is in the failure pool
            float CBody = Vision::TRAV_UNREACHABLE;
            float RegretSum = 0.0f;         // waypoint regret over the cell goals chosen
            uint32 Waypoints = 0;
            uint32 LastTicket = 0;
            std::vector<uint8> Stood;       // bake cells stood on (Revisit arenas), per cell
            uint32 LastCell = Vision::TRAV_NO_CELL;   // the bake cell the body was in at the last reward
        };

        struct EnvNav
        {
            bool Placed = false;            // a pair is drawn and the seat stands at its start
            bool LabelsOnly = false;
            uint32 MapId = 0;
            Vision::TravGrid const* Grid = nullptr;
            std::vector<float> Cost;        // the route cost-to-go to GoalCell (Vision::CostToGo)
            uint32 GoalCell = Vision::TRAV_NO_CELL;
            uint32 StartCell = Vision::TRAV_NO_CELL;
            Position Start;
            Position Goal;
            bool GoalAtBed = false;
            float CStart = Vision::TRAV_UNREACHABLE;    // the start's route distance: the optimum
            float Straight = 0.0f;
            uint32 Rung = 0;
            uint32 DetourBin = 0;
            uint32 LengthBin = 0;
            bool Replayed = false;
            bool TimedOutNoted = false;
            std::array<SeatNav, GROUP_SEATS> Seats{};
        };

        /// Where a seat of an arena failed, for the replay draw: its position and the goal it had.
        struct Failure
        {
            Position Where;
            uint32 GoalCell = Vision::TRAV_NO_CELL;
            bool GoalAtBed = false;
        };
        struct FailurePool
        {
            std::vector<Failure> Ring;
            uint32 Next = 0;
        };

        /// The bins a rung opens: how many of the detour bins and of the length bins (PointNav.RungBins<rung>).
        struct OpenBins
        {
            uint32 Detour = 1;
            uint32 Length = 1;
        };
        [[nodiscard]] OpenBins BinsOf(uint32 rung) const;
        /// The arena's bake, ensured (baked or loaded) on its first episode; null when the map cannot be baked.
        [[nodiscard]] Vision::TravGrid const* EnsureGrid(Env const& env, Map* map, Player const* bot) const;
        /// The pair draw (sec 2): the goal, its cost map and the start, into `nav`. False when the bake has no pair.
        bool DrawPair(Env& env, Map* map, Player* bot, EnvNav& nav);
        /// A failure of `env`'s arena noted at `where` with the episode's goal, for the replay draw.
        void NoteFailure(Env const& env, EnvNav const& nav, Position const& where);
        /// The world point of a bake cell: its centre at the standing height (the bed when `bed`).
        [[nodiscard]] static Position CellPoint(Vision::TravGrid const& grid, uint32 cell, bool bed);
        /// The route distance at a world point: TRAV_UNREACHABLE off the bake.
        [[nodiscard]] static float CostAt(EnvNav const& nav, float x, float y);
        /// The waypoint regret of a cell goal the seat just chose: C(its block) less the least C over the choosable
        /// blocks of the crop it was chosen from; nothing when it cannot be read.
        void NoteWaypoint(Env const& env, uint32 seatIndex, EnvNav& nav, SeatNav& seat) const;
        /// The episode's generator (seeded in BeforeLevel: from the evaluation seed in an evaluation).
        [[nodiscard]] std::mt19937& Rng(Env const& env) const;
        [[nodiscard]] float Uniform(Env const& env) const;

        std::vector<EnvNav> _envs;
        mutable std::vector<std::mt19937> _rngs;
        PartyFollowEncounter const* _follow = nullptr;
        std::mutex _poolLock;
        std::map<uint32, FailurePool> _pools;   // per arena index
    };
}

#endif
