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
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <vector>

namespace Animus::Curriculum
{
    /// **The party follow** (Opposition::PartyFollow; dungeon-curriculum I5, the M4 stage move4_follow): a party of
    /// learned followers (ArenaDefinition::PartySize, 1-4) keeps with a leader wandering an emptied dungeon.
    ///
    /// **The way** (decision 0033): the stock PathGenerator's path from the dungeon's door through each of its bosses'
    /// places in turn (InstanceBosses' follow rows), worked out once per map and kept as one chain of corners, each
    /// step of it marked walkable both ways, a drop (forward only: nothing climbs back up Ragefire's cavern) or a cut
    /// (where the path stopped short of a stop: neither way). Waypoints sample the chain every PartyFollow.
    /// WaypointYards and at each boss's place.
    ///
    /// **The leader** is in the owner's slot (StageScenario::OwnerAgent, BuildOwnerSeat), moved by the player
    /// controller and reported to the server as a client, never a spline. An episode starts at a random waypoint
    /// (StartAnywhere percent; else the door), so no follower can learn one route by heart; the leader then hops from
    /// waypoint to waypoint along the chain, HopMinYards-HopMaxYards at a time, backwards ReverseChance percent of
    /// hops from ReverseFromRung, standing at a hop's end for StopSeconds of the rung or passing on (PassChance), and
    /// never stops wandering before the clock ends. Scripted, its keys are the seek helper's (Movement::Seek) toward
    /// the next corner of its hop -- the navmesh drives the one scripted mover, the thing followed, never a policy's
    /// input or a teacher of it (decisions 0002, 0019 and 0033). At a halt it looks round now and then (LookChance),
    /// as a player does. PartyFollow.CastShare percent of training episodes hand it to a frozen checkpoint instead
    /// (stage.json cast "leader"; its objective is its hop's end); evaluations always keep the script. Its draws come
    /// from the episode's own generator, seeded by the evaluation seed in an evaluation, so an evaluation plays the
    /// same routes every time.
    ///
    /// **The ladder** (the shaping fade's rungs, SightDraw::Rung; an evaluation plays the training rung): rung 0 a
    /// slow, steady leader (walking, long stops, forward hops); rung 1 running, now and then walking, and turning back
    /// some hops; rung 2 also stopping unannounced; rung 3 stopping unannounced more often and sometimes stepping back
    /// first.
    ///
    /// **What a follower knows of the leader**: the party frames, the minimap's dots and its memory of where each
    /// member was last seen and at what pace (the party frames block, revision 3), what the camera shows (the entities
    /// block) and its own recurrent memory -- never a bearing through a wall beyond the minimap.
    ///
    /// **Death** (dungeon-curriculum I4, EntranceRespawn): a follower that dies is out Respawn.DelayMs, then alive at
    /// the episode's start point (the door, or the waypoint it began at) at full health and power, and walks back; the
    /// episode never ends on it. A dead scripted leader is stood up where it fell (it is the script's, not a seat).
    ///
    /// Paid to each follower (PartyFollow.*): FollowKept per second in the band and Settled per second standing still
    /// in it beside a leader who stands (Outcome), Regroup at each stop of the leader's (Outcome); Fidget per second
    /// moving about in the band then, Drift per second straying past DriftYards, Lost per second past LostYards and
    /// Blocking per second in a moving leader's way (Costs), Death (Cost); Stuck and Wall at their fixed price
    /// (Seek.*). Nothing is shaped: the stage's purpose is paid only as Outcome.
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

        /// One step of the way, from a corner to the next.
        enum class Step : uint8
        {
            Both,           // walkable either way
            Drop,           // a fall forward: walked down, never back up
            Cut,            // the path stopped short here: neither way
        };

        /// **A dungeon's way**: its chain of corners from the door through each boss's place, the yards along it to
        /// each, each step's kind (Steps[i]: Chain[i] to Chain[i + 1]) and the waypoints (chain indexes, ascending).
        struct Way
        {
            std::vector<Position> Chain;
            std::vector<float> Along;
            std::vector<Step> Steps;
            std::vector<uint32> Waypoints;
            /// The waypoints a party may be spawned at: the ground found under each, at its height, and no floor just
            /// above it (a path corner's height can be a lower storey's or under the floor: a party spawned there fell
            /// through the world, 2026-10-12). Hops still go to every waypoint: the leader steers by x and y.
            std::vector<uint32> Starts;
            /// Steps the leader could not walk (BLOCKED_FORWARD, BLOCKED_BACK, per step): marked when it gave up a hop
            /// there after jumping and side-stepping -- a rock lip or a ledge the stock path climbs and a player's
            /// controller does not (2026-10-12) -- and never crossed that way again by any env on the map.
            std::vector<std::atomic<uint8>> Blocked;
        };
        static constexpr uint8 BLOCKED_FORWARD = 1;
        static constexpr uint8 BLOCKED_BACK = 2;

