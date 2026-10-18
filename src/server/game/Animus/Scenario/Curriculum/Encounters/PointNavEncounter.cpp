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

#include "PointNavEncounter.h"
#include "BotFactory.h"
#include "Camera.h"
#include "CellGrid.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "GoalBlock.h"
#include "Log.h"
#include "Map.h"
#include "MapPlanner.h"
#include "MapVisionWorld.h"
#include "ObjectMgr.h"
#include "ObjectPool.h"
#include "PartyFollowEncounter.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SightDraw.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "StageState.h"
#include "StringFormat.h"
#include "Standing.h"
#include "UnitBody.h"
#include "UnitDefines.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <unordered_set>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// The run speed the time ratio's optimum is walked at (yards a second).
    constexpr float RUN_YARDS_PER_SECOND = 7.0f;
    /// Candidates tested for a SightOnly start at most (one camera ray each).
    constexpr uint32 SIGHT_TESTS = 64;
    /// Goals redrawn when no start fits the bins, before the nearest fit is taken.
    constexpr uint32 GOAL_TRIES = 4;
    /// Yards over 500: what the critic's state carries of a route distance.
    constexpr float STATE_YARDS_SCALE = 500.0f;
}

Animus::Curriculum::PointNavEncounter::PointNavEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _rngs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PointNavEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::StepCost, RewardTerm::Death, RewardTerm::FallHurt, RewardTerm::Drowned,
        RewardTerm::Lost, RewardTerm::Revisit, RewardTerm::Stuck, RewardTerm::Wall, RewardTerm::Turning };
}

std::vector<float> Animus::Curriculum::PointNavEncounter::ParseList(std::string const& list)
{
    std::vector<float> out;
    std::stringstream in(list);
    std::string item;
    while (std::getline(in, item, ','))
    {
        try
        {
            out.push_back(std::stof(item));
        }
        catch (std::exception const&)
        {
            return {};
        }
    }
    return out;
}

void Animus::Curriculum::PointNavEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // Columns the core or another encounter of the stage already reports (died, timed_out, jumps, drowned, the
    // seconds stuck, at a wall or swimming; the party follow's start_x/y/z and lost_seconds in nav6) are left to
    // them: a name twice in the row would be read as whichever the learner took last.
    auto const add = [&table](char const* name, EpisodeInfoTable::Getter getter)
    {
        if (!table.Contains(name))
            table.Add(name, std::move(getter));
    };
    auto const seatOf = [this](Env const& env, uint32 seat) -> SeatNav const&
    {
        return _envs[env.Index].Seats[std::min<uint32>(seat, GROUP_SEATS - 1)];
    };
    auto const nav = [this](Env const& env) -> EnvNav const& { return _envs[env.Index]; };
    // The stage's measure: arrived, and SPL (success weighted by path length: the optimum over the longer of the
    // optimum and the yards travelled), and the time against walking the optimum at run speed.
    add("arrived", [seatOf](Env const& env, uint32 seat) { return seatOf(env, seat).Reached ? 1.0f : 0.0f; });
    add("spl", [seatOf, nav](Env const& env, uint32 seat)
    {
        SeatNav const& s = seatOf(env, seat);
        float const optimal = nav(env).CStart;
        if (!s.Reached || optimal >= Vision::TRAV_UNREACHABLE)
            return 0.0f;
        return optimal / std::max(optimal, std::max(s.Travelled, 0.01f));
    });
    add("time_ratio", [seatOf, nav](Env const& env, uint32 seat)
    {
        SeatNav const& s = seatOf(env, seat);
        float const optimal = nav(env).CStart;
        if (!s.Reached || optimal >= Vision::TRAV_UNREACHABLE)
            return 0.0f;
        return (float(s.ArriveMs) / 1000.0f) / std::max(0.25f, optimal / RUN_YARDS_PER_SECOND);
    });
    // The trip's clock (PointNav.TimeFactor: its route's, in a drawn trip; the arena's otherwise), and how often the
    // map block's kept route was given up for another (CropPose::RouteSwitches, Vision::KeepRoute).
    add("time_limit_seconds", [](Env const& env, uint32) { return float(env.EpisodeLengthMs) / 1000.0f; });
    // Arrived within the arena's own EpisodeSeconds (the clock every trip had before PointNav.TimeFactor): the
    // measure an evaluation compares with runs on the fixed clock by.
    add("arrived_base_limit", [this, seatOf](Env const& env, uint32 seat)
    {
        SeatNav const& s = seatOf(env, seat);
        return s.Reached && s.ArriveMs <= _scenario.ArenaEpisodeMs(env) ? 1.0f : 0.0f;
    });
    add("route_switches", [this](Env const& env, uint32 seat)
    {
        return float(_scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Crop.RouteSwitches);
    });
    // The route's source (MapBlock::Observe): the share of the labelled decisions whose route came from the global
    // route over the whole mental map rather than the window plan, and the global route's recomputes after each
    // trip's first plan -- all of them, and those a contradiction of the route about to be walked.
    add("route_source_global_share", [seatOf](Env const& env, uint32 seat)
    {
        SeatNav const& s = seatOf(env, seat);
        return s.RouteDecisions ? float(s.GlobalDecisions) / float(s.RouteDecisions) : 0.0f;
    });
    add("global_replans", [seatOf](Env const& env, uint32 seat) { return float(seatOf(env, seat).GlobalReplans); });
    add("global_replans_contradiction", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).GlobalContradictions);
    });
    // The bumper (MapBlock::Observe, Vision::MentalMap::MarkBumped): the 1-yd cells walled this episode because the
    // body was pinned against a wall its map lacked.
    add("bumped_cells", [this](Env const& env, uint32 seat)
    {
        return float(_scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Crop.BumpedCells);
    });
    add("optimal_yards", [nav](Env const& env, uint32)
    {
        return nav(env).CStart >= Vision::TRAV_UNREACHABLE ? 0.0f : nav(env).CStart;
    });
    add("straight_yards", [nav](Env const& env, uint32) { return nav(env).Straight; });
    add("detour_factor", [nav](Env const& env, uint32)
    {
        EnvNav const& n = nav(env);
        return n.CStart >= Vision::TRAV_UNREACHABLE || n.Straight <= 0.0f ? 0.0f : n.CStart / n.Straight;
    });
    add("travelled_yards", [seatOf](Env const& env, uint32 seat) { return seatOf(env, seat).Travelled; });
    add("stop_error_yards", [seatOf](Env const& env, uint32 seat) { return seatOf(env, seat).StopError; });
    add("falls", [this](Env const& env, uint32 seat)
    {
        return float(_scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Falls);
    });
    add("fall_damage_share", [seatOf](Env const& env, uint32 seat) { return seatOf(env, seat).FallShare; });
    add("drowned", [this](Env const& env, uint32 seat)
    {
        return _scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Drowned ? 1.0f : 0.0f;
    });
    add("died", [this](Env const& env, uint32 seat)
    {
        return _scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Combat.Died ? 1.0f : 0.0f;
    });
    add("timed_out", [this](Env const& env, uint32 seat)
    {
        return _scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Combat.TimedOut ? 1.0f : 0.0f;
    });
    add("stuck_seconds", [seatOf](Env const& env, uint32 seat) { return float(seatOf(env, seat).StuckMs) / 1000.0f; });
    add("wall_seconds", [seatOf](Env const& env, uint32 seat) { return float(seatOf(env, seat).WallMs) / 1000.0f; });
    add("turn_radians", [seatOf](Env const& env, uint32 seat) { return seatOf(env, seat).TurnRadians; });
    add("lost_seconds", [seatOf](Env const& env, uint32 seat) { return float(seatOf(env, seat).LostMs) / 1000.0f; });
    add("revisit_seconds", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).RevisitMs) / 1000.0f;
    });
    add("jumps", [this](Env const& env, uint32 seat)
    {
        return float(_scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].Jumps);
    });
    add("swim_seconds", [this](Env const& env, uint32 seat)
    {
        return float(_scenario.Data(env).Seats[std::min<uint32>(seat, MAX_SEATS - 1)].WaterMs) / 1000.0f;
    });
    // The draw: the rung and its bins, whether the start was a replayed failure, and where (the map, the start and
    // the goal; a nav6 episode has no pair and reports its map alone).
    add("nav_rung", [nav](Env const& env, uint32) { return float(nav(env).Rung); });
    // The learner's fade ladder and convergence read the rung as `difficulty` and `at_top_rung`, as the seek and
    // follow stages report them (train.py difficulty_column); in nav6 the follow's own stand.
    add("difficulty", [nav](Env const& env, uint32) { return float(nav(env).Rung); });
    add("at_top_rung", [nav](Env const& env, uint32) { return nav(env).Rung + 1 >= RUNGS ? 1.0f : 0.0f; });
    add("detour_bin", [nav](Env const& env, uint32) { return float(nav(env).DetourBin); });
    add("length_bin", [nav](Env const& env, uint32) { return float(nav(env).LengthBin); });
    add("replayed", [nav](Env const& env, uint32) { return nav(env).Replayed ? 1.0f : 0.0f; });
    add("map", [nav](Env const& env, uint32) { return float(nav(env).MapId); });
    add("start_x", [nav](Env const& env, uint32) { return nav(env).Start.GetPositionX(); });
    add("start_y", [nav](Env const& env, uint32) { return nav(env).Start.GetPositionY(); });
    add("start_z", [nav](Env const& env, uint32) { return nav(env).Start.GetPositionZ(); });
    add("goal_x", [nav](Env const& env, uint32) { return nav(env).Goal.GetPositionX(); });
    add("goal_y", [nav](Env const& env, uint32) { return nav(env).Goal.GetPositionY(); });
    add("goal_z", [nav](Env const& env, uint32) { return nav(env).Goal.GetPositionZ(); });
    // The planner's readout: over the cell goals chosen, the route distance of the chosen block less the best
    // choosable block's (0 without a choice), and how many were chosen.
    add("waypoint_regret_yards", [seatOf](Env const& env, uint32 seat)
    {
        SeatNav const& s = seatOf(env, seat);
        return s.Waypoints ? s.RegretSum / float(s.Waypoints) : 0.0f;
    });
    add("waypoints_chosen", [seatOf](Env const& env, uint32 seat) { return float(seatOf(env, seat).Waypoints); });
    // The route on the seat's own map (the labels' MapPlanner value at the body) at the episode's first decision and
    // at its last; 0 where the map had no route.
    add("map_route_start_yards", [seatOf](Env const& env, uint32 seat)
    {
        float const yards = seatOf(env, seat).MapRouteStart;
        return yards >= Vision::TRAV_UNREACHABLE ? 0.0f : yards;
    });
    add("map_route_end_yards", [seatOf](Env const& env, uint32 seat)
    {
        float const yards = seatOf(env, seat).MapRouteLast;
        return yards >= Vision::TRAV_UNREACHABLE ? 0.0f : yards;
    });
    // The map audit (AuditField): made by the first of its columns read, at the episode's end.
    for (uint32 field = 0; field < AUDIT_COUNT; ++field)
        add(AUDIT_NAMES[field], [this, field](Env const& env, uint32 seat)
        {
            SeatNav& s = _envs[env.Index].Seats[std::min<uint32>(seat, GROUP_SEATS - 1)];
            if (!s.Audited)
                RunAudit(env, std::min<uint32>(seat, GROUP_SEATS - 1));
            return s.Audit[field];
        });
}

