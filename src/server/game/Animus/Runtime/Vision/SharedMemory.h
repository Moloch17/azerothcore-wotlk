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

#ifndef ANIMUS_VISION_SHARED_MEMORY_H
#define ANIMUS_VISION_SHARED_MEMORY_H

#include "MentalMap.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// **The shared memory** (the user, 2026-10-15: "a mental map shared by all bots, which persists"; "in the release
/// build the bots will use shared memory across all agents"): one mental map per game map id per process, which every
/// bot on that map -- every instance of it, their positions being the same -- starts from and merges what it saw back
/// into. Runtime code, the same in the forge and in a realm module: the caller says when (a seat's reset, a bot's
/// spawn or map change) and what the clock is; whether a seat reads the memory at all is the caller's choice (the
/// forge's training draws it per episode: StageDefinition::SharedMemoryShare; a realm's bots always do).
///
/// **No lock on the hot path**: the memory's tiles are immutable (SharedTile). Adopt hands a map the tile pointers,
/// which it reads as its own and clones on its first write to one (MentalMap's copy-on-write); Absorb merges the
/// tiles a map wrote (its own, cloned or new) into copies of the memory's and swaps the pointers in, under the
/// memory's mutex, once per episode a seat. A map's borrowed tiles stay valid whatever the memory does meanwhile.
///
/// **The clock**: a cell's look is stamped in map seconds (16 bits). The memory's seconds are the caller's clock
/// (`now`: the process's game seconds, which run with the sim, so a stamp a seat wrote reads the same age to every
/// other seat) plus an offset the file carries across restarts (the wall-clock time between a save and the next
/// load is added, so a memory twenty minutes old reads as twenty minutes old after a restart). A stamp is the
/// memory's second past its base, + 1. When the clock nears the stamp's range (REBASE_AT) the base moves on and every
/// stamp with it; a look older than the move is clamped to the oldest stamp, which reads no differently (the crop's
/// age byte saturates near two hours). A map remembers the base it was anchored on, so a merge after a rebase
/// shifts its stamps by the difference. The memory never keeps MAP_VISITED (a body's own ground: the map block's
/// ground counters and visited channel stay the seat's), the per-episode searched counters, nor a cell's entity
/// (MapCell::Entity, a unit's class and age: a sighting of the moment, which merged from every seat painted the
/// whole known map with creatures; the crop's class channel shows only sightings under ENTITY_RECENT_SECONDS old,
/// and a memory may be hours old). A version 3 file written before 2026-10-16 may still carry entities: the crop's
/// age gate hides them, so the format is unchanged.
///
/// **Walls by recency** (MergeCell, WallClears): a wall moves with its floor layer, and on a layer both hold the
/// newer look decides -- a map that read the memory (Adopt) sets or clears it; one that started blank (Anchor) sets
/// it, and clears it only where its rays passed the cell free in the body's band (MAP_FREE; since version 5 -- it
/// knew nothing of the memory's walls, but a pass is a look, and a corridor every blank seat walks through stayed
/// sealed in the memory while only adopted seats could clear it); the far tier's cells, which no observation clears,
/// only set. An older look changes nothing. So a misread is cleared by the next seat that looks there, rather than
/// kept for every bot for good.
///
/// **Persistence**: `<Dir>/<mapId>.amem`, written periodically (SaveSeconds of real time, on a thread of its own, off
/// the world and map threads) and at shutdown, loaded the first time a map's memory is asked for. A versioned binary
/// file (MemoryFileHeader: the magic, MEMORY_VERSION, the map id, the cell's and tile's sizes, the baked scene's
/// checksum, the clock, the wall time, the tile counts and a payload checksum) and its tiles; a file of another
/// version, map, layout or scene, or one that does not check out, is ignored (and overwritten at the next save). The
/// format is kept stable: a realm keeps these files for good, and a change to it is a new MEMORY_VERSION.
namespace Animus::Vision
{
    /// Where the memories live and their caps; set once at startup (SharedMemories::Configure).
    struct SharedMemorySettings
    {
        std::string Dir;                // the folder of the .amem files; "" keeps nothing on disk
        uint32_t MaxTiles = 4096;       // 1-yd tiles a memory keeps, the least recently merged going first
        uint32_t FarTiles = 256;        // far-tier tiles likewise
        double SaveSeconds = 600.0;     // real seconds between the periodic saves
    };

