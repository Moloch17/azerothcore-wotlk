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

#ifndef ANIMUS_LIB_CURRICULUM_SEEN_PLACES_H
#define ANIMUS_LIB_CURRICULUM_SEEN_PLACES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/// **The places a seat may be sent to in a dungeon, as a player knows them** (dungeon-curriculum G2-D3; the
/// coordinator's ruling, 2026-10-07): the goal head's TravelTo places (WorldView::Places) and its assignment in a sight
/// stage come only from
/// - what the seat has **seen**: the hostiles its entity memory holds (where it last saw them, alive), and the
///   **frontier** of its mental map (known floor beside ground it has not seen);
/// - the dungeon map's **layout** (a 3.3.5 player has the dungeon map): ground nodes with no creature on them, no
///   order and no markers -- only in SeenAndLayout;
/// - the party's **leader** (its frame and its dot on the map).
/// Never a live pack's or boss's position, never the route's "next pack" order: those stay the dungeon teacher's own
/// (a script may know them), in SeatView::Crowd.
///
/// Pure: its inputs are positions and flags, so the tests feed it by hand (DungeonStagesTest).
namespace Animus::Curriculum::SeenPlaces
{
    /// A point on the ground.
    struct Point
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };
    static_assert(sizeof(Point) == 3 * sizeof(float), "a place is a position and nothing else: no creature data");

    /// Where a stage's goal places come from (StageDefinition::GoalPlaces; <TuningPrefix>Stage.<name>.GoalPlaces).
    enum class Source : uint8_t
    {
        SeenAndLayout = 0,      // what the seat saw, its map's frontier, and the dungeon map's layout nodes
        SeenOnly = 1,           // what the seat saw and its map's frontier alone
    };

    /// A hostile the seat's entity memory holds: where it last saw it.
    struct Recalled
    {
        Point At;
        bool Hostile = false;
        bool Dead = false;
        bool GameObject = false;
    };

    /// What the choice reads. `Explored` says whether a layout node's ground is already on the seat's mental map.
    struct Input
    {
        Point Seat;
        std::vector<Recalled> Memory;
        std::vector<Point> Frontier;
        std::vector<Point> const* Layout = nullptr;     // null: seen only
        std::vector<bool> LayoutExplored;               // per layout node
        bool HasLeader = false;                         // the party's leader, not this seat (its frame, its map dot)
        Point Leader;
    };

    /// The goal head's eight places (WorldView::JOURNAL_PLACES): 0-5 what to go and see or fight -- remembered hostiles
    /// first (MEMORY_PLACES at most), then the frontier and unexplored layout nodes -- 6 the nearest way on (the
    /// frontier's, else the layout's), 7 the leader; and the assignment.
    constexpr uint32_t PLACES = 8;
    constexpr uint32_t ROAM_PLACES = PLACES - 2;
    constexpr uint32_t MEMORY_PLACES = 3;
    constexpr uint32_t WAY_ON = PLACES - 2;
    constexpr uint32_t LEADER = PLACES - 1;
    /// A frontier point or layout node closer than this to one already chosen is the same place.
    constexpr float SAME_PLACE = 10.0f;

    struct Choice
    {
        std::array<bool, PLACES> Present{};
        std::array<Point, PLACES> Where{};
        bool HasAssignment = false;
        Point Assignment;
    };

    [[nodiscard]] inline float Distance(Point const& a, Point const& b)
    {
        float const dx = a.X - b.X;
        float const dy = a.Y - b.Y;
        float const dz = a.Z - b.Z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    [[nodiscard]] inline Choice Choose(Input const& in)
    {
        Choice out;
        uint32_t next = 0;
        auto const nearestFirst = [&in](std::vector<Point>& points)
        {
            std::sort(points.begin(), points.end(), [&in](Point const& a, Point const& b)
            {
                return Distance(in.Seat, a) < Distance(in.Seat, b);
            });
        };
        auto const taken = [&out, &next](Point const& at)
        {
            for (uint32_t i = 0; i < next; ++i)
                if (Distance(out.Where[i], at) < SAME_PLACE)
                    return true;
            return false;
        };

        // The hostiles it saw and has not seen die: where it last saw them, nearest first.
        std::vector<Point> hostiles;
        for (Recalled const& entry : in.Memory)
            if (entry.Hostile && !entry.Dead && !entry.GameObject)
                hostiles.push_back(entry.At);
        nearestFirst(hostiles);
        for (Point const& at : hostiles)
        {
            if (next >= MEMORY_PLACES)
                break;
            if (taken(at))
                continue;
            out.Present[next] = true;
            out.Where[next++] = at;
        }

        // The way on: its own map's frontier, then the layout's ground it has not been to (SeenAndLayout).
        std::vector<Point> frontier = in.Frontier;
        nearestFirst(frontier);
        std::vector<Point> unexplored;
        if (in.Layout)
            for (std::size_t node = 0; node < in.Layout->size(); ++node)
                if (node >= in.LayoutExplored.size() || !in.LayoutExplored[node])
                    unexplored.push_back((*in.Layout)[node]);
        nearestFirst(unexplored);
        bool wayOn = false;
        for (std::vector<Point> const* list : { &frontier, &unexplored })
            for (Point const& at : *list)
            {
                if (!wayOn)
                {
                    wayOn = true;
                    out.Present[WAY_ON] = true;
                    out.Where[WAY_ON] = at;
                }
                if (next >= ROAM_PLACES)
                    break;
                if (taken(at))
                    continue;
                out.Present[next] = true;
                out.Where[next++] = at;
            }

        if (in.HasLeader)
        {
            out.Present[LEADER] = true;
            out.Where[LEADER] = in.Leader;
        }
        // The assignment: the nearest thing to go and see or fight, else the way on, else the leader.
        for (uint32_t place : { 0u, WAY_ON, LEADER })
            if (out.Present[place])
            {
                out.HasAssignment = true;
                out.Assignment = out.Where[place];
                break;
            }
        return out;
    }

    /// What a mental map cell is to the frontier's search: never seen, seen and open (a floor, or free in the body's
    /// band), or seen and shut (a wall, water or a hazard).
    enum class Ground : uint8_t
    {
        Unknown,
        Open,
        Shut,
    };

    /// **The frontier** of what the seat has seen: open ground within `radius` yards of `from` with unseen ground
    /// beside it, every `step` yards, at most `count` points at least SAME_PLACE apart, nearest first. `at(x, y)` reads
    /// the seat's own mental map.
    template <typename Probe>
    [[nodiscard]] std::vector<Point> Frontier(Point const& from, float radius, float step, uint32_t count, Probe at)
    {
        std::vector<Point> cells;
        int32_t const reach = int32_t(radius / step);
        for (int32_t i = -reach; i <= reach; ++i)
            for (int32_t j = -reach; j <= reach; ++j)
            {
                float const x = from.X + float(i) * step;
                float const y = from.Y + float(j) * step;
                if (float(i * i + j * j) * step * step > radius * radius || at(x, y) != Ground::Open)
                    continue;
                if (at(x + step, y) == Ground::Unknown || at(x - step, y) == Ground::Unknown
                    || at(x, y + step) == Ground::Unknown || at(x, y - step) == Ground::Unknown)
                    cells.push_back({ x, y, from.Z });
            }
        std::sort(cells.begin(), cells.end(), [&from](Point const& a, Point const& b)
        {
            return Distance(from, a) < Distance(from, b);
        });
        std::vector<Point> out;
        for (Point const& cell : cells)
        {
            if (out.size() >= count)
                break;
            if (std::none_of(out.begin(), out.end(), [&cell](Point const& kept)
                { return Distance(kept, cell) < SAME_PLACE; }))
                out.push_back(cell);
        }
        return out;
    }

    /// **The dungeon map's layout**: ground nodes along the dungeon's walkable way, one every `spacing` yards, none
    /// within `spacing` of another -- positions alone, unordered (sorted by place, so nothing of the order the way was
    /// walked in survives), no creature, pack, boss or encounter on them: what the dungeon map draws.
    [[nodiscard]] inline std::vector<Point> Layout(std::vector<Point> ground, float spacing)
    {
        // By place first, so the nodes kept do not depend on the order the ground was walked in.
        auto const byPlace = [](Point const& a, Point const& b)
        {
            return a.X != b.X ? a.X < b.X : a.Y != b.Y ? a.Y < b.Y : a.Z < b.Z;
        };
        std::sort(ground.begin(), ground.end(), byPlace);
        std::vector<Point> out;
        for (Point const& at : ground)
            if (std::none_of(out.begin(), out.end(), [&at, spacing](Point const& kept)
                { return Distance(kept, at) < spacing; }))
                out.push_back(at);
        return out;
    }
}

#endif