namespace
{
    std::mutex AuditDirLock;
    std::string AuditDir;

    /// A bake cell no body stands on: rock, a void, a slope too steep.
    bool AuditSolid(Animus::Vision::TravClass what)
    {
        using Animus::Vision::TravClass;
        return what == TravClass::Unknown || what == TravClass::Wall || what == TravClass::Slope;
    }

    bool AuditStep(Animus::Vision::TravClass what)
    {
        using Animus::Vision::TravClass;
        return what == TravClass::StepUp || what == TravClass::Jump || what == TravClass::DropSafe
            || what == TravClass::DropHurt || what == TravClass::DropLethal;
    }

    /// A band cell's kind in the dump and the counts (AuditField's AUDIT_WALL .. AUDIT_FLOOR order).
    enum AuditKind : uint8_t
    {
        KIND_FLOOR = 0,
        KIND_WALL,
        KIND_CLOSED,
        KIND_UNKNOWN,
        KIND_HAZARD,
    };

    AuditKind KindOf(float raw, float shaped)
    {
        if (raw >= Animus::Vision::PLAN_BLOCKED)
            return KIND_WALL;
        if (shaped >= Animus::Vision::PLAN_BLOCKED)
            return KIND_CLOSED;
        if (raw >= Animus::Vision::PLAN_COSTS[uint8_t(Animus::Vision::MapCode::Hazard)])
            return KIND_HAZARD;
        if (raw >= Animus::Vision::PLAN_UNKNOWN)
            return KIND_UNKNOWN;
        return KIND_FLOOR;
    }
}

void Animus::Curriculum::PointNavEncounter::SetAuditDir(std::string dir)
{
    std::lock_guard<std::mutex> guard(AuditDirLock);
    AuditDir = std::move(dir);
}

