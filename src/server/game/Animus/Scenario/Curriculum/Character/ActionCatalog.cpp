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

#include "ActionCatalog.h"
#include "BotAccounts.h"
#include "ClassKit.h"
#include "DBCStores.h"
#include "BotFactory.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TalentBuilder.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace
{
    bool IsExcludedEffect(uint32 effect)
    {
        switch (effect)
        {
            case SPELL_EFFECT_TELEPORT_UNITS:
            case SPELL_EFFECT_RESURRECT:
            case SPELL_EFFECT_RESURRECT_NEW:
            case SPELL_EFFECT_SELF_RESURRECT:
            case SPELL_EFFECT_CREATE_ITEM:
            case SPELL_EFFECT_CREATE_ITEM_2:
            case SPELL_EFFECT_OPEN_LOCK:
            case SPELL_EFFECT_LEARN_SPELL:
            case SPELL_EFFECT_LEARN_PET_SPELL:
            case SPELL_EFFECT_TRADE_SKILL:
            case SPELL_EFFECT_SKILL:
            case SPELL_EFFECT_TAMECREATURE:
            case SPELL_EFFECT_DISMISS_PET:
            case SPELL_EFFECT_DUEL:
            case SPELL_EFFECT_ENCHANT_ITEM:
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
            case SPELL_EFFECT_PICKPOCKET:
            case SPELL_EFFECT_SUMMON_OBJECT_WILD:
            case SPELL_EFFECT_PROSPECTING:
            case SPELL_EFFECT_MILLING:
            case SPELL_EFFECT_DISENCHANT:
            case SPELL_EFFECT_FEED_PET:
            case SPELL_EFFECT_SUMMON_PLAYER:
            case SPELL_EFFECT_BIND:
            case SPELL_EFFECT_STUCK:
            case SPELL_EFFECT_TRANS_DOOR:
            case SPELL_EFFECT_ADD_FARSIGHT:
                return true;
            default:
                return false;
        }
    }

    bool IsExcludedAura(uint32 aura)
    {
        switch (aura)
        {
            case SPELL_AURA_MOUNTED:
            case SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED:
            case SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED:
            case SPELL_AURA_FLY:
            case SPELL_AURA_TRACK_CREATURES:
            case SPELL_AURA_TRACK_RESOURCES:
            case SPELL_AURA_TRACK_STEALTHED:
            // Feather fall and hover used to be excluded here with the rest of the travel conveniences. They are
            // survival auras now (IsSurvivalAura): a drop off a ledge is a move the seat may choose, and Slow Fall
            // or Levitate is what decides whether it costs health, so the classes that have one need the button.
            // Water walk and water breathing left the list with them, for the dive drill and the lake fight: what a
            // build can do in water changes whether a crossing or a dive is worth it, and they are survival auras.
            case SPELL_AURA_FAR_SIGHT:
            case SPELL_AURA_BIND_SIGHT:
            case SPELL_AURA_MOD_POSSESS:
            case SPELL_AURA_MOD_CHARM:
            case SPELL_AURA_GHOST:
                return true;
            default:
                return false;
        }
    }

    bool IsDamageRelevantAura(uint32 aura)
    {
        switch (aura)
        {
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            case SPELL_AURA_PERIODIC_LEECH:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
            case SPELL_AURA_PROC_TRIGGER_SPELL:
            case SPELL_AURA_PROC_TRIGGER_DAMAGE:
            case SPELL_AURA_MOD_ATTACK_POWER:
            case SPELL_AURA_MOD_ATTACK_POWER_PCT:
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER_PCT:
            case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
            case SPELL_AURA_MOD_DAMAGE_DONE:
            case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
            case SPELL_AURA_MOD_MELEE_HASTE:
            case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
            case SPELL_AURA_MOD_RANGED_HASTE:
            case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
            case SPELL_AURA_HASTE_SPELLS:
            case SPELL_AURA_MOD_CRIT_PCT:
            case SPELL_AURA_MOD_WEAPON_CRIT_PERCENT:
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL:
            case SPELL_AURA_MOD_STAT:
            case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
            case SPELL_AURA_MOD_RESISTANCE:
            case SPELL_AURA_MOD_RESISTANCE_PCT:
            case SPELL_AURA_MOD_SHAPESHIFT:
            case SPELL_AURA_MOD_POWER_REGEN:
            case SPELL_AURA_MOD_POWER_REGEN_PERCENT:
            case SPELL_AURA_OBS_MOD_POWER:
            case SPELL_AURA_PERIODIC_ENERGIZE:
            case SPELL_AURA_ADD_FLAT_MODIFIER:
            case SPELL_AURA_ADD_PCT_MODIFIER:
            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
            case SPELL_AURA_MOD_DAMAGE_TAKEN:
            case SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT:
            case SPELL_AURA_MOD_POWER_COST_SCHOOL:
            case SPELL_AURA_MOD_HIT_CHANCE:
            case SPELL_AURA_MOD_SPELL_HIT_CHANCE:
            case SPELL_AURA_MOD_EXPERTISE:
            case SPELL_AURA_DUMMY:
            case SPELL_AURA_PERIODIC_DUMMY:
            case SPELL_AURA_MOD_STEALTH:
            case SPELL_AURA_MOD_RATING:
            case SPELL_AURA_MOD_SPELL_DAMAGE_OF_ATTACK_POWER:
            case SPELL_AURA_MOD_SPELL_DAMAGE_OF_STAT_PERCENT:
                return true;
            default:
                return false;
        }
    }

    /// What separates a player from a rotation: getting somewhere (speed), not getting hit (avoidance, immunity,
    /// reflection, breaking or preventing crowd control), losing or handing on threat, and lasting longer.
    bool IsSurvivalAura(uint32 aura)
    {
        switch (aura)
        {
            case SPELL_AURA_MOD_INCREASE_SPEED:
            case SPELL_AURA_MECHANIC_IMMUNITY:
            case SPELL_AURA_SCHOOL_IMMUNITY:
            case SPELL_AURA_DISPEL_IMMUNITY:
            case SPELL_AURA_MOD_DODGE_PERCENT:
            case SPELL_AURA_MOD_PARRY_PERCENT:
            case SPELL_AURA_MOD_BLOCK_PERCENT:
            case SPELL_AURA_REFLECT_SPELLS:
            case SPELL_AURA_REFLECT_SPELLS_SCHOOL:
            case SPELL_AURA_DEFLECT_SPELLS:
            case SPELL_AURA_MOD_TOTAL_THREAT:
            case SPELL_AURA_MOD_THREAT:
            case SPELL_AURA_FEIGN_DEATH:
            case SPELL_AURA_MOD_INVISIBILITY:
            case SPELL_AURA_SPLIT_DAMAGE_PCT:
            case SPELL_AURA_MOD_INCREASE_HEALTH:
            case SPELL_AURA_230:                // increases maximum health (Commanding Shout)
            case SPELL_AURA_FEATHER_FALL:       // Slow Fall: a fall costs nothing (Player::HandleFall)
            case SPELL_AURA_HOVER:              // Levitate: the same
            case SPELL_AURA_WATER_BREATHING:    // Unending Breath, Water Breathing: no breath timer (Player::getMaxTimer)
            case SPELL_AURA_WATER_WALK:         // Water Walking, Path of Frost: a lake is a floor
                return true;
            default:
                return false;
        }
    }

    std::string ActionName(SpellInfo const* info)
    {
        std::string name;
        for (char const* c = info->SpellName[LOCALE_enUS]; c && *c; ++c)
        {
            if (std::isalnum(static_cast<unsigned char>(*c)))
                name += char(std::tolower(static_cast<unsigned char>(*c)));
            else if (!name.empty() && name.back() != '_')
                name += '_';
        }

        while (!name.empty() && name.back() == '_')
            name.pop_back();

        return Acore::StringFormat("{}_{}", name.empty() ? "spell" : name, info->Id);
    }
}

