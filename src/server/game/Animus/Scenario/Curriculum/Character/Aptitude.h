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

#ifndef ANIMUS_LIB_CURRICULUM_APTITUDE_H
#define ANIMUS_LIB_CURRICULUM_APTITUDE_H

#include "TalentBuilder.h"
#include <array>

class Player;

namespace Animus::Curriculum
{
    struct ClassAssets;

    /// What a build can do, read off the build.
    ///
    /// This replaces Role, which was a hand-written label on a spec doing four jobs badly: it told the seat what it
    /// was, told a seat about others, decided who got spawned, and measured whether it worked. A label cannot do any
    /// of them well, because it is three bits standing in for a character. Aptitude is the same information measured
    /// instead of declared: every number here is a fact about the character in hand, and none of them is a decision
    /// about how to play it. A bear-form druid carrying heals scores on both, and what to do about that is the
    /// policy's problem -- which is the point.
    ///
    /// It also answers the case a label could never reach. Characters.RandomTalentChance and NoisyTalentChance put
    /// builds into training that no template describes, and those used to inherit whatever Role their template
    /// carried, which was simply untrue of them. Aptitude reads the build it actually has, so a player who makes a
    /// beastmaster hunter -- or something stranger -- is handed a policy that has seen builds scored the way theirs
    /// will be.
    ///
    /// Computed per seat per episode: the talents and the gear are part of the answer, so it cannot be cached on the
    /// class. With a Player in hand every question is asked of what that character actually knows
    /// (ActionCatalog::KnownRank); without one -- a candidate in a director's draft pool, described but not yet
    /// built -- the answer comes from the catalog and the talent ranks alone, which is why `bot` may be null.
    struct Aptitude
    {
        enum Feature : uint32
        {
            /// It can make something attack it.
            TAUNT                   = 0,
            /// It can cut the damage it takes: defensive cooldowns known, and a shield in hand.
            MITIGATION,
            /// It can hold what it has made angry.
            THREAT,
            /// Single-target heals with nothing else to them.
            DIRECT_HEAL,
            /// Heals that land over time.
            HOT_HEAL,
            /// Heals that reach more than one.
            AREA_HEAL,
            /// Where the build's damage comes from. Three shares of its combat actions, so a hybrid reads as one.
            MELEE_DAMAGE,
            SPELL_DAMAGE,
            RANGED_DAMAGE,
            /// Stuns, roots, fears, silences and the rest of the tactical list.
            CONTROL,
            /// It can stop a cast.
            INTERRUPT,
            /// Long buffs it brings to a group.
            BUFF,
            /// It can take something off an ally, and which four things it can take off.
            DISPEL_FRIENDLY,
            CLEANSE_MAGIC,
            CLEANSE_CURSE,
            CLEANSE_DISEASE,
            CLEANSE_POISON,
            /// It can take something off an enemy (purge, Spellsteal).
            DISPEL_OFFENSIVE,
            /// It can spend a defensive cooldown on somebody else (Hand of Protection, Pain Suppression,
            /// Guardian Spirit). Distinct from MITIGATION, which only protects the seat itself.
            PROTECT_OTHER,
            /// It can put a dead player back up, and whether it can do so mid-fight (Rebirth).
            REVIVE,
            BATTLE_REVIVE,
            /// It fights with something it summons.
            PET,
            /// It can breathe, walk on, or swim faster in water -- druid Aquatic Form, shaman Water Walking,
            /// priest Levitate, warlock Unending Breath, death knight Path of Frost. Whether a crossing is worth
            /// making is not the same question for all of them.
            WATER,
            /// Where the points went, as a share of the 71 a character may spend.
            TREE_POINTS_FIRST,
            COUNT                   = TREE_POINTS_FIRST + TalentBuilder::TREE_COUNT
        };

        /// The compressed view, for the slots that describe *somebody else*: a teammate, an owner, an opponent, or a
        /// seat the director is choosing between. Six numbers rather than the full COUNT, because a party block has
        /// PARTY_MEMBERS of them and a director has one per seat of a side.
        ///
        /// These are measurements, not roles. "Eight tenths of the way to being able to hold a pull" is a different
        /// claim from "is a tank", and it is the one that survives a build nobody planned.
        enum Brief : uint32
        {
            BRIEF_MITIGATION        = 0,
            BRIEF_HEALING,
            BRIEF_MELEE,
            BRIEF_SPELL,
            BRIEF_CONTROL,
            BRIEF_PET,
            BRIEF_COUNT
        };

        std::array<float, COUNT> Features{};

        [[nodiscard]] float operator[](uint32 feature) const
        {
            return feature < COUNT ? Features[feature] : 0.0f;
        }

        /// The six-number view of this aptitude, written into `out`.
        void WriteBrief(float* out) const;

        /// How far this build answers a demand for one feature, which is what composition asks instead of asking a
        /// role. Composition wants "somebody who can hold this pull", and that is a number, not a category.
        [[nodiscard]] float Answers(uint32 feature) const { return (*this)[feature]; }

        /// Read a character. `build` supplies the talent ranks and tree split; `bot`, when there is one, restricts
        /// every question to the spells that character actually knows and supplies its gear.
        [[nodiscard]] static Aptitude Of(ClassAssets const& assets, TalentBuilder::Build const& build,
            Player const* bot);

        /// The name of a feature, for manifests and reports.
        [[nodiscard]] static char const* FeatureName(uint32 feature);
    };

    /// What a seat is wanted for, when something has to decide who to spawn.
    ///
    /// This is the one job Role did that could not simply be deleted with it: a party with nobody who can hold a
    /// pull is a wasted episode, not a lesson. But the thing that decides it does not have to be a label somebody
    /// wrote down -- it can be a threshold on what a build measurably does. "Somebody who can hold this" and
    /// "somebody who can keep the hurt one up" are demands; "a tank" and "a healer" are names for whoever usually
    /// meets them, and names are what stop a class being drawn for a job its build could do perfectly well.
    ///
    /// A demand for nothing in particular is the common case and the honest one: a group's third, fourth and fifth
    /// seats are whoever else turned up.
    struct AptitudeDemand
    {
        uint32 Feature = Aptitude::COUNT;   // Aptitude::COUNT: no demand, anybody will do
        float AtLeast = 0.0f;

        [[nodiscard]] bool Any() const { return Feature < Aptitude::COUNT; }
        [[nodiscard]] bool MetBy(Aptitude const& aptitude) const
        {
            return !Any() || aptitude[Feature] >= AtLeast;
        }

        /// For episode info and logs: the feature demanded, or "any".
        [[nodiscard]] char const* Name() const
        {
            return Any() ? Aptitude::FeatureName(Feature) : "any";
        }

        [[nodiscard]] static AptitudeDemand Anything() { return {}; }
        /// The two a group actually has to fill. The floors are deliberately low enough that an off-template build
        /// which can really do the job is not turned away for not looking like the usual answer.
        [[nodiscard]] static AptitudeDemand HoldsThePull() { return { Aptitude::MITIGATION, 0.5f }; }
        [[nodiscard]] static AptitudeDemand KeepsThemUp() { return { Aptitude::DIRECT_HEAL, 0.34f }; }
    };
}

#endif
