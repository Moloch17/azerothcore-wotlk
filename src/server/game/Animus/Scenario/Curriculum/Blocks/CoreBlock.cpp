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

#include "CoreBlock.h"
#include "EncoderSupport.h"
#include "Item.h"
#include "Layout.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "Player.h"
#include "SpellChecks.h"
#include "SeatMemory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StageDefinition.h"
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;
    using Animus::SpellChecks::GCD_MS;

    constexpr std::array<ShapeshiftForm, 13> TRACKED_FORMS =
    {
        FORM_NONE, FORM_CAT, FORM_TREE, FORM_BEAR, FORM_DIREBEAR, FORM_MOONKIN, FORM_SHADOW, FORM_STEALTH,
        FORM_BATTLESTANCE, FORM_DEFENSIVESTANCE, FORM_BERSERKERSTANCE, FORM_METAMORPHOSIS, FORM_GHOSTWOLF
    };

    constexpr float RUNE_COOLDOWN_MS = 10000.0f;
    /// The durative actions every seat can be running (SeatOptionKind without None, up to the companion's follow,
    /// which the companion block reports itself), and the scale their time left is reported on.
    constexpr uint32 OPTION_KINDS = uint32(SeatOptionKind::Follow) - 1;
    constexpr float OPTION_SCALE_MS = 30000.0f;
    constexpr float TALENT_POINTS_AT_MAX_LEVEL = 71.0f;

    bool IsActionAllowed(SeatView const& view, uint32 action)
    {
        ActionCatalog::Action const& def = view.L->Catalog().Actions()[action];
        Player* bot = view.Bot;

        switch (def.Type)
        {
            case ActionCatalog::Kind::Noop:
                return true;
            case ActionCatalog::Kind::Soulstone:
                return false;       // a revive (companion and party blocks), never a core action
            case ActionCatalog::Kind::CancelQueued:
                return bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr;
            case ActionCatalog::Kind::Trinket:
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
                SpellInfo const* info = Encoding::TrinketSpell(item);
                return info && !Encoding::CastInProgress(bot) && !bot->HasSpellCooldown(info->Id)
                    && Encoding::CanCast(bot, info, view.Target, item);
            }
            case ActionCatalog::Kind::Spell:
                break;
        }

        return Encoding::IsSpellActionAllowed(view, view.Target, def);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::CoreBlock::Size(Layout const& layout) const
{
    uint32 const actions = uint32(layout.Catalog().Actions().size());
    uint32 const talents = uint32(layout.Assets->Talents->Talents().size());
    return { OBS_GLOBAL_COUNT + actions * ACTION_FEATURES + talents + TalentBuilder::TREE_COUNT,
             actions + ACTION_RANK_TIERS };
}

uint32 Animus::Curriculum::CoreBlock::TalentObsFirst(Layout const& layout)
{
    return layout.Slice(BlockId::Core).ObsFirst + OBS_GLOBAL_COUNT
        + uint32(layout.Catalog().Actions().size()) * ACTION_FEATURES;
}

uint32 Animus::Curriculum::CoreBlock::TreeObsFirst(Layout const& layout)
{
    return TalentObsFirst(layout) + uint32(layout.Assets->Talents->Talents().size());
}

Animus::Curriculum::ModeGroup Animus::Curriculum::CoreBlock::ModeGroupOf(Layout const& layout, uint32 local) const
{
    std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
    // The rank to cast at is a standing choice like a stance or an aura, not an act: paced by ModeLockMs so it
    // cannot be churned. Without this a fresh policy hammered the three tiers -- they do nothing in the world, so
    // every press was waste the repeat charge then had to pay for (0.29M into the first run with them: 22 repeated
    // presses an episode against 5 for the scripted baseline, which never presses them).
    if (local >= actions.size())
        return ModeGroup::RankTier;

    SpellInfo const* info = local < actions.size() && actions[local].Type == ActionCatalog::Kind::Spell
        ? sSpellMgr->GetSpellInfo(actions[local].FirstRank) : nullptr;
    if (!info)
        return ModeGroup::None;

    switch (info->GetSpellSpecific())
    {
        case SPELL_SPECIFIC_ASPECT:             return ModeGroup::Aspect;
        case SPELL_SPECIFIC_AURA:               return ModeGroup::Aura;
        case SPELL_SPECIFIC_SEAL:               return ModeGroup::Seal;
        case SPELL_SPECIFIC_WARLOCK_ARMOR:
        case SPELL_SPECIFIC_MAGE_ARMOR:
        case SPELL_SPECIFIC_ELEMENTAL_SHIELD:   return ModeGroup::Armor;
        case SPELL_SPECIFIC_PRESENCE:           return ModeGroup::Form;
        default:
            break;
    }

    return info->HasAura(SPELL_AURA_MOD_SHAPESHIFT) ? ModeGroup::Form : ModeGroup::None;
}

