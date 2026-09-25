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

#include "PartyBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "Player.h"
#include "StringFormat.h"
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    constexpr uint32 FOLLOW_TANK_MOVE_POINT_ID = 4;
    constexpr float FOLLOW_TANK_DISTANCE = 4.0f;
    constexpr float FOLLOW_TANK_MIN_DISTANCE = 8.0f;

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        if (!bot->IsAlive())
            return false;

        if (action == PartyBlock::ACTION_FOLLOW_TANK)
        {
            Player* tank = view.Tank;
            return tank && tank != bot && !bot->IsNonMeleeSpellCast(false, false, true)
                && !bot->HasUnitState(Encoding::IMMOBILE_STATES) && bot->GetDistance(tank) > FOLLOW_TANK_MIN_DISTANCE;
        }

        if (action < PartyBlock::ACTION_GUARD_FIRST)
        {
            Player* teammate = view.Teammates[action - PartyBlock::ACTION_ASSIST_FIRST].Bot;
            if (!teammate || !teammate->IsAlive())
                return false;

            int32 const slot = Encoding::SlotOf(view, teammate->GetVictim());
            return slot >= 0 && uint32(slot) != view.TargetSlot && teammate->GetVictim()->IsAlive();
        }

        if (action < PartyBlock::ACTION_REVIVE_FIRST)
        {
            Player* teammate = view.Teammates[action - PartyBlock::ACTION_GUARD_FIRST].Bot;
            return teammate && teammate->IsAlive() && Encoding::SlotAttacking(view, teammate, view.TargetSlot) >= 0;
        }

        uint32 const revives = uint32(view.L->AllyRevives.size());
        uint32 const reviveIndex = action - PartyBlock::ACTION_REVIVE_FIRST;
        return revives && reviveIndex / revives < PARTY_MEMBERS
            && Encoding::CanRevive(view, view.L->AllyRevives[reviveIndex % revives],
                view.Teammates[reviveIndex / revives].Bot);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::PartyBlock::Size(Layout const& layout) const
{
    return { OBS_GLOBAL_COUNT + PARTY_MEMBERS * MEMBER_FEATURES,
        ACTION_REVIVE_FIRST + PARTY_MEMBERS * uint32(layout.AllyRevives.size()) };
}

void Animus::Curriculum::PartyBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["members"] = PARTY_MEMBERS;
    block["member_features"] = uint32(MEMBER_FEATURES);
}

std::string Animus::Curriculum::PartyBlock::ActionName(Layout const& layout, uint32 local) const
{
    // A slot is either the seat's own group or one of the spotlights outside it (PartyEncounter::View), and the
    // name says which, so an evaluation's action counts can tell "healed my group" from "healed the main tank".
    auto const slotName = [](uint32 member)
    {
        return member < GROUP_MEMBERS ? Acore::StringFormat("group_{}", member)
            : Acore::StringFormat("spotlight_{}", member - GROUP_MEMBERS);
    };

    if (local == ACTION_FOLLOW_TANK)
        return "follow_tank";
    if (local < ACTION_GUARD_FIRST)
        return "assist_" + slotName(local - ACTION_ASSIST_FIRST);
    if (local < ACTION_REVIVE_FIRST)
        return "guard_" + slotName(local - ACTION_GUARD_FIRST);

    uint32 const revives = uint32(layout.AllyRevives.size());
    if (!revives)
        return {};

    uint32 const index = local - ACTION_REVIVE_FIRST;
    uint32 const member = index / revives;
    return member < PARTY_MEMBERS ? Acore::StringFormat("revive_{}_{}", slotName(member), index % revives)
        : std::string();
}

