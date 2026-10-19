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

#include "TravBake.h"
#include "BakedScene.h"
#include "Log.h"
#include "Map.h"
#include "MapWorldQuery.h"
#include "ModelIgnoreFlags.h"
#include "SceneRegistry.h"
#include "StringFormat.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>

namespace
{
    namespace Fs = std::filesystem;
    using namespace Animus::Vision;

    constexpr uint32_t TRAV_MAGIC = 0x56415254u;    // "TRAV"
    /// The controller sweeps a body at the knee (above STEP_UP, so a step is not a wall) and at the chest; the
    /// bake's wall test is a line of sight at both heights above the higher of the two standing points.
    constexpr float KNEE = 1.3f;
    constexpr float CHEST = 1.8f;
    /// A neighbour's floor is looked for from the jump apex down: a storey above is not a floor a walker reaches,
    /// a pit below is (PROBE_SEARCH: past the lethal drop, so the lethal class is seen).
    constexpr float PROBE_RISE = JUMP_RISE + 0.5f;
    constexpr float PROBE_SEARCH = 200.0f;
    /// Without a seed the flood starts at the box's centre, from the highest floor there: looked for from this high
    /// down to the world's floor.
    constexpr float HIGH_PROBE = 2000.0f;
    constexpr float HIGH_SEARCH = 4000.0f;

#pragma pack(push, 1)
    struct FileHeader
    {
        uint32_t Magic = TRAV_MAGIC;
        uint32_t Version = TRAV_VERSION;
        uint32_t MapId = 0;
        uint32_t Columns = 0;
        uint32_t Rows = 0;
        uint32_t Seed = TRAV_NO_CELL;
        float Box[4] = {};
        uint64_t Digest = 0;
        uint32_t Reachable = 0;
        uint32_t Reserved[3] = {};
    };
#pragma pack(pop)

    double SecondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    template <typename T>
    uint64_t Mix(uint64_t hash, T const& value)
    {
        return Scene::Fnv1a(reinterpret_cast<uint8_t const*>(&value), sizeof(value), hash);
    }

    /// What identifies a bake: the scene it was read against, the box, the seed and the format.
    uint64_t DigestOf(uint64_t sceneChecksum, TravBox const& box, TravBakeRequest const& request)
    {
        uint64_t hash = Mix(Scene::Fnv1a(nullptr, 0), sceneChecksum);
        hash = Mix(hash, box.MinX);
        hash = Mix(hash, box.MinY);
        hash = Mix(hash, box.MaxX);
        hash = Mix(hash, box.MaxY);
        uint32_t const hasSeed = request.HasSeed ? 1 : 0;
        hash = Mix(hash, hasSeed);
        hash = Mix(hash, request.SeedX);
        hash = Mix(hash, request.SeedY);
        hash = Mix(hash, request.SeedZ);
        hash = Mix(hash, TRAV_VERSION);
        return hash;
    }

    template <typename T>
    bool ReadAll(std::ifstream& in, std::vector<T>& out, std::size_t count)
    {
        out.resize(count);
        in.read(reinterpret_cast<char*>(out.data()), std::streamsize(count * sizeof(T)));
        return bool(in);
    }

    template <typename T>
    void WriteAll(std::ofstream& out, std::vector<T> const& data)
    {
        out.write(reinterpret_cast<char const*>(data.data()), std::streamsize(data.size() * sizeof(T)));
    }

