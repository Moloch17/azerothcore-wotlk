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

#ifndef ANIMUS_LIB_CURRICULUM_CORE_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_CORE_BLOCK_H

#include "Aptitude.h"
#include "Block.h"

namespace Animus::Curriculum
{
    /// The character and its class: level, race, spec, resources, swing timers and stats; per catalog action (spell or
    /// trinket) whether it is known, its cooldown and its auras; the talent build. Actions: the catalog.
    class CoreBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_LEVEL                   = 0,    // level / 80
            OBS_RACE_FIRST              = 1,    // one-hot over PLAYABLE_RACES
            /// What this character can do, measured off its build (Aptitude::COUNT features: what it can taunt,
            /// mitigate, heal, control, buff, cleanse, protect, revive, summon and swim with, and where its points
            /// went).
            ///
            /// This used to be a three-wide one-hot over Role, on the argument that a paladin had to be told
            /// whether it was holding the line or healing it. But a role is a name for what somebody expects, and
            /// the seat can be handed a build that name is simply false about -- TalentPlan::Noisy and ::Random do
            /// it deliberately, and a player making their own character does it by accident. The talents were
            /// already observed further down; what was missing was not a label but the reading of them, which is
            /// what this is. There is still deliberately no spec name here, for the same reason there is no role.
            OBS_APTITUDE_FIRST          = 11,
            OBS_HEALTH                  = 37,
            OBS_MANA                    = 38,   // fraction of max; 0 without mana
            OBS_RAGE                    = 39,   // / 100
            OBS_ENERGY                  = 40,   // fraction of max
            OBS_RUNIC_POWER             = 41,   // / 100
            OBS_RUNE_FIRST              = 42,   // 6 runes: 1 ready, else 1 - cooldown / 10 s
            OBS_COMBO_POINTS            = 48,   // on the target, / 5
            OBS_FORM_FIRST              = 49,   // one-hot over the tracked forms (13)
            OBS_GCD                     = 62,   // remaining / 1.5 s
            OBS_CASTING                 = 63,   // casting or channeling
            OBS_QUEUED_NEXT_SWING       = 64,
            OBS_MAIN_HAND_SWING         = 65,   // swing timer remaining / weapon speed
            OBS_OFF_HAND_SWING          = 66,
            OBS_RANGED_SWING            = 67,
            OBS_MAIN_HAND_SPEED         = 68,   // seconds / 4
            OBS_TARGET_HEALTH           = 69,
            OBS_TARGET_DISTANCE         = 70,   // yards / 40
            OBS_IN_MELEE_FRONT          = 71,
            OBS_ATTACK_POWER            = 72,   // / (100 + 50 * level)
            OBS_SPELL_POWER             = 73,   // / (50 + 30 * level)
            OBS_MELEE_CRIT              = 74,   // percent / 100
            OBS_SPELL_CRIT              = 75,
            OBS_MELEE_HASTE             = 76,   // rating bonus percent / 100
            OBS_SPELL_HASTE             = 77,
            OBS_MELEE_HIT               = 78,
            OBS_SPELL_HIT               = 79,
            OBS_EXPERTISE               = 80,   // / 30
            OBS_ARMOR_PENETRATION       = 81,   // rating bonus percent / 100
            OBS_LAST_STEP_DAMAGE        = 82,   // damage since the last decision / damage scale
            OBS_LAST_STEP_POWER_DELTA   = 83,   // primary power change since the last decision, as a fraction
            /// Time into the episode / 5 min, clamped. Without it a bot that stands still out of combat sees the
            /// same rows over and over, and a deterministic policy cycles through the same decisions for good:
            /// stage1_duel evaluation had warlocks start and stop one cast 299 times, 0 damage, on six seeds.
            OBS_EPISODE_TIME            = 84,
            /// What the seat has been doing (SeatMemory): one observation says nothing of it, so a policy re-decided
            /// from scratch every decision, running in and backing off by turns and dancing between stances.
            OBS_SINCE_MOVE              = 85,   // time since its last movement order / 5 s; 1 = none yet
            OBS_SINCE_MODE_CHANGE       = 86,   // time since its last stance, form, aspect, aura, seal, armor or pet
                                                // stance change / 10 s; 1 = none yet
            OBS_HEALTH_TREND            = 87,   // its health now - its average over the last few seconds
            OBS_TARGET_HEALTH_TREND     = 88,   // the same for its target
            /// Per durative action (SeatOptionKind without None): how much of its clock is left / 30 s, 0 when it is
            /// not running. Without them a running option is hidden state: the policy could not tell that it is
            /// already resting, holding an interrupt or walking a bearing -- and the seat runs several at once (a
            /// positioning option, a standby, a turn and a pitch), so one slot with one clock could not say which.
            /// Five: rest, the held interrupt, the held bearing, the held turn and the held pitch. The companion's
            /// follow is an option too, reported by the companion block so that only its layouts carry it. The
            /// direction of the last target-relative move used to sit before these; there are no target-relative
            /// moves now.
            OBS_OPTION_FIRST            = 89,
            OBS_GLOBAL_COUNT            = 94

            // Then, per catalog action: ACTION_FEATURES features (known, cooldown, aura on target, aura on self,
            // stacks, time since the seat pressed it / 10 s). Then per talent of the class: rank / max rank. Then
            // per tree: points / 71.
        };

        /// After the catalog's actions: which rank of a rankable spell to cast (RANK_TIERS: the highest known, about
        /// two thirds up, about a third up). It lived in the support block, which only the stages from the gauntlet
        /// on have -- so in the duel a seat could only ever cast the biggest heal it knew, at any deficit, and
        /// overhealing was not a habit it could break. Down-ranking is a property of casting, so it belongs here.
        static constexpr uint32 ACTION_RANK_TIERS = RANK_TIERS;

        /// The first two catalog actions are the no-op and cancel-queued.
        static constexpr uint32 FIRST_CAST_ACTION = 2;
        static constexpr uint32 ACTION_FEATURES = 6;

        void BeforeApply(SeatView& view, SeatActionResult& result) const override;

        [[nodiscard]] BlockId Id() const override { return BlockId::Core; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
        [[nodiscard]] ModeGroup ModeGroupOf(Layout const& layout, uint32 local) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;

        /// Whether the seat knows a spell that interrupts a cast (Kick, Counterspell, Mind Freeze, Shield Bash),
        /// whatever its cooldown: what makes holding an interrupt worth offering at all. Cooldowns are not counted --
        /// the hold runs for seconds and the mask would flicker under it.
        [[nodiscard]] static bool KnowsInterrupt(SeatView const& view);

        /// The character as built (level, race, spec, talent build): written whether the bot is alive or not.
        static void ObserveCharacter(SeatView const& view, float* obs);

        [[nodiscard]] static uint32 TalentObsFirst(Layout const& layout);
        [[nodiscard]] static uint32 TreeObsFirst(Layout const& layout);
    };
}

#endif
