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

#include "Bench.h"
#include "BakedWorld.h"
#include "PlayerController.h"
#include "VisionCaster.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    namespace Vi = Animus::Vision;
    namespace Sc = Animus::Vision::Scene;
    using Clock = std::chrono::steady_clock;

    /// The class colours of the audit's class panel (FrameImage.cpp's CLASS_COLOURS, copied: that file is not part of
    /// the runtime), for the kind image.
    constexpr uint8_t CLASS_COLOURS[Vi::CLASSES][3] = {
        { 30, 30, 80 }, { 60, 160, 60 }, { 160, 160, 160 }, { 170, 100, 40 }, { 40, 90, 220 }, { 240, 80, 0 },
        { 220, 0, 0 }, { 230, 230, 0 }, { 0, 230, 160 }, { 255, 0, 170 }, { 0, 160, 255 }, { 255, 170, 0 },
        { 170, 90, 230 }, { 110, 60, 180 }, { 255, 120, 120 }, { 110, 40, 40 }, { 200, 150, 60 }, { 120, 255, 80 },
        { 120, 180, 200 }, { 40, 40, 200 }, { 255, 255, 140 }, { 0, 255, 255 }, { 120, 90, 60 }, { 255, 40, 220 } };

    /// The baked static half and stand-ins for the rest: no doors, no game objects, no liquid at the camera. The floor
    /// under the pivot (the scalar the frame reports) is a down cast against the baked solids.
    class BenchWorld final : public Vi::VisionWorld
    {
    public:
        explicit BenchWorld(Vi::BakedWorld const& baked) : _baked(baked) { }

        [[nodiscard]] Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            return _baked.StaticHit(from, to);
        }
        [[nodiscard]] Vi::SurfaceHit DynamicHit(Vi::Vec3, Vi::Vec3) const override { return {}; }
        [[nodiscard]] Vi::LiquidHit ModelLiquid(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            return _baked.ModelLiquid(from, to);
        }
        [[nodiscard]] bool StaticAnyHit(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            return _baked.StaticAnyHit(from, to);
        }
        [[nodiscard]] bool DynamicAnyHit(Vi::Vec3, Vi::Vec3) const override { return false; }
        [[nodiscard]] Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            return _baked.Tile(tileX, tileY);
        }
        [[nodiscard]] Vi::TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY,
            bool liquid) const override
        {
            return _baked.Cell(tileX, tileY, cellX, cellY, liquid);
        }
        [[nodiscard]] Animus::Movement::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float FloorBelow(float x, float y, float z, float search) const override
        {
            Vi::SurfaceHit const hit = _baked.StaticHit({ x, y, z }, { x, y, z - search });
            return hit.Distance >= 0.0f ? z - hit.Distance : Animus::Movement::INVALID_FLOOR;
        }

    private:
        Vi::BakedWorld const& _baked;
    };

    /// A pose as `forge camera snapshot` sets it up: the settings' defaults, a default body, no units.
    struct Setup
    {
        Vi::Settings Settings;
        Vi::CameraState Camera;
        Vi::Pose Pose;
        Vi::Rig Rig;
    };

    Setup SetupOf(BenchWorld const& world, SceneBaker::PoseSpec const& spec)
    {
        Setup setup;
        setup.Camera.Pitch = std::clamp(spec.PitchDeg, -80.0f, 80.0f) * Vi::DEGREES;
        setup.Camera.Zoom = std::clamp(spec.Zoom, 0.0f, 50.0f);
        setup.Pose.X = spec.X;
        setup.Pose.Y = spec.Y;
        setup.Pose.Z = spec.Z;
        setup.Pose.Yaw = spec.YawDeg * Vi::DEGREES;
        setup.Pose.BodyHeight = Animus::Movement::Body().Height;
        setup.Rig = Vi::PlaceCamera(setup.Pose, setup.Camera, world);
        return setup;
    }

    double Micros(Clock::time_point from, Clock::time_point to)
    {
        return std::chrono::duration<double, std::micro>(to - from).count();
    }

    /// Decoded as the learner decodes the bytes, and written as `forge camera snapshot` writes them.
    void WriteImages(Vi::Settings const& settings, std::vector<uint8_t> const& image, std::string const& base)
    {
        uint32_t const pixels = settings.Width * settings.Height;
        std::string depth;
        std::string kind;
        std::string rise;
        std::string slope;
        auto const byte = [](float value) { return char(uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f)); };
        for (uint32_t pixel = 0; pixel < pixels; ++pixel)
        {
            float decoded[Vi::DECODED_VALUES];
            Vi::DecodePixel(&image[std::size_t(pixel) * Vi::BYTES_PER_PIXEL], decoded);
            depth += byte(decoded[Vi::CHANNEL_DISTANCE]);
            rise += byte((decoded[Vi::CHANNEL_HEIGHT] + 1.0f) / 2.0f);
            slope += byte(decoded[Vi::CHANNEL_NORMAL]);
            uint32_t const what = std::min<uint32_t>(uint32_t(decoded[Vi::CHANNEL_CLASS]), Vi::CLASSES - 1);
            bool const objective = decoded[Vi::CHANNEL_OBJECTIVE] > 0.5f;
            for (uint32_t c = 0; c < 3; ++c)
                kind += char(objective ? 255 : CLASS_COLOURS[what][c]);
        }
        auto const write = [&](std::string const& path, char const* magic, std::string const& data)
        {
            std::ofstream out(path, std::ios::binary);
            out << magic << "\n" << settings.Width << " " << settings.Height << "\n255\n" << data;
        };
        write(base + "-depth.pgm", "P5", depth);
        write(base + "-kind.ppm", "P6", kind);
        write(base + "-height.pgm", "P5", rise);
        write(base + "-normal.pgm", "P5", slope);
    }

    struct Stats
    {
        double Median = 0.0;
        double Min = 0.0;
        double Mean = 0.0;
        double P90 = 0.0;
    };

    Stats StatsOf(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        Stats stats;
        stats.Min = values.front();
        stats.Median = values[values.size() / 2];
        stats.P90 = values[std::size_t(double(values.size() - 1) * 0.9)];
        double sum = 0.0;
        for (double v : values)
            sum += v;
        stats.Mean = sum / double(values.size());
        return stats;
    }

    // ---------------------------------------------------------------------------------------- brute force
    /// A triangle test written apart from the tracer's: double precision, the same two-sided Moller-Trumbore.
    double BruteTriangle(double const* o, double const* d, Sc::TriGeom const& tri, double slack)
    {
        double e1[3];
        double e2[3];
        double v0[3];
        for (int i = 0; i < 3; ++i)
        {
            e1[i] = tri.E1[i];
            e2[i] = tri.E2[i];
            v0[i] = tri.V0[i];
        }
        double const p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
        double const det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-18)
            return -1.0;
        double const inv = 1.0 / det;
        double const s[3] = { o[0] - v0[0], o[1] - v0[1], o[2] - v0[2] };
        double const u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < -slack || u > 1.0 + slack)
            return -1.0;
        double const q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
        double const v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < -slack || u + v > 1.0 + slack)
            return -1.0;
        return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    }

    struct BruteResult
    {
        double T = -1.0;
        uint32_t Index = 0;
    };

    BruteResult Brute(Sc::TriGeom const* tris, uint32_t count, Vi::Vec3 from, Vi::Vec3 to, double slack)
    {
        double const dx = double(to.X) - from.X;
        double const dy = double(to.Y) - from.Y;
        double const dz = double(to.Z) - from.Z;
        double const length = std::sqrt(dx * dx + dy * dy + dz * dz);
        BruteResult best;
        if (!(length > 1e-6))
            return best;
        double const o[3] = { from.X, from.Y, from.Z };
        double const d[3] = { dx / length, dy / length, dz / length };
        for (uint32_t i = 0; i < count; ++i)
        {
            double const t = BruteTriangle(o, d, tris[i], slack);
            if (t >= 0.0 && t <= length && (best.T < 0.0 || t < best.T))
            {
                best.T = t;
                best.Index = i;
            }
        }
        return best;
    }

    struct Agreement
    {
        uint64_t Rays = 0;
        uint64_t SolidBothHit = 0;
        uint64_t SolidBothNone = 0;
        uint64_t SolidMismatch = 0;         // one saw a hit, the other did not, or the distances differ
        uint64_t NormalMismatch = 0;
        uint64_t LiquidBoth = 0;
        uint64_t LiquidNone = 0;
        uint64_t LiquidMismatch = 0;
        uint64_t AnyHitMismatch = 0;
        uint64_t Sweep[5] = {};
        double WorstDistance = 0.0;
    };

    void Compare(Vi::BakedWorld const& baked, Vi::Vec3 from, Vi::Vec3 to, Agreement& total)
    {
        ++total.Rays;
        Sc::SceneHeader const& header = baked.Header();
        BruteResult const solid = Brute(baked.Triangles(), header.TriCount, from, to, double(Vi::BAKED_EDGE_SLACK));
        // The cracks left: a hit nearer than the tracer's by more than 2 cm once triangles are fattened to 2 percent,
        // and the share of rays an exact (slack 0) test would have let through a crack the tracer closes.
        BruteResult const exact = Brute(baked.Triangles(), header.TriCount, from, to, 0.0);
        static const double SWEEP[5] = { 1e-5, 1e-4, 1e-3, 3e-3, 1e-2 };
        for (int k = 0; k < 5; ++k)
        {
            BruteResult const fat = Brute(baked.Triangles(), header.TriCount, from, to, SWEEP[k]);
            if (fat.T >= 0.0 && (exact.T < 0.0 || exact.T > fat.T + 1e-3))
                ++total.Sweep[k];
        }
        Vi::SurfaceHit const hit = baked.StaticHit(from, to);
        bool const anyHit = baked.StaticAnyHit(from, to);
        if (anyHit != (solid.T >= 0.0))
        {
            // A hit within rounding of the segment's end or of an edge is a tie the two tests may break differently.
            ++total.AnyHitMismatch;
        }
        if (solid.T < 0.0 && hit.Distance < 0.0)
            ++total.SolidBothNone;
        else if (solid.T >= 0.0 && hit.Distance >= 0.0)
        {
            double const diff = std::fabs(double(hit.Distance) - solid.T);
            total.WorstDistance = std::max(total.WorstDistance, diff);
            if (diff > 1e-3 + 1e-5 * solid.T)
                ++total.SolidMismatch;
            else
            {
                ++total.SolidBothHit;
                // The normal must match the brute force's triangle, turned to face the ray.
                Sc::TriNormal const& n = baked.Normals()[solid.Index];
                double const dx = double(to.X) - from.X;
                double const dy = double(to.Y) - from.Y;
                double const dz = double(to.Z) - from.Z;
                double const length = std::sqrt(dx * dx + dy * dy + dz * dz);
                double const along = (n.N[0] * dx + n.N[1] * dy + n.N[2] * dz) / length;
                double const expect = along > 0.0 ? -double(n.N[2]) : double(n.N[2]);
                if (std::fabs(expect - double(hit.NormalZ)) > 1e-3)
                    ++total.NormalMismatch;
            }
        }
        else
            ++total.SolidMismatch;

        if (baked.HasLiquid())
        {
            BruteResult const liquid = Brute(baked.LiquidTriangles(), header.LiquidTriCount, from, to,
                double(Vi::BAKED_EDGE_SLACK));
            Vi::LiquidHit const found = baked.ModelLiquid(from, to);
            if (liquid.T < 0.0 && found.Distance < 0.0)
                ++total.LiquidNone;
            else if (liquid.T >= 0.0 && found.Distance >= 0.0
                && std::fabs(double(found.Distance) - liquid.T) <= 1e-3 + 1e-5 * liquid.T
                && found.Deadly == ((baked.LiquidKinds()[liquid.Index] & Sc::LIQUID_DEADLY) != 0))
                ++total.LiquidBoth;
            else
                ++total.LiquidMismatch;
        }
    }
}

