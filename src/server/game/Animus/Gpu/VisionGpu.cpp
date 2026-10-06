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

#include "VisionGpu.h"
#include "DBCStores.h"
#include "GpuRuntime.h"
#include "Log.h"
#include "Map.h"
#include "MapTree.h"
#include "ModelInstance.h"
#include "VisionCaster.h"
#include "WorldModel.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace
{
    using namespace Animus::GpuVision;
    namespace Vi = Animus::Vision;
    using Clock = std::chrono::steady_clock;

    constexpr std::size_t GRID_COUNT = std::size_t(Vi::GRIDS * Vi::GRIDS);
    constexpr uint64_t KEY_NONE = 0;
    constexpr uint64_t KEY_NO_TERRAIN = 1;

    uint64_t TerrainKey(uint32_t mapId, int32_t tileX, int32_t tileY)
    {
        return (uint64_t(1) << 63) | (uint64_t(mapId) << 32) | (uint64_t(uint32_t(tileX)) << 16) | uint32_t(tileY);
    }

    double Ms(Clock::time_point from, Clock::time_point to)
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    std::unique_ptr<Renderer> g_shared;
}

Animus::GpuVision::SceneSource Animus::GpuVision::SourceOf(Map* map, Vision::VisionWorld const& world)
{
    SceneSource source;
    source.MapId = map->GetId();
    source.InstanceId = map->GetInstanceId();
    source.World = &world;
    source.Static = map->GetMapCollisionData().GetStaticTreeSharedPtr().get();
    source.Dynamic = &map->GetMapCollisionData().GetDynamicTree();
    // MapVisionWorld::ModelLiquid's reading: LiquidType.dbc's kind 2 magma, 3 slime.
    source.Deadly = [](uint32_t type)
    {
        LiquidTypeEntry const* entry = sLiquidTypeStore.LookupEntry(type);
        return entry && (entry->Type == 2 || entry->Type == 3);
    };
    return source;
}

Animus::GpuVision::FrameRequest Animus::GpuVision::MakeRequest(Vision::Settings const& settings,
    Vision::Pose const& pose, Vision::CameraState const& camera, Vision::Rig const& rig,
    Vision::Sight const& sight, Vision::Vec3 const* objective, uint32_t scene, uint32_t phaseMask,
    std::span<void const* const> doorOwners, FrameLists& lists, float objectiveRadius)
{
    FrameRequest request{};
    request.CameraX = rig.Camera.X;
    request.CameraY = rig.Camera.Y;
    request.CameraZ = rig.Camera.Z;
    request.Azimuth = rig.Azimuth;
    request.Elevation = rig.Elevation;
    request.FeetZ = pose.Z;
    if (objective)
    {
        request.HasObjective = 1;
        request.ObjectiveX = objective->X;
        request.ObjectiveY = objective->Y;
        request.ObjectiveZ = objective->Z;
        request.ObjectiveRadius = objectiveRadius;
    }
    request.FovH = settings.FovH;
    request.FovV = settings.FovV;
    request.Width = settings.Width;
    request.Height = settings.Height;
    request.CastWidth = settings.Width;
    request.CastHeight = settings.Height;
    if (camera.RenderWidth > 0 && camera.RenderHeight > 0)
    {
        request.CastWidth = std::min(camera.RenderWidth, settings.Width);
        request.CastHeight = std::min(camera.RenderHeight, settings.Height);
    }
    request.Scene = scene;
    request.PhaseMask = phaseMask;
    request.UnitOffset = uint32_t(lists.Units.size());
    for (Vision::UnitShape const& unit : sight.Units)
        if (!unit.Self)
            lists.Units.push_back({ unit.X, unit.Y, unit.Z, unit.Radius, unit.Height, uint32_t(unit.What),
                unit.Entity });
    request.UnitCount = uint32_t(lists.Units.size()) - request.UnitOffset;

    request.BoxOffset = uint32_t(lists.Boxes.size());
    for (Vision::BoxShape const& box : sight.Boxes)
    {
        DeviceBox entry{};
        entry.X = box.X;
        entry.Y = box.Y;
        entry.Z = box.Z;
        std::copy(std::begin(box.InvRot), std::end(box.InvRot), entry.InvRot);
        std::copy(std::begin(box.Low), std::end(box.Low), entry.Low);
        std::copy(std::begin(box.High), std::end(box.High), entry.High);
        entry.Class = uint32_t(box.What);
        entry.Entity = box.Entity;
        lists.Boxes.push_back(entry);
    }
    request.BoxCount = uint32_t(lists.Boxes.size()) - request.BoxOffset;

    // The doors the frame knows, by their records in the scene (a model the scene has no record of is never hit).
    request.DoorOffset = uint32_t(lists.Doors.size());
    for (Vision::DoorShape const& door : sight.Doors)
    {
        auto const found = std::find(doorOwners.begin(), doorOwners.end(), door.Model);
        if (found != doorOwners.end())
            lists.Doors.push_back({ uint32_t(found - doorOwners.begin()), uint32_t(door.What), door.Entity, 0 });
    }
    request.DoorCount = uint32_t(lists.Doors.size()) - request.DoorOffset;
    return request;
}

