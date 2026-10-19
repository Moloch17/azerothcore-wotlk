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

#include "SharedMemory.h"
#include "BakedScene.h"
#include "Log.h"
#include "SceneRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    using namespace Animus::Vision;
    namespace Fs = std::filesystem;

    [[nodiscard]] int64_t WallNow()
    {
        return int64_t(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    /// A stamp moved by `shift` seconds, kept a look (1 at the oldest); 0 (never) stays 0.
    [[nodiscard]] uint16_t Shifted(uint16_t stamp, int32_t shift)
    {
        if (!stamp)
            return 0;
        return uint16_t(std::clamp<int32_t>(int32_t(stamp) + shift, 1, 65535));
    }

    /// A tile's coverage counters from its cells; the per-episode searched count belongs to no episode here.
    void Recount(MapTile& tile, uint16_t& newest)
    {
        tile.KnownCells = 0;
        tile.VisitedCells = 0;
        tile.SearchedCells = 0;
        tile.SearchedEpoch = 0;
        for (MapCell const& cell : tile.Cells)
        {
            if (cell.Seen)
                ++tile.KnownCells;
            if (cell.Flags & MAP_VISITED)
                ++tile.VisitedCells;
            newest = std::max(newest, cell.Seen);
        }
    }

    struct TileRecord
    {
        int64_t Key;
        SharedTile Tile;
    };

    uint64_t RecordsChecksum(std::vector<TileRecord> const& records, uint64_t hash)
    {
        for (TileRecord const& record : records)
        {
            hash = Scene::Fnv1a(reinterpret_cast<uint8_t const*>(&record.Key), sizeof(record.Key), hash);
            hash = Scene::Fnv1a(reinterpret_cast<uint8_t const*>(record.Tile->Cells.data()),
                sizeof(MapCell) * MAP_TILE_CELLS, hash);
        }
        return hash;
    }
}

Animus::Vision::SharedMemory::SharedMemory(uint32_t mapId, uint64_t sceneChecksum,
    SharedMemorySettings const& settings) : _mapId(mapId), _sceneChecksum(sceneChecksum), _settings(settings),
    _objects(mapId)
{
}

void Animus::Vision::SharedMemory::RebaseIfDue(double now)
{
    double const relative = std::max(Relative(now), double(_newest));
    if (relative < REBASE_AT)
        return;
    double const shift = std::floor(relative - REBASE_KEEP);
    _base += shift;
    int32_t const by = -int32_t(std::min(shift, 65535.0));
    uint16_t newest = 0;
    for (Store* store : { &_fine, &_far })
        for (auto& [key, entry] : *store)
        {
            auto moved = std::make_shared<MapTile>(*entry.Tile);
            for (MapCell& cell : moved->Cells)
                cell.Seen = Shifted(cell.Seen, by);
            Recount(*moved, newest);
            entry.Tile = std::move(moved);
        }
    _newest = newest;
    LOG_INFO("module.animus", "Shared memory of map {}: its clock moved on {:.0f} s (looks older than {:.0f} s read as "
        "the oldest)", _mapId, shift, REBASE_KEEP);
}

void Animus::Vision::SharedMemory::Adopt(MentalMap& map, double now)
{
    map.Clear();
    std::lock_guard lock(_lock);
    RebaseIfDue(now);
    map._tiles.reserve(_fine.size());
    for (auto const& [key, entry] : _fine)
    {
        MentalMap::Slot& slot = map._tiles[key];
        slot.Shared = entry.Tile;
    }
    for (auto const& [key, entry] : _far)
    {
        MentalMap::Slot& slot = map._far[key];
        slot.Shared = entry.Tile;
    }
    // Past the newest look by a second at least: BeginEpisode's epoch is then above every stamp the memory holds,
    // so none of them reads as this episode's search.
    double const clock = std::max(Relative(now), double(_newest) + 1.0);
    map._clock = clock;
    map._lastGroundClock = clock;
    map._anchored = true;
    map._adopted = true;
    map._origin = _base;
}

void Animus::Vision::SharedMemory::Anchor(MentalMap& map, double now)
{
    std::lock_guard lock(_lock);
    RebaseIfDue(now);
    map.ClearAt(std::max(Relative(now), double(_newest) + 1.0));
    map._anchored = true;
    map._origin = _base;
}

void Animus::Vision::SharedMemory::MergeTile(Store& store, int64_t key, MapTile const& from, int32_t shift,
    WallClears clears)
{
    Entry& entry = store[key];
    auto merged = entry.Tile ? std::make_shared<MapTile>(*entry.Tile) : std::make_shared<MapTile>();
    for (std::size_t index = 0; index < MAP_TILE_CELLS; ++index)
    {
        MapCell cell = from.Cells[index];
        if (!cell.Seen && !cell.Flags && cell.Floor[0] == NO_FLOOR && cell.Floor[1] == NO_FLOOR && !cell.Weight[0]
            && !cell.Weight[1])
            continue;
        // A body's own ground stays its own (the map block's ground counters and visited channel), and so is what
        // it saw standing there: a unit's class is a sighting of the moment, not a feature of the ground (kept, the
        // memory painted a whole dungeon with the creatures a thousand episodes had met; the door flag stays).
        cell.Flags &= uint8_t(~MAP_VISITED);
        cell.Entity = 0;
        cell.Seen = Shifted(cell.Seen, shift);
        MergeCell(merged->Cells[index], cell, clears);
    }
    Recount(*merged, _newest);
    entry.Tile = std::move(merged);
    entry.Touched = ++_touch;
}

void Animus::Vision::SharedMemory::Trim(Store& store, uint32_t cap)
{
    while (store.size() > std::max<uint32_t>(1, cap))
    {
        auto oldest = store.begin();
        for (auto it = store.begin(); it != store.end(); ++it)
            if (it->second.Touched < oldest->second.Touched)
                oldest = it;
        store.erase(oldest);
    }
}

void Animus::Vision::SharedMemory::Absorb(MentalMap const& map, double now)
{
    if (!map._anchored)
        return;
    std::lock_guard lock(_lock);
    RebaseIfDue(now);
    // The map's stamps count from the base it was anchored on: onto today's.
    int32_t const shift = int32_t(std::clamp(std::round(map._origin - _base), -65535.0, 65535.0));
    bool merged = false;
    for (auto const& [key, slot] : map._tiles)
        if (slot.Own)
        {
            // An adopted map's missing wall is one it cleared; a blank map's, one it saw past (MAP_FREE) or never saw.
            MergeTile(_fine, key, *slot.Own, shift, map._adopted ? WallClears::Lacking : WallClears::SeenFree);
            merged = true;
        }
    for (auto const& [key, slot] : map._far)
        if (slot.Own)
        {
            MergeTile(_far, key, *slot.Own, shift, WallClears::Never);
            merged = true;
        }
    if (!merged)
        return;
    Trim(_fine, _settings.MaxTiles);
    Trim(_far, _settings.FarTiles);
    ++_revision;
}

bool Animus::Vision::SharedMemory::Dirty() const
{
    std::lock_guard lock(_lock);
    return _revision != _savedRevision;
}

std::size_t Animus::Vision::SharedMemory::Tiles() const
{
    std::lock_guard lock(_lock);
    return _fine.size();
}

std::size_t Animus::Vision::SharedMemory::FarTiles() const
{
    std::lock_guard lock(_lock);
    return _far.size();
}

bool Animus::Vision::SharedMemory::Load(std::string const& path, double now, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "no file";
        return false;
    }
    MemoryFileHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    MemoryFileHeader const expected;
    if (!in || std::memcmp(header.Magic, expected.Magic, sizeof(header.Magic)) != 0)
    {
        error = "not a memory file";
        return false;
    }
    if (header.Version != MEMORY_VERSION || header.TileSide != expected.TileSide
        || header.CellBytes != expected.CellBytes || header.CoarseCell != expected.CoarseCell)
    {
        error = "version " + std::to_string(header.Version) + " (this build reads " + std::to_string(MEMORY_VERSION)
            + ")";
        return false;
    }
    if (header.MapId != _mapId)
    {
        error = "of map " + std::to_string(header.MapId);
        return false;
    }
    if (header.SceneChecksum != _sceneChecksum)
    {
        error = "seen against another scene (the map's baked scene changed)";
        return false;
    }
    if (header.FineTiles > 1u << 20 || header.FarTiles > 1u << 20 || !std::isfinite(header.Clock))
    {
        error = "a corrupt header";
        return false;
    }
    std::vector<TileRecord> fine;
    std::vector<TileRecord> far;
    for (uint32_t index = 0; index < header.FineTiles + header.FarTiles; ++index)
    {
        TileRecord record;
        auto tile = std::make_shared<MapTile>();
        in.read(reinterpret_cast<char*>(&record.Key), sizeof(record.Key));
        in.read(reinterpret_cast<char*>(tile->Cells.data()), std::streamsize(sizeof(MapCell) * MAP_TILE_CELLS));
        if (!in)
        {
            error = "cut short";
            return false;
        }
        record.Tile = std::move(tile);
        (index < header.FineTiles ? fine : far).push_back(std::move(record));
    }
    if (RecordsChecksum(far, RecordsChecksum(fine, Scene::Fnv1a(nullptr, 0))) != header.PayloadChecksum)
    {
        error = "its checksum does not match";
        return false;
    }

    std::lock_guard lock(_lock);
    _fine.clear();
    _far.clear();
    _newest = 0;
    for (auto* pair : { &fine, &far })
        for (TileRecord& record : *pair)
        {
            auto tile = std::make_shared<MapTile>(*record.Tile);
            Recount(*tile, _newest);
            Entry& entry = (pair == &fine ? _fine : _far)[record.Key];
            entry.Tile = std::move(tile);
            entry.Touched = ++_touch;
        }
    // The clock carries on from the save, plus the wall-clock time the process was down.
    double const gap = double(std::max<int64_t>(0, WallNow() - header.WallSeconds));
    _base = 0.0;
    _offset = std::max(header.Clock, double(_newest)) + gap - now;
    RebaseIfDue(now);
    Trim(_fine, _settings.MaxTiles);
    Trim(_far, _settings.FarTiles);
    _savedRevision = _revision;
    return true;
}

