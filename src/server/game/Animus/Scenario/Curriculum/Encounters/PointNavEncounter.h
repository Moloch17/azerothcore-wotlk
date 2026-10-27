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
#include "ObjectGuid.h"
#include "Position.h"
#include "TravBake.h"
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    struct ArenaDefinition;
    class PartyFollowEncounter;

    /// **PointNav** (Opposition::PointNav; vision movement curriculum, decision 0036, CONTRACT.md secs 1-3): a point
    /// goal over a cleared dungeon, shown to the seat only as the route the map block plans to it on the seat's own
    /// map (map block revision 7, 2026-10-17: no compass, no straight line; `View` names the point), the route to be
    /// found. The arena's traversability bake (Vision::TravRegistry, Runtime/Vision/
    /// TravBake) is baked on the arena's first episode and gives every episode its pair: the goal first, one Dijkstra
    /// from it, then a start whose route length and detour (route over the straight line) fall in bins the rung opens
    /// (PointNav.DetourBins, LengthBins, RungBins0..3; the rung is the shaping fade's, SightDraw::Rung). ReplayShare
    /// percent of training episodes start where a seat of this arena failed -- pinned PinSeconds, fell for damage,
    /// drowned or timed out -- with that goal (a ring of FailurePool); an evaluation's pairs come from its seed. A
    /// SightOnly arena (nav1) keeps only starts whose eye sees the goal; a goal on a water cell is at the surface, at
    /// the bed BedShare percent of the time. Neither end is drawn on a cell the seat's own body would swim (liquid
    /// deeper than SWIM_ENTER of its height over the bed): the bake's wadeable is the tallest race's.
    ///
    /// Paid (PointNav.*): Arrive once (Outcome) on stopping (Standing::Stopped) within ArriveYards of the goal and
    /// ArriveRise of its height; StepCost per decision, Death, FallHurt per share of maximum health a fall took,
    /// Drowned on a death under water, Lost per second while the route distance to the goal exceeds the start's by
    /// LostYards, Revisit per second on bake cells stood on before (arenas with Revisit) -- Costs; Stuck and Wall at
    /// their fixed price (Controls.*, as M1). No shaping from the sim: the learner pays its potential on the route
    /// distance the STEP's labels carry. The episode ends on arrival; a death rises at the start point after
    /// Respawn.DelayMs (EntranceRespawn).
    ///
    /// **Labels** (sec 3.1; WriteLabels): per seat, the bake's classes and the route cost-to-go in the fine and
    /// coarse crop frames of the seat's last observation (SeatState::Crop), then C(body), C(start), the route
    /// cost-to-go on the seat's own map at the body and at the body's last decision's position (protocol 33: the
    /// route the map block planned on the map's own 1-yd cells, Vision::MapPlanner, read off the crop's pose,
    /// CropPose::Route*; both on the map as it is now, so their difference is movement alone, never the map changing;
    /// the first decision's "last" is the
    /// body's own; a last position more than LABEL_STEP_MAX yards off -- a rise after a death, a teleport -- reads
    /// unreachable, which the learner pays nothing for) and the goal in the body frame. **LabelsOnly** (nav6, beside
    /// PartyFollowEncounter): nothing placed or paid; the goal is the leader's position as the server last had it,
    /// and the cost map is recomputed when its cell changes.
    class PointNavEncounter final : public Encounter
    {
    public:
        PointNavEncounter(StageScenario& scenario, uint32 envs);

        /// Bytes per agent of the STEP's labels (ScenarioSpec::LabelBytes).
        static constexpr uint32 LABEL_BYTES = Vision::TRAV_LABEL_BYTES;
        /// The furthest a body moves between two decisions' labels on its own feet: further is a teleport, and the
        /// own-map route at the last position is not read (a decision at run speed moves under 4 yd).
        static constexpr float LABEL_STEP_MAX = 12.0f;
        /// A dungeon run's rungs (DrawRun): rung 0's starts within RUN_NEAR yards of route from the goal, rung 2's
        /// within RUN_GATE_BAND of the gate on its near side; summons swept from a shut door this long after it opened,
        /// within RUN_SWEEP_YARDS of it.
        static constexpr float RUN_NEAR = 120.0f;
        /// A clear way's rung 0: its starts within this many checkpoints of the goal.
        static constexpr uint32 RUN_NEAR_SITES = 4;
        /// A clear way's sites: spawns further apart in height than this are on different floors (another site).
        static constexpr float RUN_SITE_RISE = 8.0f;
        static constexpr float RUN_GATE_BAND = 60.0f;
        static constexpr uint32 RUN_SWEEP_MS = 15000;
        static constexpr float RUN_SWEEP_YARDS = 150.0f;
        /// Legs, then the whole way (ArenaDefinition::RunLegs): a leg's success rate is a running mean over about
        /// LEG_WINDOW of its training trips; it is learned at LEG_LEARNED (after LEG_MIN_TRIES), drawn by its failure
        /// rate with LEG_FLOOR the least weight; once every leg is learned LEG_FULL_SHARE of the trips are the whole
        /// way. A leg's clock is the trip clock without the arena's floor, and LEG_DOOR_SECONDS more for a door.
        static constexpr uint32 LEG_WINDOW = 20;
        static constexpr float LEG_LEARNED = 0.85f;
        static constexpr uint32 LEG_MIN_TRIES = 10;
        static constexpr float LEG_FLOOR = 0.1f;
        static constexpr float LEG_FULL_SHARE = 0.8f;
        static constexpr float LEG_DOOR_SECONDS = 30.0f;
        /// A goal in water is priced by depth too (DepthCost): the height off it past ArriveRise, added to every route
        /// cost the labels carry, in full within DEPTH_FULL yards of it across and fading to nothing at DEPTH_REACH.
        /// The bake prices a water cell the same from its bed to its surface, so the potential paid nothing for the
        /// last few yards down or up (nav5 2026-10-21: 40 % of the failed trips over the goal at the wrong depth).
        static constexpr float DEPTH_FULL = 2.0f;
        static constexpr float DEPTH_REACH = 10.0f;
        /// The failure pool's and the draw's bins are the tuning's lists; the ladder is the fade's four rungs.
        static constexpr uint32 RUNGS = 4;

        /// **The map audit** (a diagnostic; nothing of it is observed or paid): at a trip's end short of the goal, the
        /// bake's true route from the body (its cost-to-go descended along real edges) against the seat's own map --
        /// a band one bake cell either side of it priced as the fine planner prices the map (Vision::CellFactor,
        /// Vision::ConfigurationSpace), and the cheapest way through the band: its cells by what the map holds (a wall,
        /// a hole the configuration space closed, unknown, a hazard, floor), the walls by their writer
        /// (Vision::WallSource) and by what the bake has round them (a phantom on open floor, a real wall's face, a
        /// step or a drop). The audit_* columns; a timed-out evaluation trip also writes the band (DumpAudit).
        enum AuditField : uint32
        {
            AUDIT_UNREACHABLE = 0,  // 1: the body's bake cell has no route to the goal (or is off the bake)
            AUDIT_TRUE_YARDS,       // the bake's route cost-to-go from the body
            AUDIT_CORRIDOR_COST,    // the cheapest way through the band on the seat's map, planner yards
            AUDIT_TRUNCATED,        // 1: the true route was cut at AUDIT_SIDE_MAX cells across
            AUDIT_CELLS,            // the 1-yd cells of that way
            AUDIT_WALL,             // ... the map holds a wall in
            AUDIT_CLOSED,           // ... too narrow for the body (its centre within the clearance of a face)
            AUDIT_UNKNOWN,
            AUDIT_HAZARD,
            AUDIT_FLOOR,            // ... known floor or door (inflated or pinched included)
            AUDIT_FIRST_BLOCK,      // yards along the way to its first wall or closed cell (0: none)
            AUDIT_WALL_RAY,         // the way's walls by writer
            AUDIT_WALL_MESH,
            AUDIT_WALL_BUMP,
            AUDIT_WALL_INHERITED,
            AUDIT_WALL_TRUE,        // ... by the bake round them: the bake's own cell is no ground
            AUDIT_WALL_FACE,        // ... a neighbouring bake cell is (a real wall's face, smeared)
            AUDIT_WALL_STEP,        // ... a step, jump or drop is there
            AUDIT_WALL_PHANTOM,     // ... open floor all round
            AUDIT_WALL_FLOORLESS,   // the way's walls on a cell with no floor known
            AUDIT_WALL_LAYER_OFF,   // ... with floors, none within LAYER_MERGE of the bake's ground
            AUDIT_FELL,             // the trip's landings from 2 yd or more (SeatState::Drops)
            AUDIT_STRANDED,         // ... after which the body stood on dead ground (StrandedCheck)
            AUDIT_ROUTE_DROPS,      // the drops on the true route from the body
            AUDIT_ROUTE_DROPS_SEEN, // ... onto a floor the seat's map had seen
            AUDIT_COUNT
        };
        static constexpr char const* AUDIT_NAMES[AUDIT_COUNT] = { "audit_unreachable", "audit_true_yards",
            "audit_corridor_cost", "audit_truncated", "audit_cells", "audit_wall", "audit_closed", "audit_unknown",
            "audit_hazard", "audit_floor", "audit_first_block_yards", "audit_wall_ray", "audit_wall_mesh",
            "audit_wall_bump", "audit_wall_inherited", "audit_wall_true", "audit_wall_face", "audit_wall_step",
            "audit_wall_phantom", "audit_wall_floorless", "audit_wall_layer_off", "audit_fell", "audit_stranded",
            "audit_route_drops", "audit_route_drops_seen" };
        /// The band's square, 1-yd cells a side, at most: a longer true route is audited from the body this far.
        static constexpr int32 AUDIT_SIDE_MAX = 256;
        /// **Dead ground** (2026-10-19): after a landing from 2 yd or more, a body on the ground STRANDED_DECISIONS
        /// decisions running with no route to the goal from any walkable floor within STRANDED_REACH yards at its
        /// height (RouteNear) is stranded: charged PointNav.Death, and out until it rises at the start after the
        /// respawn delay, exactly as a death -- the drop led nowhere, and the planner should learn which drops do.
        static constexpr uint32 STRANDED_DECISIONS = 2;
        /// The pair pool's size: goals drawn, and starts for each (one cost-to-go a goal), and how far round a start a
        /// trap drop is looked for (TrapNear).
        static constexpr uint32 PAIR_GOALS = 200;
        static constexpr uint32 PAIR_STARTS = 40;
        static constexpr float PAIR_TRAP_YARDS = 20.0f;
        static constexpr float STRANDED_REACH = 4.0f;
        static constexpr float STRANDED_RISE = 3.0f;
        /// Where a timed-out evaluation trip's band is written (`<seed>-seat<seat>.audit`); empty for nowhere. Set by
        /// the forge as each evaluation's videos start (their directory's audit/).
        static void SetAuditDir(std::string dir);

        /// An arena's clear way (ArenaDefinition::RunClear): the checkpoints in order (their bake nodes and places),
        /// each leg's route yards from the one before (Leg[0]: 0), the yards after each (After), the gate's index, and
        /// each checkpoint's creature spawn ids (a site's; none for the gate and the goal).
        struct ClearWay
        {
            bool Built = false;
            std::vector<Position> Points;
            std::vector<uint32> Nodes;
            std::vector<float> Leg;
            std::vector<float> After;
            std::vector<std::vector<uint32>> Spawns;
            uint32 Gate = UINT32_MAX;
            /// Per checkpoint, the game object a recorded gate stands for (its spawn id: a door, the gunpowder
            /// barrel, the cannon's door), 0 for a site or the goal; a generated way has none.
            std::vector<uint32> GateSpawns;
            [[nodiscard]] bool IsGate(uint32 k) const
            {
                return k == Gate || (k < GateSpawns.size() && GateSpawns[k] != 0);
            }
            /// **The recorded line** (a recorded way's `<name>.path.csv` beside it, `python -m animus.human
            /// dungeon-run`): per leg k (checkpoint k-1, or the start, to checkpoint k) the player's positions in
            /// order, their length in yards and the seconds the player took. Empty without the file; the measures
            /// only, never a reward (InstanceEncounter's path_* columns).
            std::vector<std::vector<Position>> LegPath;
            std::vector<float> LegYards;
            std::vector<float> LegSeconds;
            /// The yards from `at` to leg `k`'s recorded line (its nearest segment), or a negative number without one.
            [[nodiscard]] float PathDistance(uint32 k, Position const& at) const;
        };
        /// The env's arena's clear way, its bake ensured first (the encounters that place a fight or a party along it
        /// build before this one does); null without RunClear or a bake.
        [[nodiscard]] ClearWay const* WayOf(Env const& env, Map* map, Player const* bot);
        /// Where checkpoint `k`'s first creature spawned (a site's own ground); false for the gate, the goal and past
        /// the end.
        [[nodiscard]] static bool SiteSpawn(ClearWay const& way, uint32 k, Position& at);
        /// The first checkpoint after `after` (-1: from the first) that is a site, wrapping round once; -1 for none.
        [[nodiscard]] static int32 NextSite(ClearWay const& way, int32 after);

        /// **The leg rates for another encounter's legs** (a dungeon wing's, ArenaDefinition::WingLegs), kept per
        /// arena and `rung` (a level ladder's): whether every one of `legs` is learned (and the share learned), the
        /// leg a training trip draws by failure rate, and a leg's outcome noted. The same rule as this encounter's
        /// own legs (LEG_*). Thread-safe.
        [[nodiscard]] bool LegsLearnedAt(uint32 arena, uint32 rung, uint32 legs, float* share = nullptr);
        [[nodiscard]] uint32 DrawLegAt(Env const& env, uint32 rung, uint32 legs);
        void NoteLegAt(uint32 arena, uint32 rung, uint32 leg, bool arrived);
        /// **The teacher's share at a leg** (TeacherTuning): Teacher.Max until the leg has been measured, then moved at
        /// most Teacher.Step a measured trip (NoteLegAt, the teacher-free trips) toward
        /// Max x clamp(1 - rate / Target, 0, 1). 0 with the teacher off.
        [[nodiscard]] float LegTau(uint32 arena, uint32 rung, uint32 leg);
        /// A drawn start (Instance.WingStarts): a checkpoint in [1, legs) not `blocked`, by its failure rate as
        /// DrawLegAt draws (untried first, LEG_FLOOR the least weight); 0 when none can be drawn. NoteStartAt counts
        /// a start's outcome toward its rate at the rung.
        [[nodiscard]] uint32 DrawStartAt(Env const& env, uint32 rung, uint32 legs, std::vector<uint8> const& blocked);
        void NoteStartAt(uint32 arena, uint32 rung, uint32 start, bool progressed);
        static constexpr float LegFullShare() { return LEG_FULL_SHARE; }
        static constexpr float LegDoorSeconds() { return LEG_DOOR_SECONDS; }

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
        /// A death rises at the start (Update); arriving ends the episode (IsTerminal).
        /// The episode ends on arrival (2026-10-14: running the clock out after it, nothing paid or charged, was more
        /// than half of nav1's decisions -- shuffling on the point). A labels-only arena (nav6) never ends it.
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

        /// Seat `seat`'s LABEL_BYTES of labels for the observation just taken (its SeatState::Crop pose, which
        /// carries the own-map route the map block planned), into `out`; `maps` that observation's map crops
        /// (`mapBytes` of them), unread since map block revision 6. Zeros, with every cost unreachable, for a seat
        /// with no bake or no body. Once a decision a seat: it keeps the body's position for the next decision's
        /// label.
        void WriteLabels(Env const& env, uint32 seat, uint8 const* maps, uint32 mapBytes, uint8* out);

        /// A comma list of numbers ("1.0,1.2,1.6") parsed; empty on garbage.
        [[nodiscard]] static std::vector<float> ParseList(std::string const& list);

    private:
        /// **A measured pair** (the pair pool, 2026-10-19): a (start, goal) of bake nodes and what its true route
        /// asks -- its length, the straight line, the rise, the corners and turning of the route string-pulled on the
        /// server's navmesh (or of the bake's own route where it drops), the level changes, drops, damaging drops and
        /// the highest, the yards swum, whether the goal is in sight from the start, whether the navmesh walks it, and
        /// whether a drop onto dead ground lies within PAIR_TRAP_YARDS of the start.
        struct NavPair
        {
            uint32 Start = Vision::TRAV_NO_CELL;
            uint32 Goal = Vision::TRAV_NO_CELL;
            float Route = 0.0f;
            float Straight = 0.0f;
            float Rise = 0.0f;
            float MaxDrop = 0.0f;
            float SwimYards = 0.0f;
            uint16 TurnDegrees = 0;
            uint8 Corners = 0;
            uint8 Levels = 0;
            uint8 Drops = 0;
            uint8 HurtDrops = 0;
            bool InSight = false;
            bool NavWalks = false;
            bool TrapNear = false;
        };
        /// An arena's measured pairs, built once on its first trip (BuildPairs).
        struct PairPool
        {
            bool Built = false;
            std::vector<NavPair> Pairs;
        };

        struct SeatNav
        {
            RespawnClock Clock;
            bool Reached = false;
            uint32 ArriveMs = 0;
            float StopError = 0.0f;
            float Travelled = 0.0f;
            float TurnRadians = 0.0f;       // radians the body turned this episode (PointNav.Turning)
            float LastTurned = 0.0f;        // SeatState::TurnedRadians at the last reward
            float LastX = 0.0f;
            float LastY = 0.0f;
            float LastZ = 0.0f;
            // Dead ground (StrandedCheck): the landings counted at the last reward, the grounded decisions still to
            // wait after the newest, whether the seat is stranded (out until the rise, as a death), and how often.
            uint32 LastDrops = 0;
            uint32 LandedChecks = 0;
            bool Stranded = false;
            /// The trip ended in a death (PointNav.DeathEnds): failed, no rise; the episode ends (IsTerminal).
            bool Failed = false;
            uint32 Strandings = 0;
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
            // The own-map route labels (WriteLabels): where the body was at the last label, and the route on its
            // own map at the episode's first label and at the newest (the map_route_* columns).
            bool HasLabelPos = false;
            float LabelX = 0.0f;
            float LabelY = 0.0f;
            float LabelZ = 0.0f;
            float MapRouteStart = Vision::TRAV_UNREACHABLE;
            float MapRouteLast = Vision::TRAV_UNREACHABLE;
            // The route's source over the labelled decisions (CropPose::RouteGlobal: the global route, else the
            // window plan) and the global route's recomputes past each trip's first plan, contradictions among them
            // (the route_source_global_share, global_replans and global_replans_contradiction columns).
            uint32 RouteDecisions = 0;
            uint32 GlobalDecisions = 0;
            uint32 GlobalReplans = 0;
            uint32 GlobalContradictions = 0;
            uint32 GlobalKept = 0;
            // Looking around (PointNav.Explore): the map's searched cells at the last reward, and what was paid.
            bool HasSearched = false;
            uint32 LastSearched = 0;
            float ExplorePaid = 0.0f;
            // The map audit (AuditField), made once at the episode's end by the first audit_* column read.
            bool Audited = false;
            std::array<float, AUDIT_COUNT> Audit{};
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
            /// The pair drawn from the pool (PairKind stages; its measures are the route_* columns), when one was.
            NavPair Pair;
            bool PairValid = false;
            /// A tower's trip (ArenaDefinition::Tower): up, then back down to the start -- the leg it is on, and the
            /// way back's route yards (in CStart from the draw, so the clock and SPL are the whole trip's).
            bool TwoLeg = false;
            uint32 Leg = 0;
            float BackYards = 0.0f;
            Position Start;
            Position Goal;
            bool GoalAtBed = false;
            float CStart = Vision::TRAV_UNREACHABLE;    // the start's route distance: the optimum
            float Straight = 0.0f;
            uint32 Rung = 0;
            uint32 DetourBin = 0;
            uint32 LengthBin = 0;
            uint32 LengthBins = 0;          // the length bins the tuning lists (the bin table's row length)
            bool BinCounted = false;        // the trip's outcome goes to its bin's arrival rate (DrawPair)
            bool Replayed = false;
            bool TimedOutNoted = false;
            /// A dungeon run's (ArenaDefinition::HasRunGate): the door allowance on the trip's clock when the start is
            /// before the gate.
            float GateSeconds = 0.0f;
            /// The live arena's shut doors this episode (ArenaDefinition::LiveShut) and how each stood at the last
            /// look; whether one has opened (door_opened) and when; summons swept from the doors till SweepUntilMs.
            std::vector<ObjectGuid> ShutDoors;
            std::vector<uint8> ShutOpen;
            std::vector<uint8> ShutPaid;    // each shut door's first opening paid (DoorOpened, once a door)
            uint32 DoorsOpened = 0;
            bool DoorOpened = false;
            uint32 DoorOpenedMs = 0;
            uint32 SweepUntilMs = 0;
            /// The clear way (ArenaDefinition::RunClear): the checkpoint the goal is now (an index into the arena's
            /// way), the route yards of the way after it (added to every route cost, so the potential and the clock
            /// are the whole trip's), and the checkpoints reached and asked this trip.
            bool Clearing = false;
            /// A leg trip (ArenaDefinition::RunLegs): its leg (the checkpoint it goes to), whether its outcome is
            /// still to be counted, and a whole-way trip of a legs arena.
            bool LegTrip = false;
            int32 RunLeg = -1;
            bool LegCounted = false;
            bool FullTrip = false;
            uint32 Checkpoint = 0;
            float RemainingAfter = 0.0f;
            uint32 CheckpointsReached = 0;
            uint32 CheckpointsTotal = 0;
            std::array<SeatNav, GROUP_SEATS> Seats{};
            /// A labels-only arena's goals, seat by seat (Encounter::RouteGoal, or nav6's leader for every seat): the
            /// point and its bake cell; and the cost-to-go of each goal cell in use (Costs, by cell). Seat 0's is also
            /// the env's Goal, GoalCell and Cost (the critic's slot).
            struct SeatGoal
            {
                bool Has = false;
                Position At;
                uint32 Cell = Vision::TRAV_NO_CELL;
            };
            std::array<SeatGoal, GROUP_SEATS> Goals{};
            std::vector<std::pair<uint32, std::vector<float>>> Costs;
        };

        /// A labels-only seat's goal and its cost-to-go (null: none yet); any other seat's is the env's.
        [[nodiscard]] static std::vector<float> const* SeatCost(EnvNav const& nav, uint32 seat, Position& goal);
        /// The labels-only goals asked of the episode's encounters (or the leader nav6 follows), their cost maps
        /// worked out when a goal cell is new and dropped when no seat's goal is on it any more.
        void UpdateGoals(Env& env, EnvNav& nav);

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
        /// A dungeon run's trip (PairKind 6): the goal the arena's RunGoal, the start drawn back along the route from
        /// it by rung -- rung 0 near the goal, rung 1 anywhere past the gate, rung 2 within RUN_GATE_BAND of the gate
        /// on the near side, rung 3 the dungeon's entrance.
        bool DrawRun(Env& env, Player* bot, EnvNav& nav);
        /// The live arena's objects back as a fresh instance has them, LiveOpen's opened, LiveItems in the bags.
        void ResetLive(Env& env, Map* map, Player* bot, EnvNav& nav);
        /// The live arena's shut doors watched (DoorOpened once, on the first opening) and what their scripts summon
        /// swept away (the dungeon is empty).
        void WatchLive(Env& env, Map* map, EnvNav& nav, RewardLedger& ledger);
        /// A walking stage (PairKind sight or detour): its pairs and its true route take no drop -- ramps and stairs
        /// are walking, drops are the vertical stage's (the cheapest route through the Deadmines dropped in 4,621 of
        /// 6,200 pairs).
        [[nodiscard]] bool WalksOnly() const;
        /// The arena's pair pool, built on first use (BuildPairs) under _poolLock; null when it has no pair.
        [[nodiscard]] PairPool const* EnsurePairs(Env const& env, Map* map, Player const* bot,
            Vision::TravGrid const& grid);
        /// Measures the arena's pairs: PAIR_GOALS goals, PAIR_STARTS starts each, on the bake and the map's navmesh.
        void BuildPairs(Env const& env, Map* map, Player const* bot, Vision::TravGrid const& grid,
            PairPool& pool) const;
        /// How far `pair` misses being a trip of the stage's PairKind at rung `rung` (StageDefinition::PairKind), 0
        /// when it fits: the draw takes a fit, else the nearest miss.
        [[nodiscard]] float PairMiss(NavPair const& pair, uint32 kind, uint32 rung, bool tower) const;
        /// The pair draw from the pool: the stage's kind at the rung, uniform over the fits that this body can stand at
        /// (swim depth), else the nearest miss. False when the pool is empty.
        bool DrawFromPool(Env& env, PairPool const& pool, Player const* bot, EnvNav& nav);
        /// A failure of `env`'s arena noted at `where` with the episode's goal, for the replay draw.
        void NoteFailure(Env const& env, EnvNav const& nav, Position const& where);
        /// A training trip's (detour, length) bin among the `detourBins` x `lengthBins` its rung opens, by the
        /// arena's failure weights (PointNav.BinWeighting), into `nav`; `lengthCount` the bins the tuning lists.
        void DrawBin(Env const& env, uint32 detourBins, uint32 lengthBins, uint32 lengthCount, EnvNav& nav);
        /// The trip's outcome (arrived, else timed out) into its bin's arrival rate, once, when the draw counts it.
        void NoteBinOutcome(Env const& env, EnvNav& nav, bool arrived);
        /// The world point of a bake node: its cell's centre at the standing height (the bed when `bed`).
        [[nodiscard]] static Position CellPoint(Vision::TravGrid const& grid, uint32 node, bool bed);
        /// The route distance at a world point, on the floor a body with its feet at z stands on there
        /// (TravGrid::NodeOf): TRAV_UNREACHABLE off the bake.
        [[nodiscard]] static float CostAt(EnvNav const& nav, float x, float y, float z);
        [[nodiscard]] static float CostAt(Vision::TravGrid const& grid, std::vector<float> const& cost, float x,
            float y, float z);
        /// The waypoint regret of a cell goal the seat just chose: C(its block) less the least C over the choosable
        /// blocks of the crop it was chosen from; nothing when it cannot be read.
        void NoteWaypoint(Env const& env, uint32 seatIndex, EnvNav& nav, SeatNav& seat) const;
        /// The map audit of `seat`'s trip into its SeatNav (AuditField), and a timed-out evaluation trip's band dumped.
        void RunAudit(Env const& env, uint32 seat);
        /// Whether a walkable bake node within STRANDED_REACH yards of (x, y), its floor within STRANDED_RISE of the
        /// feet at z, has a route to the goal: dead ground is ground with none (the snap absorbs the 2-yd bake's
        /// reading of stairs, ramps and ledges as slope or a drop).
        [[nodiscard]] bool RouteNear(EnvNav const& nav, float x, float y, float z) const;
        /// The episode's generator (seeded in BeforeLevel: from the evaluation seed in an evaluation).
        [[nodiscard]] std::mt19937& Rng(Env const& env) const;
        [[nodiscard]] float Uniform(Env const& env) const;

        std::vector<EnvNav> _envs;
        mutable std::vector<std::mt19937> _rngs;
        PartyFollowEncounter const* _follow = nullptr;
        ClearWay const& EnsureClearWay(Env const& env, Vision::TravGrid const& grid);
        /// A recorded clear way (PointNav.ClearWayFile: `index,kind,x,y,z,spawn_ids`, from
        /// `python -m animus.human dungeon-run`) as checkpoints, each one's spawn ids and whether it is the gate; the
        /// arena's RunGoal appended when the recording did not end there. False for no file or no rows.
        static bool LoadRecordedWay(std::filesystem::path const& path, ArenaDefinition const& arena,
            std::vector<Position>& points, std::vector<std::vector<uint32>>& spawns, std::vector<bool>& gates,
            std::vector<uint32>& gateSpawns);
        /// A recorded way's line (`path`: `leg,t_ms,x,y,z`) into `way`'s LegPath, LegYards and LegSeconds, for its
        /// `legs` legs; false (and nothing set) for no file, no rows or another leg count.
        static bool LoadRecordedPath(std::filesystem::path const& path, uint32 legs, ClearWay& way);
        /// The key `_legRates`/`_legTries` are kept under: an arena's own legs under its index, another rung's a
        /// thousand apart.
        [[nodiscard]] static uint32 LegKey(uint32 arena, uint32 rung) { return arena + 1000 * rung; }
        std::map<uint32, ClearWay> _clearWays;  // per arena index, under _poolLock
        /// Per arena index, each leg's running success rate and its training trips counted (RunLegs), under _poolLock.
        std::map<uint32, std::vector<float>> _legRates;
        std::map<uint32, std::vector<uint32>> _legTries;
        /// Each leg's teacher share (LegTau), keyed as the rates; a leg not yet here is at Teacher.Max.
        std::map<uint32, std::vector<float>> _legTau;
        /// The drawn starts' (DrawStartAt), keyed as the legs' are.
        std::map<uint32, std::vector<float>> _startRates;
        std::map<uint32, std::vector<uint32>> _startTries;
        /// Every leg of the arena's `legs` learned (LEG_LEARNED after LEG_MIN_TRIES); the share of them learned.
        [[nodiscard]] bool LegsLearned(uint32 arena, uint32 legs, float* share = nullptr);
        /// A training trip's leg, by failure rate (LEG_FLOOR the least weight).
        uint32 DrawLeg(Env const& env, uint32 legs);
        /// A leg trip's outcome into its leg's rate, once (training only).
        void NoteLegOutcome(Env const& env, EnvNav& nav, bool arrived);
        std::mutex _poolLock;
        std::map<uint32, FailurePool> _pools;   // per arena index
        std::map<uint32, PairPool> _pairPools;  // per arena index, under _poolLock
        /// Per arena index, each (detour, length) bin's recent arrival rate over training trips (row-major by detour
        /// bin, LengthBins a row; 0 until a trip of the bin ends), under _poolLock.
        std::map<uint32, std::vector<float>> _binRates;
    };
}

#endif
