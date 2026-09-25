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

#ifndef ANIMUS_LIB_CURRICULUM_CONTEXT_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_CONTEXT_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// The situation the bot is in, as a player would see it: who is with it, whether it fights players or creatures,
    /// whether it is flagged for PvP and what kind of map it is on. A stage that mixes arenas never tells the policy
    /// which arena an episode is; these features let it tell PvE from PvP the way a live server can. No actions.
    class ContextBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_OWNER_PRESENT           = 0,
            OBS_OWNER_ALIVE             = 1,
            OBS_TEAMMATES_ALIVE         = 2,    // living learned teammates / 3
            OBS_HOSTILE_PLAYERS         = 3,    // living enemy players in the enemy slots / PACK_SLOTS
            OBS_HOSTILE_CREATURES       = 4,    // living enemy creatures in the enemy slots / PACK_SLOTS
            OBS_NEAREST_PLAYER_DISTANCE = 5,    // yards / 60 to the nearest living enemy player; 1 without one
            OBS_PLAYER_ON_US            = 6,    // an enemy player attacks the bot or the owner
            OBS_PVP_FLAGGED             = 7,
            OBS_BATTLEGROUND_MAP        = 8,    // a battleground or arena map
            OBS_INSTANCE_MAP            = 9,    // a dungeon or raid map
            OBS_SELF_RESURRECT_ALLOWED  = 10,
            OBS_GROUP_SIZE              = 11,   // members of the bot's group / 5; 0 without a group
            OBS_COUNT                   = 12
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Context; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
