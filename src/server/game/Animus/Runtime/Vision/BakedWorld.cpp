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

#include "BakedWorld.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstring>
#include <string>

namespace
{
    namespace Sc = Animus::Vision::Scene;
    using namespace Animus::Vision;

    constexpr float EDGE_SLACK = BAKED_EDGE_SLACK;
    constexpr std::size_t STACK_SIZE = 64;

    /// A segment as the traversal reads it: origin, unit direction, its reciprocal, and whether each axis runs down.
    struct RayData
    {
        float O[3];
        float D[3];
        float Inv[3];
        bool Down[3];
    };

    [[nodiscard]] bool MakeRay(Vec3 from, Vec3 to, RayData& ray, float& length)
    {
        float const dx = to.X - from.X;
        float const dy = to.Y - from.Y;
        float const dz = to.Z - from.Z;
        length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(length > 1e-6f) || !std::isfinite(length))
            return false;
        ray.O[0] = from.X;
        ray.O[1] = from.Y;
        ray.O[2] = from.Z;
        ray.D[0] = dx / length;
        ray.D[1] = dy / length;
        ray.D[2] = dz / length;
        for (int i = 0; i < 3; ++i)
        {
            // A direction of zero stands for a huge reciprocal, not an infinity (0 * inf is NaN in the slab test).
            float const d = std::fabs(ray.D[i]) < 1e-20f ? (ray.D[i] < 0.0f ? -1e-20f : 1e-20f) : ray.D[i];
            ray.Inv[i] = 1.0f / d;
            ray.Down[i] = ray.D[i] < 0.0f;
        }
        return true;
    }

    /// Whether the ray enters the box within [0, tmax].
    [[nodiscard]] inline bool Slab(Sc::Node const& node, RayData const& ray, float tmax)
    {
        float tn = 0.0f;
        float tf = tmax;
        for (int i = 0; i < 3; ++i)
        {
            float const t0 = (node.Min[i] - ray.O[i]) * ray.Inv[i];
            float const t1 = (node.Max[i] - ray.O[i]) * ray.Inv[i];
            float const lo = t0 < t1 ? t0 : t1;
            float const hi = t0 < t1 ? t1 : t0;
            tn = lo > tn ? lo : tn;
            tf = hi < tf ? hi : tf;
        }
        return tn <= tf;
    }

    /// Moller-Trumbore, two-sided: where the ray crosses the triangle, or -1.
    [[nodiscard]] inline float Cross(RayData const& ray, Sc::TriGeom const& tri)
    {
        float const* d = ray.D;
        float const* e1 = tri.E1;
        float const* e2 = tri.E2;
        float const p0 = d[1] * e2[2] - d[2] * e2[1];
        float const p1 = d[2] * e2[0] - d[0] * e2[2];
        float const p2 = d[0] * e2[1] - d[1] * e2[0];
        float const det = e1[0] * p0 + e1[1] * p1 + e1[2] * p2;
        if (std::fabs(det) < 1e-12f)
            return -1.0f;
        float const inv = 1.0f / det;
        float const s0 = ray.O[0] - tri.V0[0];
        float const s1 = ray.O[1] - tri.V0[1];
        float const s2 = ray.O[2] - tri.V0[2];
        float const u = (s0 * p0 + s1 * p1 + s2 * p2) * inv;
        if (u < -EDGE_SLACK || u > 1.0f + EDGE_SLACK)
            return -1.0f;
        float const q0 = s1 * e1[2] - s2 * e1[1];
        float const q1 = s2 * e1[0] - s0 * e1[2];
        float const q2 = s0 * e1[1] - s1 * e1[0];
        float const v = (d[0] * q0 + d[1] * q1 + d[2] * q2) * inv;
        if (v < -EDGE_SLACK || u + v > 1.0f + EDGE_SLACK)
            return -1.0f;
        return (e2[0] * q0 + e2[1] * q1 + e2[2] * q2) * inv;
    }

    /// The nearest triangle within `tmax` (shrunk to it), or all the way down to the first found when `any`.
    template <bool Any>
    [[nodiscard]] bool Trace(Sc::Node const* nodes, uint32_t nodeCount, Sc::TriGeom const* tris, RayData const& ray,
        float& tmax, uint32_t& hit)
    {
        if (!nodeCount)
            return false;
        uint32_t stack[STACK_SIZE];
        std::size_t top = 0;
        uint32_t index = 0;
        bool found = false;
        while (true)
        {
            Sc::Node const& node = nodes[index];
            if (Slab(node, ray, tmax))
            {
                uint32_t const count = Sc::Count(node);
                if (count)
                {
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        float const t = Cross(ray, tris[node.A + i]);
                        if (t >= 0.0f && t <= tmax)
                        {
                            tmax = t;
                            hit = node.A + i;
                            found = true;
                            if constexpr (Any)
                                return true;
                        }
                    }
                }
                else if (top < STACK_SIZE)
                {
                    // Near child first: the side of the split the ray starts on.
                    bool const rightFirst = ray.Down[Sc::Axis(node)];
                    stack[top++] = rightFirst ? index + 1 : node.A;
                    index = rightFirst ? node.A : index + 1;
                    continue;
                }
            }
            if (!top)
                break;
            index = stack[--top];
        }
        return found;
    }

    [[nodiscard]] bool ValidNodes(Sc::Node const* nodes, uint32_t nodeCount, uint32_t triCount)
    {
        for (uint32_t i = 0; i < nodeCount; ++i)
        {
            Sc::Node const& node = nodes[i];
            uint32_t const count = Sc::Count(node);
            if (count)
            {
                if (count > Sc::MAX_LEAF || uint64_t(node.A) + count > triCount)
                    return false;
            }
            else if (i + 1 >= nodeCount || node.A <= i || node.A >= nodeCount)
                return false;
        }
        return true;
    }
}

