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
#include "SupportBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>
#include <string>
#include <boost/json/object.hpp>

namespace
{
    using namespace Animus::Curriculum;

    /// A friend slot's name, built rather than listed: the teammate slots are the seat's own group and then the
    /// spotlights outside it (PartyEncounter::View), and they follow PARTY_MEMBERS, which follows the raid's shape.
    /// A fixed list silently filled its tail with null pointers when FRIEND_SLOTS grew -- and a static_assert on the
    /// list's own size cannot catch that, because the size is declared as FRIEND_SLOTS and the missing entries are
    /// value-initialised. The manifest builds a std::string from every action name, so each null was a crash.
    std::string FriendName(uint32 slot)
    {
        if (slot == FRIEND_SELF)
            return "friend_self";
        if (slot == FRIEND_OWNER)
            return "friend_owner";

        uint32 const member = slot - FRIEND_TEAMMATE_FIRST;
        return member < GROUP_MEMBERS ? Acore::StringFormat("friend_group_{}", member)
            : Acore::StringFormat("friend_spotlight_{}", member - GROUP_MEMBERS);
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        if (!view.Bot->IsAlive() || action >= SupportBlock::ACTION_COUNT)
            return false;

        Unit* other = Encoding::FriendUnit(view, action);
        return action != view.FriendSlot && other && other->IsAlive();
    }

    /// The bot's own aura of `type` on `unit`: its duration left as a fraction (1 for a permanent one), or 0.
    float OwnAuraLeft(Unit const* unit, AuraType type, ObjectGuid caster)
    {
        float left = 0.0f;
        for (AuraEffect const* effect : unit->GetAuraEffectsByType(type))
        {
            if (effect->GetCasterGUID() != caster)
                continue;

            Aura const* aura = effect->GetBase();
            left = std::max(left, aura->GetMaxDuration() > 0
                ? float(aura->GetDuration()) / float(aura->GetMaxDuration()) : 1.0f);
        }

        return std::clamp(left, 0.0f, 1.0f);
    }

    /// What the unit in friend slot `slot` can do, if the seat knows.
    std::optional<Aptitude> AptitudeOf(SeatView const& view, uint32 slot)
    {
        if (slot == FRIEND_SELF)
            return view.Apt;
        if (slot == FRIEND_OWNER)
            return view.OwnerApt;
        return view.Teammates[slot - FRIEND_TEAMMATE_FIRST].Bot
            ? std::optional<Aptitude>(view.Teammates[slot - FRIEND_TEAMMATE_FIRST].Apt) : std::nullopt;
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::SupportBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_GLOBAL_COUNT + FRIEND_SLOTS * FRIEND_FEATURES, ACTION_COUNT };
}

void Animus::Curriculum::SupportBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    block["friend_slots"] = FRIEND_SLOTS;
    block["friend_features"] = uint32(FRIEND_FEATURES);
    block["rank_tiers"] = RANK_TIERS;
    block["buff_groups"] = uint32(layout.BuffGroups.size());
}

float Animus::Curriculum::SupportBlock::BuffCoverage(Layout const& layout, Unit const* unit)
{
    if (layout.BuffGroups.empty())
        return 1.0f;

    uint32 up = 0;
    for (std::vector<uint32> const& group : layout.BuffGroups)
        if (std::any_of(group.begin(), group.end(), [unit](uint32 spellId) { return unit->HasAura(spellId); }))
            ++up;

    return float(up) / float(layout.BuffGroups.size());
}

void Animus::Curriculum::SupportBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    obs[OBS_SELECTED_FIRST + std::min(view.FriendSlot, FRIEND_SLOTS - 1)] = 1.0f;
    obs[OBS_RANK_TIER_FIRST + std::min(view.RankTier, RANK_TIERS - 1)] = 1.0f;

    for (uint32 slot = 0; slot < FRIEND_SLOTS; ++slot)
    {
        Unit* other = Encoding::FriendUnit(view, slot);
        if (!other)
            continue;

        float* features = obs + OBS_GLOBAL_COUNT + slot * FRIEND_FEATURES;
        features[FRIEND_PRESENT] = 1.0f;
        features[FRIEND_ALIVE] = other->IsAlive() ? 1.0f : 0.0f;
        features[FRIEND_HEALTH] = other->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = other->GetMaxPower(POWER_MANA))
            features[FRIEND_MANA] = float(other->GetPower(POWER_MANA)) / float(maxMana);
        if (other != bot)
        {
            features[FRIEND_DISTANCE] = std::min(1.0f, bot->GetDistance(other) / 40.0f);
            features[FRIEND_IN_LINE_OF_SIGHT] = bot->IsWithinLOSInMap(other) ? 1.0f : 0.0f;
        }
        else
            features[FRIEND_IN_LINE_OF_SIGHT] = 1.0f;

        uint32 attackers = 0;
        for (uint32 enemySlot = 0; enemySlot < view.EnemyCount; ++enemySlot)
            if (Unit* enemy = view.Enemies[enemySlot]; enemy && enemy->IsAlive() && enemy->GetVictim() == other)
                ++attackers;
        if (view.Opponent && view.Opponent->IsAlive() && view.Opponent->GetVictim() == other && !view.EnemyCount)
            ++attackers;
        features[FRIEND_ATTACKERS] = std::min(1.0f, float(attackers) / float(PACK_SLOTS));

        if (std::optional<Aptitude> aptitude = AptitudeOf(view, slot))
            aptitude->WriteBrief(features + FRIEND_APTITUDE_FIRST);

        if (other->IsAlive())
        {
            features[FRIEND_OWN_HEAL_OVER_TIME] = OwnAuraLeft(other, SPELL_AURA_PERIODIC_HEAL, bot->GetGUID());
            features[FRIEND_OWN_ABSORB] = OwnAuraLeft(other, SPELL_AURA_SCHOOL_ABSORB, bot->GetGUID());
            features[FRIEND_BUFFS] = BuffCoverage(*view.L, other);
        }
    }

    for (uint32 action = 0; mask && action < ACTION_COUNT; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::SupportBlock::Apply(SeatView& view, uint32 local, SeatActionResult& /*result*/) const
{
    if (!IsAllowed(view, local))
        return;

    view.FriendSlot = local;
}

std::string Animus::Curriculum::SupportBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    return local < ACTION_COUNT ? FriendName(local) : std::string();
}
