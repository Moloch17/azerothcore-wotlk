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

/// **The route on the seat's own map** (2026-10-17): a classical planner over the mental map, run in the sim -- so a
/// label can price a decision on the route distance the bot itself can perceive, not the bake's truth, and so the
/// policy is handed the route itself (the map block's seventeen route scalars, map block revision 7) from the map as
/// it is, not from the crop's summary of it -- and never the straight line to the goal, which no nav observation
/// carries since revision 7. The nav stages' labels carry its value at the body and at the body's last position,
/// both on the map as it is now (PointNavEncounter::WriteLabels): their difference is movement alone, never the map
/// changing.
///
/// **The fine tier plans on the 1-yd cells** (memv4, 2026-10-17): PLAN_SIDE x PLAN_SIDE of the map's own cells, an
/// axis-aligned window centred on the body (the fine crop's 96-yd extent), each cell priced by its code for the feet
/// (CodeOf: PLAN_COSTS -- unknown 1.5, floor and door 1, wall 100, hazard 5 -- the learner's CLASSICAL_COSTS unknown,
/// MAP_CODE_COSTS wall and hazard, a known floor priced as walked), a floor or door whose layer nearest the feet is
/// more than PLAN_HEIGHT_BAND yards off them priced as unknown (another storey, not ground the body can step onto
/// from here; not a wall either -- a ramp's far end is such a floor). Planned over the 2-yd crop, a wall-footed
/// sub-cell made its crop cell a wall, and the route through a 4-yd corridor read 2-3 x the true one or none at all;
/// over the 1-yd cells the same memory's route is within a percent of the bake's. An 8-neighbour min-plus: a step from
/// s to n costs sqrt(dr^2 + dc^2) yards x (factor(s) + factor(n)) / 2, the exact fixed point by Dijkstra with a binary
/// heap. The goal is its own cell at 0 when it lies inside the window; else nothing, and the coarse tier stands in.
///
/// **A configuration space, not a point's** (2026-10-17; ConfigurationSpace): the body is wider than a cell, so the
/// fine factors are shaped before the search. (1) Hole closing: a non-wall cell whose two opposite neighbours are
/// walls -- north and south, east and west, or both cells of either diagonal pair -- is a wall (PLAN_BLOCKED): a 1-yd
/// gap is narrower than the body, and a route through it is one the body cannot walk. (2) Inflation: a passable cell
/// 8-adjacent to a wall (after the closing; the window's edge counts as no wall) is priced at least PLAN_INFLATED 3
/// -- soft, so a route runs down the middle of a corridor and hugs no wall, while a 2- or 3-yd passage, every cell of
/// it dear, stays routable. A hazard keeps its own 5. Unknown 1.5, hazard 5, wall 100 and the height band are as they
/// were. The coarse plan is left as it is: its cells are 8 yd, so a cell between two wall cells is a corridor, not a
/// gap, and in a dungeon nearly every coarse cell touches a wall cell -- inflation there would triple the plane and
/// erase the routing signal it exists to give.
///
/// **The coarse tier plans on the coarse crop** as before (the learner's rule, mirrored: the 8-yd crop's bytes,
/// PLAN_COSTS by the cell's code with the same height band on the cell's mean height, its goal cell clamped to the
/// crop's border along the line from the body), and its value, sampled bilinearly (grid_sample with align_corners
/// and border padding, as the learner's coarse_frame) at the centres of the cells just past the window's border --
/// each turned into the crop's heading-up frame -- seeds the window's border ring, a step onto it priced at the
/// border cell's own factor. Without a coarse crop the fine tier plans alone, with no ring.
///
/// The Dijkstra stops once the cells a caller reads are settled: the coarse tier's central block the ring samples,
/// and the fine cells round the points the caller names (PlanOnMap's `near`; the body's always); every other cell
/// is left where the search had got to, at or above its true value. The waypoint walks (FineRoute, CoarseWaypoint)
/// only ever step to a strictly lower value, and Dijkstra settles cells in value order, so every cell a walk from a
/// settled cell can reach is settled too.
///
/// **The route points** (FineRoute; map block revision 7) mirror the learner's networks.descend: from the body's
/// cell, up to PLAN_FINE_STEPS moves on the fine plan, each to the lowest neighbour (the first of equals in row-major
/// neighbour order) and only when it is strictly lower than the cell left; the cells reached after PLAN_ROUTE_STEPS
/// (5, 10, 20, 30) steps -- a step is a cell, a diagonal one counts one as the learner's does -- each from the body in
/// the body's heading-up frame: sin = -right / d (positive to the left: WoW's yaw turns left), cos = forward / d.
/// A walk that ends earlier (the goal reached, no lower neighbour) repeats its end cell for the later points; its
/// reach is the steps it made over PLAN_FINE_STEPS; valid 1 when it moved and a goal was named, every number 0 when
/// not. The coarse waypoint (CoarseWaypoint) is as before: PLAN_COARSE_STEPS on the coarse plan from the learner's
/// anchor cell PLAN_COARSE_ANCHOR, as sin, cos, dist over the walk's reach (16 yd; past 1 on a diagonal), valid.
namespace Animus::Vision
{
    /// A cell's step cost multiple by its MapCode (unknown, floor, wall, door, hazard).
    constexpr float PLAN_COSTS[MAP_CODES] = { 1.5f, 1.0f, 100.0f, 1.0f, 5.0f };
    /// The factor at or above which a cell is blocked (a wall): left out of a value read between cells.
    constexpr float PLAN_BLOCKED = 100.0f;
    /// The least factor of a passable fine cell next to a wall (ConfigurationSpace): dear, not blocked.
    constexpr float PLAN_INFLATED = 3.0f;
    /// The cost-to-go of a cell nothing reached (the learner's CLASSICAL_SENTINEL).
    constexpr float PLAN_SENTINEL = 1e6f;
    /// A floor more than this far from the feet, in yards, is priced as unknown (exactly this far is in band).
    constexpr float PLAN_HEIGHT_BAND = 3.0f;
    /// The fine plan's window: PLAN_SIDE x PLAN_SIDE 1-yd cells (the fine crop's extent), the body's cell PLAN_HALF
    /// cells in from its first.
    constexpr int32_t PLAN_SIDE = int32_t(float(CROP) * CROP_CELL / MAP_CELL);
    constexpr int32_t PLAN_HALF = PLAN_SIDE / 2;
    constexpr uint32_t PLAN_CELLS = uint32_t(PLAN_SIDE * PLAN_SIDE);
    /// The walks' steps: a cell a step on the fine plan (PLAN_FINE_STEPS of them, the route points at
    /// PLAN_ROUTE_STEPS along the walk; map block revision 7), a coarse cell on the coarse (the learner's
    /// WAYPOINT_STEPS); and the coarse walk's start, the learner's WAYPOINT_ANCHOR (READOUT_FIRST + 1: the cell
    /// ahead and left of the crop's centre).
    constexpr uint32_t PLAN_FINE_STEPS = 30;
    constexpr uint32_t PLAN_ROUTE_POINTS = 4;
    constexpr uint32_t PLAN_ROUTE_STEPS[PLAN_ROUTE_POINTS] = { 5, 10, 20, 30 };
    constexpr uint32_t PLAN_COARSE_STEPS = 2;
    constexpr int32_t PLAN_COARSE_ANCHOR = 23;
    /// The map block's route_dist scale (yards of own-map route the scalar saturates at) and the route_stall clock's
    /// (seconds without a new least own-map route the scalar saturates at).
    constexpr float PLAN_DIST_SCALE = 300.0f;
    constexpr float PLAN_STALL_SECONDS = 30.0f;

