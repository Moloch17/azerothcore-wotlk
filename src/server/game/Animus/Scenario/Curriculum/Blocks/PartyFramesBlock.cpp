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

#include "PartyFramesBlock.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <algorithm>
#include <cmath>

Animus::Curriculum::PartyFramesBlock::Dot Animus::Curriculum::PartyFramesBlock::DotOf(float selfX, float selfY,
    float facing, float otherX, float otherY, float radius)
{
    Dot dot;
    float const dx = otherX - selfX;
    float const dy = otherY - selfY;
    dot.Distance = std::sqrt(dx * dx + dy * dy);
    if (radius <= 0.0f || dot.Distance > radius)
        return dot;

    // Forward along the facing; right is clockwise of it (the core's yaw turns counter-clockwise, to the left).
    float const c = std::cos(facing);
    float const s = std::sin(facing);
    dot.Shown = true;
    dot.Forward = dx * c + dy * s;
    dot.Right = dx * s - dy * c;
    return dot;
}

Animus::Curriculum::BlockSize Animus::Curriculum::PartyFramesBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ GROUP_MEMBERS * FRAME_FEATURES, 0 };
}

void Animus::Curriculum::PartyFramesBlock::DescribeManifest(Layout const& /*layout*/,
    boost::json::object& block) const
{
    block["members"] = GROUP_MEMBERS;
    block["member_features"] = uint32(FRAME_FEATURES);
}

char const* Animus::Curriculum::PartyFramesBlock::FeatureName(uint32 feature)
{
    switch (feature)
    {
        case FRAME_PRESENT:      return "present";
        case FRAME_ALIVE:        return "alive";
        case FRAME_LEADER:       return "leader";
        case FRAME_IN_COMBAT:    return "in_combat";
        case FRAME_HEALTH:       return "health";
        case FRAME_POWER:        return "power";
        case FRAME_DOT:          return "dot";
        case FRAME_DOT_RIGHT:    return "dot_right";
        case FRAME_DOT_FORWARD:  return "dot_forward";
        case FRAME_DOT_DISTANCE: return "dot_distance";
        default:                 return "";
    }
}

void Animus::Curriculum::PartyFramesBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    for (uint32 member = 0; member < GROUP_MEMBERS; ++member)
        for (uint32 feature = 0; feature < FRAME_FEATURES; ++feature)
            names.emplace_back(Acore::StringFormat("member{}_{}", member, FeatureName(feature)));
}

void Animus::Curriculum::PartyFramesBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::fill(obs, obs + GROUP_MEMBERS * FRAME_FEATURES, 0.0f);
    float const radius = std::max(1.0f, view.MinimapYards);
    for (uint32 member = 0; member < GROUP_MEMBERS; ++member)
    {
        SeatView::PartyFrame const& frame = view.Frames[member];
        if (!frame.Present)
            continue;
        float* out = obs + member * FRAME_FEATURES;
        out[FRAME_PRESENT] = 1.0f;
        out[FRAME_ALIVE] = frame.Alive ? 1.0f : 0.0f;
        out[FRAME_LEADER] = frame.Leader ? 1.0f : 0.0f;
        out[FRAME_IN_COMBAT] = frame.InCombat ? 1.0f : 0.0f;
        out[FRAME_HEALTH] = std::clamp(frame.Health, 0.0f, 1.0f);
        out[FRAME_POWER] = std::clamp(frame.Power, 0.0f, 1.0f);
        if (!frame.DotShown)
            continue;
        out[FRAME_DOT] = 1.0f;
        out[FRAME_DOT_RIGHT] = std::clamp(frame.DotRight / radius, -1.0f, 1.0f);
        out[FRAME_DOT_FORWARD] = std::clamp(frame.DotForward / radius, -1.0f, 1.0f);
        out[FRAME_DOT_DISTANCE] = std::clamp(std::sqrt(frame.DotRight * frame.DotRight
            + frame.DotForward * frame.DotForward) / radius, 0.0f, 1.0f);
    }
}
