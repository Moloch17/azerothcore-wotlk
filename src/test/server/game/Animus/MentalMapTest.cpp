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

#include "MentalMap.h"
#include "gtest/gtest.h"
#include <cmath>
#include <vector>

namespace Vi = Animus::Vision;

namespace
{
    constexpr float PI = 3.14159265358979f;

    /// A ray from `camera` to the point `to`, ending there with what it hit.
    Vi::RayHit RayTo(Vi::Vec3 camera, Vi::Vec3 to, Vi::Class what, float normalZ)
    {
        Vi::Vec3 const d = to - camera;
        float const length = Vi::Length(d);
        return { d * (1.0f / length), length, to.Z, normalZ, what };
    }

    /// One frame of one ray.
    void Write(Vi::MentalMap& map, Vi::Vec3 camera, Vi::Vec3 to, Vi::Class what, float normalZ, float feetZ = 0.0f,
        Vi::MapWriteStats* stats = nullptr)
    {
        Vi::FrameHits frame;
        frame.Camera = camera;
        frame.Rays.push_back(RayTo(camera, to, what, normalZ));
        map.WriteFrame(frame, feetZ, 2.0f, stats);
    }

    /// A floor seen from straight above: the floor alone, no cell crossed.
    void FloorAt(Vi::MentalMap& map, float x, float y, float z)
    {
        Write(map, { x, y, z + 5.0f }, { x, y, z }, Vi::Class::Terrain, 1.0f);
    }

    std::vector<uint8_t> CropOf(Vi::MentalMap const& map, float x, float y, float z, float yaw)
    {
        std::vector<uint8_t> crop(Vi::CROP_BYTES, 0xAA);
        map.Crop(x, y, z, yaw, crop.data());
        return crop;
    }

    uint8_t CropAt(std::vector<uint8_t> const& crop, uint32_t row, uint32_t col, uint32_t channel)
    {
        return crop[(std::size_t(row) * Vi::CROP + col) * Vi::CROP_CHANNELS + channel];
    }

    Vi::CropCell CellAt(std::vector<uint8_t> const& crop, uint32_t row, uint32_t col)
    {
        return Vi::DecodeCropCell(&crop[(std::size_t(row) * Vi::CROP + col) * Vi::CROP_CHANNELS]);
    }

    /// The crop cells of a code, as (row, col).
    std::vector<std::pair<uint32_t, uint32_t>> CellsOf(std::vector<uint8_t> const& crop, Vi::MapCode code)
    {
        std::vector<std::pair<uint32_t, uint32_t>> cells;
        for (uint32_t row = 0; row < Vi::CROP; ++row)
            for (uint32_t col = 0; col < Vi::CROP; ++col)
                if (CropAt(crop, row, col, Vi::CROP_CODE) == uint8_t(code))
                    cells.emplace_back(row, col);
        return cells;
    }
}

