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

#include "Env.h"
#include "Creature.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"

Map* Animus::Env::FindMap() const
{
    return sMapMgr->FindMap(MapId, InstanceId);
}

Player* Animus::Env::FindBot(uint32 agent) const
{
    if (agent >= Bots.size())
        return nullptr;

    return ObjectAccessor::FindPlayer(Bots[agent]);
}

Creature* Animus::Env::FindTarget(uint32 target) const
{
    if (target >= Targets.size())
        return nullptr;

    Map* map = FindMap();
    return map ? map->GetCreature(Targets[target]) : nullptr;
}

Unit* Animus::Env::FindTargetUnit(uint32 target) const
{
    if (target >= Targets.size())
        return nullptr;

    Map* map = FindMap();
    if (!map)
        return nullptr;

    if (Targets[target].IsPlayer())
    {
        Player* player = ObjectAccessor::FindPlayer(Targets[target]);
        return player && player->GetMap() == map ? player : nullptr;
    }

    return map->GetCreature(Targets[target]);
}
