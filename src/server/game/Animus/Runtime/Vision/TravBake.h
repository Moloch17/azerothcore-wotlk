/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_RUNTIME_VISION_TRAV_BAKE_H
#define ANIMUS_RUNTIME_VISION_TRAV_BAKE_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Map;

namespace Animus::Vision
{
    /// **The traversability bake** (vision movement curriculum, decision 0036; CONTRACT.md sec 1): a 2-yd grid over
    /// an arena box classified by the player controller's own rules -- what a player's body can walk, step up, jump,
    /// drop and swim -- with 8-neighbour edge costs in yards. Training labels and the route cost-to-go the learner's
    /// potential and planner read; never an input to the policy and never from the navmesh. Baked at an arena's first
    /// episode and cached to disk beside the camera's scenes (`trav/`).
    enum class TravClass : uint8_t
    {
        Unknown = 0,    // outside the bake, or no ground found
        Free = 1,       // ground a body walks
        Wall = 2,       // no passable edge into it (a blocked line of sight from every neighbour)
        Water = 3,      // liquid deeper than 1.5 yd: swum
        Hazard = 4,     // magma or slime
        StepUp = 5,     // reached from a neighbour by a step (within STEP_UP)
        Jump = 6,       // reached only by a jump (STEP_UP < rise <= JUMP_RISE)
        DropSafe = 7,   // a drop under MIN_FALL_DMG_DIST starts here
        DropHurt = 8,   // a damaging drop starts here
        DropLethal = 9, // a lethal drop (damageperc >= 1) starts here: no edge
        Slope = 10,     // too steep to walk (normal z under WALKABLE_NORMAL_Z)
        Count
    };
    constexpr uint32_t TRAV_CLASSES = uint32_t(TravClass::Count);
    constexpr char const* TRAV_CLASS_NAMES[TRAV_CLASSES] = { "unknown", "free", "wall", "water", "hazard", "step_up",
        "jump", "drop_safe", "drop_hurt", "drop_lethal", "slope" };