void Animus::Curriculum::PartyBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;

    uint32 alive = bot->IsAlive() ? 1 : 0;
    float lowest = 1.0f;
    if (Player* owner = view.Owner; owner && owner->IsAlive())
    {
        ++alive;
        lowest = std::min(lowest, owner->GetHealthPct() / 100.0f);
    }

    for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
    {
        SeatView::Teammate const& other = view.Teammates[member];
        Player* teammate = other.Bot;
        if (!teammate)
            continue;

        float* features = obs + OBS_GLOBAL_COUNT + member * MEMBER_FEATURES;
        float const bearing = bot->GetRelativeAngle(teammate);

        features[MEMBER_PRESENT] = 1.0f;
        features[MEMBER_ALIVE] = teammate->IsAlive() ? 1.0f : 0.0f;
        features[MEMBER_HEALTH] = teammate->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = teammate->GetMaxPower(POWER_MANA))
            features[MEMBER_MANA] = float(teammate->GetPower(POWER_MANA)) / float(maxMana);
        features[MEMBER_DISTANCE] = std::min(1.0f, bot->GetDistance(teammate) / 40.0f);
        features[MEMBER_BEARING_SIN] = std::sin(bearing);
        features[MEMBER_BEARING_COS] = std::cos(bearing);
        features[MEMBER_IN_COMBAT] = teammate->IsInCombat() ? 1.0f : 0.0f;
        other.Apt.WriteBrief(features + MEMBER_APTITUDE_FIRST);
        WriteOneHot(PLAYABLE_CLASSES, other.Class, features + MEMBER_CLASS_FIRST);

        if (other.Goal >= 0 && other.Goal < int32(GOAL_COUNT))
            features[MEMBER_GOAL_FIRST + other.Goal] = 1.0f;

        int32 const target = Encoding::SlotOf(view, teammate->GetVictim());
        if (target >= 0)
            features[MEMBER_TARGET_FIRST + target] = 1.0f;
        else
            features[MEMBER_NO_TARGET] = 1.0f;

        uint32 attackers = 0;
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
        {
            Unit* enemy = view.Enemies[slot];
            if (enemy && enemy->IsAlive() && enemy->GetVictim() == teammate)
            {
                features[MEMBER_SLOT_ON_FIRST + slot] = 1.0f;
                ++attackers;
            }
        }
        features[MEMBER_ATTACKERS] = float(attackers) / float(PACK_SLOTS);

        if (teammate->IsAlive())
        {
            ++alive;
            lowest = std::min(lowest, teammate->GetHealthPct() / 100.0f);
            float brief[Aptitude::BRIEF_COUNT] = {};
            other.Apt.WriteBrief(brief);
            obs[OBS_BEST_MITIGATION] = std::max(obs[OBS_BEST_MITIGATION], brief[Aptitude::BRIEF_MITIGATION]);
            obs[OBS_BEST_HEALING] = std::max(obs[OBS_BEST_HEALING], brief[Aptitude::BRIEF_HEALING]);
        }
    }

    obs[OBS_ALIVE] = float(alive) / float(PARTY_MEMBERS + 2);
    obs[OBS_LOWEST_HEALTH] = lowest;

    obs[OBS_RAID_GROUP] = float(view.Raid.Group) / float(RAID_GROUPS);
    obs[OBS_RAID_ALIVE] = view.Raid.Alive;
    obs[OBS_RAID_GROUP_ALIVE] = view.Raid.GroupAlive;
    obs[OBS_RAID_IN_COMBAT] = view.Raid.InCombat;
    obs[OBS_RAID_LOWEST_HEALTH] = view.Raid.LowestHealth;
    obs[OBS_RAID_TANKS_ALIVE] = view.Raid.TanksAlive;
    obs[OBS_RAID_HEALERS_ALIVE] = view.Raid.HealersAlive;

    uint32 const actions = view.L->Slice(BlockId::Party).ActionCount;
    for (uint32 action = 0; mask && action < actions; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::PartyBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;

    if (local == ACTION_FOLLOW_TANK)
    {
        Player* tank = view.Tank;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        tank->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_TANK_DISTANCE,
            Position::NormalizeOrientation(tank->GetOrientation() + float(M_PI)));
        Encoding::MoveTo(bot, FOLLOW_TANK_MOVE_POINT_ID, x, y, z);
        return;
    }

    if (local < ACTION_REVIVE_FIRST)
    {
        bool const assist = local < ACTION_GUARD_FIRST;
        Player* teammate = view.Teammates[local - (assist ? ACTION_ASSIST_FIRST : ACTION_GUARD_FIRST)].Bot;
        int32 const slot = assist ? Encoding::SlotOf(view, teammate->GetVictim())
            : Encoding::SlotAttacking(view, teammate, view.TargetSlot);
        if (slot >= 0 && view.Enemies[slot])
            Encoding::SelectEnemy(view, uint32(slot));
        return;
    }

    uint32 const revives = uint32(view.L->AllyRevives.size());
    uint32 const reviveIndex = local - ACTION_REVIVE_FIRST;
    Encoding::Revive(view, view.L->AllyRevives[reviveIndex % revives], view.Teammates[reviveIndex / revives].Bot,
        result);
}
