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

#include "MarkerEncounter.h"
#include "MarkerReach.h"
#include <functional>
#include "UnitDefines.h"
#include "gtest/gtest.h"
#include <cmath>

using Animus::Curriculum::MarkerEncounter;
using Animus::Curriculum::MarkerRung;

// A slide along a wall that keeps most of the ground the keys ask for is the right way round a corner's inside: no
// charge. Pressing into it and getting nowhere is the full price; between them, in proportion to the shortfall.
TEST(MarkerEncounterTest, WallChargesOnlyTheGroundNotCovered)
{
    float const price = 0.03f;
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.25f, 1.75f, 1.75f, price, 0.5f), 0.0f);    // full speed along it
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.25f, 0.9f, 1.75f, price, 0.5f), 0.0f);     // a slide at 51%
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.25f, 0.0f, 1.75f, price, 0.5f), price * 0.25f);
    EXPECT_NEAR(MarkerEncounter::WallCharge(0.25f, 0.4375f, 1.75f, price, 0.5f), price * 0.25f * 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(MarkerEncounter::WallCharge(0.0f, 0.0f, 1.75f, price, 0.5f), 0.0f);      // no wall, no charge
}

// Stopped is the server's state: no moving flag (a jump is FALLING), not swimming or flying, and still. A turn is not
// moving (ratified 2026-10-05).
TEST(MarkerEncounterTest, StoppedReadsTheServersFlags)
{
    EXPECT_TRUE(MarkerEncounter::Stopped(0, 0.0f, 0.05f));
    EXPECT_TRUE(MarkerEncounter::Stopped(MOVEMENTFLAG_LEFT, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_FORWARD, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_STRAFE_LEFT, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_FALLING, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(MOVEMENTFLAG_SWIMMING, 0.0f, 0.05f));
    EXPECT_FALSE(MarkerEncounter::Stopped(0, 0.2f, 0.05f));
}

// M1's ladder: the furthest distance and the bearing widen and the radius tightens from the first rung to the last,
// which is the stage's real task (60 yd, anywhere round, half a yard).
TEST(MarkerEncounterTest, ControlsLadderEndsAtTheRealTask)
{
    MarkerRung const first = MarkerEncounter::RungTask(0, 8, 5.0f, 10.0f, 60.0f, 20.0f, 180.0f, 4.0f, 0.5f);
    MarkerRung const last = MarkerEncounter::RungTask(7, 8, 5.0f, 10.0f, 60.0f, 20.0f, 180.0f, 4.0f, 0.5f);
    EXPECT_FLOAT_EQ(first.Furthest, 10.0f);
    EXPECT_FLOAT_EQ(first.Radius, 4.0f);
    EXPECT_NEAR(first.BearingHalf, 20.0f * float(M_PI) / 180.0f, 1e-5f);
    EXPECT_FLOAT_EQ(last.Furthest, 60.0f);
    EXPECT_FLOAT_EQ(last.Radius, 0.5f);
    EXPECT_NEAR(last.BearingHalf, float(M_PI), 1e-5f);
    EXPECT_FLOAT_EQ(last.Nearest, 5.0f);
}

// M2's ladder: distance and detour window climb, the radius stays.
TEST(MarkerEncounterTest, GroundLadderWidensTheDetour)
{
    MarkerRung const first = MarkerEncounter::GroundRungTask(0, 6, 20.0f, 40.0f, 120.0f, 1.0f, 1.6f, 0.25f, 1.0f);
    MarkerRung const last = MarkerEncounter::GroundRungTask(5, 6, 20.0f, 40.0f, 120.0f, 1.0f, 1.6f, 0.25f, 1.0f);
    EXPECT_FLOAT_EQ(first.DetourMin, 1.0f);
    EXPECT_FLOAT_EQ(first.DetourMax, 1.25f);
    EXPECT_FLOAT_EQ(last.DetourMin, 1.6f);
    EXPECT_FLOAT_EQ(last.Furthest, 120.0f);
    EXPECT_FLOAT_EQ(first.Radius, last.Radius);
}

// M3's ladder: the height window and the distance climb, the radius stays.
TEST(MarkerEncounterTest, VerticalLadderRaisesTheHeight)
{
    MarkerRung const first = MarkerEncounter::VerticalRungTask(0, 6, 10.0f, 30.0f, 80.0f, 1.0f, 6.0f, 15.0f, 45.0f,
        1.0f);
    MarkerRung const last = MarkerEncounter::VerticalRungTask(5, 6, 10.0f, 30.0f, 80.0f, 1.0f, 6.0f, 15.0f, 45.0f,
        1.0f);
    EXPECT_FLOAT_EQ(first.HeightMin, 1.0f);
    EXPECT_FLOAT_EQ(first.HeightMax, 6.0f);
    EXPECT_FLOAT_EQ(last.HeightMin, 15.0f);
    EXPECT_FLOAT_EQ(last.HeightMax, 45.0f);
    EXPECT_FLOAT_EQ(last.Furthest, 80.0f);
}

namespace
{
    namespace Mv = Animus::Movement;
    namespace Reach = Animus::Curriculum::MarkerReach;

    /// Ground that rises along x only: height(x), and the floor's normal there. No models, no water.
    class Ridge final : public Mv::WorldQuery
    {
    public:
        Ridge(std::function<float(float)> height, std::function<float(float)> normal)
            : _height(std::move(height)), _normal(std::move(normal)) { }

        [[nodiscard]] float FloorBelow(float x, float /*y*/, float z, float search) const override
        {
            float const h = _height(x);
            return h <= z + 1e-4f && z - h <= search ? h : Mv::INVALID_FLOOR;
        }
        [[nodiscard]] float FloorNormalZ(float x, float /*y*/, float /*z*/) const override { return _normal(x); }
        [[nodiscard]] Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float, float, float, float up) const override { return up; }
        [[nodiscard]] bool InTerrain(float x, float /*y*/, float z) const override { return z < _height(x) - 1e-4f; }

    private:
        std::function<float(float)> _height;
        std::function<float(float)> _normal;
    };

    Reach::Result WalkAlongX(Ridge const& ridge, float to)
    {
        float const xs[] = { 0.0f, to };
        float const ys[] = { 0.0f, 0.0f };
        float const zs[] = { ridge.FloorBelow(0.0f, 0.0f, 1000.0f, 2000.0f), ridge.FloorBelow(to, 0.0f, 1000.0f,
            2000.0f) };
        return Reach::Walk(ridge, Mv::Body(), xs, ys, zs, 2);
    }

    float Flat(float) { return 1.0f; }
}

