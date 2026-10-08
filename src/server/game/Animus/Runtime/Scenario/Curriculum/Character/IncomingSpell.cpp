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

#include "IncomingSpell.h"
#include "EncoderSupport.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "Unit.h"
#include <algorithm>

namespace
{
    constexpr float CAST_SCALE_MS = 3000.0f;    // a cast this long or longer reads as "all the time there is"
    constexpr float RADIUS_SCALE_YD = 40.0f;
    constexpr uint32 LONG_CAST_MS = 2000;       // a cast at least this long is a large part of a caster's output

    /// The spell of `type` that `enemy` is part way through, if it is one that takes time: an instant is over before
    /// anything could be observed about it, and the core's own interrupt test treats a zero cast time the same way.
    /// Not const: Spell::GetCastTimeRemaining is not a const member.
    Spell* CastOfType(Unit const* enemy, CurrentSpellTypes type)
    {
        Spell* spell = enemy->GetCurrentSpell(type);
        if (!spell)
            return nullptr;

        if (type == CURRENT_CHANNELED_SPELL)
            return spell->getState() == SPELL_STATE_CASTING ? spell : nullptr;

        return spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0 ? spell : nullptr;
    }

    /// Whether the spell leaves something a dispel could take off.
    bool Dispellable(SpellInfo const* info)
    {
        switch (info->Dispel)
        {
            case DISPEL_MAGIC:
            case DISPEL_CURSE:
            case DISPEL_DISEASE:
            case DISPEL_POISON:
                return true;
            default:
                return false;
        }
    }

    bool Summons(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
            switch (effect.Effect)
            {
                case SPELL_EFFECT_SUMMON:
                case SPELL_EFFECT_SUMMON_PET:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
                case SPELL_EFFECT_TRANS_DOOR:
                    return true;
                default:
                    break;
            }

        return false;
    }

    bool Heals(SpellInfo const* info)
    {
        return info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_PCT)
            || info->HasAura(SPELL_AURA_PERIODIC_HEAL);
    }

    /// The widest radius any of its effects reaches, in yards; 0 for a spell that hits one unit where it stands.
    float Radius(SpellInfo const* info, Unit const* caster)
    {
        float widest = 0.0f;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.HasRadius())
                widest = std::max(widest, effect.CalcRadius(const_cast<Unit*>(caster)));

        return widest;
    }
}

Animus::Curriculum::IncomingSpell::Prevented Animus::Curriculum::IncomingSpell::Classify(SpellInfo const* info,
    uint32 castTimeMs)
{
    if (!info)
        return Prevented::Ordinary;

    // Most valuable first: a heal undoes work already done, an area spell hits everyone, a long cast is a large part
    // of what the caster was going to deal.
    if (Heals(info))
        return Prevented::Heal;
    if (info->IsTargetingArea() || info->HasAreaAuraEffect() || info->MaxAffectedTargets > 1)
        return Prevented::Area;
    if (castTimeMs >= LONG_CAST_MS)
        return Prevented::Long;

    return Prevented::Ordinary;
}

