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

#include "SceneBvh.h"
#include <algorithm>
#include <array>
#include <limits>

namespace
{
    namespace Sc = Animus::Vision::Scene;
    using SceneBaker::Triangle;

    constexpr uint32_t BINS = 16;
    /// Past this depth every split is a median split, which bounds the tree's depth under Scene::MAX_DEPTH.
    constexpr uint32_t SAH_DEPTH = 30;

    struct Box
    {
        float Lo[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max() };
        float Hi[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max() };

        void Add(Box const& other)
        {
            for (int i = 0; i < 3; ++i)
            {
                Lo[i] = std::min(Lo[i], other.Lo[i]);
                Hi[i] = std::max(Hi[i], other.Hi[i]);
            }
        }

        [[nodiscard]] float Area() const
        {
            float const dx = Hi[0] - Lo[0];
            float const dy = Hi[1] - Lo[1];
            float const dz = Hi[2] - Lo[2];
            return dx < 0.0f ? 0.0f : 2.0f * (dx * dy + dy * dz + dz * dx);
        }
    };

    class Builder
    {
    public:
        Builder(std::vector<Triangle> const& triangles, SceneBaker::Bvh& out) : _out(out)
        {
            uint32_t const count = uint32_t(triangles.size());
            _boxes.resize(count);
            _centre.resize(count);
            _order.resize(count);
            for (uint32_t t = 0; t < count; ++t)
            {
                _order[t] = t;
                for (int v = 0; v < 3; ++v)
                    for (int i = 0; i < 3; ++i)
                    {
                        _boxes[t].Lo[i] = std::min(_boxes[t].Lo[i], triangles[t].V[v][i]);
                        _boxes[t].Hi[i] = std::max(_boxes[t].Hi[i], triangles[t].V[v][i]);
                    }
                for (int i = 0; i < 3; ++i)
                    _centre[t][i] = 0.5f * (_boxes[t].Lo[i] + _boxes[t].Hi[i]);
            }
        }

        void Run()
        {
            if (_order.empty())
                return;
            Build(0, uint32_t(_order.size()), 0);
            _out.Order = _order;
        }

    private:
        uint32_t Build(uint32_t begin, uint32_t end, uint32_t depth)
        {
            uint32_t const index = uint32_t(_out.Nodes.size());
            _out.Nodes.emplace_back();
            _out.MaxDepth = std::max(_out.MaxDepth, depth);

            Box box;
            Box centres;
            for (uint32_t i = begin; i < end; ++i)
            {
                box.Add(_boxes[_order[i]]);
                for (int a = 0; a < 3; ++a)
                {
                    centres.Lo[a] = std::min(centres.Lo[a], _centre[_order[i]][a]);
                    centres.Hi[a] = std::max(centres.Hi[a], _centre[_order[i]][a]);
                }
            }
            Sc::Node node{};
            for (int a = 0; a < 3; ++a)
            {
                node.Min[a] = box.Lo[a] - SceneBaker::BOX_PAD;
                node.Max[a] = box.Hi[a] + SceneBaker::BOX_PAD;
            }
            if (index == 0)
                for (int a = 0; a < 3; ++a)
                {
                    _out.Min[a] = box.Lo[a];
                    _out.Max[a] = box.Hi[a];
                }

            uint32_t const count = end - begin;
            if (count <= Sc::MAX_LEAF)
            {
                node.A = begin;
                node.B = count;
                _out.Nodes[index] = node;
                return index;
            }

            uint32_t axis = 0;
            uint32_t mid = begin;
            if (depth < SAH_DEPTH)
                mid = SahSplit(begin, end, centres, axis);
            if (mid == begin || mid == end)
                mid = MedianSplit(begin, end, centres, axis);

            Build(begin, mid, depth + 1);
            uint32_t const right = Build(mid, end, depth + 1);
            node.A = right;
            node.B = axis << 16;
            _out.Nodes[index] = node;
            return index;
        }

