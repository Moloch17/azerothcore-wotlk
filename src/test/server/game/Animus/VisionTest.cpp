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

#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Vi = Animus::Vision;
namespace Mv = Animus::Movement;

namespace
{
    constexpr float DEG = Vi::DEGREES;
    constexpr float CELL = Vi::GRID_SIZE / float(Vi::GRID_CELLS);

    /// A solid axis-aligned box: a model (the static tree) or a door (the dynamic tree).
    struct Box
    {
        float X0, X1, Y0, Y1, Z0, Z1;
    };

    /// A liquid over a rectangle: a terrain cell's (cells whose centre is inside) or a WMO's (a plane over it).
    struct Pool
    {
        float X0, X1, Y0, Y1, Level;
        bool Deadly = false;

        [[nodiscard]] bool Holds(float x, float y) const { return x >= X0 && x <= X1 && y >= Y0 && y <= Y1; }
    };

    /// The distance along the segment to the box's first face (slab method), or -1.
    float SegmentBox(Vi::Vec3 from, Vi::Vec3 to, Box const& box)
    {
        Vi::Vec3 const d = to - from;
        float const length = Vi::Length(d);
        float lo = 0.0f;
        float hi = 1.0f;
        float const o[3] = { from.X, from.Y, from.Z };
        float const v[3] = { d.X, d.Y, d.Z };
        float const mins[3] = { box.X0, box.Y0, box.Z0 };
        float const maxs[3] = { box.X1, box.Y1, box.Z1 };
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::fabs(v[axis]) < 1e-9f)
            {
                if (o[axis] < mins[axis] || o[axis] > maxs[axis])
                    return -1.0f;
                continue;
            }
            float t0 = (mins[axis] - o[axis]) / v[axis];
            float t1 = (maxs[axis] - o[axis]) / v[axis];
            if (t0 > t1)
                std::swap(t0, t1);
            lo = std::max(lo, t0);
            hi = std::min(hi, t1);
            if (lo > hi)
                return -1.0f;
        }
        return lo * length;
    }

    /// GridTerrainData::getHeight's rule: the height at (fu, fv) in a cell, on the triangle holding it.
    float CellHeight(Vi::TerrainCell const& cell, float fu, float fv)
    {
        float const h1 = cell.Corner[0];
        float const h2 = cell.Corner[1];
        float const h3 = cell.Corner[2];
        float const h4 = cell.Corner[3];
        float const h5 = 2.0f * cell.Centre;
        if (fu + fv < 1.0f)
            return fu > fv ? h1 + (h2 - h1) * fu + (h5 - h1 - h2) * fv : h1 + (h5 - h1 - h3) * fu + (h3 - h1) * fv;
        return fu > fv ? (h2 + h4 - h5) * fu + (h4 - h2) * fv + (h5 - h4) : (h4 - h3) * fu + (h3 + h4 - h5) * fv
            + (h5 - h4);
    }

    /// A fake map: terrain grids (a surface sampled at each cell's corners and centre, as the extractor samples
    /// the ADT, with centres overridden one by one), liquids, models and doors. Loaded: the 3 x 3 grids round
    /// grid (32, 32), which holds x and y in (-533, 0].
    class FakeVision : public Vi::VisionWorld
    {
    public:
        std::function<float(float, float)> Surface;     // none: grids with no heights (an instance with no ADT)
        std::map<std::pair<int32_t, int32_t>, float> Centres;   // global cell (u, v) -> its centre's height
        std::set<std::pair<int32_t, int32_t>> Loaded;
        std::function<float(int32_t, int32_t)> MaxHeight = [](int32_t, int32_t) { return 1.0e4f; };
        std::vector<Box> Models;
        std::vector<Box> Doors;
        std::vector<Pool> Pools;
        std::vector<Pool> ModelPools;

        FakeVision()
        {
            for (int32_t x = 31; x <= 33; ++x)
                for (int32_t y = 31; y <= 33; ++y)
                    Loaded.insert({ x, y });
        }

        float StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Models, from, to); }
        float DynamicHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Doors, from, to); }

        Vi::LiquidHit ModelLiquid(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            Vi::LiquidHit best;
            Vi::Vec3 const d = to - from;
            float const length = Vi::Length(d);
            if (length <= 0.0f || std::fabs(d.Z) < 1e-9f)
                return best;
            for (Pool const& pool : ModelPools)
            {
                float const s = (pool.Level - from.Z) / d.Z;
                Vi::Vec3 const at = from + d * s;
                if (s >= 0.0f && s <= 1.0f && pool.Holds(at.X, at.Y) && (best.Distance < 0.0f
                    || s * length < best.Distance))
                {
                    best.Distance = s * length;
                    best.Deadly = pool.Deadly;
                }
            }
            return best;
        }

        Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            Vi::TerrainTile tile;
            tile.Loaded = Loaded.contains({ tileX, tileY });
            tile.Heights = tile.Loaded && bool(Surface);
            tile.MaxHeight = MaxHeight(tileX, tileY);
            tile.Liquid = tile.Loaded && !Pools.empty();
            return tile;
        }

        Vi::TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY, bool liquid) const override
        {
            return CellAt(tileX * Vi::GRID_CELLS + cellX, tileY * Vi::GRID_CELLS + cellY, liquid);
        }

        /// The cell at global (u, v).
        Vi::TerrainCell CellAt(int32_t u, int32_t v, bool liquid = true) const
        {
            Vi::TerrainCell cell;
            if (!Loaded.contains({ u / Vi::GRID_CELLS, v / Vi::GRID_CELLS }))
                return cell;
            if (Surface)
            {
                cell.Solid = true;
                float const offsets[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
                for (int i = 0; i < 4; ++i)
                    cell.Corner[i] = Surface(Vi::WorldOfU(float(u) + offsets[i][0]),
                        Vi::WorldOfU(float(v) + offsets[i][1]));
                auto const own = Centres.find({ u, v });
                cell.Centre = own != Centres.end() ? own->second : Surface(Vi::WorldOfU(float(u) + 0.5f),
                    Vi::WorldOfU(float(v) + 0.5f));
            }
            if (liquid)
            {
                float const x = Vi::WorldOfU(float(u) + 0.5f);
                float const y = Vi::WorldOfU(float(v) + 0.5f);
                for (Pool const& pool : Pools)
                    if (pool.Holds(x, y))
                    {
                        cell.Liquid = true;
                        cell.Level = pool.Level;
                        cell.Deadly = pool.Deadly;
                    }
            }
            return cell;
        }

        /// The terrain's height at (x, y), getHeight's answer (the old march read it); INVALID_FLOOR for none.
        float Height(float x, float y) const
        {
            float const u = Vi::GridU(x);
            float const v = Vi::GridU(y);
            Vi::TerrainCell const cell = CellAt(int32_t(std::floor(u)), int32_t(std::floor(v)), false);
            if (!cell.Solid)
                return Mv::INVALID_FLOOR;
            return CellHeight(cell, u - std::floor(u), v - std::floor(v));
        }

        Mv::Liquid LiquidAt(float x, float y, float /*z*/) const override
        {
            Mv::Liquid out;
            Vi::TerrainCell const cell = CellAt(int32_t(std::floor(Vi::GridU(x))), int32_t(std::floor(Vi::GridU(y))));
            float const ground = Height(x, y);
            // As GridTerrainData::GetLiquidData: where the ground is below the level.
            if (cell.Liquid && (ground <= Mv::INVALID_FLOOR + 1.0f || ground <= cell.Level))
            {
                out.Present = true;
                out.Level = cell.Level;
                out.Deadly = cell.Deadly;
            }
            for (Pool const& pool : ModelPools)
                if (pool.Holds(x, y))
                {
                    out.Present = true;
                    out.Level = pool.Level;
                    out.Deadly = pool.Deadly;
                }
            return out;
        }

        float FloorBelow(float x, float y, float z, float search) const override
        {
            float best = Mv::INVALID_FLOOR;
            float const ground = Height(x, y);
            if (ground > Mv::INVALID_FLOOR + 1.0f && ground <= z && ground >= z - search)
                best = ground;
            for (std::vector<Box> const* boxes : { &Models, &Doors })
                for (Box const& box : *boxes)
                    if (x >= box.X0 && x <= box.X1 && y >= box.Y0 && y <= box.Y1 && box.Z1 <= z
                        && box.Z1 >= z - search)
                        best = std::max(best, box.Z1);
            return best;
        }

        float FloorNormalZ(float /*x*/, float /*y*/, float /*z*/) const override { return 1.0f; }

    private:
        static float Nearest(std::vector<Box> const& boxes, Vi::Vec3 from, Vi::Vec3 to)
        {
            float best = -1.0f;
            for (Box const& box : boxes)
            {
                float const hit = SegmentBox(from, to, box);
                if (hit >= 0.0f && (best < 0.0f || hit < best))
                    best = hit;
            }
            return best;
        }
    };

    /// **The old march, kept for the comparison only** (05fe61d56's caster, which the ray casts replaced): the
    /// trees, then 1 yd steps to the nearest of them or the range, the terrain crossing bisected to 0.05 yd and
    /// liquids read at each step (entered from above). No units.
    Vi::Hit OldMarch(Vi::Vec3 origin, Vi::Vec3 dir, float range, FakeVision const& world, bool underwater)
    {
        constexpr float STEP = 1.0f;
        constexpr float BISECT = 0.05f;
        Vi::Vec3 const end = origin + dir * range;
        Vi::Hit best;
        best.Distance = range;
        best.Z = end.Z;
        float const model = world.StaticHit(origin, end);
        float const door = world.DynamicHit(origin, end);
        bool const modelHit = model >= 0.0f && model <= range;
        bool const doorHit = door >= 0.0f && door <= range;
        if (modelHit || doorHit)
        {
            bool const isDoor = doorHit && (!modelHit || door < model);
            best.Distance = isDoor ? door : model;
            best.What = isDoor ? Vi::Kind::Door : Vi::Kind::Model;
            best.Z = (origin + dir * best.Distance).Z;
        }
        float const limit = best.Distance;
        uint32_t const steps = uint32_t(std::ceil(limit / STEP));
        auto const below = [&](float t)
        {
            Vi::Vec3 const p = origin + dir * t;
            float const terrain = world.Height(p.X, p.Y);
            return terrain > Mv::INVALID_FLOOR + 1.0f && p.Z < terrain;
        };
        float previous = 0.0f;
        bool wasBelow = below(0.0f);
        bool above = !underwater;
        for (uint32_t step = 1; step <= steps; ++step)
        {
            float const t = std::min(float(step) * STEP, limit);
            Vi::Vec3 const p = origin + dir * t;
            float water = -1.0f;
            bool deadly = false;
            Mv::Liquid const liquid = world.LiquidAt(p.X, p.Y, p.Z);
            if (!liquid.Present || p.Z >= liquid.Level)
                above = true;
            else if (above)
            {
                water = dir.Z < -1e-6f ? std::clamp((liquid.Level - origin.Z) / dir.Z, previous, t) : t;
                deadly = liquid.Deadly;
            }
            float ground = -1.0f;
            bool const isBelow = below(t);
            if (isBelow && !wasBelow)
            {
                float lo = previous;
                float hi = t;
                while (hi - lo > BISECT)
                {
                    float const mid = 0.5f * (lo + hi);
                    (below(mid) ? hi : lo) = mid;
                }
                ground = hi;
            }
            wasBelow = isBelow;
            if (water >= 0.0f && (ground < 0.0f || water <= ground))
                return Vi::Hit{ water, deadly ? Vi::Kind::Deadly : Vi::Kind::Water, origin.Z + dir.Z * water, 1.0f };
            if (ground >= 0.0f)
                return Vi::Hit{ ground, Vi::Kind::Terrain, origin.Z + dir.Z * ground, 1.0f };
            previous = t;
        }
        return best;
    }

    float AzimuthOf(Vi::Vec3 d) { return std::atan2(d.Y, d.X); }
    float ElevationOf(Vi::Vec3 d) { return std::asin(std::clamp(d.Z, -1.0f, 1.0f)); }
    float WrapDiff(float a, float b) { return std::remainder(a - b, 2.0f * Vi::PI); }

    /// A point well inside grid (32, 32).
    constexpr float X0 = -200.0f;
    constexpr float Y0 = -200.0f;
}

