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

    constexpr int32_t SIDE = int32_t(CROP);
    constexpr int32_t CELLS = SIDE * SIDE;
    /// The crop's centre in cell coordinates: the body, at the corner of the four middle cells (goal_plane's centre).
    constexpr float CENTRE = float(SIDE - 1) * 0.5f;
    /// The eight neighbours as (row, column) offsets, and the yards of each step per yard of cell.
    constexpr int32_t DR[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    constexpr int32_t DC[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    constexpr float DIST[8] = { 1.4142136f, 1.0f, 1.4142136f, 1.0f, 1.0f, 1.4142136f, 1.0f, 1.4142136f };

    /// Each crop cell's step cost multiple (PLAN_COSTS by its code; a floor or door further than `band` off the
    /// feet priced as unknown).
    void Factors(uint8_t const* crop, float band, float* factor)
    {
        for (int32_t index = 0; index < CELLS; ++index)
        {
            uint8_t const* bytes = crop + std::size_t(index) * CROP_CHANNELS;
            uint32_t code = std::min<uint32_t>(bytes[CROP_CODE], MAP_CODES - 1);
            if ((code == uint32_t(MapCode::Floor) || code == uint32_t(MapCode::Door)) && bytes[CROP_HEIGHT])
            {
                float const height = float(int32_t(bytes[CROP_HEIGHT]) - int32_t(CROP_HEIGHT_ZERO)) * CROP_HEIGHT_STEP;
                if (std::fabs(height) > band)
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
    /// ring), over the crop's eight-neighbour graph with the learner's edge rule, into `value` (the fixed point the
    /// learner's iteration converges to); done once the `wanted` cells (a flag a cell; null: every cell) are settled.
    void Settle(float const* factor, float cell, float* value, uint8_t const* wanted, int32_t wantedCount)
    {
        thread_local std::vector<Entry> heap;
        thread_local std::vector<uint8_t> settled;
        heap.clear();
        settled.assign(std::size_t(CELLS), 0);
        for (int32_t index = 0; index < CELLS; ++index)
            if (value[index] < PLAN_SENTINEL)
                heap.push_back({ value[index], index });
        std::make_heap(heap.begin(), heap.end(), std::greater<Entry>());
        int32_t left = wanted ? wantedCount : CELLS;
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
            int32_t const row = top.Index / SIDE;
            int32_t const col = top.Index % SIDE;
            float const half = 0.5f * cell * factor[top.Index];
            for (int32_t dir = 0; dir < 8; ++dir)
            {
                int32_t const nr = row + DR[dir];
                int32_t const nc = col + DC[dir];
                if (nr < 0 || nc < 0 || nr >= SIDE || nc >= SIDE)
                    continue;
                int32_t const next = nr * SIDE + nc;
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

    /// `values` sampled bilinearly at cell coordinates (row, col), each clamped to the crop (grid_sample with
    /// align_corners and border padding, as the learner's coarse_frame).
    [[nodiscard]] float Sample(float const* values, float row, float col)
    {
        row = std::clamp(row, 0.0f, float(SIDE - 1));
        col = std::clamp(col, 0.0f, float(SIDE - 1));
        int32_t const r0 = std::min(SIDE - 2, int32_t(std::floor(row)));
        int32_t const c0 = std::min(SIDE - 2, int32_t(std::floor(col)));
        float const fr = row - float(r0);
        float const fc = col - float(c0);
        float const top = values[r0 * SIDE + c0] * (1.0f - fc) + values[r0 * SIDE + c0 + 1] * fc;
        float const bottom = values[(r0 + 1) * SIDE + c0] * (1.0f - fc) + values[(r0 + 1) * SIDE + c0 + 1] * fc;
        return top * (1.0f - fr) + bottom * fr;
    }
}

void Animus::Vision::PlanOnCrops(uint8_t const* fine, uint8_t const* coarse, float goalForward, float goalRight,
    MapPlan& out, PlanPoint const* near, uint32_t nearCount)
{
    thread_local std::vector<float> factor(CELLS);
    thread_local std::vector<uint8_t> wanted(CELLS);
    out.Fine.fill(PLAN_SENTINEL);
    out.Coarse.fill(PLAN_SENTINEL);

    // The coarse tier first: its goal cell clamped to the border along the line from the body (goal_plane with its
    // clamp; round to nearest, ties to even, as torch.round).
    if (coarse)
    {
        float const cell = float(COARSE_CELL);
        Factors(coarse, PLAN_HEIGHT_BAND, factor.data());
        float const f = goalForward / cell;
        float const r = goalRight / cell;
        float const reach = std::max(std::max(std::fabs(f), std::fabs(r)), 1e-6f);
        float const t = std::min(1.0f, CENTRE / reach);
        int32_t const row = std::clamp(int32_t(std::nearbyint(CENTRE - t * f)), 0, SIDE - 1);
        int32_t const col = std::clamp(int32_t(std::nearbyint(CENTRE + t * r)), 0, SIDE - 1);
        out.Coarse[std::size_t(row * SIDE + col)] = 0.0f;
        // Only the block the fine ring samples is needed: the ring's coarse coordinates run from CENTRE - 24.5 x
        // ratio to CENTRE + 24.5 x ratio, and a bilinear sample reads the cells either side.
        float const ratio = CROP_CELL / cell;
        int32_t const low = std::max(0, int32_t(std::floor(CENTRE - (CENTRE + 1.0f) * ratio)));
        int32_t const high = std::min(SIDE - 1, int32_t(std::ceil(CENTRE + (CENTRE + 1.0f) * ratio)));
        std::fill(wanted.begin(), wanted.end(), uint8_t(0));
        int32_t count = 0;
        // A blocked cell's own value sits a hundred yards over its neighbours' and would hold the search until
        // nearly everything had settled; the ring reads it as it is.
        for (int32_t r = low; r <= high; ++r)
            for (int32_t c = low; c <= high; ++c)
                if (factor[r * SIDE + c] < PLAN_BLOCKED)
                {
                    wanted[std::size_t(r * SIDE + c)] = 1;
                    ++count;
                }
        Settle(factor.data(), cell, out.Coarse.data(), wanted.data(), count);
    }

    // The fine tier: the goal's own cell when it lies inside the crop (goal_plane without the clamp), and the ring
    // just past the border seeded from the coarse value (coarse_frame: the coarse row 23.5 + (i - 23.5) x fine /
    // coarse at ring row i), a step onto it priced at the border cell's own factor.
    float const cell = CROP_CELL;
    Factors(fine, PLAN_HEIGHT_BAND, factor.data());
    for (int32_t index = 0; index < CELLS; ++index)
        out.FineBlocked[std::size_t(index)] = factor[index] >= PLAN_BLOCKED ? 1 : 0;
    {
        float const row = std::nearbyint(CENTRE - goalForward / cell);
        float const col = std::nearbyint(CENTRE + goalRight / cell);
        if (row >= 0.0f && row <= float(SIDE - 1) && col >= 0.0f && col <= float(SIDE - 1))
            out.Fine[std::size_t(int32_t(row) * SIDE + int32_t(col))] = 0.0f;
    }
    if (coarse)
    {
        float const ratio = cell / float(COARSE_CELL);
        auto const ring = [&](int32_t i, int32_t j)
        {
            return std::min(PLAN_SENTINEL, Sample(out.Coarse.data(), CENTRE + (float(i) - CENTRE) * ratio,
                CENTRE + (float(j) - CENTRE) * ratio));
        };
        for (int32_t row = 0; row < SIDE; ++row)
            for (int32_t col = 0; col < SIDE; ++col)
            {
                if (row != 0 && row != SIDE - 1 && col != 0 && col != SIDE - 1)
                    continue;
                float& value = out.Fine[std::size_t(row * SIDE + col)];
                for (int32_t dir = 0; dir < 8; ++dir)
                {
                    int32_t const nr = row + DR[dir];
                    int32_t const nc = col + DC[dir];
                    if (nr >= 0 && nc >= 0 && nr < SIDE && nc < SIDE)
                        continue;
                    value = std::min(value, ring(nr, nc) + DIST[dir] * cell * factor[row * SIDE + col]);
                }
            }
    }
    // The cells round each named point (the ones PlanValueAt reads; blocked ones it never reads), else the whole
    // crop.
    if (!near || !nearCount)
    {
        Settle(factor.data(), cell, out.Fine.data(), nullptr, 0);
        return;
    }
    int32_t count = 0;
    std::fill(wanted.begin(), wanted.end(), uint8_t(0));
    for (uint32_t index = 0; index < nearCount; ++index)
    {
        int32_t const r0 = int32_t(std::floor(CENTRE - near[index].Forward / cell));
        int32_t const c0 = int32_t(std::floor(CENTRE + near[index].Right / cell));
        for (int32_t r = r0; r <= r0 + 1; ++r)
            for (int32_t c = c0; c <= c0 + 1; ++c)
                if (r >= 0 && c >= 0 && r < SIDE && c < SIDE && !wanted[std::size_t(r * SIDE + c)]
                    && factor[r * SIDE + c] < PLAN_BLOCKED)
                {
                    wanted[std::size_t(r * SIDE + c)] = 1;
                    ++count;
                }
    }
    Settle(factor.data(), cell, out.Fine.data(), wanted.data(), count);
}

float Animus::Vision::PlanValueAt(MapPlan const& plan, float forward, float right)
{
    // Cell i's centre is (CENTRE - i) x cell yards ahead and (j - CENTRE) x cell to the right.
    float const row = CENTRE - forward / CROP_CELL;
    float const col = CENTRE + right / CROP_CELL;
    if (row < -0.5f || col < -0.5f || row > float(SIDE) - 0.5f || col > float(SIDE) - 0.5f)
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
            if (r < 0 || c < 0 || r >= SIDE || c >= SIDE)
                continue;
            std::size_t const index = std::size_t(r * SIDE + c);
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
