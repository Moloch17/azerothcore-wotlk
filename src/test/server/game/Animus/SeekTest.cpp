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

#include "Seek.h"
#include "gtest/gtest.h"
#include <cmath>

namespace Mv = Animus::Movement;

namespace
{
    /// Open flat ground at z = 0.
    class Open final : public Mv::WorldQuery
    {
    public:
        [[nodiscard]] float FloorBelow(float, float, float z, float search) const override
        {
            return z >= -1e-3f && z <= search ? 0.0f : Mv::INVALID_FLOOR;
        }
        [[nodiscard]] float FloorNormalZ(float, float, float) const override { return 1.0f; }
        [[nodiscard]] Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float, float, float, float up) const override { return up; }
        [[nodiscard]] bool InTerrain(float, float, float z) const override { return z < -0.5f; }
    };

    /// Drive a body with the seek helper for up to `seconds` in 50 ms ticks; how long it took to arrive (< 0: never).
    float Arrive(Mv::BodyState& body, float x, float y, float seconds, float& maxTurn)
    {
        Open world;
        Mv::Speeds speeds;
        Mv::Body shape;
        maxTurn = 0.0f;
        for (float t = 0.0f; t < seconds; t += 0.05f)
        {
            Mv::ControlState control = Mv::Seek(body, x, y);
            if (control.Forward == 0 && control.TurnRate == 0.0f)
                return t;
            maxTurn = std::max(maxTurn, std::fabs(control.TurnRate));
            Mv::Step(body, control, speeds, shape, world, 0.05f);
        }
        return -1.0f;
    }
}

// A target behind and to the side: the actor turns on the spot first (no forward while it is more than 45 degrees
// off), then runs, and arrives about as fast as a straight run would plus the turn; within the mouse's fastest rate.
TEST(SeekTest, AScriptedActorTurnsRunsAndArrives)
{
    Mv::BodyState body;
    body.Yaw = 0.0f;
    float maxTurn = 0.0f;
    float const took = Arrive(body, -10.0f, 10.0f, 10.0f, maxTurn);
    ASSERT_GE(took, 0.0f);
    float const straight = (std::sqrt(200.0f) - 2.0f) / 7.0f;
    EXPECT_LT(took, straight + 0.75f);
    EXPECT_LE(maxTurn, 6.2831853f + 1e-4f);
    EXPECT_LE(std::hypot(body.X + 10.0f, body.Y - 10.0f), 2.0f + 0.35f);
}

// Already there: nothing is held. Straight ahead: forward with no turn.
TEST(SeekTest, NothingHeldOnArrivalAndNoTurnStraightAhead)
{
    Mv::BodyState body;
    Mv::ControlState const there = Mv::Seek(body, 1.0f, 1.0f);
    EXPECT_EQ(there.Forward, 0);
    EXPECT_EQ(there.TurnRate, 0.0f);
    Mv::ControlState const ahead = Mv::Seek(body, 20.0f, 0.0f);
    EXPECT_EQ(ahead.Forward, 1);
    EXPECT_NEAR(ahead.TurnRate, 0.0f, 1e-5f);
    Mv::ControlState const behind = Mv::Seek(body, -20.0f, 0.1f);
    EXPECT_EQ(behind.Forward, 0);
    EXPECT_GT(std::fabs(behind.TurnRate), 1.0f);
}
