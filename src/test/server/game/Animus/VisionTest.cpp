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
#include <array>
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

    /// The distance along the segment to the box's first face (slab method), or -1; `entered` the face's axis (0 x, 1 y,
    /// 2 z; -1 when the segment starts inside).
    float SegmentBox(Vi::Vec3 from, Vi::Vec3 to, Box const& box, int& entered)
    {
        entered = -1;
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
            if (t0 > lo)
                entered = axis;
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

        Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Models, from, to); }
        Vi::SurfaceHit DynamicHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Doors, from, to); }

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

    private:
        /// The nearest box face along the segment, its normal z turned to face the start: a top seen from above 1,
        /// a bottom from below -1, a side 0.
        static Vi::SurfaceHit Nearest(std::vector<Box> const& boxes, Vi::Vec3 from, Vi::Vec3 to)
        {
            Vi::SurfaceHit best;
            for (Box const& box : boxes)
            {
                int axis = -1;
                float const hit = SegmentBox(from, to, box, axis);
                if (hit >= 0.0f && (best.Distance < 0.0f || hit < best.Distance))
                {
                    best.Distance = hit;
                    best.NormalZ = axis == 2 ? (to.Z < from.Z ? 1.0f : -1.0f) : 0.0f;
                }
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
        float const model = world.StaticHit(origin, end).Distance;
        float const door = world.DynamicHit(origin, end).Distance;
        bool const modelHit = model >= 0.0f && model <= range;
        bool const doorHit = door >= 0.0f && door <= range;
        if (modelHit || doorHit)
        {
            bool const isDoor = doorHit && (!modelHit || door < model);
            best.Distance = isDoor ? door : model;
            best.What = isDoor ? Vi::Class::Door : Vi::Class::Model;
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
                return Vi::Hit{ water, deadly ? Vi::Class::Deadly : Vi::Class::Water, origin.Z + dir.Z * water, 1.0f };
            if (ground >= 0.0f)
                return Vi::Hit{ ground, Vi::Class::Terrain, origin.Z + dir.Z * ground, 1.0f };
            previous = t;
        }
        return best;
    }

    /// The size these tests were written at, before the canonical image became 128 x 64 (FREELOOK A): the pixel
    /// angles and the old march's comparison are checked on it.
    Vi::Settings Legacy()
    {
        Vi::Settings settings;
        settings.Width = 64;
        settings.Height = 32;
        return settings;
    }

    /// A frame as the vision block makes it: the image's bytes and the scalars, read back as the learner decodes it.
    struct Frame
    {
        std::vector<uint8_t> Image;
        std::array<float, Vi::SCALARS> Scalars{};

        explicit Frame(Vi::Settings const& settings) : Image(Vi::ImageBytes(settings)) { }

        [[nodiscard]] float At(uint32_t pixel, uint32_t channel) const
        {
            float decoded[Vi::DECODED_VALUES];
            Vi::DecodePixel(&Image[std::size_t(pixel) * Vi::BYTES_PER_PIXEL], decoded);
            return decoded[channel];
        }
    };

    /// A pixel encoded, then decoded.
    std::array<float, Vi::DECODED_VALUES> RoundTrip(Vi::Hit const& hit, float feetZ, bool objective, uint8_t* bytes,
        uint8_t slot = 0)
    {
        Vi::EncodePixel(hit, feetZ, objective, slot, bytes);
        std::array<float, Vi::DECODED_VALUES> out{};
        Vi::DecodePixel(bytes, out.data());
        return out;
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
    Vi::Settings settings = Legacy();      // 64 x 32, 120 x 60 degrees
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
    EXPECT_EQ(hit.What, Vi::Class::Terrain);
    EXPECT_NEAR(hit.Distance, 10.0f * std::sqrt(2.0f), 2e-3f);
    EXPECT_NEAR(hit.NormalZ, 1.0f, 1e-5f);
    EXPECT_NEAR(hit.Z, 0.0f, 1e-3f);

    // Rising along +x at 45 degrees: the normal z is cos 45.
    world.Surface = [](float x, float) { return x - X0; };
    hit = Vi::CastRay(origin, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Terrain);
    EXPECT_NEAR(hit.Distance, 10.0f, 2e-3f);
    EXPECT_NEAR(hit.NormalZ, std::sqrt(0.5f), 1e-4f);

    // From under the terrain, its underside is not seen.
    world.Surface = [](float, float) { return 0.0f; };
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, -5.0f }, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Sky);

    // The cast stops at the nearest collision hit: terrain behind a wall is not seen.
    world.Models = { { X0 + 3.0f, X0 + 4.0f, Y0 - 5.0f, Y0 + 5.0f, -20.0f, 20.0f } };
    hit = Vi::CastRay(origin, down45, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Model);
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
    ASSERT_EQ(hit.What, Vi::Class::Terrain);
    EXPECT_NEAR(origin.X + hit.Distance, Vi::WorldOfU(float(u) + 0.525f), 2e-3f);
    EXPECT_NEAR(world.Height(origin.X + hit.Distance, yCentre), 9.5f, 2e-3f);
    // The old march: no step lands on the peak's top tenth of a yard.
    EXPECT_EQ(OldMarch(origin, ahead, 100.0f, world, false).What, Vi::Class::Sky);
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
    EXPECT_EQ(hit.What, Vi::Class::Water);
    EXPECT_NEAR(hit.Distance, 10.3f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 10.0f }, Vi::Direction(0.3f, -30.0f * DEG), world, {});
    EXPECT_EQ(hit.What, Vi::Class::Water);
    EXPECT_NEAR(hit.Distance, 20.0f, 1e-3f);

    world.Pools[0].Deadly = true;
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, 10.3f }, down, world, {}).What, Vi::Class::Deadly);
    world.Pools[0].Deadly = false;

    // From under the surface: looking up, through it to the sky; looking down, the bed.
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, -2.0f }, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, world, {}).What, Vi::Class::Sky);
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, -2.0f }, down, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Terrain);
    EXPECT_NEAR(hit.Distance, 8.0f, 2e-3f);

    // A liquid level below the ground (a cell's level under a hill) is not a surface.
    world.Surface = [](float, float) { return 1.0f; };
    hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 10.0f }, down, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Terrain);
    EXPECT_NEAR(hit.Distance, 9.0f, 2e-3f);
}

