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

#ifndef ANIMUS_LIB_CURRICULUM_PARTY_FOLLOW_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_PARTY_FOLLOW_ENCOUNTER_H

#include "Block.h"
#include "Encounter.h"
#include "EntranceRespawn.h"
#include "Env.h"
#include "Position.h"
#include "RoutePlanner.h"
#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace Animus::Curriculum
{
    /// **The party follow** (Opposition::PartyFollow; dungeon-curriculum I5, the M4 stage move4_follow): a party of
    /// learned followers (ArenaDefinition::PartySize, 1-4) keeps with a leader through an emptied dungeon, from its
    /// door along its route -- the place of each of its bosses in turn (InstanceBosses' Dungeon rows on the arena's
    /// map) -- stopping at each.
    ///
    /// **The leader** is in the owner's slot (StageScenario::OwnerAgent, BuildOwnerSeat), moved by the player
    /// controller and reported to the server as a client, never a spline. Scripted, its keys are the seek helper's
    /// (Movement::Seek) toward the next corner of the route planner's way to the next stop: the navmesh drives the
    /// script, never a bot's input. PartyFollow.CastShare percent of training episodes hand it to a frozen checkpoint
    /// instead (stage.json cast "leader"; its objective is the next stop); evaluations always keep the script. **Not
    /// dead code:** this leader is the one user of the owner's slot (OwnerAgent, BuildOwnerSeat, CastOwnerActive, the
    /// follow case of StageScenario's cast row), left standing when the first curriculum's owner was deleted around it.
    ///
    /// **The ladder** (the shaping fade's rungs, SightDraw::Rung; an evaluation plays the training rung): rung 0 a
    /// slow, steady leader (walking, long stops at the stops); rung 1 running; rung 2 also stopping unannounced; rung 3
    /// stopping unannounced more often and sometimes stepping back first. Drops are the route's own (Ragefire's
    /// cavern, the Deadmines' ramps), on every rung.
    ///
    /// **What a follower knows of the leader**: the party frames and the minimap's dots (the party frames block), what
    /// the camera shows (the entities block) and its own memory -- never a bearing through a wall beyond the minimap.
    ///
    /// **Death** (dungeon-curriculum I4, EntranceRespawn): a follower that dies is out Respawn.DelayMs, then alive at
    /// the instance's entrance at full health and power, and walks back; the episode never ends on it. A dead
    /// scripted leader is stood up where it fell (it is the script's, not a seat).
    ///
    /// Paid to each follower (PartyFollow.*): FollowKept per second in the band (Outcome), Regroup at each stop of the
    /// leader's (Outcome), Lost and Blocking per second (Costs), Death (Cost); Stuck and Wall at their fixed price
    /// (Seek.*). Nothing is shaped: the band is paid only as Outcome.
    class PartyFollowEncounter final : public Encounter
    {
    public:
        PartyFollowEncounter(StageScenario& scenario, uint32 envs);

        /// Whether the episode's leader is a frozen checkpoint's (its row is played); else the script's keys.
        [[nodiscard]] bool IsCast(Env const& env) const { return _envs[env.Index].Cast; }
        /// Whether the episode has a leader in the owner's slot for the controller to move.
        [[nodiscard]] bool HasLeader(Env const& env) const { return _envs[env.Index].Built; }

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
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Deactivate(Env& env) override;
        void Teardown(Env& env) override;

        /// The ladder's rungs (the fade's four, SightDraw::RUNGS).
        static constexpr uint32 RUNGS = 4;

        /// Whether a follower at (x, y) stands in a moving leader's way: within `yards` of it, inside `halfAngle`
        /// degrees either side of its facing.
        [[nodiscard]] static bool InTheWay(float leaderX, float leaderY, float leaderYaw, bool leaderMoving, float x,
            float y, float yards, float halfAngle);
        /// What a regroup `seconds` after the leader stopped is paid, as a share of PartyFollow.Regroup.
        [[nodiscard]] static float RegroupShare(float seconds, float window);
        /// How long the leader stands at a stop on rung `rung`, seconds: from `first` (rung 0) to `last` (the top).
        [[nodiscard]] static float StopSeconds(uint32 rung, float first, float last);

    private:
        /// The leader's state of mind.
        enum class Phase : uint8
        {
            Stopped,        // standing: at a stop, a sudden stop, or the start
            Walking,        // to the next stop
            BackStep,       // a few yards back, before a sudden stop
            Done,           // the route's end: it stands there for good
        };

        struct SeatFollow
        {
            RespawnClock Clock;
            uint32 InBandMs = 0;
            uint32 LostMs = 0;
            uint32 BlockingMs = 0;
            double DistanceSum = 0.0;
            uint32 Samples = 0;
            bool RegroupPending = false;
            uint32 RegroupStops = 0;            // stops it was counted for
            uint32 Regroups = 0;
            uint32 RegroupMsTotal = 0;
            uint32 LastRewardMs = 0;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
        };

        struct EnvParty
        {
            bool Built = false;
            bool Cast = false;
            uint32 Rung = 0;
            Position Entrance;

            // The leader's route and where it is on it.
            std::vector<Position> Stops;
            uint32 NextStop = 0;
            Route Way;
            uint32 LegStuckMs = 0;          // the leader's controller-stuck time when the leg began
            Phase Mode = Phase::Stopped;
            uint32 StopUntilMs = 0;
            uint32 StopStartMs = 0;
            uint32 NextSuddenMs = 0;
            Position BackTo;
            uint32 BackStopMs = 0;          // the stop a step back ends in
            uint32 StopsReached = 0;
            uint32 Skips = 0;
            uint32 SuddenStops = 0;
            uint32 RegroupStops = 0;
            uint32 Drops = 0;
            uint32 LeaderRises = 0;
            float LastLeaderZ = 0.0f;
            bool HasLeaderZ = false;
            float FallFromZ = 0.0f;
            bool Falling = false;

            std::array<SeatFollow, GROUP_SEATS> Seats{};
        };

        [[nodiscard]] Player* Leader(Env const& env) const;
        /// The dungeon's route on `mapId`: each boss's place (InstanceBosses' Dungeon rows, the world database's
        /// spawns), in the dungeon's order, found once per map.
        [[nodiscard]] std::vector<Position> RouteStops(uint32 mapId);
        /// Yards from seat `seat` to the party: to the leader while it is alive, else to the living others' centroid;
        /// negative with nobody to be with.
        [[nodiscard]] float PartyYards(Env const& env, uint32 seat, Player const* bot) const;
        /// The leader stops for `ms` (a stop of the route's, or a sudden one): the followers' regroup clocks start.
        void Stop(Env const& env, EnvParty& party, uint32 ms, bool counts) const;
        /// The next sudden stop's time, from now, on a rung that has them; never otherwise.
        void ScheduleSudden(Env const& env, EnvParty& party) const;
        /// The leader's keys this decision (the script's).
        void Steer(Env& env, EnvParty& party, Player* leader);
        void RespawnFollowers(Env& env, EnvParty& party);

        std::vector<EnvParty> _envs;
        std::mutex _routesLock;
        std::map<uint32, std::vector<Position>> _routes;
    };
}

#endif
