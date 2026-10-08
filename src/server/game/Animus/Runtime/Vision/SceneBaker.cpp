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

#include "SceneBaker.h"
#include "BakedScene.h"
#include "MapDefines.h"
#include "MapTree.h"
#include "ModelInstance.h"
#include "SceneBvh.h"
#include "VMapDefinitions.h"
#include "VMapMgr2.h"
#include "WorldModel.h"
#include <G3D/Vector3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <vector>

// Determinism (decision 0020): the cluster compares scene checksums, so the same source must give the same bytes on any
// machine. The placement math (an instance's Euler rotation, scale, translation, the mirror to the world frame) is done
// in double and rounded to float once, at the stored coordinate; the translation unit is compiled with
// -ffp-contract=off (CMake), so no fused multiply-add changes a rounding; sorting uses total orders; nothing runs on
// threads. The remaining dependence is the C library's sin and cos of a few angles per spawn, in double.

namespace
{
    namespace Sc = Animus::Vision::Scene;
    namespace Fs = std::filesystem;
    using SceneBaker::Triangle;

    /// A WMO liquid, copied out of the stock class (which the copy of its group owns).
    struct LiquidSource
    {
        uint32_t TilesX = 0;
        uint32_t TilesY = 0;
        G3D::Vector3 Corner;
        uint32_t Type = 0;
        std::vector<float> Heights;
        std::vector<uint8_t> Flags;     // empty for a liquid of one level
        G3D::AABox Bound;               // the group's bound, in model space
    };

    struct GroupSource
    {
        std::vector<G3D::Vector3> Vertices;
        std::vector<VMAP::MeshTriangle> Triangles;
        bool HasLiquid = false;
        LiquidSource Liquid;
    };

    /// FNV-1a 64 of the files a bake reads, each framed by its name and length.
    struct SourceHasher
    {
        uint64_t Value = 0xcbf29ce484222325ull;

        void Add(void const* data, std::size_t bytes)
        {
            Value = Sc::Fnv1a(static_cast<uint8_t const*>(data), bytes, Value);
        }

        void AddFile(std::string const& label, std::vector<uint8_t> const& content)
        {
            uint64_t const size = content.size();
            Add(label.data(), label.size());
            Add(&size, sizeof(size));
            Add(content.data(), content.size());
        }
    };