bool Animus::Vision::BakedWorld::Load(std::string const& path, std::string& error)
{
    *this = BakedWorld();
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
    {
        error = "cannot open " + path;
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    long const size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < long(Sc::HEADER_BYTES))
    {
        std::fclose(file);
        error = path + " is too short for a scene";
        return false;
    }
    _buffer.assign((std::size_t(size) + 3) / 4, 0u);
    bool const read = std::fread(_buffer.data(), 1, std::size_t(size), file) == std::size_t(size);
    std::fclose(file);
    if (!read)
    {
        error = "cannot read " + path;
        *this = BakedWorld();
        return false;
    }
    _bytes = std::size_t(size);

    uint8_t const* base = reinterpret_cast<uint8_t const*>(_buffer.data());
    Sc::SceneHeader const* header = reinterpret_cast<Sc::SceneHeader const*>(base);
    auto const fail = [&](std::string const& why)
    {
        error = path + ": " + why;
        *this = BakedWorld();
        return false;
    };
    if (header->Magic != Sc::MAGIC)
        return fail("not a scene file");
    if (header->Version != Sc::VERSION || header->HeaderBytes != Sc::HEADER_BYTES)
        return fail("scene version " + std::to_string(header->Version) + ", this reader has " +
            std::to_string(Sc::VERSION));
    if (header->Flags & (Sc::HAS_TERRAIN | Sc::HAS_TERRAIN_LIQUID | Sc::HAS_INSTANCES))
        return fail("the scene has terrain or instances, which this reader does not have");
    if (header->Checksum != Sc::ChecksumOf(base, _bytes))
        return fail("checksum mismatch (damaged or truncated)");

    // A section must lie inside the file and hold exactly its count of elements.
    auto const section = [&](Sc::Slot slot, uint64_t count, std::size_t element, void const*& out)
    {
        Sc::SectionEntry const& entry = header->Sections[slot];
        out = nullptr;
        if (!count)
            return entry.Bytes == 0;
        if (entry.Bytes != count * element || entry.Offset % 4 != 0 || entry.Offset < Sc::HEADER_BYTES
            || entry.Offset + entry.Bytes > _bytes)
            return false;
        out = base + entry.Offset;
        return true;
    };
    void const* tris = nullptr;
    void const* normals = nullptr;
    void const* kinds = nullptr;
    void const* nodes = nullptr;
    void const* liqTris = nullptr;
    void const* liqKinds = nullptr;
    void const* liqNodes = nullptr;
    if (!section(Sc::SLOT_TRI_GEOM, header->TriCount, sizeof(Sc::TriGeom), tris)
        || !section(Sc::SLOT_TRI_NORMAL, header->TriCount, sizeof(Sc::TriNormal), normals)
        || !section(Sc::SLOT_TRI_KIND, header->TriCount, 1, kinds)
        || !section(Sc::SLOT_NODES, header->NodeCount, sizeof(Sc::Node), nodes)
        || !section(Sc::SLOT_LIQ_GEOM, header->LiquidTriCount, sizeof(Sc::TriGeom), liqTris)
        || !section(Sc::SLOT_LIQ_KIND, header->LiquidTriCount, 1, liqKinds)
        || !section(Sc::SLOT_LIQ_NODES, header->LiquidNodeCount, sizeof(Sc::Node), liqNodes))
        return fail("a section is missing, misplaced or the wrong size");
    if (!ValidNodes(static_cast<Sc::Node const*>(nodes), header->NodeCount, header->TriCount)
        || !ValidNodes(static_cast<Sc::Node const*>(liqNodes), header->LiquidNodeCount, header->LiquidTriCount))
        return fail("the BVH is inconsistent");

    _header = header;
    _tris = static_cast<Sc::TriGeom const*>(tris);
    _normals = static_cast<Sc::TriNormal const*>(normals);
    _kinds = static_cast<uint8_t const*>(kinds);
    _nodes = static_cast<Sc::Node const*>(nodes);
    _liqTris = static_cast<Sc::TriGeom const*>(liqTris);
    _liqKinds = static_cast<uint8_t const*>(liqKinds);
    _liqNodes = static_cast<Sc::Node const*>(liqNodes);

    // The grids the footprint overlaps: u runs down as x runs up.
    bool const liquid = header->LiquidTriCount != 0;
    float const lowX = std::min(header->SolidMin[0], liquid ? header->LiquidMin[0] : header->SolidMin[0]);
    float const highX = std::max(header->SolidMax[0], liquid ? header->LiquidMax[0] : header->SolidMax[0]);
    float const lowY = std::min(header->SolidMin[1], liquid ? header->LiquidMin[1] : header->SolidMin[1]);
    float const highY = std::max(header->SolidMax[1], liquid ? header->LiquidMax[1] : header->SolidMax[1]);
    _tileLow[0] = int32_t(std::floor(GridU(highX) / float(GRID_CELLS)));
    _tileHigh[0] = int32_t(std::floor(GridU(lowX) / float(GRID_CELLS)));
    _tileLow[1] = int32_t(std::floor(GridU(highY) / float(GRID_CELLS)));
    _tileHigh[1] = int32_t(std::floor(GridU(lowY) / float(GRID_CELLS)));
    return true;
}