// Equal-angle steps, row 0 at the top and col 0 at the left: the azimuth is the facing minus yaw' (WoW's yaw turns
// left, so the right of the image is clockwise of the facing), the elevation the pitch plus pitch' (R11).
TEST(VisionTest, PixelRaysAreEqualAngleStepsAboutTheView)
{
    Vi::Settings settings;      // 64 x 32, 120 x 60 degrees
    Vi::Rig rig;
    rig.Azimuth = 30.0f * DEG;
    rig.Elevation = -15.0f * DEG;

    struct Corner { uint32_t Row, Col; float YawRight, PitchUp; };
    float const halfH = 60.0f - 0.5f * 120.0f / 64.0f;     // 59.0625: half the field less half a pixel
    float const halfV = 30.0f - 0.5f * 60.0f / 32.0f;      // 29.0625
    for (Corner const& corner : { Corner{ 0, 0, -halfH, halfV }, Corner{ 0, 63, halfH, halfV },
             Corner{ 31, 0, -halfH, -halfV }, Corner{ 31, 63, halfH, -halfV } })
    {
        float yawRight = 0.0f;
        float pitchUp = 0.0f;
        Vi::PixelAngles(settings, corner.Row, corner.Col, yawRight, pitchUp);
        EXPECT_NEAR(yawRight, corner.YawRight * DEG, 1e-5f);
        EXPECT_NEAR(pitchUp, corner.PitchUp * DEG, 1e-5f);
        Vi::Vec3 const d = Vi::PixelDirection(rig, settings, corner.Row, corner.Col);
        EXPECT_NEAR(WrapDiff(AzimuthOf(d), rig.Azimuth - corner.YawRight * DEG), 0.0f, 1e-4f);
        EXPECT_NEAR(ElevationOf(d), rig.Elevation + corner.PitchUp * DEG, 1e-4f);
    }
    // The top left looks left of the facing (counter-clockwise, + azimuth) and up.
    Vi::Vec3 const topLeft = Vi::PixelDirection(rig, settings, 0, 0);
    EXPECT_GT(WrapDiff(AzimuthOf(topLeft), rig.Azimuth), 0.0f);
    EXPECT_GT(ElevationOf(topLeft), rig.Elevation);

    // An odd grid has a centre pixel, and it is the view itself.
    settings.Width = 65;
    settings.Height = 33;
    Vi::Vec3 const centre = Vi::PixelDirection(rig, settings, 16, 32);
    EXPECT_NEAR(WrapDiff(AzimuthOf(centre), 30.0f * DEG), 0.0f, 1e-5f);
    EXPECT_NEAR(ElevationOf(centre), -15.0f * DEG, 1e-5f);
}

