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

#include "DynamicTree.h"
#include "GameObjectModel.h"
#include "GpuRuntime.h"
#include "GridTerrainData.h"
#include "MapCollisionData.h"
#include "MapTree.h"
#include "StringFormat.h"
#include "VMapDefinitions.h"
#include "VMapMgr2.h"
#include "VisionDiff.h"
#include "VisionGpu.h"
#include "gtest/gtest.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>

/// **The GPU camera on real map data** (camera-vision.GPU.md, G1 and G2 gates), for when no second worldserver
/// can be run: the map's own vmaps and maps loaded as a Map loads them (StaticMapTree, GridTerrainData, a dynamic
/// tree of game object models), a VisionWorld over them that reads them as MapVisionWorld does, and `forge camera
/// diff`'s comparison (VisionDiff) between Vision::Render and the device. Skipped unless FORGE_VISION_DATA names
/// the data directory (with maps/ and vmaps/); FORGE_GPU_RUNTIME and FORGE_GPU_LIBRARY name libamdhip64.so and
/// libforge-gpu.so for the device column (without them, the host-run kernel only). FORGE_VISION_FRAMES sets the
/// frames a place (default 64).
namespace
{
    namespace Gv = Animus::GpuVision;
    namespace Vi = Animus::Vision;

    char const* Env(char const* name)
    {
        char const* value = std::getenv(name);
        return value && *value ? value : nullptr;
    }

    /// LiquidType.dbc's magma and slime entries (the harness has no DBC store): the same answer for both casters.
    bool Deadly(uint32_t type)
    {
        static std::set<uint32_t> const deadly = { 3, 4, 15, 19, 20, 21 };
        return deadly.contains(type);
    }

    /// A map as a Map would hold it round a point: its static tree with the tiles loaded, the grids created with
    /// their terrain, a dynamic tree, and the VisionWorld MapVisionWorld would be over them.
    class DataWorld : public Vi::VisionWorld
    {
    public:
        DataWorld(std::string const& data, uint32_t mapId) : _data(data), _mapId(mapId),
            _tree(mapId, data + "vmaps")
        {
            _treeLoaded = _tree.InitMap(VMAP::VMapMgr2::getMapFileName(mapId));
        }

        ~DataWorld() override
        {
            for (auto& door : _doorModels)
                _doors.remove(*door);
        }

        /// Creates grid (x, y): its terrain and its vmap tile.
        void Create(int32_t x, int32_t y)
        {
            if (x < 0 || y < 0 || x >= Vi::GRIDS || y >= Vi::GRIDS || _grids.contains({ x, y }))
                return;
            auto terrain = std::make_unique<GridTerrainData>();
            std::string const file = Acore::StringFormat("{}maps/{:03}{:02}{:02}.map", _data, _mapId, x, y);
            if (terrain->Load(file) != TerrainMapDataReadResult::Success)
                terrain.reset();
            _grids[{ x, y }] = std::move(terrain);
            if (_treeLoaded)
                _tree.LoadMapTile(uint32(x), uint32(y));
        }

        /// The grids round (wx, wy) within `reach` yards, and one more each way.
        void CreateAround(float wx, float wy, float reach)
        {
            int32_t const x0 = int32_t(std::floor(Vi::GridU(wx + reach) / Vi::GRID_CELLS)) - 1;
            int32_t const x1 = int32_t(std::floor(Vi::GridU(wx - reach) / Vi::GRID_CELLS)) + 1;
            int32_t const y0 = int32_t(std::floor(Vi::GridU(wy + reach) / Vi::GRID_CELLS)) - 1;
            int32_t const y1 = int32_t(std::floor(Vi::GridU(wy - reach) / Vi::GRID_CELLS)) + 1;
            for (int32_t x = x0; x <= x1; ++x)
                for (int32_t y = y0; y <= y1; ++y)
                    Create(x, y);
        }

