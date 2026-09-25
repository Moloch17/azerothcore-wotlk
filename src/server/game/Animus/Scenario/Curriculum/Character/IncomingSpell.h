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

#ifndef ANIMUS_LIB_CURRICULUM_INCOMING_SPELL_H
#define ANIMUS_LIB_CURRICULUM_INCOMING_SPELL_H

#include "Define.h"

class Unit;
class SpellInfo;

namespace Animus::Curriculum
{
    /// What an enemy is casting, described by the spell's own properties rather than by which spell it is.
    ///
    /// A boss's abilities are private to its script -- file-scope `enum Spells` blocks, with no registry of them
    /// anywhere -- so there is nothing to look up and nothing to teach a policy by name. Everything here instead
    /// comes from SpellInfo, which the core fills for every spell in the game: a level 12 gnoll shaman's Lightning
    /// Bolt and a raid boss's describe themselves the same way, and an unseen encounter needs no new code. Spell ids
    /// never reach the observation, so there is nothing to memorise and nothing that overfits to the content that
    /// happened to be trained on.
    ///
    /// This is the incoming counterpart of ActionCatalog's predicates and PetBlock's ability classifier, both of
    /// which already read effects and auras rather than ids.
    class IncomingSpell
    {
    public:
        /// Per casting enemy. Schools and mechanics reuse EncoderSupport's OBSERVED_SCHOOLS and OBSERVED_MECHANICS
        /// so they mean the same thing here as in the immunity features.
        enum Feature : uint32
        {
            FEATURE_CASTING             = 0,    // it is casting or channeling something
            FEATURE_CAST_PROGRESS       = 1,    // the share of the cast already done
            FEATURE_CAST_REMAINING      = 2,    // seconds left of it / 3 s: how long there is to react
            FEATURE_AIMED_AT_ME         = 3,    // the seat is its target
            FEATURE_AREA                = 4,    // it hits an area rather than one unit
            FEATURE_CONE                = 5,    // ... a cone or a line from the caster, which moving aside leaves
            FEATURE_CHANNELED           = 6,
            FEATURE_INTERRUPTIBLE       = 7,    // an interrupt would stop it (EffectInterruptCast's own test)
            FEATURE_DISPELLABLE         = 8,    // what it leaves behind could be removed
            FEATURE_SHARED              = 9,    // its damage is split between those it hits: a soak
            FEATURE_HEALS               = 10,   // the cast most worth stopping
            FEATURE_SUMMONS             = 11,   // it brings something else into the fight
            FEATURE_RADIUS              = 12,   // its radius / 40 yd
            FEATURE_TRAVEL              = 13,   // missile flight to the seat / 3 s; 0 when it lands at once
            FEATURE_SCHOOL_FIRST        = 14,   // one-hot over OBSERVED_SCHOOLS (6)
            FEATURE_MECHANIC_FIRST      = 20,   // one-hot over OBSERVED_MECHANICS (6)
            FEATURE_COUNT               = 26
        };

        /// What stopping a cast was worth, by what the cast was. A heal undoes damage already dealt, an area spell
        /// hits the whole party, a long cast is a large part of the caster's output; an ordinary one is still worth
        /// stopping, just less. Nothing here knows which spell it is, so it holds for a boss nobody has seen.
        enum class Prevented : uint8
        {
            Ordinary = 0,
            Long,
            Area,
            Heal
        };

        [[nodiscard]] static Prevented Classify(SpellInfo const* info, uint32 castTimeMs);

        /// Write `out[0 .. FEATURE_COUNT)` for whatever `enemy` is casting, from `seat`'s point of view, and return
        /// whether it was casting at all. `out` is left untouched where nothing is being cast, so a caller that has
        /// already zeroed its observation can ignore the result.
        static bool Observe(Unit const* enemy, Unit const* seat, float* out);

        /// The spell `enemy` is casting or channeling, and how much of it is left in milliseconds; null when it is
        /// casting nothing. Mirrors the core's own interrupt test in that a cast with no cast time is not a cast.
        [[nodiscard]] static SpellInfo const* CastInProgress(Unit const* enemy, uint32* leftMs, uint32* totalMs,
            bool* channeled);

        /// Whether an interrupt would stop what `enemy` is casting, by the same test EffectInterruptCast applies.
        [[nodiscard]] static bool Interruptible(Unit const* enemy);
    };
}

#endif
