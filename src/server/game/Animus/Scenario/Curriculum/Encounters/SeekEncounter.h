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
#include "Encounter.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SeenPlaces.h"
#include <array>
#include <unordered_set>
#include <vector>

namespace Animus::Curriculum
{
    struct ArenaDefinition;

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
        };

        /// Put the episode's object in `room` of `arena`: a spot on its floor, clear of walls, else its centre; at
        /// the doorway rung, just inside its opening (else on its floor).
        bool Place(Env const& env, EnvSeek& seek, Map* map, ArenaDefinition const& arena, uint32 phase) const;
        /// ... at the hallway rung: a hallway point in sight of a seat at `start` (SightDraw::Place).
        bool PlaceInHallway(EnvSeek& seek, Map* map, ArenaDefinition const& arena, Player* bot,
            Position const& start) const;
        /// Stand the object of `kind` at `spot` (its base), turned `facing`.
        bool Summon(EnvSeek& seek, Map* map, ArenaDefinition const& arena, Position const& spot, uint32 phase) const;

        /// The room goals' bookkeeping for one decision, after the frame's floor hits were added (`counts`: the rays on
        /// each room): the glimpses, the checks, the visits and returns (charged to `ledger`), and the slots. `room` is
        /// the room the seat stands in (-1 for none).
        void TrackRooms(Env const& env, EnvSeek& seek, ArenaDefinition const& arena, Player* bot, int32 room,
            std::vector<uint32> const& counts, RewardLedger& ledger);

        std::vector<EnvSeek> _envs;
    };
}

#endif
