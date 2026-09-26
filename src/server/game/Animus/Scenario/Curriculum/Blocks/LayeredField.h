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

#ifndef ANIMUS_CURRICULUM_LAYERED_FIELD_H
#define ANIMUS_CURRICULUM_LAYERED_FIELD_H

#include "Define.h"
#include <string>
#include <vector>

class Map;

/// A layered height field of a grid: per cell, the intervals of open air above each floor -- how high the floor is,
/// how much room there is above it before the next solid, and whether liquid fills the bottom of it. Everything else
/// in the column is solid. It is the world's geometry, not a reading of it: the ground probe and a flight sense can
/// both be worked out from it at any position and heading, where a baked probe holds readings at fixed ones.
///
/// A column is made from what is reliably an open-air floor: the topmost surface seen from the sky (open above for
/// good), every navmesh floor (walkable ground and water), and the terrain. The collision geometry is two-sided, so a
/// scan downward through it cannot tell a roof's underside from a floor; these can. Headroom is an upward collision
/// ray from each floor.
///
/// Prototype: `forge lhfbake` bakes one grid in memory and measures its size and how far a flight reading taken from
/// it is from one taken live.
namespace Animus::Curriculum::LayeredField
{
    /// Headroom that means open sky.
    constexpr float OPEN_SKY = 1.0e9f;

    struct Interval
    {
        float Floor = 0.0f;
        float Headroom = OPEN_SKY;
        float Liquid = -100000.0f;      // the liquid's surface when it fills the bottom of the interval, else invalid
        uint8 LiquidFlags = 0;          // MAP_LIQUID_TYPE_* of that liquid
    };

    struct Grid
    {
        uint32 MapId = 0;
        float MinX = 0.0f;              // the first cell's centre
        float MinY = 0.0f;
        float Cell = 1.0f;
        uint32 Side = 0;
        std::vector<uint32> First;      // per cell, into Intervals; Side * Side + 1
        std::vector<Interval> Intervals;
        double Seconds = 0.0;
    };

    /// Bake the grid holding (x, y); its terrain, collision and navmesh (and its neighbours') must be loaded.
    Grid Bake(Map* map, float x, float y, float cell, uint32 threads = 0);

    /// Whether (x, y, z) is open air, by the nearest cell.
    bool Open(Grid const& grid, float x, float y, float z);

    /// Yards of open air ahead along `heading` from (x, y, z), level, out to `range`, looked at every `pitch`.
    float FlightReach(Grid const& grid, float x, float y, float z, float heading, float range, float pitch);

    /// The same, measured live: the static collision's first hit along the line, or the terrain rising above it.
    float LiveFlightReach(Map* map, float x, float y, float z, float heading, float range, float pitch);

    /// Bake time, size (raw and zstd), intervals per cell, and the flight reading against the live one at `samples`
    /// random places in the grid's open air.
    std::string Compare(Map* map, Grid const& grid, uint32 samples, uint32 seed);
}

#endif
