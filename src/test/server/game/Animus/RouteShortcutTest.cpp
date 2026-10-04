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

#include "RouteShortcut.h"
#include "gtest/gtest.h"

namespace RouteShortcut = Animus::Curriculum::RouteShortcut;

// An open route: every yard sees REACH on, so an advance is one corner 18 yards away -- one leg, not three 6-yard
// steps with a stop between.
TEST(RouteShortcutTest, AnOpenRouteIsWalkedInOneLeg)
{
    auto const clear = [](uint32, uint32) { return true; };
    auto const ahead = RouteShortcut::Corners(100, 1, clear);
    auto const back = RouteShortcut::Corners(100, -1, clear);
    EXPECT_EQ(ahead[0], RouteShortcut::REACH);
    EXPECT_EQ(ahead[99], 99u);
    EXPECT_EQ(back[50], 50 - RouteShortcut::REACH);
    EXPECT_EQ(back[0], 0u);

    auto const run = RouteShortcut::Chain(ahead, back, 10, 90);
    ASSERT_EQ(run.size(), 1u);
    EXPECT_EQ(run[0], 30u);                     // one corner REACH on; the next would pass ADVANCE_YARDS
    auto const home = RouteShortcut::Chain(ahead, back, 10, 15);
    ASSERT_EQ(home.size(), 1u);
    EXPECT_EQ(home[0], 15u);                    // never past the target
    EXPECT_TRUE(RouteShortcut::Chain(ahead, back, 40, 40).empty());
    ASSERT_EQ(RouteShortcut::Chain(ahead, back, 40, 5).size(), 1u);
    EXPECT_EQ(RouteShortcut::Chain(ahead, back, 40, 5)[0], 20u);    // back along it the same way
}

// A corner at yard 30 (nothing is seen round it): every yard before it sees no further than 30, so the run turns
// there, and the chain carries on round it within the advance's reach.
TEST(RouteShortcutTest, ARunTurnsAtACorner)
{
    auto const clear = [](uint32 from, uint32 to) { return from >= 30 || to <= 30; };
    auto const ahead = RouteShortcut::Corners(100, 1, clear);
    auto const back = RouteShortcut::Corners(100, -1, [](uint32, uint32) { return true; });
    EXPECT_EQ(ahead[20], 30u);
    EXPECT_EQ(ahead[30], 50u);
    auto const run = RouteShortcut::Chain(ahead, back, 25, 90);
    ASSERT_EQ(run.size(), 1u);
    EXPECT_EQ(run[0], 30u);                     // round the corner would be 25 yards: the next run's
    auto const near = RouteShortcut::Chain(ahead, back, 28, 90);
    ASSERT_EQ(near.size(), 1u);
}

// A route nothing can be cut on still advances yard by yard, as many corners as it allows.
TEST(RouteShortcutTest, AWindingRouteStillAdvances)
{
    auto const ahead = RouteShortcut::Corners(40, 1, [](uint32, uint32) { return false; });
    auto const back = RouteShortcut::Corners(40, -1, [](uint32, uint32) { return false; });
    EXPECT_EQ(ahead[5], 6u);
    auto const run = RouteShortcut::Chain(ahead, back, 5, 30);
    ASSERT_EQ(run.size(), RouteShortcut::MAX_POINTS);
    EXPECT_EQ(run.back(), 5u + RouteShortcut::MAX_POINTS);
}
