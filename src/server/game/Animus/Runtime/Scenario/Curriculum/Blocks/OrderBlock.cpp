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

#include "OrderBlock.h"
#include "Player.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <boost/json/array.hpp>
#include <algorithm>
#include <cmath>

Animus::Curriculum::BlockSize Animus::Curriculum::OrderBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, 0 };
}

std::string Animus::Curriculum::OrderBlock::ColumnName(uint32 column)
{
    if (column >= OBS_POSTURE_FIRST && column < OBS_POSTURE_FIRST + TEAM_POSTURE_COUNT)
        return Acore::StringFormat("posture_{}", PostureName(TeamPosture(column - OBS_POSTURE_FIRST)));
    if (column >= OBS_KIND_FIRST && column < OBS_KIND_FIRST + ORDER_KIND_COUNT)
        return Acore::StringFormat("kind_{}", OrderKindName(OrderKind(column - OBS_KIND_FIRST)));
    switch (column)
    {
        case OBS_ACTIVE:                return "active";
        case OBS_TARGET:                return "target";
        case OBS_TARGET_UNSEEN:         return "target_unseen";
        case OBS_TARGET_DISTANCE:       return "target_distance";
        case OBS_TARGET_NEAR:           return "target_near";
        case OBS_TARGET_SIN:            return "target_sin";
        case OBS_TARGET_COS:            return "target_cos";
        case OBS_TARGET_HEALTH:         return "target_health";
        case OBS_TARGET_OPEN:           return "target_open";
        case OBS_TARGET_IS_SELECTION:   return "target_is_selection";
        case OBS_TARGET_IN_REACH:       return "target_in_reach";
        case OBS_NEEDS_KEY:             return "needs_key";
        case OBS_CARRIES_KEY:           return "carries_key";
        case OBS_AGE:                   return "age";
        case OBS_DONE_NOW:              return "done_now";
        default:                        return "unknown";
    }
}

void Animus::Curriculum::OrderBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    for (uint32 column = 0; column < OBS_COUNT; ++column)
        names.emplace_back(ColumnName(column));
}

void Animus::Curriculum::OrderBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    SeatView::TeamOrder const& order = view.Order;
    if (!order.Active)
        return;
    obs[OBS_ACTIVE] = 1.0f;
    obs[OBS_POSTURE_FIRST + std::min<uint32>(uint32(order.Posture), TEAM_POSTURE_COUNT - 1)] = 1.0f;
    obs[OBS_KIND_FIRST + std::min<uint32>(uint32(order.Kind), ORDER_KIND_COUNT - 1)] = 1.0f;
    if (order.Kind == OrderKind::None)
        return;
    obs[OBS_AGE] = std::min(1.0f, order.AgeSeconds / AGE_SCALE);
    obs[OBS_DONE_NOW] = order.DoneNow ? 1.0f : 0.0f;
    obs[OBS_NEEDS_KEY] = order.NeedsKey ? 1.0f : 0.0f;
    obs[OBS_CARRIES_KEY] = order.CarriesKey ? 1.0f : 0.0f;
    if (!order.HasTarget)
        return;
    obs[OBS_TARGET] = 1.0f;
    obs[OBS_TARGET_HEALTH] = order.TargetHealth;
    obs[OBS_TARGET_OPEN] = order.TargetOpen ? 1.0f : 0.0f;
    obs[OBS_TARGET_IS_SELECTION] = order.TargetIsSelection ? 1.0f : 0.0f;
    obs[OBS_TARGET_IN_REACH] = order.TargetNear ? 1.0f : 0.0f;
    if (!order.TargetKnown || !view.Bot)
    {
        obs[OBS_TARGET_UNSEEN] = 1.0f;
        return;
    }
    // From where the seat's body is and off the facing the move block's bearings are measured from (CompassBlock).
    Movement::BodyState const* body = view.Body;
    Position const self = body ? Position(body->X, body->Y, body->Z, body->Yaw) : view.Bot->GetPosition();
    float const angle = self.GetAngle(order.Where.GetPositionX(), order.Where.GetPositionY()) - view.Facing;
    float const relative = std::atan2(std::sin(angle), std::cos(angle));
    float const range = self.GetExactDist(&order.Where);
    obs[OBS_TARGET_DISTANCE] = std::min(1.0f, range / DISTANCE_SCALE);
    obs[OBS_TARGET_NEAR] = std::min(1.0f, range / NEAR_SCALE);
    obs[OBS_TARGET_SIN] = std::sin(relative);
    obs[OBS_TARGET_COS] = std::cos(relative);
}