        /// A door of `displayId` at (x, y, z) facing `orientation`, in phase 1: its model as a GameObject's.
        GameObjectModel* AddDoor(uint32_t displayId, float x, float y, float z, float orientation)
        {
            struct Owner : GameObjectModelOwnerBase
            {
                uint32 Display = 0;
                G3D::Vector3 Pos;
                float Facing = 0.0f;
                bool IsSpawned() const override { return true; }
                uint32 GetDisplayId() const override { return Display; }
                uint32 GetPhaseMask() const override { return 1; }
                G3D::Vector3 GetPosition() const override { return Pos; }
                float GetOrientation() const override { return Facing; }
                float GetScale() const override { return 1.0f; }
                void DebugVisualizeCorner(G3D::Vector3 const&) const override { }
            };
            auto owner = std::make_unique<Owner>();
            owner->Display = displayId;
            owner->Pos = G3D::Vector3(x, y, z);
            owner->Facing = orientation;
            std::unique_ptr<GameObjectModel> model(GameObjectModel::Create(std::move(owner), _data));
            if (!model)
                return nullptr;
            _doors.insert(*model);
            _doors.balance();
            _doorModels.push_back(std::move(model));
            return _doorModels.back().get();
        }

        Gv::SceneSource Source() const
        {
            Gv::SceneSource source;
            source.MapId = _mapId;
            source.World = this;
            source.Static = _treeLoaded ? &_tree : nullptr;
            source.Dynamic = &_doors;
            source.Deadly = Deadly;
            return source;
        }

        // ---- MapVisionWorld's reading, over the harness's own holders ----

        Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            // StaticVMapCollisionData::GetSurfaceHit.
            Vi::SurfaceHit hit;
            if (!_treeLoaded)
                return hit;
            G3D::Vector3 const pos1 = VMAP::VMapMgr2::convertPositionToInternalRep(from.X, from.Y, from.Z);
            G3D::Vector3 const pos2 = VMAP::VMapMgr2::convertPositionToInternalRep(to.X, to.Y, to.Z);
            float const length = (pos2 - pos1).magnitude();
            if (!(length > 1e-6f) || !std::isfinite(length))
                return hit;
            G3D::Vector3 const dir = (pos2 - pos1) / length;
            float reach = length;
            G3D::Vector3 normal = G3D::Vector3::zero();
            if (!_tree.GetSurfaceIntersection(G3D::Ray::fromOriginAndDirection(pos1, dir), reach, normal))
                return hit;
            hit.Distance = reach;
            float const n = normal.magnitude();
            hit.NormalZ = n > 1e-12f ? (normal.dot(dir) > 0.0f ? -normal.z : normal.z) / n : 0.0f;
            return hit;
        }

        Vi::SurfaceHit DynamicHit(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            Vi::SurfaceHit hit;
            float distance = 0.0f;
            float normalZ = 0.0f;
            if (_doors.GetSurfaceHit(PHASE, from.X, from.Y, from.Z, to.X, to.Y, to.Z, distance, normalZ))
            {
                hit.Distance = distance;
                hit.NormalZ = normalZ;
            }
            return hit;
        }