void Animus::Curriculum::PointNavEncounter::RunAudit(Env const& env, uint32 seatIndex)
{
    namespace Vi = Animus::Vision;
    EnvNav const& nav = _envs[env.Index];
    SeatNav& seat = _envs[env.Index].Seats[seatIndex];
    seat.Audited = true;
    seat.Audit.fill(0.0f);
    if (!nav.Placed || nav.LabelsOnly || !nav.Grid || nav.GoalCell == Vi::TRAV_NO_CELL || nav.Cost.empty()
        || seat.Reached || !seat.HasLastPos)
        return;
    Vi::TravGrid const& grid = *nav.Grid;
    SeatState const& state = _scenario.Data(env).Seats[std::min<uint32>(seatIndex, MAX_SEATS - 1)];
    Vi::MentalMap const& map = state.Map;
    uint32 cell = grid.CellOf(seat.LastX, seat.LastY);
    if (cell == Vi::TRAV_NO_CELL || nav.Cost[cell] >= Vi::TRAV_UNREACHABLE)
    {
        seat.Audit[AUDIT_UNREACHABLE] = 1.0f;
        return;
    }
    seat.Audit[AUDIT_TRUE_YARDS] = nav.Cost[cell];

    // The true route: from the body's bake cell down the cost-to-go, along the bake's own edges, to the goal; cut
    // where its band would no longer fit AUDIT_SIDE_MAX cells across.
    auto const cellBox = [&grid](uint32 at, int32& x0, int32& y0, int32& x1, int32& y1)
    {
        float const minX = grid.CentreX(at) - 0.5f * Vi::TRAV_CELL - Vi::TRAV_CELL;
        float const minY = grid.CentreY(at) - 0.5f * Vi::TRAV_CELL - Vi::TRAV_CELL;
        x0 = int32(std::floor(minX));
        y0 = int32(std::floor(minY));
        x1 = int32(std::floor(minX + 3.0f * Vi::TRAV_CELL)) - 1;
        y1 = int32(std::floor(minY + 3.0f * Vi::TRAV_CELL)) - 1;
    };
    std::vector<uint32> path{ cell };
    int32 bx0, by0, bx1, by1;
    cellBox(cell, bx0, by0, bx1, by1);
    bool truncated = false;
    while (cell != nav.GoalCell)
    {
        uint32 best = Vi::TRAV_NO_CELL;
        float bestCost = nav.Cost[cell];
        for (int32 dir = 0; dir < Vi::TRAV_DIRS; ++dir)
        {
            if (Vi::TravEdge(grid.Edges[std::size_t(cell) * Vi::TRAV_DIRS + dir]) == Vi::TravEdge::None)
                continue;
            uint32 const next = grid.Neighbour(cell, dir);
            if (next != Vi::TRAV_NO_CELL && nav.Cost[next] < bestCost)
            {
                best = next;
                bestCost = nav.Cost[next];
            }
        }
        if (best == Vi::TRAV_NO_CELL)
            break;
        int32 x0, y0, x1, y1;
        cellBox(best, x0, y0, x1, y1);
        int32 const nx0 = std::min(bx0, x0), ny0 = std::min(by0, y0);
        int32 const nx1 = std::max(bx1, x1), ny1 = std::max(by1, y1);
        if (nx1 - nx0 + 1 > AUDIT_SIDE_MAX || ny1 - ny0 + 1 > AUDIT_SIDE_MAX)
        {
            truncated = true;
            break;
        }
        bx0 = nx0, by0 = ny0, bx1 = nx1, by1 = ny1;
        cell = best;
        path.push_back(cell);
    }
    seat.Audit[AUDIT_TRUNCATED] = truncated ? 1.0f : 0.0f;
    std::unordered_set<uint32> band;
    for (uint32 const at : path)
    {
        band.insert(at);
        for (int32 dir = 0; dir < Vi::TRAV_DIRS; ++dir)
            if (uint32 const next = grid.Neighbour(at, dir); next != Vi::TRAV_NO_CELL)
                band.insert(next);
    }

    // The square over the band, the map's cells in it priced for the bake's ground there (the body's own, where the
    // bake has none) as the fine planner prices them, then shaped into the configuration space.
    int32 const side = std::max(bx1 - bx0, by1 - by0) + 1;
    int32 const x0 = bx0;
    int32 const y0 = by0;
    std::size_t const cells = std::size_t(side) * std::size_t(side);
    std::vector<Vi::MapCell> window(cells);
    map.Window(x0, y0, side, window.data());
    float const bodyZ = grid.StandingZ(grid.CellOf(seat.LastX, seat.LastY));
    std::vector<float> raw(cells), shaped(cells), groundZ(cells);
    std::vector<uint32> bakeOf(cells);
    std::vector<uint8> inBand(cells);
    for (int32 row = 0; row < side; ++row)
        for (int32 col = 0; col < side; ++col)
        {
            std::size_t const at = std::size_t(row) * std::size_t(side) + std::size_t(col);
            uint32 const bake = grid.CellOf(float(x0 + col) + 0.5f, float(y0 + row) + 0.5f);
            bakeOf[at] = bake;
            float z = bodyZ;
            if (bake != Vi::TRAV_NO_CELL && grid.Ground[bake] != Vi::TRAV_NO_HEIGHT)
                z = grid.StandingZ(bake);
            groundZ[at] = z;
            raw[at] = Vi::CellFactor(window[at], z);
            inBand[at] = bake != Vi::TRAV_NO_CELL && band.count(bake) ? 1 : 0;
        }
    shaped = raw;
    Vi::ConfigurationSpace(shaped.data(), side);

    // The cheapest way through the band, body to goal (the true route's last cell where it was cut).
    auto const local = [&](float x, float y, int32& at) -> bool
    {
        int32 const col = int32(std::floor(x)) - x0;
        int32 const row = int32(std::floor(y)) - y0;
        if (col < 0 || row < 0 || col >= side || row >= side)
            return false;
        at = row * side + col;
        return true;
    };
    int32 from = 0, to = 0;
    float const goalX = truncated ? grid.CentreX(path.back()) : nav.Goal.GetPositionX();
    float const goalY = truncated ? grid.CentreY(path.back()) : nav.Goal.GetPositionY();
    if (!local(seat.LastX, seat.LastY, from) || !local(goalX, goalY, to))
        return;
    inBand[std::size_t(from)] = inBand[std::size_t(to)] = 1;
    constexpr int32 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    constexpr int32 DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    constexpr float STEP[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.4142136f, 1.4142136f, 1.4142136f, 1.4142136f };
    std::vector<float> dist(cells, std::numeric_limits<float>::infinity());
    std::vector<int32> parent(cells, -1);
    using Entry = std::pair<float, int32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    dist[std::size_t(from)] = 0.0f;
    open.push({ 0.0f, from });
    while (!open.empty())
    {
        auto const [value, at] = open.top();
        open.pop();
        if (value > dist[std::size_t(at)])
            continue;
        if (at == to)
            break;
        int32 const row = at / side;
        int32 const col = at % side;
        for (int32 dir = 0; dir < 8; ++dir)
        {
            int32 const r = row + DY[dir];
            int32 const c = col + DX[dir];
            if (r < 0 || c < 0 || r >= side || c >= side)
                continue;
            int32 const next = r * side + c;
            if (!inBand[std::size_t(next)])
                continue;
            float const cost = value + shaped[std::size_t(next)] * STEP[dir];
            if (cost < dist[std::size_t(next)])
            {
                dist[std::size_t(next)] = cost;
                parent[std::size_t(next)] = at;
                open.push({ cost, next });
            }
        }
    }
    if (parent[std::size_t(to)] < 0 && to != from)
        return;
    seat.Audit[AUDIT_CORRIDOR_COST] = dist[std::size_t(to)];

    // The way's cells from the body on: what the map holds in each, and of its walls, who wrote them and what the
    // bake has there.
    std::vector<int32> way;
    for (int32 at = to; at >= 0; at = parent[std::size_t(at)])
        way.push_back(at);
    std::reverse(way.begin(), way.end());
    std::vector<uint8> onWay(cells);
    float along = 0.0f;
    bool blocked = false;
    for (std::size_t index = 0; index < way.size(); ++index)
    {
        std::size_t const at = std::size_t(way[index]);
        onWay[at] = 1;
        if (index)
        {
            int32 const prev = way[index - 1];
            bool const diagonal = prev % side != way[index] % side && prev / side != way[index] / side;
            along += diagonal ? 1.4142136f : 1.0f;
        }
        seat.Audit[AUDIT_CELLS] += 1.0f;
        AuditKind const kind = KindOf(raw[at], shaped[at]);
        switch (kind)
        {
            case KIND_FLOOR: seat.Audit[AUDIT_FLOOR] += 1.0f; break;
            case KIND_WALL: seat.Audit[AUDIT_WALL] += 1.0f; break;
            case KIND_CLOSED: seat.Audit[AUDIT_CLOSED] += 1.0f; break;
            case KIND_UNKNOWN: seat.Audit[AUDIT_UNKNOWN] += 1.0f; break;
            case KIND_HAZARD: seat.Audit[AUDIT_HAZARD] += 1.0f; break;
        }
        if ((kind == KIND_WALL || kind == KIND_CLOSED) && !blocked)
        {
            blocked = true;
            seat.Audit[AUDIT_FIRST_BLOCK] = std::max(along, 0.5f);
        }
        if (kind != KIND_WALL)
            continue;
        int32 const cx = x0 + way[index] % side;
        int32 const cy = y0 + way[index] / side;
        switch (map.WallSourceOf(cx, cy))
        {
            case Vi::WallSource::Ray: seat.Audit[AUDIT_WALL_RAY] += 1.0f; break;
            case Vi::WallSource::Mesh: seat.Audit[AUDIT_WALL_MESH] += 1.0f; break;
            case Vi::WallSource::Bump: seat.Audit[AUDIT_WALL_BUMP] += 1.0f; break;
            default: seat.Audit[AUDIT_WALL_INHERITED] += 1.0f; break;
        }
        uint32 const bake = bakeOf[at];
        bool solidNear = false, stepNear = bake != Vi::TRAV_NO_CELL && AuditStep(grid.ClassOf(bake));
        if (bake != Vi::TRAV_NO_CELL)
            for (int32 dir = 0; dir < Vi::TRAV_DIRS; ++dir)
            {
                uint32 const next = grid.Neighbour(bake, dir);
                Vi::TravClass const what = next == Vi::TRAV_NO_CELL ? Vi::TravClass::Unknown : grid.ClassOf(next);
                solidNear = solidNear || AuditSolid(what);
                stepNear = stepNear || AuditStep(what);
            }
        if (bake == Vi::TRAV_NO_CELL || AuditSolid(grid.ClassOf(bake)))
            seat.Audit[AUDIT_WALL_TRUE] += 1.0f;
        else if (solidNear)
            seat.Audit[AUDIT_WALL_FACE] += 1.0f;
        else if (stepNear)
            seat.Audit[AUDIT_WALL_STEP] += 1.0f;
        else
            seat.Audit[AUDIT_WALL_PHANTOM] += 1.0f;
        float floor = 0.0f;
        if (!Vi::NearestFloor(window[at], groundZ[at], floor))
            seat.Audit[AUDIT_WALL_FLOORLESS] += 1.0f;
        else if (std::fabs(floor - groundZ[at]) > Vi::LAYER_MERGE)
            seat.Audit[AUDIT_WALL_LAYER_OFF] += 1.0f;
    }

    // A timed-out evaluation trip's band, for tools/nav_audit.py: a header, then a byte a cell for each of the map's
    // code for the bake's ground (Vision::CodeOf), the way's kind (KindOf), the wall's writer (255: no wall), the
    // bake's class, and flags (1 the band, 2 the true route's bake cells, 4 the way).
    if (!env.Evaluating || !state.Combat.TimedOut)
        return;
    std::string dir;
    {
        std::lock_guard<std::mutex> guard(AuditDirLock);
        dir = AuditDir;
    }
    if (dir.empty())
        return;
    std::unordered_set<uint32> route(path.begin(), path.end());
    std::vector<uint8> bytes(cells * 5);
    for (std::size_t at = 0; at < cells; ++at)
    {
        int32 const cx = x0 + int32(at % std::size_t(side));
        int32 const cy = y0 + int32(at / std::size_t(side));
        bytes[at] = uint8(Vi::CodeOf(window[at], groundZ[at]));
        bytes[cells + at] = uint8(KindOf(raw[at], shaped[at]));
        bytes[2 * cells + at] = raw[at] >= Vi::PLAN_BLOCKED ? uint8(map.WallSourceOf(cx, cy)) : 255;
        bytes[3 * cells + at] = bakeOf[at] == Vi::TRAV_NO_CELL ? 0 : uint8(grid.ClassOf(bakeOf[at]));
        bytes[4 * cells + at] = uint8((inBand[at] ? 1 : 0) | (bakeOf[at] != Vi::TRAV_NO_CELL
            && route.count(bakeOf[at]) ? 2 : 0) | (onWay[at] ? 4 : 0));
    }
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::filesystem::path const file = std::filesystem::path(dir)
        / Acore::StringFormat("{}-seat{}.audit", env.EpisodeSeedIndex, seatIndex);
    std::ofstream out(file, std::ios::binary);
    if (!out)
        return;
    char const magic[8] = { 'N', 'A', 'V', 'A', 'U', 'D', '1', '\0' };
    int32 const header[4] = { side, x0, y0, int32(env.EpisodeSeedIndex) };
    float const where[5] = { seat.LastX, seat.LastY, nav.Goal.GetPositionX(), nav.Goal.GetPositionY(),
        seat.MapRouteLast };
    out.write(magic, sizeof(magic));
    out.write(reinterpret_cast<char const*>(header), sizeof(header));
    out.write(reinterpret_cast<char const*>(where), sizeof(where));
    out.write(reinterpret_cast<char const*>(seat.Audit.data()), sizeof(float) * AUDIT_COUNT);
    out.write(reinterpret_cast<char const*>(bytes.data()), std::streamsize(bytes.size()));
}

