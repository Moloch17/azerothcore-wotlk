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

#include "PetBlock.h"
#include "CharmInfo.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "MotionMaster.h"
#include "Pet.h"
#include "Player.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <array>

namespace
{
    using namespace Animus::Curriculum;

    /// What a pet ability is for, most valuable first: the order ability slots are filled in.
    enum AbilityKind : uint8
    {
        KIND_INTERRUPT,
        KIND_CONTROL,
        KIND_DISPEL,
        KIND_THREAT,
        KIND_POSITIVE,
        KIND_DAMAGE,
        KIND_NONE
    };

    struct Ability
    {
        SpellInfo const* Info = nullptr;
        uint8 Flags = 0;        // 1 << AbilityKind of every kind it has
        AbilityKind Best = KIND_NONE;
    };

    Ability Classify(SpellInfo const* info)
    {
        Ability ability;
        ability.Info = info;
        auto const mark = [&ability](AbilityKind kind)
        {
            ability.Flags |= uint8(1 << kind);
            ability.Best = std::min(ability.Best, kind);
        };

        if (info->IsPositive())
            mark(KIND_POSITIVE);

        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_INTERRUPT_CAST:
                    mark(KIND_INTERRUPT);
                    break;
                case SPELL_EFFECT_DISPEL:
                case SPELL_EFFECT_STEAL_BENEFICIAL_BUFF:
                    mark(KIND_DISPEL);
                    break;
                case SPELL_EFFECT_ATTACK_ME:
                    mark(KIND_THREAT);
                    break;
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
                    mark(KIND_DAMAGE);
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                    switch (effect.ApplyAuraName)
                    {
                        case SPELL_AURA_MOD_STUN:
                        case SPELL_AURA_MOD_FEAR:
                        case SPELL_AURA_MOD_CONFUSE:
                        case SPELL_AURA_MOD_ROOT:
                        case SPELL_AURA_MOD_SILENCE:
                        case SPELL_AURA_MOD_PACIFY_SILENCE:
                        case SPELL_AURA_MOD_DECREASE_SPEED:
                            if (!info->IsPositive())
                                mark(effect.ApplyAuraName == SPELL_AURA_MOD_SILENCE ? KIND_INTERRUPT : KIND_CONTROL);
                            break;
                        case SPELL_AURA_MOD_TAUNT:
                            mark(KIND_THREAT);
                            break;
                        case SPELL_AURA_PERIODIC_DAMAGE:
                            mark(KIND_DAMAGE);
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
        }

        return ability;
    }

    /// The pet's castable abilities, most valuable kinds first, at most ABILITY_SLOTS.
    std::vector<Ability> Abilities(Creature* pet)
    {
        std::vector<Ability> found;
        auto const consider = [&found](uint32 spellId)
        {
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!info || info->IsPassive() || info->IsAutoRepeatRangedSpell())
                return;

            Ability const ability = Classify(info);
            if (ability.Best != KIND_NONE)
                found.push_back(ability);
        };

        if (Pet const* realPet = pet->ToPet(); realPet)
        {
            for (auto const& [spellId, spell] : realPet->m_spells)
                if (spell.state != PETSPELL_REMOVED && spell.active != ACT_PASSIVE)
                    consider(spellId);
        }
        else
        {
            for (uint32 spellId : pet->m_spells)
                consider(spellId);
        }

        std::sort(found.begin(), found.end(), [](Ability const& a, Ability const& b)
        {
            return a.Best != b.Best ? a.Best < b.Best : a.Info->Id < b.Info->Id;
        });

