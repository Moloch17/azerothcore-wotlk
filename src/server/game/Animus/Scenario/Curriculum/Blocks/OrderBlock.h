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

#ifndef ANIMUS_LIB_ORDER_BLOCK_H
#define ANIMUS_LIB_ORDER_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// What the side's director asked of this seat: the posture the team holds, the enemy it concentrates on,
    /// the shape it takes, and whether this seat owes the next duty.
    ///
    /// No actions. An order is advice: the seat reads it and chooses for itself, which is what lets a director be
    /// wrong without a seat being helpless, and lets a seat that has learned better disregard it. Only a directed
    /// arena carries this block, so every other layout keeps the shape it has.
    class OrderBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_ACTIVE                  = 0,    // a director is giving orders at all
            OBS_POSTURE                 = 1,    // one-hot, TEAM_POSTURE_COUNT wide
            OBS_RALLY                   = OBS_POSTURE + TEAM_POSTURE_COUNT,   // one-hot, TEAM_RALLY_COUNT wide
            OBS_RALLY_DISTANCE          = OBS_RALLY + TEAM_RALLY_COUNT,       // yards / 100 to the called place
            OBS_RALLY_SIN,                      // its bearing, relative to the seat's facing
            OBS_RALLY_COS,
            OBS_HAS_FOCUS,                      // a target was called
            OBS_FOCUS_UNSEEN,                   // ... and this seat cannot see it: act on memory, or find it
            OBS_FOCUS_IS_TARGET,                // ... and the seat is already on it
            OBS_FOCUS_DISTANCE,                 // yards / 100
            OBS_FOCUS_SIN,
            OBS_FOCUS_COS,
            OBS_FOCUS_HEALTH,                   // what is left of it
            OBS_IS_DUTY,                        // this seat owes the next interrupt or control
            OBS_COUNT
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Order; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