void Animus::Curriculum::PointNavEncounter::ResetEpisode(Env& env)
{
    // The bake and the map stay (they are the arena's); the pair and the measures go.
    EnvNav& nav = _envs[env.Index];
    Vision::TravGrid const* grid = nav.Grid;
    uint32 const mapId = nav.MapId;
    nav = EnvNav();
    nav.Grid = grid;
    nav.MapId = mapId;
}

void Animus::Curriculum::PointNavEncounter::BeforeLevel(Env& env)
{
    ArenaDefinition const& arena = _scenario.Arena(env);
    EnvNav& nav = _envs[env.Index];
    nav.LabelsOnly = arena.LabelsOnly;
    uint32 const mapId = arena.MapId ? arena.MapId : _scenario.Stage().MapId;
    if (env.Evaluating && env.EpisodeSeedIndex != Animus::NO_EPISODE_SEED)
        _rngs[env.Index].seed(uint32(env.EpisodeSeedIndex) * 2654435761u ^ (mapId * 40503u + 0x7f4a7c15u));
    else
        _rngs[env.Index].seed(urand(0, std::numeric_limits<uint32>::max() - 1));
    if (nav.LabelsOnly)
        return;     // the party follow places the party; this is its labeller

    // The seat stands at the dungeon's door until Build draws its pair: an arena on a map of its own has no ground
    // of its own, and the stage's spawn is on another map.
    EnvState& data = _scenario.Data(env);
    if (AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId))
    {
        data.EpisodeSpawn.Relocate(entrance->target_X, entrance->target_Y, entrance->target_Z,
            entrance->target_Orientation);
        data.HasEpisodeSpawn = true;
    }
    data.DungeonDifficulty = 0;
    data.RaidDifficulty = 0;
}

Animus::Curriculum::PointNavEncounter::OpenBins Animus::Curriculum::PointNavEncounter::BinsOf(uint32 rung) const
{
    CurriculumTuning::PointNavTuning const& tuning = _scenario.Tuning().PointNav;
    std::string const& spec = rung == 0 ? tuning.RungBins0 : rung == 1 ? tuning.RungBins1
        : rung == 2 ? tuning.RungBins2 : tuning.RungBins3;
    std::vector<float> const pair = ParseList(spec);
    OpenBins bins;
    if (pair.size() >= 2)
    {
        bins.Detour = uint32(std::max(1.0f, pair[0]));
        bins.Length = uint32(std::max(1.0f, pair[1]));
    }
    return bins;
}

Animus::Vision::TravGrid const* Animus::Curriculum::PointNavEncounter::EnsureGrid(Env const& env, Map* map,
    Player const* bot) const
{
    ArenaDefinition const& arena = _scenario.Arena(env);
    Vision::TravBakeRequest request;
    request.World = map;
    request.PhaseMask = bot ? bot->GetPhaseMask() : 1;
    request.Arena = arena.Name;
    request.HasBox = arena.HasBox;
    request.Box = { arena.BoxMinX, arena.BoxMinY, arena.BoxMaxX, arena.BoxMaxY };
    // The flood starts at the dungeon's door: where the entrance's trigger puts a player.
    if (AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(map->GetId()))
    {
        request.HasSeed = true;
        request.SeedX = entrance->target_X;
        request.SeedY = entrance->target_Y;
        request.SeedZ = entrance->target_Z;
    }
    std::string const travDir = (std::filesystem::path(_scenario.DataDir()) / "trav").string();
    Vision::TravBakeReport report;
    std::string error;
    Vision::TravGrid const* grid = Vision::TravRegistry::Instance().Ensure(request, travDir, report, error);
    if (!grid)
        LOG_ERROR("module.animus", "{}: arena {} has no traversability bake: {}", _scenario.Name(), arena.Name,
            error);
    return grid;
}

Position Animus::Curriculum::PointNavEncounter::CellPoint(Vision::TravGrid const& grid, uint32 cell, bool bed)
{
    float const z = bed ? grid.Ground[cell] : grid.StandingZ(cell);
    return Position(grid.CentreX(cell), grid.CentreY(cell), z, 0.0f);
}

float Animus::Curriculum::PointNavEncounter::CostAt(EnvNav const& nav, float x, float y)
{
    if (!nav.Grid || nav.Cost.size() != nav.Grid->Cells())
        return Vision::TRAV_UNREACHABLE;
    uint32 const cell = nav.Grid->CellOf(x, y);
    return cell == Vision::TRAV_NO_CELL ? Vision::TRAV_UNREACHABLE : nav.Cost[cell];
}

std::mt19937& Animus::Curriculum::PointNavEncounter::Rng(Env const& env) const
{
    return _rngs[env.Index];
}

float Animus::Curriculum::PointNavEncounter::Uniform(Env const& env) const
{
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(Rng(env));
}

