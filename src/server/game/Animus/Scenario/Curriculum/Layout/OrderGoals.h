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

#ifndef ANIMUS_LIB_CURRICULUM_ORDER_GOALS_H
#define ANIMUS_LIB_CURRICULUM_ORDER_GOALS_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **A director's order as the member's goal** (next-run plan, Wave 4): in a group under a learned director,
    /// a member or group order is the member's primary goal, and the member's own choice fills only its secondary.
    /// One planner per group: the director chooses what each member does, the member how. `enemySlot` is where
    /// the order's target sits among the member's enemy slots (-1: not among them), `objective` the journal
    /// objective an Objective order names. NO_GOAL for an order no goal says (Heal, GoTo: their targets are not
    /// in the goal space), or whose target the member cannot see.
    [[nodiscard]] inline int32 OrderGoal(OrderKind kind, int32 enemySlot, uint32 objective)
    {
        bool const enemy = enemySlot >= 0 && enemySlot < int32(NAMED_ENEMY_SLOTS);
        switch (kind)
        {
            case OrderKind::Focus:
            case OrderKind::Tank:
            case OrderKind::Interrupt:
                return enemy ? MakeGoal(SeatGoal::Fight, GOAL_TARGET_ENEMY_FIRST + uint32(enemySlot)) : NO_GOAL;
            case OrderKind::Control:
                return enemy ? MakeGoal(SeatGoal::Control, GOAL_TARGET_ENEMY_FIRST + uint32(enemySlot)) : NO_GOAL;
            case OrderKind::Objective:
                return objective < 4 ? MakeGoal(SeatGoal::Interact, GOAL_TARGET_OBJECTIVE_FIRST + objective) : NO_GOAL;
            default:
                return NO_GOAL;
        }
    }
}

#endif
