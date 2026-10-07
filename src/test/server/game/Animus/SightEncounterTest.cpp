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

#include "Block.h"
#include "Camera.h"
#include "CompassBlock.h"
#include "CurriculumTuning.h"
#include "Layout.h"
#include "MarkerEncounter.h"
#include "RewardLedger.h"
#include "SeatView.h"
#include "SeekDraw.h"
#include "SightDraw.h"
#include "SightEncounter.h"
#include "StageDefinition.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

/// **M1 controls, redesigned** (perception-goals REDESIGN §1; SightEncounter, SightDraw): the stage's definition, the
/// compass's withholding by rung, the evaluation's pairs, the placement in sight (and round a corner) on a fake world
/// of boxes, and the fixed-price wall and stuck costs. The hallway table and the evaluation pairs against the map's
/// own data are StockadeHallwaysDataTest's.
namespace
{
    namespace Cu = Animus::Curriculum;
    namespace Sd = Animus::Curriculum::SightDraw;
    namespace Vi = Animus::Vision;

    Cu::ArenaDefinition const& SightArena()
    {
        Cu::StageDefinition const* stage = Cu::FindStage("move1_controls");
        EXPECT_NE(stage, nullptr);
        return stage->Arenas.at(0);
    }

    struct Box
    {
        float X0, X1, Y0, Y1, Z0, Z1;
    };

    /// The nearest face of any box along the segment (slab method), as SeekFlagTest's.
    Vi::SurfaceHit Nearest(std::vector<Box> const& boxes, Vi::Vec3 from, Vi::Vec3 to)
    {
        Vi::SurfaceHit best;
        Vi::Vec3 const d = to - from;
        float const length = Vi::Length(d);
        for (Box const& box : boxes)
        {
            float lo = 0.0f;
            float hi = 1.0f;
            float const o[3] = { from.X, from.Y, from.Z };
            float const v[3] = { d.X, d.Y, d.Z };
            float const mins[3] = { box.X0, box.Y0, box.Z0 };
            float const maxs[3] = { box.X1, box.Y1, box.Z1 };
            bool miss = false;
            for (int axis = 0; axis < 3 && !miss; ++axis)
            {
                if (std::fabs(v[axis]) < 1e-9f)
                {
                    miss = o[axis] < mins[axis] || o[axis] > maxs[axis];
                    continue;
                }
                float t0 = (mins[axis] - o[axis]) / v[axis];
                float t1 = (maxs[axis] - o[axis]) / v[axis];
                if (t0 > t1)
                    std::swap(t0, t1);
                lo = std::max(lo, t0);
                hi = std::min(hi, t1);
                miss = lo > hi;
            }
            if (miss || (best.Distance >= 0.0f && lo * length >= best.Distance))
                continue;
            best.Distance = lo * length;
        }
        return best;
    }

    /// Loaded grids with no terrain heights (an instance), the boxes as the trees.
    class Room : public Vi::VisionWorld
    {
    public:
        std::vector<Box> Static;
        std::vector<Box> Dynamic;

        Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Static, from, to); }
        Vi::SurfaceHit DynamicHit(Vi::Vec3 from, Vi::Vec3 to) const override { return Nearest(Dynamic, from, to); }
        Vi::LiquidHit ModelLiquid(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            Vi::TerrainTile tile;
            tile.Loaded = tileX >= 31 && tileX <= 33 && tileY >= 31 && tileY <= 33;
            return tile;
        }
        Vi::TerrainCell Cell(int32_t, int32_t, int32_t, int32_t, bool) const override { return {}; }
        Animus::Movement::Liquid LiquidAt(float, float, float) const override { return {}; }
        float FloorBelow(float, float, float z, float search) const override
        {
            return z >= 0.0f && z - search <= 0.0f ? 0.0f : Animus::Movement::INVALID_FLOOR;
        }
    };

    constexpr float X = -200.0f;    // inside the loaded grids, as VisionTest's scenes are
    constexpr float Y = -200.0f;

    /// A floor, and a wall along y at x + 4 from y - 1 to y + 30, five yards high: the seat at (X, Y) cannot see past
    /// it to its +x, +y side, and a point four yards to its -y side can see round its end.
    Room Corner()
    {
        Room room;
        room.Static.push_back({ X - 60.0f, X + 60.0f, Y - 60.0f, Y + 60.0f, -1.0f, 0.0f });
        room.Static.push_back({ X + 4.0f, X + 4.5f, Y - 1.0f, Y + 30.0f, 0.0f, 5.0f });
        return room;
    }

    /// The table: 0 the spawn, 1 a stepping point 4.5 yd off it (in its sight), 2 behind the wall (seen only from 1),
    /// 3 in plain sight 15 yd behind the spawn.
    std::vector<Position> Points()
    {
        return { { X, Y, 0.0f, 0.0f }, { X + 2.0f, Y - 4.0f, 0.0f, 0.0f }, { X + 20.0f, Y + 10.0f, 0.0f, 0.0f },
            { X - 15.0f, Y, 0.0f, 0.0f } };
    }

    /// A uniform sequence that walks its numbers in order, for repeatable draws.
    struct Sequence
    {
        std::vector<float> Numbers;
        std::size_t At = 0;
        float operator()() { return Numbers.empty() ? 0.0f : Numbers[At++ % Numbers.size()]; }
    };
}