bool Animus::Curriculum::IncomingSpell::Interruptible(Unit const* enemy)
{
    // EffectInterruptCast's own test, so what the features promise is what an interrupt would actually do.
    for (uint32 type = CURRENT_FIRST_NON_MELEE_SPELL; type < CURRENT_AUTOREPEAT_SPELL; ++type)
    {
        Spell* spell = CastOfType(enemy, CurrentSpellTypes(type));
        if (!spell)
            continue;

        SpellInfo const* info = spell->m_spellInfo;
        if (!info || info->PreventionType != SPELL_PREVENTION_TYPE_SILENCE)
            continue;

        if (type == CURRENT_GENERIC_SPELL ? (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0
                                          : (info->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT) != 0)
            return true;
    }

    return false;
}

SpellInfo const* Animus::Curriculum::IncomingSpell::CastInProgress(Unit const* enemy, uint32* leftMs, uint32* totalMs,
    bool* channeled)
{
    if (!enemy)
        return nullptr;

    if (Spell* cast = CastOfType(enemy, CURRENT_GENERIC_SPELL))
    {
        SpellInfo const* info = cast->m_spellInfo;
        if (!info)
            return nullptr;

        int32 const total = cast->GetCastTime();
        int32 const left = std::clamp(cast->GetCastTimeRemaining(), 0, std::max(total, 0));
        if (leftMs)
            *leftMs = uint32(left);
        if (totalMs)
            *totalMs = uint32(std::max(total, left));
        if (channeled)
            *channeled = false;
        return info;
    }

    if (Spell* channel = CastOfType(enemy, CURRENT_CHANNELED_SPELL))
    {
        SpellInfo const* info = channel->m_spellInfo;
        if (!info)
            return nullptr;

        // A channel's clock is its duration, which is what is left of the thing happening to the seat.
        int32 const left = std::max(0, channel->GetCastTimeRemaining());
        int32 const total = std::max(left, std::max(1, info->GetMaxDuration()));
        if (leftMs)
            *leftMs = uint32(left);
        if (totalMs)
            *totalMs = uint32(total);
        if (channeled)
            *channeled = true;
        return info;
    }

    return nullptr;
}

bool Animus::Curriculum::IncomingSpell::Observe(Unit const* enemy, Unit const* seat, float* out)
{
    if (!enemy || !out)
        return false;

    uint32 leftMs = 0;
    uint32 totalMs = 0;
    bool channeled = false;
    SpellInfo const* info = CastInProgress(enemy, &leftMs, &totalMs, &channeled);
    if (!info)
        return false;

    out[FEATURE_CASTING] = 1.0f;
    out[FEATURE_CAST_PROGRESS] = totalMs ? 1.0f - float(leftMs) / float(totalMs) : 0.0f;
    out[FEATURE_CAST_REMAINING] = std::min(1.0f, float(leftMs) / CAST_SCALE_MS);
    out[FEATURE_CHANNELED] = channeled ? 1.0f : 0.0f;
    out[FEATURE_INTERRUPTIBLE] = Interruptible(enemy) ? 1.0f : 0.0f;

    // Who it is for. A cast with no unit target is aimed at a place (or at everyone), which is what the area
    // features are for; nothing here is an id, so an unfamiliar spell still says how it will arrive.
    if (Spell const* cast = enemy->GetCurrentSpell(channeled ? CURRENT_CHANNELED_SPELL : CURRENT_GENERIC_SPELL))
        out[FEATURE_AIMED_AT_ME] = seat && cast->m_targets.GetUnitTarget() == seat ? 1.0f : 0.0f;

    bool const area = info->IsTargetingArea() || info->HasAreaAuraEffect() || info->MaxAffectedTargets > 1;
    out[FEATURE_AREA] = area ? 1.0f : 0.0f;
    out[FEATURE_CONE] = info->HasAttribute(SPELL_ATTR0_CU_CONE_BACK) || info->HasAttribute(SPELL_ATTR0_CU_CONE_LINE)
        ? 1.0f : 0.0f;
    out[FEATURE_SHARED] = info->HasAttribute(SPELL_ATTR0_CU_SHARE_DAMAGE) ? 1.0f : 0.0f;
    out[FEATURE_DISPELLABLE] = Dispellable(info) ? 1.0f : 0.0f;
    out[FEATURE_HEALS] = Heals(info) ? 1.0f : 0.0f;
    out[FEATURE_SUMMONS] = Summons(info) ? 1.0f : 0.0f;
    out[FEATURE_RADIUS] = std::min(1.0f, Radius(info, enemy) / RADIUS_SCALE_YD);

    // Missile flight: a spell with a speed lands some time after its cast ends, which is time to move.
    if (info->Speed > 0.0f && seat)
        out[FEATURE_TRAVEL] = std::min(1.0f, enemy->GetDistance(seat) / info->Speed * 1000.0f / CAST_SCALE_MS);

    uint32 const schoolMask = info->GetSchoolMask();
    for (std::size_t school = 0; school < Encoding::OBSERVED_SCHOOLS.size(); ++school)
        if (schoolMask & (1 << Encoding::OBSERVED_SCHOOLS[school]))
            out[FEATURE_SCHOOL_FIRST + school] = 1.0f;

    uint64 const mechanicMask = info->GetAllEffectsMechanicMask();
    for (std::size_t mechanic = 0; mechanic < Encoding::OBSERVED_MECHANICS.size(); ++mechanic)
        if (mechanicMask & (uint64(1) << Encoding::OBSERVED_MECHANICS[mechanic]))
            out[FEATURE_MECHANIC_FIRST + mechanic] = 1.0f;

    return true;
}