bool Animus::Vision::SharedMemory::Save(std::string const& path, double now, std::string& error)
{
    // The tiles are immutable: their pointers are taken under the lock, and written without it.
    std::vector<TileRecord> fine;
    std::vector<TileRecord> far;
    MemoryFileHeader header;
    uint64_t revision = 0;
    {
        std::lock_guard lock(_lock);
        fine.reserve(_fine.size());
        for (auto const& [key, entry] : _fine)
            fine.push_back({ key, entry.Tile });
        far.reserve(_far.size());
        for (auto const& [key, entry] : _far)
            far.push_back({ key, entry.Tile });
        header.Clock = std::max(Relative(now), double(_newest));
        revision = _revision;
    }
    // In key order, so the same memory writes the same bytes.
    auto const byKey = [](TileRecord const& a, TileRecord const& b) { return a.Key < b.Key; };
    std::sort(fine.begin(), fine.end(), byKey);
    std::sort(far.begin(), far.end(), byKey);
    header.MapId = _mapId;
    header.SceneChecksum = _sceneChecksum;
    header.WallSeconds = WallNow();
    header.FineTiles = uint32_t(fine.size());
    header.FarTiles = uint32_t(far.size());
    header.PayloadChecksum = RecordsChecksum(far, RecordsChecksum(fine, Scene::Fnv1a(nullptr, 0)));

    std::error_code ec;
    Fs::create_directories(Fs::path(path).parent_path(), ec);
    std::string const partial = path + ".part";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + partial;
            return false;
        }
        out.write(reinterpret_cast<char const*>(&header), sizeof(header));
        for (auto const* records : { &fine, &far })
            for (TileRecord const& record : *records)
            {
                out.write(reinterpret_cast<char const*>(&record.Key), sizeof(record.Key));
                out.write(reinterpret_cast<char const*>(record.Tile->Cells.data()),
                    std::streamsize(sizeof(MapCell) * MAP_TILE_CELLS));
            }
        if (!out)
        {
            error = "writing " + partial + " failed";
            return false;
        }
    }
    Fs::rename(partial, path, ec);
    if (ec)
    {
        error = "renaming " + partial + ": " + ec.message();
        return false;
    }
    std::lock_guard lock(_lock);
    _savedRevision = std::max(_savedRevision, revision);
    return true;
}

