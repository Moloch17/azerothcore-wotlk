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

#include "Aptitude.h"
#include "ActionCatalog.h"
#include "ClassAssets.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>

namespace
{
    using Animus::Curriculum::ActionCatalog;

    /// A count read as a share. Two defensive cooldowns is not twice the character one is, and a feature that grew
    /// without bound would swamp the rest of the vector, so each count saturates at the point where more of it stops
    /// meaning much.
    float Share(uint32 count, uint32 enough)
    {
        return enough ? std::min(1.0f, float(count) / float(enough)) : 0.0f;
    }

    /// The spell an action would cast for this character: its highest known rank with a bot in hand, its first rank
    /// without one. A draft pool describes candidates that have not been built, and the first rank is what the
    /// class's kit says the character will eventually have.
    SpellInfo const* SpellOf(ActionCatalog::Action const& action, Player const* bot)
    {
        if (action.Type != ActionCatalog::Kind::Spell || !action.FirstRank)
            return nullptr;

        if (!bot)
            return sSpellMgr->GetSpellInfo(action.FirstRank);

        return ActionCatalog::KnownRank(bot, action.FirstRank);
    }

    bool HasEffect(SpellInfo const* info, uint32 effect)
    {
        if (!info)
            return false;

        for (SpellEffectInfo const& e : info->GetEffects())
            if (e.Effect == effect)
                return true;

        return false;
    }

    bool HasAura(SpellInfo const* info, uint32 aura)
    {
        if (!info)
            return false;

        for (SpellEffectInfo const& e : info->GetEffects())
            if (e.Effect && e.ApplyAuraName == aura)
                return true;

        return false;
    }

    /// A heal that reaches more than the one it is cast on: a party or raid area aura, or a heal effect with a
    /// radius to it.
    bool ReachesSeveral(SpellInfo const* info)
    {
        if (!info)
            return false;

        for (SpellEffectInfo const& e : info->GetEffects())
        {
            if (!e.Effect)
                continue;

            if (e.Effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY || e.Effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID)
                return true;

            if (e.IsAreaAuraEffect() || e.IsTargetingArea())
                return true;
        }

        return false;
    }

    /// It takes a target other than the caster. A defensive that can only be cast on oneself protects nobody else,
    /// however strong it is.
    bool TakesAnotherTarget(SpellInfo const* info)
    {
        return info && (info->GetExplicitTargetMask() & TARGET_FLAG_UNIT) != 0;
    }
}

void Animus::Curriculum::Aptitude::WriteBrief(float* out) const
{
    if (!out)
        return;

    out[BRIEF_MITIGATION] = std::max(Features[MITIGATION], Features[TAUNT] * Features[THREAT]);
    out[BRIEF_HEALING] = std::max({ Features[DIRECT_HEAL], Features[HOT_HEAL], Features[AREA_HEAL] });
    out[BRIEF_MELEE] = Features[MELEE_DAMAGE];
    out[BRIEF_SPELL] = Features[SPELL_DAMAGE];
    out[BRIEF_CONTROL] = Features[CONTROL];
    out[BRIEF_PET] = Features[PET];
}

char const* Animus::Curriculum::Aptitude::FeatureName(uint32 feature)
{
    switch (feature)
    {
        case TAUNT:             return "taunt";
        case MITIGATION:        return "mitigation";
        case THREAT:            return "threat";
        case DIRECT_HEAL:       return "direct_heal";
        case HOT_HEAL:          return "hot_heal";
        case AREA_HEAL:         return "area_heal";
        case MELEE_DAMAGE:      return "melee_damage";
        case SPELL_DAMAGE:      return "spell_damage";
        case RANGED_DAMAGE:     return "ranged_damage";
        case CONTROL:           return "control";
        case INTERRUPT:         return "interrupt";
        case BUFF:              return "buff";
        case DISPEL_FRIENDLY:   return "dispel_friendly";
        case CLEANSE_MAGIC:     return "cleanse_magic";
        case CLEANSE_CURSE:     return "cleanse_curse";
        case CLEANSE_DISEASE:   return "cleanse_disease";
        case CLEANSE_POISON:    return "cleanse_poison";
        case DISPEL_OFFENSIVE:  return "dispel_offensive";
        case PROTECT_OTHER:     return "protect_other";
        case REVIVE:            return "revive";
        case BATTLE_REVIVE:     return "battle_revive";
        case PET:               return "pet";
        case WATER:             return "water";
        default:                break;
    }

    if (feature >= TREE_POINTS_FIRST && feature < COUNT)
    {
        static constexpr char const* TREES[TalentBuilder::TREE_COUNT] = { "tree_0", "tree_1", "tree_2" };
        return TREES[feature - TREE_POINTS_FIRST];
    }

    return "unknown";
}

