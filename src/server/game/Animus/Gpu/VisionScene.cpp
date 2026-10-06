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

#include "VisionScene.h"
#include "BoundingIntervalHierarchy.h"
#include "DynamicTree.h"
#include "GameObjectModel.h"
#include "MapTree.h"
#include "ModelInstance.h"
#include "VisionCaster.h"
#include "WorldModel.h"
#include <algorithm>

namespace
{
    using namespace Animus::GpuVision;

    /// A BIH node's real children (BIH::subdivide): a BVH2 node has one, at its offset; a split has two, at offset
    /// and offset + 3, except where subdivide made a single-child split over empty space -- its missing side's
    /// clip is -inf (no left child) or +inf (no right child), and the offset then points 3 words short, at
    /// whatever node lies there. A leaf has none. Returns how many were written.
    uint32_t RealChildren(std::vector<uint32_t> const& nodes, uint32_t node, uint32_t (&children)[2])
    {
        if (node + 2 >= nodes.size() && !(node + 1 < nodes.size()))
            return 0;
        uint32_t const tn = nodes[node];
        uint32_t const axis = (tn & (3u << 30)) >> 30;
        uint32_t const offset = tn & ~(7u << 29);
        uint32_t count = 0;
        if (tn & (1u << 29))
        {
            if (axis < 3 && offset < nodes.size())
                children[count++] = offset;
            return count;
        }
        if (axis == 3 || node + 2 >= nodes.size())
            return 0;
        constexpr uint32_t MINUS_INF = 0xFF800000u;
        constexpr uint32_t PLUS_INF = 0x7F800000u;
        if (nodes[node + 1] != MINUS_INF && offset < nodes.size())
            children[count++] = offset;
        if (nodes[node + 2] != PLUS_INF && offset + 3 < nodes.size())
            children[count++] = offset + 3;
        return count;
    }

    void PutFloat(uint32_t* at, float value)
    {
        *at = AsBits(value);
    }

    void PutVector(uint32_t* at, G3D::Vector3 const& v)
    {
        PutFloat(at, v.x);
        PutFloat(at + 1, v.y);
        PutFloat(at + 2, v.z);
    }

    void PutBox(uint32_t* at, G3D::AABox const& box)
    {
        PutVector(at, box.low());
        PutVector(at + 3, box.high());
    }

    /// The transform and bound both kinds of instance share (ModelInstance::intersectRay's and GameObjectModel's).
    void PutTransform(uint32_t* record, uint32_t modelOffset, uint32_t flags, G3D::Vector3 const& pos,
        G3D::Matrix3 const& invRot, float invScale, float scale, G3D::AABox const& bound)
    {
        record[INSTANCE_MODEL] = modelOffset;
        record[INSTANCE_FLAGS] = flags;
        PutVector(record + INSTANCE_POS, pos);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                PutFloat(record + INSTANCE_INV_ROT + r * 3 + c, invRot[r][c]);
        PutFloat(record + INSTANCE_INV_SCALE, invScale);
        PutFloat(record + INSTANCE_SCALE, scale);
        PutBox(record + INSTANCE_BOUND, bound);
    }

    /// A liquid's words (LIQUID_HEADER, the heights, the flags), appended.
    uint32_t PackLiquid(VMAP::WmoLiquid const& liquid, Words& pool, LiquidDeadly const& deadly)
    {
        uint32_t tilesX = 0;
        uint32_t tilesY = 0;
        G3D::Vector3 corner;
        liquid.GetPosInfo(tilesX, tilesY, corner);
        float const* heights = liquid.GetHeights();
        uint8_t const* flags = liquid.GetFlags();
        if (!heights)
            return 0;   // WmoLiquid::IntersectRay: no heights, no surface (as no liquid at all)

        uint32_t const offset = uint32_t(pool.size());
        pool.resize(pool.size() + LIQUID_HEADER, 0);
        pool[offset + LIQUID_TILES_X] = tilesX;
        pool[offset + LIQUID_TILES_Y] = tilesY;
        PutVector(&pool[offset + LIQUID_CORNER], corner);
        pool[offset + LIQUID_TYPE] = liquid.GetType();
        pool[offset + LIQUID_DEADLY] = deadly && deadly(liquid.GetType()) ? 1 : 0;
        pool[offset + LIQUID_HAS_FLAGS] = flags ? 1 : 0;
        uint32_t const heightCount = flags ? (tilesX + 1) * (tilesY + 1) : 1;
        for (uint32_t i = 0; i < heightCount; ++i)
            pool.push_back(AsBits(heights[i]));
        if (flags)
        {
            uint32_t const tiles = tilesX * tilesY;
            uint32_t const start = uint32_t(pool.size());
            pool.resize(pool.size() + (tiles + 3) / 4, 0);
            for (uint32_t i = 0; i < tiles; ++i)
                pool[start + i / 4] |= uint32_t(flags[i]) << ((i % 4) * 8);
        }
        return offset;
    }
}

