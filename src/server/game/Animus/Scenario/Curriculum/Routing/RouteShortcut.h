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
/// when the route is built); the advance walks that chain of corners. Pure.
namespace Animus::Curriculum::RouteShortcut
{
    constexpr uint32 REACH = 20;            // yards a corner may skip

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
}

#endif
