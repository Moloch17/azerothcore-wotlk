// navcheck: a traversability bake (.trav, TravBake.cpp) against the server's own navmesh (mmaps, as MMapMgr loads
// them). Read-only. Built and run by navcheck.sh in a dev container.
//
//   navcheck <map.trav> <mmaps dir> [pairs] [seed]
//
// Connectivity: for random pairs of the bake's Reachable nodes, whether the bake connects them (its directed edges,
// drops included) and whether the navmesh walks between them with every corner inside the bake's box -- "navmesh
// only" pairs are connections the bake is missing. The navmesh has no drops, so "bake only" is expected where a
// drop is the way. Coverage: the navmesh polygons inside the box whose centre has no bake floor within COVER_RISE
// yards of its height -- floors the bake never found (stacked levels, a version 1 bake's one floor a cell) --
// counted by height band.
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <queue>
#include <random>
#include <string>
#include <vector>

namespace
{
    constexpr float TRAV_CELL = 2.0f;
    constexpr float NO_HEIGHT = -200000.0f;
    constexpr int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    constexpr int DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    constexpr float COVER_RISE = 1.5f;
    char const* const NAMES[] = { "unknown", "free", "wall", "water", "hazard", "step_up", "jump", "drop_safe",
        "drop_hurt", "drop_lethal", "slope" };

#pragma pack(push, 1)
    struct TravHeader
    {
        uint32_t Magic, Version, MapId, Columns, Rows, Seed;
        float Box[4];
        uint64_t Digest;
        uint32_t Reachable;
        uint32_t Reserved[3];
    };
#pragma pack(pop)
    struct TileHeader { uint32_t magic, dtVersion, mmapVersion, size; char liquids; char pad[3]; uint8_t config[36]; };

    struct Bake
    {
        TravHeader H{};
        int Layers = 1;
        std::vector<uint8_t> Classes, Edges;
        std::vector<float> Ground, Liquid, Hurt;
        std::vector<uint32_t> Reachable;
        uint32_t Cells() const { return H.Columns * H.Rows; }
        uint32_t Nodes() const { return Cells() * uint32_t(Layers); }
        int Kind(uint8_t e) const { return Layers > 1 ? (e & 0x0F) : e; }
        int Layer(uint8_t e) const { return Layers > 1 ? ((e >> 4) & 0x07) : 0; }
        float CentreX(uint32_t cell) const { return H.Box[0] + (float(cell % H.Columns) + 0.5f) * TRAV_CELL; }
        float CentreY(uint32_t cell) const { return H.Box[1] + (float(cell / H.Columns) + 0.5f) * TRAV_CELL; }
        long CellOf(float x, float y) const
        {
            int const c = int(std::floor((x - H.Box[0]) / TRAV_CELL));
            int const r = int(std::floor((y - H.Box[1]) / TRAV_CELL));
            return c < 0 || r < 0 || c >= int(H.Columns) || r >= int(H.Rows) ? -1 : long(r) * H.Columns + c;
        }
        long Neighbour(uint32_t cell, int dir) const
        {
            int const c = int(cell % H.Columns) + DX[dir];
            int const r = int(cell / H.Columns) + DY[dir];
            return c < 0 || r < 0 || c >= int(H.Columns) || r >= int(H.Rows) ? -1 : long(r) * H.Columns + c;
        }
        long Target(uint32_t node, int dir) const
        {
            uint8_t const e = Edges[size_t(node) * 8 + size_t(dir)];
            if (!Kind(e))
                return -1;
            long const cell = Neighbour(node / uint32_t(Layers), dir);
            return cell < 0 ? -1 : cell * Layers + Layer(e);
        }
        bool Inside(float x, float y) const { return x >= H.Box[0] && x < H.Box[2] && y >= H.Box[1] && y < H.Box[3]; }
    };

    template <typename T>
    bool ReadAll(FILE* f, std::vector<T>& out, size_t count)
    {
        out.resize(count);
        return fread(out.data(), sizeof(T), count, f) == count;
    }

