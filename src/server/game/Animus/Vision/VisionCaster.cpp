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

#include "VisionCaster.h"
#include <algorithm>
#include <chrono>
#include <limits>

namespace
{
    using namespace Animus::Vision;
    namespace Mv = Animus::Movement;

    Settings CurrentSettings;

    using Clock = std::chrono::steady_clock;

    constexpr float INF = std::numeric_limits<float>::infinity();
    /// A cell's liquid plane is crossed within its footprint, give or take this much of the ray (rounding).
    constexpr float FOOTPRINT_SLACK = 1e-3f;

    /// Adds the time since `mark` to `slot` and moves `mark` to now (only when a breakdown is wanted).
    void Charge(Breakdown* breakdown, uint64_t Breakdown::* slot, Clock::time_point& mark)
    {
        if (!breakdown)
            return;
        Clock::time_point const now = Clock::now();
        breakdown->*slot += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
        mark = now;
    }

    [[nodiscard]] Vec3 Cross(Vec3 a, Vec3 b)
    {
        return { a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X };
    }

    /// Moller-Trumbore, one-sided: where the ray crosses the triangle going down through its upper face (the face
    /// whose normal points up), or -1. A ray from under the terrain does not see its underside.
    [[nodiscard]] float RayTriangleFromAbove(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c, float& normalZ)
    {
        Vec3 const e1 = b - a;
        Vec3 const e2 = c - a;
        Vec3 normal = Cross(e1, e2);
        if (normal.Z < 0.0f)
            normal = normal * -1.0f;
        if (Dot(normal, dir) >= 0.0f)
            return -1.0f;
        Vec3 const p = Cross(dir, e2);
        float const det = Dot(e1, p);
        if (std::fabs(det) < 1e-12f)
            return -1.0f;
        float const inv = 1.0f / det;
        Vec3 const s = origin - a;
        float const u = Dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f)
            return -1.0f;
        Vec3 const q = Cross(s, e1);
        float const v = Dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f)
            return -1.0f;
        float const t = Dot(e2, q) * inv;
        if (t < 0.0f)
            return -1.0f;
        float const length = Length(normal);
        normalZ = length > 0.0f ? normal.Z / length : 1.0f;
        return t;
    }

    /// The terrain's height at (fu, fv) within a cell (0..1 each), on the triangle holding it: getHeight's rule.
    [[nodiscard]] float CellHeight(TerrainCell const& cell, float fu, float fv)
    {
        float const h1 = cell.Corner[0];
        float const h2 = cell.Corner[1];
        float const h3 = cell.Corner[2];
        float const h4 = cell.Corner[3];
        float const h5 = 2.0f * cell.Centre;
        if (fu + fv < 1.0f)
        {
            if (fu > fv)
                return h1 + (h2 - h1) * fu + (h5 - h1 - h2) * fv;
            return h1 + (h5 - h1 - h3) * fu + (h3 - h1) * fv;
        }
        if (fu > fv)
            return (h2 + h4 - h5) * fu + (h4 - h2) * fv + (h5 - h4);
        return (h4 - h3) * fu + (h3 + h4 - h5) * fv + (h5 - h4);
    }

    /// The squares of side `size` (in cell units) the ray's (u, v) path crosses between tStart and tEnd, in order
    /// (a 2D grid traversal: one visit a square, no fixed step). `visit(iu, iv, tIn, tOut)` returns false to stop.
    /// The first square is clamped into [low, high], as are the rest: leaving them ends the walk.
    template <typename Visit>
    void Walk(float u0, float v0, float du, float dv, float tStart, float tEnd, float size, int32_t const (&low)[2],
        int32_t const (&high)[2], Visit&& visit)
    {
        if (!(tEnd > tStart))
            return;
        float const u = u0 + du * tStart;
        float const v = v0 + dv * tStart;
        int32_t iu = std::clamp(int32_t(std::floor(u / size)), low[0], high[0]);
        int32_t iv = std::clamp(int32_t(std::floor(v / size)), low[1], high[1]);
        int32_t const stepU = du > 0.0f ? 1 : -1;
        int32_t const stepV = dv > 0.0f ? 1 : -1;
        bool const flatU = std::fabs(du) < 1e-12f;
        bool const flatV = std::fabs(dv) < 1e-12f;
        float nextU = flatU ? INF : (float(iu + (du > 0.0f ? 1 : 0)) * size - u0) / du;
        float nextV = flatV ? INF : (float(iv + (dv > 0.0f ? 1 : 0)) * size - v0) / dv;
        float const deltaU = flatU ? INF : size / std::fabs(du);
        float const deltaV = flatV ? INF : size / std::fabs(dv);
        float t = tStart;
        while (true)
        {
            float const next = std::max(t, std::min({ nextU, nextV, tEnd }));
            if (!visit(iu, iv, t, next) || next >= tEnd)
                return;
            t = next;
            if (nextU < nextV)
            {
                iu += stepU;
                nextU += deltaU;
            }
            else
            {
                iv += stepV;
                nextV += deltaV;
            }
            if (iu < low[0] || iu > high[0] || iv < low[1] || iv > high[1])
                return;
        }
    }

    constexpr int32_t NO_LIMIT_LOW[2] = { -(1 << 20), -(1 << 20) };
    constexpr int32_t NO_LIMIT_HIGH[2] = { 1 << 20, 1 << 20 };

    [[nodiscard]] bool OnMap(int32_t tileX, int32_t tileY)
    {
        return tileX >= 0 && tileY >= 0 && tileX < GRIDS && tileY < GRIDS;
    }

    /// The nearest hit along the ray within `limit`: the trees, the WMO liquids and the terrain (its liquids when
    /// `liquids`), then the units. The boom casts with no liquids and no units.
    Hit Nearest(Vec3 origin, Vec3 dir, float limit, VisionWorld const& world, std::span<UnitShape const> units,
        bool liquids, Breakdown* breakdown)
    {
        Clock::time_point mark = breakdown ? Clock::now() : Clock::time_point();
        if (breakdown)
            ++breakdown->Rays;

        Vec3 const end = origin + dir * limit;
        Hit best;
        best.Distance = limit;
        best.Z = end.Z;

        // 1. The collision trees, cast apart so a door (the dynamic tree) is told from a model (R8).
        SurfaceHit const model = world.StaticHit(origin, end);
        SurfaceHit const door = world.DynamicHit(origin, end);
        if (breakdown)
            breakdown->TreeCasts += 2;
        bool const modelHit = model.Distance >= 0.0f && model.Distance <= limit;
        bool const doorHit = door.Distance >= 0.0f && door.Distance <= limit;
        if (modelHit || doorHit)
        {
            bool const isDoor = doorHit && (!modelHit || door.Distance < model.Distance);
            SurfaceHit const& hit = isDoor ? door : model;
            best.Distance = hit.Distance;
            best.What = isDoor ? Kind::Door : Kind::Model;
            best.Z = (origin + dir * best.Distance).Z;
            // The hit triangle's own slope: a floor reads its tilt, a wall 0, a ceiling seen from under it below 0
            // (the encoding clamps it to 0, a wall's).
            best.NormalZ = hit.NormalZ;
        }
        Charge(breakdown, &Breakdown::TreeNs, mark);

        // 2. A WMO's liquid, entered from above (a rising ray only meets one from below, which it sees through).
        if (liquids && dir.Z < 0.0f)
        {
            if (breakdown)
                ++breakdown->LiquidCasts;
            LiquidHit const liquid = world.ModelLiquid(origin, origin + dir * best.Distance);
            if (liquid.Distance >= 0.0f && liquid.Distance < best.Distance)
            {
                best.Distance = liquid.Distance;
                best.What = liquid.Deadly ? Kind::Deadly : Kind::Water;
                best.Z = origin.Z + dir.Z * liquid.Distance;
                best.NormalZ = 1.0f;
            }
        }
        Charge(breakdown, &Breakdown::LiquidNs, mark);

        // 3. The terrain and its liquids, up to the nearest hit so far.
        Hit const terrain = CastTerrain(origin, dir, best.Distance, world, liquids, breakdown);
        if (terrain.What != Kind::Sky && terrain.Distance < best.Distance)
            best = terrain;
        Charge(breakdown, &Breakdown::TerrainNs, mark);

        // 4. Every unit's cylinder but the seat's own (every ray against every unit).
        for (UnitShape const& unit : units)
        {
            if (unit.Self)
                continue;
            if (breakdown)
                ++breakdown->UnitTests;
            bool top = false;
            float const distance = RayCylinder(origin, dir, best.Distance, unit, top);
            if (distance >= 0.0f && distance < best.Distance)
            {
                best.Distance = distance;
                best.What = unit.Hostile ? Kind::Hostile : Kind::Other;
                best.Z = origin.Z + dir.Z * distance;
                best.NormalZ = top ? 1.0f : 0.0f;
            }
        }
        Charge(breakdown, &Breakdown::UnitNs, mark);
        return best;
    }
}

