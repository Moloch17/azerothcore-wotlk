/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef SCENE_BAKER_BAKE_H
#define SCENE_BAKER_BAKE_H

#include <cstdint>
#include <string>

namespace SceneBaker
{
    /// What a bake did, for the log.
    struct BakeReport
    {
        uint32_t Spawns = 0;
        uint32_t M2Spawns = 0;
        uint32_t Models = 0;
        uint32_t Triangles = 0;
        uint32_t DroppedTriangles = 0;
        uint32_t LiquidTriangles = 0;
        uint32_t UnknownLiquidTypes = 0;
        uint32_t Nodes = 0;
        uint32_t LiquidNodes = 0;
        uint32_t MaxDepth = 0;
        float WorstBoundExcess = 0.0f;      // the furthest a WMO's vertices lie outside its spawn bound, yards
        double Seconds = 0.0;
        uint64_t FileBytes = 0;
        uint64_t Checksum = 0;
    };

    /// Bakes map `mapId` from `dataDir` (vmaps/, dbc/LiquidType.dbc; maps/ is checked for terrain) to the scene file
    /// `outPath`. False, with `error`, when it cannot (the map has terrain tiles, no vmap, a transform that does not
    /// agree with the spawn bounds, ...).
    [[nodiscard]] bool BakeMap(std::string const& dataDir, uint32_t mapId, std::string const& outPath,
        BakeReport& report, std::string& error);
}

#endif
