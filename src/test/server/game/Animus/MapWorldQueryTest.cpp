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

#include "MapWorldQuery.h"
#include "gtest/gtest.h"
#include <cmath>

namespace Mv = Animus::Movement;

// A sweep's rays: centre, left edge and right edge across the move, at the knee (just over a step) and the chest.
TEST(MapWorldQueryTest, SweepRaysAreTheBodysEdgesAtKneeAndChest)
{
    Mv::Body body;
    body.Radius = 0.5f;
    body.Height = 2.0f;
    Mv::SweepRay rays[6];
    Mv::SweepRays(3.0f, 0.0f, body, rays);                  // moving east: left is north
    EXPECT_FLOAT_EQ(rays[0].Dx, 0.0f);
    EXPECT_FLOAT_EQ(rays[1].Dy, 0.5f);
    EXPECT_FLOAT_EQ(rays[2].Dy, -0.5f);
    EXPECT_NEAR(rays[0].Dz, Mv::STEP_UP + 0.05f, 1e-6f);
    EXPECT_NEAR(rays[3].Dz, 1.6f, 1e-6f);                   // 0.8 of the height
    EXPECT_GT(rays[0].Dz, Mv::STEP_UP);                     // a step is never a wall
    Mv::SweepRays(0.0f, 0.0f, body, rays);                  // no horizontal move: centre rays only
    EXPECT_FLOAT_EQ(rays[1].Dx, 0.0f);
    EXPECT_FLOAT_EQ(rays[1].Dy, 0.0f);
}

// The share of a move made: stops a radius short of the hit (the body's front), whole when the hit is past it.
TEST(MapWorldQueryTest, SweepShareStopsTheBodysFrontAtTheWall)
{
    EXPECT_FLOAT_EQ(Mv::SweepShare(10.0f, 2.0f, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(Mv::SweepShare(2.5f, 2.0f, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(Mv::SweepShare(1.5f, 2.0f, 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(Mv::SweepShare(0.3f, 2.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(Mv::SweepShare(0.0f, 0.0f, 0.5f), 1.0f);
    // A ray that hit nothing, its length rounded a hair short in float: free, not a wall.
    EXPECT_FLOAT_EQ(Mv::SweepShare(2.5f - 0.0001f, 2.0f, 0.5f), 1.0f);
}
