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

#ifndef ANIMUS_LIB_CURRICULUM_PACK_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PACK_BLOCK_H

#include "Block.h"
#include "IncomingSpell.h"

namespace Animus::Curriculum
{
    /// Several enemies at once: PACK_SLOTS enemy slots. Actions: select an enemy slot. (The tactical spells --
    /// interrupts, stuns, crowd control, taunts -- are core actions, cast at the selected enemy.)
    class PackBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_ALIVE                   = 0,    // living enemies / PACK_SLOTS
            OBS_IN_COMBAT               = 1,    // enemies in combat / PACK_SLOTS
            OBS_GLOBAL_COUNT            = 2

            // Then PACK_SLOTS enemy slots of SLOT_FEATURES.
        };

        enum SlotFeature : uint32
        {
            SLOT_PRESENT                = 0,
            SLOT_ALIVE                  = 1,
            SLOT_HEALTH                 = 2,
            SLOT_DISTANCE               = 3,    // yards / 60
            SLOT_BEARING_SIN            = 4,
            SLOT_BEARING_COS            = 5,
            SLOT_BEHIND                 = 6,    // the bot is in its back arc
            SLOT_ATTACKS_BOT            = 7,
            SLOT_ATTACKS_PET            = 8,
            SLOT_CASTING                = 9,
            SLOT_IN_COMBAT              = 10,
            SLOT_CROWD_CONTROLLED       = 11,   // stunned, feared, confused, rooted, silenced or polymorphed
            SLOT_CURRENT_TARGET         = 12,
            SLOT_ELITE                  = 13,
            SLOT_LEVEL_DIFFERENCE       = 14,   // (its level - the bot's) / 5
            SLOT_IN_LINE_OF_SIGHT       = 15,   // the bot can see it past the terrain and buildings
            /// What it is casting, described by the spell's own properties (IncomingSpell): cast time left, whether
            /// it is aimed at the seat, area, cone, interruptible, dispellable, a heal, its school and mechanic.
            /// SLOT_CASTING above is the bare "it is doing something", instants included; these are the casts there
            /// is still time to answer.
            /// Where the seat stands on this enemy's threat list, over the threat of whoever it is on: 1 means it
            /// holds aggro. SLOT_ATTACKS_BOT says the enemy is on it right now; this says how close that is to
            /// changing, which is what tanking and what staying off a pack are both about.
            SLOT_THREAT_SHARE           = 16,
            SLOT_CAST_FIRST             = 17,
            SLOT_FEATURES               = SLOT_CAST_FIRST + IncomingSpell::FEATURE_COUNT
        };

        enum Action : uint32
        {
            ACTION_SLOT_FIRST           = 0,    // select enemy slot 0..PACK_SLOTS-1
            /// Interrupt the target as soon as it starts casting, decision after decision, until
            /// Options.HoldInterruptMs runs out or the policy does something else (SeatOption). The core block, which
            /// owns the spells, casts it.
            ACTION_HOLD_INTERRUPT       = PACK_SLOTS,
            ACTION_COUNT                = PACK_SLOTS + 1
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Pack; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
    };
}

#endif
