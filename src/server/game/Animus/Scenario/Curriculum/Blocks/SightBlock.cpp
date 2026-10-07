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

#include "SightBlock.h"
#include "CombatBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "Player.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <cmath>

namespace Vi = Animus::Vision;

namespace
{
    using Sight = Animus::Curriculum::SightBlock;
    using Entities = Animus::Curriculum::EntitiesBlock;

    constexpr uint32 EXTRA_FEATURES = uint32(Sight::SIGHT_FEATURES) - uint32(Entities::ENTITY_FEATURES);
    constexpr char const* EXTRA_NAMES[EXTRA_FEATURES] = { "visible", "age", "dead", "open", "used", "heading_sin",
        "heading_cos", "speed", "course_sin", "course_cos", "selected", "focused" };
    constexpr char const* ENTITY_NAMES[Entities::ENTITY_FEATURES] = { "present", "class", "type", "object", "level",
        "level_delta", "health", "reaction", "quest", "lootable", "usable", "distance", "yaw_sin", "yaw_cos",
        "pitch_sin", "pitch_cos", "centroid_x", "centroid_y", "share", "memory" };
    constexpr char const* PRESS_NAMES[uint32(Animus::Curriculum::EntityActions::Press::Count)] = { "select",
        "interact", "use_item", "assist", "focus" };

    /// A remembered entity's entity-list columns, from the camera of this frame to where it was last seen.
    void WriteRecalled(Vi::SeenList const& seen, Vi::Remembered const& entry, float* out)
    {
        float const logRange = std::log(Vi::DISTANCE_REFERENCE / Vi::NEAR);
        out[Entities::ENTITY_PRESENT] = 1.0f;
        out[Entities::ENTITY_CLASS] = float(uint32(entry.Id.What));
        out[Entities::ENTITY_TYPE] = float(entry.Entry);
        out[Entities::ENTITY_OBJECT] = entry.GameObject ? 1.0f : 0.0f;
        out[Entities::ENTITY_LEVEL] = entry.Level / Entities::LEVEL_SCALE;
        out[Entities::ENTITY_LEVEL_DELTA] = entry.GameObject ? 0.0f
            : std::clamp((entry.Level - seen.SeatLevel) / Entities::LEVEL_DELTA_SCALE, -1.0f, 1.0f);
        out[Entities::ENTITY_HEALTH] = std::clamp(entry.Health, 0.0f, 1.0f);
        out[Entities::ENTITY_REACTION] = float(entry.Reaction);
        out[Entities::ENTITY_QUEST] = entry.Id.Quest ? 1.0f : 0.0f;
        out[Entities::ENTITY_LOOTABLE] = entry.Id.Lootable ? 1.0f : 0.0f;
        out[Entities::ENTITY_USABLE] = entry.Id.Usable ? 1.0f : 0.0f;
        Vi::Vec3 const offset = entry.Position - seen.Camera;
        float const flat = std::sqrt(offset.X * offset.X + offset.Y * offset.Y);
        float const distance = Vi::Length(offset);
        out[Entities::ENTITY_DISTANCE] = std::clamp(std::log(std::max(distance, Vi::NEAR) / Vi::NEAR) / logRange,
            0.0f, 1.0f);
        float const yaw = std::atan2(offset.Y, offset.X) - seen.Azimuth;
        float const pitch = std::atan2(offset.Z, flat) - seen.Elevation;
        out[Entities::ENTITY_YAW_SIN] = std::sin(yaw);
        out[Entities::ENTITY_YAW_COS] = std::cos(yaw);
        out[Entities::ENTITY_PITCH_SIN] = std::sin(pitch);
        out[Entities::ENTITY_PITCH_COS] = std::cos(pitch);
        // No pixels: the centroid and the share stay 0.
        out[Entities::ENTITY_MEMORY] = float(entry.MemoryId);
    }