// The pivot is the head (0.9 of the body), the camera behind it along the view, above it when looking down.
TEST(VisionTest, CameraSitsBehindAndAboveThePivot)
{
    FakeVision const world;
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    pose.Z = 5.0f;
    pose.BodyHeight = 2.0f;
    Vi::CameraState camera;
    camera.Pitch = -15.0f * DEG;
    camera.Zoom = 6.0f;
    Vi::Rig const rig = Vi::PlaceCamera(pose, camera, world);
    EXPECT_NEAR(rig.Pivot.Z, 6.8f, 1e-5f);
    EXPECT_NEAR(rig.Boom, 6.0f, 1e-5f);
    EXPECT_NEAR(rig.Camera.X, X0 - 6.0f * std::cos(15.0f * DEG), 1e-3f);
    EXPECT_NEAR(rig.Camera.Y, Y0, 1e-3f);
    EXPECT_NEAR(rig.Camera.Z, 6.8f + 6.0f * std::sin(15.0f * DEG), 1e-4f);
}

// The boom pulls in to 0.2 yd short of a wall (or the terrain) behind, never nearer the pivot than 0.3 yd.
TEST(VisionTest, BoomPullsInAtAWall)
{
    FakeVision world;
    Vi::Pose pose;              // facing +x
    pose.X = X0;
    pose.Y = Y0;
    Vi::CameraState camera;
    camera.Pitch = 0.0f;
    camera.Zoom = 6.0f;

    world.Models = { { X0 - 10.0f, X0 - 3.0f, Y0 - 5.0f, Y0 + 5.0f, -5.0f, 10.0f } };
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 2.8f, 1e-3f);
    world.Models = { { X0 - 10.0f, X0 - 0.4f, Y0 - 5.0f, Y0 + 5.0f, -5.0f, 10.0f } };
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 0.3f, 1e-5f);
    world.Models.clear();
    world.Doors = { { X0 - 10.0f, X0 - 4.0f, Y0 - 5.0f, Y0 + 5.0f, -5.0f, 10.0f } };
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 3.8f, 1e-3f);
    world.Doors.clear();
    // A hillside rising behind: 1.8 yd up 1.8 yd back.
    world.Surface = [](float x, float) { return X0 - x; };
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 1.6f, 2e-3f);
    world.Surface = nullptr;
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 6.0f, 1e-5f);
}

