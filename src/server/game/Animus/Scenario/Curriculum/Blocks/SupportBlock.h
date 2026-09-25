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
#ifndef ANIMUS_LIB_CURRICULUM_SUPPORT_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_SUPPORT_BLOCK_H

#include "Aptitude.h"
#include "Block.h"

class Unit;

namespace Animus::Curriculum
{
    /// Friends to look after: the bot itself, the owner and the party's teammates (FRIEND_SLOTS), and how hard to heal
    /// them. Actions: select a friend, which the core's positive single-target spells (heals, shields, HoTs, buffs,
    /// Hands) are then cast on; set the rank tier heals with ranks are cast at, for mana (Encoding::KnownRank). A
    /// friend's features show what the bot keeps on it, as the core's aura features show what it keeps on the enemy.
    class SupportBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_SELECTED_FIRST          = 0,    // one-hot over FRIEND_SLOTS: the selected friend
            OBS_RANK_TIER_FIRST         = 5,    // one-hot over RANK_TIERS
            OBS_GLOBAL_COUNT            = 8

            // Then FRIEND_SLOTS friend slots of FRIEND_FEATURES.
        };

        enum FriendFeature : uint32
        {
            FRIEND_PRESENT              = 0,
            FRIEND_ALIVE                = 1,
            FRIEND_HEALTH               = 2,
            FRIEND_MANA                 = 3,    // 0 without mana
            FRIEND_DISTANCE             = 4,    // yards / 40; 0 for the bot itself
            FRIEND_IN_LINE_OF_SIGHT     = 5,
            FRIEND_ATTACKERS            = 6,    // enemies attacking it / PACK_SLOTS
            /// What it can do, as the six-number brief of its Aptitude; all zero when the seat has no way of
            /// knowing (an empty slot). A healer choosing who to spend a cast on wants to know what the candidate
            /// can do for itself, and a three-way label was a coarse answer to that.
            FRIEND_APTITUDE_FIRST       = 7,
            FRIEND_OWN_HEAL_OVER_TIME   = 13,   // the bot's heal over time on it, as a fraction of its duration left
            FRIEND_OWN_ABSORB           = 14,   // the bot's absorb on it
            FRIEND_BUFFS                = 15,   // share of the layout's buff groups up on it (any caster)
            FRIEND_FEATURES             = 16
        };

        enum Action : uint32
        {
            ACTION_SELECT_FRIEND_FIRST  = 0,                            // + friend slot
            /// The rank a heal is cast at moved to the core block (CoreBlock::ACTION_RANK_TIERS): it is a property
            /// of casting, and only the stages with this block had it, so a duel could not down-rank at all.
            ACTION_COUNT                = FRIEND_SLOTS
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Support; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// The share of `layout`'s buff groups (Layout::BuffGroups) up on `unit`, from any caster; 1 for a layout
        /// with none.
        [[nodiscard]] static float BuffCoverage(Layout const& layout, Unit const* unit);
    };
}

#endif
