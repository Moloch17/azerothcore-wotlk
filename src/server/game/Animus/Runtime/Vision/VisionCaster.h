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
#include "MentalMap.h"
#include "PlayerController.h"
#include <span>
#include <vector>

/// **The caster** (camera-vision INTERFACE, "What a ray hits"; camera-vision.RAYCAST.md): every pixel's ray cast
/// exactly against what the server has loaded -- the static and dynamic collision trees (two casts, the nearer kept),
/// the WMO liquids in the static tree, the terrain's own triangles cell by cell along the ray's path with each cell's
/// liquid surface, and the frame's ground hazards painted on the floor. The static world only: units and objects are
/// not drawn (EntitySensor finds them by line of sight). Nothing is marched, baked, cached or skipped: a ray ends at
/// its first hit, or is sky once it has left the loaded grids. Pure over VisionWorld, run on a live map
/// (MapVisionWorld).
namespace Animus::Vision
{
    /// A terrain grid as the camera needs it: whether it is loaded (a ray leaving the loaded grids is sky), whether
    /// it has heights and its highest point (the map file's height header), and whether it has any liquid.
    struct TerrainTile
    {
        bool Loaded = false;
        bool Heights = false;
        float MaxHeight = 0.0f;
        bool Liquid = false;
    };

    /// One terrain cell (GRID_SIZE / 128 yards a side): its heights at the corners (u, v), (u + 1, v), (u, v + 1),
    /// (u + 1, v + 1) and at its centre -- four triangles round the centre, as GridTerrainData::getHeight -- and its
    /// liquid's level (a plane over the cell) when it has one. Not Solid: a hole, or no heights.
    struct TerrainCell
    {
        bool Solid = false;
        float Corner[4] = {};
        float Centre = 0.0f;
        bool Liquid = false;
        float Level = 0.0f;
        bool Deadly = false;
    };

    /// A collision tree's first solid along a segment: how far along it (< 0 for none) and the hit triangle's normal
    /// z, turned to face the segment's start -- 1 a floor seen from above, 0 a wall, below 0 a ceiling from under it.
    /// Object, for the dynamic tree, is the game object model hit (its GameObjectModel, opaque here): the entity
    /// sensor ignores the one it is looking at.
    struct SurfaceHit
    {
        float Distance = -1.0f;
        float NormalZ = 0.0f;
        void const* Object = nullptr;
    };

    struct LiquidHit
    {
        float Distance = -1.0f;     // along the segment, or < 0 for none
        bool Deadly = false;
    };

