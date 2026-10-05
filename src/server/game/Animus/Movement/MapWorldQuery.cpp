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

#include "MapWorldQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include <algorithm>
#include <cmath>

namespace
{
    /// How far apart the four samples of a floor's slope are: wide enough to see a slope over a doodad's seams, narrow
    /// enough to be the floor under the feet.
    constexpr float NORMAL_SPAN = 0.3f;
    /// A slope sample is looked for from this far above the floor's height (a rise between samples is still found).
    constexpr float NORMAL_LOOK = 0.75f;
    /// The chest ray's height, as a share of the body's.
    constexpr float CHEST = 0.8f;
    /// The knee ray: just above the highest step, so a step is walked onto (PlayerController's ground follow) and
    /// anything taller is a wall.
    constexpr float KNEE_ABOVE_STEP = 0.05f;
}

void Animus::Movement::SweepRays(float dx, float dy, Body const& body, SweepRay (&out)[6])
{
    float const length = std::sqrt(dx * dx + dy * dy);
    // Across the move: the left of (dx, dy) is (-dy, dx). A move with no horizontal length sweeps the centre only.
    float const ax = length > 1e-6f ? -dy / length * body.Radius : 0.0f;
    float const ay = length > 1e-6f ? dx / length * body.Radius : 0.0f;
    float const heights[2] = { STEP_UP + KNEE_ABOVE_STEP, body.Height * CHEST };
    for (int h = 0; h < 2; ++h)
    {
        out[h * 3 + 0] = { 0.0f, 0.0f, heights[h] };
        out[h * 3 + 1] = { ax, ay, heights[h] };
        out[h * 3 + 2] = { -ax, -ay, heights[h] };
    }
}

float Animus::Movement::SweepShare(float free, float length, float radius)
{
    if (length <= 1e-6f)
        return 1.0f;
    if (free >= length + radius)
        return 1.0f;
    return std::clamp((free - radius) / length, 0.0f, 1.0f);
}

float Animus::Movement::MapWorldQuery::FloorBelow(float x, float y, float z, float search) const
{
    Heights.fetch_add(1, std::memory_order_relaxed);
    // Terrain, static models and the dynamic tree (Map::GetHeight's own choice between them), the highest at or below z.
    float const floor = _map->GetHeight(_phaseMask, x, y, z, true, search);
    if (floor <= INVALID_HEIGHT || floor > z + 0.05f || floor < z - search)
        return INVALID_FLOOR;
    return floor;
}

float Animus::Movement::MapWorldQuery::FloorNormalZ(float x, float y, float z) const
{
    // The floor's gradient from four samples around (x, y): the normal of the plane they span.
    float h[4];
    float const offsets[4][2] = { { NORMAL_SPAN, 0.0f }, { -NORMAL_SPAN, 0.0f }, { 0.0f, NORMAL_SPAN },
        { 0.0f, -NORMAL_SPAN } };
    for (int i = 0; i < 4; ++i)
    {
        h[i] = FloorBelow(x + offsets[i][0], y + offsets[i][1], z + NORMAL_LOOK, 2.0f * NORMAL_LOOK);
        if (h[i] <= INVALID_FLOOR + 1.0f)
            h[i] = z;   // an edge: the floor is taken as level there, so an edge reads as flat, not as a wall
    }
    float const gx = (h[0] - h[1]) / (2.0f * NORMAL_SPAN);
    float const gy = (h[2] - h[3]) / (2.0f * NORMAL_SPAN);
    return 1.0f / std::sqrt(1.0f + gx * gx + gy * gy);
}

Animus::Movement::Liquid Animus::Movement::MapWorldQuery::LiquidAt(float x, float y, float z) const
{
    LiquidData const data = _map->GetLiquidData(_phaseMask, x, y, z, 2.0f, {});
    Liquid out;
    if (data.Status == LIQUID_MAP_NO_WATER || data.Level <= INVALID_HEIGHT)
        return out;
    out.Present = true;
    out.Level = data.Level;
    out.Deadly = (data.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
    return out;
}

float Animus::Movement::MapWorldQuery::RayFree(float x0, float y0, float z0, float x1, float y1, float z1) const
{
    Rays.fetch_add(1, std::memory_order_relaxed);
    float const length = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
    float free = length;
    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    MapCollisionData const& collision = _map->GetMapCollisionData();
    if (collision.GetStaticTree().GetObjectHitPos(x0, y0, z0, x1, y1, z1, rx, ry, rz, 0.0f))
        free = std::min(free, std::sqrt((rx - x0) * (rx - x0) + (ry - y0) * (ry - y0) + (rz - z0) * (rz - z0)));
    if (collision.GetDynamicTree().GetObjectHitPos(_phaseMask, x0, y0, z0, x1, y1, z1, rx, ry, rz, 0.0f))
        free = std::min(free, std::sqrt((rx - x0) * (rx - x0) + (ry - y0) * (ry - y0) + (rz - z0) * (rz - z0)));
    return free;
}

float Animus::Movement::MapWorldQuery::Sweep(float x0, float y0, float z0, float x1, float y1, float z1,
    Body const& body) const
{
    float const dx = x1 - x0;
    float const dy = y1 - y0;
    float const dz = z1 - z0;
    float const length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length <= 1e-6f)
        return 1.0f;
    // Each ray runs a body's radius past the end, so the body's front is checked, not its centre.
    float const ux = dx / length;
    float const uy = dy / length;
    float const uz = dz / length;
    SweepRay rays[6];
    SweepRays(dx, dy, body, rays);
    float nearest = length + body.Radius;
    for (SweepRay const& ray : rays)
    {
        float const sx = x0 + ray.Dx;
        float const sy = y0 + ray.Dy;
        float const sz = z0 + ray.Dz;
        float const reach = length + body.Radius;
        nearest = std::min(nearest, RayFree(sx, sy, sz, sx + ux * reach, sy + uy * reach, sz + uz * reach));
    }
    return SweepShare(nearest, length, body.Radius);
}

float Animus::Movement::MapWorldQuery::Ceiling(float x, float y, float z, float up) const
{
    if (up <= 0.0f)
        return 0.0f;
    return std::min(up, RayFree(x, y, z, x, y, z + up));
}

bool Animus::Movement::MapWorldQuery::InTerrain(float x, float y, float z) const
{
    float const terrain = _map->GetGridHeight(x, y);
    return terrain > INVALID_HEIGHT && z < terrain - GROUND_HEIGHT_TOLERANCE;
}
