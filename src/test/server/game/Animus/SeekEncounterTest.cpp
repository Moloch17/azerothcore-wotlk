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

#include "RewardLedger.h"
#include "SeekDraw.h"
#include "SeekEncounter.h"
#include "StageDefinition.h"
#include "gtest/gtest.h"
#include <map>
#include <set>
#include <string>

namespace Cu = Animus::Curriculum;
namespace Draw = Animus::Curriculum::SeekDraw;

namespace
{
    Cu::ArenaDefinition const& SeekArena()
    {
        Cu::StageDefinition const* stage = Cu::FindStage("move2_seek");
        EXPECT_NE(stage, nullptr);
        return stage->Arenas.at(0);
    }
}

// M2 is a valid stage: no problem left it out, it extends M1, carries the camera and not the compass, and is one seek
// arena in the Stockades with 300 s episodes and a 3 yd radius.
TEST(SeekEncounterTest, TheStageIsDefined)
{
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;
    Cu::StageDefinition const* stage = Cu::FindStage("move2_seek");
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(stage->Extends, "move1_controls");
    EXPECT_EQ(stage->Blocks, (std::vector<Cu::BlockId>{ Cu::BlockId::Core, Cu::BlockId::Move, Cu::BlockId::Vision,
        Cu::BlockId::Entities, Cu::BlockId::Goal }));    // the camera brings its entity list
    EXPECT_EQ(stage->Level, 1);
    ASSERT_EQ(stage->Arenas.size(), 1u);
    Cu::ArenaDefinition const& arena = stage->Arenas[0];
    EXPECT_EQ(arena.Against, Cu::Opposition::Seek);
    EXPECT_EQ(arena.EpisodeSeconds, 300u);
    EXPECT_FLOAT_EQ(arena.SeekRadius, 3.0f);
    EXPECT_EQ(arena.MapId, 34u);
    ASSERT_EQ(arena.SpawnPoints.size(), 1u);
    EXPECT_NEAR(arena.SpawnPoints[0].GetPositionX(), 54.23f, 0.01f);
}

// The room table: 39 named rooms, each floor a convex polygon of 3 to 8 corners with some room in it, its centre on
// it, its opening outside it but near (a doorway), its height on one of the Stockades' three floors, no two rooms
// overlapping, and its walk from the spawn at least the straight line to it.
TEST(SeekEncounterTest, TheRoomTableIsWellFormed)
{
    Cu::ArenaDefinition const& arena = SeekArena();
    ASSERT_EQ(arena.Rooms.size(), 39u);
    std::set<std::string> names;
    for (Cu::SeekRoom const& room : arena.Rooms)
    {
        EXPECT_TRUE(names.insert(room.Name).second) << room.Name;
        EXPECT_GE(room.Floor.size(), 3u) << room.Name;
        EXPECT_LE(room.Floor.size(), 8u) << room.Name;
        EXPECT_GT(Draw::Area(room.Floor), 15.0f) << room.Name;
        EXPECT_TRUE(Draw::Inside(room.Floor, room.Centre.first, room.Centre.second)) << room.Name;
        EXPECT_FALSE(Draw::Inside(room.Floor, room.Opening.first, room.Opening.second)) << room.Name;
        float nearest = 1e9f;
        for (auto const& [x, y] : room.Floor)
            nearest = std::min(nearest, std::hypot(x - room.Opening.first, y - room.Opening.second));
        EXPECT_LT(nearest, 12.0f) << room.Name << "'s opening is not at the room";
        bool const level = std::fabs(room.FloorZ + 25.6f) < 1.2f || std::fabs(room.FloorZ + 34.4f) < 1.0f;
        EXPECT_TRUE(level) << room.Name << " at " << room.FloorZ;
        float const straight = std::hypot(room.Centre.first - 54.23f, room.Centre.second - 0.28f);
        EXPECT_GE(room.Walk + 1.0f, straight) << room.Name;
        // Convex: every corner on the same side of each edge.
        for (auto const& [x, y] : room.Floor)
            EXPECT_TRUE(Draw::Inside(room.Floor, x, y)) << room.Name;
    }
    for (std::size_t a = 0; a < arena.Rooms.size(); ++a)
        for (std::size_t b = 0; b < arena.Rooms.size(); ++b)
            if (a != b)
                for (auto const& [x, y] : arena.Rooms[a].Floor)
                {
                    // A corner of one strictly inside another (pulled a little toward its own centre).
                    float const px = x + 0.05f * (arena.Rooms[a].Centre.first - x);
                    float const py = y + 0.05f * (arena.Rooms[a].Centre.second - y);
                    EXPECT_FALSE(Draw::Inside(arena.Rooms[b].Floor, px, py) && std::fabs(arena.Rooms[a].FloorZ
                        - arena.Rooms[b].FloorZ) < 4.0f) << arena.Rooms[a].Name << " overlaps " << arena.Rooms[b].Name;
                }
}

