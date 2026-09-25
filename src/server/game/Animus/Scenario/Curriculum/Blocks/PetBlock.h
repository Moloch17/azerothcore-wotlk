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

#ifndef ANIMUS_LIB_CURRICULUM_PET_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PET_BLOCK_H

#include "Block.h"

class Creature;
class ObjectGuid;
class Player;

namespace Animus::Curriculum
{
    /// The pet bar of the classes with a controllable pet (hunters, warlocks, death knights, mages): its state, its
    /// most useful abilities (a Felhunter's Spell Lock, a Succubus' Seduction, a Water Elemental's Freeze, a ghoul's
    /// Gnaw, a beast's specials) and its stance and follow orders, as a player uses them. Empty for other classes.
    class PetBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_PRESENT                 = 0,
            OBS_ALIVE                   = 1,
            OBS_HEALTH                  = 2,
            OBS_POWER                   = 3,    // focus, energy or mana as a fraction
            OBS_TARGET_DISTANCE         = 4,    // yards from the pet to the bot's target / 60
            OBS_ATTACKING_TARGET        = 5,
            OBS_CASTING                 = 6,
            OBS_REACT_FIRST             = 7,    // one-hot: passive, defensive, aggressive
            OBS_FOLLOWING               = 10,
            OBS_STAYING                 = 11,
            OBS_KIND_FIRST              = 12,   // one-hot: what the pet is (PetKind)
            OBS_TEMPORARY               = 23,   // it leaves on its own (a ghoul without Master of Ghouls, an elemental)
            OBS_TIME_LEFT               = 24,   // ... seconds until it does / 60
            OBS_COMMANDABLE             = 25,   // it takes orders: a guardian fights on its own and every pet action
                                                // of it is masked, but it is still there and still fighting
            OBS_SLOT_FIRST              = 26,   // per ability slot SLOT_FEATURES
        };

        /// What a pet is: a hunter beast by its talent tree, a warlock's demon, a ghoul, a Water Elemental. The
        /// abilities alone do not tell a Voidwalker's tanking from an Imp's casting, or a tenacity beast from a
        /// ferocity one.
        enum PetKind : uint32
        {
            KIND_FEROCITY = 0,
            KIND_TENACITY,
            KIND_CUNNING,
            KIND_IMP,
            KIND_VOIDWALKER,
            KIND_SUCCUBUS,
            KIND_FELHUNTER,
            KIND_FELGUARD,
            KIND_GHOUL,
            KIND_WATER_ELEMENTAL,
            KIND_OTHER,
            KIND_COUNT
        };

        /// Per ability slot: present, on cooldown, interrupt, crowd control, dispel, threat, helps an ally, damage.
        enum SlotFeature : uint32
        {
            SLOT_PRESENT = 0,
            SLOT_ON_COOLDOWN,
            SLOT_INTERRUPT,
            SLOT_CONTROL,
            SLOT_DISPEL,
            SLOT_THREAT,
            SLOT_POSITIVE,
            SLOT_DAMAGE,
            SLOT_FEATURES
        };

        /// A talented hunter beast carries more than four castable abilities (a focus dump, its family's special,
        /// a taunt, a sprint and what its talents added), and Abilities() keeps only the best kinds: at four slots
        /// a ferocity pet's Rabid or Call of the Wild was never offered.
        static constexpr uint32 ABILITY_SLOTS = 6;

        enum Action : uint32
        {
            ACTION_ABILITY_FIRST        = 0,    // cast slot 0..ABILITY_SLOTS-1: at the target, or on itself or the bot
            ACTION_PASSIVE              = ACTION_ABILITY_FIRST + ABILITY_SLOTS,
            ACTION_DEFENSIVE,
            ACTION_AGGRESSIVE,
            ACTION_FOLLOW,
            ACTION_STAY,
            ACTION_COUNT
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Pet; }
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
        [[nodiscard]] ModeGroup ModeGroupOf(Layout const& /*layout*/, uint32 local) const override
        {
            return local == ACTION_PASSIVE || local == ACTION_DEFENSIVE || local == ACTION_AGGRESSIVE
                ? ModeGroup::PetStance : ModeGroup::None;
        }

        /// Whether the pet out right now has an ability that interrupts a cast (a Felhunter's Spell Lock, a
        /// Succubus' Seduction, a silence): for several classes that is the seat's only interrupt, so holding one
        /// has to look here too. Cooldowns are not counted, as CoreBlock::KnowsInterrupt does not count them.
        [[nodiscard]] static bool HasInterruptAbility(SeatView const& view);

        /// Whether the class has a pet this block controls.
        [[nodiscard]] static bool HasPet(uint8 playerClass);
        /// The bot's controllable pet, if one is out.
        [[nodiscard]] static Creature* FindPet(Player* bot);

        /// Once per pet (`lastPet` remembers the one already seen): a pet that came out passive starts defensive, as
        /// a player's does. A new pet's CharmInfo sets it passive, and a player's summon then loads the stance saved
        /// with the pet, which a bot has none of: every seat's pet stayed passive, only fighting what it was sent at.
        /// A stance chosen after this one is left alone.
        static void DefaultStance(Creature* pet, ObjectGuid& lastPet);

        [[nodiscard]] static PetKind KindOf(Creature const* pet);
    };
}

#endif
