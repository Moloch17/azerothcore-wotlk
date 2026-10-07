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

#include "CurriculumTuning.h"
#include "MentalMap.h"
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
// arena in the Stockades spawning on M1's hallway points (the entrance first), with a held-out sweep beside it, 300 s
// at most and a 3 yd radius.
TEST(SeekEncounterTest, TheStageIsDefined)
{
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;
    Cu::StageDefinition const* stage = Cu::FindStage("move2_seek");
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(stage->Extends, "move1_controls");
    // The camera brings its entity list; the mental map follows them (perception-goals REDESIGN §3).
    EXPECT_EQ(stage->Blocks, (std::vector<Cu::BlockId>{ Cu::BlockId::Core, Cu::BlockId::Move, Cu::BlockId::Vision,
        Cu::BlockId::Entities, Cu::BlockId::Map, Cu::BlockId::Goal }));
    EXPECT_EQ(stage->Level, 1);
    ASSERT_EQ(stage->Arenas.size(), 2u);
    Cu::ArenaDefinition const& arena = stage->Arenas[0];
    EXPECT_EQ(arena.Against, Cu::Opposition::Seek);
    EXPECT_FALSE(arena.EvalOnly);
    EXPECT_EQ(arena.EpisodeSeconds, 300u);
    EXPECT_FLOAT_EQ(arena.SeekRadius, 3.0f);
    EXPECT_EQ(arena.MapId, 34u);
    ASSERT_GT(arena.SpawnPoints.size(), 50u);
    EXPECT_NEAR(arena.SpawnPoints[0].GetPositionX(), 54.23f, 0.01f);
    // The held-out sweep: the same rooms and objects, only ever played when an evaluation pins it.
    Cu::ArenaDefinition const& sweep = stage->Arenas[1];
    EXPECT_EQ(sweep.Name, "sweep");
    EXPECT_TRUE(sweep.EvalOnly);
    EXPECT_EQ(sweep.Against, Cu::Opposition::Seek);
    EXPECT_EQ(sweep.Rooms.size(), arena.Rooms.size());
    EXPECT_EQ(sweep.EpisodeSeconds, 300u);
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

// The placement ladder (REDESIGN §2): the hallway, a front cell's doorway, a front cell, deep. The front cells are the
// thirteen opening onto the hallway (no hub); the deep rung draws the other 26. Each rung keeps a tenth of the one
// below; the first has none below.
TEST(SeekEncounterTest, TheLadderPlacesByRung)
{
    Cu::ArenaDefinition const& arena = SeekArena();
    std::vector<uint32> const front = Draw::RungRooms(arena.Rooms, Draw::Rung::Room);
    EXPECT_EQ(front.size(), 13u);
    EXPECT_EQ(Draw::RungRooms(arena.Rooms, Draw::Rung::Doorway), front);
    std::vector<uint32> const deep = Draw::RungRooms(arena.Rooms, Draw::Rung::Deep);
    EXPECT_EQ(deep.size(), 26u);
    EXPECT_TRUE(Draw::RungRooms(arena.Rooms, Draw::Rung::Hallway).empty());
    for (uint32 room : front)
    {
        std::string const& name = arena.Rooms[room].Name;
        EXPECT_EQ(name.find("_back"), std::string::npos) << name;
        EXPECT_EQ(name.find("hub"), std::string::npos) << name;
        EXPECT_EQ(name.find("_end_"), std::string::npos) << name;
    }
    for (uint32 room : deep)
        EXPECT_FALSE(arena.Rooms[room].Front);
    // The deep rung's rooms are the deeper ones: all of the deepest third is deep, and none of the deep is nearer
    // than the first front cell.
    std::vector<float> const depth = Draw::Depths(arena.Rooms);
    for (uint32 room = 0; room < arena.Rooms.size(); ++room)
        if (Draw::Tier(depth[room]) + 1 == Draw::TIERS)
            EXPECT_FALSE(arena.Rooms[room].Front) << arena.Rooms[room].Name;

    // The carry-over: below `carry`, the rung below; the hallway has none.
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Deep, 0.05f, 0.1f), Draw::Rung::Room);
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Deep, 0.5f, 0.1f), Draw::Rung::Deep);
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Doorway, 0.0f, 0.1f), Draw::Rung::Hallway);
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Hallway, 0.0f, 0.1f), Draw::Rung::Hallway);
    uint32 carried = 0;
    for (uint32 draw = 0; draw < 20000; ++draw)
        carried += Draw::PlacedRung(Draw::Rung::Room, Draw::SeedUniform(5, draw), 0.1f) == Draw::Rung::Doorway;
    EXPECT_NEAR(float(carried) / 20000.0f, 0.1f, 0.01f);

    // The doorway: just inside the opening, on the line toward the room's centre, within the spread of it.
    for (uint32 room : front)
    {
        Cu::SeekRoom const& cell = arena.Rooms[room];
        for (float u : { 0.0f, 0.5f, 1.0f })
            for (float v : { 0.0f, 0.5f, 1.0f })
            {
                auto const [x, y] = Draw::DoorwaySpot(cell, 2.0f, 1.5f, 1.0f, u, v);
                float const fromOpening = std::hypot(x - cell.Opening.first, y - cell.Opening.second);
                float const toCentre = std::hypot(cell.Centre.first - cell.Opening.first,
                    cell.Centre.second - cell.Opening.second);
                EXPECT_GE(fromOpening, std::min(toCentre, 2.0f) - 1e-3f) << cell.Name;
                EXPECT_LE(fromOpening, std::hypot(3.5f, 1.0f) + 1e-3f) << cell.Name;
                EXPECT_LT(std::hypot(x - cell.Centre.first, y - cell.Centre.second), toCentre + 1.0f) << cell.Name;
            }
    }
    // A hallway object's room is the one whose opening it is nearest.
    Cu::SeekRoom const& first = arena.Rooms[front[0]];
    EXPECT_EQ(Draw::NearestOpening(arena.Rooms, first.Opening.first, first.Opening.second), int32(front[0]));
}

