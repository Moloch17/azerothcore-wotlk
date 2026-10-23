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

#ifndef ANIMUS_VISION_OBJECT_MEMORY_H
#define ANIMUS_VISION_OBJECT_MEMORY_H

#include "Identity.h"
#include "MentalMap.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/// **Object memory** (the user, 2026-10-18: "I want it to remember objects it has seen so it knows where to find
/// them"): the things a seat's camera listed -- doors, gates, chests, nodes, mailboxes, vendors, quest givers,
/// creatures, players -- each kept by its identity with where it was, which way it faced, when it was last seen and
/// how often, the state a player reads off it (a door open or closed, a node used, a creature dead), and for a moving
/// thing the last few places it was seen at (a patrol's route). Perception-true: written only from the frame's entity
/// list (SeenList), never from the server's object lists; a thing the camera never listed is not in it.
///
/// **Identity**: a creature or game object by its spawn id (EntityInfo::SpawnId, the DB guid: the same thing in every
/// instance of its map and after a restart); a player, a summon or a temporary object by its GUID, kept while it is
/// remembered and never written to disk. Ground hazards are not kept.
///
/// **Update rules**:
/// - a sighting updates the entry in place (made on the first): position, facing, size, class, state, level and
///   health as seen; Sightings + 1; the missed-look count reset and Absent cleared; a creature or player that moved
///   TRACE_STEP from its last trace point adds one (the oldest of OBJECT_TRACE going);
/// - **expected but unseen**: an entry not listed by a frame whose stored middle is inside that frame's field of view,
///   within LOOK_REACH of the camera (and nearer than the list's farthest entry when the list was full), and not
///   behind what the frame's own rays hit (FrameHits: the ray nearest its direction stops short of it by more than
///   its radius and LOOK_SLACK) is a look that missed it; MISSED_LOOKS such looks in a row mark it Absent (a node
///   gathered, a chest gone, an NPC that moved, a creature killed and despawned). A frame without rays judges nothing;
/// - **decay**: a player is dropped PLAYER_KEEP_SECONDS after it was last seen; any other thing without a spawn id
///   TRANSIENT_KEEP_SECONDS after. A spawned creature is kept (its spawn point and its trace, the patrol it walks) as
///   fixed objects and stationary NPCs are: the age of its sighting says how stale its state is;
/// - **full** (the cap): the entry not seen by this write that matters least goes -- GUID-keyed things first, then
///   creatures, then objects and NPCs -- the oldest sighting first within each.
///
/// **Sharing and persistence** (SharedObjects): one store per game map id, beside the terrain's shared memory
/// (SharedMemory::Objects), which a seat adopts or starts blank on at the same draw and merges back into after a
/// training episode (newest look wins, by identity); saved to `<Dir>/<mapId>.aobj` with the terrain's .amem.
///
/// **For a planner** (no caller yet): Find (the remembered things of a kind or class near a place, nearest first) and
/// BlockingAt (a closed door or gate covers a map cell: a wall to a route, until a look sees it open).
///
/// Not thread safe; owned by one seat, used on its map thread (SharedObjects locks its own store).
namespace Animus::Vision
{
    /// What a remembered thing is, coarsely: how long it is kept and how a planner asks for it.
    enum class ObjectKind : uint8_t
    {
        Fixed = 0,      // a game object: a door, gate, lever, chest, mailbox, node, ...
        Npc = 1,        // a stationary NPC: vendor, quest giver, trainer, flight master (interactive, not hostile)
        Creature = 2,   // any other creature, hostile, neutral or friendly, alive or dead
        Player = 3,
        Count
    };
    constexpr uint32_t OBJECT_KINDS = uint32_t(ObjectKind::Count);
    constexpr char const* OBJECT_KIND_NAMES[OBJECT_KINDS] = { "fixed", "npc", "creature", "player" };

    /// Where an identity comes from.
    enum class ObjectSource : uint8_t
    {
        None = 0,
        CreatureSpawn = 1,  // Id: a creature's spawn id
        ObjectSpawn = 2,    // Id: a game object's spawn id
        Guid = 3,           // Id: the raw GUID (a player, a summon, a temporary object): never persisted
    };

    struct ObjectKey
    {
        ObjectSource Source = ObjectSource::None;
        uint64_t Id = 0;