        if (found.size() > PetBlock::ABILITY_SLOTS)
            found.resize(PetBlock::ABILITY_SLOTS);
        return found;
    }

    /// The unit an ability aims at: an enemy spell at the bot's target, a helpful one at the pet itself.
    Unit* AbilityTarget(SeatView const& view, Creature* pet, SpellInfo const* info)
    {
        return info->IsPositive() ? static_cast<Unit*>(pet) : view.Target;
    }

    /// A fresh spell of the pet validated as the client's pet bar is (HandlePetActionHelper); null if it cannot cast.
    Spell* PrepareAbility(Creature* pet, SpellInfo const* info, Unit* target)
    {
        if (!pet->IsAlive() || !target || pet->HasSpellCooldown(info->Id) || pet->IsNonMeleeSpellCast(false))
            return nullptr;

        CharmInfo* charmInfo = pet->GetCharmInfo();
        if (charmInfo && charmInfo->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            return nullptr;

        Spell* spell = new Spell(pet, info, TRIGGERED_NONE);
        spell->LoadScripts();
        SpellCastResult result = spell->CheckPetCast(target);
        if (result == SPELL_FAILED_UNIT_NOT_INFRONT)
            result = SPELL_CAST_OK;     // the client turns the pet

        if (result != SPELL_CAST_OK)
        {
            delete spell;
            return nullptr;
        }

        return spell;
    }

    /// The seat or its pet is in combat.
    bool Fighting(SeatView const& view, Creature const* pet)
    {
        return view.Bot->IsInCombat() || pet->IsInCombat();
    }

    bool IsAllowed(SeatView const& view, Creature* pet, std::vector<Ability> const& abilities, uint32 action)
    {
        if (!pet || !pet->IsAlive() || !view.Bot->IsAlive())
            return false;

        // A guardian takes no orders (it has no action bar of the owner's): nothing of it can be pressed, which is
        // what a player sees too. PetBlock::OBS_COMMANDABLE says so.
        CharmInfo* charmInfo = pet->GetCharmInfo();
        if (!charmInfo)
            return false;
        switch (action)
        {
            case PetBlock::ACTION_PASSIVE:
                return pet->GetReactState() != REACT_PASSIVE;
            case PetBlock::ACTION_DEFENSIVE:
                return pet->GetReactState() != REACT_DEFENSIVE;
            case PetBlock::ACTION_AGGRESSIVE:
                return pet->GetReactState() != REACT_AGGRESSIVE;
            // Not while fighting: follow and stay call the pet off its target, and stage1_duel's warlocks cycled
            // attack, follow and stay eight times a fight, their pets never landing a hit. Later stages may want
            // them back (positioning a pet in a group).
            case PetBlock::ACTION_FOLLOW:
                return charmInfo && !charmInfo->HasCommandState(COMMAND_FOLLOW) && !Fighting(view, pet);
            case PetBlock::ACTION_STAY:
                return charmInfo && !charmInfo->HasCommandState(COMMAND_STAY) && !Fighting(view, pet);
            default:
                break;
        }

        uint32 const slot = action - PetBlock::ACTION_ABILITY_FIRST;
        if (slot >= abilities.size())
            return false;

        SpellInfo const* info = abilities[slot].Info;
        Spell* spell = PrepareAbility(pet, info, AbilityTarget(view, pet, info));
        bool const castable = spell != nullptr;
        delete spell;
        return castable;
    }
}

static_assert(uint32(Animus::Curriculum::PetBlock::OBS_KIND_FIRST) + Animus::Curriculum::PetBlock::KIND_COUNT
    == Animus::Curriculum::PetBlock::OBS_TEMPORARY, "the pet kind one-hot ends where the temporary flag starts");

Animus::Curriculum::PetBlock::PetKind Animus::Curriculum::PetBlock::KindOf(Creature const* pet)
{
    enum : uint32
    {
        FAMILY_FELHUNTER        = 15,
        FAMILY_VOIDWALKER       = 16,
        FAMILY_SUCCUBUS         = 17,
        FAMILY_IMP              = 23,
        FAMILY_FELGUARD         = 29,
        FAMILY_GHOUL            = 40,
        NPC_WATER_ELEMENTAL     = 510,
        NPC_WATER_ELEMENTAL_GLYPH = 37994,   // Glyph of Eternal Water's
    };

    CreatureTemplate const* info = pet ? pet->GetCreatureTemplate() : nullptr;
    if (!info)
        return KIND_OTHER;

    if (info->Entry == NPC_WATER_ELEMENTAL || info->Entry == NPC_WATER_ELEMENTAL_GLYPH)
        return KIND_WATER_ELEMENTAL;

    switch (info->family)
    {
        case FAMILY_IMP:        return KIND_IMP;
        case FAMILY_VOIDWALKER: return KIND_VOIDWALKER;
        case FAMILY_SUCCUBUS:   return KIND_SUCCUBUS;
        case FAMILY_FELHUNTER:  return KIND_FELHUNTER;
        case FAMILY_FELGUARD:   return KIND_FELGUARD;
        case FAMILY_GHOUL:      return KIND_GHOUL;
        default:
            break;
    }

    // A hunter's beast: its talent tree (ferocity 0, tenacity 1, cunning 2).
    if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(info->family))
        if (family->petTalentType >= 0 && family->petTalentType <= 2)
            return PetKind(KIND_FEROCITY + uint32(family->petTalentType));

    return KIND_OTHER;
}

