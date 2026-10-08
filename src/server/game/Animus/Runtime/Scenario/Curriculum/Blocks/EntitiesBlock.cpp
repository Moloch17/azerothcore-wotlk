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

#include "EntitiesBlock.h"
#include "Layout.h"
#include "SeatView.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <cmath>

namespace Vi = Animus::Vision;

namespace
{
    constexpr char const* FEATURE_NAMES[Animus::Curriculum::EntitiesBlock::ENTITY_FEATURES] = { "present", "class",
        "type", "object", "level", "level_delta", "health", "reaction", "quest", "lootable", "usable", "distance",
        "yaw_sin", "yaw_cos", "pitch_sin", "pitch_cos", "los", "ang_width", "ang_height", "memory" };
}

Animus::Curriculum::BlockSize Animus::Curriculum::EntitiesBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ Vi::ENTITY_SLOTS * ENTITY_FEATURES, 0 };
}

void Animus::Curriculum::EntitiesBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    boost::json::object entities;
    entities["name"] = "visible";
    entities["slots"] = Vi::ENTITY_SLOTS;
    entities["width"] = uint32(ENTITY_FEATURES);
    entities["first"] = layout.Has(BlockId::Entities) ? layout.Slice(BlockId::Entities).ObsFirst : 0;
    entities["present"] = uint32(ENTITY_PRESENT);
    entities["class_column"] = uint32(ENTITY_CLASS);
    entities["type_column"] = uint32(ENTITY_TYPE);
    entities["classes"] = Vi::CLASS_LIMIT;
    entities["type_buckets"] = TYPE_BUCKETS;
    boost::json::array names;
    for (char const* name : FEATURE_NAMES)
        names.push_back(boost::json::string(name));
    entities["features"] = std::move(names);
    block["entities"] = std::move(entities);
}

void Animus::Curriculum::EntitiesBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    if (!view.Seen)
    {
        std::fill(obs, obs + Vi::ENTITY_SLOTS * ENTITY_FEATURES, 0.0f);
        return;
    }
    // Entity memory's one write (dungeon-curriculum I2): its clock is the seat's decisions, then this frame's list.
    if (Vi::EntityMemory* memory = view.Recall)
    {
        memory->Advance(float(view.DecisionMs) / 1000.0f);
        memory->Write(*view.Seen);
    }
    Write(*view.Seen, obs, view.Recall);
}

void Animus::Curriculum::EntitiesBlock::Write(Vision::SeenList const& seen, float* obs,
    Vision::EntityMemory const* memory)
{
    std::fill(obs, obs + Vi::ENTITY_SLOTS * ENTITY_FEATURES, 0.0f);
    for (uint32 slot = 0; slot < seen.Count && slot < Vi::ENTITY_SLOTS; ++slot)
        WriteSlot(seen, slot, memory ? memory->IdOf(seen.Info[slot].Guid) : 0, obs + slot * ENTITY_FEATURES);
}

void Animus::Curriculum::EntitiesBlock::WriteSlot(Vision::SeenList const& seen, uint32 slot, uint16 memoryId,
    float* out)
{
    float const logRange = std::log(Vi::DISTANCE_REFERENCE / Vi::NEAR);
    Vi::EntityInfo const& info = seen.Info[slot];
    out[ENTITY_PRESENT] = 1.0f;
    out[ENTITY_CLASS] = float(uint32(info.Id.What));
    out[ENTITY_TYPE] = float(info.Entry);
    out[ENTITY_OBJECT] = info.GameObject ? 1.0f : 0.0f;
    out[ENTITY_LEVEL] = info.Level / LEVEL_SCALE;
    out[ENTITY_LEVEL_DELTA] = info.GameObject ? 0.0f
        : std::clamp((info.Level - seen.SeatLevel) / LEVEL_DELTA_SCALE, -1.0f, 1.0f);
    out[ENTITY_HEALTH] = std::clamp(info.Health, 0.0f, 1.0f);
    out[ENTITY_REACTION] = float(info.Reaction);
    out[ENTITY_QUEST] = info.Id.Quest ? 1.0f : 0.0f;
    out[ENTITY_LOOTABLE] = info.Id.Lootable ? 1.0f : 0.0f;
    out[ENTITY_USABLE] = info.Id.Usable ? 1.0f : 0.0f;

    // Where it is from the camera, in the view's own frame: the yaw off the view's azimuth (+ left, WoW's) and
    // the pitch off its elevation.
    Vi::Vec3 const offset = info.Centre - seen.Camera;
    float const flat = std::sqrt(offset.X * offset.X + offset.Y * offset.Y);
    float const distance = Vi::Length(offset);
    out[ENTITY_DISTANCE] = std::clamp(std::log(std::max(distance, Vi::NEAR) / Vi::NEAR) / logRange, 0.0f, 1.0f);
    float const yaw = std::atan2(offset.Y, offset.X) - seen.Azimuth;
    float const pitch = std::atan2(offset.Z, flat) - seen.Elevation;
    out[ENTITY_YAW_SIN] = std::sin(yaw);
    out[ENTITY_YAW_COS] = std::cos(yaw);
    out[ENTITY_PITCH_SIN] = std::sin(pitch);
    out[ENTITY_PITCH_COS] = std::cos(pitch);

    out[ENTITY_LOS] = std::clamp(info.Los, 0.0f, 1.0f);
    AngularSize(distance, info.Radius, info.Height, out[ENTITY_ANG_WIDTH], out[ENTITY_ANG_HEIGHT]);
    out[ENTITY_MEMORY] = float(memoryId);
}

void Animus::Curriculum::EntitiesBlock::AngularSize(float distance, float radius, float height, float& width,
    float& tall)
{
    Vi::Settings const& settings = Vi::Current();
    float const away = std::max(distance, 0.1f);
    float const fovH = std::max(settings.FovH * Vi::DEGREES, 1e-3f);
    float const fovV = std::max(settings.FovV * Vi::DEGREES, 1e-3f);
    width = std::clamp(2.0f * std::atan(std::max(radius, 0.0f) / away) / fovH, 0.0f, 1.0f);
    tall = std::clamp(2.0f * std::atan(0.5f * std::max(height, 0.0f) / away) / fovV, 0.0f, 1.0f);
}

void Animus::Curriculum::EntitiesBlock::ReadMarks(float const* row, std::vector<Vision::EntityMark>& out)
{
    out.clear();
    for (uint32 slot = 0; slot < Vi::ENTITY_SLOTS; ++slot)
    {
        float const* in = row + slot * ENTITY_FEATURES;
        if (in[ENTITY_PRESENT] < 0.5f)
            continue;
        Vi::EntityMark mark;
        mark.What = Vi::Class(std::clamp<uint32>(uint32(std::lround(in[ENTITY_CLASS])), 0, Vi::CLASSES - 1));
        mark.Yaw = std::atan2(in[ENTITY_YAW_SIN], in[ENTITY_YAW_COS]);
        mark.Pitch = std::atan2(in[ENTITY_PITCH_SIN], in[ENTITY_PITCH_COS]);
        mark.Width = in[ENTITY_ANG_WIDTH];
        mark.Height = in[ENTITY_ANG_HEIGHT];
        mark.Los = in[ENTITY_LOS];
        out.push_back(mark);
    }
}
