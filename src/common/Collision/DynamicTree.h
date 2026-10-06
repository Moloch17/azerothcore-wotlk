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

#ifndef _DYNTREE_H
#define _DYNTREE_H

#include "Define.h"
#include "Optional.h"
#include <functional>

namespace G3D
{
    class Ray;
    class Vector3;
}

namespace VMAP
{
    struct AreaAndLiquidData;
    enum class ModelIgnoreFlags : uint32;
}

class GameObjectModel;
struct DynTreeImpl;

class DynamicMapTree
{
    DynTreeImpl* impl;

public:
    DynamicMapTree();
    ~DynamicMapTree();

    [[nodiscard]] bool isInLineOfSight(float x1, float y1, float z1, float x2, float y2, float z2, uint32 phasemask, VMAP::ModelIgnoreFlags ignoreFlags) const;

    /// `normal`, when given, takes the nearest hit triangle's normal (unnormalised, either side), and is left alone
    /// without a hit.
    bool GetIntersectionTime(uint32 phasemask, G3D::Ray const& ray, G3D::Vector3 const& endPos, float& maxDist,
        G3D::Vector3* normal = nullptr) const;

    bool GetAreaAndLiquidData(float x, float y, float z, uint32 phasemask, Optional<uint8> reqLiquidType, VMAP::AreaAndLiquidData& data) const;

    bool GetObjectHitPos(uint32 phasemask, G3D::Vector3 const& pPos1,
                         G3D::Vector3 const& pPos2, G3D::Vector3& pResultHitPos,
                         float pModifyDist) const;

    [[nodiscard]] float getHeight(float x, float y, float z, float maxSearchDist, uint32 phasemask) const;

    void insert(GameObjectModel const&);
    void remove(GameObjectModel const&);
    [[nodiscard]] bool contains(GameObjectModel const&) const;
    [[nodiscard]] int size() const;

    void balance();
    void update(uint32 diff);

    /// Every model in the tree with the cells (x * 64 + y of its 64 x 64 grid, up to 9) it was filed under, which
    /// are the cells a ray's walk tests it in. Read-only: the bots' camera copies them to the GPU
    /// (Animus/Gpu/VisionScene).
    /// It iterates the tree's own tables, which inserts, removes and rebalances change, so it may only run while
    /// the map that owns the tree is not updating: from an idle console now, and after the map update's join (on the
    /// world thread) once the GPU camera runs in training (camera-vision.GPU.md, G3). Never from a map thread.
    void VisitModels(std::function<void(GameObjectModel const&, uint16 const* cells, uint32 count)> const& visit) const;
};

#endif // _DYNTREE_H