// A ray meets the terrain's own triangles at the analytic point: a flat plain, a 45-degree slope.
TEST(VisionTest, TerrainRaysHitTheTrianglesExactly)
{
    FakeVision world;
    world.Surface = [](float, float) { return 0.0f; };
    Vi::Vec3 const origin{ X0, Y0, 10.0f };
    Vi::Vec3 const down45 = Vi::Direction(0.0f, -45.0f * DEG);
    Vi::Hit hit = Vi::CastRay(origin, down45, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 10.0f * std::sqrt(2.0f), 2e-3f);
    EXPECT_NEAR(hit.NormalZ, 1.0f, 1e-5f);
    EXPECT_NEAR(hit.Z, 0.0f, 1e-3f);

    // Rising along +x at 45 degrees: the normal z is cos 45.
    world.Surface = [](float x, float) { return x - X0; };
    hit = Vi::CastRay(origin, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 10.0f, 2e-3f);
    EXPECT_NEAR(hit.NormalZ, std::sqrt(0.5f), 1e-4f);

    // From under the terrain, its underside is not seen.
    world.Surface = [](float, float) { return 0.0f; };
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, -5.0f }, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Sky);

    // The cast stops at the nearest collision hit: terrain behind a wall is not seen.
    world.Models = { { X0 + 3.0f, X0 + 4.0f, Y0 - 5.0f, Y0 + 5.0f, -20.0f, 20.0f } };
    hit = Vi::CastRay(origin, down45, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_NEAR(hit.Distance, 3.0f * std::sqrt(2.0f), 1e-3f);
}

// A thin ridge -- one cell's centre raised 10 yd over a flat plain, a peak a fraction of a yard wide at 9.5 yd --
// is hit where the old 1 yd march stepped over it.
TEST(VisionTest, ThinRidgeTheMarchSteppedOver)
{
    FakeVision world;
    world.Surface = [](float, float) { return 0.0f; };
    int32_t const u = int32_t(std::floor(Vi::GridU(X0)));
    int32_t const v = int32_t(std::floor(Vi::GridU(Y0)));
    world.Centres[{ u, v }] = 10.0f;
    // Along +x (u falling) through the cell's centre line at 9.5 yd: it meets the face h2-h4-h5 at fu = 0.525.
    float const yCentre = Vi::WorldOfU(float(v) + 0.5f);
    Vi::Vec3 const origin{ Vi::WorldOfU(float(u) + 3.0f), yCentre, 9.5f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    Vi::Hit const hit = Vi::CastRay(origin, ahead, world, {});
    ASSERT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(origin.X + hit.Distance, Vi::WorldOfU(float(u) + 0.525f), 2e-3f);
    EXPECT_NEAR(world.Height(origin.X + hit.Distance, yCentre), 9.5f, 2e-3f);
    // The old march: no step lands on the peak's top tenth of a yard.
    EXPECT_EQ(OldMarch(origin, ahead, 100.0f, world, false).What, Vi::Kind::Sky);
}

// A cell's liquid is a plane over the cell, entered from above where the ground is below it; magma and slime are
// deadly; from under the surface it is seen through.
TEST(VisionTest, LiquidPlanesFromAboveAndBelow)
{
    FakeVision world;
    world.Surface = [](float, float) { return -10.0f; };
    world.Pools = { { X0 - 50.0f, X0 + 50.0f, Y0 - 50.0f, Y0 + 50.0f, 0.0f } };
    Vi::Vec3 const down{ 0.0f, 0.0f, -1.0f };
    Vi::Hit hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 10.3f }, down, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Water);
    EXPECT_NEAR(hit.Distance, 10.3f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 10.0f }, Vi::Direction(0.3f, -30.0f * DEG), world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Water);
    EXPECT_NEAR(hit.Distance, 20.0f, 1e-3f);

    world.Pools[0].Deadly = true;
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, 10.3f }, down, world, {}).What, Vi::Kind::Deadly);
    world.Pools[0].Deadly = false;

    // From under the surface: looking up, through it to the sky; looking down, the bed.
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, -2.0f }, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, world, {}).What, Vi::Kind::Sky);
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, -2.0f }, down, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 8.0f, 2e-3f);

    // A liquid level below the ground (a cell's level under a hill) is not a surface.
    world.Surface = [](float, float) { return 1.0f; };
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 10.0f }, down, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 9.0f, 2e-3f);
}

