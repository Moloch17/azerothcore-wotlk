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
#include "FieldGrids.h"
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
    constexpr uint8 NAV_ANY = NAV_GROUND | NAV_WATER | NAV_MAGMA | NAV_SLIME;
    constexpr uint8 OPEN_ABOVE = Field::Interval::OPEN_ABOVE;

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
    /// ground the core reports within a step of it, with the polygon's flags.
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

}

namespace Animus::Curriculum::LayeredField
{
    Grid Bake(Map* map, float x, float y, float cell, uint32 threads)
    {
        Grid grid;
        grid.MapId = map->GetId();
        grid.GridX = FieldGrids::GridIndex(x);
        grid.GridY = FieldGrids::GridIndex(y);
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
        }

        void Configure(std::string const& dir, uint32 cacheGrids)
        {
            std::unique_lock lock(g_lock);
            g_enabled = !dir.empty();
            g_dir = dir;
            g_cacheGrids = std::max<uint32>(9, cacheGrids);
            g_fields.clear();
        }

        bool Enabled()
        {
            return g_enabled;
        }

        std::string const& Dir()
        {
            return g_dir;
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

            // Read outside the lock: the other maps' routes should not wait on a file.
            auto field = std::make_shared<Grid>();
            std::shared_ptr<Grid const> loaded;
            if (Read(FileFor(mapId, gridX, gridY), *field))
                loaded = std::move(field);
            // Many more reads than the cache holds is the cache letting go of fields its routes still cross: every
            // plan there reads a file. Said once.
            uint64 const reads = FileReads.fetch_add(1, std::memory_order_relaxed) + 1;
            if (reads == uint64(g_cacheGrids) * 4)
                LOG_WARN("module.animus", "Layered fields: {} field files read with AnimusForge.Probe.CacheGrids = "
                    "{}: the routes cross more grids than it holds. Raise it (about 4 MB a field).", reads,
                    g_cacheGrids);

            std::unique_lock lock(g_lock);
            auto [entry, inserted] = g_fields.try_emplace(key);
            if (!inserted)
                return entry->second.Field;
            entry->second.Field = loaded;
            entry->second.LastRead.store(now, std::memory_order_relaxed);

            // Over the cap: let the least recently read fields go. A route still reading one keeps its own pointer.
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
