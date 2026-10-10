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

#include "MentalMap.h"
#include "StageDefinition.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

/// **The seek stage's draws** (SeekEncounter, M2): which room, which object and where in the room, as pure functions
/// of the room table and of uniform numbers in [0, 1), so they are tested with fixed numbers (SeekEncounterTest) and
/// the encounter only supplies the randomness.
namespace Animus::Curriculum::SeekDraw
{
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

    /// **The placement ladder** (perception-goals REDESIGN §2): where the object stands, in the order the difficulty
    /// comes in -- in the hallway in sight of the spawn, just inside a front cell's opening, anywhere in a front cell,
    /// and deep (the back rooms, the hubs and the end rooms). The rung follows the shaping fade's (SightDraw::Rung of
    /// the scale, the same four scales as M1's withholding ladder), so it steps on the fade's signal -- the rung's own
    /// found rate, its window and regress rules (amendment 7) -- and every rung keeps CarryShare of the one below.
    enum class Rung : uint8
    {
        Hallway = 0,
        Doorway = 1,
        Room = 2,
        Deep = 3,
        Count
    };
    constexpr uint32 RUNGS = uint32(Rung::Count);
    constexpr std::array<char const*, RUNGS> RUNG_NAMES = { "hallway", "doorway", "room", "deep" };

    /// The rung a training episode places by: the ladder's, or with probability `carry` (`u` below it) the one below
    /// (the old skills stay); the first rung has none below.
    inline Rung PlacedRung(Rung ladder, float u, float carry)
    {
        return ladder != Rung::Hallway && u < carry ? Rung(uint8(ladder) - 1) : ladder;
    }

    /// The rooms a rung draws from, in table order: the front cells (opening onto the hallway, not a hub) for the
    /// doorway and room rungs, every other room for the deep rung, none for the hallway.
    inline std::vector<uint32> RungRooms(std::vector<SeekRoom> const& rooms, Rung rung)
    {
        std::vector<uint32> out;
        for (uint32 index = 0; index < rooms.size(); ++index)
            if ((rung == Rung::Doorway || rung == Rung::Room) ? rooms[index].Front
                : rung == Rung::Deep && !rooms[index].Front)
                out.push_back(index);
        return out;
    }

    /// The rooms a rung's evaluation goes round (amendment 9: 78 episodes every 10M): the front cells for the doorway
    /// and room rungs, and every room at the deep rung, the top (each twice in 78). The hallway rung's object stands in
    /// the hallway, so it goes round every room for the episode info's sake only.
    inline std::vector<uint32> EvaluationRooms(std::vector<SeekRoom> const& rooms, Rung rung)
    {
        if (rung == Rung::Doorway || rung == Rung::Room)
            return RungRooms(rooms, rung);
        std::vector<uint32> all(rooms.size());
        std::iota(all.begin(), all.end(), 0u);
        return all;
    }

    /// A rung evaluation's room and object for seed `seed`: the rung's rooms in turn, the object types cycled with
    /// them (seed mod objects), so 78 seeds meet each of 39 rooms twice with two different objects.
    inline std::pair<uint32, uint32> RungEvaluationPick(uint32 seed, std::vector<uint32> const& rooms, uint32 objects)
    {
        uint32 const room = rooms.empty() ? 0 : rooms[seed % rooms.size()];
        return { room, seed % std::max<uint32>(1, objects) };
    }

