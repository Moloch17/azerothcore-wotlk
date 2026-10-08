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

#ifndef ANIMUS_VISION_SCENE_BAKER_H
#define ANIMUS_VISION_SCENE_BAKER_H

#include <cstdint>
#include <string>

/// **The scene baker** (baked-camera decision 0020): turns a map's extracted data (vmaps/, maps/, dbc/LiquidType.dbc
/// of a server's DataDir) into the scene file BakedWorld reads. Library code, shared by the scene_baker tool, the forge
/// worldserver (which bakes a missing scene at first start) and the realm module (which does the same on a stock core):
/// it uses only stock, public core APIs (VMAP::StaticMapTree::GetModelInstances, WorldModel::GetGroupModels,
/// GroupModel::GetMeshData, WmoLiquid's accessors) and reads the .map and .dbc files itself, so it needs no forge
/// patch of the core. Deterministic: the same source files give the same bytes (see SceneBaker.cpp for how).
namespace SceneBaker
{
    /// Bump when the baker's output for the same source could differ (a fix, a new field): every scene baked by an
    /// older baker is then invalid and is baked again.
    constexpr uint32_t BAKER_VERSION = 1;

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
        uint32_t TerrainTiles = 0;
        uint32_t TerrainHeightTiles = 0;
        uint32_t TerrainHoleTiles = 0;
        uint32_t TerrainLiquidTiles = 0;
        float WorstBoundExcess = 0.0f;      // the furthest a WMO's vertices lie outside its spawn bound, yards
        double Seconds = 0.0;
        uint64_t FileBytes = 0;
        uint64_t Checksum = 0;
        uint64_t SourceHash = 0;
    };

    /// "036.scene": a map's scene file name.
    [[nodiscard]] std::string SceneFileName(uint32_t mapId);

    /// The identity of the source data a bake of `mapId` reads: FNV-1a 64 over the contents of LiquidType.dbc, the
    /// map's .vmtree and .vmtile files, every model file its spawns name and every .map terrain tile (not their
    /// sizes or times, which differ between machines that hold the same extraction). False, with `error`, when the
    /// map has no vmap tree or a file cannot be read.
    [[nodiscard]] bool SourceIdentity(std::string const& dataDir, uint32_t mapId, uint64_t& hash,
        std::string& error);

    /// Bakes map `mapId` from `dataDir` (the server's DataDir: vmaps/, maps/, dbc/) to the scene file `outPath`,
    /// written to a temporary name beside it and renamed into place, so a reader never sees half a file and two
    /// processes baking at once leave one whole file (their bytes are the same). False, with `error`, when it
    /// cannot: no vmap tree, a damaged map file, an unwritable `outPath` (named in the error).
    [[nodiscard]] bool BakeMap(std::string const& dataDir, uint32_t mapId, std::string const& outPath,
        BakeReport& report, std::string& error);
}

#endif
