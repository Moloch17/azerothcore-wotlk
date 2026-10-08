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

#ifndef ANIMUS_VISION_SCENE_REGISTRY_H
#define ANIMUS_VISION_SCENE_REGISTRY_H

#include "BakedWorld.h"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

/// **The loaded scenes** (baked-camera decision 0020): one BakedWorld per map id, loaded once at startup and shared
/// read-only by every env and map thread, and the one place a missing or stale scene file is baked. Library code
/// (no forge config): the caller says where the server's data is and where scenes live, so the forge worldserver
/// passes `<AnimusForge.DataDir>/scenes` and a realm module can pass `<its data dir>/scenes` (the folder the module's
/// cmake installs models into, ANIMUS_MODELS_INSTALL_DIR, plus "scenes").
///
/// Threading: Ensure runs at startup, before any map thread reads, and takes the writer lock; a BakedWorld is
/// immutable once Loaded() and never moved or freed while it is in the registry (Clear is for tests and shutdown),
/// so the pointer Get returns may be kept and read from any thread without a lock.
namespace Animus::Vision
{
    /// What Ensure did for one map.
    struct SceneEnsure
    {
        bool Baked = false;             // false: an existing file was valid and is loaded as it was
        std::string Reason;             // why it was baked: "no scene file", "source data changed", ...
        double Seconds = 0.0;           // the bake's, or the load and check's
        uint32_t Triangles = 0;
        uint32_t Nodes = 0;
        uint32_t TerrainTiles = 0;
        uint64_t Bytes = 0;
        uint64_t Checksum = 0;
        std::string Path;
    };

    class SceneRegistry
    {
    public:
        [[nodiscard]] static SceneRegistry& Instance();

        /// Makes map `mapId`'s scene available: loads `<sceneDir>/<map>.scene` if it is whole, of this baker's
        /// version and built from the data now on disk (SceneBaker::SourceIdentity), else bakes it from `dataDir`
        /// (the server's DataDir: vmaps/, maps/, dbc/) into `sceneDir` and loads that. The directory is created
        /// when missing. False, with `error` naming the map, the path and the cause, when neither works: there is
        /// no fallback.
        [[nodiscard]] bool Ensure(std::string const& dataDir, std::string const& sceneDir, uint32_t mapId,
            SceneEnsure& report, std::string& error);

        /// The loaded scene of a map, or null.
        [[nodiscard]] BakedWorld const* Get(uint32_t mapId) const;

        /// "<map>:<checksum>" of every loaded scene, ascending by map id: what the cluster fingerprint carries.
        [[nodiscard]] std::vector<std::pair<uint32_t, uint64_t>> Checksums() const;

        void Clear();

    private:
        mutable std::shared_mutex _lock;
        std::map<uint32_t, std::unique_ptr<BakedWorld>> _scenes;
    };
}

#endif