// M1 is a valid stage of one sight arena: the compass, the camera and the move block; the hallway table as its spawns
// (the entrance first), M2's object pool, 60 s episodes in the Stockades, and its evaluation pairs.
TEST(SightEncounterTest, TheStageIsDefined)
{
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;
    Cu::StageDefinition const* stage = Cu::FindStage("move1_controls");
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(stage->Blocks, (std::vector<Cu::BlockId>{ Cu::BlockId::Core, Cu::BlockId::Move, Cu::BlockId::Compass,
        Cu::BlockId::Vision, Cu::BlockId::Entities, Cu::BlockId::Goal }));
    ASSERT_EQ(stage->Arenas.size(), 1u);
    Cu::ArenaDefinition const& arena = stage->Arenas[0];
    EXPECT_EQ(arena.Against, Cu::Opposition::Sight);
    EXPECT_EQ(arena.EpisodeSeconds, 60u);
    EXPECT_EQ(arena.MapId, 34u);
    EXPECT_FALSE(arena.Objective);
    ASSERT_GE(arena.SpawnPoints.size(), 150u);
    EXPECT_NEAR(arena.SpawnPoints[0].GetPositionX(), 54.23f, 0.01f);
    EXPECT_NEAR(arena.SpawnPoints[0].GetPositionY(), 0.28f, 0.01f);

    // The object pool is M2's, entry for entry, so the class and the flag look the same.
    Cu::StageDefinition const* seek = Cu::FindStage("move2_seek");
    ASSERT_NE(seek, nullptr);
    std::vector<Cu::SeekObject> const& pool = seek->Arenas.at(0).Objects;
    ASSERT_EQ(arena.Objects.size(), pool.size());
    for (std::size_t i = 0; i < pool.size(); ++i)
    {
        EXPECT_EQ(arena.Objects[i].Entry, pool[i].Entry);
        EXPECT_FLOAT_EQ(arena.Objects[i].Radius, pool[i].Radius);
    }
    EXPECT_EQ(Cu::SightEncounter::ObjectNames(arena).size(), pool.size());
}

// The hallway table is one table of distinct points, all in the Stockades' hallways (x 54 to 172, y within the wings'
// reach), on the hallways' floors (the entrance ramp at -18, the hallway at -25.6, the wings at -34).
TEST(SightEncounterTest, TheHallwayTableIsWellFormed)
{
    std::vector<Position> const& points = SightArena().SpawnPoints;
    std::set<std::pair<int32, int32>> seen;
    for (Position const& point : points)
    {
        EXPECT_GE(point.GetPositionX(), 54.0f);
        EXPECT_LE(point.GetPositionX(), 175.0f);
        EXPECT_LE(std::fabs(point.GetPositionY()), 95.0f);
        EXPECT_GE(point.GetPositionZ(), -36.0f);
        EXPECT_LE(point.GetPositionZ(), -18.0f);
        EXPECT_TRUE(seen.emplace(int32(std::lround(point.GetPositionX() * 10.0f)),
            int32(std::lround(point.GetPositionY() * 10.0f))).second)
            << point.GetPositionX() << " " << point.GetPositionY();
    }
}

