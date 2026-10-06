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
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// **The bot's camera** (camera-vision plan, naive slice V1: camera-vision.INTERFACE.md): WoW's third-person camera,
/// client-side only -- never in Movement::ControlState or the Body, so it sends nothing -- turned by the seat's own
/// look head (free look: FreeLook.h, camera-vision.FREELOOK.md). Each frame is a W x H grid of rays from the camera, each pixel
/// five channels: distance, height of the hit over the feet, the surface's normal z, what was hit, and whether the
/// ray passed the objective. A ray has no range: it ends at what it hits, or reads sky where nothing loaded is left
/// for it to hit (camera-vision.RAYCAST.md). Pure: the settings, the kinds and the geometry, with no core types.
namespace Animus::Vision
{
    /// A frame's size in pixels: columns by rows.
    struct Resolution
    {
        uint32_t Width = 0;
        uint32_t Height = 0;

        [[nodiscard]] bool operator==(Resolution const& other) const = default;
    };

    /// AnimusForge.Vision.* (worldserver.conf.dist). Angles in degrees, as the conf has them. Width x Height is the
    /// canonical image, the one the wire carries and the network reads (FREELOOK A); RenderSizes are what frames are
    /// actually cast at, each scaled up into it by nearest pixel (Upscale).
    struct Settings
    {
        uint32_t Width = 128;
        uint32_t Height = 64;
        float FovH = 120.0f;
        float FovV = 60.0f;
        float Range = 100.0f;       // yards: the units a frame can see (a ray itself has no range)
        float Zoom = 6.0f;
        float Pitch = -15.0f;       // degrees, + up: the default looks slightly down
        /// AnimusForge.Vision.RenderSizes: a seat draws one at every episode reset, by weight, and casts its frames
        /// at it; each is at most Width x Height. One entry renders every seat at it (a user's tier). The default
        /// renders about one episode in eight natively at 128 x 64 (0.4 of 3.4), so the model also sees what a GPU
        /// renderer at the canonical size will give it.
        std::vector<Resolution> RenderSizes = { { 32, 16 }, { 48, 24 }, { 64, 32 }, { 128, 64 } };
        /// ... and each one's weight in that draw (> 0), parallel to RenderSizes.
        std::vector<float> RenderWeights = { 1.0f, 1.0f, 1.0f, 0.4f };
    };

    /// AnimusForge.Vision.RenderSizes' default, as the conf writes it.
    constexpr char const* DEFAULT_RENDER_SIZES = "32x16, 48x24, 64x32, 128x64:0.4";

    /// The learner's patch is this share of the canonical width: a PATCH_COLUMNS-wide grid of patches (16 x 8 at 2:1).
    constexpr uint32_t PATCH_COLUMNS = 16;

    /// The learner's patch size at the canonical size (the manifest's "patch"): 8 at 128 x 64, 4 at 64 x 32.
    [[nodiscard]] inline uint32_t Patch(Settings const& settings)
    {
        return settings.Width / PATCH_COLUMNS > 0 ? settings.Width / PATCH_COLUMNS : 1;
    }

    /// AnimusForge.Vision.RenderSizes' text, "WxH, WxH:w, ...": the sizes kept, in order, each 1 to the canonical
    /// size's, with their weights in `weights` (w > 0; 1 when not given); an entry that does not parse or does not
    /// fit is left out with a line in `errors`. None kept: the canonical size alone, weight 1 (and a line saying so).
    [[nodiscard]] std::vector<Resolution> ParseRenderSizes(std::string const& text, Settings const& canonical,
        std::vector<float>& weights, std::vector<std::string>& errors);

