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

#ifndef ANIMUS_VISION_BAKED_SCENE_H
#define ANIMUS_VISION_BAKED_SCENE_H

#include <bit>
#include <cstddef>
#include <cstdint>

/// **The baked scene file** (baked-camera plan, section 1.2): one flat, pointer-free, little-endian file per map
/// holding the static world's collision geometry in the camera's world frame, ready to ray cast: triangles in BVH
/// leaf order, their face normals and kinds, the BVH, and the WMO liquid surfaces as triangles with their own BVH.
/// Written by the scene_baker tool, read by BakedWorld. Pure: no core types, so the baker, the tracer and a GPU
/// upload share it.
///
/// Layout: SceneHeader (512 bytes), then the sections the header's table names, each 16-byte aligned, every padding
/// byte zero. The arrays are the ones a GPU kernel would upload as they are.
namespace Animus::Vision::Scene
{
    static_assert(std::endian::native == std::endian::little, "scene files are little-endian");

    constexpr uint32_t MAGIC = 0x43534241;          // "ABSC"
    constexpr uint32_t VERSION = 1;
    constexpr uint32_t HEADER_BYTES = 512;
    constexpr uint32_t SECTION_ALIGN = 16;
    constexpr uint32_t MAX_LEAF = 4;                // triangles in a leaf, at most
    constexpr uint32_t MAX_DEPTH = 62;              // the tracer's stack holds 64 entries

    /// SceneHeader::Flags.
    enum Flag : uint32_t
    {
        HAS_TERRAIN = 1u << 0,          // v1: always 0 (the baker refuses a map with terrain tiles)
        HAS_MODEL_LIQUID = 1u << 1,     // the liquid sections hold at least one triangle
        HAS_TERRAIN_LIQUID = 1u << 2,   // v1: always 0
        HAS_INSTANCES = 1u << 3,        // v1: always 0 (phase 2: door models)
        TILED = 1u << 4                 // the source map is tiled (a continent)
    };

    /// The section table's slots.
    enum Slot : uint32_t
    {
        SLOT_TRI_GEOM = 0,
        SLOT_TRI_NORMAL,
        SLOT_TRI_KIND,
        SLOT_NODES,
        SLOT_LIQ_GEOM,
        SLOT_LIQ_KIND,
        SLOT_LIQ_NODES,
        SLOT_MODEL_TABLE,       // reserved (phase 2)
        SLOT_INSTANCE_TABLE,    // reserved (phase 2)
        SLOT_TERRAIN_INDEX,     // reserved
        SLOT_TERRAIN_HEIGHTS,   // reserved
        SLOT_TERRAIN_LIQUID,    // reserved
        SLOT_COUNT = 16
    };

    /// A triangle's kind byte (SLOT_TRI_KIND): what a player would call it.
    enum Kind : uint8_t
    {
        KIND_WMO = 0,
        KIND_M2 = 1
    };

    /// A liquid triangle's kind byte (SLOT_LIQ_KIND): LiquidType.dbc's Type in bits 0-1 (0 water, 1 ocean, 2 magma,
    /// 3 slime) and the deadly bit.
    constexpr uint8_t LIQUID_TYPE_MASK = 0x03;
    constexpr uint8_t LIQUID_DEADLY = 0x04;

    struct SectionEntry
    {
        uint64_t Offset = 0;        // from the start of the file; 0 for an absent section
        uint64_t Bytes = 0;
    };

    struct SceneHeader
    {
        uint32_t Magic = MAGIC;
        uint32_t Version = VERSION;
        uint32_t HeaderBytes = HEADER_BYTES;
        uint32_t MapId = 0;
        uint32_t Flags = 0;
        uint32_t Reserved0 = 0;
        float SolidMin[3] = {};
        float SolidMax[3] = {};
        float LiquidMin[3] = {};
        float LiquidMax[3] = {};
        uint32_t TriCount = 0;
        uint32_t NodeCount = 0;
        uint32_t LiquidTriCount = 0;
        uint32_t LiquidNodeCount = 0;
        uint32_t ModelCount = 0;            // v1: 0
        uint32_t InstanceCount = 0;         // v1: 0
        uint32_t SourceSpawnCount = 0;      // vmap instances baked (statistics)
        uint32_t SourceM2Count = 0;         // ... of them M2
        uint64_t Checksum = 0;              // FNV-1a 64 of the file with these eight bytes read as zero
        uint8_t Reserved1[144] = {};
        SectionEntry Sections[SLOT_COUNT] = {};
    };
    static_assert(sizeof(SceneHeader) == HEADER_BYTES, "the scene header is 512 bytes");
    static_assert(offsetof(SceneHeader, Checksum) == 104, "the checksum sits at byte 104");
    static_assert(offsetof(SceneHeader, Sections) == 256, "the section table starts at byte 256");

    /// A triangle as the tracer reads it: vertex 0 and the two edges from it, world space.
    struct TriGeom
    {
        float V0[3];
        float E1[3];
        float E2[3];
    };
    static_assert(sizeof(TriGeom) == 36);

    /// A triangle's unit face normal (cold: read on a hit).
    struct TriNormal
    {
        float N[3];
    };
    static_assert(sizeof(TriNormal) == 12);

    /// A BVH node. A leaf has a non-zero count in B's low 16 bits: A is its first triangle. An interior node has
    /// count 0: its left child is the next node, A its right child, and B's bits 16-17 the axis it splits.
    struct Node
    {
        float Min[3];
        uint32_t A;
        float Max[3];
        uint32_t B;
    };
    static_assert(sizeof(Node) == 32);

    [[nodiscard]] constexpr uint32_t Count(Node const& node) { return node.B & 0xFFFFu; }
    [[nodiscard]] constexpr uint32_t Axis(Node const& node) { return (node.B >> 16) & 3u; }

    /// FNV-1a 64 over `bytes`, continuing `hash`.
    [[nodiscard]] constexpr uint64_t Fnv1a(uint8_t const* data, std::size_t bytes,
        uint64_t hash = 0xcbf29ce484222325ull)
    {
        for (std::size_t i = 0; i < bytes; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    /// The file's checksum: FNV-1a 64 over every byte, the header's checksum field taken as zero.
    [[nodiscard]] inline uint64_t ChecksumOf(uint8_t const* file, std::size_t bytes)
    {
        constexpr std::size_t at = offsetof(SceneHeader, Checksum);
        if (bytes < at + sizeof(uint64_t))
            return 0;
        uint8_t const zero[sizeof(uint64_t)] = {};
        uint64_t hash = Fnv1a(file, at);
        hash = Fnv1a(zero, sizeof(zero), hash);
        return Fnv1a(file + at + sizeof(uint64_t), bytes - at - sizeof(uint64_t), hash);
    }
}

#endif
