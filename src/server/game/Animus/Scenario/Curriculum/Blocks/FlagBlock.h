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

#ifndef ANIMUS_LIB_CURRICULUM_FLAG_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_FLAG_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// A flag match from the seat's side, as a battleground's frames show it: where both flags are (at base, carried,
    /// dropped), the way to both bases and to a dropped flag, and the score. No actions: the duel, travel and core
    /// actions play it (the travel objective follows the flags).
    class FlagBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_CARRYING                = 0,    // the seat carries the other side's flag
            OBS_OWN_AT_BASE             = 1,
            OBS_OWN_CARRIED             = 2,    // by the enemy
            OBS_OWN_DROPPED             = 3,
            OBS_ENEMY_AT_BASE           = 4,
            OBS_ENEMY_DROPPED           = 5,
            OBS_OWN_BASE_DISTANCE       = 6,    // yards / 200
            OBS_OWN_BASE_SIN            = 7,    // bearing relative to the bot's facing
            OBS_OWN_BASE_COS            = 8,
            OBS_ENEMY_BASE_DISTANCE     = 9,
            OBS_ENEMY_BASE_SIN          = 10,
            OBS_ENEMY_BASE_COS          = 11,
            OBS_DROPPED_DISTANCE        = 12,   // the nearest dropped flag, either side's
            OBS_DROPPED_SIN             = 13,
            OBS_DROPPED_COS             = 14,
            OBS_OWN_SCORE               = 15,   // captures / 3
            OBS_ENEMY_SCORE             = 16,
            OBS_CAN_TAKE                = 17,   // a flag is in reach to take or return
            OBS_COUNT                   = 18
        };

        enum Action : uint32
        {
            /// Use the flag in reach: take the other side's, or return one's own. A scripted battleground scores
            /// a pickup on the object being used, never on the seat standing over it.
            ACTION_TAKE_FLAG            = 0,
            ACTION_COUNT                = 1
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Flag; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
    };
}

#endif
