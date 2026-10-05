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

#include "Kinematics.h"
#include "gtest/gtest.h"
#include <array>

namespace K = Animus::Kinematics;

// The sample is FORMAT.md section 3's, column for column: the learner reads it through motion.py unchanged.
TEST(KinematicsTest, SampleColumnsMatchTheFormat)
{
    EXPECT_EQ(K::SAMPLE_DIM, 10u);
    K::Body body;
    body.X = 1.0f;
    body.Y = -2.0f;
    body.Z = 3.5f;
    body.Yaw = 4.0f;
    body.Pitch = -0.3f;
    body.Motion = K::Mode::Swimming;
    body.Mounted = true;
    body.Speed = 4.72f;
    body.InCombat = true;

    std::array<float, K::SAMPLE_DIM> out{};
    K::Write(12.25f, body, out.data());
    std::array<float, K::SAMPLE_DIM> const expected{ 12.25f, 1.0f, -2.0f, 3.5f, 4.0f, -0.3f, 1.0f, 1.0f, 4.72f, 1.0f };
    for (uint32 i = 0; i < K::SAMPLE_DIM; ++i)
        EXPECT_FLOAT_EQ(out[i], expected[i]) << "column " << i;

    K::Clear(out.data());
    for (float value : out)
        EXPECT_EQ(value, 0.0f);
}

// A jump is airborne over land or water; water beats a ride; a flying mount is flight only off the ground.
TEST(KinematicsTest, ModeOrder)
{
    EXPECT_EQ(K::ModeOf(false, false, false), K::Mode::Ground);
    EXPECT_EQ(K::ModeOf(true, false, false), K::Mode::Airborne);
    EXPECT_EQ(K::ModeOf(true, true, false), K::Mode::Airborne);
    EXPECT_EQ(K::ModeOf(false, true, false), K::Mode::Swimming);
    EXPECT_EQ(K::ModeOf(false, false, true), K::Mode::Flying);
    EXPECT_EQ(K::ModeOf(true, false, true), K::Mode::Flying);
    EXPECT_EQ(uint8(K::Mode::Airborne), 3);
}

// Time and speed are never negative.
TEST(KinematicsTest, ClampsTimeAndSpeed)
{
    K::Body body;
    body.Speed = -1.0f;
    std::array<float, K::SAMPLE_DIM> out{};
    K::Write(-0.5f, body, out.data());
    EXPECT_EQ(out[K::T], 0.0f);
    EXPECT_EQ(out[K::SPEED], 0.0f);
}
