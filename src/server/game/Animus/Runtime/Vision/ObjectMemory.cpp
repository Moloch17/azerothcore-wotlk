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

#include "ObjectMemory.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{
    using namespace Animus::Vision;
    namespace Fs = std::filesystem;

    [[nodiscard]] int64_t WallNow()
    {
        return int64_t(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    [[nodiscard]] uint64_t Fnv1a(void const* data, std::size_t bytes, uint64_t hash)
    {
        auto const* in = static_cast<uint8_t const*>(data);
        for (std::size_t i = 0; i < bytes; ++i)
        {
            hash ^= in[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }
    constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ull;

    [[nodiscard]] float WrapAngle(float angle)
    {
        while (angle > PI)
            angle -= 2.0f * PI;
        while (angle < -PI)
            angle += 2.0f * PI;
        return angle;
    }

    [[nodiscard]] float Flat(Vec3 a, Vec3 b)
    {
        float const dx = a.X - b.X;
        float const dy = a.Y - b.Y;
        return std::sqrt(dx * dx + dy * dy);
    }

    /// How little an entry matters when the memory is full: GUID-keyed things and players first, then creatures,
    /// then objects and NPCs.
    [[nodiscard]] int Tier(RememberedObject const& entry)
    {
        if (!entry.Key.Lasting() || entry.Kind == ObjectKind::Player)
            return 0;
        return entry.Kind == ObjectKind::Creature ? 1 : 2;
    }

    [[nodiscard]] bool Moves(ObjectKind kind)
    {
        return kind == ObjectKind::Creature || kind == ObjectKind::Player;
    }

    /// The index of the frame's ray nearest direction `dir` (a unit vector): a hill climb from the pixel the view
    /// angles name, on the rays' own directions, so the caster's pixel layout is read off the rays themselves.
    [[nodiscard]] uint32_t NearestRay(FrameHits const& hits, ObjectLook const& look, float yawRight, float pitchUp,
        Vec3 dir)
    {
        uint32_t const width = hits.Width;
        uint32_t const height = hits.Height;
        if (!width || !height || hits.Rays.size() != std::size_t(width) * height)
        {
            uint32_t best = 0;
            float bestDot = -2.0f;
            for (uint32_t i = 0; i < hits.Rays.size(); ++i)
            {
                float const dot = Dot(hits.Rays[i].Dir, dir);
                if (dot > bestDot)
                {
                    bestDot = dot;
                    best = i;
                }
            }
            return best;
        }
        auto const clampTo = [](float value, uint32_t count)
        {
            return uint32_t(std::clamp(int32_t(std::floor(value)), 0, int32_t(count) - 1));
        };
        uint32_t col = clampTo((0.5f + yawRight / std::max(look.FovH, 1e-3f)) * float(width), width);
        uint32_t row = clampTo((0.5f - pitchUp / std::max(look.FovV, 1e-3f)) * float(height), height);
        float best = Dot(hits.Rays[row * width + col].Dir, dir);
        for (uint32_t step = 0; step < width + height; ++step)
        {
            uint32_t nextCol = col;
            uint32_t nextRow = row;
            for (int32_t dy = -1; dy <= 1; ++dy)
                for (int32_t dx = -1; dx <= 1; ++dx)
                {
                    int32_t const c = int32_t(col) + dx;
                    int32_t const r = int32_t(row) + dy;
                    if ((!dx && !dy) || c < 0 || r < 0 || c >= int32_t(width) || r >= int32_t(height))
                        continue;
                    float const dot = Dot(hits.Rays[uint32_t(r) * width + uint32_t(c)].Dir, dir);
                    if (dot > best)
                    {
                        best = dot;
                        nextCol = uint32_t(c);
                        nextRow = uint32_t(r);
                    }
                }
            if (nextCol == col && nextRow == row)
                break;
            col = nextCol;
            row = nextRow;
        }
        return row * width + col;
    }

    /// What another seat may read of an entry: its class and flags without the seeing seat's quests and loot.
    void StripSeatFacts(RememberedObject& entry)
    {
        if (entry.What == Class::QuestGiver)
            entry.What = entry.GameObject ? Class::UsableObject : Class::FriendlyCreature;
        else if (entry.What == Class::LootableCorpse)
            entry.What = Class::Corpse;
        else if (entry.What == Class::QuestObject)
            entry.What = Class::UsableObject;
        entry.Quest = false;
        entry.Lootable = false;
    }

    [[nodiscard]] ObjectRecord RecordOf(RememberedObject const& entry)
    {
        ObjectRecord record;
        record.Source = uint8_t(entry.Key.Source);
        record.Kind = uint8_t(entry.Kind);
        record.What = uint8_t(entry.What);
        record.Flags = uint8_t((entry.GameObject ? OBJECT_FLAG_GAME_OBJECT : 0) | (entry.Open ? OBJECT_FLAG_OPEN : 0)
            | (entry.Used ? OBJECT_FLAG_USED : 0) | (entry.Dead ? OBJECT_FLAG_DEAD : 0)
            | (entry.Absent ? OBJECT_FLAG_ABSENT : 0) | (entry.Usable ? OBJECT_FLAG_USABLE : 0));
        record.Id = entry.Key.Id;
        record.Entry = entry.Entry;
        record.Reaction = entry.Reaction;
        record.Missed = entry.Missed;
        record.TraceCount = entry.TraceCount;
        record.Level = entry.Level;
        record.Health = entry.Health;
        record.Position[0] = entry.Position.X;
        record.Position[1] = entry.Position.Y;
        record.Position[2] = entry.Position.Z;
        record.Facing = entry.Facing;
        record.Radius = entry.Radius;
        record.Height = entry.Height;
        record.FirstSeen = entry.FirstSeen;
        record.LastSeen = entry.LastSeen;
        record.LastLook = entry.LastLook;
        record.Sightings = entry.Sightings;
        for (uint32_t i = 0; i < OBJECT_TRACE; ++i)
        {
            record.Trace[i][0] = entry.Trace[i].X;
            record.Trace[i][1] = entry.Trace[i].Y;
            record.Trace[i][2] = entry.Trace[i].Z;
        }
        return record;
    }

    /// False for a record no build writes (a GUID key, a player, a class or kind out of range, not finite).
    [[nodiscard]] bool EntryOf(ObjectRecord const& record, RememberedObject& entry)
    {
        auto const source = ObjectSource(record.Source);
        if ((source != ObjectSource::CreatureSpawn && source != ObjectSource::ObjectSpawn) || !record.Id
            || record.Kind >= uint8_t(ObjectKind::Player) || record.What >= uint8_t(Class::Count)
            || record.TraceCount > OBJECT_TRACE || !std::isfinite(record.LastSeen) || !std::isfinite(record.LastLook)
            || !std::isfinite(record.FirstSeen) || !std::isfinite(record.Position[0])
            || !std::isfinite(record.Position[1]) || !std::isfinite(record.Position[2]))
            return false;
        entry = RememberedObject();
        entry.Key = { source, record.Id };
        entry.Kind = ObjectKind(record.Kind);
        entry.What = Class(record.What);
        entry.GameObject = record.Flags & OBJECT_FLAG_GAME_OBJECT;
        entry.Open = record.Flags & OBJECT_FLAG_OPEN;
        entry.Used = record.Flags & OBJECT_FLAG_USED;
        entry.Dead = record.Flags & OBJECT_FLAG_DEAD;
        entry.Absent = record.Flags & OBJECT_FLAG_ABSENT;
        entry.Usable = record.Flags & OBJECT_FLAG_USABLE;
        entry.Entry = record.Entry;
        entry.Reaction = record.Reaction;
        entry.Missed = record.Missed;
        entry.TraceCount = record.TraceCount;
        entry.Level = record.Level;
        entry.Health = record.Health;
        entry.Position = { record.Position[0], record.Position[1], record.Position[2] };
        entry.Facing = record.Facing;
        entry.Radius = record.Radius;
        entry.Height = record.Height;
        entry.FirstSeen = record.FirstSeen;
        entry.LastSeen = record.LastSeen;
        entry.LastLook = record.LastLook;
        entry.Sightings = record.Sightings;
        for (uint32_t i = 0; i < OBJECT_TRACE; ++i)
            entry.Trace[i] = { record.Trace[i][0], record.Trace[i][1], record.Trace[i][2] };
        return true;
    }
}

Animus::Vision::ObjectKey Animus::Vision::KeyOf(EntityInfo const& info)
{
    if (info.Id.What == Class::GroundHazard)
        return {};
    if (info.SpawnId)
        return { info.GameObject ? ObjectSource::ObjectSpawn : ObjectSource::CreatureSpawn, info.SpawnId };
    if (info.Guid)
        return { ObjectSource::Guid, info.Guid };
    return {};
}

Animus::Vision::ObjectKind Animus::Vision::KindOf(EntityInfo const& info)
{
    if (info.GameObject)
        return ObjectKind::Fixed;
    if (info.Id.What == Class::HostilePlayer || info.Id.What == Class::FriendlyPlayer || (!info.Entry && !info.SpawnId))
        return ObjectKind::Player;
    if (info.Id.What == Class::QuestGiver || info.Id.What == Class::Vendor || info.Id.What == Class::Trainer
        || (info.Id.Usable && info.Reaction >= 0 && !info.Dead))
        return ObjectKind::Npc;
    return ObjectKind::Creature;
}

Animus::Vision::ObjectLook Animus::Vision::LookOf(SeenList const& seen, Settings const& settings)
{
    ObjectLook look;
    look.Camera = seen.Camera;
    look.Azimuth = seen.Azimuth;
    look.Elevation = seen.Elevation;
    look.FovH = settings.FovH * DEGREES;
    look.FovV = settings.FovV * DEGREES;
    look.Range = settings.Range;
    return look;
}

float Animus::Vision::RememberedObject::TraceSpan() const
{
    if (TraceCount < 2)
        return 0.0f;
    Vec3 const newest = Trace[TraceCount - 1];
    float span = 0.0f;
    for (uint32_t i = 0; i + 1 < TraceCount; ++i)
        span = std::max(span, Flat(Trace[i], newest));
    return span;
}

void Animus::Vision::ObjectMemory::Configure(uint32_t cap)
{
    _cap = std::max<uint32_t>(1, cap);
    if (_entries.size() <= _cap)
        return;
    // Over a smaller cap: the least that matters goes first, as at a full Make.
    std::vector<uint32_t> order(_entries.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
    {
        RememberedObject const& x = _entries[a];
        RememberedObject const& y = _entries[b];
        if (Tier(x) != Tier(y))
            return Tier(x) > Tier(y);
        if (x.LastSeen != y.LastSeen)
            return x.LastSeen > y.LastSeen;
        return x.Key < y.Key;
    });
    std::vector<RememberedObject> kept;
    kept.reserve(_cap);
    std::vector<bool> keep(_entries.size(), false);
    for (uint32_t i = 0; i < _cap; ++i)
        keep[order[i]] = true;
    for (uint32_t i = 0; i < _entries.size(); ++i)
        if (keep[i])
            kept.push_back(_entries[i]);
    _entries = std::move(kept);
    Reindex();
}

void Animus::Vision::ObjectMemory::Clear()
{
    _entries.clear();
    _index.clear();
    _anchored = false;
    _anchoredAt = 0.0;
    _nextDecay = _clock + DECAY_EVERY_SECONDS;
}

void Animus::Vision::ObjectMemory::Advance(double seconds)
{
    if (seconds > 0.0 && std::isfinite(seconds))
        _clock += seconds;
    if (_clock >= _nextDecay)
    {
        Decay();
        _nextDecay = _clock + DECAY_EVERY_SECONDS;
    }
}

void Animus::Vision::ObjectMemory::Decay()
{
    std::size_t const before = _entries.size();
    std::erase_if(_entries, [&](RememberedObject const& entry)
    {
        double const age = _clock - entry.LastSeen;
        if (entry.Kind == ObjectKind::Player)
            return age > PLAYER_KEEP_SECONDS;
        return !entry.Key.Lasting() && age > TRANSIENT_KEEP_SECONDS;
    });
    if (_entries.size() != before)
        Reindex();
}

void Animus::Vision::ObjectMemory::Reindex()
{
    _index.clear();
    for (uint32_t i = 0; i < _entries.size(); ++i)
        _index[_entries[i].Key] = i;
}

Animus::Vision::RememberedObject* Animus::Vision::ObjectMemory::Slot(ObjectKey const& key)
{
    auto const found = _index.find(key);
    return found == _index.end() ? nullptr : &_entries[found->second];
}

Animus::Vision::RememberedObject const* Animus::Vision::ObjectMemory::Find(ObjectKey const& key) const
{
    auto const found = _index.find(key);
    return found == _index.end() ? nullptr : &_entries[found->second];
}

Animus::Vision::RememberedObject* Animus::Vision::ObjectMemory::Make(ObjectKey const& key)
{
    if (_entries.size() >= _cap)
    {
        // The entry not shown by this write that matters least: lowest tier, then the oldest sighting.
        int32_t victim = -1;
        for (uint32_t i = 0; i < _entries.size(); ++i)
        {
            RememberedObject const& entry = _entries[i];
            if (SeenNow(entry))
                continue;
            if (victim < 0)
            {
                victim = int32_t(i);
                continue;
            }
            RememberedObject const& worst = _entries[uint32_t(victim)];
            if (Tier(entry) < Tier(worst) || (Tier(entry) == Tier(worst) && (entry.LastSeen < worst.LastSeen
                || (entry.LastSeen == worst.LastSeen && entry.Key < worst.Key))))
                victim = int32_t(i);
        }
        if (victim < 0)
            return nullptr;
        // Swap-remove: the last entry takes the victim's place.
        uint32_t const at = uint32_t(victim);
        _index.erase(_entries[at].Key);
        if (at + 1 != _entries.size())
        {
            _entries[at] = std::move(_entries.back());
            _index[_entries[at].Key] = at;
        }
        _entries.pop_back();
    }
    RememberedObject entry;
    entry.Key = key;
    _index[key] = uint32_t(_entries.size());
    _entries.push_back(entry);
    return &_entries.back();
}

void Animus::Vision::ObjectMemory::Put(RememberedObject const& entry)
{
    if (RememberedObject* existing = Slot(entry.Key))
    {
        *existing = entry;
        return;
    }
    _index[entry.Key] = uint32_t(_entries.size());
    _entries.push_back(entry);
}

void Animus::Vision::ObjectMemory::Write(SeenList const& seen, ObjectLook const& look, FrameHits const* hits)
{
    ++_writes;
    float listedFar = std::numeric_limits<float>::infinity();
    uint32_t const count = std::min<uint32_t>(seen.Count, ENTITY_SLOTS);
    for (uint32_t slot = 0; slot < count; ++slot)
    {
        EntityInfo const& info = seen.Info[slot];
        ObjectKey const key = KeyOf(info);
        if (key.Source == ObjectSource::None)
            continue;
        RememberedObject* entry = Slot(key);
        bool const fresh = entry == nullptr;
        if (fresh)
            entry = Make(key);
        if (!entry)
            continue;
        ObjectKind const kind = KindOf(info);
        if (fresh)
        {
            entry->FirstSeen = _clock;
            entry->Kind = kind;
        }
        // A stationary NPC stays one when its mark goes (a quest handed in) or it dies; anything else is as seen.
        else if (entry->Kind != ObjectKind::Npc || kind == ObjectKind::Player || kind == ObjectKind::Fixed)
            entry->Kind = kind;
        entry->Entry = info.Entry;
        entry->What = info.Id.What;
        entry->GameObject = info.GameObject;
        entry->Open = info.Open;
        entry->Used = info.Used;
        entry->Dead = info.Dead;
        entry->Quest = info.Id.Quest;
        entry->Lootable = info.Id.Lootable;
        entry->Usable = info.Id.Usable;
        entry->Reaction = info.Reaction;
        entry->Level = info.Level;
        entry->Health = info.Health;
        entry->Position = info.Centre;
        entry->Facing = info.Orientation;
        entry->Radius = info.Radius;
        entry->Height = info.Height;
        entry->LastSeen = _clock;
        entry->LastLook = _clock;
        entry->Missed = 0;
        entry->Absent = false;
        entry->LastWrite = _writes;
        ++entry->Sightings;
        if (Moves(entry->Kind) && (!entry->TraceCount
            || Flat(entry->Trace[entry->TraceCount - 1], info.Centre) >= TRACE_STEP))
        {
            if (entry->TraceCount == OBJECT_TRACE)
            {
                std::move(entry->Trace.begin() + 1, entry->Trace.end(), entry->Trace.begin());
                --entry->TraceCount;
            }
            entry->Trace[entry->TraceCount++] = info.Centre;
        }
    }
    // The list is the nearest ENTITY_SLOTS: past its farthest, a full list says nothing of what is there.
    if (seen.Count >= ENTITY_SLOTS && count)
        listedFar = Length(seen.Info[count - 1].Centre - look.Camera);

    if (!hits || hits->Rays.empty())
        return;
    float const reach = std::min(look.Range, LOOK_REACH);
    for (RememberedObject& entry : _entries)
    {
        if (entry.LastWrite == _writes)
            continue;
        Vec3 const offset = entry.Position - look.Camera;
        float const distance = Length(offset);
        if (distance > reach || distance >= listedFar || distance < 1e-3f)
            continue;
        float const elevation = std::asin(std::clamp(offset.Z / distance, -1.0f, 1.0f));
        float const yawRight = WrapAngle(look.Azimuth - std::atan2(offset.Y, offset.X));
        float const pitchUp = elevation - look.Elevation;
        if (std::fabs(yawRight) > look.FovH / 2.0f || std::fabs(pitchUp) > look.FovV / 2.0f)
            continue;
        Vec3 const dir = offset * (1.0f / distance);
        RayHit const& ray = hits->Rays[NearestRay(*hits, look, yawRight, pitchUp, dir)];
        if (ray.What != Class::Sky && ray.Distance + LOOK_SLACK + std::max(entry.Radius, 0.0f) < distance)
            continue;
        entry.LastLook = _clock;
        if (entry.Missed < 255)
            ++entry.Missed;
        if (entry.Missed >= MISSED_LOOKS)
            entry.Absent = true;
    }
}

uint32_t Animus::Vision::ObjectMemory::Find(ObjectKind kind, Class what, Vec3 near, float radius,
    std::vector<RememberedObject const*>& out) const
{
    out.clear();
    for (RememberedObject const& entry : _entries)
    {
        if (kind != ObjectKind::Count && entry.Kind != kind)
            continue;
        if (what != Class::Count && entry.What != what)
            continue;
        if (radius > 0.0f && Flat(entry.Position, near) > radius)
            continue;
        out.push_back(&entry);
    }
    std::sort(out.begin(), out.end(), [&](RememberedObject const* a, RememberedObject const* b)
    {
        float const da = Flat(a->Position, near);
        float const db = Flat(b->Position, near);
        return da != db ? da < db : a->Key < b->Key;
    });
    return uint32_t(out.size());
}

namespace
{
    [[nodiscard]] bool Shut(RememberedObject const& entry)
    {
        return entry.What == Class::Door && entry.GameObject && !entry.Open && !entry.Absent;
    }
}

bool Animus::Vision::ObjectMemory::BlockingAt(int32_t cx, int32_t cy) const
{
    Vec3 const centre{ float(cx) + 0.5f, float(cy) + 0.5f, 0.0f };
    for (RememberedObject const& entry : _entries)
        if (Shut(entry) && Flat(entry.Position, centre) <= std::max(entry.Radius, DOOR_MIN_RADIUS))
            return true;
    return false;
}

uint32_t Animus::Vision::ObjectMemory::Blocking(std::vector<RememberedObject const*>& out) const
{
    out.clear();
    for (RememberedObject const& entry : _entries)
        if (Shut(entry))
            out.push_back(&entry);
    std::sort(out.begin(), out.end(), [](RememberedObject const* a, RememberedObject const* b)
    {
        return a->Key < b->Key;
    });
    return uint32_t(out.size());
}

uint32_t Animus::Vision::ObjectMemory::Recall(Vec3 from, RememberedObject const** out, uint32_t count) const
{
    if (!count)
        return 0;
    thread_local std::vector<std::pair<float, RememberedObject const*>> ranked;
    ranked.clear();
    for (RememberedObject const& entry : _entries)
    {
        if (SeenNow(entry))
            continue;
        float score = Length(entry.Position - from) / 20.0f + 0.5f * std::log2(1.0f + float(AgeOf(entry)));
        if (entry.Kind == ObjectKind::Fixed || entry.Kind == ObjectKind::Npc)
            score -= 1.0f;
        if (entry.Absent)
            score += 2.0f;
        ranked.emplace_back(score, &entry);
    }
    uint32_t const kept = std::min<uint32_t>(count, uint32_t(ranked.size()));
    std::partial_sort(ranked.begin(), ranked.begin() + kept, ranked.end(), [](auto const& a, auto const& b)
    {
        return a.first != b.first ? a.first < b.first : a.second->Key < b.second->Key;
    });
    for (uint32_t i = 0; i < kept; ++i)
        out[i] = ranked[i].second;
    return kept;
}

Animus::Vision::SharedObjects::SharedObjects(uint32_t mapId, uint32_t cap) : _mapId(mapId)
{
    _store.Configure(cap);
}

void Animus::Vision::SharedObjects::Adopt(ObjectMemory& memory, double now)
{
    std::lock_guard lock(_lock);
    double const clock = Relative(now);
    _store._clock = std::max(_store._clock, clock);
    _store.Decay();
    memory.Clear();
    // The memory's clock is the store's now, past every look it holds, so none of them counts as this episode's.
    double newest = _store._clock;
    for (RememberedObject const& entry : _store._entries)
        newest = std::max(newest, entry.LastLook);
    memory._clock = newest;
    std::vector<RememberedObject const*> order;
    order.reserve(_store._entries.size());
    for (RememberedObject const& entry : _store._entries)
        order.push_back(&entry);
    if (order.size() > memory._cap)
        std::partial_sort(order.begin(), order.begin() + memory._cap, order.end(),
            [](RememberedObject const* a, RememberedObject const* b)
            {
                return a->LastSeen != b->LastSeen ? a->LastSeen > b->LastSeen : a->Key < b->Key;
            });
    std::size_t const taken = std::min<std::size_t>(order.size(), memory._cap);
    memory._entries.reserve(taken);
    for (std::size_t i = 0; i < taken; ++i)
    {
        memory._entries.push_back(*order[i]);
        memory._entries.back().LastWrite = 0;
    }
    memory.Reindex();
    memory._nextDecay = memory._clock + DECAY_EVERY_SECONDS;
    memory._anchored = true;
    memory._anchoredAt = memory._clock;
}

void Animus::Vision::SharedObjects::Anchor(ObjectMemory& memory, double now)
{
    std::lock_guard lock(_lock);
    memory.Clear();
    double newest = std::max(_store._clock, Relative(now));
    for (RememberedObject const& entry : _store._entries)
        newest = std::max(newest, entry.LastLook);
    memory._clock = newest;
    memory._nextDecay = memory._clock + DECAY_EVERY_SECONDS;
    memory._anchored = true;
    memory._anchoredAt = memory._clock;
}

void Animus::Vision::SharedObjects::Absorb(ObjectMemory const& memory, double now)
{
    if (!memory._anchored)
        return;
    std::lock_guard lock(_lock);
    double const clock = std::max(_store._clock, Relative(now));
    _store._clock = clock;
    // The memory's looks onto the store's clock: its own now reads as the store's.
    double const shift = clock - memory._clock;
    bool changed = false;
    for (RememberedObject const& mine : memory._entries)
    {
        if (mine.LastLook < memory._anchoredAt)
            continue;
        RememberedObject entry = mine;
        entry.FirstSeen += shift;
        entry.LastSeen += shift;
        entry.LastLook += shift;
        entry.LastWrite = 0;
        StripSeatFacts(entry);
        RememberedObject* held = _store.Slot(entry.Key);
        if (!held)
        {
            if (_store._entries.size() >= _store._cap)
            {
                // Make's eviction, on the store (nothing in it is "shown now").
                RememberedObject* made = _store.Make(entry.Key);
                if (!made)
                    continue;
                *made = entry;
            }
            else
                _store.Put(entry);
            changed = true;
            continue;
        }
        // Newest sighting decides where it is and how it stood; newest look whether it was missing.
        if (entry.LastSeen > held->LastSeen)
        {
            double const firstSeen = std::min(held->FirstSeen, entry.FirstSeen);
            uint32_t const sightings = std::max(held->Sightings, entry.Sightings);
            ObjectKind const kind = held->Kind == ObjectKind::Npc && entry.Kind == ObjectKind::Creature
                ? ObjectKind::Npc : entry.Kind;
            double const lastLook = std::max(held->LastLook, entry.LastLook);
            uint8_t const missed = held->LastLook > entry.LastLook ? held->Missed : entry.Missed;
            bool const absent = held->LastLook > entry.LastLook ? held->Absent : entry.Absent;
            *held = entry;
            held->FirstSeen = firstSeen;
            held->Sightings = sightings;
            held->Kind = kind;
            held->LastLook = lastLook;
            held->Missed = missed;
            held->Absent = absent;
            changed = true;
        }
        else if (entry.LastLook > held->LastLook)
        {
            held->LastLook = entry.LastLook;
            held->Missed = entry.Missed;
            held->Absent = entry.Absent;
            held->FirstSeen = std::min(held->FirstSeen, entry.FirstSeen);
            changed = true;
        }
    }
    _store.Decay();
    if (changed)
        ++_revision;
}

bool Animus::Vision::SharedObjects::Dirty() const
{
    std::lock_guard lock(_lock);
    return _revision != _savedRevision;
}

std::size_t Animus::Vision::SharedObjects::Count() const
{
    std::lock_guard lock(_lock);
    return _store._entries.size();
}

std::vector<Animus::Vision::RememberedObject> Animus::Vision::SharedObjects::Snapshot() const
{
    std::lock_guard lock(_lock);
    return _store._entries;
}

bool Animus::Vision::SharedObjects::Load(std::string const& path, double now, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "no file";
        return false;
    }
    ObjectFileHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    ObjectFileHeader const expected;
    if (!in || std::memcmp(header.Magic, expected.Magic, sizeof(header.Magic)) != 0)
    {
        error = "not an object memory file";
        return false;
    }
    if (header.Version != OBJECT_MEMORY_VERSION || header.RecordBytes != sizeof(ObjectRecord))
    {
        error = "version " + std::to_string(header.Version) + " (this build reads "
            + std::to_string(OBJECT_MEMORY_VERSION) + ")";
        return false;
    }
    if (header.MapId != _mapId)
    {
        error = "of map " + std::to_string(header.MapId);
        return false;
    }
    if (header.Count > 1u << 20 || !std::isfinite(header.Clock))
    {
        error = "a corrupt header";
        return false;
    }
    std::vector<ObjectRecord> records(header.Count);
    if (header.Count)
        in.read(reinterpret_cast<char*>(records.data()), std::streamsize(sizeof(ObjectRecord) * records.size()));
    if (!in)
    {
        error = "cut short";
        return false;
    }
    if (Fnv1a(records.data(), sizeof(ObjectRecord) * records.size(), FNV_OFFSET) != header.PayloadChecksum)
    {
        error = "its checksum does not match";
        return false;
    }
    std::vector<RememberedObject> entries;
    entries.reserve(records.size());
    double newest = header.Clock;
    for (ObjectRecord const& record : records)
    {
        RememberedObject entry;
        if (!EntryOf(record, entry))
            continue;
        newest = std::max(newest, entry.LastLook);
        entries.push_back(entry);
    }

    std::lock_guard lock(_lock);
    _store._entries.clear();
    _store._index.clear();
    for (RememberedObject const& entry : entries)
        if (_store._entries.size() < _store._cap)
            _store.Put(entry);
    // The clock carries on from the save, plus the wall-clock time the process was down.
    double const gap = double(std::max<int64_t>(0, WallNow() - header.WallSeconds));
    _offset = newest + gap - now;
    _store._clock = Relative(now);
    _store._nextDecay = _store._clock + DECAY_EVERY_SECONDS;
    _savedRevision = _revision;
    return true;
}

bool Animus::Vision::SharedObjects::Save(std::string const& path, double now, std::string& error)
{
    std::vector<ObjectRecord> records;
    ObjectFileHeader header;
    uint64_t revision = 0;
    {
        std::lock_guard lock(_lock);
        std::vector<RememberedObject const*> lasting;
        for (RememberedObject const& entry : _store._entries)
            if (entry.Key.Lasting() && entry.Kind != ObjectKind::Player)
                lasting.push_back(&entry);
        // In identity order, so the same store writes the same bytes.
        std::sort(lasting.begin(), lasting.end(), [](RememberedObject const* a, RememberedObject const* b)
        {
            return a->Key < b->Key;
        });
        records.reserve(lasting.size());
        for (RememberedObject const* entry : lasting)
            records.push_back(RecordOf(*entry));
        header.Clock = std::max(Relative(now), _store._clock);
        revision = _revision;
    }
    header.MapId = _mapId;
    header.RecordBytes = uint32_t(sizeof(ObjectRecord));
    header.WallSeconds = WallNow();
    header.Count = uint32_t(records.size());
    header.PayloadChecksum = Fnv1a(records.data(), sizeof(ObjectRecord) * records.size(), FNV_OFFSET);

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
        if (!records.empty())
            out.write(reinterpret_cast<char const*>(records.data()),
                std::streamsize(sizeof(ObjectRecord) * records.size()));
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