uint32_t Animus::GpuVision::BihPushDepth(BIH const& tree)
{
    return BihPushDepth(tree.GetNodes());
}

uint32_t Animus::GpuVision::BihPushDepth(std::vector<uint32_t> const& nodes)
{
    uint32_t deepest = 0;
    // (node, splits above it), walked without recursion: a broken tree must not take the server down.
    std::vector<std::pair<uint32_t, uint32_t>> open = { { 0, 0 } };
    while (!open.empty())
    {
        auto const [node, splits] = open.back();
        open.pop_back();
        if (node >= nodes.size() || open.size() > nodes.size())
            continue;
        uint32_t children[2];
        uint32_t const count = RealChildren(nodes, node, children);
        deepest = std::max(deepest, splits);
        // A split with both children pushes the far one; a single-child split or a BVH2 node pushes nothing.
        for (uint32_t i = 0; i < count; ++i)
            if (children[i] > node)
                open.push_back({ children[i], splits + (count == 2 ? 1 : 0) });
    }
    return deepest;
}

void Animus::GpuVision::PackBih(BIH const& tree, Words& out, uint32_t record)
{
    PutBox(&out[record], tree.bound());
    std::vector<uint32_t> const& nodes = tree.GetNodes();
    std::vector<uint32_t> const& objects = tree.GetObjects();
    out[record + BIH_NODES] = uint32_t(out.size());
    out.insert(out.end(), nodes.begin(), nodes.end());
    out[record + BIH_OBJECTS] = uint32_t(out.size());
    out.insert(out.end(), objects.begin(), objects.end());
}

uint32_t Animus::GpuVision::PackModel(VMAP::WorldModel const& model, Words& pool, LiquidDeadly const& deadly,
    ModelCounts* counts)
{
    if (pool.empty())
        pool.push_back(0);  // word 0 is never a record
    std::vector<VMAP::GroupModel> const& groups = model.GetGroups();
    uint32_t const offset = uint32_t(pool.size());
    pool.resize(pool.size() + MODEL_HEADER + groups.size() * GROUP_WORDS, 0);
    pool[offset + MODEL_GROUPS] = uint32_t(groups.size());
    PackBih(model.GetGroupTree(), pool, offset + MODEL_TREE);
    ModelCounts local;
    local.Groups = uint32_t(groups.size());
    local.BihNodeWords += uint32_t(model.GetGroupTree().GetNodes().size());
    // WorldModel::IntersectRay walks a single group straight, more through the group tree.
    uint32_t const groupDepth = groups.size() == 1 ? 0 : BihPushDepth(model.GetGroupTree());
    uint32_t meshDepth = 0;
    for (uint32_t i = 0; i < groups.size(); ++i)
    {
        VMAP::GroupModel const& group = groups[i];
        uint32_t const at = GroupOffset(offset, i);
        PutBox(&pool[at + GROUP_BOUND], group.GetBound());
        std::vector<G3D::Vector3> const& vertices = group.GetVertices();
        std::vector<VMAP::MeshTriangle> const& triangles = group.GetTriangles();
        pool[at + GROUP_TRIANGLES] = uint32_t(triangles.size());
        pool[at + GROUP_VERTEX_OFFSET] = uint32_t(pool.size());
        for (G3D::Vector3 const& v : vertices)
        {
            pool.push_back(AsBits(v.x));
            pool.push_back(AsBits(v.y));
            pool.push_back(AsBits(v.z));
        }
        pool[at + GROUP_TRIANGLE_OFFSET] = uint32_t(pool.size());
        for (VMAP::MeshTriangle const& t : triangles)
        {
            pool.push_back(t.idx0);
            pool.push_back(t.idx1);
            pool.push_back(t.idx2);
        }
        PackBih(group.GetMeshTree(), pool, at + GROUP_TREE);
        if (VMAP::WmoLiquid const* liquid = group.GetLiquid())
        {
            uint32_t const liquidOffset = PackLiquid(*liquid, pool, deadly);
            pool[at + GROUP_LIQUID] = liquidOffset;
            local.Liquids += liquidOffset ? 1 : 0;
        }
        local.Triangles += uint32_t(triangles.size());
        local.Vertices += uint32_t(vertices.size());
        local.BihNodeWords += uint32_t(group.GetMeshTree().GetNodes().size());
        if (!triangles.empty())
            meshDepth = std::max(meshDepth, BihPushDepth(group.GetMeshTree()));
    }
    local.StackDepth = groupDepth + meshDepth;
    local.LiquidStackDepth = groupDepth;
    if (counts)
        *counts = local;
    return offset;
}