std::string Animus::Curriculum::PetBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT - ACTION_PASSIVE> ORDERS =
        { "pet_passive", "pet_defensive", "pet_aggressive", "pet_follow", "pet_stay" };
    static_assert(ORDERS.back() != nullptr, "every pet order needs a name");

    // Ability slots hold whatever the current pet has, most valuable kind first (Abilities).
    if (local < ACTION_PASSIVE)
        return Acore::StringFormat("pet_ability_{}", local);
    return local < ACTION_COUNT ? ORDERS[local - ACTION_PASSIVE] : std::string();
}

bool Animus::Curriculum::PetBlock::HasPet(uint8 playerClass)
{
    return playerClass == CLASS_HUNTER || playerClass == CLASS_WARLOCK || playerClass == CLASS_DEATH_KNIGHT
        || playerClass == CLASS_MAGE;
}

Creature* Animus::Curriculum::PetBlock::FindPet(Player* bot)
{
    if (!bot)
        return nullptr;

    if (Creature* pet = bot->GetGuardianPet())
        return pet;

    // A summon the core did not register as the owner's pet fights for the seat all the same: a ghoul raised without
    // Master of Ghouls, Army of the Dead, an Infernal, Feral Spirits. GetGuardianPet does not return any of them, so
    // the whole block used to read zeros while the thing dealt a tenth of the seat's damage (stage1_duel, 2026-09-17:
    // the death knight tank raised a ghoul in 90% of its fights and the policy never saw one). Orders stay masked --
    // it cannot be commanded -- but the policy can see it and what it is doing.
    if (Unit* first = Encoding::FirstPet(bot))
        return first->ToCreature();

    return nullptr;
}

void Animus::Curriculum::PetBlock::DefaultStance(Creature* pet, ObjectGuid& lastPet)
{
    if (!pet)
    {
        lastPet.Clear();
        return;
    }

    if (pet->GetGUID() == lastPet)
        return;

    lastPet = pet->GetGUID();
    if (pet->IsAlive() && pet->HasReactState(REACT_PASSIVE))
        pet->SetReactState(REACT_DEFENSIVE);
}

bool Animus::Curriculum::PetBlock::HasInterruptAbility(SeatView const& view)
{
    if (!view.L || !HasPet(view.L->Profile->Class))
        return false;

    Creature* pet = FindPet(view.Bot);
    if (!pet || !pet->IsAlive())
        return false;

    for (Ability const& ability : Abilities(pet))
        if (ability.Flags & (1 << KIND_INTERRUPT))
            return true;

    return false;
}

