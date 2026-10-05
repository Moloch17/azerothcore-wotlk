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

#include "BuildRetry.h"
#include "gtest/gtest.h"
#include <vector>

using Animus::Curriculum::RetryBuild;
using Animus::Curriculum::SETUP_BUILD_ATTEMPTS;

// One unlucky draw at setup is drawn again, as in training: a build that fails twice and then succeeds sets up.
TEST(BuildRetryTest, AFailedDrawIsDrawnAgain)
{
    uint32_t calls = 0;
    std::vector<uint32_t> failures;
    bool const built = RetryBuild(SETUP_BUILD_ATTEMPTS, [&] { return ++calls >= 3; },
        [&](uint32_t attempt) { failures.push_back(attempt); });
    EXPECT_TRUE(built);
    EXPECT_EQ(calls, 3u);
    EXPECT_EQ(failures, (std::vector<uint32_t>{ 0, 1 }));
}

// Only a stage that keeps failing is given up on, after every try.
TEST(BuildRetryTest, AStageThatKeepsFailingIsGivenUpOn)
{
    uint32_t calls = 0;
    uint32_t failures = 0;
    EXPECT_FALSE(RetryBuild(SETUP_BUILD_ATTEMPTS, [&] { ++calls; return false; }, [&](uint32_t) { ++failures; }));
    EXPECT_EQ(calls, SETUP_BUILD_ATTEMPTS);
    EXPECT_EQ(failures, SETUP_BUILD_ATTEMPTS);
    EXPECT_GE(SETUP_BUILD_ATTEMPTS, 4u);
}
