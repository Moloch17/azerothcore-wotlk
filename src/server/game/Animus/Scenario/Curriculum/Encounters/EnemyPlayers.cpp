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

#include "Encounters.h"
#include "Player.h"

namespace
{
    // Player faction templates: a human's (Alliance) and an orc's (Horde). Enemies get the other side's.
    constexpr uint32 FACTION_ALLIANCE_PLAYER = 1;
    constexpr uint32 FACTION_HORDE_PLAYER = 2;
}

void Animus::Curriculum::EnemyPlayers::MakeEnemies(Player* player, Player* enemy)
{
    enemy->SetFaction(player->GetTeamId() == TEAM_ALLIANCE ? FACTION_HORDE_PLAYER : FACTION_ALLIANCE_PLAYER);
    Flag(player);
    Flag(enemy);
}

void Animus::Curriculum::EnemyPlayers::Flag(Player* player)
{
    if (player && !player->IsPvP())
        player->UpdatePvP(true, true);
}
