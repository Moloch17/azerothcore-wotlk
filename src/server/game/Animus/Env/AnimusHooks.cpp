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

#include "AnimusHooks.h"
#include "EnvPool.h"
#include "Unit.h"
#include <atomic>

namespace
{
    std::atomic<Animus::EnvPool*> ActivePoolPtr{ nullptr };

    inline Animus::EnvPool* Pool()
    {
        return ActivePoolPtr.load(std::memory_order_acquire);
    }
}

namespace Animus::Hooks
{
    void SetActivePool(EnvPool* pool)
    {
        ActivePoolPtr.store(pool, std::memory_order_release);
    }

    EnvPool* ActivePool()
    {
        return Pool();
    }

    void Damage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type, SpellInfo const* spell)
    {
        if (EnvPool* pool = Pool())
            pool->RecordDamage(attacker, victim, damage, type, type == DIRECT_DAMAGE ? nullptr : spell);
    }

    void HealCast(Unit* healer, Unit* receiver, uint32 heal)
    {
        if (EnvPool* pool = Pool())
            pool->RecordHealCast(healer, receiver, heal);
    }

    void Heal(Unit* healer, Unit* receiver, uint32 gain, bool periodic)
    {
        if (EnvPool* pool = Pool())
            pool->RecordHeal(healer, receiver, gain, periodic);
    }

    void CastCompleted(Unit* caster, Spell* spell)
    {
        if (EnvPool* pool = Pool())
            pool->RecordCastCompleted(caster, spell);
    }

    void CastCancelled(Unit* caster, Spell* spell, bool bySelf)
    {
        if (EnvPool* pool = Pool())
            pool->RecordCastCancelled(caster, spell, bySelf);
    }
}
