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

#ifndef ANIMUS_VISION_CAMERA_H
#define ANIMUS_VISION_CAMERA_H

#include <cmath>
#include <cstdint>

/// **The bot's camera** (camera-vision plan, naive slice V1: camera-vision.INTERFACE.md): WoW's third-person camera,
/// client-side only -- never in Movement::ControlState or the Body, so it sends nothing -- held in follow mode (yaw
/// offset 0, the conf's pitch and zoom) for this slice. Each frame is a W x H grid of rays from the camera, each pixel
/// five channels: distance, height of the hit over the feet, the surface's normal z, what was hit, and whether the
/// ray passed the objective. A ray has no range: it ends at what it hits, or reads sky where nothing loaded is left
/// for it to hit (camera-vision.RAYCAST.md). Pure: the settings, the kinds and the geometry, with no core types.
namespace Animus::Vision
{
    /// AnimusForge.Vision.* (worldserver.conf.dist). Angles in degrees, as the conf has them.
    struct Settings
    {
        uint32_t Width = 64;
        uint32_t Height = 32;
        float FovH = 120.0f;
        float FovV = 60.0f;
        float Range = 100.0f;       // yards: the units a frame can see (a ray itself has no range)
        float Zoom = 6.0f;
        float Pitch = -15.0f;       // degrees, + up: the default looks slightly down
    };

    /// What a pixel's ray hit: the kind channel's values (the interface's table).
    enum class Kind : uint8_t
    {
        Sky = 0,        // nothing within range
        Terrain = 1,    // the heightfield
        Model = 2,      // the static tree: a WMO or an M2, not told apart in this slice
        Door = 3,       // the dynamic tree: a door or another game object
        Water = 4,      // a liquid's surface
        Deadly = 5,     // magma or slime
        Hostile = 6,    // a unit hostile to the seat
        Other = 7,      // any other unit
        Count
    };

    constexpr uint32_t CHANNELS = 5;
    constexpr uint32_t KINDS = uint32_t(Kind::Count);
    constexpr uint32_t KIND_CHANNEL = 3;
    constexpr uint32_t SCALARS = 7;

    enum Channel : uint32_t
    {
        CHANNEL_DISTANCE = 0,
        CHANNEL_HEIGHT = 1,
        CHANNEL_NORMAL = 2,
        CHANNEL_KIND = KIND_CHANNEL,
        CHANNEL_OBJECTIVE = 4,
    };

    /// The scalars after the image, in order.
    enum Scalar : uint32_t
    {
        SCALAR_YAW_OFFSET = 0,      // / pi
        SCALAR_PITCH,               // radians / (pi / 2)
        SCALAR_ZOOM,                // / ZOOM_SCALE
        SCALAR_BOOM,                // / ZOOM_SCALE
        SCALAR_PIVOT_HEIGHT,        // above the floor below it / PIVOT_HEIGHT_SCALE, clamped; 1 with no floor
        SCALAR_UNDERWATER,          // the camera is under a liquid's surface
        SCALAR_AIRBORNE,            // the body is falling or flying
    };

    constexpr float PI = 3.14159265358979f;
    constexpr float DEGREES = PI / 180.0f;

    /// The pivot the camera orbits: the head, this share of the collision height above the feet.
    constexpr float PIVOT_SHARE = 0.9f;
    /// The boom pulls in to this short of what it meets, and never nearer the pivot than BOOM_MIN.
    constexpr float BOOM_BACKOFF = 0.2f;
    constexpr float BOOM_MIN = 0.3f;
    /// The distance channel: ln(max(d, NEAR) / NEAR) / ln(DISTANCE_REFERENCE / NEAR), clamped to 1 (and 1 for sky).
    constexpr float NEAR = 0.25f;
    constexpr float DISTANCE_REFERENCE = 1000.0f;
    /// The longest a ray is cast when its path never leaves the loaded grids (a ray straight up or down): far past
    /// the distance channel's reference, so it is no range a frame can see.
    constexpr float REACH_MAX = 4000.0f;
    /// The height channel: (hit z - feet z) / HEIGHT_SCALE, clamped to [-1, 1].
    constexpr float HEIGHT_SCALE = 25.0f;
    /// A ray flags the objective when it passes within this many yards of it.
    constexpr float OBJECTIVE_RADIUS = 1.0f;
    /// A model or door hit is a floor when the floor found from FLOOR_LOOK above it is within FLOOR_MATCH of it.
    constexpr float FLOOR_LOOK = 0.5f;
    constexpr float FLOOR_MATCH = 0.25f;
    constexpr float ZOOM_SCALE = 12.0f;
    constexpr float PIVOT_HEIGHT_SCALE = 10.0f;
    /// The terrain's grids as the core's GridTerrainData lays them out: 64 x 64 grids of SIZE_OF_GRIDS yards, each
    /// 128 x 128 cells (MAP_RESOLUTION), indexed from the map's +x / +y edge: u = 128 * (32 - x / GRID_SIZE).
    constexpr float GRID_SIZE = 533.3333f;
    constexpr int32_t GRID_CELLS = 128;
    constexpr int32_t GRIDS = 64;

    /// A world x (y) as a cell coordinate u (v), and back: the cell is floor(u), its grid floor(u / 128).
    [[nodiscard]] constexpr float GridU(float x) { return float(GRID_CELLS) * (float(GRIDS / 2) - x / GRID_SIZE); }
    [[nodiscard]] constexpr float WorldOfU(float u) { return (float(GRIDS / 2) - u / float(GRID_CELLS)) * GRID_SIZE; }

    /// The vision block's width: the image, then the scalars.
    [[nodiscard]] constexpr uint32_t ImageCount(Settings const& settings)
    {
        return settings.Width * settings.Height * CHANNELS;
    }

    [[nodiscard]] constexpr uint32_t ObsCount(Settings const& settings)
    {
        return ImageCount(settings) + SCALARS;
    }

    /// The process's settings, set once at startup (AnimusForge::Forge::OnStartup) before any layout is built: the
    /// width and height decide the vision block's size.
    [[nodiscard]] Settings const& Current();
    void Configure(Settings const& settings);

    struct Vec3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    [[nodiscard]] inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.X + b.X, a.Y + b.Y, a.Z + b.Z }; }
    [[nodiscard]] inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.X - b.X, a.Y - b.Y, a.Z - b.Z }; }
    [[nodiscard]] inline Vec3 operator*(Vec3 a, float s) { return { a.X * s, a.Y * s, a.Z * s }; }
    [[nodiscard]] inline float Dot(Vec3 a, Vec3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    [[nodiscard]] inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }

    /// A unit vector from an azimuth (WoW's yaw: counter-clockwise from +x, so + is to the left) and an elevation
    /// (+ up), both in radians.
    [[nodiscard]] inline Vec3 Direction(float azimuth, float elevation)
    {
        return { std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth),
            std::sin(elevation) };
    }
}

#endif
