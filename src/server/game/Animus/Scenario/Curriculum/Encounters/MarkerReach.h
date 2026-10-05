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

#ifndef ANIMUS_LIB_CURRICULUM_MARKER_REACH_H
#define ANIMUS_LIB_CURRICULUM_MARKER_REACH_H

#include "PlayerController.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

/// **Whether the player controller can walk a route** (movement-curriculum, M2/M3): the navmesh plans on its own idea
/// of a walkable slope (60 degrees) and climb, the controller moves by the client's (50 degrees, a 1.19 yd step), so a
/// navmesh route is not a promise. This walks a route's corners a sample at a time against the controller's world
/// query and says whether every rise on it is a step (<= STEP_UP), a jump (<= the jump's apex, 1.64 yd) or no rise
/// (a drop of any height is physically allowed; what it costs is the fall's). A marker whose way fails here is
/// refused: no marker is placed where the controller cannot get. Pure: tested on fake worlds (MarkerReachTest).
namespace Animus::Curriculum::MarkerReach
{
    /// The jump's apex over the ground (JUMP_SPEED^2 / 2g, 1.640 yd).
    constexpr float JUMP_APEX = Movement::JUMP_SPEED * Movement::JUMP_SPEED / (2.0f * Movement::GRAVITY);
    /// Yards between samples along the route.
    constexpr float SAMPLE = 1.0f;
    /// A rise under this is level ground (the controller's own snap).
    constexpr float LEVEL = 0.05f;
    /// The walk has to end within this of the place's own floor: a route that ends a storey above or below it has not
    /// reached it.
    constexpr float END_FLOOR = 2.0f;

    struct Result
    {
        bool Reachable = false;
        uint32_t Jumps = 0;             // rises over a step and within a jump
        float MaxDrop = 0.0f;           // the deepest single drop on the way
        float EndZ = 0.0f;              // the floor the walk ended on
    };

    /// Walk the route (xs, ys, zs; `count` corners, the first where the seat stands) with a body of `shape`.
    inline Result Walk(Movement::WorldQuery const& world, Movement::Body const& shape, float const* xs, float const* ys,
        float const* zs, uint32_t count)
    {
        Result out;
        if (!count)
            return out;

        float z = world.FloorBelow(xs[0], ys[0], zs[0] + Movement::STEP_UP, 2.0f * Movement::STEP_UP);
        if (z <= Movement::INVALID_FLOOR)
            z = zs[0];
        float x = xs[0];
        float y = ys[0];
        float steep = 0.0f;             // rise climbed in a row on faces steeper than the walkable slope

        for (uint32_t corner = 1; corner < count; ++corner)
        {
            float const dx = xs[corner] - x;
            float const dy = ys[corner] - y;
            float const length = std::sqrt(dx * dx + dy * dy);
            uint32_t const samples = std::max<uint32_t>(1, uint32_t(std::ceil(length / SAMPLE)));
            float const startX = x;
            float const startY = y;
            for (uint32_t i = 1; i <= samples; ++i)
            {
                float const t = float(i) / float(samples);
                float const nx = startX + dx * t;
                float const ny = startY + dy * t;

                // Out of reach above: the ground or a model taller than a jump is in the way.
                float const over = z + JUMP_APEX + LEVEL;
                if (world.InTerrain(nx, ny, over) && !world.InTerrain(x, y, over))
                    return out;
                bool const walled = world.Sweep(x, y, z, nx, ny, z, shape) < 1.0f;
                if (walled && world.Sweep(x, y, z + JUMP_APEX, nx, ny, z + JUMP_APEX, shape) < 1.0f)
                    return out;

                float const floor = world.FloorBelow(nx, ny, over, 1000.0f);
                if (floor <= Movement::INVALID_FLOOR)
                    return out;     // nothing underneath: no way across

                float const rise = floor - z;
                if (rise > JUMP_APEX + LEVEL)
                    return out;
                bool const jump = walled || rise > Movement::STEP_UP;
                out.Jumps += jump ? 1 : 0;
                if (rise > LEVEL && world.FloorNormalZ(nx, ny, floor) < Movement::WALKABLE_NORMAL_Z)
                {
                    // Up a face too steep to walk: one jump's worth of it can be taken, no more.
                    steep += rise;
                    if (steep > JUMP_APEX + LEVEL)
                        return out;
                }
                else
                    steep = 0.0f;
                if (rise < 0.0f)
                    out.MaxDrop = std::max(out.MaxDrop, -rise);

                x = nx;
                y = ny;
                z = floor;
            }
        }

        out.Reachable = true;
        out.EndZ = z;
        return out;
    }
}

#endif