    /// 2 (2026-10-15): walls bound to their layers on merge and cleared by observation; a version 1 file holds walls
    /// on the wrong layers and misreads never cleared, and is not read.
    /// 3 (2026-10-16): the tops of ceiling panels seen from over them (RayHit::Backside) are no floors; a version 2
    /// file holds them as phantom storeys over the real floors, and is not read.
    /// 4 (2026-10-17): bodies that fell through a hole in a dungeon's collision were stood on the terrain over it
    /// (PlayerController's Resync) and painted it -- the Deadmines' 23,000 cells of floor at z 302 -- and the shore's
    /// dry edge was meshed as a hazard; a version 3 file holds both, and is not read. The bytes are version 3's.
    /// 5 (2026-10-17, memv4): a ray's own hit on a WMO's liquid plane under a known floor (the shore's dry edge, a
    /// dock) painted the floor a hazard (MentalMap::NoteHazard; a few percent of the Deadmines' hazard cells), and a
    /// wall a blank seat saw past stayed (WallClears::SeenFree); a version 4 file holds those hazards and walls, and
    /// is not read. The bytes are version 3's.
    constexpr uint32_t MEMORY_VERSION = 5;
    /// When the memory's clock (seconds past its base) passes this, the base moves on to leave REBASE_KEEP seconds
    /// of history: a seat's map runs to MentalMap's 65000-second limit from wherever it was anchored.
    constexpr double REBASE_AT = 40000.0;
    constexpr double REBASE_KEEP = 20000.0;

#pragma pack(push, 1)
    /// The .amem file's header; MEMORY_VERSION 5. Little-endian, as the forge's and the realm's machines are. The
    /// tiles follow it, FineTiles then FarTiles records of (int64 tile key, MAP_TILE_CELLS MapCell: int16 Floor[2],
    /// uint8 Flags, uint8 Entity, uint16 Seen).
    struct MemoryFileHeader
    {
        char Magic[4] = { 'A', 'M', 'E', 'M' };
        uint32_t Version = MEMORY_VERSION;
        uint32_t MapId = 0;
        uint32_t TileSide = uint32_t(MAP_TILE);
        uint32_t CellBytes = uint32_t(sizeof(MapCell));
        uint32_t CoarseCell = uint32_t(COARSE_CELL);
        uint64_t SceneChecksum = 0;     // the baked scene the memory was seen against (0: none was loaded)
        double Clock = 0.0;             // the memory's seconds past its base at the save
        int64_t WallSeconds = 0;        // the Unix time of the save
        uint32_t FineTiles = 0;
        uint32_t FarTiles = 0;
        uint64_t PayloadChecksum = 0;   // FNV-1a 64 over the tile records
        uint32_t Reserved[4] = {};
    };
#pragma pack(pop)
    static_assert(sizeof(MemoryFileHeader) == 80, "the memory file's header is 80 bytes");

    class SharedMemory
    {
    public:
        SharedMemory(uint32_t mapId, uint64_t sceneChecksum, SharedMemorySettings const& settings);
        SharedMemory(SharedMemory const&) = delete;
        SharedMemory& operator=(SharedMemory const&) = delete;

        [[nodiscard]] uint32_t MapId() const { return _mapId; }

