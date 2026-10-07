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

#ifndef ANIMUS_LIB_CURRICULUM_PARTY_FRAMES_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PARTY_FRAMES_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **What a player's UI shows of its party** (dungeon-curriculum I5, the party follow; no actions). GROUP_MEMBERS
    /// slots, the leader first, then the others in seat order (SeatView::Frames).
    ///
    /// **The party frames** are always there, as they are on a player's screen: alive, health, power, in combat, and
    /// which member leads. They say nothing of where a member is: a 3.3.5 party frame shows no position.
    ///
    /// **The minimap's party dots** (the choice, 2026-10-06): the one place a 3.3.5 client shows a party member's
    /// position without seeing it is the minimap, which draws a dot for each party member within its radius
    /// (PartyFollow.MinimapYards, 60 by default: about what the default zoom shows indoors) and nothing for one
    /// beyond it -- through walls, since the minimap is a map, not a view. No height: a dot is flat. The minimap is
    /// modelled heading-up (the client's "rotate minimap" option): right and forward of the seat's facing, which is
    /// the frame every other bearing the seat reads is in; the default north-up minimap would need the seat to know
    /// where north is, which nothing else tells it. Beyond the radius a member is known only by the frames, by what
    /// the camera shows (the entities block) and by what the seat remembers.
    class PartyFramesBlock final : public Block
    {
    public:
        enum Feature : uint32
        {
            FRAME_PRESENT = 0,
            FRAME_ALIVE,
            FRAME_LEADER,
            FRAME_IN_COMBAT,
            FRAME_HEALTH,
            FRAME_POWER,
            FRAME_DOT,                  // on the minimap
            FRAME_DOT_RIGHT,            // / the minimap's radius, in [-1, 1]
            FRAME_DOT_FORWARD,
            FRAME_DOT_DISTANCE,         // / the radius, in [0, 1]
            FRAME_FEATURES
        };

        /// Where the minimap draws a party member: shown within `radius` yards (2D) of the seat, and where, in the
        /// seat's facing frame. `facing` is the seat's yaw (the core's: 0 along +x, counter-clockwise).
        struct Dot
        {
            bool Shown = false;
            float Right = 0.0f;
            float Forward = 0.0f;
            float Distance = 0.0f;
        };
        [[nodiscard]] static Dot DotOf(float selfX, float selfY, float facing, float otherX, float otherY,
            float radius);

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        [[nodiscard]] static char const* FeatureName(uint32 feature);
    };
}

#endif
