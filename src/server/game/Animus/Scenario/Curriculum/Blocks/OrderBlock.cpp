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
#include "CombatReward.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float DISTANCE_SCALE = 100.0f;

    /// Distance and bearing to somewhere, as the other blocks write a place.
    void WritePlace(Player const* bot, Position const& place, float* out)
    {
        float const bearing = bot->GetRelativeAngle(&place);
        out[0] = std::min(1.0f, bot->GetExactDist2d(&place) / DISTANCE_SCALE);
        out[1] = std::sin(bearing);
        out[2] = std::cos(bearing);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::OrderBlock::Size(Layout const& /*layout*/) const
{
    // No actions: an order is read, not pressed.
    return { OBS_COUNT, 0 };
}

void Animus::Curriculum::OrderBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    SeatView::TeamOrder const& order = view.Order;

    // Everything stays zero when no director is giving orders, which is what an undirected arena looks like to
    // a seat whose layout happens to carry this block.
    if (!order.Active)
        return;

    Player const* bot = view.Bot;
    if (!bot)
        return;

    obs[OBS_ACTIVE] = 1.0f;
    obs[OBS_POSTURE + uint32(order.Posture)] = 1.0f;
    obs[OBS_RALLY + uint32(order.Rally)] = 1.0f;

    if (order.HasRallyPlace)
        WritePlace(bot, order.RallyPlace, &obs[OBS_RALLY_DISTANCE]);

    // A call the seat cannot see is still a call: it says so and stops there, rather than handing over a
    // distance and a bearing to something out of sight. ViewSeat's own hidden-filter never touched the order.
    if (order.FocusUnseen)
        obs[OBS_FOCUS_UNSEEN] = 1.0f;

    Unit const* focus = order.Focus;
    if (focus && focus->IsAlive())
    {
        obs[OBS_HAS_FOCUS] = 1.0f;
        // What the seat's own actions aim at. Not UNIT_FIELD_TARGET: that is set by the client's selection
        // packet, which a sessionless bot never sends, so it reads empty for every seat.
        obs[OBS_FOCUS_IS_TARGET] = view.Target == focus ? 1.0f : 0.0f;
        WritePlace(bot, *focus, &obs[OBS_FOCUS_DISTANCE]);
        obs[OBS_FOCUS_HEALTH] = CombatReward::HealthLeft(focus);
    }

    obs[OBS_IS_DUTY] = order.IsDuty ? 1.0f : 0.0f;
}