void Animus::GpuVision::PackInstance(VMAP::ModelInstance const& spawn, uint32_t modelOffset, uint32_t* record)
{
    PutTransform(record, modelOffset, spawn.flags, spawn.iPos, spawn.GetInvRot(), spawn.GetInvScale(), spawn.iScale,
        spawn.iBound);
    record[INSTANCE_PHASE] = 0;
    record[INSTANCE_SPAWNED] = 1;
}

void Animus::GpuVision::PackDoor(GameObjectModel const& door, uint32_t modelOffset, uint32_t* record)
{
    GameObjectModel::RayView const view = door.GetRayView();
    PutTransform(record, view.Model ? modelOffset : 0, 0, view.Pos, view.InvRot, view.InvScale, view.Scale,
        view.Bound);
    record[INSTANCE_PHASE] = view.PhaseMask;
    record[INSTANCE_SPAWNED] = view.Spawned ? 1 : 0;
}

bool Animus::GpuVision::PackTerrain(Vision::VisionWorld const& world, int32_t tileX, int32_t tileY, Words& out)
{
    out.clear();
    Vision::TerrainTile const tile = world.Tile(tileX, tileY);
    if (!tile.Loaded || (!tile.Heights && !tile.Liquid))
        return false;
    out.assign(TERRAIN_WORDS, 0);
    out[TERRAIN_HEIGHTS] = tile.Heights ? 1 : 0;
    out[TERRAIN_MAX] = AsBits(tile.MaxHeight);
    out[TERRAIN_LIQUID] = tile.Liquid ? 1 : 0;
    for (uint32_t x = 0; x < TERRAIN_CELLS; ++x)
        for (uint32_t y = 0; y < TERRAIN_CELLS; ++y)
        {
            // The cell exactly as CastTerrain reads it; its liquid only matters (and is only read) on a grid with
            // liquid, as there.
            Vision::TerrainCell const cell = world.Cell(tileX, tileY, int32_t(x), int32_t(y), tile.Liquid);
            uint32_t const index = x * TERRAIN_CELLS + y;
            uint32_t flags = 0;
            if (cell.Solid)
            {
                // GetCellHeights' corners: shared with the neighbours, and the same value whichever cell reads them.
                uint32_t* v9 = &out[TERRAIN_CORNERS];
                v9[x * TERRAIN_V9 + y] = AsBits(cell.Corner[0]);
                v9[(x + 1) * TERRAIN_V9 + y] = AsBits(cell.Corner[1]);
                v9[x * TERRAIN_V9 + y + 1] = AsBits(cell.Corner[2]);
                v9[(x + 1) * TERRAIN_V9 + y + 1] = AsBits(cell.Corner[3]);
                out[TERRAIN_CENTRES + index] = AsBits(cell.Centre);
                flags |= CELL_SOLID;
            }
            if (cell.Liquid)
            {
                out[TERRAIN_LEVELS + index] = AsBits(cell.Level);
                flags |= CELL_LIQUID | (cell.Deadly ? CELL_DEADLY : 0);
            }
            out[TERRAIN_FLAGS + index / 4] |= flags << ((index % 4) * 8);
        }
    return true;
}

