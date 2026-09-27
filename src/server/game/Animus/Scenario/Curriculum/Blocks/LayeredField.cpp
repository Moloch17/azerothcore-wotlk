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

#include "LayeredField.h"
#include "DetourExtended.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Log.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include "MoveBlock.h"
#include "Object.h"
#include "ProbeBake.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <zstd.h>

namespace
{
    namespace Field = Animus::Curriculum::LayeredField;
    using Animus::Curriculum::MoveBlock;
    using Animus::Curriculum::SENSE_RAYS;

    /// How far up a floor's headroom is looked for; nothing solid within it is open sky.
    constexpr float HEADROOM_SEARCH = 100.0f;
    /// Less room than this above a floor is no room: the floor is a surface seen from inside something solid.
    constexpr float MIN_HEADROOM = 0.5f;
    /// Floors closer together than this in one column are one floor.
    constexpr float FLOOR_MERGE = 0.5f;
    /// Where the topmost surface is looked for from: this far above the terrain.
    constexpr float SKY_ABOVE = 600.0f;
    /// At most this many surfaces are looked for in one column.
    constexpr uint32 MAX_SURFACES = 48;
    /// A liquid's surface up to this far below a floor is still that floor's liquid: the march asks whether there
    /// is liquid within a step and a slope of it, so a jetty above a lake has the lake.
    constexpr float LIQUID_BELOW = 10.0f;
    /// How far up or down the navmesh's surface may go from one sample of a ray to the next and still be the same
    /// surface: more than a detour climb or a slope's half yard, less than a storey.
    constexpr float NAV_CLIMB = 2.0f;
    /// How far StartPoly looks for the seat's polygon: 3 yd across, 5 up and down (its extents).
    constexpr float START_ACROSS = 3.0f;
    constexpr float START_UP = 5.0f;
    constexpr uint8 NAV_WALKABLE = NAV_GROUND | NAV_WATER;
    constexpr uint8 NAV_ANY = NAV_GROUND | NAV_WATER | NAV_MAGMA | NAV_SLIME;
    constexpr uint8 OPEN_ABOVE = Field::Interval::OPEN_ABOVE;
    constexpr float TWO_PI_F = 2.0f * float(M_PI);
    /// The seat's wedge and its rays: ProbeBake's standard bake, which the live fallback measures too.
    constexpr float SEAT_HALF_WEDGE = float(M_PI) / float(Animus::Curriculum::SENSE_RAYS);
    constexpr uint32 DENSE_SAMPLES_MAX = 400;

    struct QueryOwner
    {
        dtNavMeshQuery* Query = nullptr;

        explicit QueryOwner(dtNavMesh const* mesh)
        {
            if (!mesh)
                return;
            Query = dtAllocNavMeshQuery();
            if (Query && dtStatusFailed(Query->init(mesh, 1024)))
            {
                dtFreeNavMeshQuery(Query);
                Query = nullptr;
            }
        }

        ~QueryOwner()
        {
            if (Query)
                dtFreeNavMeshQuery(Query);
        }

        QueryOwner(QueryOwner const&) = delete;
        QueryOwner& operator=(QueryOwner const&) = delete;
    };

