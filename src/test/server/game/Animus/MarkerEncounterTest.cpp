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


#include "MarkerEncounter.h"
#include "UnitDefines.h"
#include "gtest/gtest.h"
#include <cmath>

using Animus::Curriculum::MarkerEncounter;
using Animus::Curriculum::MarkerRung;

// A slide along a wall that keeps most of the ground the keys ask for is the right way round a corner's inside: no
// charge. Pressing into it and getting nowhere is the full price; between them, in proportion to the shortfall.
TEST(MarkerEncounterTest, WallChargesOnlyTheGroundNotCovered)
{
    float const price = 0.03f;
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.25f, 1.75f, 1.75f, price, 0.5f), 0.0f);    // full speed along it
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.25f, 0.9f, 1.75f, price, 0.5f), 0.0f);     // a slide at 51%
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.25f, 0.0f, 1.75f, price, 0.5f), price * 0.25f);
    EXPECT_NEAR(MarkerEncounter::WallCharge(0.25f, 0.4375f, 1.75f, price, 0.5f), price * 0.25f * 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.0f, 0.0f, 1.75f, price, 0.5f), 0.0f);      // no wall, no charge
}

// Stopped is the server's state: no moving flag (a jump is FALLING), not swimming or flying, and still. A turn is not
// moving (ratified 2026-10-05).
TEST(MarkerEncounterTest, StoppedReadsTheServersFlags)
{
    EXPECT_TRUE(MarkerEncounter::Stopped(0, 0.0f, 0.05f));
    EXPECT_TRUE(MarkerEncounter::Stopped(MOVEMENTFLAG_LEFT, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_FORWARD, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_STRAFE_LEFT, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_FALLING, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_SWIMMING, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(0, 0.2f, 0.05f));
}

// M1's ladder: the furthest distance and the bearing widen and the radius tightens from the first rung to the last,
// which is the stage's real task (60 yd, anywhere round, half a yard).
TEST(MarkerEncounterTest, ControlsLadderEndsAtTheRealTask)
{
    MarkerRung const first = MarkerEncounter::RungTask(0, 8, 5.0f, 10.0f, 60.0f, 20.0f, 180.0f, 4.0f, 0.5f);
    MarkerRung const last = MarkerEncounter::RungTask(7, 8, 5.0f, 10.0f, 60.0f, 20.0f, 180.0f, 4.0f, 0.5f);
    EXPECT_FLOAT_EQ(first.Furthest, 10.0f);
    EXPECT_FLOAT_EQ(first.Radius, 4.0f);
    EXPECT_NEAR(first.BearingHalf, 20.0f * float(M_PI) / 180.0f, 1e-5f);
    EXPECT_FLOAT_EQ(last.Furthest, 60.0f);
    EXPECT_FLOAT_EQ(last.Radius, 0.5f);
    EXPECT_NEAR(last.BearingHalf, float(M_PI), 1e-5f);
    EXPECT_FLOAT_EQ(last.Nearest, 5.0f);
}

// M2's ladder: distance and detour window climb, the radius stays.
TEST(MarkerEncounterTest, GroundLadderWidensTheDetour)
{
    MarkerRung const first = MarkerEncounter::GroundRungTask(0, 6, 20.0f, 40.0f, 120.0f, 1.0f, 1.6f, 0.25f, 1.0f);
    MarkerRung const last = MarkerEncounter::GroundRungTask(5, 6, 20.0f, 40.0f, 120.0f, 1.0f, 1.6f, 0.25f, 1.0f);
    EXPECT_FLOAT_EQ(first.DetourMin, 1.0f);
    EXPECT_FLOAT_EQ(first.DetourMax, 1.25f);
    EXPECT_FLOAT_EQ(last.DetourMin, 1.6f);
    EXPECT_FLOAT_EQ(last.Furthest, 120.0f);
    EXPECT_FLOAT_EQ(first.Radius, last.Radius);
}
