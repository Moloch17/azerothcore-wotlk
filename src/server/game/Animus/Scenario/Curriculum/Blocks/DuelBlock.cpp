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

#include "DuelBlock.h"
#include "DBCStores.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Supplies.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    enum DuelSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
        SPELL_RECENTLY_BANDAGED = 11196,
    };

    uint32 StableSlots(Layout const& layout)
    {
        return layout.Profile->Class == CLASS_HUNTER ? STABLE_SLOTS : 0;
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        Unit* target = view.Target;

        // Dead: only its own resurrection (Soulstone, Reincarnation), as the release dialog offers it.
        if (action == DuelBlock::ACTION_SELF_RESURRECT)
            return view.SelfResurrectAllowed && !bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL)
                && !bot->HasPreventResurectionAura();

        if (!bot->IsAlive())
            return false;

        bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

        // No target needed (between gauntlet pulls too).
        switch (action)
        {
            case DuelBlock::ACTION_STOP_CASTING:
                return casting;
            case DuelBlock::ACTION_CANCEL_FORM:
                return Encoding::CancellableForm(bot) != nullptr;
            case DuelBlock::ACTION_HEALTH_POTION:
                return Encoding::CanUseItemOn(bot, view.Supplies.HealthPotion, bot);
            case DuelBlock::ACTION_MANA_POTION:
                return Encoding::CanUseItemOn(bot, view.Supplies.ManaPotion, bot);
            case DuelBlock::ACTION_HEALTHSTONE:
                return Encoding::CanUseItemOn(bot, view.Supplies.Healthstone, bot);
            case DuelBlock::ACTION_BANDAGE:
                return Encoding::CanUseItemOn(bot, view.Supplies.Bandage, bot);
            case DuelBlock::ACTION_SOULSTONE_SELF:
            {
                uint32 const soulstone = view.Supplies.Soulstone;
                SpellInfo const* info = soulstone ? Encoding::UseSpell(soulstone) : nullptr;
                return info && !bot->HasAura(info->Id) && Encoding::CanUseItemOn(bot, soulstone, bot);
            }
            default:
                break;
        }

        // Calling a stable beast needs no target: a hunter calls its pet before a fight, or between pulls. Over a dead
        // pet too (CallHunterBeast dismisses the corpse).
        if (action >= DuelBlock::ACTION_CALL_BEAST_FIRST)
        {
            uint32 const slot = action - DuelBlock::ACTION_CALL_BEAST_FIRST;
            if (slot >= view.StableCount || casting || !CanCallHunterBeast(bot))
                return false;

            SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET);
            return !callPet || !bot->GetGlobalCooldownMgr().HasGlobalCooldown(callPet);
        }

        if (!target || !target->IsAlive())
            return false;

        switch (action)
        {
            case DuelBlock::ACTION_START_ATTACK:
                return bot->GetVictim() != target && bot->IsValidAttackTarget(target);
            case DuelBlock::ACTION_PET_ATTACK:
                return std::any_of(bot->m_Controlled.begin(), bot->m_Controlled.end(), [target](Unit* pet)
                {
                    return pet->IsAlive() && pet->IsCreature() && pet->GetVictim() != target;
                });
            default:
                break;
        }

        return false;
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::DuelBlock::Size(Layout const& layout) const
{
    uint32 const stable = StableSlots(layout);
    return { OBS_COUNT_WITHOUT_STABLE + stable * STABLE_FEATURES, ACTION_COUNT_WITHOUT_STABLE + stable };
}

std::string Animus::Curriculum::DuelBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT_WITHOUT_STABLE> NAMES =
    {
        "start_attack", "pet_attack", "stop_casting", "cancel_form", "health_potion", "mana_potion", "healthstone",
        "bandage", "soulstone_self", "self_resurrect"
    };
    // back() rather than size(): a short initialiser list still fills the declared length, with nulls.
    static_assert(NAMES.back() != nullptr, "every duel action needs a name");

    if (local < NAMES.size())
        return NAMES[local];
    return Acore::StringFormat("call_beast_{}", local - ACTION_CALL_BEAST_FIRST);
}

void Animus::Curriculum::DuelBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    block["stable_slots"] = StableSlots(layout);
    block["stable_features"] = STABLE_FEATURES;
}

