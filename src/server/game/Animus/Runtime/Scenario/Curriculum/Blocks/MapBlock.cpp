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

#include "MapBlock.h"
#include "CellGrid.h"
#include "Player.h"
#include "SeatView.h"
#include "UnitBody.h"
#include "VisionCost.h"
#include <algorithm>
#include <atomic>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace Vi = Animus::Vision;

namespace
{
    constexpr char const* SCALAR_NAMES[Animus::Curriculum::MapBlock::OBS_COUNT] = { "known", "frontier", "visited",
        "kept", "searched", "new_age", "total", "frontier_sin", "frontier_cos", "frontier_dist", "region_sin",
        "region_cos", "region_dist", "clusters", "searched_cells" };

    /// The analysis's sizes (MapBlock::ConfigureCoverage): the seek tuning's defaults until the scenario sets them.
    std::atomic<uint32> ClusterMinCells{ 2 };
    std::atomic<uint32> PocketMinCells{ 6 };

    /// A bearing off the facing as sin and cos, and a distance over the reach, into three scalars.
    void Bearing(float facing, float x, float y, float fromX, float fromY, float* out)
    {
        float const relative = std::atan2(y - fromY, x - fromX) - facing;
        out[0] = std::sin(relative);
        out[1] = std::cos(relative);
        out[2] = std::min(1.0f, std::hypot(x - fromX, y - fromY) / Animus::Curriculum::Coverage::REACH);
    }
}

void Animus::Curriculum::MapBlock::ConfigureCoverage(uint32 clusterMinCells, uint32 pocketMinCells)
{
    ClusterMinCells.store(std::max<uint32>(1, clusterMinCells), std::memory_order_relaxed);
    PocketMinCells.store(std::max<uint32>(1, pocketMinCells), std::memory_order_relaxed);
}

Animus::Curriculum::BlockSize Animus::Curriculum::MapBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, 0 };
}

void Animus::Curriculum::MapBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    boost::json::object map;
    map["transport"] = "bytes";
    map["height"] = Vi::CROP;
    map["width"] = Vi::CROP;
    map["cell"] = double(Vi::CROP_CELL);
    map["heading_up"] = true;
    map["channels"] = Vi::CROP_CHANNELS;
    boost::json::array channels;
    for (char const* name : Vi::CROP_CHANNEL_NAMES)
        channels.push_back(boost::json::string(name));
    map["channel_names"] = std::move(channels);
    map["map_bytes"] = Vi::CROP_BYTES;
    map["codes"] = Vi::MAP_CODES;
    boost::json::array codes;
    for (char const* name : Vi::MAP_CODE_NAMES)
        codes.push_back(boost::json::string(name));
    map["code_names"] = std::move(codes);
    map["code_channel"] = uint32(Vi::CROP_CODE);
    map["height_channel"] = uint32(Vi::CROP_HEIGHT);
    map["height_step"] = double(Vi::CROP_HEIGHT_STEP);
    map["height_zero"] = uint32(Vi::CROP_HEIGHT_ZERO);
    map["visited_channel"] = uint32(Vi::CROP_VISITED);
    map["age_channel"] = uint32(Vi::CROP_AGE);
    map["age_scale"] = double(Vi::CROP_AGE_SCALE);
    map["age_never"] = uint32(Vi::CROP_AGE_NEVER);
    map["class_channel"] = uint32(Vi::CROP_CLASS);
    map["classes"] = Vi::CLASS_LIMIT;
    map["frontier_channel"] = uint32(Vi::CROP_FRONTIER);
    map["searched_channel"] = uint32(Vi::CROP_SEARCHED);
    map["searched_max"] = Vi::SEARCHED_MAX;
    map["new_age_scale_s"] = double(Vi::GROUND_AGE_SCALE_S);
    map["total_scale"] = double(Vi::GROUND_TOTAL_SCALE);
    map["epoch"] = "episode";
    // The coverage scalars' frame (revision 3, Coverage.h).
    map["coverage_cell_yards"] = double(Coverage::CELL);
    map["coverage_reach_yards"] = double(Coverage::REACH);
    map["cluster_scale"] = double(CLUSTER_SCALE);
    map["floor_scale"] = double(SEARCHED_SCALE);
    map["cluster_min_cells"] = ClusterMinCells.load(std::memory_order_relaxed);
    map["pocket_min_cells"] = PocketMinCells.load(std::memory_order_relaxed);
    map["scalars"] = uint32(OBS_COUNT);
    boost::json::array scalars;
    for (char const* name : SCALAR_NAMES)
        scalars.push_back(boost::json::string(name));
    map["scalar_names"] = std::move(scalars);
    block["map"] = std::move(map);
}

