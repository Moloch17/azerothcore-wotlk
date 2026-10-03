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

#include "FieldRoute.h"
#include "LayeredField.h"
#include "MapDefines.h"
#include "ProbeBake.h"
#include <cmath>
#include <map>
#include <memory>
#include <queue>
#include <limits>
#include <unordered_map>
#include "StringFormat.h"

// Named, not anonymous: the build compiles the forge's files several to a unit, and LayeredField.cpp's own helpers
// (Column, Open, ...) would meet these there.
namespace FieldRouteSearch
{
    namespace Lhf = Animus::Curriculum::LayeredField;
    using namespace Animus::Curriculum::FieldRoute;

    /// How far up or down from a point a floor of its column still counts as the point's own (the seat's feet).
    constexpr float SNAP = 3.0f;
    /// Cells around the goal that reach it: a route ends where a seat is within reach of the place.
    constexpr float GOAL_REACH = 2.5f;
    /// The liquid types that burn (GridTerrainData's MAP_LIQUID_TYPE_MAGMA and _SLIME): a header too heavy for this
    /// file's needs.
    constexpr uint8 LIQUID_MAGMA = 0x04;
    constexpr uint8 LIQUID_SLIME = 0x08;
    /// A drop costs a little more than the yard it crosses, so a ramp beside a ledge is taken when there is one.
    constexpr float DROP_COST = 0.5f;

    /// The grids a search has read, by grid index, held while it runs.
    struct Grids
    {
        uint32 MapId = 0;
        std::map<std::pair<int32, int32>, std::shared_ptr<Lhf::Grid const>> Held;

        Lhf::Grid const* At(float x, float y)
        {
            std::pair<int32, int32> const key{ Animus::Curriculum::ProbeBake::GridIndex(x),
                Animus::Curriculum::ProbeBake::GridIndex(y) };
            auto found = Held.find(key);
            if (found == Held.end())
                found = Held.emplace(key, Lhf::Store::Find(MapId, key.first, key.second)).first;
            return found->second.get();
        }
    };

    /// A cell of the world's lattice: its column and row in yards from the origin, and which of its floors.
    struct Node
    {
        int32 X = 0;
        int32 Y = 0;
        uint32 Floor = 0;
    };

    uint64 Key(int32 x, int32 y, uint32 floor)
    {
        return (uint64(uint32(x) & 0xFFFFFu) << 44) | (uint64(uint32(y) & 0xFFFFFu) << 24) | uint64(floor & 0xFFFFFFu);
    }

    /// The floors a seat can stand on in the cell holding (x, y): enough room above, not burning, and either the
    /// navmesh's or open to the sky (the rest are the undersides of something solid, which the field also holds).
    struct Column
    {
        Lhf::Interval const* Begin = nullptr;
        Lhf::Interval const* End = nullptr;
    };

    Column ColumnAt(Grids& grids, float x, float y)
    {
        Lhf::Grid const* grid = grids.At(x, y);
        if (!grid || !grid->Side)
            return {};
        // The cell holding the point: MinX is the first cell's centre, half a cell in from the grid's edge. Rounding
        // half away from zero (lround) put x = 0 one cell before a grid's first, so the line along every grid's edge
        // read as rock: Ragefire's door is on one side of x = 0 and the dungeon on the other.
        int32 const column = int32(std::floor((x - grid->MinX) / grid->Cell + 0.5f));
        int32 const row = int32(std::floor((y - grid->MinY) / grid->Cell + 0.5f));
        if (column < 0 || row < 0 || column >= int32(grid->Side) || row >= int32(grid->Side))
            return {};
        uint32 const cell = uint32(row) * grid->Side + uint32(column);
        return { grid->Intervals.data() + grid->First[cell], grid->Intervals.data() + grid->First[cell + 1] };
    }

    bool Standable(Lhf::Interval const& floor)
    {
        uint8 const nav = floor.NavFlags();
        if (nav & (NAV_MAGMA | NAV_SLIME))
            return false;
        if (floor.HasLiquid() && (floor.LiquidFlags & (LIQUID_MAGMA | LIQUID_SLIME))
            && floor.Liquid() > floor.Floor())
            return false;
        return floor.Headroom() >= MIN_HEADROOM && (nav != 0 || floor.OpenAbove());
    }

