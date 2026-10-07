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

#include "BoundingIntervalHierarchy.h"
#include "GpuRuntime.h"
#include "ModelIgnoreFlags.h"
#include "ModelInstance.h"
#include "VisionDiff.h"
#include "VisionGpu.h"
#include "WorldModel.h"
#include "gtest/gtest.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <random>

/// The GPU camera's scene and request packing (camera-vision.GPU.md G1/G2), on the host: the kernel's own code
/// (VisionDevice.h, compiled here for the host) against the CPU code it ports -- ModelInstance's ray and liquid
/// tests through a packed model, BIH::intersectRay, the terrain packing, and whole frames on a fake map against
/// Vision::Render. Nothing here needs a GPU; the data harness (VisionGpuDataTest) runs the device.
namespace
{
    namespace Gv = Animus::GpuVision;
    namespace Vi = Animus::Vision;

    G3D::AABox BoundOf(std::vector<G3D::Vector3> const& points)
    {
        G3D::AABox box(points.front());
        for (G3D::Vector3 const& p : points)
            box.merge(p);
        return box;
    }

    /// A model of `groups` groups of random triangles (and, when asked, a tiled liquid in the first and a flat one
    /// in the second), built in memory as the vmap loader builds one.
    std::shared_ptr<VMAP::WorldModel> MakeModel(std::mt19937& random, uint32_t groups, uint32_t triangles,
        bool liquids)
    {
        std::uniform_real_distribution<float> coordinate(-10.0f, 10.0f);
        std::vector<VMAP::GroupModel> models;
        for (uint32_t g = 0; g < groups; ++g)
        {
            std::vector<G3D::Vector3> vertices;
            std::vector<VMAP::MeshTriangle> tris;
            G3D::Vector3 const offset(coordinate(random) * 2.0f, coordinate(random) * 2.0f, coordinate(random));
            for (uint32_t t = 0; t < triangles; ++t)
            {
                G3D::Vector3 const centre = offset + G3D::Vector3(coordinate(random), coordinate(random),
                    coordinate(random) * 0.5f);
                uint32_t const base = uint32_t(vertices.size());
                for (int k = 0; k < 3; ++k)
                    vertices.push_back(centre + G3D::Vector3(coordinate(random), coordinate(random),
                        coordinate(random)) * 0.3f);
                tris.emplace_back(base, base + 1, base + 2);
            }
            VMAP::GroupModel group(0, g, BoundOf(vertices));
            group.setMeshData(vertices, tris);
            if (liquids && g < 2)
            {
                bool const tiled = g == 0;
                G3D::AABox const& bound = group.GetBound();
                VMAP::WmoLiquid* liquid = new VMAP::WmoLiquid(tiled ? 6 : 0, tiled ? 5 : 0, bound.low(), 13 + g);
                float* heights = liquid->GetHeightStorage();
                uint8* flags = liquid->GetFlagsStorage();
                float const level = (bound.low().z + bound.high().z) / 2.0f;
                if (tiled)
                {
                    for (uint32_t i = 0; i < 7 * 6; ++i)
                        heights[i] = level + coordinate(random) * 0.05f;
                    for (uint32_t i = 0; i < 6 * 5; ++i)
                        flags[i] = (i % 7 == 3) ? 0x0F : 0x00;
                }
                else
                    heights[0] = level;
                group.setLiquidData(liquid);
            }
            models.push_back(group);
        }
        auto model = std::make_shared<VMAP::WorldModel>();
        model->setGroupModels(models);
        return model;
    }

    /// A spawn of `model`: rotated, scaled and placed, its bound the transformed vertices' (TileAssembler's).
    VMAP::ModelInstance MakeSpawn(std::shared_ptr<VMAP::WorldModel> const& model, G3D::Vector3 const& pos,
        G3D::Vector3 const& rotation, float scale, uint32_t flags)
    {
        VMAP::ModelSpawn spawn;
        spawn.flags = flags | VMAP::MOD_HAS_BOUND;
        spawn.adtId = 0;
        spawn.ID = 1;
        spawn.iPos = pos;
        spawn.iRot = rotation;
        spawn.iScale = scale;
        spawn.iBound = G3D::AABox(pos);
        VMAP::ModelInstance probe(spawn, model);
        G3D::Matrix3 const rotate = probe.GetInvRot().transpose();
        std::vector<G3D::Vector3> world;
        for (VMAP::GroupModel const& group : model->GetGroups())
            for (G3D::Vector3 const& v : group.GetVertices())
                world.push_back(rotate * (v * scale) + pos);
        for (VMAP::GroupModel const& group : model->GetGroups())
            for (int corner = 0; corner < 8; ++corner)
                world.push_back(rotate * (group.GetBound().corner(corner) * scale) + pos);
        spawn.iBound = BoundOf(world);
        return VMAP::ModelInstance(spawn, model);
    }

