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
#include "Coverage.h"
#include "MentalMap.h"

namespace Animus::Curriculum
{
    /// **What the seat remembers of the place** (perception-goals REDESIGN §3 and the Change): its mental map
    /// (Vision::MentalMap, SeatState::Map), written every decision from that decision's camera frame as cast (the
    /// vision block leaves its rays on the seat, Vision::FrameHits), from the entities the sensor listed (the
    /// entities block's list, Vision::SeenList: where each stood, as its class) and from the seat's own body, then
    /// read as one
    /// egocentric, heading-up crop of Vision::CROP x CROP cells of Vision::CROP_CELL yards, seven bytes a cell
    /// (Vision::CropChannel: the code, the floor's height over the feet, visited, the newest look's age, the most
    /// recent entity's class, the frontier, how many of its four 1-yd cells were looked at this episode). After the
    /// vision block (its frame is this decision's). No actions.
    ///
    /// The crop travels as bytes beside the observation -- the seat's map row (SeatView::MapRow), the STEP's map
    /// section (protocol 24), like the camera's image -- and the block's float columns are its seven scalars. Nothing
    /// in it comes from the navmesh or the map's data: an unseen cell is unknown, whatever is there. The frontier is
    /// worked out from the seat's own map, as a player reads a minimap's edge.
    ///
    /// The block also records where each crop was taken from (SeatView::Crop: the pose, and how many of its pooled
    /// blocks a cell goal can name, CellGrid::Count), which the scenario decodes the learner's cell choice against.
    class MapBlock final : public Block
    {
    public:
        enum Scalar : uint32
        {
            OBS_KNOWN = 0,          // the share of the crop's cells ever seen
            OBS_FRONTIER,           // ... that are frontier
            OBS_VISITED,            // ... that the body stood on
            OBS_KEPT,               // 1 when this episode's map was kept from the one before (amendment 1)
            OBS_SEARCHED,           // the share of the crop's 1-yd cells looked at this episode
            OBS_NEW_AGE,            // seconds since the body last stood on new ground, over GROUND_AGE_SCALE_S, <= 1
            OBS_TOTAL,              // the 1-yd cells the body has stood on this episode, over GROUND_TOTAL_SCALE, <= 1
            /// **The coverage scalars** (revision 3, general search; Coverage::Analyse over this decision's crop):
            /// the nearest frontier cluster's bearing off the facing (sin, cos) and distance (over COVERAGE_REACH),
            /// the largest cluster's the same, the cluster count over CLUSTER_SCALE, and the 1-yd cells looked at
            /// THIS episode over the whole map (MentalMap::SearchedCells, over SEARCHED_SCALE): the episode-local
            /// progress record. Zeros where there is none.
            OBS_FRONTIER_SIN,
            OBS_FRONTIER_COS,
            OBS_FRONTIER_DIST,
            OBS_REGION_SIN,
            OBS_REGION_COS,
            OBS_REGION_DIST,
            OBS_CLUSTERS,
            OBS_SEARCHED_CELLS,
            OBS_COUNT
        };
        static constexpr float CLUSTER_SCALE = 8.0f;
        static constexpr float SEARCHED_SCALE = 6000.0f;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// 1: the first map (perception-goals REDESIGN §3, the Change: one 48 x 48 crop at 2 yd, six channels).
        /// 2: the searched channel (a seventh) and the three scalars searched, new_age, total (exploration v3,
        /// decision 0026). 3: the eight coverage scalars (frontier_sin .. searched_cells; general search, decision
        /// 0027); the crop is unchanged.
        [[nodiscard]] uint32 Revision() const override { return 3; }
        /// "map": { transport "bytes", height, width (Vision::CROP), cell (yards), channels (7), channel_names,
        /// map_bytes, codes, code_names, code_channel, height_channel, height_step, height_zero, visited_channel,
        /// age_channel, age_scale, age_never, class_channel, classes (Vision::CLASS_LIMIT), frontier_channel,
        /// searched_channel, searched_max, new_age_scale_s, total_scale, epoch ("episode"), scalars, scalar_names,
        /// coverage_cell_yards, coverage_reach_yards, cluster_scale, floor_scale, cluster_min_cells, pocket_min_cells }
        /// -- the block's columns are the scalars; the crop is the STEP's map section.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        /// The scalars of a crop that it alone gives (`kept`: the map was kept across the reset); the ground ones are
        /// the map's (Observe), the coverage ones the analysis's (CoverageScalars).
        static void Scalars(uint8 const* crop, bool kept, float* obs);
        /// The coverage scalars of an analysis (`facing` the body's), into obs[OBS_FRONTIER_SIN ..].
        static void CoverageScalars(Coverage::Summary const& summary, float facing, float* obs);
        /// The cluster and pocket sizes the analysis uses (Coverage::Analyse), the process's (AnimusForge.Map.* has
        /// none: they are the seek tuning's, set once at startup by the scenario; the manifest records them).
        static void ConfigureCoverage(uint32 clusterMinCells, uint32 pocketMinCells);
    };
}

#endif
