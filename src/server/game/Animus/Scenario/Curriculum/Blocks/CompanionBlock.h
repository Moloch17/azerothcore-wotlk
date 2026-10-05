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

#ifndef ANIMUS_LIB_CURRICULUM_COMPANION_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_COMPANION_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// The player the bot fights for: its health, position, class, target and attackers, and the class's revives.
    /// Actions: assist (its target), guard (an enemy on it), revive it (resurrection spells on the dead owner, a
    /// warlock's soulstone on the living one). Heals, shields and buffs on it are core actions aimed by the support
    /// block's friend selection.
    ///
    /// No follow: the engine-run follow went with every seat engine move (player-controller C9, revision 1). Keeping
    /// up with the owner is the move block's keys, learned in its own stage later (movement-curriculum).
    class CompanionBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_OWNER_PRESENT           = 0,
            OBS_OWNER_ALIVE             = 1,
            OBS_OWNER_HEALTH            = 2,
            OBS_OWNER_MANA              = 3,    // 0 without mana
            OBS_OWNER_DISTANCE          = 4,    // yards / 40
            OBS_OWNER_BEARING_SIN       = 5,
            OBS_OWNER_BEARING_COS       = 6,
            OBS_OWNER_IN_COMBAT         = 7,
            OBS_OWNER_MOVING            = 8,
            OBS_OWNER_LEVEL_DIFF        = 9,    // (owner level - bot level) / 5
            OBS_OWNER_CLASS_FIRST       = 10,   // one-hot over PLAYABLE_CLASSES (10)
            OBS_OWNER_ATTACKERS         = 20,   // enemies attacking the owner / ENEMY_COUNT_SCALE
            OBS_OWNER_TARGET_FIRST      = 21,   // one-hot: which of the NAMED_ENEMY_SLOTS the owner attacks
            OBS_OWNER_NO_TARGET         = 25,   // it attacks nothing (past the named slots: this and those all 0)
            OBS_SLOT_ON_OWNER_FIRST     = 26,   // per named enemy slot: attacking the owner
            OBS_GLOBAL_COUNT            = 30

            // Then per revive: known, cooldown.
        };

        enum Action : uint32
        {
            ACTION_ASSIST               = 0,    // target the owner's target
            ACTION_GUARD                = 1,    // target an enemy attacking the owner
            ACTION_REVIVE_FIRST         = 2     // one per revive
        };

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        /// 1: the follow action and its clock are gone (player-controller C9).
        [[nodiscard]] uint32 Revision() const override { return 1; }
    };
}

#endif
