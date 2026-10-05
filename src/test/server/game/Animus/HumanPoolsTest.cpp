/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
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

#include "HumanPools.h"
#include "gtest/gtest.h"

namespace HumanPools = Animus::Curriculum::HumanPools;

using HumanPools::Mode;
using HumanPools::Verdict;

namespace
{
    /// An on-foot arena on map 1: trips 40-160 yd, trained around (0, 0), scored around (1000, 0).
    HumanPools::ArenaTerrain OnFoot()
    {
        HumanPools::ArenaTerrain arena;
        arena.MapId = 1;
        arena.Least = 40.0f;
        arena.Most = 160.0f;
        arena.Modes = HumanPools::ModesFor(false, false, false, true);
        arena.Training = { { 0.0f, 0.0f, 0.0f } };
        arena.HeldOut = { { 1000.0f, 0.0f, 0.0f } };
        arena.Reach = 160.0f;
        arena.Seconds = 150.0f;
        return arena;
    }

    HumanPools::Trip TripOf(float fromX, float toX, Mode how = Mode::Ground, float seconds = 20.0f, float dropZ = 0.0f)
    {
        HumanPools::Trip trip;
        trip.Start = { fromX, 0.0f, 0.0f };
        trip.End = { toX, 0.0f, -dropZ };
        trip.How = how;
        trip.Seconds = seconds;
        return trip;
    }
}

// The distance band is the straight line on the ground, both edges included.
TEST(HumanPoolsTest, TheBandIsTheStraightLineEdgesIncluded)
{
    HumanPools::ArenaTerrain const arena = OnFoot();
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 40.0f), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 160.0f), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 39.0f), arena), Verdict::Distance);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 161.0f), arena), Verdict::Distance);

    // Height does not count towards it: a 100 yd trip down a 60 yd cliff is still 100 yd.
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Ground, 20.0f, 60.0f), arena), Verdict::Taken);
}

// Each kind of arena takes its own kind of trip.
TEST(HumanPoolsTest, EachArenaTakesItsOwnModes)
{
    uint8 const onFoot = HumanPools::ModesFor(false, false, false, true);
    uint8 const riding = HumanPools::ModesFor(false, false, false, false);
    uint8 const water = HumanPools::ModesFor(false, true, false, true);
    uint8 const lakebed = HumanPools::ModesFor(false, false, true, true);
    uint8 const flight = HumanPools::ModesFor(true, false, false, false);
    EXPECT_EQ(onFoot, uint8(Mode::Ground));
    EXPECT_EQ(riding, uint8(Mode::Ground) | uint8(Mode::Mounted));
    EXPECT_EQ(water, uint8(Mode::Swim));
    EXPECT_EQ(lakebed, uint8(Mode::Swim));
    EXPECT_EQ(flight, uint8(Mode::Fly));

    HumanPools::ArenaTerrain arena = OnFoot();
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Ground), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Mounted), arena), Verdict::Mode);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Swim), arena), Verdict::Mode);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Fly), arena), Verdict::Mode);

    arena.Modes = riding;
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Mounted), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Fly), arena), Verdict::Mode);

    arena.Modes = water;
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Swim), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Ground), arena), Verdict::Mode);

    arena.Modes = flight;
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Fly), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Mounted), arena), Verdict::Mode);
}

// Another map's trips are never an arena's, whatever else they match.
TEST(HumanPoolsTest, AnotherMapIsNeverTaken)
{
    HumanPools::ArenaTerrain const arena = OnFoot();
    EXPECT_EQ(HumanPools::Judge(530, TripOf(0.0f, 100.0f), arena), Verdict::Map);

    HumanPools::HardSpot spot;
    EXPECT_EQ(HumanPools::Judge(0, spot, arena), Verdict::Map);
    EXPECT_EQ(HumanPools::Judge(1, spot, arena), Verdict::Taken);
}

// A start far from the arena's ground, or either end nearer its evaluation ground, is refused.
TEST(HumanPoolsTest, TheArenaGroundAndNotItsEvaluationGround)
{
    HumanPools::ArenaTerrain const arena = OnFoot();
    EXPECT_EQ(HumanPools::Judge(1, TripOf(-200.0f, -100.0f), arena), Verdict::Terrain);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(150.0f, 50.0f), arena), Verdict::Taken);
    // The end crosses the midpoint between the training and the held-out ground.
    HumanPools::ArenaTerrain wide = arena;
    wide.Reach = 600.0f;
    wide.Most = 400.0f;
    EXPECT_EQ(HumanPools::Judge(1, TripOf(400.0f, 600.0f), wide), Verdict::HeldOut);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(550.0f, 400.0f), wide), Verdict::HeldOut);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(300.0f, 450.0f), wide), Verdict::Taken);

    HumanPools::HardSpot spot;
    spot.Pos = { 990.0f, 0.0f, 0.0f };
    wide.Reach = 2000.0f;
    EXPECT_EQ(HumanPools::Judge(1, spot, wide), Verdict::HeldOut);
}

