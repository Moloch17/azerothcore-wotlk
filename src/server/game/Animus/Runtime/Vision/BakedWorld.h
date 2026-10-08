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

#ifndef ANIMUS_VISION_BAKED_WORLD_H
#define ANIMUS_VISION_BAKED_WORLD_H

#include "BakedScene.h"
#include "VisionCaster.h"
#include <cstdint>
#include <string>
#include <vector>

/// **The baked static world** (baked-camera plan, section 1.3): a scene file (BakedScene.h) loaded read-only, and the
/// static half of VisionWorld over it -- the nearest and any-hit solid along a segment, the first liquid surface, and
/// the terrain grids' extent. A purpose-built tracer: one flat BVH over world-space triangles, an iterative
/// near-child-first traversal with a stack, a two-sided Moller-Trumbore on (vertex, edge, edge), and nothing nested: no
/// per-instance ray transform, no tree of trees. Pure over the file: no core types. The dynamic half (doors) and the
/// controller's queries stay with the world it is composed into.
namespace Animus::Vision
{
    /// Barycentric slack of the triangle test: a triangle counts as hit within this share of its size outside its
    /// edges. The collision meshes are not watertight (WMO groups leave gaps of a few millimetres and T-junctions
    /// along shared edges), so an exact test lets a ray through a crack of a solid wall; this closes cracks up to
    /// three thousandths of a triangle's size and makes a hit grazing an edge count, which no one can see.
    constexpr float BAKED_EDGE_SLACK = 3e-3f;

    class BakedWorld
    {
    public:
        /// Reads and checks the scene file at `path` (one read). False, with `error` set, when it is missing,
        /// damaged, of another version, fails its checksum, or uses a feature this reader does not have.
        [[nodiscard]] bool Load(std::string const& path, std::string& error);
        [[nodiscard]] bool Loaded() const { return _header != nullptr; }

        /// As StaticMapTree through the camera: the nearest solid on the segment (Distance < 0 for none, NormalZ the
        /// hit triangle's, turned to face the segment's start).
        [[nodiscard]] SurfaceHit StaticHit(Vec3 from, Vec3 to) const;
        /// Whether anything solid lies on the segment (stops at the first thing found).
        [[nodiscard]] bool StaticAnyHit(Vec3 from, Vec3 to) const;
        /// The nearest liquid surface on the segment, either side (the caller casts it for descending rays only).
        [[nodiscard]] LiquidHit ModelLiquid(Vec3 from, Vec3 to) const;
        /// Loaded for the terrain grids the scene's footprint overlaps (a ray leaves the world there), never with
        /// heights or liquid: a scene of this version has no terrain.
        [[nodiscard]] TerrainTile Tile(int32_t tileX, int32_t tileY) const;
        /// No solid terrain cell, no liquid: Tile never offers one.
        [[nodiscard]] TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY, bool liquid) const;

        /// Whether the scene has any liquid triangle (its header flag): a cast with none is skipped by the caller.
        [[nodiscard]] bool HasLiquid() const { return _header && (_header->Flags & Scene::HAS_MODEL_LIQUID) != 0; }
        [[nodiscard]] Scene::SceneHeader const& Header() const { return *_header; }
        [[nodiscard]] std::size_t FileBytes() const { return _bytes; }

        /// The raw arrays, for the bake check and the brute-force verification.
        [[nodiscard]] Scene::TriGeom const* Triangles() const { return _tris; }
        [[nodiscard]] Scene::TriNormal const* Normals() const { return _normals; }
        [[nodiscard]] uint8_t const* Kinds() const { return _kinds; }
        [[nodiscard]] Scene::TriGeom const* LiquidTriangles() const { return _liqTris; }
        [[nodiscard]] uint8_t const* LiquidKinds() const { return _liqKinds; }

    private:
        std::vector<uint32_t> _buffer;
        std::size_t _bytes = 0;
        Scene::SceneHeader const* _header = nullptr;
        Scene::TriGeom const* _tris = nullptr;
        Scene::TriNormal const* _normals = nullptr;
        uint8_t const* _kinds = nullptr;
        Scene::Node const* _nodes = nullptr;
        Scene::TriGeom const* _liqTris = nullptr;
        uint8_t const* _liqKinds = nullptr;
        Scene::Node const* _liqNodes = nullptr;
        int32_t _tileLow[2] = {};
        int32_t _tileHigh[2] = {};
    };
}

#endif
