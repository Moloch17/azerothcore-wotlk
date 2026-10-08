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

#include "Bake.h"
#include "BakedScene.h"
#include "Bvh.h"
#include "MapDefines.h"
#include "MapTree.h"
#include "ModelInstance.h"
#include "VMapDefinitions.h"
#include "VMapMgr2.h"
#include "WorldModel.h"
#include <G3D/Matrix3.h>
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
#include <vector>

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

    /// LiquidType.dbc: entry -> Type (0 water, 1 ocean, 2 magma, 3 slime). The file is WDBC: a 20-byte header (magic,
    /// records, fields, record size, string block size) then fixed records of uint32 fields; field 0 is the id and
    /// field 3 the type (LiquidTypefmt "nxxix...").
    bool ReadLiquidTypes(std::string const& path, std::map<uint32_t, uint32_t>& types, std::string& error)
    {
        std::ifstream in(path, std::ios::binary);
        uint32_t header[5] = {};
        if (!in || !in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(&header[0], "WDBC", 4) != 0)
        {
            error = "cannot read " + path + " as a DBC";
            return false;
        }
        uint32_t const records = header[1];
        uint32_t const fields = header[2];
        uint32_t const size = header[3];
        if (fields < 4 || size != fields * 4)
        {
            error = path + ": unexpected record layout";
            return false;
        }
        std::vector<uint32_t> record(fields);
        for (uint32_t i = 0; i < records; ++i)
        {
            if (!in.read(reinterpret_cast<char*>(record.data()), size))
            {
                error = path + ": truncated";
                return false;
            }
            types[record[0]] = record[3];
        }
        return true;
    }

    /// Whether the data dir has any terrain tile of the map (maps/<id 3 digits><y><x>.map).
    bool HasTerrain(std::string const& dataDir, uint32_t mapId)
    {
        char prefix[8];
        std::snprintf(prefix, sizeof(prefix), "%03u", mapId);
        std::error_code ec;
        for (Fs::directory_entry const& entry : Fs::directory_iterator(Fs::path(dataDir) / "maps", ec))
        {
            std::string const name = entry.path().filename().string();
            if (name.size() == 10 && name.compare(0, 3, prefix) == 0 && name.compare(7, 3, "map") == 0)
                return true;
        }
        return false;
    }

    template <typename T>
    void Put(std::vector<uint8_t>& file, T const* data, std::size_t count)
    {
        uint8_t const* bytes = reinterpret_cast<uint8_t const*>(data);
        file.insert(file.end(), bytes, bytes + count * sizeof(T));
    }

    /// Appends a section at the next 16-byte boundary (zero pad) and records it in the header.
    template <typename T>
    void AddSection(std::vector<uint8_t>& file, Sc::SceneHeader& header, Sc::Slot slot, std::vector<T> const& data)
    {
        while (file.size() % Sc::SECTION_ALIGN)
            file.push_back(0);
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
}