    Gv::V3 ToV3(G3D::Vector3 const& v)
    {
        return Gv::Make(v.x, v.y, v.z);
    }

    /// A random ray from round the spawn, towards it more often than not.
    G3D::Ray RandomRay(std::mt19937& random, G3D::Vector3 const& target)
    {
        std::uniform_real_distribution<float> around(-40.0f, 40.0f);
        G3D::Vector3 const origin = target + G3D::Vector3(around(random), around(random), around(random) * 0.5f);
        G3D::Vector3 aim = target + G3D::Vector3(around(random), around(random), around(random)) * 0.3f - origin;
        if (aim.magnitude() < 1e-3f)
            aim = G3D::Vector3(1.0f, 0.0f, 0.0f);
        return G3D::Ray::fromOriginAndDirection(origin, aim / aim.magnitude());
    }

    /// Terrain alone, as the vision tests fake it: rolling ground over the 3 x 3 grids round (32, 32), a pond and a
    /// lava pool, a hole; no trees (so the GPU's empty static tree and doors are the fake's).
    class FakeTerrain : public Vi::VisionWorld
    {
    public:
        static float Ground(float x, float y)
        {
            return 3.0f * std::sin(x * 0.05f) + 2.0f * std::cos(y * 0.07f) + 0.01f * x;
        }