// A WMO's liquid (the static tree's group liquids), with no terrain under it at all (map 34): water from above.
TEST(VisionTest, ModelLiquidTiles)
{
    FakeVision world;           // no heights
    world.ModelPools = { { X0 - 2.0f, X0 + 2.0f, Y0 - 1.0f, Y0 + 1.0f, -1.2f } };
    world.Models = { { X0 - 20.0f, X0 + 20.0f, Y0 - 5.0f, Y0 + 5.0f, -3.0f, -2.0f } };    // its bed
    Vi::Hit hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 3.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Water);
    EXPECT_NEAR(hit.Distance, 4.2f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);
    // Beside it, the bed.
    hit = Vi::CastRay(Vi::Vec3{ X0 + 5.0f, Y0, 3.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_NEAR(hit.Distance, 5.0f, 1e-4f);
    world.ModelPools[0].Deadly = true;
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, 3.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {}).What,
        Vi::Kind::Deadly);
}

// A ray climbing above a grid's highest point cannot hit it, and is sky -- unless a higher grid lies ahead.
TEST(VisionTest, ClimbingAboveTheMaxHeightIsSky)
{
    FakeVision world;
    world.Surface = [](float x, float) { return x > 0.0f ? 100.0f : 0.0f; };    // grid 31 (x > 0) is a plateau
    world.MaxHeight = [](int32_t tileX, int32_t) { return tileX == 31 ? 100.0f : 0.0f; };
    Vi::Vec3 const origin{ -100.0f, Y0, 5.0f };
    Vi::Breakdown breakdown;
    Vi::Hit hit = Vi::CastRay(origin, Vi::Direction(Vi::PI, 1.0f * DEG), world, {}, &breakdown);   // away, up
    EXPECT_EQ(hit.What, Vi::Kind::Sky);
    EXPECT_EQ(breakdown.TerrainCells, 0u);          // no cell of a grid it is above was tested

    // Towards the plateau, climbing slowly: it meets the plateau's edge on grid 31.
    hit = Vi::CastRay(origin, Vi::Direction(0.0f, 1.0f * DEG), world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_GT(origin.X + hit.Distance * std::cos(1.0f * DEG), -0.1f);
}

// A ray leaving the loaded grids is sky, at the edge of them: nothing loaded is left to hit.
TEST(VisionTest, LeavingTheLoadedGridsIsSky)
{
    FakeVision world;
    world.Loaded = { { 32, 32 } };
    world.Surface = [](float, float) { return 0.0f; };
    Vi::Vec3 const origin{ -100.0f, Y0, 5.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };      // towards x = 0, grid 31's edge
    EXPECT_NEAR(Vi::Reach(origin, ahead, world), 100.0f, 0.01f);
    Vi::Hit const hit = Vi::CastRay(origin, ahead, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Sky);
    EXPECT_NEAR(hit.Distance, 100.0f, 0.01f);
    // Straight up never leaves them: REACH_MAX.
    EXPECT_FLOAT_EQ(Vi::Reach(origin, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, world), Vi::REACH_MAX);
    // And no range: a hill 300 yd away inside them is seen.
    world.Loaded = { { 32, 32 } };
    world.Surface = [](float x, float) { return x > -150.0f ? 50.0f : 0.0f; };
    Vi::Hit const far = Vi::CastRay(Vi::Vec3{ -450.0f, Y0, 5.0f }, ahead, world, {});
    EXPECT_EQ(far.What, Vi::Kind::Terrain);
    EXPECT_NEAR(far.Distance, 300.0f, CELL);
}

// The static and dynamic trees are cast apart and the nearer kept: a door reads 3, a model 2 (R8); a wall's
// normal z is 0.
TEST(VisionTest, DoorsAndModelsAreToldApart)
{
    FakeVision world;
    Vi::Vec3 const origin{ X0, Y0, 1.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    world.Doors = { { X0 + 5.0f, X0 + 5.5f, Y0 - 2.0f, Y0 + 2.0f, 0.0f, 4.0f } };
    Vi::Hit hit = Vi::CastRay(origin, ahead, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Door);
    EXPECT_NEAR(hit.Distance, 5.0f, 1e-3f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 0.0f);

    world.Models = { { X0 + 8.0f, X0 + 9.0f, Y0 - 2.0f, Y0 + 2.0f, 0.0f, 4.0f } };
    EXPECT_EQ(Vi::CastRay(origin, ahead, world, {}).What, Vi::Kind::Door);
    world.Models = { { X0 + 3.0f, X0 + 4.0f, Y0 - 2.0f, Y0 + 2.0f, 0.0f, 4.0f } };
    hit = Vi::CastRay(origin, ahead, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_NEAR(hit.Distance, 3.0f, 1e-3f);

    // A model's top seen from above is a floor: its slope.
    hit = Vi::CastRay(Vi::Vec3{ X0 + 3.5f, Y0, 10.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);

    float pixel[Vi::CHANNELS];
    world.Models.clear();
    Vi::EncodePixel(Vi::CastRay(origin, ahead, world, {}), 0.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 3.0f);
}

// The objective flag: the closed segment from the camera to the hit (or the reach) within a yard of it (R12).
TEST(VisionTest, ObjectiveFlagIsTheSegmentWithinAYard)
{
    Vi::Vec3 const origin{ 0.0f, 0.0f, 0.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    Vi::Vec3 near{ 10.0f, 0.5f, 0.0f };
    Vi::Vec3 off{ 10.0f, 1.5f, 0.0f };
    Vi::Vec3 behindHit{ 10.0f, 0.0f, 0.0f };
    Vi::Vec3 atEnd{ 5.9f, 0.0f, 0.0f };
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 100.0f, &near), 1.0f);
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 100.0f, &off), 0.0f);
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 5.0f, &behindHit), 0.0f);
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 5.0f, &atEnd), 1.0f);       // the segment is closed
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 100.0f, nullptr), 0.0f);

    // Through Render: a marker on the ground ahead flags the pixels whose rays pass over it.
    FakeVision world;
    world.Surface = [](float, float) { return 0.0f; };
    Vi::Settings settings;
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    Vi::CameraState camera;
    camera.Pitch = -15.0f * DEG;
    Vi::Vec3 const marker{ X0 + 8.0f, Y0, 0.0f };
    std::vector<float> frame(Vi::ObsCount(settings));
    auto const flagged = [&](Vi::Vec3 const* objective)
    {
        Vi::Render(settings, pose, camera, world, {}, objective, frame.data());
        float sum = 0.0f;
        for (uint32_t pixel = 0; pixel < settings.Width * settings.Height; ++pixel)
            sum += frame[pixel * Vi::CHANNELS + Vi::CHANNEL_OBJECTIVE];
        return sum;
    };
    EXPECT_GT(flagged(&marker), 0.0f);
    EXPECT_FLOAT_EQ(flagged(nullptr), 0.0f);
}