void Animus::Curriculum::DuelBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    Unit* target = view.Target;

    // The bot's own casting and form, with or without a target.
    if (Spell* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        cast && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0)
    {
        float const total = float(cast->GetCastTime());
        float const left = std::clamp(float(cast->GetCastTimeRemaining()), 0.0f, total);
        obs[OBS_CAST_PROGRESS] = 1.0f - left / total;
        obs[OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }
    else if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        channel && channel->getState() == SPELL_STATE_CASTING)
    {
        float const left = float(std::max(0, channel->GetCastTimeRemaining()));
        float const total = std::max(left, float(std::max(1, channel->m_spellInfo->GetMaxDuration())));
        obs[OBS_CAST_PROGRESS] = 1.0f - left / total;
        obs[OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }

    // The ground it is standing on, whether or not it has a target: free, since a ground effect applies an aura to
    // whoever stands in it and the aura knows the object.
    Hazard hazard;
    if (uint32 const hazards = Encoding::StandingInHazards(bot, &hazard))
    {
        obs[OBS_HAZARDS_STANDING_IN] = std::min(1.0f, float(hazards) / 3.0f);
        if (hazard.Present)
        {
            obs[OBS_HAZARD_WAY_OUT] = std::min(1.0f, std::max(0.0f, hazard.Radius - hazard.Distance) / 20.0f);
            obs[OBS_HAZARD_BEARING_SIN] = std::sin(hazard.Bearing);
            obs[OBS_HAZARD_BEARING_COS] = std::cos(hazard.Bearing);
        }
    }

    // The nearest ground effect it is not in yet (StageScenario::TrackHazards), so it can be walked around.
    if (Hazard const& near = view.NearestHazard; near.Present)
    {
        obs[OBS_NEAR_HAZARD] = 1.0f;
        obs[OBS_NEAR_HAZARD_EDGE] = std::min(1.0f, std::max(0.0f, near.Distance - near.Radius) / 20.0f);
        obs[OBS_NEAR_HAZARD_BEARING_SIN] = std::sin(near.Bearing);
        obs[OBS_NEAR_HAZARD_BEARING_COS] = std::cos(near.Bearing);
        obs[OBS_NEAR_HAZARD_RADIUS] = std::min(1.0f, near.Radius / 20.0f);
    }

    // What has been done to the seat: the same aura list the hazards came from, read for what an enemy put on it.
    Encoding::Debuffs const debuffs = Encoding::IncomingDebuffs(bot);
    obs[OBS_DEBUFF_COUNT] = std::min(1.0f, float(debuffs.Count) / 5.0f);
    obs[OBS_DEBUFF_DISPELLABLE] = std::min(1.0f, float(debuffs.Dispellable) / 5.0f);
    obs[OBS_DEBUFF_STACKS] = std::min(1.0f, float(debuffs.Stacks) / 10.0f);
    obs[OBS_DEBUFF_LONGEST] = std::min(1.0f, float(debuffs.LongestMs) / 30000.0f);
    for (std::size_t mechanic = 0; mechanic < debuffs.Mechanics.size(); ++mechanic)
        obs[OBS_DEBUFF_MECHANIC_FIRST + mechanic] = debuffs.Mechanics[mechanic] ? 1.0f : 0.0f;

    obs[OBS_SHAPESHIFTED] = Encoding::CancellableForm(bot) ? 1.0f : 0.0f;
    obs[OBS_COMBAT_TIME] = view.CombatTime;

    obs[OBS_BOT_STUNNED] = bot->HasUnitState(UNIT_STATE_STUNNED) ? 1.0f : 0.0f;
    obs[OBS_BOT_FEARED] = bot->HasAuraType(SPELL_AURA_MOD_FEAR) || bot->HasAuraType(SPELL_AURA_MOD_CONFUSE)
        ? 1.0f : 0.0f;
    obs[OBS_BOT_ROOTED] = bot->HasAuraType(SPELL_AURA_MOD_ROOT) ? 1.0f : 0.0f;
    obs[OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE) || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE)
        ? 1.0f : 0.0f;
    obs[OBS_BOT_SNARED] = bot->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ? 1.0f : 0.0f;

    // What it carries, with or without a target.
    BattleSupplies const& supplies = view.Supplies;
    auto const carried = [bot](uint32 entry, float full)
    {
        return entry ? std::min(1.0f, float(bot->GetItemCount(entry)) / full) : 0.0f;
    };
    float const stack = float(CONSUMABLE_COUNT);
    obs[OBS_HEALTH_POTIONS] = carried(supplies.HealthPotion, stack);
    obs[OBS_MANA_POTIONS] = carried(supplies.ManaPotion, stack);
    obs[OBS_HEALTHSTONES] = carried(supplies.Healthstone, 1.0f);
    obs[OBS_BANDAGES] = carried(supplies.Bandage, stack);
    obs[OBS_POTION_COOLDOWN] = std::max(Encoding::ItemCooldownFraction(bot, supplies.HealthPotion),
        Encoding::ItemCooldownFraction(bot, supplies.ManaPotion));
    obs[OBS_HEALTHSTONE_COOLDOWN] = Encoding::ItemCooldownFraction(bot, supplies.Healthstone);
    obs[OBS_RECENTLY_BANDAGED] = bot->HasAura(SPELL_RECENTLY_BANDAGED) ? 1.0f : 0.0f;
    obs[OBS_SOULSTONE_ON_BOT] = view.SelfResurrectAllowed && bot->GetResurrectionSpellId() ? 1.0f : 0.0f;

    // Hunters: what each stable slot offers, so the policy can find the pet it prefers.
    for (uint32 slot = 0; slot < view.StableCount && slot < STABLE_SLOTS; ++slot)
    {
        CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(view.Stable[slot]);
        if (!beast)
            continue;

        float* features = obs + OBS_STABLE_FIRST + slot * STABLE_FEATURES;
        features[0] = 1.0f;
        features[1] = float(beast->family) / 50.0f;

        if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(beast->family))
            if (family->petTalentType >= 0 && family->petTalentType < 3)
                features[2 + family->petTalentType] = 1.0f;     // ferocity, tenacity, cunning
    }

    if (!target && view.HiddenTarget)
    {
        obs[OBS_TARGET_HIDDEN] = 1.0f;
        if (view.TargetSeen)
        {
            float const bearing = bot->GetRelativeAngle(&view.LastSeen);
            obs[OBS_TARGET_UNSEEN_TIME] = view.TargetUnseenTime;
            obs[OBS_LAST_SEEN_DISTANCE] = std::min(1.0f, bot->GetExactDist(&view.LastSeen) / 60.0f);
            obs[OBS_LAST_SEEN_BEARING_SIN] = std::sin(bearing);
            obs[OBS_LAST_SEEN_BEARING_COS] = std::cos(bearing);
        }

        // Its own state it still knows.
        obs[OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
        obs[OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_BOT_STEALTHED] = bot->HasStealthAura() ? 1.0f : 0.0f;
        obs[OBS_DAMAGE_TAKEN] = view.LastStepDamageTaken;
        if (Unit* pet = Encoding::FirstPet(bot))
        {
            obs[OBS_PET_OUT] = 1.0f;
            obs[OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
        }
    }

    if (target)
    {
        float const bearing = bot->GetRelativeAngle(target);
        obs[OBS_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 60.0f);
        obs[OBS_BEARING_SIN] = std::sin(bearing);
        obs[OBS_BEARING_COS] = std::cos(bearing);
        obs[OBS_BEHIND_TARGET] = target->isInBack(bot) ? 1.0f : 0.0f;
        obs[OBS_TARGET_FACING_BOT] = target->HasInArc(float(M_PI), bot) ? 1.0f : 0.0f;
        obs[OBS_TARGET_IN_COMBAT] = target->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_TARGET_ATTACKS_BOT] = target->GetVictim() == bot ? 1.0f : 0.0f;
        obs[OBS_TARGET_CASTING] = target->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        obs[OBS_TARGET_THREAT_SHARE] = Encoding::ThreatShare(target, bot);
        IncomingSpell::Observe(target, bot, obs + OBS_TARGET_CAST_FIRST);
        obs[OBS_TARGET_IN_LINE_OF_SIGHT] = bot->IsWithinLOSInMap(target) ? 1.0f : 0.0f;
        obs[OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
        obs[OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_BOT_STEALTHED] = bot->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
        obs[OBS_BOT_AUTO_ATTACKING] = bot->GetVictim() == target && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING)
            ? 1.0f : 0.0f;
        obs[OBS_DAMAGE_TAKEN] = view.LastStepDamageTaken;

        if (Unit* pet = Encoding::FirstPet(bot))
        {
            obs[OBS_PET_OUT] = 1.0f;
            obs[OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
            obs[OBS_PET_ATTACKING] = pet->GetVictim() == target ? 1.0f : 0.0f;
        }

        Encoding::WriteOpponentType(target, obs + OBS_TARGET_TYPE_FIRST);
        obs[OBS_TARGET_MAX_HEALTH] = std::min(1.0f,
            float(target->GetMaxHealth()) / float(std::max<uint32>(1, bot->GetMaxHealth())) / 4.0f);
        obs[OBS_TARGET_DAMAGE_MODIFIER] = Encoding::DamageModifier(target) / 2.0f;
        obs[OBS_TARGET_ARMOR] = Encoding::ArmorReduction(target, bot->GetLevel());
        obs[OBS_TARGET_RUN_SPEED] = target->GetSpeedRate(MOVE_RUN) / 2.0f;
        obs[OBS_TARGET_LEVEL_DIFFERENCE] = std::clamp((float(target->GetLevel()) - float(bot->GetLevel())) / 5.0f,
            -1.0f, 1.0f);
        for (uint32 i = 0; i < Encoding::OBSERVED_SCHOOLS.size(); ++i)
            obs[OBS_TARGET_IMMUNE_SCHOOL_FIRST + i] = Encoding::IsImmuneToSchool(target, Encoding::OBSERVED_SCHOOLS[i])
                ? 1.0f : 0.0f;
        for (uint32 i = 0; i < Encoding::OBSERVED_MECHANICS.size(); ++i)
            obs[OBS_TARGET_IMMUNE_MECHANIC_FIRST + i] =
                Encoding::IsImmuneToMechanic(target, Encoding::OBSERVED_MECHANICS[i]) ? 1.0f : 0.0f;
    }

    uint32 const actions = view.L->Slice(BlockId::Duel).ActionCount;
    for (uint32 action = 0; mask && action < actions; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::DuelBlock::ObserveDead(SeatView const& view, float* obs, uint8* mask)
{
    obs[OBS_DEAD] = 1.0f;
    bool const selfResurrect = IsAllowed(view, ACTION_SELF_RESURRECT);
    obs[OBS_SELF_RESURRECT] = selfResurrect ? 1.0f : 0.0f;
    if (mask)
        mask[ACTION_SELF_RESURRECT] = selfResurrect ? 1 : 0;
}

void Animus::Curriculum::DuelBlock::BeforeApply(SeatView& view, SeatActionResult& /*result*/) const
{
    // Face the target whenever not running somewhere: casts and swings need it, and turning is not a decision worth
    // learning. Written to the seat's own heading as well as to the unit, so the frame the move block measures
    // every bearing in agrees with where the seat is actually looking -- a strafe chosen after this used to be
    // measured off a heading the world had already overwritten.
    //
    // Nothing else happens here any more. This hook used to run the keep-range and stay-on-target options, and
    // to clear the positioning slot whenever there was no living target -- which, once the held bearing became a
    // positioning option, ended every bearing in every travel arena one decision after it was pressed: no target
    // there, ever. The slot is the move block's; the duel block does not touch it.
    Player* bot = view.Bot;
    Unit* target = view.Target;
    if (target && bot->IsAlive() && bot->movespline->Finalized() && !bot->HasInArc(float(M_PI) / 2, target))
    {
        bot->SetFacingToObject(target);
        view.Facing = bot->GetAngle(target);
    }
}

void Animus::Curriculum::DuelBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;
    Unit* target = view.Target;

    auto const useItem = [bot, &result](uint32 entry)
    {
        if (Encoding::UseItemOn(bot, entry, bot))
            ++result.ConsumablesUsed;
    };

    switch (local)
    {
        case ACTION_SELF_RESURRECT:
            // As CMSG_SELF_RES.
            bot->CastSpell(bot, bot->GetUInt32Value(PLAYER_SELF_RES_SPELL));
            bot->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);
            result.SelfResurrected = bot->IsAlive();
            return;
        case ACTION_HEALTH_POTION:
            useItem(view.Supplies.HealthPotion);
            return;
        case ACTION_MANA_POTION:
            useItem(view.Supplies.ManaPotion);
            return;
        case ACTION_HEALTHSTONE:
            useItem(view.Supplies.Healthstone);
            return;
        case ACTION_BANDAGE:
            useItem(view.Supplies.Bandage);
            return;
        case ACTION_SOULSTONE_SELF:
            useItem(view.Supplies.Soulstone);
            return;
        default:
            break;
    }

    switch (local)
    {
        case ACTION_START_ATTACK:
            bot->Attack(target, true);
            return;
        case ACTION_PET_ATTACK:
            if (Encoding::PetAttack(bot, target))
            {
                ++result.PetOrders;
                result.PetOrderGiven = PetOrder::Attack;
            }
            return;
        case ACTION_STOP_CASTING:
            // As CMSG_CANCEL_CAST / CMSG_CANCEL_CHANNELLING: the current cast or channel, cancelled by the caster.
            bot->InterruptNonMeleeSpells(false, 0, false, true);
            return;
        case ACTION_CANCEL_FORM:
            // As CMSG_CANCEL_AURA.
            if (SpellInfo const* form = Encoding::CancellableForm(bot))
                bot->RemoveOwnedAura(form->Id, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
            return;
        default:
            result.CallBeast = view.Stable[local - ACTION_CALL_BEAST_FIRST];
            return;
    }
}
