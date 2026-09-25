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

#ifndef ANIMUS_LIB_CURRICULUM_HOSTILES_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_HOSTILES_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// What the pack block's enemy slots do not say once players share them with creatures: per slot, whether the enemy
    /// is a player, its class, and whether it is casting a heal, stealthed or has a pet out. A separate block, so the
    /// pack block keeps its size and its seeded weights. No actions: the pack block's target slots select them.
    class HostilesBlock final : public Block
    {
    public:
        enum SlotFeature : uint32
        {
            SLOT_PLAYER                 = 0,
            SLOT_CLASS_FIRST            = 1,    // one-hot over PLAYABLE_CLASSES; 0 for a creature
            SLOT_HEALING                = 11,   // casting a heal
            SLOT_STEALTHED              = 12,
            SLOT_PET_OUT                = 13,
            SLOT_FEATURES               = 14
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Hostiles; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
