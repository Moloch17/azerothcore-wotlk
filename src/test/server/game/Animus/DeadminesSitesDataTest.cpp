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
#include "StageDefinition.h"
#include "VMapMgr2.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

/// **M3 interact's sites against the Deadmines' own data** (Stages.cpp DeadminesSites), loaded as a Map loads them but
/// without a worldserver: the map's static vmap tree (036.vmtree) and its tiles. Skipped unless FORGE_VISION_DATA names
/// the data directory (with vmaps/), so the ordinary unit test run, which has no client data, passes it by.
///
/// Every site point stands on a vmap floor within a quarter yard of its height, with a knee-height ray of
/// Seek.Clearance yards clear along each axis (an object stands there clear of the walls); every lever site's far side
/// is out of sight of its near side but through the shut door -- every line of sight from a near point's eye to a far
/// point's object passes through the door's leaf -- so the door is what keeps the named object from the seat; and each
/// opener stands within reach of its near side.
///
/// FORGE_DEADMINES_SCAN (the data directory too, run from it: the navmesh is read from DataDir ".") prints the
/// authoring scan instead: every half-yard cell the navmesh walks, on every storey, with its navmesh and vmap floor
/// heights ("P x y nav vmap"), which .agents/plans/dungeon-curriculum/tools/sites.py turns into the site table. The
/// navmesh is read only here, to author; no bot reads it.
namespace
{
    namespace Cu = Animus::Curriculum;

    char const* Env(char const* name)
    {
        char const* value = std::getenv(name);
        return value && *value ? value : nullptr;
    }

    constexpr uint32 DEADMINES = 36;
    /// The eye over the feet (a tall body's: Vision::PIVOT_SHARE x 2.4), and an object's centre over its base.
    constexpr float EYE = 1.8f;
    constexpr float OBJECT_CENTRE = 0.5f;

    /// The sites' doors and openers as the world DB spawns them (gameobject 30533, 26182, 26185, 30534; 26188, 26192,
    /// 26197, 26205): x, y, z, orientation. The doors' leaf (display 394, 400) spans local y -4.41 to 4.51, 7.44 tall.
    struct Placed
    {
        uint32 Entry;
        float X;
        float Y;
        float Z;
        float O;
    };
    constexpr Placed DOORS[] = { { 13965, -191.415f, -457.446f, 54.439f, 1.693f },
        { 16399, -168.514f, -579.861f, 19.316f, 3.124f }, { 16400, -290.294f, -536.96f, 49.435f, 1.553f },
        { 16397, -100.502f, -668.771f, 7.410f, 1.815f } };
    constexpr Placed OPENERS[] = { { 101831, -188.137f, -460.313f, 54.559f, 1.728f },
        { 101834, -165.404f, -576.924f, 19.306f, 3.299f }, { 101832, -287.282f, -539.877f, 49.432f, 1.728f },
        { 16398, -107.562f, -659.674f, 7.212f, -0.890f } };
    constexpr float LEAF_LOW = -4.41f;
    constexpr float LEAF_HIGH = 4.51f;
    constexpr float LEAF_TALL = 7.44f;

    template <std::size_t N>
    Placed const* Find(Placed const (&table)[N], uint32 entry)
    {
        for (Placed const& placed : table)
            if (placed.Entry == entry)
                return &placed;
        return nullptr;
    }

    /// Whether the segment from `a` to `b` passes through the door's shut leaf: its plane, within its width and height.
    bool ThroughLeaf(Placed const& door, G3D::Vector3 a, G3D::Vector3 b)
    {
        float const c = std::cos(door.O);
        float const s = std::sin(door.O);
        auto const along = [&](G3D::Vector3 p) { return (p.x - door.X) * c + (p.y - door.Y) * s; };
        auto const across = [&](G3D::Vector3 p) { return -(p.x - door.X) * s + (p.y - door.Y) * c; };
        float const fa = along(a);
        float const fb = along(b);
        if ((fa > 0.0f) == (fb > 0.0f))
            return false;
        float const t = fa / (fa - fb);
        G3D::Vector3 const hit = a + (b - a) * t;
        float const y = across(hit);
        return y >= LEAF_LOW && y <= LEAF_HIGH && hit.z >= door.Z - 0.5f && hit.z <= door.Z + LEAF_TALL;
    }