bool SceneBaker::BakeMap(std::string const& dataDir, uint32_t mapId, std::string const& outPath, BakeReport& report,
    std::string& error)
{
    auto const start = std::chrono::steady_clock::now();
    report = BakeReport();
    if (HasTerrain(dataDir, mapId))
    {
        error = "map " + std::to_string(mapId) + " has terrain tiles in maps/: the terrain extension is not baked yet";
        return false;
    }

    std::map<uint32_t, uint32_t> liquidTypes;
    if (!ReadLiquidTypes((Fs::path(dataDir) / "dbc" / "LiquidType.dbc").string(), liquidTypes, error))
        return false;

    // The stock vmap tree of the map, every tile of a tiled one.
    std::string const vmaps = (Fs::path(dataDir) / "vmaps").string();
    auto tree = std::make_unique<VMAP::StaticMapTree>(mapId, vmaps);
    if (!tree->InitMap(VMAP::VMapMgr2::getMapFileName(mapId)))
    {
        error = "no vmap tree for map " + std::to_string(mapId) + " in " + vmaps;
        return false;
    }
    bool const tiled = tree->isTiled();
    if (tiled)
        for (uint32_t x = 0; x < 64; ++x)
            for (uint32_t y = 0; y < 64; ++y)
                tree->LoadMapTile(x, y);
    VMAP::ModelInstance* models = nullptr;
    uint32_t count = 0;
    tree->GetModelInstances(models, count);
    if (!models)
    {
        error = "the vmap tree of map " + std::to_string(mapId) + " has no instances";
        return false;
    }

    float const mid = 0.5f * MAX_NUMBER_OF_GRIDS * SIZE_OF_GRIDS;
    std::vector<Triangle> solids;
    std::vector<Triangle> liquids;
    std::map<VMAP::WorldModel*, std::vector<GroupSource>> library;

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
                    LiquidSource& source = group.Liquid;
                    liquid->GetPosInfo(source.TilesX, source.TilesY, source.Corner);
                    source.Type = liquid->GetType();
                    source.Bound = copy.GetBound();
                    bool const tiles = source.TilesX && source.TilesY;
                    std::size_t const heights = tiles ? std::size_t(source.TilesX + 1) * (source.TilesY + 1) : 1;
                    float const* h = liquid->GetHeightStorage();
                    if (h)
                        source.Heights.assign(h, h + heights);
                    uint8_t const* flags = liquid->GetFlagsStorage();
                    if (tiles && flags)
                        source.Flags.assign(flags, flags + std::size_t(source.TilesX) * source.TilesY);
                    if (source.Heights.empty())
                        group.HasLiquid = false;
                }
                groups.push_back(std::move(group));
            }
            found = library.emplace(model, std::move(groups)).first;
            ++report.Models;
        }

        // model -> internal (the inverse of ModelInstance::intersectRay's world -> model) -> world (the mirror
        // convertPositionToInternalRep applies).
        G3D::Matrix3 const invRot = G3D::Matrix3::fromEulerAnglesZYX(G3D::pi() * instance.iRot.y / 180.f,
            G3D::pi() * instance.iRot.x / 180.f, G3D::pi() * instance.iRot.z / 180.f).inverse();
        float const scale = instance.iScale;
        auto const internal = [&](G3D::Vector3 const& v) { return (v * invRot) * scale + instance.iPos; };
        auto const world = [&](G3D::Vector3 const& v)
        {
            G3D::Vector3 const p = internal(v);
            return G3D::Vector3(mid - p.x, mid - p.y, p.z);
        };
        auto const make = [&](G3D::Vector3 const& a, G3D::Vector3 const& b, G3D::Vector3 const& c, uint8_t kind)
        {
            Triangle t;
            G3D::Vector3 const w[3] = { world(a), world(b), world(c) };
            for (int v = 0; v < 3; ++v)
            {
                t.V[v][0] = w[v].x;
                t.V[v][1] = w[v].y;
                t.V[v][2] = w[v].z;
            }
            t.Kind = kind;
            return t;
        };

        G3D::AABox ours;
        bool any = false;
        for (GroupSource const& group : found->second)
        {
            for (G3D::Vector3 const& v : group.Vertices)
            {
                G3D::Vector3 const p = internal(v);
                ours.merge(p);
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
            auto const type = liquidTypes.find(liquid.Type);
            uint8_t kind = 0;
            if (type == liquidTypes.end())
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
                    std::max(bound.low()[a] - ours.low()[a], ours.high()[a] - bound.high()[a]));
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

    Sc::SceneHeader header;
    header.MapId = mapId;
    header.Flags = (tiled ? Sc::TILED : 0u) | (liquid.Geom.empty() ? 0u : Sc::HAS_MODEL_LIQUID);
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
    while (file.size() % Sc::SECTION_ALIGN)
        file.push_back(0);
    std::memcpy(file.data(), &header, sizeof(header));
    report.Checksum = Sc::ChecksumOf(file.data(), file.size());
    std::memcpy(file.data() + offsetof(Sc::SceneHeader, Checksum), &report.Checksum, sizeof(report.Checksum));
    report.FileBytes = file.size();

    std::error_code ec;
    Fs::create_directories(Fs::path(outPath).parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<char const*>(file.data()), std::streamsize(file.size())))
    {
        error = "cannot write " + outPath;
        return false;
    }
    report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}