    /// Loads `path` into `grid` when it is whole and of this digest; false otherwise (the caller bakes).
    bool LoadGrid(std::string const& path, uint64_t digest, TravGrid& grid)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        FileHeader header;
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || header.Magic != TRAV_MAGIC || header.Version != TRAV_VERSION || header.MapId != grid.MapId
            || header.Digest != digest || header.Reserved[0] != uint32_t(TRAV_LAYERS) || header.Columns == 0
            || header.Rows == 0
            || header.Columns > uint32_t(TRAV_MAX_SIDE) || header.Rows > uint32_t(TRAV_MAX_SIDE))
            return false;
        grid.Columns = int32_t(header.Columns);
        grid.Rows = int32_t(header.Rows);
        grid.Box = { header.Box[0], header.Box[1], header.Box[2], header.Box[3] };
        grid.Seed = header.Seed;
        grid.Digest = header.Digest;
        std::size_t const nodes = grid.Nodes();
        if (!ReadAll(in, grid.Classes, nodes) || !ReadAll(in, grid.Ground, nodes) || !ReadAll(in, grid.Liquid, nodes)
            || !ReadAll(in, grid.Edges, nodes * TRAV_DIRS) || !ReadAll(in, grid.EdgeHurt, nodes * TRAV_DIRS)
            || !ReadAll(in, grid.Reachable, header.Reachable))
            return false;
        for (uint32_t node : grid.Reachable)
            if (node >= nodes)
                return false;
        return true;
    }

    bool SaveGrid(std::string const& path, TravGrid const& grid, std::string& error)
    {
        std::error_code ec;
        Fs::create_directories(Fs::path(path).parent_path(), ec);
        std::string const partial = path + ".part";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                error = "cannot write " + partial;
                return false;
            }
            FileHeader header;
            header.MapId = grid.MapId;
            header.Columns = uint32_t(grid.Columns);
            header.Rows = uint32_t(grid.Rows);
            header.Seed = grid.Seed;
            header.Box[0] = grid.Box.MinX;
            header.Box[1] = grid.Box.MinY;
            header.Box[2] = grid.Box.MaxX;
            header.Box[3] = grid.Box.MaxY;
            header.Digest = grid.Digest;
            header.Reachable = uint32_t(grid.Reachable.size());
            header.Reserved[0] = uint32_t(TRAV_LAYERS);
            out.write(reinterpret_cast<char const*>(&header), sizeof(header));
            WriteAll(out, grid.Classes);
            WriteAll(out, grid.Ground);
            WriteAll(out, grid.Liquid);
            WriteAll(out, grid.Edges);
            WriteAll(out, grid.EdgeHurt);
            WriteAll(out, grid.Reachable);
            if (!out)
            {
                error = "writing " + partial + " failed";
                return false;
            }
        }
        Fs::rename(partial, path, ec);
        if (ec)
        {
            error = "cannot move " + partial + " into place: " + ec.message();
            return false;
        }
        return true;
    }

    /// **The bake itself** (CONTRACT.md sec 1): a flood from the seed over the box. Each cell's floor is looked for
    /// from a neighbour the flood has entered -- from that neighbour's standing height plus the jump apex down, so a
    /// storey above is not this cell's floor and a pit below is -- and each edge is judged by the controller's own
    /// rules on the two standing heights: a step within STEP_UP, a jump to JUMP_RISE, a drop by Player::HandleFall's
    /// line, water swum, magma and steep floors never entered, and a wall wherever the knee or chest line between
    /// the two standing points meets a model (the static trees alone: the dungeon's doors are cleared for the nav
    /// stages, and a cache must not depend on them).
    void Bake(TravBakeRequest const& request, TravGrid& grid)
    {
        Map* map = request.World;
        Animus::Movement::MapWorldQuery const query(map, request.PhaseMask, false);
        uint32_t const nodes = grid.Nodes();
        grid.Classes.assign(nodes, uint8_t(TravClass::Unknown));
        grid.Ground.assign(nodes, TRAV_NO_HEIGHT);
        grid.Liquid.assign(nodes, TRAV_NO_HEIGHT);
        grid.Edges.assign(std::size_t(nodes) * TRAV_DIRS, uint8_t(TravEdge::None));
        grid.EdgeHurt.assign(std::size_t(nodes) * TRAV_DIRS, 0.0f);
        grid.Reachable.clear();

        std::vector<uint8_t> water(nodes, 0);
        std::vector<uint8_t> blocked(nodes, 0);         // magma, slime or too steep: never entered
        std::vector<uint8_t> entered(nodes, 0);
        std::vector<uint8_t> walkIn(nodes, 0);          // entered by something other than a jump
        std::vector<uint8_t> jumpIn(nodes, 0);
        std::vector<float> climbIn(nodes, std::numeric_limits<float>::infinity());    // the least walk-in climb
        std::vector<uint8_t> worstDrop(nodes, 0);       // 1 safe, 2 hurt, 3 lethal: the worst drop leaving the node
        // The wall test is symmetric, so each undirected pair is cast once: a node's +x, +y, +x+y and +x-y lines
        // (directions 0, 2, 4, 5) to each layer of the cell there, -1 untested, 0 blocked, 1 clear.
        std::vector<int8_t> lineClear(std::size_t(nodes) * 4 * TRAV_LAYERS, int8_t(-1));
        auto const lineSlot = [&](uint32_t from, uint32_t to, int32_t dir) -> int8_t&
        {
            bool const own = dir == 0 || dir == 2 || dir == 4 || dir == 5;
            // The other end owns a line the other way: -x is the neighbour's +x, -y its +y, -x-y its +x+y, -x+y
            // its +x-y.
            uint32_t const owner = own ? from : to;
            uint32_t const other = own ? to : from;
            int32_t const slot = own ? (dir == 0 ? 0 : dir == 2 ? 1 : dir == 4 ? 2 : 3)
                : (dir == 1 ? 0 : dir == 3 ? 1 : dir == 7 ? 2 : 3);
            return lineClear[(std::size_t(owner) * 4 + std::size_t(slot)) * TRAV_LAYERS
                + std::size_t(TravGrid::LayerOfNode(other))];
        };

        // A cell's floor as a body arriving at height fromZ finds it (the jump apex down, PROBE_SEARCH below): the
        // layer it is (a floor within TRAV_LAYER_MATCH of one the cell holds), else a new layer, classed on its
        // first find; TRAV_NO_CELL for no ground. A cell with every layer taken gives the nearest.
        auto const probe = [&](uint32_t cell, float fromZ, float search = PROBE_SEARCH) -> uint32_t
        {
            float const cx = grid.CentreX(cell);
            float const cy = grid.CentreY(cell);
            float const floor = query.FloorBelow(cx, cy, fromZ + PROBE_RISE, search + PROBE_RISE);
            if (floor <= Animus::Movement::INVALID_FLOOR + 1.0f)
                return TRAV_NO_CELL;
            uint32_t nearest = TRAV_NO_CELL;
            float nearestGap = 0.0f;
            uint32_t free = TRAV_NO_CELL;
            for (int32_t layer = 0; layer < TRAV_LAYERS; ++layer)
            {
                uint32_t const node = TravGrid::NodeAt(cell, layer);
                if (grid.Ground[node] == TRAV_NO_HEIGHT)
                {
                    if (free == TRAV_NO_CELL)
                        free = node;
                    continue;
                }
                float const gap = std::fabs(grid.Ground[node] - floor);
                if (gap <= TRAV_LAYER_MATCH)
                    return node;
                if (nearest == TRAV_NO_CELL || gap < nearestGap)
                {
                    nearest = node;
                    nearestGap = gap;
                }
            }
            if (free == TRAV_NO_CELL)
                return nearest;
            uint32_t const node = free;
            grid.Ground[node] = floor;
            Animus::Movement::Liquid const liquid = query.LiquidAt(cx, cy, floor + 0.5f);
            if (liquid.Present)
            {
                grid.Liquid[node] = liquid.Level;
                if (liquid.Deadly)
                {
                    blocked[node] = 1;
                    grid.Classes[node] = uint8_t(TravClass::Hazard);
                    return node;
                }
                if (liquid.Level - floor > TRAV_WATER_DEPTH)
                {
                    water[node] = 1;
                    return node;
                }
            }
            if (query.FloorNormalZ(cx, cy, floor) < TRAV_WALKABLE_NORMAL_Z)
            {
                blocked[node] = 1;
                grid.Classes[node] = uint8_t(TravClass::Slope);
            }
            return node;
        };
        auto const standing = [&](uint32_t node) { return water[node] ? grid.Liquid[node] : grid.Ground[node]; };

        // The seed: the entrance's cell at the entrance's floor, else the box's centre from the highest floor.
        float const seedX = request.HasSeed ? request.SeedX : 0.5f * (grid.Box.MinX + grid.Box.MaxX);
        float const seedY = request.HasSeed ? request.SeedY : 0.5f * (grid.Box.MinY + grid.Box.MaxY);
        float const seedZ = request.HasSeed ? request.SeedZ : HIGH_PROBE;
        float const seedSearch = request.HasSeed ? PROBE_SEARCH : HIGH_SEARCH;
        uint32_t const seedCell = grid.CellOf(seedX, seedY);
        grid.Seed = TRAV_NO_CELL;
        if (seedCell == TRAV_NO_CELL)
            return;
        uint32_t seed = probe(seedCell, seedZ, seedSearch);
        if (seed == TRAV_NO_CELL || blocked[seed])
        {
            // The entrance stands where the probe found nothing (a trigger just outside the walls): try its
            // neighbours before giving the arena up.
            seed = TRAV_NO_CELL;
            for (int32_t dir = 0; dir < TRAV_DIRS && seed == TRAV_NO_CELL; ++dir)
                if (uint32_t const next = grid.Neighbour(seedCell, dir); next != TRAV_NO_CELL)
                {
                    uint32_t const node = probe(next, seedZ, seedSearch);
                    if (node != TRAV_NO_CELL && !blocked[node])
                        seed = node;
                }
            if (seed == TRAV_NO_CELL)
                return;
        }
        grid.Seed = seed;

        std::queue<uint32_t> open;
        entered[seed] = 1;
        walkIn[seed] = 1;
        open.push(seed);
        while (!open.empty())
        {
            uint32_t const from = open.front();
            open.pop();
            uint32_t const fromCell = TravGrid::CellOfNode(from);
            float const zFrom = standing(from);
            float const fromX = grid.CentreX(fromCell);
            float const fromY = grid.CentreY(fromCell);
            for (int32_t dir = 0; dir < TRAV_DIRS; ++dir)
            {
                uint32_t const toCell = grid.Neighbour(fromCell, dir);
                if (toCell == TRAV_NO_CELL)
                    continue;
                // Every arrival probes: a walkway's neighbour from the walkway, the courtyard's from the courtyard.
                uint32_t const to = probe(toCell, zFrom);
                if (to == TRAV_NO_CELL || blocked[to])
                    continue;
                float const zTo = standing(to);
                float const dz = zTo - zFrom;
                TravEdge kind = TravEdge::None;
                float hurt = 0.0f;
                if (water[from] && water[to])
                    kind = std::fabs(dz) <= JUMP_RISE ? TravEdge::Swim : TravEdge::None;
                else if (!water[from] && water[to])
                    kind = dz <= JUMP_RISE ? TravEdge::Swim : TravEdge::None;   // a drop into water hurts nobody
                else if (dz > JUMP_RISE)
                    kind = TravEdge::None;
                else if (dz > TRAV_STEP_UP)
                    kind = TravEdge::Jump;
                else if (dz >= -TRAV_STEP_UP)
                    kind = TravEdge::Walk;
                else
                {
                    float const drop = -dz;
                    if (drop < FALL_DAMAGE_FROM)
                    {
                        kind = TravEdge::DropSafe;
                        worstDrop[from] = std::max<uint8_t>(worstDrop[from], 1);
                    }
                    else if (drop < FALL_LETHAL_FROM)
                    {
                        kind = TravEdge::DropHurt;
                        hurt = std::clamp(FALL_DAMAGE_SLOPE * drop + FALL_DAMAGE_INTERCEPT, 0.0f, 1.0f);
                        worstDrop[from] = std::max<uint8_t>(worstDrop[from], 2);
                    }
                    else
                        worstDrop[from] = 3;
                }
                if (kind == TravEdge::None)
                    continue;

                // The wall test: the knee and chest lines between the two standing points, at the higher one's
                // height (a step's riser is under the knee line; a drop's lip is behind the body, not in the line).
                int8_t& clear = lineSlot(from, to, dir);
                if (clear < 0)
                {
                    float const top = std::max(zFrom, zTo);
                    float const toX = grid.CentreX(toCell);
                    float const toY = grid.CentreY(toCell);
                    bool const knee = map->isInLineOfSight(fromX, fromY, top + KNEE, toX, toY, top + KNEE,
                        request.PhaseMask, LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing);
                    bool const chest = knee && map->isInLineOfSight(fromX, fromY, top + CHEST, toX, toY, top + CHEST,
                        request.PhaseMask, LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing);
                    clear = knee && chest ? 1 : 0;
                }
                if (!clear)
                    continue;

                grid.Edges[std::size_t(from) * TRAV_DIRS + dir] = MakeEdge(kind, TravGrid::LayerOfNode(to));
                grid.EdgeHurt[std::size_t(from) * TRAV_DIRS + dir] = hurt;
                if (kind == TravEdge::Jump)
                    jumpIn[to] = 1;
                else
                {
                    walkIn[to] = 1;
                    if (kind == TravEdge::Walk)
                        climbIn[to] = std::min(climbIn[to], dz);
                }
                if (!entered[to])
                {
                    entered[to] = 1;
                    open.push(to);
                }
            }
        }

        // No cutting a corner: a diagonal edge needs ground the flood entered in both cells it passes between, at a
        // height within a step of the edge's own (the line of sight clears a pillar's corner that a body's radius
        // does not; a courtyard under the corner's walkway is not the corner's ground).
        auto const sideEntered = [&](uint32_t cell, float low, float high)
        {
            if (cell == TRAV_NO_CELL)
                return false;
            for (int32_t layer = 0; layer < TRAV_LAYERS; ++layer)
            {
                uint32_t const node = TravGrid::NodeAt(cell, layer);
                if (entered[node] && standing(node) >= low - TRAV_STEP_UP && standing(node) <= high + TRAV_STEP_UP)
                    return true;
            }
            return false;
        };
        for (uint32_t node = 0; node < nodes; ++node)
            for (int32_t dir = 4; dir < TRAV_DIRS; ++dir)
            {
                uint8_t& edge = grid.Edges[std::size_t(node) * TRAV_DIRS + dir];
                if (EdgeKind(edge) == TravEdge::None)
                    continue;
                uint32_t const cell = TravGrid::CellOfNode(node);
                uint32_t const to = TravGrid::NodeAt(grid.Neighbour(cell, dir), EdgeLayer(edge));
                float const low = std::min(standing(node), standing(to));
                float const high = std::max(standing(node), standing(to));
                uint32_t const sideX = grid.Neighbour(cell, TRAV_DX[dir] > 0 ? 0 : 1);
                uint32_t const sideY = grid.Neighbour(cell, TRAV_DY[dir] > 0 ? 2 : 3);
                if (!sideEntered(sideX, low, high) || !sideEntered(sideY, low, high))
                    edge = uint8_t(TravEdge::None);
            }

        // The classes, from what the flood found: a floor never entered is a wall; a floor entered is free, a step
        // when every walk into it climbs, a jump when only jumps reach it; and the node a drop starts from is
        // labelled by its worst drop, which is what the costmap must learn to see.
        for (uint32_t node = 0; node < nodes; ++node)
        {
            if (grid.Ground[node] == TRAV_NO_HEIGHT || blocked[node])
                continue;
            if (!entered[node])
            {
                grid.Classes[node] = uint8_t(TravClass::Wall);
                continue;
            }
            TravClass label = TravClass::Free;
            if (water[node])
                label = TravClass::Water;
            else if (!walkIn[node] && jumpIn[node])
                label = TravClass::Jump;
            else if (walkIn[node] && climbIn[node] > TRAV_STEP_FROM && climbIn[node] < TRAV_STEP_UP + 1.0f)
                label = TravClass::StepUp;
            if (!water[node] && worstDrop[node])
                label = worstDrop[node] == 1 ? TravClass::DropSafe : worstDrop[node] == 2 ? TravClass::DropHurt
                    : TravClass::DropLethal;
            grid.Classes[node] = uint8_t(label);
            if (label == TravClass::Free || label == TravClass::StepUp)
                grid.Reachable.push_back(node);
        }
    }
}