// A WMO's liquid (the static tree's group liquids), with no terrain under it at all (map 34): water from above.
TEST(VisionTest, ModelLiquidTiles)
{
    FakeVision world;           // no heights
    world.ModelPools = { { X0 - 2.0f, X0 + 2.0f, Y0 - 1.0f, Y0 + 1.0f, -1.2f } };
    world.Models = { { X0 - 20.0f, X0 + 20.0f, Y0 - 5.0f, Y0 + 5.0f, -3.0f, -2.0f } };    // its bed
    Vi::Hit hit = Vi::CastRay(Vi::Vec3{ X0, Y0, 3.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Water);
    EXPECT_NEAR(hit.Distance, 4.2f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);
    // Beside it, the bed.
    hit = Vi::CastRay(Vi::Vec3{ X0 + 5.0f, Y0, 3.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Model);
    EXPECT_NEAR(hit.Distance, 5.0f, 1e-4f);
    world.ModelPools[0].Deadly = true;
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, 3.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {}).What,
        Vi::Class::Deadly);
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
    EXPECT_EQ(hit.What, Vi::Class::Sky);
    EXPECT_EQ(breakdown.TerrainCells, 0u);          // no cell of a grid it is above was tested

    // Towards the plateau, climbing slowly: it meets the plateau's edge on grid 31.
    hit = Vi::CastRay(origin, Vi::Direction(0.0f, 1.0f * DEG), world, {});
    EXPECT_EQ(hit.What, Vi::Class::Terrain);
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
    EXPECT_EQ(hit.What, Vi::Class::Sky);
    EXPECT_NEAR(hit.Distance, 100.0f, 0.01f);
    // Straight up never leaves them: REACH_MAX.
    EXPECT_FLOAT_EQ(Vi::Reach(origin, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, world), Vi::REACH_MAX);
    // And no range: a hill 300 yd away inside them is seen.
    world.Loaded = { { 32, 32 } };
    world.Surface = [](float x, float) { return x > -150.0f ? 50.0f : 0.0f; };
    Vi::Hit const far = Vi::CastRay(Vi::Vec3{ -450.0f, Y0, 5.0f }, ahead, world, {});
    EXPECT_EQ(far.What, Vi::Class::Terrain);
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
    EXPECT_EQ(hit.What, Vi::Class::Door);
    EXPECT_NEAR(hit.Distance, 5.0f, 1e-3f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 0.0f);

    world.Models = { { X0 + 8.0f, X0 + 9.0f, Y0 - 2.0f, Y0 + 2.0f, 0.0f, 4.0f } };
    EXPECT_EQ(Vi::CastRay(origin, ahead, world, {}).What, Vi::Class::Door);
    world.Models = { { X0 + 3.0f, X0 + 4.0f, Y0 - 2.0f, Y0 + 2.0f, 0.0f, 4.0f } };
    hit = Vi::CastRay(origin, ahead, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Model);
    EXPECT_NEAR(hit.Distance, 3.0f, 1e-3f);

    // A model's top seen from above is a floor: its slope.
    hit = Vi::CastRay(Vi::Vec3{ X0 + 3.5f, Y0, 10.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, {});
    EXPECT_EQ(hit.What, Vi::Class::Model);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);

    uint8_t bytes[Vi::BYTES_PER_PIXEL];
    world.Models.clear();
    EXPECT_FLOAT_EQ(RoundTrip(Vi::CastRay(origin, ahead, world, {}), 0.0f, false, bytes)[Vi::CHANNEL_CLASS], 3.0f);
    EXPECT_EQ(bytes[3], 3);
    EXPECT_EQ(bytes[4], 0);
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
    Frame frame(settings);
    auto const flagged = [&](Vi::Vec3 const* objective)
    {
        Vi::Render(settings, pose, camera, world, {}, objective, frame.Image.data(), frame.Scalars.data());
        float sum = 0.0f;
        for (uint32_t pixel = 0; pixel < settings.Width * settings.Height; ++pixel)
            sum += frame.At(pixel, Vi::CHANNEL_OBJECTIVE);
        return sum;
    };
    EXPECT_GT(flagged(&marker), 0.0f);
    EXPECT_FLOAT_EQ(flagged(nullptr), 0.0f);
}

