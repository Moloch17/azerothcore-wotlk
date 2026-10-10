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

#ifndef ANIMUS_LIB_GOAL_BLOCK_H
#define ANIMUS_LIB_GOAL_BLOCK_H

#include "Block.h"
#include "CellGrid.h"
#include "Position.h"
#include <array>

namespace Animus::Curriculum
{
    struct SeatView;

    /// A Protect goal's friend is out of danger above this share of health.
    constexpr float PROTECT_REACHED_PCT = 70.0f;

    /// **What a goal can be about, right now** (Component C). No actions: the learner reads these columns to mask
    /// its goal head -- a kind is offered only when there is something for it (an enemy for Fight, a place for
    /// TravelTo), a target only when it is there -- and to choose again at once when the goal it held has just been
    /// reached or has become impossible (OBS_ENDED). Always the last block of a layout, so the learner finds it at
    /// the end of the observation.
    ///
    /// Revision 1 (2026-10-08): the Loot, Gather and Interact kinds and the journal objective, giver and turn-in
    /// targets left the goal space (nothing ever offered them; no looting, decision 0003), so every column from
    /// OBS_TARGET_FIRST on moved, and the columns are named (DescribeColumns) so the next revision carries them.
    /// Revision 2 (2026-10-08): the order columns (from_order, the order's kind and target one-hots), written as zero
    /// since the director was deleted, left the block together with the learner's GoalHead reads of them
    /// (mappo/networks.py); the achieved columns moved up by 1 + GOAL_COUNT + GOAL_TARGETS. Still the unreleased
    /// layout generation (protocol 26): bootstrap seeds by column name.
    /// Revision 3 (2026-10-09, M2 goals): 40 columns appended (the columns above do not move): the held goal's place as
    /// M1's compass shows its mark (OBS_HELD_*), and per place slot (six rooms and the way on, the place targets 0..6)
    /// the bearing, distance, coverage and glimpse age of the place the slot holds (OBS_PLACE_FIRST). Written only in
    /// an episode that offers room goals (WorldView::RoomGoals); zero everywhere else.
    /// Revision 4 (2026-10-09, free choice goals): 14 columns appended (the columns above do not move): the secondary
    /// hold's latched point (OBS_HELD2_*), the plan's next step (OBS_NEXT_*), the share of the plan left
    /// (OBS_PLAN_LEFT) and where the seat stands in the frame of its latest choice (OBS_FROM_*). The held goal's place
    /// (OBS_HELD_*) is also the latched point of a cell goal (GOAL_CELL_JOINT). Written only in an episode that offers
    /// cell goals (WorldView::CellGoals); zero everywhere else.
    /// Revision 5 (2026-10-10, search-kind): the goal space has a tenth kind, Search (looking for an object in or at a
    /// place), appended to the kinds (ids of the others do not move): the kind one-hots grow by one, so every column
    /// from OBS_TARGET_FIRST on moved up by one and the achieved-kind one-hots by one more. The columns keep their
    /// names (DescribeColumns), which the learner's seeding carries them by. Width 124 (was 122).
    class GoalBlock final : public Block
    {
    public:
        /// Features per place slot: bearing sin and cos, distance, coverage, age.
        static constexpr uint32 PLACE_FEATURES = 5;

