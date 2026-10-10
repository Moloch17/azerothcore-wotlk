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

#ifndef ANIMUS_LIB_CURRICULUM_COVERAGE_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_COVERAGE_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **The coarse coverage map** (general search, decision 0027): a heading-up, egocentric COVERAGE_GRID x
    /// COVERAGE_GRID grid of COVERAGE_CELL-yard cells (one mental-map tile each: Vision::MAP_TILE) round the body, each
    /// cell three channels -- the tile's 1-yd cells ever seen, stood on, and looked at this episode (MentalMap's
    /// per-tile counters), each over COVERAGE_SCALE and clamped -- so the search state of the whole dungeon is in the
    /// observation, not only in the GRU. Output cell (r, c) samples the tile holding the world point at forward
    /// (5.5 - r) x 32, right (c - 5.5) x 32 yd of the body (the crop's rotation; the body sits on the corner of cells
    /// (5, 5)..(6, 6)). 144 lookups a decision. After the map block, whose map it samples; no actions.
    ///
    /// Nothing in it comes from the navmesh or the map's data: a tile the seat never looked at reads 0 in every
    /// channel, whatever is there. `known` and `visited` are the map's own and outlive an episode on a kept map, as
    /// the crop's are; `searched` is this episode's alone (the map's epoch).
    class CoverageBlock final : public Block
    {
    public:
        static constexpr uint32 COVERAGE_GRID = 12;
        static constexpr float COVERAGE_CELL = 32.0f;
        static constexpr uint32 COVERAGE_CHANNELS = 3;
        static constexpr float COVERAGE_SCALE = 1024.0f;
        static constexpr uint32 OBS_COUNT = COVERAGE_GRID * COVERAGE_GRID * COVERAGE_CHANNELS;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// 1: the first coverage map (12 x 12 x 3, [row][col][channel]).
        [[nodiscard]] uint32 Revision() const override { return 1; }
        /// "coverage": { grid, cell_yards, channels, channel_names, heading_up, scale, layout "row_col_channel" }.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
