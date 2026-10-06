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

namespace
{
    using namespace Animus::Vision;
    namespace Mv = Animus::Movement;

    Settings CurrentSettings;

    using Clock = std::chrono::steady_clock;

    /// Adds the time since `mark` to `slot` and moves `mark` to now (only when a breakdown is wanted).
    void Charge(Breakdown* breakdown, uint64_t Breakdown::* slot, Clock::time_point& mark)
    {
        if (!breakdown)
            return;
        Clock::time_point const now = Clock::now();
        breakdown->*slot += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
        mark = now;
    }

    [[nodiscard]] bool HasTerrain(float height)
    {
        return height > NO_TERRAIN;
    }

    /// The terrain's normal z at (x, y), from its heights TERRAIN_NORMAL_SPAN either side.
    [[nodiscard]] float TerrainNormalZ(VisionWorld const& world, float x, float y, float fallback)
    {
        float h[4];
        float const offsets[4][2] = { { TERRAIN_NORMAL_SPAN, 0.0f }, { -TERRAIN_NORMAL_SPAN, 0.0f },
            { 0.0f, TERRAIN_NORMAL_SPAN }, { 0.0f, -TERRAIN_NORMAL_SPAN } };
        for (int i = 0; i < 4; ++i)
        {
            h[i] = world.TerrainHeight(x + offsets[i][0], y + offsets[i][1]);
            if (!HasTerrain(h[i]))
                h[i] = fallback;    // the heightfield's edge: taken as level there
        }
        float const gx = (h[0] - h[1]) / (2.0f * TERRAIN_NORMAL_SPAN);
        float const gy = (h[2] - h[3]) / (2.0f * TERRAIN_NORMAL_SPAN);
        return 1.0f / std::sqrt(1.0f + gx * gx + gy * gy);
    }