// A unit is a vertical cylinder: its side reads normal 0, its top 1; hostile 6, other 7; the seat itself unseen.
TEST(VisionTest, RaysMeetUnitCylindersButNotTheSeat)
{
    FakeVision const world;
    Vi::UnitShape other{ X0 + 5.0f, Y0, 0.0f, 0.5f, 2.0f, false, false };
    Vi::UnitShape self{ X0 + 2.0f, Y0, 0.0f, 0.5f, 2.0f, false, true };
    Vi::Vec3 const origin{ X0, Y0, 1.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    std::vector<Vi::UnitShape> units{ self, other };

    Vi::Hit hit = Vi::CastRay(origin, ahead, world, units);
    EXPECT_EQ(hit.What, Vi::Kind::Other);
    EXPECT_NEAR(hit.Distance, 4.5f, 1e-3f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 0.0f);
    EXPECT_NEAR(hit.Z, 1.0f, 1e-5f);

    units[1].Hostile = true;
    EXPECT_EQ(Vi::CastRay(origin, ahead, world, units).What, Vi::Kind::Hostile);

    hit = Vi::CastRay(Vi::Vec3{ X0 + 5.0f, Y0, 10.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, units);
    EXPECT_EQ(hit.What, Vi::Kind::Hostile);
    EXPECT_NEAR(hit.Distance, 8.0f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);

    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, 3.0f }, ahead, world, units).What, Vi::Kind::Sky);
    bool top = true;
    EXPECT_NEAR(Vi::RayCylinder(origin, ahead, 100.0f, self, top), 1.5f, 1e-3f);
    EXPECT_FALSE(top);
    EXPECT_LT(Vi::RayCylinder(origin, ahead, 1.0f, self, top), 0.0f);
    std::vector<Vi::UnitShape> const alone{ self };
    EXPECT_EQ(Vi::CastRay(origin, ahead, world, alone).What, Vi::Kind::Sky);
}

// The five channels and the block's width; the distance is log-scaled to DISTANCE_REFERENCE (1000 yd).
TEST(VisionTest, ChannelsAreEncodedAsTheInterfaceSays)
{
    EXPECT_EQ(Vi::ObsCount(Vi::Settings()), 10247u);

    float pixel[Vi::CHANNELS];
    Vi::Hit hit;
    hit.What = Vi::Kind::Terrain;
    hit.Distance = 0.1f;
    hit.Z = 50.0f;
    hit.NormalZ = 0.8f;
    Vi::EncodePixel(hit, 0.0f, 1.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_DISTANCE], 0.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], 1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_NORMAL], 0.8f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_OBJECTIVE], 1.0f);

    hit.Distance = 5.0f;
    hit.Z = -30.0f;
    hit.What = Vi::Kind::Hostile;
    Vi::EncodePixel(hit, 0.0f, 0.0f, pixel);
    EXPECT_NEAR(pixel[Vi::CHANNEL_DISTANCE], std::log(20.0f) / std::log(4000.0f), 1e-6f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], -1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 6.0f);

    hit.Distance = 1000.0f;
    Vi::EncodePixel(hit, 0.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_DISTANCE], 1.0f);
    hit.Distance = 2500.0f;
    Vi::EncodePixel(hit, 0.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_DISTANCE], 1.0f);

    hit.Distance = 10.0f;
    hit.Z = 12.5f;
    Vi::EncodePixel(hit, 0.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], 0.5f);

    hit = Vi::Hit();
    hit.Distance = 300.0f;
    hit.Z = 80.0f;
    Vi::EncodePixel(hit, 0.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_DISTANCE], 1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], 0.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_NORMAL], 0.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 0.0f);
}