bool Animus::Curriculum::ActionCatalog::IsCombatSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive())
        return false;

    // Auto Shot, Shoot (wand), Throw.
    if (info->IsAutoRepeatRangedSpell())
        return true;

    bool useful = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (IsExcludedEffect(effect.Effect) || IsExcludedAura(effect.ApplyAuraName))
            return false;

        switch (effect.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_HEALTH_LEECH:
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_ENERGIZE:
            case SPELL_EFFECT_ENERGIZE_PCT:
            case SPELL_EFFECT_ADD_COMBO_POINTS:
            case SPELL_EFFECT_SUMMON_PET:
            case SPELL_EFFECT_SUMMON:
            case SPELL_EFFECT_RESURRECT_PET:    // Revive Pet: a hunter's dead pet is its damage and its tank
            case SPELL_EFFECT_TRIGGER_SPELL:
            case SPELL_EFFECT_DUMMY:
            case SPELL_EFFECT_SCRIPT_EFFECT:
            // Closing in and getting away: Charge, Intercept, Intervene, Blink, Disengage.
            case SPELL_EFFECT_CHARGE:
            case SPELL_EFFECT_CHARGE_DEST:
            case SPELL_EFFECT_JUMP:
            case SPELL_EFFECT_JUMP_DEST:
            case SPELL_EFFECT_LEAP:
            case SPELL_EFFECT_LEAP_BACK:
            case SPELL_EFFECT_REDIRECT_THREAT:   // Misdirection, Tricks of the Trade
            case SPELL_EFFECT_STEAL_BENEFICIAL_BUFF:
                useful = true;
                break;
            case SPELL_EFFECT_TRIGGER_MISSILE:  // a missile whose spell does the work when it lands
                useful |= effect.TriggerSpell != info->Id
                    && IsCombatSpell(sSpellMgr->GetSpellInfo(effect.TriggerSpell));
                break;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
            case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                useful |= IsDamageRelevantAura(effect.ApplyAuraName) || IsSurvivalAura(effect.ApplyAuraName);
                break;
            default:
                break;
        }
    }

    return useful;
}