Animus::Vision::SurfaceHit Animus::Vision::BakedWorld::StaticHit(Vec3 from, Vec3 to) const
{
    SurfaceHit result;
    RayData ray;
    float length = 0.0f;
    if (!_header || !MakeRay(from, to, ray, length))
        return result;
    uint32_t hit = 0;
    if (!Trace<false>(_nodes, _header->NodeCount, _tris, ray, length, hit))
        return result;
    Sc::TriNormal const& n = _normals[hit];
    float const along = n.N[0] * ray.D[0] + n.N[1] * ray.D[1] + n.N[2] * ray.D[2];
    result.Distance = length;
    result.NormalZ = along > 0.0f ? -n.N[2] : n.N[2];
    return result;
}

bool Animus::Vision::BakedWorld::StaticAnyHit(Vec3 from, Vec3 to) const
{
    RayData ray;
    float length = 0.0f;
    if (!_header || !MakeRay(from, to, ray, length))
        return false;
    uint32_t hit = 0;
    return Trace<true>(_nodes, _header->NodeCount, _tris, ray, length, hit);
}

Animus::Vision::LiquidHit Animus::Vision::BakedWorld::ModelLiquid(Vec3 from, Vec3 to) const
{
    LiquidHit result;
    RayData ray;
    float length = 0.0f;
    if (!HasLiquid() || !MakeRay(from, to, ray, length))
        return result;
    uint32_t hit = 0;
    if (!Trace<false>(_liqNodes, _header->LiquidNodeCount, _liqTris, ray, length, hit))
        return result;
    result.Distance = length;
    result.Deadly = (_liqKinds[hit] & Sc::LIQUID_DEADLY) != 0;
    return result;
}

Animus::Vision::TerrainTile Animus::Vision::BakedWorld::Tile(int32_t tileX, int32_t tileY) const
{
    TerrainTile tile;
    tile.Loaded = _header && tileX >= _tileLow[0] && tileX <= _tileHigh[0] && tileY >= _tileLow[1]
        && tileY <= _tileHigh[1];
    return tile;
}

Animus::Vision::TerrainCell Animus::Vision::BakedWorld::Cell(int32_t /*tileX*/, int32_t /*tileY*/,
    int32_t /*cellX*/, int32_t /*cellY*/, bool /*liquid*/) const
{
    return TerrainCell();
}
