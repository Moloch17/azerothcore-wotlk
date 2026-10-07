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

#ifndef ANIMUS_VISION_ENTITY_MEMORY_H
#define ANIMUS_VISION_ENTITY_MEMORY_H

#include "Identity.h"
#include <cstdint>
#include <vector>

/// **Entity memory** (perception-goals §3 and its amendments; dungeon-curriculum I2): the entities a seat has seen --
/// what each was, where and how it was when last seen, and how long ago -- so "the lever is back there", "that pack
/// patrols that way" and "the boss is dead" outlive the frame. Pure: the entities block hands it each decision's
/// entity list (Vision::SeenList), the tests write lists by hand.
///
/// **Written only from sight** (amendment 4): Write reads the frame's visible list and nothing else. An entity the
/// frame did not show keeps every last-seen value -- its place, its health, alive or dead, a door's state -- whatever
/// has happened to it since; nothing here reads the server.
///
/// **Identity**: an entity is kept by its GUID (raw), the sim's own handle that never reaches the observation; the
/// observation names it by its memory id, its entry's index + 1, stable for as long as it is remembered (the session
/// on a map, or the episode). Full, the least recently seen entry not in this frame goes first, and its id is free
/// for the next newcomer.
///
/// **Lifetime**: as the mental map (amendment 1). A training seat's memory is kept across a reset on the same instance
/// MapRunSettings::KeepShare of the time, its clock moved on by the reset's random offset (Advance), else cleared; an
/// evaluation starts empty. Capped at MemorySettings::MaxEntities: MEMORY_TRAINING_CAP in training, the realm's own.
namespace Animus::Vision
{
    /// The training cap; the realm's is AnimusForge.Memory.MaxEntities.
    constexpr uint32_t MEMORY_TRAINING_CAP = 64;
    /// A realm may keep more, up to this.
    constexpr uint32_t MEMORY_CAP_LIMIT = 1024;
    /// Two sightings at most this far apart (seconds) give a velocity: the course of a patrol.
    constexpr float VELOCITY_WINDOW = 2.0f;
    /// A course under this speed (yards a second) is standing still.
    constexpr float MOVING_SPEED = 0.3f;

    struct MemorySettings
    {
        uint32_t MaxEntities = MEMORY_TRAINING_CAP;
    };
    [[nodiscard]] MemorySettings const& MemoryCurrent();
    void ConfigureMemory(MemorySettings const& settings);

    /// One remembered entity: its last sighting.
    struct Remembered
    {
        uint64_t Guid = 0;          // 0: the entry is free
        uint16_t MemoryId = 0;      // the memory id: the entry's index + 1
        Identity Id;                // its class and the UI flags as last seen
        uint32_t Entry = 0;
        bool GameObject = false;
        float Level = 0.0f;
        float Health = 1.0f;
        int8_t Reaction = 0;
        bool Dead = false;
        bool Open = false;
        bool Used = false;
        Vec3 Position;              // its middle in the world, as seen
        float Heading = 0.0f;       // the way it faced
        Vec3 Velocity;              // its course between its last two close sightings, yards a second
        bool Moving = false;        // ... faster than MOVING_SPEED
        double LastSeen = 0.0;
        uint64_t LastWrite = 0;     // the write that last saw it
    };

    class EntityMemory
    {
    public:
        void Configure(MemorySettings const& settings);
        [[nodiscard]] uint32_t Cap() const { return uint32_t(_entries.size()); }

        /// Forget everything; the clock starts again.
        void Clear();
        /// Move the memory's clock on: every sighting ages by it.
        void Advance(float seconds) { _clock += double(seconds > 0.0f ? seconds : 0.0f); }
        [[nodiscard]] double Clock() const { return _clock; }

        /// **The one write**: what this frame showed. Each listed entity's sighting is recorded (made if new); the
        /// rest keep theirs. A write with nothing listed still counts: nothing is "visible now" after it.
        void Write(SeenList const& seen);

        [[nodiscard]] Remembered const* Find(uint64_t guid) const;
        /// The memory id of `guid`, 0 when it is not remembered.
        [[nodiscard]] uint16_t IdOf(uint64_t guid) const;
        /// Whether the last write showed it.
        [[nodiscard]] bool VisibleNow(Remembered const& entry) const
        {
            return entry.Guid && entry.LastWrite == _writes;
        }
        [[nodiscard]] float AgeOf(Remembered const& entry) const { return float(_clock - entry.LastSeen); }
        [[nodiscard]] uint32_t Count() const { return _count; }
        [[nodiscard]] std::vector<Remembered> const& Entries() const { return _entries; }

        /// How little an entry matters to a seat at `from` (lower first): the log of its age, its distance in steps of
        /// RELEVANCE_YARDS, less a bonus for its quest mark, for a thing to use (a door, a lever, a usable object) and
        /// for a living hostile. The recalled list's order and nothing else (eviction is by age alone).
        [[nodiscard]] float Irrelevance(Remembered const& entry, Vec3 from) const;
        static constexpr float RELEVANCE_YARDS = 20.0f;

        /// The `count` most relevant entries the last write did not show, most relevant first, into `out`; how many.
        uint32_t Recall(Vec3 from, Remembered const** out, uint32_t count) const;

    private:
        Remembered* Slot(uint64_t guid);
        Remembered* Make(uint64_t guid);

        std::vector<Remembered> _entries = std::vector<Remembered>(MEMORY_TRAINING_CAP);
        uint32_t _count = 0;
        double _clock = 0.0;
        uint64_t _writes = 0;
    };
}

#endif
