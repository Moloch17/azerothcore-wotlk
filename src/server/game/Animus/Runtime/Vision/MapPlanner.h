/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef ANIMUS_VISION_MAP_PLANNER_H
#define ANIMUS_VISION_MAP_PLANNER_H

#include "MentalMap.h"
#include <array>
#include <cstdint>

/// **The route on the seat's own map** (2026-10-17): the classical planner the learner runs over the map crops
/// (networks.py classical_cost_to_go, MapEncoder.classical_plan, coarse_frame, goal_plane) run here, in the sim, over
/// the same bytes the policy was handed -- so a label can price a decision on the route distance the bot itself can
/// perceive, not the bake's truth. The nav stages' labels carry its value at the body and at the body's last
/// position, both on the map as it is now (PointNavEncounter::WriteLabels): their difference is movement alone,
/// never the map changing.
///
/// The learner's rule, mirrored: an 8-neighbour min-plus over each tier's crop, a step from s to n costing
/// 0.5 x cell x sqrt(dr^2 + dc^2) x (factor(s) + factor(n)), the factor by the cell's code (PLAN_COSTS: unknown 1.5,
/// floor and door 1, wall 100, hazard 5 -- the learner's CLASSICAL_COSTS unknown, MAP_CODE_COSTS wall and hazard,
/// and a known floor priced as walked). The coarse tier (384 yd) is planned first, its goal cell clamped to the
/// crop's border; its value resampled bilinearly at the fine tier's border ring (the coarse row 23.5 + (i - 23.5) /
/// 4, clamped to the crop) seeds the fine tier (96 yd), a step onto the ring priced at the border cell's own factor;
/// the fine goal is its cell when the goal lies inside the fine crop, else nothing (the ring stands in). Two things
/// are the sim's own: the value is the exact fixed point (Dijkstra with a binary heap, not the learner's 40
/// iterations, which leave cells more than 40 steps from a source at the sentinel: the two agree wherever the
/// learner's has converged); and a floor or door whose height is more than PLAN_HEIGHT_BAND yards off the feet (the
/// fine tier's nearest layer, the coarse tier's mean) is priced as unknown -- the crop's height channel says it is
/// another storey, not ground the body can step onto from here, and not a wall either (a ramp's far end is such a
/// floor). The learner's classical_factor applies the same band (2026-10-17).
///
/// The Dijkstra stops once the cells a caller reads are settled: the coarse tier's central block the ring samples
/// (coarse cells 17..30 a side), and the fine cells round the points the caller names (PlanOnCrops' `near`); every
/// other cell is left where the search had got to, at or above its true value. A whole crop costs about twice as
/// much (plantime, 2026-10-17: 200-330 us for both tiers settled whole).
namespace Animus::Vision
{
    /// A cell's step cost multiple by its MapCode (unknown, floor, wall, door, hazard).
    constexpr float PLAN_COSTS[MAP_CODES] = { 1.5f, 1.0f, 100.0f, 1.0f, 5.0f };
    /// The factor at or above which a cell is blocked (a wall): left out of a value read between cells.
    constexpr float PLAN_BLOCKED = 100.0f;
    /// The cost-to-go of a cell nothing reached (the learner's CLASSICAL_SENTINEL).
    constexpr float PLAN_SENTINEL = 1e6f;
    /// A floor more than this far from the feet, in yards, is priced as unknown (exactly this far is in band).
    constexpr float PLAN_HEIGHT_BAND = 3.0f;

    /// Both tiers' cost-to-go, CROP x CROP each (yards of cost; PLAN_SENTINEL where nothing reached), and which fine
    /// cells are blocked.
    struct MapPlan
    {
        std::array<float, CROP * CROP> Fine{};
        std::array<float, CROP * CROP> Coarse{};
        std::array<uint8_t, CROP * CROP> FineBlocked{};
    };

    /// A point in the crops' frame, yards.
    struct PlanPoint
    {
        float Forward = 0.0f;
        float Right = 0.0f;
    };

    /// Plans to the goal at (`goalForward`, `goalRight`) yards in the crops' frame over the fine crop's bytes
    /// (CROP_BYTES at CROP_CELL yards a cell) and the coarse crop's (the same bytes at COARSE_CELL yards; null for
    /// none: the fine tier then plans alone, with no ring). The fine search stops once the cells round the `near`
    /// points (the ones PlanValueAt will be asked for; none: the whole crop) are settled.
    void PlanOnCrops(uint8_t const* fine, uint8_t const* coarse, float goalForward, float goalRight, MapPlan& out,
        PlanPoint const* near = nullptr, uint32_t nearCount = 0);

    /// The plan's fine value at (`forward`, `right`) yards in the crops' frame, in yards: bilinear over the four cells
    /// round the point, blocked and unreached cells left out and the weights renormalised (a wall's own cell holds a
    /// value a hundred yards up; a body walking along it must not be charged for that). TRAV_UNREACHABLE (65504)
    /// outside the fine crop, or where every cell round the point is blocked or unreached.
    [[nodiscard]] float PlanValueAt(MapPlan const& plan, float forward, float right);
}

#endif
