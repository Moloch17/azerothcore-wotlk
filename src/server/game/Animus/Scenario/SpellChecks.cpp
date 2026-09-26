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

#include "SpellChecks.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include <algorithm>

float Animus::SpellChecks::CooldownFraction(Player const* bot, SpellInfo const* info)
{
    if (!info)
        return 0.0f;

    uint32 const full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
    return full ? std::min(1.0f, float(bot->GetSpellCooldownDelay(info->Id)) / float(full)) : 0.0f;
}

float Animus::SpellChecks::AuraFraction(Unit const* unit, uint32 spellId, ObjectGuid caster, float* stacks)
{
    Aura const* aura = unit->GetAura(spellId, caster);
    if (!aura)
        return 0.0f;

    if (stacks)
        *stacks = std::max(*stacks, std::min(1.0f, float(std::max(aura->GetStackAmount(), aura->GetCharges())) / 5.0f));

    if (aura->GetMaxDuration() <= 0)
        return 1.0f;    // permanent (stances, forms, presences, auras)

    return std::clamp(float(aura->GetDuration()) / float(aura->GetMaxDuration()), 0.0f, 1.0f);
}

bool Animus::SpellChecks::CheckCast(Player* bot, SpellInfo const* info, SpellCastTargets const& targets,
    Item* castItem, uint32* reason)
{
    // Decided without building anything, where Spell::CheckCast is certain to refuse. Every one of these is a
    // condition CheckCast fails on wherever in its order it comes (it has no early success), under exactly the
    // conditions it checks them: the mount rule, the form rule (unless an aura lets this caster ignore forms) and the
    // combat rule (unless an aura lifts it). They are two fifths of what the masks' checks are asked -- a mounted seat
    // is asked about every spell it has -- and each of those used to build, check and delete a Spell to say no.
    // A caller that wants the reason gets CheckCast's own, from the full check.
    if (!reason)
    {
        SpellCastResult early = SPELL_CAST_OK;
        if (bot->IsMounted() && !info->IsPassive() && !info->HasAttribute(SPELL_ATTR0_ALLOW_WHILE_MOUNTED))
            early = bot->IsInFlight() ? SPELL_FAILED_NOT_ON_TAXI : SPELL_FAILED_NOT_MOUNTED;
        else if (bot->GetAuraEffectsByType(SPELL_AURA_MOD_IGNORE_SHAPESHIFT).empty())
            early = info->CheckShapeshift(bot->GetShapeshiftForm());
        if (early == SPELL_CAST_OK && bot->IsInCombat() && !info->CanBeUsedInCombat()
            && bot->GetAuraEffectsByType(SPELL_AURA_ABILITY_IGNORE_AURASTATE).empty())
            early = SPELL_FAILED_AFFECTING_COMBAT;
        if (early != SPELL_CAST_OK)
            return false;
    }

    // Build the spell, validate it, throw it away.
    Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
    spell->m_CastItem = castItem;
    spell->LoadScripts();
    spell->InitExplicitTargets(targets);

    // A strict check of a demon summon stuns the warlock's current pet (Summoning Disorientation, Spell::CheckCast):
    // the core means it for a summon a player starts, and a mask checks every summon every decision. stage1_duel's
    // demons were stunned for the whole of every fight, ignored every attack order and dealt no damage. Those are
    // checked loosely, with the two strict-only checks a self-cast summon needs done here instead.
    bool const stunsPet = info->HasEffect(SPELL_EFFECT_SUMMON_PET) && bot->IsClass(CLASS_WARLOCK, CLASS_CONTEXT_PET)
        && bot->GetPet();
    SpellCastResult result = spell->CheckCast(!stunsPet);
    delete spell;

    if (stunsPet && result == SPELL_CAST_OK)
        result = bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) ? SPELL_FAILED_NOT_READY
            : info->CheckShapeshift(bot->GetShapeshiftForm());

    if (reason)
        *reason = uint32(result);

    return result == SPELL_CAST_OK;
}
