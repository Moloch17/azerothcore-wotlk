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

#include "MoveSpline.h"
#include "gtest/gtest.h"

// movement-smooth A3: a seat's run is orientation-fixed, and the head is turned on the run itself
// (MoveSpline::ReaimFacing, through Encoding::ReaimRun) rather than relaunching it. The guarantee a spell's facing
// check rests on is that what the spline writes onto the unit every tick (ComputePosition) is the re-aimed angle
// from then on, for the rest of the run and at its end.
namespace
{
    // Ten yards east at 7 yd/s, looking north (1.57) the whole way, as Encoding::MoveTo launches it.
    Movement::MoveSpline LaunchRun(bool fixed)
    {
        Movement::MoveSplineInitArgs args;
        args.path = { G3D::Vector3(0.0f, 0.0f, 0.0f), G3D::Vector3(10.0f, 0.0f, 0.0f) };
        args.velocity = 7.0f;
        args.initialOrientation = 1.57f;
        args.splineId = 1;
        args.walk = false;
        if (fixed)
        {
            args.flags.orientationFixed = true;
            args.flags.EnableFacingAngle();
            args.facing.angle = 1.57f;
        }
        Movement::MoveSpline spline;
        spline.Initialize(args);
        return spline;
    }
}

TEST(ReaimFacingTest, ARunUnderWayLooksWhereItIsTurnedForTheRestOfItAndAtItsEnd)
{
    Movement::MoveSpline spline = LaunchRun(true);
    spline.updateState(500);
    ASSERT_FALSE(spline.Finalized());
    EXPECT_FLOAT_EQ(spline.ComputePosition().orientation, 1.57f);

    uint32 const id = spline.GetId();
    EXPECT_TRUE(spline.ReaimFacing(3.0f));
    EXPECT_EQ(spline.GetId(), id);                                  // the same run: no relaunch, no hitch
    EXPECT_FLOAT_EQ(spline.ComputePosition().orientation, 3.0f);
    spline.updateState(500);
    EXPECT_FLOAT_EQ(spline.ComputePosition().orientation, 3.0f);

    spline.updateState(5000);
    ASSERT_TRUE(spline.Finalized());
    EXPECT_FLOAT_EQ(spline.ComputePosition().orientation, 3.0f);    // the final angle followed
    EXPECT_FALSE(spline.ReaimFacing(1.0f));                         // a finished run has no head to turn
}

TEST(ReaimFacingTest, ARunFacingAlongItsPathIsNotTurned)
{
    Movement::MoveSpline spline = LaunchRun(false);
    spline.updateState(500);
    EXPECT_FALSE(spline.ReaimFacing(3.0f));
    EXPECT_NEAR(spline.ComputePosition().orientation, 0.0f, 1e-4f);  // still east, along the path
}
