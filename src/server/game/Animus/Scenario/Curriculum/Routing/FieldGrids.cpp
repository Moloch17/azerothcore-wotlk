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

#include "FieldGrids.h"
#include "DBCStores.h"
#include "GridDefines.h"
#include "InstanceBosses.h"
#include "MapDefines.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include "World.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>

int32 Animus::Curriculum::FieldGrids::GridIndex(float coordinate)
{
    return int32(std::floor(coordinate / SIZE_OF_GRIDS));
}

std::vector<Animus::Curriculum::FieldGrids::GridRef> Animus::Curriculum::FieldGrids::StageGrids(
    Animus::Curriculum::StageDefinition const& stage, bool wholeMaps)
{
    // Each map's points: the stage's on its own map, an arena's on the arena's map (ArenaDefinition::MapId).
    std::map<uint32, std::vector<Position>> points;
    std::vector<Position>& stagePoints = points[stage.MapId];
    stagePoints = stage.SpawnPoints;
    stagePoints.insert(stagePoints.end(), stage.HeldOutSpawnPoints.begin(), stage.HeldOutSpawnPoints.end());
    std::set<uint32> maps = { stage.MapId };
    for (Animus::Curriculum::ArenaDefinition const& arena : stage.Arenas)
    {
        uint32 const arenaMap = arena.MapId ? arena.MapId : stage.MapId;
        maps.insert(arenaMap);
        std::vector<Position>& arenaPoints = points[arenaMap];
        arenaPoints.insert(arenaPoints.end(), arena.SpawnPoints.begin(), arena.SpawnPoints.end());
        arenaPoints.insert(arenaPoints.end(), arena.HeldOutSpawnPoints.begin(), arena.HeldOutSpawnPoints.end());
        for (Animus::Curriculum::BossRow const& row : Animus::Curriculum::InstanceLadderRows(arena.Instance))
            maps.insert(row.MapId);
    }

    // An objective is at most forty yards from its spawn and a seat looks forty further: a neighbour within eighty
    // of the point is on the stage too.
    constexpr float REACH = 80.0f;
    std::set<GridRef> grids;
    for (uint32 mapId : maps)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        if (!entry)
            continue;
        if (!entry->Instanceable() && !wholeMaps)
        {
            if (auto const found = points.find(mapId); found != points.end())
                for (Position const& point : found->second)
                    for (int32 dx = -1; dx <= 1; ++dx)
                        for (int32 dy = -1; dy <= 1; ++dy)
                            grids.insert({ mapId, GridIndex(point.GetPositionX() + float(dx) * REACH),
                                GridIndex(point.GetPositionY() + float(dy) * REACH) });
            continue;
        }

        // Every mmtile of the map: MMMXXYY.mmtile, XX and YY the core's grid coordinates, which count down from
        // +x/+y where the fields count up from 0.
        std::string const prefix = Acore::StringFormat("{:03}", mapId);
        std::error_code error;
        for (auto const& file : std::filesystem::directory_iterator(sWorld->GetDataPath() + "mmaps", error))
        {
            std::string const name = file.path().filename().string();
            if (name.size() != 14 || name.compare(0, 3, prefix) != 0 || file.path().extension() != ".mmtile")
                continue;
            int32 const coreX = std::atoi(name.substr(3, 2).c_str());
            int32 const coreY = std::atoi(name.substr(5, 2).c_str());
            grids.insert({ mapId, int32(CENTER_GRID_ID) - 1 - coreX, int32(CENTER_GRID_ID) - 1 - coreY });
        }
    }
    return { grids.begin(), grids.end() };
}
