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

#include "MemoryEntitiesBlock.h"
#include "EntitiesBlock.h"
#include "Layout.h"
#include "ObjectMemory.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <cmath>

namespace Vi = Animus::Vision;

namespace
{
    constexpr char const* ENTITY_NAMES[Animus::Curriculum::EntitiesBlock::ENTITY_FEATURES] = { "present", "class",
        "type", "object", "level", "level_delta", "health", "reaction", "quest", "lootable", "usable", "distance",
        "yaw_sin", "yaw_cos", "pitch_sin", "pitch_cos", "los", "ang_width", "ang_height", "memory" };
    constexpr char const* MEMORY_NAMES[Animus::Curriculum::MemoryEntitiesBlock::MEMORY_EXTRA] = { "age", "sightings",
        "absent", "kind_fixed", "kind_npc", "kind_creature", "kind_player", "open", "used", "dead", "body_sin",
        "body_cos", "body_distance", "height_delta", "trace_span" };
    static_assert(Vi::OBJECT_KINDS == 4, "the kind one-hot is four columns");
}

uint32 Animus::Curriculum::MemoryEntitiesBlock::Width()
{
    return uint32(EntitiesBlock::ENTITY_FEATURES) + uint32(MEMORY_EXTRA);
}

Animus::Curriculum::BlockSize Animus::Curriculum::MemoryEntitiesBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ MEMORY_SLOTS * Width(), 0 };
}

void Animus::Curriculum::MemoryEntitiesBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    boost::json::object described;
    described["name"] = "remembered";
    described["slots"] = MEMORY_SLOTS;
    described["width"] = Width();
    described["base_width"] = uint32(EntitiesBlock::ENTITY_FEATURES);
    described["first"] = layout.Has(BlockId::MemoryEntities) ? layout.Slice(BlockId::MemoryEntities).ObsFirst : 0;
    described["present"] = uint32(EntitiesBlock::ENTITY_PRESENT);
    described["class_column"] = uint32(EntitiesBlock::ENTITY_CLASS);
    described["type_column"] = uint32(EntitiesBlock::ENTITY_TYPE);
    described["object_column"] = uint32(EntitiesBlock::ENTITY_OBJECT);
    described["memory_column"] = uint32(EntitiesBlock::ENTITY_MEMORY);
    described["classes"] = Vi::CLASS_LIMIT;
    described["type_buckets"] = EntitiesBlock::TYPE_BUCKETS;
    boost::json::array kinds;
    for (char const* name : Vi::OBJECT_KIND_NAMES)
        kinds.push_back(boost::json::string(name));
    described["kinds"] = std::move(kinds);
    described["age_scale_s"] = AGE_SCALE_S;
    described["sightings_log_max"] = SIGHTINGS_LOG_MAX;
    described["distance_log_max"] = DISTANCE_LOG_MAX;
    described["height_scale"] = HEIGHT_SCALE;
    described["trace_scale"] = TRACE_SCALE;
    described["missed_looks"] = uint32(Vi::MISSED_LOOKS);
    boost::json::array names;
    for (char const* name : ENTITY_NAMES)
        names.push_back(boost::json::string(name));
    for (char const* name : MEMORY_NAMES)
        names.push_back(boost::json::string(name));
    described["features"] = std::move(names);
    block["memory_entities"] = std::move(described);
}

void Animus::Curriculum::MemoryEntitiesBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::fill(obs, obs + MEMORY_SLOTS * Width(), 0.0f);
    Vi::ObjectMemory const* memory = view.Objects;
    if (!view.Seen || !memory || view.ObjectsHidden || !memory->Count())
        return;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (Movement::BodyState const* body = view.Body)
    {
        x = body->X;
        y = body->Y;
        z = body->Z;
    }
    else if (Player const* bot = view.Bot)
    {
        x = bot->GetPositionX();
        y = bot->GetPositionY();
        z = bot->GetPositionZ();
    }
    else
        return;
    Vi::RememberedObject const* recalled[MEMORY_SLOTS];
    uint32 const count = memory->Recall(Vi::Vec3{ x, y, z }, recalled, MEMORY_SLOTS);
    for (uint32 slot = 0; slot < count; ++slot)
        WriteSlot(*view.Seen, *recalled[slot], memory->AgeOf(*recalled[slot]), x, y, z, view.Facing,
            obs + slot * Width());
}

void Animus::Curriculum::MemoryEntitiesBlock::WriteSlot(Vision::SeenList const& seen,
    Vision::RememberedObject const& entry, double age, float feetX, float feetY, float feetZ, float facing, float* out)
{
    // The entity list's columns, from this frame's camera, through the list's own writer: a one-entity list.
    thread_local Vi::SeenList one;
    one.Count = 1;
    one.Camera = seen.Camera;
    one.Azimuth = seen.Azimuth;
    one.Elevation = seen.Elevation;
    one.SeatLevel = seen.SeatLevel;
    Vi::EntityInfo& info = one.Info[0];
    info = Vi::EntityInfo();
    info.Id.What = entry.What;
    info.Id.Quest = entry.Quest;
    info.Id.Lootable = entry.Lootable;
    info.Id.Usable = entry.Usable;
    info.Entry = entry.Entry;
    info.GameObject = entry.GameObject;
    info.Level = entry.Level;
    info.Health = entry.Health;
    info.Reaction = entry.Reaction;
    info.Centre = entry.Position;
    info.Radius = entry.Radius;
    info.Height = entry.Height;
    info.Los = 0.0f;
    EntitiesBlock::WriteSlot(one, 0, 0, out);

    float* extra = out + EntitiesBlock::ENTITY_FEATURES;
    extra[MEMORY_AGE] = std::clamp(float(std::log2(1.0 + std::max(0.0, age)) / std::log2(1.0 + AGE_SCALE_S)), 0.0f,
        1.0f);
    extra[MEMORY_SIGHTINGS] = std::clamp(std::log1p(float(entry.Sightings)) / std::log1p(SIGHTINGS_LOG_MAX), 0.0f,
        1.0f);
    extra[MEMORY_ABSENT] = entry.Absent ? 1.0f : 0.0f;
    extra[MEMORY_KIND_FIRST + std::min<uint32>(uint32(entry.Kind), Vi::OBJECT_KINDS - 1)] = 1.0f;
    extra[MEMORY_OPEN] = entry.Open ? 1.0f : 0.0f;
    extra[MEMORY_USED] = entry.Used ? 1.0f : 0.0f;
    extra[MEMORY_DEAD] = entry.Dead ? 1.0f : 0.0f;
    float const dx = entry.Position.X - feetX;
    float const dy = entry.Position.Y - feetY;
    float const flat = std::sqrt(dx * dx + dy * dy);
    if (flat > 1e-3f)
    {
        float const bearing = std::atan2(dy, dx) - facing;
        extra[MEMORY_BODY_SIN] = std::sin(bearing);
        extra[MEMORY_BODY_COS] = std::cos(bearing);
    }
    extra[MEMORY_BODY_DIST] = std::clamp(std::log1p(flat) / std::log1p(DISTANCE_LOG_MAX), 0.0f, 1.0f);
    extra[MEMORY_HEIGHT] = std::clamp((entry.Position.Z - feetZ) / HEIGHT_SCALE, -1.0f, 1.0f);
    extra[MEMORY_TRACE_SPAN] = std::clamp(entry.TraceSpan() / TRACE_SCALE, 0.0f, 1.0f);
}
