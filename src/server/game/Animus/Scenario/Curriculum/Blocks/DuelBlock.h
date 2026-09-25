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

#ifndef ANIMUS_LIB_CURRICULUM_DUEL_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_DUEL_BLOCK_H

#include "Block.h"
#include "IncomingSpell.h"

class Player;
class Unit;

namespace Animus::Curriculum
{
    /// Fighting something that fights back: where the target is and what it does, the bot's casting, form and pet,
    /// what it carries (potions, healthstones, bandages, a soulstone), death, and a hunter's stable. Actions:
    /// auto-attack, pet attack, stop casting, cancel form, the consumables, resurrecting itself when dead, call a
    /// stabled beast.
    ///
    /// **No movement.** This block used to issue the pathfinder's orders -- move to the target, behind it, to
    /// casting range, back off, stop, keep range, stay on the target, break line of sight -- positions the engine
    /// chose and walked to on the policy's behalf. They are gone: the seat's feet are the move block's bearings,
    /// chosen against the target's bearing and distance this block still reports, and where to stand in a fight is
    /// learned rather than ordered. Facing the target while standing still stays, because a facing is aiming and
    /// not a position, and a swing or a cast needs it.
    class DuelBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_DISTANCE                = 0,    // yards / 60
            OBS_BEARING_SIN             = 1,    // direction to the target relative to the bot's facing
            OBS_BEARING_COS             = 2,
            OBS_BEHIND_TARGET           = 3,    // the bot is in the target's back arc
            OBS_TARGET_FACING_BOT       = 4,
            OBS_TARGET_IN_COMBAT        = 5,
            OBS_TARGET_ATTACKS_BOT      = 6,
            OBS_TARGET_CASTING          = 7,
            OBS_BOT_MOVING              = 8,
            OBS_BOT_IN_COMBAT           = 9,
            OBS_BOT_STEALTHED           = 10,
            OBS_BOT_AUTO_ATTACKING      = 11,
            OBS_DAMAGE_TAKEN            = 12,   // since the last decision / bot max health
            OBS_PET_OUT                 = 13,
            OBS_PET_HEALTH              = 14,
            OBS_PET_ATTACKING           = 15,   // the pet's victim is the target
            OBS_COMBAT_TIME             = 16,   // time the bot has been in combat / 60 s; 0 out of combat
            OBS_CAST_PROGRESS           = 17,   // fraction of the current cast time done; 0 when not casting
            OBS_CAST_REMAINING          = 18,   // seconds left of the current cast / 3
            OBS_SHAPESHIFTED            = 19,   // in a form the bot can cancel
            OBS_HEALTH_POTIONS          = 20,   // carried / CONSUMABLE_COUNT
            OBS_MANA_POTIONS            = 21,
            OBS_HEALTHSTONES            = 22,   // carried (at most 1)
            OBS_BANDAGES                = 23,   // carried / CONSUMABLE_COUNT
            OBS_POTION_COOLDOWN         = 24,   // the shared potion cooldown left, as a fraction
            OBS_HEALTHSTONE_COOLDOWN    = 25,
            OBS_RECENTLY_BANDAGED       = 26,   // no bandage can be used yet
            OBS_SOULSTONE_ON_BOT        = 27,   // the bot will be able to resurrect itself if it dies
            OBS_DEAD                    = 28,
            OBS_SELF_RESURRECT          = 29,   // dead, and able to resurrect itself (Soulstone, Reincarnation)
            // The target hides (stealth, invisibility): every live target feature above is 0, these remember it.
            OBS_TARGET_HIDDEN           = 30,
            OBS_TARGET_UNSEEN_TIME      = 31,   // time since the bot last saw it / 20 s
            OBS_LAST_SEEN_DISTANCE      = 32,   // yards to where it was last seen / 60
            OBS_LAST_SEEN_BEARING_SIN   = 33,   // direction to that place relative to the bot's facing
            OBS_LAST_SEEN_BEARING_COS   = 34,
            OBS_TARGET_IN_LINE_OF_SIGHT = 35,   // nothing in the way: casts can reach it, and it can reach the bot
            // What the target is, which decides what works on it: a player reads the same off a nameplate and a
            // tooltip, and learns it after a first spell fails. Without it a Fear on a fear-immune undead or a fire
            // spell on a fire elemental just failed with nothing to say why.
            OBS_TARGET_TYPE_FIRST       = 36,   // one-hot over Encoding::OPPONENT_TYPES (7); a player is humanoid
            OBS_TARGET_MAX_HEALTH       = 43,   // its max health / the bot's / 4, clamped
            OBS_TARGET_DAMAGE_MODIFIER  = 44,   // its template's damage multiplier / 2 (1 for a player)
            OBS_TARGET_ARMOR            = 45,   // the share of the bot's physical hits its armor takes off (0-0.75)
            OBS_TARGET_RUN_SPEED        = 46,   // run speed rate / 2
            OBS_TARGET_LEVEL_DIFFERENCE = 47,   // (its level - the bot's) / 5, clamped to [-1, 1]
            OBS_TARGET_IMMUNE_SCHOOL_FIRST = 48, // immune to Encoding::OBSERVED_SCHOOLS (6)
            OBS_TARGET_IMMUNE_MECHANIC_FIRST = 54, // immune to Encoding::OBSERVED_MECHANICS (6)
            // The bot's own crowd control, with or without a target.
            OBS_BOT_STUNNED             = 60,
            OBS_BOT_FEARED              = 61,   // feared or confused
            OBS_BOT_ROOTED              = 62,
            OBS_BOT_SILENCED            = 63,
            OBS_BOT_SNARED              = 64,
            /// What the target is casting, by the spell's own properties (IncomingSpell): how long is left of it,
            /// whether it is aimed at the seat, area, cone, interruptible, dispellable, a heal, school and mechanic.
            /// OBS_TARGET_CASTING above is the bare "it is doing something"; these are the casts there is still time
            /// to answer, and they are what tells a seat which cast is worth an interrupt.
            /// Where the seat stands on the target's threat list (1 = it holds aggro).
            OBS_TARGET_THREAT_SHARE     = 65,
            /// The hostile ground effects the seat is standing in (a fire pool, a poison cloud, a consecration):
            /// how many / 3, and the one it is deepest inside -- how far it still has to walk to leave it / 20 yd,
            /// and which way its centre lies, so moving away from that bearing is the way out. Without these a
            /// ground effect is invisible: the seat only ever saw the damage arrive from nowhere.
            OBS_HAZARDS_STANDING_IN     = 66,
            OBS_HAZARD_WAY_OUT          = 67,
            OBS_HAZARD_BEARING_SIN      = 68,
            OBS_HAZARD_BEARING_COS      = 69,
            /// What has been done to the seat (Encoding::Debuffs): how many harmful auras are on it, how many of
            /// them a dispel could remove, the worst stack count, how long the longest has left, and which crowd
            /// control mechanics are among them. Its own buffs were always visible per catalog action; what an
            /// enemy put on it never was.
            /// The nearest hostile ground effect the seat is NOT in yet, within 30 yd: how far its edge is / 20 yd,
            /// which way its centre lies, and how wide it is. Standing-in tells a seat to leave; this is what lets
            /// it stay out, since nothing else in the observation tells clear ground from ground about to be walked
            /// into. 0 when there is none nearby.
            OBS_NEAR_HAZARD             = 70,   // there is one
            OBS_NEAR_HAZARD_EDGE        = 71,   // yards to its edge / 20; 0 when the seat is already inside it
            OBS_NEAR_HAZARD_BEARING_SIN = 72,
            OBS_NEAR_HAZARD_BEARING_COS = 73,
            OBS_NEAR_HAZARD_RADIUS      = 74,   // its radius / 20
            OBS_DEBUFF_COUNT            = 75,   // harmful auras / 5
            OBS_DEBUFF_DISPELLABLE      = 76,   // ... of them dispellable / 5
            OBS_DEBUFF_STACKS           = 77,   // the most stacks any one has / 10
            OBS_DEBUFF_LONGEST          = 78,   // the longest left to run / 30 s
            OBS_DEBUFF_MECHANIC_FIRST   = 79,   // which of Encoding::OBSERVED_MECHANICS are on it (6)
            OBS_TARGET_CAST_FIRST       = 85,
            /// Hunters: per stable slot STABLE_FEATURES.
            OBS_STABLE_FIRST            = OBS_TARGET_CAST_FIRST + IncomingSpell::FEATURE_COUNT,
            OBS_COUNT_WITHOUT_STABLE    = OBS_STABLE_FIRST
        };

        /// Per stabled beast: offered, family / 50, ferocity, tenacity, cunning.
        static constexpr uint32 STABLE_FEATURES = 5;

        enum Action : uint32
        {
            ACTION_START_ATTACK         = 0,    // start auto-attack on the target
            ACTION_PET_ATTACK           = 1,    // send pets and guardians at the target
            ACTION_STOP_CASTING         = 2,    // cancel the current cast or channel
            ACTION_CANCEL_FORM          = 3,    // leave the current shapeshift form, as right-clicking it does
            ACTION_HEALTH_POTION        = 4,    // drink a healing potion
            ACTION_MANA_POTION          = 5,
            ACTION_HEALTHSTONE          = 6,
            ACTION_BANDAGE              = 7,    // bandage itself (a channel, broken by damage)
            ACTION_SOULSTONE_SELF       = 8,    // warlocks: soulstone itself
            ACTION_SELF_RESURRECT       = 9,    // dead: use its Soulstone or Reincarnation (not in the PvP stages)
            ACTION_CALL_BEAST_FIRST     = 10,   // hunters: call stable slot 0..STABLE_SLOTS-1
            ACTION_COUNT_WITHOUT_STABLE = 10
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Duel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] ModeGroup ModeGroupOf(Layout const& /*layout*/, uint32 local) const override
        {
            return local == ACTION_CANCEL_FORM ? ModeGroup::Form : ModeGroup::None;
        }

        /// A dead bot's features and mask (every other block stays empty): dead, and whether it can resurrect itself.
        static void ObserveDead(SeatView const& view, float* obs, uint8* mask);
    };
}

#endif