Animus::Vision::TravRegistry& Animus::Vision::TravRegistry::Instance()
{
    static TravRegistry registry;
    return registry;
}

Animus::Vision::TravGrid const* Animus::Vision::TravRegistry::Get(uint32_t mapId, std::string const& arena) const
{
    std::lock_guard lock(_lock);
    auto const found = _grids.find(std::to_string(mapId) + "-" + arena);
    return found == _grids.end() ? nullptr : found->second.get();
}

Animus::Vision::TravGrid const* Animus::Vision::TravRegistry::Ensure(TravBakeRequest const& request,
    std::string const& travDir, TravBakeReport& report, std::string& error)
{
    std::lock_guard lock(_lock);
    report = TravBakeReport();
    if (!request.World)
    {
        error = "no map to bake";
        return nullptr;
    }
    uint32_t const mapId = request.World->GetId();
    std::string const key = std::to_string(mapId) + "-" + request.Arena;
    if (auto const found = _grids.find(key); found != _grids.end())
        return found->second.get();

    auto const start = std::chrono::steady_clock::now();
    BakedWorld const* scene = SceneRegistry::Instance().Get(mapId);
    if (!scene)
    {
        error = "map " + std::to_string(mapId) + " has no baked scene to bake traversability against";
        return nullptr;
    }
    Scene::SceneHeader const& header = scene->Header();

    // The box: the arena's, else the scene's solid bounds (a dungeon's walls hold its whole floor); clipped to
    // TRAV_MAX_SIDE cells a side round the seed.
    TravBox box = request.HasBox ? request.Box
        : TravBox{ header.SolidMin[0], header.SolidMin[1], header.SolidMax[0], header.SolidMax[1] };
    float const limit = float(TRAV_MAX_SIDE) * TRAV_CELL;
    float const centreX = request.HasSeed ? request.SeedX : 0.5f * (box.MinX + box.MaxX);
    float const centreY = request.HasSeed ? request.SeedY : 0.5f * (box.MinY + box.MaxY);
    if (box.MaxX - box.MinX > limit)
    {
        box.MinX = std::max(box.MinX, centreX - 0.5f * limit);
        box.MaxX = std::min(box.MaxX, box.MinX + limit);
    }
    if (box.MaxY - box.MinY > limit)
    {
        box.MinY = std::max(box.MinY, centreY - 0.5f * limit);
        box.MaxY = std::min(box.MaxY, box.MinY + limit);
    }
    // Whole cells: the box is rounded out to the cell size.
    int32_t const columns = int32_t(std::ceil((box.MaxX - box.MinX) / TRAV_CELL));
    int32_t const rows = int32_t(std::ceil((box.MaxY - box.MinY) / TRAV_CELL));
    if (columns <= 0 || rows <= 0)
    {
        error = "arena " + request.Arena + " of map " + std::to_string(mapId) + " has an empty box";
        return nullptr;
    }
    box.MaxX = box.MinX + float(columns) * TRAV_CELL;
    box.MaxY = box.MinY + float(rows) * TRAV_CELL;

    auto grid = std::make_unique<TravGrid>();
    grid->MapId = mapId;
    grid->Name = key;
    grid->Box = box;
    grid->Columns = columns;
    grid->Rows = rows;
    grid->Digest = DigestOf(header.Checksum, box, request);
    std::string const path = (Fs::path(travDir) / (key + ".trav")).string();
    report.Path = path;
    if (!LoadGrid(path, grid->Digest, *grid))
    {
        grid->Box = box;
        grid->Columns = columns;
        grid->Rows = rows;
        Bake(request, *grid);
        report.Baked = true;
        std::string saveError;
        if (!SaveGrid(path, *grid, saveError))
            LOG_WARN("module.animus", "Traversability bake {}: not cached ({}); it is baked again next start",
                key, saveError);
    }
    report.Seconds = SecondsSince(start);
    for (uint8_t label : grid->Classes)
        if (label < TRAV_CLASSES)
            ++report.Histogram[label];
    std::string histogram;
    for (uint32_t label = 0; label < TRAV_CLASSES; ++label)
        if (report.Histogram[label])
            histogram += Acore::StringFormat("{}{} {}", histogram.empty() ? "" : ", ", TRAV_CLASS_NAMES[label],
                report.Histogram[label]);
    LOG_INFO("module.animus", "Traversability bake {}: {} ({:.1f} s): {} x {} cells over ({:.0f}, {:.0f})-({:.0f}, "
        "{:.0f}), {} reachable; {}", key, report.Baked ? "baked" : "loaded", report.Seconds, columns, rows, box.MinX,
        box.MinY, box.MaxX, box.MaxY, grid->Reachable.size(), histogram);
    TravGrid const* raw = grid.get();
    _grids[key] = std::move(grid);
    return raw;
}