    constexpr float TRAV_CELL = 2.0f;
    constexpr float TRAV_UNREACHABLE = 65504.0f;   // the largest f16: no route
    /// Player::HandleFall: the first damaging drop, the damage line (share of maximum health per yard and its
    /// intercept) and the drop that kills from full health.
    constexpr float FALL_DAMAGE_FROM = 13.48f;
    constexpr float FALL_DAMAGE_SLOPE = 0.018f;
    constexpr float FALL_DAMAGE_INTERCEPT = -0.2426f;
    constexpr float FALL_LETHAL_FROM = (1.0f - FALL_DAMAGE_INTERCEPT) / FALL_DAMAGE_SLOPE;    // 69.03 yd
    /// A standing jump's rise (PlayerController JUMP_SPEED 7.9555 at gravity 19.29: 1.64 yd).
    constexpr float JUMP_RISE = 1.64f;
    /// The highest rise walked without a jump and the steepest walkable floor (the controller's STEP_UP and
    /// WALKABLE_NORMAL_Z, repeated here so the bake's rules read in one place).
    constexpr float TRAV_STEP_UP = 1.1917536f;
    constexpr float TRAV_WALKABLE_NORMAL_Z = 0.6427876f;
    /// Liquid deeper than this over its bed is swum (Water); shallower is waded (Free).
    constexpr float TRAV_WATER_DEPTH = 1.5f;
    /// What swimming and jumping cost over walking the same yards (swim speed 4.7 against run speed 7).
    constexpr float TRAV_SWIM_FACTOR = 1.5f;
    constexpr float TRAV_JUMP_FACTOR = 1.5f;
    /// A walk edge that climbs more than this is a step (the TO cell reads StepUp): a ramp's cells climb less over
    /// two yards, stairs more.
    constexpr float TRAV_STEP_FROM = 0.6f;
    /// The bake's file format (the cache is rebaked when it changes).
    constexpr uint32_t TRAV_VERSION = 1;
    /// Most cells a side the bake allows (a 2048-yd box): a dungeon is a few hundred.
    constexpr int32_t TRAV_MAX_SIDE = 1024;
    /// Cells a side of a label grid (the map crop's CROP), and the coarse tier's cell.
    constexpr uint32_t TRAV_LABEL_SIDE = 48;
    constexpr float TRAV_LABEL_COARSE_CELL = 8.0f;
    /// Bytes per agent of the STEP's labels (CONTRACT.md sec 3.1): two 48 x 48 class grids, two 48 x 48 f16
    /// cost-to-go grids, TRAV_LABEL_SCALAR_COUNT f32 scalars, at these offsets. The scalars (protocol 33): the bake's
    /// route cost-to-go at the body and at the start, the route cost-to-go on the seat's OWN map (MapPlanner over the
    /// crops the policy saw) at the body and at the body's last position -- both on the map as it is now
    /// (PointNavEncounter::WriteLabels) -- then the goal's forward and right yards in the body frame.
    constexpr uint32_t TRAV_LABEL_CELLS = TRAV_LABEL_SIDE * TRAV_LABEL_SIDE;
    constexpr uint32_t TRAV_LABEL_FINE_CLASSES = 0;
    constexpr uint32_t TRAV_LABEL_COARSE_CLASSES = TRAV_LABEL_CELLS;
    constexpr uint32_t TRAV_LABEL_FINE_COST = 2 * TRAV_LABEL_CELLS;
    constexpr uint32_t TRAV_LABEL_COARSE_COST = 4 * TRAV_LABEL_CELLS;
    constexpr uint32_t TRAV_LABEL_SCALARS = 6 * TRAV_LABEL_CELLS;
    constexpr uint32_t TRAV_LABEL_SCALAR_COUNT = 6;
    constexpr uint32_t TRAV_LABEL_C_BODY = 0;
    constexpr uint32_t TRAV_LABEL_C_START = 1;
    constexpr uint32_t TRAV_LABEL_MAP_C_BODY = 2;
    constexpr uint32_t TRAV_LABEL_MAP_C_LAST = 3;
    constexpr uint32_t TRAV_LABEL_GOAL_FORWARD = 4;
    constexpr uint32_t TRAV_LABEL_GOAL_RIGHT = 5;
    constexpr uint32_t TRAV_LABEL_BYTES = TRAV_LABEL_SCALARS + TRAV_LABEL_SCALAR_COUNT * 4;
    static_assert(TRAV_LABEL_BYTES == 13848, "the labels' layout is the contract's");

