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

#include "TeacherSpells.h"
#include "ClassAssets.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

/*
 * The spell picks the teacher's choices name (WingTeacher::Spell), as the removed `fight` baseline made them: the
 * first allowed core action whose spell does the thing, in catalog order.
 */

namespace
{
    using namespace Animus::Curriculum;

    /// A seat's row, read by block: features and actions by their block-relative index.
    class Row
    {
    public:
        Row(Layout const& layout, float const* obs, uint8 const* mask) : _layout(layout), _obs(obs), _mask(mask) { }

        [[nodiscard]] bool Has(BlockId block) const { return _layout.Has(block); }

        [[nodiscard]] float Obs(BlockId block, uint32 feature) const
        {
            return _obs[_layout.Slice(block).ObsFirst + feature];
        }

        /// The row's action for `action` of `block`, if the layout has it and it is allowed.
        [[nodiscard]] std::optional<int32> Allowed(BlockId block, uint32 action) const
        {
            if (!Has(block))
                return std::nullopt;
            BlockSlice const& slice = _layout.Slice(block);
            if (action >= slice.ActionCount || !_mask[slice.ActionFirst + action])
                return std::nullopt;
            return int32(slice.ActionFirst + action);
        }

    private:
        Layout const& _layout;
        float const* _obs;
        uint8 const* _mask;
    };

    /// The summons a pet class casts when its pet is not out, best first.
    constexpr std::array<uint32, 7> PET_SUMMONS =
    {
        30146,  // Summon Felguard
        697,    // Summon Voidwalker
        691,    // Summon Felhunter
        712,    // Summon Succubus
        688,    // Summon Imp
        46584,  // Raise Dead
        31687,  // Summon Water Elemental
    };
    constexpr uint32 CALL_PET = 883;    // a hunter's Call Pet (its first stable beast)

    /// The core block's per-action features: aura on the target, aura on the seat.
    constexpr uint32 ACTION_AURA_ON_TARGET = 2;
    constexpr uint32 ACTION_AURA_ON_SELF = 3;

    float ActionFeature(Row const& row, uint32 action, uint32 feature)
    {
        return row.Obs(BlockId::Core, CoreBlock::OBS_GLOBAL_COUNT + action * CoreBlock::ACTION_FEATURES + feature);
    }

    SpellInfo const* SpellOf(ActionCatalog::Action const& action)
    {
        return action.Type == ActionCatalog::Kind::Spell ? sSpellMgr->GetSpellInfo(action.FirstRank) : nullptr;
    }

    enum class SpellUse : uint8
    {
        Other,
        Damage,             // hits the target now
        DamageOverTime,     // only ticks: worth casting while it isn't on the target
        Buff,               // an aura on the seat or its party with no cooldown of its own
        Form,               // a shapeshift: only the spec's own
    };

