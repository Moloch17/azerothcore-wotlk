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

#ifndef ANIMUS_LIB_CURRICULUM_MAP_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_MAP_BLOCK_H

#include "Block.h"
#include "MentalMap.h"

namespace Animus::Curriculum
{
    /// **What the seat remembers of the place** (perception-goals REDESIGN §3 and the Change): its mental map
    /// (Vision::MentalMap, SeatState::Map), written every decision from that decision's camera frame as cast (the
    /// vision block leaves its rays on the seat, Vision::FrameHits) and from the seat's own body, then read as one
    /// egocentric, heading-up crop of Vision::CROP x CROP cells of Vision::CROP_CELL yards, six bytes a cell
    /// (Vision::CropChannel: the code, the floor's height over the feet, visited, the newest look's age, the most
    /// recent entity's class, the frontier). After the vision block (its frame is this decision's). No actions.
    ///
    /// The crop travels as bytes beside the observation -- the seat's map row (SeatView::MapRow), the STEP's map
    /// section (protocol 24), like the camera's image -- and the block's float columns are its four scalars. Nothing
    /// in it comes from the navmesh or the map's data: an unseen cell is unknown, whatever is there. The frontier is
    /// worked out from the seat's own map, as a player reads a minimap's edge.
    class MapBlock final : public Block
    {
    public:
        enum Scalar : uint32
        {
            OBS_KNOWN = 0,          // the share of the crop's cells ever seen
            OBS_FRONTIER,           // ... that are frontier
            OBS_VISITED,            // ... that the body stood on
            OBS_KEPT,               // 1 when this episode's map was kept from the one before (amendment 1)
            OBS_COUNT
        };

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// 1: the first map (perception-goals REDESIGN §3, the Change: one 48 x 48 crop at 2 yd, six channels).
        [[nodiscard]] uint32 Revision() const override { return 1; }
        /// "map": { transport "bytes", height, width (Vision::CROP), cell (yards), channels (6), channel_names,
        /// map_bytes, codes, code_names, code_channel, height_channel, height_step, height_zero, visited_channel,
        /// age_channel, age_scale, age_never, class_channel, classes (Vision::CLASS_LIMIT), frontier_channel,
        /// scalars, scalar_names } -- the block's columns are the scalars; the crop is the STEP's map section.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        /// The scalars of a crop (`kept`: the map was kept across the reset).
        static void Scalars(uint8 const* crop, bool kept, float* obs);
    };
}

#endif