        [[nodiscard]] bool operator==(ObjectKey const& other) const = default;
        [[nodiscard]] bool operator<(ObjectKey const& other) const
        {
            return Source != other.Source ? Source < other.Source : Id < other.Id;
        }
        /// Kept on disk and across instances (a spawn id), rather than for as long as the thing is in mind.
        [[nodiscard]] bool Lasting() const
        {
            return Source == ObjectSource::CreatureSpawn || Source == ObjectSource::ObjectSpawn;
        }
    };

    struct ObjectKeyHash
    {
        [[nodiscard]] std::size_t operator()(ObjectKey const& key) const
        {
            return std::size_t((key.Id * 0x9E3779B97F4A7C15ull) ^ (uint64_t(key.Source) << 61));
        }
    };

    /// The identity of a listed entity (spawn id, else GUID); Source None for one memory does not keep (a ground
    /// hazard, an entity with neither).
    [[nodiscard]] ObjectKey KeyOf(EntityInfo const& info);
    /// The kind of a listed entity, as its class and flags say.
    [[nodiscard]] ObjectKind KindOf(EntityInfo const& info);

    /// The places a moving thing was last seen at (a patrol's route).
    constexpr uint32_t OBJECT_TRACE = 8;
    /// ... one taken when it has moved this far (2D) from the last.
    constexpr float TRACE_STEP = 2.0f;
    /// Looks in a row that miss an entry where it should be in view before it is marked Absent.
    constexpr uint8_t MISSED_LOOKS = 4;
    /// The farthest a look judges an entry from (yards from the camera): a 32 x 16 frame is 3.75 degrees a pixel.
    constexpr float LOOK_REACH = 40.0f;
    /// A ray that stops this much (plus the entry's radius) short of an entry has its view of it blocked.
    constexpr float LOOK_SLACK = 1.0f;
    constexpr double PLAYER_KEEP_SECONDS = 120.0;
    constexpr double TRANSIENT_KEEP_SECONDS = 600.0;
    /// How often Advance looks for entries to drop (memory seconds).
    constexpr double DECAY_EVERY_SECONDS = 5.0;
    /// A seat's cap and a map's shared store's.
    constexpr uint32_t OBJECT_SEAT_CAP = 1024;
    constexpr uint32_t OBJECT_SHARED_CAP = 8192;
    /// A remembered door or gate shuts at least this radius round its middle (BlockingAt), yards.
    constexpr float DOOR_MIN_RADIUS = 1.0f;

    struct RememberedObject
    {
        ObjectKey Key;
        uint32_t Entry = 0;                 // the creature's or game object's template, 0 a player
        Class What = Class::OtherObject;    // its class at the last sighting
        ObjectKind Kind = ObjectKind::Fixed;
        bool GameObject = false;
        // The state a player reads off it, as last seen.
        bool Open = false;                  // a door or button standing open
        bool Used = false;                  // a game object used (activated, looted, gathered)
        bool Dead = false;
        bool Quest = false;                 // the seeing seat's own UI facts (stripped when shared)
        bool Lootable = false;
        bool Usable = false;
        /// Not where it was at the last MISSED_LOOKS looks that should have shown it.
        bool Absent = false;
        uint8_t Missed = 0;
        int8_t Reaction = 0;
        float Level = 0.0f;
        float Health = 1.0f;
        Vec3 Position;                      // its middle, as seen
        float Facing = 0.0f;
        float Radius = 0.0f;
        float Height = 0.0f;
        double FirstSeen = 0.0;             // memory seconds
        double LastSeen = 0.0;
        double LastLook = 0.0;              // the last sighting or missed look
        uint32_t Sightings = 0;
        /// Its trace: TraceCount places, oldest first (a moving thing's: creatures and players).
        uint8_t TraceCount = 0;
        std::array<Vec3, OBJECT_TRACE> Trace{};
        uint64_t LastWrite = 0;             // the write that listed it last (ObjectMemory::SeenNow)

        /// The trace's largest 2D extent from its newest point: 0 for a thing that stood still.
        [[nodiscard]] float TraceSpan() const;
    };

    /// The camera of a frame, as an expected-but-unseen look judges it: its place and view, the field of view
    /// (radians) and the frame's range.
    struct ObjectLook
    {
        Vec3 Camera;
        float Azimuth = 0.0f;
        float Elevation = 0.0f;
        float FovH = 0.0f;
        float FovV = 0.0f;
        float Range = 100.0f;
    };
    /// A SeenList's camera with the camera settings' field of view and range.
    [[nodiscard]] ObjectLook LookOf(SeenList const& seen, Settings const& settings);

    class SharedObjects;

