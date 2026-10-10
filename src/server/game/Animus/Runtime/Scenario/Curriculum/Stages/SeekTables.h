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

#ifndef ANIMUS_LIB_CURRICULUM_SEEK_TABLES_H
#define ANIMUS_LIB_CURRICULUM_SEEK_TABLES_H

#include "StageDefinition.h"
#include <vector>

/// **The seek stage's per-map tables beyond the Stockades** (general search, decision 0027): Ragefire Chasm (map 389,
/// trained on beside the Stockades) and the Deadmines (map 36, held out). Reward and placement geometry only, never
/// observed (decision 0019): where the object may be hidden (SeekRoom), where the seat spawns and the hallway rung's
/// object stands (the hallway points, the entrance first). Authored offline from the maps' navmesh and baked scene
/// (SeekTables.cpp says how); checked in as data.
namespace Animus::Curriculum::SeekTables
{
    [[nodiscard]] std::vector<Position> RagefireHallways();
    [[nodiscard]] std::vector<SeekRoom> RagefireRooms();
    [[nodiscard]] std::vector<Position> DeadminesHallways();
    [[nodiscard]] std::vector<SeekRoom> DeadminesRooms();
}

#endif
