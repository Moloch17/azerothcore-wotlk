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
#include "BakedWorld.h"
#include "Bench.h"
#include "SceneRegistry.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
    int Usage(char const* program)
    {
        std::printf(
            "usage:\n"
            "  %1$s bake <data dir> <map id> [out file]    bake <data dir>/scenes/<map>.scene from vmaps/, maps/ and dbc/\n"
            "  %1$s ensure <data dir> <scene dir> <map id>...\n"
            "                                              what the worldserver does at startup: load each scene if it\n"
            "                                              is whole and current, else bake it (and say why)\n"
            "  %1$s info <scene file>                      print a scene's header\n"
            "  %1$s bench <scene> <poses.txt> <out dir> [reps]\n"
            "                                              render each pose (name x y z yawDeg pitchDeg zoom a line)\n"
            "                                              with Vision::Render, time it, write the images\n"
            "  %1$s verify <scene> <poses.txt> [random rays]\n"
            "                                              check the tracer against a brute-force triangle test\n",
            program);
        return 1;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return Usage(argv[0]);
    std::string const command = argv[1];
    std::string error;

    if (command == "bake" && argc >= 4)
    {
        uint32_t const mapId = uint32_t(std::strtoul(argv[3], nullptr, 10));
        std::string const out = argc >= 5 ? argv[4]
            : (std::filesystem::path(argv[2]) / "scenes" / SceneBaker::SceneFileName(mapId)).string();
        SceneBaker::BakeReport report;
        if (!SceneBaker::BakeMap(argv[2], mapId, out, report, error))
        {
            std::printf("bake of map %u failed: %s\n", mapId, error.c_str());
            return 1;
        }
        std::printf("map %u -> %s\n  %u spawns (%u M2), %u distinct models, worst vertex outside a spawn bound "
            "%.4f yd\n"
            "  %u solid triangles (%u dropped), %u BVH nodes; %u liquid triangles (%u unknown liquid types), %u nodes; "
            "max depth %u\n  terrain: %u tiles (%u with heights, %u with holes, %u with liquid)\n"
            "  %llu bytes, checksum %016llx, source %016llx, %.2f s\n", mapId, out.c_str(), report.Spawns,
            report.M2Spawns, report.Models, report.WorstBoundExcess, report.Triangles, report.DroppedTriangles,
            report.Nodes, report.LiquidTriangles, report.UnknownLiquidTypes, report.LiquidNodes, report.MaxDepth,
            report.TerrainTiles, report.TerrainHeightTiles, report.TerrainHoleTiles, report.TerrainLiquidTiles,
            (unsigned long long)report.FileBytes, (unsigned long long)report.Checksum,
            (unsigned long long)report.SourceHash, report.Seconds);
        return 0;
    }

    if (command == "ensure" && argc >= 5)
    {
        int failed = 0;
        for (int i = 4; i < argc; ++i)
        {
            uint32_t const mapId = uint32_t(std::strtoul(argv[i], nullptr, 10));
            Animus::Vision::SceneEnsure result;
            if (!Animus::Vision::SceneRegistry::Instance().Ensure(argv[2], argv[3], mapId, result, error))
            {
                std::printf("map %u FAILED: %s\n", mapId, error.c_str());
                ++failed;
                continue;
            }
            std::string const how = result.Baked ? "baked (" + result.Reason + ")" : "loaded";
            std::printf("map %u %s: %u triangles, %u nodes, %u terrain tiles, %llu bytes, checksum %016llx, "
                "%.3f s\n", mapId, how.c_str(), result.Triangles, result.Nodes, result.TerrainTiles,
                (unsigned long long)result.Bytes, (unsigned long long)result.Checksum, result.Seconds);
        }
        return failed ? 1 : 0;
    }

    if (command == "info" && argc >= 3)
    {
        Animus::Vision::BakedWorld world;
        if (!world.Load(argv[2], error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        auto const& h = world.Header();
        std::printf("map %u, version %u (baker %u), flags %x, %zu bytes, checksum %016llx, source %016llx\n  solid box (%.2f %.2f %.2f) - "
            "(%.2f %.2f %.2f)\n  %u triangles, %u nodes; %u liquid triangles, %u nodes; %u spawns (%u M2); %u terrain tiles\n",
            h.MapId, h.Version, h.BakerVersion, h.Flags, world.FileBytes(), (unsigned long long)h.Checksum,
            (unsigned long long)h.SourceHash, h.SolidMin[0],
            h.SolidMin[1], h.SolidMin[2], h.SolidMax[0], h.SolidMax[1], h.SolidMax[2], h.TriCount, h.NodeCount,
            h.LiquidTriCount, h.LiquidNodeCount, h.SourceSpawnCount, h.SourceM2Count, h.TerrainTileCount);
        return 0;
    }

    if (command == "bench" && argc >= 5)
    {
        std::vector<SceneBaker::PoseSpec> poses;
        if (!SceneBaker::ReadPoses(argv[3], poses, error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        uint32_t const reps = argc >= 6 ? uint32_t(std::strtoul(argv[5], nullptr, 10)) : 400;
        if (!SceneBaker::RunBench(argv[2], poses, argv[4], reps, error))
        {
            std::printf("bench failed: %s\n", error.c_str());
            return 1;
        }
        return 0;
    }

    if (command == "verify" && argc >= 4)
    {
        std::vector<SceneBaker::PoseSpec> poses;
        if (!SceneBaker::ReadPoses(argv[3], poses, error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        uint32_t const randomRays = argc >= 5 ? uint32_t(std::strtoul(argv[4], nullptr, 10)) : 20000;
        if (!SceneBaker::RunVerify(argv[2], poses, randomRays, error))
        {
            std::printf("verify failed%s%s\n", error.empty() ? "" : ": ", error.c_str());
            return 1;
        }
        std::printf("verify ok\n");
        return 0;
    }

    return Usage(argv[0]);
}
