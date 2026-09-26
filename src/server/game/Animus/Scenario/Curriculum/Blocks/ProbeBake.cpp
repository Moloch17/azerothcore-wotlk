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

#include "ProbeBake.h"
#include "DetourExtended.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include "MoveBlock.h"
#include "Object.h"
#include "StringFormat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <random>
#include <sstream>
#include <thread>
#include <tuple>
#include <zstd.h>

namespace
{
    namespace Ground = Animus::Curriculum::GroundSense;
    using Animus::Curriculum::MoveBlock;
    using Animus::Curriculum::SENSE_RAYS;

    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// Half the angle between two of a seat's rays: the wedge each of them stands for.
    constexpr float SEAT_HALF_WEDGE = float(M_PI) / float(SENSE_RAYS);
    /// Floors closer together than this in one column are one floor seen twice (two tiles' polygons, or a
    /// polygon and its detail mesh's neighbour).
    constexpr float FLOOR_MERGE = 1.0f;
    constexpr int COLUMN_POLYS = 64;
    /// Whoever stands there. The march only asks whether liquid is present and how high its surface is, never
    /// how deep a body is in it, so the height does not reach any reading; it is fixed so the bake is one thing.
    constexpr float BAKE_COLLISION = 2.0f;

    /// The live probe's ray headings: MoveBlock's RayHeading, sixteenths of a turn clockwise from the facing.
    float RayHeading(float facing, uint32 ray)
    {
        return Position::NormalizeOrientation(facing - float(ray) * TWO_PI / float(SENSE_RAYS));
    }

    float AngleBetween(float a, float b)
    {
        return std::fabs(std::atan2(std::sin(a - b), std::cos(a - b)));
    }

    struct QueryOwner
    {
        dtNavMeshQuery* Query = nullptr;

