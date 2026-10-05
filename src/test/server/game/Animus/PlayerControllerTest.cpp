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

#include "PlayerController.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace Mv = Animus::Movement;

namespace
{
    constexpr float PI = 3.14159265f;

    /// A fake world: a height field, solid boxes (each a floor on top and a wall on its sides), water rectangles.
    struct Box
    {
        float X0, X1, Y0, Y1, Z0, Z1;
        [[nodiscard]] bool Contains(float x, float y, float z, float grow = 0.0f) const
        {
            return x >= X0 - grow && x <= X1 + grow && y >= Y0 - grow && y <= Y1 + grow && z >= Z0 && z <= Z1;
        }
    };

    struct Water
    {
        float X0, X1, Y0, Y1, Level;
    };

    class FakeWorld : public Mv::WorldQuery
    {
    public:
        std::function<float(float, float)> Ground = [](float, float) { return 0.0f; };
        std::vector<Box> Boxes;
        std::vector<Water> Waters;

        float FloorBelow(float x, float y, float z, float search) const override
        {
            float best = Mv::INVALID_FLOOR;
            float const g = Ground(x, y);
            if (g <= z + 1e-4f && g >= z - search)
                best = g;
            for (Box const& b : Boxes)
                if (x >= b.X0 && x <= b.X1 && y >= b.Y0 && y <= b.Y1 && b.Z1 <= z + 1e-4f && b.Z1 >= z - search)
                    best = std::max(best, b.Z1);
            return best;
        }

        float FloorNormalZ(float x, float y, float z) const override
        {
            for (Box const& b : Boxes)
                if (x >= b.X0 && x <= b.X1 && y >= b.Y0 && y <= b.Y1 && std::fabs(b.Z1 - z) < 0.05f)
                    return 1.0f;
            float const e = 0.05f;
            float const gx = (Ground(x + e, y) - Ground(x - e, y)) / (2.0f * e);
            float const gy = (Ground(x, y + e) - Ground(x, y - e)) / (2.0f * e);
            return 1.0f / std::sqrt(1.0f + gx * gx + gy * gy);
        }

        Mv::Liquid LiquidAt(float x, float y, float /*z*/) const override
        {
            for (Water const& w : Waters)
                if (x >= w.X0 && x <= w.X1 && y >= w.Y0 && y <= w.Y1)
                    return { true, w.Level, false };
            return {};
        }

        float Sweep(float x0, float y0, float z0, float x1, float y1, float z1, Mv::Body const& body) const override
        {
            constexpr int N = 64;
            float freeShare = 0.0f;
            for (int i = 1; i <= N; ++i)
            {
                float const t = float(i) / float(N);
                float const x = x0 + (x1 - x0) * t;
                float const y = y0 + (y1 - y0) * t;
                float const z = z0 + (z1 - z0) * t;
                float const knee = z + Mv::STEP_UP + 0.05f;
                float const chest = z + body.Height * 0.8f;
                for (Box const& b : Boxes)
                    if (b.Contains(x, y, knee, body.Radius) || b.Contains(x, y, chest, body.Radius))
                        return freeShare;
                freeShare = t;
            }
            return 1.0f;
        }

        float Ceiling(float x, float y, float z, float up) const override
        {
            float room = up;
            for (Box const& b : Boxes)
                if (x >= b.X0 && x <= b.X1 && y >= b.Y0 && y <= b.Y1 && b.Z0 >= z)
                    room = std::min(room, b.Z0 - z);
            return room;
        }

        bool InTerrain(float x, float y, float z) const override
        {
            return z < Ground(x, y) - 0.05f;
        }
    };

    Mv::BodyState At(float x, float y, float z, float yaw = 0.0f)
    {
        Mv::BodyState body;
        body.X = x;
        body.Y = y;
        body.Z = z;
        body.Yaw = yaw;
        return body;
    }