    SpellUse UseOf(SpellInfo const* info)
    {
        if (!info)
            return SpellUse::Other;
        if (info->HasAura(SPELL_AURA_MOD_SHAPESHIFT))
            return SpellUse::Form;
        if (!info->IsPositive())
        {
            // Crowd control that damage breaks would undo the fight's own hits.
            if (info->HasAura(SPELL_AURA_MOD_CONFUSE) || info->HasAura(SPELL_AURA_MOD_FEAR)
                || info->HasAura(SPELL_AURA_TRANSFORM))
                return SpellUse::Other;
            bool direct = info->DmgClass == SPELL_DAMAGE_CLASS_MELEE || info->DmgClass == SPELL_DAMAGE_CLASS_RANGED;
            bool periodic = false;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                switch (effect.Effect)
                {
                    case SPELL_EFFECT_SCHOOL_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                    case SPELL_EFFECT_HEALTH_LEECH:
                        direct = true;
                        break;
                    default:
                        break;
                }
                switch (effect.ApplyAuraName)
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_LEECH:
                    case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                        periodic = true;
                        break;
                    default:
                        break;
                }
            }
            return direct ? SpellUse::Damage : periodic ? SpellUse::DamageOverTime : SpellUse::Other;
        }
        // A buff to keep up, cast once: not a cooldown, not speed, not stealth or invisibility, not feigning death.
        bool const aura = info->HasEffect(SPELL_EFFECT_APPLY_AURA)
            || info->HasEffect(SPELL_EFFECT_APPLY_AREA_AURA_PARTY)
            || info->HasEffect(SPELL_EFFECT_APPLY_AREA_AURA_RAID);
        if (!aura || info->RecoveryTime || info->CategoryRecoveryTime || info->HasAura(SPELL_AURA_MOD_INCREASE_SPEED)
            || info->HasAura(SPELL_AURA_MOD_STEALTH) || info->HasAura(SPELL_AURA_MOD_INVISIBILITY)
            || info->HasAura(SPELL_AURA_FEIGN_DEATH))
            return SpellUse::Other;
        return SpellUse::Buff;
    }

    struct SpecForm
    {
        uint8 Class;
        std::string_view Spec;
        std::array<uint32, 2> Forms;
    };

    constexpr std::array<SpecForm, 4> SPEC_FORMS =
    {{
        { CLASS_DRUID, "balance", { 24858, 0 } },           // Moonkin Form
        { CLASS_DRUID, "feral_cat", { 768, 0 } },           // Cat Form
        { CLASS_DRUID, "feral_bear", { 9634, 5487 } },      // Dire Bear Form, Bear Form
        { CLASS_PRIEST, "shadow", { 15473, 0 } },           // Shadowform
    }};

    bool IsSpell(std::vector<ActionCatalog::Action> const& actions, uint32 action)
    {
        return actions[action].Type == ActionCatalog::Kind::Spell;
    }

    /// The spec's own form, while in no form (the tracked forms' one-hot starts with FORM_NONE).
    std::optional<int32> SpecFormSpell(Row const& row, Layout const& layout, SpecProfile const* spec)
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        if (!spec || !layout.Profile || row.Obs(BlockId::Core, CoreBlock::OBS_FORM_FIRST) <= 0.0f)
            return std::nullopt;
        for (SpecForm const& entry : SPEC_FORMS)
        {
            if (entry.Class != layout.Profile->Class || entry.Spec != spec->Name)
                continue;
            for (uint32 form : entry.Forms)
                for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
                    if (form && IsSpell(actions, action) && actions[action].FirstRank == form)
                        if (std::optional<int32> shift = row.Allowed(BlockId::Core, action))
                            return shift;
        }
        return std::nullopt;
    }

    /// The first allowed damaging spell (one that only ticks while it isn't on the target).
    std::optional<int32> DamageSpell(Row const& row, Layout const& layout)
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
        {
            if (!IsSpell(actions, action))
                continue;
            SpellUse const use = UseOf(sSpellMgr->GetSpellInfo(actions[action].FirstRank));
            if (use == SpellUse::Damage
                || (use == SpellUse::DamageOverTime && ActionFeature(row, action, ACTION_AURA_ON_TARGET) == 0.0f))
                if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                    return cast;
        }
        return std::nullopt;
    }

    /// Out of combat, a buff not already on the seat, one per exclusive kind (a seal, an aura, an armor).
    std::optional<int32> BuffSpell(Row const& row, Layout const& layout)
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        std::vector<SpellSpecificType> active;
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
            if (IsSpell(actions, action) && ActionFeature(row, action, ACTION_AURA_ON_SELF) > 0.0f)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(actions[action].FirstRank))
                    if (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL)
                        active.push_back(info->GetSpellSpecific());
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
        {
            if (!IsSpell(actions, action) || ActionFeature(row, action, ACTION_AURA_ON_SELF) > 0.0f)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(actions[action].FirstRank);
            if (UseOf(info) != SpellUse::Buff || (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL
                && std::find(active.begin(), active.end(), info->GetSpellSpecific()) != active.end()))
                continue;
            if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                return cast;
        }
        return std::nullopt;
    }

    template <typename Wanted>
    std::optional<int32> FirstSpell(Row const& row, Layout const& layout, Wanted wanted)
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
            if (IsSpell(actions, action) && wanted(actions[action]))
                if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                    return cast;
        return std::nullopt;
    }

    bool IsTaunt(ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action);
        return info && (info->HasEffect(SPELL_EFFECT_ATTACK_ME) || info->HasAura(SPELL_AURA_MOD_TAUNT));
    }

    bool IsInterrupt(ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action);
        return info && info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST);
    }

    bool IsAreaThreat(ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action);
        return info && !info->IsPositive() && info->IsAffectingArea() && !info->HasAura(SPELL_AURA_MOD_FEAR)
            && !info->HasAura(SPELL_AURA_MOD_CONFUSE);
    }

    bool IsHighThreat(ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action);
        if (!info || info->IsPositive())
            return false;
        SpellThreatEntry const* threat = sSpellMgr->GetSpellThreatEntry(info->GetFirstRankSpell()->Id);
        return threat && (threat->flatMod > 0 || threat->pctMod > 1.0f || threat->apPctMod > 0.0f);
    }

    bool IsCrowdControl(ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action);
        if (!info || info->IsPositive() || info->IsAffectingArea() || info->HasAura(SPELL_AURA_MOD_FEAR))
            return false;
        return info->HasAura(SPELL_AURA_MOD_CONFUSE) || info->HasAura(SPELL_AURA_TRANSFORM)
            || (info->HasAura(SPELL_AURA_MOD_STUN) && info->GetMaxDuration() >= 8000);
    }

    /// Something to pull with from range: a hostile spell or shot that reaches 20 yd or more and is cast quickly.
    bool IsRangedPull(ActionCatalog::Action const& action)
    {
        SpellInfo const* info = SpellOf(action);
        if (!info || info->IsPositive() || info->IsAffectingArea() || info->GetMaxRange(false) < 20.0f
            || info->CalcCastTime() > 2000 || IsCrowdControl(action) || info->HasAura(SPELL_AURA_MOD_FEAR))
            return false;
        return info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) || info->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE)
            || info->HasEffect(SPELL_EFFECT_ATTACK_ME) || info->HasAura(SPELL_AURA_PERIODIC_DAMAGE)
            || info->DmgClass == SPELL_DAMAGE_CLASS_RANGED;
    }
}

