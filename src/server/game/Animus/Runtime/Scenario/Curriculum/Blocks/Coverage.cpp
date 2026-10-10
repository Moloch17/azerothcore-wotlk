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

#include "Coverage.h"
#include <algorithm>
#include <vector>

namespace
{
    namespace Cv = Animus::Curriculum::Coverage;
    namespace Vi = Animus::Vision;

    constexpr int32 SIDE = int32(Cv::SIDE);
    constexpr int32 N4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    constexpr int32 N8[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

    [[nodiscard]] constexpr bool InCrop(int32 row, int32 col)
    {
        return row >= 0 && col >= 0 && row < SIDE && col < SIDE;
    }

    [[nodiscard]] constexpr std::size_t At(int32 row, int32 col)
    {
        return std::size_t(row) * Cv::SIDE + std::size_t(col);
    }

    /// The scratch of one thread: the component labels, the BFS queue and the per-component tallies.
    struct Scratch
    {
        std::vector<int32> Label;
        std::vector<uint32> Queue;
        struct Part
        {
            uint32 Size = 0;
            double SumRow = 0.0;
            double SumCol = 0.0;
            double SumZ = 0.0;
            uint32 Heights = 0;
            uint32 First = 0;       // the first cell, for a tie-free order
        };
        std::vector<Part> Parts;
    };

    /// Label the connected components of the cells with `bit` set (over `neighbours` of `count`), the parts in
    /// `scratch.Parts`, the labels in `scratch.Label` (-1 none).
    void Components(Cv::Summary const& summary, uint8 const* crop, float z, uint8 bit, int32 const (*neighbours)[2],
        uint32 count, Scratch& scratch)
    {
        scratch.Label.assign(Cv::CELLS, -1);
        scratch.Parts.clear();
        for (int32 row = 0; row < SIDE; ++row)
            for (int32 col = 0; col < SIDE; ++col)
            {
                std::size_t const start = At(row, col);
                if (!(summary.Cell[start] & bit) || scratch.Label[start] >= 0)
                    continue;
                int32 const id = int32(scratch.Parts.size());
                scratch.Parts.push_back(Scratch::Part{});
                scratch.Parts.back().First = uint32(start);
                scratch.Queue.clear();
                scratch.Queue.push_back(uint32(start));
                scratch.Label[start] = id;
                while (!scratch.Queue.empty())
                {
                    uint32 const cell = scratch.Queue.back();
                    scratch.Queue.pop_back();
                    int32 const r = int32(cell / Cv::SIDE);
                    int32 const c = int32(cell % Cv::SIDE);
                    Scratch::Part& part = scratch.Parts.back();
                    ++part.Size;
                    part.SumRow += r;
                    part.SumCol += c;
                    uint8 const height = crop[cell * Vi::CROP_CHANNELS + Vi::CROP_HEIGHT];
                    if (height)
                    {
                        part.SumZ += z + float(int32(height) - int32(Vi::CROP_HEIGHT_ZERO)) * Vi::CROP_HEIGHT_STEP;
                        ++part.Heights;
                    }
                    for (uint32 n = 0; n < count; ++n)
                    {
                        int32 const nr = r + neighbours[n][0];
                        int32 const nc = c + neighbours[n][1];
                        if (!InCrop(nr, nc))
                            continue;
                        std::size_t const next = At(nr, nc);
                        if ((summary.Cell[next] & bit) && scratch.Label[next] < 0)
                        {
                            scratch.Label[next] = id;
                            scratch.Queue.push_back(uint32(next));
                        }
                    }
                }
            }
    }

    /// The parts of `minSize` or more cells, largest first (ties by the first cell), at most `most`, into `order`.
    void Largest(std::vector<Scratch::Part> const& parts, uint32 minSize, uint32 most, std::vector<int32>& order)
    {
        order.clear();
        for (int32 id = 0; id < int32(parts.size()); ++id)
            if (parts[std::size_t(id)].Size >= std::max<uint32>(1, minSize))
                order.push_back(id);
        std::sort(order.begin(), order.end(), [&parts](int32 a, int32 b)
        {
            Scratch::Part const& pa = parts[std::size_t(a)];
            Scratch::Part const& pb = parts[std::size_t(b)];
            return pa.Size != pb.Size ? pa.Size > pb.Size : pa.First < pb.First;
        });
        if (order.size() > most)
            order.resize(most);
    }
}

void Animus::Curriculum::Coverage::Analyse(uint8 const* crop, float x, float y, float z, float yaw,
    uint32 clusterMinCells, uint32 pocketMinCells, Summary& out)
{
    thread_local Scratch scratch;
    thread_local std::vector<int32> clusters;
    thread_local std::vector<int32> clusterIndex;
    thread_local std::vector<int32> clusterLabel;
    thread_local std::vector<int32> chambers;
    thread_local std::vector<int32> chamberIndex;
    out = Summary();
    out.Valid = crop != nullptr;
    out.X = x;
    out.Y = y;
    out.Z = z;
    out.Yaw = yaw;
    out.ChamberOf.fill(-1);
    if (!crop)
        return;

    // The cells: known, open, and (a second pass) frontier and eroded.
    for (std::size_t cell = 0; cell < CELLS; ++cell)
    {
        uint8 const code = crop[cell * Vi::CROP_CHANNELS + Vi::CROP_CODE];
        uint8 bits = 0;
        if (code != uint8(Vi::MapCode::Unknown))
            bits |= CELL_KNOWN;
        if (code == uint8(Vi::MapCode::Floor) || code == uint8(Vi::MapCode::Door))
            bits |= CELL_OPEN;
        out.Cell[cell] = bits;
        out.KnownCells += bits & CELL_KNOWN ? 1 : 0;
    }
    for (int32 row = 0; row < int32(SIDE); ++row)
        for (int32 col = 0; col < int32(SIDE); ++col)
        {
            uint8& bits = out.Cell[At(row, col)];
            if (!(bits & CELL_OPEN))
                continue;
            bool frontier = false;
            for (auto const& n : N4)
            {
                int32 const nr = row + n[0];
                int32 const nc = col + n[1];
                if (InCrop(nr, nc) && !(out.Cell[At(nr, nc)] & CELL_KNOWN))
                    frontier = true;
            }
            bool eroded = true;
            for (auto const& n : N8)
            {
                int32 const nr = row + n[0];
                int32 const nc = col + n[1];
                if (!InCrop(nr, nc) || !(out.Cell[At(nr, nc)] & CELL_OPEN))
                    eroded = false;
            }
            if (frontier)
            {
                bits |= CELL_FRONTIER;
                ++out.FrontierCells;
            }
            if (eroded)
                bits |= CELL_ERODED;
        }

    // The frontier clusters: 8-connected, ClusterMinCells or more, the largest MAX_CLUSTERS kept.
    Components(out, crop, z, CELL_FRONTIER, N8, 8, scratch);
    Largest(scratch.Parts, clusterMinCells, MAX_CLUSTERS, clusters);
    clusterIndex.assign(scratch.Parts.size(), -1);
    for (int32 id : clusters)
    {
        Scratch::Part const& part = scratch.Parts[std::size_t(id)];
        Cluster& cluster = out.Clusters[out.ClusterCount];
        clusterIndex[std::size_t(id)] = int32(out.ClusterCount++);
        cluster.Size = part.Size;
        float const row = float(part.SumRow / part.Size);
        float const col = float(part.SumCol / part.Size);
        cluster.Forward = (float(SIDE) * 0.5f - 0.5f - row) * CELL;
        cluster.Right = (col + 0.5f - float(SIDE) * 0.5f) * CELL;
        float const c = std::cos(yaw);
        float const s = std::sin(yaw);
        cluster.X = x + cluster.Forward * c + cluster.Right * s;
        cluster.Y = y + cluster.Forward * s - cluster.Right * c;
        cluster.Z = part.Heights ? float(part.SumZ / part.Heights) : z;
        cluster.Key = CellKey(cluster.X, cluster.Y, cluster.Z, KEY_YARDS);
    }
    // The clusters' labels are kept for the pocket touch below.
    clusterLabel = scratch.Label;

    // The chambers: the 4-connected parts of the eroded cells, PocketMinCells or more, the largest MAX_CHAMBERS kept
    // and grown back by a cell over the open cells (first come).
    Components(out, crop, z, CELL_ERODED, N4, 4, scratch);
    Largest(scratch.Parts, pocketMinCells, MAX_CHAMBERS, chambers);
    chamberIndex.assign(scratch.Parts.size(), -1);
    for (int32 id : chambers)
        chamberIndex[std::size_t(id)] = int32(out.ChamberCount++);
    for (std::size_t cell = 0; cell < CELLS; ++cell)
        if (scratch.Label[cell] >= 0 && chamberIndex[std::size_t(scratch.Label[cell])] >= 0)
            out.ChamberOf[cell] = int8(chamberIndex[std::size_t(scratch.Label[cell])]);
    for (int32 row = 0; row < int32(SIDE); ++row)
        for (int32 col = 0; col < int32(SIDE); ++col)
        {
            std::size_t const cell = At(row, col);
            if (out.ChamberOf[cell] >= 0 || !(out.Cell[cell] & CELL_OPEN))
                continue;
            for (auto const& n : N8)
            {
                int32 const nr = row + n[0];
                int32 const nc = col + n[1];
                if (!InCrop(nr, nc))
                    continue;
                std::size_t const next = At(nr, nc);
                // A core cell's label, not a grown one's: the growth is one cell deep.
                if (scratch.Label[next] >= 0 && chamberIndex[std::size_t(scratch.Label[next])] >= 0)
                {
                    out.ChamberOf[cell] = int8(chamberIndex[std::size_t(scratch.Label[next])]);
                    break;
                }
            }
        }

    // Reached from the body over open cells (a BFS from the four cells under the body), and the own chamber.
    int32 const centre = int32(SIDE) / 2;
    scratch.Queue.clear();
    for (int32 dr = -1; dr <= 0; ++dr)
        for (int32 dc = -1; dc <= 0; ++dc)
        {
            std::size_t const cell = At(centre + dr, centre + dc);
            if (out.ChamberOf[cell] >= 0 && out.Own < 0)
                out.Own = out.ChamberOf[cell];
            if (!(out.Cell[cell] & CELL_REACHED))
            {
                out.Cell[cell] |= CELL_REACHED;
                scratch.Queue.push_back(uint32(cell));
            }
        }
    for (std::size_t head = 0; head < scratch.Queue.size(); ++head)
    {
        uint32 const cell = scratch.Queue[head];
        int32 const r = int32(cell / SIDE);
        int32 const c = int32(cell % SIDE);
        for (auto const& n : N4)
        {
            int32 const nr = r + n[0];
            int32 const nc = c + n[1];
            if (!InCrop(nr, nc))
                continue;
            std::size_t const next = At(nr, nc);
            if ((out.Cell[next] & CELL_OPEN) && !(out.Cell[next] & CELL_REACHED))
            {
                out.Cell[next] |= CELL_REACHED;
                scratch.Queue.push_back(uint32(next));
            }
        }
    }

    // Per chamber: size, centroid and key over its grown cells; pocket when reached and not own.
    struct Tally
    {
        uint32 Size = 0;
        double SumRow = 0.0;
        double SumCol = 0.0;
        double SumZ = 0.0;
        uint32 Heights = 0;
        bool Reached = false;
    };
    std::array<Tally, MAX_CHAMBERS> tallies{};
    for (std::size_t cell = 0; cell < CELLS; ++cell)
    {
        int8 const index = out.ChamberOf[cell];
        if (index < 0)
            continue;
        Tally& tally = tallies[std::size_t(index)];
        ++tally.Size;
        tally.SumRow += double(cell / SIDE);
        tally.SumCol += double(cell % SIDE);
        uint8 const height = crop[cell * Vi::CROP_CHANNELS + Vi::CROP_HEIGHT];
        if (height)
        {
            tally.SumZ += z + float(int32(height) - int32(Vi::CROP_HEIGHT_ZERO)) * Vi::CROP_HEIGHT_STEP;
            ++tally.Heights;
        }
        tally.Reached = tally.Reached || (out.Cell[cell] & CELL_REACHED);
    }
    for (uint32 index = 0; index < out.ChamberCount; ++index)
    {
        Tally const& tally = tallies[index];
        Chamber& chamber = out.Chambers[index];
        chamber.Size = tally.Size;
        float const row = tally.Size ? float(tally.SumRow / tally.Size) : float(centre);
        float const col = tally.Size ? float(tally.SumCol / tally.Size) : float(centre);
        float const forward = (float(SIDE) * 0.5f - 0.5f - row) * CELL;
        float const right = (col + 0.5f - float(SIDE) * 0.5f) * CELL;
        float const c = std::cos(yaw);
        float const s = std::sin(yaw);
        chamber.X = x + forward * c + right * s;
        chamber.Y = y + forward * s - right * c;
        chamber.Z = tally.Heights ? float(tally.SumZ / tally.Heights) : z;
        chamber.Key = CellKey(chamber.X, chamber.Y, chamber.Z, KEY_YARDS);
        chamber.Own = int32(index) == out.Own;
        chamber.Pocket = !chamber.Own && tally.Reached;
    }

    // A cluster at a pocket: one of its cells eight-adjacent to a pocket's cell.
    for (int32 row = 0; row < int32(SIDE); ++row)
        for (int32 col = 0; col < int32(SIDE); ++col)
        {
            std::size_t const cell = At(row, col);
            int32 const label = clusterLabel[cell];
            if (label < 0 || clusterIndex[std::size_t(label)] < 0)
                continue;
            Cluster& cluster = out.Clusters[std::size_t(clusterIndex[std::size_t(label)])];
            if (cluster.AtPocket)
                continue;
            for (auto const& n : N8)
            {
                int32 const nr = row + n[0];
                int32 const nc = col + n[1];
                if (!InCrop(nr, nc))
                    continue;
                int8 const index = out.ChamberOf[At(nr, nc)];
                if (index >= 0 && out.Chambers[std::size_t(index)].Pocket)
                {
                    cluster.AtPocket = true;
                    break;
                }
            }
        }
}
