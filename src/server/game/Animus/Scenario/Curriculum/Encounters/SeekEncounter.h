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

#ifndef ANIMUS_LIB_CURRICULUM_SEEK_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_SEEK_ENCOUNTER_H

#include "Block.h"
#include "Coverage.h"
#include "Encounter.h"
#include "ObjectGuid.h"
#include "PlayerController.h"
#include "Position.h"
#include "SeenPlaces.h"
#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Animus::Curriculum
{
    struct ArenaDefinition;
    struct SeatState;
    struct StageDefinition;

    /// **M2 seek** (Opposition::Seek; perception-goals plan §4, "M2 in detail"): one real object hidden in one of a
    /// dungeon's rooms, found by sight and stopped beside. The seat has no compass: the object is shown only by the
    /// camera's objective flag, on the pixels whose rays pass by it before they hit anything (line of sight), standing
    /// in for a quest object's glow. Its memory is the GRU's and its mental map's (the map block).
    ///
    /// **The ladder** (perception-goals REDESIGN §2; SeekDraw::Rung): each reset places by the shaping fade's rung --
    /// in the hallway in sight of the spawn (SightDraw::Place over the arena's hallway points), just inside a front
    /// cell's opening, anywhere in a front cell, deep (the back rooms, hubs and end rooms) -- or, Seek.CarryShare of
    /// the time, the rung below; its episode lasts Seek.RungSeconds of the placement's rung. The seat stands at a
    /// random hallway point (the arena's spawn points, drawn by the scenario), facing a random way. The object
    /// (uniformly of the pool) is a real gameobject whose display has a collision model, so the camera's rays hit it,
    /// turned at random; the objective point the flag marks is its centre. The object of the episode before is
    /// removed first, and so are the map's own game objects (ObjectPool), so the one object in the dungeon is the one
    /// to find.
    ///
    /// An evaluation plays at the training rung (amendment 9): seed i places in the rung's i-th room in turn
    /// (SeekDraw::EvaluationRooms) with the object types cycled, so its found rate is the rung's, which the fade's
    /// gate reads. The held-out arena (EvalOnly, the stage's "sweep") is every (room, object) pair once a pass
    /// (SeekDraw::EvaluationPick: 195 episodes) at the top rung, 300 s.
    ///
    /// **Room goals** (M2 goals plan, 2026-10-09; Seek.Goals): the goal head is offered the rooms the seat's own frames
    /// showed -- a first frame with Seek.GlimpseRays floor rays on a room's polygon, bound to one of six slots in the
    /// order of the glimpses and held until the room is checked -- and the nearest frontier of its mental map as the
    /// way on (WorldView::Places, GoalBlock). A room's place is the mean of the floor points its rays hit this
    /// episode, never the room table's centre or opening. The room is checked when the seat stood in it for
    /// Seek.EnterDwellMs or Seek.CheckedShare of its floor cells were hit; the slot is Done for one observation (the
    /// goal reached), then released and rebound to the next waiting room. The table of rooms only says which room a
    /// ray or a position belongs to: it is reward and bookkeeping geometry, never observed.
    ///
    /// **Explore, don't circle, get unstuck** (2026-10-10, decision 0023): Explore pays each 2-yd cell of floor a ray
    /// of the seat's camera lands on for the first time this episode (the room bonus in a room not yet entered; an
    /// episode-local record beside the mental map, which outlives the episode) and FrontierPull each yard closed on the
    /// nearest frontier of the seat's own map (a best-distance ratchet per frontier cluster); both are paid at
    /// max(the shaping scale, Seek.ExploreFloor) and capped. Circling (a Cost at a fixed price) charges going round in
    /// circles. The trap drill starts Seek.TrapShare of the training episodes with the seat against the jamb of a
    /// door of the room table, facing it, and pays Escape (Aid) once for getting 6 yd away: a start distribution, no
    /// scripted action.
    ///
    /// **Exploration v2** (decision 0025): ExploreCap 5.0; a cell first seen from inside its own room is worth
    /// Seek.ExploreInsideBonus (going in) where the corridor-seen cell of an unentered room keeps ExploreRoomBonus
    /// (looking in); RoomEntry pays each room's first entry once (back rooms times RoomEntryBackMult) in its own cap;
    /// FrontierPull pulls only toward a frontier in a room or at a door opening, not in the corridor; Stale (a Cost at
    /// a fixed price) charges every second after StaleRoomMs with no new room entered (v3). Both sweeps play the deep
    /// rung's clock; the evaluation records found_300.
    ///
    /// **Exploration v3** (decision 0026): the Exploring family, Stale and the trap drill apply from the placed rung
    /// Seek.ExploreFromRung up; the unseeded room draw weighs SeekDraw::HARD_ROOMS by Seek.HardRoomWeight; the trap
    /// pose is tighter and its Escape needs a turn of Seek.TrapEscapeTurnDeg too; the Wall charge grows with the
    /// contiguous pin (Seek.WallEscalateSeconds, WallEscalateMax).
    ///
    /// **General search** (decision 0027): the stage trains on the Stockades and Ragefire Chasm (an env's episode
    /// draws its arena, hence its map) and is measured on the Deadmines, held out. The map-derived terms read the
    /// seat's own map alone (the coverage analysis of this decision's crop, Coverage::Summary): FrontierClear pays a
    /// frontier cluster approached and resolved by positive evidence, PocketEntry a chamber behind a narrowing
    /// entered for the first time (seen as a separate one before), in FrontierPull's and RoomEntry's caps; Stale's
    /// clock restarts on those events on a table-free arena; Explore's bonuses are the pockets' there, and
    /// FrontierPull aims at the nearest cluster at a pocket. The table terms (RoomEntry, the room-based Stale clock,
    /// the corridor filter, the table's Explore bonuses) pay only on an arena with TableTerms. Revisit charges ground
    /// stood on again. A seeded, unpinned episode goes round the trainable arenas (StageScenario::DrawArena) and
    /// cycles the rooms with the seed divided by their count.
    ///
    /// **Movement pacing** (decision 0027's amendments): the trap drill has its own rung (Seek.TrapFromRung), more
    /// poses (an inside corner, a pillar's edge) and, Seek.TrapReplayShare of the time, a pose where a training seat
    /// pinned (recorded with its held keys into a per-stage ring buffer, never a script); Circling's turn clause holds
    /// only while a movement key is held; Recovered (Aid) pays back a share of a pin's Stuck and Wall charges once the
    /// seat is away from it.
    ///
    /// Paid: Arrive once, stopped within the arena's SeekRadius of the object (Outcome); StepCost and Death (Cost);
    /// Stuck and Wall (Cost, at their own fixed price from the first step: RewardLedger::AddFixed); Sighting,
    /// NewGround and RoomSeen, the training-only aids (Shaping, faded), each by the episode's own bookkeeping, never
    /// the remembered map's. Measured: found (overall and by rung), the time to the first frame with the flag, from
    /// there to the arrival, the rooms looked into, entered before finding it and re-entered, by room and by object
    /// (stage.json episode_categories).
    class SeekEncounter final : public Encounter
    {
    public:
        SeekEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        [[nodiscard]] int32 AchievedGoal(Env const& env, uint32 seat) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

        /// The rooms' and objects' names of the stage's seek arena, in the order the episode info columns seek_room and
        /// seek_object index them (stage.json episode_categories).
        [[nodiscard]] static std::vector<std::string> RoomNames(ArenaDefinition const& arena);
        [[nodiscard]] static std::vector<std::string> ObjectNames(ArenaDefinition const& arena);
        /// Each arena's offset into the union of the stage's seek room tables (seek_room = offset + room): the distinct
        /// tables in arena order, told apart by their first room's name, each listed once; arenas sharing a table
        /// share its offset (the Stockades' keep 0). Empty for a stage with no seek arena; 0 for an arena that is not
        /// one.
        [[nodiscard]] static std::vector<uint32> RoomOffsets(StageDefinition const& stage);

    private:
        /// What the episode knows of one room (room goals): the floor cells its rays hit, the centroid of the hit
        /// points, the glimpse, the check and the visits. Kept for every room from the episode's start.
        struct RoomTrack
        {
            std::unordered_set<uint64> Hit;     // cells (SeekDraw::CoverCell) of its floor hit by this episode's rays
            uint32 Floor = 1;                   // the cells its polygon covers
            double SumX = 0.0;                  // the hit points, summed: the place the room has in the goal block
            double SumY = 0.0;
            double SumZ = 0.0;
            uint32 Hits = 0;
            bool Glimpsed = false;
            uint32 GlimpseMs = 0;
            uint32 Order = 0;                   // the glimpse's rank this episode
            int32 Slot = -1;                    // the place slot it is bound to
            bool Checked = false;
            uint32 CheckedMs = 0;
            bool Visited = false;               // stood in it for Seek.EnterDwellMs
            uint32 DwellMs = 0;
            uint32 AwayMs = 0;                  // since leaving a visited room: far enough outside it, for how long
            bool Armed = false;                 // ... for long enough: entering it again is a return
            bool Approached = false;            // came within APPROACH_YARDS of its opening (a reading only)
        };

        /// One decision of the circling window: the clock, where the seat stood and faced, and the ground and the turn
        /// since the decision before.
        struct CircleSample
        {
            uint32 Ms = 0;
            float X = 0.0f;
            float Y = 0.0f;
            float Yaw = 0.0f;
            float Step = 0.0f;
            float Turn = 0.0f;
        };

        struct EnvSeek
        {
            bool Placed = false;
            ObjectGuid Object;
            Position Spot;                  // the object's base, where it stands
            Position Centre;                // the objective point: its centre, which the flag marks
            int32 Room = -1;
            uint32 ObjectIndex = 0;
            uint32 Rung = 0;                // the placement's (SeekDraw::Rung)
            uint32 LadderRung = 0;          // the ladder's, before the carry-over
            bool Carried = false;           // placed at the rung below the ladder's
            bool Sweep = false;             // the held-out sweep: every (room, object) pair at the top rung
            std::vector<bool> Looked;       // rooms whose floor a frame showed this episode (RoomSeen)
            uint32 RoomsLooked = 0;
            float Depth = 0.0f;
            uint32 Tier = 0;
            bool Fallback = false;          // placed at the room's centre: no drawn spot passed
            bool Found = false;
            uint32 FoundMs = 0;
            bool SightPaid = false;
            uint32 SightMs = 0;
            uint32 VisibleDecisions = 0;
            uint32 Decisions = 0;
            int32 LastRoom = -1;
            std::vector<bool> Entered;
            uint32 RoomsEntered = 0;
            uint32 RoomsReentered = 0;
            uint32 RoomsBeforeFound = 0;
            std::unordered_set<uint64> Cells;
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            float Travelled = 0.0f;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
            float LastZ = 0.0f;
            // Room goals: offered this episode, the rooms, the slots (a room each, -1 free), and the tallies.
            bool RoomGoals = false;
            std::vector<RoomTrack> Track;
            std::array<int32, GOAL_ROOM_SLOTS> SlotRoom{ -1, -1, -1, -1, -1, -1 };
            uint32 Glimpses = 0;
            int32 AchievedSlot = -1;            // the slot of a room checked at this decision
            uint32 RoomsChecked = 0;
            uint32 Returns = 0;
            uint32 MaxWaiting = 0;
            bool FirstGoal = false;
            uint32 FirstGoalMs = 0;
            bool ObjectChecked = false;         // the object's room was checked, and when
            uint32 ObjectCheckedMs = 0;
            float CoverAtFind = 0.0f;
            // The way on: the nearest frontier of the seat's map, refreshed every few seconds (View).
            mutable std::vector<SeenPlaces::Point> Frontier;
            mutable uint32 FrontierMs = 0;
            mutable bool FrontierReady = false;

            // Explore: the 2-yd cells of floor a ray landed on this episode, what was paid (nominal, before the
            // scale) and the tallies.
            std::unordered_set<uint64> Seen;
            float ExploreNominal = 0.0f;
            uint32 ExploreCells = 0;
            uint32 ExploreRoomCells = 0;
            uint32 BackRoomVisits = 0;
            // Exploration v2 (decision 0025): the cells first seen from inside their own room (the Seek.ExploreInsideBonus
            // ones), the room entries (EnterDwellMs inside the polygon, the start room excepted) and what they paid
            // (nominal), the back and end-back ones among them, and when the Explore cap was reached (-1: not).
            uint32 ExploreInsideCells = 0;
            uint32 RoomEntriesNew = 0;
            uint32 BackRoomEntries = 0;
            float RoomEntryNominal = 0.0f;
            int32 StartRoom = -1;
            int32 ExploreCapHitMs = -1;
            // Stale (v3): the clock of the last new room of the table visited (the start room's first dwell counts),
            // the seconds charged and the runs begun; the rooms whose opening the seat came near (a reading).
            uint32 LastNewRoomMs = 0;
            uint32 RoomsApproached = 0;
            bool HardRoom = false;              // the drawn room is one of SeekDraw::HARD_ROOMS
            bool StaleOn = false;
            float StaleSeconds = 0.0f;
            uint32 StaleEvents = 0;
            // Frontier pull: the nearest frontier cluster (refreshed every few seconds) and the best distance reached
            // to each cluster (a 10-yd grid cell of its point), so that going back and forth pays nothing.
            bool HasFrontier = false;
            SeenPlaces::Point FrontierAt;
            uint64 FrontierKey = 0;
            uint32 FrontierCheckedMs = 0;
            bool FrontierChecked = false;
            std::unordered_map<uint64, float> FrontierBest;
            float FrontierNominal = 0.0f;
            // Circling: the window of decisions, whether it held at the last one, and the tallies.
            std::vector<CircleSample> Circle;
            std::size_t CircleHead = 0;
            bool CircleOn = false;
            float CirclingSeconds = 0.0f;
            uint32 CirclingEvents = 0;
            // The trap drill: whether the episode started in a pose, where, and the way out.
            bool Trap = false;
            Position TrapStart;
            bool Escaped = false;
            uint32 EscapeMs = 0;
            float TrapMaxTurn = 0.0f;           // the most the body's yaw differed from the pose's inside the window
            float TrapPinSeconds = 0.0f;        // Wall-charged seconds inside the window
            // The escalating Wall charge (v3): the run of contiguous Wall-charged milliseconds, the longest run, the
            // runs of 2 s or more that ended, and the nominal surplus the escalation charged.
            uint32 WallRunMs = 0;
            uint32 WallPinMax = 0;
            uint32 WallPinEvents = 0;
            float WallExtra = 0.0f;

            // **General search** (decision 0027). FrontierClear: each cluster key's largest size seen, the keys
            // cleared, and the nominal paid. PocketEntry: when each chamber key was last a pocket (and where),
            // how long the seat has dwelt in each own chamber, the pockets entered, the chamber it started in, and
            // the nominal paid; the decisions it stood in a chamber. Stale's table-free clock (LastNewMs: the last
            // room on a table arena, else the last entry or clear). The pocket cells Explore paid (the 2x ones).
            struct Place
            {
                uint32 Ms = 0;              // the stamp (PocketSeenMs), or the size peak (ClusterPeak)
                float X = 0.0f;             // the bin's centroid, for the 10-yd neighbourhood tests
                float Y = 0.0f;
            };
            std::unordered_map<uint64, Place> ClusterPeak;
            std::unordered_map<uint64, Place> ClustersCleared;
            float FrontierClearNominal = 0.0f;
            std::unordered_map<uint64, Place> PocketSeenMs;
            uint64 OwnKey = 0;              // the own chamber's key at the last decision (0 none)
            uint32 OwnDwellMs = 0;          // how long it has been the own one
            std::unordered_map<uint64, Place> PocketsEntered;
            uint64 StartChamber = 0;
            float StartChamberX = 0.0f;
            float StartChamberY = 0.0f;
            float PocketEntryNominal = 0.0f;
            uint32 ChamberDecisions = 0;
            uint32 LastNewMs = 0;
            uint32 ExplorePocketCells = 0;
            // Revisit: when the body last stood on each 2-yd cell (episode-local), the weighed seconds charged and
            // the decisions on stood ground.
            std::unordered_map<uint64, uint32> StoodMs;
            float RevisitSeconds = 0.0f;
            uint32 RevisitDecisions = 0;
            // **Movement pacing**: the pin the rebate watches (a run of Stuck- or Wall-charged decisions: its
            // charge, its length, where it began, when it ended), the rebates paid and their nominal sum; the trap
            // pose's source (0 geometric, 1 replayed) and whether this episode's pin onset was recorded.
            float PinCharge = 0.0f;
            uint32 PinRunMs = 0;
            bool PinOn = false;
            bool PinWatch = false;          // a run of RECOVER_MIN_MS or more ended or runs: an escape is watched for
            Position PinPoint;
            uint32 PinWatchMs = 0;          // when the watch began (the run's start)
            uint32 Recoveries = 0;
            float RecoveredNominal = 0.0f;
            uint32 TrapSource = 0;
            bool PinSpent = false;          // the run's rebate was paid or its window passed: nothing more this run
            uint32 LastStuckRunMs = 0;      // the controller's Stuck run at the last decision (the record's edge)
        };

        /// **A pin pose** (movement pacing M3): where a training seat's Stuck run reached two seconds, with the keys it
        /// held -- the per-stage replay table the trap drill draws from (TrapReplayShare), a ring of PIN_POSES.
        struct PinPose
        {
            Position Pose;
            Movement::ControlState Held;
        };
        static constexpr std::size_t PIN_POSES = 512;

        /// Put the episode's object in `room` of `arena`: a spot on its floor, clear of walls, else its centre; at
        /// the doorway rung, just inside its opening (else on its floor).
        bool Place(Env const& env, EnvSeek& seek, Map* map, ArenaDefinition const& arena, uint32 phase) const;
        /// ... at the hallway rung: a hallway point in sight of a seat at `start` (SightDraw::Place).
        bool PlaceInHallway(EnvSeek& seek, Map* map, ArenaDefinition const& arena, Player* bot,
            Position const& start) const;
        /// Stand the object of `kind` at `spot` (its base), turned `facing`.
        bool Summon(EnvSeek& seek, Map* map, ArenaDefinition const& arena, Position const& spot, uint32 phase) const;

        /// Explore for one decision: the cells this frame's floor rays landed on for the first time (paid unless `paid`
        /// is false: the first look is the spawn's view, not a search) and, in the same pass, the nearest frontier's
        /// closing distance (FrontierPull).
        void Explore(Env const& env, EnvSeek& seek, ArenaDefinition const& arena, SeatState const& seat, Player* bot,
            bool paid, RewardLedger& ledger);
        /// Circling for one decision: the window's path, turning and net displacement; charged unless `stuck` (the
        /// decision already paid Stuck).
        void Circle(Env const& env, EnvSeek& seek, Player* bot, float moved, bool stuck, bool keys,
            RewardLedger& ledger);
        /// Stale for one decision: the cost per second once Seek.StaleRoomMs have passed with no new room of the table
        /// entered (a table arena) or no pocket entered nor cluster cleared (table-free), unless the decision already
        /// paid Stuck or Wall (`paid`).
        void Stale(Env const& env, EnvSeek& seek, bool paid, RewardLedger& ledger);
        /// The trap drill's pose: a point Seek.TrapGapNear-Far yd from the jamb of a random door of the arena's room
        /// table, facing it, on the room's floor with the way to the opening clear; or an inside corner (two walls
        /// within a few yards at right angles, facing the vertex) or a pillar's edge (a solid ahead that the slides
        /// either side clear) on a random room's floor. False when no pose passed.
        bool TrapPose(Map* map, ArenaDefinition const& arena, Player* bot, Position& pose) const;
        /// ... one of the recorded pin poses (PinPose), with its held keys; false while the table is empty.
        bool ReplayPose(Position& pose, Movement::ControlState& held);
        /// Record a pin onset into the replay table.
        void RecordPin(Position const& pose, Movement::ControlState const& held);

        /// FrontierClear for one decision: the clusters of this decision's summary against the episode's record (the
        /// peaks, the keys cleared by positive evidence), paid unless `paid` is false.
        void Clusters(Env const& env, EnvSeek& seek, SeatState const& seat, bool paid, bool tableTerms,
            RewardLedger& ledger);
        /// PocketEntry for one decision: the pockets of the summary stamped, the own chamber's dwell, an entry paid.
        void Pockets(Env const& env, EnvSeek& seek, SeatState const& seat, bool paid, bool tableTerms,
            RewardLedger& ledger);
        /// Revisit for one decision: the body's 2-yd cell against when it last stood there, charged unless `paid`.
        void Revisit(Env const& env, EnvSeek& seek, Player* bot, bool paid, RewardLedger& ledger);
        /// The pin rebate's bookkeeping for one decision: `charge` what Stuck and Wall took this decision.
        void Recover(Env const& env, EnvSeek& seek, Player* bot, float charge, RewardLedger& ledger);

        /// The room goals' bookkeeping for one decision, after the frame's floor hits were added (`counts`: the rays on
        /// each room): the glimpses, the checks, the visits and returns (charged to `ledger`), and the slots. `room` is
        /// the room the seat stands in (-1 for none).
        void TrackRooms(Env const& env, EnvSeek& seek, ArenaDefinition const& arena, Player* bot, int32 room,
            std::vector<uint32> const& counts, bool tableTerms, RewardLedger& ledger);
        /// Whether `arena` pays the table terms: its flag and Seek.TableTerms.
        [[nodiscard]] bool TableTerms(ArenaDefinition const& arena) const;

        std::vector<EnvSeek> _envs;
        // The replay table, written by Reward on the map threads and read by Build on the world thread.
        std::mutex _pinMutex;
        std::array<PinPose, PIN_POSES> _pins{};
        std::size_t _pinCount = 0;
        std::size_t _pinNext = 0;
    };
}

#endif