    /// Just inside a room's opening (the doorway rung): from the opening toward the room's centre, `inside` yards plus
    /// up to `deeper` more by `u`, and up to `spread` yards either side of that line by `v`.
    inline std::pair<float, float> DoorwaySpot(SeekRoom const& room, float inside, float deeper, float spread, float u,
        float v)
    {
        auto const [ox, oy] = room.Opening;
        float dx = room.Centre.first - ox;
        float dy = room.Centre.second - oy;
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-3f)
            return room.Centre;
        dx /= length;
        dy /= length;
        float const along = std::min(length, inside + std::clamp(u, 0.0f, 1.0f) * deeper);
        float const side = (2.0f * std::clamp(v, 0.0f, 1.0f) - 1.0f) * spread;
        return { ox + dx * along - dy * side, oy + dy * along + dx * side };
    }

    inline bool Inside(std::vector<std::pair<float, float>> const& floor, float x, float y);

    /// **Looked into a room** (REDESIGN §2): per room, the frame's cast rays whose hit is a floor (the terrain or a
    /// model, normal z at least Vision::FLOOR_NORMAL, within Vision::WRITE_REACH of the camera) inside its floor
    /// polygon, within `rise` of its floor's height. The frame alone: nothing the mental map remembers.
    /// `visit(room, x, y, z)` is called for each such ray with the room it fell in and the floor point it hit (a ray
    /// falls in the first room that takes it).
    template <typename Visit>
    inline void FloorHits(std::vector<SeekRoom> const& rooms, Vision::FrameHits const& hits, float rise, Visit&& visit)
    {
        // Each room's bounds, so most rays are turned away by four comparisons.
        std::vector<std::array<float, 4>> bounds(rooms.size());
        for (std::size_t index = 0; index < rooms.size(); ++index)
        {
            std::array<float, 4>& box = bounds[index];
            box = { 1e9f, -1e9f, 1e9f, -1e9f };
            for (auto const& [x, y] : rooms[index].Floor)
                box = { std::min(box[0], x), std::max(box[1], x), std::min(box[2], y), std::max(box[3], y) };
        }
        for (Vision::RayHit const& ray : hits.Rays)
        {
            if ((ray.What != Vision::Class::Terrain && ray.What != Vision::Class::Model)
                || ray.NormalZ < Vision::FLOOR_NORMAL || ray.Distance > Vision::WRITE_REACH)
                continue;
            float const x = hits.Camera.X + ray.Dir.X * ray.Distance;
            float const y = hits.Camera.Y + ray.Dir.Y * ray.Distance;
            for (std::size_t index = 0; index < rooms.size(); ++index)
            {
                std::array<float, 4> const& box = bounds[index];
                if (x < box[0] || x > box[1] || y < box[2] || y > box[3]
                    || std::fabs(ray.Z - rooms[index].FloorZ) > rise || !Inside(rooms[index].Floor, x, y))
                    continue;
                visit(uint32(index), x, y, ray.Z);
                break;
            }
        }
    }

    inline std::vector<uint32> FloorRays(std::vector<SeekRoom> const& rooms, Vision::FrameHits const& hits,
        float rise = 4.0f)
    {
        std::vector<uint32> counts(rooms.size(), 0);
        FloorHits(rooms, hits, rise, [&counts](uint32 room, float, float, float) { ++counts[room]; });
        return counts;
    }

    /// The rooms a frame looks into for the first time this episode, from its floor rays per room (FloorRays) --
    /// RoomSeen's, by the episode's own `looked` (amendment 6: never the remembered map, so a map kept from before
    /// takes no aid away) -- marked in it: at least `minRays` of the frame's floor rays on each.
    inline std::vector<uint32> NewlyLookedFrom(std::vector<uint32> const& counts, std::vector<bool>& looked,
        uint32 minRays)
    {
        std::vector<uint32> out;
        if (looked.size() != counts.size())
            looked.assign(counts.size(), false);
        for (uint32 index = 0; index < counts.size(); ++index)
            if (!looked[index] && counts[index] >= std::max<uint32>(1, minRays))
            {
                looked[index] = true;
                out.push_back(index);
            }
        return out;
    }

    inline std::vector<uint32> NewlyLooked(std::vector<SeekRoom> const& rooms, Vision::FrameHits const& hits,
        std::vector<bool>& looked, uint32 minRays)
    {
        return NewlyLookedFrom(FloorRays(rooms, hits), looked, minRays);
    }

    /// The side of a floor cell the room goals count coverage in, yards.
    constexpr float COVER_CELL = 3.0f;

    /// A floor point's cell, as one key (two signed 32-bit indexes).
    inline uint64 CoverCell(float x, float y)
    {
        return uint64(uint32(int32(std::floor(x / COVER_CELL)))) << 32
            | uint64(uint32(int32(std::floor(y / COVER_CELL))));
    }

    /// How many cells of COVER_CELL yards a room's floor polygon covers: the distinct cells of points sampled across it
    /// every half cell, at least 1. The denominator of a room's coverage.
    inline uint32 FloorCells(std::vector<std::pair<float, float>> const& floor)
    {
        if (floor.size() < 3)
            return 1;
        float lowX = 1e9f;
        float highX = -1e9f;
        float lowY = 1e9f;
        float highY = -1e9f;
        for (auto const& [x, y] : floor)
        {
            lowX = std::min(lowX, x);
            highX = std::max(highX, x);
            lowY = std::min(lowY, y);
            highY = std::max(highY, y);
        }
        std::vector<uint64> cells;
        float const step = COVER_CELL * 0.5f;
        for (float x = lowX; x <= highX; x += step)
            for (float y = lowY; y <= highY; y += step)
                if (Inside(floor, x, y))
                    cells.push_back(CoverCell(x, y));
        std::sort(cells.begin(), cells.end());
        cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
        return std::max<uint32>(1, uint32(cells.size()));
    }

    /// How far (x, y) is outside a room's floor polygon, yards: 0 inside it, else the distance to its nearest edge.
    inline float OutsideBy(std::vector<std::pair<float, float>> const& floor, float x, float y)
    {
        if (floor.size() < 3 || Inside(floor, x, y))
            return 0.0f;
        float nearest = 1e9f;
        for (std::size_t i = 0; i < floor.size(); ++i)
        {
            auto const& [ax, ay] = floor[i];
            auto const& [bx, by] = floor[(i + 1) % floor.size()];
            float const ex = bx - ax;
            float const ey = by - ay;
            float const length2 = ex * ex + ey * ey;
            float const along = length2 > 1e-9f
                ? std::clamp(((x - ax) * ex + (y - ay) * ey) / length2, 0.0f, 1.0f) : 0.0f;
            nearest = std::min(nearest, std::hypot(x - (ax + ex * along), y - (ay + ey * along)));
        }
        return nearest;
    }

    /// The episode's length at a placement rung, seconds: `seconds[rung]`.
    inline uint32 RungSeconds(Rung rung, std::array<uint32, RUNGS> const& seconds)
    {
        return seconds[std::min<uint32>(uint32(rung), RUNGS - 1)];
    }

    /// The room whose opening is nearest (x, y) (a hallway object's room, for the episode info).
    inline int32 NearestOpening(std::vector<SeekRoom> const& rooms, float x, float y)
    {
        int32 best = -1;
        float nearest = 0.0f;
        for (uint32 index = 0; index < rooms.size(); ++index)
        {
            float const d = std::hypot(rooms[index].Opening.first - x, rooms[index].Opening.second - y);
            if (best < 0 || d < nearest)
            {
                best = int32(index);
                nearest = d;
            }
        }
        return best;
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

    /// How many episodes one pass of the evaluation sweep is: every (room, object) pair once (39 x 5 = 195).
    inline uint32 SweepLength(uint32 rooms, uint32 objects)
    {
        return std::max<uint32>(1, rooms) * std::max<uint32>(1, objects);
    }

    /// An evaluation's room and object for seed `seed`: a deterministic sweep over every (room, object) pair, seed i
    /// the pair i mod SweepLength -- the rooms in table order (nearest first), each with every object in turn -- so an
    /// evaluation of SweepLength episodes (or a multiple) meets each pair exactly once (or that many times), and the
    /// per-room and per-object tables are balanced. A class (seed mod castings) meets rooms spread over every depth.
    inline std::pair<uint32, uint32> EvaluationPick(uint32 seed, uint32 rooms, uint32 objects)
    {
        objects = std::max<uint32>(1, objects);
        uint32 const pair = seed % SweepLength(rooms, objects);
        return { pair / objects, pair % objects };
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