    /// The memory's columns of a slot.
    void WriteMemory(Vi::SeenList const& seen, Vi::EntityMemory const& memory, Vi::Remembered const& entry,
        bool visible, uint64 selected, uint64 focus, float* out)
    {
        out[Sight::SIGHT_VISIBLE] = visible ? 1.0f : 0.0f;
        float const age = visible ? 0.0f : std::max(0.0f, memory.AgeOf(entry));
        out[Sight::SIGHT_AGE] = std::min(1.0f, std::log2(1.0f + age) / std::log2(1.0f + Sight::AGE_SCALE));
        out[Sight::SIGHT_DEAD] = entry.Dead ? 1.0f : 0.0f;
        out[Sight::SIGHT_OPEN] = entry.Open ? 1.0f : 0.0f;
        out[Sight::SIGHT_USED] = entry.Used ? 1.0f : 0.0f;
        float const heading = entry.Heading - seen.Azimuth;
        out[Sight::SIGHT_HEADING_SIN] = std::sin(heading);
        out[Sight::SIGHT_HEADING_COS] = std::cos(heading);
        float const speed = std::sqrt(entry.Velocity.X * entry.Velocity.X + entry.Velocity.Y * entry.Velocity.Y);
        out[Sight::SIGHT_SPEED] = entry.Moving ? std::min(2.0f, speed / Sight::SPEED_SCALE) : 0.0f;
        if (entry.Moving)
        {
            float const course = std::atan2(entry.Velocity.Y, entry.Velocity.X) - seen.Azimuth;
            out[Sight::SIGHT_COURSE_SIN] = std::sin(course);
            out[Sight::SIGHT_COURSE_COS] = std::cos(course);
        }
        out[Sight::SIGHT_SELECTED] = selected && entry.Guid == selected ? 1.0f : 0.0f;
        out[Sight::SIGHT_FOCUSED] = focus && entry.Guid == focus ? 1.0f : 0.0f;
    }
}

uint32 Animus::Curriculum::SightBlock::Width(Layout const& layout)
{
    // Read off the layout's block list, not Has: Layout::Build sizes this block before it reaches the combat block
    // after it, when Has does not know it yet.
    bool const combat = std::find(layout.Blocks.begin(), layout.Blocks.end(), BlockId::Combat) != layout.Blocks.end();
    return SIGHT_FEATURES + (combat ? uint32(CombatBlock::COMBAT_SLOT_FEATURES) : 0);
}

Animus::Curriculum::BlockSize Animus::Curriculum::SightBlock::Size(Layout const& layout) const
{
    return BlockSize{ SIGHT_SLOTS * Width(layout), ACTION_COUNT };
}

void Animus::Curriculum::SightBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    bool const has = layout.Has(BlockId::Sight);
    boost::json::object sight;
    sight["name"] = "sight";
    sight["slots"] = SIGHT_SLOTS;
    sight["visible_slots"] = SIGHT_VISIBLE_SLOTS;
    sight["recalled_slots"] = SIGHT_RECALLED_SLOTS;
    sight["width"] = Width(layout);
    sight["first"] = has ? layout.Slice(BlockId::Sight).ObsFirst : 0;
    sight["present"] = uint32(Entities::ENTITY_PRESENT);
    sight["class_column"] = uint32(Entities::ENTITY_CLASS);
    sight["type_column"] = uint32(Entities::ENTITY_TYPE);
    sight["object_column"] = uint32(Entities::ENTITY_OBJECT);
    sight["memory_column"] = uint32(Entities::ENTITY_MEMORY);
    sight["visible_column"] = uint32(SIGHT_VISIBLE);
    sight["classes"] = Vi::CLASS_LIMIT;
    sight["type_buckets"] = Entities::TYPE_BUCKETS;
    sight["memory_ids"] = Vi::MEMORY_TRAINING_CAP;
    boost::json::array names;
    for (char const* name : ENTITY_NAMES)
        names.push_back(boost::json::string(name));
    for (char const* name : EXTRA_NAMES)
        names.push_back(boost::json::string(name));
    // The combat block's per-target columns after the sight block's own, in a layout with one (I3).
    if (Width(layout) > SIGHT_FEATURES)
    {
        boost::json::object combat;
        GetBlock(BlockId::Combat).DescribeManifest(layout, combat);
        for (boost::json::value const& name : combat["slot_features"].as_array())
            names.push_back(boost::json::string("combat_" + std::string(name.as_string())));
    }
    sight["features"] = std::move(names);
    boost::json::array pointers;
    uint32 const actions = has ? layout.Slice(BlockId::Sight).ActionFirst : 0;
    for (uint32 press = 0; press < uint32(EntityActions::Press::Count); ++press)
    {
        boost::json::object pointer;
        pointer["press"] = PRESS_NAMES[press];
        pointer["first"] = actions + press * SIGHT_SLOTS;
        pointer["count"] = SIGHT_SLOTS;
        pointers.emplace_back(std::move(pointer));
    }
    sight["pointers"] = std::move(pointers);
    block["sight"] = std::move(sight);
}

