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

#ifndef ANIMUS_VISION_MENTAL_MAP_H
#define ANIMUS_VISION_MENTAL_MAP_H

#include "Camera.h"
#include "Identity.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

/// **The mental map** (perception-goals REDESIGN §3, its amendments and the Change): what a seat has seen and where it
/// has been, written only from its own camera's frames and its own body -- never from the navmesh, the map's data or
/// anything the seat did not see. Pure: the vision block hands it the frame's rays (FrameHits), the map block the
/// body, and the tests write both by hand.
///
/// **Storage**: world-anchored tiles of MAP_TILE x MAP_TILE cells of a yard, kept sparsely by tile key, eight bytes a
/// cell (MapCell): two floor heights (stacked floors kept apart, amendment 2), what was seen there as flags (a wall
/// at either floor's height, a door, water or a hazard, a step or drop edge), whether the body stood there, when the
/// cell was last seen (map seconds), and the class of the entity last seen there with its age. Capped at
/// MapSettings::MaxTiles, the least recently written tile going first; a realm's cap folds an evicted tile into
/// coarse tiles (MapSettings::CoarseTiles, COARSE_CELL yards a cell) instead of forgetting it.
///
/// **Writes** (WriteFrame, from the frame as cast, before it is scaled up): every ray's hit within WRITE_REACH marks
/// its cell -- a floor (normal z at least FLOOR_NORMAL) at its height layer, else a wall within the body's band of a
/// floor, a door, water or a hazard, or a unit's or game object's class -- and the cells the ray crossed on its way
/// are "seen free" by a 2D grid step over the map's own cells, but only where the ray's segment over the cell lies in
/// the body's band above that cell's floor (with no floor known, within FREE_BAND yards of the feet). A pass-through
/// never marks a floor. WriteBody marks the cells the body stood on visited (and their floor, from the feet).
///
/// **Reads** (Crop): the MapBlock's one egocentric, heading-up crop (the Change, 2026-10-06): CROP x CROP cells of
/// CROP_CELL yards round the body, each summarising its four 1-yd cells, six bytes a cell (CropChannel).
namespace Animus::Vision
{
    /// Cells a tile side, and a cell's size in yards.
    constexpr int32_t MAP_TILE = 32;
    constexpr float MAP_CELL = 1.0f;
    constexpr int32_t MAP_TILE_CELLS = MAP_TILE * MAP_TILE;

    /// A floor height on the map: quarter yards, NO_FLOOR for none.
    constexpr float FLOOR_STEP = 0.25f;
    constexpr int16_t NO_FLOOR = INT16_MIN;
    /// A hit whose normal z is at least this is a floor; at most CEILING_NORMAL a ceiling seen from under it (nothing
    /// for the map); in between a wall.
    constexpr float FLOOR_NORMAL = 0.7f;
    constexpr float CEILING_NORMAL = -0.3f;
    /// A floor within this of a layer's height is that layer (re-seen); further, another storey.
    constexpr float LAYER_MERGE = 1.5f;
    /// Neighbouring floors further apart than this, with no layer of one within it of the other's, are an edge.
    constexpr float EDGE_RISE = 1.0f;
    /// "Seen free" with no floor known: the ray's segment within this many yards of the feet (amendment 2).
    constexpr float FREE_BAND = 2.0f;
    /// A wall counts within the body's band of a floor, and this much below it: a lintel over a doorway, high in a
    /// hall, is no wall where a body walks.
    constexpr float WALL_SLACK = 0.5f;
    /// Rays write their hits, and walk their cells, this far from the camera at most (amendment 2).
    constexpr float WRITE_REACH = 64.0f;

    /// MapCell::Flags.
    enum MapFlag : uint8_t
    {
        MAP_WALL_LOW = 0x01,        // a wall in the band of Floor[0] (or anywhere near the feet, no floor known)
        MAP_WALL_HIGH = 0x02,       // a wall in the band of Floor[1]
        MAP_DOOR = 0x04,            // a door or button (Class::Door)
        MAP_HAZARD = 0x08,          // water, magma or slime
        MAP_EDGE = 0x10,            // a step or drop to a neighbouring floor
        MAP_VISITED = 0x20,         // the body stood here
        MAP_FREE = 0x40,            // a ray passed here within the body's band, hitting nothing
    };

    /// One yard of the map, eight bytes.
    struct MapCell
    {
        int16_t Floor[2] = { NO_FLOOR, NO_FLOOR };  // the floor layers, quarter yards, the lower first
        uint8_t Flags = 0;
        /// The class of the entity last seen here (bits 0-4; 0 none) and its age's bucket past Seen (bits 5-7,
        /// EntityAgeBucket): how long before the cell was last seen the entity was.
        uint8_t Entity = 0;
        uint16_t Seen = 0;                          // the map second it was last seen, + 1; 0 never
    };
    static_assert(sizeof(MapCell) == 8, "a map cell is eight bytes");

