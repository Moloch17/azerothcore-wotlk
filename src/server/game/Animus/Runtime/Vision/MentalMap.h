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
/// **Writes** (WriteFrame, from the frame as cast, before it is scaled up): every ray's hit marks its cell -- a floor
/// (normal z at least FLOOR_NORMAL, never a RayHit::Backside) at its height layer, else a wall within the body's band
/// of a floor, a door, water or a hazard, or a unit's or game object's class -- within WRITE_REACH in the 1-yd cells,
/// beyond it (at any distance; sky never) in the far tier's COARSE_CELL-yd cells (_far, the map-the-distance change,
/// 2026-10-15), so the coarse crop fills with the hall a seat looks down. The cells the ray crossed on its way (within
/// WRITE_REACH) are "seen free" by a 2D grid step over the map's own cells, but only where the ray's segment over the
/// cell lies in the body's band above that cell's floor (with no floor known, within FREE_BAND yards of the feet), or
/// -- a ray that ends on a floor -- in the body's band above THAT floor within HIT_BAND_REACH yards of it (a deck or a
/// terrace seen from above or below). A pass-through never marks a floor. Then the frame is **meshed** (depth-image
/// meshing): each quad of neighbouring rays is two triangles, and a triangle whose three hits are one surface -- the
/// same kind (floor, water/hazard), its plane facing the camera, as level as its hits' own normals say, with no range
/// jump between its hits and no more rise across it than those normals allow plus a step -- fills the cells under it at
/// its interpolated height, a pair of neighbouring wall hits at a near depth the cells between them; never across a
/// depth discontinuity (an edge, a doorway, a drop), and at most MESH_TRIANGLE_CELLS cells a triangle and
/// MESH_FRAME_CELLS a frame. WriteBody marks the cells the body stood on visited (and their floor, from the feet).
///
/// **Walls are bound to a floor layer** (MAP_WALL_LOW to Floor[0]'s band, MAP_WALL_HIGH to Floor[1]'s) and move with
/// it: a layer's wall goes when the layer is replaced by another storey, and an observation of the layer clears it --
/// a 1-yd floor hit on it, the body standing on it, or a ray seen free through its cell low in its band (within
/// FREE_CLEAR_RISE of the floor, crossing FREE_CLEAR_SPAN of the cell) -- unless a wall was written on the cell or a
/// neighbour in the same frame (MAP_WALL_FRAME; a frame's clears wait for its end: a cell at a wall's foot holds floor
/// and wall both, and a wall seen at a grazing angle is hit in some of its cells and passed in front of in the
/// others). A meshed floor clears nothing (it is interpolated), nor does a far cell. A wall whose band has no layer in
/// a cell that has floors (a ground wall under a deck the cell holds) is not written.
///
/// **Shared memory** (SharedMemory.h): a map's tiles can be borrowed read-only from a shared memory (copy-on-write: a
/// borrowed tile is cloned on its first write), and its clock anchored to the shared memory's, so a memory 20
/// minutes old reads as 20 minutes old; the shared memory merges the tiles a map wrote back into itself (MergeCell).
///
/// **Reads** (Crop): the MapBlock's one egocentric, heading-up crop (the Change, 2026-10-06): CROP x CROP cells of
/// CROP_CELL yards round the body, each summarising its four 1-yd cells, eight bytes a cell (CropChannel); and, for
/// the nav stages (map block revision 5, CONTRACT.md sec 3.2), the same crop at COARSE_CELL yards a cell -- 384 yd
/// across, each cell summarising its 64 1-yd cells -- the planner's far tier. A crop cell reads WALL only when most of
/// its 1-yd cells are walls (CROP_WALL_MIN of the four; more than half of the 64): with any wall sub-cell a wall (map
/// block revisions 1-5) a 4-yd corridor's two wall-footed cells sealed it at 2 yd, and the route planned over the
/// shared memory's crop read 2-3 x the true route (memv4, 2026-10-17). The wall count itself is the CROP_WALLS channel
/// (revision 6). The route itself is planned on the 1-yd cells (MapPlanner.h).
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
    /// Rays write their hits into the 1-yd cells, and walk their cells "seen free", this far from the camera at most
    /// (amendment 2); a hit further off is written into the far tier's COARSE_CELL-yd cells.
    constexpr float WRITE_REACH = 64.0f;
    /// A ray that ends on a floor marks the cells it crossed in the body's band above that floor "seen free" this far
    /// (horizontally, yards) before the hit.
    constexpr float HIT_BAND_REACH = 12.0f;
    /// Meshing: a triangle fills at most this many cells (more: written in the far tier's cells, else skipped), and a
    /// frame at most MESH_FRAME_CELLS cells in all.
    constexpr uint32_t MESH_TRIANGLE_CELLS = 256;
    constexpr uint32_t MESH_FRAME_CELLS = 24576;
    /// A meshed triangle's longest side, horizontally, yards: within WRITE_REACH, and beyond it.
    constexpr float MESH_SPAN_NEAR = 24.0f;
    constexpr float MESH_SPAN_FAR = 96.0f;
    /// A meshed floor's plane is as level as its hits' own normals, to this (a step's riser bridged over at a grazing
    /// angle tilts the plane; a slope's hits tilt with it).
    constexpr float MESH_NORMAL_SLACK = 0.1f;
    /// Two neighbouring wall hits are one wall when their ranges differ by at most this share of the nearer, plus
    /// MESH_WALL_SLACK yards.
    constexpr float MESH_WALL_JUMP = 0.1f;
    constexpr float MESH_WALL_SLACK = 0.5f;
    /// A floor or water triangle's hits are one surface when the furthest's range exceeds the nearest's by at most
    /// this share of the nearest, plus MESH_FLOOR_SLACK yards (looser than a wall's: neighbouring rows on a floor at a
    /// grazing angle differ by up to twice the range at the coarsest render size), and when its hits rise across it
    /// at most what their own normals allow over its span plus MESH_STEP_RISE yards (a ground hit joined to a raised
    /// one -- a ledge's top, a beam -- is no tilted floor).
    constexpr float MESH_FLOOR_JUMP = 1.0f;
    constexpr float MESH_FLOOR_SLACK = 1.0f;
    constexpr float MESH_STEP_RISE = 1.0f;
    /// A ray seen free through a cell clears the wall of the floor layer it passed in only when its segment over the
    /// cell came within this many yards of that floor (a ray at head height says nothing of a crate at the knees)...
    constexpr float FREE_CLEAR_RISE = 1.0f;
    /// ... and crossed at least this much of the cell, horizontally (a ray clipping a corner in front of a wall's
    /// face is no look at the cell).
    constexpr float FREE_CLEAR_SPAN = 0.5f;

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
        MAP_WALL_FRAME = 0x80,      // a wall was written here this frame (inside WriteFrame only; never kept)
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
    /// The 1-yd cell coordinate holding world coordinate `v`.
    [[nodiscard]] inline int32_t MapCellOf(float v) { return int32_t(std::floor(v / MAP_CELL)); }

    /// The oldest sighting the crop's class channel (CROP_CLASS) still shows, seconds: a unit seen here this long ago
    /// is not here for the policy's purposes (a patrol's loop is under a minute; the next bucket, 4 min, is "it was
    /// here once"). Older marks stay in the cell but paint no class -- a map kept across resets
    /// (MapRunSettings::KeepShare, its clock moved on by minutes) or borrowed from the shared memory would otherwise
    /// show every creature a seat had ever met as present (seed 216, 2026-10-16).
    constexpr float ENTITY_RECENT_SECONDS = 60.0f;

    /// An entity's age past its cell's last look, in eight buckets: under 1 s, 4 s, 15 s, 1 min, 4 min, 15 min, 1 h,
    /// and older; and the bucket's start.
    constexpr float ENTITY_AGE_BUCKETS[8] = { 0.0f, 1.0f, 4.0f, 15.0f, 60.0f, 240.0f, 900.0f, 3600.0f };
    [[nodiscard]] uint8_t EntityAgeBucket(float seconds);

    /// The map's caps (AnimusForge.Map.*): fine tiles, and coarse tiles an evicted fine tile folds into (0: none, a
    /// fine tile evicted is forgotten -- training's); and the far tier's tiles (COARSE_CELL-yd cells written from hits
    /// beyond WRITE_REACH, 256 yd a tile: 256 of them is four kilometres square).
    struct MapSettings
    {
        uint32_t MaxTiles = 4096;
        uint32_t CoarseTiles = 0;
        uint32_t FarTiles = 256;
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
    /// The coarse crop reads a far or folded cell's floor only within this many yards of the feet (2026-10-17): an
    /// 8-yd cell holds floors at any height a seat ever looked at from anywhere -- the sea under the Deadmines'
    /// cliffs, the hills over them, another bot's storey -- and painted as floor outside the dungeon's shell they
    /// were 47-74 % of a memory-on coarse crop, a straight line through the walls to the planner. A cell with no
    /// floor (a wall or a hazard alone) has no height to judge and is read as it is.
    constexpr float COARSE_READ_BAND = 6.0f;

    /// One ray of a frame as cast (before the frame is scaled up): from the camera along Dir (a unit vector) to its
    /// hit at Distance -- for sky, where the ray left the loaded grids -- the hit's height and normal z, and what it
    /// hit. Backside: the hit reads as a floor but is the top of the level's shell -- a model's triangle met from
    /// behind (SurfaceHit::BackFace) with no room over it (Render; Camera.h's ENCLOSURE_HEADROOM), a one-sided ceiling
    /// panel seen from over it; never a floor for the map.
    struct RayHit
    {
        Vec3 Dir;
        float Distance = 0.0f;
        float Z = 0.0f;
        float NormalZ = 0.0f;
        Class What = Class::Sky;
        bool Backside = false;
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
        uint32_t Hits = 0;          // rays whose hit marked a cell (a 1-yd cell, or a far one: FarHits)
        uint32_t Floors = 0;
        uint32_t Walls = 0;
        uint32_t Entities = 0;
        uint32_t Steps = 0;         // cells the grid step visited
        uint32_t Free = 0;          // ... marked seen free
        uint32_t FreeOnHit = 0;     // ... of them, free only by the band above the ray's own floor hit
        uint32_t FarHits = 0;       // hits beyond WRITE_REACH, written into the far tier's cells
        uint32_t Meshed = 0;        // 1-yd cells a meshed triangle or wall pair filled
        uint32_t MeshedFar = 0;     // far cells a meshed triangle or wall pair filled
        uint32_t MeshRejected = 0;  // triangles of one kind not filled: a discontinuity, too wide, or past the budget
        uint32_t WallsCleared = 0;  // layers' walls an observation cleared (a floor hit, a pass low in the band)
    };

    /// A tile of the map: MAP_TILE x MAP_TILE cells (1-yd in the fine tier, COARSE_CELL-yd in the coarse and far
    /// tiers), with the coverage counters (general search): cells ever seen (a Seen 0 -> stamp transition), cells the
    /// body stood on (MAP_VISITED first set), cells looked at this episode (Seen below the epoch -> at or above it)
    /// with the epoch the count belongs to; a count of another epoch reads as 0 and restarts.
    struct MapTile
    {
        std::array<MapCell, MAP_TILE_CELLS> Cells{};
        uint16_t KnownCells = 0;
        uint16_t VisitedCells = 0;
        uint16_t SearchedCells = 0;
        uint16_t SearchedEpoch = 0;
    };
    /// A tile as a shared memory holds it: immutable once made, read by any number of maps without a lock.
    using SharedTile = std::shared_ptr<MapTile const>;

    class SharedMemory;

    /// **The crop** (the Change): CROP x CROP cells of CROP_CELL yards, heading-up -- row 0 the furthest ahead, column
    /// 0 the furthest left -- the body at its centre; CROP_CHANNELS bytes a cell, [row][col][channel].
    constexpr uint32_t CROP = 48;
    constexpr float CROP_CELL = 2.0f;
    constexpr uint32_t CROP_CHANNELS = 8;
    constexpr uint32_t CROP_BYTES = CROP * CROP * CROP_CHANNELS;

    enum CropChannel : uint32_t
    {
        CROP_CODE = 0,          // MapCode: wall when CROP_WALL_MIN of the four cells are, else the rest by priority
                                // (door > hazard > floor > unknown)
        CROP_HEIGHT,            // the floor layer nearest the feet: 0 none, else 128 + (z - feet) / CROP_HEIGHT_STEP
        CROP_VISITED,           // 1 any of the four was stood on
        CROP_AGE,               // the newest look: CROP_AGE_NEVER never, else round(CROP_AGE_SCALE x log2(1 + s))
        CROP_CLASS,             // the most recent entity's class (Class) seen under ENTITY_RECENT_SECONDS ago, 0 none
        CROP_FRONTIER,          // 1 any of the four is known floor beside a cell never seen
        CROP_SEARCHED,          // 0..SEARCHED_MAX how many of the four were looked at THIS episode (Seen >= epoch)
        CROP_WALLS,             // 0..CROP_WALLS_MAX how many of the four are walls (the coarse crop's 64 scaled)
    };

    /// A fine crop cell is a wall when at least this many of its four 1-yd cells are (map block revision 6).
    constexpr uint32_t CROP_WALL_MIN = 3;
    /// The most CROP_WALLS reads: the four 1-yd cells of a fine crop cell; a coarse cell's 64 are scaled to it.
    constexpr uint32_t CROP_WALLS_MAX = 4;

    /// The most CROP_SEARCHED reads (a crop cell is four 1-yd cells).
    constexpr uint32_t SEARCHED_MAX = 4;
    /// The map block's journal scalar of seconds since the body last stood on ground it had never stood on, over this
    /// and clamped to 1 (the cells stood on are on a log scale since map block revision 4: MapBlock::TOTAL_LOG_MAX).
    constexpr float GROUND_AGE_SCALE_S = 120.0f;

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
        "frontier", "searched", "walls" };

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
        uint8_t Searched = 0;   // of the four 1-yd cells, how many were looked at this episode
        uint8_t Walls = 0;      // ... how many are walls (CROP_WALLS)
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
        /// Forget everything; the clock starts at `clock` (a shared memory's anchor: SharedMemory::Anchor).
        void ClearAt(double clock);
        /// Move the map's clock on: every look ages by it.
        void Advance(float seconds);
        [[nodiscard]] double Clock() const { return _clock; }
        /// The clock as a cell's Seen stamps it (0 is "never").
        [[nodiscard]] uint16_t Stamp() const;

        /// Start an episode on this map (kept or cleared): every look stamped before now is an earlier episode's, so
        /// CROP_SEARCHED counts only the looks from here on, and the ground counters restart. Call after Advance() /
        /// Clear(); a kept map must have advanced by at least a second so no earlier stamp equals the new epoch.
        void BeginEpisode();
        /// The 1-yd cells the body first stood on this episode (MAP_VISITED not yet set; a kept map's earlier ground
        /// is not new ground).
        [[nodiscard]] uint32_t GroundTotal() const { return _groundTotal; }
        /// The 1-yd cells looked at this episode (a look stamped at or after the epoch, counted at its first such
        /// stamp), over the whole map: the map block's searched_cells scalar (general search, decision 0027).
        [[nodiscard]] uint32_t SearchedCells() const { return _searchedTotal; }
        /// The per-tile counters the coverage block samples (CoverageBlock): the cells of tile (tx, ty) ever seen, the
        /// cells the body stood on, and the cells looked at this episode (0 when the tile's count is an earlier
        /// episode's). Zeros for a tile not kept (a coarse tile has no counters).
        void TileCounts(int32_t tx, int32_t ty, uint16_t& known, uint16_t& visited, uint16_t& searched) const;
        /// Seconds since the body last stood on such a cell.
        [[nodiscard]] double SecondsSinceGround() const { return _clock - _lastGroundClock; }

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
        /// The kept 1-yd tile (tx, ty)'s MAP_TILE_CELLS cells, row by row (y first), or null when it is not kept (a
        /// folded coarse tile is not one). Valid until the map is next written (a borrowed tile is cloned on its first
        /// write): the global route's read (GlobalPlanner.h), which fetches it afresh every decision.
        [[nodiscard]] MapCell const* TileCells(int32_t tx, int32_t ty) const;
        /// The tile coordinates the kept 1-yd tiles span, inclusive; false when none is kept.
        bool TileBounds(int32_t& tx0, int32_t& ty0, int32_t& tx1, int32_t& ty1) const;
        /// The 1-yd cells first seen since the map was last cleared (a cell's Seen going 0 -> a stamp; a borrowed
        /// tile's cells were seen before and count nothing): the global route's replan trigger (GlobalRoute).
        [[nodiscard]] uint64_t KnownTotal() const { return _knownTotal; }
        /// A window of `side` x `side` 1-yd cells from cell (x0, y0), row by row (y first), into `out`: each the kept
        /// tile's cell, else the folded coarse cell standing in for it (a realm's; the far tier never), else a blank
        /// cell. One copy of the tiles' rows rather than a tile lookup a sample (the crop's and the planner's read).
        void Window(int32_t x0, int32_t y0, int32_t side, MapCell* out) const;

        /// The crop round (x, y) at the feet's height z, heading-up for yaw (WoW's, counter-clockwise from +x), into
        /// `out` (CROP_BYTES). `cell` is a crop cell's side: CROP_CELL (each cell a wall when CROP_WALL_MIN of its
        /// four 1-yd cells are, else the others by priority), or wider (COARSE_CELL: each cell a wall when more than
        /// half of its 1-yd cells are, else the plurality of the rest, the mean floor height, any visited,
        /// the newest look, any frontier, the cells looked at this episode over 16 scaled to SEARCHED_MAX, the most
        /// recent entity's class; the same bytes and the same frame -- the coarse and far tiers' 8-yd cells, each by
        /// its centre, filling what the 1-yd cells leave unknown, a cell with a floor only when that floor is within
        /// COARSE_READ_BAND of the feet). The 2-yd crop never reads the far tier.
        void Crop(float x, float y, float z, float yaw, uint8_t* out, float cell = CROP_CELL) const;

        [[nodiscard]] std::size_t Tiles() const { return _tiles.size(); }
        /// The far tier's tiles (COARSE_CELL-yd cells written from hits beyond WRITE_REACH).
        [[nodiscard]] std::size_t FarTiles() const { return _far.size(); }
        /// The far tier's cell at world (x, y), or null when its tile is not kept.
        [[nodiscard]] MapCell const* FindFar(float x, float y) const;
        /// Whether the clock is a shared memory's (SharedMemory::Adopt / Anchor): only such a map is merged back.
        [[nodiscard]] bool Anchored() const { return _anchored; }
        /// Whether the map started from a shared memory's tiles (SharedMemory::Adopt): a wall it lacks on a newer look
        /// is one it cleared, so its merge may clear the memory's (MergeCell).
        [[nodiscard]] bool Adopted() const { return _adopted; }

        [[nodiscard]] static int64_t TileKey(int32_t tx, int32_t ty)
        {
            return (int64_t(tx) << 32) ^ int64_t(uint32_t(ty));
        }

    private:
        friend class SharedMemory;
        using Tile = MapTile;

        /// A kept tile: the map's own (Own), or borrowed from a shared memory (Shared, read-only: the first write
        /// clones it into Own and lets the borrowed one go), and when it was last touched (the cap's order).
        struct Slot
        {
            SharedTile Shared;
            std::unique_ptr<Tile> Own;
            uint64_t Touched = 0;

            [[nodiscard]] Tile const* Read() const { return Own ? Own.get() : Shared.get(); }
        };
        using Slots = std::unordered_map<int64_t, Slot>;

        /// The cell, its tile made when missing (and the cap kept), or cloned when borrowed.
        MapCell& Touch(int32_t cx, int32_t cy);
        /// The far tier's cell (COARSE_CELL-yd cell coordinates), its tile made or cloned likewise.
        MapCell& TouchFar(int32_t ccx, int32_t ccy);
        [[nodiscard]] MapCell const* FindFarCell(int32_t ccx, int32_t ccy) const;
        void Evict();
        void Fold(int64_t key, Tile const& tile);
        [[nodiscard]] MapCell const* FindCoarse(int32_t cx, int32_t cy) const;
        /// The cell (cx, cy) of `slots`' tiles, in their own cell coordinates, or null.
        [[nodiscard]] static MapCell const* FindIn(Slots const& slots, int32_t cx, int32_t cy);
        /// Crop at a cell wider than CROP_CELL: the kept tiles' cells scattered into the crop's cells (a window copy
        /// of 547 x 547 cells for 8-yd cells would cost more than the crop itself).
        void CropWide(float x, float y, float z, float yaw, float cell, uint8_t* out) const;

        /// Stamp a look on `cell`, which Touch returned last (its tile is _lastTile, whose counters move).
        void MarkSeen(MapCell& cell, uint16_t stamp);
        void NoteGround(MapCell const& cell);
        void WriteFloor(int32_t cx, int32_t cy, float z, uint16_t stamp, MapWriteStats* stats);
        void WriteEntity(MapCell& cell, Class what, uint16_t stamp);
        /// A wall hit at height `z` on 1-yd cell (cx, cy), when it is in the body's band of a floor (the cell's own,
        /// else a neighbour's, else the feet's); true when it was written.
        bool WriteWall(int32_t cx, int32_t cy, float z, float feetZ, float bodyHeight, uint16_t stamp);
        /// The 1-yd cell (cx, cy) when its tile is the map's own (not borrowed), else null.
        [[nodiscard]] MapCell* OwnCell(int32_t cx, int32_t cy);
        /// An observation of `cell`'s floor layer `layer` (-1: the cell has no floor known) with nothing in the way:
        /// its wall, if it has one, is cleared at the frame's end (SettleFrame).
        void NoteClear(int32_t cx, int32_t cy, MapCell const& cell, int32_t layer);
        /// The frame's end: the noted clears applied, except on or beside a cell with a wall written this frame; the
        /// MAP_WALL_FRAME marks removed; the frame's hazard hits written where no floor stands above them.
        void SettleFrame(MapWriteStats* stats);
        /// A water, magma or slime hit at height `z` on a 1-yd cell (far: a far cell): written at the frame's end
        /// (SettleFrame) unless the cell's floor nearest the hit stands above it -- a WMO's liquid plane runs on
        /// under the shore's dry edge and under a dock, and the caster takes the plane wherever a solid is not nearer
        /// (VisionCaster.cpp's model-liquid step has no ground-below test; the terrain's has). The frame's own floor
        /// hits count, whatever their order among its rays. At the memv4 poses (2026-10-17) this was 3 % of the
        /// Deadmines' hazard cells and none of Shadowfang's: most of the "false" hazards there are the waterline the
        /// bake calls Free (it has no Water class in either map).
        void NoteHazard(int32_t cx, int32_t cy, float z, bool far);
        /// A hit beyond WRITE_REACH into the far tier: floor at its layer, a wall in the band of the far cell's floor
        /// (else the feet's), water or a hazard.
        void WriteFar(Class what, float x, float y, float z, float normalZ, float feetZ, float bodyHeight,
            uint16_t stamp, MapWriteStats* stats);
        /// The frame's depth-image meshing (the map-the-distance change, part A).
        void Mesh(FrameHits const& hits, float feetZ, float bodyHeight, uint16_t stamp, MapWriteStats* stats);

        MapSettings _settings;
        Slots _tiles;
        Slots _coarse;
        Slots _far;
        double _clock = 0.0;
        bool _anchored = false;         // the clock is a shared memory's (SharedMemory::Adopt / Anchor)
        bool _adopted = false;          // ... and the tiles were its (SharedMemory::Adopt)
        /// ... and the shared memory's second the clock's 0 is (the memory's stamp base when anchored): a stamp of
        /// this map is the shared memory's stamp plus (its base now - _origin) (SharedMemory::Absorb).
        double _origin = 0.0;
        uint16_t _epoch = 1;            // the first Stamp() of this episode
        uint32_t _groundTotal = 0;
        uint32_t _searchedTotal = 0;
        uint64_t _knownTotal = 0;
        double _lastGroundClock = 0.0;
        uint64_t _touch = 0;
        // The last tile Touch found: a frame writes runs of cells in one tile; and TouchFar's.
        int64_t _lastKey = 0;
        Slot* _lastSlot = nullptr;
        Tile* _lastTile = nullptr;
        int64_t _lastFarKey = 0;
        Slot* _lastFarSlot = nullptr;
        bool _hasBody = false;
        float _bodyX = 0.0f;
        float _bodyY = 0.0f;
        /// The 1-yd cells marked MAP_WALL_FRAME this frame (TileKey-packed cell coordinates), and the clears noted
        /// (the cell, and its layer's height: NaN for a cell with no floor known).
        std::vector<int64_t> _frameWalls;
        struct PendingClear
        {
            int32_t X;
            int32_t Y;
            float Z;
        };
        std::vector<PendingClear> _frameClears;
        /// The frame's hazard hits (NoteHazard): the cell, the hit's height, and whether the cell is a far one.
        struct PendingHazard
        {
            int32_t X;
            int32_t Y;
            float Z;
            bool Far;
        };
        std::vector<PendingHazard> _frameHazards;
    };

    /// A cell's code for a body whose feet are at `feetZ` (MapCode): the wall of the floor layer nearest the feet,
    /// else a door, water or a hazard, else a floor, else unknown.
    [[nodiscard]] MapCode CodeOf(MapCell const& cell, float feetZ);
    /// The floor layer of `cell` nearest `feetZ`, or false when it has none.
    [[nodiscard]] bool NearestFloor(MapCell const& cell, float feetZ, float& z);
    /// Whether a cell was ever seen (any look at all: a hit, a pass in the band, the body).
    [[nodiscard]] inline bool Known(MapCell const& cell) { return cell.Seen != 0; }
    /// How a merge treats a wall the newer source lacks on a layer both hold (MergeCell).
    enum class WallClears : uint8_t
    {
        Never,      // the target's wall stays: the far tier's cells, which no observation clears
        SeenFree,   // ... unless the source saw the cell free (MAP_FREE: a ray passed in the body's band): a map that
                    // started blank (SharedMemory::Anchor) knew nothing of the memory's walls, but a pass is a look
        Lacking,    // the wall goes: the source read the memory (MentalMap::Adopted), so a wall it lacks it cleared
    };

    /// `from` folded into a shared memory's cell `into`: its floors as layers, its flags, the newer look and entity.
    /// Both cells' stamps on one clock. A wall moves with its floor layer (the source's layer 0 is not the target's
    /// layer 0 once the layers are merged); a wall with no floor known goes to a target with at most one layer, else
    /// is dropped (its height is unknown). Walls by recency: on a layer both hold, a newer `from` sets its wall --
    /// and clears one it lacks as `clears` says (WallClears); an equal look adds, an older one leaves the target's be
    /// (and moves or evicts none of its layers); a layer new to the target takes `from`'s wall.
    void MergeCell(MapCell& into, MapCell const& from, WallClears clears);
}

#endif