void Animus::Curriculum::MapBlock::Scalars(uint8 const* crop, bool kept, float* obs)
{
    uint32 known = 0;
    uint32 frontier = 0;
    uint32 visited = 0;
    uint32 searched = 0;
    for (uint32 cell = 0; cell < Vi::CROP * Vi::CROP; ++cell)
    {
        uint8 const* bytes = crop + std::size_t(cell) * Vi::CROP_CHANNELS;
        known += bytes[Vi::CROP_AGE] != Vi::CROP_AGE_NEVER ? 1 : 0;
        frontier += bytes[Vi::CROP_FRONTIER] ? 1 : 0;
        visited += bytes[Vi::CROP_VISITED] ? 1 : 0;
        searched += std::min<uint32>(bytes[Vi::CROP_SEARCHED], Vi::SEARCHED_MAX);
    }
    float const cells = float(Vi::CROP * Vi::CROP);
    obs[OBS_KNOWN] = float(known) / cells;
    obs[OBS_FRONTIER] = float(frontier) / cells;
    obs[OBS_VISITED] = float(visited) / cells;
    obs[OBS_KEPT] = kept ? 1.0f : 0.0f;
    obs[OBS_SEARCHED] = float(searched) / (float(Vi::SEARCHED_MAX) * cells);
    // The ground ones are the map's own (Observe); the coverage ones the analysis's (CoverageScalars).
    std::fill(obs + OBS_NEW_AGE, obs + OBS_COUNT, 0.0f);
}

void Animus::Curriculum::MapBlock::CoverageScalars(Coverage::Summary const& summary, float facing, float* obs)
{
    std::fill(obs + OBS_FRONTIER_SIN, obs + OBS_SEARCHED_CELLS, 0.0f);
    if (!summary.Valid || !summary.ClusterCount)
        return;
    // The nearest cluster by its centroid, and the largest (the clusters are kept largest first).
    Coverage::Cluster const* nearest = nullptr;
    float best = 0.0f;
    for (uint32 index = 0; index < summary.ClusterCount; ++index)
    {
        Coverage::Cluster const& cluster = summary.Clusters[index];
        float const distance = std::hypot(cluster.X - summary.X, cluster.Y - summary.Y);
        if (!nearest || distance < best)
        {
            nearest = &cluster;
            best = distance;
        }
    }
    Bearing(facing, nearest->X, nearest->Y, summary.X, summary.Y, obs + OBS_FRONTIER_SIN);
    Bearing(facing, summary.Clusters[0].X, summary.Clusters[0].Y, summary.X, summary.Y, obs + OBS_REGION_SIN);
    obs[OBS_CLUSTERS] = std::min(1.0f, float(summary.ClusterCount) / CLUSTER_SCALE);
}

void Animus::Curriculum::MapBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    // No map until this decision's says otherwise: every cell unknown (the zero row).
    if (view.MapRow)
        std::memset(view.MapRow, 0, Vi::CROP_BYTES);
    if (view.Crop)
        *view.Crop = CropPose();
    if (view.CoverageOut)
        *view.CoverageOut = Coverage::Summary();
    Vi::MentalMap* map = view.Map;
    Player* bot = view.Bot;
    if (!map || !bot || !bot->IsInWorld())
        return;

    auto const start = std::chrono::steady_clock::now();
    // The seat's own view of itself, as the camera takes it: the controller's body, else the server's position.
    float x = bot->GetPositionX();
    float y = bot->GetPositionY();
    float z = bot->GetPositionZ();
    bool grounded = !bot->IsInWater() && !bot->IsFalling() && !bot->IsFlying();
    if (Movement::BodyState const* body = view.Body)
    {
        x = body->X;
        y = body->Y;
        z = body->Z;
        grounded = body->Kind == Movement::Mode::Ground;
    }
    float const bodyHeight = Movement::ShapeOf(bot).Height;

    // The map's clock is the seat's decisions; then this decision's frame (as cast) and the body are written.
    map->Advance(float(view.DecisionMs) / 1000.0f);
    if (view.Hits && !view.Hits->Rays.empty())
        map->WriteFrame(*view.Hits, z, bodyHeight);
    // The entities the sensor listed this decision (the image no longer carries them).
    if (view.Seen)
        map->WriteEntities(*view.Seen);
    map->WriteBody(x, y, z, grounded);

    thread_local std::vector<uint8> scratch(Vi::CROP_BYTES);
    uint8* crop = view.MapRow ? view.MapRow : scratch.data();
    map->Crop(x, y, z, view.Facing, crop);
    Scalars(crop, view.MapKept, obs);
    obs[OBS_NEW_AGE] = std::min(1.0f, float(map->SecondsSinceGround()) / Vi::GROUND_AGE_SCALE_S);
    obs[OBS_TOTAL] = std::min(1.0f, float(map->GroundTotal()) / Vi::GROUND_TOTAL_SCALE);
    // The coverage analysis of this crop (general search): the scalars' clusters, and the seek encounter's terms
    // through the seat's summary (CoverageOut), the same frame the encounter's other reads are of.
    thread_local Coverage::Summary analysis;
    Coverage::Summary& summary = view.CoverageOut ? *view.CoverageOut : analysis;
    Coverage::Analyse(crop, x, y, z, view.Facing, ClusterMinCells.load(std::memory_order_relaxed),
        PocketMinCells.load(std::memory_order_relaxed), summary);
    CoverageScalars(summary, view.Facing, obs);
    obs[OBS_SEARCHED_CELLS] = std::min(1.0f, float(map->SearchedCells()) / SEARCHED_SCALE);
    // Where this crop was taken from and how many of its blocks a cell goal can name (free choice goals): the ACT that
    // answers this observation is decoded against exactly this pose (StageScenario::ApplyGoals).
    if (view.Crop)
        *view.Crop = CropPose{ true, x, y, z, view.Facing, CellGrid::Count(crop) };
    Vi::Cost::AddMap(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
        - start).count()), map->Tiles());
}
