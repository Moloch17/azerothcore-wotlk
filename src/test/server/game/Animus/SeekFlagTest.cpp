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

#include "Camera.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cmath>
#include <vector>

/// **The seek object's flag sits on the object** (M2): the camera flags a ray that passes within the object's own
/// radius and a quarter yard of its centre (Vision::ObjectiveRadiusFor), not a yard, so a small object hidden behind a
/// crate or a wall flags nothing through it, and a visible one is flagged. A room of boxes: a floor (the static
/// tree), a crate and the hidden object (the dynamic tree, as a spawned game object is).
namespace
{
    namespace Vi = Animus::Vision;

    struct Box
    {
        float X0, X1, Y0, Y1, Z0, Z1;
    };

    /// The nearest face of any box along the segment (slab method), its normal z facing the start.
    Vi::SurfaceHit Nearest(std::vector<Box> const& boxes, Vi::Vec3 from, Vi::Vec3 to)
    {
        Vi::SurfaceHit best;
        Vi::Vec3 const d = to - from;
        float const length = Vi::Length(d);
        for (Box const& box : boxes)
        {
            float lo = 0.0f;
            float hi = 1.0f;
            int entered = -1;
            float const o[3] = { from.X, from.Y, from.Z };
            float const v[3] = { d.X, d.Y, d.Z };
            float const mins[3] = { box.X0, box.Y0, box.Z0 };
            float const maxs[3] = { box.X1, box.Y1, box.Z1 };
            bool miss = false;
            for (int axis = 0; axis < 3 && !miss; ++axis)
            {
                if (std::fabs(v[axis]) < 1e-9f)
                {
                    miss = o[axis] < mins[axis] || o[axis] > maxs[axis];
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
                miss = lo > hi;
            }
            if (miss || (best.Distance >= 0.0f && lo * length >= best.Distance))
                continue;
            best.Distance = lo * length;
            best.NormalZ = entered == 2 ? (to.Z < from.Z ? 1.0f : -1.0f) : 0.0f;
        }
        return best;
    }

    /// Loaded grids with no terrain heights (an instance), the boxes as the trees.
    class Room : public Vi::VisionWorld
    {
    public:
        std::vector<Box> Static;
        std::vector<Box> Dynamic;

        Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Static, from, to); }
        Vi::SurfaceHit DynamicHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Dynamic, from, to); }
        Vi::LiquidHit ModelLiquid(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            Vi::TerrainTile tile;
            tile.Loaded = tileX >= 31 && tileX <= 33 && tileY >= 31 && tileY <= 33;
            return tile;
        }
        Vi::TerrainCell Cell(int32_t, int32_t, int32_t, int32_t, bool) const override { return {}; }
        Animus::Movement::Liquid LiquidAt(float, float, float) const override { return {}; }
        float FloorBelow(float, float, float z, float search) const override
        {
            return z >= 0.0f && z - search <= 0.0f ? 0.0f : Animus::Movement::INVALID_FLOOR;
        }
    };

    constexpr float X = -200.0f;    // inside the loaded grids, as VisionTest's scenes are
    constexpr float Y = -200.0f;

    /// The strongbox (Hidden Strongbox's Chest01.m2: 0.8 x 1.2 x 0.6 yd) standing at x + 9.74, and its centre.
    Box const STRONGBOX = { X + 9.34f, X + 10.14f, Y - 0.58f, Y + 0.58f, 0.0f, 0.61f };
    Vi::Vec3 const CENTRE{ X + 9.74f, Y, 0.305f };
    /// A crate (Stormwindcrate01.m2: 1.2 x 1.3 x 1.24 yd) just in front of it, between it and the seat.
    Box const CRATE = { X + 8.0f, X + 9.24f, Y - 0.66f, Y + 0.66f, 0.0f, 1.24f };
    /// A wall, the bars of a cell with no gap between them: 3 yd high, in front of it.
    Box const WALL = { X + 8.9f, X + 9.1f, Y - 4.0f, Y + 4.0f, 0.0f, 3.0f };

    /// The flagged pixels of the seat's frame at (X, Y) facing +x, the camera at its default, `radius` the flag's.
    uint32_t Flagged(Room const& room, float radius)
    {
        Vi::Settings const settings;
        Vi::Pose pose;
        pose.X = X;
        pose.Y = Y;
        pose.Z = 0.0f;
        Vi::CameraState camera;
        camera.Pitch = -10.0f * Vi::DEGREES;
        camera.Zoom = settings.Zoom;
        std::vector<uint8_t> image(Vi::ImageBytes(settings));
        std::vector<float> scalars(Vi::SCALARS);
        Vi::Render(settings, pose, camera, room, {}, &CENTRE, image.data(), scalars.data(), nullptr, radius);
        return Vi::CountObjectivePixels(image.data(), uint32_t(image.size()));
    }

    Room Floor()
    {
        Room room;
        room.Static.push_back({ X - 50.0f, X + 50.0f, Y - 50.0f, Y + 50.0f, -1.0f, 0.0f });
        room.Dynamic.push_back(STRONGBOX);
        return room;
    }
}

// The radius: an object's own and a quarter yard, never more than the default yard.
TEST(SeekFlagTest, TheRadiusIsTheObjectsOwnAndAQuarterYard)
{
    EXPECT_FLOAT_EQ(Vi::ObjectiveRadiusFor(0.58f), 0.83f);
    EXPECT_FLOAT_EQ(Vi::ObjectiveRadiusFor(0.5f), 0.75f);
    EXPECT_FLOAT_EQ(Vi::ObjectiveRadiusFor(0.78f), Vi::OBJECTIVE_RADIUS);
    EXPECT_FLOAT_EQ(Vi::ObjectiveRadiusFor(3.0f), Vi::OBJECTIVE_RADIUS);
    // ObjectiveFlag at the radius given: a ray 0.9 yd off the centre is outside the strongbox's, inside the yard.
    Vi::Vec3 const origin{ 0.0f, 0.0f, 0.0f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    Vi::Vec3 const beside{ 10.0f, 0.9f, 0.0f };
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 100.0f, &beside), 1.0f);
    EXPECT_FLOAT_EQ(Vi::ObjectiveFlag(origin, ahead, 100.0f, &beside, Vi::ObjectiveRadiusFor(0.58f)), 0.0f);
}

// In plain sight, the strongbox is flagged.
TEST(SeekFlagTest, AVisibleObjectIsFlagged)
{
    Room const room = Floor();
    EXPECT_GT(Flagged(room, Vi::ObjectiveRadiusFor(0.58f)), 0u);
}

// Behind a crate it is not: the rays that pass over the crate miss its own radius. The default yard round its centre
// reached over the crate's top and flagged pixels of a thing nobody could see.
TEST(SeekFlagTest, AnObjectBehindACrateIsNotFlagged)
{
    Room room = Floor();
    room.Dynamic.push_back(CRATE);
    EXPECT_EQ(Flagged(room, Vi::ObjectiveRadiusFor(0.58f)), 0u);
    EXPECT_GT(Flagged(room, Vi::OBJECTIVE_RADIUS), 0u) << "the default yard no longer leaks: this scene shows nothing";
}

// Behind a wall (a cell's bars with no gap) it is not either.
TEST(SeekFlagTest, AnObjectBehindAWallIsNotFlagged)
{
    Room room = Floor();
    room.Static.push_back(WALL);
    EXPECT_EQ(Flagged(room, Vi::ObjectiveRadiusFor(0.58f)), 0u);
}
