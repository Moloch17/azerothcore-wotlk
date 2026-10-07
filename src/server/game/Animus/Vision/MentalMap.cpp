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
#include <limits>

namespace
{
    using namespace Animus::Vision;

    /// The clock is kept below this many seconds: a cell's Seen is 16 bits. A map older than that starts again.
    constexpr double CLOCK_LIMIT = 65000.0;
    /// The body's line from where it last stood is walked only when it is this near (a teleport is no walk).
    constexpr float BODY_STEP_MAX = 8.0f;

    [[nodiscard]] int32_t FloorDiv(int32_t a, int32_t b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    [[nodiscard]] int32_t CellOf(float v)
    {
        return int32_t(std::floor(v / MAP_CELL));
    }

    [[nodiscard]] bool HasFloor(MapCell const& cell)
    {
        return cell.Floor[0] != NO_FLOOR || cell.Floor[1] != NO_FLOOR;
    }

    /// Keeps the floor layers in order, the lower first; a wall's flag moves with its layer.
    void SortLayers(MapCell& cell)
    {
        bool const swap = (cell.Floor[0] == NO_FLOOR && cell.Floor[1] != NO_FLOOR)
            || (cell.Floor[0] != NO_FLOOR && cell.Floor[1] != NO_FLOOR && cell.Floor[0] > cell.Floor[1]);
        if (!swap)
            return;
        std::swap(cell.Floor[0], cell.Floor[1]);
        uint8_t const low = cell.Flags & MAP_WALL_LOW;
        uint8_t const high = cell.Flags & MAP_WALL_HIGH;
        cell.Flags = uint8_t((cell.Flags & ~(MAP_WALL_LOW | MAP_WALL_HIGH)) | (low ? MAP_WALL_HIGH : 0)
            | (high ? MAP_WALL_LOW : 0));
    }

    /// A floor at `z` into the cell's layers: the layer within LAYER_MERGE takes it (the latest look), else a free
    /// layer, else the nearer layer is replaced.
    void InsertFloor(MapCell& cell, float z)
    {
        int16_t const q = QuantiseFloor(z);
        for (int16_t& layer : cell.Floor)
            if (layer != NO_FLOOR && std::fabs(FloorHeight(layer) - z) <= LAYER_MERGE)
            {
                layer = q;
                SortLayers(cell);
                return;
            }
        if (cell.Floor[0] == NO_FLOOR)
            cell.Floor[0] = q;
        else if (cell.Floor[1] == NO_FLOOR)
            cell.Floor[1] = q;
        else
        {
            bool const lower = std::fabs(FloorHeight(cell.Floor[0]) - z) <= std::fabs(FloorHeight(cell.Floor[1]) - z);
            cell.Floor[lower ? 0 : 1] = q;
        }
        SortLayers(cell);
    }

    /// When the entity filed in a cell was seen, in map seconds (approximate: its bucket's start before the cell's
    /// last look).
    [[nodiscard]] double EntityTime(MapCell const& cell)
    {
        return double(cell.Seen) - 1.0 - double(ENTITY_AGE_BUCKETS[cell.Entity >> 5]);
    }

    /// The age of a cell's entity at map second `clock`, or a negative number for none.
    [[nodiscard]] double EntityAge(MapCell const& cell, double clock)
    {
        if (!(cell.Entity & CLASS_MASK) || !cell.Seen)
            return -1.0;
        return std::max(0.0, clock - EntityTime(cell));
    }

    /// `from` folded into `into` (a coarse cell): its floors as layers, its flags, the newer look and entity.
    void Merge(MapCell& into, MapCell const& from)
    {
        for (int16_t layer : from.Floor)
            if (layer != NO_FLOOR)
                InsertFloor(into, FloorHeight(layer));
        into.Flags |= from.Flags;
        if (from.Entity & CLASS_MASK)
        {
            if (!(into.Entity & CLASS_MASK) || EntityTime(from) >= EntityTime(into))
            {
                // Re-expressed against the newer of the two looks below.
                double const time = EntityTime(from);
                uint16_t const seen = std::max(into.Seen, from.Seen);
                into.Entity = uint8_t((from.Entity & CLASS_MASK)
                    | (EntityAgeBucket(float(double(seen) - 1.0 - time)) << 5));
                into.Seen = seen;
                return;
            }
        }
        if (from.Seen > into.Seen)
        {
            if (into.Entity & CLASS_MASK)
            {
                double const time = EntityTime(into);
                into.Entity = uint8_t((into.Entity & CLASS_MASK)
                    | (EntityAgeBucket(float(double(from.Seen) - 1.0 - time)) << 5));
            }
            into.Seen = from.Seen;
        }
    }

    [[nodiscard]] bool IsEntityClass(Class what)
    {
        return what >= Class::Door;
    }
}

uint8_t Animus::Vision::EntityAgeBucket(float seconds)
{
    uint8_t bucket = 0;
    for (uint8_t index = 1; index < 8; ++index)
        if (seconds >= ENTITY_AGE_BUCKETS[index])
            bucket = index;
    return bucket;
}

uint8_t Animus::Vision::AgeByte(float seconds)
{
    float const scaled = std::round(CROP_AGE_SCALE * std::log2(1.0f + std::max(0.0f, seconds)));
    return uint8_t(std::min(254.0f, scaled));
}

Animus::Vision::CropCell Animus::Vision::DecodeCropCell(uint8_t const* bytes)
{
    CropCell cell;
    cell.Code = MapCode(std::min<uint8_t>(bytes[CROP_CODE], uint8_t(MAP_CODES - 1)));
    cell.HasHeight = bytes[CROP_HEIGHT] != 0;
    cell.Height = cell.HasHeight ? float(int32_t(bytes[CROP_HEIGHT]) - int32_t(CROP_HEIGHT_ZERO)) * CROP_HEIGHT_STEP
        : 0.0f;
    cell.Visited = bytes[CROP_VISITED] != 0;
    cell.Seen = bytes[CROP_AGE] != CROP_AGE_NEVER;
    cell.Age = cell.Seen ? std::exp2(float(bytes[CROP_AGE]) / CROP_AGE_SCALE) - 1.0f : 0.0f;
    cell.Entity = uint8_t(bytes[CROP_CLASS] & CLASS_MASK);
    cell.Frontier = bytes[CROP_FRONTIER] != 0;
    return cell;
}

bool Animus::Vision::NearestFloor(MapCell const& cell, float feetZ, float& z)
{
    bool found = false;
    for (int16_t layer : cell.Floor)
    {
        if (layer == NO_FLOOR)
            continue;
        float const height = FloorHeight(layer);
        if (!found || std::fabs(height - feetZ) < std::fabs(z - feetZ))
            z = height;
        found = true;
    }
    return found;
}

Animus::Vision::MapCode Animus::Vision::CodeOf(MapCell const& cell, float feetZ)
{
    float z = 0.0f;
    bool const floor = NearestFloor(cell, feetZ, z);
    // The wall of the layer the feet are nearest; with no floor known, a wall at either.
    uint8_t wall = MAP_WALL_LOW | MAP_WALL_HIGH;
    if (floor && cell.Floor[1] != NO_FLOOR)
        wall = FloorHeight(cell.Floor[1]) == z ? MAP_WALL_HIGH : MAP_WALL_LOW;
    else if (floor)
        wall = MAP_WALL_LOW;
    if (cell.Flags & wall)
        return MapCode::Wall;
    if (cell.Flags & MAP_DOOR)
        return MapCode::Door;
    if (cell.Flags & MAP_HAZARD)
        return MapCode::Hazard;
    return floor ? MapCode::Floor : MapCode::Unknown;
}

void Animus::Vision::MentalMap::Clear()
{
    _tiles.clear();
    _coarse.clear();
    _clock = 0.0;
    _touch = 0;
    _lastTile = nullptr;
    _hasBody = false;
}

void Animus::Vision::MentalMap::Advance(float seconds)
{
    _clock += std::max(0.0f, seconds);
    if (_clock >= CLOCK_LIMIT)
        Clear();
}

uint16_t Animus::Vision::MentalMap::Stamp() const
{
    return uint16_t(std::min(65535.0, std::floor(_clock) + 1.0));
}

Animus::Vision::MapCell& Animus::Vision::MentalMap::Touch(int32_t cx, int32_t cy)
{
    int32_t const tx = FloorDiv(cx, MAP_TILE);
    int32_t const ty = FloorDiv(cy, MAP_TILE);
    int64_t const key = TileKey(tx, ty);
    Tile* tile = _lastTile && _lastKey == key ? _lastTile : nullptr;
    if (!tile)
    {
        auto found = _tiles.find(key);
        if (found == _tiles.end())
        {
            if (_tiles.size() >= std::max<uint32_t>(1, _settings.MaxTiles))
                Evict();
            found = _tiles.emplace(key, std::make_unique<Tile>()).first;
        }
        tile = found->second.get();
        _lastKey = key;
        _lastTile = tile;
    }
    tile->Touched = ++_touch;
    return tile->Cells[std::size_t((cy - ty * MAP_TILE) * MAP_TILE + (cx - tx * MAP_TILE))];
}

void Animus::Vision::MentalMap::Evict()
{
    auto oldest = _tiles.end();
    for (auto it = _tiles.begin(); it != _tiles.end(); ++it)
        if (oldest == _tiles.end() || it->second->Touched < oldest->second->Touched)
            oldest = it;
    if (oldest == _tiles.end())
        return;
    if (_settings.CoarseTiles)
        Fold(oldest->first, *oldest->second);
    if (_lastTile == oldest->second.get())
        _lastTile = nullptr;
    _tiles.erase(oldest);
}

void Animus::Vision::MentalMap::Fold(int64_t key, Tile const& tile)
{
    int32_t const tx = int32_t(key >> 32);
    int32_t const ty = int32_t(uint32_t(key & 0xFFFFFFFF));
    for (int32_t row = 0; row < MAP_TILE; ++row)
        for (int32_t col = 0; col < MAP_TILE; ++col)
        {
            MapCell const& cell = tile.Cells[std::size_t(row * MAP_TILE + col)];
            if (!cell.Seen && !HasFloor(cell) && !cell.Flags)
                continue;
            int32_t const ccx = FloorDiv(tx * MAP_TILE + col, COARSE_CELL);
            int32_t const ccy = FloorDiv(ty * MAP_TILE + row, COARSE_CELL);
            int32_t const ctx = FloorDiv(ccx, MAP_TILE);
            int32_t const cty = FloorDiv(ccy, MAP_TILE);
            int64_t const coarseKey = TileKey(ctx, cty);
            auto found = _coarse.find(coarseKey);
            if (found == _coarse.end())
            {
                if (_coarse.size() >= _settings.CoarseTiles)
                {
                    auto oldest = _coarse.begin();
                    for (auto it = _coarse.begin(); it != _coarse.end(); ++it)
                        if (it->second->Touched < oldest->second->Touched)
                            oldest = it;
                    _coarse.erase(oldest);
                }
                found = _coarse.emplace(coarseKey, std::make_unique<Tile>()).first;
            }
            found->second->Touched = ++_touch;
            Merge(found->second->Cells[std::size_t((ccy - cty * MAP_TILE) * MAP_TILE + (ccx - ctx * MAP_TILE))], cell);
        }
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindCell(int32_t cx, int32_t cy) const
{
    int32_t const tx = FloorDiv(cx, MAP_TILE);
    int32_t const ty = FloorDiv(cy, MAP_TILE);
    auto const found = _tiles.find(TileKey(tx, ty));
    if (found == _tiles.end())
        return nullptr;
    return &found->second->Cells[std::size_t((cy - ty * MAP_TILE) * MAP_TILE + (cx - tx * MAP_TILE))];
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::Find(float x, float y) const
{
    return FindCell(CellOf(x), CellOf(y));
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindCoarse(int32_t cx, int32_t cy) const
{
    if (_coarse.empty())
        return nullptr;
    int32_t const ccx = FloorDiv(cx, COARSE_CELL);
    int32_t const ccy = FloorDiv(cy, COARSE_CELL);
    int32_t const ctx = FloorDiv(ccx, MAP_TILE);
    int32_t const cty = FloorDiv(ccy, MAP_TILE);
    auto const found = _coarse.find(TileKey(ctx, cty));
    if (found == _coarse.end())
        return nullptr;
    return &found->second->Cells[std::size_t((ccy - cty * MAP_TILE) * MAP_TILE + (ccx - ctx * MAP_TILE))];
}

void Animus::Vision::MentalMap::MarkSeen(MapCell& cell, uint16_t stamp)
{
    // The entity filed here stays (aged, never assumed still there, nor gone): its age is re-expressed against the
    // new look.
    if ((cell.Entity & CLASS_MASK) && cell.Seen && stamp != cell.Seen)
    {
        double const time = EntityTime(cell);
        cell.Entity = uint8_t((cell.Entity & CLASS_MASK) | (EntityAgeBucket(float(double(stamp) - 1.0 - time)) << 5));
    }
    cell.Seen = stamp;
}

void Animus::Vision::MentalMap::WriteEntity(MapCell& cell, Class what, uint16_t stamp)
{
    cell.Seen = stamp;
    cell.Entity = uint8_t(uint8_t(what) & CLASS_MASK);
}

void Animus::Vision::MentalMap::WriteFloor(int32_t cx, int32_t cy, float z, uint16_t stamp, MapWriteStats* stats)
{
    MapCell& cell = Touch(cx, cy);
    InsertFloor(cell, z);
    MarkSeen(cell, stamp);
    if (stats)
        ++stats->Floors;

    // A step or a drop: a neighbouring floor with no layer within EDGE_RISE of this one.
    static int32_t const NEIGHBOURS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    bool edge = false;
    for (auto const& offset : NEIGHBOURS)
    {
        MapCell const* next = FindCell(cx + offset[0], cy + offset[1]);
        float nz = 0.0f;
        if (!next || !NearestFloor(*next, z, nz) || std::fabs(nz - z) <= EDGE_RISE)
            continue;
        edge = true;
        Touch(cx + offset[0], cy + offset[1]).Flags |= MAP_EDGE;
    }
    if (edge)
        Touch(cx, cy).Flags |= MAP_EDGE;
}

void Animus::Vision::MentalMap::WriteFrame(FrameHits const& hits, float feetZ, float bodyHeight, MapWriteStats* stats)
{
    uint16_t const stamp = Stamp();
    Vec3 const camera = hits.Camera;
    for (RayHit const& ray : hits.Rays)
    {
        bool const sky = ray.What == Class::Sky;
        bool const hitWithin = !sky && ray.Distance <= WRITE_REACH;
        float const reach = std::min(ray.Distance, WRITE_REACH);
        int32_t const hitX = CellOf(camera.X + ray.Dir.X * ray.Distance);
        int32_t const hitY = CellOf(camera.Y + ray.Dir.Y * ray.Distance);

        // **Seen free** (amendment 2): a 2D grid step over the map's cells from the camera to the hit (or the reach),
        // each cell taking the stretch of the ray above it; free only where that stretch is in the body's band.
        float const horizontal = std::sqrt(ray.Dir.X * ray.Dir.X + ray.Dir.Y * ray.Dir.Y);
        if (horizontal > 1e-4f && reach > 0.0f)
        {
            int32_t cx = CellOf(camera.X);
            int32_t cy = CellOf(camera.Y);
            int32_t const stepX = ray.Dir.X > 0.0f ? 1 : -1;
            int32_t const stepY = ray.Dir.Y > 0.0f ? 1 : -1;
            float const inf = std::numeric_limits<float>::infinity();
            float const deltaX = std::fabs(ray.Dir.X) > 1e-9f ? MAP_CELL / std::fabs(ray.Dir.X) : inf;
            float const deltaY = std::fabs(ray.Dir.Y) > 1e-9f ? MAP_CELL / std::fabs(ray.Dir.Y) : inf;
            float const edgeX = (ray.Dir.X > 0.0f ? float(cx + 1) : float(cx)) * MAP_CELL;
            float const edgeY = (ray.Dir.Y > 0.0f ? float(cy + 1) : float(cy)) * MAP_CELL;
            float nextX = std::fabs(ray.Dir.X) > 1e-9f ? (edgeX - camera.X) / ray.Dir.X : inf;
            float nextY = std::fabs(ray.Dir.Y) > 1e-9f ? (edgeY - camera.Y) / ray.Dir.Y : inf;
            float enter = 0.0f;
            while (enter < reach)
            {
                float const leave = std::min({ nextX, nextY, reach });
                // The hit's own cell is the hit's to mark.
                if (hitWithin && cx == hitX && cy == hitY)
                    break;
                if (stats)
                    ++stats->Steps;
                float const zA = camera.Z + ray.Dir.Z * enter;
                float const zB = camera.Z + ray.Dir.Z * leave;
                float const low = std::min(zA, zB);
                float const high = std::max(zA, zB);
                MapCell const* known = FindCell(cx, cy);
                bool free = false;
                if (known && HasFloor(*known))
                {
                    for (int16_t layer : known->Floor)
                        if (layer != NO_FLOOR && low >= FloorHeight(layer) - FLOOR_STEP
                            && high <= FloorHeight(layer) + bodyHeight)
                            free = true;
                }
                else
                    free = low >= feetZ - FREE_BAND && high <= feetZ + FREE_BAND;
                if (free)
                {
                    MapCell& cell = Touch(cx, cy);
                    cell.Flags |= MAP_FREE;
                    MarkSeen(cell, stamp);
                    if (stats)
                        ++stats->Free;
                }
                if (leave >= reach)
                    break;
                enter = leave;
                if (nextX < nextY)
                {
                    cx += stepX;
                    nextX += deltaX;
                }
                else
                {
                    cy += stepY;
                    nextY += deltaY;
                }
            }
        }

        if (!hitWithin)
            continue;
        if (stats)
            ++stats->Hits;
        switch (ray.What)
        {
            case Class::Terrain:
            case Class::Model:
            {
                if (ray.NormalZ >= FLOOR_NORMAL)
                {
                    WriteFloor(hitX, hitY, ray.Z, stamp, stats);
                    break;
                }
                if (ray.NormalZ <= CEILING_NORMAL)
                    break;
                // A wall within the body's band above a floor: the cell's own, else a neighbour's, else the feet's.
                float reference = 0.0f;
                bool referenced = false;
                uint8_t flag = MAP_WALL_LOW;
                MapCell const* here = FindCell(hitX, hitY);
                if (here)
                    for (uint32_t layer = 0; layer < 2; ++layer)
                        if (here->Floor[layer] != NO_FLOOR && FloorHeight(here->Floor[layer]) <= ray.Z + WALL_SLACK)
                        {
                            reference = FloorHeight(here->Floor[layer]);
                            referenced = true;
                            flag = layer == 0 ? MAP_WALL_LOW : MAP_WALL_HIGH;
                        }
                if (!referenced)
                {
                    static int32_t const NEIGHBOURS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (auto const& offset : NEIGHBOURS)
                    {
                        MapCell const* next = FindCell(hitX + offset[0], hitY + offset[1]);
                        float nz = 0.0f;
                        if (next && NearestFloor(*next, ray.Z, nz) && nz <= ray.Z + WALL_SLACK
                            && (!referenced || nz > reference))
                        {
                            reference = nz;
                            referenced = true;
                        }
                    }
                }
                bool const inBand = referenced ? ray.Z <= reference + bodyHeight + WALL_SLACK
                    : ray.Z >= feetZ - FREE_BAND && ray.Z <= feetZ + bodyHeight + WALL_SLACK;
                if (!inBand)
                    break;
                MapCell& cell = Touch(hitX, hitY);
                cell.Flags |= flag;
                MarkSeen(cell, stamp);
                if (stats)
                    ++stats->Walls;
                break;
            }
            case Class::Water:
            case Class::Deadly:
            {
                MapCell& cell = Touch(hitX, hitY);
                cell.Flags |= MAP_HAZARD;
                MarkSeen(cell, stamp);
                break;
            }
            default:
            {
                if (!IsEntityClass(ray.What))
                    break;
                MapCell& cell = Touch(hitX, hitY);
                if (ray.What == Class::Door)
                    cell.Flags |= MAP_DOOR;
                WriteEntity(cell, ray.What, stamp);
                if (stats)
                    ++stats->Entities;
                break;
            }
        }
    }
}

void Animus::Vision::MentalMap::WriteBody(float x, float y, float z, bool grounded)
{
    uint16_t const stamp = Stamp();
    // The line from where it last stood, a cell at a time.
    if (_hasBody)
    {
        float const dx = x - _bodyX;
        float const dy = y - _bodyY;
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length <= BODY_STEP_MAX)
        {
            int32_t const steps = int32_t(std::ceil(length / (0.5f * MAP_CELL)));
            for (int32_t step = 1; step < steps; ++step)
            {
                float const t = float(step) / float(steps);
                MapCell& cell = Touch(CellOf(_bodyX + dx * t), CellOf(_bodyY + dy * t));
                cell.Flags |= MAP_VISITED;
                MarkSeen(cell, stamp);
            }
        }
    }
    MapCell& cell = Touch(CellOf(x), CellOf(y));
    cell.Flags |= MAP_VISITED;
    if (grounded)
        InsertFloor(cell, z);
    MarkSeen(cell, stamp);
    _hasBody = true;
    _bodyX = x;
    _bodyY = y;
}

void Animus::Vision::MentalMap::Crop(float x, float y, float z, float yaw, uint8_t* out) const
{
    // A window of the map's cells round the body, wide enough for the turned crop and a cell of neighbours for the
    // frontier: one copy of the tiles' rows rather than a tile lookup per sample.
    float const halfDiagonal = float(CROP) * CROP_CELL * 0.5f * 1.41422f;
    int32_t const radius = int32_t(std::ceil(halfDiagonal / MAP_CELL)) + 2;
    int32_t const side = 2 * radius + 1;
    int32_t const x0 = CellOf(x) - radius;
    int32_t const y0 = CellOf(y) - radius;
    thread_local std::vector<MapCell> window;
    window.assign(std::size_t(side) * std::size_t(side), MapCell());
    int32_t const tx0 = FloorDiv(x0, MAP_TILE);
    int32_t const tx1 = FloorDiv(x0 + side - 1, MAP_TILE);
    int32_t const ty0 = FloorDiv(y0, MAP_TILE);
    int32_t const ty1 = FloorDiv(y0 + side - 1, MAP_TILE);
    for (int32_t ty = ty0; ty <= ty1; ++ty)
        for (int32_t tx = tx0; tx <= tx1; ++tx)
        {
            auto const found = _tiles.find(TileKey(tx, ty));
            Tile const* tile = found == _tiles.end() ? nullptr : found->second.get();
            if (!tile && _coarse.empty())
                continue;
            int32_t const cx0 = std::max(x0, tx * MAP_TILE);
            int32_t const cx1 = std::min(x0 + side - 1, tx * MAP_TILE + MAP_TILE - 1);
            int32_t const cy0 = std::max(y0, ty * MAP_TILE);
            int32_t const cy1 = std::min(y0 + side - 1, ty * MAP_TILE + MAP_TILE - 1);
            for (int32_t cy = cy0; cy <= cy1; ++cy)
                for (int32_t cx = cx0; cx <= cx1; ++cx)
                {
                    MapCell const* cell = tile
                        ? &tile->Cells[std::size_t((cy - ty * MAP_TILE) * MAP_TILE + (cx - tx * MAP_TILE))]
                        : FindCoarse(cx, cy);
                    if (cell)
                        window[std::size_t(cy - y0) * std::size_t(side) + std::size_t(cx - x0)] = *cell;
                }
        }
    auto const at = [&](int32_t cx, int32_t cy) -> MapCell const&
    {
        static MapCell const none;
        if (cx < x0 || cy < y0 || cx >= x0 + side || cy >= y0 + side)
            return none;
        return window[std::size_t(cy - y0) * std::size_t(side) + std::size_t(cx - x0)];
    };

    // Each crop cell's four 1-yd samples, turned with the facing: forward along (cos yaw, sin yaw), right along
    // (sin yaw, -cos yaw) (WoW's yaw turns left).
    float const forwardX = std::cos(yaw);
    float const forwardY = std::sin(yaw);
    float const rightX = forwardY;
    float const rightY = -forwardX;
    static float const SUB[4][2] = { { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, -0.5f }, { -0.5f, 0.5f } };
    static int32_t const NEIGHBOURS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    static uint8_t const PRIORITY[MAP_CODES] = { 0, 1, 4, 3, 2 };   // unknown, floor, wall, door, hazard
    float const centre = float(CROP) * 0.5f;
    for (uint32_t row = 0; row < CROP; ++row)
        for (uint32_t col = 0; col < CROP; ++col)
        {
            float const forward = (centre - 0.5f - float(row)) * CROP_CELL;
            float const right = (float(col) + 0.5f - centre) * CROP_CELL;
            MapCode code = MapCode::Unknown;
            bool hasHeight = false;
            float height = 0.0f;
            bool visited = false;
            double newest = -1.0;
            double entityAge = -1.0;
            uint8_t entity = 0;
            bool frontier = false;
            for (auto const& sub : SUB)
            {
                float const f = forward + sub[0] * (CROP_CELL * 0.5f);
                float const r = right + sub[1] * (CROP_CELL * 0.5f);
                int32_t const cx = CellOf(x + f * forwardX + r * rightX);
                int32_t const cy = CellOf(y + f * forwardY + r * rightY);
                MapCell const& cell = at(cx, cy);
                MapCode const here = CodeOf(cell, z);
                if (PRIORITY[uint8_t(here)] > PRIORITY[uint8_t(code)])
                    code = here;
                float floor = 0.0f;
                if (NearestFloor(cell, z, floor) && (!hasHeight || std::fabs(floor - z) < std::fabs(height - z)))
                {
                    height = floor;
                    hasHeight = true;
                }
                visited = visited || (cell.Flags & MAP_VISITED);
                if (cell.Seen)
                {
                    double const age = std::max(0.0, _clock - (double(cell.Seen) - 1.0));
                    if (newest < 0.0 || age < newest)
                        newest = age;
                }
                double const age = EntityAge(cell, _clock);
                if (age >= 0.0 && (entityAge < 0.0 || age < entityAge))
                {
                    entityAge = age;
                    entity = uint8_t(cell.Entity & CLASS_MASK);
                }
                if (!frontier && here == MapCode::Floor)
                    for (auto const& offset : NEIGHBOURS)
                        if (!Known(at(cx + offset[0], cy + offset[1])))
                        {
                            frontier = true;
                            break;
                        }
            }
            uint8_t* cell = out + (std::size_t(row) * CROP + col) * CROP_CHANNELS;
            cell[CROP_CODE] = uint8_t(code);
            cell[CROP_HEIGHT] = hasHeight ? uint8_t(std::clamp(int32_t(CROP_HEIGHT_ZERO)
                + int32_t(std::lround((height - z) / CROP_HEIGHT_STEP)), 1, 255)) : 0;
            cell[CROP_VISITED] = visited ? 1 : 0;
            cell[CROP_AGE] = newest < 0.0 ? CROP_AGE_NEVER : AgeByte(float(newest));
            cell[CROP_CLASS] = entity;
            cell[CROP_FRONTIER] = frontier ? 1 : 0;
        }
}