SpellInfo const* Animus::Curriculum::ActionCatalog::KnownRank(Player const* bot, uint32 firstRank)
{
    SpellInfo const* first = sSpellMgr->GetSpellInfo(firstRank);
    if (!first)
        return nullptr;

    for (SpellInfo const* rank = first->GetLastRankSpell(); rank; rank = rank->GetPrevRankSpell())
    {
        if (bot->HasSpell(rank->Id))
            return rank;

        if (rank == first)
            break;
    }

    return bot->HasSpell(first->Id) ? first : nullptr;
}

Animus::Curriculum::ActionCatalog::ActionCatalog(uint8 playerClass, ClassKit const& kit,
    TalentBuilder const& talents)
{
    std::set<uint32> candidates;

    // Every spell a level 80 bot of each allowed race knows after its trainers: starting spells,
    // racials and the kit (talent-gated ranks are covered by the talents' own first ranks below).
    for (uint8 race = RACE_HUMAN; race <= RACE_DRAENEI; ++race)
    {
        if (!sObjectMgr->GetPlayerInfo(race, playerClass))
            continue;

        // A throwaway level 80 character of the race, never placed in the world: what it can learn.
        Animus::BotFactory::BotSpec spec;
        spec.Name = Acore::StringFormat("Forgeprobe{}", race);
        spec.Race = race;
        spec.Class = playerClass;
        spec.Gender = GENDER_MALE;
        spec.Level = DEFAULT_MAX_LEVEL;
        spec.AccountId = Animus::BotAccounts::Probe(race);

        Player* probe = Animus::BotFactory::Create(spec);
        if (!probe)
            continue;

        kit.Learn(probe);
        for (auto const& [spellId, spell] : probe->GetSpellMap())
            if (spell->State != PLAYERSPELL_REMOVED)
                candidates.insert(spellId);

        Animus::BotFactory::DestroyUnplaced(probe);
    }

    for (ClassKit::KitSpell const& spell : kit.Spells())
        candidates.insert(spell.SpellId);

    for (TalentBuilder::Talent const& talent : talents.Talents())
        for (uint32 rankSpell : talent.RankSpells)
            if (rankSpell)
                candidates.insert(rankSpell);

    // What those spells teach when learned (Player::_addSpell learns a LEARN_SPELL effect's spell): the Feral Charge
    // talent is Feral Charge - Bear and Feral Charge - Cat, and the talent itself is never cast.
    std::vector<uint32> toTeach(candidates.begin(), candidates.end());
    while (!toTeach.empty())
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(toTeach.back());
        toTeach.pop_back();
        if (!info)
            continue;

        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell
                && candidates.insert(effect.TriggerSpell).second)
                toTeach.push_back(effect.TriggerSpell);
    }

    // Rank chains by first rank, for each kind; a chain belongs to the first kind any of its ranks fits
    // (combat, then tactical, then sustain), so the lists never overlap.
    std::set<uint32> chains;
    std::set<uint32> tacticalChains;
    std::set<uint32> sustainChains;
    auto const chainOf = [](SpellInfo const* info)
    {
        SpellInfo const* first = info->GetFirstRankSpell();
        return first ? first->Id : info->Id;
    };

    // A spell that only stuns, fears or roots its caster (Grovel) is never worth a slot.
    std::erase_if(candidates, [](uint32 spellId) { return IsSelfControlSpell(sSpellMgr->GetSpellInfo(spellId)); });

    for (uint32 spellId : candidates)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId); IsCombatSpell(info))
            chains.insert(chainOf(info));

    for (uint32 spellId : candidates)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId); IsTacticalSpell(info)
            && !chains.contains(chainOf(info)))
            tacticalChains.insert(chainOf(info));

    for (uint32 spellId : candidates)
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId); IsSustainSpell(info)
            && !chains.contains(chainOf(info)) && !tacticalChains.contains(chainOf(info)))
            sustainChains.insert(chainOf(info));

    auto const spellAction = [](uint32 firstRank, Group from)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(firstRank);

        Action action;
        action.From = from;
        action.Type = Kind::Spell;
        action.Name = ActionName(info);
        action.FirstRank = firstRank;
        action.NextSwing = info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING)
            || info->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE);
        action.Healing = IsHealingSpell(info);
        action.Rankable = action.Healing && info->GetNextRankSpell();
        action.DirectHeal = IsDirectHealSpell(info);
        action.Defensive = IsDefensiveSpell(info);
        action.LongBuff = IsLongBuff(info);
        action.KeepsAura = KeepsAuraSpell(info);
        action.DispelMask = DispelMaskOf(info);
        action.Dispel = action.DispelMask != 0;
        action.DispelFriendly = action.Dispel && info->IsPositive();
        action.FeatherFall = info->HasAura(SPELL_AURA_FEATHER_FALL) || info->HasAura(SPELL_AURA_HOVER);
        action.WaterBreathing = info->HasAura(SPELL_AURA_WATER_BREATHING);
        action.WaterWalk = info->HasAura(SPELL_AURA_WATER_WALK);
        return action;
    };

    _actions.push_back({ Kind::Noop, "noop" });
    _actions.push_back({ Kind::CancelQueued, "cancel_queued" });

    for (uint32 firstRank : chains)
        _actions.push_back(spellAction(firstRank, Group::Combat));

    _actions.push_back({ Kind::Trinket, "trinket_1", 0, EQUIPMENT_SLOT_TRINKET1 });
    _actions.push_back({ Kind::Trinket, "trinket_2", 0, EQUIPMENT_SLOT_TRINKET2 });
    // Weapons, shields and held books with a use effect (Rituals of the New Moon, Arcanite Ripper): about 70 of the
    // items the gear pools draw from, whose effects were otherwise out of reach.
    _actions.push_back({ Kind::Trinket, "use_main_hand", 0, EQUIPMENT_SLOT_MAINHAND });
    _actions.push_back({ Kind::Trinket, "use_off_hand", 0, EQUIPMENT_SLOT_OFFHAND });

    for (uint32 firstRank : tacticalChains)
        _tactical.push_back(spellAction(firstRank, Group::Tactical));

    for (uint32 firstRank : sustainChains)
        _sustain.push_back(spellAction(firstRank, Group::Sustain));

    // The whole kit is the core's: a duel healer heals itself and a duel mage polymorphs, as players do. The
    // lists stay for what casts them elsewhere (ally heals).
    _actions.insert(_actions.end(), _tactical.begin(), _tactical.end());
    _actions.insert(_actions.end(), _sustain.begin(), _sustain.end());

    std::set<uint32> reviveChains;
    for (uint32 spellId : candidates)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        // Resurrections target a dead ally's corpse (TARGET_FLAG_CORPSE_ALLY), not a living unit.
        bool const targeted = info
            && (info->NeedsExplicitUnitTarget() || (info->GetExplicitTargetMask() & TARGET_FLAG_CORPSE_MASK));
        if (targeted && !info->IsPassive()
            && (info->HasEffect(SPELL_EFFECT_RESURRECT) || info->HasEffect(SPELL_EFFECT_RESURRECT_NEW)))
            reviveChains.insert(chainOf(info));
    }

    for (uint32 firstRank : reviveChains)
        _revives.push_back(spellAction(firstRank, Group::None));

    if (playerClass == CLASS_WARLOCK)
        _revives.push_back({ Kind::Soulstone, "soulstone" });

    LOG_DEBUG("module.animus", "Class {}: {} actions, {} tactical, {} sustain, {} revives, from {} candidate spells",
        playerClass, _actions.size(), _tactical.size(), _sustain.size(), _revives.size(), candidates.size());

    // Number the actions of every list, so a seat can key a table by action without searching for it.
    for (std::vector<Action>* list : { &_actions, &_tactical, &_sustain, &_revives })
        for (uint32 index = 0; index < list->size(); ++index)
            (*list)[index].Index = index;
}