// A frame: [row][col][channel] with row 0 at the top, then the seven scalars (R16).
TEST(VisionTest, FrameLayoutAndScalars)
{
    FakeVision world;
    world.Surface = [](float, float) { return 0.0f; };
    Vi::Settings settings;
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    pose.Airborne = true;
    Vi::CameraState camera;
    camera.Pitch = -15.0f * DEG;
    camera.Zoom = 6.0f;
    std::vector<float> frame(Vi::ObsCount(settings), -7.0f);
    uint32_t const rays = Vi::Render(settings, pose, camera, world, {}, nullptr, frame.data());
    EXPECT_EQ(rays, 64u * 32u + 1u);
    for (float value : frame)
        EXPECT_NE(value, -7.0f);

    auto const at = [&](uint32_t row, uint32_t col, uint32_t channel)
    {
        return frame[(std::size_t(row) * settings.Width + col) * Vi::CHANNELS + channel];
    };
    EXPECT_FLOAT_EQ(at(0, 32, Vi::CHANNEL_KIND), float(Vi::Kind::Sky));
    EXPECT_FLOAT_EQ(at(31, 32, Vi::CHANNEL_KIND), float(Vi::Kind::Terrain));
    EXPECT_LT(at(31, 32, Vi::CHANNEL_DISTANCE), at(20, 32, Vi::CHANNEL_DISTANCE));
    EXPECT_NEAR(at(31, 32, Vi::CHANNEL_HEIGHT), 0.0f, 1e-3f);

    float const* scalars = frame.data() + Vi::ImageCount(settings);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_YAW_OFFSET], 0.0f);
    EXPECT_NEAR(scalars[Vi::SCALAR_PITCH], -1.0f / 6.0f, 1e-5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_ZOOM], 0.5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_BOOM], 0.5f);
    EXPECT_NEAR(scalars[Vi::SCALAR_PIVOT_HEIGHT], 0.18f, 1e-5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_UNDERWATER], 0.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_AIRBORNE], 1.0f);

    world.Surface = [](float, float) { return -30.0f; };
    world.Pools = { { X0 - 50.0f, X0 + 50.0f, Y0 - 50.0f, Y0 + 50.0f, 20.0f } };
    Vi::Render(settings, pose, camera, world, {}, nullptr, frame.data());
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_PIVOT_HEIGHT], 1.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_UNDERWATER], 1.0f);
}

// The ray casts against the old march on one fake world (rolling terrain, a lake, a wall, a door): the same kinds,
// distances within the march's own tolerance, except where the march was wrong. Its errors, by kind: it stepped over
// a crossing a ray left again within a yard (a grazed crest or a thin edge); its range (100 yd) cut the ray short; or
// a step landed under a liquid's level in a cell with liquid, and the march put the surface where the ray crossed
// the level in a cell with none -- the ray entered the liquid through its side, where the cast (a plane over each
// liquid cell) sees no surface; or its step landed past a shore, so it saw the land behind the water's edge.
TEST(VisionTest, RaycastsAgainstTheOldMarch)
{
    FakeVision world;
    // Rolling ground (never below -5) with a basin 12 yd deep round (X0 + 25, Y0), and a lake at -6 over it: its
    // shore is wherever the basin's side climbs through -6, all inside the lake's rectangle.
    world.Surface = [](float x, float y)
    {
        float const dx = x - (X0 + 25.0f);
        float const dy = y - Y0;
        return 3.0f * std::sin(x / 7.0f) + 2.0f * std::cos(y / 5.0f) - 12.0f * std::exp(-(dx * dx + dy * dy) / 120.0f);
    };
    world.Pools = { { X0 + 5.0f, X0 + 45.0f, Y0 - 20.0f, Y0 + 20.0f, -6.0f } };
    world.Models = { { X0 + 20.0f, X0 + 21.0f, Y0 + 20.0f, Y0 + 30.0f, -10.0f, 12.0f } };
    world.Doors = { { X0 + 15.0f, X0 + 15.5f, Y0 - 30.0f, Y0 - 25.0f, -10.0f, 8.0f } };
    Vi::Settings settings;
    uint32_t agree = 0;
    uint32_t steppedOver = 0;
    uint32_t pastRange = 0;
    uint32_t sideEntry = 0;
    uint32_t shore = 0;
    uint32_t other = 0;
    float worst = 0.0f;
    for (float yaw : { 0.0f, 50.0f, 140.0f, 230.0f, 300.0f })
        for (float height : { 4.0f, 9.0f })
        {
            Vi::Vec3 const camera{ X0, Y0, height };
            Vi::Rig rig;
            rig.Camera = camera;
            rig.Azimuth = yaw * DEG;
            rig.Elevation = -15.0f * DEG;
            for (uint32_t row = 0; row < settings.Height; ++row)
                for (uint32_t col = 0; col < settings.Width; ++col)
                {
                    Vi::Vec3 const dir = Vi::PixelDirection(rig, settings, row, col);
                    Vi::Hit const now = Vi::CastRay(camera, dir, world, {});
                    Vi::Hit const old = OldMarch(camera, dir, 100.0f, world, false);
                    bool const nowSky = now.What == Vi::Kind::Sky;
                    if (now.What == old.What && (nowSky || std::fabs(now.Distance - old.Distance) <= 0.1f))
                    {
                        ++agree;
                        if (!nowSky)
                            worst = std::max(worst, std::fabs(now.Distance - old.Distance));
                    }
                    else if (old.What == Vi::Kind::Sky && now.Distance > 100.0f)
                        ++pastRange;
                    else if ((old.What == Vi::Kind::Water || old.What == Vi::Kind::Deadly) && [&]
                        {
                            // Where the march put the surface, the cell has no liquid: its step after landed under
                            // the level in a cell that has.
                            Vi::Vec3 const at = camera + dir * old.Distance;
                            return !world.LiquidAt(at.X, at.Y, at.Z).Present;
                        }())
                        ++sideEntry;
                    else if ((now.What == Vi::Kind::Water || now.What == Vi::Kind::Deadly)
                        && old.What == Vi::Kind::Terrain && now.Distance <= old.Distance)
                        ++shore;    // the surface just before the shore: the march's step landed past it, on land
                    else if (now.Distance < old.Distance - 0.1f)
                        ++steppedOver;      // the cast found a crossing nearer than any the march's steps sampled
                    else
                    {
                        ++other;
                        ADD_FAILURE() << "ray at yaw " << yaw << " row " << row << " col " << col << ": now kind "
                            << int(now.What) << " at " << now.Distance << ", old kind " << int(old.What) << " at "
                            << old.Distance;
                    }
                }
        }
    std::cout << "[ vision ] raycast vs march: " << agree << " agree (worst " << worst << " yd), " << steppedOver
        << " the march stepped over, " << pastRange << " past its 100 yd range, " << sideEntry
        << " the march entered a liquid through its side, " << shore << " the march stepped past a shore, " << other
        << " other\n";
    EXPECT_EQ(other, 0u);
    EXPECT_GT(agree, (agree + steppedOver + sideEntry + shore) * 9 / 10);
}