        enum Obs : uint32
        {
            OBS_KIND_FIRST              = 0,                            // per SeatGoal: something for it is there
            OBS_TARGET_FIRST            = OBS_KIND_FIRST + GOAL_COUNT,  // per GoalTarget: it is there
            OBS_ENDED                   = OBS_TARGET_FIRST + GOAL_TARGETS,  // the goal held was reached or lost
            OBS_REACHED,                                                // ... reached (what the learner predicts)
            // The next-run format: the columns above keep their places.
            OBS_SECONDARY_ENDED,                                        // the secondary ended: both sides drop it
            OBS_EVENT,                                                  // choose again now (SeatView::GoalEvent)
            // What was achieved this decision, whatever was pursued (hindsight):
            OBS_ACHIEVED_KIND_FIRST,
            OBS_ACHIEVED_TARGET_FIRST   = OBS_ACHIEVED_KIND_FIRST + GOAL_COUNT,
            // Revision 3: the held primary goal's place (present, bearing sin and cos off the facing, distance over
            // OBJECTIVE_SCALE, distance over NEAR_SCALE), as CompassBlock's columns.
            OBS_HELD_FIRST              = OBS_ACHIEVED_TARGET_FIRST + GOAL_TARGETS,
            OBS_HELD_PRESENT            = OBS_HELD_FIRST,
            OBS_HELD_SIN,
            OBS_HELD_COS,
            OBS_HELD_DIST,
            OBS_HELD_NEAR,
            // ... and per place slot k (PLACE_SLOTS of them, the place targets 0..PLACE_SLOTS-1), PLACE_FEATURES
            // columns: bearing sin and cos off the facing, distance over OBJECTIVE_SCALE, coverage, age.
            OBS_PLACE_FIRST,
            // Revision 4: the secondary hold's latched point, encoded as OBS_HELD_*; the plan's next step (the queue's
            // first cell goal), the same five; the number of cell goals left in the plan over three (the held one
            // and the two queued); and whether the seat stands inside the crop of its latest choice and, if it does,
            // its pooled block's row and column there, over the grid and centred ((index + 0.5) / GRID).
            OBS_HELD2_FIRST             = OBS_PLACE_FIRST + GOAL_PLACE_SLOTS * PLACE_FEATURES,
            OBS_NEXT_FIRST              = OBS_HELD2_FIRST + 5,
            OBS_PLAN_LEFT               = OBS_NEXT_FIRST + 5,
            OBS_FROM_PRESENT,
            OBS_FROM_ROW,
            OBS_FROM_COL,
            OBS_COUNT
        };

        /// The place slots (six rooms and the way on); PLACE_FEATURES columns each.
        static constexpr uint32 PLACE_SLOTS = GOAL_PLACE_SLOTS;
        static constexpr float OBJECTIVE_SCALE = 500.0f;        // CompassBlock's, so a bearing means the same
        static constexpr float NEAR_SCALE = 40.0f;
        static constexpr float AGE_SCALE_S = 120.0f;

