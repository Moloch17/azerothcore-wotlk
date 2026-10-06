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
#include <vector>

namespace Vi = Animus::Vision;
namespace Mv = Animus::Movement;

namespace
{
    constexpr float DEG = Vi::DEGREES;

    /// A solid axis-aligned box: a model (the static tree) or a door (the dynamic tree).
    struct Box
    {
        float X0, X1, Y0, Y1, Z0, Z1;
    };

    /// A liquid over a rectangle.
    struct Pool
    {
        float X0, X1, Y0, Y1, Level;
        bool Deadly = false;
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

    class FakeVision : public Vi::VisionWorld
    {
    public:
        /// The heightfield, or none (at or below NO_TERRAIN: an instance with no ADT, as map 34).
        std::function<float(float, float)> Ground = [](float, float) { return Mv::INVALID_FLOOR; };
        std::vector<Box> Models;
        std::vector<Box> Doors;
        std::vector<Pool> Pools;

        float StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Models, from, to); }
        float DynamicHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Doors, from, to); }
        float TerrainHeight(float x, float y) const override { return Ground(x, y); }

        Mv::Liquid LiquidAt(float x, float y, float /*z*/) const override
        {
            Mv::Liquid out;
            for (Pool const& pool : Pools)
                if (x >= pool.X0 && x <= pool.X1 && y >= pool.Y0 && y <= pool.Y1)
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
            float const ground = Ground(x, y);
            if (ground > Vi::NO_TERRAIN && ground <= z && ground >= z - search)
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

    float AzimuthOf(Vi::Vec3 d) { return std::atan2(d.Y, d.X); }
    float ElevationOf(Vi::Vec3 d) { return std::asin(std::clamp(d.Z, -1.0f, 1.0f)); }
    float WrapDiff(float a, float b) { return std::remainder(a - b, 2.0f * Vi::PI); }
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
    pose.X = 10.0f;
    pose.Y = 20.0f;
    pose.Z = 5.0f;
    pose.BodyHeight = 2.0f;
    Vi::CameraState camera;
    camera.Pitch = -15.0f * DEG;
    camera.Zoom = 6.0f;
    Vi::Rig const rig = Vi::PlaceCamera(pose, camera, world);
    EXPECT_NEAR(rig.Pivot.Z, 6.8f, 1e-5f);
    EXPECT_NEAR(rig.Boom, 6.0f, 1e-5f);
    EXPECT_NEAR(rig.Camera.X, 10.0f - 6.0f * std::cos(15.0f * DEG), 1e-4f);
    EXPECT_NEAR(rig.Camera.Y, 20.0f, 1e-4f);
    EXPECT_NEAR(rig.Camera.Z, 6.8f + 6.0f * std::sin(15.0f * DEG), 1e-4f);
}

// The boom pulls in to 0.2 yd short of a wall behind, never nearer the pivot than 0.3 yd.
TEST(VisionTest, BoomPullsInAtAWall)
{
    FakeVision world;
    Vi::Pose pose;              // facing +x at the origin
    Vi::CameraState camera;
    camera.Pitch = 0.0f;
    camera.Zoom = 6.0f;

    world.Models = { { -10.0f, -3.0f, -5.0f, 5.0f, -5.0f, 10.0f } };
    Vi::Rig rig = Vi::PlaceCamera(pose, camera, world);
    EXPECT_NEAR(rig.Boom, 2.8f, 1e-4f);
    EXPECT_NEAR(rig.Camera.X, -2.8f, 1e-4f);

    world.Models = { { -10.0f, -0.4f, -5.0f, 5.0f, -5.0f, 10.0f } };
    rig = Vi::PlaceCamera(pose, camera, world);
    EXPECT_NEAR(rig.Boom, 0.3f, 1e-5f);

    // A door pulls it in as a wall does; the terrain too (a hillside behind).
    world.Models.clear();
    world.Doors = { { -10.0f, -4.0f, -5.0f, 5.0f, -5.0f, 10.0f } };
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 3.8f, 1e-4f);
    world.Doors.clear();
    world.Ground = [](float x, float) { return -x; };      // rising behind: 1.8 yd up 1.8 yd back
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 1.8f - 0.2f, 0.06f);

    world.Ground = [](float, float) { return Mv::INVALID_FLOOR; };
    EXPECT_NEAR(Vi::PlaceCamera(pose, camera, world).Boom, 6.0f, 1e-5f);
}

