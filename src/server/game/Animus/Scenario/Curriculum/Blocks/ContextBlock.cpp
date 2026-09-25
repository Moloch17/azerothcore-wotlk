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

#include "ContextBlock.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>

Animus::Curriculum::BlockSize Animus::Curriculum::ContextBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, 0 };
}

void Animus::Curriculum::ContextBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    Player* bot = view.Bot;
    Player* owner = view.Owner;

    obs[OBS_OWNER_PRESENT] = owner ? 1.0f : 0.0f;
    obs[OBS_OWNER_ALIVE] = owner && owner->IsAlive() ? 1.0f : 0.0f;

    uint32 teammates = 0;
    for (SeatView::Teammate const& teammate : view.Teammates)
        teammates += teammate.Bot && teammate.Bot->IsAlive() ? 1 : 0;
    obs[OBS_TEAMMATES_ALIVE] = float(teammates) / float(PARTY_MEMBERS);

    uint32 players = 0;
    uint32 creatures = 0;
    float nearest = 1.0f;
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
    {
        Unit* enemy = view.Enemies[slot];
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!enemy->IsPlayer())
        {
            ++creatures;
            continue;
        }

        ++players;
        nearest = std::min(nearest, bot->GetDistance(enemy) / 60.0f);
        Unit const* victim = enemy->GetVictim();
        if (victim && (victim == bot || victim == owner))
            obs[OBS_PLAYER_ON_US] = 1.0f;
    }

    obs[OBS_HOSTILE_PLAYERS] = float(players) / float(PACK_SLOTS);
    obs[OBS_HOSTILE_CREATURES] = float(creatures) / float(PACK_SLOTS);
    obs[OBS_NEAREST_PLAYER_DISTANCE] = nearest;
    obs[OBS_PVP_FLAGGED] = bot->IsPvP() ? 1.0f : 0.0f;

    if (Map const* map = bot->FindMap())
    {
        obs[OBS_BATTLEGROUND_MAP] = map->IsBattlegroundOrArena() ? 1.0f : 0.0f;
        obs[OBS_INSTANCE_MAP] = map->IsDungeon() || map->IsRaid() ? 1.0f : 0.0f;
    }

    obs[OBS_SELF_RESURRECT_ALLOWED] = view.SelfResurrectAllowed ? 1.0f : 0.0f;
    if (Group const* group = bot->GetGroup())
        obs[OBS_GROUP_SIZE] = std::min(1.0f, float(group->GetMembersCount()) / 5.0f);
}