    class ObjectMemory
    {
    public:
        ObjectMemory() = default;

        /// The cap (at least 1); entries past it go by the eviction order.
        void Configure(uint32_t cap);
        /// Forgets everything and leaves the shared store's anchor; the clock is kept.
        void Clear();
        /// The memory's clock moves on; the decay runs every DECAY_EVERY_SECONDS.
        void Advance(double seconds);
        /// Records a frame's list (see the file comment); `hits` the frame's rays (null or empty: no look is judged).
        /// Whether the line from a camera to a point is clear of the world (VisionCaster's SegmentBlocked): the
        /// looks' judge where there are no frame rays (a stage that renders nothing, StageDefinition::Camera).
        using LineClear = std::function<bool(Vec3 const& from, Vec3 const& to)>;
        void Write(SeenList const& seen, ObjectLook const& look, FrameHits const* hits, LineClear const& clear = {});

        [[nodiscard]] RememberedObject const* Find(ObjectKey const& key) const;
        /// Listed by the last Write.
        [[nodiscard]] bool SeenNow(RememberedObject const& entry) const
        {
            return _writes && entry.LastWrite == _writes;
        }
        [[nodiscard]] double AgeOf(RememberedObject const& entry) const
        {
            return std::max(0.0, _clock - entry.LastSeen);
        }

        /// **For a planner**: the remembered things of `kind` (Count: any) and `what` (Class::Count: any) within
        /// `radius` yards (2D; <= 0: anywhere) of `near`, nearest first (ties by identity), Absent ones included (a
        /// caller reads Absent); how many, into `out` (cleared first).
        uint32_t Find(ObjectKind kind, Class what, Vec3 near, float radius,
            std::vector<RememberedObject const*>& out) const;
        /// **For a planner**: whether a remembered closed door or gate (class Door, not Open, not Absent) covers the
        /// 1-yd map cell (cx, cy): its middle within max(radius, DOOR_MIN_RADIUS) of the cell's centre.
        /// The single-cell probe: a linear scan of the entries. A planner over a window stamps Blocking's
        /// footprints once instead.
        [[nodiscard]] bool BlockingAt(int32_t cx, int32_t cy) const;
        /// **For a planner**: the remembered closed doors and gates (BlockingAt's rule), into `out` (cleared first),
        /// in identity order; each shuts the cells within max(Radius, DOOR_MIN_RADIUS) of its Position. How many.
        uint32_t Blocking(std::vector<RememberedObject const*>& out) const;

        /// The `count` most relevant entries not listed by the last write, into `out`, most relevant first: nearest
        /// `from`, sooner seen, a fixed thing or an NPC before a creature, an Absent one after the rest (score
        /// distance / 20 + log2(1 + age) / 2 - 1 for fixed or NPC + 2 for Absent; ties by identity). How many.
        uint32_t Recall(Vec3 from, RememberedObject const** out, uint32_t count) const;

        [[nodiscard]] std::vector<RememberedObject> const& Entries() const { return _entries; }
        [[nodiscard]] std::size_t Count() const { return _entries.size(); }
        [[nodiscard]] double Clock() const { return _clock; }
        [[nodiscard]] uint32_t Cap() const { return _cap; }
        /// Anchored on a shared store (SharedObjects::Adopt or Anchor): its looks go back into it (Absorb).
        [[nodiscard]] bool Anchored() const { return _anchored; }

    private:
        friend class SharedObjects;

        RememberedObject* Slot(ObjectKey const& key);
        RememberedObject* Make(ObjectKey const& key);
        void Decay();
        void Reindex();
        /// Inserts (or replaces) an entry as it is; the cap is the caller's.
        void Put(RememberedObject const& entry);

        std::vector<RememberedObject> _entries;
        std::unordered_map<ObjectKey, uint32_t, ObjectKeyHash> _index;
        double _clock = 0.0;
        double _nextDecay = 0.0;
        uint64_t _writes = 0;
        uint32_t _cap = OBJECT_SEAT_CAP;
        bool _anchored = false;
        double _anchoredAt = 0.0;
    };

