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
#include "Position.h"
#include <array>

namespace Animus::Curriculum
{
    struct SeatView;

    /// **What a goal can be about, right now** (Component C). No actions: the learner reads these columns to mask
    /// its goal head -- a kind is offered only when there is something for it (an enemy for Fight, a corpse for
    /// Loot, a journal place for TravelTo), a target only when it is there -- and to choose again at once when the
    /// goal it held has just been reached or has become impossible (OBS_ENDED). Always the last block of a layout,
    /// so the learner finds it at the end of the observation.
    class GoalBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_KIND_FIRST              = 0,                            // per SeatGoal: something for it is there
            OBS_TARGET_FIRST            = OBS_KIND_FIRST + GOAL_COUNT,  // per GoalTarget: it is there
            OBS_ENDED                   = OBS_TARGET_FIRST + GOAL_TARGETS,  // the goal held was reached or lost
            OBS_REACHED,                                                // ... reached (what the learner predicts)
            OBS_COUNT
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Goal; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        /// Which targets are there, and which kinds have something to be about (at least Fight, always).
        static void Available(SeatView const& view, std::array<bool, GOAL_COUNT>& kinds,
            std::array<bool, GOAL_TARGETS>& targets);

        /// Whether `goal` is reached, read off the world as it is now (the enemy it named is dead or held, the seat
        /// or the friend is healthy again, the place is reached, nothing is left to loot or gather, the objective or
        /// the giver's quest is done), and whether it is still possible (its kind and target are on offer). The
        /// forge and the module both end a goal on these, so the learner re-chooses at the same moments in both.
        static void Status(SeatView const& view, int32 goal, bool& reached, bool& possible);

        /// Whether a goal Status calls reached was *reached* -- made true -- rather than true already when it was
        /// chosen (Fight about no one with nothing to fight, Recover at full health). One true on choice is held,
        /// neither paid nor ended, until it stops being true; reached after that, it counts. `fresh` is set by the
        /// caller when a new goal is chosen and cleared here; `satisfiedAtChoice` is the caller's to keep per goal.
        /// The forge pays Goals.Reached and ends goals on this, and the module ends them on it, so both agree.
        static bool Earned(bool reached, bool& fresh, bool& satisfiedAtChoice);

        /// Where a place target is (a journal objective's, the giver, the turn-in, a found place, the assigned
        /// area); false for a target that is not a place, or not there.
        static bool PlaceOf(SeatView const& view, uint32 target, Position& where);

        /// How near a journal place counts as reached (TravelTo).
        static constexpr float PLACE_REACH = 20.0f;
    };
}

#endif
