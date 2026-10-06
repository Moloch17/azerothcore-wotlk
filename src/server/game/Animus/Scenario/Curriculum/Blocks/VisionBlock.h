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

#ifndef ANIMUS_LIB_CURRICULUM_VISION_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_VISION_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **What the seat sees through its camera** (camera-vision INTERFACE, naive slice): a third-person camera
    /// behind the seat's facing (follow mode: yaw offset 0, AnimusForge.Vision.Pitch and Zoom), rendered every
    /// decision by the ray caster (Vision::Render) into an image of AnimusForge.Vision.Height rows by Width
    /// columns, five channels a pixel -- distance, height over the feet, the surface's normal z, the kind, the
    /// objective -- then seven scalars (Vision::Scalar). No actions: the camera is fixed in this slice.
    ///
    /// The camera is the client's alone: nothing here writes the controls, the body or the server's facing. The
    /// units it sees are gathered from the grid around the seat, on its map's own update, so a stage with this
    /// block refuses AnimusForge.ObserveAfterJoin (AnimusForge::Forge::StartCurrent).
    class VisionBlock final : public Block
    {
    public:
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// 1: the first image layout (camera-vision V1).
        [[nodiscard]] uint32 Revision() const override { return 1; }
        /// "image": { height, width, channels, kinds, kind_channel, scalars } -- the image starts at the block's
        /// first column, the scalars right after it -- and "camera": the settings it was rendered with.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
