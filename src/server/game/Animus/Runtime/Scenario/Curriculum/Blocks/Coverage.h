/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
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

#ifndef ANIMUS_LIB_CURRICULUM_COVERAGE_H
#define ANIMUS_LIB_CURRICULUM_COVERAGE_H

#include "Define.h"
#include "MentalMap.h"
#include <array>
#include <cmath>
#include <cstdint>

/// **The coverage analysis of the seat's own map** (general search, decision 0027): what the mental-map crop the seat
/// already has (Vision::CROP x CROP cells of CROP_CELL yards, heading-up, MapBlock::Observe) says about where the
/// unseen ground is and how the seen ground is shaped. Pure over the crop bytes and the pose: computed once a decision
/// in MapBlock::Observe, read by the map block's scalars and by the seek encounter's map-derived terms. Nothing here
/// reads the navmesh, the room table or the world: a seat on any map gets the same analysis of its own memory.
///
/// Cells: **Open** = floor or door, **Shut** = wall or hazard, **Unknown** = code 0. A FRONTIER cell is Open with an
/// Unknown four-neighbour (the crop's frontier channel at 1 yd is the same rule; recomputed here at 2 yd so it is one
/// rule for the whole analysis). A FRONTIER CLUSTER is an 8-connected set of ClusterMinCells or more frontier cells.
/// The CHAMBERS are the 4-connected parts, of PocketMinCells or more cells, of the Open cells eroded by one cell (a
/// cell survives when its eight neighbours are Open: a 2-yd erosion, which parts a 3-yd doorway as the room table's
/// authoring erosion did), each grown back by a cell; the one holding a cell under the body is **Own**. A chamber is a
/// POCKET when it is not Own and the body can reach it over Open cells (a BFS from the body's cell): a chamber behind
/// a narrowing, not behind unseen ground (that gap is a frontier cluster). The plan's "reachable through one opening"
/// is not computed: a pocket here is "a chamber behind a narrowing", which is what the room table encodes.
///
/// Keys are world points at KEY_YARDS (10 yd, SeenPlaces::SAME_PLACE), as the seek encounter's frontier keys, so the
/// egocentric frame farms nothing: a cluster or a chamber keeps its key as the seat turns.
namespace Animus::Curriculum::Coverage
{
    constexpr uint32 SIDE = Vision::CROP;
    constexpr uint32 CELLS = SIDE * SIDE;
    constexpr float CELL = Vision::CROP_CELL;
    constexpr uint32 MAX_CLUSTERS = 32;
    constexpr uint32 MAX_CHAMBERS = 16;
    constexpr float KEY_YARDS = 10.0f;
    /// How far the crop reaches from the body along an axis, yards (the scalar scale of a distance).
    constexpr float REACH = float(SIDE) * CELL * 0.5f;

    /// Summary::Cell bits.
    enum CellBit : uint8
    {
        CELL_KNOWN = 0x01,          // code != 0
        CELL_OPEN = 0x02,           // floor or door
        CELL_FRONTIER = 0x04,       // open with an unknown four-neighbour
        CELL_ERODED = 0x08,         // open with eight open neighbours (a chamber's core)
        CELL_REACHED = 0x10,        // reached from the body over open cells
    };

    /// A 10-yd world cell as one key: x and y in `cell`-yard squares, z in storeys of three yards (the seek
    /// encounter's CellKey, shared so the keys compare).
    [[nodiscard]] inline uint64 CellKey(float x, float y, float z, float cell)
    {
        uint64 const cx = uint64(uint32(int32(std::floor(x / cell)))) & 0x1FFFFF;
        uint64 const cy = uint64(uint32(int32(std::floor(y / cell)))) & 0x1FFFFF;
        uint64 const cz = uint64(uint32(int32(std::floor(z / 3.0f)))) & 0x3FFFFF;
        return cx << 43 | cy << 22 | cz;
    }

    struct Cluster
    {
        uint64 Key = 0;
        uint32 Size = 0;            // frontier cells
        float X = 0.0f;             // the centroid, world
        float Y = 0.0f;
        float Z = 0.0f;             // the mean floor height of its cells (the feet's where none is known)
        float Forward = 0.0f;       // the centroid in the body's frame, yards (+ ahead, + right)
        float Right = 0.0f;
        bool AtPocket = false;      // a cell of it is eight-adjacent to a pocket's cells
    };

    struct Chamber
    {
        uint64 Key = 0;
        uint32 Size = 0;            // cells, grown back
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        bool Own = false;           // holds a cell under the body
        bool Pocket = false;        // not own, reached over open cells
    };

    /// What one decision's crop says (SeatState::Coverage, written through SeatView::CoverageOut by MapBlock).
    struct Summary
    {
        bool Valid = false;
        float X = 0.0f;             // the pose the crop was taken at (the body)
        float Y = 0.0f;
        float Z = 0.0f;
        float Yaw = 0.0f;
        uint32 ClusterCount = 0;    // by size, the largest first
        std::array<Cluster, MAX_CLUSTERS> Clusters{};
        uint32 ChamberCount = 0;
        std::array<Chamber, MAX_CHAMBERS> Chambers{};
        int32 Own = -1;             // the own chamber's index, -1 when the body stands in none (a doorway, a passage)
        uint32 FrontierCells = 0;
        uint32 KnownCells = 0;
        std::array<int8, CELLS> ChamberOf{};    // per cell, the chamber's index or -1
        std::array<uint8, CELLS> Cell{};        // per cell, CellBit flags
    };

    /// The analysis of `crop` (Vision::CROP_BYTES, as MapBlock::Observe wrote it) taken at the body's (x, y, z) facing
    /// `yaw`. No allocation after the first call on a thread (thread-local scratch). Deterministic in the bytes.
    void Analyse(uint8 const* crop, float x, float y, float z, float yaw, uint32 clusterMinCells,
        uint32 pocketMinCells, Summary& out);

    /// The world point of cell (row, col) of a crop taken at `pose`: its centre, at the body's height (the heights
    /// are the crop's own: Analyse reads them for the centroids).
    inline void WorldOf(Summary const& pose, uint32 row, uint32 col, float& x, float& y)
    {
        float const forward = (float(SIDE) * 0.5f - 0.5f - float(row)) * CELL;
        float const right = (float(col) + 0.5f - float(SIDE) * 0.5f) * CELL;
        float const c = std::cos(pose.Yaw);
        float const s = std::sin(pose.Yaw);
        x = pose.X + forward * c + right * s;
        y = pose.Y + forward * s - right * c;
    }

    /// The cell of a crop taken at `pose` that holds the world point (x, y); false outside the window.
    inline bool Locate(Summary const& pose, float x, float y, uint32& row, uint32& col)
    {
        float const dx = x - pose.X;
        float const dy = y - pose.Y;
        float const c = std::cos(pose.Yaw);
        float const s = std::sin(pose.Yaw);
        float const forward = dx * c + dy * s;
        float const right = dx * s - dy * c;
        float const r = std::floor(float(SIDE) * 0.5f - forward / CELL);
        float const k = std::floor(float(SIDE) * 0.5f + right / CELL);
        if (r < 0.0f || k < 0.0f || r >= float(SIDE) || k >= float(SIDE))
            return false;
        row = uint32(r);
        col = uint32(k);
        return true;
    }

    [[nodiscard]] inline uint8 CellAt(Summary const& summary, uint32 row, uint32 col)
    {
        return summary.Cell[std::size_t(row) * SIDE + col];
    }
}

#endif
