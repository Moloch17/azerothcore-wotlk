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

#ifndef ANIMUS_LIB_CURRICULUM_ROUTE_SHORTCUT_H
#define ANIMUS_LIB_CURRICULUM_ROUTE_SHORTCUT_H

#include "Define.h"
#include <algorithm>
#include <cmath>
#include <vector>

/// **A dungeon's field route walked corner to corner** (movement-smooth A8). The field route is a yard-by-yard walk
/// over the layered field's cells, eight ways round: walked a few yards at a time it zigzags along every diagonal and
/// the advance stopped at each step. The yards stay as they are -- everything else names places on the route by its
/// yard -- and each yard is given the farthest yard within REACH it can be walked to straight (`clear`, decided once
/// when the route is built); the advance walks that chain of corners. Pure, so it is tested on its own
/// (RouteShortcutTest).
namespace Animus::Curriculum::RouteShortcut
{
    constexpr uint32 REACH = 20;            // yards a corner may skip
    constexpr uint32 ADVANCE_YARDS = 18;    // how far along the route one advance run goes
    constexpr std::size_t MAX_POINTS = 6;   // corners an advance run walks through at most

    /// For each of `count` yards, the farthest yard within REACH towards `step` (+1 on along the route, -1 back) that
    /// `clear(from, to)` says is walked straight; the next yard when none further is. The last yard (or the first,
    /// going back) is its own.
    template <class Clear>
    [[nodiscard]] std::vector<uint32> Corners(std::size_t count, int32 step, Clear&& clear)
    {
        std::vector<uint32> corner(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            int32 const end = step > 0 ? int32(std::min(count - 1, i + REACH)) : std::max(0, int32(i) - int32(REACH));
            int32 best = int32(i);
            if (int32(i) != end)
            {
                best = int32(i) + step;
                for (int32 j = end; j != best; j -= step)
                    if (clear(uint32(i), uint32(j)))
                    {
                        best = j;
                        break;
                    }
            }
            corner[i] = uint32(best);
        }
        return corner;
    }

    struct Point
    {
        float X = 0.0f;
        float Y = 0.0f;
    };

    /// A closed door, as a disc on the ground (centre and radius, yards).
    struct Door
    {
        Point At;
        float Radius = 0.0f;
    };

    /// Where along the segment `a` -> `b` (as a share of it, 0..1) it first comes within `door`'s radius; negative
    /// when it never does.
    [[nodiscard]] inline float EntersDoor(Point a, Point b, Door const& door)
    {
        float const dx = b.X - a.X;
        float const dy = b.Y - a.Y;
        float const fx = a.X - door.At.X;
        float const fy = a.Y - door.At.Y;
        float const qa = dx * dx + dy * dy;
        float const qb = 2.0f * (fx * dx + fy * dy);
        float const qc = fx * fx + fy * fy - door.Radius * door.Radius;
        if (qc <= 0.0f)
            return 0.0f;                        // starts inside it
        if (qa <= 0.0f)
            return -1.0f;
        float const disc = qb * qb - 4.0f * qa * qc;
        if (disc < 0.0f)
            return -1.0f;
        float const t = (-qb - std::sqrt(disc)) / (2.0f * qa);
        return t >= 0.0f && t <= 1.0f ? t : -1.0f;
    }

    /// A run from `start` through `path` cut short at the first closed door it would walk into: a spline goes through
    /// anything, so a run must end where the door is and the seat open it (movement-smooth A8). Returns how many of
    /// `path`'s points are kept; when the cut falls inside a segment, `cut` is where it now ends (and is the last
    /// point kept). 0: the seat is at a door already, and there is no run.
    [[nodiscard]] inline std::size_t CutAtDoors(Point start, std::vector<Point>& path, std::vector<Door> const& doors)
    {
        Point from = start;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            float first = 2.0f;
            for (Door const& door : doors)
                if (float const t = EntersDoor(from, path[i], door); t >= 0.0f)
                    first = std::min(first, t);
            if (first <= 1.0f)
            {
                Point const cut{ from.X + (path[i].X - from.X) * first, from.Y + (path[i].Y - from.Y) * first };
                if (std::hypot(cut.X - from.X, cut.Y - from.Y) < 0.5f)
                    return i;                   // nothing to walk before the door
                path[i] = cut;
                return i + 1;
            }
            from = path[i];
        }
        return path.size();
    }

    /// The corners from `from` towards `target` (inclusive, never past it), at most ADVANCE_YARDS yards of route and
    /// MAX_POINTS corners: the run one advance walks. Empty when the seat is at the target.
    [[nodiscard]] inline std::vector<uint32> Chain(std::vector<uint32> const& ahead, std::vector<uint32> const& back,
        uint32 from, uint32 target)
    {
        std::vector<uint32> chain;
        if (from >= ahead.size() || target >= ahead.size() || back.size() != ahead.size())
            return chain;
        uint32 at = from;
        while (at != target && chain.size() < MAX_POINTS)
        {
            uint32 const span = at < target ? std::min(ahead[at], target) : std::max(back[at], target);
            if (span == at)
                break;
            uint32 const walked = from < target ? span - from : from - span;
            if (walked > ADVANCE_YARDS && !chain.empty())
                break;
            chain.push_back(span);
            at = span;
        }
        return chain;
    }
}

#endif
