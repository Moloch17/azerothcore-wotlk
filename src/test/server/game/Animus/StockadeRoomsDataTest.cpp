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
#include "DetourNavMeshQuery.h"
#include "MMapMgr.h"
#include "MapTree.h"
#include "ModelIgnoreFlags.h"
#include "SeekDraw.h"
#include "StageDefinition.h"
#include "VMapMgr2.h"
#include "gtest/gtest.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

/// **The seek stage's rooms against the Stockades' own data** (M2: Stages.cpp StockadeRooms), loaded as a Map loads
/// them but without a worldserver: the map's static vmap tree (034.vmtree). Skipped unless FORGE_VISION_DATA names the
/// data directory (with vmaps/), so the ordinary unit test run, which has no client data, passes it by.
///
/// Every room's floor polygon, sampled every yard, has a floor (the vmaps, looked for from a yard above the room's
/// height down four) within Seek.FloorTolerance of the room's FloorZ -- every sample, not most -- and its Centre is
/// on the floor within half a yard; and most of each room is clear for an object (SeekEncounter's placement test, a
/// knee-height ray of Seek.Clearance along each axis), so its placement draws seldom fall back to the centre.
///
/// FORGE_STOCKADE_SCAN (the data directory too, run from it: the navmesh is read from DataDir ".") prints the authoring
/// scan instead: every half-yard (STEP) cell the navmesh walks, with its navmesh and vmap floor heights ("P x y nav
/// vmap"), which var tools segmented into the room table. The navmesh is read only here, to author; no bot reads it.
namespace
{
    namespace Cu = Animus::Curriculum;

    char const* Env(char const* name)
    {
        char const* value = std::getenv(name);
        return value && *value ? value : nullptr;
    }

    constexpr uint32 STOCKADE = 34;

    /// The vmap floor under (x, y), looked for from `from` down `reach` yards; NaN without one.
    float FloorAt(VMAP::StaticMapTree const& tree, float x, float y, float from, float reach)
    {
        float const height = tree.getHeight(VMAP::VMapMgr2::convertPositionToInternalRep(x, y, from), reach);
        return std::isfinite(height) ? height : std::nanf("");
    }
}

TEST(StockadeRoomsDataTest, EveryRoomSampleIsOnTheFloor)
{
    char const* data = Env("FORGE_VISION_DATA");
    if (!data)
        GTEST_SKIP() << "FORGE_VISION_DATA is not set";
    VMAP::StaticMapTree tree(STOCKADE, std::string(data) + "vmaps");
    ASSERT_TRUE(tree.InitMap(VMAP::VMapMgr2::getMapFileName(STOCKADE)));

    Cu::StageDefinition const* stage = Cu::FindStage("move2_seek");
    ASSERT_NE(stage, nullptr);
    Cu::CurriculumTuning::SeekTuning const tuning;
    std::size_t total = 0;
    for (Cu::SeekRoom const& room : stage->Arenas.at(0).Rooms)
    {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (auto const& [x, y] : room.Floor)
        {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
        uint32 samples = 0, clear = 0;
        float worst = 0.0f;
        for (float x = std::floor(minX); x <= maxX; x += 1.0f)
            for (float y = std::floor(minY); y <= maxY; y += 1.0f)
            {
                if (!Cu::SeekDraw::Inside(room.Floor, x, y))
                    continue;
                ++samples;
                float const floor = FloorAt(tree, x, y, room.FloorZ + 1.0f, 4.0f);
                ASSERT_FALSE(std::isnan(floor)) << room.Name << " has no floor at " << x << " " << y;
                worst = std::max(worst, std::fabs(floor - room.FloorZ));
                EXPECT_LE(std::fabs(floor - room.FloorZ), tuning.FloorTolerance)
                    << room.Name << " at " << x << " " << y << ": floor " << floor << ", the room's " << room.FloorZ;
                bool open = true;
                static float const AXES[4][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
                for (auto const& axis : AXES)
                {
                    G3D::Vector3 const from = VMAP::VMapMgr2::convertPositionToInternalRep(x, y, floor + 0.5f);
                    G3D::Vector3 const to = VMAP::VMapMgr2::convertPositionToInternalRep(x + axis[0] * tuning.Clearance,
                        y + axis[1] * tuning.Clearance, floor + 0.5f);
                    open = open && tree.isInLineOfSight(from, to, VMAP::ModelIgnoreFlags::Nothing);
                }
                clear += open ? 1 : 0;
            }
        total += samples;
        EXPECT_GE(samples, 10u) << room.Name;
        // Most of the room has space for an object: the placement's 24 draws then all but never fall back.
        EXPECT_GE(float(clear), 0.5f * float(samples)) << room.Name << ": " << clear << " of " << samples << " clear";
        float const centre = FloorAt(tree, room.Centre.first, room.Centre.second, room.FloorZ + 1.0f, 4.0f);
        EXPECT_LE(std::fabs(centre - room.FloorZ), 0.5f) << room.Name << "'s centre";
        std::cout << "  " << room.Name << ": " << samples << " samples, " << clear << " clear, worst floor "
            << worst << " yd off\n";
    }
    EXPECT_GT(total, 3000u);
}

TEST(StockadeRoomsDataTest, AuthoringScan)
{
    char const* data = Env("FORGE_STOCKADE_SCAN");
    if (!data)
        GTEST_SKIP() << "FORGE_STOCKADE_SCAN is not set";
    VMAP::StaticMapTree tree(STOCKADE, std::string(data) + "vmaps");
    ASSERT_TRUE(tree.InitMap(VMAP::VMapMgr2::getMapFileName(STOCKADE)));
    std::shared_ptr<dtNavMesh> mesh = MMAP::MMapMgr::LoadNavMesh(STOCKADE);
    ASSERT_TRUE(mesh);
    for (int32 x : { 31, 32 })
        for (int32 y : { 31, 32 })
            MMAP::MMapMgr::LoadTile(mesh.get(), STOCKADE, x, y);
    MMAP::ManagedNavMeshQuery query = MMAP::MMapMgr::CreateNavMeshQuery(mesh.get());
    dtQueryFilter filter;

    float const step = Env("STEP") ? float(std::atof(Env("STEP"))) : 0.5f;
    for (float x = 210.0f; x >= 40.0f; x -= step)
        for (float y = 160.0f; y >= -160.0f; y -= step)
        {
            // Detour's frame is (y, z, x).
            float const centre[3] = { y, -28.0f, x };
            float const extents[3] = { 0.2f, 8.0f, 0.2f };
            dtPolyRef poly = 0;
            float nearest[3] = {};
            if (!dtStatusSucceed(query->findNearestPoly(centre, extents, &filter, &poly, nearest)) || !poly
                || std::fabs(nearest[0] - y) > 0.05f || std::fabs(nearest[2] - x) > 0.05f)
                continue;
            float const floor = FloorAt(tree, x, y, nearest[1] + 1.5f, 4.0f);
            std::cout << "P " << x << " " << y << " " << nearest[1] << " " << (std::isnan(floor) ? 100.0f : floor)
                << "\n";
        }
}
