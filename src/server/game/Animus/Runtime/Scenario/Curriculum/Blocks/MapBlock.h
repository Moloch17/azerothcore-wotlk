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
#include "MapPlanner.h"
#include "MentalMap.h"
#include <algorithm>
#include <cmath>

namespace Animus::Curriculum
{
    /// **What the seat remembers of the place** (perception-goals REDESIGN §3 and the Change): its mental map
    /// (Vision::MentalMap, SeatState::Map), written every decision from that decision's camera frame as cast (the
    /// vision block leaves its rays on the seat, Vision::FrameHits), from the entities the sensor listed (the
    /// entities block's list, Vision::SeenList: where each stood, as its class) and from the seat's own body, then
    /// read as one
    /// egocentric, heading-up crop of Vision::CROP x CROP cells of Vision::CROP_CELL yards, eight bytes a cell
    /// (Vision::CropChannel: the code, the floor's height over the feet, visited, the newest look's age, the most
    /// recent entity's class, the frontier, how many of its four 1-yd cells were looked at this episode, how many are
    /// walls), and -- a stage with two map tiers (StageDefinition::MapTiers) -- the same crop at Vision::COARSE_CELL
    /// yards behind it. After the vision block (its frame is this decision's). No actions.
    ///
    /// The crop travels as bytes beside the observation -- the seat's map row (SeatView::MapRow), the STEP's map
    /// section (protocol 24), like the camera's image -- and the block's float columns are its scalars. Nothing
    /// in it comes from the navmesh or the map's data: an unseen cell is unknown, whatever is there. The frontier is
    /// worked out from the seat's own map, as a player reads a minimap's edge.
    ///
    /// **The route scalars** (revision 7, 2026-10-17; Vision::MapPlanner): when the encounter names an objective
    /// (SeatView::HasObjective, not withheld), the block plans the route to it on the seat's own map -- the 1-yd
    /// cells round the body shaped into the body's configuration space, the coarse crop as the ring beyond them --
    /// and hands the policy the route itself: the fine plan's walk read at 5, 10, 20 and 30 steps (Vision::FineRoute),
    /// the coarse plan's waypoint (Vision::CoarseWaypoint), the own-map route distance at the body and how long it
    /// has been since that distance last fell (Vision::RouteStall). Nothing here is the straight line to the goal:
    /// since revision 7 the nav stages carry no compass, and the route is the only word the policy gets of where
    /// the goal lies. The plan's value at the body and at the crop before's pose go with the crop's pose
    /// (CropPose::Route*) for the nav labels (PointNavEncounter::WriteLabels), so the route is planned once a
    /// decision.
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
            OBS_TOTAL,              // the 1-yd cells the body has stood on this episode, LogShare over TOTAL_LOG_MAX
            /// **The coverage scalars** (revision 3, general search; Coverage::Analyse over this decision's crop):
            /// the nearest frontier cluster's bearing off the facing (sin, cos) and distance (over COVERAGE_REACH),
            /// the largest cluster's the same, the cluster count over CLUSTER_SCALE, and the 1-yd cells looked at
            /// THIS episode over the whole map (MentalMap::SearchedCells, LogShare over SEARCHED_LOG_MAX): the
            /// episode-local progress record. Zeros where there is none.
            OBS_FRONTIER_SIN,
            OBS_FRONTIER_COS,
            OBS_FRONTIER_DIST,
            OBS_REGION_SIN,
            OBS_REGION_COS,
            OBS_REGION_DIST,
            OBS_CLUSTERS,
            OBS_SEARCHED_CELLS,
            /// **The route scalars** (revision 7; Vision::MapPlanner): the fine plan's walk from the body, read at
            /// Vision::PLAN_ROUTE_STEPS (5, 10, 20, 30 steps of a cell) as sin and cos of each point's bearing off
            /// the facing (the convention the compass block had: sin positive to the left), the steps the walk made
            /// over Vision::PLAN_FINE_STEPS, and valid 1 when the walk moved and an objective is named (every fine
            /// value 0 otherwise); the coarse plan's waypoint (Vision::PLAN_COARSE_STEPS coarse cells) as sin, cos,
            /// its distance over the walk's reach (past 1 on a diagonal) and valid; the own-map route distance at the
            /// body over Vision::PLAN_DIST_SCALE with its valid (both 0 when unreachable); and the stall clock --
            /// seconds since that distance last fell below its least this episode, over Vision::PLAN_STALL_SECONDS,
            /// clamped to 1 (0 at the first decision; it keeps running while unreachable).
            /// The objective in the body frame, raw yards forward and right (0 both with no objective or one withheld):
            /// the planner school's goal (animus.school; its route is what the policy sees), never read by the
            /// policy's own embedding, which stops before these -- the learner's rule. The columns sit immediately
            /// before the route's seventeen.
            OBS_GOAL_FORWARD_YARDS,
            OBS_GOAL_RIGHT_YARDS,
            OBS_ROUTE_P5_SIN,
            OBS_ROUTE_P5_COS,
            OBS_ROUTE_P10_SIN,
            OBS_ROUTE_P10_COS,
            OBS_ROUTE_P20_SIN,
            OBS_ROUTE_P20_COS,
            OBS_ROUTE_P30_SIN,
            OBS_ROUTE_P30_COS,
            OBS_ROUTE_FINE_REACH,
            OBS_ROUTE_FINE_VALID,
            OBS_ROUTE_COARSE_SIN,
            OBS_ROUTE_COARSE_COS,
            OBS_ROUTE_COARSE_DIST,
            OBS_ROUTE_COARSE_VALID,
            OBS_ROUTE_DIST,
            OBS_ROUTE_DIST_VALID,
            OBS_ROUTE_STALL,
            OBS_COUNT
        };
        static constexpr float CLUSTER_SCALE = 8.0f;
        /// The episode's whole-map totals on a log scale that no dungeon fills (revision 4, decision 0031): the 1-yd
        /// cells looked at (searched_cells) and stood on (total). Linear over 6000 and 3000 they reached 1 about a
        /// third of the way into most rung-2 episodes and said nothing after; log1p(n) / log1p(max) keeps the early
        /// cells fine (100 -> 0.39, 1000 -> 0.59) and still rises at 20000 (0.84).
        static constexpr float SEARCHED_LOG_MAX = 131072.0f;
        static constexpr float TOTAL_LOG_MAX = 32768.0f;
        [[nodiscard]] static float LogShare(float count, float max)
        {
            return std::min(1.0f, std::log1p(std::max(0.0f, count)) / std::log1p(max));
        }

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// 1: the first map (perception-goals REDESIGN §3, the Change: one 48 x 48 crop at 2 yd, six channels).
        /// 2: the searched channel (a seventh) and the three scalars searched, new_age, total (exploration v3,
        /// decision 0026). 3: the eight coverage scalars (frontier_sin .. searched_cells; general search, decision
        /// 0027); the crop is unchanged. 4: searched_cells and total on the log scale (LogShare; decision 0031): the
        /// same columns, no longer capped a third of the way into an episode. 5: the crop may come in two tiers
        /// (vision movement curriculum, CONTRACT.md sec 3.2): the manifest's `tiers` lists each tier's cell, fine
        /// first (2 yd) then coarse (8 yd, Vision::COARSE_CELL, MentalMap::CropWide), `map_bytes` is the tiers'
        /// total, and the STEP's map section carries the tiers in that order. A stage with StageDefinition::MapTiers
        /// 1 is byte-identical to revision 4. 6: the crop's eighth channel, walls (Vision::CROP_WALLS: CROP_BYTES
        /// 18,432 a tier), a crop cell a wall only when most of its 1-yd cells are (Vision::CROP_WALL_MIN), and the
        /// eight route scalars appended (route_fine_sin .. route_coarse_valid; memv4, 2026-10-17): a learner built
        /// on revision 5's bytes refuses it. 7 (2026-10-17): the eight route scalars are seventeen (route_p5_sin ..
        /// route_stall: the fine walk at four points, its reach, the coarse waypoint, the own-map distance and the
        /// stall clock), the planner works in the body's configuration space (Vision::ConfigurationSpace), and the
        /// nav stages drop the compass: the route is the goal's only observation. The crop is unchanged. Still 7
        /// (2026-10-17, later): two scalars, goal_forward_yards and goal_right_yards, inserted before the seventeen
        /// for the planner school's input (a learner finds them by name).
        [[nodiscard]] uint32 Revision() const override { return 7; }
        /// "map": { transport "bytes", height, width (Vision::CROP), cell (yards), tiers [{cell, bytes}...],
        /// tier_bytes (one tier's), channels (8), channel_names, map_bytes (every tier's), codes, code_names,
        /// code_channel, height_channel, height_step, height_zero, visited_channel, age_channel, age_scale, age_never,
        /// class_channel, classes (Vision::CLASS_LIMIT), frontier_channel, searched_channel, searched_max,
        /// walls_channel, walls_max, wall_min (Vision::CROP_WALL_MIN), new_age_scale_s, total_log_max, epoch
        /// ("episode"), scalars, scalar_names, coverage_cell_yards, coverage_reach_yards, cluster_scale,
        /// searched_cells_log_max, cluster_min_cells, pocket_min_cells, route_points_yards ([5, 10, 20, 30]),
        /// route_fine_reach_yards (30), route_coarse_reach_yards (16), route_dist_yards (300), route_stall_seconds
        /// (30) }
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
