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

#include "SceneRegistry.h"
#include "SceneBaker.h"
#include <chrono>
#include <filesystem>

namespace
{
    namespace Fs = std::filesystem;

    double SecondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
}

Animus::Vision::SceneRegistry& Animus::Vision::SceneRegistry::Instance()
{
    static SceneRegistry registry;
    return registry;
}

bool Animus::Vision::SceneRegistry::Ensure(std::string const& dataDir, std::string const& sceneDir, uint32_t mapId,
    SceneEnsure& report, std::string& error)
{
    std::unique_lock lock(_lock);
    auto const start = std::chrono::steady_clock::now();
    report = SceneEnsure();
    std::string const path = (Fs::path(sceneDir) / SceneBaker::SceneFileName(mapId)).string();
    report.Path = path;
    std::string const where = "map " + std::to_string(mapId) + " (scene " + path + ")";

    auto scene = std::make_unique<BakedWorld>();
    std::string loadError;
    std::error_code ec;
    if (!Fs::exists(path, ec))
        report.Reason = "no scene file";
    else if (!scene->Load(path, loadError))
        report.Reason = "invalid scene file (" + loadError + ")";
    else
    {
        Scene::SceneHeader const& header = scene->Header();
        uint64_t source = 0;
        std::string identityError;
        if (header.MapId != mapId)
            report.Reason = "scene file of map " + std::to_string(header.MapId);
        else if (header.BakerVersion != SceneBaker::BAKER_VERSION)
            report.Reason = "baked by baker version " + std::to_string(header.BakerVersion) + ", this is "
                + std::to_string(SceneBaker::BAKER_VERSION);
        else if (!SceneBaker::SourceIdentity(dataDir, mapId, source, identityError))
        {
            error = where + ": cannot read the map's source data in " + dataDir + ": " + identityError;
            return false;
        }
        else if (header.SourceHash != source)
            report.Reason = "the extracted map data changed since it was baked";
    }

    if (report.Reason.empty())
    {
        report.Seconds = SecondsSince(start);
    }
    else
    {
        SceneBaker::BakeReport baked;
        std::string bakeError;
        if (!SceneBaker::BakeMap(dataDir, mapId, path, baked, bakeError))
        {
            error = where + ": " + report.Reason + ", and baking it from " + dataDir + " failed: " + bakeError
                + ". The same bake by hand: scene_baker bake " + dataDir + " " + std::to_string(mapId) + " " + path;
            return false;
        }
        scene = std::make_unique<BakedWorld>();
        if (!scene->Load(path, loadError))
        {
            error = where + ": baked, but the file does not load: " + loadError;
            return false;
        }
        report.Baked = true;
        report.Seconds = baked.Seconds;
    }

    Scene::SceneHeader const& header = scene->Header();
    report.Triangles = header.TriCount;
    report.Nodes = header.NodeCount;
    report.TerrainTiles = header.TerrainTileCount;
    report.Bytes = scene->FileBytes();
    report.Checksum = header.Checksum;
    _scenes[mapId] = std::move(scene);
    return true;
}

Animus::Vision::BakedWorld const* Animus::Vision::SceneRegistry::Get(uint32_t mapId) const
{
    std::shared_lock lock(_lock);
    auto const found = _scenes.find(mapId);
    return found == _scenes.end() ? nullptr : found->second.get();
}

std::vector<std::pair<uint32_t, uint64_t>> Animus::Vision::SceneRegistry::Checksums() const
{
    std::shared_lock lock(_lock);
    std::vector<std::pair<uint32_t, uint64_t>> out;
    for (auto const& [mapId, scene] : _scenes)
        out.emplace_back(mapId, scene->Header().Checksum);
    return out;
}

void Animus::Vision::SceneRegistry::Clear()
{
    std::unique_lock lock(_lock);
    _scenes.clear();
}