        /// The binned surface-area-heuristic split: bins the centres along each axis, takes the cheapest cut, and
        /// partitions the range in place (a two-pointer partition by bin, so the result is the same on every machine).
        /// Returns the first index of the right side, or `begin` when no cut separates the triangles.
        uint32_t SahSplit(uint32_t begin, uint32_t end, Box const& centres, uint32_t& axisOut)
        {
            float bestCost = std::numeric_limits<float>::max();
            uint32_t bestAxis = 0;
            uint32_t bestCut = 0;
            bool found = false;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                float const extent = centres.Hi[axis] - centres.Lo[axis];
                if (!(extent > 0.0f))
                    continue;
                Box bins[BINS];
                uint32_t counts[BINS] = {};
                for (uint32_t i = begin; i < end; ++i)
                {
                    uint32_t const b = BinOf(_centre[_order[i]][axis], centres.Lo[axis], extent);
                    bins[b].Add(_boxes[_order[i]]);
                    ++counts[b];
                }
                // Left areas and counts of the cuts after bin k; right ones swept back.
                float leftArea[BINS - 1];
                uint32_t leftCount[BINS - 1];
                Box run;
                uint32_t total = 0;
                for (uint32_t k = 0; k + 1 < BINS; ++k)
                {
                    run.Add(bins[k]);
                    total += counts[k];
                    leftArea[k] = run.Area();
                    leftCount[k] = total;
                }
                Box back;
                uint32_t after = 0;
                for (uint32_t k = BINS - 1; k > 0; --k)
                {
                    back.Add(bins[k]);
                    after += counts[k];
                    uint32_t const cut = k - 1;
                    if (leftCount[cut] == 0 || after == 0)
                        continue;
                    float const cost = leftArea[cut] * float(leftCount[cut]) + back.Area() * float(after);
                    // Ties keep the earlier axis and the lower cut (cuts are visited high to low, so <=).
                    if (cost < bestCost || (cost == bestCost && axis == bestAxis && cut < bestCut))
                    {
                        bestCost = cost;
                        bestAxis = axis;
                        bestCut = cut;
                        found = true;
                    }
                }
            }
            if (!found)
                return begin;
            float const extent = centres.Hi[bestAxis] - centres.Lo[bestAxis];
            uint32_t lo = begin;
            uint32_t hi = end;
            while (lo < hi)
            {
                if (BinOf(_centre[_order[lo]][bestAxis], centres.Lo[bestAxis], extent) <= bestCut)
                    ++lo;
                else
                {
                    --hi;
                    std::swap(_order[lo], _order[hi]);
                }
            }
            axisOut = bestAxis;
            return lo;
        }

        /// Half and half along the widest centre axis, ordered by (centre, input index): a total order.
        uint32_t MedianSplit(uint32_t begin, uint32_t end, Box const& centres, uint32_t& axisOut)
        {
            uint32_t axis = 0;
            for (uint32_t a = 1; a < 3; ++a)
                if (centres.Hi[a] - centres.Lo[a] > centres.Hi[axis] - centres.Lo[axis])
                    axis = a;
            std::sort(_order.begin() + begin, _order.begin() + end, [&](uint32_t x, uint32_t y)
            {
                if (_centre[x][axis] != _centre[y][axis])
                    return _centre[x][axis] < _centre[y][axis];
                return x < y;
            });
            axisOut = axis;
            return begin + (end - begin) / 2;
        }

        static uint32_t BinOf(float value, float low, float extent)
        {
            uint32_t const b = uint32_t((value - low) / extent * float(BINS));
            return b >= BINS ? BINS - 1 : b;
        }

        SceneBaker::Bvh& _out;
        std::vector<Box> _boxes;
        std::vector<std::array<float, 3>> _centre;
        std::vector<uint32_t> _order;
    };
}

SceneBaker::Bvh SceneBaker::BuildBvh(std::vector<Triangle> const& triangles)
{
    Bvh bvh;
    Builder builder(triangles, bvh);
    builder.Run();
    return bvh;
}