void Animus::GpuVision::LayOut(std::vector<FrameRequest>& requests, std::size_t& imageBytes, std::size_t& castBytes)
{
    imageBytes = 0;
    castBytes = 0;
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        FrameRequest& request = requests[i];
        request.Frame = uint32_t(i);
        request.ImageOffset = imageBytes;
        request.ScratchOffset = castBytes;
        imageBytes += std::size_t(request.Width) * request.Height * Vi::BYTES_PER_PIXEL;
        castBytes += std::size_t(request.CastWidth) * request.CastHeight * Vi::BYTES_PER_PIXEL;
    }
}

Animus::GpuVision::Renderer::Renderer(ForgeGpuApi const* api) : _api(api) { }

uint32_t Animus::GpuVision::Renderer::SceneStackDepth(Scene const& scene)
{
    return std::max(scene.Tree ? scene.Tree->Scene.StackDepth() : 0u, scene.Doors.StackDepth());
}

Animus::GpuVision::Renderer::~Renderer()
{
    if (!_api)
        return;
    for (auto& [key, terrain] : _terrain)
        Release(terrain.Device);
    for (auto& [key, tree] : _statics)
        for (Buffer* buffer : { &tree->Top, &tree->Slots, &tree->LiquidTop, &tree->LiquidSlots, &tree->Instances,
            &tree->Models })
            Release(*buffer);
    for (auto& scene : _scenes)
        if (scene)
            for (Buffer* buffer : { &scene->GridTable, &scene->DoorRecords, &scene->DoorCells, &scene->DoorModels })
                Release(*buffer);
    for (Buffer* buffer : { &_requests, &_units, &_boxes, &_doors, &_counts, &_slots, &_slotOf, &_views, &_scratch,
        &_image, &_overflows })
        Release(*buffer);
}

bool Animus::GpuVision::Renderer::Ensure(Buffer& buffer, std::size_t bytes, std::string& error, bool* moved)
{
    if (moved)
        *moved = false;
    if (!_api || bytes <= buffer.Capacity)
        return true;
    // Grown by half again, so a scene that keeps growing (tiles loading) reallocates rarely.
    std::size_t const capacity = std::max(bytes, buffer.Capacity + buffer.Capacity / 2);
    Release(buffer);
    if (_api->Alloc(&buffer.Pointer, capacity))
    {
        error = std::string("device allocation: ") + _api->LastError();
        buffer.Pointer = nullptr;
        return false;
    }
    buffer.Capacity = capacity;
    _deviceBytes += capacity;
    if (moved)
        *moved = true;
    return true;
}

bool Animus::GpuVision::Renderer::Upload(Buffer& buffer, void const* host, std::size_t bytes, std::size_t at,
    std::string& error)
{
    if (!_api || !bytes)
        return true;
    if (_api->CopyToDevice(static_cast<uint8_t*>(buffer.Pointer) + at, host, bytes))
    {
        error = std::string("device copy: ") + _api->LastError();
        return false;
    }
    return true;
}

bool Animus::GpuVision::Renderer::UploadWords(Buffer& buffer, Words const& words, std::string& error)
{
    std::size_t const bytes = std::max<std::size_t>(words.size(), 1) * sizeof(uint32_t);
    return Ensure(buffer, bytes, error) && Upload(buffer, words.data(), words.size() * sizeof(uint32_t), 0, error);
}

void Animus::GpuVision::Renderer::Release(Buffer& buffer)
{
    if (_api && buffer.Pointer)
    {
        _api->Free(buffer.Pointer);
        _deviceBytes -= buffer.Capacity;
    }
    buffer = Buffer();
}

void Animus::GpuVision::Renderer::ReleaseTerrain(uint64_t key)
{
    auto const found = _terrain.find(key);
    if (found == _terrain.end() || --found->second.Refs)
        return;
    Release(found->second.Device);
    _terrain.erase(found);
}