// A unit is a vertical cylinder: its side reads normal 0, its top 1; hostile 6, other 7; the seat itself unseen.
TEST(VisionTest, RaysMeetUnitCylindersButNotTheSeat)
{
    FakeVision const world;
    Vi::UnitShape other{ X0 + 5.0f, Y0, 0.0f, 0.5f, 2.0f, Vi::Class::NeutralCreature, false };
    Vi::UnitShape self{ X0 + 2.0f, Y0, 0.0f, 0.5f, 2.0f, Vi::Class::NeutralCreature, true };
    Vi::Vec3 const origin{ X0, Y0, 1.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    std::vector<Vi::UnitShape> units{ self, other };

    Vi::Hit hit = Vi::CastRay(origin, ahead, world, units);
    EXPECT_EQ(hit.What, Vi::Class::NeutralCreature);
    EXPECT_NEAR(hit.Distance, 4.5f, 1e-3f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 0.0f);
    EXPECT_NEAR(hit.Z, 1.0f, 1e-5f);

    units[1].What = Vi::Class::HostileCreature;
    EXPECT_EQ(Vi::CastRay(origin, ahead, world, units).What, Vi::Class::HostileCreature);

    hit = Vi::CastRay(Vi::Vec3{ X0 + 5.0f, Y0, 10.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, world, units);
    EXPECT_EQ(hit.What, Vi::Class::HostileCreature);
    EXPECT_NEAR(hit.Distance, 8.0f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);

    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ X0, Y0, 3.0f }, ahead, world, units).What, Vi::Class::Sky);
    bool top = true;
    EXPECT_NEAR(Vi::RayCylinder(origin, ahead, 100.0f, self, top), 1.5f, 1e-3f);
    EXPECT_FALSE(top);
    EXPECT_LT(Vi::RayCylinder(origin, ahead, 1.0f, self, top), 0.0f);
    std::vector<Vi::UnitShape> const alone{ self };
    EXPECT_EQ(Vi::CastRay(origin, ahead, world, alone).What, Vi::Class::Sky);
}