bool Animus::Curriculum::PointNavEncounter::DrawPair(Env& env, Map* map, Player* bot, EnvNav& nav)
{
    CurriculumTuning::PointNavTuning const& tuning = _scenario.Tuning().PointNav;
    ArenaDefinition const& arena = _scenario.Arena(env);
    Vision::TravGrid const& grid = *nav.Grid;
    if (grid.Reachable.size() < 2)
        return false;
    auto const pick = [&](std::size_t count) { return std::min(count - 1, std::size_t(Uniform(env) * float(count))); };

    // The rung's bins: the detour and length edges the tuning lists, and how many of each the rung opens.
    std::vector<float> detours = ParseList(tuning.DetourBins);
    std::vector<float> lengths = ParseList(tuning.LengthBins);
    if (detours.size() < 2)
        detours = { 1.0f, 4.0f };
    if (lengths.size() < 2)
        lengths = { 5.0f, 400.0f };
    OpenBins const open = BinsOf(nav.Rung);
    uint32 const detourBins = std::min<uint32>(open.Detour, uint32(detours.size()) - 1);
    uint32 const lengthBins = std::min<uint32>(open.Length, uint32(lengths.size()) - 1);
    // Training draws the bin by where trips fail (PointNav.BinWeighting); an evaluation, and a seeded trip, uniformly
    // as ever, from the same two draws of its generator.
    bool const training = !env.Evaluating && env.EpisodeSeedIndex == Animus::NO_EPISODE_SEED;
    nav.LengthBins = uint32(lengths.size()) - 1;
    nav.BinCounted = training;
    if (training && tuning.BinWeighting)
        DrawBin(env, detourBins, lengthBins, nav.LengthBins, nav);
    else
    {
        nav.DetourBin = uint32(pick(detourBins));
        nav.LengthBin = uint32(pick(lengthBins));
    }
    float const detourLow = detours[nav.DetourBin];
    float const detourHigh = detours[nav.DetourBin + 1];
    float const lengthLow = lengths[nav.LengthBin];
    float const lengthHigh = lengths[nav.LengthBin + 1];

    // A failure replayed (training only): the seat starts where a seat of this arena failed, with that goal.
    if (!env.Evaluating && env.EpisodeSeedIndex == Animus::NO_EPISODE_SEED && tuning.ReplayShare > 0
        && Uniform(env) * 100.0f < float(tuning.ReplayShare))
    {
        std::lock_guard lock(_poolLock);
        auto const found = _pools.find(_scenario.Data(env).Arena);
        if (found != _pools.end() && !found->second.Ring.empty())
        {
            Failure const& failure = found->second.Ring[pick(found->second.Ring.size())];
            uint32 const start = grid.CellOf(failure.Where.GetPositionX(), failure.Where.GetPositionY());
            if (failure.GoalCell < grid.Cells() && start != Vision::TRAV_NO_CELL)
            {
                Vision::CostToGo(grid, failure.GoalCell, tuning.DropHurtYards, nav.Cost);
                if (nav.Cost[start] < Vision::TRAV_UNREACHABLE)
                {
                    nav.GoalCell = failure.GoalCell;
                    nav.GoalAtBed = failure.GoalAtBed;
                    nav.StartCell = start;
                    // The failure's bake cell at its standing height, not the point itself (2026-10-17): a seat that
                    // timed out mid-fall through a hole in the collision filed the hole, and every replay of it fell
                    // through again, died in the void and rose there to fall once more. The cell's centre is ground
                    // the bake vouches for.
                    nav.Start = CellPoint(grid, start, false);
                    nav.Replayed = true;
                    nav.BinCounted = false;     // its pair is not the drawn bin's
                }
            }
        }
    }

    if (!nav.Replayed)
    {
        // The goal first, one Dijkstra, then a start among the cells whose route length and detour fall in the bins;
        // the goal drawn again a few times when none does, then the nearest fit. SightOnly: the start's eye sees the
        // goal (SightDraw::InSight, the camera's own ray).
        Vision::MapVisionWorld const world(map, bot->GetPhaseMask());
        Movement::Body const shape = Movement::ShapeOf(bot);
        float const eyeRise = Vision::PIVOT_SHARE * shape.Height;
        // The cells this body is drawn on: the bake's Free and StepUp cells (Reachable) less those whose liquid it
        // would swim -- the controller swims from SWIM_ENTER of the body's height over the bed (PlayerController),
        // the bake calls liquid under TRAV_WATER_DEPTH wadeable for everyone (one bake a map), so a gnome swims
        // where a tauren wades: a start it could only swim from, or a goal it could only tread water at, is not
        // drawn for it. Liquid[] of a dry cell is TRAV_NO_HEIGHT, far under any floor.
        float const swimDepth = Movement::SWIM_ENTER * shape.Height;
        std::vector<uint32> drawable;
        drawable.reserve(grid.Reachable.size());
        for (uint32 cell : grid.Reachable)
            if (grid.Liquid[cell] - grid.Ground[cell] < swimDepth)
                drawable.push_back(cell);
        if (drawable.size() < 2)
            return false;
        std::vector<uint32> fits;
        uint32 bestCell = Vision::TRAV_NO_CELL;
        float bestMiss = std::numeric_limits<float>::infinity();
        float const detourMid = 0.5f * (detourLow + detourHigh);
        float const lengthMid = 0.5f * (lengthLow + lengthHigh);
        for (uint32 attempt = 0; attempt < GOAL_TRIES && nav.StartCell == Vision::TRAV_NO_CELL; ++attempt)
        {
            uint32 const goal = drawable[pick(drawable.size())];
            Vision::CostToGo(grid, goal, tuning.DropHurtYards, nav.Cost);
            Position const goalPoint = CellPoint(grid, goal, false);
            fits.clear();
            for (uint32 cell : drawable)
            {
                float const route = nav.Cost[cell];
                if (cell == goal || route >= Vision::TRAV_UNREACHABLE || route <= 0.0f)
                    continue;
                Position const point = CellPoint(grid, cell, false);
                float const straight = std::max(0.5f, point.GetExactDist(&goalPoint));
                float const detour = route / straight;
                if (route >= lengthLow && route < lengthHigh && detour >= detourLow && detour < detourHigh)
                    fits.push_back(cell);
                // The nearest fit, by how far the pair misses the bins' middles (in log yards and log detour).
                float const miss = std::fabs(std::log(route / lengthMid)) + std::fabs(std::log(detour / detourMid));
                if (miss < bestMiss)
                {
                    bestMiss = miss;
                    bestCell = cell;
                    nav.GoalCell = goal;
                }
            }
            if (fits.empty())
                continue;
            // Uniform over the fits; a SightOnly arena tests them in a uniform order for the one in sight.
            std::shuffle(fits.begin(), fits.end(), Rng(env));
            uint32 const tests = arena.SightOnly ? std::min<uint32>(SIGHT_TESTS, uint32(fits.size())) : 1;
            for (uint32 index = 0; index < tests; ++index)
            {
                uint32 const cell = fits[index];
                Position const point = CellPoint(grid, cell, false);
                if (arena.SightOnly && !SightDraw::InSight({ point.GetPositionX(), point.GetPositionY(),
                    point.GetPositionZ() + eyeRise }, { goalPoint.GetPositionX(), goalPoint.GetPositionY(),
                    goalPoint.GetPositionZ() + 1.0f }, Vision::OBJECTIVE_RADIUS, world))
                    continue;
                nav.StartCell = cell;
                nav.GoalCell = goal;
                break;
            }
        }
        if (nav.StartCell == Vision::TRAV_NO_CELL)
        {
            if (bestCell == Vision::TRAV_NO_CELL)
                return false;
            // The nearest fit's goal may be an earlier try's: its cost map again.
            Vision::CostToGo(grid, nav.GoalCell, tuning.DropHurtYards, nav.Cost);
            nav.StartCell = bestCell;
        }
        nav.Start = CellPoint(grid, nav.StartCell, false);
        // A goal on water: at the surface, or at the bed BedShare percent of the time (nav5's dives).
        nav.GoalAtBed = grid.ClassOf(nav.GoalCell) == Vision::TravClass::Water
            && Uniform(env) * 100.0f < float(tuning.BedShare);
    }

    nav.Goal = CellPoint(grid, nav.GoalCell, nav.GoalAtBed);
    nav.CStart = nav.Cost[nav.StartCell];
    nav.Straight = nav.Start.GetExactDist(&nav.Goal);
    return nav.CStart < Vision::TRAV_UNREACHABLE;
}