// The terrain is marched a yard at a time and the crossing bisected to 0.05 yd; its slope from the heights.
TEST(VisionTest, TerrainMarchFindsTheCrossing)
{
    FakeVision world;
    world.Ground = [](float, float) { return 0.0f; };
    Vi::Vec3 const origin{ 0.0f, 0.0f, 10.0f };
    Vi::Vec3 const down45 = Vi::Direction(0.0f, -45.0f * DEG);
    Vi::Hit hit = Vi::CastRay(origin, down45, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 10.0f * std::sqrt(2.0f), Vi::BISECT_TO);
    EXPECT_NEAR(hit.NormalZ, 1.0f, 1e-5f);

    // A 45-degree slope rising along +x: its normal z is cos 45.
    world.Ground = [](float x, float) { return x; };
    hit = Vi::CastRay(origin, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 10.0f, Vi::BISECT_TO);
    EXPECT_NEAR(hit.NormalZ, std::sqrt(0.5f), 1e-3f);

    // No terrain (a map with no ADT): only sky.
    world.Ground = [](float, float) { return Mv::INVALID_FLOOR; };
    hit = Vi::CastRay(origin, down45, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Sky);
    EXPECT_FLOAT_EQ(hit.Distance, 100.0f);

    // The march stops at the nearest collision hit: terrain behind a wall is not seen.
    world.Ground = [](float, float) { return 0.0f; };
    world.Models = { { 3.0f, 4.0f, -5.0f, 5.0f, -20.0f, 20.0f } };
    hit = Vi::CastRay(origin, down45, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_NEAR(hit.Distance, 3.0f * std::sqrt(2.0f), 1e-4f);
}

// A liquid's surface entered from above, at its plane; read where there is no terrain at all (R10).
TEST(VisionTest, LiquidSurfacesAreHitFromAbove)
{
    FakeVision world;           // no terrain, as map 34
    world.Pools = { { -50.0f, 50.0f, -50.0f, 50.0f, 0.0f } };
    Vi::Vec3 const down{ 0.0f, 0.0f, -1.0f };
    Vi::Hit hit = Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 10.3f }, down, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Water);
    EXPECT_NEAR(hit.Distance, 10.3f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);

    world.Pools[0].Deadly = true;
    hit = Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 10.3f }, down, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Deadly);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);
    world.Pools[0].Deadly = false;

    // From under the surface: looking up the surface is ignored; looking down the bed is seen.
    hit = Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, -2.0f }, Vi::Vec3{ 0.0f, 0.0f, 1.0f }, 100.0f, world, {}, true);
    EXPECT_EQ(hit.What, Vi::Kind::Sky);
    world.Ground = [](float, float) { return -10.0f; };
    hit = Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, -2.0f }, down, 100.0f, world, {}, true);
    EXPECT_EQ(hit.What, Vi::Kind::Terrain);
    EXPECT_NEAR(hit.Distance, 8.0f, Vi::BISECT_TO);

    // A model above the water is nearer.
    world.Models = { { -1.0f, 1.0f, -1.0f, 1.0f, 3.0f, 4.0f } };
    hit = Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 10.3f }, down, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_NEAR(hit.Distance, 6.3f, 1e-4f);
    // ... and it is a floor seen from above: its top's slope.
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);
}