    /// Both tiers' cost-to-go (yards of cost; PLAN_SENTINEL where nothing reached): the fine window's PLAN_SIDE x
    /// PLAN_SIDE 1-yd cells, row by row (y first, as MentalMap::Window), and the coarse crop's CROP x CROP; which fine
    /// cells are blocked; and where the window is (its first cell, world 1-yd cell coordinates), the body and facing
    /// it was planned at, whether a coarse crop seeded its ring, and whether a goal was named at all.
    struct MapPlan
    {
        std::array<float, PLAN_CELLS> Fine{};
        std::array<float, CROP * CROP> Coarse{};
        std::array<uint8_t, PLAN_CELLS> FineBlocked{};
        int32_t X0 = 0;
        int32_t Y0 = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Yaw = 0.0f;
        bool HasCoarse = false;
        bool HasGoal = false;
    };

    /// A point in the body's heading-up frame, yards.
    struct PlanPoint
    {
        float Forward = 0.0f;
        float Right = 0.0f;
    };

    /// A waypoint as the map block's coarse scalars carry it (route_coarse_sin, cos, dist, valid).
    struct PlanWaypoint
    {
        float Sin = 0.0f;
        float Cos = 0.0f;
        float Dist = 0.0f;
        float Valid = 0.0f;
    };

