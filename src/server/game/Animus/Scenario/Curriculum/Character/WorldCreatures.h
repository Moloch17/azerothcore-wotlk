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

#ifndef ANIMUS_LIB_CURRICULUM_WORLD_CREATURES_H
#define ANIMUS_LIB_CURRICULUM_WORLD_CREATURES_H

#include "Define.h"
#include <unordered_set>

/// What the world database says about creature entries, shared by the opponent pools and the hunter stable. Each set
/// is queried once, on first use (world thread).
namespace Animus::Curriculum::WorldCreatures
{
    /// Entries spawned somewhere in the world (the creature table).
    [[nodiscard]] std::unordered_set<uint32> const& SpawnedIds();

    /// Entries that walk waypoint paths: scripted set pieces (rares on patrol, escorts), not fair opponents or pets.
    [[nodiscard]] std::unordered_set<uint32> const& WaypointWalkerIds();
}

#endif