int32_t Animus::GpuVision::Renderer::Sync(SceneSource const& source, std::string& error)
{
    if (!source.World)
    {
        error = "no world to read the terrain from";
        return -1;
    }

    auto found = std::find_if(_scenes.begin(), _scenes.end(), [&](std::unique_ptr<Scene> const& scene)
    {
        return scene && scene->MapId == source.MapId && scene->InstanceId == source.InstanceId;
    });
    if (found == _scenes.end())
    {
        auto scene = std::make_unique<Scene>();
        scene->MapId = source.MapId;
        scene->InstanceId = source.InstanceId;
        scene->HostGrids.assign(GRID_COUNT, nullptr);
        scene->DeviceGrids.assign(GRID_COUNT, 0);
        // A forgotten instance's slot is reused, so the indices of the others stay put.
        found = std::find(_scenes.begin(), _scenes.end(), nullptr);
        if (found == _scenes.end())
        {
            _scenes.push_back(nullptr);
            found = _scenes.end() - 1;
        }
        *found = std::move(scene);
    }
    Scene& scene = **found;
    int32_t const index = int32_t(found - _scenes.begin());

    // 1. The grids created, against what the scene holds: a grid created since is packed (or shared, when another
    // instance of the map has it), one let go is released.
    bool gridsChanged = !scene.GridTable.Pointer && _api;
    for (int32_t tileX = 0; tileX < Vi::GRIDS; ++tileX)
        for (int32_t tileY = 0; tileY < Vi::GRIDS; ++tileY)
        {
            std::size_t const at = std::size_t(tileX) * Vi::GRIDS + tileY;
            Vi::TerrainTile const tile = source.World->Tile(tileX, tileY);
            uint64_t key = KEY_NONE;
            if (tile.Loaded)
                key = (tile.Heights || tile.Liquid) ? TerrainKey(source.MapId, tileX, tileY) : KEY_NO_TERRAIN;
            if (key == scene.GridKeys[at])
                continue;
            gridsChanged = true;
            if (scene.GridKeys[at] > KEY_NO_TERRAIN)
                ReleaseTerrain(scene.GridKeys[at]);
            scene.GridKeys[at] = key;
            scene.HostGrids[at] = nullptr;
            scene.DeviceGrids[at] = 0;
            if (key == KEY_NO_TERRAIN)
            {
                scene.HostGrids[at] = NoTerrain();
                scene.DeviceGrids[at] = 1;
            }
            else if (key != KEY_NONE)
            {
                Terrain& terrain = _terrain[key];
                if (!terrain.Refs)
                {
                    PackTerrain(*source.World, tileX, tileY, terrain.Host);
                    if (!UploadWords(terrain.Device, terrain.Host, error))
                        return -1;
                }
                ++terrain.Refs;
                scene.HostGrids[at] = terrain.Host.data();
                scene.DeviceGrids[at] = uint64_t(uintptr_t(terrain.Device.Pointer));
            }
        }
    if (gridsChanged && _api)
        if (!Ensure(scene.GridTable, GRID_COUNT * sizeof(uint64_t), error)
            || !Upload(scene.GridTable, scene.DeviceGrids.data(), GRID_COUNT * sizeof(uint64_t), 0, error))
            return -1;

    // 2. The static tree, shared by every instance on it.
    scene.Tree = nullptr;
    if (source.Static)
    {
        std::unique_ptr<Static>& slot = _statics[{ source.MapId, source.Static }];
        if (!slot)
        {
            slot = std::make_unique<Static>();
            slot->Tree = source.Static;
        }
        Static& tree = *slot;
        scene.Tree = &tree;
        std::size_t const modelWordsBefore = tree.Scene.Models.Data().size();
        bool const fresh = tree.Scene.Top.empty();
        if (tree.Scene.Sync(*source.Static, source.Deadly) || fresh)
        {
            // The CPU tree's own counts, for the report's check against the packing.
            tree.CpuLoaded = 0;
            tree.CpuTriangles = 0;
            VMAP::ModelInstance const* values = source.Static->GetTreeValues();
            for (uint32_t i = 0; i < source.Static->GetTreeValueCount(); ++i)
                if (VMAP::WorldModel const* model = values[i].GetWorldModel())
                {
                    ++tree.CpuLoaded;
                    for (VMAP::GroupModel const& group : model->GetGroups())
                        tree.CpuTriangles += group.GetTriangles().size();
                }

            if (_api)
            {
                if (!UploadWords(tree.Top, tree.Scene.Top, error)
                    || !UploadWords(tree.Slots, tree.Scene.Slots, error)
                    || !UploadWords(tree.LiquidTop, tree.Scene.LiquidTop, error)
                    || !UploadWords(tree.LiquidSlots, tree.Scene.LiquidSlots, error)
                    || !UploadWords(tree.Instances, tree.Scene.Instances, error))
                    return -1;
                // The model pool only grows: the new tail, or all of it when the buffer had to move.
                Words const& models = tree.Scene.Models.Data();
                bool moved = false;
                if (!Ensure(tree.Models, models.size() * sizeof(uint32_t), error, &moved))
                    return -1;
                std::size_t const from = moved ? 0 : std::min(tree.UploadedModelWords, modelWordsBefore);
                if (!Upload(tree.Models, models.data() + from, (models.size() - from) * sizeof(uint32_t),
                    from * sizeof(uint32_t), error))
                    return -1;
                tree.UploadedModelWords = models.size();
            }
        }
    }

    // 3. The instance's doors, uploaded when they changed.
    if (source.Dynamic && (scene.Doors.Sync(*source.Dynamic, source.Deadly) || !scene.DoorCells.Pointer) && _api)
        if (!UploadWords(scene.DoorRecords, scene.Doors.Records, error)
            || !UploadWords(scene.DoorCells, scene.Doors.Cells, error)
            || !UploadWords(scene.DoorModels, scene.Doors.Models.Data(), error))
            return -1;

    // The worst stack a ray can need here, against the largest the kernel is compiled with: past it a walk drops
    // nodes, which the overflow count then shows. Said once a scene and depth.
    uint32_t const depth = SceneStackDepth(scene);
    if (depth > uint32_t(MAX_STACK) && depth != scene.WarnedDepth)
    {
        scene.WarnedDepth = depth;
        int32_t gridX = -1;
        int32_t gridY = -1;
        if (scene.Tree && scene.Tree->Scene.DeepestSlot != NO_INSTANCE)
        {
            StaticScene const& packed = scene.Tree->Scene;
            uint32_t const* record = &packed.Instances[std::size_t(packed.SlotTable[packed.DeepestSlot])
                * INSTANCE_WORDS];
            // The static tree's space: x and y mirrored about the middle, so its grid is the coordinate's.
            gridX = int32_t(std::floor(AsFloat(record[INSTANCE_POS]) / Vi::GRID_SIZE));
            gridY = int32_t(std::floor(AsFloat(record[INSTANCE_POS + 1]) / Vi::GRID_SIZE));
        }
        LOG_WARN("module.animus", "GPU camera: map {} instance {} needs a {}-deep BIH stack (its spawn tree {}, its "
            "deepest model near grid ({}, {})), past the kernel's largest ({}): rays there may drop nodes, which "
            "the overflow count shows", source.MapId, source.InstanceId, depth,
            scene.Tree ? scene.Tree->Scene.TopDepth : 0, gridX, gridY, MAX_STACK);
    }

    if (_api && _api->Synchronize())
    {
        error = std::string("device sync: ") + _api->LastError();
        return -1;
    }
    return index;
}

