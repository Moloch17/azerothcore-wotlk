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
            AUDIT_CLOSED,           // ... the configuration space closed (a 1-yd hole between walls)
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
            AUDIT_COUNT
        };
        static constexpr char const* AUDIT_NAMES[AUDIT_COUNT] = { "audit_unreachable", "audit_true_yards",
            "audit_corridor_cost", "audit_truncated", "audit_cells", "audit_wall", "audit_closed", "audit_unknown",
            "audit_hazard", "audit_floor", "audit_first_block_yards", "audit_wall_ray", "audit_wall_mesh",
            "audit_wall_bump", "audit_wall_inherited", "audit_wall_true", "audit_wall_face", "audit_wall_step",
            "audit_wall_phantom", "audit_wall_floorless", "audit_wall_layer_off" };
        /// The band's square, 1-yd cells a side, at most: a longer true route is audited from the body this far.
        static constexpr int32 AUDIT_SIDE_MAX = 256;
        /// Where a timed-out evaluation trip's band is written (`<seed>-seat<seat>.audit`); empty for nowhere. Set by
        /// the forge as each evaluation's videos start (their directory's audit/).
        static void SetAuditDir(std::string dir);

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
            float MapRouteStart = Vision::TRAV_UNREACHABLE;
            float MapRouteLast = Vision::TRAV_UNREACHABLE;
            // The route's source over the labelled decisions (CropPose::RouteGlobal: the global route, else the
            // window plan) and the global route's recomputes past each trip's first plan, contradictions among them
            // (the route_source_global_share, global_replans and global_replans_contradiction columns).
            uint32 RouteDecisions = 0;
            uint32 GlobalDecisions = 0;
            uint32 GlobalReplans = 0;
            uint32 GlobalContradictions = 0;
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
        /// A training trip's (detour, length) bin among the `detourBins` x `lengthBins` its rung opens, by the
        /// arena's failure weights (PointNav.BinWeighting), into `nav`; `lengthCount` the bins the tuning lists.
        void DrawBin(Env const& env, uint32 detourBins, uint32 lengthBins, uint32 lengthCount, EnvNav& nav);
        /// The trip's outcome (arrived, else timed out) into its bin's arrival rate, once, when the draw counts it.
        void NoteBinOutcome(Env const& env, EnvNav& nav, bool arrived);
        /// The world point of a bake cell: its centre at the standing height (the bed when `bed`).
        [[nodiscard]] static Position CellPoint(Vision::TravGrid const& grid, uint32 cell, bool bed);
        /// The route distance at a world point: TRAV_UNREACHABLE off the bake.
        [[nodiscard]] static float CostAt(EnvNav const& nav, float x, float y);
        /// The waypoint regret of a cell goal the seat just chose: C(its block) less the least C over the choosable
        /// blocks of the crop it was chosen from; nothing when it cannot be read.
        void NoteWaypoint(Env const& env, uint32 seatIndex, EnvNav& nav, SeatNav& seat) const;
        /// The map audit of `seat`'s trip into its SeatNav (AuditField), and a timed-out evaluation trip's band dumped.
        void RunAudit(Env const& env, uint32 seat);
        /// The episode's generator (seeded in BeforeLevel: from the evaluation seed in an evaluation).
        [[nodiscard]] std::mt19937& Rng(Env const& env) const;
        [[nodiscard]] float Uniform(Env const& env) const;

        std::vector<EnvNav> _envs;
        mutable std::vector<std::mt19937> _rngs;
        PartyFollowEncounter const* _follow = nullptr;
        std::mutex _poolLock;
        std::map<uint32, FailurePool> _pools;   // per arena index
        /// Per arena index, each (detour, length) bin's recent arrival rate over training trips (row-major by detour
        /// bin, LengthBins a row; 0 until a trip of the bin ends), under _poolLock.
        std::map<uint32, std::vector<float>> _binRates;
    };
}

#endif