// The cost (R22) on two fake scenes: a hallway of models with a door and a WMO gutter, no terrain (as map 34), and
// rolling terrain with a lake. The fake trees and grids cost next to nothing, so this measures the caster's own
// work (the cell traversal and triangles, the units), not VMAP's or GridTerrainData's: `forge camera snapshot`
// measures the live map.
TEST(VisionTest, TimingHarness)
{
    FakeVision hallway;
    hallway.Models = {
        { X0 - 10.0f, X0 + 120.0f, Y0 - 5.0f, Y0 - 4.0f, -2.0f, 8.0f },     // the left wall
        { X0 - 10.0f, X0 + 120.0f, Y0 + 4.0f, Y0 + 5.0f, -2.0f, 8.0f },     // the right wall
        { X0 - 10.0f, X0 + 120.0f, Y0 - 5.0f, Y0 + 3.0f, -2.0f, -1.0f },    // the floor (its top at -1)
        { X0 - 10.0f, X0 + 120.0f, Y0 + 3.0f, Y0 + 4.0f, -3.0f, -2.0f },    // the gutter's bed
        { X0 - 10.0f, X0 + 120.0f, Y0 - 5.0f, Y0 + 5.0f, 6.0f, 7.0f },      // the ceiling
        { X0 + 110.0f, X0 + 111.0f, Y0 - 5.0f, Y0 + 5.0f, -2.0f, 8.0f },    // the end
    };
    hallway.Doors = { { X0 + 40.0f, X0 + 40.5f, Y0 - 4.0f, Y0 - 1.0f, -1.0f, 4.0f } };
    hallway.ModelPools = { { X0 - 10.0f, X0 + 120.0f, Y0 + 3.0f, Y0 + 4.0f, -1.2f } };

    FakeVision field;
    field.Surface = [](float x, float y) { return 3.0f * std::sin(x / 7.0f) + 2.0f * std::cos(y / 5.0f); };
    field.Pools = { { X0 + 10.0f, X0 + 40.0f, Y0 - 15.0f, Y0 + 15.0f, 1.5f } };
    field.MaxHeight = [](int32_t, int32_t) { return 5.0f; };    // the height header's: the field's highest point

    Vi::Settings const settings;
    Vi::CameraState camera;
    camera.Pitch = settings.Pitch * DEG;
    camera.Zoom = settings.Zoom;
    std::vector<float> frame(Vi::ObsCount(settings));
    std::vector<Vi::UnitShape> crowd;
    for (int i = 0; i < 20; ++i)
        crowd.push_back(Vi::UnitShape{ X0 + 10.0f + 4.0f * float(i), Y0 + float(i % 5) - 2.0f, -1.0f, 0.4f, 2.0f,
            i % 2 == 0, false });

    struct Scene { char const* Name; FakeVision const* World; float FeetZ; };
    for (Scene const& scene : { Scene{ "hallway", &hallway, -1.0f }, Scene{ "field", &field, 4.0f } })
        for (bool withUnits : { false, true })
        {
            Vi::Pose pose;
            pose.X = X0;
            pose.Y = Y0;
            pose.Z = scene.FeetZ;
            std::span<Vi::UnitShape const> const seen = withUnits ? std::span<Vi::UnitShape const>(crowd)
                : std::span<Vi::UnitShape const>();
            constexpr int FRAMES = 10;
            uint32_t rays = 0;
            auto const start = std::chrono::steady_clock::now();
            for (int i = 0; i < FRAMES; ++i)
                rays = Vi::Render(settings, pose, camera, *scene.World, seen, nullptr, frame.data());
            double const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count() / FRAMES;
            Vi::Breakdown breakdown;
            Vi::Render(settings, pose, camera, *scene.World, seen, nullptr, frame.data(), &breakdown);
            std::cout << "[ vision ] " << scene.Name << ", " << seen.size() << " units: " << us << " us/frame, "
                << rays << " rays/frame; with clocks: trees " << double(breakdown.TreeNs) / 1e3 << " us ("
                << breakdown.TreeCasts << " casts), WMO liquids " << double(breakdown.LiquidNs) / 1e3 << " us ("
                << breakdown.LiquidCasts << "), terrain " << double(breakdown.TerrainNs) / 1e3 << " us ("
                << breakdown.TerrainTiles << " grids, " << breakdown.TerrainCells << " cells), units "
                << double(breakdown.UnitNs) / 1e3 << " us (" << breakdown.UnitTests << " tests)\n";
            EXPECT_EQ(rays, 64u * 32u + 1u);
            EXPECT_EQ(breakdown.Rays, rays);
        }
}