// Amendment 4 (writes only from sight): a frame's floor hit marks its cell a floor at its height; a cell no ray hit or
// crossed in the band stays unknown, and nothing is written but what the rays and the body give.
TEST(MentalMapTest, WritesComeOnlyFromCastPixelsAndTheBody)
{
    Vi::MentalMap map;
    Vi::MapWriteStats stats;
    Write(map, { 0.5f, 0.5f, 3.0f }, { 10.5f, 0.5f, 0.0f }, Vi::Class::Terrain, 1.0f, 0.0f, &stats);
    EXPECT_EQ(stats.Hits, 1u);
    EXPECT_EQ(stats.Floors, 1u);

    Vi::MapCell const* hit = map.Find(10.5f, 0.5f);
    ASSERT_NE(hit, nullptr);
    float z = 99.0f;
    EXPECT_TRUE(Vi::NearestFloor(*hit, 0.0f, z));
    EXPECT_FLOAT_EQ(z, 0.0f);
    EXPECT_EQ(Vi::CodeOf(*hit, 0.0f), Vi::MapCode::Floor);
    EXPECT_TRUE(Vi::Known(*hit));

    // A cell beside the ray's path, in the same tile: never seen.
    Vi::MapCell const* beside = map.Find(10.5f, 5.5f);
    ASSERT_NE(beside, nullptr);
    EXPECT_FALSE(Vi::Known(*beside));
    EXPECT_EQ(Vi::CodeOf(*beside, 0.0f), Vi::MapCode::Unknown);
    // A tile no ray reached is not kept at all.
    EXPECT_EQ(map.Find(200.0f, 200.0f), nullptr);

    // The body: where it stands is visited, and on the ground its floor.
    map.WriteBody(-20.5f, -20.5f, 1.0f, true);
    Vi::MapCell const* stood = map.Find(-20.5f, -20.5f);
    ASSERT_NE(stood, nullptr);
    EXPECT_TRUE(stood->Flags & Vi::MAP_VISITED);
    EXPECT_EQ(Vi::CodeOf(*stood, 1.0f), Vi::MapCode::Floor);
    // Airborne, it is visited but no floor is taken from it.
    map.WriteBody(-60.5f, -60.5f, 30.0f, false);
    Vi::MapCell const* flown = map.Find(-60.5f, -60.5f);
    ASSERT_NE(flown, nullptr);
    EXPECT_TRUE(flown->Flags & Vi::MAP_VISITED);
    EXPECT_FALSE(Vi::NearestFloor(*flown, 30.0f, z));
}

// Amendments 2 and 11: a ray that crosses cells over a void -- down a 33-yard drop to the bottom of a shaft -- marks no
// floor on its way, and its cells are seen free only while it is within the body's band of the feet; only the hit's
// own cell is a floor. A sky ray marks no floor however far it runs, and nothing past 64 yards.
TEST(MentalMapTest, ARayOverAVoidMarksNoFloor)
{
    Vi::MentalMap map;
    Vi::MapWriteStats stats;
    // z falls from 3 at x 0.5 to -30 at x 40.5: within two yards of the feet (no floor known) only for x ~1.7 to 6.6.
    Write(map, { 0.5f, 0.5f, 3.0f }, { 40.5f, 0.5f, -30.0f }, Vi::Class::Terrain, 1.0f, 0.0f, &stats);
    EXPECT_EQ(stats.Floors, 1u);
    EXPECT_GT(stats.Free, 0u);
    for (int32_t x = 0; x < 40; ++x)
    {
        Vi::MapCell const* cell = map.Find(float(x) + 0.5f, 0.5f);
        ASSERT_NE(cell, nullptr);
        float z = 0.0f;
        EXPECT_FALSE(Vi::NearestFloor(*cell, 0.0f, z)) << x;
        if (x >= 7)
            EXPECT_FALSE(Vi::Known(*cell)) << x;
    }
    EXPECT_TRUE(map.Find(4.5f, 0.5f)->Flags & Vi::MAP_FREE);
    EXPECT_EQ(Vi::CodeOf(*map.Find(40.5f, 0.5f), -30.0f), Vi::MapCode::Floor);

    Vi::MentalMap open;
    Vi::FrameHits sky;
    sky.Camera = { 0.5f, 0.5f, 3.0f };
    sky.Rays.push_back({ { 1.0f, 0.0f, 0.0f }, 500.0f, 0.0f, 0.0f, Vi::Class::Sky });
    open.WriteFrame(sky, 2.0f, 2.0f);
    for (int32_t x = 0; x < 64; ++x)
    {
        Vi::MapCell const* cell = open.Find(float(x) + 0.5f, 0.5f);
        ASSERT_NE(cell, nullptr);
        float z = 0.0f;
        EXPECT_FALSE(Vi::NearestFloor(*cell, 2.0f, z));
        EXPECT_TRUE(cell->Flags & Vi::MAP_FREE);       // level with the head: within the band, seen free
    }
    Vi::MapCell const* past = open.Find(70.5f, 0.5f);
    EXPECT_TRUE(!past || !Vi::Known(*past));
}

