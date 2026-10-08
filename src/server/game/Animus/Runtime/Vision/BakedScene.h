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
    constexpr uint32_t VERSION = 3;
    constexpr uint32_t HEADER_BYTES = 512;
    constexpr uint32_t SECTION_ALIGN = 16;
    constexpr uint32_t MAX_LEAF = 4;                // triangles in a leaf, at most
    constexpr uint32_t MAX_DEPTH = 62;              // the tracer's stack holds 64 entries

    /// SceneHeader::Flags.
    enum Flag : uint32_t
    {
        HAS_TERRAIN = 1u << 0,          // the terrain index holds at least one tile (a .map file of the map)
        HAS_MODEL_LIQUID = 1u << 1,     // the liquid sections hold at least one triangle
        HAS_TERRAIN_LIQUID = 1u << 2,   // at least one terrain tile has liquid
        HAS_INSTANCES = 1u << 3,        // always 0 (phase 2: door models)
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
        SLOT_TERRAIN_INDEX,     // TerrainRec per tile, sorted by (TileX, TileY)
        SLOT_TERRAIN_HEIGHTS,   // floats: per height-bearing, non-flat tile, V9 (129 x 129) then V8 (128 x 128)
        SLOT_TERRAIN_LIQUID,    // per liquid tile: float Level[128 * 128], then uint8 Kind[128 * 128]
        SLOT_TERRAIN_HOLES,     // uint16 [256] per tile with holes (the map file's hole words)
        SLOT_TERRAIN_BLOCKS,    // BlockRange [256] per height-bearing, non-flat tile (v3): ground heights per block
        SLOT_TERRAIN_LIQUID_BLOCKS, // BlockRange [256] per liquid tile (v3): liquid levels per block
        SLOT_SOURCE_MODELS,     // bytes (v3): the source model files read below vmaps/, each NUL-ended
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

    /// One terrain tile (a grid of 128 x 128 cells, the map file of the same name). Heights are the file's, decoded to
    /// float at bake: V9 indexed x * 129 + y, V8 x * 128 + y, as the core's GridTerrainData indexes them.
    enum TerrainFlag : uint32_t
    {
        TERRAIN_HEIGHTS = 1u << 0,      // the file has a height section (an absent one is a tile with no ground)
        TERRAIN_FLAT = 1u << 1,         // heights are all FlatHeight (no arrays); holes are not read, as the core
        TERRAIN_HOLES = 1u << 2,        // HoleIndex names this tile's hole words
        TERRAIN_LIQUID = 1u << 3        // LiquidIndex names this tile's liquid
    };

    constexpr uint32_t TERRAIN_CELLS = 128;
    /// The tile's culling hierarchy (v3): 16 x 16 blocks of 8 x 8 cells, block (bx, by) at index bx * 16 + by with
    /// bx = cellX / 8 and by = cellY / 8 (the same axes as the cells).
    constexpr uint32_t TERRAIN_BLOCK_CELLS = 8;
    constexpr uint32_t TERRAIN_BLOCKS_SIDE = TERRAIN_CELLS / TERRAIN_BLOCK_CELLS;
    constexpr uint32_t TERRAIN_BLOCK_COUNT = TERRAIN_BLOCKS_SIDE * TERRAIN_BLOCKS_SIDE;
    /// The reader derives one level above them (not stored): 4 x 4 super-blocks of 4 x 4 blocks to a tile.
    constexpr uint32_t TERRAIN_SUPER_BLOCKS = 4;
    constexpr uint32_t TERRAIN_SUPERS_SIDE = TERRAIN_BLOCKS_SIDE / TERRAIN_SUPER_BLOCKS;
    constexpr uint32_t TERRAIN_SUPER_COUNT = TERRAIN_SUPERS_SIDE * TERRAIN_SUPERS_SIDE;
    constexpr uint32_t TERRAIN_V9 = 129;
    constexpr uint32_t TERRAIN_NONE = 0xFFFFFFFFu;
    constexpr uint8_t TERRAIN_LIQUID_NONE = 0;
    constexpr uint8_t TERRAIN_LIQUID_WATER = 1;
    constexpr uint8_t TERRAIN_LIQUID_DEADLY = 3;    // magma or slime (bit 0 is "has liquid", bit 1 deadly)

    struct TerrainRec
    {
        int32_t TileX;
        int32_t TileY;
        uint32_t Flags;
        float MaxHeight;            // the tile's highest point, as the map file's height header has it (the walk's
                                    // tile test: unchanged since v1)
        float FlatHeight;           // the height of a flat tile
        uint32_t HeightIndex;       // tile number in SLOT_TERRAIN_HEIGHTS, or TERRAIN_NONE
        uint32_t HoleIndex;         // tile number in SLOT_TERRAIN_HOLES, or TERRAIN_NONE
        uint32_t LiquidIndex;       // tile number in SLOT_TERRAIN_LIQUID, or TERRAIN_NONE
        float MinHeight;            // v3: the tile's lowest point of its height arrays (FlatHeight for a flat tile)
    };
    static_assert(sizeof(TerrainRec) == 36);

    /// The lowest and highest height (or liquid level) over a block's cells, with the cells' shared edges: every
    /// corner of its 9 x 9 vertices and centre of its 8 x 8 cells, holes included (a bound, so conservative). A
    /// block with no liquid reads Min = FLT_MAX, Max = -FLT_MAX (BLOCK_EMPTY_*), which no range overlaps.
    struct BlockRange
    {
        float Min;
        float Max;
    };
    static_assert(sizeof(BlockRange) == 8);
    constexpr float BLOCK_EMPTY_MIN = 3.4028234663852886e38f;
    constexpr float BLOCK_EMPTY_MAX = -3.4028234663852886e38f;

    constexpr std::size_t TERRAIN_HEIGHT_FLOATS = TERRAIN_V9 * TERRAIN_V9 + TERRAIN_CELLS * TERRAIN_CELLS;
    constexpr std::size_t TERRAIN_LIQUID_BYTES = TERRAIN_CELLS * TERRAIN_CELLS * (sizeof(float) + 1);

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
        uint32_t BakerVersion = 0;          // SceneBaker::BAKER_VERSION that wrote the file
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
        uint64_t SourceHash = 0;            // FNV-1a 64 over the contents of every source file the bake read
        uint32_t TerrainTileCount = 0;
        uint32_t Reserved2 = 0;
        float TerrainHeightMin = 0.0f;      // v3: the lowest / highest ground height of any height-bearing tile
        float TerrainHeightMax = 0.0f;      // (from the arrays; a flat tile's FlatHeight); 0, 0 with none
        float TerrainLiquidMin = 0.0f;      // v3: the lowest / highest level of any terrain liquid cell; 0, 0 with none
        float TerrainLiquidMax = 0.0f;
        uint64_t SourceFilesDigest = 0;     // v3: FNV-1a 64 over the (name, size, mtime ns) of every source file
        uint8_t Reserved1[104] = {};
        SectionEntry Sections[SLOT_COUNT] = {};
    };
    static_assert(sizeof(SceneHeader) == HEADER_BYTES, "the scene header is 512 bytes");
    static_assert(offsetof(SceneHeader, Checksum) == 104, "the checksum sits at byte 104");
    static_assert(offsetof(SceneHeader, SourceHash) == 112, "the source hash sits at byte 112");
    static_assert(offsetof(SceneHeader, SourceFilesDigest) == 144, "the source files digest sits at byte 144");
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

    /// The file's checksum: FNV-1a 64 over every byte, the header's checksum field and its source files digest taken
    /// as zero. The digest is the one thing a file carries that differs between machines holding the same extraction
    /// (it holds file times), so it stays out of the checksum the cluster compares.
    [[nodiscard]] inline uint64_t ChecksumOf(uint8_t const* file, std::size_t bytes)
    {
        constexpr std::size_t first = offsetof(SceneHeader, Checksum);
        constexpr std::size_t second = offsetof(SceneHeader, SourceFilesDigest);
        static_assert(first + sizeof(uint64_t) <= second);
        if (bytes < second + sizeof(uint64_t))
            return 0;
        uint8_t const zero[sizeof(uint64_t)] = {};
        uint64_t hash = Fnv1a(file, first);
        hash = Fnv1a(zero, sizeof(zero), hash);
        hash = Fnv1a(file + first + sizeof(uint64_t), second - first - sizeof(uint64_t), hash);
        hash = Fnv1a(zero, sizeof(zero), hash);
        return Fnv1a(file + second + sizeof(uint64_t), bytes - second - sizeof(uint64_t), hash);
    }
}

#endif
