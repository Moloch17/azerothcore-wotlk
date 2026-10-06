/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_CURRICULUM_FLOOR_SCAN_H
#define ANIMUS_CURRICULUM_FLOOR_SCAN_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

/// `forge floorscan` (player-controller, 2026-10-05: the user's M1 Stockades hallway had floor the navmesh walks and
/// the player controller's world query does not): each cell of a box compared between the controller's floor (the
/// vmaps and terrain, MapWorldQuery) and the navmesh's walkable surface (Detour, as the route planner reads it). Pure,
/// so the classification is tested on its own; the command reads the map.
namespace Animus::Curriculum::FloorScan
{
    /// A cell's verdict, worst first in Severity.
    enum class Cell : uint8_t
    {
        Ok,             // both: the same height (within MISMATCH_YARDS), the controller's floor walkable
        Hole,           // the navmesh walks it, the controller has no floor within a step of it
        Mismatch,       // both, more than MISMATCH_YARDS apart
        Steep,          // both, the controller's floor steeper than the walkable 50 degrees
        NoNav,          // the controller has a floor the navmesh does not walk (a ledge, a table, outside the mesh)
        Unwalkable,     // neither
    };

    /// The navmesh's own imprecision at rims and slopes' feet reads 0.5-0.6 yd (the first M1 hallway scan, 1429 of
    /// its cells): past this the two disagree.
    constexpr float MISMATCH_YARDS = 0.75f;
    constexpr float WALKABLE_NORMAL_Z = 0.6427876f;     // cos 50 degrees (PlayerController.h)
    constexpr float GRID_YARDS = 533.3333f;             // SIZE_OF_GRIDS
    constexpr float NAV_REACH_Z = 10.0f;                // how far above and below z the navmesh's surface is looked for

    inline Cell Classify(bool nav, float navZ, bool floor, float floorZ, float normalZ)
    {
        if (!nav)
            return floor ? Cell::NoNav : Cell::Unwalkable;
        if (!floor)
            return Cell::Hole;
        if (std::fabs(floorZ - navZ) > MISMATCH_YARDS)
            return Cell::Mismatch;
        if (normalZ < WALKABLE_NORMAL_Z)
            return Cell::Steep;
        return Cell::Ok;
    }

    /// The ASCII map's glyph, and which verdict a coarser map cell shows when it holds several (the worst).
    inline char Glyph(Cell cell)
    {
        switch (cell)
        {
            case Cell::Ok: return '.';
            case Cell::Hole: return 'H';
            case Cell::Mismatch: return 'M';
            case Cell::Steep: return 'S';
            case Cell::NoNav: return 'n';
            default: return ' ';
        }
    }

    inline int Severity(Cell cell)
    {
        switch (cell)
        {
            case Cell::Hole: return 5;
            case Cell::Mismatch: return 4;
            case Cell::Steep: return 3;
            case Cell::NoNav: return 2;
            case Cell::Ok: return 1;
            default: return 0;
        }
    }

    inline char const* Name(Cell cell)
    {
        switch (cell)
        {
            case Cell::Ok: return "OK";
            case Cell::Hole: return "HOLE";
            case Cell::Mismatch: return "MISMATCH";
            case Cell::Steep: return "STEEP";
            case Cell::NoNav: return "NONAV";
            default: return "UNWALKABLE";
        }
    }

    /// The grids (FieldGrids::GridIndex's numbering: floor(coordinate / GRID_YARDS)) a span [a, b] touches, widened by
    /// `margin` yards: first and last.
    inline std::pair<int32_t, int32_t> GridSpan(float a, float b, float margin = 0.0f)
    {
        float const low = std::min(a, b) - margin;
        float const high = std::max(a, b) + margin;
        return { int32_t(std::floor(low / GRID_YARDS)), int32_t(std::floor(high / GRID_YARDS)) };
    }
}

#endif