bool Animus::Curriculum::ActionCatalog::IsHealingSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive())
        return false;

    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_PCT:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                return true;
            default:
                break;
        }

        if (effect.IsAura() && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL
            || effect.ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH || effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB))
            return true;
    }

    return false;
}

bool Animus::Curriculum::ActionCatalog::IsDirectHealSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive() || !info->IsPositive())
        return false;

    bool heal = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (effect.IsAura() || effect.HasRadius() || effect.IsTargetingArea())
            return false;

        switch (effect.Effect)
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_PCT:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                heal = true;
                break;
            default:
                return false;
        }
    }

    return heal;
}

bool Animus::Curriculum::ActionCatalog::IsDefensiveSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive() || !info->IsPositive())
        return false;

    int32 const duration = info->GetMaxDuration();
    if (duration <= 0 || duration >= 5 * MINUTE * IN_MILLISECONDS)
        return false;

    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.IsAura())
            continue;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                if (effect.BasePoints < 0)
                    return true;
                break;
            case SPELL_AURA_SCHOOL_IMMUNITY:
            case SPELL_AURA_DAMAGE_IMMUNITY:
            case SPELL_AURA_SPLIT_DAMAGE_PCT:
            case SPELL_AURA_MOD_DODGE_PERCENT:
            case SPELL_AURA_MOD_PARRY_PERCENT:
            case SPELL_AURA_MOD_BLOCK_PERCENT:
                return true;
            default:
                break;
        }
    }

    return false;
}