        /// 1: Loot, Gather, Interact and the journal targets left the goal space. 2: the order columns left the block.
        /// 3: the held-goal and place-slot columns (see the class comment).
        /// 4: the secondary hold, the plan's next step, the plan left and the choice frame (cell goals).
        /// 5: the search kind (one more kind column and achieved-kind column; the columns after the kinds moved).
        [[nodiscard]] uint32 Revision() const override { return 5; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        /// Revision 3's columns: the held goal's place and the place slots (WorldView::RoomGoals only); revision 4's:
        /// the cell goals' held points, next step, plan left and choice frame (WorldView::CellGoals only).
        static void ObservePlaces(SeatView const& view, float* obs);

        /// Which targets are there, and which kinds have something to be about (at least Fight, always).
        static void Available(SeatView const& view, std::array<bool, GOAL_COUNT>& kinds,
            std::array<bool, GOAL_TARGETS>& targets);

        /// Whether `goal` is reached, read off the world as it is now (the enemy it named is dead or held, the seat
        /// or the friend is healthy again, the place is reached), and whether it is still possible (its kind and target
        /// are on offer). The
        /// forge and the module both end a goal on these, so the learner re-chooses at the same moments in both.
        /// `cell` is the point a cell goal (GOAL_CELL_JOINT) holds, where the scenario keeps one (SeatView::HeldCell of
        /// the hold's slot): a cell goal is possible while its point was latched, and reached within
        /// WorldView::CellReach of it. Other goals ignore it.
        static void Status(SeatView const& view, int32 goal, bool& reached, bool& possible,
            CellPoint const* cell = nullptr);

        /// Whether a goal Status calls reached was *reached* -- made true -- rather than true already when it was
        /// chosen (Fight about no one with nothing to fight, Recover at full health). One true on choice is held,
        /// neither paid nor ended, until it stops being true; reached after that, it counts. `fresh` is set by the
        /// caller when a new goal is chosen and cleared here; `satisfiedAtChoice` is the caller's to keep per goal.
        /// The forge pays Goals.Reached and ends goals on this, and the module ends them on it, so both agree.
        static bool Earned(bool reached, bool& fresh, bool& satisfiedAtChoice);

        /// Where a place target is (a route place, or the assigned area -- the trip's objective in a stage without a
        /// route); false for a target that is not a place, or not there.
        /// A cell goal's place is the point it latched: `slot` is the hold's (0 primary, 1 secondary).
        static bool PlaceOf(SeatView const& view, uint32 target, Position& where, uint32 slot = 0);

        /// Whether a seat is told where a trip's objective is (SeatView::ObjectivePlaceKnown): its stage carries the
        /// compass and this episode shows it.
        [[nodiscard]] static constexpr bool ObjectivePlaceKnown(bool compassBlock, bool compassWithheld)
        {
            return compassBlock && !compassWithheld;
        }

        /// Whether a target is a room slot of an episode with room goals (place_0..place_5; place_6 is the way on,
        /// which is reached by distance as any place).
        [[nodiscard]] static constexpr bool IsRoomTarget(uint32 target)
        {
            return target >= GOAL_TARGET_PLACE_FIRST && target < GOAL_TARGET_PLACE_FIRST + GOAL_ROOM_SLOTS;
        }

        /// Whether a kind is one about a place: TravelTo, and Search, which the seek stage's room and cell goals are
        /// (Seek.SearchGoals; travel_to when it is 0).
        [[nodiscard]] static constexpr bool IsPlaceKind(int32 kind)
        {
            return kind == int32(SeatGoal::TravelTo) || kind == int32(SeatGoal::Search);
        }

        /// Whether a goal is a place kind about a room slot (IsRoomGoal) or about a room slot or the way on
        /// (IsPlaceGoal): what the seek stage's room goals count and price.
        [[nodiscard]] static constexpr bool IsRoomGoal(int32 goal)
        {
            return goal >= 0 && IsPlaceKind(GoalKindOf(goal)) && IsRoomTarget(GoalTargetOf(goal));
        }
        [[nodiscard]] static constexpr bool IsPlaceGoal(int32 goal)
        {
            return goal >= 0 && IsPlaceKind(GoalKindOf(goal))
                && GoalTargetOf(goal) >= GOAL_TARGET_PLACE_FIRST
                && GoalTargetOf(goal) < GOAL_TARGET_PLACE_FIRST + GOAL_PLACE_SLOTS;
        }

        /// Whether a goal is the cell goal (a place kind about GOAL_CELL_TARGET), and whether it is a plan's goal of either kind: a place
        /// (IsPlaceGoal) or a cell. What the seek stage's plan counts (the share of decisions under one, the first).
        /// A cell goal is the cell target of either place kind: search (GOAL_CELL_JOINT, Seek.SearchGoals 1) or, at
        /// SearchGoals 0, travel_to -- the joint the manifest publishes as goals.cells.joint.
        [[nodiscard]] static constexpr bool IsCellGoal(int32 goal)
        {
            return goal >= 0 && IsPlaceKind(GoalKindOf(goal)) && GoalTargetOf(goal) == GOAL_CELL_TARGET;
        }
        [[nodiscard]] static constexpr bool IsPlanGoal(int32 goal) { return IsPlaceGoal(goal) || IsCellGoal(goal); }

        /// How near a journal place counts as reached (TravelTo).
        static constexpr float PLACE_REACH = 20.0f;
    };
}

#endif