Animus::Vision::Settings const& Animus::Vision::Current()
{
    return CurrentSettings;
}

void Animus::Vision::Configure(Settings const& settings)
{
    CurrentSettings = settings;
}

float Animus::Vision::Reach(Vec3 origin, Vec3 dir, VisionWorld const& world)
{
    // The grids the ray's path crosses, until one is not loaded.
    float const du = -dir.X * float(GRID_CELLS) / GRID_SIZE;
    float const dv = -dir.Y * float(GRID_CELLS) / GRID_SIZE;
    float reach = REACH_MAX;
    Walk(GridU(origin.X), GridU(origin.Y), du, dv, 0.0f, REACH_MAX, float(GRID_CELLS), NO_LIMIT_LOW, NO_LIMIT_HIGH,
        [&](int32_t tileX, int32_t tileY, float tIn, float /*tOut*/)
        {
            if (OnMap(tileX, tileY) && world.Tile(tileX, tileY).Loaded)
                return true;
            reach = tIn;
            return false;
        });
    return reach;
}

Animus::Vision::Hit Animus::Vision::CastTerrain(Vec3 origin, Vec3 dir, float limit, VisionWorld const& world,
    bool liquids, Breakdown* breakdown)
{
    Hit best;
    best.Distance = limit;
    best.Z = origin.Z + dir.Z * limit;
    bool found = false;
    float const u0 = GridU(origin.X);
    float const v0 = GridU(origin.Y);
    float const du = -dir.X * float(GRID_CELLS) / GRID_SIZE;
    float const dv = -dir.Y * float(GRID_CELLS) / GRID_SIZE;

    Walk(u0, v0, du, dv, 0.0f, limit, float(GRID_CELLS), NO_LIMIT_LOW, NO_LIMIT_HIGH,
        [&](int32_t tileX, int32_t tileY, float tileIn, float tileOut)
        {
            // Off the loaded grids, nothing more can be hit.
            if (!OnMap(tileX, tileY))
                return false;
            TerrainTile const tile = world.Tile(tileX, tileY);
            if (!tile.Loaded)
                return false;
            if (breakdown)
                ++breakdown->TerrainTiles;
            // Its ground only if the ray comes down to the grid's highest point over it (a ray above that and
            // climbing never can); its liquid only going down.
            float const lowest = std::min(origin.Z + dir.Z * tileIn, origin.Z + dir.Z * tileOut);
            bool const ground = tile.Heights && lowest <= tile.MaxHeight;
            bool const water = liquids && tile.Liquid && dir.Z < 0.0f;
            if (!ground && !water)
                return true;

            int32_t const low[2] = { tileX * GRID_CELLS, tileY * GRID_CELLS };
            int32_t const high[2] = { low[0] + GRID_CELLS - 1, low[1] + GRID_CELLS - 1 };
            Walk(u0, v0, du, dv, tileIn, tileOut, 1.0f, low, high,
                [&](int32_t u, int32_t v, float cellIn, float cellOut)
                {
                    if (breakdown)
                        ++breakdown->TerrainCells;
                    TerrainCell const cell = world.Cell(tileX, tileY, u - low[0], v - low[1], water);
                    float nearest = -1.0f;
                    if (ground && cell.Solid)
                    {
                        // The four triangles round the centre, in world space.
                        auto const at = [&](float cu, float cv, float z)
                        {
                            return Vec3{ WorldOfU(float(u) + cu), WorldOfU(float(v) + cv), z };
                        };
                        Vec3 const h1 = at(0.0f, 0.0f, cell.Corner[0]);
                        Vec3 const h2 = at(1.0f, 0.0f, cell.Corner[1]);
                        Vec3 const h3 = at(0.0f, 1.0f, cell.Corner[2]);
                        Vec3 const h4 = at(1.0f, 1.0f, cell.Corner[3]);
                        Vec3 const h5 = at(0.5f, 0.5f, cell.Centre);
                        Vec3 const triangles[4][3] = { { h1, h2, h5 }, { h1, h3, h5 }, { h2, h4, h5 }, { h3, h4, h5 } };
                        for (auto const& triangle : triangles)
                        {
                            float normalZ = 1.0f;
                            float const t = RayTriangleFromAbove(origin, dir, triangle[0], triangle[1], triangle[2],
                                normalZ);
                            if (t >= 0.0f && t <= limit && (nearest < 0.0f || t < nearest))
                            {
                                nearest = t;
                                best.What = Kind::Terrain;
                                best.NormalZ = normalZ;
                            }
                        }
                    }
                    if (water && cell.Liquid)
                    {
                        // The cell's liquid is a plane over its footprint, where the ground is below it.
                        float const t = (cell.Level - origin.Z) / dir.Z;
                        if (t >= 0.0f && t <= limit && t >= cellIn - FOOTPRINT_SLACK && t <= cellOut + FOOTPRINT_SLACK
                            && (nearest < 0.0f || t < nearest))
                        {
                            Vec3 const p = origin + dir * t;
                            float const fu = std::clamp(GridU(p.X) - float(u), 0.0f, 1.0f);
                            float const fv = std::clamp(GridU(p.Y) - float(v), 0.0f, 1.0f);
                            if (!cell.Solid || CellHeight(cell, fu, fv) <= cell.Level)
                            {
                                nearest = t;
                                best.What = cell.Deadly ? Kind::Deadly : Kind::Water;
                                best.NormalZ = 1.0f;
                            }
                        }
                    }
                    if (nearest < 0.0f)
                        return true;
                    best.Distance = nearest;
                    best.Z = origin.Z + dir.Z * nearest;
                    found = true;
                    return false;
                });
            return !found;
        });

    if (!found)
        best = Hit{ limit, Kind::Sky, origin.Z + dir.Z * limit, 0.0f };
    return best;
}

