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

#include "MapBlock.h"
#include "Player.h"
#include "SeatView.h"
#include "UnitBody.h"
#include "VisionCost.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <chrono>
#include <cstring>
#include <vector>

namespace Vi = Animus::Vision;

namespace
{
    constexpr char const* SCALAR_NAMES[Animus::Curriculum::MapBlock::OBS_COUNT] = { "known", "frontier", "visited",
        "kept" };
}

Animus::Curriculum::BlockSize Animus::Curriculum::MapBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, 0 };
}

void Animus::Curriculum::MapBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    boost::json::object map;
    map["transport"] = "bytes";
    map["height"] = Vi::CROP;
    map["width"] = Vi::CROP;
    map["cell"] = double(Vi::CROP_CELL);
    map["heading_up"] = true;
    map["channels"] = Vi::CROP_CHANNELS;
    boost::json::array channels;
    for (char const* name : Vi::CROP_CHANNEL_NAMES)
        channels.push_back(boost::json::string(name));
    map["channel_names"] = std::move(channels);
    map["map_bytes"] = Vi::CROP_BYTES;
    map["codes"] = Vi::MAP_CODES;
    boost::json::array codes;
    for (char const* name : Vi::MAP_CODE_NAMES)
        codes.push_back(boost::json::string(name));
    map["code_names"] = std::move(codes);
    map["code_channel"] = uint32(Vi::CROP_CODE);
    map["height_channel"] = uint32(Vi::CROP_HEIGHT);
    map["height_step"] = double(Vi::CROP_HEIGHT_STEP);
    map["height_zero"] = uint32(Vi::CROP_HEIGHT_ZERO);
    map["visited_channel"] = uint32(Vi::CROP_VISITED);
    map["age_channel"] = uint32(Vi::CROP_AGE);
    map["age_scale"] = double(Vi::CROP_AGE_SCALE);
    map["age_never"] = uint32(Vi::CROP_AGE_NEVER);
    map["class_channel"] = uint32(Vi::CROP_CLASS);
    map["classes"] = Vi::CLASS_LIMIT;
    map["frontier_channel"] = uint32(Vi::CROP_FRONTIER);
    map["scalars"] = uint32(OBS_COUNT);
    boost::json::array scalars;
    for (char const* name : SCALAR_NAMES)
        scalars.push_back(boost::json::string(name));
    map["scalar_names"] = std::move(scalars);
    block["map"] = std::move(map);
}

void Animus::Curriculum::MapBlock::Scalars(uint8 const* crop, bool kept, float* obs)
{
    uint32 known = 0;
    uint32 frontier = 0;
    uint32 visited = 0;
    for (uint32 cell = 0; cell < Vi::CROP * Vi::CROP; ++cell)
    {
        uint8 const* bytes = crop + std::size_t(cell) * Vi::CROP_CHANNELS;
        known += bytes[Vi::CROP_AGE] != Vi::CROP_AGE_NEVER ? 1 : 0;
        frontier += bytes[Vi::CROP_FRONTIER] ? 1 : 0;
        visited += bytes[Vi::CROP_VISITED] ? 1 : 0;
    }
    float const cells = float(Vi::CROP * Vi::CROP);
    obs[OBS_KNOWN] = float(known) / cells;
    obs[OBS_FRONTIER] = float(frontier) / cells;
    obs[OBS_VISITED] = float(visited) / cells;
    obs[OBS_KEPT] = kept ? 1.0f : 0.0f;
}

void Animus::Curriculum::MapBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    // No map until this decision's says otherwise: every cell unknown (the zero row).
    if (view.MapRow)
        std::memset(view.MapRow, 0, Vi::CROP_BYTES);
    Vi::MentalMap* map = view.Map;
    Player* bot = view.Bot;
    if (!map || !bot || !bot->IsInWorld())
        return;

    auto const start = std::chrono::steady_clock::now();
    // The seat's own view of itself, as the camera takes it: the controller's body, else the server's position.
    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float z = bot->GetPositionZ();
    bool grounded = !bot->IsInWater() && !bot->IsFalling() && !bot->IsFlying();
    if (Movement::BodyState const* body = view.Body)
    {
        x = body->X;
        y = body->Y;
        z = body->Z;
        grounded = body->Kind == Movement::Mode::Ground;
    }
    float const bodyHeight = Movement::ShapeOf(bot).Height;

    // The map's clock is the seat's decisions; then this decision's frame (as cast) and the body are written.
    map->Advance(float(view.DecisionMs) / 1000.0f);
    if (view.Hits && !view.Hits->Rays.empty())
        map->WriteFrame(*view.Hits, z, bodyHeight);
    map->WriteBody(x, y, z, grounded);

    thread_local std::vector<uint8> scratch(Vi::CROP_BYTES);
    uint8* crop = view.MapRow ? view.MapRow : scratch.data();
    map->Crop(x, y, z, view.Facing, crop);
    Scalars(crop, view.MapKept, obs);
    Vi::Cost::AddMap(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
        - start).count()), map->Tiles());
}