// A pixel's five bytes (camera-vision.BYTES.md, perception-goals 1a), and their decode back to the five channels
// and the slot: distance at 0.25, 1, 100 and 1000 yd and sky; height +-25 yd and clamped; normal 0, 0.5, 1; every
// class with and without the objective, and every slot. The block's float columns are the eleven scalars; the
// canonical image is 128 x 64.
TEST(VisionTest, PixelsTravelAsFiveBytes)
{
    EXPECT_EQ(Vi::ObsCount(Vi::Settings()), 11u);
    EXPECT_EQ(Vi::BYTES_PER_PIXEL, 5u);
    EXPECT_EQ(Vi::ImageBytes(Vi::Settings()), 128u * 64u * 5u);

    uint8_t bytes[Vi::BYTES_PER_PIXEL];
    Vi::Hit hit;
    hit.What = Vi::Class::Terrain;
    hit.NormalZ = 1.0f;
    float const logRange = std::log(4000.0f);
    for (float distance : { 0.1f, 0.25f, 1.0f, 100.0f, 1000.0f, 2500.0f })
    {
        hit.Distance = distance;
        std::array<float, Vi::DECODED_VALUES> const out = RoundTrip(hit, 0.0f, false, bytes);
        float const exact = std::clamp(std::log(std::max(distance, 0.25f) / 0.25f) / logRange, 0.0f, 1.0f);
        EXPECT_EQ(bytes[0], uint8_t(std::lround(254.0f * exact))) << distance;
        EXPECT_NEAR(out[Vi::CHANNEL_DISTANCE], exact, 0.5f / 254.0f + 1e-6f) << distance;
        EXPECT_NE(bytes[0], Vi::SKY_BYTE);      // 255 is sky alone: 1000 yd and beyond is 254
    }
    hit.Distance = 0.25f;
    RoundTrip(hit, 0.0f, false, bytes);
    EXPECT_EQ(bytes[0], 0);
    hit.Distance = 1000.0f;
    RoundTrip(hit, 0.0f, false, bytes);
    EXPECT_EQ(bytes[0], 254);

    // Height over the feet in 0.2 yd steps round 128: +-25 yd is +-125 steps, and past it clamps.
    hit.Distance = 10.0f;
    struct Rise { float Dz; uint8_t Byte; float Decoded; };
    for (Rise const& rise : { Rise{ 0.0f, 128, 0.0f }, Rise{ 25.0f, 253, 1.0f }, Rise{ -25.0f, 3, -1.0f },
             Rise{ 40.0f, 253, 1.0f }, Rise{ -60.0f, 3, -1.0f }, Rise{ 1.0f, 133, 5.0f / 125.0f },
             Rise{ -0.29f, 127, -1.0f / 125.0f } })
    {
        hit.Z = 7.0f + rise.Dz;
        std::array<float, Vi::DECODED_VALUES> const out = RoundTrip(hit, 7.0f, false, bytes);
        EXPECT_EQ(bytes[1], rise.Byte) << rise.Dz;
        EXPECT_NEAR(out[Vi::CHANNEL_HEIGHT], rise.Decoded, 1e-6f) << rise.Dz;
    }

    // The normal's z in 255ths.
    hit.Z = 0.0f;
    for (float normal : { 0.0f, 0.5f, 1.0f, 1.3f, -0.2f })
    {
        hit.NormalZ = normal;
        std::array<float, Vi::DECODED_VALUES> const out = RoundTrip(hit, 0.0f, false, bytes);
        EXPECT_EQ(bytes[2], uint8_t(std::lround(255.0f * std::clamp(normal, 0.0f, 1.0f)))) << normal;
        EXPECT_NEAR(out[Vi::CHANNEL_NORMAL], std::clamp(normal, 0.0f, 1.0f), 0.5f / 255.0f + 1e-6f) << normal;
    }

    // Every class in the low five bits, the objective in bit 5, bits 6-7 clear; every slot in byte 4, whatever
    // the class (a listed entity's, or one past the list's cap: slot 0 with its class kept).
    for (uint32_t value = 0; value < Vi::CLASSES; ++value)
        for (bool objective : { false, true })
            for (uint32_t slot = 0; slot <= Vi::ENTITY_SLOTS; ++slot)
            {
                hit.What = Vi::Class(value);
                hit.Distance = 10.0f;
                std::array<float, Vi::DECODED_VALUES> const out = RoundTrip(hit, 0.0f, objective, bytes,
                    uint8_t(slot));
                EXPECT_EQ(bytes[3], uint8_t(value | (objective ? 0x20u : 0u)));
                EXPECT_EQ(bytes[3] & Vi::RESERVED_BITS, 0);
                EXPECT_EQ(Vi::ClassOfByte(bytes[3]), Vi::Class(value));
                EXPECT_EQ(bytes[4], slot);
                EXPECT_FLOAT_EQ(out[Vi::CHANNEL_CLASS], float(value));
                EXPECT_FLOAT_EQ(out[Vi::CHANNEL_OBJECTIVE], objective ? 1.0f : 0.0f);
                EXPECT_FLOAT_EQ(out[Vi::CHANNEL_SLOT], float(slot));
            }

    // The coarse kinds of revision 4 follow from the class: its first six unchanged, units hostile or other, the
    // rest game objects (the old "door").
    for (uint32_t value = 0; value <= uint32_t(Vi::Class::Deadly); ++value)
        EXPECT_EQ(uint32_t(Vi::KindOf(Vi::Class(value))), value);
    EXPECT_EQ(Vi::KindOf(Vi::Class::HostileCreature), Vi::Kind::Hostile);
    EXPECT_EQ(Vi::KindOf(Vi::Class::HostilePlayer), Vi::Kind::Hostile);
    for (Vi::Class value : { Vi::Class::NeutralCreature, Vi::Class::FriendlyCreature, Vi::Class::FriendlyPlayer,
             Vi::Class::QuestGiver, Vi::Class::Vendor, Vi::Class::Trainer, Vi::Class::LootableCorpse,
             Vi::Class::Corpse })
        EXPECT_EQ(Vi::KindOf(value), Vi::Kind::Other) << Vi::CLASS_NAMES[uint32_t(value)];
    for (Vi::Class value : { Vi::Class::Chest, Vi::Class::Herb, Vi::Class::Ore, Vi::Class::Mailbox,
             Vi::Class::QuestObject, Vi::Class::UsableObject, Vi::Class::OtherObject })
        EXPECT_EQ(Vi::KindOf(value), Vi::Kind::Door) << Vi::CLASS_NAMES[uint32_t(value)];

    // Sky: distance 255 (1.0), height 128 (0), whatever the ray's end.
    hit = Vi::Hit();
    hit.Distance = 300.0f;
    hit.Z = 80.0f;
    std::array<float, Vi::DECODED_VALUES> const sky = RoundTrip(hit, 0.0f, false, bytes);
    EXPECT_EQ(bytes[0], 255);
    EXPECT_EQ(bytes[1], 128);
    EXPECT_EQ(bytes[2], 0);
    EXPECT_EQ(bytes[3], 0);
    EXPECT_EQ(bytes[4], 0);
    EXPECT_FLOAT_EQ(sky[Vi::CHANNEL_DISTANCE], 1.0f);
    EXPECT_FLOAT_EQ(sky[Vi::CHANNEL_HEIGHT], 0.0f);

    // Every byte value decodes in range, as the learner's table does.
    for (uint32_t value = 0; value < 256; ++value)
    {
        uint8_t const in[5] = { uint8_t(value), uint8_t(value), uint8_t(value), uint8_t(value), uint8_t(value) };
        float out[Vi::DECODED_VALUES];
        Vi::DecodePixel(in, out);
        EXPECT_FLOAT_EQ(out[Vi::CHANNEL_DISTANCE], value == 255 ? 1.0f : float(value) / 254.0f);
        EXPECT_FLOAT_EQ(out[Vi::CHANNEL_HEIGHT], (float(value) - 128.0f) / 125.0f);
        EXPECT_FLOAT_EQ(out[Vi::CHANNEL_NORMAL], float(value) / 255.0f);
        EXPECT_FLOAT_EQ(out[Vi::CHANNEL_CLASS], float(value & 31));
        EXPECT_FLOAT_EQ(out[Vi::CHANNEL_OBJECTIVE], float((value >> 5) & 1));
        EXPECT_FLOAT_EQ(out[Vi::CHANNEL_SLOT], float(value));
    }
}