    /// The geometry a camera sees. Heights are the feet's convention of Movement::WorldQuery.
    class VisionWorld
    {
    public:
        virtual ~VisionWorld() = default;
        /// The first solid of the static tree (WMOs, M2s) along the segment, and its slope.
        [[nodiscard]] virtual SurfaceHit StaticHit(Vec3 from, Vec3 to) const = 0;
        /// ... of the dynamic tree (doors and other game objects, phase-masked).
        [[nodiscard]] virtual SurfaceHit DynamicHit(Vec3 from, Vec3 to) const = 0;
        /// The first WMO liquid surface along the segment (the static tree's group liquids).
        [[nodiscard]] virtual LiquidHit ModelLiquid(Vec3 from, Vec3 to) const = 0;
        /// Whether anything solid of the static tree (WMOs and M2s alike) lies on the segment: an any-hit test, which
        /// stops at the first thing found (entity-sensing's shadow rays).
        [[nodiscard]] virtual bool StaticAnyHit(Vec3 from, Vec3 to) const = 0;
        /// ... of the dynamic tree (enabled collision game objects, phase-masked).
        [[nodiscard]] virtual bool DynamicAnyHit(Vec3 from, Vec3 to) const = 0;
        /// Terrain grid (tileX, tileY), as GridCoord numbers them (u / 128, v / 128).
        [[nodiscard]] virtual TerrainTile Tile(int32_t tileX, int32_t tileY) const = 0;
        /// Cell (cellX, cellY), 0 to 127, of that grid; its liquid only when `liquid` is asked for.
        [[nodiscard]] virtual TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY,
            bool liquid) const = 0;
        [[nodiscard]] virtual Movement::Liquid LiquidAt(float x, float y, float z) const = 0;
        /// The highest floor (terrain, models, doors) at or below z within `search`, or Movement::INVALID_FLOOR.
        [[nodiscard]] virtual float FloorBelow(float x, float y, float z, float search) const = 0;
    };

    /// **A ground hazard as a ray sees it** (Class::GroundHazard): a flat disc on the ground at the area's centre
    /// (Z the floor there) with its radius -- what the client draws of a fire pool or a poison cloud. A ray that
    /// meets the floor (a normal z of FLOOR_NORMAL or more) within the disc, and within HAZARD_REACH of its height,
    /// reads the hazard's class instead of the terrain's. A flat decal: it hides nothing behind it.
    struct HazardDisc
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Radius = 0.0f;
    };

    /// The disc's thickness as the entity list sizes it, and how far below the area's centre its middle sits.
    constexpr float HAZARD_THICKNESS = 0.2f;
    constexpr float HAZARD_SINK = 0.05f;
    /// A floor hit is painted when its height is within this of the disc's.
    constexpr float HAZARD_REACH = 0.5f;

    /// **The hazards a frame paints**: the static world's pixels read these on the floor. Empty is a frame with none.
    using Sight = std::span<HazardDisc const>;

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

    /// The camera's own state, in radians and yards (FreeLook::CameraOf builds it from a seat's look). The held rates
    /// (radians a second, + left / + up) are only reported, as scalars; the camera has already been turned by them.
    /// RenderWidth x RenderHeight is the size the frame is cast at, scaled up into the canonical image; 0 casts at
    /// the canonical size itself.
    struct CameraState
    {
        float YawOffset = 0.0f;
        float Pitch = 0.0f;
        float Zoom = 6.0f;
        float YawRate = 0.0f;
        float PitchRate = 0.0f;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
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
        Class What = Class::Sky;
        float Z = 0.0f;             // the hit's height
        float NormalZ = 0.0f;
    };

    /// Where a frame's time and work went: filled only when asked for (the snapshot and the timing test), since
    /// the clock reads cost something of their own.
    struct Breakdown
    {
        uint64_t StaticNs = 0;      // the static world's cast (the static tree, or the baked scene)
        uint64_t DynamicNs = 0;     // the dynamic tree's cast (doors and other game objects)
        uint64_t LiquidNs = 0;      // the WMO liquids
        uint64_t TerrainNs = 0;     // the loaded grids' extent and the terrain cells (their triangles and liquid)
        uint64_t HazardNs = 0;      // the hazard discs on a floor hit
        uint32_t Rays = 0;          // pixel rays and the boom
        uint32_t SegmentRays = 0;   // the entity sensor's shadow rays (SegmentBlocked)
        uint32_t StaticCasts = 0;
        uint32_t DynamicCasts = 0;
        uint32_t LiquidCasts = 0;
        uint32_t TerrainTiles = 0;  // grids a ray's terrain cast entered
        uint32_t TerrainCells = 0;  // cells whose triangles or liquid it tested
        uint32_t HazardTests = 0;   // hazard discs tested against a floor hit
    };

    /// **A segment's line of sight** (entity-sensing's shadow ray): whether anything solid lies on the open segment
    /// from `from` to `to` -- the static tree (any-hit), the dynamic tree, and the terrain from above; no liquids, no
    /// units. `ignore`, a game object model (SurfaceHit::Object), is the target itself: a model between the ends that
    /// is the target does not block, and the rest of the segment is tested only as far as its surface. Counts into
    /// `breakdown` as one ray.
    [[nodiscard]] bool SegmentBlocked(VisionWorld const& world, Vec3 from, Vec3 to, void const* ignore = nullptr,
        Breakdown* breakdown = nullptr);

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

    /// How far the ray runs before its path leaves the loaded terrain grids (REACH_MAX when it never does).
    [[nodiscard]] float Reach(Vec3 origin, Vec3 dir, VisionWorld const& world);

    /// The terrain's first triangle (from above) or cell liquid surface (from above, where the ground there is below
    /// it) along the ray within `limit`, visiting the cells its path crosses in order; Sky when none.
    [[nodiscard]] Hit CastTerrain(Vec3 origin, Vec3 dir, float limit, VisionWorld const& world, bool liquids,
        Breakdown* breakdown = nullptr);

    /// The nearest thing along `dir` (a unit vector) from `origin`: nothing is a range, only the loaded grids'
    /// extent (Reach), at which the ray is Sky. A liquid's surface is only ever entered from above, so a camera
    /// under one sees through it upwards. In order: the trees (a dynamic hit is a door), the WMO liquids, the
    /// terrain; the nearest wins, the earlier on a tie. A floor hit within a hazard disc of `sight` reads as the
    /// hazard.
    [[nodiscard]] Hit CastRay(Vec3 origin, Vec3 dir, VisionWorld const& world, Sight const& sight,
        Breakdown* breakdown = nullptr);

    /// 1 when the closed segment from `origin` to `distance` along `dir` (the hit, or the reach on sky) comes within
    /// `radius` (OBJECTIVE_RADIUS, or an object's ObjectiveRadiusFor) of the objective, else 0 (and 0 with none).
    [[nodiscard]] float ObjectiveFlag(Vec3 origin, Vec3 dir, float distance, Vec3 const* objective,
        float radius = OBJECTIVE_RADIUS);

    /// A pixel's four bytes (Camera.h, BYTES_PER_PIXEL).
    void EncodePixel(Hit const& hit, float feetZ, bool objective, uint8_t* out);
    /// The learner's decode of them: the five image channels (distance, height, normal, class, objective), quantised,
    /// DECODED_VALUES in all. For `forge camera snapshot` and the audit; the network decodes its own.
    void DecodePixel(uint8_t const* in, float* out);

    /// A whole frame's static world: the image into `image` (ImageBytes, [row][col][byte] with row 0 at the top; null
    /// to cast no pixel), the eleven scalars into `scalars`. `rig` is the camera as PlaceCamera placed it (once a
    /// decision: the sensor reads it too). `settings` is the canonical size; the pixels are cast at the camera's
    /// RenderWidth x RenderHeight (the same field of view, fewer and wider rays) and scaled up into the image by
    /// nearest pixel (Upscale). `sight` takes the frame's ground hazards. Returns the rays actually cast (every cast
    /// pixel's and the boom's).
    ///
    /// **The frame's rays for the mental map** (perception-goals REDESIGN §3, amendment 2): `hits`, when given, takes
    /// every cast pixel's ray -- its direction, its hit's distance, height and normal z, and its class (a hazard
    /// painted on the floor included, as the pixel has it) -- at the size the frame was cast at, not the canonical
    /// image's upscaled copies.
    uint32_t Render(Settings const& settings, Rig const& rig, Pose const& pose, CameraState const& camera,
        VisionWorld const& world, Sight const& sight, Vec3 const* objective, uint8_t* image, float* scalars,
        Breakdown* breakdown = nullptr, float objectiveRadius = OBJECTIVE_RADIUS, FrameHits* hits = nullptr);
}

#endif