/// A held interrupt (SeatOptionKind::HoldInterrupt) the seat has no spell of its own for: the pet's. The core block
/// runs first and only leaves the hold standing when it found nothing to cast, so a seat with both uses its own
/// (stage2_pack 2026-09-18: the warlock pressed the hold 7.8 times a fight for 0.01 interrupts -- its interrupt is
/// the felhunter's Spell Lock, which the option never looked at).
void Animus::Curriculum::PetBlock::BeforeApply(SeatView& view, SeatActionResult& result) const
{
    if (!view.Option || !view.Option->Running(SeatOptionKind::HoldInterrupt, view.NowMs)
        || !HasPet(view.L->Profile->Class))
        return;

    Unit* target = view.Target;
    if (!target || !target->IsAlive() || !target->IsNonMeleeSpellCast(false))
        return;

    Creature* pet = FindPet(view.Bot);
    if (!pet)
        return;

    std::vector<Ability> const abilities = Abilities(pet);
    for (std::size_t slot = 0; slot < abilities.size(); ++slot)
    {
        if (!(abilities[slot].Flags & (1 << KIND_INTERRUPT)))
            continue;

        uint32 const action = ACTION_ABILITY_FIRST + uint32(slot);
        if (!IsAllowed(view, pet, abilities, action))
            continue;

        Apply(view, action, result);
        view.Option->Stop(SeatOptionKind::HoldInterrupt);
        return;
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::PetBlock::Size(Layout const& layout) const
{
    if (!HasPet(layout.Profile->Class))
        return {};

    return { OBS_SLOT_FIRST + ABILITY_SLOTS * SLOT_FEATURES, ACTION_COUNT };
}

void Animus::Curriculum::PetBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    if (!HasPet(view.L->Profile->Class))
        return;

    Creature* pet = FindPet(view.Bot);
    if (!pet)
        return;

    std::vector<Ability> const abilities = Abilities(pet);

    obs[OBS_PRESENT] = 1.0f;
    obs[OBS_ALIVE] = pet->IsAlive() ? 1.0f : 0.0f;
    obs[OBS_HEALTH] = pet->GetHealthPct() / 100.0f;
    Powers const power = pet->getPowerType();
    if (uint32 const maxPower = pet->GetMaxPower(power))
        obs[OBS_POWER] = float(pet->GetPower(power)) / float(maxPower);
    if (Unit* target = view.Target)
    {
        obs[OBS_TARGET_DISTANCE] = std::min(1.0f, pet->GetDistance(target) / 60.0f);
        obs[OBS_ATTACKING_TARGET] = pet->GetVictim() == target ? 1.0f : 0.0f;
    }
    obs[OBS_CASTING] = pet->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
    obs[OBS_REACT_FIRST + std::min<uint32>(uint32(pet->GetReactState()), 2)] = 1.0f;
    obs[uint32(OBS_KIND_FIRST) + KindOf(pet)] = 1.0f;

    // A temporary pet's time left: a controlled pet counts its own duration, a guardian its summon timer.
    uint32 msLeft = 0;
    if (Pet const* realPet = pet->ToPet(); realPet && realPet->isTemporarySummoned())
        msLeft = uint32(realPet->GetDuration().count());
    else if (TempSummon* summon = pet->ToTempSummon(); summon && !pet->ToPet() && summon->GetTimer())
        msLeft = summon->GetTimer();
    if (msLeft)
    {
        obs[OBS_TEMPORARY] = 1.0f;
        obs[OBS_TIME_LEFT] = std::min(1.0f, float(msLeft) / 60000.0f);
    }
    if (CharmInfo* charmInfo = pet->GetCharmInfo())
    {
        obs[OBS_COMMANDABLE] = 1.0f;
        obs[OBS_FOLLOWING] = charmInfo->HasCommandState(COMMAND_FOLLOW) ? 1.0f : 0.0f;
        obs[OBS_STAYING] = charmInfo->HasCommandState(COMMAND_STAY) ? 1.0f : 0.0f;
    }

    for (std::size_t slot = 0; slot < abilities.size(); ++slot)
    {
        float* features = obs + OBS_SLOT_FIRST + slot * SLOT_FEATURES;
        Ability const& ability = abilities[slot];
        features[SLOT_PRESENT] = 1.0f;
        features[SLOT_ON_COOLDOWN] = pet->HasSpellCooldown(ability.Info->Id) ? 1.0f : 0.0f;
        features[SLOT_INTERRUPT] = ability.Flags & (1 << KIND_INTERRUPT) ? 1.0f : 0.0f;
        features[SLOT_CONTROL] = ability.Flags & (1 << KIND_CONTROL) ? 1.0f : 0.0f;
        features[SLOT_DISPEL] = ability.Flags & (1 << KIND_DISPEL) ? 1.0f : 0.0f;
        features[SLOT_THREAT] = ability.Flags & (1 << KIND_THREAT) ? 1.0f : 0.0f;
        features[SLOT_POSITIVE] = ability.Flags & (1 << KIND_POSITIVE) ? 1.0f : 0.0f;
        features[SLOT_DAMAGE] = ability.Flags & (1 << KIND_DAMAGE) ? 1.0f : 0.0f;
    }

    for (uint32 action = 0; mask && action < ACTION_COUNT; ++action)
        mask[action] = IsAllowed(view, pet, abilities, action) ? 1 : 0;
}

void Animus::Curriculum::PetBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!HasPet(view.L->Profile->Class))
        return;

    Creature* pet = FindPet(view.Bot);
    std::vector<Ability> const abilities = pet ? Abilities(pet) : std::vector<Ability>();
    if (!IsAllowed(view, pet, abilities, local))
        return;

    CharmInfo* charmInfo = pet->GetCharmInfo();
    if (local >= ACTION_PASSIVE)
        ++result.PetOrders;

    switch (local)
    {
        case ACTION_PASSIVE:
            // HandlePetActionHelper, ACT_REACTION.
            pet->AttackStop();
            if (Pet* realPet = pet->ToPet())
                realPet->ClearCastWhenWillAvailable();
            pet->ClearInPetCombat();
            pet->SetReactState(REACT_PASSIVE);
            result.PetOrderGiven = PetOrder::Passive;
            return;
        case ACTION_DEFENSIVE:
            pet->SetReactState(REACT_DEFENSIVE);
            result.PetOrderGiven = PetOrder::Defensive;
            return;
        case ACTION_AGGRESSIVE:
            pet->SetReactState(REACT_AGGRESSIVE);
            result.PetOrderGiven = PetOrder::Aggressive;
            return;
        case ACTION_FOLLOW:
            // HandlePetActionHelper, COMMAND_FOLLOW.
            pet->AttackStop();
            pet->InterruptNonMeleeSpells(false);
            pet->ClearInPetCombat();
            pet->GetMotionMaster()->MoveFollow(view.Bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
            if (Pet* realPet = pet->ToPet())
                realPet->ClearCastWhenWillAvailable();
            charmInfo->SetCommandState(COMMAND_FOLLOW);
            charmInfo->SetIsCommandAttack(false);
            charmInfo->SetIsAtStay(false);
            charmInfo->SetIsReturning(true);
            charmInfo->SetIsCommandFollow(true);
            charmInfo->SetIsFollowing(false);
            charmInfo->RemoveStayPosition();
            charmInfo->SetForcedSpell(0);
            charmInfo->SetForcedTargetGUID();
            result.PetOrderGiven = PetOrder::Follow;
            return;
        case ACTION_STAY:
            // HandlePetActionHelper, COMMAND_STAY.
            pet->StopMovingOnCurrentPos();
            pet->GetMotionMaster()->Clear(false);
            pet->GetMotionMaster()->MoveIdle();
            charmInfo->SetCommandState(COMMAND_STAY);
            charmInfo->SetIsCommandAttack(false);
            charmInfo->SetIsCommandFollow(false);
            charmInfo->SetIsFollowing(false);
            charmInfo->SetIsReturning(false);
            charmInfo->SetIsAtStay(true);
            charmInfo->SaveStayPosition(false);
            if (Pet* realPet = pet->ToPet())
                realPet->ClearCastWhenWillAvailable();
            charmInfo->SetForcedSpell(0);
            charmInfo->SetForcedTargetGUID();
            result.PetOrderGiven = PetOrder::Stay;
            return;
        default:
            break;
    }

    // HandlePetActionHelper, ACT_ENABLED.
    SpellInfo const* info = abilities[local - ACTION_ABILITY_FIRST].Info;
    Unit* target = AbilityTarget(view, pet, info);
    Spell* spell = PrepareAbility(pet, info, target);
    if (!spell)
        return;

    if (!info->IsCooldownStartedOnEvent())
        pet->AddSpellCooldown(info->Id, 0, 0);

    if (Unit* unitTarget = spell->m_targets.GetUnitTarget(); unitTarget && !view.Bot->IsFriendlyTo(unitTarget)
        && pet->GetVictim() != unitTarget && pet->IsAIEnabled)
        pet->AI()->AttackStart(unitTarget);

    if (spell->prepare(&(spell->m_targets)) == SPELL_CAST_OK)
        ++result.PetAbilities;
}
