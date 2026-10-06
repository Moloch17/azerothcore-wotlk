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

#ifndef ANIMUS_GPU_VISION_GPU_H
#define ANIMUS_GPU_VISION_GPU_H

#include "DeviceApi.h"
#include "VisionCaster.h"
#include "VisionScene.h"
#include <array>
#include <map>
#include <memory>
#include <span>
#include <string>

class Map;

/// **The GPU camera's scenes on the device, and its frames** (camera-vision.GPU.md, G1 and G2).
///
/// A Renderer keeps, for every map instance it is asked to cast in, a scene: the grids it has created (each
/// terrain grid packed once and shared by every instance of its map, reference-counted, freed when the last
/// instance holding it lets the grid go), its static tree (packed once per tree, which a dungeon's instances share
/// with their parent; it only grows, as the CPU's tree does: StaticMapTree keeps a tile's spawns once loaded) and
/// its doors (per instance, repacked from the dynamic tree and uploaded when they changed). Sync brings a scene up
/// to date on the CPU and uploads what changed; Cast launches the frames; Emulate runs the same kernel code on the
/// host copy, for the tests and `forge camera diff`.
///
/// World thread only (amendment 1: every upload, free and launch there). Built lazily, by the first Sync that
/// needs a grid -- for now only `forge gpu scene` and `forge camera diff` call it, so with AnimusForge.Gpu.Observe
/// off nothing here runs at all (amendment 4); G3 moves the CPU packing to the map threads at grid load.
namespace Animus::GpuVision
{
    /// What a scene is made of. MapId names the terrain (an instance shares its parent's); the trees are read at
    /// Sync, never held.
    struct SceneSource
    {
        uint32_t MapId = 0;
        uint32_t InstanceId = 0;
        Vision::VisionWorld const* World = nullptr;
        VMAP::StaticMapTree const* Static = nullptr;
        DynamicMapTree const* Dynamic = nullptr;
        LiquidDeadly Deadly;
    };

    /// A live map instance's source: `world` (its MapVisionWorld) must outlive the Sync.
    [[nodiscard]] SceneSource SourceOf(Map* map, Vision::VisionWorld const& world);

    /// One grid of a scene, for `forge gpu scene`: its terrain and the static spawns whose bounds touch it.
    struct GridReport
    {
        bool Created = false;
        bool Terrain = false;
        uint64_t TerrainBytes = 0;
        uint32_t Spawns = 0;            // spawns with a model touching the grid
        uint32_t Models = 0;            // distinct models among them
        uint64_t Triangles = 0;         // their triangles, every spawn counted
        uint64_t ModelBytes = 0;        // the distinct models' packed size
        uint64_t BihNodeWords = 0;      // ... of which BIH nodes
    };

    /// A whole scene: the CPU tree's own counts beside the packing's, which must agree.
    struct SceneReport
    {
        uint32_t Slots = 0;             // the static tree's spawns
        uint32_t CpuLoadedSlots = 0;    // ... with a model, as the CPU tree has them
        uint32_t LoadedSlots = 0;       // ... as packed
        uint64_t CpuTriangles = 0;      // the loaded spawns' triangles, read from the CPU's models
        uint64_t Triangles = 0;         // ... from the packing
        uint32_t Models = 0;
        ModelCounts Counts;             // the distinct models' (each once)
        uint64_t StaticBytes = 0;       // the tree, slots, instances and models
        uint32_t Doors = 0;
        uint64_t DoorBytes = 0;
        uint32_t Grids = 0;             // created
        uint32_t TerrainGrids = 0;      // ... with a terrain grid packed
        uint64_t TerrainBytes = 0;
        uint64_t DeviceBytes = 0;       // what this renderer holds on the device in all
    };

    struct CastTiming
    {
        double UploadMs = 0.0;
        double KernelMs = 0.0;
        double DownloadMs = 0.0;
        uint64_t Rays = 0;
    };

    /// The GPU path of Vision::Render for one seat: the request for its pixels, the rig the CPU placed (the boom is
    /// one CPU ray, as the pivot floor and the underwater test are: the scalars stay Render's), the size drawn
    /// clamped to the canonical one as Render clamps it, and its units appended to `unitsOut` without its own.
    [[nodiscard]] FrameRequest MakeRequest(Vision::Settings const& settings, Vision::Pose const& pose,
        Vision::CameraState const& camera, Vision::Rig const& rig, std::span<Vision::UnitShape const> units,
        Vision::Vec3 const* objective, uint32_t scene, uint32_t phaseMask, std::vector<DeviceUnit>& unitsOut);

