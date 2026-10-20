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
#include "GlobalPlanner.h"
#include "Layout.h"
#include "ObjectMemory.h"
#include "Player.h"
#include "StageDefinition.h"
#include "SeatView.h"
#include "UnitBody.h"
#include "TravBake.h"
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
        "region_cos", "region_dist", "clusters", "searched_cells", "goal_forward_yards", "goal_right_yards",
        "route_p5_sin", "route_p5_cos", "route_p10_sin",
        "route_p10_cos", "route_p20_sin", "route_p20_cos", "route_p30_sin", "route_p30_cos", "route_fine_reach",
        "route_fine_valid", "route_coarse_sin", "route_coarse_cos", "route_coarse_dist", "route_coarse_valid",
        "route_dist", "route_dist_valid", "route_stall" };

    /// The analysis's sizes (MapBlock::ConfigureCoverage): the seek tuning's defaults until the scenario sets them.
    std::atomic<uint32> ClusterMinCells{ 2 };
    std::atomic<uint32> PocketMinCells{ 6 };
    /// The route's hysteresis (MapBlock::ConfigureRoute; Vision::KeepRoute): the nav tuning's defaults until the
    /// scenario sets them.
    std::atomic<bool> RouteKeep{ true };
    std::atomic<float> RouteKeepShare{ 0.1f };
    std::atomic<float> RouteKeepYards{ 5.0f };
    /// How far from the body the crop before's pose is read on the global field (above the labels' 12-yd step).
    constexpr float GLOBAL_LAST_YARDS = 16.0f;
    /// The bumper's pin (Vision::MentalMap::MarkBumped): how long the held keys must have got nowhere (SeatView::
    /// PinnedMs, under a tenth of what they asked -- under 0.7 yd a second at a run, 0.25 at a walk) before the body's
    /// wall contact is a wall on the map. A body sliding along a face makes its way and never pins, so rubbing along a
    /// wall paints nothing.
    constexpr uint32 BUMP_PINNED_MS = 1000;

    /// The global route's doors (Vision::GlobalBlocking): the closed doors and gates the seat's object memory holds
    /// (ObjectMemory::Blocking: class Door, not seen open, not absent), gathered once a decision, so a route's cell is
    /// asked of a handful of discs rather than the whole memory (BlockingAt's own scan, once a cell).
    class DoorBlocking final : public Vi::GlobalBlocking
    {
    public:
        explicit DoorBlocking(Vi::ObjectMemory const* objects)
        {
            if (!objects)
                return;
            thread_local std::vector<Vi::RememberedObject const*> found;
            objects->Blocking(found);
            for (Vi::RememberedObject const* entry : found)
                _doors.push_back({ entry->Position.X, entry->Position.Y,
                    std::max(entry->Radius, Vi::DOOR_MIN_RADIUS) });
        }
        [[nodiscard]] bool Any() const { return !_doors.empty(); }
        [[nodiscard]] bool Blocked(int32_t cx, int32_t cy, float /*z*/) const override
        {
            // ObjectMemory::BlockingAt's rule: the door's middle within its radius of the cell's centre.
            float const x = float(cx) + 0.5f;
            float const y = float(cy) + 0.5f;
            for (Door const& door : _doors)
                if (std::hypot(door.X - x, door.Y - y) <= door.Radius)
                    return true;
            return false;
        }

    private:
        struct Door
        {
            float X;
            float Y;
            float Radius;
        };
        std::vector<Door> _doors;
    };

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

void Animus::Curriculum::MapBlock::ConfigureRoute(bool keep, float share, float yards)
{
    RouteKeep.store(keep, std::memory_order_relaxed);
    RouteKeepShare.store(std::max(0.0f, share), std::memory_order_relaxed);
    RouteKeepYards.store(std::max(0.0f, yards), std::memory_order_relaxed);
}

Animus::Curriculum::BlockSize Animus::Curriculum::MapBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, 0 };
}