        Vi::LiquidHit ModelLiquid(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            // StaticVMapCollisionData::GetLiquidHit.
            Vi::LiquidHit hit;
            if (!_treeLoaded)
                return hit;
            G3D::Vector3 const pos1 = VMAP::VMapMgr2::convertPositionToInternalRep(from.X, from.Y, from.Z);
            G3D::Vector3 const pos2 = VMAP::VMapMgr2::convertPositionToInternalRep(to.X, to.Y, to.Z);
            float const length = (pos2 - pos1).magnitude();
            if (!(length > 1e-6f) || !std::isfinite(length))
                return hit;
            float reach = length;
            uint32 type = 0;
            if (!_tree.GetLiquidIntersection(G3D::Ray::fromOriginAndDirection(pos1, (pos2 - pos1) / length), reach,
                type))
                return hit;
            hit.Distance = reach;
            hit.Deadly = Deadly(type);
            return hit;
        }

        Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            Vi::TerrainTile tile;
            auto const found = _grids.find({ tileX, tileY });
            tile.Loaded = found != _grids.end();
            if (tile.Loaded && found->second)
            {
                tile.Heights = found->second->HasHeights();
                tile.MaxHeight = found->second->GetMaxHeight();
                tile.Liquid = found->second->HasLiquid();
            }
            return tile;
        }

        Vi::TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY, bool liquid) const override
        {
            Vi::TerrainCell cell;
            auto const found = _grids.find({ tileX, tileY });
            if (found == _grids.end() || !found->second)
                return cell;
            GridTerrainData const& terrain = *found->second;
            cell.Solid = terrain.GetCellHeights(cellX, cellY, cell.Corner, cell.Centre);
            if (liquid && terrain.HasLiquid())
            {
                float const x = Vi::WorldOfU(float(tileX * Vi::GRID_CELLS + cellX) + 0.5f);
                float const y = Vi::WorldOfU(float(tileY * Vi::GRID_CELLS + cellY) + 0.5f);
                uint32 flags = 0;
                cell.Liquid = terrain.GetLiquidSurface(x, y, cell.Level, flags);
                cell.Deadly = (flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
            }
            return cell;
        }

        Animus::Movement::Liquid LiquidAt(float, float, float) const override { return {}; }

        float FloorBelow(float x, float y, float z, float search) const override
        {
            float best = Animus::Movement::INVALID_FLOOR;
            int32_t const tileX = int32_t(std::floor(Vi::GridU(x) / Vi::GRID_CELLS));
            int32_t const tileY = int32_t(std::floor(Vi::GridU(y) / Vi::GRID_CELLS));
            auto const found = _grids.find({ tileX, tileY });
            if (found != _grids.end() && found->second && found->second->HasHeights())
            {
                float const ground = found->second->getHeight(x, y);
                if (ground <= z + 0.5f && ground >= z - search)
                    best = ground;
            }
            if (_treeLoaded)
            {
                float const model = _tree.getHeight(VMAP::VMapMgr2::convertPositionToInternalRep(x, y, z + 0.5f),
                    search);
                if (std::isfinite(model) && model > best)
                    best = model;
            }
            return best;
        }

        static constexpr uint32 PHASE = 1;

    private:
        std::string _data;
        uint32_t _mapId;
        VMAP::StaticMapTree _tree;
        bool _treeLoaded = false;
        std::map<std::pair<int32_t, int32_t>, std::unique_ptr<GridTerrainData>> _grids;
        DynamicVMapCollisionData _doors;
        std::vector<std::unique_ptr<GameObjectModel>> _doorModels;
    };

    /// The device, loaded once for the whole run (as the worldserver loads it), or null.
    ForgeGpuApi const* Device()
    {
        static ForgeGpuApi const* api = []() -> ForgeGpuApi const*
        {
            char const* runtime = Env("FORGE_GPU_RUNTIME");
            char const* library = Env("FORGE_GPU_LIBRARY");
            if (!runtime || !library)
                return nullptr;
            std::string why;
            if (!Animus::Gpu::LoadFrom(runtime, library, why))
            {
                std::cout << "no device: " << why << "\n";
                return nullptr;
            }
            ForgeGpuApi const* loaded = Animus::Gpu::Api();
            if (loaded->Init(0))
            {
                std::cout << "no device: " << loaded->LastError() << "\n";
                return nullptr;
            }
            return loaded;
        }();
        return api;
    }

    uint32_t Frames()
    {
        char const* frames = Env("FORGE_VISION_FRAMES");
        return frames ? uint32_t(std::max(1, std::atoi(frames))) : 64;
    }

    void Print(std::string const& label, Gv::Renderer const& renderer, int32_t scene, int32_t tileX, int32_t tileY)
    {
        auto const mb = [](uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); };
        Gv::SceneReport const all = renderer.Report(scene);
        Gv::GridReport const grid = renderer.ReportGrid(scene, tileX, tileY);
        std::cout << Acore::StringFormat("[{}] scene: {} spawns, {} loaded (CPU tree {}), triangles {} (CPU {}); {} "
            "models ({} groups, {} triangles, {} BIH node words, {} liquids), static {:.2f} MB; {} doors; {} grids, "
            "{} with terrain ({:.2f} MB); device {:.2f} MB\n", label, all.Slots, all.LoadedSlots, all.CpuLoadedSlots,
            all.Triangles, all.CpuTriangles, all.Models, all.Counts.Groups, all.Counts.Triangles,
            all.Counts.BihNodeWords, all.Counts.Liquids, mb(all.StaticBytes), all.Doors, all.Grids,
            all.TerrainGrids, mb(all.TerrainBytes), mb(all.DeviceBytes));
        std::cout << Acore::StringFormat("[{}] grid ({}, {}): terrain {:.2f} MB, {} spawns touch it ({} triangles), "
            "{} models {:.2f} MB: {:.2f} MB with its terrain\n", label, tileX, tileY, mb(grid.TerrainBytes),
            grid.Spawns, grid.Triangles, grid.Models, mb(grid.ModelBytes), mb(grid.TerrainBytes + grid.ModelBytes));
    }

    /// One place: its grids created, its scene synced (counts checked against the CPU tree), and a diff run.
    /// `surface`, when given, is a liquid's level there: the frames whose camera is under it are counted.
    void RunPlace(std::string const& label, uint32_t mapId, float x, float y, float z, float radius,
        float surface = -1.0e9f)
    {
        char const* data = Env("FORGE_VISION_DATA");
        DataWorld world(std::string(data) + (std::string(data).back() == '/' ? "" : "/"), mapId);
        world.CreateAround(x, y, radius);
        Gv::Renderer renderer(Device());
        std::string error;
        int32_t const scene = renderer.Sync(world.Source(), error);
        ASSERT_GE(scene, 0) << error;
        int32_t const tileX = int32_t(std::floor(Vi::GridU(x) / Vi::GRID_CELLS));
        int32_t const tileY = int32_t(std::floor(Vi::GridU(y) / Vi::GRID_CELLS));
        Print(label, renderer, scene, tileX, tileY);
        Gv::SceneReport const report = renderer.Report(scene);
        EXPECT_EQ(report.LoadedSlots, report.CpuLoadedSlots);
        EXPECT_EQ(report.Triangles, report.CpuTriangles);

        Vi::Settings const settings;
        std::vector<Gv::DiffFrame> const frames = Gv::RandomFrames(world, settings, x, y, z, Frames(), radius);
        if (surface > -1.0e8f)
        {
            uint32_t under = 0;
            for (Gv::DiffFrame const& frame : frames)
                under += Vi::PlaceCamera(frame.Pose, frame.Camera, world).Camera.Z < surface;
            std::cout << Acore::StringFormat("[{}] {} of {} cameras under the surface at {}, {} above it\n", label,
                under, frames.size(), surface, frames.size() - under);
            EXPECT_GT(under, 0u) << label;
            EXPECT_LT(under, frames.size()) << label;
        }
        Gv::DiffReport const diff = Gv::RunDiff(renderer, scene, world, DataWorld::PHASE, settings, frames,
            Env("FORGE_VISION_EMULATE") != nullptr, Env("FORGE_VISION_REPEATS")
            ? uint32_t(std::max(1, std::atoi(Env("FORGE_VISION_REPEATS")))) : 1);
        for (std::string const& line : Gv::FormatDiff(diff))
            std::cout << "[" << label << "] " << line << "\n";
        EXPECT_EQ(diff.ScalarsExact, diff.Frames);
        if (diff.Device)
        {
            EXPECT_EQ(diff.GpuTally.UpscaleExact, diff.Frames);
            EXPECT_GE(diff.GpuTally.NonEdgeShare(), 0.999) << label;
            EXPECT_LE(diff.GpuTally.EdgeMismatches, diff.GpuTally.Pixels / 100) << label;
        }
    }
}

class VisionGpuDataTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Env("FORGE_VISION_DATA"))
            GTEST_SKIP() << "FORGE_VISION_DATA is not set (the data directory with maps/ and vmaps/)";
    }
};

TEST_F(VisionGpuDataTest, Stockades)
{
    RunPlace("stockades", 34, 54.23f, 0.28f, -18.34f, 30.0f);
}

TEST_F(VisionGpuDataTest, Barrens)
{
    // The Crossroads.
    RunPlace("barrens", 1, -450.0f, -2650.0f, 95.0f, 40.0f);
}

TEST_F(VisionGpuDataTest, LakeFromAboveAndBelow)
{
    // Stonebull Lake, Mulgore: its surface is at -15 and its bed 30 yd down; poses on the shore and the bed (the
    // floor under the jittered feet), so the cameras see the water from above and from under it.
    RunPlace("lake", 1, -1996.0f, -754.0f, -20.0f, 60.0f, -15.0f);
}

TEST_F(VisionGpuDataTest, Forest)
{
    // Ashenvale round Astranaar: a dense outdoor forest grid, for the scene's memory (amendment 3).
    RunPlace("ashenvale", 1, 2750.0f, -400.0f, 110.0f, 40.0f);
}

TEST_F(VisionGpuDataTest, Elwynn)
{
    RunPlace("elwynn", 0, -9450.0f, 60.0f, 56.0f, 40.0f);
}