std::string Animus::Curriculum::CoreBlock::ActionName(Layout const& layout, uint32 local) const
{
    std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
    if (local < actions.size())
        return actions[local].Name;

    static constexpr std::array<char const*, ACTION_RANK_TIERS> TIER_NAMES = { "rank_high", "rank_mid", "rank_low" };
    static_assert(TIER_NAMES.back() != nullptr, "every rank tier needs a name");
    uint32 const tier = local - uint32(actions.size());
    return tier < TIER_NAMES.size() ? TIER_NAMES[tier] : std::string();
}

void Animus::Curriculum::CoreBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    block["action_features"] = ACTION_FEATURES;

    boost::json::array& catalog = block["catalog"].emplace_array();
    for (ActionCatalog::Action const& action : layout.Catalog().Actions())
    {
        boost::json::object& entry = catalog.emplace_back(boost::json::object()).get_object();
        switch (action.Type)
        {
            case ActionCatalog::Kind::Noop:
                entry["kind"] = "noop";
                break;
            case ActionCatalog::Kind::CancelQueued:
                entry["kind"] = "cancel_queued";
                break;
            case ActionCatalog::Kind::Trinket:
                entry["kind"] = "trinket";
                entry["slot"] = action.EquipmentSlot;
                break;
            case ActionCatalog::Kind::Soulstone:
                entry["kind"] = "soulstone";
                break;
            case ActionCatalog::Kind::Spell:
                entry["kind"] = "spell";
                entry["first_rank"] = action.FirstRank;
                entry["next_swing"] = action.NextSwing;
                entry["group"] = action.From == ActionCatalog::Group::Tactical ? "tactical"
                    : action.From == ActionCatalog::Group::Sustain ? "sustain" : "combat";
                break;
        }
    }

    boost::json::array& talents = block["talents"].emplace_array();
    for (TalentBuilder::Talent const& talent : layout.Assets->Talents->Talents())
        talents.push_back(boost::json::array{ talent.TalentId, talent.MaxRank });
}

static_assert(uint32(Animus::Curriculum::CoreBlock::OBS_OPTION_FIRST) + OPTION_KINDS
    == uint32(Animus::Curriculum::CoreBlock::OBS_GLOBAL_COUNT), "every durative action needs its own clock");
static_assert(uint32(Animus::Curriculum::CoreBlock::OBS_APTITUDE_FIRST)
    + uint32(Animus::Curriculum::Aptitude::COUNT)
    == uint32(Animus::Curriculum::CoreBlock::OBS_HEALTH), "the aptitude vector has to fit where it is written");

bool Animus::Curriculum::CoreBlock::KnowsInterrupt(SeatView const& view)
{
    if (!view.L)
        return false;

    for (ActionCatalog::Action const& def : view.L->Catalog().Actions())
        if (def.Type == ActionCatalog::Kind::Spell
            && ActionCatalog::IsInterruptingSpell(Encoding::KnownRank(view, def)))
            return true;

    return false;
}

void Animus::Curriculum::CoreBlock::ObserveCharacter(SeatView const& view, float* obs)
{
    Layout const& layout = *view.L;
    float* core = obs + layout.Slice(BlockId::Core).ObsFirst;

    core[OBS_LEVEL] = float(view.Level) / float(DEFAULT_MAX_LEVEL);
    WriteOneHot(PLAYABLE_RACES, view.Race, core + OBS_RACE_FIRST);
    for (uint32 feature = 0; feature < Aptitude::COUNT; ++feature)
        core[OBS_APTITUDE_FIRST + feature] = view.Apt[feature];

    if (!view.Build)
        return;

    std::vector<TalentBuilder::Talent> const& talents = layout.Assets->Talents->Talents();
    float* talentObs = obs + TalentObsFirst(layout);
    for (uint32 i = 0; i < talents.size() && i < view.Build->Ranks.size(); ++i)
        talentObs[i] = float(view.Build->Ranks[i]) / float(std::max<uint8>(1, talents[i].MaxRank));

    float* treeObs = obs + TreeObsFirst(layout);
    for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
        treeObs[tree] = float(view.Build->TreePoints[tree]) / TALENT_POINTS_AT_MAX_LEVEL;
}