    /// Lays the frames out: each request's canonical image at ImageOffset of `imageBytes`, its cast frame at
    /// ScratchOffset of `castBytes` (an unscaled frame is cast straight into the image; its copy there is filled
    /// after the cast).
    void LayOut(std::vector<FrameRequest>& requests, std::size_t& imageBytes, std::size_t& castBytes);

    class Renderer
    {
    public:
        /// `api` null: a host-only renderer (packing and Emulate, no device).
        explicit Renderer(ForgeGpuApi const* api);
        ~Renderer();
        Renderer(Renderer const&) = delete;
        Renderer& operator=(Renderer const&) = delete;

        [[nodiscard]] bool OnDevice() const { return _api != nullptr; }

        /// The scene of `source`, brought up to date and uploaded: its index, or -1 with `error`.
        int32_t Sync(SceneSource const& source, std::string& error);

        /// A map instance gone (its Map unloaded): its grids' references, its grid table and its doors let go. The
        /// static tree stays, as the CPU's does while the map lives; another instance's scene keeps its index.
        void Forget(uint32_t mapId, uint32_t instanceId);
        /// The terrain grids held (each once, however many instances share it).
        [[nodiscard]] uint32_t TerrainGrids() const { return uint32_t(_terrain.size()); }

        /// Every request cast on the device (LayOut first): the canonical images into `images`, the cast frames
        /// into `casts`.
        bool Cast(std::vector<FrameRequest> const& requests, std::vector<DeviceUnit> const& units,
            std::vector<uint8_t>& images, std::vector<uint8_t>& casts, CastTiming& timing, std::string& error);
        /// The same on the host, over the host copy of the scenes.
        void Emulate(std::vector<FrameRequest> const& requests, std::vector<DeviceUnit> const& units,
            std::vector<uint8_t>& images, std::vector<uint8_t>& casts) const;

        [[nodiscard]] SceneReport Report(int32_t scene) const;
        [[nodiscard]] GridReport ReportGrid(int32_t scene, int32_t tileX, int32_t tileY) const;
        /// The host view of a scene (the tests read it).
        [[nodiscard]] SceneView const& HostView(int32_t scene) const;

    private:
        struct Buffer
        {
            void* Pointer = nullptr;
            std::size_t Capacity = 0;
        };
        struct Terrain
        {
            Words Host;
            Buffer Device;
            uint32_t Refs = 0;
        };
        struct Static
        {
            StaticScene Scene;
            Buffer Top;
            Buffer Slots;
            Buffer Instances;
            Buffer Models;
            std::size_t UploadedModelWords = 0;
            VMAP::StaticMapTree const* Tree = nullptr;
            uint32_t CpuLoaded = 0;
            uint64_t CpuTriangles = 0;
        };
        struct Scene
        {
            uint32_t MapId = 0;
            uint32_t InstanceId = 0;
            std::array<uint64_t, std::size_t(Vision::GRIDS* Vision::GRIDS)> GridKeys{};
            std::vector<uint32_t const*> HostGrids;
            std::vector<uint64_t> DeviceGrids;
            Buffer GridTable;
            Static* Tree = nullptr;
            DoorScene Doors;
            Buffer DoorRecords;
            Buffer DoorCells;
            Buffer DoorModels;
            SceneView Host;
            SceneView Device;
        };

        bool Ensure(Buffer& buffer, std::size_t bytes, std::string& error, bool* moved = nullptr);
        bool Upload(Buffer& buffer, void const* host, std::size_t bytes, std::size_t at, std::string& error);
        bool UploadWords(Buffer& buffer, Words const& words, std::string& error);
        void Release(Buffer& buffer);
        void ReleaseTerrain(uint64_t key);

        ForgeGpuApi const* _api;
        std::map<uint64_t, Terrain> _terrain;
        std::map<std::pair<uint32_t, VMAP::StaticMapTree const*>, std::unique_ptr<Static>> _statics;
        std::vector<std::unique_ptr<Scene>> _scenes;
        Buffer _requests;
        Buffer _units;
        Buffer _views;
        Buffer _scratch;
        Buffer _image;
        uint64_t _deviceBytes = 0;
    };

    /// The process's renderer for the commands, made the first time one asks (on the device when the device
    /// library is loaded); SharedMade says whether that has happened.
    [[nodiscard]] Renderer& Shared();
    [[nodiscard]] bool SharedMade();
}

#endif