Animus::Curriculum::Aptitude Animus::Curriculum::Aptitude::Of(ClassAssets const& assets,
    TalentBuilder::Build const& build, Player const* bot)
{
    Aptitude out;
    if (!assets.Catalog)
        return out;

    uint32 taunts = 0;
    uint32 defensives = 0;
    uint32 protectOthers = 0;
    uint32 threats = 0;
    uint32 directHeals = 0;
    uint32 hots = 0;
    uint32 areaHeals = 0;
    uint32 buffs = 0;
    uint32 controls = 0;
    uint32 interrupts = 0;
    uint32 cleanses = 0;
    uint32 purges = 0;
    uint32 cleanseMask = 0;
    uint32 pets = 0;
    uint32 water = 0;
    uint32 melee = 0;
    uint32 spell = 0;
    uint32 ranged = 0;
    uint32 damageActions = 0;

    auto const read = [&](ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action, bot);
        if (!info)
            return;

        if (HasEffect(info, SPELL_EFFECT_ATTACK_ME) || HasAura(info, SPELL_AURA_MOD_TAUNT))
            ++taunts;

        if (HasAura(info, SPELL_AURA_MOD_THREAT))
            ++threats;

        if (action.Defensive)
        {
            ++defensives;
            if (TakesAnotherTarget(info) && info->IsPositive())
                ++protectOthers;
        }

        if (action.DirectHeal)
            ++directHeals;
        else if (action.Healing && action.KeepsAura)
            ++hots;

        if (action.Healing && ReachesSeveral(info))
            ++areaHeals;

        if (action.LongBuff)
            ++buffs;

        if (ActionCatalog::IsInterruptingSpell(info))
            ++interrupts;

        if (action.Dispel)
        {
            if (action.DispelFriendly)
            {
                ++cleanses;
                cleanseMask |= action.DispelMask;
            }
            else
                ++purges;
        }

        if (HasEffect(info, SPELL_EFFECT_SUMMON_PET))
            ++pets;

        if (HasAura(info, SPELL_AURA_WATER_BREATHING) || HasAura(info, SPELL_AURA_WATER_WALK)
            || HasAura(info, SPELL_AURA_MOD_INCREASE_SWIM_SPEED))
            ++water;

        if (action.From != ActionCatalog::Group::Combat)
            return;

        switch (info->DmgClass)
        {
            case SPELL_DAMAGE_CLASS_MELEE:  ++melee;  ++damageActions; break;
            case SPELL_DAMAGE_CLASS_RANGED: ++ranged; ++damageActions; break;
            case SPELL_DAMAGE_CLASS_MAGIC:  ++spell;  ++damageActions; break;
            default: break;
        }
    };

    for (ActionCatalog::Action const& action : assets.Catalog->Actions())
        read(action);
    for (ActionCatalog::Action const& action : assets.Catalog->Tactical())
        read(action);
    for (ActionCatalog::Action const& action : assets.Catalog->Sustain())
        read(action);

    for (ActionCatalog::Action const& action : assets.Catalog->Tactical())
        if (SpellOf(action, bot))
            ++controls;

    for (ActionCatalog::Action const& action : assets.Catalog->Revives())
    {
        SpellInfo const* info = SpellOf(action, bot);
        if (!info && action.Type != ActionCatalog::Kind::Soulstone)
            continue;

        out.Features[REVIVE] = 1.0f;
        // Rebirth and a soulstone are the two that work while the fight is still going; every other resurrection
        // is barred in combat, which is what makes bringing somebody back mid-pull a capability of its own.
        if (!info || !info->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL))
            out.Features[BATTLE_REVIVE] = 1.0f;
    }

    out.Features[TAUNT] = taunts ? 1.0f : 0.0f;
    out.Features[THREAT] = Share(threats + taunts, 2);
    out.Features[DIRECT_HEAL] = Share(directHeals, 3);
    out.Features[HOT_HEAL] = Share(hots, 3);
    out.Features[AREA_HEAL] = Share(areaHeals, 2);
    out.Features[CONTROL] = Share(controls, 5);
    out.Features[INTERRUPT] = interrupts ? 1.0f : 0.0f;
    out.Features[BUFF] = Share(buffs, 5);
    out.Features[DISPEL_FRIENDLY] = cleanses ? 1.0f : 0.0f;
    out.Features[CLEANSE_MAGIC] = (cleanseMask & (1 << DISPEL_MAGIC)) ? 1.0f : 0.0f;
    out.Features[CLEANSE_CURSE] = (cleanseMask & (1 << DISPEL_CURSE)) ? 1.0f : 0.0f;
    out.Features[CLEANSE_DISEASE] = (cleanseMask & (1 << DISPEL_DISEASE)) ? 1.0f : 0.0f;
    out.Features[CLEANSE_POISON] = (cleanseMask & (1 << DISPEL_POISON)) ? 1.0f : 0.0f;
    out.Features[DISPEL_OFFENSIVE] = purges ? 1.0f : 0.0f;
    out.Features[PROTECT_OTHER] = Share(protectOthers, 2);
    out.Features[PET] = pets ? 1.0f : 0.0f;
    out.Features[WATER] = Share(water, 2);

    if (damageActions)
    {
        out.Features[MELEE_DAMAGE] = float(melee) / float(damageActions);
        out.Features[SPELL_DAMAGE] = float(spell) / float(damageActions);
        out.Features[RANGED_DAMAGE] = float(ranged) / float(damageActions);
    }

    // Mitigation is the one feature gear speaks to as loudly as the build does: a shield is most of what separates
    // a character that can stand in front from one that cannot, and armour says how long it lasts there.
    float mitigation = Share(defensives, 4);
    if (bot)
    {
        if (Item const* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            if (ItemTemplate const* item = offHand->GetTemplate(); item && item->InventoryType == INVTYPE_SHIELD)
                mitigation = std::min(1.0f, mitigation + 0.5f);

        float const level = float(std::max<uint32>(1, bot->GetLevel()));
        float const armour = float(bot->GetArmor()) / (85.0f * level + 400.0f);
        mitigation = std::min(1.0f, mitigation + 0.25f * std::min(1.0f, armour));
    }

    out.Features[MITIGATION] = mitigation;

    for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
        out.Features[TREE_POINTS_FIRST + tree] = tree < build.TreePoints.size()
            ? std::min(1.0f, float(build.TreePoints[tree]) / 71.0f) : 0.0f;

    return out;
}