    /// Run `seconds` of 50 ms ticks; returns the highest Z reached.
    float Drive(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::WorldQuery const& world,
        float seconds, float tick = 0.05f)
    {
        Mv::Body const shape;
        float apex = body.Z;
        int const ticks = int(std::lround(seconds / tick));
        for (int i = 0; i < ticks; ++i)
        {
            Mv::Step(body, control, speeds, shape, world, tick);
            apex = std::max(apex, body.Z);
        }
        return apex;
    }
}

// The run speeds by the client's rules (fn 0x987570): forward run, back (with or without a strafe) at min(run, run
// back), strafe at run, a diagonal no faster (each component times 0.7071), and walking caps every direction --
// backward included -- at min(run, walk).
TEST(PlayerControllerTest, GroundSpeedsFollowTheClientsRules)
{
    FakeWorld world;
    Mv::Speeds const speeds;
    struct Case { int8_t forward, strafe; bool walk; float yards; float x, y; };
    for (Case c : { Case{ 1, 0, false, 7.0f, 7.0f, 0.0f }, Case{ -1, 0, false, 4.5f, -4.5f, 0.0f },
            Case{ 0, 1, false, 7.0f, 0.0f, -7.0f }, Case{ 0, -1, false, 7.0f, 0.0f, 7.0f },
            Case{ 1, 1, false, 7.0f, 4.9497f, -4.9497f }, Case{ 1, 0, true, 2.5f, 2.5f, 0.0f },
            Case{ -1, 1, false, 4.5f, -3.1820f, -3.1820f }, Case{ -1, 0, true, 2.5f, -2.5f, 0.0f } })
    {
        Mv::BodyState body = At(0, 0, 0);
        Mv::ControlState control;
        control.Forward = c.forward;
        control.Strafe = c.strafe;
        control.Walk = c.walk;
        Drive(body, control, speeds, world, 1.0f);
        EXPECT_NEAR(std::hypot(body.X, body.Y), c.yards, 1e-3f) << int(c.forward) << " " << int(c.strafe);
        EXPECT_NEAR(body.X, c.x, 1e-3f);
        EXPECT_NEAR(body.Y, c.y, 1e-3f);
        EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    }
}

// Turning: the held rate, wrapped; keyboard turning is slowed to 0.75 while moving (the client's 0x9e9ee4).
TEST(PlayerControllerTest, TurningIsTheHeldRateAndTheKeyboardIsSlowedWhileMoving)
{
    FakeWorld world;
    Mv::Speeds const speeds;
    Mv::BodyState body = At(0, 0, 0, 0.0f);
    Mv::ControlState control;
    control.TurnRate = PI / 2.0f;
    Drive(body, control, speeds, world, 1.0f);
    EXPECT_NEAR(body.Yaw, PI / 2.0f, 1e-4f);
    control.TurnRate = -PI;
    Drive(body, control, speeds, world, 1.0f);
    EXPECT_NEAR(body.Yaw, 1.5f * PI, 1e-4f);                        // wrapped into 0..2pi

    Mv::BodyState keys = At(0, 0, 0, 0.0f);
    Mv::ControlState held;
    held.TurnRate = PI;
    held.KeyboardTurn = true;
    held.Forward = 1;
    Drive(keys, held, speeds, world, 0.5f);
    EXPECT_NEAR(keys.Yaw, PI * 0.5f * Mv::KEYBOARD_TURN_WHILE_MOVING, 1e-4f);
}

// A rise up to STEP_UP (the client's 1.1917536, C0c) is walked onto; a higher one is a wall. Was 1.5 climbs / 2.0
// walls under the provisional 1.6: a 1.5 yd crate now needs a jump (apex 1.640), as the client's step rule says.
TEST(PlayerControllerTest, AStepIsWalkedOntoAndAHigherRiseIsAWall)
{
    Mv::Speeds const speeds;
    for (auto [height, climbs] : { std::pair{ 1.0f, true }, std::pair{ 1.5f, false } })
    {
        FakeWorld world;
        world.Boxes.push_back({ 3.0f, 20.0f, -5.0f, 5.0f, -10.0f, height });
        Mv::BodyState body = At(0, 0, 0);
        Mv::ControlState control;
        control.Forward = 1;
        Drive(body, control, speeds, world, 1.5f);
        if (climbs)
        {
            EXPECT_NEAR(body.Z, height, 1e-4f);
            EXPECT_GT(body.X, 5.0f);
        }
        else
        {
            EXPECT_NEAR(body.Z, 0.0f, 1e-4f);
            EXPECT_LT(body.X, 3.0f);
            EXPECT_TRUE(body.AgainstWall);
        }
    }
}