void Animus::Curriculum::MapBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
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
    // Revision 5: the tiers (StageDefinition::MapTiers), fine first; map_bytes is their total and tier_bytes one
    // tier's, so a learner reads the tier count off either.
    uint32 const tiers = std::max<uint32>(1, layout.Stage ? layout.Stage->MapTiers : 1);
    boost::json::array tierList;
    for (uint32 tier = 0; tier < tiers; ++tier)
    {
        boost::json::object entry;
        entry["cell"] = tier == 0 ? double(Vi::CROP_CELL) : double(Vi::COARSE_CELL);
        entry["bytes"] = Vi::CROP_BYTES;
        tierList.push_back(std::move(entry));
    }
    map["tiers"] = std::move(tierList);
    map["tier_bytes"] = Vi::CROP_BYTES;
    map["map_bytes"] = Vi::CROP_BYTES * tiers;
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
    // Revision 6: the walls channel and the crop's wall rule. Revision 7: the route scalars' frame -- the fine
    // walk's points (steps of a cell: yards), its reach, the coarse walk's, the distance scale and the stall clock.
    map["walls_channel"] = uint32(Vi::CROP_WALLS);
    map["walls_max"] = Vi::CROP_WALLS_MAX;
    map["wall_min"] = Vi::CROP_WALL_MIN;
    boost::json::array points;
    for (uint32 step : Vi::PLAN_ROUTE_STEPS)
        points.push_back(double(float(step) * Vi::MAP_CELL));
    map["route_points_yards"] = std::move(points);
    map["route_fine_reach_yards"] = double(float(Vi::PLAN_FINE_STEPS) * Vi::MAP_CELL);
    map["route_coarse_reach_yards"] = double(float(Vi::PLAN_COARSE_STEPS) * float(Vi::COARSE_CELL));
    map["route_dist_yards"] = double(Vi::PLAN_DIST_SCALE);
    map["route_stall_seconds"] = double(Vi::PLAN_STALL_SECONDS);
    // The fine route's hysteresis (Vision::KeepRoute; the same scalars, followed differently): on or off, and the
    // margin a new walk must undercut the kept one's cost by (a share of it and yards, both).
    map["route_keep"] = RouteKeep.load(std::memory_order_relaxed);
    map["route_keep_share"] = double(RouteKeepShare.load(std::memory_order_relaxed));
    map["route_keep_yards"] = double(RouteKeepYards.load(std::memory_order_relaxed));
    map["route_keep_off_cells"] = Vi::PLAN_KEEP_OFF;
    map["new_age_scale_s"] = double(Vi::GROUND_AGE_SCALE_S);
    // Revision 4: total and searched_cells are log1p(cells) / log1p(the max), clamped to 1 (MapBlock::LogShare).
    map["total_log_max"] = double(TOTAL_LOG_MAX);
    map["epoch"] = "episode";
    // The coverage scalars' frame (revision 3, Coverage.h).
    map["coverage_cell_yards"] = double(Coverage::CELL);
    map["coverage_reach_yards"] = double(Coverage::REACH);
    map["cluster_scale"] = double(CLUSTER_SCALE);
    map["searched_cells_log_max"] = double(SEARCHED_LOG_MAX);
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
    // The tiers the stage asks for (revision 5), fine first; the pose the crop before was taken at, before this
    // decision's replaces it (the own-map route at it is the labels' C_map(last)).
    uint32 const tiers = std::max<uint32>(1, view.L && view.L->Stage ? view.L->Stage->MapTiers : 1);
    CropPose const last = view.Crop ? *view.Crop : CropPose();
    // No map until this decision's says otherwise: every cell unknown (the zero row).
    if (view.MapRow)
        std::memset(view.MapRow, 0, std::size_t(Vi::CROP_BYTES) * tiers);
    if (view.Crop)
    {
        *view.Crop = CropPose();
        view.Crop->RouteSwitches = last.RouteSwitches;     // the episode's count survives a decision with no map
        view.Crop->BumpedCells = last.BumpedCells;         // ... and the bumper's
    }
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
    // The look-ahead cone the writes are counted against (PointNav.Explore): the body and its facing.
    map->SetLookCone(x, y, std::cos(view.Facing), std::sin(view.Facing));
    if (view.Hits && !view.Hits->Rays.empty())
        map->WriteFrame(*view.Hits, z, bodyHeight);
    // The entities the sensor listed this decision (the image no longer carries them).
    if (view.Seen)
        map->WriteEntities(*view.Seen);
    map->WriteBody(x, y, z, grounded);
    // **The bumper** (Vision::MentalMap::MarkBumped): a body on the ground holding a move key that has got nowhere
    // BUMP_PINNED_MS or more while the controller reports it against a wall felt what stopped it -- a wall the camera
    // never wrote, a doodad with no floor recorded under it: the remembered route's false cells (7-15 % of them in the
    // nav1_sight evaluation, where 39 of 256 trips were pinned ~57 of their 60 s). The cells ahead along the way the
    // keys push are walled before the crop and the plans read the map, so this decision's crop, window plan and global
    // route (its contradiction or bump recompute, GlobalRoute::Update) all see it. Perception-true: a player feels the
    // collision; nothing of the actions or the observation's layout changes. Falling, swimming or flying never bumps
    // (Mode::Ground only), nor a vertical key alone, nor a rooted body; the body's sweep meets geometry only
    // (MapWorldQuery), never a unit.
    if (Movement::BodyState const* body = view.Body; body && view.Controls && body->Kind == Movement::Mode::Ground
        && body->AgainstWall && view.PinnedMs >= BUMP_PINNED_MS && !bot->HasUnitState(UNIT_STATE_ROOT))
    {
        Movement::ControlState const& held = view.Controls->Held;
        if (held.Forward || held.Strafe)
        {
            // The push in the world: forward along the yaw, strafe right along (sin yaw, -cos yaw).
            float const cosYaw = std::cos(body->Yaw);
            float const sinYaw = std::sin(body->Yaw);
            float const pushX = float(held.Forward) * cosYaw + float(held.Strafe) * sinYaw;
            float const pushY = float(held.Forward) * sinYaw - float(held.Strafe) * cosYaw;
            map->MarkBumped(x, y, z, pushX, pushY, Movement::ShapeOf(bot).Radius);
        }
    }

    thread_local std::vector<uint8> scratch(2 * Vi::CROP_BYTES);
    uint8* crop = view.MapRow ? view.MapRow : scratch.data();
    map->Crop(x, y, z, view.Facing, crop);
    // The coarse tier behind the fine crop: the same pose, 8-yd cells (MentalMap::CropWide).
    uint8 const* coarse = nullptr;
    if (tiers >= 2)
    {
        uint8* row = crop + Vi::CROP_BYTES;
        map->Crop(x, y, z, view.Facing, row, float(Vi::COARSE_CELL));
        coarse = row;
    }
    Scalars(crop, view.MapKept, obs);
    obs[OBS_NEW_AGE] = std::min(1.0f, float(map->SecondsSinceGround()) / Vi::GROUND_AGE_SCALE_S);
    obs[OBS_TOTAL] = LogShare(float(map->GroundTotal()), TOTAL_LOG_MAX);
    // The coverage analysis of this crop (general search): the scalars' clusters, and the seek encounter's terms
    // through the seat's summary (CoverageOut), the same frame the encounter's other reads are of.
    thread_local Coverage::Summary analysis;
    Coverage::Summary& summary = view.CoverageOut ? *view.CoverageOut : analysis;
    Coverage::Analyse(crop, x, y, z, view.Facing, ClusterMinCells.load(std::memory_order_relaxed),
        PocketMinCells.load(std::memory_order_relaxed), summary);
    CoverageScalars(summary, view.Facing, obs);
    obs[OBS_SEARCHED_CELLS] = LogShare(float(map->SearchedCells()), SEARCHED_LOG_MAX);
    // Where this crop was taken from and how many of its blocks a cell goal can name (free choice goals): the ACT that
    // answers this observation is decoded against exactly this pose (StageScenario::ApplyGoals).
    CropPose pose{ true, x, y, z, view.Facing, CellGrid::Count(crop) };
    pose.BumpedCells = map->BumpedCells();
    // The body's clearance (signed-distance map): what both planners and the nav audit hold a cell's centre to.
    float const clearance = Vi::BodyClearance(Movement::ShapeOf(bot).Radius);
    pose.Clearance = clearance;
    // The route on the seat's own map to the objective the encounter names (revision 7): planned once here, its
    // points the policy's and its values the labels' (CropPose::Route*). The objective is the sim's knowledge, not
    // the bot's: nothing of the straight line to it leaves this block, only the route on the bot's own map. An
    // objective withheld from the seat (M1's ladder) is none.
    if (view.HasObjective && !view.CompassWithheld)
    {
        // The objective and the crop before's pose in the body frame: forward along the facing, right along
        // (sin yaw, -cos yaw).
        float const cosYaw = std::cos(view.Facing);
        float const sinYaw = std::sin(view.Facing);
        float const dx = view.Objective.GetPositionX() - x;
        float const dy = view.Objective.GetPositionY() - y;
        float const goalForward = dx * cosYaw + dy * sinYaw;
        float const goalRight = dx * sinYaw - dy * cosYaw;
        // The school's goal (OBS_GOAL_*): the same two numbers the plan is made from, in raw yards.
        obs[OBS_GOAL_FORWARD_YARDS] = goalForward;
        obs[OBS_GOAL_RIGHT_YARDS] = goalRight;
        // The global route over the whole mental map (Vision::GlobalRoute), where it routes the body: the goal a place
        // the map has seen and the body's cost reached. Its field is kept between decisions and recomputed when the
        // goal moves, the map contradicts the route (a new wall on it, a door remembered closed), new ground is seen
        // or its clock is due; so the route the policy follows, the labels' C_map(body) and C_map(last), and the
        // stall clock all read one fixed field between recomputes. Off under the "no_global" arm.
        pose.RouteValid = true;
        Vi::GlobalRoute* router = view.NoGlobal ? nullptr : view.Global;
        bool global = false;
        // The doors outlive every read of the field this decision: a read that resumes the search touches new nodes,
        // and a node's first touch asks them.
        DoorBlocking const doors(router && !view.ObjectsHidden ? view.Objects : nullptr);
        if (router)
        {
            if (!last.Valid)
                router->Reset();            // an episode's first decision (its pose was reset with the episode)
            global = router->Update(*map, x, y, z, view.Objective.GetPositionX(), view.Objective.GetPositionY(),
                view.Objective.GetPositionZ(), doors.Any() ? &doors : nullptr, clearance, view.PlanDrops);
            pose.GlobalKept = router->KeptThisUpdate();
            Vi::GlobalReplan const did = router->ThisUpdate();
            pose.GlobalReplanned = did != Vi::GlobalReplan::None && did != Vi::GlobalReplan::Fresh;
            pose.GlobalContradiction = did == Vi::GlobalReplan::Contradiction;
        }
        Vi::PlanRoute fine;
        Vi::PlanWaypoint far;
        float routeDist = Vi::TRAV_UNREACHABLE;
        if (global)
        {
            // The route points and the coarse waypoint off the global field (its walk from the body, 30 cells and 16
            // yd of it), its value at the body and at the crop before's pose; the window plan is not made. The walk's
            // first cells are kept as KeepRoute's, so a decision that falls back on the window plan follows on from
            // where this route was going until the window plan undercuts it by the margin.
            Vi::GlobalField& field = router->Field();
            pose.RouteGlobal = true;
            pose.RouteBody = field.ValueAt(x, y, z);
            // The crop before's pose is read only within GLOBAL_LAST_YARDS of the body (the labels read it within
            // their LABEL_STEP_MAX, 12 yd): a rise or a teleport would re-aim the search across the map for nothing.
            if (last.Valid)
            {
                pose.RouteLastValid = true;
                pose.LastX = last.X;
                pose.LastY = last.Y;
                pose.RouteLast = std::hypot(last.X - x, last.Y - y) <= GLOBAL_LAST_YARDS
                    ? field.ValueAt(last.X, last.Y, last.Z) : Vi::TRAV_UNREACHABLE;
            }
            fine = field.Route(x, y, z, view.Facing);
            far = field.Coarse(x, y, z, view.Facing);
            field.Walk(x, y, z, pose.Kept);
            pose.RouteSwitches = last.RouteSwitches;
            routeDist = pose.RouteBody;
        }
        else
        {
            // The points the plan must settle beyond the body: the crop before's pose (the labels' C_map(last)) and
            // the end of the walk the decision before followed (Vision::KeepRoute prices the kept walk to there).
            Vi::PlanPoint near[2];
            uint32 points = 0;
            if (last.Valid)
            {
                float const lx = last.X - x;
                float const ly = last.Y - y;
                near[points++] = Vi::PlanPoint{ lx * cosYaw + ly * sinYaw, lx * sinYaw - ly * cosYaw };
            }
            if (Vi::KeptEnd(last.Kept, x, y, view.Facing, near[points]))
                ++points;
            thread_local Vi::MapPlan plan;
            Vi::PlanOnMap(*map, x, y, z, view.Facing, goalForward, goalRight, coarse, plan, near, points, clearance,
                view.Objective.GetPositionZ());
            pose.RouteBody = Vi::PlanValueAt(plan, 0.0f, 0.0f);
            if (last.Valid)
            {
                pose.RouteLastValid = true;
                pose.LastX = last.X;
                pose.LastY = last.Y;
                pose.RouteLast = Vi::PlanValueAt(plan, near[0].Forward, near[0].Right);
            }
            // The fine walk followed: the one the decision before followed, kept unless this plan's undercuts it by
            // the margin (PointNav.RouteKeep*), else this plan's own (Vision::KeepRoute); the next decision keeps it
            // in turn.
            Vi::RouteKeeping keeping;
            keeping.Enabled = RouteKeep.load(std::memory_order_relaxed);
            keeping.Share = RouteKeepShare.load(std::memory_order_relaxed);
            keeping.Yards = RouteKeepYards.load(std::memory_order_relaxed);
            Vi::RouteChoice const choice = Vi::KeepRoute(plan, last.Kept, keeping);
            pose.Kept = choice.Walk;
            pose.RouteSwitches = last.RouteSwitches + (choice.Switched ? 1 : 0);
            fine = choice.Route;
            far = Vi::CoarseWaypoint(plan);
            routeDist = choice.Kept ? choice.Cost : pose.RouteBody;
        }
        static_assert(Vi::PLAN_ROUTE_POINTS == 4 && OBS_ROUTE_P30_COS == OBS_ROUTE_P5_SIN + 7);
        for (uint32 point = 0; point < Vi::PLAN_ROUTE_POINTS; ++point)
        {
            obs[OBS_ROUTE_P5_SIN + 2 * point] = fine.Sin[point];
            obs[OBS_ROUTE_P5_COS + 2 * point] = fine.Cos[point];
        }
        obs[OBS_ROUTE_FINE_REACH] = fine.Reach;
        obs[OBS_ROUTE_FINE_VALID] = fine.Valid;
        obs[OBS_ROUTE_COARSE_SIN] = far.Sin;
        obs[OBS_ROUTE_COARSE_COS] = far.Cos;
        obs[OBS_ROUTE_COARSE_DIST] = far.Dist;
        obs[OBS_ROUTE_COARSE_VALID] = far.Valid;
        // The own-map route distance at the body -- the kept walk's cost when it is followed, else the plan's value at
        // the body (a point's reading, blocked cells left out) -- and the stall clock carried from the pose before,
        // on the plan's value alone.
        bool const reachable = pose.RouteBody < Vi::TRAV_UNREACHABLE;
        if (routeDist < Vi::TRAV_UNREACHABLE)
        {
            obs[OBS_ROUTE_DIST] = std::min(1.0f, routeDist / Vi::PLAN_DIST_SCALE);
            obs[OBS_ROUTE_DIST_VALID] = 1.0f;
        }
        pose.RouteBestValid = last.RouteBestValid;
        pose.RouteBest = last.RouteBest;
        pose.RouteStall = last.RouteStall;
        Vi::RouteStall(pose.RouteBody, reachable, last.Valid ? float(view.DecisionMs) / 1000.0f : 0.0f,
            pose.RouteBestValid, pose.RouteBest, pose.RouteStall);
        obs[OBS_ROUTE_STALL] = std::min(1.0f, pose.RouteStall / Vi::PLAN_STALL_SECONDS);
    }
    else
    {
        // No objective this decision: the clock neither runs nor resets, the least so far is kept; no walk is kept
        // (there is nothing to plan to), and the switches so far stand.
        pose.RouteBestValid = last.RouteBestValid;
        pose.RouteBest = last.RouteBest;
        pose.RouteStall = last.RouteStall;
        pose.RouteSwitches = last.RouteSwitches;
    }
    if (view.Crop)
        *view.Crop = pose;
    Vi::Cost::AddMap(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
        - start).count()), map->Tiles());
}