// Every rise a step (<= 1.19 yd) or a jump (<= the 1.64 yd apex); a drop of any height is allowed; a face steeper
// than the client walks only for one jump's worth; anything else is not a way the controller can go.
TEST(MarkerEncounterTest, ReachWalksStepsJumpsAndDropsAndRefusesTheRest)
{
    EXPECT_NEAR(Reach::JUMP_APEX, 1.640f, 0.01f);

    Reach::Result const flat = WalkAlongX(Ridge([](float) { return 0.0f; }, Flat), 20.0f);
    EXPECT_TRUE(flat.Reachable);
    EXPECT_EQ(flat.Jumps, 0u);

    Reach::Result const step = WalkAlongX(Ridge([](float x) { return x >= 5.0f ? 1.0f : 0.0f; }, Flat), 20.0f);
    EXPECT_TRUE(step.Reachable);
    EXPECT_EQ(step.Jumps, 0u);

    Reach::Result const hop = WalkAlongX(Ridge([](float x) { return x >= 5.0f ? 1.5f : 0.0f; }, Flat), 20.0f);
    EXPECT_TRUE(hop.Reachable);
    EXPECT_EQ(hop.Jumps, 1u);

    EXPECT_FALSE(WalkAlongX(Ridge([](float x) { return x >= 5.0f ? 2.0f : 0.0f; }, Flat), 20.0f).Reachable);

    Reach::Result const drop = WalkAlongX(Ridge([](float x) { return x >= 5.0f ? -10.0f : 0.0f; }, Flat), 20.0f);
    EXPECT_TRUE(drop.Reachable);
    EXPECT_NEAR(drop.MaxDrop, 10.0f, 1e-3f);

    // 55 degrees for five yards up: too steep to walk, more than a jump. 40 degrees: walked.
    auto const slope = [](float tangent) { return [tangent](float x) { return std::clamp(x - 5.0f, 0.0f, 5.0f / tangent)
        * tangent; }; };
    EXPECT_FALSE(WalkAlongX(Ridge(slope(std::tan(55.0f * float(M_PI) / 180.0f)),
        [](float x) { return x > 5.0f && x < 8.6f ? std::cos(55.0f * float(M_PI) / 180.0f) : 1.0f; }), 20.0f)
        .Reachable);
    EXPECT_TRUE(WalkAlongX(Ridge(slope(std::tan(40.0f * float(M_PI) / 180.0f)),
        [](float x) { return x > 5.0f && x < 11.0f ? std::cos(40.0f * float(M_PI) / 180.0f) : 1.0f; }), 20.0f)
        .Reachable);
}
