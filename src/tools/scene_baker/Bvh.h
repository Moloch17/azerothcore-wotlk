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

#ifndef SCENE_BAKER_BVH_H
#define SCENE_BAKER_BVH_H

#include "BakedScene.h"
#include <cstdint>
#include <vector>

namespace SceneBaker
{
    /// A triangle's world-space vertices, as the baker holds them.
    struct Triangle
    {
        float V[3][3];
        uint8_t Kind = 0;       // Scene::Kind for a solid, the liquid kind byte for a liquid
    };

    /// The BVH over a triangle list: `Order[i]` is the input triangle that ends up i-th (leaf order), and `Nodes` are
    /// laid out depth first (a node's left child follows it), as Scene::Node describes. Deterministic: a pure function
    /// of the input list (binned SAH, a hand-written partition, a total order for the fallback split).
    struct Bvh
    {
        std::vector<Animus::Vision::Scene::Node> Nodes;
        std::vector<uint32_t> Order;
        uint32_t MaxDepth = 0;
        float Min[3] = {};          // the triangles' own box (not padded)
        float Max[3] = {};
    };

    /// Node boxes are the triangles' boxes widened by this much, so a ray along a face the triangle lies flat in still
    /// enters the box.
    constexpr float BOX_PAD = 1e-4f;

    [[nodiscard]] Bvh BuildBvh(std::vector<Triangle> const& triangles);
}

#endif
