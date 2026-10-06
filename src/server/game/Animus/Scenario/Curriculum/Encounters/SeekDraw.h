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

#ifndef ANIMUS_LIB_CURRICULUM_SEEK_DRAW_H
#define ANIMUS_LIB_CURRICULUM_SEEK_DRAW_H

#include "StageDefinition.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

/// **The seek stage's draws** (SeekEncounter, M2): which room, which object and where in the room, as pure functions
/// of the room table and of uniform numbers in [0, 1), so they are tested with fixed numbers (SeekEncounterTest) and
/// the encounter only supplies the randomness.
namespace Animus::Curriculum::SeekDraw
{
    /// The room ladder's floor: every room's weight is at least this (all rooms are always possible), against the
    /// 1 the favoured end of the ladder gets.
    constexpr float WEIGHT_FLOOR = 0.1f;
    /// Depth tiers the rooms fall in for the episode info (`difficulty`): the nearest third, the middle, the
    /// deepest.
    constexpr uint32 TIERS = 3;

    /// Each room's depth in [0, 1]: its rank by walking distance from the spawn (SeekRoom::Walk), the nearest 0 and the
    /// deepest 1; ties keep the table's order.
    inline std::vector<float> Depths(std::vector<SeekRoom> const& rooms)
    {
        std::vector<uint32> order(rooms.size());
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [&rooms](uint32 a, uint32 b)
        {
            return rooms[a].Walk < rooms[b].Walk;
        });
        std::vector<float> depth(rooms.size(), 0.0f);
        for (std::size_t rank = 0; rank < order.size(); ++rank)
            depth[order[rank]] = order.size() > 1 ? float(rank) / float(order.size() - 1) : 1.0f;
        return depth;
    }

    /// A depth's tier: 0 the nearest third, TIERS - 1 the deepest.
    inline uint32 Tier(float depth)
    {
        return std::min<uint32>(TIERS - 1, uint32(std::max(0.0f, depth) * float(TIERS)));
    }

    /// **The room ladder** (the shaping fade's rungs, perception-goals §4): `ladder` in [0, 1] is how far the fade has
    /// come, 1 - the learner's shaping scale (rungs 1, 0.5, 0.25, 0 read 0, 0.5, 0.75, 1). At 0 the rooms seen from the
    /// hallway are favoured, the nearest at 1 + WEIGHT_FLOOR and the deepest at WEIGHT_FLOOR; at 0.5 every room is as
    /// likely; at 1 the deepest are favoured as the nearest were. Every room keeps at least WEIGHT_FLOOR.
    inline std::vector<float> Weights(std::vector<SeekRoom> const& rooms, float ladder)
    {
        float const t = std::clamp(ladder, 0.0f, 1.0f);
        std::vector<float> weights = Depths(rooms);
        for (float& weight : weights)
            weight = WEIGHT_FLOOR + (1.0f - t) * (1.0f - weight) + t * weight;
        return weights;
    }

    /// The index `u` in [0, 1) falls on, each index taking its weight's share; the last for u at or past the total.
    inline uint32 Pick(std::vector<float> const& weights, float u)
    {
        float const total = std::accumulate(weights.begin(), weights.end(), 0.0f);
        float at = std::clamp(u, 0.0f, 1.0f) * total;
        for (uint32 index = 0; index < weights.size(); ++index)
        {
            if (at < weights[index])
                return index;
            at -= weights[index];
        }
        return weights.empty() ? 0 : uint32(weights.size() - 1);
    }

    /// An evaluation's room and object for seed `seed`: every room in turn, and the object cycling once a lap of the
    /// rooms, so 512 seeds meet each of 39 rooms 13 times and every object in every tier.
    inline std::pair<uint32, uint32> EvaluationPick(uint32 seed, uint32 rooms, uint32 objects)
    {
        rooms = std::max<uint32>(1, rooms);
        return { seed % rooms, (seed / rooms) % std::max<uint32>(1, objects) };
    }

    /// A uniform number in [0, 1) fixed by an evaluation seed and a salt (splitmix64): an evaluation places the object
    /// at the same spot for every checkpoint it scores.
    inline float SeedUniform(uint32 seed, uint32 salt)
    {
        uint64 z = (uint64(seed) << 32 | salt) + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return float(z >> 40) / float(1ull << 24);
    }

    /// Whether (x, y) is inside a convex polygon (either winding; a point on an edge is inside).
    inline bool Inside(std::vector<std::pair<float, float>> const& floor, float x, float y)
    {
        int sign = 0;
        for (std::size_t i = 0; i < floor.size(); ++i)
        {
            auto const& [ax, ay] = floor[i];
            auto const& [bx, by] = floor[(i + 1) % floor.size()];
            float const cross = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
            if (std::fabs(cross) < 1e-6f)
                continue;
            int const side = cross > 0.0f ? 1 : -1;
            if (sign && side != sign)
                return false;
            sign = side;
        }
        return floor.size() >= 3;
    }

    /// The polygon's area, square yards.
    inline float Area(std::vector<std::pair<float, float>> const& floor)
    {
        float twice = 0.0f;
        for (std::size_t i = 0; i < floor.size(); ++i)
        {
            auto const& [ax, ay] = floor[i];
            auto const& [bx, by] = floor[(i + 1) % floor.size()];
            twice += ax * by - bx * ay;
        }
        return std::fabs(twice) * 0.5f;
    }

    /// A point uniform over a convex polygon's area from three uniform numbers: `u` picks a triangle of the fan from
    /// the first corner by its area, `v` and `w` a point uniform in it.
    inline std::pair<float, float> PointIn(std::vector<std::pair<float, float>> const& floor, float u, float v, float w)
    {
        if (floor.size() < 3)
            return floor.empty() ? std::pair<float, float>{ 0.0f, 0.0f } : floor.front();
        std::vector<float> areas;
        for (std::size_t i = 1; i + 1 < floor.size(); ++i)
            areas.push_back(Area({ floor[0], floor[i], floor[i + 1] }));
        std::size_t const tri = Pick(areas, u) + 1;
        float a = std::clamp(v, 0.0f, 1.0f);
        float b = std::clamp(w, 0.0f, 1.0f);
        if (a + b > 1.0f)
        {
            a = 1.0f - a;
            b = 1.0f - b;
        }
        auto const& [x0, y0] = floor[0];
        auto const& [x1, y1] = floor[tri];
        auto const& [x2, y2] = floor[tri + 1];
        return { x0 + a * (x1 - x0) + b * (x2 - x0), y0 + a * (y1 - y0) + b * (y2 - y0) };
    }

    /// The room whose floor (x, y, z) stands on -- inside its polygon, within `rise` yards of its floor's height --
    /// or -1 (the hallways, the doorways and the polygons' margins along the walls).
    inline int32 RoomAt(std::vector<SeekRoom> const& rooms, float x, float y, float z, float rise = 4.0f)
    {
        for (std::size_t index = 0; index < rooms.size(); ++index)
            if (std::fabs(z - rooms[index].FloorZ) <= rise && Inside(rooms[index].Floor, x, y))
                return int32(index);
        return -1;
    }
}

#endif