// A trip a human took longer than the whole clock over is not one the arena can set.
TEST(HumanPoolsTest, NoLongerThanTheClock)
{
    HumanPools::ArenaTerrain const arena = OnFoot();
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Ground, 150.0f), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 100.0f, Mode::Ground, 151.0f), arena), Verdict::Clock);
}

// A ledge arena wants the end below the start, inside its drop range give or take the slack.
TEST(HumanPoolsTest, ALedgeArenaWantsTheDrop)
{
    HumanPools::ArenaTerrain arena = OnFoot();
    arena.Descent = true;
    arena.DropMin = 5.0f;
    arena.DropMax = 80.0f;
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 60.0f, Mode::Ground, 20.0f, 10.0f), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 60.0f, Mode::Ground, 20.0f, 4.5f), arena), Verdict::Taken);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 60.0f, Mode::Ground, 20.0f, 2.0f), arena), Verdict::Drop);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 60.0f, Mode::Ground, 20.0f, -10.0f), arena), Verdict::Drop);
    EXPECT_EQ(HumanPools::Judge(1, TripOf(0.0f, 60.0f, Mode::Ground, 20.0f, 90.0f), arena), Verdict::Drop);
}

// A drowning is a start only where the arena has water.
TEST(HumanPoolsTest, ADrowningOnlyWhereThereIsWater)
{
    HumanPools::ArenaTerrain arena = OnFoot();
    HumanPools::HardSpot spot;
    spot.Kind = HumanPools::SpotKind::Drown;
    EXPECT_EQ(HumanPools::Judge(1, spot, arena), Verdict::Kind);
    arena.Wet = true;
    EXPECT_EQ(HumanPools::Judge(1, spot, arena), Verdict::Taken);
    spot.Kind = HumanPools::SpotKind::Stuck;
    arena.Wet = false;
    EXPECT_EQ(HumanPools::Judge(1, spot, arena), Verdict::Taken);
}

// Filter keeps what Judge takes and counts the rest by reason.
TEST(HumanPoolsTest, FilterTalliesEveryVerdict)
{
    HumanPools::Parsed<HumanPools::Trip> parsed;
    parsed.Maps[1] = { TripOf(0.0f, 100.0f), TripOf(0.0f, 10.0f), TripOf(0.0f, 100.0f, Mode::Fly) };
    parsed.Maps[530] = { TripOf(0.0f, 100.0f) };
    HumanPools::Tally tally;
    std::vector<HumanPools::Trip> const taken = HumanPools::Filter(parsed, OnFoot(), tally);
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(tally.Of(Verdict::Taken), 1u);
    EXPECT_EQ(tally.Of(Verdict::Distance), 1u);
    EXPECT_EQ(tally.Of(Verdict::Mode), 1u);
    EXPECT_EQ(tally.Of(Verdict::Map), 1u);
    EXPECT_EQ(tally.Refusals(), "map 1, mode 1, distance 1");
    EXPECT_EQ(HumanPools::Tally().Refusals(), "none");
}

// The hard-spot draw is weighted by count.
TEST(HumanPoolsTest, SpotsAreDrawnByCount)
{
    std::vector<HumanPools::HardSpot> spots(3);
    spots[0].Count = 1;
    spots[1].Count = 3;
    spots[2].Count = 6;
    std::vector<uint64> const weights = HumanPools::Weights(spots);
    ASSERT_EQ(weights, (std::vector<uint64>{ 1, 4, 10 }));
    EXPECT_EQ(HumanPools::Pick(weights, 0), 0u);
    EXPECT_EQ(HumanPools::Pick(weights, 1), 1u);
    EXPECT_EQ(HumanPools::Pick(weights, 3), 1u);
    EXPECT_EQ(HumanPools::Pick(weights, 4), 2u);
    EXPECT_EQ(HumanPools::Pick(weights, 9), 2u);
}

