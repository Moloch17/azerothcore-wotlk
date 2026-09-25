/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
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

#ifndef ANIMUS_HOOKS_H
#define ANIMUS_HOOKS_H

#include "Define.h"
#include "SharedDefines.h"

class Spell;
class SpellInfo;
class Unit;
enum DamageEffectType : uint8;

namespace Animus
{
    class EnvPool;

    /// What the core tells the env pool about combat, as direct calls from the sites that know the whole event:
    /// Unit::DealDamage with its spell, DealHeal with whether the heal was a periodic tick, Spell::cast and
    /// Spell::cancel. These used to be ScriptMgr hooks correlated through thread-local guesses; in core the
    /// facts are passed. Every call is on a map thread and touches only the env whose map that thread updates.
    namespace Hooks
    {
        /// The pool being fed, set on the world thread between map updates (a plan starting or ending). Null while
        /// nothing runs: every hook below returns at once.
        void SetActivePool(EnvPool* pool);
        [[nodiscard]] EnvPool* ActivePool();

        void Damage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type, SpellInfo const* spell);
        void HealCast(Unit* healer, Unit* receiver, uint32 heal);
        void Heal(Unit* healer, Unit* receiver, uint32 gain, bool periodic);
        void CastCompleted(Unit* caster, Spell* spell);
        void CastCancelled(Unit* caster, Spell* spell, bool bySelf);
    }
}

#endif
