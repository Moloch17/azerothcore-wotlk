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

#ifndef ANIMUS_MOVEMENT_MAP_WORLD_QUERY_H
#define ANIMUS_MOVEMENT_MAP_WORLD_QUERY_H

#include "Define.h"
#include "PlayerController.h"
#include <atomic>

class Map;

/// The player controller's WorldQuery over a live map (player-controller plan, C2): the terrain and static models
/// (Map::GetHeight's terrain + VMAP rules), liquids (Map::GetLiquidData), and both collision trees -- the static models
/// and the dynamic one, which holds game objects, so a closed door is a wall and an open one is not -- for sweeps and
/// ceilings. Read-only queries of the map the body is on, made from that map's own update (thread-safe as the rest of
/// a map update's reads are). The layered-field fast path is C8.
namespace Animus::Movement
{
    class MapWorldQuery final : public WorldQuery
    {
    public:
        MapWorldQuery(Map* map, uint32 phaseMask) : _map(map), _phaseMask(phaseMask) { }

        [[nodiscard]] float FloorBelow(float x, float y, float z, float search) const override;
        [[nodiscard]] float FloorNormalZ(float x, float y, float z) const override;
        [[nodiscard]] Liquid LiquidAt(float x, float y, float z) const override;
        [[nodiscard]] float Sweep(float x0, float y0, float z0, float x1, float y1, float z1,
            Body const& body) const override;
        [[nodiscard]] float Ceiling(float x, float y, float z, float up) const override;
        [[nodiscard]] bool InTerrain(float x, float y, float z) const override;

        /// Collision rays cast and height queries made (for the cost line, C8).
        static inline std::atomic<uint64> Rays{ 0 };
        static inline std::atomic<uint64> Heights{ 0 };

    private:
        /// How far along the segment from (x0,y0,z0) to (x1,y1,z1) the first solid is, in yards; the whole length
        /// when nothing is hit. Both the static models and the dynamic tree.
        [[nodiscard]] float RayFree(float x0, float y0, float z0, float x1, float y1, float z1) const;

        Map* _map;
        uint32 _phaseMask;
    };

    /// The side offsets of a sweep: a body is a cylinder, swept as rays from its centre and its left and right edges
    /// (the radius across the move), at the knee -- just above STEP_UP, so a step is not a wall -- and at the chest.
    /// Pure, so it is tested on its own.
    struct SweepRay
    {
        float Dx = 0.0f;        // offset of the ray's start from the body's centre (and of its end from the target)
        float Dy = 0.0f;
        float Dz = 0.0f;        // height above the feet
    };
    /// The six rays of a body moving along (dx, dy): three across (centre, left edge, right edge) at two heights.
    void SweepRays(float dx, float dy, Body const& body, SweepRay (&out)[6]);
    /// The share of a move of `length` yards a body can make when its nearest ray meets a solid after `free` yards:
    /// stops `radius` short of the hit along the move (the body's front), at least 0.
    [[nodiscard]] float SweepShare(float free, float length, float radius);
}

#endif