    [[nodiscard]] inline int16_t QuantiseFloor(float z)
    {
        float const q = std::round(z / FLOOR_STEP);
        return int16_t(std::clamp(q, float(INT16_MIN + 1), float(INT16_MAX)));
    }
    [[nodiscard]] inline float FloorHeight(int16_t q) { return float(q) * FLOOR_STEP; }

    /// An entity's age past its cell's last look, in eight buckets: under 1 s, 4 s, 15 s, 1 min, 4 min, 15 min, 1 h,
    /// and older; and the bucket's start.
    constexpr float ENTITY_AGE_BUCKETS[8] = { 0.0f, 1.0f, 4.0f, 15.0f, 60.0f, 240.0f, 900.0f, 3600.0f };
    [[nodiscard]] uint8_t EntityAgeBucket(float seconds);

    /// The map's caps (AnimusForge.Map.*): fine tiles, and coarse tiles an evicted fine tile folds into (0: none, a
    /// fine tile evicted is forgotten -- training's).
    struct MapSettings
    {
        uint32_t MaxTiles = 4096;
        uint32_t CoarseTiles = 0;
    };

    /// The process's map settings (AnimusForge.Map.*, set once at startup like the camera's): the caps, and how a
    /// training seat's map lives across resets (amendment 1) -- kept on the same instance KeepShare of the time, its
    /// clock moved on by a random 0 to AgeOffsetSeconds when it is, so the model sees stale memory as a shipped bot
    /// will; else cleared.
    struct MapRunSettings
    {
        MapSettings Caps;
        float KeepShare = 0.5f;
        float AgeOffsetSeconds = 600.0f;
    };
    [[nodiscard]] MapRunSettings const& MapCurrent();
    void ConfigureMap(MapRunSettings const& settings);

    /// A coarse cell's side, yards (amendment 3: far tiles kept at 8 yd).
    constexpr int32_t COARSE_CELL = 8;

    /// One ray of a frame as cast (before the frame is scaled up): from the camera along Dir (a unit vector) to its
    /// hit at Distance -- for sky, where the ray left the loaded grids -- the hit's height and normal z, and what it
    /// hit.
    struct RayHit
    {
        Vec3 Dir;
        float Distance = 0.0f;
        float Z = 0.0f;
        float NormalZ = 0.0f;
        Class What = Class::Sky;
    };

    /// A frame's rays (Render's `hits`): the camera they start from and one per cast pixel, row by row.
    struct FrameHits
    {
        Vec3 Camera;
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<RayHit> Rays;
    };

    /// What a write did, for the tests and the cost row.
    struct MapWriteStats
    {
        uint32_t Hits = 0;          // rays whose hit marked a cell
        uint32_t Floors = 0;
        uint32_t Walls = 0;
        uint32_t Entities = 0;
        uint32_t Steps = 0;         // cells the grid step visited
        uint32_t Free = 0;          // ... marked seen free
    };

    /// **The crop** (the Change): CROP x CROP cells of CROP_CELL yards, heading-up -- row 0 the furthest ahead, column
    /// 0 the furthest left -- the body at its centre; CROP_CHANNELS bytes a cell, [row][col][channel].
    constexpr uint32_t CROP = 48;
    constexpr float CROP_CELL = 2.0f;
    constexpr uint32_t CROP_CHANNELS = 6;
    constexpr uint32_t CROP_BYTES = CROP * CROP * CROP_CHANNELS;

    enum CropChannel : uint32_t
    {
        CROP_CODE = 0,          // MapCode, the four cells' by priority (wall > door > hazard > floor > unknown)
        CROP_HEIGHT,            // the floor layer nearest the feet: 0 none, else 128 + (z - feet) / CROP_HEIGHT_STEP
        CROP_VISITED,           // 1 any of the four was stood on
        CROP_AGE,               // the newest look: CROP_AGE_NEVER never, else round(CROP_AGE_SCALE x log2(1 + s))
        CROP_CLASS,             // the most recent entity's class (Class), 0 none
        CROP_FRONTIER,          // 1 any of the four is known floor beside a cell never seen
    };

    /// What a crop cell is (CROP_CODE): MAP_CODES values.
    enum class MapCode : uint8_t
    {
        Unknown = 0,
        Floor = 1,
        Wall = 2,
        Door = 3,
        Hazard = 4,
        Count
    };
    constexpr uint32_t MAP_CODES = uint32_t(MapCode::Count);
    constexpr char const* MAP_CODE_NAMES[MAP_CODES] = { "unknown", "floor", "wall", "door", "hazard" };

    constexpr float CROP_HEIGHT_STEP = 0.25f;
    constexpr uint8_t CROP_HEIGHT_ZERO = 128;
    constexpr uint8_t CROP_AGE_NEVER = 255;
    constexpr float CROP_AGE_SCALE = 20.0f;
    constexpr char const* CROP_CHANNEL_NAMES[CROP_CHANNELS] = { "code", "height", "visited", "age", "class",
        "frontier" };