// The evaluation at a rung (amendment 9): 78 episodes go round the rung's rooms, the object types cycled with them;
// at the top rung every one of the 39 rooms twice, with two different objects; at the room rung the front cells.
TEST(SeekEncounterTest, TheEvaluationPlaysTheTrainingRung)
{
    Cu::ArenaDefinition const& arena = SeekArena();
    std::vector<uint32> const all = Draw::EvaluationRooms(arena.Rooms, Draw::Rung::Deep);
    ASSERT_EQ(all.size(), 39u);
    std::map<uint32, std::set<uint32>> met;
    for (uint32 seed = 0; seed < 78; ++seed)
    {
        auto const [room, object] = Draw::RungEvaluationPick(seed, all, 5);
        EXPECT_TRUE(met[room].insert(object).second) << "room " << room << " met object " << object << " twice";
    }
    EXPECT_EQ(met.size(), 39u);
    for (auto const& [room, objects] : met)
        EXPECT_EQ(objects.size(), 2u);
    std::vector<uint32> const front = Draw::EvaluationRooms(arena.Rooms, Draw::Rung::Room);
    EXPECT_EQ(front, Draw::RungRooms(arena.Rooms, Draw::Rung::Room));
    for (uint32 seed = 0; seed < 78; ++seed)
        EXPECT_TRUE(arena.Rooms[Draw::RungEvaluationPick(seed, front, 5).first].Front);
}

