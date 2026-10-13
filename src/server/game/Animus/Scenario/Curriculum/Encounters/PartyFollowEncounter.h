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
#include <map>
#include <mutex>
#include <random>
#include <vector>

class Group;

namespace Animus::Curriculum
{
    /// **The party follow** (Opposition::PartyFollow; dungeon-curriculum I5, the M4 stage move4_follow): a party of
    /// learned followers (ArenaDefinition::PartySize, 1-4) keeps with the leader of its group wandering an emptied
    /// dungeon (decision 0033; the move4-follow rework, 2026-10-13).
    ///
    /// **The way:** the stock PathGenerator's path from the dungeon's door through each of its bosses' places in turn
    /// (InstanceBosses' follow rows), worked out once per map and kept as one chain of corners, each step of it
    /// marked walkable both ways, downhill one way only (a drop or a slope a player's controller cannot climb back:
    /// MakeWay's rule, so the leader never leads where a follower cannot go) or a cut (where the path stopped short of
    /// a stop: neither way). Waypoints sample the chain every PartyFollow.WaypointYards and at each boss's place.
    ///
    /// **The leader** is a player in the owner's slot (StageScenario::BuildOwnerSeat) and the real leader of the
    /// party's group (a sim Group it leads, the followers its members, as a realm's group is), so the followers'
    /// party frames read it the way they read a human leader on a realm (PartyFramesBlock::FillFromGroup). It has no
    /// learner's row and no controller: the encounter moves it along the way at its run speed, hop by hop -- a
    /// script, the thing followed and never a teacher of the policy (decisions 0002, 0019); the deliberate exception
    /// to "controller-only movement", which is for learning seats. An episode starts at a random waypoint
    /// (StartAnywhere percent; else the door); the leader hops HopMinYards-HopMaxYards along the way, backwards
    /// ReverseChance percent of hops from ReverseFromRung, passing on without a stop PassChance percent, in bursts of
    /// speed (BurstFromRung) now and then, standing at a hop's end for StopSeconds of the rung, looking round at some
    /// halts (LookChance), until the clock ends. Its draws come from the episode's own generator, seeded by the
    /// evaluation seed in an evaluation, so an evaluation replays the same wanderings.
    ///
    /// **Seen as a player is seen:** the server learns a player's position only from its movement packets. The
    /// leader's true pose moves every update, but the server's copy of it (its Player's position and facing, which
    /// every follower's frame, dot, recall, camera and reward reads) is set only when a client would have sent one
    /// (Movement::Cadence): at once on a start, a stop or a facing change of 0.1 rad, else every 500 ms while it
    /// moves, and once at the episode's start. leader_report_age_ms says how stale that copy was at the decisions
    /// (leader_moving_report_age_ms: at the ones the leader moved through; stops send nothing).
    ///
    /// **The ladder** (the shaping fade's rungs, SightDraw::Rung; an evaluation plays the training rung): rung 0
    /// forward hops and long stops; rung 1 turning back and speed bursts; rung 2 also stopping unannounced; rung 3
    /// stopping unannounced more often and sometimes stepping back first.
    ///
    /// **Death** (dungeon-curriculum I4, EntranceRespawn): a follower that dies is out Respawn.DelayMs, then alive at
    /// the episode's start point (the door, or the waypoint it began at) at full health and power, and walks back; the
    /// episode never ends on it. A dead leader is stood up where it fell (it is the script's, not a seat).
    ///
    /// Paid to each follower (PartyFollow.*): FollowKept per second in the band and Settled per second standing still
    /// in it beside a leader who stands (Outcome), Regroup at each stop of the leader's (Outcome); Approach (Shaping)
    /// per yard of the gap closed; Fidget per second moving about in the band then, Drift per second straying past
    /// DriftYards (DriftMovingYards while the leader moves), Lost per second past LostYards and Blocking per second in
    /// a moving leader's way (Costs), Death (Cost); Stuck and Wall at their fixed price (Seek.*), Wall escalating as
    /// the seek stage's does (Seek.WallEscalateSeconds). The stage's purpose is paid only as Outcome.
    class PartyFollowEncounter final : public Encounter
    {
    public:
        PartyFollowEncounter(StageScenario& scenario, uint32 envs);

        /// Whether the episode has a leader in the owner's slot.
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
            DownForward,    // a drop, or a slope too steep to climb: walked forward (down), never back
            DownBackward,   // the same the other way: walked backward (down), never forward
            Cut,            // the path stopped short here: neither way
        };

