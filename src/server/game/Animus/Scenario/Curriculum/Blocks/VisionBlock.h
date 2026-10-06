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
    /// objective -- packed in four bytes (Vision::BYTES_PER_PIXEL, camera-vision.BYTES.md), and seven scalars
    /// (Vision::Scalar). The block's float columns are the scalars alone; the image goes to the seat's byte row
    /// (SeatView::Image), which travels beside the observations (EnvPool::Image, the STEP's image section). No
    /// actions: the camera is fixed in this slice.
    ///
    /// The camera is the client's alone: nothing here writes the controls, the body or the server's facing. The
    /// units it sees are gathered from the grid around the seat, on its map's own update, so a stage with this
    /// block refuses AnimusForge.ObserveAfterJoin (AnimusForge::Forge::StartCurrent).
    class VisionBlock final : public Block
    {
    public:
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// 1: the first image layout (camera-vision V1). 2: the ray casts' distance channel, log-scaled to a fixed
        /// 1,000 yd instead of the range, and sky only past the loaded grids (camera-vision.RAYCAST.md): the same
        /// columns read differently, so an encoder trained on revision 1 starts fresh. 3: the image left the float
        /// columns for bytes (camera-vision.BYTES.md); the block's columns are the seven scalars.
        [[nodiscard]] uint32 Revision() const override { return 3; }
        /// "image": { height, width, channels (5, decoded), kinds, kind_channel, scalars, transport "bytes",
        /// bytes_per_pixel 4 } -- the block's columns are the scalars; the image is the STEP's byte section -- and
        /// "camera": the settings it was rendered with.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