    /// 1 (2026-10-18): the first.
    constexpr uint32_t OBJECT_MEMORY_VERSION = 1;

#pragma pack(push, 1)
    /// The .aobj file's header (OBJECT_MEMORY_VERSION). Little-endian. Count ObjectRecords follow it.
    struct ObjectFileHeader
    {
        char Magic[4] = { 'A', 'O', 'B', 'J' };
        uint32_t Version = OBJECT_MEMORY_VERSION;
        uint32_t MapId = 0;
        uint32_t RecordBytes = 176;
        double Clock = 0.0;             // the store's seconds at the save
        int64_t WallSeconds = 0;        // the Unix time of the save
        uint32_t Count = 0;
        uint64_t PayloadChecksum = 0;   // FNV-1a 64 over the records
        uint32_t Reserved[5] = {};
    };

    /// One remembered thing on disk: a spawn's (a GUID-keyed thing and a player are never written).
    struct ObjectRecord
    {
        uint8_t Source = 0;
        uint8_t Kind = 0;
        uint8_t What = 0;
        uint8_t Flags = 0;              // ObjectFlag
        uint64_t Id = 0;
        uint32_t Entry = 0;
        int8_t Reaction = 0;
        uint8_t Missed = 0;
        uint8_t TraceCount = 0;
        uint8_t Pad = 0;
        float Level = 0.0f;
        float Health = 0.0f;
        float Position[3] = {};
        float Facing = 0.0f;
        float Radius = 0.0f;
        float Height = 0.0f;
        double FirstSeen = 0.0;
        double LastSeen = 0.0;
        double LastLook = 0.0;
        uint32_t Sightings = 0;
        float Trace[OBJECT_TRACE][3] = {};
    };
#pragma pack(pop)
    static_assert(sizeof(ObjectFileHeader) == 64, "the object memory file's header is 64 bytes");
    static_assert(sizeof(ObjectRecord) == 176, "an object memory record is 176 bytes");

    enum ObjectFlag : uint8_t
    {
        OBJECT_FLAG_GAME_OBJECT = 0x01,
        OBJECT_FLAG_OPEN        = 0x02,
        OBJECT_FLAG_USED        = 0x04,
        OBJECT_FLAG_DEAD        = 0x08,
        OBJECT_FLAG_ABSENT      = 0x10,
        OBJECT_FLAG_USABLE      = 0x20,
    };

    /// **A map's shared object memory**: what every seat on the map saw, merged by identity; the newest sighting
    /// decides where a thing is and how it stood, the newest look whether it was missing. Seat-specific facts (a
    /// quest mark, a quest's glow, a corpse's loot sparkle) are stripped on the way in: what another seat's quests
    /// made of a thing is not this one's. Its clock is the caller's (`now`, the process's game seconds) plus an
    /// offset the file carries across restarts (the wall-clock gap between a save and the next load is added).
    /// Thread safe (one mutex; a seat touches it at an episode's first observation).
    class SharedObjects
    {
    public:
        explicit SharedObjects(uint32_t mapId, uint32_t cap = OBJECT_SHARED_CAP);
        SharedObjects(SharedObjects const&) = delete;
        SharedObjects& operator=(SharedObjects const&) = delete;

        /// `memory` starts as this store (cleared first, its clock the store's now, the store's entries copied --
        /// the most recently seen first when they pass its cap), anchored so Absorb can merge it back.
        void Adopt(ObjectMemory& memory, double now);
        /// `memory` starts blank on the store's clock, anchored: an episode that does not read it but writes it.
        void Anchor(ObjectMemory& memory, double now);
        /// Merges the entries an anchored memory saw or looked for since it was anchored; its clock is moved onto the
        /// store's (its newest look reads as `now`). An unanchored memory is ignored.
        void Absorb(ObjectMemory const& memory, double now);

        /// Loads `path`: false with `error` when it is missing, of another version, map or record size, or does not
        /// check out (the store is left as it was).
        [[nodiscard]] bool Load(std::string const& path, double now, std::string& error);
        /// Writes the lasting entries to `path` (a .part file renamed over it), in identity order.
        [[nodiscard]] bool Save(std::string const& path, double now, std::string& error);
        [[nodiscard]] bool Dirty() const;
        [[nodiscard]] std::size_t Count() const;
        [[nodiscard]] uint32_t MapId() const { return _mapId; }
        /// A copy of the entries, on the store's clock (inspection).
        [[nodiscard]] std::vector<RememberedObject> Snapshot() const;

    private:
        [[nodiscard]] double Relative(double now) const { return now + _offset; }

        uint32_t _mapId;
        mutable std::mutex _lock;
        ObjectMemory _store;            // the store's entries, on its own clock (Relative)
        double _offset = 0.0;
        uint64_t _revision = 0;
        uint64_t _savedRevision = 0;
    };
}

#endif