        explicit QueryOwner(dtNavMesh const* mesh)
        {
            if (!mesh)
                return;
            Query = dtAllocNavMeshQuery();
            // The live query's node pool (MMapMgr::CreateNavMeshQuery), so findDistanceToWall gives up where the
            // live one does.
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

    /// The floor of `floors` a seat at `z` stands on: the nearest within a step, or -1.
    int32 FloorAt(float const* floors, uint32 count, float z)
    {
        int32 best = -1;
        float bestGap = MoveBlock::MAX_STEP;
        for (uint32 floor = 0; floor < count; ++floor)
        {
            float const gap = std::fabs(floors[floor] - z);
            if (gap <= bestGap)
            {
                bestGap = gap;
                best = int32(floor);
            }
        }
        return best;
    }

    Ground::Bearing Scaled(Ground::Bearing const& bearing, float weight)
    {
        return { bearing.Reach * weight, bearing.Step * weight, bearing.Shore * weight, bearing.Burns * weight };
    }

    void Accumulate(Ground::Bearing& sum, Ground::Bearing const& add)
    {
        sum.Reach += add.Reach;
        sum.Step += add.Step;
        sum.Shore += add.Shore;
        sum.Burns += add.Burns;
    }

    /// One reading of one floor of one cell for the seat's rays, turned from compass bearings to the seat's frame.
    void TurnReadings(Animus::Curriculum::ProbeBake::Table const& table, uint32 floor, float facing,
        Animus::Curriculum::ProbeBake::Turn turn, Ground::Bearing* out)
    {
        uint32 const bearings = table.Bake.Bearings;
        Animus::Curriculum::ProbeBake::PackedBearing const* packed = &table.Readings[std::size_t(floor) * bearings];
        auto readings = [packed](uint32 bearing) { return Animus::Curriculum::ProbeBake::Unpack(packed[bearing]); };
        float const spacing = TWO_PI / float(bearings);
        for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
        {
            float const heading = RayHeading(facing, ray);
            uint32 const nearest = uint32(std::lround(heading / spacing)) % bearings;
            if (turn == Animus::Curriculum::ProbeBake::Turn::Nearest)
            {
                out[ray] = readings(nearest);
                continue;
            }

            // Every baked wedge the ray's own wedge overlaps. Two wedges touch when their centres are closer than
            // their half-widths added; the small margin keeps a wedge that only touches at an edge out.
            Ground::Bearing worst = readings(nearest);
            float const reach = SEAT_HALF_WEDGE + spacing / 2.0f - 1e-4f;
            int32 const span = int32(std::ceil(reach / spacing));
            for (int32 offset = -span; offset <= span; ++offset)
            {
                if (offset == 0)
                    continue;
                uint32 const bearing = uint32((int32(nearest) + offset + int32(bearings) * 4) % int32(bearings));
                if (AngleBetween(float(bearing) * spacing, heading) < reach)
                    worst = Ground::Worst(worst, readings(bearing));
            }
            out[ray] = worst;
        }
    }

    /// Mean and tail of the gap between two readings, per field.
    struct Gap
    {
        double Sum[5] = {};
        uint64 Far[5] = {};
        uint64 Count = 0;
        uint64 RoomCount = 0;

        void Add(Animus::Curriculum::ProbeBake::Reading const& a, Animus::Curriculum::ProbeBake::Reading const& b)
        {
            for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
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

    Animus::Curriculum::ProbeBake::Reading Live(Map* map, dtNavMeshQuery const* query, Ground::Origin const& at,
        float facing, uint32 wedgeRays, float pitch, bool legacy)
    {
        Animus::Curriculum::ProbeBake::Reading reading;
        dtPolyRef const start = Ground::StartPoly(query, at);
        for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
        {
            float const heading = RayHeading(facing, ray);
            reading.Rays[ray] = legacy
                ? Ground::Sense(map, query, start, at, heading, 0.0f)
                : Ground::SenseWedge(map, query, start, at, heading, SEAT_HALF_WEDGE, wedgeRays, pitch);
        }
        reading.Room = Ground::MeasureRoom(query, start, at);
        return reading;
    }
}

namespace Animus::Curriculum::ProbeBake
{
    std::vector<float> Floors(Map* map, dtNavMeshQuery const* query, float x, float y)
    {
        std::vector<float> floors;
        if (!query)
            return floors;

        dtNavMesh const* mesh = query->getAttachedNavMesh();
        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);

        // A thin column through the whole height of the world. Detour's axes are {y, z, x}.
        float const centre[3] = { y, 0.0f, x };
        float const extents[3] = { 0.01f, 5000.0f, 0.01f };
        dtPolyRef polys[COLUMN_POLYS];
        int count = 0;
        if (dtStatusFailed(query->queryPolygons(centre, extents, &filter, polys, &count, COLUMN_POLYS)))
            return floors;

        for (int index = 0; index < count; ++index)
        {
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(mesh->getTileAndPolyByRef(polys[index], &tile, &poly))
                || poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;

            // getPolyHeight answers only for a point inside the polygon: the column's bounding box overlapping a
            // polygon's is not the column passing through it.
            float height = 0.0f;
            if (dtStatusFailed(query->getPolyHeight(polys[index], centre, &height)))
                continue;

            // The mesh is simplified; the seat stands on the ground the core reports. Downward from a step above,
            // the core's own idiom, and kept only if it is the same floor.
            float const ground = map->GetHeight(PHASEMASK_NORMAL, x, y, height + MoveBlock::MAX_STEP, true,
                MoveBlock::MAX_STEP * 2.0f);
            floors.push_back(ground > INVALID_HEIGHT && std::fabs(ground - height) < MoveBlock::MAX_STEP
                ? ground : height);
        }

        std::sort(floors.begin(), floors.end());
        std::vector<float> merged;
        for (float floor : floors)
            if (merged.empty() || floor - merged.back() > FLOOR_MERGE)
                merged.push_back(floor);
        return merged;
    }

    Table Bake(Map* map, float x, float y, Settings const& settings)
    {
        Table table;
        table.MapId = map->GetId();
        table.Bake = settings;
        table.Bake.Bearings = std::max<uint32>(1, settings.Bearings);
        float const gridX = std::floor(x / SIZE_OF_GRIDS) * SIZE_OF_GRIDS;
        float const gridY = std::floor(y / SIZE_OF_GRIDS) * SIZE_OF_GRIDS;
        table.Side = uint32(std::ceil(SIZE_OF_GRIDS / settings.Cell));
        table.MinX = gridX + settings.Cell / 2.0f;
        table.MinY = gridY + settings.Cell / 2.0f;

        uint32 const cells = table.Side * table.Side;
        uint32 const bearings = table.Bake.Bearings;
        std::vector<std::vector<float>> floors(cells);
        std::vector<std::vector<Ground::Bearing>> readings(cells);
        std::vector<std::vector<Ground::Room>> rooms(cells);

        dtNavMesh const* mesh = map->GetMapCollisionData().GetMMapData().GetNavMesh();
        uint32 const threads = settings.Threads ? settings.Threads : std::max(1u, std::thread::hardware_concurrency());
        std::atomic<uint32> nextRow{ 0 };
        auto const started = std::chrono::steady_clock::now();

        auto work = [&]()
        {
            QueryOwner owner(mesh);
            dtNavMeshQuery const* query = owner.Query;
            float const spacing = TWO_PI / float(bearings);
            for (uint32 row = nextRow++; row < table.Side; row = nextRow++)
            {
                for (uint32 column = 0; column < table.Side; ++column)
                {
                    uint32 const cell = row * table.Side + column;
                    float const cx = table.MinX + float(column) * settings.Cell;
                    float const cy = table.MinY + float(row) * settings.Cell;
                    floors[cell] = Floors(map, query, cx, cy);
                    for (float z : floors[cell])
                    {
                        Ground::Origin const at{ cx, cy, z, PHASEMASK_NORMAL, BAKE_COLLISION };
                        dtPolyRef const start = Ground::StartPoly(query, at);
                        if (settings.WedgeRays == 3)
                        {
                            // Three rays a wedge, at its two edges and its centre: each edge is also the next
                            // wedge's, so the bearings take 2 * Bearings headings, not 3 * -- and the result is
                            // SenseWedge's exactly.
                            std::vector<Ground::Bearing> half(2 * bearings);
                            for (uint32 index = 0; index < 2 * bearings; ++index)
                                half[index] = Ground::Sense(map, query, start, at, float(index) * spacing / 2.0f,
                                    settings.Pitch);
                            for (uint32 bearing = 0; bearing < bearings; ++bearing)
                                readings[cell].push_back(Ground::Worst(Ground::Worst(
                                    half[(2 * bearing + 2 * bearings - 1) % (2 * bearings)], half[2 * bearing]),
                                    half[2 * bearing + 1]));
                        }
                        else
                        {
                            for (uint32 bearing = 0; bearing < bearings; ++bearing)
                                readings[cell].push_back(Ground::SenseWedge(map, query, start, at,
                                    float(bearing) * spacing, spacing / 2.0f, settings.WedgeRays, settings.Pitch));
                        }
                        rooms[cell].push_back(Ground::MeasureRoom(query, start, at));
                    }
                }
            }
        };

        std::vector<std::thread> pool;
        for (uint32 index = 1; index < threads; ++index)
            pool.emplace_back(work);
        work();
        for (std::thread& thread : pool)
            thread.join();

        table.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        table.First.reserve(cells + 1);
        for (uint32 cell = 0; cell < cells; ++cell)
        {
            table.First.push_back(uint32(table.FloorZ.size()));
            table.FloorZ.insert(table.FloorZ.end(), floors[cell].begin(), floors[cell].end());
            for (Ground::Bearing const& bearing : readings[cell])
                table.Readings.push_back(Pack(bearing));
            for (Ground::Room const& room : rooms[cell])
                table.Rooms.push_back(Pack(room));
        }
        table.First.push_back(uint32(table.FloorZ.size()));
        return table;
    }

    bool Lookup(Table const& table, float x, float y, float z, float facing, Turn turn, bool blend, Reading& out)
    {
        float const u = (x - table.MinX) / table.Bake.Cell;
        float const v = (y - table.MinY) / table.Bake.Cell;
        auto floorOf = [&](int32 column, int32 row) -> int32
        {
            if (column < 0 || row < 0 || column >= int32(table.Side) || row >= int32(table.Side))
                return -1;
            uint32 const cell = uint32(row) * table.Side + uint32(column);
            uint32 const first = table.First[cell];
            int32 const floor = FloorAt(&table.FloorZ[first], table.First[cell + 1] - first, z);
            return floor < 0 ? -1 : int32(first) + floor;
        };

        // The nearest cell with a floor within a step: a cell's centre can fall in a wall the seat stands against,
        // and then the next nearest of the four around the point is where it stands.
        int32 nearest = floorOf(int32(std::lround(u)), int32(std::lround(v)));
        if (nearest < 0)
        {
            int32 const column = int32(std::floor(u));
            int32 const row = int32(std::floor(v));
            float best = 4.0f;
            for (int32 corner = 0; corner < 4; ++corner)
            {
                int32 const c = column + (corner & 1);
                int32 const r = row + (corner >> 1);
                float const distance = (u - float(c)) * (u - float(c)) + (v - float(r)) * (v - float(r));
                int32 const floor = floorOf(c, r);
                if (floor >= 0 && distance < best)
                {
                    best = distance;
                    nearest = floor;
                }
            }
        }
        if (!blend)
        {
            if (nearest < 0)
                return false;
            TurnReadings(table, uint32(nearest), facing, turn, out.Rays);
            out.Room = Unpack(table.Rooms[nearest]);
            return true;
        }

        // The four cells around the point, each on its own floor within a step of z, weighted by how near; a
        // cell with no such floor drops out and the others share its weight.
        int32 const column = int32(std::floor(u));
        int32 const row = int32(std::floor(v));
        float const fu = u - float(column);
        float const fv = v - float(row);
        Ground::Bearing sum[SENSE_RAYS] = {};
        for (Ground::Bearing& bearing : sum)
            bearing = { 0.0f, 0.0f, 0.0f, 0.0f };
        float clearance = 0.0f;
        float total = 0.0f;
        for (int32 corner = 0; corner < 4; ++corner)
        {
            int32 const dc = corner & 1;
            int32 const dr = corner >> 1;
            float const weight = (dc ? fu : 1.0f - fu) * (dr ? fv : 1.0f - fv);
            int32 const floor = floorOf(column + dc, row + dr);
            if (floor < 0 || weight <= 0.0f)
                continue;
            Ground::Bearing turned[SENSE_RAYS];
            TurnReadings(table, uint32(floor), facing, turn, turned);
            for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
                Accumulate(sum[ray], Scaled(turned[ray], weight));
            clearance += Unpack(table.Rooms[floor]).Clearance * weight;
            total += weight;
        }
        if (total <= 0.0f)
            return false;

        for (uint32 ray = 0; ray < SENSE_RAYS; ++ray)
            out.Rays[ray] = Scaled(sum[ray], 1.0f / total);
        // Which way is out is a direction and does not average; the nearest cell's, when it has one.
        out.Room = nearest >= 0 ? Unpack(table.Rooms[nearest]) : Ground::Room();
        out.Room.Clearance = clearance / total;
        return true;
    }

    std::string Compare(Map* map, Table const& table, uint32 samples, uint32 seed, float nearX, float nearY,
        float radius)
    {
        std::ostringstream out;
        out << std::fixed;

        uint32 const cells = table.Side * table.Side;
        uint32 histogram[5] = {};
        for (uint32 cell = 0; cell < cells; ++cell)
            ++histogram[std::min<uint32>(4, table.First[cell + 1] - table.First[cell])];
        std::size_t const floors = table.FloorZ.size();
        // As a file would hold it: a byte per reading field, the room in two, the floor's height in two, and an
        // index entry per cell.
        std::size_t const bytes = std::size_t(cells) * 4 + floors * (std::size_t(table.Bake.Bearings) * 4 + 4);

        out << std::setprecision(1) << "Baked map " << table.MapId << " grid at (" << table.MinX - table.Bake.Cell / 2
            << ", " << table.MinY - table.Bake.Cell / 2 << "): " << table.Side << "x" << table.Side << " cells of "
            << table.Bake.Cell << " yd, " << table.Bake.Bearings << " bearings, " << table.Bake.WedgeRays
            << " rays a wedge, march pitch " << std::setprecision(2) << table.Bake.Pitch << " yd\n";
        out << std::setprecision(1) << "  " << table.Seconds << " s to bake, " << floors
            << " floors; cells with 0/1/2/3/4+ floors: " << histogram[0] << "/" << histogram[1] << "/"
            << histogram[2] << "/" << histogram[3] << "/" << histogram[4] << "; "
            << std::setprecision(2) << double(bytes) / (1024.0 * 1024.0) << " MB quantised\n";

        dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        if (!query)
        {
            out << "  no navmesh query on this map: nothing to compare\n";
            return out.str();
        }

        std::mt19937 random(seed);
        std::uniform_real_distribution<float> along(0.0f, SIZE_OF_GRIDS);
        std::uniform_real_distribution<float> angle(0.0f, TWO_PI);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        float const gridX = table.MinX - table.Bake.Cell / 2.0f;
        float const gridY = table.MinY - table.Bake.Cell / 2.0f;

        Gap stale;          // the live probe against itself, as a seat reads it up to one refresh late
        Gap denseStale;     // the same for the dense wedge probe: what a table's cell size is to be judged by
        Gap semantic;       // today's probe against the dense wedge one, both live
        Gap tables[4];      // the dense live probe against each way of reading the table
        Gap total;          // today's probe against the table read the chosen way
        static constexpr char const* TABLE_NAMES[4] = {
            "dense live vs table nearest", "dense live vs table overlap", "dense live vs table nearest+blend",
            "dense live vs table overlap+blend" };
        uint32 compared = 0;
        uint32 misses = 0;
        uint32 offTable = 0;
        double legacyNs = 0.0;
        double denseNs = 0.0;
        double lookupNs = 0.0;

        for (uint32 attempt = 0; compared < samples && attempt < samples * 20; ++attempt)
        {
            float x = gridX + along(random);
            float y = gridY + along(random);
            if (radius > 0.0f)
            {
                // Within `radius` of the point asked about, and on the grid the table covers.
                float const spin = angle(random);
                float const out = radius * std::sqrt(unit(random));
                x = std::clamp(nearX + out * std::cos(spin), gridX, gridX + SIZE_OF_GRIDS - 0.01f);
                y = std::clamp(nearY + out * std::sin(spin), gridY, gridY + SIZE_OF_GRIDS - 0.01f);
            }
            std::vector<float> const here = Floors(map, query, x, y);
            if (here.empty())
            {
                ++misses;
                continue;
            }
            std::size_t const pick = std::size_t(unit(random) * float(here.size()));
            float const z = here[std::min<std::size_t>(here.size() - 1, pick)];
            float const facing = angle(random);
            Ground::Origin const at{ x, y, z, PHASEMASK_NORMAL, BAKE_COLLISION };

            auto mark = std::chrono::steady_clock::now();
            Reading const legacy = Live(map, query, at, facing, 1, 0.0f, true);
            auto now = std::chrono::steady_clock::now();
            legacyNs += std::chrono::duration<double, std::nano>(now - mark).count();
            mark = now;
            Reading const dense = Live(map, query, at, facing, table.Bake.WedgeRays, table.Bake.Pitch, false);
            now = std::chrono::steady_clock::now();
            denseNs += std::chrono::duration<double, std::nano>(now - mark).count();

            Reading looked[4];
            bool found = true;
            for (uint32 way = 0; way < 4; ++way)
            {
                mark = std::chrono::steady_clock::now();
                found = found && Lookup(table, x, y, z, facing, way & 1 ? Turn::Overlap : Turn::Nearest, way >= 2,
                    looked[way]);
                lookupNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - mark).count();
            }
            if (!found)
            {
                ++offTable;
                continue;
            }

            // Where the seat might have been when its probe was last refreshed: up to MARCH_REFRESH_YARDS away,
            // turned up to MARCH_REFRESH_RADIANS, on the same floor.
            float const back = angle(random);
            float const distance = MoveBlock::MARCH_REFRESH_YARDS * unit(random);
            float const oldX = x + distance * std::cos(back);
            float const oldY = y + distance * std::sin(back);
            std::vector<float> const there = Floors(map, query, oldX, oldY);
            int32 const oldFloor = there.empty() ? -1 : FloorAt(there.data(), uint32(there.size()), z);
            if (oldFloor >= 0)
            {
                float const oldFacing = facing + (2.0f * unit(random) - 1.0f) * MoveBlock::MARCH_REFRESH_RADIANS;
                Ground::Origin const old{ oldX, oldY, there[oldFloor], PHASEMASK_NORMAL, BAKE_COLLISION };
                stale.Add(legacy, Live(map, query, old, oldFacing, 1, 0.0f, true));
                denseStale.Add(dense, Live(map, query, old, oldFacing, table.Bake.WedgeRays, table.Bake.Pitch, false));
            }

            semantic.Add(legacy, dense);
            for (uint32 way = 0; way < 4; ++way)
                tables[way].Add(dense, looked[way]);
            total.Add(legacy, looked[1]);
            ++compared;
        }

        out << std::setprecision(1) << "  " << compared << " places compared (" << misses << " draws off the mesh, "
            << offTable << " with no table floor within a step)\n";
        if (compared)
            out << "  per place: today's probe " << legacyNs / compared / 1000.0 << " us, the dense wedge probe "
                << denseNs / compared / 1000.0 << " us, a table lookup " << lookupNs / compared / 4000.0 << " us\n";
        out << "\n  mean |gap| and share over 0.1   " << "      reach          step         shore         burns"
            << "     clearance\n";
        stale.Print(out, "today vs itself one refresh late");
        denseStale.Print(out, "dense vs itself one refresh late");
        semantic.Print(out, "today vs dense live (the change)");
        for (uint32 way = 0; way < 4; ++way)
            tables[way].Print(out, TABLE_NAMES[way]);
        total.Print(out, "today vs table overlap");
        return out.str();
    }

    Settings StandardSettings()
    {
        return Settings{ 2.0f, 16, 3, 0.5f, 0 };
    }

    int32 GridIndex(float coordinate)
    {
        return int32(std::floor(coordinate / SIZE_OF_GRIDS));
    }

    Reading SenseLive(Map* map, dtNavMeshQuery const* query, Ground::Origin const& at, float facing)
    {
        Settings const standard = StandardSettings();
        return Live(map, query, at, facing, standard.WedgeRays, standard.Pitch, false);
    }

    namespace
    {
        constexpr uint32 FILE_MAGIC = 0x42525041;      // "APRB"
        /// 1: the payload as it is; 2: the payload zstd-compressed, its raw size after the header.
        constexpr uint32 FILE_VERSION = 2;
        constexpr uint8 NO_WAY_OUT = 255;
        /// zstd's level for the tables: they are written once and read many times, and at 15 a grid still
        /// compresses in well under a second of the half minute its bake takes.
        constexpr int ZSTD_LEVEL = 15;

        struct Header
        {
            uint32 Magic = FILE_MAGIC;
            uint32 Version = FILE_VERSION;
            uint32 MapId = 0;
            int32 GridX = 0;
            int32 GridY = 0;
            float Cell = 0.0f;
            uint32 Bearings = 0;
            uint32 WedgeRays = 0;
            float Pitch = 0.0f;
            uint32 Side = 0;
            float MinX = 0.0f;
            float MinY = 0.0f;
            uint32 Floors = 0;
        };

        uint8 Unit(float value)
        {
            return uint8(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        template <typename T>
        void Append(std::vector<char>& payload, std::vector<T> const& values)
        {
            char const* bytes = reinterpret_cast<char const*>(values.data());
            payload.insert(payload.end(), bytes, bytes + values.size() * sizeof(T));
        }

        template <typename T>
        bool Take(std::vector<char> const& payload, std::size_t& offset, std::vector<T>& values, std::size_t count)
        {
            if (payload.size() - offset < count * sizeof(T))
                return false;
            values.resize(count);
            std::memcpy(values.data(), payload.data() + offset, count * sizeof(T));
            offset += count * sizeof(T);
            return true;
        }

        bool ReadHeader(std::ifstream& in, Header& header)
        {
            return in && in.read(reinterpret_cast<char*>(&header), sizeof(header)) && header.Magic == FILE_MAGIC
                && (header.Version == 1 || header.Version == 2) && header.Side > 0 && header.Side <= 4096
                && header.Bearings > 0 && header.Bearings <= 256;
        }
    }

    PackedBearing Pack(Ground::Bearing const& bearing)
    {
        return { Unit(bearing.Reach), int8(std::lround(std::clamp(bearing.Step, -1.0f, 1.0f) * 127.0f)),
            Unit(bearing.Shore), Unit(bearing.Burns) };
    }

    Ground::Bearing Unpack(PackedBearing const& bearing)
    {
        return { float(bearing.Reach) / 255.0f, float(bearing.Step) / 127.0f, float(bearing.Shore) / 255.0f,
            float(bearing.Burns) / 255.0f };
    }

    PackedRoom Pack(Ground::Room const& room)
    {
        float const turn = Position::NormalizeOrientation(room.Away) / TWO_PI;
        return { Unit(room.Clearance), room.Directed ? uint8(std::lround(turn * 255.0f) % 255) : NO_WAY_OUT };
    }

    Ground::Room Unpack(PackedRoom const& room)
    {
        Ground::Room out;
        out.Clearance = float(room.Clearance) / 255.0f;
        out.Directed = room.Away != NO_WAY_OUT;
        out.Away = out.Directed ? float(room.Away) / 255.0f * TWO_PI : 0.0f;
        return out;
    }

    std::size_t Table::Bytes() const
    {
        return sizeof(Table) + First.capacity() * sizeof(uint32) + FloorZ.capacity() * sizeof(float)
            + Readings.capacity() * sizeof(PackedBearing) + Rooms.capacity() * sizeof(PackedRoom);
    }

    bool Write(Table const& table, std::string const& path)
    {
        Header header;
        header.MapId = table.MapId;
        header.GridX = GridIndex(table.MinX);
        header.GridY = GridIndex(table.MinY);
        header.Cell = table.Bake.Cell;
        header.Bearings = table.Bake.Bearings;
        header.WedgeRays = table.Bake.WedgeRays;
        header.Pitch = table.Bake.Pitch;
        header.Side = table.Side;
        header.MinX = table.MinX;
        header.MinY = table.MinY;
        header.Floors = uint32(table.FloorZ.size());

        std::vector<char> payload;
        Append(payload, table.First);
        Append(payload, table.FloorZ);
        Append(payload, table.Readings);
        Append(payload, table.Rooms);
        std::vector<char> compressed(ZSTD_compressBound(payload.size()));
        std::size_t const size = ZSTD_compress(compressed.data(), compressed.size(), payload.data(), payload.size(),
            ZSTD_LEVEL);
        if (ZSTD_isError(size))
            return false;
        uint64 const rawBytes = payload.size();

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
            out.write(reinterpret_cast<char const*>(&rawBytes), sizeof(rawBytes));
            out.write(compressed.data(), std::streamsize(size));
            if (!out)
                return false;
        }
        std::filesystem::rename(partial, target, error);
        return !error;
    }

    bool Read(std::string const& path, Table& table)
    {
        std::ifstream in(path, std::ios::binary);
        Header header;
        if (!ReadHeader(in, header))
            return false;

        std::vector<char> payload;
        if (header.Version == 1)
            payload.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        else
        {
            uint64 rawBytes = 0;
            if (!in.read(reinterpret_cast<char*>(&rawBytes), sizeof(rawBytes)) || rawBytes > (uint64(1) << 32))
                return false;
            std::vector<char> const compressed{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
            payload.resize(rawBytes);
            std::size_t const size = ZSTD_decompress(payload.data(), payload.size(), compressed.data(),
                compressed.size());
            if (ZSTD_isError(size) || size != rawBytes)
                return false;
        }

        table.MapId = header.MapId;
        table.Bake = Settings{ header.Cell, header.Bearings, header.WedgeRays, header.Pitch, 0 };
        table.Side = header.Side;
        table.MinX = header.MinX;
        table.MinY = header.MinY;
        std::size_t const cells = std::size_t(header.Side) * header.Side;
        std::size_t offset = 0;
        return Take(payload, offset, table.First, cells + 1) && table.First.back() == header.Floors
            && Take(payload, offset, table.FloorZ, header.Floors)
            && Take(payload, offset, table.Readings, std::size_t(header.Floors) * header.Bearings)
            && Take(payload, offset, table.Rooms, header.Floors);
    }

    uint32 FileVersion(std::string const& path)
    {
        std::ifstream in(path, std::ios::binary);
        Header header;
        return ReadHeader(in, header) ? header.Version : 0;
    }

    namespace Store
    {
        namespace
        {
            struct Entry
            {
                std::shared_ptr<Table const> Table;     // nullptr: the grid has no file
                std::atomic<uint64> LastRead{ 0 };
            };

            bool g_baked = false;
            std::string g_dir;
            uint32 g_cacheGrids = 64;
            std::atomic<uint64> g_clock{ 0 };
            std::shared_mutex g_lock;
            /// Every grid asked about: its table, or none for a grid without a file (asked once, remembered).
            std::map<std::tuple<uint32, int32, int32>, Entry> g_tables;
        }

        void Configure(bool baked, std::string const& dir, uint32 cacheGrids)
        {
            std::unique_lock lock(g_lock);
            g_baked = baked;
            g_dir = dir;
            g_cacheGrids = std::max<uint32>(1, cacheGrids);
            g_tables.clear();
        }

        bool Baked()
        {
            return g_baked;
        }

        std::string const& Dir()
        {
            return g_dir;
        }

        std::string FileFor(uint32 mapId, int32 gridX, int32 gridY)
        {
            return (std::filesystem::path(g_dir) / Acore::StringFormat("{:03}_{}_{}.probe", mapId, gridX, gridY))
                .string();
        }

        std::shared_ptr<Table const> Find(uint32 mapId, float x, float y)
        {
            auto const key = std::make_tuple(mapId, GridIndex(x), GridIndex(y));
            uint64 const now = ++g_clock;
            {
                std::shared_lock lock(g_lock);
                auto const found = g_tables.find(key);
                if (found != g_tables.end())
                {
                    found->second.LastRead.store(now, std::memory_order_relaxed);
                    return found->second.Table;
                }
            }

            // Read outside the lock: a grid's file takes a few milliseconds, and the other maps' seats should not
            // wait on it. Two seats arriving on a new grid together may both read it; the first one in is kept.
            auto table = std::make_shared<Table>();
            std::shared_ptr<Table const> loaded;
            if (Read(FileFor(mapId, std::get<1>(key), std::get<2>(key)), *table))
                loaded = std::move(table);

            std::unique_lock lock(g_lock);
            auto [entry, inserted] = g_tables.try_emplace(key);
            if (!inserted)
                return entry->second.Table;
            entry->second.Table = loaded;
            entry->second.LastRead.store(now, std::memory_order_relaxed);

            // Over the cap: let the least recently read tables go. A seat still reading one keeps its own pointer.
            uint32 held = 0;
            for (auto const& [other, cached] : g_tables)
                held += cached.Table ? 1 : 0;
            while (held > g_cacheGrids)
            {
                auto oldest = g_tables.end();
                for (auto it = g_tables.begin(); it != g_tables.end(); ++it)
                    if (it->second.Table && it != entry && (oldest == g_tables.end()
                        || it->second.LastRead.load(std::memory_order_relaxed)
                            < oldest->second.LastRead.load(std::memory_order_relaxed)))
                        oldest = it;
                if (oldest == g_tables.end())
                    break;
                g_tables.erase(oldest);
                --held;
            }
            return loaded;
        }

        uint32 Loaded()
        {
            std::shared_lock lock(g_lock);
            uint32 count = 0;
            for (auto const& [key, entry] : g_tables)
                count += entry.Table ? 1 : 0;
            return count;
        }

        std::size_t Bytes()
        {
            std::shared_lock lock(g_lock);
            std::size_t bytes = 0;
            for (auto const& [key, entry] : g_tables)
                bytes += entry.Table ? entry.Table->Bytes() : 0;
            return bytes;
        }
    }
}