    /// A float as IEEE half (round to nearest even; the costs are 0..65504, so no overflow is met, but the
    /// conversion is whole for the scalars' sake). No helper existed in the tree.
    [[nodiscard]] inline uint16_t FloatToHalf(float value)
    {
        uint32_t bits;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        uint32_t const sign = (bits >> 16) & 0x8000u;
        int32_t const exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;
        if (((bits >> 23) & 0xFFu) == 0xFFu)                        // inf or nan
            return uint16_t(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
        if (exponent >= 0x1F)                                       // too large: inf
            return uint16_t(sign | 0x7C00u);
        if (exponent <= 0)
        {
            if (exponent < -10)                                     // too small: zero
                return uint16_t(sign);
            mantissa |= 0x800000u;                                  // a subnormal half
            uint32_t const shift = uint32_t(14 - exponent);
            uint32_t half = mantissa >> shift;
            uint32_t const rest = mantissa & ((1u << shift) - 1);
            uint32_t const halfway = 1u << (shift - 1);
            if (rest > halfway || (rest == halfway && (half & 1u)))
                ++half;
            return uint16_t(sign | half);
        }
        uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
        uint32_t const rest = mantissa & 0x1FFFu;
        if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
            ++half;                                                 // may carry into the exponent: still right
        return uint16_t(sign | half);
    }

    /// The box a bake covers, yards (world x and y).
    struct TravBox
    {
        float MinX = 0.0f;
        float MinY = 0.0f;
        float MaxX = 0.0f;
        float MaxY = 0.0f;
    };

    /// An edge's kind, from a cell to one of its eight neighbours (TravGrid::Edges). The cost is read off the kind
    /// at Dijkstra (TravEdgeCost), so the bake does not depend on the tuning that prices a hurting drop.
    enum class TravEdge : uint8_t
    {
        None = 0,
        Walk,           // dist
        Swim,           // dist x TRAV_SWIM_FACTOR (into, through or out of water without a climb)
        Jump,           // dist x TRAV_JUMP_FACTOR
        DropSafe,       // dist
        DropHurt,       // dist + damageperc x DropHurtYards
    };

    /// The eight neighbours' offsets (dx, dy in cells), in TravGrid::Edges order; TRAV_OPPOSITE[d] is the way back.
    constexpr int32_t TRAV_DIRS = 8;
    constexpr int32_t TRAV_DX[TRAV_DIRS] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    constexpr int32_t TRAV_DY[TRAV_DIRS] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    constexpr int32_t TRAV_OPPOSITE[TRAV_DIRS] = { 1, 0, 3, 2, 7, 6, 5, 4 };
    constexpr uint32_t TRAV_NO_CELL = 0xFFFFFFFFu;
    /// Ground[] and Liquid[] of a cell with no floor or no liquid.
    constexpr float TRAV_NO_HEIGHT = -200000.0f;

    /// **One arena's bake**: the grid, its cells' classes and heights, and the edges. Immutable once baked or loaded
    /// and shared by every env and thread (TravRegistry keeps it).
    struct TravGrid
    {
        uint32_t MapId = 0;
        std::string Name;               // "<mapId>-<arena>"
        TravBox Box;
        int32_t Columns = 0;
        int32_t Rows = 0;
        uint64_t Digest = 0;
        uint32_t Seed = TRAV_NO_CELL;   // the cell the flood began at
        std::vector<uint8_t> Classes;   // TravClass per cell
        std::vector<float> Ground;      // the floor's height, TRAV_NO_HEIGHT for none
        std::vector<float> Liquid;      // the liquid's level, TRAV_NO_HEIGHT for none
        std::vector<uint8_t> Edges;     // TravEdge, cells x TRAV_DIRS
        std::vector<float> EdgeHurt;    // DropHurt's damageperc (share of max health), cells x TRAV_DIRS
        /// The cells a pair may be drawn from: Free or StepUp, entered by the flood from the seed.
        std::vector<uint32_t> Reachable;

        [[nodiscard]] uint32_t Cells() const { return uint32_t(Columns) * uint32_t(Rows); }
        /// The cell holding world (x, y), or TRAV_NO_CELL outside the box.
        [[nodiscard]] uint32_t CellOf(float x, float y) const
        {
            float const col = std::floor((x - Box.MinX) / TRAV_CELL);
            float const row = std::floor((y - Box.MinY) / TRAV_CELL);
            if (col < 0.0f || row < 0.0f || col >= float(Columns) || row >= float(Rows))
                return TRAV_NO_CELL;
            return uint32_t(row) * uint32_t(Columns) + uint32_t(col);
        }
        [[nodiscard]] float CentreX(uint32_t cell) const
        {
            return Box.MinX + (float(cell % uint32_t(Columns)) + 0.5f) * TRAV_CELL;
        }
        [[nodiscard]] float CentreY(uint32_t cell) const
        {
            return Box.MinY + (float(cell / uint32_t(Columns)) + 0.5f) * TRAV_CELL;
        }
        [[nodiscard]] TravClass ClassOf(uint32_t cell) const
        {
            return cell < Classes.size() ? TravClass(Classes[cell]) : TravClass::Unknown;
        }
        /// Where a body stands on the cell: the liquid's surface of a water cell, else the floor.
        [[nodiscard]] float StandingZ(uint32_t cell) const
        {
            return ClassOf(cell) == TravClass::Water ? Liquid[cell] : Ground[cell];
        }
        /// The neighbour of `cell` in direction `dir`, or TRAV_NO_CELL off the grid.
        [[nodiscard]] uint32_t Neighbour(uint32_t cell, int32_t dir) const
        {
            int32_t const col = int32_t(cell % uint32_t(Columns)) + TRAV_DX[dir];
            int32_t const row = int32_t(cell / uint32_t(Columns)) + TRAV_DY[dir];
            if (col < 0 || row < 0 || col >= Columns || row >= Rows)
                return TRAV_NO_CELL;
            return uint32_t(row) * uint32_t(Columns) + uint32_t(col);
        }
        /// The yards an edge's step covers: 2 straight, 2.83 diagonal.
        [[nodiscard]] static float StepYards(int32_t dir) { return dir < 4 ? TRAV_CELL : TRAV_CELL * 1.4142136f; }
    };

    /// A bake's log line: cells, and how many of each class.
    struct TravBakeReport
    {
        bool Baked = false;             // false: loaded from the cache
        double Seconds = 0.0;
        std::string Path;
        uint32_t Histogram[TRAV_CLASSES] = {};
    };

    /// What a bake is asked for: the map and phase to query, the box (HasBox false: the scene's solid bounds), and
    /// the seed the flood starts from (HasSeed false: the box's centre, from the highest floor there).
    struct TravBakeRequest
    {
        Map* World = nullptr;
        uint32_t PhaseMask = 1;
        std::string Arena;
        bool HasBox = false;
        TravBox Box;
        bool HasSeed = false;
        float SeedX = 0.0f;
        float SeedY = 0.0f;
        float SeedZ = 0.0f;
    };

    /// **The loaded bakes**: one TravGrid per (map, arena), baked at the arena's first episode on the worldserver
    /// (the world thread: Map::GetHeight loads an instance's grids) and cached to `<travDir>/<mapId>-<arena>.trav`
    /// (TRAV_VERSION in the header, an FNV digest of the scene's checksum, the box, the seed and the version: a
    /// mismatch rebakes). A grid is immutable once in the registry and never freed while the server runs, so the
    /// pointer may be kept and read from any thread without a lock.
    class TravRegistry
    {
    public:
        [[nodiscard]] static TravRegistry& Instance();

        /// The grid of `request`'s arena, loading or baking it on the first ask. Null, with `error`, when the map
        /// has no scene or the box is empty.
        [[nodiscard]] TravGrid const* Ensure(TravBakeRequest const& request, std::string const& travDir,
            TravBakeReport& report, std::string& error);
        /// A grid already ensured, or null.
        [[nodiscard]] TravGrid const* Get(uint32_t mapId, std::string const& arena) const;

    private:
        mutable std::mutex _lock;
        std::map<std::string, std::unique_ptr<TravGrid>> _grids;
    };

    /// The cost of an edge of `kind` over `yards`, a hurting drop's `damageperc` priced at `dropHurtYards` per
    /// whole health bar.
    [[nodiscard]] inline float TravEdgeCost(TravEdge kind, float yards, float damageperc, float dropHurtYards)
    {
        switch (kind)
        {
            case TravEdge::Walk:
            case TravEdge::DropSafe:
                return yards;
            case TravEdge::Swim:
                return yards * TRAV_SWIM_FACTOR;
            case TravEdge::Jump:
                return yards * TRAV_JUMP_FACTOR;
            case TravEdge::DropHurt:
                return yards + damageperc * dropHurtYards;
            default:
                return TRAV_UNREACHABLE;
        }
    }

    /// **The route cost-to-go** from every cell to `goal`, yards (TRAV_UNREACHABLE where no route reaches it; edges
    /// are directed, so this is Dijkstra over the reversed edges from the goal). `out` is sized to the grid.
    void CostToGo(TravGrid const& grid, uint32_t goal, float dropHurtYards, std::vector<float>& out);

    /// **A label grid** (CONTRACT.md sec 3.1): TRAV_LABEL_SIDE x TRAV_LABEL_SIDE cells of `cell` yards in the map
    /// crop's frame -- heading-up for `yaw`, row 0 the furthest ahead, column 0 the furthest left, the body at the
    /// centre: MentalMap::Crop's geometry -- each the bake's class and cost-to-go at the cell, a bake cell with no
    /// ground (rock or a void inside the box) labelled Wall. A cell wider than the bake's (the coarse tier) covers
    /// several bake cells: the majority class of those that are not Unknown, Wall only when none of them is passable
    /// (Unknown when the cell is outside the box), and the least cost. `classes` gets TRAV_LABEL_CELLS bytes, `costs` as many halves. An empty
    /// `cost` (no goal) writes TRAV_UNREACHABLE everywhere.
    void SampleLabels(TravGrid const& grid, std::vector<float> const& cost, float x, float y, float yaw, float cell,
        uint8_t* classes, uint16_t* costs);
}

#endif
