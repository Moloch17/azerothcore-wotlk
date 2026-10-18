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

#include "GlobalPlanner.h"
#include "TravBake.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
    using namespace Animus::Vision;

    /// The eight neighbours as (row, column) = (y, x) offsets in the fine plan's order; direction d and 7 - d are
    /// opposite. The yards of each step.
    constexpr int32_t DR[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    constexpr int32_t DC[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    constexpr float DIST[8] = { 1.4142136f, 1.0f, 1.4142136f, 1.0f, 1.0f, 1.4142136f, 1.0f, 1.4142136f };
    MapCell const BLANK{};

    static_assert(MAP_TILE == 32, "the tile arithmetic below shifts by five");
    /// The tile holding 1-yd cell coordinate c (floor division: C++20's >> of a negative is arithmetic), and the
    /// cell's coordinate within it.
    [[nodiscard]] int32_t TileOf(int32_t c) { return c >> 5; }
    [[nodiscard]] int32_t LocalOf(int32_t c) { return c & 31; }

    [[nodiscard]] bool HasFloor(MapCell const& cell)
    {
        return cell.Floor[0] != NO_FLOOR || cell.Floor[1] != NO_FLOOR;
    }

    /// The floor layer of `cell` nearest z (-1 for none).
    [[nodiscard]] int32_t NearestLayer(MapCell const& cell, float z)
    {
        int32_t best = -1;
        float distance = 0.0f;
        for (int32_t layer = 0; layer < 2; ++layer)
        {
            if (cell.Floor[layer] == NO_FLOOR)
                continue;
            float const d = std::fabs(FloorHeight(cell.Floor[layer]) - z);
            if (best < 0 || d < distance)
            {
                best = layer;
                distance = d;
            }
        }
        return best;
    }

    [[nodiscard]] uint8_t WallBit(int32_t layer) { return layer ? MAP_WALL_HIGH : MAP_WALL_LOW; }
}

void Animus::Vision::GlobalField::Use(GlobalTiles const& tiles, GlobalBlocking const* blocking)
{
    _tiles = &tiles;
    _blocking = blocking;
    ++_call;
    if (!_call)
    {
        // The stamp wrapped: no tile may read as fetched this call.
        std::fill(_fetched.begin(), _fetched.end(), 0u);
        _call = 1;
    }
}

void Animus::Vision::GlobalField::Clear()
{
    _active = false;
    _settled = 0;
    _blockOf.clear();
    _tileCells.clear();
    _fetched.clear();
    _blockTile.clear();
    _value.clear();
    _factor.clear();
    _height.clear();
    _next.clear();
    _state.clear();
    _heap.clear();
}

std::size_t Animus::Vision::GlobalField::Bytes() const
{
    return _blockOf.capacity() * sizeof(int32_t) + _tileCells.capacity() * sizeof(MapCell const*)
        + _fetched.capacity() * sizeof(uint32_t) + _blockTile.capacity() * sizeof(int32_t)
        + _value.capacity() * sizeof(float) + _factor.capacity() * sizeof(float)
        + _height.capacity() * sizeof(int16_t) + _next.capacity() + _state.capacity()
        + _heap.capacity() * sizeof(Entry);
}

bool Animus::Vision::GlobalField::Contains(float x, float y) const
{
    return TileIndex(MapCellOf(x), MapCellOf(y)) >= 0;
}

int32_t Animus::Vision::GlobalField::TileIndex(int32_t cx, int32_t cy) const
{
    int32_t const tx = TileOf(cx) - _tx0;
    int32_t const ty = TileOf(cy) - _ty0;
    if (!_active || tx < 0 || ty < 0 || tx >= _tw || ty >= _th)
        return -1;
    return ty * _tw + tx;
}

Animus::Vision::MapCell const& Animus::Vision::GlobalField::CellAt(int32_t cx, int32_t cy)
{
    int32_t const index = TileIndex(cx, cy);
    if (index < 0)
        return BLANK;
    if (_fetched[std::size_t(index)] != _call)
    {
        _tileCells[std::size_t(index)] = _tiles ? _tiles->Tile(index % _tw + _tx0, index / _tw + _ty0) : nullptr;
        _fetched[std::size_t(index)] = _call;
    }
    MapCell const* cells = _tileCells[std::size_t(index)];
    if (!cells)
        return BLANK;
    int32_t const lx = LocalOf(cx);
    int32_t const ly = LocalOf(cy);
    return cells[std::size_t(ly * MAP_TILE + lx)];
}

int32_t Animus::Vision::GlobalField::Block(int32_t tileIndex)
{
    int32_t& block = _blockOf[std::size_t(tileIndex)];
    if (block >= 0)
        return block;
    block = int32_t(_blockTile.size());
    _blockTile.push_back(tileIndex);
    std::size_t const nodes = _blockTile.size() * std::size_t(BLOCK_NODES);
    _value.resize(nodes, PLAN_SENTINEL);
    _factor.resize(nodes, 0.0f);
    _height.resize(nodes, 0);
    _next.resize(nodes, 0);
    _state.resize(nodes, 0);
    return block;
}

int32_t Animus::Vision::GlobalField::NodeOf(int32_t cx, int32_t cy, int32_t slot)
{
    int32_t const index = TileIndex(cx, cy);
    if (index < 0)
        return -1;
    int32_t const lx = LocalOf(cx);
    int32_t const ly = LocalOf(cy);
    return Block(index) * BLOCK_NODES + slot * SLOT_NODES + ly * MAP_TILE + lx;
}

int32_t Animus::Vision::GlobalField::FindNode(int32_t cx, int32_t cy, int32_t slot) const
{
    int32_t const index = TileIndex(cx, cy);
    if (index < 0 || _blockOf[std::size_t(index)] < 0)
        return -1;
    int32_t const lx = LocalOf(cx);
    int32_t const ly = LocalOf(cy);
    return _blockOf[std::size_t(index)] * BLOCK_NODES + slot * SLOT_NODES + ly * MAP_TILE + lx;
}

void Animus::Vision::GlobalField::CellOfNode(int32_t node, int32_t& cx, int32_t& cy, int32_t& slot) const
{
    int32_t const tile = _blockTile[std::size_t(node / BLOCK_NODES)];
    int32_t const within = node % BLOCK_NODES;
    slot = within / SLOT_NODES;
    int32_t const local = within % SLOT_NODES;
    cx = (tile % _tw + _tx0) * MAP_TILE + local % MAP_TILE;
    cy = (tile / _tw + _ty0) * MAP_TILE + local / MAP_TILE;
}

bool Animus::Vision::GlobalField::WallAt(int32_t cx, int32_t cy, float z)
{
    if (TileIndex(cx, cy) < 0)
        return false;           // the domain's edge is no wall, as the window's is not
    MapCell const& cell = CellAt(cx, cy);
    int32_t const layer = NearestLayer(cell, z);
    if (layer < 0)
        return (cell.Flags & (MAP_WALL_LOW | MAP_WALL_HIGH)) != 0;
    if (std::fabs(FloorHeight(cell.Floor[layer]) - z) > PLAN_HEIGHT_BAND)
        return false;           // another storey: no wall at this height
    return (cell.Flags & WallBit(layer)) != 0;
}

void Animus::Vision::GlobalField::Touch(int32_t node, int32_t cx, int32_t cy, int32_t slot, float z)
{
    if (_state[std::size_t(node)] & NODE_TOUCHED)
        return;
    MapCell const cell = CellAt(cx, cy);
    bool const floor = HasFloor(cell) && cell.Floor[slot] != NO_FLOOR;
    uint8_t const wall = floor ? WallBit(slot) : uint8_t(MAP_WALL_LOW | MAP_WALL_HIGH);
    float factor = 0.0f;
    if ((cell.Flags & wall) || (_blocking && _blocking->Blocked(cx, cy, z)))
        factor = _pricing.Codes[uint8_t(MapCode::Wall)];
    else if (cell.Flags & MAP_DOOR)
        factor = _pricing.Codes[uint8_t(MapCode::Door)];
    else if (cell.Flags & MAP_HAZARD)
        factor = _pricing.Codes[uint8_t(MapCode::Hazard)];
    else if (floor)
        factor = _pricing.Codes[uint8_t(MapCode::Floor)];
    else
        factor = _unknownCost ? std::max(_floorFactor, _unknownCost->Factor(cx, cy, z))
            : _pricing.Codes[uint8_t(MapCode::Unknown)];
    // The configuration space at this node's height: a hole the body never stood in closed, a cell by a wall dear.
    if (factor < PLAN_BLOCKED)
    {
        bool w[8];
        bool any = false;
        for (int32_t dir = 0; dir < 8; ++dir)
        {
            w[dir] = WallAt(cx + DC[dir], cy + DR[dir], z);
            any = any || w[dir];
        }
        bool const closed = (w[1] && w[6]) || (w[3] && w[4]) || (w[0] && w[7]) || (w[2] && w[5]);
        if (closed && !(cell.Flags & MAP_VISITED))
            factor = PLAN_BLOCKED;
        else if (any && factor < _pricing.Pinched)
        {
            // A 2-yd passage: a wall within two cells on both sides along an axis or a diagonal (the directions
            // d and 7 - d are opposite).
            bool pinched = false;
            for (int32_t dir = 0; dir < 4 && !pinched; ++dir)
            {
                int32_t const back = 7 - dir;
                bool const one = w[dir] || WallAt(cx + 2 * DC[dir], cy + 2 * DR[dir], z);
                bool const other = w[back] || WallAt(cx + 2 * DC[back], cy + 2 * DR[back], z);
                pinched = one && other;
            }
            factor = std::max(factor, pinched ? _pricing.Pinched : _pricing.Inflated);
        }
    }
    _factor[std::size_t(node)] = factor;
    _height[std::size_t(node)] = QuantiseFloor(floor ? FloorHeight(cell.Floor[slot]) : z);
    _state[std::size_t(node)] |= uint8_t(NODE_TOUCHED | (floor ? NODE_FLOOR : 0));
}

void Animus::Vision::GlobalField::Begin(float gx, float gy, float gz, float bx, float by)
{
    Clear();
    // The domain: the kept tiles' span with the body's and the goal's tiles, a margin round it, capped round the
    // body-goal box.
    int32_t const gtx = TileOf(MapCellOf(gx));
    int32_t const gty = TileOf(MapCellOf(gy));
    int32_t const btx = TileOf(MapCellOf(bx));
    int32_t const bty = TileOf(MapCellOf(by));
    int32_t lowX = std::min(gtx, btx);
    int32_t lowY = std::min(gty, bty);
    int32_t highX = std::max(gtx, btx);
    int32_t highY = std::max(gty, bty);
    int32_t kx0 = 0;
    int32_t ky0 = 0;
    int32_t kx1 = 0;
    int32_t ky1 = 0;
    int32_t x0 = lowX;
    int32_t y0 = lowY;
    int32_t x1 = highX;
    int32_t y1 = highY;
    if (_tiles && _tiles->Bounds(kx0, ky0, kx1, ky1))
    {
        x0 = std::min(x0, kx0);
        y0 = std::min(y0, ky0);
        x1 = std::max(x1, kx1);
        y1 = std::max(y1, ky1);
    }
    x0 = std::max(x0 - GLOBAL_MARGIN_TILES, lowX - GLOBAL_SPAN_TILES);
    y0 = std::max(y0 - GLOBAL_MARGIN_TILES, lowY - GLOBAL_SPAN_TILES);
    x1 = std::min(x1 + GLOBAL_MARGIN_TILES, highX + GLOBAL_SPAN_TILES);
    y1 = std::min(y1 + GLOBAL_MARGIN_TILES, highY + GLOBAL_SPAN_TILES);
    _floorFactor = std::min(_pricing.Codes[uint8_t(MapCode::Floor)], _pricing.Codes[uint8_t(MapCode::Door)]);
    _tx0 = x0;
    _ty0 = y0;
    _tw = x1 - x0 + 1;
    _th = y1 - y0 + 1;
    std::size_t const tiles = std::size_t(_tw) * std::size_t(_th);
    _blockOf.assign(tiles, -1);
    _tileCells.assign(tiles, nullptr);
    _fetched.assign(tiles, 0u);
    _active = true;

    // The goal's node: its cell's floor layer nearest its height (any distance: the goal is where it is), else the
    // floorless cell's at the goal's height.
    int32_t const cx = MapCellOf(gx);
    int32_t const cy = MapCellOf(gy);
    MapCell const& cell = CellAt(cx, cy);
    int32_t const layer = NearestLayer(cell, gz);
    int32_t const slot = std::max(0, layer);
    int32_t const node = NodeOf(cx, cy, slot);
    Touch(node, cx, cy, slot, gz);
    _value[std::size_t(node)] = 0.0f;
    _targetX = MapCellOf(bx);
    _targetY = MapCellOf(by);
    _heap.push_back({ Heuristic(cx, cy), 0.0f, node });
}

float Animus::Vision::GlobalField::Heuristic(int32_t cx, int32_t cy) const
{
    // The octile distance to the target cell at the cheapest factor a step can have (1: floor), a lower bound of
    // every route's cost there -- consistent, so a settled node's value is exact whatever the target.
    int32_t const dx = std::abs(cx - _targetX);
    int32_t const dy = std::abs(cy - _targetY);
    int32_t const low = std::min(dx, dy);
    return (float(std::max(dx, dy) - low) + 1.4142136f * float(low)) * MAP_CELL * _floorFactor;
}

void Animus::Vision::GlobalField::Retarget(int32_t cx, int32_t cy)
{
    if (std::max(std::abs(cx - _targetX), std::abs(cy - _targetY)) <= GLOBAL_RETARGET_CELLS)
        return;
    _targetX = cx;
    _targetY = cy;
    for (Entry& entry : _heap)
    {
        int32_t ex = 0;
        int32_t ey = 0;
        int32_t slot = 0;
        CellOfNode(entry.Node, ex, ey, slot);
        entry.Key = entry.Value + Heuristic(ex, ey);
    }
    std::make_heap(_heap.begin(), _heap.end(), std::greater<Entry>());
}

bool Animus::Vision::GlobalField::Step()
{
    while (!_heap.empty())
    {
        std::pop_heap(_heap.begin(), _heap.end(), std::greater<Entry>());
        Entry const top = _heap.back();
        _heap.pop_back();
        std::size_t const at = std::size_t(top.Node);
        if ((_state[at] & NODE_SETTLED) || top.Value > _value[at])
            continue;               // a stale entry
        _state[at] |= NODE_SETTLED;
        ++_settled;
        int32_t cx = 0;
        int32_t cy = 0;
        int32_t slot = 0;
        CellOfNode(top.Node, cx, cy, slot);
        float const height = FloorHeight(_height[at]);
        float const half = 0.5f * _factor[at];
        auto const relax = [&](int32_t nx, int32_t ny, int32_t nslot, float nz, int32_t dir, bool carried)
        {
            int32_t const next = NodeOf(nx, ny, nslot);
            std::size_t const n = std::size_t(next);
            if (_state[n] & NODE_SETTLED)
                return;
            Touch(next, nx, ny, nslot, nz);
            float const candidate = top.Value + DIST[dir] * (half + 0.5f * _factor[n]);
            if (candidate >= _value[n])
                return;
            _value[n] = candidate;
            // The walk from `next` goes to this node: the opposite direction.
            _next[n] = uint8_t((7 - dir) | (slot << 3));
            _state[n] |= NODE_NEXT;
            if (carried)
                _height[n] = QuantiseFloor(nz);
            _heap.push_back({ candidate + Heuristic(nx, ny), candidate, next });
            std::push_heap(_heap.begin(), _heap.end(), std::greater<Entry>());
        };
        for (int32_t dir = 0; dir < 8; ++dir)
        {
            int32_t const nx = cx + DC[dir];
            int32_t const ny = cy + DR[dir];
            if (TileIndex(nx, ny) < 0)
                continue;
            MapCell const cell = CellAt(nx, ny);
            if (!HasFloor(cell))
            {
                relax(nx, ny, 0, height, dir, true);
                continue;
            }
            for (int32_t layer = 0; layer < 2; ++layer)
            {
                if (cell.Floor[layer] == NO_FLOOR)
                    continue;
                float const floor = FloorHeight(cell.Floor[layer]);
                // Walking from the neighbour's floor onto this node: a rise of height - floor.
                float const rise = height - floor;
                if (rise > GLOBAL_STEP_UP || -rise > GLOBAL_STEP_DOWN)
                    continue;
                relax(nx, ny, layer, floor, dir, false);
            }
        }
        return true;
    }
    return false;
}

int32_t Animus::Vision::GlobalField::Attach(int32_t cx, int32_t cy, float z)
{
    if (TileIndex(cx, cy) < 0)
        return -1;
    MapCell const& cell = CellAt(cx, cy);
    int32_t const layer = NearestLayer(cell, z);
    if (layer >= 0)
    {
        if (std::fabs(FloorHeight(cell.Floor[layer]) - z) > GLOBAL_ATTACH_BAND)
            return -1;
        return FindNode(cx, cy, layer);
    }
    int32_t const node = FindNode(cx, cy, 0);
    if (node < 0 || !(_state[std::size_t(node)] & NODE_TOUCHED)
        || std::fabs(FloorHeight(_height[std::size_t(node)]) - z) > GLOBAL_ATTACH_BAND)
        return -1;
    return node;
}

float Animus::Vision::GlobalField::LeastNear(int32_t cx, int32_t cy) const
{
    float least = PLAN_SENTINEL;
    for (int32_t dy = -2; dy <= 2; ++dy)
        for (int32_t dx = -2; dx <= 2; ++dx)
            for (int32_t slot = 0; slot < 2; ++slot)
            {
                int32_t const node = FindNode(cx + dx, cy + dy, slot);
                if (node >= 0 && (_state[std::size_t(node)] & NODE_SETTLED))
                    least = std::min(least, _value[std::size_t(node)]);
            }
    return least;
}

void Animus::Vision::GlobalField::SettleNode(int32_t cx, int32_t cy, float z)
{
    if (TileIndex(cx, cy) < 0)
        return;
    bool aimed = false;
    for (uint32_t step = 0; true; ++step)
    {
        int32_t const node = Attach(cx, cy, z);
        if (node >= 0 && (_state[std::size_t(node)] & NODE_SETTLED))
            return;
        if (!aimed)
        {
            // The search is steered at its target: aim it at this cell before resuming.
            Retarget(cx, cy);
            aimed = true;
        }
        // A cell with no node the feet attach to (a floorless cell reached at another storey's height, a floor out
        // of the band) is never settled: give up once the search has passed its neighbourhood by GLOBAL_GIVE_UP,
        // rather than flood the whole domain.
        if ((step & 63) == 63 && !_heap.empty() && _heap.front().Key > LeastNear(cx, cy) + GLOBAL_GIVE_UP)
            return;
        if (!Step())
            return;
    }
}

void Animus::Vision::GlobalField::SettleTo(float limit)
{
    while (!_heap.empty() && _heap.front().Key <= limit)
        Step();
}

float Animus::Vision::GlobalField::SettlePoint(float x, float y, float z, float margin)
{
    if (!_active)
        return PLAN_SENTINEL;
    int32_t const cx = MapCellOf(x);
    int32_t const cy = MapCellOf(y);
    SettleNode(cx, cy, z);
    float least = PLAN_SENTINEL;
    int32_t const own = Attach(cx, cy, z);
    if (own >= 0 && (_state[std::size_t(own)] & NODE_SETTLED))
        least = _value[std::size_t(own)];
    else
        least = LeastNear(cx, cy);
    // Then every node whose value plus its distance to the point is within the margin of the point's (the heap's
    // key): the cells round it, and with a wider margin a band round the route.
    if (least < PLAN_SENTINEL)
    {
        Retarget(cx, cy);
        SettleTo(least + margin);
    }
    return least;
}

void Animus::Vision::GlobalField::SettleAround(float x, float y, float z)
{
    // The band round the route, so a body that strays a few yards off it is still on settled nodes.
    (void)SettlePoint(x, y, z, GLOBAL_SETTLE_MARGIN);
}

float Animus::Vision::GlobalField::ValueAt(float x, float y, float z)
{
    if (!_active || !Contains(x, y))
        return TRAV_UNREACHABLE;
    float const col = x / MAP_CELL - 0.5f;
    float const row = y / MAP_CELL - 0.5f;
    int32_t const c0 = int32_t(std::floor(col));
    int32_t const r0 = int32_t(std::floor(row));
    float const fc = col - float(c0);
    float const fr = row - float(r0);
    float sum = 0.0f;
    float weight = 0.0f;
    (void)SettlePoint(x, y, z, GLOBAL_POINT_MARGIN);
    for (int32_t dr = 0; dr < 2; ++dr)
        for (int32_t dc = 0; dc < 2; ++dc)
        {
            int32_t const node = Attach(c0 + dc, r0 + dr, z);
            if (node < 0)
                continue;
            std::size_t const at = std::size_t(node);
            if (!(_state[at] & NODE_SETTLED) || _factor[at] >= PLAN_BLOCKED || _value[at] >= PLAN_SENTINEL)
                continue;
            float const w = (dr ? fr : 1.0f - fr) * (dc ? fc : 1.0f - fc);
            sum += w * _value[at];
            weight += w;
        }
    if (weight <= 1e-6f)
        return TRAV_UNREACHABLE;
    return std::min(sum / weight, TRAV_UNREACHABLE);
}

int32_t Animus::Vision::GlobalField::StartNode(float x, float y, float z)
{
    if (!_active)
        return -1;
    int32_t const bx = MapCellOf(x);
    int32_t const by = MapCellOf(y);
    (void)SettlePoint(x, y, z, GLOBAL_POINT_MARGIN);
    int32_t const own = Attach(bx, by, z);
    if (own >= 0 && (_state[std::size_t(own)] & NODE_SETTLED) && _factor[std::size_t(own)] < PLAN_BLOCKED)
        return own;
    // A wall's foot (or a cell with no node at the feet): the lowest settled unblocked node round the body.
    int32_t const c0 = int32_t(std::floor(x / MAP_CELL - 0.5f));
    int32_t const r0 = int32_t(std::floor(y / MAP_CELL - 0.5f));
    int32_t best = -1;
    for (int32_t r = r0; r <= r0 + 1; ++r)
        for (int32_t c = c0; c <= c0 + 1; ++c)
        {
            int32_t const node = Attach(c, r, z);
            if (node < 0)
                continue;
            std::size_t const at = std::size_t(node);
            if (!(_state[at] & NODE_SETTLED) || _factor[at] >= PLAN_BLOCKED)
                continue;
            if (best < 0 || _value[at] < _value[std::size_t(best)])
                best = node;
        }
    return best;
}

int32_t Animus::Vision::GlobalField::NextOf(int32_t node) const
{
    uint8_t const state = _state[std::size_t(node)];
    if (!(state & NODE_NEXT))
        return -1;
    uint8_t const next = _next[std::size_t(node)];
    int32_t const dir = next & 7;
    int32_t const slot = (next >> 3) & 1;
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t own = 0;
    CellOfNode(node, cx, cy, own);
    return FindNode(cx + DC[dir], cy + DR[dir], slot);
}

Animus::Vision::PlanRoute Animus::Vision::GlobalField::Route(float x, float y, float z, float yaw)
{
    int32_t node = StartNode(x, y, z);
    if (node < 0)
        return PlanRoute();
    float const forwardX = std::cos(yaw);
    float const forwardY = std::sin(yaw);
    PlanRoute route;
    uint32_t steps = 0;
    uint32_t point = 0;
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t slot = 0;
    CellOfNode(node, cx, cy, slot);
    for (uint32_t step = 1; step <= PLAN_FINE_STEPS && point < PLAN_ROUTE_POINTS; ++step)
    {
        int32_t const next = NextOf(node);
        bool const moved = next >= 0;
        if (moved)
        {
            node = next;
            steps = step;
            CellOfNode(node, cx, cy, slot);
        }
        if (moved && step < PLAN_ROUTE_STEPS[point])
            continue;
        float const dx = (float(cx) + 0.5f) * MAP_CELL - x;
        float const dy = (float(cy) + 0.5f) * MAP_CELL - y;
        float const forward = dx * forwardX + dy * forwardY;
        float const right = dx * forwardY - dy * forwardX;
        float const distance = std::max(1e-6f, std::hypot(forward, right));
        for (; point < PLAN_ROUTE_POINTS && (!moved || step >= PLAN_ROUTE_STEPS[point]); ++point)
        {
            route.Sin[point] = -right / distance;
            route.Cos[point] = forward / distance;
        }
        if (!moved)
            break;
    }
    if (!steps || (cx == MapCellOf(x) && cy == MapCellOf(y)))
        return PlanRoute();
    route.Reach = float(steps) / float(PLAN_FINE_STEPS);
    route.Valid = 1.0f;
    return route;
}

Animus::Vision::PlanWaypoint Animus::Vision::GlobalField::Coarse(float x, float y, float z, float yaw)
{
    int32_t node = StartNode(x, y, z);
    if (node < 0)
        return PlanWaypoint();
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t slot = 0;
    CellOfNode(node, cx, cy, slot);
    float walked = 0.0f;
    bool moved = false;
    for (uint32_t step = 0; step < GLOBAL_PATH_CHECK && walked < GLOBAL_COARSE_YARDS; ++step)
    {
        int32_t const next = NextOf(node);
        if (next < 0)
            break;
        walked += DIST[_next[std::size_t(node)] & 7] * MAP_CELL;
        node = next;
        moved = true;
        CellOfNode(node, cx, cy, slot);
    }
    if (!moved)
        return PlanWaypoint();
    float const dx = (float(cx) + 0.5f) * MAP_CELL - x;
    float const dy = (float(cy) + 0.5f) * MAP_CELL - y;
    float const forward = dx * std::cos(yaw) + dy * std::sin(yaw);
    float const right = dx * std::sin(yaw) - dy * std::cos(yaw);
    float const distance = std::max(1e-6f, std::hypot(forward, right));
    return PlanWaypoint{ -right / distance, forward / distance, distance / GLOBAL_COARSE_YARDS, 1.0f };
}

void Animus::Vision::GlobalField::Walk(float x, float y, float z, PlanWalk& out)
{
    out = PlanWalk();
    int32_t node = StartNode(x, y, z);
    while (node >= 0 && out.Count < PLAN_KEPT_CELLS)
    {
        int32_t cx = 0;
        int32_t cy = 0;
        int32_t slot = 0;
        CellOfNode(node, cx, cy, slot);
        out.X[out.Count] = cx;
        out.Y[out.Count] = cy;
        ++out.Count;
        node = NextOf(node);
    }
    if (out.Count < 2)
        out = PlanWalk();
}

bool Animus::Vision::GlobalField::RouteChanged(float x, float y, float z, float hookYards, float& yards)
{
    yards = 0.0f;
    int32_t node = StartNode(x, y, z);
    for (uint32_t step = 0; node >= 0 && step < GLOBAL_PATH_CHECK; ++step)
    {
        std::size_t const at = std::size_t(node);
        int32_t cx = 0;
        int32_t cy = 0;
        int32_t slot = 0;
        CellOfNode(node, cx, cy, slot);
        if (_factor[at] < PLAN_BLOCKED)
        {
            MapCell const& cell = CellAt(cx, cy);
            float const height = FloorHeight(_height[at]);
            bool changed = false;
            if (_state[at] & NODE_FLOOR)
            {
                // The layer it was: gone, or moved to another storey (a new layer under it renumbers the two).
                changed = cell.Floor[slot] == NO_FLOOR
                    || std::fabs(FloorHeight(cell.Floor[slot]) - height) > LAYER_MERGE
                    || (cell.Flags & WallBit(slot));
            }
            else
            {
                int32_t const layer = NearestLayer(cell, height);
                if (layer < 0)
                    changed = (cell.Flags & (MAP_WALL_LOW | MAP_WALL_HIGH)) != 0;
                else
                    changed = std::fabs(FloorHeight(cell.Floor[layer]) - height) <= PLAN_HEIGHT_BAND
                        && (cell.Flags & WallBit(layer));
            }
            if (!changed && _blocking && yards <= hookYards)
                changed = _blocking->Blocked(cx, cy, height);
            if (changed)
                return true;
        }
        int32_t const next = NextOf(node);
        if (next >= 0)
            yards += DIST[_next[at] & 7] * MAP_CELL;
        node = next;
    }
    return false;
}

float Animus::Vision::GlobalField::WalkYards(float x, float y, float z, uint32_t maxSteps,
    std::vector<std::pair<int32_t, int32_t>>* cells)
{
    int32_t node = StartNode(x, y, z);
    if (node < 0)
        return TRAV_UNREACHABLE;
    float walked = 0.0f;
    for (uint32_t step = 0; step < maxSteps; ++step)
    {
        if (cells)
        {
            int32_t cx = 0;
            int32_t cy = 0;
            int32_t slot = 0;
            CellOfNode(node, cx, cy, slot);
            cells->emplace_back(cx, cy);
        }
        int32_t const next = NextOf(node);
        if (next < 0)
            break;
        walked += DIST[_next[std::size_t(node)] & 7] * MAP_CELL;
        node = next;
    }
    return _value[std::size_t(node)] <= 0.0f ? walked : TRAV_UNREACHABLE;
}

void Animus::Vision::GlobalRoute::Reset()
{
    _field.Clear();
    _plans = 0;
    _replans = 0;
    _contradictions = 0;
    _goalKnown = false;
    _last = GlobalReplan::None;
    _this = GlobalReplan::None;
}

bool Animus::Vision::GlobalRoute::Update(MentalMap const& map, float x, float y, float z, float gx, float gy,
    float gz, GlobalBlocking const* blocking)
{
    _tiles.Bind(map);
    _field.Use(_tiles, blocking);
    GlobalReplan reason = GlobalReplan::None;
    float yards = 0.0f;
    if (!_field.Active() || std::hypot(gx - _goalX, gy - _goalY) > GLOBAL_GOAL_MOVED
        || std::fabs(gz - _goalZ) > GLOBAL_ATTACH_BAND)
        reason = GlobalReplan::Fresh;
    else if (!_field.Contains(x, y))
        reason = GlobalReplan::Left;
    else if (_field.RouteChanged(x, y, z, GLOBAL_CONTRADICTION_YARDS, yards))
        reason = yards <= GLOBAL_CONTRADICTION_YARDS ? GlobalReplan::Contradiction : GlobalReplan::RouteWalled;
    else if (map.KnownTotal() >= _planKnown + GLOBAL_REPLAN_KNOWN)
        reason = GlobalReplan::Known;
    else if (map.Clock() >= _planClock + double(GLOBAL_REPLAN_SECONDS))
        reason = GlobalReplan::Clock;
    _this = reason;
    if (reason != GlobalReplan::None)
    {
        _field.Begin(gx, gy, gz, x, y);
        _goalX = gx;
        _goalY = gy;
        _goalZ = gz;
        _planClock = map.Clock();
        _planKnown = map.KnownTotal();
        _last = reason;
        if (++_plans > 1)
            ++_replans;
        if (reason == GlobalReplan::Contradiction)
            ++_contradictions;
    }
    // The goal is a place the map has seen: its cell or a neighbour known (asked every decision: a goal first seen
    // mid-trip routes from then on).
    _goalKnown = false;
    for (int32_t dy = -1; dy <= 1 && !_goalKnown; ++dy)
        for (int32_t dx = -1; dx <= 1 && !_goalKnown; ++dx)
            if (MapCell const* cell = map.FindCell(MapCellOf(gx) + dx, MapCellOf(gy) + dy))
                _goalKnown = Known(*cell);
    _field.SettleAround(x, y, z);
    return _goalKnown && _field.ValueAt(x, y, z) < TRAV_UNREACHABLE;
}
