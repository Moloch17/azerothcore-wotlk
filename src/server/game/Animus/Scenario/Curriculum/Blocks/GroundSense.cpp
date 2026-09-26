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

#include "GroundSense.h"
#include "DetourExtended.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapDefines.h"
#include "MoveBlock.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    constexpr int NAV_RAY_POLYS = 16;           // polygons a single bearing's raycast may cross
    /// The most samples a dense march takes: MARCH_MAX at the finest pitch it is asked for.
    constexpr uint32 DENSE_SAMPLES_MAX = 400;
}

namespace Animus::Curriculum::GroundSense
{
    dtPolyRef StartPoly(dtNavMeshQuery const* query, Origin const& at)
    {
        if (!query)
            return 0;

        // Extents match the core's own lookup in cs_mmaps; a place the mesh does not cover gets no rays, and the
        // height march still answers.
        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);
        float const point[3] = { at.Y, at.Z, at.X };
        float const extents[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef startRef = 0;
        if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &startRef, nullptr)))
            return 0;
        return startRef;
    }

    /// How far the seat could walk along `heading` before it leaves the navmesh, up to `range`.
    ///
    /// This is the sense the block did not have. MarchBearing samples the *height* of the ground at five points
    /// and blocks on a change between two of them, so a vertical wall standing on a flat floor returns the same
    /// z at six yards and at twelve: the step is zero, nothing blocks, and the ray reports clear ground straight
    /// through the wall. It detected slopes and drops, never obstacles. Outdoors that passes, because a cliff is
    /// a height change; indoors every wall, door frame and table is a vertical face on a level floor and the
    /// seat walked into all of them blind.
    ///
    /// A navmesh raycast stops where walkable space stops, which is what an obstacle is to a pair of legs. It is
    /// also per-polygon, so unlike a height sample it cannot be confused by the storey above.
    ///
    /// **The filter is what makes this three senses instead of one.** dtNavMeshQuery::raycast tests
    /// `filter->passFilter()` on every polygon it steps into, and the mesh carries liquid as its own area flags
    /// (TerrainBuilder: water and ocean are NAV_WATER, magma NAV_MAGMA, slime NAV_SLIME). So NAV_GROUND alone
    /// stops at the water's edge and gives the distance to the shore; NAV_GROUND | NAV_WATER crosses the water
    /// and stops at the far side; and the difference between the two is how wide the water is that way -- which
    /// is exactly what "is this crossing worth it" needs and what no observation has ever carried. Leaving magma
    /// and slime out of both is what makes a lava edge a continuous distance rather than a flag sampled at five
    /// points, which a seat could walk straight between.
    ///
    /// Flags are always set explicitly: a default-constructed dtQueryFilterExt includes 0xffff and would happily
    /// cross slime, and the runtime's own filter never includes NAV_SLIME at all (GetNavTerrain folds slime into
    /// NAV_MAGMA), so neither default is the one wanted here.
    /// Returns yards to the first polygon edge the filter refuses to cross, or a negative number when there is
    /// no answer -- off the mesh, or a failed query. That distinction matters: a failed obstacle ray that read
    /// as `range` would be a seat told the way is clear to the horizon because the question could not be asked.
    /// Every caller must decide what silence means for what it is asking, and none of them may treat it as
    /// clear ground.
    float NavRay(dtNavMeshQuery const* query, dtPolyRef startRef, float x, float y, float z, float heading,
        float range, uint16 includeFlags)
    {
        if (!query || !startRef)
            return -1.0f;

        dtQueryFilterExt filter;
        filter.setIncludeFlags(includeFlags);
        filter.setExcludeFlags(0);

        // Detour's axes are {y, z, x}, not the world's (x, y, z). Getting this wrong is silent -- the ray simply
        // goes somewhere else -- so it is written out rather than swizzled in passing.
        float const from[3] = { y, z, x };
        float const to[3] = { y + range * std::sin(heading), z, x + range * std::cos(heading) };

        float t = 0.0f;
        float normal[3] = { 0.0f, 0.0f, 0.0f };
        dtPolyRef visited[NAV_RAY_POLYS];
        int count = 0;
        if (dtStatusFailed(query->raycast(startRef, from, to, &filter, &t, normal, visited, &count,
            NAV_RAY_POLYS)))
            return -1.0f;

        // Detour reports t = FLT_MAX when the ray ran the whole way without leaving the mesh.
        return t >= 1.0f ? range : t * range;
    }

    /// March one bearing outward and say where it stops.
    ///
    /// This replaces a single height sample twelve yards out, compared against the seat's own feet. That sample
    /// answered "is the point twelve yards that way roughly level with me", which is three different questions
    /// short of the one a pair of legs is asking. It could not see past twelve yards, it read a gentle slope as
    /// a wall because MAX_STEP was measured over the whole twelve, and a wall, a cliff, a lava lake and the edge
    /// of the map all came back as the same 0.
    ///
    /// What comes back now is the distance to the first thing that stops the ray, which means the same at six
    /// yards as at forty, plus what stopped it: the signed height change, whether there was water it could swim,
    /// and whether there was liquid that burns. Each cell is judged against the cell before it, so ground that
    /// climbs steadily stays walkable and only a real discontinuity blocks.
    ///
    /// Still geometry and not a route. Nothing here says which way to go.
    ///
    /// Dense (`pitch` above 0) it is the same judgement made every `pitch` yards: a sample against the one before
    /// it is the step test, with the allowance its own short gap earns, and a sample against the ground
    /// MARCH_WINDOW behind it is the slope test, with the allowance the first legacy cell earned over the same
    /// distance -- so ground that climbs admits exactly what it did, and a fence, a gap or a ledge narrower
    /// than a cell no longer falls between two samples.
    March MarchBearing(Map* map, Origin const& at, float heading, float pitch)
    {
        March march;
        float const dx = std::cos(heading);
        float const dy = std::sin(heading);

        uint32 const samples = pitch > 0.0f
            ? std::min(DENSE_SAMPLES_MAX, uint32(std::lround(MoveBlock::MARCH_MAX / pitch)))
            : MoveBlock::MARCH_CELLS;
        // The ground under every sample so far, for the slope window: the origin first.
        std::array<float, DENSE_SAMPLES_MAX + 1> ground{};
        std::array<float, DENSE_SAMPLES_MAX + 1> ranges{};
        ground[0] = at.Z;
        ranges[0] = 0.0f;
        uint32 window = 0;              // the sample the slope is judged from, MARCH_WINDOW or more behind

        float previousZ = at.Z;
        float previousRange = 0.0f;
        float free = 0.0f;
        float blockedStep = 0.0f;
        float worstStep = 0.0f;
        bool blocked = false;

        for (uint32 cell = 0; cell < samples && !blocked; ++cell)
        {
            float const range = pitch > 0.0f
                ? std::min(MoveBlock::MARCH_MAX, float(cell + 1) * pitch)
                : MoveBlock::MARCH_RANGES[cell];
            float const gap = range - previousRange;
            float const allowance = MoveBlock::MAX_STEP + gap * MoveBlock::MARCH_SLOPE;
            float const x = at.X + range * dx;
            float const y = at.Y + range * dy;

            // Liquid before ground, because the ground test cannot tell a lake from a cliff and would call it
            // the latter: mmaps drops the terrain under real liquid, so GetHeight comes back INVALID_HEIGHT over
            // any water worth swimming -- the same answer it gives for the edge of the map. Which liquid it is
            // decides everything, and LiquidData::Flags is what carries it; Status only says how deep the stuff
            // is, so a test on Status alone called magma "water" and handed the seat a lava lake to cross.
            LiquidData const liquid = map->GetLiquidData(at.Phase, x, y, previousZ, at.Collision, {});
            bool const liquidHere = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT
                && liquid.Level >= previousZ - allowance;

            if (liquidHere && (liquid.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0)
            {
                // Somewhere to die, not somewhere to go. The ray stops short of it, and the seat can tell this
                // apart from a wall because burns says so.
                march.Burns = 1.0f;
                blocked = true;
                break;
            }

            float z = 0.0f;
            if (liquidHere && (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0)
            {
                // Water is somewhere the seat can go, so the ray carries on across it. What it costs to go there
                // is OBS_SWIM_SPEED's to say.
                march.Water = 1.0f;
                z = liquid.Level;
            }
            else
            {
                // Search from just above the last cell, downwards. The origin used to be twenty yards up, which
                // is harmless in open country and wrong inside a building: Map::GetHeight casts a strictly
                // downward ray from the z it is given, so starting above the ceiling returns the floor of the
                // storey above and the seat is told it can walk there. Starting a step's height up finds the
                // ground it could actually reach and nothing higher.
                z = map->GetHeight(at.Phase, x, y, previousZ + MoveBlock::MAX_STEP, true,
                    MoveBlock::MARCH_SEARCH);
                if (z <= INVALID_HEIGHT)
                {
                    // No ground within twenty yards either way: a long drop, or off the map. Reported as a
                    // drop, because that is what it is to something on legs.
                    blockedStep = -1.0f;
                    blocked = true;
                    break;
                }

                float const step = z - previousZ;
                if (std::fabs(step) > allowance)
                {
                    blockedStep = std::clamp(step / allowance, -1.0f, 1.0f);
                    blocked = true;
                    break;
                }

                // The legacy cells are each their own window. A dense march judges the slope over the last
                // MARCH_WINDOW yards, from the sample at least that far behind (the origin until there is one).
                float slope = step / allowance;
                if (pitch > 0.0f)
                {
                    while (window + 1 <= cell && range - ranges[window + 1] >= MARCH_WINDOW)
                        ++window;
                    float const span = range - ranges[window];
                    float const spanAllowance = MoveBlock::MAX_STEP + span * MoveBlock::MARCH_SLOPE;
                    float const climb = z - ground[window];
                    if (std::fabs(climb) > spanAllowance)
                    {
                        blockedStep = std::clamp(climb / spanAllowance, -1.0f, 1.0f);
                        blocked = true;
                        break;
                    }
                    slope = climb / spanAllowance;
                }

                // The five cells keep the comparison they always made, which weighs the new step in yards
                // against the worst so far as a share of its allowance: the probe the policies were trained on.
                // The dense march compares like with like.
                if (pitch > 0.0f ? std::fabs(slope) > std::fabs(worstStep) : std::fabs(step) > std::fabs(worstStep))
                    worstStep = slope;
            }

            if (pitch > 0.0f)
            {
                ground[cell + 1] = z;
                ranges[cell + 1] = range;
            }
            previousZ = z;
            previousRange = range;
            free = range;
        }

        march.Reach = free / MoveBlock::MARCH_MAX;
        // What stopped the ray if something did, and otherwise the steepest thing it walked over -- so a bearing
        // that is clear but climbing still reads differently from one that is clear and flat.
        march.Step = blocked ? blockedStep : std::clamp(worstStep, -1.0f, 1.0f);
        return march;
    }

    Rays CastRays(dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at, float heading)
    {
        // Three rays, which differ only in what their filter will cross. The dry one walks ground alone, so it
        // stops at a shore, a lava edge or a wall. The wet one may cross water, so it stops at a lava edge or a
        // wall. The last crosses everything liquid, so it stops only where the mesh itself ends.
        //
        // What the pair is good for is `dry` itself: the yards to the water's edge along this bearing,
        // continuous, where the plane it replaced was a yes or no sampled at five fixed ranges. It does not give
        // the width of the crossing -- the wet filter crosses ground too, so past a shore it runs on over the far
        // bank until a wall stops it, and a narrow channel reads the same as a lake.
        //
        // wet against all is the pair that does mean exactly one thing, because those two filters differ in
        // nothing but magma and slime: if the ray that may not cross them stops short of the ray that may, what
        // stopped it was burning, and it stopped at the burning edge.
        Rays rays;
        rays.Wet = NavRay(query, startRef, at.X, at.Y, at.Z, heading, MoveBlock::MARCH_MAX, NAV_GROUND | NAV_WATER);
        rays.Dry = NavRay(query, startRef, at.X, at.Y, at.Z, heading, MoveBlock::MARCH_MAX, NAV_GROUND);
        rays.All = NavRay(query, startRef, at.X, at.Y, at.Z, heading, MoveBlock::MARCH_MAX,
            NAV_GROUND | NAV_WATER | NAV_MAGMA | NAV_SLIME);
        return rays;
    }

    Bearing Combine(March const& march, Rays const& rays)
    {
        Bearing bearing;
        bearing.Reach = march.Reach;
        bearing.Step = march.Step;
        bearing.Burns = march.Burns;

        // The nearer of the two senses wins: the march sees drops the mesh calls walkable, the ray sees walls the
        // march is blind to, and a seat wants to know about whichever comes first.
        if (rays.Wet >= 0.0f && rays.Dry >= 0.0f)
        {
            bearing.Reach = std::min(bearing.Reach, rays.Wet / MoveBlock::MARCH_MAX);
            bearing.Shore = std::min(rays.Dry / MoveBlock::MARCH_MAX, bearing.Reach);
        }
        else
        {
            // No mesh here, or no answer from it. Fall back to what the height march saw, and say the dry reach
            // is the reach -- claiming water of unknown width would be worse than claiming none.
            bearing.Shore = march.Water > 0.0f ? 0.0f : bearing.Reach;
        }

        // Where the burning starts, graded by how close it is: 1 at the seat's feet, falling to 0 at the far end
        // of the march, and exactly 0 when there is none.
        //
        // This is the ray's answer and not the march's, because the march cannot answer it. It samples liquid at
        // five fixed ranges, so a lava edge at nine yards falls between the cells at six and at twelve and reads
        // as no lava at all -- and worse, if the twelve-yard cell lands past the edge it returns no height, which
        // the march reports as a drop. That is a seat walking into lava believing it is stepping off a ledge.
        // The mesh is built from the same liquid data, so when it answers it answers about the same lava, only
        // continuously and at the true edge.
        if (rays.All >= 0.0f && rays.Wet >= 0.0f)
        {
            // BURN_EDGE_MARGIN guards float noise between two rays cast from one origin; it is not the mesh's own
            // 1.8 yd simplification error, which both rays share and which therefore cancels.
            bearing.Burns = rays.All > rays.Wet + MoveBlock::BURN_EDGE_MARGIN
                ? 1.0f - std::clamp(rays.Wet / MoveBlock::MARCH_MAX, 0.0f, 1.0f)
                : 0.0f;
        }
        // else: no mesh answer, so the march's own flag stands as written, coarse but not silent.
        return bearing;
    }

    Bearing Sense(Map* map, dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at, float heading,
        float pitch)
    {
        return Combine(MarchBearing(map, at, heading, pitch), CastRays(query, startRef, at, heading));
    }

    Bearing Worst(Bearing const& a, Bearing const& b)
    {
        Bearing worst = a.Reach <= b.Reach ? a : b;
        worst.Shore = std::min(a.Shore, b.Shore);
        worst.Burns = std::max(a.Burns, b.Burns);
        return worst;
    }

    Bearing SenseWedge(Map* map, dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at, float heading,
        float halfWidth, uint32 rays, float pitch)
    {
        if (rays <= 1)
            return Sense(map, query, startRef, at, heading, pitch);

        // Evenly across the wedge, edges included: rays - 1 gaps from one side to the other.
        Bearing worst;
        for (uint32 ray = 0; ray < rays; ++ray)
        {
            float const offset = -halfWidth + 2.0f * halfWidth * float(ray) / float(rays - 1);
            Bearing const one = Sense(map, query, startRef, at, heading + offset, pitch);
            worst = ray == 0 ? one : Worst(worst, one);
        }
        return worst;
    }

    Room MeasureRoom(dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at)
    {
        // How much room there is, and which way is out. The filter is the walkable set a seat actually uses, so a
        // lava edge and a shoreline both count as an edge to keep off -- which is the honest answer for something
        // on legs.
        Room room;
        if (!query || !startRef)
            return room;

        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);
        float const point[3] = { at.Y, at.Z, at.X };
        float distance = 0.0f;
        float hit[3] = { 0.0f, 0.0f, 0.0f };
        float normal[3] = { 0.0f, 0.0f, 0.0f };
        if (!dtStatusSucceed(query->findDistanceToWall(startRef, point, MoveBlock::CLEARANCE_RANGE, &filter,
            &distance, hit, normal)))
            return room;

        if (std::isfinite(distance))
            room.Clearance = std::clamp(distance / MoveBlock::CLEARANCE_RANGE, 0.0f, 1.0f);

        // hitNormal is normalize(centre - hit): it already points from the wall back at the seat, which is the
        // way out. Detour's axes are {y, z, x}, so the world components are [2] and [0].
        //
        // And it is not always a direction. Detour builds that vector by subtracting the hit from the centre and
        // normalising in place, dividing by the vector's own length -- so a seat standing exactly on an edge,
        // where the hit *is* the centre, normalises a zero vector and gets three NaNs. atan2 carries them, sin
        // and cos carry them, and two NaN observation planes reach the networks, which return a NaN logit, which
        // torch.multinomial reports as a probability tensor containing inf or nan, naming nothing. A seat on an
        // edge is not rare: it is a doorway.
        //
        // There is no direction out of a point that is already on the edge, so the honest reading is the one the
        // probe starts with -- no direction at all -- and the clearance distance beside it still says the seat
        // is against something.
        //
        // And it is only a direction when a wall was found at all. findDistanceToWall writes hitPos only inside
        // its loop, on a hit, but subtracts and normalises unconditionally at the end -- so with nothing in range
        // it normalises (centre - {0, 0, 0}), which is the bearing from the world origin to the seat. That is
        // finite, non-zero and completely wrong: it would hand the policy an absolute compass reading of where in
        // the world it is standing, in open country, where the held-out arenas exist precisely to prove it is
        // reading terrain and not remembering places. Nothing further out than the search radius has a way out
        // to point at.
        float const outX = normal[2];
        float const outY = normal[0];
        if (distance < MoveBlock::CLEARANCE_RANGE
            && std::isfinite(outX) && std::isfinite(outY) && outX * outX + outY * outY > 1e-6f)
        {
            room.Directed = true;
            room.Away = std::atan2(outY, outX);
        }
        return room;
    }
}
