/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "RoutePlanner.h"
#include "DetourExtended.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace
{
    /// Nodes the planner's own query may open. The core's is 1024, which is a ceiling on how far a single
    /// search reaches before it gives up and answers partially -- fine for a creature walking to its next
    /// patrol point, not for a route across a zone. Detour's own limit is 65535.
    constexpr int ROUTE_NODES = 65535;

    /// Polygons one corridor may cross, and corners one route may turn at.
    constexpr int ROUTE_POLYS = 2048;

    /// The box findNearestPoly looks in, matching the core's own lookup in cs_mmaps and everywhere else.
    constexpr float EXTENT_XY = 3.0f;
    constexpr float EXTENT_Z = 5.0f;

    /// One query per navmesh, keyed on the mesh rather than the map id: instanced maps share their parent's
    /// mesh through a shared_ptr, so keying on the id would build the same query many times over.
    std::unordered_map<dtNavMesh const*, dtNavMeshQuery*> g_queries;

    dtNavMeshQuery* QueryFor(Map* map)
    {
        if (!map)
            return nullptr;

        dtNavMesh const* mesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        if (!mesh)
            return nullptr;

        auto found = g_queries.find(mesh);
        if (found != g_queries.end())
            return found->second;

        dtNavMeshQuery* query = dtAllocNavMeshQuery();
        if (!query)
            return nullptr;

        if (dtStatusFailed(query->init(mesh, ROUTE_NODES)))
        {
            dtFreeNavMeshQuery(query);
            return nullptr;
        }

        g_queries.emplace(mesh, query);
        return query;
    }

    /// The set a seat may walk. Magma and slime are left out deliberately -- a route through lava is not a
    /// route -- and the flags are always set rather than defaulted, because a default-constructed filter
    /// includes 0xffff and would happily plan through both.
    void Configure(dtQueryFilterExt& filter)
    {
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);
    }

    float Flat(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }
}

float Animus::Curriculum::Route::RemainingFrom(float x, float y, float /*z*/) const
{
    if (!Valid || Count == 0)
        return -1.0f;

    if (Count == 1)
        return Flat(x, y, X[0], Y[0]);

    // The seat's closest point on the route, and the arc length from there -- not "distance to the next corner
    // plus the total from it".
    //
    // The difference is not pedantry, it is a reward exploit. Distance-to-the-next-corner steps down every time
    // a corner is passed: a seat five yards short of a turn is five yards plus the rest, and the instant the
    // corner is counted as reached the five yards vanish, because the leg to the *following* corner is measured
    // from the turn rather than from the seat. Potential shaping pays for that drop. It is free, it is worth
    // about five yards a corner, and it repeats -- the first measurement of it had failing episodes earning
    // 0.687 of progress reward against arrivals' 0.386, which is to say wandering paid better than arriving.
    //
    // Projecting removes it: the value is continuous everywhere, including across a corner, so the only way to
    // make it fall is to actually get closer to the objective.
    float best = -1.0f;
    for (uint32 i = 0; i + 1 < Count; ++i)
    {
        float const sx = X[i];
        float const sy = Y[i];
        float const dx = X[i + 1] - sx;
        float const dy = Y[i + 1] - sy;
        float const span = dx * dx + dy * dy;

        float t = span > 0.0f ? ((x - sx) * dx + (y - sy) * dy) / span : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);

        float const px = sx + t * dx;
        float const py = sy + t * dy;
        float const total = Flat(x, y, px, py) + Flat(px, py, X[i + 1], Y[i + 1]) + Remaining[i + 1];
        if (best < 0.0f || total < best)
            best = total;
    }

    return best;
}

void Animus::Curriculum::Route::Advance(float x, float y, float /*z*/, float reachedWithin)
{
    if (!Valid)
        return;

    // Next is a steering hint -- which way to walk now -- and nothing else. It is deliberately not part of
    // RemainingFrom, which projects instead, so moving it can no longer move the reward.
    //
    // Every corner already reached, not only the next: a seat carried past two of them by one spline step
    // should not spend the following decisions walking back to the first.
    while (Next + 1 < Count && Flat(x, y, X[Next], Y[Next]) <= reachedWithin)
        ++Next;
}

Animus::Curriculum::RoutePlanner& Animus::Curriculum::RoutePlanner::Instance()
{
    static RoutePlanner planner;
    return planner;
}

Animus::Curriculum::RoutePlanner::~RoutePlanner()
{
    for (auto& [mesh, query] : g_queries)
        dtFreeNavMeshQuery(query);

    g_queries.clear();
}

