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

void Animus::Vision::MergeCell(MapCell& into, MapCell const& from)
{
    Merge(into, from);
}

namespace
{
    MapRunSettings CurrentMapSettings;
}

Animus::Vision::MapRunSettings const& Animus::Vision::MapCurrent()
{
    return CurrentMapSettings;
}

void Animus::Vision::ConfigureMap(MapRunSettings const& settings)
{
    CurrentMapSettings = settings;
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
    cell.Searched = uint8_t(std::min<uint32_t>(bytes[CROP_SEARCHED], SEARCHED_MAX));
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
    _far.clear();
    _clock = 0.0;
    _anchored = false;
    _origin = 0.0;
    _touch = 0;
    _lastSlot = nullptr;
    _lastTile = nullptr;
    _lastFarSlot = nullptr;
    _hasBody = false;
    _epoch = 1;
    _groundTotal = 0;
    _searchedTotal = 0;
    _lastGroundClock = 0.0;
}

void Animus::Vision::MentalMap::ClearAt(double clock)
{
    Clear();
    _clock = std::clamp(clock, 0.0, CLOCK_LIMIT - 1.0);
    _lastGroundClock = _clock;
}

void Animus::Vision::MentalMap::Advance(float seconds)
{
    _clock += std::max(0.0f, seconds);
    if (_clock >= CLOCK_LIMIT)
        Clear();
}

void Animus::Vision::MentalMap::BeginEpisode()
{
    _epoch = Stamp();
    _groundTotal = 0;
    _searchedTotal = 0;
    _lastGroundClock = _clock;
}

void Animus::Vision::MentalMap::TileCounts(int32_t tx, int32_t ty, uint16_t& known, uint16_t& visited,
    uint16_t& searched) const
{
    known = visited = searched = 0;
    auto const found = _tiles.find(TileKey(tx, ty));
    if (found == _tiles.end())
        return;
    Tile const& tile = *found->second.Read();
    known = tile.KnownCells;
    visited = tile.VisitedCells;
    searched = tile.SearchedEpoch == _epoch ? tile.SearchedCells : 0;
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
    Slot* slot = _lastSlot && _lastKey == key ? _lastSlot : nullptr;
    if (!slot)
    {
        auto found = _tiles.find(key);
        if (found == _tiles.end())
        {
            if (_tiles.size() >= std::max<uint32_t>(1, _settings.MaxTiles))
                Evict();
            found = _tiles.emplace(key, Slot()).first;
            found->second.Own = std::make_unique<Tile>();
        }
        slot = &found->second;
        // A borrowed tile (a shared memory's) is cloned on its first write: the shared one is never written.
        if (!slot->Own)
        {
            slot->Own = std::make_unique<Tile>(*slot->Shared);
            slot->Shared.reset();
        }
        _lastKey = key;
        _lastSlot = slot;
        _lastTile = slot->Own.get();
    }
    slot->Touched = ++_touch;
    return _lastTile->Cells[std::size_t((cy - ty * MAP_TILE) * MAP_TILE + (cx - tx * MAP_TILE))];
}

Animus::Vision::MapCell& Animus::Vision::MentalMap::TouchFar(int32_t ccx, int32_t ccy)
{
    int32_t const tx = FloorDiv(ccx, MAP_TILE);
    int32_t const ty = FloorDiv(ccy, MAP_TILE);
    int64_t const key = TileKey(tx, ty);
    Slot* slot = _lastFarSlot && _lastFarKey == key ? _lastFarSlot : nullptr;
    if (!slot)
    {
        auto found = _far.find(key);
        if (found == _far.end())
        {
            if (_far.size() >= std::max<uint32_t>(1, _settings.FarTiles))
            {
                auto oldest = _far.begin();
                for (auto it = _far.begin(); it != _far.end(); ++it)
                    if (it->second.Touched < oldest->second.Touched)
                        oldest = it;
                _far.erase(oldest);
            }
            found = _far.emplace(key, Slot()).first;
            found->second.Own = std::make_unique<Tile>();
        }
        slot = &found->second;
        if (!slot->Own)
        {
            slot->Own = std::make_unique<Tile>(*slot->Shared);
            slot->Shared.reset();
        }
        _lastFarKey = key;
        _lastFarSlot = slot;
    }
    slot->Touched = ++_touch;
    return slot->Own->Cells[std::size_t((ccy - ty * MAP_TILE) * MAP_TILE + (ccx - tx * MAP_TILE))];
}