// The evaluation pairs: two different points each, 10 to 120 yd apart, and some of them round a corner.
TEST(SightEncounterTest, TheEvaluationPairsAreFixedAndInTheBand)
{
    Cu::ArenaDefinition const& arena = SightArena();
    ASSERT_GE(arena.SightPairs.size(), 16u);
    uint32 corners = 0;
    std::set<uint32> spawns;
    for (Cu::SightPair const& pair : arena.SightPairs)
    {
        ASSERT_LT(pair.Spawn, arena.SpawnPoints.size());
        ASSERT_LT(pair.Object, arena.SpawnPoints.size());
        float const apart = arena.SpawnPoints[pair.Spawn].GetExactDist2d(&arena.SpawnPoints[pair.Object]);
        EXPECT_GE(apart, 10.0f);
        EXPECT_LE(apart, 120.0f);
        corners += pair.Corner ? 1 : 0;
        spawns.insert(pair.Spawn);
    }
    EXPECT_GT(corners, 0u);
    EXPECT_LT(corners, arena.SightPairs.size() / 2);
    // Over the hallways, not from one place: the entrance is one of the spawns.
    EXPECT_GE(spawns.size(), arena.SightPairs.size() * 3 / 4);
    EXPECT_TRUE(spawns.contains(0u));
}

// Withholding by rung: the fade's four scales are the four rungs, the chances 0, 0.25, 0.6 and 0.9 there (the tuning's
// defaults), linear between, and 0 and 1 are never left.
TEST(SightEncounterTest, TheCompassIsWithheldMoreOftenEachRung)
{
    EXPECT_EQ(Sd::Rung(1.0f), 0u);
    EXPECT_EQ(Sd::Rung(0.5f), 1u);
    EXPECT_EQ(Sd::Rung(0.25f), 2u);
    EXPECT_EQ(Sd::Rung(0.0f), 3u);
    Cu::CurriculumTuning::ControlsTuning const tuning;
    std::array<float, Sd::RUNGS> const chances = { tuning.Withhold0, tuning.Withhold1, tuning.Withhold2,
        tuning.Withhold3 };
    EXPECT_FLOAT_EQ(Sd::WithholdChance(1.0f, chances), 0.0f);
    EXPECT_FLOAT_EQ(Sd::WithholdChance(0.5f, chances), 0.25f);
    EXPECT_FLOAT_EQ(Sd::WithholdChance(0.25f, chances), 0.6f);
    EXPECT_FLOAT_EQ(Sd::WithholdChance(0.0f, chances), 0.9f);
    EXPECT_NEAR(Sd::WithholdChance(0.75f, chances), 0.125f, 1e-6f);
    EXPECT_NEAR(Sd::WithholdChance(0.375f, chances), 0.425f, 1e-6f);
    EXPECT_FLOAT_EQ(Sd::WithholdChance(-1.0f, chances), 0.9f);
    EXPECT_FLOAT_EQ(Sd::WithholdChance(2.0f, chances), 0.0f);
    EXPECT_FLOAT_EQ(Sd::WithholdChance(0.0f, { 0.0f, 0.5f, 1.0f, 3.0f }), 1.0f);

    // And the draw follows the chance: at the 0.6 rung about 60% of a thousand episodes withhold it.
    uint32 withheld = 0;
    for (uint32 i = 0; i < 4000; ++i)
        withheld += Animus::Curriculum::SeekDraw::SeedUniform(i, 7) < Sd::WithholdChance(0.25f, chances) ? 1 : 0;
    EXPECT_NEAR(float(withheld) / 4000.0f, 0.6f, 0.03f);
}

// Withheld, the compass reads as absent: its presence and every value 0, whatever the objective and the detour --
// while the view still has the objective, for the camera's flag. Shown, it reads as before.
TEST(SightEncounterTest, AWithheldCompassReadsAsAbsent)
{
    Cu::Block const& compass = Cu::GetBlock(Cu::BlockId::Compass);
    Cu::SeatView view;
    view.HasObjective = true;
    view.Objective = Position(10.0f, 0.0f, 0.0f);
    view.Detour = 2.0f;
    view.CompassWithheld = true;
    std::vector<float> obs(Cu::CompassBlock::OBS_COUNT, 0.5f);
    compass.Observe(view, obs.data(), nullptr);
    for (float value : obs)
        EXPECT_FLOAT_EQ(value, 0.0f);
    EXPECT_TRUE(view.HasObjective);
    // No action: the block has none to mask, withheld or not.
    Cu::Layout layout;
    EXPECT_EQ(compass.Size(layout).Actions, 0u);

    view.CompassWithheld = false;
    std::fill(obs.begin(), obs.end(), 0.0f);
    compass.Observe(view, obs.data(), nullptr);
    EXPECT_FLOAT_EQ(obs[Cu::CompassBlock::OBS_DETOUR], 0.5f);
}

