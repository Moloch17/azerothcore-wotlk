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

#ifndef ANIMUS_LIB_CURRICULUM_PARTY_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PARTY_BLOCK_H

#include "Aptitude.h"
#include "Block.h"

namespace Animus::Curriculum
{
    /// The other learned party members: PARTY_MEMBERS teammate slots, each with what it is doing and the goal it
    /// says it is pursuing, so a party can divide the work (the owner has the companion block). Actions:
    /// follow the tank, assist and guard each teammate, then each revive on each teammate. Heals, shields and buffs on
    /// them are core actions aimed by the support block's friend selection.
    class PartyBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_ALIVE                   = 0,    // living players in the slots (bot and owner included) / 5
            OBS_LOWEST_HEALTH           = 1,    // the most hurt living ally's health (owner and teammates)
            /// The best mitigation and the best healing among the living teammates other than the bot. Two flags
            /// once ("there is a tank", "there is a healer"); a group that has somebody most of the way to holding
            /// a pull is in a different position from one that has nobody, and a flag could not say so.
            OBS_BEST_MITIGATION         = 2,
            OBS_BEST_HEALING            = 3,
            // The raid the seat's group is part of, which it cannot act on one by one (SeatView::RaidView). All
            // zero in a party, where the group is the whole of it.
            OBS_RAID_GROUP              = 4,    // the seat's group index / RAID_GROUPS
            OBS_RAID_ALIVE              = 5,    // living seats, as a share of the seats in play
            OBS_RAID_GROUP_ALIVE        = 6,    // ... of the seat's own group
            OBS_RAID_IN_COMBAT          = 7,    // seats in combat, as a share of the living
            OBS_RAID_LOWEST_HEALTH      = 8,    // the most hurt living seat anywhere in the raid
            OBS_RAID_TANKS_ALIVE        = 9,    // living tanks / RAID_GROUPS, clamped
            OBS_RAID_HEALERS_ALIVE      = 10,   // living healers / RAID_GROUPS, clamped
            OBS_GLOBAL_COUNT            = 11

            // Then PARTY_MEMBERS teammate slots of MEMBER_FEATURES: GROUP_MEMBERS of the seat's own group, then
            // SPOTLIGHT_SLOTS raiders outside it (PartyEncounter::View).
        };

        enum MemberFeature : uint32
        {
            MEMBER_PRESENT              = 0,
            MEMBER_ALIVE                = 1,
            MEMBER_HEALTH               = 2,
            MEMBER_MANA                 = 3,
            MEMBER_DISTANCE             = 4,    // yards / 40
            MEMBER_BEARING_SIN          = 5,
            MEMBER_BEARING_COS          = 6,
            MEMBER_IN_COMBAT            = 7,
            /// What it can do, as the six-number brief of its Aptitude (mitigation, healing, melee, spell,
            /// control, pet). It was a three-wide role one-hot; "that one is a healer" is less than "that one has
            /// these heals", and the second is true of a build nobody planned.
            MEMBER_APTITUDE_FIRST       = 8,
            MEMBER_CLASS_FIRST          = 14,   // one-hot over PLAYABLE_CLASSES
            MEMBER_ATTACKERS            = 24,   // enemies attacking it / PACK_SLOTS
            MEMBER_TARGET_FIRST         = 25,   // one-hot: which enemy slot it attacks
            MEMBER_NO_TARGET            = 29,
            MEMBER_SLOT_ON_FIRST        = 30,   // per enemy slot: attacking it
            MEMBER_GOAL_FIRST           = 34,   // one-hot over GOAL_COUNT: the goal it is pursuing (none: all 0)
            MEMBER_FEATURES             = 34 + GOAL_COUNT
        };

        enum Action : uint32
        {
            ACTION_FOLLOW_TANK          = 0,
            ACTION_ASSIST_FIRST         = 1,                        // + member
            ACTION_GUARD_FIRST          = 1 + PARTY_MEMBERS,        // + member
            ACTION_REVIVE_FIRST         = 1 + 2 * PARTY_MEMBERS     // + member * revives + revive
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Party; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] bool IsMovement(uint32 local) const override { return local == ACTION_FOLLOW_TANK; }
    };
}

#endif
