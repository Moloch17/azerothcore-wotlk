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
#include <array>
#include <span>
#include <vector>

/// **The caster** (camera-vision INTERFACE, "What a ray hits"; camera-vision.RAYCAST.md): every pixel's ray cast
/// exactly against what the server has loaded -- the static and dynamic collision trees (two casts, the nearer kept),
/// the WMO liquids in the static tree, the terrain's own triangles cell by cell along the ray's path with each cell's
/// liquid surface, and every visible unit's cylinder. Nothing is marched, baked, cached or skipped: a ray ends at its
/// first hit, or is sky once it has left the loaded grids. Pure over VisionWorld, so it is tested on fake worlds
/// (VisionTest) and run on a live map (MapVisionWorld).
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
    /// Object, for the dynamic tree, is the game object model hit (its GameObjectModel, opaque here): the frame's
    /// DoorShape of it says what it is (perception-goals 1a).
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
        /// Terrain grid (tileX, tileY), as GridCoord numbers them (u / 128, v / 128).
        [[nodiscard]] virtual TerrainTile Tile(int32_t tileX, int32_t tileY) const = 0;
        /// Cell (cellX, cellY), 0 to 127, of that grid; its liquid only when `liquid` is asked for.
        [[nodiscard]] virtual TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY,
            bool liquid) const = 0;
        [[nodiscard]] virtual Movement::Liquid LiquidAt(float x, float y, float z) const = 0;
        /// The highest floor (terrain, models, doors) at or below z within `search`, or Movement::INVALID_FLOOR.
        [[nodiscard]] virtual float FloorBelow(float x, float y, float z, float search) const = 0;
    };

    /// A unit as a ray sees it: a vertical cylinder from its feet. `Self` is the seat's own character, never seen.
    /// Entity is its number in the frame (NumberNearest: 1 the nearest), 0 past MAX_SEEN or not numbered.
    struct UnitShape
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Radius = 0.0f;
        float Height = 0.0f;
        /// What it is to the seat (Classify): its pixels' class.
        Class What = Class::NeutralCreature;
        bool Self = false;
        uint8_t Entity = 0;
    };

    /// **A game object with no collision model** (a herb, most chests, a mailbox) as a ray sees it: its display's
    /// bounding box (GameObjectDisplayInfo's bounds, scaled), turned as the object is (perception-goals 1a: cast as
    /// its bounding shape, as a unit is a cylinder). The box's space is the object's: a point p is at
    /// InvRot (p - origin) there, InvRot row-major (the inverse of the object's rotation).
    struct BoxShape
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float InvRot[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        float Low[3] = {};
        float High[3] = {};
        Class What = Class::OtherObject;
        uint8_t Entity = 0;
    };

    /// A game object with a collision model, in the dynamic tree: what a ray that hits `Model` (its GameObjectModel)
    /// has hit. A dynamic hit on a model no DoorShape names is a door with no entity (Class::Door).
    struct DoorShape
    {
        void const* Model = nullptr;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        Class What = Class::Door;
        uint8_t Entity = 0;
    };

    /// **The entities a frame can see**, as the caster reads them: the units' cylinders, the colliderless game
    /// objects' boxes and the collision game objects' identities. Built from units alone where there is nothing
    /// else (the tests).
    struct Sight
    {
        Sight() = default;
        Sight(std::span<UnitShape const> units) : Units(units) { }
        Sight(std::vector<UnitShape> const& units) : Units(units) { }

        std::span<UnitShape const> Units;
        std::span<BoxShape const> Boxes;
        std::span<DoorShape const> Doors;
    };

    /// **A frame's entity list**, as the frame decided it (perception-goals 1b): Slots[s - 1] is pixel slot s's
    /// entity (its number) and its pixels at the size the frame was cast at (CastWidth x CastHeight), the nearest
    /// first.
    struct FrameSlots
    {
        uint32_t Count = 0;
        uint32_t CastWidth = 0;
        uint32_t CastHeight = 0;
        std::array<SlotStat, ENTITY_SLOTS> Slots{};
        /// The camera the frame was seen from (Render fills it): where it was, and the view's azimuth and elevation.
        Vec3 Camera;
        float Azimuth = 0.0f;
        float Elevation = 0.0f;
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
        uint8_t Entity = 0;         // the entity's number in the frame (NumberNearest), 0 none
    };

    /// Where a frame's time and work went: filled only when asked for (the snapshot and the timing test), since
    /// the clock reads cost something of their own.
    struct Breakdown
    {
        uint64_t TreeNs = 0;        // the static and dynamic casts
        uint64_t LiquidNs = 0;      // the WMO liquids
        uint64_t TerrainNs = 0;     // the loaded grids' extent and the terrain cells (their triangles and liquid)
        uint64_t UnitNs = 0;        // the cylinders
        uint32_t Rays = 0;          // pixel rays and the boom
        uint32_t TreeCasts = 0;
        uint32_t LiquidCasts = 0;
        uint32_t TerrainTiles = 0;  // grids a ray's terrain cast entered
        uint32_t TerrainCells = 0;  // cells whose triangles or liquid it tested
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

    /// How far the ray runs before its path leaves the loaded terrain grids (REACH_MAX when it never does).
    [[nodiscard]] float Reach(Vec3 origin, Vec3 dir, VisionWorld const& world);

    /// The terrain's first triangle (from above) or cell liquid surface (from above, where the ground there is below
    /// it) along the ray within `limit`, visiting the cells its path crosses in order; Sky when none.
    [[nodiscard]] Hit CastTerrain(Vec3 origin, Vec3 dir, float limit, VisionWorld const& world, bool liquids,
        Breakdown* breakdown = nullptr);

    /// The nearest thing along `dir` (a unit vector) from `origin`: nothing is a range, only the loaded grids'
    /// extent (Reach), at which the ray is Sky. A liquid's surface is only ever entered from above, so a camera
    /// under one sees through it upwards. In order: the trees (a dynamic hit is its model's DoorShape), the WMO
    /// liquids, the terrain, the units' cylinders, the boxes; the nearest wins, the earlier on a tie.
    [[nodiscard]] Hit CastRay(Vec3 origin, Vec3 dir, VisionWorld const& world, Sight const& sight,
        Breakdown* breakdown = nullptr);

    /// The distance along `dir` at which the ray meets the cylinder, or < 0 for a miss; `top` says the cap was hit.
    [[nodiscard]] float RayCylinder(Vec3 origin, Vec3 dir, float limit, UnitShape const& unit, bool& top);

    /// The distance along `dir` at which the ray enters the box from outside, within `limit`, or < 0 for a miss (a
    /// ray from inside sees none of it, as with a cylinder); `normalZ` takes the entered face's world normal z,
    /// turned to face the ray (1 a top seen from above).
    [[nodiscard]] float RayBox(Vec3 origin, Vec3 dir, float limit, BoxShape const& box, float& normalZ);

    /// Numbers a frame's entities nearest first (perception-goals 1b): numbers[i] is 1 + the rank of distances[i]
    /// (squared distances from the seat's head; a tie goes to the lower i) when that rank is below MAX_SEEN, else
    /// 0 -- an entity past the cap is still cast, with its class, and never listed.
    void NumberNearest(std::span<float const> distances, std::span<uint8_t> numbers);

    /// 1 when the closed segment from `origin` to `distance` along `dir` (the hit, or the reach on sky) comes within
    /// `radius` (OBJECTIVE_RADIUS, or an object's ObjectiveRadiusFor) of the objective, else 0 (and 0 with none).
    [[nodiscard]] float ObjectiveFlag(Vec3 origin, Vec3 dir, float distance, Vec3 const* objective,
        float radius = OBJECTIVE_RADIUS);

    /// A pixel's five bytes (Camera.h, BYTES_PER_PIXEL): `slot` is byte 4 as it is (while a frame is cast, the
    /// hit's entity number; Render then makes it the slot).
    void EncodePixel(Hit const& hit, float feetZ, bool objective, uint8_t slot, uint8_t* out);
    /// The learner's decode of them: the five image channels (distance, height, normal, class, objective), quantised,
    /// then the entity slot (CHANNEL_SLOT), DECODED_VALUES in all. For `forge camera snapshot` and the tests; the
    /// network decodes its own.
    void DecodePixel(uint8_t const* in, float* out);

    /// A whole frame: the image into `image` (ImageBytes, [row][col][byte] with row 0 at the top; null to cast no
    /// pixel), the eleven scalars into `scalars`. `settings` is the canonical size; the pixels are cast at the
    /// camera's RenderWidth x RenderHeight (the same field of view, fewer and wider rays) and scaled up into the
    /// image by nearest pixel (Upscale). Returns the rays actually cast (every cast pixel's and the boom's).
    ///
    /// **The entity slots** (perception-goals 1b): the cast frame's pixels are counted by entity number
    /// (CountEntities), the entities with a pixel take the slots in number order -- the nearest first -- up to
    /// ENTITY_SLOTS (AssignSlots), and byte 4 becomes each pixel's slot (0 past the cap, its class kept) before the
    /// frame is scaled up. `slots`, when given, takes the list.
    ///
    /// **The frame's rays for the mental map** (perception-goals REDESIGN §3, amendment 2): `hits`, when given, takes
    /// every cast pixel's ray -- its direction, its hit's distance, height and normal z, and its class -- at the size
    /// the frame was cast at, not the canonical image's upscaled copies.
    uint32_t Render(Settings const& settings, Pose const& pose, CameraState const& camera, VisionWorld const& world,
        Sight const& sight, Vec3 const* objective, uint8_t* image, float* scalars, Breakdown* breakdown = nullptr,
        float objectiveRadius = OBJECTIVE_RADIUS, FrameSlots* slots = nullptr, FrameHits* hits = nullptr);

    /// A cast frame's pixels per entity number (byte 4): counts[n] -- its pixels and the sums of their rows and
    /// columns -- for n from 1 to MAX_SEEN (counts[0] holds the pixels of no entity).
    void CountEntities(uint8_t const* frame, uint32_t width, uint32_t height,
        std::array<SlotStat, MAX_SEEN + 1>& counts);
    /// The slots from the counts: the numbers with a pixel, ascending, up to ENTITY_SLOTS; slotOf[n] the slot of
    /// number n (0 none).
    void AssignSlots(std::array<SlotStat, MAX_SEEN + 1> const& counts, FrameSlots& slots,
        std::array<uint8_t, MAX_SEEN + 1>& slotOf);
}

#endif
