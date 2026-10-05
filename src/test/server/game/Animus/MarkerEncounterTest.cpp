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

#include "FollowEncounter.h"
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

namespace
{
    /// A two-storey building along x: the ground floor at 0 everywhere, a slab (the upper floor) at 4 for x >= 10 with
    /// a roof at 8 over all of it, and stairs from x = 4 to 10 rising to the slab, 0.67 yd a yard.
    class TwoStoreys final : public Mv::WorldQuery
    {
    public:
        static constexpr float UPPER = 4.0f;
        static constexpr float ROOF = 8.0f;

        [[nodiscard]] float FloorBelow(float x, float /*y*/, float z, float search) const override
        {
            float best = Mv::INVALID_FLOOR;
            for (float floor : { 0.0f, Stair(x), x >= 10.0f ? UPPER : Mv::INVALID_FLOOR, ROOF })
                if (floor > Mv::INVALID_FLOOR && floor <= z + 1e-4f && z - floor <= search)
                    best = std::max(best, floor);
            return best;
        }
        [[nodiscard]] float FloorNormalZ(float, float, float) const override { return 1.0f; }
        [[nodiscard]] Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float x, float /*y*/, float z, float up) const override
        {
            float const over = x >= 10.0f && z < UPPER - 1e-3f ? UPPER : ROOF;
            return std::clamp(over - z, 0.0f, up);
        }
        [[nodiscard]] bool InTerrain(float, float, float) const override { return false; }

    private:
        static float Stair(float x)
        {
            return x >= 4.0f && x < 10.0f ? std::floor(x - 4.0f + 1.0f) * UPPER / 6.0f : Mv::INVALID_FLOOR;
        }
    };
}

// Up the stairs: the upper storey is found under the roof over it, not on the roof, and the way up the stairs is one
// the controller walks (every stair under STEP_UP), ending on the upper floor.
TEST(MarkerEncounterTest, AnUpstairsMarkerIsFoundAndReachable)
{
    TwoStoreys const building;
    float floors[4];
    uint32_t const found = Reach::UpperFloors(building, 12.0f, 0.0f, 0.0f, 3.0f, 12.0f, floors, 4);
    ASSERT_GE(found, 1u);
    EXPECT_FLOAT_EQ(floors[0], TwoStoreys::UPPER);
    for (uint32_t i = 0; i < found; ++i)
        EXPECT_LT(floors[i], TwoStoreys::ROOF);         // the roof has no headroom over it in here: never a floor

    float const xs[] = { 0.0f, 4.0f, 10.0f, 12.0f };
    float const ys[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float const zs[] = { 0.0f, 0.0f, TwoStoreys::UPPER, TwoStoreys::UPPER };
    Reach::Result const up = Reach::Walk(building, Mv::Body(), xs, ys, zs, 4);
    EXPECT_TRUE(up.Reachable);
    EXPECT_EQ(up.Jumps, 0u);
    EXPECT_NEAR(up.EndZ, TwoStoreys::UPPER, 1e-4f);
}

namespace
{
    /// A lake along x: dry ground at 0 before x = 5, a bed at -5 under water at 0 from 5 to 15, and a far bank of
    /// `bank` yards over the surface from 15 on. `deadly` makes the liquid magma.
    class Lake final : public Mv::WorldQuery
    {
    public:
        Lake(float bank, bool deadly) : _bank(bank), _deadly(deadly) { }

        [[nodiscard]] float FloorBelow(float x, float /*y*/, float z, float search) const override
        {
            float const h = Height(x);
            return h <= z + 1e-4f && z - h <= search ? h : Mv::INVALID_FLOOR;
        }
        [[nodiscard]] float FloorNormalZ(float, float, float) const override { return 1.0f; }
        [[nodiscard]] Mv::Liquid LiquidAt(float x, float, float) const override
        {
            Mv::Liquid liquid;
            liquid.Present = x >= 5.0f && x < 15.0f;
            liquid.Level = 0.0f;
            liquid.Deadly = _deadly;
            return liquid;
        }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float, float, float, float up) const override { return up; }
        [[nodiscard]] bool InTerrain(float x, float, float z) const override { return z < Height(x) - 1e-4f; }

    private:
        [[nodiscard]] float Height(float x) const { return x < 5.0f ? 0.0f : x < 15.0f ? -5.0f : _bank; }
        float _bank;
        bool _deadly;
    };

    Reach::Result Across(Lake const& lake, float to)
    {
        float const xs[] = { 0.0f, to };
        float const ys[] = { 0.0f, 0.0f };
        float const zs[] = { 0.0f, 0.0f };
        return Reach::Walk(lake, Mv::Body(), xs, ys, zs, 2);
    }
}

