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

#include "Standing.h"
#include "StageScenario.h"
#include "StageState.h"
#include "UnitDefines.h"
#include "gtest/gtest.h"

namespace Standing = Animus::Curriculum::Standing;

// A slide along a wall that keeps most of the ground the keys ask for is the right way round a corner's inside: no
// charge. Pressing into it and getting nowhere is the full price; between them, in proportion to the shortfall.
TEST(StandingTest, WallChargesOnlyTheGroundNotCovered)
{
    float const price = 0.03f;
    EXPECT_FLOAT_EQ(Standing::WallCharge(0.25f, 1.75f, 1.75f, price, 0.5f), 0.0f);    // full speed along it
    EXPECT_FLOAT_EQ(Standing::WallCharge(0.25f, 0.9f, 1.75f, price, 0.5f), 0.0f);     // a slide at 51%
    EXPECT_FLOAT_EQ(Standing::WallCharge(0.25f, 0.0f, 1.75f, price, 0.5f), price * 0.25f);
    EXPECT_NEAR(Standing::WallCharge(0.25f, 0.4375f, 1.75f, price, 0.5f), price * 0.25f * 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(Standing::WallCharge(0.0f, 0.0f, 1.75f, price, 0.5f), 0.0f);      // no wall, no charge
}

// Stopped is the server's state: no moving flag (a jump is FALLING), not swimming or flying, and still. A turn is not
// moving (ratified 2026-10-05).
TEST(StandingTest, StoppedReadsTheServersFlags)
{
    EXPECT_TRUE(Standing::Stopped(0, 0.0f, 0.05f));
    EXPECT_TRUE(Standing::Stopped(MOVEMENTFLAG_LEFT, 0.0f, 0.05f));
    EXPECT_FALSE(Standing::Stopped(MOVEMENTFLAG_FORWARD, 0.0f, 0.05f));
    EXPECT_FALSE(Standing::Stopped(MOVEMENTFLAG_STRAFE_LEFT, 0.0f, 0.05f));
    EXPECT_FALSE(Standing::Stopped(MOVEMENTFLAG_FALLING, 0.0f, 0.05f));
    EXPECT_FALSE(Standing::Stopped(MOVEMENTFLAG_SWIMMING, 0.0f, 0.05f));
    EXPECT_FALSE(Standing::Stopped(0, 0.2f, 0.05f));
}

// Where a distance to the leader falls (too close, in the band, behind, lost).
TEST(StandingTest, FollowBands)
{
    EXPECT_EQ(Standing::Band(2.0f, 3.0f, 10.0f, 30.0f), 0u);
    EXPECT_EQ(Standing::Band(3.0f, 3.0f, 10.0f, 30.0f), 1u);
    EXPECT_EQ(Standing::Band(10.0f, 3.0f, 10.0f, 30.0f), 1u);
    EXPECT_EQ(Standing::Band(20.0f, 3.0f, 10.0f, 30.0f), 2u);
    EXPECT_EQ(Standing::Band(31.0f, 3.0f, 10.0f, 30.0f), 3u);
}

// course_kinks reads the body's way between ticks (the ground step zeroes its velocity): a straight run never kinks,
// a 90 degree turn within a tick does, and standing still forgets the course.
TEST(StandingTest, CourseKinksReadTheWayBetweenTicks)
{
    using Animus::Curriculum::SeatState;
    using Animus::Curriculum::StageScenario;
    SeatState seat;
    float x = 0.0f;
    EXPECT_FALSE(StageScenario::CourseKink(seat, x, 0.0f, 50));
    for (int tick = 0; tick < 5; ++tick)
    {
        x += 0.35f;                                     // 7 yd/s for 50 ms
        EXPECT_FALSE(StageScenario::CourseKink(seat, x, 0.0f, 50));
    }
    EXPECT_TRUE(StageScenario::CourseKink(seat, x, 0.35f, 50));     // a right angle within a tick
    EXPECT_FALSE(StageScenario::CourseKink(seat, x, 0.70f, 50));    // and on along the new course
    EXPECT_FALSE(StageScenario::CourseKink(seat, x, 0.70f, 50));    // stopped: no course
    EXPECT_FALSE(StageScenario::CourseKink(seat, x + 0.35f, 0.70f, 50)); // moving again: a new course, not a kink
}