// The band: a ray through the body's band over known floor makes it free, one over the band does not; a wall in the
// band is marked, one high above the floor (a lintel over a doorway) is not.
TEST(MentalMapTest, FreeAndWallsOnlyInTheBodysBand)
{
    Vi::MentalMap map;
    for (int32_t x = 1; x < 12; ++x)
        FloorAt(map, float(x) + 0.5f, 0.5f, 0.0f);
    Vi::MapWriteStats stats;
    Write(map, { 0.5f, 0.5f, 1.0f }, { 12.0f, 0.5f, 1.0f }, Vi::Class::Model, 0.0f, 0.0f, &stats);
    EXPECT_GE(stats.Free, 10u);
    EXPECT_EQ(stats.Walls, 1u);
    EXPECT_TRUE(map.Find(5.5f, 0.5f)->Flags & Vi::MAP_FREE);
    EXPECT_EQ(Vi::CodeOf(*map.Find(12.0f, 0.5f), 0.0f), Vi::MapCode::Wall);

    Vi::MentalMap high;
    for (int32_t x = 1; x < 13; ++x)
        FloorAt(high, float(x) + 0.5f, 0.5f, 0.0f);
    Write(high, { 0.5f, 0.5f, 3.5f }, { 12.0f, 0.5f, 3.5f }, Vi::Class::Model, 0.0f, 0.0f, &stats);
    EXPECT_FALSE(high.Find(5.5f, 0.5f)->Flags & Vi::MAP_FREE);
    // 3.5 yards over its floor, past a two-yard body and the slack: no wall where the body walks.
    EXPECT_EQ(Vi::CodeOf(*high.Find(12.0f, 0.5f), 0.0f), Vi::MapCode::Floor);
}

// Amendment 2: two floors in one cell -- a bridge over a hall -- are two layers; the crop reads the one nearest the
// feet, and a wall belongs to the layer it stands on.
TEST(MentalMapTest, TwoHeightLayers)
{
    Vi::MentalMap map;
    Vi::FrameHits frame;
    frame.Camera = { 0.5f, 0.5f, 20.0f };
    frame.Rays.push_back(RayTo(frame.Camera, { 5.5f, 0.5f, 0.0f }, Vi::Class::Terrain, 1.0f));
    frame.Rays.push_back(RayTo(frame.Camera, { 5.5f, 0.5f, 8.0f }, Vi::Class::Model, 0.95f));
    map.WriteFrame(frame, 8.0f, 2.0f);
    Vi::MapCell const* cell = map.Find(5.5f, 0.5f);
    ASSERT_NE(cell, nullptr);
    EXPECT_FLOAT_EQ(Vi::FloorHeight(cell->Floor[0]), 0.0f);
    EXPECT_FLOAT_EQ(Vi::FloorHeight(cell->Floor[1]), 8.0f);
    float z = 0.0f;
    EXPECT_TRUE(Vi::NearestFloor(*cell, 7.0f, z));
    EXPECT_FLOAT_EQ(z, 8.0f);
    EXPECT_TRUE(Vi::NearestFloor(*cell, 1.0f, z));
    EXPECT_FLOAT_EQ(z, 0.0f);

    // The lower floor seen again a little higher (a slope, rounding) is the same layer, not a third.
    FloorAt(map, 5.5f, 0.5f, 0.5f);
    EXPECT_FLOAT_EQ(Vi::FloorHeight(cell->Floor[0]), 0.5f);
    EXPECT_FLOAT_EQ(Vi::FloorHeight(cell->Floor[1]), 8.0f);

    // A wall at the upper floor's height: a wall to a body up there, not to one below.
    Write(map, { 0.5f, 0.5f, 9.0f }, { 5.5f, 0.5f, 9.0f }, Vi::Class::Model, 0.0f, 8.0f);
    EXPECT_TRUE(cell->Flags & Vi::MAP_WALL_HIGH);
    EXPECT_FALSE(cell->Flags & Vi::MAP_WALL_LOW);
    EXPECT_EQ(Vi::CodeOf(*cell, 8.0f), Vi::MapCode::Wall);
    EXPECT_EQ(Vi::CodeOf(*cell, 0.0f), Vi::MapCode::Floor);

    // The crop from the origin facing +x: the cell (5, 0) is sampled by crop cell (21, 23); its height is the layer
    // nearest the feet, relative to them -- the lower one, half a yard under feet at 1.
    std::vector<uint8_t> const below = CropOf(map, 0.0f, 0.0f, 1.0f, 0.0f);
    Vi::CropCell const low = CellAt(below, 21, 23);
    EXPECT_EQ(low.Code, Vi::MapCode::Floor);
    ASSERT_TRUE(low.HasHeight);
    EXPECT_FLOAT_EQ(low.Height, -0.5f);
    std::vector<uint8_t> const above = CropOf(map, 0.0f, 0.0f, 8.0f, 0.0f);
    Vi::CropCell const up = CellAt(above, 21, 23);
    EXPECT_EQ(up.Code, Vi::MapCode::Wall);
    ASSERT_TRUE(up.HasHeight);
    EXPECT_FLOAT_EQ(up.Height, 0.0f);
}

