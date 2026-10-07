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

#include "ObjectPool.h"
#include "GameObject.h"
#include "Map.h"
#include "StageDefinition.h"
#include <cmath>
#include <vector>

GameObject* Animus::Curriculum::ObjectPool::Summon(Map* map, SeekObject const& kind, Position const& spot,
    uint32 phase)
{
    if (!map)
        return nullptr;
    float const facing = spot.GetOrientation();
    map->LoadGrid(spot.GetPositionX(), spot.GetPositionY());
    // No respawn: the encounter removes it at the next reset. The rotation is the facing's, about the vertical.
    GameObject* object = map->SummonGameObject(kind.Entry, spot, 0.0f, 0.0f, std::sin(facing * 0.5f),
        std::cos(facing * 0.5f), 0);
    if (object)
        object->SetPhaseMask(phase, true);
    return object;
}

void Animus::Curriculum::ObjectPool::Remove(Map* map, ObjectGuid& object)
{
    if (map && !object.IsEmpty())
        if (GameObject* spawned = map->GetGameObject(object))
        {
            // Gone at once, not at the map's next update (Delete only queues it): out of every phase, so no camera
            // sees it, and its model out of the dynamic tree's casts, so the next placement's line of sight and the
            // seat standing where it stood meet nothing of it.
            spawned->SetPhaseMask(0, false);
            spawned->EnableCollision(false);
            spawned->Delete();
        }
    object.Clear();
}

void Animus::Curriculum::ObjectPool::ClearOwn(Map* map)
{
    if (!map)
        return;
    std::vector<GameObject*> own;
    for (auto const& [spawnId, object] : map->GetGameObjectBySpawnIdStore())
        if (object && object->IsInWorld() && object->isSpawned())
            own.push_back(object);
    for (GameObject* object : own)
        object->DespawnOrUnsummon(0ms, Seconds(WEEK));
}