        /// **A dungeon's way**: its chain of corners from the door through each boss's place, the yards along it to
        /// each, each step's kind (Steps[i]: Chain[i] to Chain[i + 1]), the waypoints (chain indexes, ascending) and
        /// the ones a party may start at (their ground found at their height, no floor just above, and not a dead
        /// end). Dead[i] marks a waypoint the leader could never leave for another live one (a stretch walled in by
        /// cuts, the foot of a drop before a cut): never a start, never a hop's end.
        struct Way
        {
            std::vector<Position> Chain;
            std::vector<float> Along;
            std::vector<Step> Steps;
            std::vector<uint32> Waypoints;
            std::vector<uint32> Starts;
            std::vector<bool> Dead;
        };

        /// The way through `stops` from `door` as `legs` (the corners of each leg, the stock PathGenerator's), with
        /// waypoints every `waypointYards` and at each leg's end; a leg that ended more than `shortYards` from its
        /// stop is cut there. A step that climbs `dropYards` or more, or more than `climbPerYard` of its run plus
        /// `stepUp` (what a player's controller steps up), is walked downhill only.
        [[nodiscard]] static Way MakeWay(Position const& door, std::vector<Position> const& stops,
            std::vector<std::vector<Position>> const& legs, float waypointYards, float shortYards, float dropYards,
            float climbPerYard, float stepUp);
        /// Whether the leader may walk the chain from index `from` to index `to` (either way along it).
        [[nodiscard]] static bool Walkable(Way const& way, uint32 from, uint32 to);
        /// Fills way.Dead: a waypoint is dead when no live waypoint is walkable from it (to a fixed point).
        static void MarkDead(Way& way);

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
            uint32 OutOfSightMs = 0;            // the leader neither in the camera's frame nor in line of sight
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
            uint32 WallRunMs = 0;               // contiguous decisions that paid Wall (the escalation's run)
            uint32 WallPinMax = 0;              // the longest such run
            float WallExtra = 0.0f;             // what the escalation added
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
        };

        struct EnvParty
        {
            bool Built = false;
            uint32 Rung = 0;
            uint32 MapId = 0;
            Group* PartyGroup = nullptr;
            /// Where the episode began, and where a dead follower rises: the door, or the waypoint drawn.
            Position Start;
            float StartAlong = 0.0f;            // yards along the way from the door

            // The leader's hop: the chain index it last stood at, the one it walks to, the corners between and the
            // one it walks to now, and how it goes.
            uint32 At = 0;
            uint32 Target = 0;
            std::vector<Position> Hop;
            uint32 Corner = 0;
            bool HopStops = true;
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
            // A burst of speed: the rate on its run speed until BurstUntilMs.
            uint32 BurstUntilMs = 0;
            float BurstRate = 1.0f;

            // The leader's true pose, moved every update, and the pose the server holds of it: set at a player's
            // report cadence (Report), what everything the followers read comes from.
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            float Yaw = 0.0f;
            bool Moving = false;
            float RepX = 0.0f;
            float RepY = 0.0f;
            float RepZ = 0.0f;
            float RepYaw = 0.0f;
            bool RepMoving = false;
            uint32 RepMs = 0;                   // when the server's copy was last set
            bool Reported = false;              // ... at all this episode
            uint32 LastUpdateMs = 0;            // the clock at the last Update (its dt)
            uint64 ReportAgeSumMs = 0;          // the copy's age at each update, summed
            uint32 ReportAgeSamples = 0;
            uint64 MovingReportAgeSumMs = 0;    // ... at the updates the leader moved through
            uint32 MovingReportAgeSamples = 0;

            uint32 Hops = 0;
            uint32 HopsDone = 0;
            uint32 ReverseHops = 0;
            uint32 PassHops = 0;
            uint32 StopsReached = 0;
            uint32 SuddenStops = 0;
            uint32 RegroupStops = 0;
            uint32 LookStops = 0;
            uint32 Bursts = 0;
            uint32 Reports = 0;
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
        /// The party's group: the leader leading, the followers its members (Build); gone with Disband.
        void FormGroup(Env& env, EnvParty& party, Player* leader);
        void Disband(EnvParty& party);
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
        /// Moves the leader's true pose by `dt` ms along its plan (the script).
        void Walk(Env& env, EnvParty& party, Player* leader, uint32 dt);
        /// Sets the server's copy of the leader's pose when a client would have reported it (or at `force`).
        void Report(Env const& env, EnvParty& party, Player* leader, bool force);
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
    };
}

#endif