    /// The standable floor of the column at (x, y) nearest `z`, within SNAP; false without one.
    bool FloorNear(Grids& grids, float x, float y, float z, uint32& index, float& height)
    {
        Column const column = ColumnAt(grids, x, y);
        float best = SNAP;
        bool found = false;
        uint32 i = 0;
        for (Lhf::Interval const* floor = column.Begin; floor != column.End; ++floor, ++i)
            if (Standable(*floor) && std::fabs(floor->Floor() - z) <= best)
            {
                best = std::fabs(floor->Floor() - z);
                index = i;
                height = floor->Floor();
                found = true;
            }
        return found;
    }

    struct Open
    {
        float F = 0.0f;
        uint64 Id = 0;
        bool operator<(Open const& other) const { return F > other.F; }     // a min-heap on F
    };

    struct Visit
    {
        Node At;
        float Z = 0.0f;
        float G = 0.0f;
        uint64 From = 0;
        bool Closed = false;
    };
}

bool Animus::Curriculum::FieldRoute::Covers(uint32 mapId, float x, float y)
{
    using namespace FieldRouteSearch;
    Grids grids;
    grids.MapId = mapId;
    return ColumnAt(grids, x, y).Begin != nullptr;
}

namespace FieldRouteSearch
{
    struct Diagnosis
    {
        bool Enabled = false;
        bool Start = false;
        std::string StartFloors;
        uint32 Expanded = 0;
        float Closest = 0.0f;
        Position At;
    };

    bool Search(uint32 mapId, Position const& from, Position const& to, std::vector<Position>& out,
        uint32 maxNodes, Diagnosis* diagnosis);
}

bool Animus::Curriculum::FieldRoute::Plan(uint32 mapId, Position const& from, Position const& to,
    std::vector<Position>& out, uint32 maxNodes)
{
    return FieldRouteSearch::Search(mapId, from, to, out, maxNodes, nullptr);
}

std::string Animus::Curriculum::FieldRoute::Report(uint32 mapId, Position const& from, Position const& to)
{
    std::vector<Position> out;
    FieldRouteSearch::Diagnosis diagnosis;
    bool const found = FieldRouteSearch::Search(mapId, from, to, out, 4000000, &diagnosis);
    float length = 0.0f;
    for (std::size_t i = 1; i < out.size(); ++i)
        length += out[i - 1].GetExactDist(&out[i]);
    return Acore::StringFormat("field store {}; start floors [{}]; start {}; {} cells expanded; {}; closest {:.1f} yd "
        "at ({:.1f} {:.1f} {:.1f})", diagnosis.Enabled ? "enabled" : "DISABLED", diagnosis.StartFloors,
        diagnosis.Start ? "taken" : "NONE", diagnosis.Expanded,
        found ? Acore::StringFormat("ARRIVED, {} yards walked, {:.0f} yd long", out.size(), length) : "NO WAY",
        diagnosis.Closest, diagnosis.At.GetPositionX(), diagnosis.At.GetPositionY(), diagnosis.At.GetPositionZ());
}