void Animus::GpuVision::Renderer::Forget(uint32_t mapId, uint32_t instanceId)
{
    for (std::unique_ptr<Scene>& scene : _scenes)
    {
        if (!scene || scene->MapId != mapId || scene->InstanceId != instanceId)
            continue;
        for (uint64_t key : scene->GridKeys)
            if (key > KEY_NO_TERRAIN)
                ReleaseTerrain(key);
        for (Buffer* buffer : { &scene->GridTable, &scene->DoorRecords, &scene->DoorCells, &scene->DoorModels })
            Release(*buffer);
        scene.reset();
    }
}

Animus::GpuVision::SceneView const& Animus::GpuVision::Renderer::HostView(int32_t index) const
{
    Scene& scene = *_scenes[std::size_t(index)];
    SceneView& view = scene.Host;
    view = SceneView();
    view.Grids = scene.HostGrids.data();
    if (scene.Tree)
    {
        view.Top = scene.Tree->Scene.Top.data();
        view.Slots = scene.Tree->Scene.Slots.data();
        view.LiquidTop = scene.Tree->Scene.LiquidTop.data();
        view.LiquidSlots = scene.Tree->Scene.LiquidSlots.data();
        view.Instances = scene.Tree->Scene.Instances.data();
        view.Models = scene.Tree->Scene.Models.Data().data();
    }
    view.DoorCount = scene.Doors.Count;
    if (view.DoorCount)
    {
        view.Doors = scene.Doors.Records.data();
        view.DoorCells = scene.Doors.Cells.data();
        view.DoorModels = scene.Doors.Models.Data().data();
    }
    return view;
}