// Both files as FORMAT.md section 5 has them.
TEST(HumanPoolsTest, ParsesBothFiles)
{
    auto const trips = HumanPools::ParseTrips(R"({"format": 1, "maps": {"1": [
        {"start": [1, 2, 3], "end": [4.5, 5, 6], "seconds": 12.5, "mode": "swim", "path": [[1, 2, 3], [4, 5, 6]]},
        {"start": [0, 0, 0], "end": [9, 9, 9], "seconds": 3, "mode": "mounted"}], "530": []}})");
    ASSERT_TRUE(trips.Ok()) << trips.Error;
    EXPECT_EQ(trips.Count(), 2u);
    ASSERT_EQ(trips.Maps.at(1).size(), 2u);
    HumanPools::Trip const& first = trips.Maps.at(1)[0];
    EXPECT_FLOAT_EQ(first.End.X, 4.5f);
    EXPECT_FLOAT_EQ(first.Seconds, 12.5f);
    EXPECT_EQ(first.How, Mode::Swim);
    EXPECT_EQ(first.Path.size(), 2u);
    EXPECT_EQ(trips.Maps.at(1)[1].How, Mode::Mounted);
    EXPECT_TRUE(trips.Maps.at(1)[1].Path.empty());
    EXPECT_TRUE(trips.Maps.at(530).empty());

    auto const spots = HumanPools::ParseHardSpots(R"({"format": 1, "maps": {"0": [
        {"pos": [1, 2, 3], "kind": "fall", "count": 7}, {"pos": [4, 5, 6], "kind": "drown", "count": 1}]}})");
    ASSERT_TRUE(spots.Ok()) << spots.Error;
    ASSERT_EQ(spots.Maps.at(0).size(), 2u);
    EXPECT_EQ(spots.Maps.at(0)[0].Kind, HumanPools::SpotKind::Fall);
    EXPECT_EQ(spots.Maps.at(0)[0].Count, 7u);
    EXPECT_EQ(spots.Maps.at(0)[1].Kind, HumanPools::SpotKind::Drown);
}

// Anything malformed refuses the whole file, with a reason, and leaves nothing to draw from.
TEST(HumanPoolsTest, MalformedFilesAreRefusedWhole)
{
    char const* const badTrips[] = {
        "",
        "not json",
        "[1, 2]",
        R"({"maps": {}})",
        R"({"format": 2, "maps": {}})",
        R"({"format": "1", "maps": {}})",
        R"({"format": 1})",
        R"({"format": 1, "maps": []})",
        R"({"format": 1, "maps": {"kalimdor": []}})",
        R"({"format": 1, "maps": {"1": {}}})",
        R"({"format": 1, "maps": {"1": [{"start": [1, 2], "end": [1, 2, 3], "seconds": 1, "mode": "ground"}]}})",
        R"({"format": 1, "maps": {"1": [{"start": [1, 2, 3], "end": [1, 2, 3], "seconds": 1, "mode": "walk"}]}})",
        R"({"format": 1, "maps": {"1": [{"start": [1, 2, 3], "end": [1, 2, 3], "seconds": -1, "mode": "ground"}]}})",
        R"({"format": 1, "maps": {"1": [{"start": [1, 2, 3], "end": [1, 2, 3], "mode": "ground"}]}})",
        R"({"format": 1, "maps": {"1": [{"start": [1, 2, 3], "end": [1, "x", 3], "seconds": 1, "mode": "fly"}]}})",
        R"({"format": 1, "maps": {"1": [{"start": [1, 2, 3], "end": [1, 2, 3], "seconds": 1, "mode": "fly",
            "path": [[1, 2]]}]}})",
        // A good map does not save a file with a bad one.
        R"({"format": 1, "maps": {"0": [{"start": [1, 2, 3], "end": [1, 2, 3], "seconds": 1, "mode": "ground"}],
            "1": [7]}})",
    };
    for (char const* text : badTrips)
    {
        auto const parsed = HumanPools::ParseTrips(text);
        EXPECT_FALSE(parsed.Ok()) << text;
        EXPECT_FALSE(parsed.Error.empty()) << text;
        EXPECT_TRUE(parsed.Maps.empty()) << text;
    }

    char const* const badSpots[] = {
        "{",
        R"({"format": 1, "maps": {"1": [{"pos": [1, 2, 3], "kind": "death"}]}})",
        R"({"format": 1, "maps": {"1": [{"pos": [1, 2, 3], "kind": "death", "count": 0}]}})",
        R"({"format": 1, "maps": {"1": [{"pos": [1, 2, 3], "kind": "death", "count": 2.5}]}})",
        R"({"format": 1, "maps": {"1": [{"pos": [1, 2, 3], "kind": "death", "count": "2"}]}})",
        R"({"format": 1, "maps": {"1": [{"pos": [1, 2, 3], "kind": "lava", "count": 2}]}})",
        R"({"format": 1, "maps": {"1": [{"pos": [1, 2, 3, 4], "kind": "stuck", "count": 2}]}})",
        R"({"format": 1, "maps": {"-1": [{"pos": [1, 2, 3], "kind": "stuck", "count": 2}]}})",
    };
    for (char const* text : badSpots)
    {
        auto const parsed = HumanPools::ParseHardSpots(text);
        EXPECT_FALSE(parsed.Ok()) << text;
        EXPECT_TRUE(parsed.Maps.empty()) << text;
    }
}