bool Animus::Curriculum::PointNavEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;
    EnvNav& nav = _envs[env.Index];
    bool const labelsOnly = nav.LabelsOnly;
    nav = EnvNav();
    nav.LabelsOnly = labelsOnly;
    nav.MapId = map->GetId();
    nav.Rung = std::min<uint32>(SightDraw::Rung(_scenario.ShapingScale()), RUNGS - 1);
    // The dungeon's own doors and levers out of the way: the bake knows the static walls alone.
    ObjectPool::ClearOwn(map);
    nav.Grid = EnsureGrid(env, map, bot);
    if (!nav.Grid)
        return false;
    if (nav.LabelsOnly)
    {
        // Nothing placed: the leader's position is the goal (Update), the party's start the follow's.
        nav.Start = _scenario.SpawnPointFor(env);
        nav.StartCell = nav.Grid->CellOf(nav.Start.GetPositionX(), nav.Start.GetPositionY());
        return true;
    }
    if (!DrawPair(env, map, bot, nav))
    {
        LOG_ERROR("module.animus", "{}: arena {} on map {} has no (start, goal) pair to draw", _scenario.Name(),
            _scenario.Arena(env).Name, map->GetId());
        return false;
    }

    // The trip's clock (PointNav.TimeFactor): TimeFactor x the best route at run speed + TimeSlackSeconds, at least
    // TimeMinSeconds and the arena's own EpisodeSeconds (so no trip has less time than the fixed clock gave it, and
    // arrived_base_limit compares exactly), at most the arena's EpisodeSeconds x TimeCap (StageScenario sizes the
    // spec's longest episode by that cap); the arena's own length when off. The episode ends on it (EnvPool), the
    // timeout is judged on it (Reward) and the critic's episode-time column reads it (STATE_EPISODE_TIME).
    {
        CurriculumTuning::PointNavTuning const& tuning = _scenario.Tuning().PointNav;
        uint32 const arenaMs = _scenario.ArenaEpisodeMs(env);
        env.EpisodeLengthMs = arenaMs;
        if (tuning.TimeFactor > 0.0f && nav.CStart < Vision::TRAV_UNREACHABLE)
        {
            float const capSeconds = float(arenaMs) / 1000.0f * std::max(1.0f, tuning.TimeCap);
            float const bestSeconds = nav.CStart / RUN_YARDS_PER_SECOND;
            float const seconds = std::clamp(tuning.TimeFactor * bestSeconds + tuning.TimeSlackSeconds,
                std::min(std::max(tuning.TimeMinSeconds, float(arenaMs) / 1000.0f), capSeconds), capSeconds);
            env.EpisodeLengthMs = std::max<uint32>(1000, uint32(seconds * 1000.0f));
        }
    }

    // The seat at its start, facing a random way; the start is the episode's spawn, so the critic's origin and the
    // rise after a death are both there.
    float const facing = Uniform(env) * TWO_PI;
    Position const start(nav.Start.GetPositionX(), nav.Start.GetPositionY(), nav.Start.GetPositionZ(), facing);
    nav.Start = start;
    BotFactory::TeleportWithinMap(bot, start);
    EnvState& data = _scenario.Data(env);
    data.EpisodeSpawn = start;
    data.HasEpisodeSpawn = true;
    nav.Placed = true;
    if (_scenario.Arena(env).Revisit)
        nav.Seats[0].Stood.assign(nav.Grid->Cells(), 0);
    return true;
}

void Animus::Curriculum::PointNavEncounter::Update(Env& env)
{
    EnvNav& nav = _envs[env.Index];
    if (!nav.Grid)
        return;
    CurriculumTuning::RespawnTuning const& respawn = _scenario.Tuning().Respawn;
    EnvState& data = _scenario.Data(env);
    if (nav.LabelsOnly)
    {
        // The goal is the leader as the server last had it (PartyFollowEncounter::LeaderReported); its cost map is
        // worked out again only when its cell moves, on this map thread, once for every follower.
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!_follow || !_follow->LeaderReported(env, x, y, z))
            return;
        uint32 const cell = nav.Grid->CellOf(x, y);
        nav.Goal.Relocate(x, y, z, 0.0f);
        if (cell != Vision::TRAV_NO_CELL && cell != nav.GoalCell)
        {
            nav.GoalCell = cell;
            Vision::CostToGo(*nav.Grid, cell, _scenario.Tuning().PointNav.DropHurtYards, nav.Cost);
            nav.CStart = nav.StartCell == Vision::TRAV_NO_CELL ? Vision::TRAV_UNREACHABLE : nav.Cost[nav.StartCell];
        }
        return;
    }
    if (!nav.Placed)
        return;

    // A death rises at the start point after the delay (EntranceRespawn); the episode goes on.
    for (uint32 seatIndex = 0; seatIndex < data.ActiveSeats && seatIndex < GROUP_SEATS; ++seatIndex)
    {
        Player* bot = _scenario.SeatBot(env, seatIndex);
        if (!bot || !bot->IsInWorld())
            continue;
        SeatNav& seat = nav.Seats[seatIndex];
        RespawnClock::Step const step = seat.Clock.Note(env.EpisodeElapsedMs, bot->IsAlive(), -1.0f,
            respawn.DelayMs, respawn.RejoinYards);
        if (step == RespawnClock::Step::Rise)
        {
            RiseAtEntrance(bot, data.Seats[seatIndex], nav.Start, env.EpisodeElapsedMs);
            seat.Clock.Risen(env.EpisodeElapsedMs);
            seat.HasLastPos = false;
        }
    }
}

bool Animus::Curriculum::PointNavEncounter::SelectTarget(Env const& env, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seat acts without a target. The party follow's labeller leaves the choice to it.
    if (_envs[env.Index].LabelsOnly)
        return false;
    target = nullptr;
    return true;
}

void Animus::Curriculum::PointNavEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    EnvNav const& nav = _envs[env.Index];
    // The point: the sim's knowledge of the goal, which the map block plans the route to on the seat's own map
    // (the route scalars are the policy's only word of it: no compass in a nav stage, map block revision 7); never
    // withheld; the camera flags nothing (there is no object). A labeller (nav6) names the leader's position as the
    // server last had it, once Update has one: the party follow's own View (run before this one) names no objective.
    if (nav.LabelsOnly && nav.GoalCell == Vision::TRAV_NO_CELL)
        return;
    view.HasObjective = nav.LabelsOnly || nav.Placed;
    view.Objective = nav.Goal;
    view.ObjectiveRadius = 0.0f;
    view.CompassWithheld = false;
}

void Animus::Curriculum::PointNavEncounter::NoteFailure(Env const& env, EnvNav const& nav, Position const& where)
{
    if (env.Evaluating || nav.GoalCell == Vision::TRAV_NO_CELL)
        return;
    uint32 const size = std::max<uint32>(1, _scenario.Tuning().PointNav.FailurePool);
    std::lock_guard lock(_poolLock);
    FailurePool& pool = _pools[_scenario.Data(env).Arena];
    Failure const failure{ where, nav.GoalCell, nav.GoalAtBed };
    if (pool.Ring.size() < size)
        pool.Ring.push_back(failure);
    else
    {
        pool.Ring[pool.Next % size] = failure;
        pool.Next = (pool.Next + 1) % size;
    }
}

void Animus::Curriculum::PointNavEncounter::DrawBin(Env const& env, uint32 detourBins, uint32 lengthBins,
    uint32 lengthCount, EnvNav& nav)
{
    CurriculumTuning::PointNavTuning const& tuning = _scenario.Tuning().PointNav;
    float const floor = std::max(0.0f, tuning.BinFloor);
    std::lock_guard lock(_poolLock);
    std::vector<float>& rates = _binRates[_scenario.Data(env).Arena];
    std::size_t const cells = std::size_t(lengthCount) * std::max<uint32>(detourBins, 1);
    if (rates.size() < cells)
        rates.resize(cells, 0.0f);
    // Each open bin's weight: (1 - its arrival rate) + the floor; a bin no trip has ended in reads rate 0.
    auto const weight = [&](uint32 detour, uint32 length)
    {
        float const rate = std::clamp(rates[std::size_t(detour) * lengthCount + length], 0.0f, 1.0f);
        return 1.0f - rate + floor;
    };
    float total = 0.0f;
    for (uint32 detour = 0; detour < detourBins; ++detour)
        for (uint32 length = 0; length < lengthBins; ++length)
            total += weight(detour, length);
    nav.DetourBin = 0;
    nav.LengthBin = 0;
    if (total <= 0.0f)
        return;
    float draw = Uniform(env) * total;
    for (uint32 detour = 0; detour < detourBins; ++detour)
        for (uint32 length = 0; length < lengthBins; ++length)
        {
            nav.DetourBin = detour;
            nav.LengthBin = length;
            draw -= weight(detour, length);
            if (draw < 0.0f)
                return;
        }
}

void Animus::Curriculum::PointNavEncounter::NoteBinOutcome(Env const& env, EnvNav& nav, bool arrived)
{
    if (!nav.BinCounted || nav.Replayed || env.Evaluating || !nav.LengthBins)
        return;
    nav.BinCounted = false;
    float const alpha = 1.0f / float(std::max<uint32>(1, _scenario.Tuning().PointNav.BinWindow));
    std::lock_guard lock(_poolLock);
    std::vector<float>& rates = _binRates[_scenario.Data(env).Arena];
    std::size_t const index = std::size_t(nav.DetourBin) * nav.LengthBins + nav.LengthBin;
    if (rates.size() <= index)
        rates.resize(index + 1, 0.0f);
    rates[index] += alpha * ((arrived ? 1.0f : 0.0f) - rates[index]);
}

