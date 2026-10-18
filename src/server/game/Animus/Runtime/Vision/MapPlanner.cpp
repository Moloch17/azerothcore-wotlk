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

#include "MapPlanner.h"
#include "TravBake.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

namespace
{
    using namespace Animus::Vision;

    constexpr int32_t COARSE_SIDE = int32_t(CROP);
    constexpr int32_t COARSE_CELLS = COARSE_SIDE * COARSE_SIDE;
    /// The coarse crop's centre in cell coordinates: the body, at the corner of the four middle cells (goal_plane's
    /// centre).
    constexpr float COARSE_CENTRE = float(COARSE_SIDE - 1) * 0.5f;
    /// The eight neighbours as (row, column) offsets in the learner's order (networks.NEIGHBOURS), and the yards of
    /// each step per yard of cell.
    constexpr int32_t DR[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    constexpr int32_t DC[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    constexpr float DIST[8] = { 1.4142136f, 1.0f, 1.4142136f, 1.0f, 1.0f, 1.4142136f, 1.0f, 1.4142136f };

    /// Each coarse crop cell's step cost multiple (PLAN_COSTS by its code; a floor or door further than
    /// PLAN_HEIGHT_BAND off the feet priced as unknown).
    void CoarseFactors(uint8_t const* crop, float* factor)
    {
        for (int32_t index = 0; index < COARSE_CELLS; ++index)
        {
            uint8_t const* bytes = crop + std::size_t(index) * CROP_CHANNELS;
            uint32_t code = std::min<uint32_t>(bytes[CROP_CODE], MAP_CODES - 1);
            if ((code == uint32_t(MapCode::Floor) || code == uint32_t(MapCode::Door)) && bytes[CROP_HEIGHT])
            {
                float const height = float(int32_t(bytes[CROP_HEIGHT]) - int32_t(CROP_HEIGHT_ZERO)) * CROP_HEIGHT_STEP;
                if (std::fabs(height) > PLAN_HEIGHT_BAND)
                    code = uint32_t(MapCode::Unknown);
            }
            factor[index] = PLAN_COSTS[code];
        }
    }

    struct Entry
    {
        float Value;
        int32_t Index;
        bool operator>(Entry const& other) const { return Value > other.Value; }
    };

    /// Dijkstra from the cells `value` already holds below the sentinel (the goal at 0, a border seeded from the
    /// ring), over a `side` x `side` grid's eight-neighbour graph with the learner's edge rule, into `value` (the
    /// fixed point the learner's iteration converges to); done once the `wanted` cells (a flag a cell; null: every
    /// cell) are settled.
    void Settle(float const* factor, int32_t side, float cell, float* value, uint8_t const* wanted,
        int32_t wantedCount)
    {
        int32_t const cells = side * side;
        thread_local std::vector<Entry> heap;
        thread_local std::vector<uint8_t> settled;
        heap.clear();
        settled.assign(std::size_t(cells), 0);
        for (int32_t index = 0; index < cells; ++index)
            if (value[index] < PLAN_SENTINEL)
                heap.push_back({ value[index], index });
        std::make_heap(heap.begin(), heap.end(), std::greater<Entry>());
        int32_t left = wanted ? wantedCount : cells;
        while (!heap.empty() && left > 0)
        {
            std::pop_heap(heap.begin(), heap.end(), std::greater<Entry>());
            Entry const top = heap.back();
            heap.pop_back();
            if (top.Value > value[top.Index] || settled[std::size_t(top.Index)])
                continue;               // a stale entry: the cell was settled lower since
            settled[std::size_t(top.Index)] = 1;
            if (!wanted || wanted[top.Index])
                --left;
            int32_t const row = top.Index / side;
            int32_t const col = top.Index % side;
            float const half = 0.5f * cell * factor[top.Index];
            for (int32_t dir = 0; dir < 8; ++dir)
            {
                int32_t const nr = row + DR[dir];
                int32_t const nc = col + DC[dir];
                if (nr < 0 || nc < 0 || nr >= side || nc >= side)
                    continue;
                int32_t const next = nr * side + nc;
                float const candidate = top.Value + DIST[dir] * (half + 0.5f * cell * factor[next]);
                if (candidate < value[next])
                {
                    value[next] = candidate;
                    heap.push_back({ candidate, next });
                    std::push_heap(heap.begin(), heap.end(), std::greater<Entry>());
                }
            }
        }
    }

    /// The coarse `values` sampled bilinearly at crop cell coordinates (row, col), each clamped to the crop
    /// (grid_sample with align_corners and border padding, as the learner's coarse_frame).
    [[nodiscard]] float SampleCoarse(float const* values, float row, float col)
    {
        row = std::clamp(row, 0.0f, float(COARSE_SIDE - 1));
        col = std::clamp(col, 0.0f, float(COARSE_SIDE - 1));
        int32_t const r0 = std::min(COARSE_SIDE - 2, int32_t(std::floor(row)));
        int32_t const c0 = std::min(COARSE_SIDE - 2, int32_t(std::floor(col)));
        float const fr = row - float(r0);
        float const fc = col - float(c0);
        float const top = values[r0 * COARSE_SIDE + c0] * (1.0f - fc) + values[r0 * COARSE_SIDE + c0 + 1] * fc;
        float const bottom = values[(r0 + 1) * COARSE_SIDE + c0] * (1.0f - fc)
            + values[(r0 + 1) * COARSE_SIDE + c0 + 1] * fc;
        return top * (1.0f - fr) + bottom * fr;
    }

    /// The learner's descend over a `side` x `side` grid from cell (row, col): `steps` moves at most, each to the
    /// lowest neighbour (the first of equals) and only when strictly lower than the cell left. The cell reached.
    void Descend(float const* value, int32_t side, uint32_t steps, int32_t& row, int32_t& col)
    {
        for (uint32_t step = 0; step < steps; ++step)
        {
            float const here = value[row * side + col];
            float best = here;
            int32_t bestDir = -1;
            for (int32_t dir = 0; dir < 8; ++dir)
            {
                int32_t const nr = row + DR[dir];
                int32_t const nc = col + DC[dir];
                if (nr < 0 || nc < 0 || nr >= side || nc >= side)
                    continue;
                float const candidate = value[nr * side + nc];
                if (candidate < best)
                {
                    best = candidate;
                    bestDir = dir;
                }
            }
            if (bestDir < 0)
                return;
            row += DR[bestDir];
            col += DC[bestDir];
        }
    }

    /// A waypoint from the body to (`forward`, `right`) yards in its frame, `reach` yards the walk's most.
    [[nodiscard]] PlanWaypoint Waypoint(float forward, float right, float reach)
    {
        float const distance = std::max(1e-6f, std::hypot(forward, right));
        return PlanWaypoint{ -right / distance, forward / distance, distance / reach, 1.0f };
    }
}

float Animus::Vision::CellFactor(MapCell const& cell, float z)
{
    MapCode code = CodeOf(cell, z);
    if (code == MapCode::Floor || code == MapCode::Door)
    {
        float floor = 0.0f;
        if (NearestFloor(cell, z, floor) && std::fabs(floor - z) > PLAN_HEIGHT_BAND)
            code = MapCode::Unknown;
    }
    return PLAN_COSTS[uint8_t(code)];
}

void Animus::Vision::ConfigurationSpace(float* factor, int32_t side)
{
    // The walls as read, on a grid padded by one cell of no-wall all round (the window's edge is no wall: what lies
    // past it is not known to be one), so a neighbour read needs no bounds check.
    int32_t const padded = side + 2;
    thread_local std::vector<uint8_t> wall;
    wall.assign(std::size_t(padded * padded), 0);
    auto const mark = [&]()
    {
        for (int32_t row = 0; row < side; ++row)
        {
            float const* from = factor + row * side;
            uint8_t* to = wall.data() + (row + 1) * padded + 1;
            for (int32_t col = 0; col < side; ++col)
                to[col] = from[col] >= PLAN_BLOCKED ? 1 : 0;
        }
    };
    mark();
    // Hole closing: a cell walled on two opposite sides is a wall -- judged on the walls as read, so a closed hole
    // does not close its neighbour in turn (two cells between walls are a 2-yd passage, which the body fits).
    for (int32_t row = 0; row < side; ++row)
        for (int32_t col = 0; col < side; ++col)
        {
            uint8_t const* at = wall.data() + (row + 1) * padded + col + 1;
            if (*at)
                continue;
            bool const closed = (at[-padded] && at[padded]) || (at[-1] && at[1])
                || (at[-padded - 1] && at[padded + 1]) || (at[-padded + 1] && at[padded - 1]);
            if (closed)
                factor[row * side + col] = PLAN_BLOCKED;
        }
    mark();
    // Inflation: a passable cell 8-adjacent to a wall (the closed holes included) costs PLAN_INFLATED at least, one
    // of those with a wall within two cells on both sides along an axis or a diagonal (a 2-yd passage) PLAN_PINCHED;
    // dearer cells (unknown, a hazard) keep their own. Read two cells out, past the one cell of padding: the bounds
    // are checked.
    auto const wallAt = [&](int32_t row, int32_t col)
    {
        if (row < 0 || col < 0 || row >= side || col >= side)
            return false;
        return wall[std::size_t((row + 1) * padded + col + 1)] != 0;
    };
    constexpr int32_t AXES[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };
    for (int32_t row = 0; row < side; ++row)
        for (int32_t col = 0; col < side; ++col)
        {
            float& value = factor[row * side + col];
            uint8_t const* at = wall.data() + (row + 1) * padded + col + 1;
            if (*at || value >= PLAN_PINCHED)
                continue;
            if (!(at[-padded - 1] | at[-padded] | at[-padded + 1] | at[-1] | at[1] | at[padded - 1] | at[padded]
                | at[padded + 1]))
                continue;
            bool pinched = false;
            for (auto const& axis : AXES)
            {
                bool const one = wallAt(row + axis[0], col + axis[1]) || wallAt(row + 2 * axis[0], col + 2 * axis[1]);
                bool const other = wallAt(row - axis[0], col - axis[1])
                    || wallAt(row - 2 * axis[0], col - 2 * axis[1]);
                pinched = pinched || (one && other);
            }
            value = std::max(value, pinched ? PLAN_PINCHED : PLAN_INFLATED);
        }
}

void Animus::Vision::PlanOnMap(MentalMap const& map, float x, float y, float z, float yaw, float goalForward,
    float goalRight, uint8_t const* coarse, MapPlan& out, PlanPoint const* near, uint32_t nearCount)
{
    thread_local std::vector<float> factor(PLAN_CELLS);
    thread_local std::vector<uint8_t> wanted(PLAN_CELLS);
    thread_local std::vector<MapCell> window(PLAN_CELLS);
    out.Fine.fill(PLAN_SENTINEL);
    out.Coarse.fill(PLAN_SENTINEL);
    out.X0 = MapCellOf(x) - PLAN_HALF;
    out.Y0 = MapCellOf(y) - PLAN_HALF;
    out.X = x;
    out.Y = y;
    out.Yaw = yaw;
    out.HasCoarse = coarse != nullptr;
    out.HasGoal = true;
    // The body's frame: forward along (cos yaw, sin yaw), right along (sin yaw, -cos yaw) (WoW's yaw turns left).
    float const forwardX = std::cos(yaw);
    float const forwardY = std::sin(yaw);
    float const rightX = forwardY;
    float const rightY = -forwardX;

    // The coarse tier first: its goal cell clamped to the border along the line from the body (goal_plane with its
    // clamp; round to nearest, ties to even, as torch.round).
    if (coarse)
    {
        float const cell = float(COARSE_CELL);
        CoarseFactors(coarse, factor.data());
        float const f = goalForward / cell;
        float const r = goalRight / cell;
        float const reach = std::max(std::max(std::fabs(f), std::fabs(r)), 1e-6f);
        float const t = std::min(1.0f, COARSE_CENTRE / reach);
        int32_t const row = std::clamp(int32_t(std::nearbyint(COARSE_CENTRE - t * f)), 0, COARSE_SIDE - 1);
        int32_t const col = std::clamp(int32_t(std::nearbyint(COARSE_CENTRE + t * r)), 0, COARSE_SIDE - 1);
        out.Coarse[std::size_t(row * COARSE_SIDE + col)] = 0.0f;
        // Only the block the window's ring samples is needed: the ring's cells lie within (PLAN_HALF + 1) x sqrt 2
        // yards of the body whichever way it faces, and a bilinear sample reads the cells either side.
        float const span = (float(PLAN_HALF) + 1.0f) * 1.41422f / cell;
        int32_t const low = std::max(0, int32_t(std::floor(COARSE_CENTRE - span)));
        int32_t const high = std::min(COARSE_SIDE - 1, int32_t(std::ceil(COARSE_CENTRE + span)));
        std::fill(wanted.begin(), wanted.begin() + COARSE_CELLS, uint8_t(0));
        int32_t count = 0;
        // A blocked cell's own value sits a hundred yards over its neighbours' and would hold the search until
        // nearly everything had settled; the ring reads it as it is.
        for (int32_t cr = low; cr <= high; ++cr)
            for (int32_t cc = low; cc <= high; ++cc)
                if (factor[cr * COARSE_SIDE + cc] < PLAN_BLOCKED)
                {
                    wanted[std::size_t(cr * COARSE_SIDE + cc)] = 1;
                    ++count;
                }
        Settle(factor.data(), COARSE_SIDE, cell, out.Coarse.data(), wanted.data(), count);
    }

    // The fine tier: the map's own cells, each priced for the feet.
    map.Window(out.X0, out.Y0, PLAN_SIDE, window.data());
    for (uint32_t index = 0; index < PLAN_CELLS; ++index)
        factor[index] = CellFactor(window[index], z);
    // ... shaped into the body's configuration space: the 1-yd holes closed, the cells along the walls dear.
    ConfigurationSpace(factor.data(), PLAN_SIDE);
    for (uint32_t index = 0; index < PLAN_CELLS; ++index)
    {
        out.FineFactor[index] = factor[index];
        out.FineBlocked[index] = factor[index] >= PLAN_BLOCKED ? 1 : 0;
    }
    // The goal's own cell when it lies inside the window (goal_plane without the clamp).
    {
        float const gx = x + goalForward * forwardX + goalRight * rightX;
        float const gy = y + goalForward * forwardY + goalRight * rightY;
        int32_t const col = MapCellOf(gx) - out.X0;
        int32_t const row = MapCellOf(gy) - out.Y0;
        if (row >= 0 && row < PLAN_SIDE && col >= 0 && col < PLAN_SIDE)
            out.Fine[std::size_t(row * PLAN_SIDE + col)] = 0.0f;
    }
    // The ring just past the window's border, seeded from the coarse value at each ring cell's centre turned into
    // the crop's frame (coarse_frame), a step onto it priced at the border cell's own factor.
    if (coarse)
    {
        auto const ring = [&](int32_t row, int32_t col)
        {
            float const dx = (float(out.X0 + col) + 0.5f) * MAP_CELL - x;
            float const dy = (float(out.Y0 + row) + 0.5f) * MAP_CELL - y;
            float const forward = dx * forwardX + dy * forwardY;
            float const right = dx * rightX + dy * rightY;
            return std::min(PLAN_SENTINEL, SampleCoarse(out.Coarse.data(), COARSE_CENTRE - forward / float(COARSE_CELL),
                COARSE_CENTRE + right / float(COARSE_CELL)));
        };
        for (int32_t row = 0; row < PLAN_SIDE; ++row)
            for (int32_t col = 0; col < PLAN_SIDE; ++col)
            {
                if (row != 0 && row != PLAN_SIDE - 1 && col != 0 && col != PLAN_SIDE - 1)
                    continue;
                float& value = out.Fine[std::size_t(row * PLAN_SIDE + col)];
                for (int32_t dir = 0; dir < 8; ++dir)
                {
                    int32_t const nr = row + DR[dir];
                    int32_t const nc = col + DC[dir];
                    if (nr >= 0 && nc >= 0 && nr < PLAN_SIDE && nc < PLAN_SIDE)
                        continue;
                    value = std::min(value, ring(nr, nc) + DIST[dir] * MAP_CELL * factor[row * PLAN_SIDE + col]);
                }
            }
    }
    // The cells round the body and each named point (the ones PlanValueAt reads and the waypoint's walk starts
    // from; blocked ones it never reads).
    int32_t count = 0;
    std::fill(wanted.begin(), wanted.end(), uint8_t(0));
    auto const want = [&](float forward, float right)
    {
        float const px = x + forward * forwardX + right * rightX;
        float const py = y + forward * forwardY + right * rightY;
        // The four cells whose centres surround the point.
        int32_t const c0 = int32_t(std::floor(px / MAP_CELL - 0.5f)) - out.X0;
        int32_t const r0 = int32_t(std::floor(py / MAP_CELL - 0.5f)) - out.Y0;
        for (int32_t r = r0; r <= r0 + 1; ++r)
            for (int32_t c = c0; c <= c0 + 1; ++c)
                if (r >= 0 && c >= 0 && r < PLAN_SIDE && c < PLAN_SIDE && !wanted[std::size_t(r * PLAN_SIDE + c)]
                    && factor[r * PLAN_SIDE + c] < PLAN_BLOCKED)
                {
                    wanted[std::size_t(r * PLAN_SIDE + c)] = 1;
                    ++count;
                }
    };
    want(0.0f, 0.0f);
    for (uint32_t index = 0; index < nearCount; ++index)
        want(near[index].Forward, near[index].Right);
    Settle(factor.data(), PLAN_SIDE, MAP_CELL, out.Fine.data(), wanted.data(), count);
}

float Animus::Vision::PlanValueAt(MapPlan const& plan, float forward, float right)
{
    float const forwardX = std::cos(plan.Yaw);
    float const forwardY = std::sin(plan.Yaw);
    float const px = plan.X + forward * forwardX + right * forwardY;
    float const py = plan.Y + forward * forwardY - right * forwardX;
    // Cell (row, col)'s centre is at window coordinates (col + 0.5, row + 0.5).
    float const col = px / MAP_CELL - float(plan.X0) - 0.5f;
    float const row = py / MAP_CELL - float(plan.Y0) - 0.5f;
    if (row < -0.5f || col < -0.5f || row > float(PLAN_SIDE) - 0.5f || col > float(PLAN_SIDE) - 0.5f)
        return TRAV_UNREACHABLE;
    int32_t const r0 = int32_t(std::floor(row));
    int32_t const c0 = int32_t(std::floor(col));
    float const fr = row - float(r0);
    float const fc = col - float(c0);
    float sum = 0.0f;
    float weight = 0.0f;
    for (int32_t dr = 0; dr < 2; ++dr)
        for (int32_t dc = 0; dc < 2; ++dc)
        {
            int32_t const r = r0 + dr;
            int32_t const c = c0 + dc;
            if (r < 0 || c < 0 || r >= PLAN_SIDE || c >= PLAN_SIDE)
                continue;
            std::size_t const index = std::size_t(r * PLAN_SIDE + c);
            if (plan.FineBlocked[index] || plan.Fine[index] >= PLAN_SENTINEL)
                continue;
            float const w = (dr ? fr : 1.0f - fr) * (dc ? fc : 1.0f - fc);
            sum += w * plan.Fine[index];
            weight += w;
        }
    if (weight <= 1e-6f)
        return TRAV_UNREACHABLE;
    return std::min(sum / weight, TRAV_UNREACHABLE);
}

Animus::Vision::PlanWalk Animus::Vision::FineWalk(MapPlan const& plan)
{
    PlanWalk walk;
    if (!plan.HasGoal)
        return walk;
    // The body's own cell; a wall's foot (blocked) stands in for nothing, so the lowest unblocked cell round the body
    // starts the walk instead.
    int32_t row = MapCellOf(plan.Y) - plan.Y0;
    int32_t col = MapCellOf(plan.X) - plan.X0;
    if (plan.FineBlocked[std::size_t(row * PLAN_SIDE + col)])
    {
        float best = PLAN_SENTINEL;
        int32_t const c0 = int32_t(std::floor(plan.X / MAP_CELL - 0.5f)) - plan.X0;
        int32_t const r0 = int32_t(std::floor(plan.Y / MAP_CELL - 0.5f)) - plan.Y0;
        for (int32_t r = r0; r <= r0 + 1; ++r)
            for (int32_t c = c0; c <= c0 + 1; ++c)
            {
                if (r < 0 || c < 0 || r >= PLAN_SIDE || c >= PLAN_SIDE)
                    continue;
                std::size_t const index = std::size_t(r * PLAN_SIDE + c);
                if (!plan.FineBlocked[index] && plan.Fine[index] < best)
                {
                    best = plan.Fine[index];
                    row = r;
                    col = c;
                }
            }
    }
    walk.X[0] = plan.X0 + col;
    walk.Y[0] = plan.Y0 + row;
    walk.Count = 1;
    // One step at a time, downhill, until PLAN_FINE_STEPS or no lower neighbour.
    while (walk.Count < PLAN_KEPT_CELLS)
    {
        int32_t const fromRow = row;
        int32_t const fromCol = col;
        Descend(plan.Fine.data(), PLAN_SIDE, 1, row, col);
        if (row == fromRow && col == fromCol)
            break;
        walk.X[walk.Count] = plan.X0 + col;
        walk.Y[walk.Count] = plan.Y0 + row;
        ++walk.Count;
    }
    return walk;
}

Animus::Vision::PlanRoute Animus::Vision::WalkRoute(MapPlan const& plan, PlanWalk const& walk)
{
    // No step, or a walk that ends on the body's own cell, is no route.
    if (walk.Count < 2)
        return PlanRoute();
    uint32_t const steps = walk.Count - 1;
    if (walk.X[steps] == MapCellOf(plan.X) && walk.Y[steps] == MapCellOf(plan.Y))
        return PlanRoute();
    float const forwardX = std::cos(plan.Yaw);
    float const forwardY = std::sin(plan.Yaw);
    PlanRoute route;
    for (uint32_t point = 0; point < PLAN_ROUTE_POINTS; ++point)
    {
        // The cell after PLAN_ROUTE_STEPS[point] steps; a walk that ended earlier, its end.
        uint32_t const index = std::min(PLAN_ROUTE_STEPS[point], steps);
        float const dx = (float(walk.X[index]) + 0.5f) * MAP_CELL - plan.X;
        float const dy = (float(walk.Y[index]) + 0.5f) * MAP_CELL - plan.Y;
        float const forward = dx * forwardX + dy * forwardY;
        float const right = dx * forwardY - dy * forwardX;
        float const distance = std::max(1e-6f, std::hypot(forward, right));
        route.Sin[point] = -right / distance;
        route.Cos[point] = forward / distance;
    }
    route.Reach = float(steps) / float(PLAN_FINE_STEPS);
    route.Valid = 1.0f;
    return route;
}

Animus::Vision::PlanRoute Animus::Vision::FineRoute(MapPlan const& plan)
{
    return WalkRoute(plan, FineWalk(plan));
}

bool Animus::Vision::KeptEnd(PlanWalk const& kept, float x, float y, float yaw, PlanPoint& out)
{
    if (!kept.Count)
        return false;
    float const dx = (float(kept.X[kept.Count - 1]) + 0.5f) * MAP_CELL - x;
    float const dy = (float(kept.Y[kept.Count - 1]) + 0.5f) * MAP_CELL - y;
    out = PlanPoint{ dx * std::cos(yaw) + dy * std::sin(yaw), dx * std::sin(yaw) - dy * std::cos(yaw) };
    return true;
}

Animus::Vision::RouteChoice Animus::Vision::KeepRoute(MapPlan const& plan, PlanWalk const& kept,
    RouteKeeping const& keeping)
{
    RouteChoice choice;
    PlanWalk const fresh = FineWalk(plan);
    auto const indexOf = [&plan](int32_t wx, int32_t wy, int32_t& index)
    {
        int32_t const col = wx - plan.X0;
        int32_t const row = wy - plan.Y0;
        if (row < 0 || col < 0 || row >= PLAN_SIDE || col >= PLAN_SIDE)
            return false;
        index = row * PLAN_SIDE + col;
        return true;
    };
    auto const chebyshev = [](int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        return std::max(std::abs(ax - bx), std::abs(ay - by));
    };
    float freshCost = TRAV_UNREACHABLE;
    int32_t startIndex = 0;
    if (fresh.Count && indexOf(fresh.X[0], fresh.Y[0], startIndex)
        && plan.Fine[std::size_t(startIndex)] < PLAN_SENTINEL)
        freshCost = std::min(plan.Fine[std::size_t(startIndex)], TRAV_UNREACHABLE);

    // The kept walk from its cell nearest the body's start, re-priced on this plan and extended on it.
    PlanWalk follow;
    bool onKept = false;
    bool blocked = false;
    float keptCost = TRAV_UNREACHABLE;
    uint32_t nearest = 0;
    if (kept.Count && fresh.Count)
    {
        int32_t const sx = fresh.X[0];
        int32_t const sy = fresh.Y[0];
        // The join: the furthest-along cell of it within PLAN_KEEP_OFF of the start, so the walk never steps back to
        // where the body was; a join two cells off only past an open middle cell (no step through a 1-yd wall).
        auto const open = [&](int32_t wx, int32_t wy)
        {
            int32_t cell = 0;
            return indexOf(wx, wy, cell) && !plan.FineBlocked[std::size_t(cell)];
        };
        for (uint32_t index = kept.Count; index-- > 0 && !onKept;)
        {
            int32_t const jx = kept.X[index];
            int32_t const jy = kept.Y[index];
            int32_t const d = chebyshev(jx, jy, sx, sy);
            if (d > PLAN_KEEP_OFF)
                continue;
            if (d > 1)
            {
                int32_t const mx = sx + jx;
                int32_t const my = sy + jy;
                if (!open(mx >> 1, my >> 1) || !open((mx + 1) >> 1, (my + 1) >> 1))
                    continue;
            }
            nearest = index;
            onKept = true;
        }
    }
    if (onKept)
    {
        follow.X[0] = fresh.X[0];
        follow.Y[0] = fresh.Y[0];
        follow.Count = 1;
        int32_t previous = startIndex;
        float cost = 0.0f;
        for (uint32_t index = nearest; index < kept.Count && follow.Count < PLAN_KEPT_CELLS; ++index)
        {
            int32_t const wx = kept.X[index];
            int32_t const wy = kept.Y[index];
            if (wx == follow.X[follow.Count - 1] && wy == follow.Y[follow.Count - 1])
                continue;
            int32_t cell = 0;
            if (!indexOf(wx, wy, cell) || plan.FineBlocked[std::size_t(cell)])
            {
                blocked = true;
                break;
            }
            // The Dijkstra's edge rule: the step's yards x the mean of its two cells' factors.
            float const yards = MAP_CELL
                * std::hypot(float(wx - follow.X[follow.Count - 1]), float(wy - follow.Y[follow.Count - 1]));
            cost += yards * 0.5f * (plan.FineFactor[std::size_t(previous)] + plan.FineFactor[std::size_t(cell)]);
            follow.X[follow.Count] = wx;
            follow.Y[follow.Count] = wy;
            ++follow.Count;
            previous = cell;
        }
        float const endValue = plan.Fine[std::size_t(previous)];
        if (!blocked && endValue < PLAN_SENTINEL)
        {
            keptCost = std::min(cost + endValue, TRAV_UNREACHABLE);
            // Extended downhill on this plan from its end (settled: the end was a `near` point).
            int32_t row = previous / PLAN_SIDE;
            int32_t col = previous % PLAN_SIDE;
            while (follow.Count < PLAN_KEPT_CELLS)
            {
                int32_t const fromRow = row;
                int32_t const fromCol = col;
                Descend(plan.Fine.data(), PLAN_SIDE, 1, row, col);
                if (row == fromRow && col == fromCol)
                    break;
                follow.X[follow.Count] = plan.X0 + col;
                follow.Y[follow.Count] = plan.Y0 + row;
                ++follow.Count;
            }
        }
        else
            blocked = true;
    }

    // Kept unless the new walk undercuts it by more than the margin (both parts), or it is blocked.
    bool const keep = keeping.Enabled && onKept && !blocked && keptCost < TRAV_UNREACHABLE
        && !(freshCost < keptCost - std::max(keeping.Share * keptCost, keeping.Yards));
    choice.Kept = keep;
    choice.Walk = keep ? follow : fresh;
    choice.Cost = keep ? keptCost : freshCost;
    choice.Route = WalkRoute(plan, choice.Walk);
    if (keep || !onKept || fresh.Count < 2)
        return choice;
    // The new walk followed instead: a switch when it leaves the old one -- a cell of it, as far along as the old one
    // reaches, more than PLAN_KEEP_OFF cells from every cell of the old (the kept walk as extended, or what is left
    // of it from the body when it was blocked).
    PlanWalk const& old = blocked ? kept : follow;
    uint32_t const first = blocked ? nearest : 0;
    uint32_t const reach = std::min(fresh.Count, old.Count - first);
    for (uint32_t index = 0; index < reach && !choice.Switched; ++index)
    {
        bool near = false;
        for (uint32_t other = first; other < old.Count && !near; ++other)
            near = chebyshev(fresh.X[index], fresh.Y[index], old.X[other], old.Y[other]) <= PLAN_KEEP_OFF;
        choice.Switched = !near;
    }
    return choice;
}

void Animus::Vision::RouteStall(float cost, bool reachable, float seconds, bool& hasBest, float& best, float& stall)
{
    stall += std::max(0.0f, seconds);
    if (reachable && (!hasBest || cost < best))
    {
        hasBest = true;
        best = cost;
        stall = 0.0f;
    }
}

Animus::Vision::PlanWaypoint Animus::Vision::CoarseWaypoint(MapPlan const& plan)
{
    if (!plan.HasGoal || !plan.HasCoarse)
        return PlanWaypoint();
    int32_t row = PLAN_COARSE_ANCHOR;
    int32_t col = PLAN_COARSE_ANCHOR;
    Descend(plan.Coarse.data(), COARSE_SIDE, PLAN_COARSE_STEPS, row, col);
    if (row == PLAN_COARSE_ANCHOR && col == PLAN_COARSE_ANCHOR)
        return PlanWaypoint();
    // The crop's frame is the body's: row 0 the furthest ahead, column 0 the furthest left.
    float const forward = (COARSE_CENTRE - float(row)) * float(COARSE_CELL);
    float const right = (float(col) - COARSE_CENTRE) * float(COARSE_CELL);
    return Waypoint(forward, right, float(PLAN_COARSE_STEPS) * float(COARSE_CELL));
}