    /// The fine plan's route as the map block's scalars carry it (route_p5_sin .. route_fine_valid): the bearing of
    /// the cell reached after each of PLAN_ROUTE_STEPS steps, the steps the walk made over PLAN_FINE_STEPS, valid.
    struct PlanRoute
    {
        float Sin[PLAN_ROUTE_POINTS] = {};
        float Cos[PLAN_ROUTE_POINTS] = {};
        float Reach = 0.0f;
        float Valid = 0.0f;
    };

    /// Plans to the goal at (`goalForward`, `goalRight`) yards in the body's frame -- the body at (x, y), its feet at
    /// z, facing `yaw` (WoW's) -- over the map's 1-yd cells round the body, with the coarse crop's bytes (`coarse`,
    /// CROP_BYTES at COARSE_CELL yards; null for none: the fine tier then plans alone, with no ring) planned first as
    /// the window's ring. The fine search stops once the cells round the body and the `near` points (the ones
    /// PlanValueAt will be asked for) are settled.
    void PlanOnMap(MentalMap const& map, float x, float y, float z, float yaw, float goalForward, float goalRight,
        uint8_t const* coarse, MapPlan& out, PlanPoint const* near = nullptr, uint32_t nearCount = 0);

    /// The plan's fine value at (`forward`, `right`) yards in the body's frame, in yards: bilinear over the four cells
    /// round the point, blocked and unreached cells left out and the weights renormalised (a wall's own cell holds a
    /// value a hundred yards up; a body walking along it must not be charged for that). TRAV_UNREACHABLE (65504)
    /// outside the window, or where every cell round the point is blocked or unreached.
    [[nodiscard]] float PlanValueAt(MapPlan const& plan, float forward, float right);

    /// Shapes a `side` x `side` grid of fine factors into the body's configuration space (the class comment): the
    /// holes closed to PLAN_BLOCKED, the passable cells next to a wall raised to PLAN_INFLATED at least. Exposed for
    /// the harness; PlanOnMap applies it to the fine window.
    void ConfigurationSpace(float* factor, int32_t side);

    /// The fine plan's route points: the walk from the body's cell (from the lowest unblocked cell round the body
    /// when its own is a wall's foot), PLAN_FINE_STEPS at most, read at PLAN_ROUTE_STEPS.
    [[nodiscard]] PlanRoute FineRoute(MapPlan const& plan);

    /// The route_stall clock (map block revision 7): `stall` is the seconds since the body's own-map cost-to-go
    /// last fell below its least so far this episode (`best`, `hasBest`), advanced by `seconds` (0 at an episode's
    /// first decision: nothing has elapsed) and reset to 0 by a new least; it keeps running while the cost is
    /// unreachable (`reachable` false). The scalar is the clock over PLAN_STALL_SECONDS, clamped to 1.
    void RouteStall(float cost, bool reachable, float seconds, bool& hasBest, float& best, float& stall);
    /// The coarse plan's waypoint: the walk from PLAN_COARSE_ANCHOR, PLAN_COARSE_STEPS at most; invalid without a
    /// coarse crop.
    [[nodiscard]] PlanWaypoint CoarseWaypoint(MapPlan const& plan);
}

#endif
