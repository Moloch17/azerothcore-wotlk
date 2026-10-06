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

#ifndef ANIMUS_GPU_VISION_SCENE_H
#define ANIMUS_GPU_VISION_SCENE_H

#include "VisionDevice.h"
#include <functional>
#include <unordered_map>
#include <vector>

class BIH;
class DynamicMapTree;
class GameObjectModel;

namespace VMAP
{
    class ModelInstance;
    class StaticMapTree;
    class WorldModel;
}

namespace Animus::Vision
{
    class VisionWorld;
}

/// **The GPU camera's scene, packed on the CPU** (camera-vision.GPU.md, G1): the words VisionDevice.h lays out,
/// copied from what the server has loaded -- nothing read from or written to disk, nothing baked. Two levels, as the
/// CPU walks them: the static tree's BIH over its spawns, each spawn a transform over its model, each model packed
/// once (its group BIH, its groups' mesh BIHs, triangles, vertices and liquids) in its own space and shared by every
/// spawn of it. The terrain grids are packed from a VisionWorld's own Tile and Cell, so they hold exactly what the
/// CPU caster reads. Pure CPU, no device: the device copies are VisionGpu's.
namespace Animus::GpuVision
{
    using Words = std::vector<uint32_t>;
    /// A WMO liquid's LiquidType is magma or slime (LiquidType.dbc; MapVisionWorld::ModelLiquid's test).
    using LiquidDeadly = std::function<bool(uint32_t type)>;

    /// The record of `tree` at `record` (BIH_WORDS reserved there by the caller), its arrays appended to `out`.
    void PackBih(BIH const& tree, Words& out, uint32_t record);

    /// What a packed model holds.
    struct ModelCounts
    {
        uint32_t Groups = 0;
        uint32_t Triangles = 0;
        uint32_t Vertices = 0;
        uint32_t BihNodeWords = 0;
        uint32_t Liquids = 0;
        /// The most entries a ray's walk of the model holds on the shared stack at once: its group tree's push
        /// depth (none with one group, which is walked straight) plus its deepest mesh tree's. Its liquid walk
        /// needs the group tree's alone (LiquidStackDepth).
        uint32_t StackDepth = 0;
        uint32_t LiquidStackDepth = 0;
    };

    /// The most entries BIH::intersectRay's stack can hold walking `tree`: a node is pushed only where the ray
    /// passes both children of a split, and stays only while the walk is below that split, so the deepest count of
    /// splits on a path from the root bounds it (BVH2 nodes and leaves push nothing).
    [[nodiscard]] uint32_t BihPushDepth(BIH const& tree);
    /// ... over a packed node array (BIH::GetNodes' layout).
    [[nodiscard]] uint32_t BihPushDepth(std::vector<uint32_t> const& nodes);

    /// `model` appended to `pool` (never at word 0): returns its offset.
    uint32_t PackModel(VMAP::WorldModel const& model, Words& pool, LiquidDeadly const& deadly,
        ModelCounts* counts = nullptr);

    /// A static spawn's or a door's record (INSTANCE_WORDS at `record`).
    void PackInstance(VMAP::ModelInstance const& spawn, uint32_t modelOffset, uint32_t* record);
    void PackDoor(GameObjectModel const& door, uint32_t modelOffset, uint32_t* record);

    /// Terrain grid (tileX, tileY) as `world` reads it: false, with `out` empty, when the grid is not created or has
    /// neither heights nor liquid (the kernel's NoTerrain); else TERRAIN_WORDS in `out`.
    bool PackTerrain(Vision::VisionWorld const& world, int32_t tileX, int32_t tileY, Words& out);

    /// The models a scene's spawns or doors use, each packed once, appended as they first appear.
    class ModelPool
    {
    public:
        ModelPool() : _words(1, 0) { }
        /// The model's offset, packing it the first time.
        uint32_t Acquire(VMAP::WorldModel const* model, LiquidDeadly const& deadly);
        [[nodiscard]] Words const& Data() const { return _words; }
        [[nodiscard]] uint32_t Models() const { return uint32_t(_offsets.size()); }
        /// Counts summed over the models, but for the stack depths: the deepest of them.
        [[nodiscard]] ModelCounts const& Counts() const { return _counts; }
        /// A packed model's size in words, and its counts.
        [[nodiscard]] uint32_t SizeOf(VMAP::WorldModel const* model) const;
        [[nodiscard]] ModelCounts CountsOf(VMAP::WorldModel const* model) const;

    private:
        struct Entry
        {
            uint32_t Offset = 0;
            uint32_t Size = 0;
            ModelCounts Counts;
        };
        Words _words;
        std::unordered_map<VMAP::WorldModel const*, Entry> _offsets;
        ModelCounts _counts;
    };

    /// A static tree's packing: each slot's instance (SlotTable, NO_INSTANCE where the spawn has no model), the
    /// instances and their models, and the CPU tree twice, pruned: Top with every subtree that holds no loaded
    /// spawn made an empty leaf, LiquidTop the same for the spawns with a WMO liquid (Slots and LiquidSlots map
    /// their objects to instances). The CPU tree is built over every spawn of the map, most of them in tiles never
    /// loaded, and a ray walks all of their leaves; the pruned trees walk only what a ray can hit, in the CPU's
    /// order. Sync follows the tree: a spawn whose tile has loaded since is added, and both are pruned again.
    class StaticScene
    {
    public:
        /// Brings the packing up to `tree`: true when anything changed.
        bool Sync(VMAP::StaticMapTree const& tree, LiquidDeadly const& deadly);

        Words Top;
        Words Slots;
        Words LiquidTop;
        Words LiquidSlots;
        Words SlotTable;
        Words Instances;
        ModelPool Models;
        /// The model each slot was packed with (null: none), and its instance's index.
        std::vector<VMAP::WorldModel const*> SlotModels;
        std::vector<uint32_t> SlotInstance;
        uint32_t LoadedSlots = 0;
        /// The two top trees' push depths (BihPushDepth), and the deepest model under each: the stack a ray's
        /// static walk can need is TopDepth plus the deepest model's StackDepth (StackDepth below), its liquid
        /// walk LiquidTopDepth plus the deepest LiquidStackDepth.
        uint32_t TopDepth = 0;
        uint32_t LiquidTopDepth = 0;
        [[nodiscard]] uint32_t StackDepth() const;
        /// A loaded spawn of the deepest model (its slot), for the warning that names where it is.
        uint32_t DeepestSlot = NO_INSTANCE;

    private:
        void BuildTrees(BIH const& tree);
    };

    /// A map instance's doors (its dynamic tree's game object models): their records, the 64 x 64 cell table (an
    /// offset and a count a cell, then the door indices) and their models. Rebuilt on every Sync, and reported
    /// changed only when the words differ (a door opened, shut, moved, spawned or despawned).
    class DoorScene
    {
    public:
        bool Sync(DynamicMapTree const& tree, LiquidDeadly const& deadly);

        Words Records;
        Words Cells;
        ModelPool Models;
        /// Each record's GameObjectModel: what a frame's DoorShapes name (perception-goals 1a).
        std::vector<void const*> Owners;
        uint32_t Count = 0;
        /// A door's walk needs its model's stack alone (the cells are a list, not a tree).
        [[nodiscard]] uint32_t StackDepth() const { return Models.Counts().StackDepth; }
    };
}

#endif
