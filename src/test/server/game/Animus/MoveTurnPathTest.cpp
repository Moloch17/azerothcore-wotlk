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

#include "MoveTurnPath.h"
#include "gtest/gtest.h"

namespace MoveTurnPath = Animus::Curriculum::MoveTurnPath;

namespace
{
    constexpr float RATE = 0.7853982f;          // MoveBlock::TURN_RATE, 45 degrees a decision
    constexpr float SPACING = 1.75f;            // 7 yd/s over a 250 ms decision
}

// An about-turn chosen this decision (one 45 degree step already taken, 135 to come): this decision's leg at the
// heading it has now, two more legs a decision apart each 45 degrees further round, and from the decision the last
// step lands on, straight on at the turn's end.
TEST(MoveTurnPathTest, OneLegADecisionAtTheHeadingEachStepGivesThenStraightOn)
{
    auto const legs = MoveTurnPath::Legs(0.5f, 3.0f * RATE, RATE, SPACING, 21.0f);
    ASSERT_EQ(legs.size(), 4u);
    for (unsigned i = 0; i < 3; ++i)
    {
        EXPECT_NEAR(legs[i].Heading, 0.5f + float(i) * RATE, 1e-5f);
        EXPECT_FLOAT_EQ(legs[i].Length, SPACING);
    }
    EXPECT_NEAR(legs[3].Heading, 0.5f + 3.0f * RATE, 1e-5f);
    EXPECT_NEAR(legs[3].Length, 21.0f - 3.0f * SPACING, 1e-4f);
}

// A partial last step (15 degrees left over) and a right turn, which wraps below zero.
TEST(MoveTurnPathTest, APartStepAndARightTurnAcrossZero)
{
    float const fifteen = RATE / 3.0f;
    auto const legs = MoveTurnPath::Legs(0.2f, -(RATE + fifteen), RATE, SPACING, 21.0f);
    ASSERT_EQ(legs.size(), 3u);
    EXPECT_NEAR(legs[0].Heading, 0.2f, 1e-5f);
    EXPECT_NEAR(legs[1].Heading, 0.2f - RATE + 2.0f * float(M_PI), 1e-5f);
    // Straight on where the turn ended, the rest of the reach.
    EXPECT_NEAR(legs[2].Heading, 0.2f - RATE - fifteen + 2.0f * float(M_PI), 1e-5f);
    EXPECT_NEAR(legs[2].Length, 21.0f - 2.0f * SPACING, 1e-4f);
}

// The run is never longer than its reach: a short reach cuts the turn's legs off where it runs out.
TEST(MoveTurnPathTest, CutShortAtTheReach)
{
    auto const legs = MoveTurnPath::Legs(0.0f, 3.0f * RATE, RATE, SPACING, 4.0f);
    ASSERT_EQ(legs.size(), 3u);
    float total = 0.0f;
    for (auto const& leg : legs)
        total += leg.Length;
    EXPECT_FLOAT_EQ(total, 4.0f);
    EXPECT_NEAR(legs[2].Length, 4.0f - 2.0f * SPACING, 1e-5f);
}

TEST(MoveTurnPathTest, NoTurnIsOneStraightLegAndNoReachIsNone)
{
    auto const legs = MoveTurnPath::Legs(1.0f, 0.0f, RATE, SPACING, 21.0f);
    ASSERT_EQ(legs.size(), 1u);
    EXPECT_FLOAT_EQ(legs[0].Length, 21.0f);
    EXPECT_TRUE(MoveTurnPath::Legs(1.0f, RATE, RATE, SPACING, 0.0f).empty());
    EXPECT_EQ(MoveTurnPath::Legs(1.0f, RATE, RATE, 0.0f, 21.0f).size(), 1u);
}

// Off the ground (movement-smooth A2 in the water and the air): a turn and a climb stepped side by side, one leg a
// decision while either lasts, then straight on at the heading and pitch they end on.
TEST(MoveTurnPathTest, AirLegsStepTheTurnAndTheClimbTogether)
{
    constexpr float PITCH_RATE = 0.5235988f;    // MoveBlock::PITCH_RATE, 30 degrees a decision
    // 90 degrees of turn (two steps) and 60 of climb (two steps) from level.
    auto const legs = MoveTurnPath::AirLegs(0.0f, 2.0f * RATE, RATE, 0.0f, 2.0f * PITCH_RATE, PITCH_RATE, SPACING,
        21.0f);
    ASSERT_EQ(legs.size(), 3u);
    EXPECT_NEAR(legs[0].Heading, 0.0f, 1e-5f);
    EXPECT_NEAR(legs[0].Pitch, 0.0f, 1e-5f);
    EXPECT_NEAR(legs[1].Heading, RATE, 1e-5f);
    EXPECT_NEAR(legs[1].Pitch, PITCH_RATE, 1e-5f);
    EXPECT_NEAR(legs[2].Heading, 2.0f * RATE, 1e-5f);
    EXPECT_NEAR(legs[2].Pitch, 2.0f * PITCH_RATE, 1e-5f);
    EXPECT_NEAR(legs[2].Length, 21.0f - 2.0f * SPACING, 1e-4f);
}

// The longer of the two sets the legs: a dive of three steps under a turn of one, then straight on.
TEST(MoveTurnPathTest, AirLegsLastAsLongAsTheLongerOfTurnAndClimb)
{
    constexpr float PITCH_RATE = 0.5235988f;
    auto const legs = MoveTurnPath::AirLegs(1.0f, -RATE, RATE, 0.5f, -3.0f * PITCH_RATE, PITCH_RATE, SPACING, 21.0f);
    ASSERT_EQ(legs.size(), 4u);
    EXPECT_NEAR(legs[1].Heading, 1.0f - RATE, 1e-5f);
    EXPECT_NEAR(legs[3].Heading, 1.0f - RATE, 1e-5f);           // the turn is done after one step
    EXPECT_NEAR(legs[3].Pitch, 0.5f - 3.0f * PITCH_RATE, 1e-5f);
    // A pitch alone is a curve too; neither left is one straight leg at the pitch the seat has.
    EXPECT_EQ(MoveTurnPath::AirLegs(0.0f, 0.0f, RATE, 0.0f, PITCH_RATE, PITCH_RATE, SPACING, 21.0f).size(), 2u);
    auto const straight = MoveTurnPath::AirLegs(0.0f, 0.0f, RATE, 0.3f, 0.0f, PITCH_RATE, SPACING, 21.0f);
    ASSERT_EQ(straight.size(), 1u);
    EXPECT_NEAR(straight[0].Pitch, 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(straight[0].Length, 21.0f);
}