void Animus::Curriculum::CoreBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    Unit* target = view.Target;
    ObjectGuid const botGuid = bot->GetGUID();
    uint8 const level = bot->GetLevel();

    obs[OBS_HEALTH] = bot->GetHealthPct() / 100.0f;
    if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
        obs[OBS_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
    obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / 1000.0f;
    if (uint32 const maxEnergy = bot->GetMaxPower(POWER_ENERGY))
        obs[OBS_ENERGY] = float(bot->GetPower(POWER_ENERGY)) / float(maxEnergy);
    obs[OBS_RUNIC_POWER] = float(bot->GetPower(POWER_RUNIC_POWER)) / 1000.0f;

    if (bot->getClass() == CLASS_DEATH_KNIGHT)
        for (uint8 rune = 0; rune < MAX_RUNES; ++rune)
            obs[OBS_RUNE_FIRST + rune] = 1.0f - std::min(1.0f, float(bot->GetRuneCooldown(rune)) / RUNE_COOLDOWN_MS);

    if (target)
        obs[OBS_COMBO_POINTS] = float(bot->GetComboPoints(target)) / 5.0f;

    ShapeshiftForm const form = bot->GetShapeshiftForm();
    for (uint32 i = 0; i < TRACKED_FORMS.size(); ++i)
        obs[OBS_FORM_FIRST + i] = TRACKED_FORMS[i] == form ? 1.0f : 0.0f;

    obs[OBS_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
    obs[OBS_QUEUED_NEXT_SWING] = bot->GetCurrentSpell(CURRENT_MELEE_SPELL) ? 1.0f : 0.0f;

    for (auto const& [index, attack] : { std::pair{ OBS_MAIN_HAND_SWING, BASE_ATTACK },
        std::pair{ OBS_OFF_HAND_SWING, OFF_ATTACK }, std::pair{ OBS_RANGED_SWING, RANGED_ATTACK } })
    {
        if (uint32 const attackTime = bot->GetAttackTime(attack))
            obs[index] = std::clamp(float(bot->getAttackTimer(attack)) / float(attackTime), 0.0f, 1.0f);
    }

    obs[OBS_MAIN_HAND_SPEED] = float(bot->GetAttackTime(BASE_ATTACK)) / 4000.0f;
    if (target)
    {
        obs[OBS_TARGET_HEALTH] = target->GetHealthPct() / 100.0f;
        obs[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 40.0f);
        obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(target) && bot->HasInArc(2 * float(M_PI) / 3, target)
            ? 1.0f : 0.0f;
    }

    obs[OBS_ATTACK_POWER] = bot->GetTotalAttackPowerValue(BASE_ATTACK) / (100.0f + 50.0f * level);
    obs[OBS_SPELL_POWER] = float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC)) / (50.0f + 30.0f * level);
    obs[OBS_MELEE_CRIT] = bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE) / 100.0f;

    float spellCrit = 0.0f;
    for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
        spellCrit = std::max(spellCrit, bot->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school));
    obs[OBS_SPELL_CRIT] = spellCrit / 100.0f;

    obs[OBS_MELEE_HASTE] = bot->GetRatingBonusValue(CR_HASTE_MELEE) / 100.0f;
    obs[OBS_SPELL_HASTE] = bot->GetRatingBonusValue(CR_HASTE_SPELL) / 100.0f;
    obs[OBS_MELEE_HIT] = bot->GetRatingBonusValue(CR_HIT_MELEE) / 100.0f;
    obs[OBS_SPELL_HIT] = bot->GetRatingBonusValue(CR_HIT_SPELL) / 100.0f;
    obs[OBS_EXPERTISE] = float(bot->GetUInt32Value(PLAYER_EXPERTISE)) / 30.0f;
    obs[OBS_ARMOR_PENETRATION] = bot->GetRatingBonusValue(CR_ARMOR_PENETRATION) / 100.0f;
    obs[OBS_LAST_STEP_DAMAGE] = view.LastStepDamage;
    obs[OBS_LAST_STEP_POWER_DELTA] = view.LastStepPowerDelta;
    obs[OBS_EPISODE_TIME] = view.EpisodeTime;

    SeatMemory const* memory = view.Memory;
    if (memory)
    {
        obs[OBS_SINCE_MOVE] = memory->SinceMove(view.NowMs);
        obs[OBS_SINCE_MODE_CHANGE] = memory->SinceModeChange(view.NowMs);
        obs[OBS_HEALTH_TREND] = memory->SelfHealthTrend();
        obs[OBS_TARGET_HEALTH_TREND] = memory->TargetHealthTrend();
    }
    else
    {
        obs[OBS_SINCE_MOVE] = 1.0f;
        obs[OBS_SINCE_MODE_CHANGE] = 1.0f;
    }

    // The durative actions it is running, and how much of each one's clock is left.
    if (view.Option)
    {
        for (SeatOption const& option : view.Option->Slots)
        {
            if (option.Kind == SeatOptionKind::None || view.NowMs >= option.UntilMs)
                continue;

            uint32 const kind = uint32(option.Kind) - 1;
            if (kind < OPTION_KINDS)
                obs[OBS_OPTION_FIRST + kind] = std::min(1.0f, float(option.UntilMs - view.NowMs) / OPTION_SCALE_MS);
        }
    }

    std::vector<ActionCatalog::Action> const& actions = view.L->Catalog().Actions();
    for (uint32 action = 0; action < actions.size(); ++action)
    {
        SpellInfo const* info = nullptr;
        if (actions[action].Type == ActionCatalog::Kind::Spell)
            info = Encoding::KnownRank(view, actions[action]);
        else if (actions[action].Type == ActionCatalog::Kind::Trinket)
            info = Encoding::TrinketSpell(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, actions[action].EquipmentSlot));

        if (info)
        {
            float* features = obs + OBS_GLOBAL_COUNT + action * ACTION_FEATURES;
            float stacks = 0.0f;
            features[0] = 1.0f;
            features[1] = Animus::SpellChecks::CooldownFraction(bot, info);
            features[2] = target ? Animus::SpellChecks::AuraFraction(target, info->Id, botGuid, &stacks) : 0.0f;
            features[3] = Animus::SpellChecks::AuraFraction(bot, info->Id, botGuid, &stacks);
            features[4] = stacks;

            if (!obs[OBS_GCD] && info->StartRecoveryTime)
                obs[OBS_GCD] = std::min(1.0f, float(bot->GetGlobalCooldownMgr().GetGlobalCooldown(info)) / GCD_MS);
        }

        obs[OBS_GLOBAL_COUNT + action * ACTION_FEATURES + 5] = memory
            ? memory->SincePressed(view.L->Slice(BlockId::Core).ActionFirst + action, view.NowMs) : 1.0f;

        if (mask && action > 0)
            mask[action] = IsActionAllowed(view, action) ? 1 : 0;
    }

    // The rank tier to cast rankable spells at: always offered but the one already chosen, so a press is a change.
    if (mask)
        for (uint32 tier = 0; tier < ACTION_RANK_TIERS; ++tier)
            mask[actions.size() + tier] = view.Bot->IsAlive() && tier != view.RankTier ? 1 : 0;
}

