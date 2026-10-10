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

#include "CoverageBlock.h"
#include "MentalMap.h"
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
    constexpr char const* CHANNEL_NAMES[Animus::Curriculum::CoverageBlock::COVERAGE_CHANNELS] = { "known", "visited",
        "searched" };

    [[nodiscard]] int32 FloorDiv(int32 value, int32 by)
    {
        return value >= 0 ? value / by : -((-value + by - 1) / by);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::CoverageBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, 0 };
}

void Animus::Curriculum::CoverageBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    boost::json::object coverage;
    coverage["grid"] = COVERAGE_GRID;
    coverage["cell_yards"] = double(COVERAGE_CELL);
    coverage["channels"] = COVERAGE_CHANNELS;
    boost::json::array channels;
    for (char const* name : CHANNEL_NAMES)
        channels.push_back(boost::json::string(name));
    coverage["channel_names"] = std::move(channels);
    coverage["heading_up"] = true;
    coverage["scale"] = double(COVERAGE_SCALE);
    coverage["layout"] = "row_col_channel";
    block["coverage"] = std::move(coverage);
}

void Animus::Curriculum::CoverageBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    Vi::MentalMap const* map = view.Map;
    Player const* bot = view.Bot;
    if (!map || !bot || !bot->IsInWorld())
        return;

    // Where the body is, as the map block reads it: the controller's body, else the server's position.
    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    if (Movement::BodyState const* body = view.Body)
    {
        x = body->X;
        y = body->Y;
    }
    // Each output cell's centre, turned with the facing: forward along (cos yaw, sin yaw), right along
    // (sin yaw, -cos yaw), as the crop is (MentalMap::Crop).
    float const forwardX = std::cos(view.Facing);
    float const forwardY = std::sin(view.Facing);
    float const rightX = forwardY;
    float const rightY = -forwardX;
    float const half = float(COVERAGE_GRID) * 0.5f - 0.5f;
    for (uint32 row = 0; row < COVERAGE_GRID; ++row)
        for (uint32 col = 0; col < COVERAGE_GRID; ++col)
        {
            float const forward = (half - float(row)) * COVERAGE_CELL;
            float const right = (float(col) - half) * COVERAGE_CELL;
            float const px = x + forward * forwardX + right * rightX;
            float const py = y + forward * forwardY + right * rightY;
            int32 const tx = FloorDiv(int32(std::floor(px / Vi::MAP_CELL)), Vi::MAP_TILE);
            int32 const ty = FloorDiv(int32(std::floor(py / Vi::MAP_CELL)), Vi::MAP_TILE);
            uint16 known = 0;
            uint16 visited = 0;
            uint16 searched = 0;
            map->TileCounts(tx, ty, known, visited, searched);
            float* out = obs + (std::size_t(row) * COVERAGE_GRID + col) * COVERAGE_CHANNELS;
            out[0] = std::min(1.0f, float(known) / COVERAGE_SCALE);
            out[1] = std::min(1.0f, float(visited) / COVERAGE_SCALE);
            out[2] = std::min(1.0f, float(searched) / COVERAGE_SCALE);
        }
}