// The object pool is the hard-coded five, each a real entry with a model height, uniquely named.
TEST(SeekEncounterTest, TheObjectPoolIsTheHardCodedList)
{
    Cu::ArenaDefinition const& arena = SeekArena();
    std::map<std::string, uint32> const expected = { { "chest", 144111 }, { "crate", 179972 }, { "barrel", 179967 },
        { "sack", 180660 }, { "strongbox", 2039 } };
    ASSERT_EQ(arena.Objects.size(), expected.size());
    for (Cu::SeekObject const& object : arena.Objects)
    {
        ASSERT_TRUE(expected.contains(object.Kind)) << object.Kind;
        EXPECT_EQ(object.Entry, expected.at(object.Kind)) << object.Kind;
        EXPECT_GT(object.Height, 0.5f) << object.Kind;
        EXPECT_LT(object.Height, 2.0f) << object.Kind;
    }
    EXPECT_EQ(Cu::SeekEncounter::ObjectNames(arena).size(), expected.size());
    EXPECT_EQ(Cu::SeekEncounter::RoomNames(arena).size(), arena.Rooms.size());
}

// The room ladder: at the first rung (shaping 1) the nearest room is the likeliest and the deepest the least; half way
// (shaping 0.5) every room is as likely; faded (shaping 0) the other way round. Every room keeps the floor's weight.
TEST(SeekEncounterTest, RoomWeightsMoveWithTheFadesRungs)
{
    Cu::ArenaDefinition const& arena = SeekArena();
    std::vector<float> const depth = Draw::Depths(arena.Rooms);
    std::size_t const nearest = std::min_element(depth.begin(), depth.end()) - depth.begin();
    std::size_t const deepest = std::max_element(depth.begin(), depth.end()) - depth.begin();
    EXPECT_EQ(arena.Rooms[deepest].Name, "west_end_2_back");
    EXPECT_FLOAT_EQ(depth[deepest], 1.0f);
    EXPECT_FLOAT_EQ(depth[nearest], 0.0f);

    // The fade's rungs [1, 0.5, 0.25, 0] read as ladders 0, 0.5, 0.75 and 1.
    for (float shaping : { 1.0f, 0.5f, 0.25f, 0.0f })
    {
        std::vector<float> const weights = Draw::Weights(arena.Rooms, 1.0f - shaping);
        for (float weight : weights)
            EXPECT_GE(weight, Draw::WEIGHT_FLOOR - 1e-6f);
        if (shaping == 1.0f)
        {
            EXPECT_FLOAT_EQ(weights[nearest], 1.0f + Draw::WEIGHT_FLOOR);
            EXPECT_FLOAT_EQ(weights[deepest], Draw::WEIGHT_FLOOR);
        }
        else if (shaping == 0.5f)
            for (float weight : weights)
                EXPECT_FLOAT_EQ(weight, 0.5f + Draw::WEIGHT_FLOOR);
        else if (shaping == 0.0f)
        {
            EXPECT_FLOAT_EQ(weights[nearest], Draw::WEIGHT_FLOOR);
            EXPECT_FLOAT_EQ(weights[deepest], 1.0f + Draw::WEIGHT_FLOOR);
        }
    }

    // Drawn with a fixed sequence (SeedUniform, seed 7): at the first rung the nearest third is drawn far more often
    // than the deepest, faded the other way round, and every room comes up at both ends of the ladder.
    for (float ladder : { 0.0f, 1.0f })
    {
        std::vector<float> const weights = Draw::Weights(arena.Rooms, ladder);
        std::vector<uint32> tiers(Draw::TIERS, 0);
        std::set<uint32> seen;
        for (uint32 draw = 0; draw < 20000; ++draw)
        {
            uint32 const room = Draw::Pick(weights, Draw::SeedUniform(7, draw));
            seen.insert(room);
            ++tiers[Draw::Tier(depth[room])];
        }
        EXPECT_EQ(seen.size(), arena.Rooms.size());
        if (ladder == 0.0f)
            EXPECT_GT(tiers[0], 3 * tiers[Draw::TIERS - 1]);
        else
            EXPECT_GT(tiers[Draw::TIERS - 1], 3 * tiers[0]);
    }
}