void Animus::Vision::MentalMap::Evict()
{
    auto oldest = _tiles.end();
    for (auto it = _tiles.begin(); it != _tiles.end(); ++it)
        if (oldest == _tiles.end() || it->second.Touched < oldest->second.Touched)
            oldest = it;
    if (oldest == _tiles.end())
        return;
    if (_settings.CoarseTiles)
        Fold(oldest->first, *oldest->second.Read());
    if (_lastSlot == &oldest->second)
    {
        _lastSlot = nullptr;
        _lastTile = nullptr;
    }
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
                        if (it->second.Touched < oldest->second.Touched)
                            oldest = it;
                    _coarse.erase(oldest);
                }
                found = _coarse.emplace(coarseKey, Slot()).first;
                found->second.Own = std::make_unique<Tile>();
            }
            found->second.Touched = ++_touch;
            Merge(found->second.Own->Cells[std::size_t((ccy - cty * MAP_TILE) * MAP_TILE + (ccx - ctx * MAP_TILE))],
                cell);
        }
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindIn(Slots const& slots, int32_t cx, int32_t cy)
{
    if (slots.empty())
        return nullptr;
    int32_t const tx = FloorDiv(cx, MAP_TILE);
    int32_t const ty = FloorDiv(cy, MAP_TILE);
    auto const found = slots.find(TileKey(tx, ty));
    if (found == slots.end())
        return nullptr;
    return &found->second.Read()->Cells[std::size_t((cy - ty * MAP_TILE) * MAP_TILE + (cx - tx * MAP_TILE))];
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindCell(int32_t cx, int32_t cy) const
{
    return FindIn(_tiles, cx, cy);
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindFarCell(int32_t ccx, int32_t ccy) const
{
    return FindIn(_far, ccx, ccy);
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindFar(float x, float y) const
{
    return FindFarCell(FloorDiv(CellOf(x), COARSE_CELL), FloorDiv(CellOf(y), COARSE_CELL));
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::Find(float x, float y) const
{
    return FindCell(CellOf(x), CellOf(y));
}

Animus::Vision::MapCell const* Animus::Vision::MentalMap::FindCoarse(int32_t cx, int32_t cy) const
{
    if (_coarse.empty())
        return nullptr;
    return FindIn(_coarse, FloorDiv(cx, COARSE_CELL), FloorDiv(cy, COARSE_CELL));
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
    // The tile's coverage counters (general search): the cell is the one Touch returned last, so its tile is
    // _lastTile. A first look ever counts as known; a first look this episode as searched.
    if (Tile* tile = _lastTile)
    {
        if (!cell.Seen)
            ++tile->KnownCells;
        if (cell.Seen < _epoch && stamp >= _epoch)
        {
            if (tile->SearchedEpoch != _epoch)
            {
                tile->SearchedEpoch = _epoch;
                tile->SearchedCells = 0;
            }
            ++tile->SearchedCells;
            ++_searchedTotal;
        }
    }
    cell.Seen = stamp;
}

void Animus::Vision::MentalMap::WriteEntity(MapCell& cell, Class what, uint16_t stamp)
{
    MarkSeen(cell, stamp);
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

bool Animus::Vision::MentalMap::WriteWall(int32_t cx, int32_t cy, float z, float feetZ, float bodyHeight,
    uint16_t stamp)
{
    // A wall within the body's band above a floor: the cell's own, else a neighbour's, else the feet's.
    float reference = 0.0f;
    bool referenced = false;
    uint8_t flag = MAP_WALL_LOW;
    MapCell const* here = FindCell(cx, cy);
    if (here)
        for (uint32_t layer = 0; layer < 2; ++layer)
            if (here->Floor[layer] != NO_FLOOR && FloorHeight(here->Floor[layer]) <= z + WALL_SLACK)
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
            MapCell const* next = FindCell(cx + offset[0], cy + offset[1]);
            float nz = 0.0f;
            if (next && NearestFloor(*next, z, nz) && nz <= z + WALL_SLACK && (!referenced || nz > reference))
            {
                reference = nz;
                referenced = true;
            }
        }
    }
    bool const inBand = referenced ? z <= reference + bodyHeight + WALL_SLACK
        : z >= feetZ - FREE_BAND && z <= feetZ + bodyHeight + WALL_SLACK;
    if (!inBand)
        return false;
    MapCell& cell = Touch(cx, cy);
    cell.Flags |= flag;
    MarkSeen(cell, stamp);
    return true;
}

void Animus::Vision::MentalMap::WriteFar(Class what, float x, float y, float z, float normalZ, float feetZ,
    float bodyHeight, uint16_t stamp, MapWriteStats* stats)
{
    int32_t const ccx = FloorDiv(CellOf(x), COARSE_CELL);
    int32_t const ccy = FloorDiv(CellOf(y), COARSE_CELL);
    switch (what)
    {
        case Class::Terrain:
        case Class::Model:
        {
            if (normalZ >= FLOOR_NORMAL)
            {
                MapCell& cell = TouchFar(ccx, ccy);
                InsertFloor(cell, z);
                cell.Seen = stamp;
                if (stats)
                    ++stats->Floors;
                break;
            }
            if (normalZ <= CEILING_NORMAL)
                return;
            // A wall in the band of the far cell's own floor (its highest layer at or below the hit), else the
            // feet's: an 8-yd cell holds the floor beside its wall more often than not.
            float reference = 0.0f;
            bool referenced = false;
            uint8_t flag = MAP_WALL_LOW;
            if (MapCell const* here = FindFarCell(ccx, ccy))
                for (uint32_t layer = 0; layer < 2; ++layer)
                    if (here->Floor[layer] != NO_FLOOR && FloorHeight(here->Floor[layer]) <= z + WALL_SLACK)
                    {
                        reference = FloorHeight(here->Floor[layer]);
                        referenced = true;
                        flag = layer == 0 ? MAP_WALL_LOW : MAP_WALL_HIGH;
                    }
            bool const inBand = referenced ? z <= reference + bodyHeight + WALL_SLACK
                : z >= feetZ - FREE_BAND && z <= feetZ + bodyHeight + WALL_SLACK;
            if (!inBand)
                return;
            MapCell& cell = TouchFar(ccx, ccy);
            cell.Flags |= flag;
            cell.Seen = stamp;
            if (stats)
                ++stats->Walls;
            break;
        }
        case Class::Water:
        case Class::Deadly:
        {
            MapCell& cell = TouchFar(ccx, ccy);
            cell.Flags |= MAP_HAZARD;
            cell.Seen = stamp;
            break;
        }
        default:
            // An entity's class is the sensor's to write (WriteEntities), at its own cell.
            return;
    }
    if (stats)
    {
        ++stats->Hits;
        ++stats->FarHits;
    }
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
        float const hitXf = camera.X + ray.Dir.X * ray.Distance;
        float const hitYf = camera.Y + ray.Dir.Y * ray.Distance;
        int32_t const hitX = CellOf(hitXf);
        int32_t const hitY = CellOf(hitYf);
        // A ray ending on a floor: the band above that floor near the hit is seen free too (part C).
        bool const floorHit = (ray.What == Class::Terrain || ray.What == Class::Model) && ray.NormalZ >= FLOOR_NORMAL;

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
            // Where along the ray the band above the hit's floor starts to count (HIT_BAND_REACH across the ground).
            float const bandFrom = floorHit ? ray.Distance - HIT_BAND_REACH / horizontal : inf;
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
                bool onHit = false;
                if (!free && leave >= bandFrom && low >= ray.Z - FLOOR_STEP && high <= ray.Z + bodyHeight)
                    free = onHit = true;
                if (free)
                {
                    MapCell& cell = Touch(cx, cy);
                    cell.Flags |= MAP_FREE;
                    MarkSeen(cell, stamp);
                    if (stats)
                    {
                        ++stats->Free;
                        if (onHit)
                            ++stats->FreeOnHit;
                    }
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

        if (sky)
            continue;
        // Past the near reach, at any distance: the far tier's cells (part B).
        if (!hitWithin)
        {
            WriteFar(ray.What, hitXf, hitYf, ray.Z, ray.NormalZ, feetZ, bodyHeight, stamp, stats);
            continue;
        }
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
                if (WriteWall(hitX, hitY, ray.Z, feetZ, bodyHeight, stamp) && stats)
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

    Mesh(hits, feetZ, bodyHeight, stamp, stats);
}

void Animus::Vision::MentalMap::Mesh(FrameHits const& hits, float feetZ, float bodyHeight, uint16_t stamp,
    MapWriteStats* stats)
{
    uint32_t const width = hits.Width;
    uint32_t const height = hits.Height;
    if (width < 2 || height < 2 || hits.Rays.size() != std::size_t(width) * height)
        return;

    // Each ray's hit as a point of the depth image: where it is, its range (3D) and its kind.
    enum : uint8_t
    {
        KIND_NONE = 0,      // sky, a ceiling, an entity: never meshed
        KIND_FLOOR,
        KIND_WALL,
        KIND_HAZARD,
    };
    struct Point
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Range = 0.0f;
        float NormalZ = 0.0f;
        uint8_t Kind = KIND_NONE;
    };
    Vec3 const camera = hits.Camera;
    thread_local std::vector<Point> points;
    points.resize(hits.Rays.size());
    for (std::size_t index = 0; index < hits.Rays.size(); ++index)
    {
        RayHit const& ray = hits.Rays[index];
        Point& point = points[index];
        point.X = camera.X + ray.Dir.X * ray.Distance;
        point.Y = camera.Y + ray.Dir.Y * ray.Distance;
        point.Z = ray.Z;
        point.Range = ray.Distance;
        point.NormalZ = ray.NormalZ;
        point.Kind = KIND_NONE;
        if (ray.What == Class::Terrain || ray.What == Class::Model)
            point.Kind = ray.NormalZ >= FLOOR_NORMAL ? KIND_FLOOR
                : ray.NormalZ > CEILING_NORMAL ? KIND_WALL : KIND_NONE;
        else if (ray.What == Class::Water || ray.What == Class::Deadly)
            point.Kind = KIND_HAZARD;
    }

    uint32_t budget = MESH_FRAME_CELLS;
    auto const reject = [stats]()
    {
        if (stats)
            ++stats->MeshRejected;
    };

    // A triangle of one surface (floor or water/hazard): its cells, at the plane's height, when the three hits are
    // one surface -- the plane faces the camera (a drop's far floor seen past an edge makes a plane that faces away),
    // as level as a floor, and as level as its hits' own normals (a riser bridged at a grazing angle tilts it).
    auto const triangle = [&](Point const& a, Point const& b, Point const& c)
    {
        uint8_t const kind = a.Kind;
        float const span = std::max({ std::hypot(b.X - a.X, b.Y - a.Y), std::hypot(c.X - b.X, c.Y - b.Y),
            std::hypot(a.X - c.X, a.Y - c.Y) });
        bool far = std::min({ a.Range, b.Range, c.Range }) > WRITE_REACH;
        if (span > (far ? MESH_SPAN_FAR : MESH_SPAN_NEAR))
            return reject();
        float cell = far ? float(COARSE_CELL) : MAP_CELL;
        // All three in one cell: the hits wrote it, there is nothing between them.
        if (int32_t(std::floor(a.X / cell)) == int32_t(std::floor(b.X / cell))
            && int32_t(std::floor(a.X / cell)) == int32_t(std::floor(c.X / cell))
            && int32_t(std::floor(a.Y / cell)) == int32_t(std::floor(b.Y / cell))
            && int32_t(std::floor(a.Y / cell)) == int32_t(std::floor(c.Y / cell)))
            return;
        Vec3 const ab{ b.X - a.X, b.Y - a.Y, b.Z - a.Z };
        Vec3 const ac{ c.X - a.X, c.Y - a.Y, c.Z - a.Z };
        Vec3 normal{ ab.Y * ac.Z - ab.Z * ac.Y, ab.Z * ac.X - ab.X * ac.Z, ab.X * ac.Y - ab.Y * ac.X };
        float const length = Length(normal);
        if (length < 1e-6f)
            return reject();
        if (normal.Z < 0.0f)
            normal = normal * -1.0f;
        float const nz = normal.Z / length;
        if (nz < FLOOR_NORMAL || nz < std::min({ a.NormalZ, b.NormalZ, c.NormalZ }) - MESH_NORMAL_SLACK)
            return reject();
        Vec3 const toCamera{ camera.X - a.X, camera.Y - a.Y, camera.Z - a.Z };
        if (Dot(toCamera, normal) <= 0.0f)
            return reject();

        // The cells whose centres the triangle covers (barycentric in the ground plane); a near triangle too wide
        // for its budget goes to the far tier's cells.
        float const minX = std::min({ a.X, b.X, c.X });
        float const maxX = std::max({ a.X, b.X, c.X });
        float const minY = std::min({ a.Y, b.Y, c.Y });
        float const maxY = std::max({ a.Y, b.Y, c.Y });
        auto const cellsAt = [&](float size)
        {
            return uint32_t(int32_t(std::floor(maxX / size)) - int32_t(std::floor(minX / size)) + 1)
                * uint32_t(int32_t(std::floor(maxY / size)) - int32_t(std::floor(minY / size)) + 1);
        };
        if (!far && cellsAt(cell) > MESH_TRIANGLE_CELLS)
        {
            far = true;
            cell = float(COARSE_CELL);
        }
        uint32_t const cells = cellsAt(cell);
        if (cells > MESH_TRIANGLE_CELLS || cells > budget)
            return reject();
        float const denominator = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
        if (std::fabs(denominator) < 1e-6f)
            return reject();
        int32_t const x0 = int32_t(std::floor(minX / cell));
        int32_t const x1 = int32_t(std::floor(maxX / cell));
        int32_t const y0 = int32_t(std::floor(minY / cell));
        int32_t const y1 = int32_t(std::floor(maxY / cell));
        constexpr float INSIDE = -0.02f;
        for (int32_t cy = y0; cy <= y1; ++cy)
            for (int32_t cx = x0; cx <= x1; ++cx)
            {
                float const px = (float(cx) + 0.5f) * cell;
                float const py = (float(cy) + 0.5f) * cell;
                float const wa = ((b.Y - c.Y) * (px - c.X) + (c.X - b.X) * (py - c.Y)) / denominator;
                float const wb = ((c.Y - a.Y) * (px - c.X) + (a.X - c.X) * (py - c.Y)) / denominator;
                float const wc = 1.0f - wa - wb;
                if (wa < INSIDE || wb < INSIDE || wc < INSIDE)
                    continue;
                float const z = wa * a.Z + wb * b.Z + wc * c.Z;
                --budget;
                MapCell& written = far ? TouchFar(cx, cy) : Touch(cx, cy);
                if (kind == KIND_FLOOR)
                    InsertFloor(written, z);
                else
                    written.Flags |= MAP_HAZARD;
                if (far)
                    written.Seen = stamp;
                else
                    MarkSeen(written, stamp);
                if (stats)
                    ++(far ? stats->MeshedFar : stats->Meshed);
            }
    };

    // A pair of neighbouring wall hits at a near range (one wall, no doorway or corner between them): the cells on
    // the line between them, each a wall at its interpolated height when it is in the band (WriteWall).
    auto const wall = [&](Point const& a, Point const& b)
    {
        float const span = std::hypot(b.X - a.X, b.Y - a.Y);
        bool const far = std::min(a.Range, b.Range) > WRITE_REACH;
        if (std::fabs(a.Range - b.Range) > MESH_WALL_JUMP * std::min(a.Range, b.Range) + MESH_WALL_SLACK
            || span > (far ? MESH_SPAN_FAR : MESH_SPAN_NEAR))
            return reject();
        float const cell = far ? float(COARSE_CELL) : MAP_CELL;
        uint32_t const samples = uint32_t(std::ceil(span / (0.5f * cell)));
        if (samples < 2)
            return;
        if (samples > MESH_TRIANGLE_CELLS || samples > budget)
            return reject();
        int32_t lastX = INT32_MIN;
        int32_t lastY = INT32_MIN;
        for (uint32_t step = 1; step < samples; ++step)
        {
            float const t = float(step) / float(samples);
            float const x = a.X + (b.X - a.X) * t;
            float const y = a.Y + (b.Y - a.Y) * t;
            float const z = a.Z + (b.Z - a.Z) * t;
            int32_t const cx = int32_t(std::floor(x / cell));
            int32_t const cy = int32_t(std::floor(y / cell));
            if (cx == lastX && cy == lastY)
                continue;
            lastX = cx;
            lastY = cy;
            --budget;
            if (far)
            {
                WriteFar(Class::Model, x, y, z, 0.0f, feetZ, bodyHeight, stamp, nullptr);
                if (stats)
                    ++stats->MeshedFar;
            }
            else if (WriteWall(cx, cy, z, feetZ, bodyHeight, stamp) && stats)
                ++stats->Meshed;
        }
    };

    // Each quad of neighbouring rays (row r and r + 1, column c and c + 1) as two triangles; each ray's wall pair to
    // its right and below.
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col)
        {
            if (!budget)
                return;
            std::size_t const at = std::size_t(row) * width + col;
            Point const& p00 = points[at];
            bool const right = col + 1 < width;
            bool const below = row + 1 < height;
            if (p00.Kind == KIND_WALL)
            {
                if (right && points[at + 1].Kind == KIND_WALL)
                    wall(p00, points[at + 1]);
                if (below && points[at + width].Kind == KIND_WALL)
                    wall(p00, points[at + width]);
            }
            if (!right || !below)
                continue;
            Point const& p01 = points[at + 1];
            Point const& p10 = points[at + width];
            Point const& p11 = points[at + width + 1];
            bool const surface00 = p00.Kind == KIND_FLOOR || p00.Kind == KIND_HAZARD;
            bool const surface11 = p11.Kind == KIND_FLOOR || p11.Kind == KIND_HAZARD;
            if (surface00 && p01.Kind == p00.Kind && p10.Kind == p00.Kind)
                triangle(p00, p01, p10);
            if (surface11 && p01.Kind == p11.Kind && p10.Kind == p11.Kind)
                triangle(p11, p10, p01);
        }
}

void Animus::Vision::MentalMap::WriteEntities(SeenList const& seen, MapWriteStats* stats)
{
    uint16_t const stamp = Stamp();
    for (uint32_t slot = 0; slot < seen.Count && slot < ENTITY_SLOTS; ++slot)
    {
        EntityInfo const& info = seen.Info[slot];
        if (!IsEntityClass(info.Id.What))
            continue;
        MapCell& cell = Touch(CellOf(info.Centre.X), CellOf(info.Centre.Y));
        if (info.Id.What == Class::Door)
            cell.Flags |= MAP_DOOR;
        WriteEntity(cell, info.Id.What, stamp);
        if (stats)
            ++stats->Entities;
    }
}

void Animus::Vision::MentalMap::NoteGround(MapCell const& cell)
{
    if (cell.Flags & MAP_VISITED)
        return;
    ++_groundTotal;
    _lastGroundClock = _clock;
    // The cell is the one Touch returned last (WriteBody): its tile's stood-on count.
    if (_lastTile)
        ++_lastTile->VisitedCells;
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
                NoteGround(cell);
                cell.Flags |= MAP_VISITED;
                MarkSeen(cell, stamp);
            }
        }
    }
    MapCell& cell = Touch(CellOf(x), CellOf(y));
    NoteGround(cell);
    cell.Flags |= MAP_VISITED;
    if (grounded)
        InsertFloor(cell, z);
    MarkSeen(cell, stamp);
    _hasBody = true;
    _bodyX = x;
    _bodyY = y;
}