std::span<void const* const> Animus::GpuVision::Renderer::DoorOwners(int32_t scene) const
{
    if (scene < 0 || std::size_t(scene) >= _scenes.size() || !_scenes[std::size_t(scene)])
        return {};
    return _scenes[std::size_t(scene)]->Doors.Owners;
}

bool Animus::GpuVision::Renderer::Cast(std::vector<FrameRequest> const& requests, FrameLists const& lists,
    std::vector<uint8_t>& images, std::vector<uint8_t>& casts, std::vector<Vision::FrameSlots>& slots,
    CastTiming& timing, std::string& error)
{
    timing = CastTiming();
    if (!_api)
    {
        error = "no device";
        return false;
    }
    std::size_t imageBytes = 0;
    std::size_t castBytes = 0;
    uint32_t maxCast = 0;
    uint32_t maxPixels = 0;
    uint32_t maxCastWidth = 0;
    uint32_t maxCastHeight = 0;
    uint32_t stackDepth = 0;
    for (FrameRequest const& request : requests)
    {
        if (request.Scene < _scenes.size() && _scenes[request.Scene])
            stackDepth = std::max(stackDepth, SceneStackDepth(*_scenes[request.Scene]));
        maxCastWidth = std::max(maxCastWidth, request.CastWidth);
        maxCastHeight = std::max(maxCastHeight, request.CastHeight);
        imageBytes = std::max<std::size_t>(imageBytes, request.ImageOffset
            + std::size_t(request.Width) * request.Height * Vi::BYTES_PER_PIXEL);
        castBytes = std::max<std::size_t>(castBytes, request.ScratchOffset
            + std::size_t(request.CastWidth) * request.CastHeight * Vi::BYTES_PER_PIXEL);
        maxCast = std::max(maxCast, request.CastWidth * request.CastHeight);
        maxPixels = std::max(maxPixels, request.Width * request.Height);
        timing.Rays += uint64_t(request.CastWidth) * request.CastHeight;
    }

    // The scenes' device views, built now: a shared tree's buffers may have moved since another scene's Sync.
    std::vector<SceneView> views(_scenes.size());
    for (std::size_t i = 0; i < _scenes.size(); ++i)
    {
        if (!_scenes[i])
            continue;
        Scene const& scene = *_scenes[i];
        SceneView& view = views[i];
        view.Grids = static_cast<uint32_t const* const*>(scene.GridTable.Pointer);
        if (scene.Tree)
        {
            view.Top = static_cast<uint32_t const*>(scene.Tree->Top.Pointer);
            view.Slots = static_cast<uint32_t const*>(scene.Tree->Slots.Pointer);
            view.LiquidTop = static_cast<uint32_t const*>(scene.Tree->LiquidTop.Pointer);
            view.LiquidSlots = static_cast<uint32_t const*>(scene.Tree->LiquidSlots.Pointer);
            view.Instances = static_cast<uint32_t const*>(scene.Tree->Instances.Pointer);
            view.Models = static_cast<uint32_t const*>(scene.Tree->Models.Pointer);
        }
        view.DoorCount = scene.Doors.Count;
        if (view.DoorCount)
        {
            view.Doors = static_cast<uint32_t const*>(scene.DoorRecords.Pointer);
            view.DoorCells = static_cast<uint32_t const*>(scene.DoorCells.Pointer);
            view.DoorModels = static_cast<uint32_t const*>(scene.DoorModels.Pointer);
        }
    }

    Clock::time_point const start = Clock::now();
    std::size_t const frames = std::max<std::size_t>(requests.size(), 1);
    if (!Ensure(_requests, frames * sizeof(FrameRequest), error)
        || !Ensure(_units, std::max<std::size_t>(lists.Units.size(), 1) * sizeof(DeviceUnit), error)
        || !Ensure(_boxes, std::max<std::size_t>(lists.Boxes.size(), 1) * sizeof(DeviceBox), error)
        || !Ensure(_doors, std::max<std::size_t>(lists.Doors.size(), 1) * sizeof(DeviceDoor), error)
        || !Ensure(_counts, frames * COUNTS_PER_FRAME * sizeof(Vision::SlotStat), error)
        || !Ensure(_slots, frames * Vision::ENTITY_SLOTS * sizeof(Vision::SlotStat), error)
        || !Ensure(_slotOf, frames * COUNTS_PER_FRAME, error)
        || !Ensure(_views, std::max<std::size_t>(views.size(), 1) * sizeof(SceneView), error)
        || !Ensure(_scratch, std::max<std::size_t>(castBytes, 4), error)
        || !Ensure(_image, std::max<std::size_t>(imageBytes, 4), error)
        || !Ensure(_overflows, sizeof(uint32_t), error)
        || !Upload(_requests, requests.data(), requests.size() * sizeof(FrameRequest), 0, error)
        || !Upload(_units, lists.Units.data(), lists.Units.size() * sizeof(DeviceUnit), 0, error)
        || !Upload(_boxes, lists.Boxes.data(), lists.Boxes.size() * sizeof(DeviceBox), 0, error)
        || !Upload(_doors, lists.Doors.data(), lists.Doors.size() * sizeof(DeviceDoor), 0, error)
        || !Upload(_views, views.data(), views.size() * sizeof(SceneView), 0, error))
        return false;
    uint32_t overflows = 0;
    if (!Upload(_overflows, &overflows, sizeof(overflows), 0, error))
        return false;
    if (_api->Synchronize())
    {
        error = std::string("device sync: ") + _api->LastError();
        return false;
    }
    Clock::time_point const uploaded = Clock::now();

    ForgeVisionLaunch launch{};
    launch.Requests = static_cast<FrameRequest const*>(_requests.Pointer);
    launch.RequestCount = uint32_t(requests.size());
    launch.MaxCastPixels = maxCast;
    launch.MaxPixels = maxPixels;
    launch.MaxCastWidth = maxCastWidth;
    launch.MaxCastHeight = maxCastHeight;
    launch.StackDepth = stackDepth;
    launch.Overflows = static_cast<uint32_t*>(_overflows.Pointer);
    timing.StackSize = uint32_t(STACK_SIZES[StackIndexFor(stackDepth)]);
    timing.StackDepth = stackDepth;
    launch.Units = static_cast<DeviceUnit const*>(_units.Pointer);
    launch.Boxes = static_cast<DeviceBox const*>(_boxes.Pointer);
    launch.Doors = static_cast<DeviceDoor const*>(_doors.Pointer);
    launch.Counts = static_cast<Vision::SlotStat*>(_counts.Pointer);
    launch.Slots = static_cast<Vision::SlotStat*>(_slots.Pointer);
    launch.SlotOf = static_cast<uint8_t*>(_slotOf.Pointer);
    launch.Scenes = static_cast<SceneView const*>(_views.Pointer);
    launch.Scratch = static_cast<uint8_t*>(_scratch.Pointer);
    launch.Image = static_cast<uint8_t*>(_image.Pointer);
    if (_api->CastVision(&launch) || _api->Synchronize())
    {
        error = std::string("cast: ") + _api->LastError();
        return false;
    }
    Clock::time_point const cast = Clock::now();

    // The entity lists first, on their own (the read-back the entity block waits on): ENTITY_SLOTS a frame.
    std::vector<Vision::SlotStat> tables(requests.size() * Vision::ENTITY_SLOTS);
    timing.SlotBytes = tables.size() * sizeof(Vision::SlotStat);
    if (_api->CopyToHost(tables.data(), _slots.Pointer, timing.SlotBytes) || _api->Synchronize())
    {
        error = std::string("slot read-back: ") + _api->LastError();
        return false;
    }
    Clock::time_point const read = Clock::now();
    slots.assign(requests.size(), Vision::FrameSlots());
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        Vision::FrameSlots& frame = slots[i];
        frame.CastWidth = requests[i].CastWidth;
        frame.CastHeight = requests[i].CastHeight;
        for (uint32_t slot = 0; slot < Vision::ENTITY_SLOTS; ++slot)
        {
            Vision::SlotStat const& stat = tables[i * Vision::ENTITY_SLOTS + slot];
            if (!stat.Entity)
                break;
            frame.Slots[slot] = stat;
            frame.Count = slot + 1;
        }
    }

    images.assign(imageBytes, 0);
    casts.assign(castBytes, 0);
    if (_api->CopyToHost(images.data(), _image.Pointer, imageBytes)
        || _api->CopyToHost(casts.data(), _scratch.Pointer, castBytes)
        || _api->CopyToHost(&overflows, _overflows.Pointer, sizeof(overflows)) || _api->Synchronize())
    {
        error = std::string("download: ") + _api->LastError();
        return false;
    }
    // An unscaled frame was cast straight into its image.
    for (FrameRequest const& request : requests)
        if (!Scaled(request))
            std::copy_n(images.begin() + std::ptrdiff_t(request.ImageOffset),
                std::size_t(request.Width) * request.Height * Vi::BYTES_PER_PIXEL,
                casts.begin() + std::ptrdiff_t(request.ScratchOffset));
    Clock::time_point const done = Clock::now();
    timing.Overflows = overflows;
    _lastOverflows = overflows;
    _totalOverflows += overflows;
    timing.UploadMs = Ms(start, uploaded);
    timing.KernelMs = Ms(uploaded, cast);
    timing.SlotsMs = Ms(cast, read);
    timing.DownloadMs = Ms(read, done);
    return true;
}