// A row with no frame (a director, an absent agent, a seat with no character or no map) is "nothing seen": every
// pixel sky, height 0, normal 0, no objective -- never the zeros that would read as a wall at the camera.
TEST(VisionTest, NoFrameRowIsSky)
{
    std::vector<uint8_t> row(Vi::ImageBytes(Vi::Settings()), 0x5A);
    Vi::FillNoFrame(row.data(), uint32_t(row.size()));
    for (std::size_t at = 0; at < row.size(); at += Vi::BYTES_PER_PIXEL)
    {
        ASSERT_EQ(row[at], 255);
        ASSERT_EQ(row[at + 1], 128);
        ASSERT_EQ(row[at + 2], 0);
        ASSERT_EQ(row[at + 3], 0);
        ASSERT_EQ(row[at + 4], 0);
        float out[Vi::DECODED_VALUES];
        Vi::DecodePixel(&row[at], out);
        ASSERT_FLOAT_EQ(out[Vi::CHANNEL_DISTANCE], 1.0f);
        ASSERT_FLOAT_EQ(out[Vi::CHANNEL_HEIGHT], 0.0f);
        ASSERT_FLOAT_EQ(out[Vi::CHANNEL_NORMAL], 0.0f);
        ASSERT_FLOAT_EQ(out[Vi::CHANNEL_CLASS], float(Vi::Class::Sky));
        ASSERT_FLOAT_EQ(out[Vi::CHANNEL_OBJECTIVE], 0.0f);
    }
}