void Animus::Vision::MentalMap::Crop(float x, float y, float z, float yaw, uint8_t* out, float cell) const
{
    if (cell > CROP_CELL + 0.01f)
    {
        CropWide(x, y, z, yaw, cell, out);
        return;
    }
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
            Tile const* tile = found == _tiles.end() ? nullptr : found->second.Read();
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
            uint8_t searched = 0;
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
                if (cell.Seen >= _epoch)
                    ++searched;
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
            cell[CROP_SEARCHED] = searched;
        }
}

void Animus::Vision::MentalMap::CropWide(float x, float y, float z, float yaw, float cell, uint8_t* out) const
{
    // The crop's frame (Crop): forward along (cos yaw, sin yaw), right along (sin yaw, -cos yaw); a 1-yd cell goes to
    // the crop cell its centre falls in, row 0 the furthest ahead.
    float const forwardX = std::cos(yaw);
    float const forwardY = std::sin(yaw);
    float const rightX = forwardY;
    float const rightY = -forwardX;
    float const centre = float(CROP) * 0.5f;
    float const half = centre * cell;
    float const reach = half * 1.41422f;            // the turned crop's corner
    int32_t const cellsPer = std::max<int32_t>(1, int32_t(std::lround(cell / MAP_CELL)));
    uint32_t const perCell = uint32_t(cellsPer) * uint32_t(cellsPer);
    // Every 1-yd cell counted against the searched channel's scale: count / (perCell / SEARCHED_MAX).
    uint32_t const searchedPer = std::max<uint32_t>(1, perCell / SEARCHED_MAX);

    struct Sum
    {
        uint32_t Codes[MAP_CODES] = {};
        float Height = 0.0f;
        uint32_t Floors = 0;
        bool Visited = false;
        double Newest = -1.0;
        double EntityAge = -1.0;
        uint8_t Entity = 0;
        bool Frontier = false;
        uint32_t Searched = 0;
        uint32_t Known = 0;
        // The coarse and far tiers' cells that fell here, each weighing the 1-yd cells it stands for: they fill
        // what the 1-yd cells leave unknown (CoarseKnown at most), never outvote them.
        float CoarseCodes[MAP_CODES] = {};
        float CoarseKnown = 0.0f;
        float CoarseHeight = 0.0f;
        uint32_t CoarseFloors = 0;
    };
    thread_local std::vector<Sum> sums;
    sums.assign(std::size_t(CROP) * CROP, Sum());

    static int32_t const NEIGHBOURS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int32_t const cx0 = CellOf(x - reach);
    int32_t const cx1 = CellOf(x + reach);
    int32_t const cy0 = CellOf(y - reach);
    int32_t const cy1 = CellOf(y + reach);
    auto const scatter = [&](int32_t tx, int32_t ty, Tile const& tile)
    {
        int32_t const fx0 = std::max(cx0, tx * MAP_TILE);
        int32_t const fx1 = std::min(cx1, tx * MAP_TILE + MAP_TILE - 1);
        int32_t const fy0 = std::max(cy0, ty * MAP_TILE);
        int32_t const fy1 = std::min(cy1, ty * MAP_TILE + MAP_TILE - 1);
        for (int32_t cy = fy0; cy <= fy1; ++cy)
            for (int32_t cx = fx0; cx <= fx1; ++cx)
            {
                MapCell const& mapCell =
                    tile.Cells[std::size_t((cy - ty * MAP_TILE) * MAP_TILE + (cx - tx * MAP_TILE))];
                if (!Known(mapCell))
                    continue;
                float const dx = (float(cx) + 0.5f) * MAP_CELL - x;
                float const dy = (float(cy) + 0.5f) * MAP_CELL - y;
                float const forward = dx * forwardX + dy * forwardY;
                float const right = dx * rightX + dy * rightY;
                float const row = std::floor(centre - forward / cell);
                float const col = std::floor(centre + right / cell);
                if (row < 0.0f || col < 0.0f || row >= float(CROP) || col >= float(CROP))
                    continue;
                Sum& sum = sums[std::size_t(row) * CROP + std::size_t(col)];
                MapCode const here = CodeOf(mapCell, z);
                ++sum.Codes[uint8_t(here)];
                ++sum.Known;
                float floor = 0.0f;
                if (NearestFloor(mapCell, z, floor))
                {
                    sum.Height += floor;
                    ++sum.Floors;
                }
                sum.Visited = sum.Visited || (mapCell.Flags & MAP_VISITED);
                if (mapCell.Seen >= _epoch)
                    ++sum.Searched;
                double const age = std::max(0.0, _clock - (double(mapCell.Seen) - 1.0));
                if (sum.Newest < 0.0 || age < sum.Newest)
                    sum.Newest = age;
                double const entityAge = EntityAge(mapCell, _clock);
                if (entityAge >= 0.0 && (sum.EntityAge < 0.0 || entityAge < sum.EntityAge))
                {
                    sum.EntityAge = entityAge;
                    sum.Entity = uint8_t(mapCell.Entity & CLASS_MASK);
                }
                if (!sum.Frontier && here == MapCode::Floor)
                    for (auto const& offset : NEIGHBOURS)
                    {
                        MapCell const* beside = FindCell(cx + offset[0], cy + offset[1]);
                        if (!beside)
                            beside = FindCoarse(cx + offset[0], cy + offset[1]);
                        if (!beside || !Known(*beside))
                        {
                            sum.Frontier = true;
                            break;
                        }
                    }
            }
    };
    for (auto const& [key, slot] : _tiles)
    {
        int32_t const tx = int32_t(key >> 32);
        int32_t const ty = int32_t(uint32_t(key & 0xFFFFFFFFu));
        if (tx * MAP_TILE > cx1 || tx * MAP_TILE + MAP_TILE - 1 < cx0 || ty * MAP_TILE > cy1
            || ty * MAP_TILE + MAP_TILE - 1 < cy0)
            continue;
        scatter(tx, ty, *slot.Read());
    }

    // The coarse tiers (a realm's folded tiles, Fold; the far tier, written from hits beyond WRITE_REACH): each cell
    // stands for COARSE_CELL x COARSE_CELL yards and goes, by its centre, to the crop cell it falls in, weighing the
    // 1-yd cells it covers (at most a crop cell's).
    float const coarseWeight = float(std::min<uint32_t>(perCell, uint32_t(COARSE_CELL * COARSE_CELL)));
    int32_t const coarseSpan = MAP_TILE * COARSE_CELL;     // a coarse tile's side, in 1-yd cells
    auto const knownCoarse = [this](int32_t ccx, int32_t ccy)
    {
        MapCell const* far = FindIn(_far, ccx, ccy);
        MapCell const* folded = FindIn(_coarse, ccx, ccy);
        return (far && Known(*far)) || (folded && Known(*folded));
    };
    auto const scatterCoarse = [&](int64_t key, Tile const& tile)
    {
        int32_t const tx = int32_t(key >> 32);
        int32_t const ty = int32_t(uint32_t(key & 0xFFFFFFFFu));
        if (tx * coarseSpan > cx1 || tx * coarseSpan + coarseSpan - 1 < cx0 || ty * coarseSpan > cy1
            || ty * coarseSpan + coarseSpan - 1 < cy0)
            return;
        int32_t const fx0 = std::max(FloorDiv(cx0, COARSE_CELL), tx * MAP_TILE);
        int32_t const fx1 = std::min(FloorDiv(cx1, COARSE_CELL), tx * MAP_TILE + MAP_TILE - 1);
        int32_t const fy0 = std::max(FloorDiv(cy0, COARSE_CELL), ty * MAP_TILE);
        int32_t const fy1 = std::min(FloorDiv(cy1, COARSE_CELL), ty * MAP_TILE + MAP_TILE - 1);
        for (int32_t ccy = fy0; ccy <= fy1; ++ccy)
            for (int32_t ccx = fx0; ccx <= fx1; ++ccx)
            {
                MapCell const& mapCell =
                    tile.Cells[std::size_t((ccy - ty * MAP_TILE) * MAP_TILE + (ccx - tx * MAP_TILE))];
                if (!Known(mapCell))
                    continue;
                float const dx = (float(ccx) + 0.5f) * float(COARSE_CELL) - x;
                float const dy = (float(ccy) + 0.5f) * float(COARSE_CELL) - y;
                float const forward = dx * forwardX + dy * forwardY;
                float const right = dx * rightX + dy * rightY;
                float const row = std::floor(centre - forward / cell);
                float const col = std::floor(centre + right / cell);
                if (row < 0.0f || col < 0.0f || row >= float(CROP) || col >= float(CROP))
                    continue;
                Sum& sum = sums[std::size_t(row) * CROP + std::size_t(col)];
                MapCode const here = CodeOf(mapCell, z);
                sum.CoarseCodes[uint8_t(here)] += coarseWeight;
                sum.CoarseKnown += coarseWeight;
                float floor = 0.0f;
                if (NearestFloor(mapCell, z, floor))
                {
                    sum.CoarseHeight += floor;
                    ++sum.CoarseFloors;
                }
                sum.Visited = sum.Visited || (mapCell.Flags & MAP_VISITED);
                double const age = std::max(0.0, _clock - (double(mapCell.Seen) - 1.0));
                if (sum.Newest < 0.0 || age < sum.Newest)
                    sum.Newest = age;
                double const entityAge = EntityAge(mapCell, _clock);
                if (entityAge >= 0.0 && (sum.EntityAge < 0.0 || entityAge < sum.EntityAge))
                {
                    sum.EntityAge = entityAge;
                    sum.Entity = uint8_t(mapCell.Entity & CLASS_MASK);
                }
                if (!sum.Frontier && here == MapCode::Floor)
                    for (auto const& offset : NEIGHBOURS)
                        if (!knownCoarse(ccx + offset[0], ccy + offset[1]))
                        {
                            sum.Frontier = true;
                            break;
                        }
            }
    };
    for (auto const& [key, slot] : _coarse)
        scatterCoarse(key, *slot.Read());
    for (auto const& [key, slot] : _far)
        scatterCoarse(key, *slot.Read());

    static uint8_t const PRIORITY[MAP_CODES] = { 0, 1, 4, 3, 2 };   // unknown, floor, wall, door, hazard
    for (uint32_t index = 0; index < CROP * CROP; ++index)
    {
        Sum const& sum = sums[index];
        // The majority over all the cell's 1-yd cells, the unseen ones counted as unknown; a tie goes to the code
        // the fine crop would rank higher.
        // The coarse tiers' cells fill what the 1-yd cells leave unknown, scaled down to fit it.
        float counts[MAP_CODES];
        for (uint32_t kind = 0; kind < MAP_CODES; ++kind)
            counts[kind] = float(sum.Codes[kind]);
        float known = float(sum.Known);
        float const open = float(perCell) - known;
        if (sum.CoarseKnown > 0.0f && open > 0.0f)
        {
            float const scale = std::min(1.0f, open / sum.CoarseKnown);
            for (uint32_t kind = 0; kind < MAP_CODES; ++kind)
                counts[kind] += sum.CoarseCodes[kind] * scale;
            known += sum.CoarseKnown * scale;
        }
        counts[uint8_t(MapCode::Unknown)] += std::max(0.0f, float(perCell) - known);
        MapCode code = MapCode::Unknown;
        for (uint32_t candidate = 1; candidate < MAP_CODES; ++candidate)
            if (counts[candidate] > counts[uint8_t(code)]
                || (counts[candidate] == counts[uint8_t(code)] && PRIORITY[candidate] > PRIORITY[uint8_t(code)]))
                code = MapCode(candidate);
        uint8_t* bytes = out + std::size_t(index) * CROP_CHANNELS;
        bytes[CROP_CODE] = uint8_t(code);
        float const meanHeight = sum.Floors ? sum.Height / float(sum.Floors)
            : sum.CoarseFloors ? sum.CoarseHeight / float(sum.CoarseFloors) : 0.0f;
        bytes[CROP_HEIGHT] = sum.Floors || sum.CoarseFloors ? uint8_t(std::clamp(int32_t(CROP_HEIGHT_ZERO)
            + int32_t(std::lround((meanHeight - z) / CROP_HEIGHT_STEP)), 1, 255)) : 0;
        bytes[CROP_VISITED] = sum.Visited ? 1 : 0;
        bytes[CROP_AGE] = sum.Newest < 0.0 ? CROP_AGE_NEVER : AgeByte(float(sum.Newest));
        bytes[CROP_CLASS] = sum.Entity;
        bytes[CROP_FRONTIER] = sum.Frontier ? 1 : 0;
        bytes[CROP_SEARCHED] = uint8_t(std::min<uint32_t>(SEARCHED_MAX, sum.Searched / searchedPer));
    }
}