    /// The navmesh's floors in the column at (x, y), every flag included: each polygon's height there, snapped to the
    /// ground the core reports within a step of it (ProbeBake::Floors' rule), with the polygon's flags.
    std::vector<std::pair<float, uint8>> NavFloors(Map* map, dtNavMeshQuery const* query, float x, float y)
    {
        std::vector<std::pair<float, uint8>> floors;
        if (!query)
            return floors;
        dtNavMesh const* mesh = query->getAttachedNavMesh();
        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_ANY);
        filter.setExcludeFlags(0);
        float const centre[3] = { y, 0.0f, x };
        float const extents[3] = { 0.01f, 5000.0f, 0.01f };
        dtPolyRef polys[64];
        int count = 0;
        if (dtStatusFailed(query->queryPolygons(centre, extents, &filter, polys, &count, 64)))
            return floors;
        for (int index = 0; index < count; ++index)
        {
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mesh->getTileAndPolyByRef(polys[index], &tile, &poly))
                || poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;
            float height = 0.0f;
            if (dtStatusFailed(query->getPolyHeight(polys[index], centre, &height)))
                continue;
            float const ground = map->GetHeight(PHASEMASK_NORMAL, x, y, height + MoveBlock::MAX_STEP, true,
                MoveBlock::MAX_STEP * 2.0f);
            floors.emplace_back(ground > INVALID_HEIGHT && std::fabs(ground - height) < MoveBlock::MAX_STEP
                ? ground : height, uint8(poly->flags & NAV_ANY));
        }
        return floors;
    }

    std::vector<Field::Interval> Column(Map* map, dtNavMeshQuery const* query, float x, float y)
    {
        StaticVMapCollisionData const& collision = map->GetMapCollisionData().GetStaticTree();
        float const terrain = map->GetGridHeight(x, y);

        // The flags byte carries OPEN_ABOVE while the column is being built.
        std::vector<std::pair<float, uint8>> floors = NavFloors(map, query, x, y);
        for (auto& floor : floors)
            floor.second |= OPEN_ABOVE;
        if (terrain > INVALID_HEIGHT)
            floors.emplace_back(terrain, OPEN_ABOVE);
        // Every surface from the sky down: what Map::GetHeight could answer from any height in the column.
        float from = (terrain > INVALID_HEIGHT ? terrain : 0.0f) + SKY_ABOVE;
        for (uint32 surface = 0; surface < MAX_SURFACES; ++surface)
        {
            float const hit = collision.getHeight(x, y, from, SKY_ABOVE * 4.0f);
            if (hit <= INVALID_HEIGHT)
                break;
            floors.emplace_back(hit, surface == 0 ? OPEN_ABOVE : uint8(0));
            from = hit - 0.02f;
        }
        std::sort(floors.begin(), floors.end());

        // Floors closer than FLOOR_MERGE are one floor (a polygon and the ground under it, a slab's two faces): the
        // highest of them, with every navmesh flag any of them had.
        std::vector<std::pair<float, uint8>> merged;
        float groupStart = 0.0f;
        for (auto const& [floor, flags] : floors)
        {
            if (!merged.empty() && floor - groupStart <= FLOOR_MERGE)
            {
                merged.back().first = floor;
                merged.back().second |= flags;
                continue;
            }
            groupStart = floor;
            merged.emplace_back(floor, flags);
        }

        std::vector<Field::Interval> column;
        for (auto const& [floor, flags] : merged)
        {
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            bool const roofed = collision.GetObjectHitPos(x, y, floor + 0.05f, x, y, floor + HEADROOM_SEARCH, rx, ry,
                rz, 0.0f);
            float const headroom = roofed ? std::max(0.0f, rz - floor) : Field::OPEN_SKY;
            // A navmesh floor stays whatever the collision says above it: the mesh was built for the seat's height,
            // so a polygon is room enough by definition, and a ray from just above it can graze its own surface.
            if (headroom < MIN_HEADROOM && !(flags & NAV_ANY))
                continue;

            Field::Interval interval;
            interval.Floor8 = int16(std::clamp(std::lround(floor * 8.0f), -32767L, 32767L));
            interval.Headroom8 = headroom >= Field::OPEN_SKY ? Field::Interval::SKY
                : uint16(std::clamp(long(std::max(headroom, MIN_HEADROOM) * 8.0f), 1L, 65534L));
            interval.Flags = uint8(flags & (NAV_ANY | OPEN_ABOVE));
            LiquidData const liquid = map->GetLiquidData(PHASEMASK_NORMAL, x, y, floor + 0.1f, 2.0f, {});
            if (liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > floor - LIQUID_BELOW)
            {
                interval.Liquid8 = int16(std::clamp(std::lround((liquid.Level - interval.Floor()) * 8.0f), -32767L,
                    32767L));
                interval.LiquidFlags = uint8(liquid.Flags);
            }
            column.push_back(interval);
        }
        return column;
    }

    /// One cell's intervals, lowest first; empty off the grid.
    struct Span
    {
        Field::Interval const* Begin = nullptr;
        Field::Interval const* End = nullptr;
        bool OnGrid = false;
    };

    Span SpanOf(Field::Grid const& grid, float x, float y)
    {
        Span span;
        // The nearest cell centre: MinX is half a cell in from the grid's edge, so on the grid the rounded value is
        // never below zero and truncating it is rounding it.
        float const fx = (x - grid.MinX) / grid.Cell + 0.5f;
        float const fy = (y - grid.MinY) / grid.Cell + 0.5f;
        if (!(fx >= 0.0f && fy >= 0.0f))
            return span;
        int32 const column = int32(fx);
        int32 const row = int32(fy);
        if (column >= int32(grid.Side) || row >= int32(grid.Side))
            return span;
        uint32 const cell = uint32(row) * grid.Side + uint32(column);
        span.Begin = grid.Intervals.data() + grid.First[cell];
        span.End = grid.Intervals.data() + grid.First[cell + 1];
        span.OnGrid = true;
        return span;
    }

    Field::Grid const* GridAt(Field::View const& view, float x, float y)
    {
        int32 const gx = Animus::Curriculum::ProbeBake::GridIndex(x) - view.CentreX + 1;
        int32 const gy = Animus::Curriculum::ProbeBake::GridIndex(y) - view.CentreY + 1;
        if (gx < 0 || gy < 0 || gx > 2 || gy > 2)
            return nullptr;
        return view.Grids[gx][gy];
    }

    Span SpanAt(Field::View const& view, float x, float y)
    {
        Field::Grid const* grid = GridAt(view, x, y);
        return grid ? SpanOf(*grid, x, y) : Span{};
    }

    /// The centre of the cell holding (x, y), on its grid's lattice.
    bool CellCentre(Field::View const& view, float x, float y, float& cx, float& cy)
    {
        Field::Grid const* grid = GridAt(view, x, y);
        if (!grid)
            return false;
        cx = grid->MinX + std::round((x - grid->MinX) / grid->Cell) * grid->Cell;
        cy = grid->MinY + std::round((y - grid->MinY) / grid->Cell) * grid->Cell;
        return true;
    }

    /// Map::GetHeight from `from` downward, `search` deep: the highest floor at or under it.
    float HeightBelow(Span const& span, float from, float search)
    {
        float best = INVALID_HEIGHT;
        for (Field::Interval const* interval = span.Begin; interval != span.End; ++interval)
            if (interval->Floor() <= from + 0.05f && interval->Floor() >= from - search)
                best = interval->Floor();
        return best;
    }

    /// Map::GetLiquidData at height z: the highest liquid of the floors under it.
    Field::Interval const* LiquidAt(Span const& span, float z)
    {
        Field::Interval const* best = nullptr;
        for (Field::Interval const* interval = span.Begin; interval != span.End; ++interval)
            if (interval->Floor() <= z + MoveBlock::MAX_STEP && interval->HasLiquid()
                && (!best || interval->Liquid() > best->Liquid()))
                best = interval;
        return best;
    }

    /// The navmesh floor in a cell nearest z within `within`, or nullptr.
    Field::Interval const* NavFloor(Span const& span, float z, float within, uint8 flags)
    {
        Field::Interval const* best = nullptr;
        float bestGap = within;
        for (Field::Interval const* interval = span.Begin; interval != span.End; ++interval)
        {
            float const gap = std::fabs(interval->Floor() - z);
            if ((interval->NavFlags() & flags) && gap <= bestGap)
            {
                bestGap = gap;
                best = interval;
            }
        }
        return best;
    }

    /// The cell under a point that walks along a line: the grid it is on is looked up again only when it leaves it.
    struct Cursor
    {
        Field::View const& View;
        Field::Grid const* Grid = nullptr;
        float Low[2] = { 1.0f, 1.0f };      // the current grid's bounds; empty until a grid is found
        float High[2] = { 0.0f, 0.0f };

        explicit Cursor(Field::View const& view) : View(view) { }

        Span At(float x, float y)
        {
            if (!(x >= Low[0] && x < High[0] && y >= Low[1] && y < High[1]))
            {
                Grid = GridAt(View, x, y);
                if (!Grid)
                {
                    // No grid here: forget the last one's bounds too, or the next point inside them would be
                    // looked up on no grid at all.
                    Low[0] = Low[1] = 1.0f;
                    High[0] = High[1] = 0.0f;
                    return Span{};
                }
                Low[0] = float(Grid->GridX) * SIZE_OF_GRIDS;
                Low[1] = float(Grid->GridY) * SIZE_OF_GRIDS;
                High[0] = Low[0] + SIZE_OF_GRIDS;
                High[1] = Low[1] + SIZE_OF_GRIDS;
            }
            return SpanOf(*Grid, x, y);
        }
    };

    /// The seat's floor on the mesh, as StartPoly finds its polygon: the nearest walkable navmesh floor within 5 yd
    /// up or down, in the seat's cell or, failing that, the cells within 3 yd. INVALID_HEIGHT when there is none.
    float StartFloor(Field::View const& view, float x, float y, float z)
    {
        if (Field::Interval const* own = NavFloor(SpanAt(view, x, y), z, START_UP, NAV_WALKABLE))
            return own->Floor();
        float ax = 0.0f, ay = 0.0f;
        if (!CellCentre(view, x, y, ax, ay))
            return INVALID_HEIGHT;
        int32 const reach = int32(std::ceil(START_ACROSS / view.Cell));
        float best = INVALID_HEIGHT;
        float bestDistance = START_ACROSS * START_ACROSS + 1.0f;
        for (int32 dr = -reach; dr <= reach; ++dr)
            for (int32 dc = -reach; dc <= reach; ++dc)
            {
                float const cx = ax + float(dc) * view.Cell;
                float const cy = ay + float(dr) * view.Cell;
                float const distance = (cx - x) * (cx - x) + (cy - y) * (cy - y);
                if (distance >= bestDistance)
                    continue;
                if (Field::Interval const* floor = NavFloor(SpanAt(view, cx, cy), z, START_UP, NAV_WALKABLE))
                {
                    best = floor->Floor();
                    bestDistance = distance;
                }
            }
        return best;
    }

    /// One heading of the ground probe over the field: GroundSense::MarchBearing (dense) and GroundSense::CastRays,
    /// combined, in one walk. They sample the same points -- the march every `pitch`, and the rays every `pitch` on
    /// the navmesh's surface, noting where each filter is first refused. The filters are nested (ground, then water,
    /// then magma and slime), so where the widest stops the others have stopped too. `start` is the seat's floor on
    /// the mesh (StartFloor), INVALID_HEIGHT for no rays.
    Animus::Curriculum::GroundSense::Bearing FieldHeading(Field::View const& view, float x0, float y0, float z0,
        float start, float heading, float pitch)
    {
        namespace Move = Animus::Curriculum;
        using Move::MoveBlock;
        Move::GroundSense::March march;
        Move::GroundSense::Rays rays;
        float const dx = std::cos(heading);
        float const dy = std::sin(heading);
        uint32 const samples = std::min(DENSE_SAMPLES_MAX, uint32(std::lround(MoveBlock::MARCH_MAX / pitch)));
        std::array<float, DENSE_SAMPLES_MAX + 1> ground;
        std::array<float, DENSE_SAMPLES_MAX + 1> ranges;
        ground[0] = z0;
        ranges[0] = 0.0f;
        uint32 window = 0;

        float previousZ = z0;
        float previousRange = 0.0f;
        float free = 0.0f;
        float blockedStep = 0.0f;
        float worstStep = 0.0f;
        bool marching = true;
        bool raying = start > INVALID_HEIGHT;
        float rayZ = start;
        Cursor cursor(view);

        for (uint32 cell = 0; cell < samples && (marching || raying); ++cell)
        {
            float const range = std::min(MoveBlock::MARCH_MAX, float(cell + 1) * pitch);
            Span const span = cursor.At(x0 + range * dx, y0 + range * dy);

            if (raying)
            {
                // The edge is somewhere between the last sample the rays held and this one: halfway.
                float const edge = range - pitch * 0.5f;
                Field::Interval const* floor = NavFloor(span, rayZ, NAV_CLIMB, NAV_ANY);
                uint8 const flags = floor ? floor->NavFlags() : 0;
                if (rays.Dry < 0.0f && !(flags & NAV_GROUND))
                    rays.Dry = edge;
                if (rays.Wet < 0.0f && !(flags & NAV_WALKABLE))
                    rays.Wet = edge;
                if (!(flags & NAV_ANY))
                {
                    rays.All = edge;
                    raying = false;
                }
                else
                    rayZ = floor->Floor();
            }

            if (!marching)
                continue;
            float const gap = range - previousRange;
            float const allowance = MoveBlock::MAX_STEP + gap * MoveBlock::MARCH_SLOPE;
            Field::Interval const* liquid = LiquidAt(span, previousZ);
            bool const liquidHere = liquid && liquid->Liquid() >= previousZ - allowance;
            if (liquidHere && (liquid->LiquidFlags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0)
            {
                march.Burns = 1.0f;
                marching = false;
                continue;
            }

            float z = 0.0f;
            if (liquidHere && (liquid->LiquidFlags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0)
            {
                march.Water = 1.0f;
                z = liquid->Liquid();
            }
            else
            {
                z = HeightBelow(span, previousZ + MoveBlock::MAX_STEP, MoveBlock::MARCH_SEARCH);
                if (z <= INVALID_HEIGHT)
                {
                    blockedStep = -1.0f;
                    marching = false;
                    continue;
                }
                float const step = z - previousZ;
                if (std::fabs(step) > allowance)
                {
                    blockedStep = std::clamp(step / allowance, -1.0f, 1.0f);
                    marching = false;
                    continue;
                }
                while (window + 1 <= cell && range - ranges[window + 1] >= Move::GroundSense::MARCH_WINDOW)
                    ++window;
                float const span = range - ranges[window];
                float const spanAllowance = MoveBlock::MAX_STEP + span * MoveBlock::MARCH_SLOPE;
                float const climb = z - ground[window];
                if (std::fabs(climb) > spanAllowance)
                {
                    blockedStep = std::clamp(climb / spanAllowance, -1.0f, 1.0f);
                    marching = false;
                    continue;
                }
                float const slope = climb / spanAllowance;
                if (std::fabs(slope) > std::fabs(worstStep))
                    worstStep = slope;
            }

            ground[cell + 1] = z;
            ranges[cell + 1] = range;
            previousZ = z;
            previousRange = range;
            free = range;
        }

        if (start > INVALID_HEIGHT)
            for (float* ray : { &rays.Dry, &rays.Wet, &rays.All })
                if (*ray < 0.0f)
                    *ray = MoveBlock::MARCH_MAX;
        march.Reach = free / MoveBlock::MARCH_MAX;
        march.Step = marching ? std::clamp(worstStep, -1.0f, 1.0f) : blockedStep;
        return Move::GroundSense::Combine(march, rays);
    }

    /// GroundSense::MeasureRoom over the field: the walkable surface around the seat flooded out to the clearance
    /// range, and the nearest edge of it -- a side between a cell the flood reached and one it could not.
    Animus::Curriculum::GroundSense::Room FieldRoom(Field::View const& view, float x, float y, float z)
    {
        namespace Move = Animus::Curriculum;
        Animus::Curriculum::GroundSense::Room room;
        float const radius = Move::MoveBlock::CLEARANCE_RANGE;
        float const cell = view.Cell;
        float ax = 0.0f, ay = 0.0f;
        if (!CellCentre(view, x, y, ax, ay))
            return room;
        int32 const reach = int32(std::ceil(radius / cell)) + 1;
        int32 const side = 2 * reach + 1;
        // Per cell of the window around the seat: the floor the flood stands on there, or NaN where it has not been.
        std::vector<float> floors(std::size_t(side * side), std::numeric_limits<float>::quiet_NaN());
        std::vector<int32> queue;
        queue.reserve(floors.size());

        Field::Interval const* start = NavFloor(SpanAt(view, ax, ay), z, START_UP, NAV_WALKABLE);
        if (!start)
            return room;
        floors[std::size_t(reach * side + reach)] = start->Floor();
        queue.push_back(reach * side + reach);

        float best = radius;
        float hitX = 0.0f, hitY = 0.0f;
        float const half = cell * 0.5f;
        Cursor cursor(view);
        static constexpr int32 STEPS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (std::size_t next = 0; next < queue.size(); ++next)
        {
            int32 const wr = queue[next] / side;
            int32 const wc = queue[next] % side;
            float const here = floors[std::size_t(queue[next])];
            float const cx = ax + float(wc - reach) * cell;
            float const cy = ay + float(wr - reach) * cell;
            for (auto const& [sc, sr] : STEPS)
            {
                int32 const nc = wc + sc;
                int32 const nr = wr + sr;
                if (nc < 0 || nr < 0 || nc >= side || nr >= side)
                    continue;
                std::size_t const slot = std::size_t(nr * side + nc);
                if (!std::isnan(floors[slot]))
                    continue;
                Span const span = cursor.At(cx + float(sc) * cell, cy + float(sr) * cell);
                if (!span.OnGrid)
                    continue;       // no grid there: not the field's to say
                Field::Interval const* floor = NavFloor(span, here, NAV_CLIMB, NAV_WALKABLE);
                // The side between the two cells, and the nearest point of it to the seat.
                float const ex = cx + float(sc) * half;
                float const ey = cy + float(sr) * half;
                float const px = sc ? ex : std::clamp(x, cx - half, cx + half);
                float const py = sr ? ey : std::clamp(y, cy - half, cy + half);
                float const distance = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
                if (!floor)
                {
                    if (distance < best)
                    {
                        best = distance;
                        hitX = px;
                        hitY = py;
                    }
                    continue;
                }
                if (distance >= radius)
                    continue;       // the flood goes no further than a wall could matter
                floors[slot] = floor->Floor();
                queue.push_back(int32(slot));
            }
        }

        room.Clearance = std::clamp(best / radius, 0.0f, 1.0f);
        float const outX = x - hitX;
        float const outY = y - hitY;
        if (best < radius && outX * outX + outY * outY > 1e-6f)
        {
            room.Directed = true;
            room.Away = std::atan2(outY, outX);
        }
        return room;
    }

    /// The ground probe's per-field gaps, as ProbeBake's compare prints them.
    struct ReadingGap
    {
        double Sum[5] = {};
        uint64 Far[5] = {};
        uint64 Count = 0;
        uint64 RoomCount = 0;

        void Add(Animus::Curriculum::ProbeBake::Reading const& a, Animus::Curriculum::ProbeBake::Reading const& b)
        {
            for (uint32 ray = 0; ray < Animus::Curriculum::SENSE_RAYS; ++ray)
            {
                float const gaps[4] = {
                    std::fabs(a.Rays[ray].Reach - b.Rays[ray].Reach), std::fabs(a.Rays[ray].Step - b.Rays[ray].Step),
                    std::fabs(a.Rays[ray].Shore - b.Rays[ray].Shore),
                    std::fabs(a.Rays[ray].Burns - b.Rays[ray].Burns) };
                for (uint32 field = 0; field < 4; ++field)
                {
                    Sum[field] += gaps[field];
                    Far[field] += gaps[field] > 0.1f ? 1 : 0;
                }
                ++Count;
            }
            float const room = std::fabs(a.Room.Clearance - b.Room.Clearance);
            Sum[4] += room;
            Far[4] += room > 0.1f ? 1 : 0;
            ++RoomCount;
        }

        void Print(std::ostringstream& out, char const* name) const
        {
            out << "  " << std::left << std::setw(34) << name << std::right;
            for (uint32 field = 0; field < 5; ++field)
            {
                uint64 const count = field < 4 ? Count : RoomCount;
                double const mean = count ? Sum[field] / double(count) : 0.0;
                double const far = count ? 100.0 * double(Far[field]) / double(count) : 0.0;
                out << "  " << std::setw(6) << std::setprecision(3) << mean << " " << std::setw(5)
                    << std::setprecision(1) << far << "%";
            }
            out << "\n";
        }
    };

    struct Gap
    {
        double Sum = 0.0;
        uint64 Far = 0;
        uint64 VeryFar = 0;
        uint64 Count = 0;
        uint64 Obstructed = 0;          // live found something within range
        double ObstructedSum = 0.0;

        void Add(float baked, float live, float range)
        {
            float const gap = std::fabs(baked - live);
            Sum += gap;
            Far += gap > 2.0f ? 1 : 0;
            VeryFar += gap > 5.0f ? 1 : 0;
            ++Count;
            if (live < range)
            {
                ++Obstructed;
                ObstructedSum += gap;
            }
        }
    };
}

namespace Animus::Curriculum::LayeredField
{
    Grid Bake(Map* map, float x, float y, float cell, uint32 threads)
    {
        Grid grid;
        grid.MapId = map->GetId();
        grid.GridX = ProbeBake::GridIndex(x);
        grid.GridY = ProbeBake::GridIndex(y);
        grid.Cell = cell;
        float const gridX = std::floor(x / SIZE_OF_GRIDS) * SIZE_OF_GRIDS;
        float const gridY = std::floor(y / SIZE_OF_GRIDS) * SIZE_OF_GRIDS;
        grid.Side = uint32(std::ceil(SIZE_OF_GRIDS / cell));
        grid.MinX = gridX + cell / 2.0f;
        grid.MinY = gridY + cell / 2.0f;

        uint32 const cells = grid.Side * grid.Side;
        std::vector<std::vector<Interval>> columns(cells);
        dtNavMesh const* mesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        uint32 const workers = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
        std::atomic<uint32> nextRow{ 0 };
        auto const started = std::chrono::steady_clock::now();

        auto work = [&]()
        {
            QueryOwner owner(mesh);
            for (uint32 row = nextRow++; row < grid.Side; row = nextRow++)
                for (uint32 column = 0; column < grid.Side; ++column)
                    columns[row * grid.Side + column] = Column(map, owner.Query, grid.MinX + float(column) * cell,
                        grid.MinY + float(row) * cell);
        };
        std::vector<std::thread> pool;
        for (uint32 index = 1; index < workers; ++index)
            pool.emplace_back(work);
        work();
        for (std::thread& thread : pool)
            thread.join();
        grid.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        grid.First.reserve(cells + 1);
        for (std::vector<Interval> const& column : columns)
        {
            grid.First.push_back(uint32(grid.Intervals.size()));
            grid.Intervals.insert(grid.Intervals.end(), column.begin(), column.end());
        }
        grid.First.push_back(uint32(grid.Intervals.size()));
        return grid;
    }

    bool Open(Grid const& grid, float x, float y, float z)
    {
        int32 const column = int32(std::lround((x - grid.MinX) / grid.Cell));
        int32 const row = int32(std::lround((y - grid.MinY) / grid.Cell));
        if (column < 0 || row < 0 || column >= int32(grid.Side) || row >= int32(grid.Side))
            return true;        // off the grid: not this table's to say
        uint32 const cell = uint32(row) * grid.Side + uint32(column);
        for (uint32 index = grid.First[cell]; index < grid.First[cell + 1]; ++index)
        {
            Interval const& interval = grid.Intervals[index];
            if (interval.OpenAbove() && z >= interval.Floor() && z < interval.Floor() + interval.Headroom())
                return true;
        }
        return false;
    }

    float FlightReach(Grid const& grid, float x, float y, float z, float heading, float range, float pitch)
    {
        float const dx = std::cos(heading);
        float const dy = std::sin(heading);
        for (float along = pitch; along <= range; along += pitch)
            if (!Open(grid, x + along * dx, y + along * dy, z))
                return along - pitch;
        return range;
    }

    bool Open(View const& view, float x, float y, float z)
    {
        Span const span = SpanAt(view, x, y);
        if (!span.OnGrid)
            return true;        // a grid with no field: nothing known to be solid there
        for (Interval const* interval = span.Begin; interval != span.End; ++interval)
            if (interval->OpenAbove() && z >= interval->Floor() && z < interval->Floor() + interval->Headroom())
                return true;
        return false;
    }

    float FlightReach(View const& view, float x, float y, float z, float heading, float range, float pitch)
    {
        float const dx = std::cos(heading);
        float const dy = std::sin(heading);
        for (float along = pitch; along <= range; along += pitch)
            if (!Open(view, x + along * dx, y + along * dy, z))
                return along - pitch;
        return range;
    }

    float LiveFlightReach(Map* map, float x, float y, float z, float heading, float range, float pitch)
    {
        float const dx = std::cos(heading);
        float const dy = std::sin(heading);
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        float reach = range;
        if (map->GetMapCollisionData().GetStaticTree().GetObjectHitPos(x, y, z, x + range * dx, y + range * dy, z, rx,
            ry, rz, 0.0f))
            reach = std::sqrt((rx - x) * (rx - x) + (ry - y) * (ry - y));
        for (float along = pitch; along < reach; along += pitch)
        {
            float const ground = map->GetGridHeight(x + along * dx, y + along * dy);
            if (ground > INVALID_HEIGHT && ground > z)
                return along - pitch;
        }
        return reach;
    }

    bool Sense(View const& view, float x, float y, float z, float facing, ProbeBake::Reading& out)
    {
        if (!SpanAt(view, x, y).OnGrid)
            return false;

        namespace Ground = GroundSense;
        float const start = StartFloor(view, x, y, z);
        float const pitch = ProbeBake::StandardSettings().Pitch;
        // Each ray's wedge is its own heading and SEAT_HALF_WEDGE either side, and the rays are twice that apart,
        // so a wedge's edges are its neighbours' edges: 32 headings answer all 16 wedges, each measured once.
        // (The standard bake's three rays a wedge.)
        std::array<Ground::Bearing, 2 * SENSE_RAYS> headings;
        for (uint32 index = 0; index < 2 * SENSE_RAYS; ++index)
        {
            // Index 2 * ray is the ray's own heading; 2 * ray + 1 the edge between it and the next ray round.
            float const heading = facing - float(index) * SEAT_HALF_WEDGE;
            headings[index] = FieldHeading(view, x, y, z, start, heading, pitch);
        }
        for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
        {
            // SenseWedge's order, -half to +half: the edge towards the next ray, the heading, the previous edge.
            Ground::Bearing worst = headings[2 * ray + 1];
            worst = Ground::Worst(worst, headings[2 * ray]);
            worst = Ground::Worst(worst, headings[(2 * ray + 2 * SENSE_RAYS - 1) % (2 * SENSE_RAYS)]);
            out.Rays[ray] = worst;
        }
        out.Room = start > INVALID_HEIGHT ? FieldRoom(view, x, y, start) : Ground::Room{};
        return true;
    }

    bool SenseCompass(View const& view, float x, float y, float z, Compass& out)
    {
        if (!SpanAt(view, x, y).OnGrid)
            return false;

        float const start = StartFloor(view, x, y, z);
        float const pitch = ProbeBake::StandardSettings().Pitch;
        for (uint32 index = 0; index < 2 * SENSE_RAYS; ++index)
            out.Headings[index] = FieldHeading(view, x, y, z, start, -float(index) * SEAT_HALF_WEDGE, pitch);
        out.Room = start > INVALID_HEIGHT ? FieldRoom(view, x, y, start) : GroundSense::Room{};
        return true;
    }

    void RaysFor(GroundSense::Bearing const* headings, float facing, ProbeBake::Reading& out)
    {
        // Sense's index i lies along facing - i * SEAT_HALF_WEDGE; the compass's j along -j * SEAT_HALF_WEDGE. They
        // meet at j = i - facing / SEAT_HALF_WEDGE, rounded to the nearest heading.
        constexpr int32 HEADINGS = int32(2 * SENSE_RAYS);
        int32 turn = int32(std::lround(facing / SEAT_HALF_WEDGE)) % HEADINGS;
        if (turn < 0)
            turn += HEADINGS;
        auto const at = [headings, turn](int32 index) -> GroundSense::Bearing const&
        {
            return headings[((index - turn) % HEADINGS + HEADINGS) % HEADINGS];
        };
        for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
        {
            int32 const own = int32(2 * ray);
            GroundSense::Bearing worst = at(own + 1);
            worst = GroundSense::Worst(worst, at(own));
            worst = GroundSense::Worst(worst, at(own - 1));
            out.Rays[ray] = worst;
        }
    }

    namespace
    {
        /// The ground probe from the field against the dense live one (ProbeBake::SenseLive), at places on the
        /// mesh's floors at least a march from the grid's edges, the field's only grid.
        void CompareGround(Map* map, View const& view, uint32 samples, uint32 seed, float nearX, float nearY,
            float radius, std::ostringstream& out)
        {
            dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
            if (!query)
            {
                out << "  no navmesh query on this map: no ground probe to compare\n";
                return;
            }

            Grid const& grid = *view.Grids[1][1];
            std::mt19937 random(seed);
            // Anywhere on the centre grid: a march that crosses its edge reads the neighbour. A missing
            // neighbour keeps its places a march away.
            bool whole = true;
            for (auto const& row : view.Grids)
                for (Grid const* neighbour : row)
                    whole = whole && neighbour;
            float const edge = whole ? 0.0f : MoveBlock::MARCH_MAX + 5.0f;
            float const gridX = grid.MinX - grid.Cell / 2.0f;
            float const gridY = grid.MinY - grid.Cell / 2.0f;
            std::uniform_real_distribution<float> along(edge, SIZE_OF_GRIDS - edge);
            std::uniform_real_distribution<float> angle(0.0f, TWO_PI_F);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);

            ReadingGap denseStale;      // the dense live probe against itself one refresh late: the scale
            ReadingGap field;           // the dense live probe against the field's
            uint32 compared = 0;
            uint32 misses = 0;
            double liveNs = 0.0;
            double fieldNs = 0.0;
            double roomNs = 0.0;
            for (uint32 attempt = 0; compared < samples && attempt < samples * 20; ++attempt)
            {
                float x = gridX + along(random);
                float y = gridY + along(random);
                if (radius > 0.0f)
                {
                    float const spin = angle(random);
                    float const out = radius * std::sqrt(unit(random));
                    x = std::clamp(nearX + out * std::cos(spin), gridX + edge, gridX + SIZE_OF_GRIDS - edge - 0.01f);
                    y = std::clamp(nearY + out * std::sin(spin), gridY + edge, gridY + SIZE_OF_GRIDS - edge - 0.01f);
                }
                std::vector<float> const here = ProbeBake::Floors(map, query, x, y);
                if (here.empty())
                {
                    ++misses;
                    continue;
                }
                float const z = here[std::min<std::size_t>(here.size() - 1, std::size_t(unit(random)
                    * float(here.size())))];
                float const facing = angle(random);
                GroundSense::Origin const at{ x, y, z, PHASEMASK_NORMAL, 2.0f };

                auto mark = std::chrono::steady_clock::now();
                ProbeBake::Reading const live = ProbeBake::SenseLive(map, query, at, facing);
                auto now = std::chrono::steady_clock::now();
                liveNs += std::chrono::duration<double, std::nano>(now - mark).count();
                ProbeBake::Reading baked;
                Sense(view, x, y, z, facing, baked);
                fieldNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - now).count();
                now = std::chrono::steady_clock::now();
                float const startZ = StartFloor(view, x, y, z);
                if (startZ > INVALID_HEIGHT)
                    FieldRoom(view, x, y, startZ);
                roomNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - now).count();

                float const back = angle(random);
                float const distance = MoveBlock::MARCH_REFRESH_YARDS * unit(random);
                float const oldX = x + distance * std::cos(back);
                float const oldY = y + distance * std::sin(back);
                std::vector<float> const there = ProbeBake::Floors(map, query, oldX, oldY);
                float oldZ = INVALID_HEIGHT;
                for (float floor : there)
                    if (std::fabs(floor - z) <= MoveBlock::MAX_STEP
                        && (oldZ <= INVALID_HEIGHT || std::fabs(floor - z) < std::fabs(oldZ - z)))
                        oldZ = floor;
                if (oldZ > INVALID_HEIGHT)
                {
                    float const oldFacing = facing + (2.0f * unit(random) - 1.0f) * MoveBlock::MARCH_REFRESH_RADIANS;
                    denseStale.Add(live, ProbeBake::SenseLive(map, query,
                        GroundSense::Origin{ oldX, oldY, oldZ, PHASEMASK_NORMAL, 2.0f }, oldFacing));
                }
                field.Add(live, baked);
                ++compared;
            }

            out << std::setprecision(1) << "  ground probe" << (whole ? "" : " (a neighbour grid missing)") << ": "
                << compared << " places on the mesh compared (" << misses
                << " draws off it); dense live " << liveNs / std::max(1u, compared) / 1000.0 << " us, from the field "
                << fieldNs / std::max(1u, compared) / 1000.0 << " us a place (the room " << roomNs
                / std::max(1u, compared) / 1000.0 << " us of it)\n";
            out << "  mean |gap| and share over 0.1   " << "      reach          step         shore         burns"
                << "     clearance\n";
            denseStale.Print(out, "dense vs itself one refresh late");
            field.Print(out, "dense live vs layered field");
        }
    }

    std::string Compare(Map* map, View const& view, uint32 samples, uint32 seed, float nearX, float nearY,
        float radius)
    {
        Grid const& grid = *view.Grids[1][1];
        std::ostringstream out;
        out << std::fixed;
        uint32 const cells = grid.Side * grid.Side;
        uint32 histogram[6] = {};
        uint32 liquid = 0;
        for (uint32 cell = 0; cell < cells; ++cell)
            ++histogram[std::min<uint32>(5, grid.First[cell + 1] - grid.First[cell])];
        for (Interval const& interval : grid.Intervals)
            liquid += interval.HasLiquid() ? 1 : 0;

        // As a file holds it: a count a cell, then the intervals as they are.
        std::vector<uint8> packed;
        packed.reserve(cells + grid.Intervals.size() * sizeof(Interval));
        for (uint32 cell = 0; cell < cells; ++cell)
            packed.push_back(uint8(std::min<uint32>(255, grid.First[cell + 1] - grid.First[cell])));
        uint8 const* raw = reinterpret_cast<uint8 const*>(grid.Intervals.data());
        packed.insert(packed.end(), raw, raw + grid.Intervals.size() * sizeof(Interval));
        std::vector<char> compressed(ZSTD_compressBound(packed.size()));
        std::size_t const zipped = ZSTD_compress(compressed.data(), compressed.size(), packed.data(), packed.size(),
            15);

        out << std::setprecision(1) << "Layered field, map " << grid.MapId << " grid at (" << grid.MinX - grid.Cell / 2
            << ", " << grid.MinY - grid.Cell / 2 << "): " << grid.Side << "x" << grid.Side << " cells of "
            << std::setprecision(2) << grid.Cell << " yd, " << std::setprecision(1) << grid.Seconds << " s to bake\n";
        out << "  " << grid.Intervals.size() << " intervals (" << liquid << " with liquid); cells with 0/1/2/3/4/5+: "
            << histogram[0] << "/" << histogram[1] << "/" << histogram[2] << "/" << histogram[3] << "/" << histogram[4]
            << "/" << histogram[5] << "; " << std::setprecision(2) << double(packed.size()) / (1024.0 * 1024.0)
            << " MB packed, " << double(ZSTD_isError(zipped) ? 0 : zipped) / (1024.0 * 1024.0) << " MB zstd\n";

        CompareGround(map, view, samples, seed, nearX, nearY, radius, out);

        std::mt19937 random(seed);
        float const edge = 45.0f;
        std::uniform_real_distribution<float> along(edge, SIZE_OF_GRIDS - edge);
        std::uniform_real_distribution<float> angle(0.0f, 2.0f * float(M_PI));
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        float const gridX = grid.MinX - grid.Cell / 2.0f;
        float const gridY = grid.MinY - grid.Cell / 2.0f;
        constexpr float RANGE = 40.0f;
        constexpr float PITCH = 0.5f;

        Gap low;        // up to 10 yd above the floor: among trees, walls and roofs
        Gap high;       // 10-60 yd up
        double bakedNs = 0.0;
        double liveNs = 0.0;
        uint32 compared = 0;
        for (uint32 attempt = 0; compared < samples && attempt < samples * 20; ++attempt)
        {
            float const x = gridX + along(random);
            float const y = gridY + along(random);
            int32 const column = int32(std::lround((x - grid.MinX) / grid.Cell));
            int32 const row = int32(std::lround((y - grid.MinY) / grid.Cell));
            uint32 const cell = uint32(row) * grid.Side + uint32(column);
            uint32 const count = grid.First[cell + 1] - grid.First[cell];
            if (!count)
                continue;
            Interval const& interval = grid.Intervals[grid.First[cell] + std::min(count - 1, uint32(unit(random)
                * float(count)))];
            if (!interval.OpenAbove())
                continue;
            float const room = std::min(interval.Headroom() - 0.5f, 60.0f);
            if (room < 2.0f)
                continue;
            float const z = interval.Floor() + 1.5f + unit(random) * (room - 1.5f);
            float const heading = angle(random);

            auto mark = std::chrono::steady_clock::now();
            float const baked = FlightReach(grid, x, y, z, heading, RANGE, PITCH);
            auto now = std::chrono::steady_clock::now();
            bakedNs += std::chrono::duration<double, std::nano>(now - mark).count();
            float const live = LiveFlightReach(map, x, y, z, heading, RANGE, PITCH);
            liveNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - now).count();

            (z - interval.Floor() < 10.0f ? low : high).Add(baked, live, RANGE);
            ++compared;
        }

        out << std::setprecision(1) << "  " << compared << " flight readings compared (40 yd, level): baked "
            << bakedNs / std::max(1u, compared) / 1000.0 << " us, live " << liveNs / std::max(1u, compared) / 1000.0
            << " us a bearing\n";
        auto print = [&out](char const* name, Gap const& gap)
        {
            double const n = double(std::max<uint64>(1, gap.Count));
            out << "  " << name << ": " << gap.Count << " readings, mean |gap| " << std::setprecision(2) << gap.Sum / n
                << " yd, over 2 yd " << std::setprecision(1) << 100.0 * double(gap.Far) / n << "%, over 5 yd "
                << 100.0 * double(gap.VeryFar) / n << "%; where something was in range (" << gap.Obstructed
                << ") mean |gap| " << std::setprecision(2)
                << gap.ObstructedSum / double(std::max<uint64>(1, gap.Obstructed)) << " yd\n";
        };
        print("0-10 yd up ", low);
        print("10-60 yd up", high);
        return out.str();
    }

    std::size_t Grid::Bytes() const
    {
        return First.capacity() * sizeof(uint32) + Intervals.capacity() * sizeof(Interval);
    }

    namespace
    {
        constexpr uint32 FILE_MAGIC = 0x464C4841;      // "AHLF"
        constexpr uint32 FILE_VERSION = 1;
        constexpr int ZSTD_LEVEL = 15;

        struct Header
        {
            uint32 Magic = FILE_MAGIC;
            uint32 Version = FILE_VERSION;
            uint32 MapId = 0;
            int32 GridX = 0;
            int32 GridY = 0;
            float Cell = 0.0f;
            uint32 Side = 0;
            float MinX = 0.0f;
            float MinY = 0.0f;
            uint32 Intervals = 0;
            uint64 RawBytes = 0;
        };
    }

    bool Write(Grid const& grid, std::string const& path)
    {
        uint32 const cells = grid.Side * grid.Side;
        Header header;
        header.MapId = grid.MapId;
        header.GridX = grid.GridX;
        header.GridY = grid.GridY;
        header.Cell = grid.Cell;
        header.Side = grid.Side;
        header.MinX = grid.MinX;
        header.MinY = grid.MinY;
        header.Intervals = uint32(grid.Intervals.size());

        // A count a cell (a column never holds more than MAX_SURFACES and its extras, well under 255), then the
        // intervals as they are in memory.
        std::vector<uint8> payload;
        payload.reserve(cells + grid.Intervals.size() * sizeof(Interval));
        for (uint32 cell = 0; cell < cells; ++cell)
            payload.push_back(uint8(std::min<uint32>(255, grid.First[cell + 1] - grid.First[cell])));
        uint8 const* raw = reinterpret_cast<uint8 const*>(grid.Intervals.data());
        payload.insert(payload.end(), raw, raw + grid.Intervals.size() * sizeof(Interval));
        header.RawBytes = payload.size();

        std::vector<char> compressed(ZSTD_compressBound(payload.size()));
        std::size_t const size = ZSTD_compress(compressed.data(), compressed.size(), payload.data(), payload.size(),
            ZSTD_LEVEL);
        if (ZSTD_isError(size))
            return false;

        // Written beside and renamed over, so a reader never meets half a file.
        std::filesystem::path const target(path);
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        std::string const partial = path + ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(reinterpret_cast<char const*>(&header), sizeof(header));
            out.write(compressed.data(), std::streamsize(size));
            if (!out)
                return false;
        }
        std::filesystem::rename(partial, target, error);
        return !error;
    }

    bool Read(std::string const& path, Grid& grid)
    {
        std::ifstream in(path, std::ios::binary);
        Header header;
        if (!in.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.Magic != FILE_MAGIC
            || header.Version != FILE_VERSION || header.Side == 0 || header.Side > 8192
            || header.RawBytes > (uint64(1) << 32))
            return false;
        std::vector<char> const compressed{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        std::vector<uint8> payload(header.RawBytes);
        std::size_t const size = ZSTD_decompress(payload.data(), payload.size(), compressed.data(), compressed.size());
        std::size_t const cells = std::size_t(header.Side) * header.Side;
        if (ZSTD_isError(size) || size != header.RawBytes
            || size != cells + std::size_t(header.Intervals) * sizeof(Interval))
            return false;

        grid.MapId = header.MapId;
        grid.GridX = header.GridX;
        grid.GridY = header.GridY;
        grid.Cell = header.Cell;
        grid.Side = header.Side;
        grid.MinX = header.MinX;
        grid.MinY = header.MinY;
        grid.First.resize(cells + 1);
        uint32 total = 0;
        for (std::size_t cell = 0; cell < cells; ++cell)
        {
            grid.First[cell] = total;
            total += payload[cell];
        }
        grid.First[cells] = total;
        if (total != header.Intervals)
            return false;
        grid.Intervals.resize(header.Intervals);
        std::memcpy(grid.Intervals.data(), payload.data() + cells, std::size_t(header.Intervals) * sizeof(Interval));
        return true;
    }

    namespace Store
    {
        namespace
        {
            struct Entry
            {
                std::shared_ptr<Grid const> Field;      // nullptr: the grid has no file
                std::atomic<uint64> LastRead{ 0 };
            };

            bool g_enabled = false;
            std::string g_dir;
            uint32 g_cacheGrids = 64;
            std::atomic<uint64> g_clock{ 0 };
            std::shared_mutex g_lock;
            /// Every grid asked about: its field, or none for a grid without a file (asked once, remembered).
            std::map<std::tuple<uint32, int32, int32>, Entry> g_fields;

            /// Once a grid, like the tables.
            void WarnMissing(uint32 mapId, int32 gridX, int32 gridY)
            {
                static std::mutex logged;
                static std::set<std::tuple<uint32, int32, int32>> warned;
                std::lock_guard guard(logged);
                if (warned.emplace(mapId, gridX, gridY).second)
                    LOG_WARN("module.animus", "Ground probe: no field for map {} grid ({}, {}) in {}: seats on it or "
                        "near enough to see into it keep their last reading. Bake it with `forge fieldstage` and "
                        "ship the file.", mapId, gridX, gridY, g_dir);
            }
        }

        void Configure(bool enabled, std::string const& dir, uint32 cacheGrids)
        {
            std::unique_lock lock(g_lock);
            g_enabled = enabled;
            g_dir = dir;
            g_cacheGrids = std::max<uint32>(9, cacheGrids);
            g_fields.clear();
        }

        bool Enabled()
        {
            return g_enabled;
        }

        std::string FileFor(uint32 mapId, int32 gridX, int32 gridY)
        {
            return (std::filesystem::path(g_dir) / Acore::StringFormat("{:03}_{}_{}.field", mapId, gridX, gridY))
                .string();
        }

        std::shared_ptr<Grid const> Find(uint32 mapId, int32 gridX, int32 gridY)
        {
            auto const key = std::make_tuple(mapId, gridX, gridY);
            uint64 const now = ++g_clock;
            {
                std::shared_lock lock(g_lock);
                auto const found = g_fields.find(key);
                if (found != g_fields.end())
                {
                    found->second.LastRead.store(now, std::memory_order_relaxed);
                    return found->second.Field;
                }
            }

            // Read outside the lock, as the tables are: the other maps' seats should not wait on a file.
            auto field = std::make_shared<Grid>();
            std::shared_ptr<Grid const> loaded;
            if (Read(FileFor(mapId, gridX, gridY), *field))
                loaded = std::move(field);
            // Many more reads than the cache holds is the cache letting go of fields its seats still stand on:
            // every refresh there reads a file. Said once.
            uint64 const reads = FileReads.fetch_add(1, std::memory_order_relaxed) + 1;
            if (reads == uint64(g_cacheGrids) * 4)
                LOG_WARN("module.animus", "Ground probe: {} field files read with AnimusForge.Probe.CacheGrids = {}: "
                    "the seats stand on more grids than it holds. Raise it (about 4 MB a field).", reads,
                    g_cacheGrids);

            std::unique_lock lock(g_lock);
            auto [entry, inserted] = g_fields.try_emplace(key);
            if (!inserted)
                return entry->second.Field;
            entry->second.Field = loaded;
            entry->second.LastRead.store(now, std::memory_order_relaxed);

            // Over the cap: let the least recently read fields go. A probe still reading one keeps its own pointer.
            uint32 held = 0;
            for (auto const& [other, cached] : g_fields)
                held += cached.Field ? 1 : 0;
            while (held > g_cacheGrids)
            {
                auto oldest = g_fields.end();
                for (auto it = g_fields.begin(); it != g_fields.end(); ++it)
                    if (it->second.Field && it != entry && (oldest == g_fields.end()
                        || it->second.LastRead.load(std::memory_order_relaxed)
                            < oldest->second.LastRead.load(std::memory_order_relaxed)))
                        oldest = it;
                if (oldest == g_fields.end())
                    break;
                g_fields.erase(oldest);
                --held;
            }
            return loaded;
        }

        bool Gather(uint32 mapId, float x, float y, Neighbourhood& out)
        {
            int32 const gridX = ProbeBake::GridIndex(x);
            int32 const gridY = ProbeBake::GridIndex(y);
            out.Held[1][1] = Find(mapId, gridX, gridY);
            if (!out.Held[1][1])
            {
                WarnMissing(mapId, gridX, gridY);
                return false;
            }
            out.View.CentreX = gridX;
            out.View.CentreY = gridY;
            out.View.Cell = out.Held[1][1]->Cell;
            // Only the neighbours a probe from here can read into: a march and its rays run MARCH_MAX, the room
            // CLEARANCE_RANGE, both less. A seat well inside its grid reads that grid alone, so a stage holds the
            // grids its seats stand on rather than all their neighbours too.
            float const reach = MoveBlock::MARCH_MAX + 2.0f;
            float const inX = x - float(gridX) * SIZE_OF_GRIDS;
            float const inY = y - float(gridY) * SIZE_OF_GRIDS;
            bool const near[2][3] = {
                { inX < reach, true, inX > SIZE_OF_GRIDS - reach },
                { inY < reach, true, inY > SIZE_OF_GRIDS - reach } };
            for (int32 dx = -1; dx <= 1; ++dx)
                for (int32 dy = -1; dy <= 1; ++dy)
                {
                    if ((dx || dy) && near[0][dx + 1] && near[1][dy + 1])
                    {
                        out.Held[dx + 1][dy + 1] = Find(mapId, gridX + dx, gridY + dy);
                        // A neighbour the probe can read into with no field would read as a cliff at the grid's
                        // edge: measure live instead, as where the seat's own grid has none. `forge fieldstage`
                        // writes a grid with no floor too, so a missing file is only ever one not baked.
                        if (!out.Held[dx + 1][dy + 1])
                        {
                            WarnMissing(mapId, gridX + dx, gridY + dy);
                            return false;
                        }
                    }
                    out.View.Grids[dx + 1][dy + 1] = out.Held[dx + 1][dy + 1].get();
                }
            return true;
        }

        uint32 Loaded()
        {
            std::shared_lock lock(g_lock);
            uint32 count = 0;
            for (auto const& [key, entry] : g_fields)
                count += entry.Field ? 1 : 0;
            return count;
        }

        std::size_t Bytes()
        {
            std::shared_lock lock(g_lock);
            std::size_t bytes = 0;
            for (auto const& [key, entry] : g_fields)
                bytes += entry.Field ? entry.Field->Bytes() : 0;
            return bytes;
        }
    }
}