        /// `map` starts with this memory as its own (its contents cleared first): the memory's tiles borrowed, its
        /// clock the memory's now (or past the memory's newest look, so none of it counts as this episode's), and
        /// anchored so Absorb can merge it back. Call before MentalMap::BeginEpisode. `now`: the caller's clock.
        void Adopt(MentalMap& map, double now);
        /// `map` starts blank on this memory's clock, anchored: an episode that does not read the memory but whose
        /// looks still go into it (Absorb).
        void Anchor(MentalMap& map, double now);
        /// Merges what an anchored map wrote (the tiles it owns: cloned from the memory's or new) into the memory;
        /// a map that is not anchored is ignored. Its stamps are shifted onto the memory's base; its visits stay its.
        void Absorb(MentalMap const& map, double now);

        /// Loads `path` (see the file comment); false with `error` when it is missing, of another version, map,
        /// layout or scene, or does not check out -- the memory is then left as it was.
        [[nodiscard]] bool Load(std::string const& path, double now, std::string& error);
        /// Writes the memory to `path` (a .part file renamed over it). The tiles are taken under the lock, written
        /// outside it.
        [[nodiscard]] bool Save(std::string const& path, double now, std::string& error);
        /// Whether anything was merged since the last save or load.
        [[nodiscard]] bool Dirty() const;

        [[nodiscard]] std::size_t Tiles() const;
        [[nodiscard]] std::size_t FarTiles() const;

    private:
        struct Entry
        {
            SharedTile Tile;
            uint64_t Touched = 0;
        };
        using Store = std::unordered_map<int64_t, Entry>;

        /// The memory's seconds past its base at the caller's `now`. Under _lock.
        [[nodiscard]] double Relative(double now) const { return now + _offset - _base; }
        /// Moves the base on when the clock nears the stamp's range. Under _lock.
        void RebaseIfDue(double now);
        /// Merges one tile a map owns into `store`, its stamps shifted by `shift`; `clears`: the map's missing walls
        /// on newer looks clear the memory's (MergeCell). Under _lock.
        void MergeTile(Store& store, int64_t key, MapTile const& from, int32_t shift, WallClears clears);
        static void Trim(Store& store, uint32_t cap);

        uint32_t _mapId;
        uint64_t _sceneChecksum;
        SharedMemorySettings _settings;
        mutable std::mutex _lock;
        Store _fine;
        Store _far;
        double _offset = 0.0;           // the memory's seconds = the caller's + _offset
        double _base = 0.0;             // the memory's second a stamp counts from (stamp 1)
        uint16_t _newest = 0;           // the newest stamp held
        uint64_t _touch = 0;
        uint64_t _revision = 0;         // merges so far
        uint64_t _savedRevision = 0;    // ... as of the last save or load
    };

    /// The process's shared memories, one per game map id, made (and loaded from disk) the first time each is asked
    /// for; never freed before shutdown, so a pointer Get returns may be kept.
    class SharedMemories
    {
    public:
        [[nodiscard]] static SharedMemories& Instance();
        ~SharedMemories();

        void Configure(SharedMemorySettings const& settings);
        [[nodiscard]] SharedMemorySettings Settings() const;

        /// Map `mapId`'s memory (any thread): made on the first call, loaded from `<Dir>/<mapId>.amem` when the
        /// file is there and current (the scene checksum is the loaded scene's: SceneRegistry).
        [[nodiscard]] SharedMemory* Get(uint32_t mapId, double now);
        /// The periodic save (the world thread, every tick): when SaveSeconds of real time have passed since the
        /// last, every memory changed since its last save is written on a thread of its own; never waits.
        void SaveDue(double now);
        /// Every changed memory written now, after any save in flight (shutdown).
        void SaveAll(double now);

        [[nodiscard]] std::string PathOf(uint32_t mapId) const;

    private:
        SharedMemories() = default;
        void SaveList(std::vector<SharedMemory*> const& memories, double now) const;
        [[nodiscard]] std::vector<SharedMemory*> List() const;

        mutable std::mutex _lock;
        SharedMemorySettings _settings;
        std::map<uint32_t, std::unique_ptr<SharedMemory>> _memories;
        std::thread _saver;
        std::atomic<bool> _saving{ false };
        std::chrono::steady_clock::time_point _lastSave = std::chrono::steady_clock::now();
    };
}

#endif