    bool LoadBake(char const* path, Bake& bake)
    {
        FILE* f = fopen(path, "rb");
        if (!f || fread(&bake.H, sizeof(bake.H), 1, f) != 1)
            return false;
        bake.Layers = bake.H.Version >= 2 && bake.H.Reserved[0] ? int(bake.H.Reserved[0]) : 1;
        size_t const n = bake.Nodes();
        bool const ok = ReadAll(f, bake.Classes, n) && ReadAll(f, bake.Ground, n) && ReadAll(f, bake.Liquid, n)
            && ReadAll(f, bake.Edges, n * 8) && ReadAll(f, bake.Hurt, n * 8)
            && ReadAll(f, bake.Reachable, bake.H.Reachable);
        fclose(f);
        return ok;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: navcheck <map.trav> <mmaps dir> [pairs] [seed]\n");
        return 2;
    }
    Bake bake;
    if (!LoadBake(argv[1], bake))
    {
        printf("cannot read %s\n", argv[1]);
        return 1;
    }
    int const pairs = argc > 3 ? atoi(argv[3]) : 300;
    std::mt19937 rng(argc > 4 ? unsigned(atoi(argv[4])) : 1u);
    std::string const dir = argv[2];
    char mapName[16];
    snprintf(mapName, sizeof(mapName), "%03u", bake.H.MapId);
    printf("bake map %u version %u, %u x %u cells, %d layer(s), box (%.0f,%.0f)-(%.0f,%.0f), %zu reachable\n",
        bake.H.MapId, bake.H.Version, bake.H.Columns, bake.H.Rows, bake.Layers, bake.H.Box[0], bake.H.Box[1],
        bake.H.Box[2], bake.H.Box[3], bake.Reachable.size());

    // The navmesh, as MMapMgr loads it.
    FILE* f = fopen((dir + "/" + mapName + ".mmap").c_str(), "rb");
    dtNavMeshParams params;
    if (!f || fread(&params, sizeof(params), 1, f) != 1)
    {
        printf("no %s.mmap in %s\n", mapName, dir.c_str());
        return 1;
    }
    fclose(f);
    dtNavMesh* mesh = dtAllocNavMesh();
    mesh->init(&params);
    DIR* d = opendir(dir.c_str());
    int tiles = 0;
    while (dirent* e = readdir(d))
    {
        std::string const name = e->d_name;
        if (name.size() < 10 || name.compare(0, 3, mapName) != 0 || name.substr(name.size() - 7) != ".mmtile")
            continue;
        FILE* t = fopen((dir + "/" + name).c_str(), "rb");
        TileHeader h;
        if (fread(&h, sizeof(h), 1, t) != 1)
        {
            fclose(t);
            continue;
        }
        unsigned char* data = static_cast<unsigned char*>(dtAlloc(h.size, DT_ALLOC_PERM));
        if (fread(data, h.size, 1, t) == 1
            && dtStatusSucceed(mesh->addTile(data, int(h.size), DT_TILE_FREE_DATA, 0, nullptr)))
            ++tiles;
        fclose(t);
    }
    closedir(d);
    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    query->init(mesh, 65535);
    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    printf("navmesh: %d tiles\n", tiles);

