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

#include "EntityMemory.h"
#include <algorithm>
#include <cmath>

namespace
{
    // Set once at startup (AnimusForge.Memory.*), read by every seat's reset.
    Animus::Vision::MemorySettings CurrentMemorySettings;
}

Animus::Vision::MemorySettings const& Animus::Vision::MemoryCurrent()
{
    return CurrentMemorySettings;
}

void Animus::Vision::ConfigureMemory(MemorySettings const& settings)
{
    CurrentMemorySettings = settings;
}

void Animus::Vision::EntityMemory::Configure(MemorySettings const& settings)
{
    uint32_t const cap = std::clamp<uint32_t>(settings.MaxEntities, 1, MEMORY_CAP_LIMIT);
    if (cap == _entries.size())
        return;
    // A new cap forgets: the ids are the entries' places.
    _entries.assign(cap, Remembered());
    _count = 0;
}

void Animus::Vision::EntityMemory::Clear()
{
    std::fill(_entries.begin(), _entries.end(), Remembered());
    _count = 0;
    _clock = 0.0;
    _writes = 0;
}

Animus::Vision::Remembered* Animus::Vision::EntityMemory::Slot(uint64_t guid)
{
    for (Remembered& entry : _entries)
        if (entry.Guid == guid)
            return &entry;
    return nullptr;
}

Animus::Vision::Remembered const* Animus::Vision::EntityMemory::Find(uint64_t guid) const
{
    if (!guid)
        return nullptr;
    for (Remembered const& entry : _entries)
        if (entry.Guid == guid)
            return &entry;
    return nullptr;
}

uint16_t Animus::Vision::EntityMemory::IdOf(uint64_t guid) const
{
    Remembered const* entry = Find(guid);
    return entry ? entry->MemoryId : 0;
}

Animus::Vision::Remembered* Animus::Vision::EntityMemory::Make(uint64_t guid)
{
    Remembered* place = nullptr;
    for (Remembered& entry : _entries)
        if (!entry.Guid)
        {
            place = &entry;
            break;
        }
    if (!place)
    {
        // Full: the least recently seen goes, never one this write has already shown.
        for (Remembered& entry : _entries)
            if (entry.LastWrite != _writes && (!place || entry.LastSeen < place->LastSeen))
                place = &entry;
        if (!place)
            return nullptr;
        --_count;
    }
    uint16_t const id = uint16_t(place - _entries.data() + 1);
    *place = Remembered();
    place->Guid = guid;
    place->MemoryId = id;
    place->FirstSeen = _clock;
    ++_count;
    return place;
}

void Animus::Vision::EntityMemory::Write(SeenList const& seen)
{
    ++_writes;
    uint32_t const count = std::min<uint32_t>(seen.Count, ENTITY_SLOTS);
    for (uint32_t slot = 0; slot < count; ++slot)
    {
        EntityInfo const& info = seen.Info[slot];
        if (!info.Guid)
            continue;
        Remembered* entry = Slot(info.Guid);
        bool const known = entry != nullptr;
        if (!entry)
            entry = Make(info.Guid);
        if (!entry)
            continue;

        // The course: from its last sighting when that was close enough in time to be the same walk.
        float const since = float(_clock - entry->LastSeen);
        if (known && entry->LastWrite + 1 >= _writes && since > 0.0f && since <= VELOCITY_WINDOW)
        {
            Vec3 const step = (info.Centre - entry->Position) * (1.0f / since);
            entry->Velocity = { step.X, step.Y, step.Z };
            entry->Moving = std::sqrt(step.X * step.X + step.Y * step.Y) >= MOVING_SPEED;
        }
        else if (!known)
        {
            entry->Velocity = Vec3();
            entry->Moving = false;
        }

        entry->Id = info.Id;
        entry->Entry = info.Entry;
        entry->GameObject = info.GameObject;
        entry->Level = info.Level;
        entry->Health = info.Health;
        entry->Reaction = info.Reaction;
        entry->Dead = info.Dead;
        entry->Open = info.Open;
        entry->Used = info.Used;
        entry->Position = info.Centre;
        entry->Heading = info.Orientation;
        entry->LastSeen = _clock;
        entry->LastWrite = _writes;
    }
}

float Animus::Vision::EntityMemory::Irrelevance(Remembered const& entry, Vec3 from) const
{
    float const age = std::max(0.0f, AgeOf(entry));
    float const distance = Length(entry.Position - from);
    float score = std::log2(1.0f + age) + distance / RELEVANCE_YARDS;
    if (entry.Id.Quest)
        score -= 2.0f;
    if (entry.Id.Usable || entry.Id.What == Class::Door)
        score -= 1.0f;
    if (entry.Reaction < 0 && !entry.Dead)
        score -= 1.0f;
    return score;
}

uint32_t Animus::Vision::EntityMemory::Recall(Vec3 from, Remembered const** out, uint32_t count) const
{
    thread_local std::vector<std::pair<float, Remembered const*>> ranked;
    ranked.clear();
    for (Remembered const& entry : _entries)
        if (entry.Guid && !VisibleNow(entry))
            ranked.emplace_back(Irrelevance(entry, from), &entry);
    uint32_t const kept = std::min<uint32_t>(count, uint32_t(ranked.size()));
    std::partial_sort(ranked.begin(), ranked.begin() + kept, ranked.end(), [](auto const& a, auto const& b)
    {
        return a.first != b.first ? a.first < b.first : a.second->MemoryId < b.second->MemoryId;
    });
    for (uint32_t i = 0; i < kept; ++i)
        out[i] = ranked[i].second;
    return kept;
}