bool Animus::Curriculum::ActionCatalog::IsLongBuff(SpellInfo const* info)
{
    if (!info || info->IsPassive() || !info->IsPositive() || info->GetMaxDuration() < 10 * MINUTE * IN_MILLISECONDS)
        return false;

    bool aura = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (!effect.IsAura())
            return false;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_PERIODIC_HEAL:
            case SPELL_AURA_OBS_MOD_HEALTH:
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_MOD_SHAPESHIFT:
            case SPELL_AURA_MOD_STEALTH:
            case SPELL_AURA_MOUNTED:
            case SPELL_AURA_MOD_REGEN:
            case SPELL_AURA_MOD_POWER_REGEN:
                return false;
            default:
                aura = true;
                break;
        }
    }

    return aura;
}

bool Animus::Curriculum::ActionCatalog::KeepsAuraSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive() || !info->IsPositive() || info->StackAmount > 1 || info->GetMaxDuration() <= 0)
        return false;

    bool kept = IsLongBuff(info) || IsDefensiveSpell(info);
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (effect.IsAreaAuraEffect() || effect.HasRadius())
            return false;

        kept |= effect.IsAura() && (effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL
            || effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB);
    }

    return kept;
}

bool Animus::Curriculum::ActionCatalog::IsSelfControlSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive())
        return false;

    bool control = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (!effect.IsAura())
            return false;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_STUN:
            case SPELL_AURA_MOD_CONFUSE:
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_ROOT:
            case SPELL_AURA_MOD_SILENCE:
            case SPELL_AURA_MOD_PACIFY_SILENCE:
            case SPELL_AURA_TRANSFORM:
                break;
            default:
                return false;
        }

        if (effect.TargetA.GetTarget() != TARGET_UNIT_CASTER || effect.TargetB.GetTarget())
            return false;

        control = true;
    }

    return control;
}