std::optional<int32> Animus::Curriculum::TeacherSpellFor(WingTeacher::Spell spell, Layout const& layout,
    SpecProfile const* spec, float const* obs, uint8 const* mask)
{
    using Spell = WingTeacher::Spell;
    if (!layout.Has(BlockId::Core) || !obs || !mask)
        return std::nullopt;
    Row const row(layout, obs, mask);
    switch (spell)
    {
        case Spell::Damage:
            return DamageSpell(row, layout);
        case Spell::Taunt:
            return FirstSpell(row, layout, IsTaunt);
        case Spell::AreaThreat:
            return FirstSpell(row, layout, IsAreaThreat);
        case Spell::HighThreat:
            return FirstSpell(row, layout, IsHighThreat);
        case Spell::RangedPull:
            return FirstSpell(row, layout, IsRangedPull);
        case Spell::Interrupt:
            return FirstSpell(row, layout, IsInterrupt);
        case Spell::Heal:
            // A plain heal first; a heal over time or a shield where that is all it has.
            if (std::optional<int32> heal = FirstSpell(row, layout,
                [](ActionCatalog::Action const& action) { return action.DirectHeal; }))
                return heal;
            return FirstSpell(row, layout, [](ActionCatalog::Action const& action) { return action.Healing; });
        case Spell::Defensive:
            return FirstSpell(row, layout, [](ActionCatalog::Action const& action) { return action.Defensive; });
        case Spell::Buff:
            if (row.Has(BlockId::Duel) && row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_IN_COMBAT) > 0.0f)
                return std::nullopt;
            if (std::optional<int32> shift = SpecFormSpell(row, layout, spec))
                return shift;
            return BuffSpell(row, layout);
        case Spell::Summon:
        {
            if (std::optional<int32> revive = FirstSpell(row, layout, [](ActionCatalog::Action const& action)
                {
                    SpellInfo const* info = SpellOf(action);
                    return info && info->HasEffect(SPELL_EFFECT_RESURRECT_PET);
                }))
                return revive;
            std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
            for (uint32 summon : PET_SUMMONS)
                for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
                    if (IsSpell(actions, action) && actions[action].FirstRank == summon)
                        if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                            return cast;
            return FirstSpell(row, layout, [](ActionCatalog::Action const& action)
            {
                return action.Type == ActionCatalog::Kind::Spell && action.FirstRank == CALL_PET;
            });
        }
        case Spell::None:
            break;
    }
    return std::nullopt;
}