// A frame: [row][col][channel] with row 0 at the top, then the eleven scalars (R16, FREELOOK B).
TEST(VisionTest, FrameLayoutAndScalars)
{
    FakeVision world;
    world.Surface = [](float, float) { return 0.0f; };
    Vi::Settings settings = Legacy();
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    pose.Airborne = true;
    Vi::CameraState camera;
    camera.Pitch = -15.0f * DEG;
    camera.Zoom = 6.0f;
    Frame frame(settings);
    frame.Scalars.fill(-7.0f);
    uint32_t const rays = Vi::Render(settings, pose, camera, world, {}, nullptr, frame.Image.data(),
        frame.Scalars.data());
    EXPECT_EQ(rays, 64u * 32u + 1u);
    for (float value : frame.Scalars)
        EXPECT_NE(value, -7.0f);

    auto const at = [&](uint32_t row, uint32_t col, uint32_t channel)
    {
        return frame.At(row * settings.Width + col, channel);
    };
    EXPECT_FLOAT_EQ(at(0, 32, Vi::CHANNEL_CLASS), float(Vi::Class::Sky));
    EXPECT_FLOAT_EQ(at(31, 32, Vi::CHANNEL_CLASS), float(Vi::Class::Terrain));
    EXPECT_LT(at(31, 32, Vi::CHANNEL_DISTANCE), at(20, 32, Vi::CHANNEL_DISTANCE));
    EXPECT_NEAR(at(31, 32, Vi::CHANNEL_HEIGHT), 0.0f, 1e-3f);

    float const* scalars = frame.Scalars.data();
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_YAW_SIN], 0.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_YAW_COS], 1.0f);
    EXPECT_NEAR(scalars[Vi::SCALAR_PITCH], -1.0f / 6.0f, 1e-5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_ZOOM], 0.5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_BOOM], 0.5f);
    EXPECT_NEAR(scalars[Vi::SCALAR_PIVOT_HEIGHT], 0.18f, 1e-5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_UNDERWATER], 0.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_AIRBORNE], 1.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_YAW_RATE], 0.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_PITCH_RATE], 0.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_RENDER_WIDTH], 1.0f);

    world.Surface = [](float, float) { return -30.0f; };
    world.Pools = { { X0 - 50.0f, X0 + 50.0f, Y0 - 50.0f, Y0 + 50.0f, 20.0f } };
    Vi::Render(settings, pose, camera, world, {}, nullptr, frame.Image.data(), frame.Scalars.data());
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_PIVOT_HEIGHT], 1.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_UNDERWATER], 1.0f);

    // With no image to write, the scalars alone and no pixel cast (only the boom).
    EXPECT_EQ(Vi::Render(settings, pose, camera, world, {}, nullptr, nullptr, frame.Scalars.data()), 1u);
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
    Vi::Settings settings = Legacy();
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
                    bool const nowSky = now.What == Vi::Class::Sky;
                    if (now.What == old.What && (nowSky || std::fabs(now.Distance - old.Distance) <= 0.1f))
                    {
                        ++agree;
                        if (!nowSky)
                            worst = std::max(worst, std::fabs(now.Distance - old.Distance));
                    }
                    else if (old.What == Vi::Class::Sky && now.Distance > 100.0f)
                        ++pastRange;
                    else if ((old.What == Vi::Class::Water || old.What == Vi::Class::Deadly) && [&]
                        {
                            // Where the march put the surface, the cell has no liquid: its step after landed under
                            // the level in a cell that has.
                            Vi::Vec3 const at = camera + dir * old.Distance;
                            return !world.LiquidAt(at.X, at.Y, at.Z).Present;
                        }())
                        ++sideEntry;
                    else if ((now.What == Vi::Class::Water || now.What == Vi::Class::Deadly)
                        && old.What == Vi::Class::Terrain && now.Distance <= old.Distance)
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