void Animus::Curriculum::PointNavEncounter::NoteWaypoint(Env const& env, uint32 seatIndex, EnvNav& nav,
    SeatNav& seat) const
{
    SeatState& state = _scenario.Data(env).Seats[seatIndex];
    GoalHold const& held = state.Holds[0];
    if (!state.CellGoals || !GoalBlock::IsCellGoal(held.Goal) || !held.CellValid || held.Ticket == seat.LastTicket)
        return;
    seat.LastTicket = held.Ticket;
    CropPose const& pose = state.ChoicePose;
    if (!pose.Valid)
        return;
    // The crop the choice was made from, as LatchCell reads it back, and the least route distance over its
    // choosable blocks against the chosen block's.
    thread_local std::vector<uint8> crop(Vision::CROP_BYTES);
    state.Map.Crop(pose.X, pose.Y, pose.Z, pose.Yaw, crop.data());
    float best = Vision::TRAV_UNREACHABLE;
    for (uint32 r = 0; r < CellGrid::GRID; ++r)
        for (uint32 c = 0; c < CellGrid::GRID; ++c)
        {
            if (!CellGrid::Choosable(crop.data(), r, c))
                continue;
            Position const point = CellGrid::WorldPoint(crop.data(), pose, r, c);
            best = std::min(best, CostAt(nav, point.GetPositionX(), point.GetPositionY()));
        }
    float const chosen = CostAt(nav, held.Place.GetPositionX(), held.Place.GetPositionY());
    if (best >= Vision::TRAV_UNREACHABLE || chosen >= Vision::TRAV_UNREACHABLE)
        return;
    seat.RegretSum += std::max(0.0f, chosen - best);
    ++seat.Waypoints;
}

void Animus::Curriculum::PointNavEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    EnvNav& nav = _envs[env.Index];
    if (nav.LabelsOnly || !nav.Placed || !bot || seatIndex >= GROUP_SEATS)
        return;
    CurriculumTuning::PointNavTuning const& tuning = _scenario.Tuning().PointNav;
    CurriculumTuning::ControlsTuning const& controls = _scenario.Tuning().Controls;
    SeatNav& seat = nav.Seats[seatIndex];
    SeatState& state = _scenario.Data(env).Seats[seatIndex];
    uint32 const stepMs = _scenario.StepMs(env);
    float const seconds = float(stepMs) / 1000.0f;
    ++seat.Decisions;

    // The clock, and a death (paid each time; the seat rises at the start, Update). Drowning is the death under
    // water the scenario noted (SeatState::Drowned), paid once.
    CombatTally& tally = state.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }
    if (state.Drowned && !seat.DrownedPaid)
    {
        seat.DrownedPaid = true;
        ledger.Add(RewardTerm::Drowned, -tuning.Drowned);
        NoteFailure(env, nav, bot->GetPosition());
    }
    // Falls: what the server's own HandleFall took since the last reward, as a share of maximum health
    // (PlayerLink's tally, summed into SeatState::FallDamage by TrackController).
    float const fallDelta = std::max(0.0f, state.FallDamage - seat.LastFallDamage);
    seat.LastFallDamage = state.FallDamage;
    if (fallDelta > 0.0f)
    {
        seat.FallShare += fallDelta;
        ledger.Add(RewardTerm::FallHurt, -tuning.FallHurt * fallDelta);
        NoteFailure(env, nav, bot->GetPosition());
    }
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (!seat.Reached && timeIsUp)
    {
        tally.TimedOut = true;
        if (!nav.TimedOutNoted)
        {
            nav.TimedOutNoted = true;
            NoteFailure(env, nav, bot->GetPosition());
            NoteBinOutcome(env, nav, false);
        }
    }
    if (seat.Reached)
        return;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost);
    if (!bot->IsAlive() || seat.Clock.Out)
        return;

    // Where the body is: the controller's own (a client's), not the server's copy. The server's copy moves only with
    // the client's reports -- every 0.1 rad of turning, but every 500 ms on a straight run -- so a straight runner
    // read 0 and 3.5 yd on alternate decisions and was charged as pinned on the 0s, while a spinning seat was read
    // exactly (2026-10-14: nav1's seats learned to spin and never ran forward).
    Movement::BodyState const& body = state.Mover.Body;
    bool const own = state.Mover.Started();
    float const bodyX = own ? body.X : bot->GetPositionX();
    float const bodyY = own ? body.Y : bot->GetPositionY();
    float const bodyZ = own ? body.Z : bot->GetPositionZ();

    // How far the body went since the last decision.
    float moved = 0.0f;
    if (seat.HasLastPos)
    {
        float const dx = bodyX - seat.LastX;
        float const dy = bodyY - seat.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
        seat.Travelled += moved;
    }
    bool const firstLook = !seat.HasLastPos;
    seat.LastX = bodyX;
    seat.LastY = bodyY;
    seat.HasLastPos = true;

    // Wall and Stuck at their fixed price (as M1), and a pin PinSeconds long into the failure pool, once per pin.
    uint32 const stuckMs = state.StuckMs - std::min(state.StuckMs, seat.LastStuckMs);
    uint32 const wallMs = state.WallMs - std::min(state.WallMs, seat.LastWallMs);
    seat.LastStuckMs = state.StuckMs;
    seat.LastWallMs = state.WallMs;
    seat.StuckMs += stuckMs;
    seat.WallMs += wallMs;
    if (stuckMs)
        ledger.AddFixed(RewardTerm::Stuck, -controls.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = state.Controls.Held;
        // Back with or without a strafe moves at the back speed (WishOf), so that is what was asked.
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * seconds;
        float const charge = Standing::WallCharge(float(wallMs) / 1000.0f, moved, asked, controls.Wall,
            controls.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
    }
    // The body's turning, priced per radian it actually turned since the last decision, either way (PointNav.Turning;
    // full price from the first step, off the cost ladder): the look head turns the camera for free, the body's turn
    // is what costs. Per radian rather than per second held (heading.CONTRACT.md sec 3): the policy names a heading
    // and the controller eases to it, so a turn costs what it swept, and holding a heading costs nothing.
    float const turned = std::max(0.0f, state.TurnedRadians - seat.LastTurned);
    seat.LastTurned = state.TurnedRadians;
    seat.TurnRadians += turned;
    if (turned > 0.0f && tuning.Turning > 0.0f)
        ledger.AddFixed(RewardTerm::Turning, -tuning.Turning * turned);
    if (state.StuckRunMs >= tuning.PinSeconds * 1000)
    {
        if (!seat.PinNoted)
        {
            seat.PinNoted = true;
            NoteFailure(env, nav, bot->GetPosition());
        }
    }
    else
        seat.PinNoted = false;

    // The route distance where the body stands: Lost while it exceeds the start's by LostYards (off the bake is
    // lost too); Revisit on stepping onto a bake cell stood on before, in the arenas that ask -- a step, not a
    // dwell: standing still on one's own cell costs nothing (the plan's standing-still-pays-0).
    seat.CBody = CostAt(nav, bodyX, bodyY);
    if (seat.CBody > nav.CStart + tuning.LostYards)
    {
        seat.LostMs += stepMs;
        ledger.Add(RewardTerm::Lost, -tuning.Lost * seconds);
    }
    uint32 const cell = nav.Grid->CellOf(bodyX, bodyY);
    if (!seat.Stood.empty() && cell != Vision::TRAV_NO_CELL)
    {
        if (cell != seat.LastCell && seat.Stood[cell])
        {
            seat.RevisitMs += stepMs;
            ledger.Add(RewardTerm::Revisit, -tuning.Revisit * seconds);
        }
        seat.Stood[cell] = 1;
    }
    seat.LastCell = cell;
    NoteWaypoint(env, seatIndex, nav, seat);

    // Arriving: stopped within ArriveYards of the goal (2D) and ArriveRise of its height. The first look is at
    // rest and is no stop.
    float const distance = std::hypot(bodyX - nav.Goal.GetPositionX(), bodyY - nav.Goal.GetPositionY());
    bool const inside = distance <= tuning.ArriveYards
        && std::fabs(bodyZ - nav.Goal.GetPositionZ()) <= tuning.ArriveRise;
    bool const stopped = Standing::Stopped(bot->GetUnitMovementFlags(), moved, tuning.StopMoved);
    if (firstLook || !(stopped && inside))
        return;
    seat.Reached = true;
    seat.ArriveMs = env.EpisodeElapsedMs;
    seat.StopError = distance;
    NoteBinOutcome(env, nav, true);
    // Paid by PointNav.ArriveWeight: 1 the route over the yards walked (the spl column); 2 the route at run speed over
    // the seconds taken (1 over the time_ratio column, at most 1).
    float share = 1.0f;
    if (tuning.ArriveWeight == 1 && nav.CStart < Vision::TRAV_UNREACHABLE)
        share = nav.CStart / std::max(nav.CStart, std::max(seat.Travelled, 0.01f));
    else if (tuning.ArriveWeight == 2 && nav.CStart < Vision::TRAV_UNREACHABLE)
        share = std::min(1.0f, std::max(0.25f, nav.CStart / RUN_YARDS_PER_SECOND)
            / std::max(0.25f, float(seat.ArriveMs) / 1000.0f));
    ledger.Add(RewardTerm::Arrive, tuning.Arrive * share);
}

