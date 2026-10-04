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

#include "MovePrice.h"
#include "gtest/gtest.h"

namespace MovePrice = Animus::Curriculum::MovePrice;

namespace
{
    constexpr float DEG = float(M_PI) / 180.0f;
}

// A choice weighs less the longer ago it was, smoothly: no window edge.
TEST(MovePriceTest, RecencyDecaysWithNoEdge)
{
    EXPECT_FLOAT_EQ(MovePrice::Recency(0, 750), 1.0f);
    EXPECT_NEAR(MovePrice::Recency(750, 750), 0.3679f, 1e-4f);
    EXPECT_GT(MovePrice::Recency(250, 750), MovePrice::Recency(500, 750));
    EXPECT_NEAR(MovePrice::Recency(1499, 750) / MovePrice::Recency(1501, 750), 1.0f, 0.01f);
    EXPECT_FLOAT_EQ(MovePrice::Recency(100, 0), 0.0f);
    // The default decay (2500 ms): a weave two or three seconds apart still costs, a correction five seconds on
    // little.
    EXPECT_NEAR(MovePrice::Recency(2000, 2500), 0.449f, 1e-3f);
    EXPECT_NEAR(MovePrice::Recency(3000, 2500), 0.301f, 1e-3f);
    EXPECT_NEAR(MovePrice::Recency(5000, 2500), 0.135f, 1e-3f);
}

// The columns keep the old window; a weave is a reversal past it and within four seconds.
TEST(MovePriceTest, ReversalsAndWeavesAreCountedApart)
{
    EXPECT_EQ(MovePrice::CountAs(1499), 1u);
    EXPECT_EQ(MovePrice::CountAs(1500), 2u);
    EXPECT_EQ(MovePrice::CountAs(3999), 2u);
    EXPECT_EQ(MovePrice::CountAs(4000), 0u);
}

// A reversal costs the angle it takes back, in quarter turns; the same way, or about, nothing.
TEST(MovePriceTest, AReversalCostsTheAngleItUndoes)
{
    EXPECT_NEAR(MovePrice::Undone(45 * DEG, -45 * DEG), 0.5f, 1e-5f);
    EXPECT_NEAR(MovePrice::Undone(45 * DEG, -15 * DEG), 1.0f / 6.0f, 1e-5f);
    EXPECT_NEAR(MovePrice::Undone(-135 * DEG, 90 * DEG), 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(MovePrice::Undone(45 * DEG, 15 * DEG), 0.0f);
    EXPECT_FLOAT_EQ(MovePrice::Undone(45 * DEG, -180 * DEG), 0.0f);
    EXPECT_FLOAT_EQ(MovePrice::Undone(0.0f, -45 * DEG), 0.0f);
}

// A bearing swung round costs at least what a turn of the same angle undoing one costs.
TEST(MovePriceTest, ABearingSwungCostsAtLeastATurnPerDegree)
{
    EXPECT_FLOAT_EQ(MovePrice::BearingSwing(1, 8), 0.5f);
    EXPECT_FLOAT_EQ(MovePrice::BearingSwing(2, 8), 1.0f);
    EXPECT_FLOAT_EQ(MovePrice::BearingSwing(4, 8), 2.0f);
    EXPECT_FLOAT_EQ(MovePrice::BearingSwing(7, 8), 0.5f);
    for (uint32 apart = 1; apart < 4; ++apart)
    {
        float const angle = float(apart) * 45 * DEG;
        EXPECT_GE(MovePrice::BearingSwing(apart, 8) + 1e-6f, MovePrice::Undone(angle, -angle));
    }
}

TEST(MovePriceTest, SteeringEffortIsInProportionToItsAngle)
{
    EXPECT_NEAR(MovePrice::EffortOf(15 * DEG, 45 * DEG), 1.0f / 3.0f, 1e-5f);
    EXPECT_FLOAT_EQ(MovePrice::EffortOf(-90 * DEG, 45 * DEG), 1.0f);
    EXPECT_FLOAT_EQ(MovePrice::EffortOf(0.3f, 0.0f), 1.0f);
}

TEST(MovePriceTest, ChargedOnlyOnceSettled)
{
    EXPECT_FALSE(MovePrice::Settled(250, 500));
    EXPECT_TRUE(MovePrice::Settled(500, 500));
    EXPECT_TRUE(MovePrice::Settled(0, 0));
}
