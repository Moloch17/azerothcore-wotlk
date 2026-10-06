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

#ifndef ANIMUS_VISION_FRAME_IMAGE_H
#define ANIMUS_VISION_FRAME_IMAGE_H

#include "Camera.h"
#include <array>
#include <string>

/// **A frame as a person looks at it**: the bytes a seat's camera sent (Camera.h, BYTES_PER_PIXEL a pixel), decoded as
/// the learner decodes them, drawn as one PNG for `forge camera snapshot` and the training audit
/// (AnimusForge.Vision.AuditInterval).
namespace Animus::Vision
{
    /// A kind's colour in the kind panel (the objective's pixels are white whatever they hit).
    extern uint8_t const KIND_COLOURS[KINDS][3];
    extern char const* const KIND_NAMES[KINDS];

    /// Pixels of each kind in a frame.
    [[nodiscard]] std::array<uint32_t, KINDS> KindCounts(Settings const& settings, uint8_t const* image);

    /// Output pixels between FramePng's panels.
    constexpr uint32_t PANEL_GAP = 4;

    /// The frame as an RGB PNG of four panels side by side, each the image scaled up `scale` times (nearest
    /// pixel), PANEL_GAP grey pixels apart:
    /// - depth: the distance channel, near dark and sky white;
    /// - kind: KIND_COLOURS, the objective white;
    /// - height over the feet: mid-grey at the feet, lighter above, darker below;
    /// - slope: the surface's normal z, white for level ground, black for a wall (and sky).
    [[nodiscard]] std::string FramePng(Settings const& settings, uint8_t const* image, uint32_t scale);

    /// CompositePng's colours: the sky, the haze far things fade into, the yards by which they have faded, and the
    /// height between two contour lines.
    constexpr uint8_t COMPOSITE_SKY[3] = { 30, 30, 80 };
    constexpr uint8_t COMPOSITE_HAZE[3] = { 90, 90, 120 };
    constexpr float COMPOSITE_FOG_YARDS = 80.0f;
    constexpr float COMPOSITE_CONTOUR = 1.0f;

    /// **Every layer in one picture** (var/camera/composite.py, from the bytes rather than the panels): one RGB
    /// panel, the image scaled up `scale` times (nearest pixel). Per pixel: the objective white; sky
    /// COMPOSITE_SKY; else the kind's colour (KIND_COLOURS), shaded by the slope (0.45 a wall to 1 a floor) and
    /// fogged by the distance (min(1, yards / 80)^0.7 towards COMPOSITE_HAZE x 0.6), and darkened to 0.35 where the
    /// pixel and its right or lower neighbour lie across a whole yard of height over the feet (a contour line;
    /// within the height channel's range only).
    [[nodiscard]] std::string CompositePng(Settings const& settings, uint8_t const* image, uint32_t scale);
}

#endif
