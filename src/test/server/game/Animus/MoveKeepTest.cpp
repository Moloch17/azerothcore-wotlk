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

#include "MoveKeep.h"
#include "gtest/gtest.h"

namespace MoveKeep = Animus::Curriculum::MoveKeep;

// A run on foot (7 yd/s) at 250 ms decisions is kept while two decisions of travel, 3.5 yards, are left: the old
// "half an 8-yard step" let go at 4 yards, so a fresh 21-yard run now lasts about 2.5 s instead of ~0.6 s.
TEST(MoveKeepTest, KeptWhileTwoDecisionsOfTravelAreLeft)
{
    EXPECT_TRUE(MoveKeep::KeepRun(3.6f, 7.0f, 250, 0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(3.4f, 7.0f, 250, 0.0f, 0.0f, 0.0f));
    // Faster: the same yards are less time.
    EXPECT_FALSE(MoveKeep::KeepRun(3.6f, 14.0f, 250, 0.0f, 0.0f, 0.0f));
    // Longer decisions need more travel left.
    EXPECT_FALSE(MoveKeep::KeepRun(3.6f, 7.0f, 500, 0.0f, 0.0f, 0.0f));
}

TEST(MoveKeepTest, LetGoWhenCourseClimbOrFacingNoLongerFit)
{
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, MoveKeep::HEADING_SLACK + 0.01f, 0.0f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, -(MoveKeep::HEADING_SLACK + 0.01f), 0.0f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, 0.0f, MoveKeep::PITCH_SLACK + 0.01f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, 0.0f, 0.0f, MoveKeep::FACING_SLACK + 0.01f));
    EXPECT_TRUE(MoveKeep::KeepRun(20.0f, 7.0f, 250, 0.3f, -0.15f, 0.05f));
}

TEST(MoveKeepTest, NoVelocityOrNoDecisionIsNeverKept)
{
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 0.0f, 250, 0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 0, 0.0f, 0.0f, 0.0f));
}

TEST(MoveKeepTest, ReachIsThreeSecondsOfTravelAndNeverShort)
{
    EXPECT_FLOAT_EQ(MoveKeep::Reach(7.0f), 21.0f);
    EXPECT_FLOAT_EQ(MoveKeep::Reach(1.0f), MoveKeep::MIN_REACH);
    EXPECT_FLOAT_EQ(MoveKeep::Reach(-3.0f), MoveKeep::MIN_REACH);
}

// A launch reaches no further than the option has left plus two decisions: a held bearing lapsing without a refresh
// does not coast on for the rest of a three-second run.
TEST(MoveKeepTest, ReachShrinksAsTheBearingNearsItsEnd)
{
    EXPECT_FLOAT_EQ(MoveKeep::CappedReach(7.0f, 3000, 250), 21.0f);         // the full three seconds
    EXPECT_FLOAT_EQ(MoveKeep::CappedReach(7.0f, 1500, 250), 14.0f);         // (1.5 + 0.5) s at 7 yd/s
    EXPECT_LT(MoveKeep::CappedReach(7.0f, 1000, 250), MoveKeep::CappedReach(7.0f, 2000, 250));
    EXPECT_FLOAT_EQ(MoveKeep::CappedReach(7.0f, 0, 250), MoveKeep::MIN_REACH);   // never below while it runs
    EXPECT_FLOAT_EQ(MoveKeep::CappedReach(1.0f, 0, 250), MoveKeep::MIN_REACH);
}

TEST(MoveKeepTest, ALapsedBearingStopsARunWithMoreThanTwoDecisionsLeft)
{
    EXPECT_TRUE(MoveKeep::CoastsTooFar(10.0f, 7.0f, 250));
    EXPECT_FALSE(MoveKeep::CoastsTooFar(3.0f, 7.0f, 250));
    EXPECT_FALSE(MoveKeep::CoastsTooFar(10.0f, 0.0f, 250));
}