void Animus::Curriculum::SightBlock::Write(Vision::SeenList const& seen, Vision::EntityMemory const& memory,
    uint64 selected, uint64 focus, float* obs, std::array<uint64, SIGHT_SLOTS>& guids, uint32 width)
{
    std::fill(obs, obs + SIGHT_SLOTS * width, 0.0f);
    guids.fill(0);

    // The visible half, in the frame's slot order.
    uint32 const visible = std::min<uint32>(seen.Count, SIGHT_VISIBLE_SLOTS);
    for (uint32 slot = 0; slot < visible; ++slot)
    {
        Vi::EntityInfo const& info = seen.Info[slot];
        float* out = obs + slot * width;
        Vi::Remembered const* entry = memory.Find(info.Guid);
        Entities::WriteSlot(seen, slot, entry ? entry->MemoryId : 0, out);
        guids[slot] = info.Guid;
        if (entry)
            WriteMemory(seen, memory, *entry, true, selected, focus, out);
        else
        {
            // Not kept (a memory too small for the frame): what the frame shows of it.
            Vi::Remembered shown;
            shown.Guid = info.Guid;
            shown.Dead = info.Dead;
            shown.Open = info.Open;
            shown.Used = info.Used;
            shown.Heading = info.Orientation;
            WriteMemory(seen, memory, shown, true, selected, focus, out);
        }
    }

    // The remembered half: the most relevant the frame did not show, from where the camera is.
    std::array<Vi::Remembered const*, SIGHT_RECALLED_SLOTS> recalled{};
    uint32 const count = memory.Recall(seen.Camera, recalled.data(), SIGHT_RECALLED_SLOTS);
    for (uint32 i = 0; i < count; ++i)
    {
        uint32 const slot = SIGHT_VISIBLE_SLOTS + i;
        float* out = obs + slot * width;
        WriteRecalled(seen, *recalled[i], out);
        WriteMemory(seen, memory, *recalled[i], false, selected, focus, out);
        guids[slot] = recalled[i]->Guid;
    }
}

void Animus::Curriculum::SightBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    uint32 const width = view.L ? Width(*view.L) : uint32(SIGHT_FEATURES);
    std::fill(obs, obs + SIGHT_SLOTS * width, 0.0f);
    if (mask)
        std::fill(mask, mask + ACTION_COUNT, uint8(0));
    if (view.SightGuids)
        view.SightGuids->fill(0);
    Player* bot = view.Bot;
    if (!view.Seen || !view.Recall || !view.SightGuids || !bot || !bot->IsInWorld())
        return;

    uint64 const selected = bot->GetTarget().GetRawValue();
    uint64 const focus = view.Focus ? view.Focus->GetRawValue() : 0;
    Write(*view.Seen, *view.Recall, selected, focus, obs, *view.SightGuids, width);

    // The combat block's columns (I3): what each visible unit's nameplate shows of the fight, read off the unit the
    // frame showed; a remembered slot keeps them 0.
    if (width > SIGHT_FEATURES)
        for (uint32 slot = 0; slot < SIGHT_VISIBLE_SLOTS; ++slot)
        {
            uint64 const guid = (*view.SightGuids)[slot];
            float* out = obs + slot * width;
            if (!guid || out[Entities::ENTITY_OBJECT] > 0.5f)
                continue;
            if (Unit* unit = Encoding::UnitThrough(*bot, ObjectGuid(guid)); unit && unit->IsInWorld()
                && unit->GetMap() == bot->GetMap())
                CombatBlock::WriteSlot(unit, bot, out + SIGHT_FEATURES);
        }
    if (!mask)
        return;

    // A slot with nothing in it cannot be pressed, and a game object cannot be targeted; nothing else is masked.
    for (uint32 slot = 0; slot < SIGHT_SLOTS; ++slot)
    {
        if (!(*view.SightGuids)[slot])
            continue;
        bool const object = obs[slot * width + Entities::ENTITY_OBJECT] > 0.5f;
        mask[ACTION_INTERACT_FIRST + slot] = 1;
        mask[ACTION_USE_ITEM_FIRST + slot] = 1;
        mask[ACTION_SELECT_FIRST + slot] = object ? 0 : 1;
        mask[ACTION_ASSIST_FIRST + slot] = object ? 0 : 1;
        mask[ACTION_FOCUS_FIRST + slot] = object ? 0 : 1;
    }
}

void Animus::Curriculum::SightBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (local >= ACTION_COUNT || !view.SightGuids || !view.Bot)
        return;
    uint64 const guid = (*view.SightGuids)[SlotOf(local)];
    if (!guid)
        return;
    ObjectGuid scratch;
    ObjectGuid& focus = view.Focus ? *view.Focus : scratch;
    EntityActions::ClientPort& port = view.Port ? *view.Port : EntityActions::SessionPort();
    EntityActions::Apply(PressOf(local), view.Bot, guid, focus, result, port);
}

std::string Animus::Curriculum::SightBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    if (local >= ACTION_COUNT)
        return {};
    return Acore::StringFormat("{}_{}", PRESS_NAMES[local / SIGHT_SLOTS], SlotOf(local));
}
