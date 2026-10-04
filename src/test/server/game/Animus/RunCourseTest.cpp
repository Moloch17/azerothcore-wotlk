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

#include "RunCourse.h"
#include "gtest/gtest.h"

// A turn walked as one run (movement-smooth A2): two legs of one decision each at 7 yd/s, east then north, launched
// at a decision. At the next decision the spline is a millisecond short of the first leg's end; half a decision
// ahead it is on the second, which is the leg that decision walks, so the keep check reads north and keeps the run.
TEST(RunCourseTest, HalfADecisionAheadReadsTheLegTheComingDecisionWalks)
{
    Movement::MoveSplineInitArgs args;
    args.path = { G3D::Vector3(0.0f, 0.0f, 0.0f), G3D::Vector3(1.75f, 0.0f, 0.0f), G3D::Vector3(1.75f, 1.75f, 0.0f),
        G3D::Vector3(1.75f, 10.0f, 0.0f) };
    args.velocity = 7.0f;
    args.splineId = 1;
    args.walk = false;
    args.flags.orientationFixed = true;
    Movement::MoveSpline run;
    run.Initialize(args);

    EXPECT_NEAR(*Animus::Curriculum::RunCourse(run, 125), 0.0f, 1e-4f);          // launched: east
    run.updateState(250);
    ASSERT_FALSE(run.Finalized());
    EXPECT_NEAR(*Animus::Curriculum::RunCourse(run, 0), 0.0f, 1e-4f);            // still on the first leg ...
    EXPECT_NEAR(*Animus::Curriculum::RunCourse(run, 125), float(M_PI) / 2.0f, 1e-4f);  // ... the next is north
    run.updateState(250);
    EXPECT_NEAR(*Animus::Curriculum::RunCourse(run, 125), float(M_PI) / 2.0f, 1e-4f);  // and the straight rest
}