    /// The nearest hit along the ray: the trees, then the march to that hit (terrain, and liquids when `liquids`),
    /// then the units. The boom casts with no liquids and no units.
    Hit Nearest(Vec3 origin, Vec3 dir, float range, VisionWorld const& world, std::span<UnitShape const> units,
        bool underwater, bool liquids, Breakdown* breakdown)
    {
        Clock::time_point mark = breakdown ? Clock::now() : Clock::time_point();
        if (breakdown)
            ++breakdown->Rays;

        Vec3 const end = origin + dir * range;
        Hit best;
        best.Distance = range;
        best.Z = end.Z;

        // 1. The collision trees, cast apart so a door (the dynamic tree) is told from a model (R8).
        float const model = world.StaticHit(origin, end);
        float const door = world.DynamicHit(origin, end);
        if (breakdown)
            breakdown->TreeCasts += 2;
        bool const modelHit = model >= 0.0f && model <= range;
        bool const doorHit = door >= 0.0f && door <= range;
        if (modelHit || doorHit)
        {
            bool const isDoor = doorHit && (!modelHit || door < model);
            best.Distance = isDoor ? door : model;
            best.What = isDoor ? Kind::Door : Kind::Model;
            Vec3 const at = origin + dir * best.Distance;
            best.Z = at.Z;
            // A floor when a floor is found just above it within FLOOR_MATCH: its slope; else a wall or a ceiling.
            float const floor = world.FloorBelow(at.X, at.Y, at.Z + FLOOR_LOOK, FLOOR_LOOK + FLOOR_MATCH);
            best.NormalZ = floor > Mv::INVALID_FLOOR + 1.0f && std::fabs(floor - at.Z) <= FLOOR_MATCH
                ? world.FloorNormalZ(at.X, at.Y, floor) : 0.0f;
        }
        Charge(breakdown, &Breakdown::TreeNs, mark);

        // 2. The march, 1 yd at a time up to the nearest collision hit (R9). Where there is no terrain only the
        // terrain comparison is skipped: the liquids are still read (a WMO's water, map 34's gutters; R10).
        float const limit = best.Distance;
        uint32_t const steps = uint32_t(std::ceil(limit / MARCH_STEP));
        float previous = 0.0f;
        auto const below = [&](float t)
        {
            Vec3 const p = origin + dir * t;
            float const terrain = world.TerrainHeight(p.X, p.Y);
            return HasTerrain(terrain) && p.Z < terrain;
        };
        bool wasBelow = below(0.0f);
        bool above = !underwater;
        for (uint32_t step = 1; step <= steps; ++step)
        {
            float const t = std::min(float(step) * MARCH_STEP, limit);
            if (breakdown)
                ++breakdown->MarchSteps;
            Vec3 const p = origin + dir * t;

            // The liquid's surface, entered from above, at the plane's exact crossing.
            float water = -1.0f;
            bool deadly = false;
            if (liquids)
            {
                Mv::Liquid const liquid = world.LiquidAt(p.X, p.Y, p.Z);
                if (!liquid.Present || p.Z >= liquid.Level)
                    above = true;
                else if (above)
                {
                    water = dir.Z < -1e-6f ? std::clamp((liquid.Level - origin.Z) / dir.Z, previous, t) : t;
                    deadly = liquid.Deadly;
                }
            }

            // The terrain, from above it to under it, bisected to BISECT_TO.
            float ground = -1.0f;
            bool const isBelow = below(t);
            if (isBelow && !wasBelow)
            {
                float lo = previous;
                float hi = t;
                while (hi - lo > BISECT_TO)
                {
                    float const mid = 0.5f * (lo + hi);
                    (below(mid) ? hi : lo) = mid;
                }
                ground = hi;
            }
            wasBelow = isBelow;

            if (water >= 0.0f && (ground < 0.0f || water <= ground))
            {
                best.Distance = water;
                best.What = deadly ? Kind::Deadly : Kind::Water;
                best.Z = origin.Z + dir.Z * water;
                best.NormalZ = 1.0f;
                break;
            }
            if (ground >= 0.0f)
            {
                Vec3 const at = origin + dir * ground;
                best.Distance = ground;
                best.What = Kind::Terrain;
                best.Z = at.Z;
                best.NormalZ = TerrainNormalZ(world, at.X, at.Y, at.Z);
                break;
            }
            previous = t;
        }
        Charge(breakdown, &Breakdown::MarchNs, mark);

        // 3. Every unit's cylinder but the seat's own (naive: every ray against every unit).
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
        // One cast from the pivot back along the view: the camera pulls in to BOOM_BACKOFF short of what it meets,
        // never nearer than BOOM_MIN (or the zoom, when that is nearer still).
        Hit const back = Nearest(rig.Pivot, forward * -1.0f, zoom, world, {}, false, false, breakdown);
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

Animus::Vision::Hit Animus::Vision::CastRay(Vec3 origin, Vec3 dir, float range, VisionWorld const& world,
    std::span<UnitShape const> units, bool underwater, Breakdown* breakdown)
{
    return Nearest(origin, dir, range, world, units, underwater, true, breakdown);
}

float Animus::Vision::RayCylinder(Vec3 origin, Vec3 dir, float range, UnitShape const& unit, bool& top)
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
            if (t >= 0.0f && t <= range && z >= bottom && z <= head)
                best = t;
        }
    }

    // The caps: the top seen from above, the bottom from below.
    auto const cap = [&](float plane, bool isTop)
    {
        if (std::fabs(dir.Z) < 1e-9f)
            return;
        float const t = (plane - origin.Z) / dir.Z;
        if (t < 0.0f || t > range || (best >= 0.0f && t >= best))
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

void Animus::Vision::EncodePixel(Hit const& hit, float feetZ, float range, float objective, float* out)
{
    bool const sky = hit.What == Kind::Sky;
    out[CHANNEL_DISTANCE] = sky ? 1.0f : std::clamp(std::log(std::max(hit.Distance, NEAR) / NEAR)
        / std::log(std::max(range, NEAR * 2.0f) / NEAR), 0.0f, 1.0f);
    out[CHANNEL_HEIGHT] = sky ? 0.0f : std::clamp((hit.Z - feetZ) / HEIGHT_SCALE, -1.0f, 1.0f);
    out[CHANNEL_NORMAL] = std::clamp(hit.NormalZ, 0.0f, 1.0f);
    out[CHANNEL_KIND] = float(uint32_t(hit.What));
    out[CHANNEL_OBJECTIVE] = objective;
}

uint32_t Animus::Vision::Render(Settings const& settings, Pose const& pose, CameraState const& camera,
    VisionWorld const& world, std::span<UnitShape const> units, Vec3 const* objective, float* out,
    Breakdown* breakdown)
{
    Rig const rig = PlaceCamera(pose, camera, world, breakdown);
    Mv::Liquid const liquid = world.LiquidAt(rig.Camera.X, rig.Camera.Y, rig.Camera.Z);
    bool const underwater = liquid.Present && rig.Camera.Z < liquid.Level;

    for (uint32_t row = 0; row < settings.Height; ++row)
        for (uint32_t col = 0; col < settings.Width; ++col)
        {
            Vec3 const dir = PixelDirection(rig, settings, row, col);
            Hit const hit = CastRay(rig.Camera, dir, settings.Range, world, units, underwater, breakdown);
            // From the camera to the hit, or to the range on a miss (R12).
            float const flag = ObjectiveFlag(rig.Camera, dir, hit.Distance, objective);
            EncodePixel(hit, pose.Z, settings.Range, flag, out + (std::size_t(row) * settings.Width + col) * CHANNELS);
        }

    float* scalars = out + ImageCount(settings);
    scalars[SCALAR_YAW_OFFSET] = camera.YawOffset / PI;
    scalars[SCALAR_PITCH] = camera.Pitch / (PI / 2.0f);
    scalars[SCALAR_ZOOM] = camera.Zoom / ZOOM_SCALE;
    scalars[SCALAR_BOOM] = rig.Boom / ZOOM_SCALE;
    float const floor = world.FloorBelow(rig.Pivot.X, rig.Pivot.Y, rig.Pivot.Z, PIVOT_HEIGHT_SCALE);
    scalars[SCALAR_PIVOT_HEIGHT] = floor > Mv::INVALID_FLOOR + 1.0f
        ? std::clamp((rig.Pivot.Z - floor) / PIVOT_HEIGHT_SCALE, 0.0f, 1.0f) : 1.0f;
    scalars[SCALAR_UNDERWATER] = underwater ? 1.0f : 0.0f;
    scalars[SCALAR_AIRBORNE] = pose.Airborne ? 1.0f : 0.0f;
    return settings.Width * settings.Height + (camera.Zoom > 0.0f ? 1 : 0);
}