uint32_t Animus::GpuVision::ModelPool::Acquire(VMAP::WorldModel const* model, LiquidDeadly const& deadly)
{
    if (!model)
        return 0;
    auto const found = _offsets.find(model);
    if (found != _offsets.end())
        return found->second.Offset;
    Entry entry;
    uint32_t const before = uint32_t(_words.size());
    entry.Offset = PackModel(*model, _words, deadly, &entry.Counts);
    entry.Size = uint32_t(_words.size()) - before;
    _counts.Groups += entry.Counts.Groups;
    _counts.Triangles += entry.Counts.Triangles;
    _counts.Vertices += entry.Counts.Vertices;
    _counts.BihNodeWords += entry.Counts.BihNodeWords;
    _counts.Liquids += entry.Counts.Liquids;
    _counts.StackDepth = std::max(_counts.StackDepth, entry.Counts.StackDepth);
    _counts.LiquidStackDepth = std::max(_counts.LiquidStackDepth, entry.Counts.LiquidStackDepth);
    _offsets.emplace(model, entry);
    return entry.Offset;
}

uint32_t Animus::GpuVision::ModelPool::SizeOf(VMAP::WorldModel const* model) const
{
    auto const found = _offsets.find(model);
    return found == _offsets.end() ? 0 : found->second.Size;
}

Animus::GpuVision::ModelCounts Animus::GpuVision::ModelPool::CountsOf(VMAP::WorldModel const* model) const
{
    auto const found = _offsets.find(model);
    return found == _offsets.end() ? ModelCounts() : found->second.Counts;
}

bool Animus::GpuVision::StaticScene::Sync(VMAP::StaticMapTree const& tree, LiquidDeadly const& deadly)
{
    bool changed = false;
    uint32_t const count = tree.GetTreeValueCount();
    if (Top.empty() || SlotModels.size() != count)
    {
        SlotTable.assign(count, NO_INSTANCE);
        SlotModels.assign(count, nullptr);
        SlotInstance.assign(count, NO_INSTANCE);
        Instances.clear();
        LoadedSlots = 0;
        changed = true;
    }

    // Every slot against what it was packed with: a tile loaded since fills slots, and none is ever emptied while
    // the tree lives (StaticMapTree::UnloadMapTile keeps its spawns), but a slot is followed either way.
    VMAP::ModelInstance const* values = tree.GetTreeValues();
    for (uint32_t slot = 0; slot < count; ++slot)
    {
        VMAP::WorldModel const* model = values[slot].GetWorldModel();
        if (model == SlotModels[slot])
            continue;
        changed = true;
        SlotModels[slot] = model;
        if (!model)
        {
            SlotTable[slot] = NO_INSTANCE;
            --LoadedSlots;
            continue;
        }
        if (SlotInstance[slot] == NO_INSTANCE)
        {
            SlotInstance[slot] = uint32_t(Instances.size() / INSTANCE_WORDS);
            Instances.resize(Instances.size() + INSTANCE_WORDS, 0);
        }
        uint32_t const offset = Models.Acquire(model, deadly);
        PackInstance(values[slot], offset, &Instances[std::size_t(SlotInstance[slot]) * INSTANCE_WORDS]);
        SlotTable[slot] = SlotInstance[slot];
        ++LoadedSlots;
    }
    if (changed)
        BuildTrees(tree.GetTree());
    return changed;
}

