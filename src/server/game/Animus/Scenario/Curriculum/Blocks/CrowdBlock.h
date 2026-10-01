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

#ifndef ANIMUS_LIB_CURRICULUM_CROWD_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_CROWD_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// What is on the party past the pack block's PACK_SLOTS enemies, and the pack ahead: a whole dungeon's fights
    /// had a median of eight creatures on the party (2026-10-01), half of them out of sight of a seat that saw four.
    /// Counts over every creature on the party, the next CROWD_SLOTS of them one by one, and the nearest pack not yet
    /// in the fight. A separate block, so the blocks built on PACK_SLOTS keep their sizes and their seeded weights. No
    /// actions: what it shows is fought through the pack block's slots, and stopped short of by the feet.
    class CrowdBlock final : public Block
    {
    public:
        enum Feature : uint32
        {
            OBS_PRESENT         = 0,    // the encounter reports a crowd (a whole dungeon)
            OBS_ON_PARTY        = 1,    // creatures whose victim is a player of the party, / 8
            OBS_ON_TANK         = 2,    // ... on the party's tank, / 8
            OBS_LOOSE           = 3,    // ... on anybody else, / 8
            OBS_ELITES          = 4,    // elites on the party, / 4
            OBS_UNSEEN          = 5,    // on the party past the pack block's slots, / 4
            OBS_AHEAD_DISTANCE  = 6,    // the nearest pack not in the fight, / 60 yd (1: none in sight)
            OBS_AHEAD_SIZE      = 7,    // how many stand with it, / 6
            OBS_AHEAD_SIN       = 8,    // its bearing in the seat's own frame
            OBS_AHEAD_COS       = 9,
            OBS_IS_TANK         = 10,   // this seat is the one the party's crowd is counted against
            OBS_SLOT_FIRST      = 11
        };

        enum SlotFeature : uint32
        {
            SLOT_PRESENT        = 0,
            SLOT_HEALTH         = 1,
            SLOT_DISTANCE       = 2,    // / 40 yd
            SLOT_SIN            = 3,
            SLOT_COS            = 4,
            SLOT_IN_COMBAT      = 5,
            SLOT_ON_ME          = 6,
            SLOT_ON_TANK        = 7,
            SLOT_ELITE          = 8,
            SLOT_FEATURES       = 9
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Crowd; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