    bool ReadFile(Fs::path const& path, std::vector<uint8_t>& content, std::string& error)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            error = "cannot open " + path.string();
            return false;
        }
        std::streamsize const size = in.tellg();
        in.seekg(0);
        content.resize(std::size_t(size));
        if (size > 0 && !in.read(reinterpret_cast<char*>(content.data()), size))
        {
            error = "cannot read " + path.string();
            return false;
        }
        return true;
    }

    /// One source file as the cheap pre-check sees it: its name below the data directory, size and modification time.
    struct FileStat
    {
        std::string Name;
        uint64_t Size = ~0ull;      // ~0 for a file that cannot be stat'ed
        int64_t TimeNs = 0;
    };

    FileStat StatFile(Fs::path const& root, std::string const& name)
    {
        FileStat stat;
        stat.Name = name;
        std::error_code ec;
        uint64_t const size = Fs::file_size(root / name, ec);
        if (ec)
            return stat;
        auto const time = Fs::last_write_time(root / name, ec);
        if (ec)
            return stat;
        stat.Size = size;
        stat.TimeNs = int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
        return stat;
    }

    /// FNV-1a 64 over the stats in name order: name, size, time.
    uint64_t DigestOf(std::vector<FileStat> stats)
    {
        std::sort(stats.begin(), stats.end(), [](FileStat const& a, FileStat const& b) { return a.Name < b.Name; });
        uint64_t hash = 0xcbf29ce484222325ull;
        for (FileStat const& stat : stats)
        {
            hash = Sc::Fnv1a(reinterpret_cast<uint8_t const*>(stat.Name.data()), stat.Name.size(), hash);
            hash = Sc::Fnv1a(reinterpret_cast<uint8_t const*>(&stat.Size), sizeof(stat.Size), hash);
            hash = Sc::Fnv1a(reinterpret_cast<uint8_t const*>(&stat.TimeNs), sizeof(stat.TimeNs), hash);
        }
        return hash;
    }

    std::string TileMapName(uint32_t mapId, int32_t x, int32_t y)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "%03u%02d%02d.map", mapId, x, y);
        return name;
    }

    /// LiquidType.dbc: entry -> Type (0 water, 1 ocean, 2 magma, 3 slime). The file is WDBC: a 20-byte header (magic,
    /// records, fields, record size, string block size) then fixed records of uint32 fields; field 0 is the id and
    /// field 3 the type (LiquidTypefmt "nxxix...").
    bool ParseLiquidTypes(std::vector<uint8_t> const& file, std::string const& path,
        std::map<uint32_t, uint32_t>& types, std::string& error)
    {
        uint32_t header[5] = {};
        if (file.size() < sizeof(header) || std::memcmp(file.data(), "WDBC", 4) != 0)
        {
            error = "cannot read " + path + " as a DBC";
            return false;
        }
        std::memcpy(header, file.data(), sizeof(header));
        uint32_t const records = header[1];
        uint32_t const fields = header[2];
        uint32_t const size = header[3];
        if (fields < 4 || size != fields * 4)
        {
            error = path + ": unexpected record layout";
            return false;
        }
        if (file.size() < sizeof(header) + std::size_t(records) * size)
        {
            error = path + ": truncated";
            return false;
        }
        for (uint32_t i = 0; i < records; ++i)
        {
            uint32_t id = 0;
            uint32_t type = 0;
            uint8_t const* record = file.data() + sizeof(header) + std::size_t(i) * size;
            std::memcpy(&id, record, 4);
            std::memcpy(&type, record + 12, 4);
            types[id] = type;
        }
        return true;
    }

    // The .map terrain file (GridTerrainData.h's map_fileheader and friends, format version 9), read here so the
    // baker needs no core class: it sees every tile whole, which the core's accessors do not offer.
    constexpr uint32_t MAP_MAGIC = 0x5350414D;          // "MAPS"
    constexpr uint32_t MAP_VERSION = 9;
    constexpr uint32_t MAP_HEIGHT_MAGIC = 0x5447484D;   // "MHGT"
    constexpr uint32_t MAP_LIQUID_MAGIC = 0x51494C4D;   // "MLIQ"
    constexpr uint32_t HEIGHT_NO_HEIGHT = 0x0001;
    constexpr uint32_t HEIGHT_AS_INT16 = 0x0002;
    constexpr uint32_t HEIGHT_AS_INT8 = 0x0004;
    constexpr uint8_t LIQUID_NO_TYPE = 0x01;
    constexpr uint8_t LIQUID_NO_HEIGHT = 0x02;
    constexpr uint32_t LIQUID_FLAG_DARK_WATER = 0x10;
    constexpr uint32_t LIQUID_FLAG_MAGMA = 0x04;
    constexpr uint32_t LIQUID_FLAG_SLIME = 0x08;

    struct MapFileHeader
    {
        uint32_t MapMagic;
        uint32_t VersionMagic;
        uint32_t BuildMagic;
        uint32_t AreaMapOffset;
        uint32_t AreaMapSize;
        uint32_t HeightMapOffset;
        uint32_t HeightMapSize;
        uint32_t LiquidMapOffset;
        uint32_t LiquidMapSize;
        uint32_t HolesOffset;
        uint32_t HolesSize;
    };

    struct MapHeightHeader
    {
        uint32_t Fourcc;
        uint32_t Flags;
        float GridHeight;
        float GridMaxHeight;
    };

    struct MapLiquidHeader
    {
        uint32_t Fourcc;
        uint8_t Flags;
        uint8_t LiquidFlags;
        uint16_t LiquidType;
        uint8_t OffsetX;
        uint8_t OffsetY;
        uint8_t Width;
        uint8_t Height;
        float LiquidLevel;
    };

    /// One baked terrain tile before it is laid into the sections.
    struct TerrainTile
    {
        Sc::TerrainRec Rec = {};
        std::vector<float> Heights;         // V9 then V8, or empty
        std::vector<uint16_t> Holes;        // 256 words, or empty
        std::vector<float> Level;           // 128 x 128, or empty
        std::vector<uint8_t> Kind;
        std::vector<Sc::BlockRange> Blocks;         // 256 ground height ranges of a non-flat tile, or empty
        std::vector<Sc::BlockRange> LiquidBlocks;   // 256 liquid level ranges, or empty
        float DataMax = 0.0f;                       // the highest height of the arrays (FlatHeight when flat)
        float LiquidMin = 0.0f;
        float LiquidMax = 0.0f;
    };

    /// The culling ranges of a tile's height arrays (V9 then V8): per 8 x 8 block of cells the lowest and highest of
    /// its 9 x 9 corners and 8 x 8 centres, edges shared with the next block included.
    void HeightBlocks(std::vector<float> const& heights, std::vector<Sc::BlockRange>& blocks, float& low, float& high)
    {
        float const* v9 = heights.data();
        float const* v8 = v9 + Sc::TERRAIN_V9 * Sc::TERRAIN_V9;
        blocks.assign(Sc::TERRAIN_BLOCK_COUNT, Sc::BlockRange{ Sc::BLOCK_EMPTY_MIN, Sc::BLOCK_EMPTY_MAX });
        low = Sc::BLOCK_EMPTY_MIN;
        high = Sc::BLOCK_EMPTY_MAX;
        for (uint32_t bx = 0; bx < Sc::TERRAIN_BLOCKS_SIDE; ++bx)
            for (uint32_t by = 0; by < Sc::TERRAIN_BLOCKS_SIDE; ++by)
            {
                Sc::BlockRange& block = blocks[bx * Sc::TERRAIN_BLOCKS_SIDE + by];
                uint32_t const x0 = bx * Sc::TERRAIN_BLOCK_CELLS;
                uint32_t const y0 = by * Sc::TERRAIN_BLOCK_CELLS;
                for (uint32_t x = x0; x <= x0 + Sc::TERRAIN_BLOCK_CELLS; ++x)
                    for (uint32_t y = y0; y <= y0 + Sc::TERRAIN_BLOCK_CELLS; ++y)
                    {
                        float const h = v9[std::size_t(x) * Sc::TERRAIN_V9 + y];
                        block.Min = std::min(block.Min, h);
                        block.Max = std::max(block.Max, h);
                    }
                for (uint32_t x = x0; x < x0 + Sc::TERRAIN_BLOCK_CELLS; ++x)
                    for (uint32_t y = y0; y < y0 + Sc::TERRAIN_BLOCK_CELLS; ++y)
                    {
                        float const h = v8[std::size_t(x) * Sc::TERRAIN_CELLS + y];
                        block.Min = std::min(block.Min, h);
                        block.Max = std::max(block.Max, h);
                    }
                low = std::min(low, block.Min);
                high = std::max(high, block.Max);
            }
    }

    /// ... and of a tile's liquid: the levels of the cells that have liquid (an empty block stays empty).
    void LiquidBlocks(std::vector<float> const& level, std::vector<uint8_t> const& kind,
        std::vector<Sc::BlockRange>& blocks, float& low, float& high)
    {
        blocks.assign(Sc::TERRAIN_BLOCK_COUNT, Sc::BlockRange{ Sc::BLOCK_EMPTY_MIN, Sc::BLOCK_EMPTY_MAX });
        low = Sc::BLOCK_EMPTY_MIN;
        high = Sc::BLOCK_EMPTY_MAX;
        for (uint32_t x = 0; x < Sc::TERRAIN_CELLS; ++x)
            for (uint32_t y = 0; y < Sc::TERRAIN_CELLS; ++y)
            {
                std::size_t const cell = std::size_t(x) * Sc::TERRAIN_CELLS + y;
                if (kind[cell] == Sc::TERRAIN_LIQUID_NONE)
                    continue;
                Sc::BlockRange& block = blocks[(x / Sc::TERRAIN_BLOCK_CELLS) * Sc::TERRAIN_BLOCKS_SIDE
                    + y / Sc::TERRAIN_BLOCK_CELLS];
                block.Min = std::min(block.Min, level[cell]);
                block.Max = std::max(block.Max, level[cell]);
                low = std::min(low, level[cell]);
                high = std::max(high, level[cell]);
            }
    }

    template <typename T>
    bool Take(std::vector<uint8_t> const& file, std::size_t& at, T* out, std::size_t count)
    {
        std::size_t const bytes = sizeof(T) * count;
        if (at > file.size() || file.size() - at < bytes)
            return false;
        std::memcpy(out, file.data() + at, bytes);
        at += bytes;
        return true;
    }

    /// A tile's ground and liquid, as GridTerrainData::Load reads them and GetCellHeights / resolveLiquid decode them
    /// (the liquid's area override, AreaTable.dbc's LiquidTypeOverride, is not applied: no map the curriculum uses
    /// remaps one liquid to another).
    bool ParseTerrainTile(std::vector<uint8_t> const& file, std::string const& path, int32_t tileX, int32_t tileY,
        std::map<uint32_t, uint32_t> const& liquidTypes, TerrainTile& tile, std::string& error)
    {
        MapFileHeader header;
        std::size_t at = 0;
        if (!Take(file, at, &header, 1) || header.MapMagic != MAP_MAGIC || header.VersionMagic != MAP_VERSION)
        {
            error = path + ": not a map file of version 9";
            return false;
        }
        tile.Rec.TileX = tileX;
        tile.Rec.TileY = tileY;
        tile.Rec.HeightIndex = Sc::TERRAIN_NONE;
        tile.Rec.HoleIndex = Sc::TERRAIN_NONE;
        tile.Rec.LiquidIndex = Sc::TERRAIN_NONE;
        bool flat = false;

        if (header.HeightMapOffset)
        {
            MapHeightHeader height;
            at = header.HeightMapOffset;
            if (!Take(file, at, &height, 1) || height.Fourcc != MAP_HEIGHT_MAGIC)
            {
                error = path + ": damaged height section";
                return false;
            }
            tile.Rec.Flags |= Sc::TERRAIN_HEIGHTS;
            tile.Rec.MaxHeight = std::max(height.GridMaxHeight, height.GridHeight);
            tile.Rec.FlatHeight = height.GridHeight;
            std::size_t const v9 = Sc::TERRAIN_V9 * Sc::TERRAIN_V9;
            std::size_t const v8 = Sc::TERRAIN_CELLS * Sc::TERRAIN_CELLS;
            if (height.Flags & HEIGHT_NO_HEIGHT)
            {
                flat = true;
                tile.Rec.Flags |= Sc::TERRAIN_FLAT;
                tile.Rec.MinHeight = height.GridHeight;
                tile.DataMax = height.GridHeight;
            }
            else
            {
                tile.Heights.resize(v9 + v8);
                bool ok = false;
                if (height.Flags & HEIGHT_AS_INT16)
                {
                    std::vector<uint16_t> raw(v9 + v8);
                    ok = Take(file, at, raw.data(), raw.size());
                    // (max - min) / 65535, a float over an int, as GridTerrainData::Load divides.
                    float const scale = (height.GridMaxHeight - height.GridHeight) / 65535;
                    for (std::size_t i = 0; ok && i < raw.size(); ++i)
                        tile.Heights[i] = float(raw[i]) * scale + height.GridHeight;
                }
                else if (height.Flags & HEIGHT_AS_INT8)
                {
                    std::vector<uint8_t> raw(v9 + v8);
                    ok = Take(file, at, raw.data(), raw.size());
                    float const scale = (height.GridMaxHeight - height.GridHeight) / 255;
                    for (std::size_t i = 0; ok && i < raw.size(); ++i)
                        tile.Heights[i] = float(raw[i]) * scale + height.GridHeight;
                }
                else
                    ok = Take(file, at, tile.Heights.data(), tile.Heights.size());
                if (!ok)
                {
                    error = path + ": truncated height data";
                    return false;
                }
                HeightBlocks(tile.Heights, tile.Blocks, tile.Rec.MinHeight, tile.DataMax);
            }
        }

        if (header.HolesSize && !flat)
        {
            std::vector<uint16_t> holes(256);
            at = header.HolesOffset;
            if (!Take(file, at, holes.data(), holes.size()))
            {
                error = path + ": truncated hole data";
                return false;
            }
            if (std::any_of(holes.begin(), holes.end(), [](uint16_t word) { return word != 0; }))
            {
                tile.Rec.Flags |= Sc::TERRAIN_HOLES;
                tile.Holes = std::move(holes);
            }
        }

        if (header.LiquidMapOffset)
        {
            MapLiquidHeader liquid;
            at = header.LiquidMapOffset;
            if (!Take(file, at, &liquid, 1) || liquid.Fourcc != MAP_LIQUID_MAGIC)
            {
                error = path + ": damaged liquid section";
                return false;
            }
            std::vector<uint16_t> entries;
            std::vector<uint8_t> flags;
            std::vector<float> levels;
            bool const typed = !(liquid.Flags & LIQUID_NO_TYPE);
            if (typed)
            {
                entries.resize(256);
                flags.resize(256);
                if (!Take(file, at, entries.data(), entries.size()) || !Take(file, at, flags.data(), flags.size()))
                {
                    error = path + ": truncated liquid data";
                    return false;
                }
            }
            if (!(liquid.Flags & LIQUID_NO_HEIGHT))
            {
                levels.resize(std::size_t(liquid.Width) * liquid.Height);
                if (!Take(file, at, levels.data(), levels.size()))
                {
                    error = path + ": truncated liquid heights";
                    return false;
                }
            }
            if (typed || liquid.LiquidFlags)
            {
                std::vector<float> level(Sc::TERRAIN_CELLS * Sc::TERRAIN_CELLS, 0.0f);
                std::vector<uint8_t> kind(Sc::TERRAIN_CELLS * Sc::TERRAIN_CELLS, Sc::TERRAIN_LIQUID_NONE);
                bool any = false;
                for (uint32_t x = 0; x < Sc::TERRAIN_CELLS; ++x)
                    for (uint32_t y = 0; y < Sc::TERRAIN_CELLS; ++y)
                    {
                        std::size_t const group = std::size_t(x >> 3) * 16 + (y >> 3);
                        uint32_t type = typed ? flags[group] : liquid.LiquidFlags;
                        uint32_t const entry = typed ? entries[group] : liquid.LiquidType;
                        auto const known = liquidTypes.find(entry);
                        if (known != liquidTypes.end())
                        {
                            type &= LIQUID_FLAG_DARK_WATER;
                            type |= 1u << known->second;
                        }
                        if (type == 0)
                            continue;
                        int32_t const lx = int32_t(x) - liquid.OffsetY;
                        int32_t const ly = int32_t(y) - liquid.OffsetX;
                        if (lx < 0 || lx >= liquid.Height || ly < 0 || ly >= liquid.Width)
                            continue;
                        std::size_t const cell = std::size_t(x) * Sc::TERRAIN_CELLS + y;
                        level[cell] = levels.empty() ? liquid.LiquidLevel
                            : levels[std::size_t(lx) * liquid.Width + std::size_t(ly)];
                        kind[cell] = (type & (LIQUID_FLAG_MAGMA | LIQUID_FLAG_SLIME)) ? Sc::TERRAIN_LIQUID_DEADLY
                            : Sc::TERRAIN_LIQUID_WATER;
                        any = true;
                    }
                if (any)
                {
                    tile.Rec.Flags |= Sc::TERRAIN_LIQUID;
                    LiquidBlocks(level, kind, tile.LiquidBlocks, tile.LiquidMin, tile.LiquidMax);
                    tile.Level = std::move(level);
                    tile.Kind = std::move(kind);
                }
            }
        }
        return true;
    }

    /// Everything a bake or an identity check starts from: the tree, the type table, the terrain files, the hash.
    struct Source
    {
        std::unique_ptr<VMAP::StaticMapTree> Tree;
        VMAP::ModelInstance* Models = nullptr;
        uint32_t Count = 0;
        bool Tiled = false;
        std::map<uint32_t, uint32_t> LiquidTypes;
        std::vector<TerrainTile> Terrain;   // sorted by (TileX, TileY)
        std::vector<std::string> ModelNames;    // the model files read (below vmaps/), in a fixed order
        std::vector<FileStat> Stats;        // each source file, stat'ed just before it was read
        uint64_t Hash = 0;
    };

    /// `parseTerrain`: also decode the .map tiles (an identity check only hashes them).
    bool OpenSource(std::string const& dataDir, uint32_t mapId, bool parseTerrain, Source& source,
        std::string& error)
    {
        SourceHasher hash;
        Fs::path const root(dataDir);

        std::vector<uint8_t> file;
        Fs::path const dbc = root / "dbc" / "LiquidType.dbc";
        source.Stats.push_back(StatFile(root, "dbc/LiquidType.dbc"));
        if (!ReadFile(dbc, file, error) || !ParseLiquidTypes(file, dbc.string(), source.LiquidTypes, error))
            return false;
        hash.AddFile("LiquidType.dbc", file);

        std::string const treeName = VMAP::VMapMgr2::getMapFileName(mapId);
        source.Stats.push_back(StatFile(root, "vmaps/" + treeName));
        if (!ReadFile(root / "vmaps" / treeName, file, error))
        {
            error = "no vmap tree for map " + std::to_string(mapId) + ": " + error;
            return false;
        }
        hash.AddFile(treeName, file);
        std::error_code ec;
        for (uint32_t x = 0; x < 64; ++x)
            for (uint32_t y = 0; y < 64; ++y)
            {
                std::string const name = VMAP::StaticMapTree::getTileFileName(mapId, x, y);
                if (Fs::exists(root / "vmaps" / name, ec))
                {
                    source.Stats.push_back(StatFile(root, "vmaps/" + name));
                    if (!ReadFile(root / "vmaps" / name, file, error))
                        return false;
                    hash.AddFile(name, file);
                }
            }

        // The stock vmap tree of the map, every tile of a tiled one.
        source.Tree = std::make_unique<VMAP::StaticMapTree>(mapId, (root / "vmaps").string());
        if (!source.Tree->InitMap(treeName))
        {
            error = "the vmap tree of map " + std::to_string(mapId) + " in " + (root / "vmaps").string()
                + " cannot be loaded";
            return false;
        }
        source.Tiled = source.Tree->isTiled();
        if (source.Tiled)
            for (uint32_t x = 0; x < 64; ++x)
                for (uint32_t y = 0; y < 64; ++y)
                    source.Tree->LoadMapTile(x, y);
        source.Tree->GetModelInstances(source.Models, source.Count);
        if (!source.Models)
        {
            error = "the vmap tree of map " + std::to_string(mapId) + " has no instances";
            return false;
        }

        std::set<std::string> models;
        for (uint32_t i = 0; i < source.Count; ++i)
            if (source.Models[i].getWorldModel())
                models.insert(source.Models[i].name);
        for (std::string const& name : models)
        {
            // An M2 spawn's name ends in a NUL: the file the operating system opens (and the hash covers) is the one
            // the name names up to it. The model list the scene keeps names that file.
            std::string const file_name((name + ".vmo").c_str());
            if (source.ModelNames.empty() || source.ModelNames.back() != file_name)
                source.ModelNames.push_back(file_name);
            source.Stats.push_back(StatFile(root, "vmaps/" + file_name));
            if (!ReadFile(root / "vmaps" / (name + ".vmo"), file, error))
                return false;
            hash.AddFile(name + ".vmo", file);
        }

        for (int32_t x = 0; x < 64; ++x)
            for (int32_t y = 0; y < 64; ++y)
            {
                std::string const name = TileMapName(mapId, x, y);
                Fs::path const path = root / "maps" / name;
                if (!Fs::exists(path, ec))
                    continue;
                source.Stats.push_back(StatFile(root, "maps/" + name));
                if (!ReadFile(path, file, error))
                    return false;
                hash.AddFile(name, file);
                if (!parseTerrain)
                    continue;
                TerrainTile tile;
                if (!ParseTerrainTile(file, path.string(), x, y, source.LiquidTypes, tile, error))
                    return false;
                source.Terrain.push_back(std::move(tile));
            }
        source.Hash = hash.Value;
        return true;
    }

    template <typename T>
    void Put(std::vector<uint8_t>& file, T const* data, std::size_t count)
    {
        uint8_t const* bytes = reinterpret_cast<uint8_t const*>(data);
        file.insert(file.end(), bytes, bytes + count * sizeof(T));
    }

    void Align(std::vector<uint8_t>& file)
    {
        while (file.size() % Sc::SECTION_ALIGN)
            file.push_back(0);
    }

    /// Appends a section at the next 16-byte boundary (zero pad) and records it in the header.
    template <typename T>
    void AddSection(std::vector<uint8_t>& file, Sc::SceneHeader& header, Sc::Slot slot, std::vector<T> const& data)
    {
        Align(file);
        if (data.empty())
            return;
        header.Sections[slot] = { file.size(), data.size() * sizeof(T) };
        Put(file, data.data(), data.size());
    }

    /// The scene arrays of a BVH over `triangles`: geometry (vertex 0 and edges), normals, kinds, in leaf order.
    struct Arrays
    {
        SceneBaker::Bvh Bvh;
        std::vector<Sc::TriGeom> Geom;
        std::vector<Sc::TriNormal> Normals;
        std::vector<uint8_t> Kinds;
    };

    Arrays Arrange(std::vector<Triangle> const& triangles)
    {
        Arrays out;
        out.Bvh = SceneBaker::BuildBvh(triangles);
        out.Geom.reserve(triangles.size());
        out.Normals.reserve(triangles.size());
        out.Kinds.reserve(triangles.size());
        for (uint32_t index : out.Bvh.Order)
        {
            Triangle const& t = triangles[index];
            Sc::TriGeom geom;
            double e1[3];
            double e2[3];
            for (int i = 0; i < 3; ++i)
            {
                geom.V0[i] = t.V[0][i];
                geom.E1[i] = t.V[1][i] - t.V[0][i];
                geom.E2[i] = t.V[2][i] - t.V[0][i];
                e1[i] = double(t.V[1][i]) - double(t.V[0][i]);
                e2[i] = double(t.V[2][i]) - double(t.V[0][i]);
            }
            double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                e1[0] * e2[1] - e1[1] * e2[0] };
            double const length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            Sc::TriNormal normal;
            for (int i = 0; i < 3; ++i)
                normal.N[i] = float(n[i] / length);
            out.Geom.push_back(geom);
            out.Normals.push_back(normal);
            out.Kinds.push_back(t.Kind);
        }
        return out;
    }

    /// A triangle worth keeping: finite, with an area that survives normalising its normal.
    bool Usable(Triangle const& t)
    {
        double e1[3];
        double e2[3];
        for (int i = 0; i < 3; ++i)
        {
            for (int v = 0; v < 3; ++v)
                if (!std::isfinite(t.V[v][i]))
                    return false;
            e1[i] = double(t.V[1][i]) - double(t.V[0][i]);
            e2[i] = double(t.V[2][i]) - double(t.V[0][i]);
        }
        double const n0 = e1[1] * e2[2] - e1[2] * e2[1];
        double const n1 = e1[2] * e2[0] - e1[0] * e2[2];
        double const n2 = e1[0] * e2[1] - e1[1] * e2[0];
        return n0 * n0 + n1 * n1 + n2 * n2 > 1e-14;
    }

    /// A spawn's placement, in double: internal = R * v * scale + position, R = Rz(rot.y) * Ry(rot.x) * Rx(rot.z)
    /// in degrees (the matrix ModelInstance's inverse rotation is the inverse of, G3D::Matrix3::fromEulerAnglesZYX),
    /// then the mirror to the world frame (convertPositionToInternalRep's inverse).
    struct Placement
    {
        double R[3][3];
        double Scale;
        double Position[3];
        double Mid;

        Placement(VMAP::ModelInstance const& instance, double mid)
        {
            double const pi = 3.14159265358979323846;
            double const yaw = pi * double(instance.iRot.y) / 180.0;
            double const pitch = pi * double(instance.iRot.x) / 180.0;
            double const roll = pi * double(instance.iRot.z) / 180.0;
            double const cz = std::cos(yaw);
            double const sz = std::sin(yaw);
            double const cy = std::cos(pitch);
            double const sy = std::sin(pitch);
            double const cx = std::cos(roll);
            double const sx = std::sin(roll);
            double const z[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
            double const y[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
            double const x[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
            double yx[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    yx[i][j] = y[i][0] * x[0][j] + y[i][1] * x[1][j] + y[i][2] * x[2][j];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    R[i][j] = z[i][0] * yx[0][j] + z[i][1] * yx[1][j] + z[i][2] * yx[2][j];
            Scale = double(instance.iScale);
            Position[0] = double(instance.iPos.x);
            Position[1] = double(instance.iPos.y);
            Position[2] = double(instance.iPos.z);
            Mid = mid;
        }

        /// model space -> the spawn's internal frame
        void Internal(G3D::Vector3 const& v, double (&out)[3]) const
        {
            double const in[3] = { double(v.x), double(v.y), double(v.z) };
            for (int i = 0; i < 3; ++i)
                out[i] = (R[i][0] * in[0] + R[i][1] * in[1] + R[i][2] * in[2]) * Scale + Position[i];
        }

        /// model space -> the world frame
        void World(G3D::Vector3 const& v, float (&out)[3]) const
        {
            double p[3];
            Internal(v, p);
            out[0] = float(Mid - p[0]);
            out[1] = float(Mid - p[1]);
            out[2] = float(p[2]);
        }
    };
}

std::string SceneBaker::SceneFileName(uint32_t mapId)
{
    char name[16];
    std::snprintf(name, sizeof(name), "%03u.scene", mapId);
    return name;
}

bool SceneBaker::SourceIdentity(std::string const& dataDir, uint32_t mapId, uint64_t& hash, std::string& error)
{
    Source source;
    if (!OpenSource(dataDir, mapId, false, source, error))
        return false;
    hash = source.Hash;
    return true;
}

bool SceneBaker::SourceDigest(std::string const& dataDir, uint32_t mapId, std::string const& modelNames,
    uint64_t& digest, std::string& error)
{
    Fs::path const root(dataDir);
    std::vector<FileStat> stats;
    stats.push_back(StatFile(root, "dbc/LiquidType.dbc"));
    std::string const treeName = VMAP::VMapMgr2::getMapFileName(mapId);
    stats.push_back(StatFile(root, "vmaps/" + treeName));
    std::error_code ec;
    for (uint32_t x = 0; x < 64; ++x)
        for (uint32_t y = 0; y < 64; ++y)
        {
            std::string const name = VMAP::StaticMapTree::getTileFileName(mapId, x, y);
            if (Fs::exists(root / "vmaps" / name, ec))
                stats.push_back(StatFile(root, "vmaps/" + name));
        }
    for (std::size_t at = 0; at < modelNames.size();)
    {
        std::string const name(modelNames.c_str() + at);
        stats.push_back(StatFile(root, "vmaps/" + name));
        at += name.size() + 1;
    }
    for (int32_t x = 0; x < 64; ++x)
        for (int32_t y = 0; y < 64; ++y)
        {
            std::string const name = TileMapName(mapId, x, y);
            if (Fs::exists(root / "maps" / name, ec))
                stats.push_back(StatFile(root, "maps/" + name));
        }
    // A source file that cannot be stat'ed is never a match: the content hash then reports it.
    for (FileStat const& stat : stats)
        if (stat.Size == ~0ull)
        {
            error = "cannot stat " + stat.Name;
            return false;
        }
    digest = DigestOf(std::move(stats));
    return true;
}

bool SceneBaker::RefreshDigest(std::string const& scenePath, uint64_t digest, std::string& error)
{
    // Eight bytes in place: the digest is outside the checksum, so nothing else in the file changes, and a write that
    // is torn or lost leaves a digest that matches nothing, which the next start answers with the content hash again.
    std::fstream stream(scenePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream)
    {
        error = "cannot open " + scenePath + " for writing";
        return false;
    }
    stream.seekp(std::streamoff(offsetof(Sc::SceneHeader, SourceFilesDigest)));
    stream.write(reinterpret_cast<char const*>(&digest), sizeof(digest));
    stream.flush();
    if (!stream)
    {
        error = "cannot write " + scenePath;
        return false;
    }
    return true;
}

bool SceneBaker::BakeMap(std::string const& dataDir, uint32_t mapId, std::string const& outPath, BakeReport& report,
    std::string& error)
{
    auto const start = std::chrono::steady_clock::now();
    report = BakeReport();

    Source source;
    if (!OpenSource(dataDir, mapId, true, source, error))
        return false;
    report.SourceHash = source.Hash;
    VMAP::ModelInstance* const models = source.Models;
    uint32_t const count = source.Count;

    double const mid = double(0.5f * MAX_NUMBER_OF_GRIDS * SIZE_OF_GRIDS);
    std::vector<Triangle> solids;
    std::vector<Triangle> liquids;
    std::map<VMAP::WorldModel*, std::vector<GroupSource>> library;    // looked up by pointer, never iterated

    for (uint32_t i = 0; i < count; ++i)
    {
        VMAP::ModelInstance& instance = models[i];
        VMAP::WorldModel* model = instance.getWorldModel();
        if (!model)
            continue;
        ++report.Spawns;
        bool const m2 = (instance.flags & VMAP::MOD_M2) != 0;
        report.M2Spawns += m2 ? 1 : 0;

        // Each distinct model's groups, copied out once.
        auto found = library.find(model);
        if (found == library.end())
        {
            std::vector<GroupSource> groups;
            std::vector<VMAP::GroupModel> copies;
            model->GetGroupModels(copies);
            for (VMAP::GroupModel& copy : copies)
            {
                GroupSource group;
                VMAP::WmoLiquid* liquid = nullptr;
                copy.GetMeshData(group.Vertices, group.Triangles, liquid);
                if (liquid)
                {
                    group.HasLiquid = true;
                    LiquidSource& liquidSource = group.Liquid;
                    liquid->GetPosInfo(liquidSource.TilesX, liquidSource.TilesY, liquidSource.Corner);
                    liquidSource.Type = liquid->GetType();
                    liquidSource.Bound = copy.GetBound();
                    bool const tiles = liquidSource.TilesX && liquidSource.TilesY;
                    std::size_t const heights = tiles
                        ? std::size_t(liquidSource.TilesX + 1) * (liquidSource.TilesY + 1) : 1;
                    float const* h = liquid->GetHeightStorage();
                    if (h)
                        liquidSource.Heights.assign(h, h + heights);
                    uint8_t const* flags = liquid->GetFlagsStorage();
                    if (tiles && flags)
                        liquidSource.Flags.assign(flags,
                            flags + std::size_t(liquidSource.TilesX) * liquidSource.TilesY);
                    if (liquidSource.Heights.empty())
                        group.HasLiquid = false;
                }
                groups.push_back(std::move(group));
            }
            found = library.emplace(model, std::move(groups)).first;
            ++report.Models;
        }

        Placement const place(instance, mid);
        auto const make = [&](G3D::Vector3 const& a, G3D::Vector3 const& b, G3D::Vector3 const& c, uint8_t kind)
        {
            Triangle t;
            place.World(a, t.V[0]);
            place.World(b, t.V[1]);
            place.World(c, t.V[2]);
            t.Kind = kind;
            return t;
        };

        double low[3] = { INFINITY, INFINITY, INFINITY };
        double high[3] = { -INFINITY, -INFINITY, -INFINITY };
        bool any = false;
        for (GroupSource const& group : found->second)
        {
            for (G3D::Vector3 const& v : group.Vertices)
            {
                double p[3];
                place.Internal(v, p);
                for (int a = 0; a < 3; ++a)
                {
                    low[a] = std::min(low[a], p[a]);
                    high[a] = std::max(high[a], p[a]);
                }
                any = true;
            }
            for (VMAP::MeshTriangle const& tri : group.Triangles)
            {
                if (tri.idx0 >= group.Vertices.size() || tri.idx1 >= group.Vertices.size()
                    || tri.idx2 >= group.Vertices.size())
                {
                    ++report.DroppedTriangles;
                    continue;
                }
                Triangle const t = make(group.Vertices[tri.idx0], group.Vertices[tri.idx1],
                    group.Vertices[tri.idx2], m2 ? Sc::KIND_M2 : Sc::KIND_WMO);
                if (Usable(t))
                    solids.push_back(t);
                else
                    ++report.DroppedTriangles;
            }
            if (!group.HasLiquid)
                continue;
            LiquidSource const& liquid = group.Liquid;
            auto const type = source.LiquidTypes.find(liquid.Type);
            uint8_t kind = 0;
            if (type == source.LiquidTypes.end())
                ++report.UnknownLiquidTypes;
            else
            {
                kind = uint8_t(type->second & Sc::LIQUID_TYPE_MASK);
                if (type->second == 2 || type->second == 3)
                    kind |= Sc::LIQUID_DEADLY;
            }
            auto const add = [&](G3D::Vector3 const& a, G3D::Vector3 const& b, G3D::Vector3 const& c)
            {
                Triangle const t = make(a, b, c, kind);
                if (Usable(t))
                    liquids.push_back(t);
            };
            if (liquid.TilesX && liquid.TilesY && !liquid.Flags.empty())
            {
                // Each used tile as WmoLiquid::GetLiquidHeight tessellates it.
                uint32_t const row = liquid.TilesX + 1;
                for (uint32_t y = 0; y < liquid.TilesY; ++y)
                    for (uint32_t x = 0; x < liquid.TilesX; ++x)
                    {
                        if ((liquid.Flags[x + y * liquid.TilesX] & 0x0F) == 0x0F)
                            continue;
                        auto const corner = [&](uint32_t cx, uint32_t cy)
                        {
                            return G3D::Vector3(liquid.Corner.x + float(x + cx) * LIQUID_TILE_SIZE,
                                liquid.Corner.y + float(y + cy) * LIQUID_TILE_SIZE,
                                liquid.Heights[(x + cx) + (y + cy) * row]);
                        };
                        add(corner(0, 0), corner(1, 0), corner(1, 1));
                        add(corner(0, 0), corner(1, 1), corner(0, 1));
                    }
            }
            else
            {
                // One level over the group's footprint.
                float const z = liquid.Heights[0];
                G3D::Vector3 const lo(liquid.Bound.low().x, liquid.Bound.low().y, z);
                G3D::Vector3 const hi(liquid.Bound.high().x, liquid.Bound.high().y, z);
                add(lo, G3D::Vector3(hi.x, lo.y, z), hi);
                add(lo, hi, G3D::Vector3(lo.x, hi.y, z));
            }
        }

        // A WMO's vertices must lie inside its spawn's bound (internal space): a wrong rotation convention puts them
        // yards outside it.
        if (any && (instance.flags & VMAP::MOD_HAS_BOUND))
        {
            G3D::AABox const& bound = instance.iBound;
            for (int a = 0; a < 3; ++a)
            {
                report.WorstBoundExcess = std::max(report.WorstBoundExcess,
                    float(std::max(double(bound.low()[a]) - low[a], high[a] - double(bound.high()[a]))));
            }
        }
    }
    if (report.WorstBoundExcess > 0.5f)
    {
        error = "the transformed vertices lie " + std::to_string(report.WorstBoundExcess)
            + " yd outside their spawn bounds: the rotation convention is wrong";
        return false;
    }
    if (solids.empty())
    {
        error = "map " + std::to_string(mapId) + " has no solid triangle";
        return false;
    }

    Arrays const solid = Arrange(solids);
    Arrays const liquid = Arrange(liquids);
    report.Triangles = uint32_t(solid.Geom.size());
    report.LiquidTriangles = uint32_t(liquid.Geom.size());
    report.Nodes = uint32_t(solid.Bvh.Nodes.size());
    report.LiquidNodes = uint32_t(liquid.Bvh.Nodes.size());
    report.MaxDepth = std::max(solid.Bvh.MaxDepth, liquid.Bvh.MaxDepth);
    if (report.MaxDepth > Sc::MAX_DEPTH)
    {
        error = "the BVH is deeper than the tracer's stack";
        return false;
    }

    // The terrain sections, tile by tile in (TileX, TileY) order (the files were read in that order).
    std::vector<Sc::TerrainRec> index;
    std::vector<float> heights;
    std::vector<uint16_t> holes;
    std::vector<uint8_t> liquidBytes;
    std::vector<Sc::BlockRange> blocks;
    std::vector<Sc::BlockRange> liquidBlocks;
    float heightMin = Sc::BLOCK_EMPTY_MIN;
    float heightMax = Sc::BLOCK_EMPTY_MAX;
    float levelMin = Sc::BLOCK_EMPTY_MIN;
    float levelMax = Sc::BLOCK_EMPTY_MAX;
    for (TerrainTile& tile : source.Terrain)
    {
        if (tile.Rec.Flags & Sc::TERRAIN_HEIGHTS)
        {
            heightMin = std::min(heightMin, tile.Rec.MinHeight);
            heightMax = std::max(heightMax, tile.DataMax);
        }
        if (!tile.Heights.empty())
        {
            tile.Rec.HeightIndex = uint32_t(heights.size() / Sc::TERRAIN_HEIGHT_FLOATS);
            heights.insert(heights.end(), tile.Heights.begin(), tile.Heights.end());
            blocks.insert(blocks.end(), tile.Blocks.begin(), tile.Blocks.end());
            ++report.TerrainHeightTiles;
        }
        if (!tile.Holes.empty())
        {
            tile.Rec.HoleIndex = uint32_t(holes.size() / 256);
            holes.insert(holes.end(), tile.Holes.begin(), tile.Holes.end());
            ++report.TerrainHoleTiles;
        }
        if (!tile.Kind.empty())
        {
            tile.Rec.LiquidIndex = uint32_t(liquidBytes.size() / Sc::TERRAIN_LIQUID_BYTES);
            Put(liquidBytes, tile.Level.data(), tile.Level.size());
            Put(liquidBytes, tile.Kind.data(), tile.Kind.size());
            liquidBlocks.insert(liquidBlocks.end(), tile.LiquidBlocks.begin(), tile.LiquidBlocks.end());
            levelMin = std::min(levelMin, tile.LiquidMin);
            levelMax = std::max(levelMax, tile.LiquidMax);
            ++report.TerrainLiquidTiles;
        }
        index.push_back(tile.Rec);
    }
    report.TerrainTiles = uint32_t(index.size());

    std::vector<uint8_t> modelBytes;
    Sc::SceneHeader header;
    header.MapId = mapId;
    header.BakerVersion = SceneBaker::BAKER_VERSION;
    header.SourceHash = source.Hash;
    header.Flags = (source.Tiled ? Sc::TILED : 0u) | (liquid.Geom.empty() ? 0u : Sc::HAS_MODEL_LIQUID)
        | (index.empty() ? 0u : Sc::HAS_TERRAIN) | (report.TerrainLiquidTiles ? Sc::HAS_TERRAIN_LIQUID : 0u);
    for (int a = 0; a < 3; ++a)
    {
        header.SolidMin[a] = solid.Bvh.Min[a];
        header.SolidMax[a] = solid.Bvh.Max[a];
        header.LiquidMin[a] = liquid.Geom.empty() ? 0.0f : liquid.Bvh.Min[a];
        header.LiquidMax[a] = liquid.Geom.empty() ? 0.0f : liquid.Bvh.Max[a];
    }
    header.TriCount = report.Triangles;
    header.NodeCount = report.Nodes;
    header.LiquidTriCount = report.LiquidTriangles;
    header.LiquidNodeCount = report.LiquidNodes;
    header.TerrainTileCount = report.TerrainTiles;
    header.TerrainHeightMin = heightMin <= heightMax ? heightMin : 0.0f;
    header.TerrainHeightMax = heightMin <= heightMax ? heightMax : 0.0f;
    header.TerrainLiquidMin = levelMin <= levelMax ? levelMin : 0.0f;
    header.TerrainLiquidMax = levelMin <= levelMax ? levelMax : 0.0f;
    {
        std::string models;
        for (std::string const& name : source.ModelNames)
        {
            models += name;
            models.push_back('\0');
        }
        modelBytes.assign(models.begin(), models.end());
    }
    header.SourceFilesDigest = DigestOf(source.Stats);
    header.SourceSpawnCount = report.Spawns;
    header.SourceM2Count = report.M2Spawns;

    std::vector<uint8_t> file(sizeof(Sc::SceneHeader), 0);
    AddSection(file, header, Sc::SLOT_TRI_GEOM, solid.Geom);
    AddSection(file, header, Sc::SLOT_TRI_NORMAL, solid.Normals);
    AddSection(file, header, Sc::SLOT_TRI_KIND, solid.Kinds);
    AddSection(file, header, Sc::SLOT_NODES, solid.Bvh.Nodes);
    AddSection(file, header, Sc::SLOT_LIQ_GEOM, liquid.Geom);
    AddSection(file, header, Sc::SLOT_LIQ_KIND, liquid.Kinds);
    AddSection(file, header, Sc::SLOT_LIQ_NODES, liquid.Bvh.Nodes);
    AddSection(file, header, Sc::SLOT_TERRAIN_INDEX, index);
    AddSection(file, header, Sc::SLOT_TERRAIN_HEIGHTS, heights);
    AddSection(file, header, Sc::SLOT_TERRAIN_LIQUID, liquidBytes);
    AddSection(file, header, Sc::SLOT_TERRAIN_HOLES, holes);
    AddSection(file, header, Sc::SLOT_TERRAIN_BLOCKS, blocks);
    AddSection(file, header, Sc::SLOT_TERRAIN_LIQUID_BLOCKS, liquidBlocks);
    AddSection(file, header, Sc::SLOT_SOURCE_MODELS, modelBytes);
    Align(file);
    std::memcpy(file.data(), &header, sizeof(header));
    report.Checksum = Sc::ChecksumOf(file.data(), file.size());
    std::memcpy(file.data() + offsetof(Sc::SceneHeader, Checksum), &report.Checksum, sizeof(report.Checksum));
    report.FileBytes = file.size();

    // Beside the file under a name of its own, then renamed over it: whole or not there at all.
    Fs::path const out(outPath);
    std::error_code ec;
    if (out.has_parent_path())
    {
        Fs::create_directories(out.parent_path(), ec);
        if (ec)
        {
            error = "cannot create the scene directory " + out.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    Fs::path temporary = out;
    temporary += ".tmp." + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()))
        + "." + std::to_string(uint64_t(reinterpret_cast<std::uintptr_t>(&report)));
    bool written = false;
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (stream && stream.write(reinterpret_cast<char const*>(file.data()), std::streamsize(file.size())))
        {
            stream.flush();
            written = bool(stream);
        }
    }
    if (!written)
    {
        error = "cannot write " + temporary.string() + " (the scene directory must be writable)";
        Fs::remove(temporary, ec);
        return false;
    }
    Fs::rename(temporary, out, ec);
    if (ec)
    {
        error = "cannot rename " + temporary.string() + " to " + out.string() + ": " + ec.message();
        Fs::remove(temporary, ec);
        return false;
    }
    report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}