// The evaluation: every casting meets each of its pairs with the compass and without, in consecutive rounds, and over
// an evaluation every pair is played both ways -- also with an even casting count, where a compass bit on the seed's
// parity would give each casting one state only.
TEST(SightEncounterTest, EveryEvaluationPairIsPlayedWithAndWithoutTheCompass)
{
    constexpr uint32 PAIRS = 32;
    for (uint32 castings : { 1u, 2u, 18u, 20u, 33u })
    {
        for (uint32 seed = 0; seed < 600; ++seed)
        {
            Sd::EvaluationEpisode const first = Sd::EvaluationPick(seed, castings, PAIRS);
            ASSERT_LT(first.Pair, PAIRS);
            uint32 const round = seed / castings;
            if (round % 2)
                continue;
            Sd::EvaluationEpisode const second = Sd::EvaluationPick(seed + castings, castings, PAIRS);
            EXPECT_FALSE(first.Withheld);
            EXPECT_TRUE(second.Withheld);
            EXPECT_EQ(first.Pair, second.Pair);
        }
        // Each casting sees both states.
        std::map<uint32, std::set<bool>> byCasting;
        for (uint32 seed = 0; seed < 512; ++seed)
            byCasting[seed % castings].insert(Sd::EvaluationPick(seed, castings, PAIRS).Withheld);
        for (auto const& [casting, states] : byCasting)
            EXPECT_EQ(states.size(), 2u) << castings << " castings, casting " << casting;
    }
    // A 512-episode evaluation of 18 castings (28 whole rounds) meets every pair both ways, as often as any other
    // within one.
    for (uint32 castings : { 18u, 20u, 7u })
    {
        uint32 const whole = 512 / (2 * castings) * (2 * castings);
        std::map<uint32, std::map<bool, uint32>> byPair;
        for (uint32 seed = 0; seed < whole; ++seed)
        {
            Sd::EvaluationEpisode const pick = Sd::EvaluationPick(seed, castings, PAIRS);
            ++byPair[pick.Pair][pick.Withheld];
        }
        EXPECT_EQ(byPair.size(), PAIRS) << castings;
        uint32 least = whole, most = 0;
        for (auto const& [pair, states] : byPair)
        {
            ASSERT_EQ(states.size(), 2u) << "pair " << pair;
            EXPECT_EQ(states.at(false), states.at(true)) << "pair " << pair;
            least = std::min(least, states.at(false));
            most = std::max(most, states.at(false));
        }
        EXPECT_LE(most - least, 1u) << castings;
    }
}

// Line of sight, the camera's way: a ray from the eye to the centre meets nothing before the object's own radius;
// the object standing there does not hide itself; a wall does.
TEST(SightEncounterTest, InSightIsOneCameraRayFromTheEye)
{
    Room room = Corner();
    Vi::Vec3 const eye{ X, Y, 1.8f };
    EXPECT_TRUE(Sd::InSight(eye, { X - 15.0f, Y, 0.5f }, 0.5f, room));
    EXPECT_FALSE(Sd::InSight(eye, { X + 20.0f, Y + 10.0f, 0.5f }, 0.5f, room));
    // The object itself (a crate's box round the centre, in the dynamic tree): still in sight.
    room.Dynamic.push_back({ X - 15.6f, X - 14.4f, Y - 0.6f, Y + 0.6f, 0.0f, 1.2f });
    EXPECT_TRUE(Sd::InSight(eye, { X - 15.0f, Y, 0.6f }, 0.66f, room));
    // A crate in front of it hides it.
    room.Dynamic.push_back({ X - 8.6f, X - 7.4f, Y - 0.6f, Y + 0.6f, 0.0f, 2.5f });
    EXPECT_FALSE(Sd::InSight(eye, { X - 15.0f, Y, 0.6f }, 0.66f, room));
}