// The static and dynamic trees are cast apart and the nearer kept: a door reads 3, a model 2 (R8); a wall's
// normal z is 0.
TEST(VisionTest, DoorsAndModelsAreToldApart)
{
    FakeVision world;
    Vi::Vec3 const origin{ 0.0f, 0.0f, 1.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    world.Doors = { { 5.0f, 5.5f, -2.0f, 2.0f, 0.0f, 4.0f } };
    Vi::Hit hit = Vi::CastRay(origin, ahead, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Door);
    EXPECT_NEAR(hit.Distance, 5.0f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 0.0f);

    world.Models = { { 8.0f, 9.0f, -2.0f, 2.0f, 0.0f, 4.0f } };
    EXPECT_EQ(Vi::CastRay(origin, ahead, 100.0f, world, {}, false).What, Vi::Kind::Door);
    world.Models = { { 3.0f, 4.0f, -2.0f, 2.0f, 0.0f, 4.0f } };
    hit = Vi::CastRay(origin, ahead, 100.0f, world, {}, false);
    EXPECT_EQ(hit.What, Vi::Kind::Model);
    EXPECT_NEAR(hit.Distance, 3.0f, 1e-4f);

    float pixel[Vi::CHANNELS];
    world.Models.clear();
    Vi::EncodePixel(Vi::CastRay(origin, ahead, 100.0f, world, {}, false), 0.0f, 100.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 3.0f);
}

// The objective flag: the closed segment from the camera to the hit (or the range) within a yard of it (R12).
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

    // Through Render: a marker on the floor ahead flags the pixels whose rays pass over it.
    FakeVision world;
    world.Ground = [](float, float) { return 0.0f; };
    Vi::Settings settings;
    Vi::Pose pose;
    Vi::CameraState camera;
    camera.Pitch = -15.0f * DEG;
    Vi::Vec3 const marker{ 8.0f, 0.0f, 0.0f };
    std::vector<float> frame(Vi::ObsCount(settings));
    Vi::Render(settings, pose, camera, world, {}, &marker, frame.data());
    float flagged = 0.0f;
    for (uint32_t pixel = 0; pixel < settings.Width * settings.Height; ++pixel)
        flagged += frame[pixel * Vi::CHANNELS + Vi::CHANNEL_OBJECTIVE];
    EXPECT_GT(flagged, 0.0f);
    Vi::Render(settings, pose, camera, world, {}, nullptr, frame.data());
    flagged = 0.0f;
    for (uint32_t pixel = 0; pixel < settings.Width * settings.Height; ++pixel)
        flagged += frame[pixel * Vi::CHANNELS + Vi::CHANNEL_OBJECTIVE];
    EXPECT_FLOAT_EQ(flagged, 0.0f);
}

// A unit is a vertical cylinder: its side reads normal 0, its top 1; hostile 6, other 7; the seat itself unseen.
TEST(VisionTest, RaysMeetUnitCylindersButNotTheSeat)
{
    FakeVision const world;
    Vi::UnitShape other{ 5.0f, 0.0f, 0.0f, 0.5f, 2.0f, false, false };
    Vi::UnitShape self{ 2.0f, 0.0f, 0.0f, 0.5f, 2.0f, false, true };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    std::vector<Vi::UnitShape> units{ self, other };

    Vi::Hit hit = Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 1.0f }, ahead, 100.0f, world, units, false);
    EXPECT_EQ(hit.What, Vi::Kind::Other);
    EXPECT_NEAR(hit.Distance, 4.5f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 0.0f);
    EXPECT_NEAR(hit.Z, 1.0f, 1e-5f);

    units[1].Hostile = true;
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 1.0f }, ahead, 100.0f, world, units, false).What,
        Vi::Kind::Hostile);

    // From above: the top cap.
    hit = Vi::CastRay(Vi::Vec3{ 5.0f, 0.0f, 10.0f }, Vi::Vec3{ 0.0f, 0.0f, -1.0f }, 100.0f, world, units, false);
    EXPECT_EQ(hit.What, Vi::Kind::Hostile);
    EXPECT_NEAR(hit.Distance, 8.0f, 1e-4f);
    EXPECT_FLOAT_EQ(hit.NormalZ, 1.0f);

    // Over its head: a miss. And the pure test of a cylinder.
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 3.0f }, ahead, 100.0f, world, units, false).What, Vi::Kind::Sky);
    bool top = true;
    EXPECT_NEAR(Vi::RayCylinder(Vi::Vec3{ 0.0f, 0.0f, 1.0f }, ahead, 100.0f, self, top), 1.5f, 1e-4f);
    EXPECT_FALSE(top);
    EXPECT_LT(Vi::RayCylinder(Vi::Vec3{ 0.0f, 0.0f, 1.0f }, ahead, 1.0f, self, top), 0.0f);   // beyond range

    // Only the seat in the way: the ray passes through it.
    std::vector<Vi::UnitShape> const alone{ self };
    EXPECT_EQ(Vi::CastRay(Vi::Vec3{ 0.0f, 0.0f, 1.0f }, ahead, 100.0f, world, alone, false).What, Vi::Kind::Sky);
}