bool FieldRouteSearch::Search(uint32 mapId, Position const& from, Position const& to, std::vector<Position>& out,
    uint32 maxNodes, Diagnosis* diagnosis)
{
    out.clear();
    if (diagnosis)
        diagnosis->Enabled = Lhf::Store::Enabled();
    if (!Lhf::Store::Enabled())
        return false;
    Grids grids;
    grids.MapId = mapId;

    if (diagnosis)
    {
        Column const column = ColumnAt(grids, from.GetPositionX(), from.GetPositionY());
        for (Lhf::Interval const* floor = column.Begin; floor != column.End; ++floor)
            diagnosis->StartFloors += Acore::StringFormat("{}{:.1f}/{:.1f}/{}{}", diagnosis->StartFloors.empty()
                ? "" : " ", floor->Floor(), std::min(999.0f, floor->Headroom()), uint32(floor->NavFlags()),
                Standable(*floor) ? "" : "x");
    }
    uint32 startFloor = 0;
    float startZ = 0.0f;
    if (!FloorNear(grids, from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(), startFloor, startZ))
        return false;
    if (diagnosis)
    {
        diagnosis->Start = true;
        diagnosis->Closest = std::numeric_limits<float>::max();
    }

    // The lattice is whole yards from the origin; the field's cells are a yard too, so each lattice point reads its
    // own cell.
    Node const start{ int32(std::lround(from.GetPositionX())), int32(std::lround(from.GetPositionY())), startFloor };
    float const goalX = to.GetPositionX();
    float const goalY = to.GetPositionY();
    float const goalZ = to.GetPositionZ();
    auto const heuristic = [&](float x, float y, float z)
    {
        float const dx = x - goalX;
        float const dy = y - goalY;
        float const dz = std::max(0.0f, std::fabs(z - goalZ) - SNAP);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    std::unordered_map<uint64, Visit> visits;
    visits.reserve(1 << 16);
    std::priority_queue<Open> open;
    uint64 const startId = Key(start.X, start.Y, start.Floor);
    visits[startId] = Visit{ start, startZ, 0.0f, startId, false };
    open.push({ heuristic(float(start.X), float(start.Y), startZ), startId });

    static constexpr int32 STEPS[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 },
        { -1, -1 } };
    uint64 reached = 0;
    bool found = false;
    uint32 expanded = 0;
    while (!open.empty() && expanded < maxNodes)
    {
        Open const top = open.top();
        open.pop();
        Visit& current = visits[top.Id];
        if (current.Closed)
            continue;
        current.Closed = true;
        ++expanded;
        if (diagnosis)
        {
            diagnosis->Expanded = expanded;
            float const near = heuristic(float(current.At.X), float(current.At.Y), current.Z);
            if (near < diagnosis->Closest)
            {
                diagnosis->Closest = near;
                diagnosis->At.Relocate(float(current.At.X), float(current.At.Y), current.Z);
            }
        }

        float const cx = float(current.At.X);
        float const cy = float(current.At.Y);
        float const dxGoal = cx - goalX;
        float const dyGoal = cy - goalY;
        if (dxGoal * dxGoal + dyGoal * dyGoal <= GOAL_REACH * GOAL_REACH && std::fabs(current.Z - goalZ) <= SNAP)
        {
            reached = top.Id;
            found = true;
            break;
        }

        Node const at = current.At;
        float const z = current.Z;
        float const g = current.G;
        for (auto const& step : STEPS)
        {
            int32 const nx = at.X + step[0];
            int32 const ny = at.Y + step[1];
            Column const column = ColumnAt(grids, float(nx), float(ny));
            float const across = (step[0] && step[1]) ? 1.41421356f : 1.0f;
            uint32 index = 0;
            for (Lhf::Interval const* floor = column.Begin; floor != column.End; ++floor, ++index)
            {
                if (!Standable(*floor))
                    continue;
                float const rise = floor->Floor() - z;
                if (rise > MAX_CLIMB || rise < -MAX_DROP)
                    continue;
                float const cost = across + (rise < -MAX_CLIMB ? DROP_COST + (-rise - MAX_CLIMB) * 0.1f : 0.0f)
                    + std::fabs(rise) * 0.1f;
                uint64 const id = Key(nx, ny, index);
                auto [visit, inserted] = visits.try_emplace(id);
                if (visit->second.Closed)
                    continue;
                float const ng = g + cost;
                if (!inserted && visit->second.G <= ng)
                    continue;
                visit->second = Visit{ Node{ nx, ny, index }, floor->Floor(), ng, top.Id, false };
                open.push({ ng + heuristic(float(nx), float(ny), floor->Floor()), id });
            }
        }
    }

    if (!found)
        return false;

    std::vector<Position> backwards;
    for (uint64 id = reached;;)
    {
        Visit const& visit = visits[id];
        backwards.emplace_back(float(visit.At.X), float(visit.At.Y), visit.Z);
        if (visit.From == id)
            break;
        id = visit.From;
    }
    out.assign(backwards.rbegin(), backwards.rend());
    return true;
}