bool SceneBaker::ReadPoses(std::string const& path, std::vector<PoseSpec>& poses, std::string& error)
{
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot read " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream words(line);
        PoseSpec pose;
        if (!(words >> pose.Name >> pose.X >> pose.Y >> pose.Z >> pose.YawDeg >> pose.PitchDeg >> pose.Zoom))
        {
            error = "bad pose line: " + line;
            return false;
        }
        poses.push_back(pose);
    }
    return !poses.empty();
}

bool SceneBaker::RunBench(std::string const& scenePath, std::vector<PoseSpec> const& poses, std::string const& outDir,
    uint32_t reps, std::string& error)
{
    Vi::BakedWorld baked;
    auto const loadStart = Clock::now();
    if (!baked.Load(scenePath, error))
        return false;
    double const loadUs = Micros(loadStart, Clock::now());
    BenchWorld const world(baked);
    std::filesystem::create_directories(outDir);

    std::ostringstream json;
    json << "{\n \"scene\": \"" << scenePath << "\", \"load_us\": " << loadUs << ", \"file_bytes\": "
         << baked.FileBytes() << ", \"reps\": " << reps << ",\n \"poses\": {\n";
    bool first = true;
    for (PoseSpec const& spec : poses)
    {
        Setup const setup = SetupOf(world, spec);
        std::vector<uint8_t> image(Vi::ImageBytes(setup.Settings));
        std::array<float, Vi::SCALARS> scalars{};
        auto const render = [&](Vi::Breakdown* breakdown)
        {
            return Vi::Render(setup.Settings, setup.Rig, setup.Pose, setup.Camera, world, {}, nullptr, image.data(),
                scalars.data(), breakdown);
        };

        uint32_t rays = 0;
        for (uint32_t i = 0; i < 50; ++i)
            rays = render(nullptr);
        std::vector<double> wall;
        wall.reserve(reps);
        for (uint32_t i = 0; i < reps; ++i)
        {
            auto const start = Clock::now();
            render(nullptr);
            wall.push_back(Micros(start, Clock::now()));
        }
        Stats const stats = StatsOf(wall);

        // The breakdown's clocks cost something of their own: they are timed apart and the remainder is "other".
        Vi::Breakdown breakdown;
        constexpr uint32_t BREAKDOWN_RUNS = 100;
        double breakdownWall = 0.0;
        for (uint32_t i = 0; i < BREAKDOWN_RUNS; ++i)
        {
            auto const start = Clock::now();
            render(&breakdown);
            breakdownWall += Micros(start, Clock::now());
        }
        double const runs = double(BREAKDOWN_RUNS);
        double const staticUs = double(breakdown.StaticNs) / 1e3 / runs;
        double const liquidUs = double(breakdown.LiquidNs) / 1e3 / runs;
        double const terrainUs = double(breakdown.TerrainNs) / 1e3 / runs;
        double const hazardUs = double(breakdown.HazardNs) / 1e3 / runs;
        double const dynamicUs = double(breakdown.DynamicNs) / 1e3 / runs;
        double const otherUs = breakdownWall / runs - staticUs - liquidUs - terrainUs - hazardUs - dynamicUs;

        // The same frame's parts timed alone, each over the frame's own pixel rays: the tracer on its own (static
        // cast, then the liquid cast), the grid-extent walk (Reach), and the direction and encoding of every pixel.
        // What is left of the frame's wall time is the caster's own bookkeeping.
        std::vector<Vi::Vec3> dirs;
        std::vector<Vi::Vec3> ends;
        for (uint32_t row = 0; row < setup.Settings.Height; ++row)
            for (uint32_t col = 0; col < setup.Settings.Width; ++col)
            {
                Vi::Vec3 const dir = Vi::PixelDirection(setup.Rig, setup.Settings, row, col);
                dirs.push_back(dir);
                ends.push_back(setup.Rig.Camera + dir * Vi::Reach(setup.Rig.Camera, dir, world));
            }
        auto const timeLoop = [&](auto&& body)
        {
            std::vector<double> samples;
            for (uint32_t i = 0; i < 20 + reps / 4; ++i)
            {
                auto const start = Clock::now();
                body();
                if (i >= 20)
                    samples.push_back(Micros(start, Clock::now()));
            }
            return StatsOf(samples).Median;
        };
        volatile float sink = 0.0f;
        double const traceUs = timeLoop([&]()
        {
            float total = 0.0f;
            for (std::size_t i = 0; i < ends.size(); ++i)
                total += baked.StaticHit(setup.Rig.Camera, ends[i]).Distance;
            sink = total;
        });
        double const liquidAloneUs = timeLoop([&]()
        {
            float total = 0.0f;
            for (std::size_t i = 0; i < ends.size(); ++i)
                if (dirs[i].Z < 0.0f)
                    total += baked.ModelLiquid(setup.Rig.Camera, ends[i]).Distance;
            sink = total;
        });
        double const reachUs = timeLoop([&]()
        {
            float total = 0.0f;
            for (std::size_t i = 0; i < dirs.size(); ++i)
                total += Vi::Reach(setup.Rig.Camera, dirs[i], world);
            sink = total;
        });
        double const pixelUs = timeLoop([&]()
        {
            float total = 0.0f;
            uint8_t out[Vi::BYTES_PER_PIXEL];
            for (uint32_t row = 0; row < setup.Settings.Height; ++row)
                for (uint32_t col = 0; col < setup.Settings.Width; ++col)
                {
                    Vi::Vec3 const dir = Vi::PixelDirection(setup.Rig, setup.Settings, row, col);
                    Vi::Hit hit;
                    hit.Distance = 5.0f + dir.X;
                    hit.What = Vi::Class::Model;
                    hit.Z = dir.Z;
                    Vi::EncodePixel(hit, setup.Pose.Z, false, out);
                    total += float(out[0]);
                }
            sink = total;
        });
        (void)sink;

        render(nullptr);
        WriteImages(setup.Settings, image, outDir + "/newcam-" + spec.Name);

        std::printf("%-16s rays %u  median %.0f us  min %.0f us  mean %.0f us  p90 %.0f us  (%.3f us/ray)  "
            "breakdown: static %.0f (%u casts), liquid %.0f (%u), terrain %.0f (%u grids), other %.0f | "
            "alone: trace %.0f, "
            "liquid %.0f, reach %.0f, pixel dir+encode %.0f\n",
            spec.Name.c_str(), rays, stats.Median, stats.Min, stats.Mean, stats.P90, stats.Median / double(rays),
            staticUs, breakdown.StaticCasts / BREAKDOWN_RUNS, liquidUs, breakdown.LiquidCasts / BREAKDOWN_RUNS,
            terrainUs, breakdown.TerrainTiles / BREAKDOWN_RUNS, otherUs, traceUs, liquidAloneUs, reachUs, pixelUs);

        json << (first ? "" : ",\n") << "  \"" << spec.Name << "\": {\"pose\": [" << spec.X << ", " << spec.Y << ", "
             << spec.Z << ", " << spec.YawDeg << ", " << spec.PitchDeg << ", " << spec.Zoom << "], \"rays\": " << rays
             << ", \"median_us\": " << stats.Median << ", \"min_us\": " << stats.Min << ", \"mean_us\": " << stats.Mean
             << ", \"p90_us\": " << stats.P90 << ", \"us_per_ray\": " << stats.Median / double(rays)
             << ", \"static_us\": " << staticUs << ", \"static_casts\": " << breakdown.StaticCasts / BREAKDOWN_RUNS
             << ", \"liquid_us\": " << liquidUs << ", \"liquid_casts\": " << breakdown.LiquidCasts / BREAKDOWN_RUNS
             << ", \"terrain_us\": " << terrainUs << ", \"terrain_grids\": " << breakdown.TerrainTiles / BREAKDOWN_RUNS
             << ", \"other_us\": " << otherUs << ", \"trace_alone_us\": " << traceUs << ", \"liquid_alone_us\": "
             << liquidAloneUs << ", \"reach_alone_us\": " << reachUs << ", \"pixel_alone_us\": " << pixelUs << "}";
        first = false;
    }
    json << "\n }\n}\n";
    std::ofstream out(outDir + "/new_timing.json");
    out << json.str();
    return bool(out);
}