// Episodes by rung (REDESIGN §2): 90, 120, 200 and 300 s with the conf's defaults; the arena's own 300 s is the longest,
// which the SPEC announces.
TEST(SeekEncounterTest, EpisodeLengthByRung)
{
    Cu::CurriculumTuning::SeekTuning const tuning;
    std::array<uint32, Draw::RUNGS> const seconds = { tuning.RungSeconds0, tuning.RungSeconds1, tuning.RungSeconds2,
        tuning.RungSeconds3 };
    EXPECT_EQ(Draw::RungSeconds(Draw::Rung::Hallway, seconds), 90u);
    EXPECT_EQ(Draw::RungSeconds(Draw::Rung::Doorway, seconds), 120u);
    EXPECT_EQ(Draw::RungSeconds(Draw::Rung::Room, seconds), 200u);
    EXPECT_EQ(Draw::RungSeconds(Draw::Rung::Deep, seconds), 300u);
    EXPECT_EQ(SeekArena().EpisodeSeconds, 300u);
    // Wall and Stuck: small next to Arrive, as M1's.
    EXPECT_LE(tuning.Wall * 300.0f, tuning.Arrive * 2.0f);
    EXPECT_LE(tuning.Stuck * 300.0f, tuning.Arrive * 2.0f);
    EXPECT_FLOAT_EQ(tuning.CarryShare, 0.1f);
}

// "Looked into a room" (REDESIGN §2, amendment 6): paid once a room an episode, on the first frame whose cast rays put
// RoomSeenRays or more on its floor, by the episode's own list -- a mental map kept from before, which already knows
// the room, changes nothing: the next episode pays it again.
TEST(SeekEncounterTest, LookingIntoARoomIsPaidPerEpisode)
{
    Cu::ArenaDefinition const& arena = SeekArena();
    uint32 const index = Draw::RungRooms(arena.Rooms, Draw::Rung::Room).front();
    Cu::SeekRoom const& room = arena.Rooms[index];
    // A frame from the opening: four rays onto the room's floor near its centre, one onto the hallway, one sky.
    Animus::Vision::FrameHits frame;
    frame.Camera = { room.Opening.first, room.Opening.second, room.FloorZ + 3.0f };
    auto const ray = [&](float x, float y, float z, Animus::Vision::Class what, float normal)
    {
        Animus::Vision::Vec3 const d{ x - frame.Camera.X, y - frame.Camera.Y, z - frame.Camera.Z };
        float const length = Animus::Vision::Length(d);
        frame.Rays.push_back({ d * (1.0f / length), length, z, normal, what });
    };
    for (float offset : { -0.5f, 0.0f, 0.5f, 1.0f })
        ray(room.Centre.first + offset, room.Centre.second, room.FloorZ, Animus::Vision::Class::Model, 1.0f);
    ray(100.0f, 0.0f, -25.6f, Animus::Vision::Class::Model, 1.0f);
    frame.Rays.push_back({ { 0.0f, 0.0f, 1.0f }, 500.0f, 0.0f, 0.0f, Animus::Vision::Class::Sky });
    EXPECT_EQ(Draw::FloorRays(arena.Rooms, frame)[index], 4u);

    std::vector<bool> looked;
    EXPECT_EQ(Draw::NewlyLooked(arena.Rooms, frame, looked, 3), std::vector<uint32>{ index });
    EXPECT_TRUE(Draw::NewlyLooked(arena.Rooms, frame, looked, 3).empty());      // once an episode
    // Too few rays on it: not looked into.
    std::vector<bool> fresh;
    EXPECT_TRUE(Draw::NewlyLooked(arena.Rooms, frame, fresh, 5).empty());

    // The seat's mental map, kept across the reset, knows the room's floor from that frame...
    Animus::Vision::MentalMap memory;
    memory.WriteFrame(frame, room.FloorZ, 2.0f);
    Animus::Vision::MapCell const* known = memory.Find(room.Centre.first, room.Centre.second);
    ASSERT_NE(known, nullptr);
    EXPECT_TRUE(Animus::Vision::Known(*known));
    memory.Advance(120.0f);
    // ... and the next episode, whose list starts empty, is paid for the same look all the same.
    std::vector<bool> nextEpisode;
    EXPECT_EQ(Draw::NewlyLooked(arena.Rooms, frame, nextEpisode, 3), std::vector<uint32>{ index });
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
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::RoomSeen), Cu::RewardCategory::Shaping);
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::RoomSeen), "room_seen");
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Arrive), Cu::RewardCategory::Outcome);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::StepCost), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::Sighting), "sighting");
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::NewGround), "new_ground");
}