Animus::Vision::SharedMemories& Animus::Vision::SharedMemories::Instance()
{
    static SharedMemories instance;
    return instance;
}

Animus::Vision::SharedMemories::~SharedMemories()
{
    if (_saver.joinable())
        _saver.join();
}

void Animus::Vision::SharedMemories::Configure(SharedMemorySettings const& settings)
{
    std::lock_guard lock(_lock);
    _settings = settings;
}

Animus::Vision::SharedMemorySettings Animus::Vision::SharedMemories::Settings() const
{
    std::lock_guard lock(_lock);
    return _settings;
}

std::string Animus::Vision::SharedMemories::PathOf(uint32_t mapId) const
{
    std::lock_guard lock(_lock);
    if (_settings.Dir.empty())
        return "";
    return (Fs::path(_settings.Dir) / (std::to_string(mapId) + ".amem")).string();
}

std::string Animus::Vision::SharedMemories::ObjectPathOf(uint32_t mapId) const
{
    std::lock_guard lock(_lock);
    if (_settings.Dir.empty())
        return "";
    return (Fs::path(_settings.Dir) / (std::to_string(mapId) + ".aobj")).string();
}

Animus::Vision::SharedMemory* Animus::Vision::SharedMemories::Get(uint32_t mapId, double now)
{
    std::lock_guard lock(_lock);
    if (auto const found = _memories.find(mapId); found != _memories.end())
        return found->second.get();
    BakedWorld const* scene = SceneRegistry::Instance().Get(mapId);
    uint64_t const checksum = scene ? scene->Header().Checksum : 0;
    auto memory = std::make_unique<SharedMemory>(mapId, checksum, _settings);
    if (!_settings.Dir.empty())
    {
        std::string const path = (Fs::path(_settings.Dir) / (std::to_string(mapId) + ".amem")).string();
        std::string error;
        if (memory->Load(path, now, error))
            LOG_INFO("module.animus", "Shared memory of map {}: loaded {} tiles and {} far tiles from {}", mapId,
                memory->Tiles(), memory->FarTiles(), path);
        else if (error != "no file")
            LOG_WARN("module.animus", "Shared memory of map {}: {} not used ({}); the memory starts empty", mapId,
                path, error);
        // Its object memory, from its own file (ObjectMemory.h).
        std::string const objects = (Fs::path(_settings.Dir) / (std::to_string(mapId) + ".aobj")).string();
        if (memory->Objects().Load(objects, now, error))
            LOG_INFO("module.animus", "Shared memory of map {}: loaded {} remembered objects from {}", mapId,
                memory->Objects().Count(), objects);
        else if (error != "no file")
            LOG_WARN("module.animus", "Shared memory of map {}: {} not used ({}); its object memory starts empty",
                mapId, objects, error);
    }
    SharedMemory* raw = memory.get();
    _memories.emplace(mapId, std::move(memory));
    return raw;
}