// The five channels and the block's width.
TEST(VisionTest, ChannelsAreEncodedAsTheInterfaceSays)
{
    EXPECT_EQ(Vi::ObsCount(Vi::Settings()), 10247u);

    float pixel[Vi::CHANNELS];
    Vi::Hit hit;
    hit.What = Vi::Kind::Terrain;
    hit.Distance = 0.1f;
    hit.Z = 50.0f;
    hit.NormalZ = 0.8f;
    Vi::EncodePixel(hit, 0.0f, 100.0f, 1.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_DISTANCE], 0.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], 1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_NORMAL], 0.8f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_OBJECTIVE], 1.0f);

    hit.Distance = 5.0f;
    hit.Z = -30.0f;
    hit.What = Vi::Kind::Hostile;
    Vi::EncodePixel(hit, 0.0f, 100.0f, 0.0f, pixel);
    EXPECT_NEAR(pixel[Vi::CHANNEL_DISTANCE], std::log(20.0f) / std::log(400.0f), 1e-6f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], -1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 6.0f);

    hit.Distance = 10.0f;
    hit.Z = 12.5f;
    Vi::EncodePixel(hit, 0.0f, 100.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], 0.5f);

    hit = Vi::Hit();
    hit.Distance = 100.0f;
    hit.Z = 80.0f;
    Vi::EncodePixel(hit, 0.0f, 100.0f, 0.0f, pixel);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_DISTANCE], 1.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_HEIGHT], 0.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_NORMAL], 0.0f);
    EXPECT_FLOAT_EQ(pixel[Vi::CHANNEL_KIND], 0.0f);
}

// A frame: [row][col][channel] with row 0 at the top, then the seven scalars (R16).
TEST(VisionTest, FrameLayoutAndScalars)
{
    FakeVision world;
    world.Ground = [](float, float) { return 0.0f; };
    Vi::Settings settings;
    Vi::Pose pose;
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
    // The top row looks 14 degrees above the horizon: sky. The bottom row looks 44 degrees down: the ground.
    EXPECT_FLOAT_EQ(at(0, 32, Vi::CHANNEL_KIND), float(Vi::Kind::Sky));
    EXPECT_FLOAT_EQ(at(31, 32, Vi::CHANNEL_KIND), float(Vi::Kind::Terrain));
    EXPECT_LT(at(31, 32, Vi::CHANNEL_DISTANCE), at(20, 32, Vi::CHANNEL_DISTANCE));
    EXPECT_NEAR(at(31, 32, Vi::CHANNEL_HEIGHT), 0.0f, 0.01f);

    float const* scalars = frame.data() + Vi::ImageCount(settings);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_YAW_OFFSET], 0.0f);
    EXPECT_NEAR(scalars[Vi::SCALAR_PITCH], -1.0f / 6.0f, 1e-5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_ZOOM], 0.5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_BOOM], 0.5f);
    EXPECT_NEAR(scalars[Vi::SCALAR_PIVOT_HEIGHT], 0.18f, 1e-5f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_UNDERWATER], 0.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_AIRBORNE], 1.0f);

    // No floor within 10 yd: 1. A camera under a liquid's surface: underwater.
    world.Ground = [](float, float) { return -30.0f; };
    world.Pools = { { -50.0f, 50.0f, -50.0f, 50.0f, 20.0f } };
    Vi::Render(settings, pose, camera, world, {}, nullptr, frame.data());
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_PIVOT_HEIGHT], 1.0f);
    EXPECT_FLOAT_EQ(scalars[Vi::SCALAR_UNDERWATER], 1.0f);
}

