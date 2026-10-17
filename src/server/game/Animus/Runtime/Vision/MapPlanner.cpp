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

    /// A 1-yd map cell's step cost multiple for feet at `z`: its code, a floor or door whose layer nearest the feet
    /// is further than PLAN_HEIGHT_BAND off them priced as unknown.
    [[nodiscard]] float CellFactor(MapCell const& cell, float z)
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
    {
        factor[index] = CellFactor(window[index], z);
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

Animus::Vision::PlanWaypoint Animus::Vision::FineWaypoint(MapPlan const& plan)
{
    if (!plan.HasGoal)
        return PlanWaypoint();
    // The body's own cell; a wall's foot (blocked) stands in for nothing, so the lowest unblocked cell round the body
    // starts the walk instead.
    int32_t const bodyCol = MapCellOf(plan.X) - plan.X0;
    int32_t const bodyRow = MapCellOf(plan.Y) - plan.Y0;
    int32_t row = bodyRow;
    int32_t col = bodyCol;
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
    Descend(plan.Fine.data(), PLAN_SIDE, PLAN_FINE_STEPS, row, col);
    if (row == bodyRow && col == bodyCol)
        return PlanWaypoint();
    float const dx = (float(plan.X0 + col) + 0.5f) * MAP_CELL - plan.X;
    float const dy = (float(plan.Y0 + row) + 0.5f) * MAP_CELL - plan.Y;
    float const forwardX = std::cos(plan.Yaw);
    float const forwardY = std::sin(plan.Yaw);
    return Waypoint(dx * forwardX + dy * forwardY, dx * forwardY - dy * forwardX, float(PLAN_FINE_STEPS) * MAP_CELL);
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
