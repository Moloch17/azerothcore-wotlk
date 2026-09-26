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
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include "Object.h"
#include "ProbeBake.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <zstd.h>

namespace
{
    namespace Field = Animus::Curriculum::LayeredField;

    /// How far up a floor's headroom is looked for; nothing solid within it is open sky.
    constexpr float HEADROOM_SEARCH = 100.0f;
    /// Less room than this above a floor is no room: the floor is a surface seen from inside something solid.
    constexpr float MIN_HEADROOM = 0.5f;
    /// Floors closer together than this in one column are one floor.
    constexpr float FLOOR_MERGE = 0.5f;
    /// Where the topmost surface is looked for from: this far above the terrain.
    constexpr float SKY_ABOVE = 600.0f;

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

    std::vector<Field::Interval> Column(Map* map, dtNavMeshQuery const* query, float x, float y)
    {
        StaticVMapCollisionData const& collision = map->GetMapCollisionData().GetStaticTree();
        float const terrain = map->GetGridHeight(x, y);
        float const sky = (terrain > INVALID_HEIGHT ? terrain : 0.0f) + SKY_ABOVE;
        float const top = map->GetHeight(PHASEMASK_NORMAL, x, y, sky, true, SKY_ABOVE * 4.0f);

        std::vector<float> floors = Animus::Curriculum::ProbeBake::Floors(map, query, x, y);
        if (terrain > INVALID_HEIGHT)
            floors.push_back(terrain);
        if (top > INVALID_HEIGHT)
            floors.push_back(top);
        std::sort(floors.begin(), floors.end());

        std::vector<Field::Interval> column;
        for (float floor : floors)
        {
            if (!column.empty() && floor - column.back().Floor <= FLOOR_MERGE)
                continue;

            Field::Interval interval;
            interval.Floor = floor;
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            bool const roofed = collision.GetObjectHitPos(x, y, floor + 0.05f, x, y, floor + HEADROOM_SEARCH, rx, ry,
                rz, 0.0f);
            interval.Headroom = roofed ? std::max(0.0f, rz - floor) : Field::OPEN_SKY;
            if (interval.Headroom < MIN_HEADROOM)
                continue;

            LiquidData const liquid = map->GetLiquidData(PHASEMASK_NORMAL, x, y, floor + 0.1f, 2.0f, {});
            if (liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > floor)
            {
                interval.Liquid = liquid.Level;
                interval.LiquidFlags = uint8(liquid.Flags);
            }
            column.push_back(interval);
        }
        return column;
    }

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
            if (z >= interval.Floor && z < interval.Floor + interval.Headroom)
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

    std::string Compare(Map* map, Grid const& grid, uint32 samples, uint32 seed)
    {
        std::ostringstream out;
        out << std::fixed;
        uint32 const cells = grid.Side * grid.Side;
        uint32 histogram[6] = {};
        uint32 liquid = 0;
        for (uint32 cell = 0; cell < cells; ++cell)
            ++histogram[std::min<uint32>(5, grid.First[cell + 1] - grid.First[cell])];
        for (Interval const& interval : grid.Intervals)
            liquid += interval.Liquid > INVALID_HEIGHT ? 1 : 0;

        // As a file would hold it: per interval the floor and headroom in eighths of a yard (int16, uint16, open sky
        // 0xFFFF), the liquid's surface above the floor in eighths (uint16, 0 none) and its flags; a count per cell.
        std::vector<uint8> packed;
        packed.reserve(cells + grid.Intervals.size() * 7);
        for (uint32 cell = 0; cell < cells; ++cell)
            packed.push_back(uint8(std::min<uint32>(255, grid.First[cell + 1] - grid.First[cell])));
        for (Interval const& interval : grid.Intervals)
        {
            int16 const floor = int16(std::clamp(std::lround(interval.Floor * 8.0f), -32768L, 32767L));
            uint16 const headroom = interval.Headroom >= OPEN_SKY ? 0xFFFF
                : uint16(std::min(65534L, std::lround(interval.Headroom * 8.0f)));
            uint16 const depth = interval.Liquid > INVALID_HEIGHT
                ? uint16(std::min(65535L, std::lround((interval.Liquid - interval.Floor) * 8.0f))) : 0;
            for (uint16 word : { uint16(floor), headroom, depth })
            {
                packed.push_back(uint8(word & 0xFF));
                packed.push_back(uint8(word >> 8));
            }
            packed.push_back(interval.LiquidFlags);
        }
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
            float const room = std::min(interval.Headroom - 0.5f, 60.0f);
            if (room < 2.0f)
                continue;
            float const z = interval.Floor + 1.5f + unit(random) * (room - 1.5f);
            float const heading = angle(random);

            auto mark = std::chrono::steady_clock::now();
            float const baked = FlightReach(grid, x, y, z, heading, RANGE, PITCH);
            auto now = std::chrono::steady_clock::now();
            bakedNs += std::chrono::duration<double, std::nano>(now - mark).count();
            float const live = LiveFlightReach(map, x, y, z, heading, RANGE, PITCH);
            liveNs += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - now).count();

            (z - interval.Floor < 10.0f ? low : high).Add(baked, live, RANGE);
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
}