// The placement: in sight of the spawn, and never behind the wall; round a corner, the point behind the wall that a
// stepping point a few yards off sees -- and with no such stepping point, a fallback in sight that says so.
TEST(SightEncounterTest, ThePlacementIsInSightOrJustRoundACorner)
{
    Room const room = Corner();
    std::vector<Position> const points = Points();
    Sd::Viewing viewing;
    viewing.Nearest = 10.0f;
    viewing.Furthest = 120.0f;
    for (float first : { 0.0f, 0.3f, 0.6f, 0.9f })
    {
        Sequence uniform{ { first, 0.5f, 0.1f } };
        Sd::Placement const plain = Sd::Place(points, points[0], false, viewing, room, uniform);
        EXPECT_EQ(plain.Point, 3);
        EXPECT_FALSE(plain.Corner);

        Sequence again{ { first, 0.5f, 0.1f } };
        Sd::Placement const corner = Sd::Place(points, points[0], true, viewing, room, again);
        EXPECT_EQ(corner.Point, 2);
        EXPECT_TRUE(corner.Corner);
        EXPECT_EQ(corner.Step, 1);
    }

    // Without the stepping point the corner cannot be had: the point in sight, and Corner false.
    std::vector<Position> noStep = { points[0], points[2], points[3] };
    Sequence uniform{ { 0.2f } };
    Sd::Placement const fallback = Sd::Place(noStep, noStep[0], true, viewing, room, uniform);
    EXPECT_EQ(fallback.Point, 2);
    EXPECT_FALSE(fallback.Corner);

    // Out of the band: nothing.
    viewing.Nearest = 30.0f;
    Sequence none{ { 0.2f } };
    EXPECT_EQ(Sd::Place(points, points[0], false, viewing, room, none).Point, -1);
}

// Wall and Stuck at their fixed price: with the cost ladder at x0, Add pays a noise price nothing and AddFixed pays it
// in full, in the step, the episode's column and the score alike; a Shaping term through AddFixed keeps its fade.
TEST(SightEncounterTest, WallAndStuckArePaidOffTheCostLadder)
{
    Cu::RewardLedger ledger;
    ledger.SetCosts(0.0f);
    ledger.SetShaping(0.5f);
    EXPECT_FLOAT_EQ(ledger.Add(Cu::RewardTerm::Wall, -0.02f), 0.0f);
    EXPECT_FLOAT_EQ(ledger.AddFixed(Cu::RewardTerm::Wall, -0.02f), -0.02f);
    EXPECT_FLOAT_EQ(ledger.AddFixed(Cu::RewardTerm::Stuck, -0.03f), -0.03f);
    EXPECT_FLOAT_EQ(ledger.Episode(Cu::RewardTerm::Wall), -0.02f);
    EXPECT_FLOAT_EQ(ledger.Episode(Cu::RewardTerm::Stuck), -0.03f);
    EXPECT_FLOAT_EQ(ledger.AddFixed(Cu::RewardTerm::Progress, 1.0f), 0.5f);
    EXPECT_FLOAT_EQ(ledger.TakeStep(), -0.02f - 0.03f + 0.5f);
    EXPECT_FLOAT_EQ(ledger.Score(), -0.02f - 0.02f - 0.03f);

    // Small beside an arrival: a whole 60 s episode pinned to a wall, pressing and stuck, costs less than Arrive.
    Cu::CurriculumTuning::ControlsTuning const controls;
    Cu::CurriculumTuning::MarkerTuning const markers;
    EXPECT_GT(controls.Wall, 0.0f);
    EXPECT_GT(controls.Stuck, 0.0f);
    EXPECT_LT((controls.Wall + controls.Stuck) * 60.0f, markers.Arrive);
    // The wall's charge is MarkerEncounter's: nothing for a slide that keeps half its ground, all of it at none.
    EXPECT_FLOAT_EQ(Cu::MarkerEncounter::WallCharge(1.0f, 0.0f, 1.75f, controls.Wall, controls.WallSlide),
        controls.Wall);
    EXPECT_FLOAT_EQ(Cu::MarkerEncounter::WallCharge(1.0f, 1.0f, 1.75f, controls.Wall, controls.WallSlide), 0.0f);
}

// The stop radius is the object's bounding radius and the tolerance, and the stop's precision is the air between the
// body and the object's widest side.
TEST(SightEncounterTest, TheStopIsMeasuredFromTheObjectsSide)
{
    Cu::CurriculumTuning::ControlsTuning const controls;
    EXPECT_FLOAT_EQ(controls.ArriveTolerance, 1.0f);
    EXPECT_FLOAT_EQ(Cu::SightEncounter::StopGap(0.78f + 0.389f, 0.78f, 0.389f), 0.0f);
    EXPECT_NEAR(Cu::SightEncounter::StopGap(1.78f, 0.78f, 0.389f), 0.611f, 1e-5f);
    EXPECT_FLOAT_EQ(Cu::SightEncounter::StopGap(0.5f, 0.78f, 0.389f), 0.0f);
    // The arrive radius leaves room for the body: wider than the object and the body's own radius together.
    for (Cu::SeekObject const& object : SightArena().Objects)
        EXPECT_GT(object.Radius + controls.ArriveTolerance, object.Radius + 0.389f);
}