Animus::Vision::Rig Animus::Vision::PlaceCamera(Pose const& pose, CameraState const& camera,
    VisionWorld const& world, Breakdown* breakdown)
{
    Rig rig;
    rig.Pivot = { pose.X, pose.Y, pose.Z + PIVOT_SHARE * pose.BodyHeight };
    rig.Azimuth = pose.Yaw + camera.YawOffset;
    rig.Elevation = camera.Pitch;
    Vec3 const forward = Direction(rig.Azimuth, rig.Elevation);
    float const zoom = std::max(0.0f, camera.Zoom);
    rig.Boom = zoom;
    if (zoom > 0.0f)
    {
        // One cast from the pivot back along the view, against the trees and the terrain: the camera pulls in to
        // BOOM_BACKOFF short of what it meets, never nearer than BOOM_MIN (or the zoom, when that is nearer still).
        Hit const back = Nearest(rig.Pivot, forward * -1.0f, zoom, world, {}, false, breakdown);
        if (back.What != Kind::Sky)
            rig.Boom = std::min(zoom, std::max(std::min(BOOM_MIN, zoom), back.Distance - BOOM_BACKOFF));
    }
    rig.Camera = rig.Pivot - forward * rig.Boom;
    return rig;
}

void Animus::Vision::PixelAngles(Settings const& settings, uint32_t row, uint32_t col, float& yawRight,
    float& pitchUp)
{
    yawRight = ((float(col) + 0.5f) / float(settings.Width) * settings.FovH - settings.FovH / 2.0f) * DEGREES;
    pitchUp = (settings.FovV / 2.0f - (float(row) + 0.5f) / float(settings.Height) * settings.FovV) * DEGREES;
}

