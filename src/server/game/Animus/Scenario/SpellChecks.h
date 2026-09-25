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

#ifndef ANIMUS_LIB_SPELL_CHECKS_H
#define ANIMUS_LIB_SPELL_CHECKS_H

#include "Define.h"
#include "ObjectGuid.h"

class Item;
class Player;
class SpellCastTargets;
class SpellInfo;
class Unit;

/*
 * Spell state every scenario reads the same way: cooldowns, auras and the core's cast check run without casting.
 */
namespace Animus::SpellChecks
{
    enum SharedSpells : uint32
    {
        SPELL_BATTLE_STANCE     = 2457,
        SPELL_DEFENSIVE_STANCE  = 71,
    };

    /// The global cooldown most spells start.
    constexpr float GCD_MS = 1500.0f;

    /// Remaining cooldown of a spell as a fraction of its full cooldown; 0 for a null spell.
    [[nodiscard]] float CooldownFraction(Player const* bot, SpellInfo const* info);

    /// Remaining duration fraction of `caster`'s aura `spellId` on `unit` (1 for a permanent aura, 0 without it).
    /// With `stacks`, raises it to the aura's stacks or charges / 5.
    [[nodiscard]] float AuraFraction(Unit const* unit, uint32 spellId, ObjectGuid caster, float* stacks = nullptr);

    /// The core's own cast validation (Spell::CheckCast) for `targets`, without casting: the PetAI pattern.
    /// `reason`, when given, takes the core's SpellCastResult as a number -- SPELL_CAST_OK (255) when it passed.
    [[nodiscard]] bool CheckCast(Player* bot, SpellInfo const* info, SpellCastTargets const& targets,
        Item* castItem = nullptr, uint32* reason = nullptr);
}

#endif