void Animus::Vision::CostToGo(TravGrid const& grid, uint32_t goal, float dropHurtYards, std::vector<float>& out)
{
    uint32_t const nodes = grid.Nodes();
    out.assign(nodes, TRAV_UNREACHABLE);
    if (goal >= nodes)
        return;
    using Entry = std::pair<float, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    out[goal] = 0.0f;
    open.emplace(0.0f, goal);
    while (!open.empty())
    {
        auto const [cost, node] = open.top();
        open.pop();
        if (cost > out[node])
            continue;
        uint32_t const cell = TravGrid::CellOfNode(node);
        int32_t const layer = TravGrid::LayerOfNode(node);
        // Every node with an edge into this one: a node of the neighbour in direction d whose edge back along
        // OPPOSITE[d] lands on this node's layer.
        for (int32_t dir = 0; dir < TRAV_DIRS; ++dir)
        {
            uint32_t const fromCell = grid.Neighbour(cell, dir);
            if (fromCell == TRAV_NO_CELL)
                continue;
            for (int32_t fromLayer = 0; fromLayer < TRAV_LAYERS; ++fromLayer)
            {
                uint32_t const from = TravGrid::NodeAt(fromCell, fromLayer);
                std::size_t const slot = std::size_t(from) * TRAV_DIRS + std::size_t(TRAV_OPPOSITE[dir]);
                uint8_t const edge = grid.Edges[slot];
                TravEdge const kind = EdgeKind(edge);
                if (kind == TravEdge::None || EdgeLayer(edge) != layer)
                    continue;
                float const next = cost + TravEdgeCost(kind, TravGrid::StepYards(dir), grid.EdgeHurt[slot],
                    dropHurtYards);
                if (next < out[from])
                {
                    out[from] = next;
                    open.emplace(next, from);
                }
            }
        }
    }
}

