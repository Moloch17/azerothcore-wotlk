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

#include "WingLadder.h"
#include "gtest/gtest.h"

namespace Cu = Animus::Curriculum;

namespace
{
    constexpr uint32_t WINDOW = 4;

    Cu::WingLadder Ladder(uint32_t start = 0)
    {
        return Cu::WingLadder(9, WINDOW, 0.6f, start);
    }

    /// A window of probes, all making `progress`; the last result.
    Cu::WingLadder::Result Probes(Cu::WingLadder& ladder, float progress, uint32_t count = WINDOW)
    {
        Cu::WingLadder::Result last;
        for (uint32_t i = 0; i < count; ++i)
            last = ladder.Note(ladder.Rung(), true, progress);
        return last;
    }
}

TEST(WingLadderTest, ItStepsDownOnTheProbesAlone)
{
    Cu::WingLadder ladder = Ladder();
    // Runs that are not probes never move it, however well they do.
    for (uint32_t i = 0; i < 20; ++i)
        EXPECT_FALSE(ladder.Note(0, false, 1.0f).Moved);
    EXPECT_EQ(ladder.Rung(), 0u);
    // A run of a rung it has left is ignored.
    EXPECT_FALSE(Probes(ladder, 0.7f, WINDOW - 1).Moved);
    EXPECT_TRUE(Probes(ladder, 0.7f, 1).Moved);
    EXPECT_EQ(ladder.Rung(), 1u);
    EXPECT_FALSE(ladder.Note(0, true, 1.0f).Moved);
}

TEST(WingLadderTest, AHarderRungsLowerScoreNeverStepsItBack)
{
    Cu::WingLadder ladder = Ladder();
    ASSERT_TRUE(Probes(ladder, 0.9f).Moved);
    ASSERT_TRUE(Probes(ladder, 0.9f).Moved);
    EXPECT_EQ(ladder.Rung(), 2u);

    // The harder rung scores far under half of what stepped onto it, for a long time.
    for (uint32_t i = 0; i < 20; ++i)
    {
        Cu::WingLadder::Result const result = Probes(ladder, 0.2f);
        EXPECT_FALSE(result.Moved);
        EXPECT_EQ(ladder.Rung(), 2u);
    }
    // And a zero changes nothing either.
    EXPECT_FALSE(Probes(ladder, 0.0f, 3 * WINDOW).Moved);
    EXPECT_EQ(ladder.Rung(), 2u);
}

TEST(WingLadderTest, TheAlarmFiresOnceAfterThreeLowReadsAndClearsOnRecovery)
{
    Cu::WingLadder ladder = Ladder();
    ASSERT_TRUE(Probes(ladder, 0.8f).Moved);
    ASSERT_EQ(ladder.Rung(), 1u);
    // The floor is max(0.1, 0.25 x 0.8) = 0.2. Two low reads, then a third: the alarm, once.
    EXPECT_FALSE(Probes(ladder, 0.05f).Alarm);
    EXPECT_FALSE(Probes(ladder, 0.05f).Alarm);
    EXPECT_EQ(ladder.CollapsedRung(), -1);
    Cu::WingLadder::Result const third = Probes(ladder, 0.05f);
    ASSERT_TRUE(third.Alarm);
    EXPECT_NE(third.Alarm->find("WARNING"), std::string::npos);
    EXPECT_EQ(ladder.CollapsedRung(), 1);
    EXPECT_FALSE(third.Moved);

    // It stays flagged and does not say it again while it lasts.
    for (uint32_t i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(Probes(ladder, 0.05f).Alarm);
        EXPECT_EQ(ladder.CollapsedRung(), 1);
    }
    // A read over the floor clears it; the ladder is where it was.
    EXPECT_FALSE(Probes(ladder, 0.5f, WINDOW).Moved);
    EXPECT_EQ(ladder.CollapsedRung(), -1);
    EXPECT_EQ(ladder.Rung(), 1u);
}

TEST(WingLadderTest, ReadsAreFreshProbesNotASlidingWindow)
{
    Cu::WingLadder ladder = Ladder();
    ASSERT_TRUE(Probes(ladder, 0.8f).Moved);
    // Fewer than three windows of low probes is fewer than three reads.
    Probes(ladder, 0.05f, 2 * WINDOW + WINDOW - 1);
    EXPECT_EQ(ladder.CollapsedRung(), -1);
    EXPECT_TRUE(Probes(ladder, 0.05f, 1).Alarm);
}

TEST(WingLadderTest, TheFirstRungHasNoAlarm)
{
    Cu::WingLadder ladder = Ladder();
    EXPECT_FALSE(Probes(ladder, 0.0f, 10 * WINDOW).Alarm);
    EXPECT_EQ(ladder.CollapsedRung(), -1);
}

TEST(WingLadderTest, AFollowerTakesTheHostsRungWithinTheLadder)
{
    Cu::WingLadder ladder = Ladder();
    ladder.Follow(5);
    EXPECT_EQ(ladder.Rung(), 5u);
    ladder.Follow(99);
    EXPECT_EQ(ladder.Rung(), 8u);
}