Animus::Vision::Vec3 Animus::Vision::PixelDirection(Rig const& rig, Settings const& settings, uint32_t row,
    uint32_t col)
{
    float yawRight = 0.0f;
    float pitchUp = 0.0f;
    PixelAngles(settings, row, col, yawRight, pitchUp);
    return Direction(rig.Azimuth - yawRight, rig.Elevation + pitchUp);
}

Animus::Vision::Hit Animus::Vision::CastRay(Vec3 origin, Vec3 dir, VisionWorld const& world,
    std::span<UnitShape const> units, Breakdown* breakdown)
{
    // No range: as far as the loaded grids go, which is where the terrain ends too.
    Clock::time_point mark = breakdown ? Clock::now() : Clock::time_point();
    float const reach = Reach(origin, dir, world);
    Charge(breakdown, &Breakdown::TerrainNs, mark);
    return Nearest(origin, dir, reach, world, units, true, breakdown);
}

float Animus::Vision::RayCylinder(Vec3 origin, Vec3 dir, float limit, UnitShape const& unit, bool& top)
{
    top = false;
    float best = -1.0f;
    float const ox = origin.X - unit.X;
    float const oy = origin.Y - unit.Y;
    float const r2 = unit.Radius * unit.Radius;
    float const bottom = unit.Z;
    float const head = unit.Z + unit.Height;

    // The side: the nearer root of the ray's horizontal distance reaching the radius, from outside.
    float const a = dir.X * dir.X + dir.Y * dir.Y;
    float const c = ox * ox + oy * oy - r2;
    if (a > 1e-9f && c > 0.0f)
    {
        float const b = 2.0f * (ox * dir.X + oy * dir.Y);
        float const discriminant = b * b - 4.0f * a * c;
        if (discriminant >= 0.0f)
        {
            float const t = (-b - std::sqrt(discriminant)) / (2.0f * a);
            float const z = origin.Z + dir.Z * t;
            if (t >= 0.0f && t <= limit && z >= bottom && z <= head)
                best = t;
        }
    }

    // The caps: the top seen from above, the bottom from below.
    auto const cap = [&](float plane, bool isTop)
    {
        if (std::fabs(dir.Z) < 1e-9f)
            return;
        float const t = (plane - origin.Z) / dir.Z;
        if (t < 0.0f || t > limit || (best >= 0.0f && t >= best))
            return;
        float const x = ox + dir.X * t;
        float const y = oy + dir.Y * t;
        if (x * x + y * y > r2)
            return;
        best = t;
        top = isTop;
    };
    if (origin.Z > head && dir.Z < 0.0f)
        cap(head, true);
    if (origin.Z < bottom && dir.Z > 0.0f)
        cap(bottom, false);
    return best;
}