        Vi::SurfaceHit StaticHit(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::SurfaceHit DynamicHit(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::LiquidHit ModelLiquid(Vi::Vec3, Vi::Vec3) const override { return {}; }

        Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            Vi::TerrainTile tile;
            tile.Loaded = tileX >= 31 && tileX <= 33 && tileY >= 31 && tileY <= 33;
            tile.Heights = tile.Loaded && !(tileX == 33 && tileY == 31);
            tile.MaxHeight = 6.0f;
            tile.Liquid = tile.Loaded && tileX == 32 && tileY == 32;
            return tile;
        }

        Vi::TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY, bool liquid) const override
        {
            Vi::TerrainCell cell;
            Vi::TerrainTile const tile = Tile(tileX, tileY);
            int32_t const u = tileX * Vi::GRID_CELLS + cellX;
            int32_t const v = tileY * Vi::GRID_CELLS + cellY;
            if (tile.Heights && !(cellX == 40 && cellY == 41))
            {
                cell.Solid = true;
                float const offsets[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
                for (int i = 0; i < 4; ++i)
                    cell.Corner[i] = Ground(Vi::WorldOfU(float(u) + offsets[i][0]),
                        Vi::WorldOfU(float(v) + offsets[i][1]));
                cell.Centre = Ground(Vi::WorldOfU(float(u) + 0.5f), Vi::WorldOfU(float(v) + 0.5f)) + 0.1f;
            }
            if (liquid && tile.Liquid)
            {
                float const x = Vi::WorldOfU(float(u) + 0.5f);
                float const y = Vi::WorldOfU(float(v) + 0.5f);
                if (x > -260.0f && x < -200.0f && y > -260.0f && y < -180.0f)
                {
                    cell.Liquid = true;
                    cell.Level = 1.5f;
                }
                else if (x > -150.0f && x < -130.0f && y > -300.0f && y < -280.0f)
                {
                    cell.Liquid = true;
                    cell.Level = 2.5f;
                    cell.Deadly = true;
                }
            }
            return cell;
        }

        Animus::Movement::Liquid LiquidAt(float, float, float) const override { return {}; }
        float FloorBelow(float x, float y, float z, float search) const override
        {
            float const ground = Ground(x, y);
            return ground <= z && ground >= z - search ? ground : Animus::Movement::INVALID_FLOOR;
        }
    };
}

TEST(VisionGpuTest, PackedSpawnCastsAsModelInstance)
{
    std::mt19937 random(7);
    for (uint32_t groups : { 1u, 5u })
    {
        std::shared_ptr<VMAP::WorldModel> const model = MakeModel(random, groups, 60, false);
        G3D::Vector3 const pos(1200.0f, -340.0f, 55.0f);
        VMAP::ModelInstance const spawn = MakeSpawn(model, pos, G3D::Vector3(4.0f, 37.0f, -12.0f), 1.3f, 0);

        Gv::Words pool(1, 0);
        uint32_t const offset = Gv::PackModel(*model, pool, nullptr);
        uint32_t record[Gv::INSTANCE_WORDS] = {};
        Gv::PackInstance(spawn, offset, record);

        uint32_t hits = 0;
        uint32_t agree = 0;
        uint32_t const rays = 4000;
        for (uint32_t i = 0; i < rays; ++i)
        {
            G3D::Ray const ray = RandomRay(random, pos);
            float const reach = std::uniform_real_distribution<float>(5.0f, 80.0f)(random);
            float cpuDistance = reach;
            G3D::Vector3 cpuNormal = G3D::Vector3::zero();
            bool const cpuHit = spawn.intersectRay(ray, cpuDistance, false, VMAP::ModelIgnoreFlags::Nothing,
                &cpuNormal);
            float gpuDistance = reach;
            Gv::V3 gpuNormal = Gv::Make(0.0f, 0.0f, 0.0f);
            Gv::BihStackNode nodes[Gv::MAX_STACK];
            bool overflow = false;
            bool const gpuHit = Gv::InstanceRay(record, pool.data(), ToV3(ray.origin()), ToV3(ray.direction()),
                gpuDistance, gpuNormal, Gv::BihStack{ nodes, Gv::MAX_STACK, &overflow });
            EXPECT_FALSE(overflow);
            hits += cpuHit;
            if (cpuHit != gpuHit)
                continue;
            if (cpuHit && (std::fabs(cpuDistance - gpuDistance) > 1e-4f * std::max(1.0f, cpuDistance)
                || std::fabs(cpuNormal.z - gpuNormal.Z) > 1e-4f * std::max(1.0f, std::fabs(cpuNormal.z))))
                continue;
            ++agree;
        }
        EXPECT_GT(hits, rays / 50) << groups << " groups: the rays should hit the model often";
        // The same arithmetic in the same compiler: every ray agrees but for a rounding at a triangle's very edge.
        EXPECT_GE(agree, rays - rays / 1000) << groups << " groups";
    }
}

TEST(VisionGpuTest, PackedLiquidMatchesModelInstance)
{
    std::mt19937 random(11);
    std::shared_ptr<VMAP::WorldModel> const model = MakeModel(random, 3, 20, true);
    G3D::Vector3 const pos(-800.0f, 2100.0f, -15.0f);
    VMAP::ModelInstance const spawn = MakeSpawn(model, pos, G3D::Vector3(0.0f, 21.0f, 0.0f), 0.9f, 0);
    // An M2 never has liquid (ModelInstance::intersectLiquid).
    VMAP::ModelInstance const m2 = MakeSpawn(model, pos, G3D::Vector3(0.0f, 21.0f, 0.0f), 0.9f, VMAP::MOD_M2);

    Gv::Words pool(1, 0);
    uint32_t const offset = Gv::PackModel(*model, pool, [](uint32_t type) { return type == 14; });
    uint32_t record[Gv::INSTANCE_WORDS] = {};
    uint32_t m2Record[Gv::INSTANCE_WORDS] = {};
    Gv::PackInstance(spawn, offset, record);
    Gv::PackInstance(m2, offset, m2Record);

    uint32_t hits = 0;
    uint32_t agree = 0;
    uint32_t const rays = 4000;
    for (uint32_t i = 0; i < rays; ++i)
    {
        G3D::Ray const ray = RandomRay(random, pos);
        float cpuDistance = 100.0f;
        uint32 cpuType = 0;
        bool const cpuHit = spawn.intersectLiquid(ray, cpuDistance, cpuType);
        float gpuDistance = 100.0f;
        Gv::LiquidFound found = { 0, false };
        Gv::BihStackNode nodes[Gv::MAX_STACK];
        bool overflow = false;
        Gv::BihStack const stack = { nodes, Gv::MAX_STACK, &overflow };
        bool const gpuHit = Gv::InstanceLiquid(record, pool.data(), ToV3(ray.origin()), ToV3(ray.direction()),
            gpuDistance, found, stack);
        float m2Distance = 100.0f;
        Gv::LiquidFound m2Found = { 0, false };
        EXPECT_FALSE(Gv::InstanceLiquid(m2Record, pool.data(), ToV3(ray.origin()), ToV3(ray.direction()),
            m2Distance, m2Found, stack));
        hits += cpuHit;
        if (cpuHit == gpuHit && (!cpuHit || (std::fabs(cpuDistance - gpuDistance) <= 1e-4f * cpuDistance
            && cpuType == found.Type && found.Deadly == (cpuType == 14))))
            ++agree;
    }
    EXPECT_GT(hits, rays / 50);
    EXPECT_GE(agree, rays - rays / 1000);
}

TEST(VisionGpuTest, StackDepthBoundsTheWalk)
{
    // A tree's push depth bounds what its walk holds: a stack of exactly that never drops a node; a stack too
    // small does, and says so.
    std::mt19937 random(5);
    std::uniform_real_distribution<float> coordinate(-200.0f, 200.0f);
    std::vector<G3D::AABox> boxes;
    for (int i = 0; i < 2000; ++i)
    {
        G3D::Vector3 const low(coordinate(random), coordinate(random), coordinate(random) * 0.2f);
        boxes.emplace_back(low, low + G3D::Vector3(4.0f, 4.0f, 4.0f));
    }
    auto bounds = [](G3D::AABox const& box, G3D::AABox& out) { out = box; };
    BIH tree;
    tree.build(boxes, bounds);
    uint32_t const depth = Gv::BihPushDepth(tree);
    EXPECT_GT(depth, 3u);
    EXPECT_LT(depth, uint32_t(Gv::MAX_STACK));
    Gv::Words words(Gv::BIH_WORDS, 0);
    Gv::PackBih(tree, words, 0);

    std::vector<Gv::BihStackNode> nodes(depth);
    bool everFull = false;
    for (int i = 0; i < 3000; ++i)
    {
        G3D::Vector3 const origin(coordinate(random) * 1.5f, coordinate(random) * 1.5f, coordinate(random) * 0.1f);
        G3D::Vector3 dir(coordinate(random), coordinate(random), coordinate(random) * 0.05f);
        dir /= dir.magnitude();
        float reach = 1000.0f;
        bool overflow = false;
        // Visits that never shorten the ray: the walk reaches every leaf it crosses, the deepest the stack gets.
        Gv::BihRayOn(Gv::BihStack{ nodes.data(), int(depth), &overflow }, words.data(), 0, Gv::Make(origin.x,
            origin.y, origin.z), Gv::Make(dir.x, dir.y, dir.z), reach, [](uint32_t, float&, Gv::BihStack) { });
        ASSERT_FALSE(overflow) << "ray " << i;
        // A stack of 1 holds no split's sibling past the first: the walk says it dropped one.
        bool tight = false;
        reach = 1000.0f;
        Gv::BihRayOn(Gv::BihStack{ nodes.data(), 1, &tight }, words.data(), 0, Gv::Make(origin.x, origin.y,
            origin.z), Gv::Make(dir.x, dir.y, dir.z), reach, [](uint32_t, float&, Gv::BihStack) { });
        everFull |= tight;
    }
    EXPECT_TRUE(everFull);
    EXPECT_EQ(Gv::STACK_SIZES[Gv::StackIndexFor(depth)] >= int(depth), true);
    EXPECT_EQ(Gv::StackIndexFor(5000), Gv::STACK_COUNT - 1);
}

TEST(VisionGpuTest, BihWalkVisitsAsTheCpuTree)
{
    std::mt19937 random(3);
    std::uniform_real_distribution<float> coordinate(-200.0f, 200.0f);
    std::uniform_real_distribution<float> extent(0.5f, 12.0f);
    std::vector<G3D::AABox> boxes;
    for (int i = 0; i < 500; ++i)
    {
        G3D::Vector3 const low(coordinate(random), coordinate(random), coordinate(random) * 0.2f);
        boxes.emplace_back(low, low + G3D::Vector3(extent(random), extent(random), extent(random)));
    }
    auto bounds = [](G3D::AABox const& box, G3D::AABox& out) { out = box; };
    BIH tree;
    tree.build(boxes, bounds);
    Gv::Words words(Gv::BIH_WORDS, 0);
    Gv::PackBih(tree, words, 0);

    for (int i = 0; i < 2000; ++i)
    {
        G3D::Vector3 const origin(coordinate(random), coordinate(random), coordinate(random) * 0.3f);
        G3D::Vector3 dir(coordinate(random), coordinate(random), coordinate(random) * 0.2f);
        if (i % 50 == 0)
            dir = G3D::Vector3(0.0f, 0.0f, -1.0f);  // straight down: two axes the BIH's fuzzy test skips
        dir /= dir.magnitude();
        G3D::Ray const ray = G3D::Ray::fromOriginAndDirection(origin, dir);

        // Each box the walk reaches shortens the ray to its entry: the visits and the end are the walk's own.
        struct Visits
        {
            std::vector<G3D::AABox> const* Boxes;
            std::vector<uint32_t> Seen;
            bool operator()(G3D::Ray const& r, uint32 entry, float& maxDist, bool)
            {
                Seen.push_back(entry);
                float const t = r.intersectionTime((*Boxes)[entry]);
                if (t < maxDist)
                    maxDist = t;
                return false;
            }
        } cpu{ &boxes, {} };
        float cpuDistance = 300.0f;
        tree.intersectRay(ray, cpu, cpuDistance, false);

        std::vector<uint32_t> gpuSeen;
        float gpuDistance = 300.0f;
        Gv::BihRay(words.data(), 0, ToV3(origin), ToV3(dir), gpuDistance, [&](uint32_t entry, float& maxDist)
        {
            gpuSeen.push_back(entry);
            float const t = ray.intersectionTime(boxes[entry]);
            if (t < maxDist)
                maxDist = t;
        });
        ASSERT_EQ(cpu.Seen, gpuSeen) << "ray " << i;
        ASSERT_EQ(cpuDistance, gpuDistance) << "ray " << i;
    }
}

TEST(VisionGpuTest, TerrainPacksTheWorldsCells)
{
    FakeTerrain const world;
    Gv::Words words;
    ASSERT_TRUE(Gv::PackTerrain(world, 32, 32, words));
    ASSERT_EQ(words.size(), Gv::TERRAIN_WORDS);
    EXPECT_EQ(words[Gv::TERRAIN_HEIGHTS], 1u);
    EXPECT_EQ(words[Gv::TERRAIN_LIQUID], 1u);
    EXPECT_EQ(Gv::AsFloat(words[Gv::TERRAIN_MAX]), 6.0f);
    for (uint32_t x = 0; x < Gv::TERRAIN_CELLS; x += 7)
        for (uint32_t y = 0; y < Gv::TERRAIN_CELLS; y += 5)
        {
            Vi::TerrainCell const cell = world.Cell(32, 32, int32_t(x), int32_t(y), true);
            uint32_t const index = x * Gv::TERRAIN_CELLS + y;
            uint32_t const flags = (words[Gv::TERRAIN_FLAGS + index / 4] >> ((index % 4) * 8)) & 0xFF;
            EXPECT_EQ((flags & Gv::CELL_SOLID) != 0, cell.Solid);
            EXPECT_EQ((flags & Gv::CELL_LIQUID) != 0, cell.Liquid);
            EXPECT_EQ((flags & Gv::CELL_DEADLY) != 0, cell.Liquid && cell.Deadly);
            if (cell.Solid)
            {
                uint32_t const* v9 = &words[Gv::TERRAIN_CORNERS];
                EXPECT_EQ(Gv::AsFloat(v9[x * Gv::TERRAIN_V9 + y]), cell.Corner[0]);
                EXPECT_EQ(Gv::AsFloat(v9[(x + 1) * Gv::TERRAIN_V9 + y]), cell.Corner[1]);
                EXPECT_EQ(Gv::AsFloat(v9[x * Gv::TERRAIN_V9 + y + 1]), cell.Corner[2]);
                EXPECT_EQ(Gv::AsFloat(v9[(x + 1) * Gv::TERRAIN_V9 + y + 1]), cell.Corner[3]);
                EXPECT_EQ(Gv::AsFloat(words[Gv::TERRAIN_CENTRES + index]), cell.Centre);
            }
            if (cell.Liquid)
                EXPECT_EQ(Gv::AsFloat(words[Gv::TERRAIN_LEVELS + index]), cell.Level);
        }
    // The hole.
    uint32_t const hole = 40 * Gv::TERRAIN_CELLS + 41;
    EXPECT_EQ((words[Gv::TERRAIN_FLAGS + hole / 4] >> ((hole % 4) * 8)) & Gv::CELL_SOLID, 0u);
    // A grid with neither heights nor liquid is NoTerrain; one not created, nothing.
    EXPECT_FALSE(Gv::PackTerrain(world, 33, 31, words));
    EXPECT_TRUE(words.empty());
    EXPECT_FALSE(Gv::PackTerrain(world, 10, 10, words));
}

TEST(VisionGpuTest, RequestsCarryTheSeatsFrame)
{
    Vi::Settings settings;
    Vi::Pose pose;
    pose.X = 10.0f;
    pose.Y = 20.0f;
    pose.Z = 30.0f;
    Vi::Rig rig;
    rig.Camera = { 11.0f, 21.0f, 32.0f };
    rig.Azimuth = 1.25f;
    rig.Elevation = -0.3f;
    std::vector<Vi::UnitShape> units(3);
    units[0].Self = true;
    units[1].X = 5.0f;
    units[1].What = Vi::Class::HostileCreature;
    units[1].Entity = 7;
    units[2].Radius = 0.7f;
    Vi::Vec3 const objective{ 1.0f, 2.0f, 3.0f };
    // A box, a door the scene has (record 1) and one it does not.
    std::vector<Vi::BoxShape> boxes(1);
    boxes[0].X = 4.0f;
    boxes[0].High[2] = 1.5f;
    boxes[0].What = Vi::Class::Herb;
    boxes[0].Entity = 2;
    int const doorA = 0;
    int const doorB = 0;
    int const stranger = 0;
    std::vector<Vi::DoorShape> doors = { { &doorB, 0.0f, 0.0f, 0.0f, Vi::Class::Chest, 3 },
        { &stranger, 0.0f, 0.0f, 0.0f, Vi::Class::Door, 4 } };
    std::vector<void const*> const owners = { &doorA, &doorB };
    Vi::Sight sight(units);
    sight.Boxes = boxes;
    sight.Doors = doors;

    Gv::FrameLists packed;
    packed.Units.resize(2);  // two units of an earlier seat already there
    Vi::CameraState camera;
    camera.RenderWidth = 48;
    camera.RenderHeight = 24;
    Gv::FrameRequest const drawn = Gv::MakeRequest(settings, pose, camera, rig, sight, &objective, 3, 5, owners,
        packed);
    EXPECT_EQ(drawn.CastWidth, 48u);
    EXPECT_EQ(drawn.CastHeight, 24u);
    EXPECT_EQ(drawn.Width, 128u);
    EXPECT_EQ(drawn.Height, 64u);
    EXPECT_TRUE(Gv::Scaled(drawn));
    EXPECT_EQ(drawn.CameraX, 11.0f);
    EXPECT_EQ(drawn.CameraZ, 32.0f);
    EXPECT_EQ(drawn.Azimuth, 1.25f);
    EXPECT_EQ(drawn.Elevation, -0.3f);
    EXPECT_EQ(drawn.FeetZ, 30.0f);
    EXPECT_EQ(drawn.HasObjective, 1u);
    EXPECT_EQ(drawn.ObjectiveZ, 3.0f);
    EXPECT_EQ(drawn.Scene, 3u);
    EXPECT_EQ(drawn.PhaseMask, 5u);
    // The seat's own cylinder is left out; the others follow the earlier seat's, in order.
    EXPECT_EQ(drawn.UnitOffset, 2u);
    EXPECT_EQ(drawn.UnitCount, 2u);
    ASSERT_EQ(packed.Units.size(), 4u);
    EXPECT_EQ(packed.Units[2].X, 5.0f);
    EXPECT_EQ(packed.Units[2].Class, uint32_t(Vi::Class::HostileCreature));
    EXPECT_EQ(packed.Units[2].Entity, 7u);
    EXPECT_EQ(packed.Units[3].Radius, 0.7f);
    EXPECT_EQ(drawn.BoxOffset, 0u);
    ASSERT_EQ(drawn.BoxCount, 1u);
    EXPECT_EQ(packed.Boxes[0].X, 4.0f);
    EXPECT_EQ(packed.Boxes[0].High[2], 1.5f);
    EXPECT_EQ(packed.Boxes[0].InvRot[4], 1.0f);
    EXPECT_EQ(packed.Boxes[0].Class, uint32_t(Vi::Class::Herb));
    EXPECT_EQ(packed.Boxes[0].Entity, 2u);
    // Only the door the scene has a record of, by that record.
    ASSERT_EQ(drawn.DoorCount, 1u);
    EXPECT_EQ(packed.Doors[0].Record, 1u);
    EXPECT_EQ(packed.Doors[0].Class, uint32_t(Vi::Class::Chest));
    EXPECT_EQ(packed.Doors[0].Entity, 3u);

    // No size, or one past the canonical, casts at (or clamped to) the canonical size, as Render does.
    camera.RenderWidth = 0;
    Gv::FrameRequest const canonical = Gv::MakeRequest(settings, pose, camera, rig, sight, nullptr, 0, 1, owners,
        packed);
    EXPECT_EQ(canonical.CastWidth, 128u);
    EXPECT_EQ(canonical.CastHeight, 64u);
    EXPECT_FALSE(Gv::Scaled(canonical));
    EXPECT_EQ(canonical.HasObjective, 0u);
    camera.RenderWidth = 400;
    camera.RenderHeight = 32;
    Gv::FrameRequest const wide = Gv::MakeRequest(settings, pose, camera, rig, sight, nullptr, 0, 1, owners, packed);
    EXPECT_EQ(wide.CastWidth, 128u);
    EXPECT_EQ(wide.CastHeight, 32u);

    std::vector<Gv::FrameRequest> requests = { drawn, canonical, wide };
    std::size_t imageBytes = 0;
    std::size_t castBytes = 0;
    Gv::LayOut(requests, imageBytes, castBytes);
    EXPECT_EQ(requests[2].Frame, 2u);
    EXPECT_EQ(requests[0].ImageOffset, 0u);
    EXPECT_EQ(requests[1].ImageOffset, 128u * 64 * Vi::BYTES_PER_PIXEL);
    EXPECT_EQ(requests[2].ImageOffset, 2u * 128 * 64 * Vi::BYTES_PER_PIXEL);
    EXPECT_EQ(requests[1].ScratchOffset, 48u * 24 * Vi::BYTES_PER_PIXEL);
    EXPECT_EQ(requests[2].ScratchOffset, 48u * 24 * Vi::BYTES_PER_PIXEL + 128 * 64 * Vi::BYTES_PER_PIXEL);
    EXPECT_EQ(imageBytes, 3u * 128 * 64 * Vi::BYTES_PER_PIXEL);
    EXPECT_EQ(castBytes, std::size_t(48 * 24 + 128 * 64 + 128 * 32) * Vi::BYTES_PER_PIXEL);
}

TEST(VisionGpuTest, EmulatedFramesMatchRender)
{
    FakeTerrain const world;
    Gv::Renderer renderer(nullptr);
    Gv::SceneSource source;
    source.MapId = 9999;
    source.World = &world;
    std::string error;
    int32_t const scene = renderer.Sync(source, error);
    ASSERT_GE(scene, 0) << error;
    Gv::SceneReport const report = renderer.Report(scene);
    EXPECT_EQ(report.Grids, 9u);
    EXPECT_EQ(report.TerrainGrids, 8u);     // (33, 31) has neither heights nor liquid
    EXPECT_EQ(report.DeviceBytes, 0u);

    Vi::Settings const settings;
    // Round the pond, the lava and the hole, every render size in turn.
    // Units and colliderless boxes round each, numbered: the classes, slots and entity lists must agree too.
    std::vector<Gv::DiffFrame> frames = Gv::RandomFrames(world, settings, -210.0f, -230.0f, 5.0f, 24, 120.0f, 12,
        8);
    Gv::DiffReport const diff = Gv::RunDiff(renderer, scene, world, 1, settings, frames, true);
    for (std::string const& line : Gv::FormatDiff(diff))
        std::cout << line << "\n";
    EXPECT_FALSE(diff.Device);
    ASSERT_TRUE(diff.Emulated);
    EXPECT_EQ(diff.ScalarsExact, diff.Frames);
    EXPECT_EQ(diff.EmulatedTally.UpscaleExact, diff.Frames);
    EXPECT_EQ(diff.EmulatedOverflows, 0u);
    EXPECT_EQ(diff.EmulatedTally.BySize.size(), 4u);
    EXPECT_GE(diff.EmulatedTally.NonEdgeShare(), 0.999);
    EXPECT_LE(diff.EmulatedTally.EdgeMismatches, diff.EmulatedTally.Pixels / 100);
    // Identity exactly (perception-goals P2): no class or entity mismatch off an edge, every entity list equal,
    // and some entities seen to make the check mean something.
    EXPECT_EQ(diff.EmulatedTally.OffEdgeIdentity, 0u);
    EXPECT_EQ(diff.EmulatedTally.SlotTablesExact, diff.Frames);
    EXPECT_GT(diff.EmulatedTally.Listed, 0u);
    // Ground hazards (dungeon-curriculum I3: fire is seen): the random frames lay discs round the seat, and the device
    // draws them as the CPU does.
    EXPECT_GT(diff.EmulatedTally.CpuClasses[uint32_t(Vi::Class::GroundHazard)], 0u);
}

TEST(VisionGpuTest, InstancesShareTerrainUntilTheLastLetsGo)
{
    FakeTerrain const world;
    Gv::Renderer renderer(nullptr);
    std::string error;
    Gv::SceneSource source;
    source.MapId = 4242;
    source.World = &world;
    source.InstanceId = 1;
    int32_t const first = renderer.Sync(source, error);
    source.InstanceId = 2;
    int32_t const second = renderer.Sync(source, error);
    ASSERT_GE(first, 0) << error;
    ASSERT_GE(second, 0) << error;
    EXPECT_NE(first, second);
    // Both instances' eight terrain grids are the same eight, held once.
    EXPECT_EQ(renderer.TerrainGrids(), 8u);
    EXPECT_EQ(renderer.HostView(first).Grids[32 * Vi::GRIDS + 32], renderer.HostView(second).Grids[32 * Vi::GRIDS
        + 32]);
    renderer.Forget(4242, 1);
    EXPECT_EQ(renderer.TerrainGrids(), 8u);
    // A new instance takes the forgotten one's slot; the second keeps its own.
    source.InstanceId = 3;
    EXPECT_EQ(renderer.Sync(source, error), first);
    renderer.Forget(4242, 3);
    renderer.Forget(4242, 2);
    EXPECT_EQ(renderer.TerrainGrids(), 0u);
}

TEST(VisionGpuTest, ALibraryFromAnotherBuildIsRefused)
{
    ForgeGpuApi api{};
    std::string why;
    api.Version = FORGE_GPU_API_VERSION;
    EXPECT_TRUE(Animus::Gpu::AcceptApi(&api, "libforge-gpu.so", why));
    api.Version = FORGE_GPU_API_VERSION - 1;
    EXPECT_FALSE(Animus::Gpu::AcceptApi(&api, "/x/libforge-gpu.so", why));
    EXPECT_NE(why.find("/x/libforge-gpu.so is from another build (API version "
        + std::to_string(FORGE_GPU_API_VERSION - 1) + ", this worldserver wants "
        + std::to_string(FORGE_GPU_API_VERSION) + ")"), std::string::npos) << why;
    EXPECT_FALSE(Animus::Gpu::AcceptApi(nullptr, "libforge-gpu.so", why));
    EXPECT_NE(why.find("API version 0"), std::string::npos) << why;
}

TEST(VisionGpuTest, NothingRunsUnlessAsked)
{
    // With AnimusForge.Gpu.Observe off nothing calls the GPU camera (amendment 4): its renderer is made only by
    // the commands that ask for it, so no test or startup path here has made one.
    EXPECT_FALSE(Gv::SharedMade());
}