bool Animus::Curriculum::ActionCatalog::IsTacticalSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive())
        return false;

    bool tactical = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (IsExcludedEffect(effect.Effect) || IsExcludedAura(effect.ApplyAuraName))
            return false;

        switch (effect.Effect)
        {
            case SPELL_EFFECT_INTERRUPT_CAST:
            case SPELL_EFFECT_KNOCK_BACK:
            case SPELL_EFFECT_ATTACK_ME:
            case SPELL_EFFECT_DISTRACT:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:  // traps
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
                tactical = true;
                break;
            case SPELL_EFFECT_DISPEL:
                tactical |= !info->IsPositive();
                break;
            case SPELL_EFFECT_TRIGGER_MISSILE:  // Freezing Arrow: a missile that drops a Freezing Trap where it lands
                tactical |= effect.TriggerSpell != info->Id
                    && IsTacticalSpell(sSpellMgr->GetSpellInfo(effect.TriggerSpell));
                break;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                switch (effect.ApplyAuraName)
                {
                    case SPELL_AURA_MOD_STUN:
                    case SPELL_AURA_MOD_SILENCE:
                    case SPELL_AURA_MOD_PACIFY_SILENCE:
                    case SPELL_AURA_MOD_CONFUSE:
                    case SPELL_AURA_MOD_FEAR:
                    case SPELL_AURA_MOD_ROOT:
                    case SPELL_AURA_TRANSFORM:
                    case SPELL_AURA_MOD_TAUNT:
                    case SPELL_AURA_MOD_DISARM:
                    case SPELL_AURA_MOD_DISARM_OFFHAND:
                    case SPELL_AURA_MOD_DISARM_RANGED:
                        tactical = true;
                        break;
                    case SPELL_AURA_MOD_DECREASE_SPEED:     // snares on an enemy, not a slowed stealth
                        tactical |= !info->IsPositive();
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    return tactical;
}

bool Animus::Curriculum::ActionCatalog::IsSustainSpell(SpellInfo const* info)
{
    if (!info || info->IsPassive())
        return false;

    bool sustain = false;
    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (!effect.Effect)
            continue;

        if (IsExcludedEffect(effect.Effect) || IsExcludedAura(effect.ApplyAuraName))
            return false;

        switch (effect.Effect)
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_PCT:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                sustain = true;
                break;
            case SPELL_EFFECT_DISPEL:
                sustain |= info->IsPositive();
                break;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                sustain |= effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL
                    || effect.ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH
                    || effect.ApplyAuraName == SPELL_AURA_SCHOOL_ABSORB;
                break;
            default:
                break;
        }
    }

    return sustain;
}

uint32 Animus::Curriculum::ActionCatalog::DispelMaskOf(SpellInfo const* info)
{
    if (!info)
        return 0;

    uint32 mask = 0;
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.Effect == SPELL_EFFECT_DISPEL)
            mask |= SpellInfo::GetDispelMask(DispelType(effect.MiscValue));

    return mask;
}

bool Animus::Curriculum::ActionCatalog::IsInterruptingSpell(SpellInfo const* info)
{
    // Null where every other predicate here takes it: a seat that knows no rank of the action yet
    // (Encoding::KnownRank), which is what CoreBlock's held interrupt asks about every decision it runs.
    if (!info)
        return false;

    for (SpellEffectInfo const& effect : info->GetEffects())
    {
        if (effect.Effect == SPELL_EFFECT_INTERRUPT_CAST || effect.Effect == SPELL_EFFECT_KNOCK_BACK)
            return true;

        if (effect.Effect == SPELL_EFFECT_APPLY_AURA)
        {
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_TRANSFORM:
                    return true;
                default:
                    break;
            }
        }
    }

    return false;
}