    /// The vmap floor under (x, y), looked for from `from` down `reach` yards; NaN without one.
    float FloorAt(VMAP::StaticMapTree const& tree, float x, float y, float from, float reach)
    {
        float const height = tree.getHeight(VMAP::VMapMgr2::convertPositionToInternalRep(x, y, from), reach);
        return std::isfinite(height) ? height : std::nanf("");
    }

    /// The map's static tree with every tile the dungeon has.
    bool Load(VMAP::StaticMapTree& tree)
    {
        if (!tree.InitMap(VMAP::VMapMgr2::getMapFileName(DEADMINES)))
            return false;
        for (uint32 x = 28; x <= 36; ++x)
            for (uint32 y = 28; y <= 36; ++y)
                tree.LoadMapTile(x, y);
        return true;
    }

    std::vector<Cu::InteractSite> const& SitesOf()
    {
        Cu::StageDefinition const* stage = Cu::FindStage("move3_interact");
        static std::vector<Cu::InteractSite> const none;
        return stage ? stage->Arenas.at(0).Sites : none;
    }
}

TEST(DeadminesSitesDataTest, EverySitePointIsOnTheFloorAndClear)
{
    char const* data = Env("FORGE_VISION_DATA");
    if (!data)
        GTEST_SKIP() << "FORGE_VISION_DATA is not set";
    VMAP::StaticMapTree tree(DEADMINES, std::string(data) + "vmaps");
    ASSERT_TRUE(Load(tree));
    ASSERT_EQ(SitesOf().size(), 4u);
    Cu::CurriculumTuning::SeekTuning const tuning;
    uint32 points = 0;
    for (Cu::InteractSite const& site : SitesOf())
        for (std::vector<Position> const* side : { &site.Near, &site.Far })
            for (Position const& point : *side)
            {
                ++points;
                float const x = point.GetPositionX();
                float const y = point.GetPositionY();
                float const floor = FloorAt(tree, x, y, point.GetPositionZ() + 1.0f, 4.0f);
                ASSERT_FALSE(std::isnan(floor)) << site.Name << ": no floor at " << x << " " << y;
                EXPECT_LE(std::fabs(floor - point.GetPositionZ()), 0.25f) << site.Name << " " << x << " " << y
                    << ": floor " << floor << ", the table's " << point.GetPositionZ();
                static float const AXES[4][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
                for (auto const& axis : AXES)
                {
                    G3D::Vector3 const from = VMAP::VMapMgr2::convertPositionToInternalRep(x, y, floor + 0.5f);
                    G3D::Vector3 const to = VMAP::VMapMgr2::convertPositionToInternalRep(x + axis[0] * tuning.Clearance,
                        y + axis[1] * tuning.Clearance, floor + 0.5f);
                    EXPECT_TRUE(tree.isInLineOfSight(from, to, VMAP::ModelIgnoreFlags::Nothing)) << site.Name << " "
                        << x << " " << y << " is within " << tuning.Clearance << " yd of a wall";
                }
            }
    std::cout << "  " << points << " site points on the floor and clear\n";
}

TEST(DeadminesSitesDataTest, OnlyTheShutDoorShowsTheFarSide)
{
    char const* data = Env("FORGE_VISION_DATA");
    if (!data)
        GTEST_SKIP() << "FORGE_VISION_DATA is not set";
    VMAP::StaticMapTree tree(DEADMINES, std::string(data) + "vmaps");
    ASSERT_TRUE(Load(tree));
    ASSERT_EQ(SitesOf().size(), 4u);
    for (Cu::InteractSite const& site : SitesOf())
    {
        Placed const* door = Find(DOORS, site.Door);
        Placed const* opener = Find(OPENERS, site.Opener);
        ASSERT_NE(door, nullptr) << site.Name;
        ASSERT_NE(opener, nullptr) << site.Name;

        // The opener within reach of the near side: a seat stands within a few yards of it.
        float nearest = 1e9f;
        for (Position const& point : site.Near)
            nearest = std::min(nearest, point.GetExactDist2d(opener->X, opener->Y));
        EXPECT_LE(nearest, 8.0f) << site.Name << "'s opener is " << nearest << " yd from its near side";

        // Every line of sight from the near side to the far side goes through the shut door.
        uint32 seen = 0;
        uint32 pairs = 0;
        for (Position const& near : site.Near)
            for (Position const& far : site.Far)
            {
                ++pairs;
                G3D::Vector3 const eye(near.GetPositionX(), near.GetPositionY(), near.GetPositionZ() + EYE);
                G3D::Vector3 const object(far.GetPositionX(), far.GetPositionY(), far.GetPositionZ() + OBJECT_CENTRE);
                if (!tree.isInLineOfSight(VMAP::VMapMgr2::convertPositionToInternalRep(eye.x, eye.y, eye.z),
                    VMAP::VMapMgr2::convertPositionToInternalRep(object.x, object.y, object.z),
                    VMAP::ModelIgnoreFlags::Nothing))
                    continue;
                ++seen;
                EXPECT_TRUE(ThroughLeaf(*door, eye, object)) << site.Name << ": (" << eye.x << " " << eye.y
                    << ") sees (" << object.x << " " << object.y << ") past the door";
            }
        std::cout << "  " << site.Name << ": " << site.Near.size() << " near, " << site.Far.size() << " far, " << seen
            << " of " << pairs << " sight lines open with the door gone\n";
    }
}

TEST(DeadminesSitesDataTest, AuthoringScan)
{
    char const* data = Env("FORGE_DEADMINES_SCAN");
    if (!data)
        GTEST_SKIP() << "FORGE_DEADMINES_SCAN is not set";
    VMAP::StaticMapTree tree(DEADMINES, std::string(data) + "vmaps");
    ASSERT_TRUE(Load(tree));
    std::shared_ptr<dtNavMesh> mesh = MMAP::MMapMgr::LoadNavMesh(DEADMINES);
    ASSERT_TRUE(mesh);
    for (int32 x = 29; x <= 36; ++x)
        for (int32 y = 29; y <= 36; ++y)
            MMAP::MMapMgr::LoadTile(mesh.get(), DEADMINES, x, y);
    MMAP::ManagedNavMeshQuery query = MMAP::MMapMgr::CreateNavMeshQuery(mesh.get());
    dtQueryFilter filter;

    float const step = Env("STEP") ? float(std::atof(Env("STEP"))) : 0.5f;
    float const x0 = Env("X0") ? float(std::atof(Env("X0"))) : -340.0f;
    float const x1 = Env("X1") ? float(std::atof(Env("X1"))) : 40.0f;
    float const y0 = Env("Y0") ? float(std::atof(Env("Y0"))) : -940.0f;
    float const y1 = Env("Y1") ? float(std::atof(Env("Y1"))) : -300.0f;
    for (float x = x1; x >= x0; x -= step)
        for (float y = y1; y >= y0; y -= step)
        {
            // Every storey: a nearest polygon looked for round each of a column of heights, kept once per floor.
            std::set<int32> floors;
            for (float z = -20.0f; z <= 90.0f; z += 5.0f)
            {
                // Detour's frame is (y, z, x).
                float const centre[3] = { y, z, x };
                float const extents[3] = { 0.2f, 2.6f, 0.2f };
                dtPolyRef poly = 0;
                float nearest[3] = {};
                if (!dtStatusSucceed(query->findNearestPoly(centre, extents, &filter, &poly, nearest)) || !poly
                    || std::fabs(nearest[0] - y) > 0.05f || std::fabs(nearest[2] - x) > 0.05f)
                    continue;
                if (!floors.insert(int32(std::lround(nearest[1] * 2.0f))).second)
                    continue;
                float const floor = FloorAt(tree, x, y, nearest[1] + 1.5f, 4.0f);
                std::cout << "P " << x << " " << y << " " << nearest[1] << " "
                    << (std::isnan(floor) ? 100.0f : floor) << "\n";
            }
        }
}
