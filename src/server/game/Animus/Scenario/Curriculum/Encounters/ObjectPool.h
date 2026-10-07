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

#ifndef ANIMUS_LIB_CURRICULUM_OBJECT_POOL_H
#define ANIMUS_LIB_CURRICULUM_OBJECT_POOL_H

#include "ObjectGuid.h"
#include "Position.h"

class GameObject;
class Map;

namespace Animus::Curriculum
{
    struct SeekObject;

    /// **The seek pool's objects in the world** (the movement stages that put one real object down: M2 seek's room
    /// object and M1's hallway object): spawning one, removing it, and clearing the dungeon's own game objects so the
    /// one object in it is the episode's. SeekEncounter does the same in its own Place and Build (written first);
    /// these are the same steps, kept apart so M1 shares them without reaching into M2's encounter.
    namespace ObjectPool
    {
        /// `kind` standing at `spot` (its base; the spot's orientation is its facing, about the vertical), in `phase`,
        /// with no respawn; null when it could not be spawned.
        GameObject* Summon(Map* map, SeekObject const& kind, Position const& spot, uint32 phase);

        /// Remove the object `object` names from `map`, if it is still there, and clear the guid.
        void Remove(Map* map, ObjectGuid& object);

        /// The map's own spawned game objects (the Stockades' chests, the Hallow's End pumpkins) despawned for a week:
        /// cheap enough every reset (a few dozen), and one that respawned goes again.
        void ClearOwn(Map* map);
    }
}

#endif