float Animus::Vision::ObjectiveFlag(Vec3 origin, Vec3 dir, float distance, Vec3 const* objective)
{
    if (!objective)
        return 0.0f;
    Vec3 const toward = *objective - origin;
    float const along = std::clamp(Dot(toward, dir), 0.0f, std::max(0.0f, distance));
    return Length(toward - dir * along) <= OBJECTIVE_RADIUS ? 1.0f : 0.0f;
}

void Animus::Vision::EncodePixel(Hit const& hit, float feetZ, bool objective, uint8_t* out)
{
    bool const sky = hit.What == Kind::Sky;
    float const distance = std::clamp(std::log(std::max(hit.Distance, NEAR) / NEAR)
        / std::log(DISTANCE_REFERENCE / NEAR), 0.0f, 1.0f);
    out[0] = sky ? SKY_BYTE : uint8_t(std::lround(DISTANCE_LEVELS * distance));
    int32_t const steps = std::clamp(int32_t(std::lround((hit.Z - feetZ) / HEIGHT_STEP)), -HEIGHT_LIMIT, HEIGHT_LIMIT);
    out[1] = sky ? HEIGHT_ZERO : uint8_t(int32_t(HEIGHT_ZERO) + steps);
    out[2] = uint8_t(std::lround(255.0f * std::clamp(hit.NormalZ, 0.0f, 1.0f)));
    out[3] = uint8_t((uint8_t(hit.What) & KIND_MASK) | (objective ? OBJECTIVE_BIT : 0));
}

