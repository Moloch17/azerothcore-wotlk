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

#include "Camera.h"
#include "CurriculumTuning.h"
#include "MapTree.h"
#include "ModelIgnoreFlags.h"
#include "SeekDraw.h"
#include "SightDraw.h"
#include "StageDefinition.h"
#include "VMapMgr2.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

/// **M1's hallway table and evaluation pairs against the Stockades' own data** (Stages.cpp StockadeHallways and
/// StockadeSightPairs), loaded as a Map loads them but without a worldserver: the map's static vmap tree (034.vmtree),
/// cast through by Vision::CastRay as the camera casts (the Stockades has no terrain heights, and the dynamic tree is
/// empty once the dungeon's own objects are cleared, as SightEncounter clears them). Skipped unless FORGE_VISION_DATA
/// names the data directory (with vmaps/), so the ordinary unit test run, which has no client data, passes it by.
///
/// Every hallway point stands on a vmap floor within a quarter yard of its height, with a knee-height ray of
/// Seek.Clearance yards clear along each axis (an object stands there clear of the walls); every evaluation pair is
/// what it says at both a small and a tall body's eye -- in sight of the spawn, or round a corner: out of its sight and
/// in sight of a stepping point a few yards off -- for each object of the pool.
///
/// FORGE_SIGHT_AUTHOR (the data directory too) prints a set of pairs instead, drawn as SightDraw::Place draws them,
/// for StockadeSightPairs (how its table was authored).
namespace
{
    namespace Cu = Animus::Curriculum;
    namespace Sd = Animus::Curriculum::SightDraw;
    namespace Vi = Animus::Vision;

    char const* Env(char const* name)
    {
        char const* value = std::getenv(name);
        return value && *value ? value : nullptr;
    }

    constexpr uint32 STOCKADE = 34;
    /// A gnome's body and a tauren's (the controller's collision heights), the eyes the pairs are checked from.
    constexpr float BODIES[] = { 1.0f, 2.4f };

    /// The vmap floor under (x, y), looked for from `from` down `reach` yards; NaN without one.
    float FloorAt(VMAP::StaticMapTree const& tree, float x, float y, float from, float reach)
    {
        float const height = tree.getHeight(VMAP::VMapMgr2::convertPositionToInternalRep(x, y, from), reach);
        return std::isfinite(height) ? height : std::nanf("");
    }