// Water deep enough is swum at its surface; a bank is climbed out on if the swim jump (2.145 yd) carries it, though a
// jump from the ground (1.64) would not; a taller bank, or magma, is no way at all.
TEST(MarkerEncounterTest, ReachSwimsAcrossAndClimbsOutOnABankTheSwimJumpCarries)
{
    EXPECT_NEAR(Reach::SWIM_JUMP_APEX, 2.145f, 0.01f);

    Reach::Result const swim = Across(Lake(1.8f, false), 20.0f);
    EXPECT_TRUE(swim.Reachable);
    EXPECT_EQ(swim.Swims, 1u);
    EXPECT_EQ(swim.BanksClimbed, 1u);
    EXPECT_FALSE(swim.EndSwimming);

    Reach::Result const dive = Across(Lake(1.8f, false), 10.0f);
    EXPECT_TRUE(dive.Reachable);
    EXPECT_TRUE(dive.EndSwimming);                      // a lakebed marker is under the water the walk ends in

    EXPECT_FALSE(Across(Lake(2.5f, false), 20.0f).Reachable);
    EXPECT_FALSE(Across(Lake(0.0f, true), 20.0f).Reachable);
}

// M5's ladder: the distance band moves out and the detour window up, never over its ceiling.
TEST(MarkerEncounterTest, RoutesLadderLengthensTheTripAndCapsTheDetour)
{
    MarkerRung const first = MarkerEncounter::RoutesRungTask(0, 6, 150.0f, 250.0f, 400.0f, 600.0f, 1.3f, 2.4f, 0.6f,
        3.0f, 2.0f);
    MarkerRung const last = MarkerEncounter::RoutesRungTask(5, 6, 150.0f, 250.0f, 400.0f, 600.0f, 1.3f, 2.4f, 0.6f,
        3.0f, 2.0f);
    EXPECT_FLOAT_EQ(first.Nearest, 150.0f);
    EXPECT_FLOAT_EQ(first.Furthest, 250.0f);
    EXPECT_NEAR(first.DetourMin, 1.3f, 1e-5f);
    EXPECT_NEAR(first.DetourMax, 1.9f, 1e-5f);
    EXPECT_FLOAT_EQ(last.Nearest, 400.0f);
    EXPECT_FLOAT_EQ(last.Furthest, 600.0f);
    EXPECT_NEAR(last.DetourMin, 2.4f, 1e-5f);
    EXPECT_FLOAT_EQ(last.DetourMax, 3.0f);
}

// M7: where a distance to the leader falls (too close, in the band, behind, lost) and the leader's trips lengthening.
TEST(MarkerEncounterTest, FollowBandsAndTrips)
{
    using Animus::Curriculum::FollowEncounter;
    EXPECT_EQ(FollowEncounter::Band(2.0f, 3.0f, 10.0f, 30.0f), 0u);
    EXPECT_EQ(FollowEncounter::Band(3.0f, 3.0f, 10.0f, 30.0f), 1u);
    EXPECT_EQ(FollowEncounter::Band(10.0f, 3.0f, 10.0f, 30.0f), 1u);
    EXPECT_EQ(FollowEncounter::Band(20.0f, 3.0f, 10.0f, 30.0f), 2u);
    EXPECT_EQ(FollowEncounter::Band(31.0f, 3.0f, 10.0f, 30.0f), 3u);
    EXPECT_FLOAT_EQ(FollowEncounter::TripFurthest(0, 6, 60.0f, 200.0f), 60.0f);
    EXPECT_FLOAT_EQ(FollowEncounter::TripFurthest(5, 6, 60.0f, 200.0f), 200.0f);
}