bool Animus::Curriculum::RoutePlanner::Plan(Map* map, Position const& from, Position const& to, Route& out)
{
    out.Clear();

    dtNavMeshQuery* query = QueryFor(map);
    if (!query)
        return false;

    dtQueryFilterExt filter;
    Configure(filter);

    // Detour's axes are {y, z, x}, not the world's. Written out rather than swizzled in passing, because
    // getting it wrong is silent: the search simply happens somewhere else and answers about the wrong place.
    float const start[3] = { from.GetPositionY(), from.GetPositionZ(), from.GetPositionX() };
    float const end[3] = { to.GetPositionY(), to.GetPositionZ(), to.GetPositionX() };
    float const extents[3] = { EXTENT_XY, EXTENT_Z, EXTENT_XY };

    dtPolyRef startRef = 0;
    dtPolyRef endRef = 0;
    float startPos[3] = {};
    float endPos[3] = {};
    if (dtStatusFailed(query->findNearestPoly(start, extents, &filter, &startRef, startPos)) || !startRef)
        return false;
    if (dtStatusFailed(query->findNearestPoly(end, extents, &filter, &endRef, endPos)) || !endRef)
        return false;

    dtPolyRef corridor[ROUTE_POLYS];
    int polys = 0;
    if (dtStatusFailed(query->findPath(startRef, endRef, startPos, endPos, &filter, corridor, &polys,
        ROUTE_POLYS)) || polys <= 0)
        return false;

    // findPath answers with the corridor it could reach. If it does not end on the objective's own polygon the
    // way runs out somewhere short, and the honest end of the route is the last polygon it did reach -- which
    // is what closestPointOnPoly gives. Reported rather than refused: a partial way is still a way to set off
    // along, and Complete is how a caller tells the difference.
    bool const complete = corridor[polys - 1] == endRef;
    float target[3] = { endPos[0], endPos[1], endPos[2] };
    if (!complete && dtStatusFailed(query->closestPointOnPoly(corridor[polys - 1], end, target, nullptr)))
        return false;

    float corners[Route::MAX_CORNERS * 3];
    unsigned char flags[Route::MAX_CORNERS];
    dtPolyRef visited[Route::MAX_CORNERS];
    int count = 0;
    if (dtStatusFailed(query->findStraightPath(startPos, target, corridor, polys, corners, flags, visited,
        &count, int(Route::MAX_CORNERS))) || count <= 0)
        return false;

    out.Count = uint32(count);
    for (int i = 0; i < count; ++i)
    {
        // Back out of Detour's axes the same way PathGenerator does.
        out.X[i] = corners[i * 3 + 2];
        out.Y[i] = corners[i * 3 + 0];
        out.Z[i] = corners[i * 3 + 1];
    }

    // Length from each corner to the end, walked backwards so every entry is a running total. This is the
    // whole reason the route is worth caching: with it, the distance left from anywhere costs one subtraction.
    out.Remaining[count - 1] = 0.0f;
    for (int i = count - 2; i >= 0; --i)
        out.Remaining[i] = out.Remaining[i + 1] + Flat(out.X[i], out.Y[i], out.X[i + 1], out.Y[i + 1]);

    out.Length = out.Remaining[0];
    out.Complete = complete;
    out.From.Relocate(from);
    out.To.Relocate(to);
    out.Next = out.Count > 1 ? 1 : 0;   // corner 0 is where the seat already is
    out.Valid = true;
    return true;
}

std::string Animus::Curriculum::RoutePlanner::Report(Map* map, Position const& from, Position const& to)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(2);

    if (!map)
        return "no map\n";
    if (!map->GetMapCollisionData().GetMMapData().GetNavMesh())
        return "no navmesh on this map -- mmaps are not loaded for it\n";

    Route route;
    if (!Plan(map, from, to, route))
    {
        text << "  no route: one of the ends is off the mesh, or nothing connects them\n";
        return text.str();
    }

    float const crow = Flat(from.GetPositionX(), from.GetPositionY(), to.GetPositionX(), to.GetPositionY());
    text << "  " << (route.Complete ? "complete" : "PARTIAL -- the way runs out short of the objective")
         << "\n";
    text << "  corners      " << route.Count << "\n";
    text << "  length       " << route.Length << " yd against " << crow << " yd straight line ("
         << (crow > 0.0f ? route.Length / crow : 0.0f) << "x)\n";
    if (!route.Complete)
        text << "  ends at      (" << route.X[route.Count - 1] << ", " << route.Y[route.Count - 1] << ", "
             << route.Z[route.Count - 1] << "), " << Flat(route.X[route.Count - 1], route.Y[route.Count - 1],
             to.GetPositionX(), to.GetPositionY()) << " yd short\n";

    // The first few turns, which is what a seat would actually be told to walk.
    uint32 const show = std::min<uint32>(route.Count, 8);
    text << "\n  corner            x          y          z     left\n";
    text << "  ------  ----------  ---------  ---------  -------\n";
    for (uint32 i = 0; i < show; ++i)
        text << "  " << std::setw(6) << i << "  " << std::setw(10) << route.X[i] << "  " << std::setw(9)
             << route.Y[i] << "  " << std::setw(9) << route.Z[i] << "  " << std::setw(7)
             << route.Remaining[i] << "\n";
    if (route.Count > show)
        text << "  ... " << (route.Count - show) << " more\n";

    return text.str();
}
