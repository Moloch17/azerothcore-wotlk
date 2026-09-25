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

#include "PvpBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "Player.h"
#include "Spell.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include <algorithm>
#include <array>

namespace
{
    enum PvpSpells : uint32
    {
        SPELL_EVERY_MAN_FOR_HIMSELF = 59752,
        SPELL_WILL_OF_THE_FORSAKEN  = 7744,
    };

    constexpr uint32 MAJOR_COOLDOWN_MS = 60000;
    constexpr float CC_LEFT_SCALE_MS = 8000.0f;

    constexpr std::array<DiminishingGroup, Animus::Curriculum::PvpBlock::DR_GROUP_COUNT> DR_GROUPS = {
        DIMINISHING_CONTROLLED_STUN, DIMINISHING_OPENING_STUN, DIMINISHING_FEAR, DIMINISHING_DISORIENT,
        DIMINISHING_CONTROLLED_ROOT, DIMINISHING_SILENCE, DIMINISHING_HORROR, DIMINISHING_CYCLONE,
    };

    constexpr std::array<AuraType, 8> CONTROL_AURAS = {
        SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE, SPELL_AURA_MOD_ROOT,
        SPELL_AURA_MOD_SILENCE, SPELL_AURA_MOD_PACIFY_SILENCE, SPELL_AURA_TRANSFORM, SPELL_AURA_MOD_PACIFY,
    };

    void WriteDiminishing(Unit* unit, float* out)
    {
        for (std::size_t i = 0; i < DR_GROUPS.size(); ++i)
            out[i] = std::min(1.0f, float(unit->GetDiminishing(DR_GROUPS[i])) / float(DIMINISHING_LEVEL_IMMUNE));
    }

    /// The longest crowd control on `unit` still running, / CC_LEFT_SCALE_MS.
    float ControlLeft(Unit const* unit)
    {
        int32 left = 0;
        for (AuraType type : CONTROL_AURAS)
            for (AuraEffect const* effect : unit->GetAuraEffectsByType(type))
                if (!effect->GetBase()->IsPassive() && !effect->GetSpellInfo()->IsPositive())
                    left = std::max(left, effect->GetBase()->GetDuration());

        return std::min(1.0f, float(left) / CC_LEFT_SCALE_MS);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::PvpBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, 0 };
}

void Animus::Curriculum::PvpBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    Player* bot = view.Bot;

    obs[OBS_BOT_STUNNED] = bot->HasUnitState(Encoding::STUN_STATES) ? 1.0f : 0.0f;
    obs[OBS_BOT_ROOTED] = bot->HasUnitState(UNIT_STATE_ROOT) ? 1.0f : 0.0f;
    obs[OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE) || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE)
        ? 1.0f : 0.0f;
    obs[OBS_MIRROR] = view.Mirror ? 1.0f : 0.0f;
    WriteDiminishing(bot, obs + OBS_BOT_DR_FIRST);
    obs[OBS_BOT_CC_LEFT] = ControlLeft(bot);

    Player* opponent = view.Opponent;
    if (!opponent)
        return;

    WriteDiminishing(opponent, obs + OBS_OPPONENT_DR_FIRST);

    WriteOneHot(PLAYABLE_CLASSES, view.OpponentClass, obs + OBS_OPPONENT_CLASS_FIRST);
    view.OpponentApt.WriteBrief(obs + OBS_OPPONENT_APTITUDE_FIRST);
    obs[OBS_OPPONENT_LEVEL_DIFF] = (float(opponent->GetLevel()) - float(bot->GetLevel())) / 5.0f;

    for (uint8 slot : { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 })
        if (SpellInfo const* use = Encoding::TrinketSpell(opponent->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            obs[OBS_OPPONENT_TRINKET_CD] = std::max(obs[OBS_OPPONENT_TRINKET_CD],
                Animus::SpellChecks::CooldownFraction(opponent, use));

    for (uint32 racial : { SPELL_EVERY_MAN_FOR_HIMSELF, SPELL_WILL_OF_THE_FORSAKEN })
        if (opponent->HasSpell(racial))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(racial))
                obs[OBS_OPPONENT_BREAK_CD] = Animus::SpellChecks::CooldownFraction(opponent, info);

    // The map holds cooldowns that ended but were not cleaned up yet, so ask the core whether each is still running:
    // it compares against the clock cooldowns run on (the sim clock in the forge core, the wall clock in a stock one).
    uint32 majors = 0;
    for (auto const& [spellId, cooldown] : opponent->GetSpellCooldownMap())
        if (!cooldown.itemid && cooldown.maxduration >= MAJOR_COOLDOWN_MS && opponent->GetSpellCooldownDelay(spellId))
            ++majors;
    obs[OBS_OPPONENT_MAJOR_CDS] = std::min(1.0f, float(majors) / 4.0f);

    if (view.OpponentHidden)
    {
        obs[OBS_OPPONENT_HIDDEN] = 1.0f;
        return;
    }

    if (uint32 const maxMana = opponent->GetMaxPower(POWER_MANA))
        obs[OBS_OPPONENT_MANA] = float(opponent->GetPower(POWER_MANA)) / float(maxMana);

    Powers const power = opponent->getPowerType();
    if (power != POWER_MANA)
        if (uint32 const maxPower = opponent->GetMaxPower(power))
            obs[OBS_OPPONENT_RAGE_ENERGY] = float(opponent->GetPower(power)) / float(maxPower);

    obs[OBS_OPPONENT_CONTROLLED] = Encoding::IsCrowdControlled(opponent) ? 1.0f : 0.0f;
    obs[OBS_OPPONENT_CC_LEFT] = ControlLeft(opponent);
    obs[OBS_OPPONENT_STEALTHED] = opponent->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
    obs[OBS_OPPONENT_PET_OUT] = opponent->GetPetGUID() || !opponent->m_Controlled.empty() ? 1.0f : 0.0f;

    if (Spell const* cast = opponent->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (cast->m_spellInfo->HasEffect(SPELL_EFFECT_HEAL) || cast->m_spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
            obs[OBS_OPPONENT_HEALING] = 1.0f;
}