uint32_t Animus::GpuVision::Renderer::Emulate(std::vector<FrameRequest> const& requests, FrameLists const& lists,
    std::vector<uint8_t>& images, std::vector<uint8_t>& casts, std::vector<Vision::FrameSlots>& slots) const
{
    uint32_t stackDepth = 0;
    for (FrameRequest const& request : requests)
        if (request.Scene < _scenes.size() && _scenes[request.Scene])
            stackDepth = std::max(stackDepth, SceneStackDepth(*_scenes[request.Scene]));
    int const stack = StackIndexFor(stackDepth);
    uint32_t overflows = 0;
    std::size_t imageBytes = 0;
    std::size_t castBytes = 0;
    for (FrameRequest const& request : requests)
    {
        imageBytes = std::max<std::size_t>(imageBytes, request.ImageOffset
            + std::size_t(request.Width) * request.Height * Vi::BYTES_PER_PIXEL);
        castBytes = std::max<std::size_t>(castBytes, request.ScratchOffset
            + std::size_t(request.CastWidth) * request.CastHeight * Vi::BYTES_PER_PIXEL);
    }
    images.assign(imageBytes, 0);
    casts.assign(castBytes, 0);
    slots.assign(requests.size(), Vision::FrameSlots());
    std::array<Vision::SlotStat, COUNTS_PER_FRAME> counts;
    std::array<Vision::SlotStat, Vision::ENTITY_SLOTS> table;
    std::array<uint8_t, COUNTS_PER_FRAME> slotOf;
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        FrameRequest const& request = requests[i];
        SceneView const& view = HostView(int32_t(request.Scene));
        FrameSight const sight = SightOf(request, lists.Units.data(), lists.Boxes.data(), lists.Doors.data());
        uint8_t* cast = casts.data() + request.ScratchOffset;
        for (uint32_t row = 0; row < request.CastHeight; ++row)
            for (uint32_t col = 0; col < request.CastWidth; ++col)
                overflows += CastPixelSized(stack, request, view, sight, row, col,
                    cast + (std::size_t(row) * request.CastWidth + col) * Vi::BYTES_PER_PIXEL);

        // The kernels' reduction and slots, in the same steps.
        counts.fill(Vision::SlotStat());
        for (uint32_t row = 0; row < request.CastHeight; ++row)
            for (uint32_t col = 0; col < request.CastWidth; ++col)
                if (uint8_t const number = NumberAt(request, cast, row, col))
                {
                    ++counts[number].Pixels;
                    counts[number].SumRow += row;
                    counts[number].SumCol += col;
                }
        AssignFrameSlots(counts.data(), table.data(), slotOf.data());
        Vision::FrameSlots& frame = slots[i];
        frame.CastWidth = request.CastWidth;
        frame.CastHeight = request.CastHeight;
        for (uint32_t slot = 0; slot < Vision::ENTITY_SLOTS && table[slot].Entity; ++slot)
        {
            frame.Slots[slot] = table[slot];
            frame.Count = slot + 1;
        }

        uint8_t* image = images.data() + request.ImageOffset;
        for (uint32_t r = 0; r < request.Height; ++r)
            for (uint32_t c = 0; c < request.Width; ++c)
                UpscalePixel(cast, request.CastWidth, request.CastHeight, image, request.Width, request.Height, r,
                    c, slotOf.data());
        for (std::size_t pixel = 0; pixel < std::size_t(request.CastWidth) * request.CastHeight; ++pixel)
        {
            uint8_t& slot = cast[pixel * Vi::BYTES_PER_PIXEL + Vi::SLOT_BYTE];
            slot = slotOf[slot];
        }
    }
    return overflows;
}