TEST_F(VisionGpuDataTest, DoorOpenAndShut)
{
    char const* data = Env("FORGE_VISION_DATA");
    std::string const dir = std::string(data) + (std::string(data).back() == '/' ? "" : "/");
    LoadGameObjectModelList(dir);

    // A door model from the list (the first whose name says door), in front of a camera on open ground.
    uint32_t doorDisplay = 0;
    if (FILE* list = std::fopen((dir + "vmaps/" + VMAP::GAMEOBJECT_MODELS).c_str(), "rb"))
    {
        char magic[8];
        if (std::fread(magic, 1, 8, list) == 8)
            while (true)
            {
                uint32 displayId = 0;
                uint8 isWmo = 0;
                uint32 length = 0;
                char name[500];
                float bounds[6];
                if (std::fread(&displayId, 4, 1, list) != 1 || std::fread(&isWmo, 1, 1, list) != 1
                    || std::fread(&length, 4, 1, list) != 1 || length >= sizeof(name)
                    || std::fread(name, 1, length, list) != length || std::fread(bounds, 4, 6, list) != 6)
                    break;
                std::string lower(name, length);
                for (char& c : lower)
                    c = char(std::tolower(uint8(c)));
                if (lower.find("door") != std::string::npos && bounds[3] - bounds[0] > 2.0f
                    && bounds[5] - bounds[2] > 2.0f)
                {
                    doorDisplay = displayId;
                    std::cout << "[door] display " << displayId << ": " << std::string(name, length) << "\n";
                    break;
                }
            }
        std::fclose(list);
    }
    ASSERT_NE(doorDisplay, 0u) << "no door model in " << VMAP::GAMEOBJECT_MODELS;

    float const x = -450.0f;
    float const y = -2650.0f;
    DataWorld world(dir, 1);
    world.CreateAround(x, y, 30.0f);
    float const ground = world.FloorBelow(x, y, 200.0f, 400.0f);
    ASSERT_GT(ground, Animus::Movement::INVALID_FLOOR + 1.0f);
    GameObjectModel* door = world.AddDoor(doorDisplay, x + 6.0f, y, ground, 0.0f);
    ASSERT_NE(door, nullptr);

    Gv::Renderer renderer(Device());
    Vi::Settings const settings;
    std::vector<Gv::DiffFrame> frames = Gv::RandomFrames(world, settings, x, y, ground, Frames(), 2.0f);
    for (Gv::DiffFrame& frame : frames)
    {
        // Facing the door (+x), the camera turned a little either way.
        frame.Pose.Yaw = 0.0f;
        frame.Camera.YawOffset *= 0.2f;
        frame.Camera.Pitch *= 0.2f;
        frame.Camera.Zoom *= 0.3f;
    }
    for (bool shut : { true, false })
    {
        if (shut)
            door->enable(1);
        else
            door->disable();
        std::string error;
        int32_t const scene = renderer.Sync(world.Source(), error);
        ASSERT_GE(scene, 0) << error;
        Gv::DiffReport const diff = Gv::RunDiff(renderer, scene, world, DataWorld::PHASE, settings, frames, false);
        std::string const label = shut ? "door shut" : "door open";
        for (std::string const& line : Gv::FormatDiff(diff))
            std::cout << "[" << label << "] " << line << "\n";
        if (diff.Device)
        {
            EXPECT_GE(diff.GpuTally.NonEdgeShare(), 0.999) << label;
            EXPECT_EQ(diff.GpuTally.UpscaleExact, diff.Frames);
            // The door is seen while shut and gone once open.
            uint64_t const doorPixels = diff.GpuTally.CpuKinds[uint32_t(Vi::Kind::Door)];
            if (shut)
                EXPECT_GT(doorPixels, diff.GpuTally.Pixels / 200) << label;
            else
                EXPECT_EQ(doorPixels, 0u) << label;
        }
    }
}