    /// The size a seat casts its frames at this episode: one of RenderSizes, drawn by RenderWeights (a missing or
    /// non-positive weight counts as 1), `roll(total)` giving a number in [0, total) from the pool's random numbers.
    /// Rolled only when there is a choice -- so one entry draws nothing and every seat renders at it. None
    /// configured: the canonical size.
    template <typename Roll>
    [[nodiscard]] Resolution DrawRenderSize(Settings const& settings, Roll&& roll)
    {
        std::vector<Resolution> const& sizes = settings.RenderSizes;
        if (sizes.empty())
            return { settings.Width, settings.Height };
        if (sizes.size() == 1)
            return sizes.front();
        auto const weight = [&settings](std::size_t i)
        {
            return i < settings.RenderWeights.size() && settings.RenderWeights[i] > 0.0f ? settings.RenderWeights[i]
                : 1.0f;
        };
        float total = 0.0f;
        for (std::size_t i = 0; i < sizes.size(); ++i)
            total += weight(i);
        float pick = float(roll(total));
        for (std::size_t i = 0; i < sizes.size(); ++i)
        {
            if (pick < weight(i))
                return sizes[i];
            pick -= weight(i);
        }
        return sizes.back();        // a roll at the very top of the range (rounding)
    }

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
    constexpr uint32_t SCALARS = 11;

    /// **A pixel on the wire** (camera-vision.BYTES.md, vision block revision 3): four bytes, which the learner decodes
    /// back to the five channels below.
    ///   0 distance: SKY_BYTE for sky, else round(254 x the log-scaled distance, 0..1);
    ///   1 height over the feet: HEIGHT_ZERO + clamp(round(dz / HEIGHT_STEP), -HEIGHT_LIMIT, HEIGHT_LIMIT); sky 128;
    ///   2 normal z: round(255 x clamp(nz, 0, 1));
    ///   3 the kind in the low four bits, the objective flag in bit 4 (OBJECTIVE_BIT); bits 5-7 are 0.
    constexpr uint32_t BYTES_PER_PIXEL = 4;
    constexpr uint8_t SKY_BYTE = 255;
    constexpr float DISTANCE_LEVELS = 254.0f;
    constexpr uint8_t HEIGHT_ZERO = 128;
    constexpr float HEIGHT_STEP = 0.2f;
    constexpr int32_t HEIGHT_LIMIT = 125;
    constexpr uint8_t KIND_MASK = 0x0F;
    constexpr uint8_t OBJECTIVE_BIT = 0x10;

    enum Channel : uint32_t
    {
        CHANNEL_DISTANCE = 0,
        CHANNEL_HEIGHT = 1,
        CHANNEL_NORMAL = 2,
        CHANNEL_KIND = KIND_CHANNEL,
        CHANNEL_OBJECTIVE = 4,
    };

    /// The scalars after the image, in order (vision block revision 4, FREELOOK B).
    enum Scalar : uint32_t
    {
        SCALAR_YAW_SIN = 0,         // sin(yaw offset)
        SCALAR_YAW_COS,             // cos(yaw offset)
        SCALAR_PITCH,               // radians / (pi / 2)
        SCALAR_ZOOM,                // / ZOOM_SCALE
        SCALAR_BOOM,                // / ZOOM_SCALE
        SCALAR_PIVOT_HEIGHT,        // above the floor below it / PIVOT_HEIGHT_SCALE, clamped; 1 with no floor
        SCALAR_UNDERWATER,          // the camera is under a liquid's surface
        SCALAR_AIRBORNE,            // the body is falling or flying
        SCALAR_YAW_RATE,            // the held yaw rate / YAW_RATE_SCALE (+ left)
        SCALAR_PITCH_RATE,          // the held pitch rate / PITCH_RATE_SCALE (+ up)
        SCALAR_RENDER_WIDTH,        // the width the frame was cast at / the canonical width
        SCALAR_COUNT
    };
    static_assert(SCALAR_COUNT == SCALARS, "the contract's eleven vision scalars");

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
    /// The height channel, decoded: (hit z - feet z) / HEIGHT_SCALE, clamped to [-1, 1] (HEIGHT_LIMIT steps of
    /// HEIGHT_STEP on the wire).
    constexpr float HEIGHT_SCALE = 25.0f;
    /// A ray flags the objective when it passes within this many yards of it.
    constexpr float OBJECTIVE_RADIUS = 1.0f;
    constexpr float ZOOM_SCALE = 12.0f;
    /// The held rates' scales: the fastest of each the look head offers (FreeLook.h).
    constexpr float YAW_RATE_SCALE = 180.0f * DEGREES;
    constexpr float PITCH_RATE_SCALE = 60.0f * DEGREES;
    constexpr float PIVOT_HEIGHT_SCALE = 10.0f;
    /// The terrain's grids as the core's GridTerrainData lays them out: 64 x 64 grids of SIZE_OF_GRIDS yards, each
    /// 128 x 128 cells (MAP_RESOLUTION), indexed from the map's +x / +y edge: u = 128 * (32 - x / GRID_SIZE).
    constexpr float GRID_SIZE = 533.3333f;
    constexpr int32_t GRID_CELLS = 128;
    constexpr int32_t GRIDS = 64;