std::vector<Animus::Vision::SharedMemory*> Animus::Vision::SharedMemories::List() const
{
    std::lock_guard lock(_lock);
    std::vector<SharedMemory*> out;
    for (auto const& [mapId, memory] : _memories)
        out.push_back(memory.get());
    return out;
}

void Animus::Vision::SharedMemories::SaveList(std::vector<SharedMemory*> const& memories, double now) const
{
    for (SharedMemory* memory : memories)
    {
        std::string error;
        if (memory->Dirty() && !memory->Save(PathOf(memory->MapId()), now, error))
            LOG_WARN("module.animus", "Shared memory of map {}: not saved ({})", memory->MapId(), error);
        if (memory->Objects().Dirty() && !memory->Objects().Save(ObjectPathOf(memory->MapId()), now, error))
            LOG_WARN("module.animus", "Shared memory of map {}: its objects not saved ({})", memory->MapId(), error);
    }
}

void Animus::Vision::SharedMemories::SaveDue(double now)
{
    if (_saving.load())
        return;
    auto const clock = std::chrono::steady_clock::now();
    SharedMemorySettings const settings = Settings();
    if (settings.Dir.empty() || std::chrono::duration<double>(clock - _lastSave).count() < settings.SaveSeconds)
        return;
    _lastSave = clock;
    if (_saver.joinable())
        _saver.join();
    std::vector<SharedMemory*> memories = List();
    if (memories.empty())
        return;
    _saving = true;
    _saver = std::thread([this, memories = std::move(memories), now]()
    {
        SaveList(memories, now);
        _saving = false;
    });
}

void Animus::Vision::SharedMemories::SaveAll(double now)
{
    if (_saver.joinable())
        _saver.join();
    if (Settings().Dir.empty())
        return;
    std::vector<SharedMemory*> const memories = List();
    SaveList(memories, now);
    for (SharedMemory* memory : memories)
        LOG_INFO("module.animus", "Shared memory of map {}: {} tiles and {} far tiles kept in {}, {} objects in {}",
            memory->MapId(), memory->Tiles(), memory->FarTiles(), PathOf(memory->MapId()), memory->Objects().Count(),
            ObjectPathOf(memory->MapId()));
}