void Animus::Vision::DecodePixel(uint8_t const* in, float* out)
{
    out[CHANNEL_DISTANCE] = in[0] == SKY_BYTE ? 1.0f : float(in[0]) / DISTANCE_LEVELS;
    out[CHANNEL_HEIGHT] = float(int32_t(in[1]) - int32_t(HEIGHT_ZERO)) / float(HEIGHT_LIMIT);
    out[CHANNEL_NORMAL] = float(in[2]) / 255.0f;
    out[CHANNEL_KIND] = float(in[3] & KIND_MASK);
    out[CHANNEL_OBJECTIVE] = float((in[3] >> 4) & 1);
}

uint32_t Animus::Vision::Render(Settings const& settings, Pose const& pose, CameraState const& camera,
    VisionWorld const& world, std::span<UnitShape const> units, Vec3 const* objective, uint8_t* image,
    float* scalars, Breakdown* breakdown)
{
    Rig const rig = PlaceCamera(pose, camera, world, breakdown);
    Mv::Liquid const liquid = world.LiquidAt(rig.Camera.X, rig.Camera.Y, rig.Camera.Z);
    bool const underwater = liquid.Present && rig.Camera.Z < liquid.Level;

    for (uint32_t row = 0; image && row < settings.Height; ++row)
        for (uint32_t col = 0; col < settings.Width; ++col)
        {
            Vec3 const dir = PixelDirection(rig, settings, row, col);
            Hit const hit = CastRay(rig.Camera, dir, world, units, breakdown);
            // From the camera to the hit, or to where the ray left the loaded grids on sky (R12).
            bool const flag = ObjectiveFlag(rig.Camera, dir, hit.Distance, objective) > 0.5f;
            EncodePixel(hit, pose.Z, flag, image + (std::size_t(row) * settings.Width + col) * BYTES_PER_PIXEL);
        }

    scalars[SCALAR_YAW_OFFSET] = camera.YawOffset / PI;
    scalars[SCALAR_PITCH] = camera.Pitch / (PI / 2.0f);
    scalars[SCALAR_ZOOM] = camera.Zoom / ZOOM_SCALE;
    scalars[SCALAR_BOOM] = rig.Boom / ZOOM_SCALE;
    float const floor = world.FloorBelow(rig.Pivot.X, rig.Pivot.Y, rig.Pivot.Z, PIVOT_HEIGHT_SCALE);
    scalars[SCALAR_PIVOT_HEIGHT] = floor > Mv::INVALID_FLOOR + 1.0f
        ? std::clamp((rig.Pivot.Z - floor) / PIVOT_HEIGHT_SCALE, 0.0f, 1.0f) : 1.0f;
    scalars[SCALAR_UNDERWATER] = underwater ? 1.0f : 0.0f;
    scalars[SCALAR_AIRBORNE] = pose.Airborne ? 1.0f : 0.0f;
    return (image ? settings.Width * settings.Height : 0) + (camera.Zoom > 0.0f ? 1 : 0);
}