// The crop is heading-up, 2 yards a cell, the body at its centre: ahead is up whatever the facing, the right is to the
// right, and behind is below.
TEST(MentalMapTest, TheCropTurnsWithTheFacing)
{
    // A wall in the 1-yd cell (10, -1): 10.5 yards along +x, half a yard to the -y side.
    Vi::MentalMap east;
    Write(east, { 7.5f, -0.5f, 1.0f }, { 10.5f, -0.5f, 1.0f }, Vi::Class::Model, 0.0f);
    using Cells = std::vector<std::pair<uint32_t, uint32_t>>;
    // Facing +x: ahead (row 18: forward 10 to 12) and just right of the centre line (col 24).
    EXPECT_EQ(CellsOf(CropOf(east, 0.0f, 0.0f, 0.0f, 0.0f), Vi::MapCode::Wall), (Cells{ { 18, 24 } }));
    // Facing +y: the wall is on the right (col 29: right 10 to 12), level with the body (row 24: forward 0 to -2).
    EXPECT_EQ(CellsOf(CropOf(east, 0.0f, 0.0f, 0.0f, PI / 2.0f), Vi::MapCode::Wall), (Cells{ { 24, 29 } }));
    // Facing -x: behind (row 29), and now on the left of the centre line.
    EXPECT_EQ(CellsOf(CropOf(east, 0.0f, 0.0f, 0.0f, PI), Vi::MapCode::Wall), (Cells{ { 29, 23 } }));

    // The same wall turned a quarter about the body, the body turned with it: the same crop.
    Vi::MentalMap north;
    Write(north, { 0.5f, 7.5f, 1.0f }, { 0.5f, 10.5f, 1.0f }, Vi::Class::Model, 0.0f);
    EXPECT_EQ(CellsOf(CropOf(north, 0.0f, 0.0f, 0.0f, PI / 2.0f), Vi::MapCode::Wall), (Cells{ { 18, 24 } }));

    // Turned 45 degrees, the crop resamples the turned grid: a wall ten yards ahead along the diagonal is still about
    // five cells up the centre line.
    Vi::MentalMap diagonal;
    Write(diagonal, { 5.0f, 5.0f, 1.0f }, { 7.5f, 7.5f, 1.0f }, Vi::Class::Model, 0.0f);
    Cells const turned = CellsOf(CropOf(diagonal, 0.0f, 0.0f, 0.0f, PI / 4.0f), Vi::MapCode::Wall);
    ASSERT_FALSE(turned.empty());
    for (auto const& [row, col] : turned)
    {
        EXPECT_GE(row, 17u);
        EXPECT_LE(row, 19u);
        EXPECT_GE(col, 23u);
        EXPECT_LE(col, 24u);
    }
}

