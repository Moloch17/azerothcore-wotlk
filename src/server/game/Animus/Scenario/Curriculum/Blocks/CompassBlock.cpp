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

#include "CompassBlock.h"
#include "Player.h"
#include "SeatView.h"
#include "UnitBody.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <algorithm>
#include <cmath>

Animus::Curriculum::BlockSize Animus::Curriculum::CompassBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, 0 };
}

void Animus::Curriculum::CompassBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["objective_scale"] = double(OBJECTIVE_SCALE);
    block["near_scale"] = double(NEAR_SCALE);
}

char const* Animus::Curriculum::CompassBlock::ColumnName(uint32 column)
{
    switch (column)
    {
        case OBS_OBJECTIVE:             return "objective";
        case OBS_OBJECTIVE_BEARING_SIN: return "objective_bearing_sin";
        case OBS_OBJECTIVE_BEARING_COS: return "objective_bearing_cos";
        case OBS_OBJECTIVE_DISTANCE:    return "objective_distance";
        case OBS_OBJECTIVE_NEAR:        return "objective_near";
        case OBS_DETOUR:                return "detour";
        default:                        return "";
    }
}

void Animus::Curriculum::CompassBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    for (uint32 column = 0; column < OBS_COUNT; ++column)
        names.emplace_back(ColumnName(column));
}

void Animus::Curriculum::CompassBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    // Withheld this episode (M1's withholding ladder): absent, every column 0 as if the block read nothing at all.
    // An input gone, never a mask: the actions are what they were.
    if (view.CompassWithheld)
    {
        std::fill(obs, obs + OBS_COUNT, 0.0f);
        return;
    }

    // The detour is the scenario's to measure (the travel encounter, at the episode's build): 0 without one.
    obs[OBS_DETOUR] = std::clamp(view.Detour / 4.0f, 0.0f, 1.0f);

    Player* bot = view.Bot;
    if (!bot || !view.HasObjective)
        return;

    // From where the seat's body is, as the move block reads it (a client knows its own position), and off the
    // facing the move block's bearings are measured from.
    Movement::BodyState const* body = view.Body;
    Position const self = body ? Position(body->X, body->Y, body->Z, body->Yaw) : bot->GetPosition();
    float const angle = self.GetAngle(view.Objective.GetPositionX(), view.Objective.GetPositionY()) - view.Facing;
    float const relative = std::atan2(std::sin(angle), std::cos(angle));
    float const range = self.GetExactDist2d(&view.Objective);
    obs[OBS_OBJECTIVE] = 1.0f;
    obs[OBS_OBJECTIVE_BEARING_SIN] = std::sin(relative);
    obs[OBS_OBJECTIVE_BEARING_COS] = std::cos(relative);
    obs[OBS_OBJECTIVE_DISTANCE] = std::min(1.0f, range / OBJECTIVE_SCALE);
    obs[OBS_OBJECTIVE_NEAR] = std::min(1.0f, range / NEAR_SCALE);
}