Animus::GpuVision::SceneReport Animus::GpuVision::Renderer::Report(int32_t index) const
{
    SceneReport report;
    Scene const& scene = *_scenes[std::size_t(index)];
    if (Static const* tree = scene.Tree)
    {
        StaticScene const& packed = tree->Scene;
        report.Slots = uint32_t(packed.SlotTable.size());
        report.CpuLoadedSlots = tree->CpuLoaded;
        report.LoadedSlots = packed.LoadedSlots;
        report.CpuTriangles = tree->CpuTriangles;
        for (uint32_t slot = 0; slot < packed.SlotTable.size(); ++slot)
            if (packed.SlotTable[slot] != NO_INSTANCE)
                report.Triangles += packed.Models.CountsOf(packed.SlotModels[slot]).Triangles;
        report.Models = packed.Models.Models();
        report.Counts = packed.Models.Counts();
        report.StaticBytes = (packed.Top.size() + packed.Slots.size() + packed.LiquidTop.size()
            + packed.LiquidSlots.size() + packed.Instances.size()
            + packed.Models.Data().size()) * sizeof(uint32_t);
    }
    report.Doors = scene.Doors.Count;
    report.DoorBytes = (scene.Doors.Records.size() + scene.Doors.Cells.size() + scene.Doors.Models.Data().size())
        * sizeof(uint32_t);
    for (uint64_t key : scene.GridKeys)
    {
        if (key == KEY_NONE)
            continue;
        ++report.Grids;
        if (key == KEY_NO_TERRAIN)
            continue;
        ++report.TerrainGrids;
        report.TerrainBytes += TERRAIN_WORDS * sizeof(uint32_t);
    }
    report.DeviceBytes = _deviceBytes;
    report.StackDepth = SceneStackDepth(scene);
    if (scene.Tree)
    {
        report.StaticTopDepth = scene.Tree->Scene.TopDepth;
        report.ModelStackDepth = scene.Tree->Scene.Models.Counts().StackDepth;
    }
    return report;
}