bool Animus::Curriculum::PointNavEncounter::IsTerminal(Env const& env) const
{
    EnvNav const& nav = _envs[env.Index];
    if (nav.LabelsOnly || !nav.Placed)
        return false;
    uint32 const seats = std::min<uint32>(_scenario.Data(env).ActiveSeats, uint32(nav.Seats.size()));
    for (uint32 seat = 0; seat < seats; ++seat)
        if (!nav.Seats[seat].Reached)
            return false;
    return seats > 0;
}

void Animus::Curriculum::PointNavEncounter::WriteState(Env const& env, float* state) const
{
    // The critic sees the goal in the first enemy slot (the stage has no enemies): where it is, relative to the
    // spawn as every position in the state is; the route distance from the body in its health column and from the
    // start in its max-health column, each over 500 yd.
    EnvNav const& nav = _envs[env.Index];
    if (!nav.Grid || (!nav.Placed && !nav.LabelsOnly) || nav.GoalCell == Vision::TRAV_NO_CELL)
        return;
    Position const& origin = _scenario.SpawnPointFor(env);
    float* slot = state + StageScenario::STATE_GLOBAL_COUNT + MAX_SEATS * StageScenario::STATE_SEAT_FEATURES;
    slot[StageScenario::STATE_ENEMY_PRESENT] = 1.0f;
    slot[StageScenario::STATE_ENEMY_ALIVE] = nav.Seats[0].Reached ? 0.0f : 1.0f;
    slot[StageScenario::STATE_ENEMY_X] = Encoding::RelativePosition(nav.Goal.GetPositionX(), origin.GetPositionX());
    slot[StageScenario::STATE_ENEMY_Y] = Encoding::RelativePosition(nav.Goal.GetPositionY(), origin.GetPositionY());
    float const body = nav.Seats[0].CBody;
    slot[StageScenario::STATE_ENEMY_HEALTH] = body >= Vision::TRAV_UNREACHABLE ? 1.0f
        : std::min(1.0f, body / STATE_YARDS_SCALE);
    slot[StageScenario::STATE_ENEMY_MAX_HEALTH] = nav.CStart >= Vision::TRAV_UNREACHABLE ? 1.0f
        : std::min(1.0f, nav.CStart / STATE_YARDS_SCALE);
}

void Animus::Curriculum::PointNavEncounter::WriteLabels(Env const& env, uint32 seatIndex, uint8 const* /*maps*/,
    uint32 /*mapBytes*/, uint8* out)
{
    namespace Vi = Animus::Vision;
    std::memset(out, 0, LABEL_BYTES);
    uint16_t* fineCost = reinterpret_cast<uint16_t*>(out + Vi::TRAV_LABEL_FINE_COST);
    uint16_t* coarseCost = reinterpret_cast<uint16_t*>(out + Vi::TRAV_LABEL_COARSE_COST);
    float* scalars = reinterpret_cast<float*>(out + Vi::TRAV_LABEL_SCALARS);
    uint16_t const unreachable = Vi::FloatToHalf(Vi::TRAV_UNREACHABLE);
    std::fill(fineCost, fineCost + Vi::TRAV_LABEL_CELLS, unreachable);
    std::fill(coarseCost, coarseCost + Vi::TRAV_LABEL_CELLS, unreachable);
    // The four route costs unreachable, the goal's yards 0 (the memset), until a goal and a body say otherwise.
    scalars[Vi::TRAV_LABEL_C_BODY] = Vi::TRAV_UNREACHABLE;
    scalars[Vi::TRAV_LABEL_C_START] = Vi::TRAV_UNREACHABLE;
    scalars[Vi::TRAV_LABEL_MAP_C_BODY] = Vi::TRAV_UNREACHABLE;
    scalars[Vi::TRAV_LABEL_MAP_C_LAST] = Vi::TRAV_UNREACHABLE;

    EnvNav& nav = _envs[env.Index];
    if (!nav.Grid || seatIndex >= MAX_SEATS)
        return;
    Player const* bot = _scenario.SeatBotInWorld(env, seatIndex);
    if (!bot)
        return;
    // The pose the map crop was taken at (the same observation), else the body's.
    SeatState const& state = _scenario.Data(env).Seats[seatIndex];
    CropPose const& pose = state.Crop;
    float const x = pose.Valid ? pose.X : bot->GetPositionX();
    float const y = pose.Valid ? pose.Y : bot->GetPositionY();
    float const yaw = pose.Valid ? pose.Yaw : bot->GetOrientation();
    bool const haveGoal = nav.GoalCell != Vi::TRAV_NO_CELL && nav.Cost.size() == nav.Grid->Cells();
    static std::vector<float> const noCost;
    std::vector<float> const& cost = haveGoal ? nav.Cost : noCost;
    Vi::SampleLabels(*nav.Grid, cost, x, y, yaw, Vi::TRAV_CELL, out + Vi::TRAV_LABEL_FINE_CLASSES, fineCost);
    Vi::SampleLabels(*nav.Grid, cost, x, y, yaw, Vi::TRAV_LABEL_COARSE_CELL, out + Vi::TRAV_LABEL_COARSE_CLASSES,
        coarseCost);
    if (!haveGoal)
        return;
    scalars[Vi::TRAV_LABEL_C_BODY] = CostAt(nav, x, y);
    scalars[Vi::TRAV_LABEL_C_START] = nav.CStart;
    // The goal in the body frame: forward along the facing, right along (sin yaw, -cos yaw): a label, no observation.
    float const cosYaw = std::cos(yaw);
    float const sinYaw = std::sin(yaw);
    float const dx = nav.Goal.GetPositionX() - x;
    float const dy = nav.Goal.GetPositionY() - y;
    float const goalForward = dx * cosYaw + dy * sinYaw;
    float const goalRight = dx * sinYaw - dy * cosYaw;
    scalars[Vi::TRAV_LABEL_GOAL_FORWARD] = goalForward;
    scalars[Vi::TRAV_LABEL_GOAL_RIGHT] = goalRight;

    // The route on the seat's own map (protocol 33): the plan the map block made this observation on the map's own
    // 1-yd cells (MapBlock::Observe, Vision::PlanOnMap: the policy's route waypoints come from the same plan), at
    // the body and at the body's last decision's position -- both on the map as it is now, so their difference is
    // movement alone. The first decision's last is the body's own; a last position more than LABEL_STEP_MAX off (a
    // rise after a death, a teleport), past the plan's window, or not the pose the block planned at reads
    // unreachable, which the learner pays nothing for.
    if (seatIndex >= GROUP_SEATS)
        return;
    SeatNav& seat = nav.Seats[seatIndex];
    if (pose.Valid && pose.RouteValid)
    {
        bool lastNear = false;
        if (seat.HasLabelPos && pose.RouteLastValid && pose.LastX == seat.LabelX && pose.LastY == seat.LabelY)
        {
            float const lx = seat.LabelX - x;
            float const ly = seat.LabelY - y;
            lastNear = lx * lx + ly * ly <= LABEL_STEP_MAX * LABEL_STEP_MAX;
        }
        float const atBody = pose.RouteBody;
        scalars[Vi::TRAV_LABEL_MAP_C_BODY] = atBody;
        scalars[Vi::TRAV_LABEL_MAP_C_LAST] = !seat.HasLabelPos ? atBody
            : lastNear ? pose.RouteLast : Vi::TRAV_UNREACHABLE;
        if (!seat.HasLabelPos)
            seat.MapRouteStart = atBody;
        seat.MapRouteLast = atBody;
        ++seat.RouteDecisions;
        seat.GlobalDecisions += pose.RouteGlobal ? 1 : 0;
        seat.GlobalReplans += pose.GlobalReplanned ? 1 : 0;
        seat.GlobalContradictions += pose.GlobalContradiction ? 1 : 0;
    }
    seat.HasLabelPos = true;
    seat.LabelX = x;
    seat.LabelY = y;
}