void Animus::GpuVision::StaticScene::BuildTrees(BIH const& tree)
{
    DeepestSlot = NO_INSTANCE;
    uint32_t deepest = 0;
    for (uint32_t slot = 0; slot < SlotTable.size(); ++slot)
        if (SlotTable[slot] != NO_INSTANCE && (DeepestSlot == NO_INSTANCE
            || Models.CountsOf(SlotModels[slot]).StackDepth > deepest))
        {
            DeepestSlot = slot;
            deepest = Models.CountsOf(SlotModels[slot]).StackDepth;
        }

    // The CPU's own tree, its split planes and leaves as they are, with every subtree that holds no spawn a ray
    // could hit made an empty leaf: the walk then meets the loaded spawns in the CPU's order with the CPU's
    // intervals (so the same nearest hit, ties and all), without descending the thousands of subtrees of tiles
    // never loaded. A tree built afresh over the loaded spawns' bounds was as fast but not the same: the CPU's
    // tree file bounds a spawn otherwise, and the two found different triangles at a few pixels.
    auto const build = [&](bool liquidsOnly, Words& top) -> uint32_t
    {
        top.assign(BIH_WORDS, 0);
        PackBih(tree, top, 0);
        uint32_t* nodes = &top[top[BIH_NODES]];
        uint32_t const nodeWords = top[BIH_OBJECTS] - top[BIH_NODES];
        std::vector<uint32_t> const original(nodes, nodes + nodeWords);
        uint32_t const* objects = &top[top[BIH_OBJECTS]];
        uint32_t const objectCount = uint32_t(top.size()) - top[BIH_OBJECTS];
        auto const live = [&](uint32_t object)
        {
            if (object >= SlotTable.size() || SlotTable[object] == NO_INSTANCE)
                return false;
            if (!liquidsOnly)
                return true;
            // ModelInstance::intersectLiquid: an M2 has none, nor has a model without a liquid surface.
            uint32_t const* record = &Instances[std::size_t(SlotTable[object]) * INSTANCE_WORDS];
            return !(record[INSTANCE_FLAGS] & SPAWN_M2) && Models.CountsOf(SlotModels[object]).Liquids > 0;
        };

        // Which nodes hold a live spawn, children before parents (every child lies after its parent).
        std::vector<uint8_t> holds(nodeWords, 0);
        std::vector<uint32_t> order;
        std::vector<uint32_t> open = { 0 };
        while (!open.empty())
        {
            uint32_t const node = open.back();
            open.pop_back();
            if (node + 1 >= nodeWords)
                continue;
            order.push_back(node);
            uint32_t children[2];
            uint32_t const count = RealChildren(original, node, children);
            for (uint32_t i = 0; i < count; ++i)
                if (children[i] > node)
                    open.push_back(children[i]);
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            uint32_t const node = *it;
            uint32_t const tn = original[node];
            uint32_t const offset = tn & ~(7u << 29);
            uint32_t children[2];
            uint32_t const count = RealChildren(original, node, children);
            bool any = false;
            if (!(tn & (1u << 29)) && ((tn & (3u << 30)) >> 30) == 3)
            {
                for (uint32_t i = 0; i < original[node + 1] && offset + i < objectCount && !any; ++i)
                    any = live(objects[offset + i]);
            }
            else
                for (uint32_t i = 0; i < count; ++i)
                    any = any || (children[i] > node && holds[children[i]]);
            holds[node] = any;
        }
        // An empty leaf where nothing is: the walk enters it and leaves (the root too, for a tree with nothing).
        for (uint32_t node : order)
            if (!holds[node])
            {
                nodes[node] = 3u << 30;
                nodes[node + 1] = 0;
            }
        return BihPushDepth(std::vector<uint32_t>(nodes, nodes + nodeWords));
    };
    Slots = SlotTable;
    LiquidSlots = SlotTable;
    TopDepth = build(false, Top);
    LiquidTopDepth = build(true, LiquidTop);
}

uint32_t Animus::GpuVision::StaticScene::StackDepth() const
{
    ModelCounts const& counts = Models.Counts();
    return std::max(TopDepth + counts.StackDepth, LiquidTopDepth + counts.LiquidStackDepth);
}

bool Animus::GpuVision::DoorScene::Sync(DynamicMapTree const& tree, LiquidDeadly const& deadly)
{
    Words records;
    std::vector<void const*> owners;
    std::vector<std::vector<uint32_t>> lists(DOOR_GRID_CELLS);
    uint32_t const before = uint32_t(Models.Data().size());
    tree.VisitModels([&](GameObjectModel const& door, uint16 const* cells, uint32_t cellCount)
    {
        uint32_t const index = uint32_t(records.size() / INSTANCE_WORDS);
        owners.push_back(&door);
        records.resize(records.size() + INSTANCE_WORDS, 0);
        uint32_t const offset = Models.Acquire(door.GetRayView().Model, deadly);
        PackDoor(door, offset, &records[std::size_t(index) * INSTANCE_WORDS]);
        for (uint32_t i = 0; i < cellCount; ++i)
            lists[cells[i]].push_back(index);
    });

    Words cells(DOOR_GRID_CELLS * 2, 0);
    for (uint32_t cell = 0; cell < DOOR_GRID_CELLS; ++cell)
    {
        cells[cell * 2] = uint32_t(cells.size());
        cells[cell * 2 + 1] = uint32_t(lists[cell].size());
        cells.insert(cells.end(), lists[cell].begin(), lists[cell].end());
    }

    bool const changed = records != Records || cells != Cells || Models.Data().size() != before
        || owners != Owners;
    Records.swap(records);
    Cells.swap(cells);
    Owners.swap(owners);
    Count = uint32_t(Records.size() / INSTANCE_WORDS);
    return changed;
}