    // Coverage: every navmesh polygon centre inside the box against the bake's floors there.
    std::map<int, std::pair<int, int>> bands;   // 10-yd band -> (polygons, missing from the bake)
    for (int i = 0; i < mesh->getMaxTiles(); ++i)
    {
        dtMeshTile const* tile = static_cast<dtNavMesh const*>(mesh)->getTile(i);
        if (!tile || !tile->header)
            continue;
        for (int p = 0; p < tile->header->polyCount; ++p)
        {
            dtPoly const& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND || !poly.vertCount)
                continue;
            float c[3] = { 0, 0, 0 };
            for (int v = 0; v < poly.vertCount; ++v)
                for (int k = 0; k < 3; ++k)
                    c[k] += tile->verts[poly.verts[v] * 3 + k] / float(poly.vertCount);
            float const x = c[2], y = c[0], z = c[1];       // Detour (y, z, x) is WoW (x, y, z)
            if (!bake.Inside(x, y))
                continue;
            long const cell = bake.CellOf(x, y);
            bool found = false;
            for (int l = 0; l < bake.Layers && cell >= 0 && !found; ++l)
            {
                size_t const node = size_t(cell) * bake.Layers + l;
                found = bake.Ground[node] != NO_HEIGHT && std::fabs(bake.Ground[node] - z) <= COVER_RISE;
            }
            auto& band = bands[int(std::floor(z / 10.0f)) * 10];
            ++band.first;
            band.second += found ? 0 : 1;
        }
    }
    int all = 0, missing = 0;
    printf("coverage (navmesh polygons in the box, by height; missing = no bake floor within %.1f yd):\n", COVER_RISE);
    for (auto const& [z, count] : bands)
    {
        printf("  z %5d..%-5d %6d polygons, %6d missing (%.0f%%)\n", z, z + 10, count.first, count.second,
            100.0 * count.second / std::max(1, count.first));
        all += count.first;
        missing += count.second;
    }
    printf("  all: %d polygons, %d missing (%.1f%%)\n", all, missing, 100.0 * missing / std::max(1, all));

    // Connectivity over random pairs of reachable nodes.
    if (bake.Reachable.size() < 2)
        return 0;
    std::vector<uint8_t> seen(bake.Nodes());
    int both = 0, bakeOnly = 0, meshOnly = 0, neither = 0, unsnapped = 0, shown = 0;
    for (int i = 0; i < pairs; ++i)
    {
        uint32_t const a = bake.Reachable[rng() % bake.Reachable.size()];
        uint32_t const b = bake.Reachable[rng() % bake.Reachable.size()];
        if (a == b)
            continue;
        std::fill(seen.begin(), seen.end(), 0);
        std::queue<uint32_t> open;
        open.push(a);
        seen[a] = 1;
        bool bakeLinks = false;
        while (!open.empty() && !bakeLinks)
        {
            uint32_t const n = open.front();
            open.pop();
            for (int dir = 0; dir < 8; ++dir)
            {
                long const m = bake.Target(n, dir);
                if (m >= 0 && !seen[size_t(m)])
                {
                    seen[size_t(m)] = 1;
                    bakeLinks = bakeLinks || uint32_t(m) == b;
                    open.push(uint32_t(m));
                }
            }
        }
        uint32_t const ca = a / uint32_t(bake.Layers), cb = b / uint32_t(bake.Layers);
        float pa[3] = { bake.CentreY(ca), bake.Ground[a], bake.CentreX(ca) };
        float pb[3] = { bake.CentreY(cb), bake.Ground[b], bake.CentreX(cb) };
        float const extents[3] = { 1.5f, 2.0f, 1.5f };
        dtPolyRef ra = 0, rb = 0;
        float na[3], nb[3];
        query->findNearestPoly(pa, extents, &filter, &ra, na);
        query->findNearestPoly(pb, extents, &filter, &rb, nb);
        if (!ra || !rb)
        {
            ++unsnapped;
            continue;
        }
        std::vector<dtPolyRef> polys(8192);
        int count = 0;
        dtStatus const status = query->findPath(ra, rb, na, nb, &filter, polys.data(), &count, int(polys.size()));
        bool meshLinks = count > 0 && !(status & DT_PARTIAL_RESULT) && polys[count - 1] == rb;
        if (meshLinks)
        {
            std::vector<float> corners(3 * 1024);
            int n = 0;
            query->findStraightPath(na, nb, polys.data(), count, corners.data(), nullptr, nullptr, &n, 1024);
            for (int k = 0; k < n && meshLinks; ++k)
                meshLinks = bake.Inside(corners[3 * k + 2], corners[3 * k]);
        }
        if (bakeLinks && meshLinks)
            ++both;
        else if (bakeLinks)
            ++bakeOnly;
        else if (meshLinks)
        {
            ++meshOnly;
            if (shown++ < 8)
                printf("  navmesh only: (%.0f,%.0f,%.1f) %s -> (%.0f,%.0f,%.1f) %s\n", bake.CentreX(ca),
                    bake.CentreY(ca), bake.Ground[a], NAMES[std::min<int>(bake.Classes[a], 10)], bake.CentreX(cb),
                    bake.CentreY(cb), bake.Ground[b], NAMES[std::min<int>(bake.Classes[b], 10)]);
        }
        else
            ++neither;
    }
    printf("connectivity over %d pairs: both %d, bake only %d (a drop, or the navmesh leaves the box), navmesh only %d"
        " (the bake is missing a way), neither %d, off the navmesh %d\n", pairs, both, bakeOnly, meshOnly, neither,
        unsnapped);
    return 0;
}