        /// The way through `stops` from `door` as `legs` (the corners of each leg, the stock PathGenerator's), with
        /// waypoints every `waypointYards` and at each leg's end; a leg that ended more than `shortYards` from its
        /// stop is cut there, and a fall of `dropYards` or more between two corners is a drop.
        [[nodiscard]] static Way MakeWay(Position const& door, std::vector<Position> const& stops,
            std::vector<std::vector<Position>> const& legs, float waypointYards, float shortYards, float dropYards);
        /// Whether the leader may walk the chain from index `from` to index `to` (either way along it).
        [[nodiscard]] static bool Walkable(Way const& way, uint32 from, uint32 to);

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
            Stopped,        // standing: at a hop's end, a sudden stop, or the start
            Walking,        // along its hop
            BackStep,       // a few yards back, before a sudden stop
        };

        struct SeatFollow
        {
            RespawnClock Clock;
            uint32 InBandMs = 0;
            uint32 LostMs = 0;
            uint32 BlockingMs = 0;
            uint32 DriftMs = 0;
            uint32 WaitMs = 0;                  // in the band beside a leader who has stood SettleAfterMs
            uint32 SettledMs = 0;               // ... standing still
            uint32 FidgetMs = 0;                // ... moving about
            float LastGap = -1.0f;              // the capped gap past the band at the last reward, -1 none yet
            float ApproachYards = 0.0f;         // the gap's yards closed, less opened, this episode
            float MovedYards = 0.0f;            // yards walked this episode (alive, between rewards)
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
            uint32 MapId = 0;
            /// Where the episode began, and where a dead follower rises: the door, or the waypoint drawn.
            Position Start;
            float StartAlong = 0.0f;            // yards along the way from the door

            // The leader's hop: the chain index it last stood at, the one it walks to, the corners between and the
            // one it walks to now, and how it goes.
            uint32 At = 0;
            uint32 Target = 0;
            std::vector<Position> Hop;
            std::vector<uint32> HopIndex;       // each hop corner's chain index
            uint32 Corner = 0;
            bool HopStops = true;
            bool HopWalk = true;
            uint32 LegStuckMs = 0;              // the leader's controller-stuck time when the hop began
            Phase Mode = Phase::Stopped;
            uint32 StopUntilMs = 0;
            uint32 StopStartMs = 0;
            uint32 NextSuddenMs = 0;
            Position BackTo;
            uint32 BackStopMs = 0;              // the stop a step back ends in
            // Looking round at a halt: a turn at LookRate from LookFromMs to LookUntilMs, and maybe one back after.
            uint32 LookFromMs = 0;
            uint32 LookUntilMs = 0;
            float LookRate = 0.0f;
            bool LookAgain = false;

            uint32 Hops = 0;
            uint32 HopsDone = 0;
            uint32 ReverseHops = 0;
            uint32 PassHops = 0;
            uint32 StopsReached = 0;
            uint32 Skips = 0;
            uint32 SuddenStops = 0;
            uint32 RegroupStops = 0;
            uint32 LookStops = 0;
            uint32 Drops = 0;
            uint32 LeaderRises = 0;
            float LastLeaderZ = 0.0f;
            bool HasLeaderZ = false;
            float FallFromZ = 0.0f;
            bool Falling = false;

            std::array<SeatFollow, GROUP_SEATS> Seats{};
        };

        [[nodiscard]] Player* Leader(Env const& env) const;
        /// The dungeon's stops on `mapId`: each boss's place (InstanceBosses' follow rows, the world database's
        /// spawns, read once at startup), in the dungeon's order, found once per map.
        [[nodiscard]] std::vector<Position> RouteStops(uint32 mapId);
        /// The map's way when it has been worked out (the first episode on a map works it out), else null.
        [[nodiscard]] Way const* WayOf(uint32 mapId) const;
        /// Works out `map`'s way once (MakeWay over the stock PathGenerator's legs). Called from Build, on the thread
        /// that resets: PathGenerator uses the map's one navmesh query, which is safe only on the world thread outside
        /// MapMgr::Update or inside the map's own task (a PartyFollow stage resets on the world thread: StageScenario::
        /// ResetsStayOnMap).
        Way const* WorkOutWay(Map* map, Player* walker, Position const& door);
        /// Yards from seat `seat` to the party: to the leader while it is alive, else to the living others' centroid;
        /// negative with nobody to be with.
        [[nodiscard]] float PartyYards(Env const& env, uint32 seat, Player const* bot) const;
        /// The leader stops for `ms` (a stop at a hop's end, or a sudden one): the followers' regroup clocks start, and
        /// it may look round.
        void Stop(Env const& env, EnvParty& party, uint32 ms, bool counts) const;
        /// The next sudden stop's time, from now, on a rung that has them; never otherwise.
        void ScheduleSudden(Env const& env, EnvParty& party) const;
        /// Draws the leader's next hop from where it stands (party.At); false when the way has nowhere to go.
        bool PickHop(Env const& env, EnvParty& party) const;
        /// The leader's keys this decision (the script's).
        void Steer(Env& env, EnvParty& party, Player* leader);
        /// The leader gave its hop up at its current corner: the step into that corner is marked blocked that way
        /// (Way::Blocked), for every env on the map.
        void BlockStep(EnvParty const& party);
        void RespawnFollowers(Env& env, EnvParty& party);

        /// The episode's generator (seeded in BeforeLevel: from the evaluation seed in an evaluation).
        [[nodiscard]] std::mt19937& Rng(Env const& env) const { return _rngs[env.Index]; }
        [[nodiscard]] bool Chance(Env const& env, int32 percent) const;
        [[nodiscard]] float Uniform(Env const& env, float low, float high) const;

        std::vector<EnvParty> _envs;
        /// Per env, the chain index BeforeLevel drew the episode's start at (0: the door), for Build.
        std::vector<uint32> _starts;
        mutable std::vector<std::mt19937> _rngs;
        mutable std::mutex _routesLock;
        std::map<uint32, std::vector<Position>> _routes;
        std::map<uint32, Way> _ways;
        /// Give-ups told per map (the first SKIPS_TOLD of each, in the log).
        std::map<uint32, uint32> _skipsTold;
        static constexpr uint32 SKIPS_TOLD = 25;
    };
}

#endif