    /// The camera's world over the static tree alone, read as MapVisionWorld reads a map's (the surface hit as
    /// StaticVMapCollisionData::GetSurfaceHit): an instance with no terrain heights and nothing in the dynamic tree.
    class TreeWorld : public Vi::VisionWorld
    {
    public:
        explicit TreeWorld(VMAP::StaticMapTree const& tree) : _tree(tree) { }

        Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            Vi::SurfaceHit hit;
            G3D::Vector3 const pos1 = VMAP::VMapMgr2::convertPositionToInternalRep(from.X, from.Y, from.Z);
            G3D::Vector3 const pos2 = VMAP::VMapMgr2::convertPositionToInternalRep(to.X, to.Y, to.Z);
            float const length = (pos2 - pos1).magnitude();
            if (!(length > 1e-6f) || !std::isfinite(length))
                return hit;
            G3D::Vector3 const dir = (pos2 - pos1) / length;
            float reach = length;
            G3D::Vector3 normal = G3D::Vector3::zero();
            if (!_tree.GetSurfaceIntersection(G3D::Ray::fromOriginAndDirection(pos1, dir), reach, normal))
                return hit;
            hit.Distance = reach;
            float const n = normal.magnitude();
            hit.NormalZ = n > 1e-12f ? (normal.dot(dir) > 0.0f ? -normal.z : normal.z) / n : 0.0f;
            return hit;
        }
        Vi::SurfaceHit DynamicHit(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::LiquidHit ModelLiquid(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::TerrainTile Tile(int32_t, int32_t) const override
        {
            Vi::TerrainTile tile;
            tile.Loaded = true;
            return tile;
        }
        Vi::TerrainCell Cell(int32_t, int32_t, int32_t, int32_t, bool) const override { return {}; }
        Animus::Movement::Liquid LiquidAt(float, float, float) const override { return {}; }
        float FloorBelow(float x, float y, float z, float search) const override
        {
            float const floor = FloorAt(_tree, x, y, z, search);
            return std::isnan(floor) ? Animus::Movement::INVALID_FLOOR : floor;
        }

    private:
        VMAP::StaticMapTree const& _tree;
    };

    Sd::Viewing ViewingFor(float body, Cu::SeekObject const& object)
    {
        Cu::CurriculumTuning::ControlsTuning const controls;
        Sd::Viewing viewing;
        viewing.EyeRise = Vi::PIVOT_SHARE * body;
        viewing.CentreRise = object.Height * 0.5f;
        viewing.Radius = object.Radius;
        viewing.Nearest = controls.Nearest;
        viewing.Furthest = controls.Furthest;
        viewing.CornerStep = controls.CornerStep;
        viewing.Attempts = 1000;
        return viewing;
    }

    /// Whether pair (spawn, object) is what `corner` says at every body's eye and for every object of the pool.
    bool PairHolds(std::vector<Position> const& points, std::vector<Cu::SeekObject> const& pool, uint32 spawn,
        uint32 object, bool corner, Vi::VisionWorld const& world, std::string* why = nullptr)
    {
        for (float body : BODIES)
            for (Cu::SeekObject const& kind : pool)
            {
                Sd::Viewing const viewing = ViewingFor(body, kind);
                bool const seen = Sd::Seen(points, points[spawn], object, viewing, world);
                if (!corner && !seen)
                {
                    if (why)
                        *why = "not in sight at body " + std::to_string(body) + " for " + kind.Kind;
                    return false;
                }
                if (corner && seen)
                {
                    if (why)
                        *why = "in sight at body " + std::to_string(body) + " for " + kind.Kind;
                    return false;
                }
                if (corner && Sd::SeenFromStep(points, Sd::Steps(points, points[spawn], viewing, world), object,
                    viewing, world) < 0)
                {
                    if (why)
                        *why = "no stepping point sees it at body " + std::to_string(body) + " for " + kind.Kind;
                    return false;
                }
            }
        return true;
    }
}

TEST(StockadeHallwaysDataTest, EveryHallwayPointIsOnTheFloorAndClear)
{
    char const* data = Env("FORGE_VISION_DATA");
    if (!data)
        GTEST_SKIP() << "FORGE_VISION_DATA is not set";
    VMAP::StaticMapTree tree(STOCKADE, std::string(data) + "vmaps");
    ASSERT_TRUE(tree.InitMap(VMAP::VMapMgr2::getMapFileName(STOCKADE)));

    Cu::StageDefinition const* stage = Cu::FindStage("move1_controls");
    ASSERT_NE(stage, nullptr);
    Cu::CurriculumTuning::SeekTuning const tuning;
    std::vector<Position> const& points = stage->Arenas.at(0).SpawnPoints;
    float worst = 0.0f;
    uint32 clear = 0;
    for (Position const& point : points)
    {
        float const x = point.GetPositionX();
        float const y = point.GetPositionY();
        float const floor = FloorAt(tree, x, y, point.GetPositionZ() + 1.0f, 4.0f);
        ASSERT_FALSE(std::isnan(floor)) << "no floor at " << x << " " << y;
        worst = std::max(worst, std::fabs(floor - point.GetPositionZ()));
        EXPECT_LE(std::fabs(floor - point.GetPositionZ()), 0.25f) << x << " " << y << ": floor " << floor
            << ", the table's " << point.GetPositionZ();
        bool open = true;
        static float const AXES[4][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
        for (auto const& axis : AXES)
        {
            G3D::Vector3 const from = VMAP::VMapMgr2::convertPositionToInternalRep(x, y, floor + 0.5f);
            G3D::Vector3 const to = VMAP::VMapMgr2::convertPositionToInternalRep(x + axis[0] * tuning.Clearance,
                y + axis[1] * tuning.Clearance, floor + 0.5f);
            open = open && tree.isInLineOfSight(from, to, VMAP::ModelIgnoreFlags::Nothing);
        }
        EXPECT_TRUE(open) << x << " " << y << " is within " << tuning.Clearance << " yd of a wall";
        clear += open ? 1 : 0;
    }
    std::cout << "  " << points.size() << " hallway points, " << clear << " clear, worst floor " << worst
        << " yd off\n";
}

TEST(StockadeHallwaysDataTest, EveryEvaluationPairIsWhatItSays)
{
    char const* data = Env("FORGE_VISION_DATA");
    if (!data)
        GTEST_SKIP() << "FORGE_VISION_DATA is not set";
    VMAP::StaticMapTree tree(STOCKADE, std::string(data) + "vmaps");
    ASSERT_TRUE(tree.InitMap(VMAP::VMapMgr2::getMapFileName(STOCKADE)));
    for (int32 x = 30; x <= 33; ++x)
        for (int32 y = 30; y <= 33; ++y)
            tree.LoadMapTile(uint32(x), uint32(y));
    TreeWorld const world(tree);

    Cu::StageDefinition const* stage = Cu::FindStage("move1_controls");
    ASSERT_NE(stage, nullptr);
    Cu::ArenaDefinition const& arena = stage->Arenas.at(0);
    for (Cu::SightPair const& pair : arena.SightPairs)
    {
        std::string why;
        EXPECT_TRUE(PairHolds(arena.SpawnPoints, arena.Objects, pair.Spawn, pair.Object, pair.Corner, world, &why))
            << "pair " << pair.Spawn << " -> " << pair.Object << (pair.Corner ? " (corner)" : "") << ": " << why;
    }

    // The walls are there to the caster: from the entrance, the straight hallway ahead is in sight and the wing
    // corridors round the crossing are not.
    Sd::Viewing const tall = ViewingFor(BODIES[1], arena.Objects[0]);
    uint32 inSight = 0, hidden = 0;
    for (uint32 point = 1; point < arena.SpawnPoints.size(); ++point)
    {
        bool const seen = Sd::Seen(arena.SpawnPoints, arena.SpawnPoints[0], point, tall, world);
        if (std::fabs(arena.SpawnPoints[point].GetPositionY()) > 30.0f)
            EXPECT_FALSE(seen) << "a wing corridor point in sight of the entrance: "
                << arena.SpawnPoints[point].GetPositionX() << " " << arena.SpawnPoints[point].GetPositionY();
        inSight += seen ? 1 : 0;
        hidden += seen ? 0 : 1;
    }
    EXPECT_GT(inSight, 20u);
    EXPECT_GT(hidden, 20u);
    std::cout << "  from the entrance: " << inSight << " points in sight, " << hidden << " hidden\n";

    // And the draw itself finds a place in sight from every hallway point, at both eyes: no spawn of the table
    // leaves the episode without an object (the scenario would draw the spawn again).
    uint32 placed = 0;
    for (uint32 spawn = 0; spawn < arena.SpawnPoints.size(); ++spawn)
    {
        Animus::Curriculum::SeekObject const& kind = arena.Objects[spawn % arena.Objects.size()];
        uint32 salt = 0;
        auto uniform = [&]() { return Cu::SeekDraw::SeedUniform(spawn, ++salt); };
        Sd::Placement const placement = Sd::Place(arena.SpawnPoints, arena.SpawnPoints[spawn], false,
            ViewingFor(BODIES[spawn % 2], kind), world, uniform);
        EXPECT_GE(placement.Point, 0) << "nothing in sight of " << arena.SpawnPoints[spawn].GetPositionX() << " "
            << arena.SpawnPoints[spawn].GetPositionY();
        placed += placement.Point >= 0 ? 1 : 0;
    }
    std::cout << "  " << arena.SightPairs.size() << " pairs hold; " << placed << " of " << arena.SpawnPoints.size()
        << " spawns place an object in sight\n";
}

TEST(StockadeHallwaysDataTest, AuthoringPairs)
{
    char const* data = Env("FORGE_SIGHT_AUTHOR");
    if (!data)
        GTEST_SKIP() << "FORGE_SIGHT_AUTHOR is not set";
    VMAP::StaticMapTree tree(STOCKADE, std::string(data) + "vmaps");
    ASSERT_TRUE(tree.InitMap(VMAP::VMapMgr2::getMapFileName(STOCKADE)));
    for (int32 x = 30; x <= 33; ++x)
        for (int32 y = 30; y <= 33; ++y)
            tree.LoadMapTile(uint32(x), uint32(y));
    TreeWorld const world(tree);

    Cu::StageDefinition const* stage = Cu::FindStage("move1_controls");
    ASSERT_NE(stage, nullptr);
    Cu::ArenaDefinition const& arena = stage->Arenas.at(0);
    std::vector<Position> const& points = arena.SpawnPoints;
    uint32 const count = uint32(points.size());
    uint32 const plain = uint32(std::atoi(Env("SIGHT_PLAIN") ? Env("SIGHT_PLAIN") : "24"));
    uint32 const corners = uint32(std::atoi(Env("SIGHT_CORNERS") ? Env("SIGHT_CORNERS") : "8"));
    // Spawns spread over the table: the entrance first, then a stride coprime with its length.
    uint32 spawn = 0;
    std::set<uint32> used;
    for (uint32 k = 0; k < plain + corners; ++k)
    {
        bool const corner = k >= plain;
        bool done = false;
        for (uint32 tries = 0; tries < count && !done; ++tries, spawn = (spawn + 97) % count)
        {
            if (used.contains(spawn))
                continue;
            uint32 salt = 0;
            auto uniform = [&]() { return Cu::SeekDraw::SeedUniform(1000 + k * 64 + tries, ++salt); };
            Cu::SeekObject const& kind = arena.Objects[k % arena.Objects.size()];
            Sd::Placement const placement = Sd::Place(points, points[spawn], corner, ViewingFor(BODIES[1], kind),
                world, uniform);
            if (placement.Point < 0 || placement.Corner != corner
                || !PairHolds(points, arena.Objects, spawn, uint32(placement.Point), corner, world))
                continue;
            Position const& from = points[spawn];
            Position const& to = points[uint32(placement.Point)];
            std::cout << "            { " << spawn << ", " << placement.Point << ", " << (corner ? "true" : "false")
                << " },  // (" << from.GetPositionX() << " " << from.GetPositionY() << ") -> (" << to.GetPositionX()
                << " " << to.GetPositionY() << "), " << from.GetExactDist2d(&to) << " yd\n";
            used.insert(spawn);
            done = true;
        }
        EXPECT_TRUE(done) << "no " << (corner ? "corner " : "") << "pair for " << k;
    }
}
