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
    /// **What the party's director asked of this seat** (the director, restored 2026-10-26): the posture the party
    /// holds, and the seat's own order -- its kind, where what it names is (when the seat sees or remembers it), what
    /// is left of it, whether it is already the seat's selection, open or within reach, whether it takes a key item
    /// and the seat carries one, the order's age, and whether it was carried out this decision.
    ///
    /// No actions. An order is advice: the seat reads it and chooses for itself, which is what lets a director be
    /// wrong without a seat being helpless; the seat is paid for carrying it out (DirectorEncounter). Only a directed
    /// stage carries this block.
    class OrderBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_ACTIVE                  = 0,    // a director is giving orders at all
            OBS_POSTURE_FIRST           = 1,    // one-hot over TeamPosture
            OBS_KIND_FIRST              = OBS_POSTURE_FIRST + TEAM_POSTURE_COUNT,   // one-hot over OrderKind
            OBS_TARGET                  = OBS_KIND_FIRST + ORDER_KIND_COUNT,        // the order names something
            OBS_TARGET_UNSEEN,                  // ... that the seat neither sees nor remembers: told only
            OBS_TARGET_DISTANCE,                // yards / DISTANCE_SCALE, when known
            OBS_TARGET_NEAR,                    // the same over NEAR_SCALE yards
            OBS_TARGET_SIN,                     // its bearing off the seat's facing
            OBS_TARGET_COS,
            OBS_TARGET_HEALTH,
            OBS_TARGET_OPEN,
            OBS_TARGET_IS_SELECTION,
            OBS_TARGET_IN_REACH,
            OBS_NEEDS_KEY,
            OBS_CARRIES_KEY,
            OBS_AGE,                            // seconds / AGE_SCALE, clamped
            OBS_DONE_NOW,
            OBS_COUNT
        };

        static constexpr float DISTANCE_SCALE = 100.0f;
        static constexpr float NEAR_SCALE = 20.0f;
        static constexpr float AGE_SCALE = 60.0f;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        [[nodiscard]] static std::string ColumnName(uint32 column);
    };
}

#endif
