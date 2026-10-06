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

#include "WorldModel.h"
#include "gtest/gtest.h"
#include <G3D/Ray.h>
#include <G3D/Vector3.h>

namespace
{
    constexpr float TILE = 533.333f / 128.0f;      // LIQUID_TILE_SIZE

    /// A 4 x 3-tile liquid from (100, 200), every tile used at level 5 but tile (2, 1), and corner (4, 3) at 7.
    VMAP::WmoLiquid MakeLiquid()
    {
        VMAP::WmoLiquid liquid(4, 3, G3D::Vector3(100.0f, 200.0f, 0.0f), 13);
        float* heights = liquid.GetHeightStorage();
        for (int i = 0; i < 5 * 4; ++i)
            heights[i] = 5.0f;
        heights[4 + 3 * 5] = 7.0f;
        uint8* flags = liquid.GetFlagsStorage();
        for (int i = 0; i < 4 * 3; ++i)
            flags[i] = 0;
        flags[2 + 1 * 4] = 0x0F;        // disabled (GetLiquidHeight's 0x?F)
        return liquid;
    }
}

// The camera's opt-in liquid cast: a ray crossing a WMO liquid's tiles meets the surface GetLiquidHeight describes.
TEST(WmoLiquidTest, RayMeetsTheTiledSurface)
{
    VMAP::WmoLiquid const liquid = MakeLiquid();

    // Straight down onto tile (0, 0): the level.
    float distance = 1000.0f;
    G3D::Ray down = G3D::Ray::fromOriginAndDirection(G3D::Vector3(100.0f + 0.5f * TILE, 200.0f + 0.5f * TILE, 20.0f),
        G3D::Vector3(0.0f, 0.0f, -1.0f));
    ASSERT_TRUE(liquid.IntersectRay(down, distance));
    EXPECT_NEAR(distance, 15.0f, 1e-4f);

    // Its height agrees with GetLiquidHeight wherever it is hit, the raised corner's slope included.
    G3D::Vector3 const from(90.0f, 205.0f, 30.0f);
    G3D::Vector3 const to(100.0f + 3.6f * TILE, 200.0f + 2.7f * TILE, 6.0f);
    G3D::Ray slant = G3D::Ray::fromOriginAndDirection(from, (to - from).direction());
    distance = 1000.0f;
    ASSERT_TRUE(liquid.IntersectRay(slant, distance));
    G3D::Vector3 const hit = from + (to - from).direction() * distance;
    float level = 0.0f;
    ASSERT_TRUE(liquid.GetLiquidHeight(hit, level));
    EXPECT_NEAR(hit.z, level, 1e-3f);

    // A disabled tile is no surface; nor is anything past `distance`, or outside the tiles.
    distance = 1000.0f;
    G3D::Ray hole = G3D::Ray::fromOriginAndDirection(G3D::Vector3(100.0f + 2.5f * TILE, 200.0f + 1.5f * TILE, 20.0f),
        G3D::Vector3(0.0f, 0.0f, -1.0f));
    EXPECT_FALSE(liquid.IntersectRay(hole, distance));
    distance = 10.0f;
    EXPECT_FALSE(liquid.IntersectRay(down, distance));
    distance = 1000.0f;
    G3D::Ray outside = G3D::Ray::fromOriginAndDirection(G3D::Vector3(50.0f, 50.0f, 20.0f),
        G3D::Vector3(0.0f, 0.0f, -1.0f));
    EXPECT_FALSE(liquid.IntersectRay(outside, distance));

    // Coming in under the level through its side: no surface is crossed (the camera's terrain liquid agrees).
    distance = 1000.0f;
    G3D::Vector3 const low(80.0f, 200.0f + 0.5f * TILE, 6.0f);
    G3D::Vector3 const drop = (G3D::Vector3(100.0f + 0.5f * TILE, 200.0f + 0.5f * TILE, 4.0f) - low).direction();
    EXPECT_FALSE(liquid.IntersectRay(G3D::Ray::fromOriginAndDirection(low, drop), distance));
}