void Animus::Vision::SampleLabels(TravGrid const& grid, std::vector<float> const& cost, float x, float y, float z,
    float yaw, float cell, uint8_t* classes, uint16_t* costs)
{
    // The crop's frame (MentalMap::Crop): forward along (cos yaw, sin yaw), right along (sin yaw, -cos yaw).
    float const forwardX = std::cos(yaw);
    float const forwardY = std::sin(yaw);
    float const rightX = forwardY;
    float const rightY = -forwardX;
    uint32_t const samples = std::max<uint32_t>(1, uint32_t(std::lround(cell / TRAV_CELL)));
    float const centre = float(TRAV_LABEL_SIDE) * 0.5f;
    bool const haveCost = cost.size() == grid.Nodes();
    for (uint32_t row = 0; row < TRAV_LABEL_SIDE; ++row)
        for (uint32_t col = 0; col < TRAV_LABEL_SIDE; ++col)
        {
            float const forward = (centre - 0.5f - float(row)) * cell;
            float const right = (float(col) + 0.5f - centre) * cell;
            uint32_t counts[TRAV_CLASSES] = {};
            float best = TRAV_UNREACHABLE;
            for (uint32_t i = 0; i < samples; ++i)
                for (uint32_t j = 0; j < samples; ++j)
                {
                    float const f = forward + ((float(i) + 0.5f) / float(samples) - 0.5f) * cell;
                    float const r = right + ((float(j) + 0.5f) / float(samples) - 0.5f) * cell;
                    float const px = x + f * forwardX + r * rightX;
                    float const py = y + f * forwardY + r * rightY;
                    if (grid.CellOf(px, py) == TRAV_NO_CELL)
                    {
                        ++counts[0];
                        continue;
                    }
                    // The floor of the body's own storey there; a cell with none is rock or a void (a Wall).
                    uint32_t const at = grid.NodeOf(px, py, z);
                    if (at == TRAV_NO_CELL)
                    {
                        ++counts[uint32_t(TravClass::Wall)];
                        continue;
                    }
                    // No ground inside the bake's box is rock or a void: a wall to the body. Left Unknown it was
                    // never trained on, and the walls the costmap learned were the few unreachable floors (nav1,
                    // 2026-10-15: 32 wall cells of 15,656 in Stockades, 78% Unknown; wall IoU 0.02).
                    uint32_t const kind = grid.Classes[at] == uint8_t(TravClass::Unknown)
                        ? uint32_t(TravClass::Wall) : std::min<uint32_t>(grid.Classes[at], TRAV_CLASSES - 1);
                    ++counts[kind];
                    if (haveCost)
                        best = std::min(best, cost[at]);
                }
            // A cell wider than the bake's (the coarse tier) is passable if any of it is: rock round a corridor
            // narrower than the cell must not outvote the corridor.
            if (samples > 1)
            {
                uint32_t passable = 0;
                for (uint32_t candidate = 1; candidate < TRAV_CLASSES; ++candidate)
                    if (candidate != uint32_t(TravClass::Wall))
                        passable += counts[candidate];
                if (passable)
                    counts[uint32_t(TravClass::Wall)] = 0;
            }
            // The majority of the known classes; Unknown only when nothing under the cell is known.
            uint32_t label = 0;
            for (uint32_t candidate = 1; candidate < TRAV_CLASSES; ++candidate)
                if (counts[candidate] > counts[label] || (label == 0 && counts[candidate] > 0))
                    label = candidate;
            std::size_t const index = std::size_t(row) * TRAV_LABEL_SIDE + col;
            classes[index] = uint8_t(label);
            costs[index] = FloatToHalf(std::min(best, TRAV_UNREACHABLE));
        }
}