// A frame cast at a render size (FREELOOK A): the same field of view with fewer rays, scaled up by nearest pixel into
// the canonical image -- byte for byte the frame a canonical camera of that size casts, upscaled -- with the rays
// actually cast returned and the render width's share in the scalars; and the free camera's turn and rates read out.
TEST(VisionTest, RenderAtADrawnSize)
{
    FakeVision world;
    world.Surface = [](float x, float y) { return 2.0f * std::sin(x / 5.0f) + std::cos(y / 3.0f); };
    world.Pools = { { X0 + 5.0f, X0 + 30.0f, Y0 - 10.0f, Y0 + 10.0f, 0.5f } };
    Vi::Settings const settings;    // 128 x 64
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    pose.Z = 3.0f;
    Vi::Vec3 const marker{ X0 + 8.0f, Y0, 1.0f };
    for (Vi::Resolution const size : settings.RenderSizes)
    {
        Vi::CameraState camera;
        camera.Pitch = -15.0f * DEG;
        camera.Zoom = 6.0f;
        camera.RenderWidth = size.Width;
        camera.RenderHeight = size.Height;
        Frame frame(settings);
        uint32_t const rays = Vi::Render(settings, pose, camera, world, {}, &marker, frame.Image.data(),
            frame.Scalars.data());
        EXPECT_EQ(rays, size.Width * size.Height + 1u);
        EXPECT_FLOAT_EQ(frame.Scalars[Vi::SCALAR_RENDER_WIDTH], float(size.Width) / 128.0f);

        Vi::Settings small = settings;
        small.Width = size.Width;
        small.Height = size.Height;
        Vi::CameraState canonical = camera;
        canonical.RenderWidth = 0;
        canonical.RenderHeight = 0;
        Frame cast(small);
        Vi::Render(small, pose, canonical, world, {}, &marker, cast.Image.data(), cast.Scalars.data());
        std::vector<uint8_t> expected(frame.Image.size());
        Vi::Upscale(cast.Image.data(), size.Width, size.Height, expected.data(), settings.Width, settings.Height);
        EXPECT_EQ(frame.Image, expected) << size.Width << "x" << size.Height;
    }

    // The camera's own state in the scalars: the yaw offset as its sine and cosine, the held rates by their scales.
    Vi::CameraState camera;
    camera.YawOffset = 90.0f * DEG;
    camera.Pitch = -45.0f * DEG;
    camera.YawRate = -90.0f * DEG;
    camera.PitchRate = 60.0f * DEG;
    Frame frame(settings);
    EXPECT_EQ(Vi::Render(settings, pose, camera, world, {}, nullptr, frame.Image.data(), frame.Scalars.data()),
        128u * 64u + 1u);
    EXPECT_NEAR(frame.Scalars[Vi::SCALAR_YAW_SIN], 1.0f, 1e-6f);
    EXPECT_NEAR(frame.Scalars[Vi::SCALAR_YAW_COS], 0.0f, 1e-6f);
    EXPECT_NEAR(frame.Scalars[Vi::SCALAR_PITCH], -0.5f, 1e-6f);
    EXPECT_NEAR(frame.Scalars[Vi::SCALAR_YAW_RATE], -0.5f, 1e-6f);
    EXPECT_NEAR(frame.Scalars[Vi::SCALAR_PITCH_RATE], 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(frame.Scalars[Vi::SCALAR_RENDER_WIDTH], 1.0f);
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

    Vi::Settings const settings = Legacy();
    Vi::CameraState camera;
    camera.Pitch = settings.Pitch * DEG;
    camera.Zoom = settings.Zoom;
    Frame frame(settings);
    std::vector<Vi::UnitShape> crowd;
    for (int i = 0; i < 20; ++i)
        crowd.push_back(Vi::UnitShape{ X0 + 10.0f + 4.0f * float(i), Y0 + float(i % 5) - 2.0f, -1.0f, 0.4f, 2.0f,
            i % 2 == 0 ? Vi::Class::HostileCreature : Vi::Class::NeutralCreature, false });

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
                rays = Vi::Render(settings, pose, camera, *scene.World, seen, nullptr, frame.Image.data(),
                    frame.Scalars.data());
            double const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count() / FRAMES;
            Vi::Breakdown breakdown;
            Vi::Render(settings, pose, camera, *scene.World, seen, nullptr, frame.Image.data(),
                frame.Scalars.data(), &breakdown);
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
