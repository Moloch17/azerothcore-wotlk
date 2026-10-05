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
    EXPECT_TRUE(MoveKeep::KeepRun(3.6f, 7.0f, 250, 0.0f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(3.4f, 7.0f, 250, 0.0f, 0.0f));
    // Faster: the same yards are less time.
    EXPECT_FALSE(MoveKeep::KeepRun(3.6f, 14.0f, 250, 0.0f, 0.0f));
    // Longer decisions need more travel left.
    EXPECT_FALSE(MoveKeep::KeepRun(3.6f, 7.0f, 500, 0.0f, 0.0f));
}

TEST(MoveKeepTest, LetGoWhenCourseOrClimbNoLongerFit)
{
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, MoveKeep::HEADING_SLACK + 0.01f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, -(MoveKeep::HEADING_SLACK + 0.01f), 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 250, 0.0f, MoveKeep::PITCH_SLACK + 0.01f));
    EXPECT_TRUE(MoveKeep::KeepRun(20.0f, 7.0f, 250, 0.3f, -0.15f));
}

TEST(MoveKeepTest, NoVelocityOrNoDecisionIsNeverKept)
{
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 0.0f, 250, 0.0f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepRun(20.0f, 7.0f, 0, 0.0f, 0.0f));
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

// With a client watching, a run drawn 20 degrees or less off where the seat looks is left alone; past that it is
// relaunched once. Either way round, across the wrap at pi.
TEST(MoveKeepTest, ARunDrawnTooFarOffWhereTheSeatLooksIsRelaunched)
{
    float const twenty = 20.0f * float(M_PI) / 180.0f;
    EXPECT_FALSE(MoveKeep::FacingRelaunch(1.0f, 1.0f + 0.3f, twenty));
    EXPECT_TRUE(MoveKeep::FacingRelaunch(1.0f, 1.0f + 0.4f, twenty));
    EXPECT_TRUE(MoveKeep::FacingRelaunch(1.0f, 1.0f - 0.4f, twenty));
    EXPECT_FALSE(MoveKeep::FacingRelaunch(3.1f, -3.1f, twenty));    // 0.08 apart across the wrap
    EXPECT_TRUE(MoveKeep::FacingRelaunch(0.0f, 0.01f, 0.0f));
}

// A run to where the ground ends is kept with any time left, so long as it still goes the way the seat wants.
TEST(MoveKeepTest, ARunToTheEdgeIsKeptToItsEnd)
{
    EXPECT_FALSE(MoveKeep::KeepRun(1.0f, 7.0f, 250, 0.0f, 0.0f));          // half a decision left: a plain run goes
    EXPECT_TRUE(MoveKeep::KeepEdgeRun(7.0f, 0.0f, 0.0f));                   // an edge run stays
    EXPECT_FALSE(MoveKeep::KeepEdgeRun(7.0f, MoveKeep::HEADING_SLACK + 0.01f, 0.0f));
    EXPECT_FALSE(MoveKeep::KeepEdgeRun(7.0f, 0.0f, MoveKeep::PITCH_SLACK + 0.01f));
    EXPECT_FALSE(MoveKeep::KeepEdgeRun(0.0f, 0.0f, 0.0f));
}

// A sprint or a slow of more than 5% relaunches the run at the new speed; less is left alone.
TEST(MoveKeepTest, ASpeedChangeOfMoreThanFivePercentRelaunches)
{
    EXPECT_FALSE(MoveKeep::SpeedChanged(7.0f, 7.3f));
    EXPECT_TRUE(MoveKeep::SpeedChanged(7.0f, 7.4f));
    EXPECT_TRUE(MoveKeep::SpeedChanged(7.0f, 4.5f));
    EXPECT_FALSE(MoveKeep::SpeedChanged(0.0f, 7.0f));
}

// A seat 2 yards tall swims from 1.5 yards of water and walks again below 0.8; between, it keeps what it was doing,
// whatever the core's flickering in-water flag says.
TEST(MoveKeepTest, SwimmingAndWalkingChangeOverWithHysteresis)
{
    EXPECT_TRUE(MoveKeep::SwimMode(false, true, 1.5f, 2.0f));
    EXPECT_FALSE(MoveKeep::SwimMode(false, true, 1.2f, 2.0f));      // wading: still walking
    EXPECT_TRUE(MoveKeep::SwimMode(true, false, 1.2f, 2.0f));       // the flag flickered off: still swimming
    EXPECT_TRUE(MoveKeep::SwimMode(true, true, 1.0f, 2.0f));
    EXPECT_FALSE(MoveKeep::SwimMode(true, false, 0.7f, 2.0f));      // shallow and out: walking
    EXPECT_FALSE(MoveKeep::SwimMode(false, false, 0.0f, 2.0f));
}

// The speed follows the mode Steer chose, never the core's flag: a swim launched where the flag flickered off is
// still a swim at swim speed, and a walk in wading water is still a run.
TEST(MoveKeepTest, TheRunSpeedFollowsTheSteeredMode)
{
    EXPECT_EQ(MoveKeep::SteerMoveType(false, MoveKeep::SwimMode(true, false, 1.2f, 2.0f), false), MOVE_SWIM);
    EXPECT_EQ(MoveKeep::SteerMoveType(false, MoveKeep::SwimMode(false, true, 1.2f, 2.0f), false), MOVE_RUN);
    EXPECT_EQ(MoveKeep::SteerMoveType(false, true, true), MOVE_SWIM_BACK);
    EXPECT_EQ(MoveKeep::SteerMoveType(false, false, true), MOVE_RUN_BACK);
    EXPECT_EQ(MoveKeep::SteerMoveType(true, true, false), MOVE_FLIGHT);
    EXPECT_EQ(MoveKeep::SteerMoveType(true, false, true), MOVE_FLIGHT_BACK);
}

// Every run the keep lets go has a cause, in the keep's own order: the forced relaunches first, then the course and
// the climb, then the time left -- split by whether CappedReach launched the run short (restart-causes).
TEST(MoveKeepTest, ARunLetGoIsGivenTheCauseThatLetItGo)
{
    using MoveKeep::Relaunch;
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, 10.0f, 7.0f, 250, 0.0f, 0.0f, false), Relaunch::None);
    EXPECT_EQ(MoveKeep::WhyRelaunched(true, true, 10.0f, 7.0f, 250, 1.0f, 1.0f, true), Relaunch::Shown);
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, true, 10.0f, 7.0f, 250, 1.0f, 1.0f, true), Relaunch::Speed);
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, 1.0f, 7.0f, 250, 0.4f, 0.0f, true), Relaunch::Course);
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, 1.0f, 7.0f, 250, 0.0f, 0.3f, true), Relaunch::Climb);
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, 3.4f, 7.0f, 250, 0.0f, 0.0f, false), Relaunch::Time);
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, 3.4f, 7.0f, 250, 0.0f, 0.0f, true), Relaunch::TimeCapped);
    EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, 3.4f, 0.0f, 250, 0.0f, 0.0f, false), Relaunch::Time);

    // Never None where KeepRun refuses, and None wherever it keeps.
    for (float remaining : { 0.5f, 3.4f, 3.6f, 20.0f })
        for (float heading : { 0.0f, 0.3f, -0.4f })
            for (float pitch : { 0.0f, 0.25f })
                EXPECT_EQ(MoveKeep::WhyRelaunched(false, false, remaining, 7.0f, 250, heading, pitch, false)
                    == Relaunch::None, MoveKeep::KeepRun(remaining, 7.0f, 250, heading, pitch));
    EXPECT_STREQ(MoveKeep::RelaunchName(Relaunch::TimeCapped), "time_capped");
}