// The object is drawn uniformly (training), and an evaluation meets every room, and every object, in turn.
TEST(SeekEncounterTest, ObjectsAreUniformAndEvaluationsCoverEveryRoom)
{
    std::vector<float> const even(5, 1.0f);
    std::vector<uint32> counts(5, 0);
    for (uint32 draw = 0; draw < 50000; ++draw)
        ++counts[Draw::Pick(even, Draw::SeedUniform(11, draw))];
    for (uint32 count : counts)
        EXPECT_NEAR(float(count) / 50000.0f, 0.2f, 0.01f);

    // One pass of the sweep is every (room, object) pair exactly once; a second pass repeats it.
    EXPECT_EQ(Draw::SweepLength(39, 5), 195u);
    std::map<std::pair<uint32, uint32>, uint32> met;
    for (uint32 seed = 0; seed < 2 * 195; ++seed)
        ++met[Draw::EvaluationPick(seed, 39, 5)];
    EXPECT_EQ(met.size(), 195u);
    for (auto const& [pair, times] : met)
    {
        EXPECT_LT(pair.first, 39u);
        EXPECT_LT(pair.second, 5u);
        EXPECT_EQ(times, 2u);
    }
    EXPECT_EQ(Draw::EvaluationPick(0, 39, 5), (std::pair<uint32, uint32>{ 0u, 0u }));
    EXPECT_EQ(Draw::EvaluationPick(7, 39, 5), (std::pair<uint32, uint32>{ 1u, 2u }));
    EXPECT_EQ(Draw::EvaluationPick(195, 39, 5), (std::pair<uint32, uint32>{ 0u, 0u }));
    // A class cast every 30 seeds (seed mod castings) meets rooms at every depth, not one end of the table.
    std::set<uint32> tiers;
    for (uint32 seed = 7; seed < 195; seed += 30)
        tiers.insert(Draw::Tier(float(Draw::EvaluationPick(seed, 39, 5).first) / 38.0f));
    EXPECT_EQ(tiers.size(), std::size_t(Draw::TIERS));
}

// A placement point is inside the room's floor for any three numbers; the corners and the middle of the range land
// where the fan says.
TEST(SeekEncounterTest, PlacementPointsAreOnTheFloor)
{
    for (Cu::SeekRoom const& room : SeekArena().Rooms)
        for (uint32 draw = 0; draw < 200; ++draw)
        {
            auto const [x, y] = Draw::PointIn(room.Floor, Draw::SeedUniform(3, 3 * draw),
                Draw::SeedUniform(3, 3 * draw + 1), Draw::SeedUniform(3, 3 * draw + 2));
            EXPECT_TRUE(Draw::Inside(room.Floor, x, y)) << room.Name << " " << x << " " << y;
            EXPECT_EQ(Draw::RoomAt(SeekArena().Rooms, x, y, room.FloorZ), &room - SeekArena().Rooms.data());
        }
    // The hallway is no room.
    EXPECT_EQ(Draw::RoomAt(SeekArena().Rooms, 100.0f, 0.0f, -25.6f), -1);
    // A square's fan: (0, 0, 0) is its first corner.
    std::vector<std::pair<float, float>> const square = { { 0.0f, 0.0f }, { 2.0f, 0.0f }, { 2.0f, 2.0f },
        { 0.0f, 2.0f } };
    EXPECT_EQ(Draw::PointIn(square, 0.0f, 0.0f, 0.0f), (std::pair<float, float>{ 0.0f, 0.0f }));
    EXPECT_FLOAT_EQ(Draw::Area(square), 4.0f);
}

// The aids are Shaping (the fade takes them away); the stage's objective, Arrive, is an Outcome; the clock a Cost.
TEST(SeekEncounterTest, TheAidsAreShapingAndFindingIsTheOutcome)
{
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Sighting), Cu::RewardCategory::Shaping);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::NewGround), Cu::RewardCategory::Shaping);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Arrive), Cu::RewardCategory::Outcome);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::StepCost), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::Sighting), "sighting");
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::NewGround), "new_ground");
}
