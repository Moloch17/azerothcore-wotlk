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

#ifndef ANIMUS_VISION_VISION_CASTER_H
#define ANIMUS_VISION_VISION_CASTER_H

#include "Camera.h"
#include "PlayerController.h"
#include <span>

/// **The naive caster** (camera-vision INTERFACE, "What a ray hits"): every pixel's ray against the static and
/// dynamic collision trees (two casts, the nearer kept), a 1 yd march for the terrain and liquids up to that hit, and
/// every unit's cylinder. No optimisation of any kind: this is the reference the later casters are measured against.
/// Pure over VisionWorld, so it is tested on fake worlds (VisionTest) and run on a live map (MapVisionWorld).
namespace Animus::Vision
{
    /// The geometry a camera sees. Heights are the feet's convention of Movement::WorldQuery.
    class VisionWorld
    {
    public:
        virtual ~VisionWorld() = default;
        /// How far along the segment the first solid of the static tree (WMOs, M2s) is, or < 0 when none is.
        [[nodiscard]] virtual float StaticHit(Vec3 from, Vec3 to) const = 0;
        /// ... of the dynamic tree (doors and other game objects, phase-masked).
        [[nodiscard]] virtual float DynamicHit(Vec3 from, Vec3 to) const = 0;
        /// The terrain heightfield's height alone, or at most NO_TERRAIN where there is none.
        [[nodiscard]] virtual float TerrainHeight(float x, float y) const = 0;
        [[nodiscard]] virtual Movement::Liquid LiquidAt(float x, float y, float z) const = 0;
        /// The highest floor (terrain, models, doors) at or below z within `search`, or Movement::INVALID_FLOOR.
        [[nodiscard]] virtual float FloorBelow(float x, float y, float z, float search) const = 0;
        [[nodiscard]] virtual float FloorNormalZ(float x, float y, float z) const = 0;
    };

    /// A unit as a ray sees it: a vertical cylinder from its feet. `Self` is the seat's own character, never seen.
    struct UnitShape
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Radius = 0.0f;
        float Height = 0.0f;
        bool Hostile = false;
        bool Self = false;
    };

    /// Where the seat is: its feet, its facing (radians, WoW's counter-clockwise yaw), its body height.
    struct Pose
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Yaw = 0.0f;
        float BodyHeight = 2.0f;
        bool Airborne = false;
    };

    /// The camera's own state, in radians and yards. Follow mode: yaw offset 0.
    struct CameraState
    {
        float YawOffset = 0.0f;
        float Pitch = 0.0f;
        float Zoom = 6.0f;
    };

    /// The camera placed for a frame: the pivot, the camera after the boom's pull-in, and the view's angles.
    struct Rig
    {
        Vec3 Pivot;
        Vec3 Camera;
        float Azimuth = 0.0f;       // facing + yaw offset
        float Elevation = 0.0f;     // the camera's pitch
        float Boom = 0.0f;          // pivot to camera, yards
    };

    struct Hit
    {
        float Distance = 0.0f;      // from the ray's origin
        Kind What = Kind::Sky;
        float Z = 0.0f;             // the hit's height
        float NormalZ = 0.0f;
    };

    /// Where a frame's time and work went: filled only when asked for (the snapshot and the timing test), since
    /// the clock reads cost something of their own.
    struct Breakdown
    {
        uint64_t TreeNs = 0;        // the static and dynamic casts, with the model floor test
        uint64_t MarchNs = 0;       // the terrain and liquid march
        uint64_t UnitNs = 0;        // the cylinders
        uint32_t Rays = 0;          // pixel rays and the boom
        uint32_t TreeCasts = 0;
        uint32_t MarchSteps = 0;
        uint32_t UnitTests = 0;
    };

    /// The pivot, the boom's pull-in (one cast from the pivot back along the view, against the trees and the
    /// terrain) and the camera.
    [[nodiscard]] Rig PlaceCamera(Pose const& pose, CameraState const& camera, VisionWorld const& world,
        Breakdown* breakdown = nullptr);

    /// The angles of pixel (row, col), row 0 at the top and col 0 at the left, as offsets from the view: yaw' + to
    /// the right, pitch' + up, in equal-angle steps:
    ///     yaw'   = (col + 0.5) / W * FovH - FovH / 2
    ///     pitch' = FovV / 2 - (row + 0.5) / H * FovV
    /// The ray's azimuth is the view's minus yaw' (WoW's yaw turns left, so the right of the image is clockwise of
    /// the facing) and its elevation the view's plus pitch'.
    void PixelAngles(Settings const& settings, uint32_t row, uint32_t col, float& yawRight, float& pitchUp);
    [[nodiscard]] Vec3 PixelDirection(Rig const& rig, Settings const& settings, uint32_t row, uint32_t col);

    /// The nearest thing along `dir` (a unit vector) from `origin` within `range`; Sky at `range` when nothing is.
    /// `underwater`: the origin is under a liquid's surface, which the ray ignores until it has risen above it.
    [[nodiscard]] Hit CastRay(Vec3 origin, Vec3 dir, float range, VisionWorld const& world,
        std::span<UnitShape const> units, bool underwater, Breakdown* breakdown = nullptr);

    /// The distance along `dir` at which the ray meets the cylinder, or < 0 for a miss; `top` says the cap was hit.
    [[nodiscard]] float RayCylinder(Vec3 origin, Vec3 dir, float range, UnitShape const& unit, bool& top);

    /// 1 when the closed segment from `origin` to `distance` along `dir` comes within OBJECTIVE_RADIUS of the
    /// objective, else 0 (and 0 with none).
    [[nodiscard]] float ObjectiveFlag(Vec3 origin, Vec3 dir, float distance, Vec3 const* objective);

    /// A pixel's five channels.
    void EncodePixel(Hit const& hit, float feetZ, float range, float objective, float* out);

    /// A whole frame into `out` (ObsCount floats): the image, [row][col][channel] with row 0 at the top, then the
    /// seven scalars. Returns the rays cast (every pixel's and the boom's).
    uint32_t Render(Settings const& settings, Pose const& pose, CameraState const& camera, VisionWorld const& world,
        std::span<UnitShape const> units, Vec3 const* objective, float* out, Breakdown* breakdown = nullptr);
}

#endif