// A rise in the terrain itself (not a model: the terrain is in no collision tree, so no sweep sees it) higher than a
// step is a wall, not an edge to fall off.
TEST(PlayerControllerTest, ATerrainRiseHigherThanAStepIsAWallNotAFall)
{
    FakeWorld world;
    world.Ground = [](float x, float) { return x < 3.0f ? 0.0f : 3.0f; };
    Mv::Speeds const speeds;
    Mv::BodyState body = At(0, 0, 0);
    Mv::ControlState control;
    control.Forward = 1;
    Drive(body, control, speeds, world, 1.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_NEAR(body.Z, 0.0f, 1e-4f);
    EXPECT_LT(body.X, 3.0f);
    EXPECT_TRUE(body.AgainstWall);
}

// Up a slope of 40 degrees, refused up one of 55 (walkable: normal.z >= cos 50, the client's 0xa37f0c).
TEST(PlayerControllerTest, SlopesUpToFiftyDegreesAreWalkable)
{
    Mv::Speeds const speeds;
    for (auto [degrees, climbs] : { std::pair{ 40.0f, true }, std::pair{ 55.0f, false } })
    {
        FakeWorld world;
        float const k = std::tan(degrees * PI / 180.0f);
        world.Ground = [k](float x, float) { return x > 0.0f ? x * k : 0.0f; };
        Mv::BodyState body = At(-1.0f, 0, 0);
        Mv::ControlState control;
        control.Forward = 1;
        Drive(body, control, speeds, world, 1.0f);
        if (climbs)
            EXPECT_GT(body.Z, 2.0f) << degrees;
        else
        {
            EXPECT_LT(body.Z, 0.3f) << degrees;
            EXPECT_TRUE(body.SteepSlope);
        }
    }
}

// Off a ledge: a fall under the client's gravity, timed as h = g t^2 / 2, landing with its height reported.
TEST(PlayerControllerTest, OffALedgeIsAFallTimedByGravity)
{
    FakeWorld world;
    world.Boxes.push_back({ -50.0f, 0.0f, -5.0f, 5.0f, -10.0f, 10.0f });
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(-0.1f, 0, 10.0f);
    Mv::ControlState control;
    control.Forward = 1;
    bool fell = false;
    float fallSeconds = 0.0f;
    float height = 0.0f;
    for (int i = 0; i < 200; ++i)
    {
        Mv::Step(body, control, speeds, shape, world, 0.05f);
        if (body.Kind == Mv::Mode::Falling && !fell)
        {
            fell = true;
            fallSeconds = 0.0f;
        }
        else if (fell)
            fallSeconds += 0.05f;
        if (body.Landed)
        {
            height = body.FallHeight;
            break;
        }
    }
    ASSERT_TRUE(fell);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_NEAR(body.Z, 0.0f, 1e-4f);
    EXPECT_NEAR(height, 10.0f, 1e-3f);
    EXPECT_NEAR(fallSeconds, std::sqrt(2.0f * 10.0f / Mv::GRAVITY), 0.06f);   // within a tick of 1.018 s
    control.Forward = 0;
}

// A jump at the client's 7.9555473: apex 1.640 yd, 0.825 s in the air, the run kept and no air control.
TEST(PlayerControllerTest, AJumpReachesTheClientsApexAndKeepsItsRun)
{
    FakeWorld world;
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(0, 0, 0);
    Mv::ControlState control;
    control.Forward = 1;
    control.Jump = true;
    float apex = 0.0f;
    float air = 0.0f;
    for (int i = 0; i < 100; ++i)
    {
        Mv::Step(body, control, speeds, shape, world, 0.01f);
        apex = std::max(apex, body.Z);
        if (body.Kind == Mv::Mode::Falling)
            air += 0.01f;
        if (i == 10)
            control.Forward = 0;            // letting go in the air changes nothing: no air control
        if (body.Landed)
            break;
    }
    EXPECT_FALSE(control.Jump);
    EXPECT_NEAR(apex, Mv::JUMP_SPEED * Mv::JUMP_SPEED / (2.0f * Mv::GRAVITY), 0.01f);  // 1.640
    EXPECT_NEAR(air, 2.0f * Mv::JUMP_SPEED / Mv::GRAVITY, 0.03f);                       // 0.825 s
    EXPECT_NEAR(body.X, 7.0f * 2.0f * Mv::JUMP_SPEED / Mv::GRAVITY, 0.3f);             // the launch run throughout
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
}

// Into a wall square on: stopped short. Along a wall at an angle: slid along it.
TEST(PlayerControllerTest, AWallStopsSquareOnAndSlidesAtAnAngle)
{
    Mv::Speeds const speeds;
    FakeWorld world;
    world.Boxes.push_back({ 3.0f, 4.0f, -50.0f, 50.0f, -10.0f, 20.0f });
    Mv::BodyState square = At(0, 0, 0, 0.0f);
    Mv::ControlState control;
    control.Forward = 1;
    Drive(square, control, speeds, world, 1.0f);
    EXPECT_LT(square.X, 3.0f - Mv::Body{}.Radius + 0.01f);
    EXPECT_NEAR(square.Y, 0.0f, 1e-3f);
    EXPECT_TRUE(square.AgainstWall);

    Mv::BodyState angled = At(0, 0, 0, PI / 4.0f);         // 45 degrees into the same wall
    Drive(angled, control, speeds, world, 2.0f);
    EXPECT_LT(angled.X, 3.0f);
    EXPECT_GT(angled.Y, 6.0f);                              // carried on along it
}

// Into deep water: swimming; along at swim speed under the surface; out up the far shore onto the ground.
TEST(PlayerControllerTest, WaterIsSwumAndLeftAtTheShore)
{
    FakeWorld world;
    world.Ground = [](float x, float) { return x < 5.0f ? 0.0f : x < 25.0f ? -4.0f : 0.0f; };
    // Level with the banks: a swimmer floats with its feet a yard under, so the far bank is a step (1.0 < STEP_UP).
    // At -0.2 (before C0c) the bank was 1.2 up, now more than the client's step: a wall, taken with the swim jump.
    world.Waters.push_back({ 5.0f, 25.0f, -50.0f, 50.0f, 0.0f });
    Mv::Speeds const speeds;
    Mv::BodyState body = At(0, 0, 0);
    Mv::ControlState control;
    control.Forward = 1;
    Drive(body, control, speeds, world, 1.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Swimming);
    float const x0 = body.X;
    Drive(body, control, speeds, world, 1.0f);
    EXPECT_NEAR(body.X - x0, speeds.Swim, 0.2f);
    EXPECT_LE(body.Z, 0.0f - Mv::FLOAT_DEPTH * Mv::Body{}.Height + 1e-4f);     // floats half under
    Drive(body, control, speeds, world, 4.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_GT(body.X, 25.0f);
    EXPECT_NEAR(body.Z, 0.0f, 1e-4f);
}

// A bank of terrain more than a step above a swimmer's feet is a wall, not a way out (and not a fall off it).
TEST(PlayerControllerTest, AHighBankIsAWallToASwimmer)
{
    FakeWorld world;
    world.Ground = [](float x, float) { return x < 5.0f ? -4.0f : 1.0f; };
    world.Waters.push_back({ -50.0f, 5.0f, -50.0f, 50.0f, 0.0f });
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(0, 0, -Mv::FLOAT_DEPTH * shape.Height);
    body.Kind = Mv::Mode::Swimming;
    Mv::ControlState control;
    control.Forward = 1;
    Drive(body, control, speeds, world, 3.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Swimming);
    EXPECT_LT(body.X, 5.0f);
    EXPECT_TRUE(body.AgainstWall);
}

// A jump swimming at the surface is the breach: up at 9.0967 out of the water, and back into it.
TEST(PlayerControllerTest, ASwimJumpAtTheSurfaceBreachesAndLandsBackInTheWater)
{
    FakeWorld world;
    world.Ground = [](float, float) { return -10.0f; };
    world.Waters.push_back({ -50.0f, 50.0f, -50.0f, 50.0f, 0.0f });
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(0, 0, -Mv::FLOAT_DEPTH * shape.Height);
    body.Kind = Mv::Mode::Swimming;
    ASSERT_TRUE(Mv::CanJump(body, shape, world));
    Mv::ControlState control;
    control.Jump = true;
    Mv::Step(body, control, speeds, shape, world, 0.01f);
    EXPECT_EQ(body.Kind, Mv::Mode::Falling);
    EXPECT_NEAR(body.Vz, Mv::SWIM_JUMP_SPEED - Mv::GRAVITY * 0.01f, 1e-3f);
    float apex = body.Z;
    for (int i = 0; i < 200 && !body.Landed; ++i)
    {
        Mv::Step(body, control, speeds, shape, world, 0.01f);
        apex = std::max(apex, body.Z);
    }
    EXPECT_GT(apex, 0.0f);                                  // the feet left the water
    EXPECT_TRUE(body.LandedInWater);
    EXPECT_EQ(body.Kind, Mv::Mode::Swimming);

}

// The client's swim jump has no depth test (fn 0x9883f0, C0c): six yards down it still launches at 9.0967, rises
// its apex (2.145 yd) through the water and is swimming again on the way down. Was masked below the surface band
// before C0c.
TEST(PlayerControllerTest, ASwimJumpUnderwaterIsAHopThroughTheWater)
{
    FakeWorld world;
    world.Ground = [](float, float) { return -20.0f; };
    world.Waters.push_back({ -50.0f, 50.0f, -50.0f, 50.0f, 0.0f });
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(0, 0, -6.0f);
    body.Kind = Mv::Mode::Swimming;
    ASSERT_TRUE(Mv::CanJump(body, shape, world));
    Mv::ControlState control;
    control.Jump = true;
    float apex = body.Z;
    for (int i = 0; i < 200 && !body.Landed; ++i)
    {
        Mv::Step(body, control, speeds, shape, world, 0.01f);
        apex = std::max(apex, body.Z);
    }
    EXPECT_NEAR(apex - (-6.0f), Mv::SWIM_JUMP_SPEED * Mv::SWIM_JUMP_SPEED / (2.0f * Mv::GRAVITY), 0.05f);
    EXPECT_TRUE(body.LandedInWater);
    EXPECT_EQ(body.Kind, Mv::Mode::Swimming);
}

// Ascending or descending goes at 45 degrees (fn 0x987700): forward and up each 0.7071 of the speed; up alone is
// straight up at 0.7071 of it (interpreted, C6).
TEST(PlayerControllerTest, AscendingIsAtFortyFiveDegrees)
{
    FakeWorld world;
    world.Ground = [](float, float) { return -50.0f; };
    world.Waters.push_back({ -100.0f, 100.0f, -100.0f, 100.0f, 0.0f });
    Mv::Speeds const speeds;
    Mv::BodyState body = At(0, 0, -20.0f);
    body.Kind = Mv::Mode::Swimming;
    Mv::ControlState control;
    control.Vertical = 1;
    Drive(body, control, speeds, world, 1.0f);
    EXPECT_NEAR(body.Z - (-20.0f), Mv::VERTICAL_SHARE * speeds.Swim, 1e-3f);
    EXPECT_NEAR(body.X, 0.0f, 1e-4f);
    Mv::BodyState both = At(0, 0, -20.0f);
    both.Kind = Mv::Mode::Swimming;
    control.Forward = 1;
    Drive(both, control, speeds, world, 1.0f);
    EXPECT_NEAR(both.X, Mv::VERTICAL_SHARE * speeds.Swim, 1e-3f);
    EXPECT_NEAR(both.Z - (-20.0f), Mv::VERTICAL_SHARE * speeds.Swim, 1e-3f);
}

// A breach next to a low shore lands on it.
TEST(PlayerControllerTest, ASwimJumpBesideAShoreCanLandOnIt)
{
    FakeWorld world;
    world.Ground = [](float x, float) { return x < 1.0f ? -10.0f : 0.3f; };
    world.Waters.push_back({ -50.0f, 1.0f, -50.0f, 50.0f, 0.0f });
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(0.0f, 0, -Mv::FLOAT_DEPTH * shape.Height);
    body.Kind = Mv::Mode::Swimming;
    Mv::ControlState control;
    control.Forward = 1;
    control.Jump = true;
    for (int i = 0; i < 200 && !body.Landed; ++i)
        Mv::Step(body, control, speeds, shape, world, 0.01f);
    EXPECT_TRUE(body.Landed);
    EXPECT_FALSE(body.LandedInWater);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_NEAR(body.Z, 0.3f, 1e-4f);
}

// Flight: ascend takes off, the ceiling holds the climb, descend lands on the ground; dismounted in the air, a fall.
TEST(PlayerControllerTest, FlightTakesOffClimbsToTheCeilingAndLands)
{
    FakeWorld world;
    Mv::Speeds speeds;
    speeds.CanFly = true;
    speeds.CeilingAboveGround = 20.0f;
    Mv::BodyState body = At(0, 0, 0);
    Mv::ControlState control;
    control.Vertical = 1;
    Drive(body, control, speeds, world, 5.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Flying);
    EXPECT_NEAR(body.Z, 20.0f, 1e-3f);
    control.Vertical = -1;
    Drive(body, control, speeds, world, 5.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_NEAR(body.Z, 0.0f, 1e-3f);

    control.Vertical = 1;
    Drive(body, control, speeds, world, 1.0f);
    ASSERT_EQ(body.Kind, Mv::Mode::Flying);
    control.Vertical = 0;
    speeds.CanFly = false;                                  // dismounted in the air
    Drive(body, control, speeds, world, 3.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
}

// Deterministic, and a 250 ms tick moves the body exactly as five 50 ms ticks do (sub-steps of at most 50 ms).
TEST(PlayerControllerTest, OneLongTickIsTheSameAsFiveShortOnes)
{
    FakeWorld world;
    world.Boxes.push_back({ 4.0f, 30.0f, -50.0f, 50.0f, -10.0f, 1.2f });
    world.Boxes.push_back({ 12.0f, 13.0f, -50.0f, 2.0f, -10.0f, 30.0f });
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState a = At(0, 0, 0, 0.3f);
    Mv::BodyState b = a;
    Mv::ControlState ca;
    ca.Forward = 1;
    ca.Strafe = -1;
    ca.TurnRate = 0.4f;
    ca.Jump = true;
    Mv::ControlState cb = ca;
    for (int decision = 0; decision < 40; ++decision)
    {
        if (decision == 12)
        {
            ca.Jump = cb.Jump = true;
            ca.TurnRate = cb.TurnRate = -1.0f;
        }
        Mv::Step(a, ca, speeds, shape, world, 0.25f);
        for (int i = 0; i < 5; ++i)
            Mv::Step(b, cb, speeds, shape, world, 0.05f);
        ASSERT_FLOAT_EQ(a.X, b.X) << decision;
        ASSERT_FLOAT_EQ(a.Y, b.Y) << decision;
        ASSERT_FLOAT_EQ(a.Z, b.Z) << decision;
        ASSERT_FLOAT_EQ(a.Yaw, b.Yaw) << decision;
        ASSERT_EQ(a.Kind, b.Kind) << decision;
    }
}

// Motion the core imposes (a knockback, a fear, a spell's jump) is the core's: the controller is not stepped while
// it lasts, and takes the body back where it ends -- in the air a fall, on a floor the ground -- with the seat's held
// controls kept.
TEST(PlayerControllerTest, AfterImposedMotionTheBodyIsTakenBackWhereTheCoreLeftIt)
{
    FakeWorld world;
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Mv::BodyState body = At(0, 0, 0);
    Mv::ControlState control;
    control.Forward = 1;
    Drive(body, control, speeds, world, 0.5f);
    body.Vx = 99.0f;                                        // stale controller velocity the core's motion overrode

    // A knockback leaves the unit 6 yd up and 10 yd away: taken back falling from there, no stale velocity.
    Mv::Resync(body, 20.0f, 0.0f, 6.0f, 1.0f, shape, world);
    EXPECT_EQ(body.Kind, Mv::Mode::Falling);
    EXPECT_EQ(body.Vx, 0.0f);
    EXPECT_NEAR(body.Yaw, 1.0f, 1e-6f);
    Drive(body, control, speeds, world, 2.0f);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_NEAR(body.Z, 0.0f, 1e-4f);
    EXPECT_EQ(control.Forward, 1);                          // still held: the seat runs on

    // A fear ends on the ground: taken back standing there.
    Mv::Resync(body, 40.0f, 5.0f, 0.05f, 0.0f, shape, world);
    EXPECT_EQ(body.Kind, Mv::Mode::Ground);
    EXPECT_NEAR(body.Z, 0.0f, 1e-4f);
}

// What the action mask asks: jump on the ground (and at the surface), not in the air; height steered in water and
// air, or on the ground with flight to take off into.
TEST(PlayerControllerTest, JumpAndVerticalSteeringAreOnlyWhereTheyDoSomething)
{
    FakeWorld world;
    Mv::Body const shape;
    Mv::Speeds walker;
    Mv::Speeds flyer;
    flyer.CanFly = true;
    Mv::BodyState ground = At(0, 0, 0);
    EXPECT_TRUE(Mv::CanJump(ground, shape, world));
    EXPECT_FALSE(Mv::CanSteerVertically(ground, walker));
    EXPECT_TRUE(Mv::CanSteerVertically(ground, flyer));
    Mv::BodyState falling = ground;
    falling.Kind = Mv::Mode::Falling;
    EXPECT_FALSE(Mv::CanJump(falling, shape, world));
    Mv::BodyState swimming = ground;
    swimming.Kind = Mv::Mode::Swimming;
    EXPECT_TRUE(Mv::CanJump(swimming, shape, world));             // at any depth (C0c)
    Mv::BodyState flying = ground;
    flying.Kind = Mv::Mode::Flying;
    EXPECT_FALSE(Mv::CanJump(flying, shape, world));
    EXPECT_TRUE(Mv::CanSteerVertically(flying, flyer));
}

// The movement flags the core and clients read, from the body and its controls.
TEST(PlayerControllerTest, MovementFlagsAreThePlayersOwn)
{
    Mv::Speeds speeds;
    Mv::BodyState body = At(0, 0, 0);
    Mv::ControlState control;
    control.Forward = 1;
    control.Strafe = -1;
    control.TurnRate = 1.0f;
    EXPECT_EQ(Mv::MovementFlags(body, control, speeds),
        Mv::Flag::FORWARD | Mv::Flag::STRAFE_LEFT | Mv::Flag::LEFT);
    body.Kind = Mv::Mode::Swimming;
    control = {};
    control.Forward = -1;
    control.PitchRate = -0.5f;
    control.Vertical = 1;
    EXPECT_EQ(Mv::MovementFlags(body, control, speeds),
        Mv::Flag::BACKWARD | Mv::Flag::PITCH_DOWN | Mv::Flag::ASCENDING | Mv::Flag::SWIMMING);
    body.Kind = Mv::Mode::Falling;
    speeds.SlowFall = true;
    EXPECT_EQ(Mv::MovementFlags(body, {}, speeds), Mv::Flag::FALLING | Mv::Flag::FALLING_SLOW);
}