void Animus::Curriculum::CoreBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    std::vector<ActionCatalog::Action> const& catalog = view.L->Catalog().Actions();
    if (local >= catalog.size())
    {
        // The rank to cast rankable spells at, until it is chosen again (EncoderSupport::KnownRank).
        view.RankTier = std::min(local - uint32(catalog.size()), ACTION_RANK_TIERS - 1);
        return;
    }

    if (local == 0)
        return;

    Player* bot = view.Bot;
    ActionCatalog::Action const& def = catalog[local];
    switch (def.Type)
    {
        case ActionCatalog::Kind::Noop:
        case ActionCatalog::Kind::Soulstone:
            return;
        case ActionCatalog::Kind::CancelQueued:
            if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
                bot->InterruptSpell(CURRENT_MELEE_SPELL);
            return;
        case ActionCatalog::Kind::Trinket:
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
            SpellInfo const* info = Encoding::TrinketSpell(item);
            if (!info || bot->HasSpellCooldown(info->Id))
                return;

            bot->CastItemUseSpell(item, Encoding::TargetsFor(info, bot, view.Target), 1, 0);
            if (bot->HasSpellCooldown(info->Id))
                ++(def.EquipmentSlot == EQUIPMENT_SLOT_TRINKET1 || def.EquipmentSlot == EQUIPMENT_SLOT_TRINKET2
                    ? result.TrinketUses : result.ItemUses);
            return;
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    if (Encoding::ApplySpellAction(view, view.Target, def, result) && def.From == ActionCatalog::Group::Sustain)
        ++result.SustainCasts;
}

void Animus::Curriculum::CoreBlock::BeforeApply(SeatView& view, SeatActionResult& result) const
{
    if (!view.Option || !view.Option->Running(SeatOptionKind::HoldInterrupt, view.NowMs))
        return;

    // Holding an interrupt is over once there is nothing left to interrupt.
    Player* bot = view.Bot;
    Unit* target = view.Target;
    if (!bot->IsAlive() || !target || !target->IsAlive())
    {
        view.Option->Stop(SeatOptionKind::HoldInterrupt);
        return;
    }

    // The moment the target casts, the first interrupt the seat has and may use goes off, and the hold is done: an
    // interrupt is spent, not held on.
    if (!target->IsNonMeleeSpellCast(false))
        return;

    std::vector<ActionCatalog::Action> const& actions = view.L->Catalog().Actions();
    for (uint32 action = 0; action < actions.size(); ++action)
    {
        ActionCatalog::Action const& def = actions[action];
        if (def.Type != ActionCatalog::Kind::Spell
            || !ActionCatalog::IsInterruptingSpell(Encoding::KnownRank(view, def))
            || !IsActionAllowed(view, action))
            continue;

        Apply(view, action, result);
        view.Option->Stop(SeatOptionKind::HoldInterrupt);
        return;
    }
}