// The naive cost on a fake Stockades hallway (R22): a corridor of models with a water gutter and no terrain, at the
// default 64 x 32, with no units and with twenty. The fake trees cost next to nothing, so this measures the
// caster's own work (the march and the units), not VMAP's: `forge camera snapshot` measures the live map.
TEST(VisionTest, TimingHarnessOnAFakeHallway)
{
    FakeVision world;
    world.Models = {
        { -10.0f, 120.0f, -5.0f, -4.0f, -2.0f, 8.0f },     // the left wall
        { -10.0f, 120.0f, 4.0f, 5.0f, -2.0f, 8.0f },       // the right wall
        { -10.0f, 120.0f, -5.0f, 3.0f, -2.0f, -1.0f },     // the floor (its top at -1)
        { -10.0f, 120.0f, 3.0f, 4.0f, -3.0f, -2.0f },      // the gutter's bed, under its water
        { -10.0f, 120.0f, -5.0f, 5.0f, 6.0f, 7.0f },       // the ceiling
        { 110.0f, 111.0f, -5.0f, 5.0f, -2.0f, 8.0f },      // the end
    };
    world.Doors = { { 40.0f, 40.5f, -4.0f, -1.0f, -1.0f, 4.0f } };
    world.Pools = { { -10.0f, 120.0f, 3.0f, 4.0f, -1.2f } };
    Vi::Settings const settings;
    Vi::Pose pose;
    pose.Z = -1.0f;
    Vi::CameraState camera;
    camera.Pitch = settings.Pitch * DEG;
    camera.Zoom = settings.Zoom;
    std::vector<float> frame(Vi::ObsCount(settings));

    std::vector<Vi::UnitShape> crowd;
    for (int i = 0; i < 20; ++i)
        crowd.push_back(Vi::UnitShape{ 10.0f + 4.0f * float(i), float(i % 5) - 2.0f, -1.0f, 0.4f, 2.0f, i % 2 == 0,
            false });

    for (std::vector<Vi::UnitShape> const* units : { static_cast<std::vector<Vi::UnitShape> const*>(nullptr),
             static_cast<std::vector<Vi::UnitShape> const*>(&crowd) })
    {
        std::span<Vi::UnitShape const> const seen = units ? std::span<Vi::UnitShape const>(*units)
            : std::span<Vi::UnitShape const>();
        constexpr int FRAMES = 20;
        uint32_t rays = 0;
        auto const start = std::chrono::steady_clock::now();
        for (int i = 0; i < FRAMES; ++i)
            rays = Vi::Render(settings, pose, camera, world, seen, nullptr, frame.data());
        double const us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count() / FRAMES;
        Vi::Breakdown breakdown;
        Vi::Render(settings, pose, camera, world, seen, nullptr, frame.data(), &breakdown);
        std::cout << "[ vision ] fake hallway, " << seen.size() << " units: " << us << " us/frame, " << rays
            << " rays/frame; with clocks: trees " << double(breakdown.TreeNs) / 1e3 << " us (" << breakdown.TreeCasts
            << " casts), march " << double(breakdown.MarchNs) / 1e3 << " us (" << breakdown.MarchSteps
            << " steps), units " << double(breakdown.UnitNs) / 1e3 << " us (" << breakdown.UnitTests << " tests)\n";
        EXPECT_EQ(rays, 64u * 32u + 1u);
        EXPECT_EQ(breakdown.Rays, rays);
        EXPECT_EQ(breakdown.TreeCasts, 2u * rays);
    }
    // The hallway reads as one: floor, walls and the door ahead, and the gutter's water.
    uint32_t counts[Vi::KINDS] = {};
    for (uint32_t pixel = 0; pixel < settings.Width * settings.Height; ++pixel)
        ++counts[uint32_t(frame[pixel * Vi::CHANNELS + Vi::CHANNEL_KIND])];
    EXPECT_GT(counts[uint32_t(Vi::Kind::Model)], 0u);
    EXPECT_GT(counts[uint32_t(Vi::Kind::Door)], 0u);
    EXPECT_GT(counts[uint32_t(Vi::Kind::Water)], 0u);
    EXPECT_GT(counts[uint32_t(Vi::Kind::Hostile)] + counts[uint32_t(Vi::Kind::Other)], 0u);
}