    [[nodiscard]] uint8_t AgeByte(float seconds);

    /// A crop cell's bytes read back as the learner reads them: the code, the floor's height over the feet (yards;
    /// HasHeight false for none), visited, the newest look's age (seconds, to the byte's rounding; Seen false for
    /// never), the entity's class and the frontier.
    struct CropCell
    {
        MapCode Code = MapCode::Unknown;
        bool HasHeight = false;
        float Height = 0.0f;
        bool Visited = false;
        bool Seen = false;
        float Age = 0.0f;
        uint8_t Entity = 0;
        bool Frontier = false;
    };
    [[nodiscard]] CropCell DecodeCropCell(uint8_t const* bytes);

    class MentalMap
    {
    public:
        MentalMap() = default;
        MentalMap(MentalMap const&) = delete;
        MentalMap& operator=(MentalMap const&) = delete;
        MentalMap(MentalMap&&) = default;
        MentalMap& operator=(MentalMap&&) = default;

        void Configure(MapSettings const& settings) { _settings = settings; }
        [[nodiscard]] MapSettings const& Settings() const { return _settings; }

        /// Forget everything; the clock starts again.
        void Clear();
        /// Move the map's clock on: every look ages by it.
        void Advance(float seconds);
        [[nodiscard]] double Clock() const { return _clock; }
        /// The clock as a cell's Seen stamps it (0 is "never").
        [[nodiscard]] uint16_t Stamp() const;

        /// Write a frame's rays (amendment 2): `feetZ` and `bodyHeight` are the seat's, for the band "seen free" and a
        /// wall are judged in.
        void WriteFrame(FrameHits const& hits, float feetZ, float bodyHeight, MapWriteStats* stats = nullptr);
        /// Write the entities the sensor listed (entity-sensing): the image no longer draws units or objects, so a
        /// frame's rays leave no entity class on the map. Each listed entity marks the cell at its middle with its
        /// class (WriteEntity), a door (open or closed) with MAP_DOOR as well. Called after WriteFrame, the same
        /// decision.
        void WriteEntities(SeenList const& seen, MapWriteStats* stats = nullptr);
        /// Write where the body stands: visited, seen, and on the ground its floor; the cells on the line from where
        /// it last stood are visited too (a fast body crosses more than one a decision).
        void WriteBody(float x, float y, float z, bool grounded);

        /// The cell at world (x, y), or null when its tile is not kept (a coarse cell is not one).
        [[nodiscard]] MapCell const* Find(float x, float y) const;
        [[nodiscard]] MapCell const* FindCell(int32_t cx, int32_t cy) const;

        /// The crop round (x, y) at the feet's height z, heading-up for yaw (WoW's, counter-clockwise from +x), into
        /// `out` (CROP_BYTES).
        void Crop(float x, float y, float z, float yaw, uint8_t* out) const;

        [[nodiscard]] std::size_t Tiles() const { return _tiles.size(); }

    private:
        struct Tile
        {
            std::array<MapCell, MAP_TILE_CELLS> Cells{};
            uint64_t Touched = 0;
        };

        [[nodiscard]] static int64_t TileKey(int32_t tx, int32_t ty)
        {
            return (int64_t(tx) << 32) ^ int64_t(uint32_t(ty));
        }

        /// The cell, its tile made when missing (and the cap kept).
        MapCell& Touch(int32_t cx, int32_t cy);
        void Evict();
        void Fold(int64_t key, Tile const& tile);
        [[nodiscard]] MapCell const* FindCoarse(int32_t cx, int32_t cy) const;

        void MarkSeen(MapCell& cell, uint16_t stamp);
        void WriteFloor(int32_t cx, int32_t cy, float z, uint16_t stamp, MapWriteStats* stats);
        void WriteEntity(MapCell& cell, Class what, uint16_t stamp);

        MapSettings _settings;
        std::unordered_map<int64_t, std::unique_ptr<Tile>> _tiles;
        std::unordered_map<int64_t, std::unique_ptr<Tile>> _coarse;
        double _clock = 0.0;
        uint64_t _touch = 0;
        // The last tile Touch found: a frame writes runs of cells in one tile.
        int64_t _lastKey = 0;
        Tile* _lastTile = nullptr;
        bool _hasBody = false;
        float _bodyX = 0.0f;
        float _bodyY = 0.0f;
    };

    /// A cell's code for a body whose feet are at `feetZ` (MapCode): the wall of the floor layer nearest the feet,
    /// else a door, water or a hazard, else a floor, else unknown.
    [[nodiscard]] MapCode CodeOf(MapCell const& cell, float feetZ);
    /// The floor layer of `cell` nearest `feetZ`, or false when it has none.
    [[nodiscard]] bool NearestFloor(MapCell const& cell, float feetZ, float& z);
    /// Whether a cell was ever seen (any look at all: a hit, a pass in the band, the body).
    [[nodiscard]] inline bool Known(MapCell const& cell) { return cell.Seen != 0; }
}

#endif