bool SceneBaker::RunVerify(std::string const& scenePath, std::vector<PoseSpec> const& poses, uint32_t randomRays,
    std::string& error)
{
    Vi::BakedWorld baked;
    if (!baked.Load(scenePath, error))
        return false;
    BenchWorld const world(baked);
    Sc::SceneHeader const& header = baked.Header();
    Agreement pixels;
    for (PoseSpec const& spec : poses)
    {
        Setup const setup = SetupOf(world, spec);
        for (uint32_t row = 0; row < setup.Settings.Height; ++row)
            for (uint32_t col = 0; col < setup.Settings.Width; ++col)
            {
                Vi::Vec3 const dir = Vi::PixelDirection(setup.Rig, setup.Settings, row, col);
                Compare(baked, setup.Rig.Camera, setup.Rig.Camera + dir * 500.0f, pixels);
            }
    }

    // Random segments inside the scene: any origin, any direction, up to 60 yards.
    Agreement random;
    uint64_t state = 0x9e3779b97f4a7c15ull;
    auto const next = [&]()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return float((state >> 40) & 0xFFFFFF) / float(0x1000000);
    };
    for (uint32_t i = 0; i < randomRays; ++i)
    {
        Vi::Vec3 from;
        from.X = header.SolidMin[0] + next() * (header.SolidMax[0] - header.SolidMin[0]);
        from.Y = header.SolidMin[1] + next() * (header.SolidMax[1] - header.SolidMin[1]);
        from.Z = header.SolidMin[2] + next() * (header.SolidMax[2] - header.SolidMin[2]);
        float const z = 2.0f * next() - 1.0f;
        float const angle = 6.2831853f * next();
        float const r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        Vi::Vec3 const dir{ r * std::cos(angle), r * std::sin(angle), z };
        Compare(baked, from, from + dir * (1.0f + 59.0f * next()), random);
    }

    auto const print = [](char const* name, Agreement const& a)
    {
        std::printf("%s: %llu rays; solid: %llu hit-agree, %llu none-agree, %llu MISMATCH (normal mismatches %llu, "
            "any-hit mismatches %llu, worst |dt| %.2e yd); rays an exact test lets through a crack, closed by slack "
            "1e-5 %llu, 1e-4 %llu, 1e-3 %llu, 3e-3 %llu, 1e-2 %llu; "
            "liquid: %llu agree, %llu none, %llu MISMATCH\n", name,
            (unsigned long long)a.Rays, (unsigned long long)a.SolidBothHit, (unsigned long long)a.SolidBothNone,
            (unsigned long long)a.SolidMismatch, (unsigned long long)a.NormalMismatch,
            (unsigned long long)a.AnyHitMismatch, a.WorstDistance, (unsigned long long)a.Sweep[0],
            (unsigned long long)a.Sweep[1], (unsigned long long)a.Sweep[2], (unsigned long long)a.Sweep[3],
            (unsigned long long)a.Sweep[4], (unsigned long long)a.LiquidBoth,
            (unsigned long long)a.LiquidNone, (unsigned long long)a.LiquidMismatch);
    };
    print("pixel rays", pixels);
    print("random segments", random);
    // A float tracer and a double brute force part only for a ray that lies within rounding of the slack's boundary
    // (an edge hit or a miss by a hair): allowed up to five rays in ten thousand (rays lying along a mesh edge).
    uint64_t const rays = pixels.Rays + random.Rays;
    uint64_t const mismatches = pixels.SolidMismatch + pixels.LiquidMismatch + random.SolidMismatch
        + random.LiquidMismatch;
    std::printf("%llu of %llu rays differ (limit five in ten thousand), worst |dt| %.2e yd\n",
        (unsigned long long)mismatches, (unsigned long long)rays, std::max(pixels.WorstDistance, random.WorstDistance));
    return double(mismatches) <= 5e-4 * double(rays);
}