Animus::GpuVision::GridReport Animus::GpuVision::Renderer::ReportGrid(int32_t index, int32_t tileX,
    int32_t tileY) const
{
    GridReport report;
    Scene const& scene = *_scenes[std::size_t(index)];
    if (tileX < 0 || tileY < 0 || tileX >= Vi::GRIDS || tileY >= Vi::GRIDS)
        return report;
    uint64_t const key = scene.GridKeys[std::size_t(tileX) * Vi::GRIDS + tileY];
    report.Created = key != KEY_NONE;
    report.Terrain = key > KEY_NO_TERRAIN;
    report.TerrainBytes = report.Terrain ? TERRAIN_WORDS * sizeof(uint32_t) : 0;
    if (!scene.Tree)
        return report;

    // The static tree's space is the world's mirrored about the middle: grid (x, y) is [x, x + 1) x [y, y + 1)
    // grid sizes there (the vmtile's own square).
    StaticScene const& packed = scene.Tree->Scene;
    float const lowX = float(tileX) * Vi::GRID_SIZE;
    float const lowY = float(tileY) * Vi::GRID_SIZE;
    float const highX = lowX + Vi::GRID_SIZE;
    float const highY = lowY + Vi::GRID_SIZE;
    std::vector<VMAP::WorldModel const*> seen;
    for (uint32_t slot = 0; slot < packed.SlotTable.size(); ++slot)
    {
        if (packed.SlotTable[slot] == NO_INSTANCE)
            continue;
        uint32_t const* record = &packed.Instances[std::size_t(packed.SlotTable[slot]) * INSTANCE_WORDS];
        uint32_t const* bound = record + INSTANCE_BOUND;
        if (AsFloat(bound[3]) < lowX || AsFloat(bound[0]) > highX || AsFloat(bound[4]) < lowY
            || AsFloat(bound[1]) > highY)
            continue;
        VMAP::WorldModel const* model = packed.SlotModels[slot];
        ModelCounts const counts = packed.Models.CountsOf(model);
        ++report.Spawns;
        report.Triangles += counts.Triangles;
        if (std::find(seen.begin(), seen.end(), model) != seen.end())
            continue;
        seen.push_back(model);
        ++report.Models;
        report.ModelBytes += uint64_t(packed.Models.SizeOf(model)) * sizeof(uint32_t);
        report.BihNodeWords += counts.BihNodeWords;
    }
    return report;
}

Animus::GpuVision::Renderer& Animus::GpuVision::Shared()
{
    // Made on the device once the device library is loaded; a host-only one made before that is replaced.
    ForgeGpuApi const* api = Animus::Gpu::Api();
    if (!g_shared || (api && !g_shared->OnDevice()))
        g_shared = std::make_unique<Renderer>(api);
    return *g_shared;
}

bool Animus::GpuVision::SharedMade()
{
    return g_shared != nullptr;
}