    /// A world x (y) as a cell coordinate u (v), and back: the cell is floor(u), its grid floor(u / 128).
    [[nodiscard]] constexpr float GridU(float x) { return float(GRID_CELLS) * (float(GRIDS / 2) - x / GRID_SIZE); }
    [[nodiscard]] constexpr float WorldOfU(float u) { return (float(GRIDS / 2) - u / float(GRID_CELLS)) * GRID_SIZE; }

    /// An agent's image on the wire: [row][col][byte], row 0 at the top, BYTES_PER_PIXEL a pixel, at the canonical
    /// size whatever size it was cast at.
    [[nodiscard]] inline uint32_t ImageBytes(Settings const& settings)
    {
        return settings.Width * settings.Height * BYTES_PER_PIXEL;
    }

    /// A row with no frame (a director, an absent agent, a seat with no character or no map): every pixel
    /// { SKY_BYTE, HEIGHT_ZERO, 0, 0 } -- sky, height 0, normal 0, no objective -- "nothing seen", in the range the
    /// network trains on, where zeros would read as a wall at the camera a yard below the feet.
    inline void FillNoFrame(uint8_t* image, uint32_t bytes)
    {
        for (uint32_t at = 0; at + BYTES_PER_PIXEL <= bytes; at += BYTES_PER_PIXEL)
        {
            image[at] = SKY_BYTE;
            image[at + 1] = HEIGHT_ZERO;
            image[at + 2] = 0;
            image[at + 3] = 0;
        }
    }

    /// The pixels of an image row that carry the objective flag (OBJECTIVE_BIT): how much of the frame shows the
    /// objective, 0 when it is out of sight (the seek stage's sight measures).
    [[nodiscard]] inline uint32_t CountObjectivePixels(uint8_t const* image, uint32_t bytes)
    {
        uint32_t count = 0;
        for (uint32_t at = 3; at < bytes; at += BYTES_PER_PIXEL)
            count += (image[at] & OBJECTIVE_BIT) ? 1u : 0u;
        return count;
    }

    /// **Nearest-pixel upscaling** (FREELOOK A): a frame cast at w x h into the canonical W x H, canonical pixel
    /// (r, c) taking cast pixel (floor(r h / H), floor(c w / W)). Every byte, the objective flag with them, is copied
    /// as it is; nothing is interpolated. w <= W and h <= H.
    inline void Upscale(uint8_t const* from, uint32_t w, uint32_t h, uint8_t* to, uint32_t W, uint32_t H)
    {
        for (uint32_t r = 0; r < H; ++r)
        {
            uint32_t const sr = uint32_t(uint64_t(r) * h / H);
            for (uint32_t c = 0; c < W; ++c)
            {
                uint32_t const sc = uint32_t(uint64_t(c) * w / W);
                uint8_t const* in = from + (std::size_t(sr) * w + sc) * BYTES_PER_PIXEL;
                uint8_t* out = to + (std::size_t(r) * W + c) * BYTES_PER_PIXEL;
                for (uint32_t b = 0; b < BYTES_PER_PIXEL; ++b)
                    out[b] = in[b];
            }
        }
    }

    /// The vision block's float columns: the scalars alone (revision 3 on; the image travels as bytes beside them).
    [[nodiscard]] inline uint32_t ObsCount(Settings const& /*settings*/)
    {
        return SCALARS;
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