// The Change: each 2-yard crop cell summarises its four 1-yard cells -- the code by priority (wall > door > hazard >
// floor > unknown), the floor nearest the feet, visited if any, the newest look, the most recent entity, frontier if
// any.
TEST(MentalMapTest, TheTwoYardCellSummarisesItsFour)
{
    Vi::MentalMap map;
    // The body at the origin facing +x: crop cell (23, 23) covers forward 0 to 2 and right -2 to 0, the 1-yd cells
    // x 0..1, y 0..1.
    FloorAt(map, 0.5f, 0.5f, 0.5f);
    FloorAt(map, 1.5f, 0.5f, 0.75f);
    FloorAt(map, 0.5f, 1.5f, 0.75f);
    FloorAt(map, 1.5f, 1.5f, 1.0f);
    std::vector<uint8_t> crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    Vi::CropCell cell = CellAt(crop, 23, 23);
    EXPECT_EQ(cell.Code, Vi::MapCode::Floor);
    ASSERT_TRUE(cell.HasHeight);
    EXPECT_FLOAT_EQ(cell.Height, 0.5f);         // the lowest of 0.5, 0.75, 0.75 and 1: nearest the feet at 0
    EXPECT_FALSE(cell.Visited);
    EXPECT_TRUE(cell.Seen);
    EXPECT_TRUE(cell.Frontier);                 // known floor beside cells never seen
    EXPECT_EQ(cell.Entity, 0u);

    // A door in one of the four is the cell's, until a wall in another outranks it; the door's class is its entity.
    Write(map, { -2.0f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, Vi::Class::Door, 0.0f);
    crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    cell = CellAt(crop, 23, 23);
    EXPECT_EQ(cell.Code, Vi::MapCode::Door);
    EXPECT_EQ(cell.Entity, uint8_t(Vi::Class::Door));
    Write(map, { -2.0f, 1.5f, 0.5f }, { 1.5f, 1.5f, 0.5f }, Vi::Class::Model, 0.0f);
    map.WriteBody(1.2f, 0.8f, 0.75f, true);
    crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    cell = CellAt(crop, 23, 23);
    EXPECT_EQ(cell.Code, Vi::MapCode::Wall);
    EXPECT_TRUE(cell.Visited);
    EXPECT_EQ(cell.Entity, uint8_t(Vi::Class::Door));

    // The far corner: never seen, no height, nothing in it, no frontier.
    Vi::CropCell const far = CellAt(crop, 0, 0);
    EXPECT_EQ(far.Code, Vi::MapCode::Unknown);
    EXPECT_FALSE(far.Seen);
    EXPECT_FALSE(far.HasHeight);
    EXPECT_FALSE(far.Visited);
    EXPECT_EQ(far.Entity, 0u);
    EXPECT_FALSE(far.Frontier);
}

// The frontier: known floor beside a cell never seen, from the seat's own map; a floor ringed by seen cells is none.
TEST(MentalMapTest, Frontier)
{
    Vi::MentalMap map;
    for (int32_t x = -4; x <= 4; ++x)
        for (int32_t y = -4; y <= 4; ++y)
            FloorAt(map, float(x) + 0.5f, float(y) + 0.5f, 0.0f);
    std::vector<uint8_t> const crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    // Inside the patch (the 1-yd cells -2..1 each way): no frontier.
    for (uint32_t row = 23; row <= 24; ++row)
        for (uint32_t col = 23; col <= 24; ++col)
            EXPECT_EQ(CropAt(crop, row, col, Vi::CROP_FRONTIER), 0u) << row << " " << col;
    // Its rim -- the 1-yd cells at x 4, crop row 21 -- is.
    EXPECT_EQ(CropAt(crop, 21, 23, Vi::CROP_FRONTIER), 1u);
    EXPECT_EQ(CropAt(crop, 21, 24, Vi::CROP_FRONTIER), 1u);
    // Past it, nothing known: no frontier either.
    EXPECT_EQ(CropAt(crop, 19, 23, Vi::CROP_FRONTIER), 0u);
    // Seen free all round the rim (a ray across it at knee height), the rim is no longer frontier on that side.
    Vi::MentalMap ringed;
    for (int32_t x = -4; x <= 4; ++x)
        for (int32_t y = -4; y <= 4; ++y)
            FloorAt(ringed, float(x) + 0.5f, float(y) + 0.5f, 0.0f);
    for (float y : { -1.5f, -0.5f, 0.5f, 1.5f })
        Write(ringed, { 0.5f, y, 0.5f }, { 9.5f, y, 0.5f }, Vi::Class::Model, 0.0f);
    std::vector<uint8_t> const after = CropOf(ringed, 0.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_EQ(CropAt(after, 21, 23, Vi::CROP_FRONTIER), 0u);
}

// Persistence and ageing (amendment 1): kept across a reset, the map's clock moves on by the offset and every look is
// that much older; an entity's age is kept against its cell's newer looks, never assumed still there.
TEST(MentalMapTest, PersistenceAndAgeing)
{
    Vi::MentalMap map;
    Vi::FrameHits frame;
    frame.Camera = { 0.0f, 0.0f, 3.0f };
    frame.Rays.push_back(RayTo(frame.Camera, { 4.5f, -0.5f, 0.0f }, Vi::Class::Terrain, 1.0f));
    frame.Rays.push_back(RayTo(frame.Camera, { 4.5f, -1.5f, 0.5f }, Vi::Class::Chest, 1.0f));
    map.WriteFrame(frame, 0.0f, 2.0f);
    // Forward 4 to 6, right 0 to 2: crop cell (21, 24).
    std::vector<uint8_t> crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    Vi::CropCell const fresh = CellAt(crop, 21, 24);
    EXPECT_TRUE(fresh.Seen);
    EXPECT_LT(fresh.Age, 1.0f);
    EXPECT_EQ(fresh.Entity, uint8_t(Vi::Class::Chest));

    // A reset that keeps the map, 300 s on.
    map.Advance(300.0f);
    crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    Vi::CropCell const aged = CellAt(crop, 21, 24);
    EXPECT_NEAR(aged.Age, 300.0f, 300.0f * 0.04f);
    EXPECT_EQ(aged.Entity, uint8_t(Vi::Class::Chest));

    // The chest's cell seen again with no chest there: the look is new, the chest's memory stays, five minutes old.
    FloorAt(map, 4.5f, -1.5f, 0.0f);
    Vi::MapCell const* chest = map.Find(4.5f, -1.5f);
    ASSERT_NE(chest, nullptr);
    EXPECT_EQ(chest->Entity & Vi::CLASS_MASK, uint8_t(Vi::Class::Chest));
    EXPECT_EQ(chest->Entity >> 5, Vi::EntityAgeBucket(300.0f));
    EXPECT_EQ(chest->Seen, map.Stamp());
    crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    Vi::CropCell const looked = CellAt(crop, 21, 24);
    EXPECT_LT(looked.Age, 1.0f);
    EXPECT_EQ(looked.Entity, uint8_t(Vi::Class::Chest));

    // A reset that does not keep it: nothing at all.
    map.Clear();
    EXPECT_EQ(map.Tiles(), 0u);
    EXPECT_EQ(map.Clock(), 0.0);
    crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    for (uint32_t row = 0; row < Vi::CROP; ++row)
        for (uint32_t col = 0; col < Vi::CROP; ++col)
            ASSERT_EQ(CropAt(crop, row, col, Vi::CROP_AGE), Vi::CROP_AGE_NEVER);
}

// The caps: the least recently written tile goes first; with coarse tiles (the realm's), it is folded into them and
// still read by the crop.
TEST(MentalMapTest, CapsAndCoarseTiles)
{
    Vi::MentalMap training;
    training.Configure({ 2, 0 });
    FloorAt(training, 0.5f, 0.5f, 0.0f);
    FloorAt(training, 40.5f, 0.5f, 0.0f);
    FloorAt(training, 80.5f, 0.5f, 0.0f);
    EXPECT_EQ(training.Tiles(), 2u);
    EXPECT_EQ(training.Find(0.5f, 0.5f), nullptr);
    EXPECT_NE(training.Find(80.5f, 0.5f), nullptr);

    Vi::MentalMap realm;
    realm.Configure({ 1, 4 });
    FloorAt(realm, 0.5f, 0.5f, 0.0f);
    FloorAt(realm, 40.5f, 0.5f, 0.0f);
    EXPECT_EQ(realm.Tiles(), 1u);
    EXPECT_EQ(realm.CoarseTileCount(), 1u);
    EXPECT_EQ(realm.Find(0.5f, 0.5f), nullptr);
    // The evicted floor is read from its 8-yard coarse cell: from x 10 facing -x, it is ahead.
    std::vector<uint8_t> const crop = CropOf(realm, 10.0f, 0.0f, 0.0f, PI);
    EXPECT_GE(CellsOf(crop, Vi::MapCode::Floor).size(), 4u);
}

// The crop's bytes read back as the learner reads them (DecodeCropCell, animus.mappo.networks.decode_map): every
// channel round trips, the age to its log byte's rounding.
TEST(MentalMapTest, CropBytesRoundTrip)
{
    EXPECT_EQ(Vi::CROP_BYTES, 48u * 48u * 6u);
    EXPECT_EQ(Vi::AgeByte(0.0f), 0u);
    EXPECT_EQ(Vi::AgeByte(1e9f), 254u);
    for (float seconds : { 0.0f, 1.0f, 10.0f, 60.0f, 600.0f, 3600.0f })
    {
        uint8_t const bytes[Vi::CROP_CHANNELS] = { uint8_t(Vi::MapCode::Door), uint8_t(Vi::CROP_HEIGHT_ZERO + 6), 1,
            Vi::AgeByte(seconds), uint8_t(Vi::Class::Chest), 1 };
        Vi::CropCell const cell = Vi::DecodeCropCell(bytes);
        EXPECT_EQ(cell.Code, Vi::MapCode::Door);
        EXPECT_TRUE(cell.HasHeight);
        EXPECT_FLOAT_EQ(cell.Height, 1.5f);
        EXPECT_TRUE(cell.Visited);
        EXPECT_TRUE(cell.Seen);
        EXPECT_NEAR(cell.Age, seconds, 0.04f * seconds + 0.04f);
        EXPECT_EQ(cell.Entity, uint8_t(Vi::Class::Chest));
        EXPECT_TRUE(cell.Frontier);
    }
    uint8_t const none[Vi::CROP_CHANNELS] = { 0, 0, 0, Vi::CROP_AGE_NEVER, 0, 0 };
    Vi::CropCell const nothing = Vi::DecodeCropCell(none);
    EXPECT_EQ(nothing.Code, Vi::MapCode::Unknown);
    EXPECT_FALSE(nothing.HasHeight);
    EXPECT_FALSE(nothing.Seen);

    // A map's crop through the same decode: a floor two yards under the feet reads -2.
    Vi::MentalMap map;
    FloorAt(map, 4.5f, -0.5f, -2.0f);
    std::vector<uint8_t> const crop = CropOf(map, 0.0f, 0.0f, 0.0f, 0.0f);
    Vi::CropCell const floor = CellAt(crop, 21, 24);
    EXPECT_EQ(floor.Code, Vi::MapCode::Floor);
    EXPECT_FLOAT_EQ(floor.Height, -2.0f);
}
