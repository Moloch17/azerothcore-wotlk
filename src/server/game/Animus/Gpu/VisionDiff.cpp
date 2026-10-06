/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
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

#include "VisionDiff.h"
#include "FrameImage.h"
#include "Random.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace
{
    using namespace Animus::GpuVision;
    namespace Vi = Animus::Vision;
    using Clock = std::chrono::steady_clock;

    uint32_t KindOf(uint8_t const* pixel)
    {
        return pixel[3] & Vi::KIND_MASK;
    }

    /// The cast frame (w x h) a canonical image was scaled up from: cast pixel (sr, sc) is at the first canonical
    /// row and column Upscale fills from it (every one is used, as w <= W and h <= H).
    void Downscale(uint8_t const* image, uint32_t W, uint32_t H, uint8_t* cast, uint32_t w, uint32_t h)
    {
        for (uint32_t sr = 0; sr < h; ++sr)
        {
            uint32_t const r = uint32_t((uint64_t(sr) * H + h - 1) / h);
            for (uint32_t sc = 0; sc < w; ++sc)
            {
                uint32_t const c = uint32_t((uint64_t(sc) * W + w - 1) / w);
                std::memcpy(cast + (std::size_t(sr) * w + sc) * Vi::BYTES_PER_PIXEL,
                    image + (std::size_t(r) * W + c) * Vi::BYTES_PER_PIXEL, Vi::BYTES_PER_PIXEL);
            }
        }
    }

    std::string Percent(uint64_t part, uint64_t whole)
    {
        return Acore::StringFormat("{:.3f}%", whole ? 100.0 * double(part) / double(whole) : 100.0);
    }

    void FormatTally(std::vector<std::string>& lines, char const* name, Animus::GpuVision::DiffTally const& tally)
    {
        lines.push_back(Acore::StringFormat("  {}: {} of {} pixels identical within the tolerances ({}), {} "
            "byte-exact; non-edge {} identical (gate 99.9%); edge mismatches {} of all (gate ~1%)", name,
            tally.Identical, tally.Pixels, Percent(tally.Identical, tally.Pixels),
            Percent(tally.ExactBytes, tally.Pixels), Percent(tally.Identical, tally.NonEdge()),
            Percent(tally.EdgeMismatches, tally.Pixels)));
        lines.push_back(Acore::StringFormat("    mismatches by cause: kind {}, objective {}, distance {}, height {}, "
            "normal {}", tally.Kind, tally.Objective, tally.Distance, tally.Height, tally.Normal));
        if (!tally.KindPairs.empty())
        {
            std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> pairs;
            for (auto const& [pair, count] : tally.KindPairs)
                pairs.push_back({ count, pair });
            std::sort(pairs.rbegin(), pairs.rend());
            std::string text;
            for (std::size_t i = 0; i < pairs.size() && i < 8; ++i)
                text += Acore::StringFormat("{}{}->{} {}", text.empty() ? "" : ", ",
                    Vi::KIND_NAMES[std::min<uint32_t>(pairs[i].second.first, Vi::KINDS - 1)],
                    Vi::KIND_NAMES[std::min<uint32_t>(pairs[i].second.second, Vi::KINDS - 1)], pairs[i].first);
            lines.push_back("    kind mismatches (CPU->other): " + text);
        }
        std::string sizes;
        for (auto const& [size, counts] : tally.BySize)
            sizes += Acore::StringFormat("{}{}x{} {}", sizes.empty() ? "" : ", ", size.first, size.second,
                Percent(counts.second, counts.first));
        lines.push_back(Acore::StringFormat("    by render size: {}; upscale bit-exact in {} of {} frames", sizes,
            tally.UpscaleExact, tally.Frames));
        std::string kinds;
        for (uint32_t kind = 0; kind < Vi::KINDS; ++kind)
            if (tally.CpuKinds[kind])
                kinds += Acore::StringFormat("{}{} {}", kinds.empty() ? "" : ", ", Vi::KIND_NAMES[kind],
                    Percent(tally.CpuKinds[kind], tally.Pixels));
        lines.push_back("    the CPU's pixels by kind: " + kinds);
        for (std::string const& sample : tally.Samples)
            lines.push_back("    mismatch: " + sample);
    }
}

std::vector<Animus::GpuVision::DiffFrame> Animus::GpuVision::RandomFrames(Vision::VisionWorld const& world,
    Vision::Settings const& settings, float x, float y, float z, uint32_t count, float radius)
{
    // Every size a seat can draw, and the canonical one (a single-size conf, or the cast that is not scaled).
    std::vector<Vision::Resolution> sizes = settings.RenderSizes;
    Vision::Resolution const canonical{ settings.Width, settings.Height };
    if (std::find(sizes.begin(), sizes.end(), canonical) == sizes.end())
        sizes.push_back(canonical);

    std::vector<DiffFrame> frames(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        DiffFrame& frame = frames[i];
        float const angle = frand(0.0f, 2.0f * Vi::PI);
        float const distance = radius * std::sqrt(frand(0.0f, 1.0f));
        frame.Pose.X = x + distance * std::cos(angle);
        frame.Pose.Y = y + distance * std::sin(angle);
        float const floor = world.FloorBelow(frame.Pose.X, frame.Pose.Y, z + 10.0f, 40.0f);
        frame.Pose.Z = floor > Animus::Movement::INVALID_FLOOR + 1.0f ? floor : z;
        frame.Pose.Yaw = frand(0.0f, 2.0f * Vi::PI);
        frame.Pose.BodyHeight = Animus::Movement::Body().Height;
        frame.Camera.YawOffset = frand(-Vi::PI, Vi::PI);
        frame.Camera.Pitch = frand(-80.0f, 80.0f) * Vi::DEGREES;
        frame.Camera.Zoom = frand(0.0f, 15.0f);
        Vision::Resolution const size = sizes[i % sizes.size()];
        frame.Camera.RenderWidth = size.Width;
        frame.Camera.RenderHeight = size.Height;

        // The seat's own cylinder, which no ray sees, and a few others round it.
        Vision::UnitShape self;
        self.X = frame.Pose.X;
        self.Y = frame.Pose.Y;
        self.Z = frame.Pose.Z;
        self.Radius = 0.5f;
        self.Height = frame.Pose.BodyHeight;
        self.Self = true;
        frame.Units.push_back(self);
        uint32_t const others = urand(0, 6);
        for (uint32_t u = 0; u < others; ++u)
        {
            Vision::UnitShape unit;
            float const at = frand(0.0f, 2.0f * Vi::PI);
            float const away = frand(1.5f, 15.0f);
            unit.X = frame.Pose.X + away * std::cos(at);
            unit.Y = frame.Pose.Y + away * std::sin(at);
            unit.Z = frame.Pose.Z + frand(-1.0f, 1.0f);
            unit.Radius = frand(0.3f, 1.5f);
            unit.Height = frand(1.0f, 3.0f);
            unit.Hostile = urand(0, 1) == 1;
            frame.Units.push_back(unit);
        }
        frame.HasObjective = urand(0, 9) < 7;
        if (frame.HasObjective)
        {
            float const at = frand(0.0f, 2.0f * Vi::PI);
            float const away = frand(2.0f, 25.0f);
            frame.Objective = { frame.Pose.X + away * std::cos(at), frame.Pose.Y + away * std::sin(at),
                frame.Pose.Z + frand(-2.0f, 4.0f) };
            // Half the frames an object's own radius (the seek stage's: 0.75 to 1 yd), half the default yard.
            if (urand(0, 1))
                frame.ObjectiveRadius = Vi::ObjectiveRadiusFor(frand(0.3f, 0.9f));
        }
    }
    return frames;
}

void Animus::GpuVision::CompareFrame(uint8_t const* cpu, uint8_t const* other, uint32_t w, uint32_t h,
    DiffTally& tally)
{
    auto& bySize = tally.BySize[{ w, h }];
    for (uint32_t row = 0; row < h; ++row)
        for (uint32_t col = 0; col < w; ++col)
        {
            std::size_t const at = (std::size_t(row) * w + col) * Vi::BYTES_PER_PIXEL;
            uint8_t const* a = cpu + at;
            uint8_t const* b = other + at;
            ++tally.Pixels;
            ++bySize.first;
            if (!std::memcmp(a, b, Vi::BYTES_PER_PIXEL))
                ++tally.ExactBytes;
            uint32_t const kindA = KindOf(a);
            uint32_t const kindB = KindOf(b);
            ++tally.CpuKinds[std::min<uint32_t>(kindA, Vi::KINDS - 1)];
            bool const kind = kindA != kindB;
            bool const objective = (a[3] & Vi::OBJECTIVE_BIT) != (b[3] & Vi::OBJECTIVE_BIT);
            bool const distance = std::abs(int32_t(a[0]) - int32_t(b[0])) > 1;
            bool const height = std::abs(int32_t(a[1]) - int32_t(b[1])) > 1;
            bool const normal = std::abs(int32_t(a[2]) - int32_t(b[2])) > 2;
            if (!kind && !objective && !distance && !height && !normal)
            {
                ++tally.Identical;
                ++bySize.second;
                continue;
            }
            if (tally.Samples.size() < 6)
                tally.Samples.push_back(Acore::StringFormat("frame {} ({}, {}) {}x{}: CPU {} {} {} {:#x}, other {} {} "
                    "{} {:#x}", tally.Frame, row, col, w, h, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]));
            tally.Kind += kind;
            tally.Objective += objective;
            tally.Distance += distance;
            tally.Height += height;
            tally.Normal += normal;
            if (kind)
                ++tally.KindPairs[{ kindA, kindB }];

            bool edge = false;
            for (int32_t dr = -1; dr <= 1 && !edge; ++dr)
                for (int32_t dc = -1; dc <= 1 && !edge; ++dc)
                {
                    int32_t const r = int32_t(row) + dr;
                    int32_t const c = int32_t(col) + dc;
                    if ((!dr && !dc) || r < 0 || c < 0 || r >= int32_t(h) || c >= int32_t(w))
                        continue;
                    std::size_t const near = (std::size_t(r) * w + std::size_t(c)) * Vi::BYTES_PER_PIXEL;
                    uint32_t const nearA = KindOf(cpu + near);
                    uint32_t const nearB = KindOf(other + near);
                    edge = kind ? (nearA == kindB || nearB == kindA) : nearA != kindA;
                }
            tally.EdgeMismatches += edge;
        }
}

Animus::GpuVision::DiffReport Animus::GpuVision::RunDiff(Renderer& renderer, int32_t scene,
    Vision::VisionWorld const& world, uint32_t phaseMask, Vision::Settings const& settings,
    std::vector<DiffFrame> const& frames, bool emulate, uint32_t repeats)
{
    DiffReport report;
    report.Frames = uint32_t(frames.size());
    uint32_t const bytes = Vision::ImageBytes(settings);
    std::vector<uint8_t> cpuImages(std::size_t(bytes) * frames.size());
    std::vector<FrameRequest> requests;
    std::vector<DeviceUnit> units;

    // 1. The CPU: each frame exactly as a seat renders it; and the request the GPU path makes of the same seat
    // (the rig and the scalars stay on the CPU: the boom is one ray, the floor and liquid queries are its own).
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        DiffFrame const& frame = frames[i];
        Vision::Vec3 const* objective = frame.HasObjective ? &frame.Objective : nullptr;
        std::array<float, Vision::SCALARS> cpuScalars{};
        Clock::time_point const start = Clock::now();
        Vision::Render(settings, frame.Pose, frame.Camera, world, frame.Units, objective,
            cpuImages.data() + std::size_t(bytes) * i, cpuScalars.data(), nullptr, frame.ObjectiveRadius);
        report.CpuMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();

        std::array<float, Vision::SCALARS> gpuScalars{};
        Vision::Render(settings, frame.Pose, frame.Camera, world, frame.Units, objective, nullptr,
            gpuScalars.data());
        report.ScalarsExact += !std::memcmp(cpuScalars.data(), gpuScalars.data(), sizeof(float) * Vision::SCALARS);

        Vision::Rig const rig = Vision::PlaceCamera(frame.Pose, frame.Camera, world);
        FrameRequest const request = MakeRequest(settings, frame.Pose, frame.Camera, rig, frame.Units, objective,
            uint32_t(scene), phaseMask, units, frame.ObjectiveRadius);
        report.Rays += uint64_t(request.CastWidth) * request.CastHeight;
        requests.push_back(request);
    }
    std::size_t imageBytes = 0;
    std::size_t castBytes = 0;
    LayOut(requests, imageBytes, castBytes);

    // The CPU's cast frames, out of its canonical images.
    std::vector<uint8_t> cpuCasts(castBytes);
    for (std::size_t i = 0; i < requests.size(); ++i)
        Downscale(cpuImages.data() + std::size_t(bytes) * i, settings.Width, settings.Height,
            cpuCasts.data() + requests[i].ScratchOffset, requests[i].CastWidth, requests[i].CastHeight);

    auto const compare = [&](std::vector<uint8_t> const& images, std::vector<uint8_t> const& casts,
        DiffTally& tally)
    {
        std::vector<uint8_t> upscaled(bytes);
        for (FrameRequest const& request : requests)
        {
            tally.Frame = tally.Frames;
            ++tally.Frames;
            CompareFrame(cpuCasts.data() + request.ScratchOffset, casts.data() + request.ScratchOffset,
                request.CastWidth, request.CastHeight, tally);
            Vision::Upscale(casts.data() + request.ScratchOffset, request.CastWidth, request.CastHeight,
                upscaled.data(), request.Width, request.Height);
            tally.UpscaleExact += !std::memcmp(upscaled.data(), images.data() + request.ImageOffset, bytes);
        }
    };

    // 2. The device: every frame in one launch.
    if (renderer.OnDevice())
    {
        std::vector<uint8_t> images;
        std::vector<uint8_t> casts;
        report.Device = renderer.Cast(requests, units, images, casts, report.Gpu, report.DeviceError);
        if (report.Device)
            compare(images, casts, report.GpuTally);
        for (uint32_t i = 1; report.Device && i < repeats; ++i)
        {
            CastTiming again;
            if (!renderer.Cast(requests, units, images, casts, again, report.DeviceError))
                break;
            report.Gpu.KernelMs = std::min(report.Gpu.KernelMs, again.KernelMs);
            report.Gpu.Overflows = std::max(report.Gpu.Overflows, again.Overflows);
        }
    }
    else
        report.DeviceError = "no device library loaded";

    // 3. The same kernel code on the host.
    if (emulate)
    {
        std::vector<uint8_t> images;
        std::vector<uint8_t> casts;
        Clock::time_point const start = Clock::now();
        report.EmulatedOverflows = renderer.Emulate(requests, units, images, casts);
        report.EmulatedMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        report.Emulated = true;
        compare(images, casts, report.EmulatedTally);
    }
    return report;
}

std::vector<std::string> Animus::GpuVision::FormatDiff(DiffReport const& report)
{
    std::vector<std::string> lines;
    double const frames = std::max<double>(1.0, report.Frames);
    auto const raysPerSecond = [&](double ms) { return ms > 0.0 ? double(report.Rays) / (ms / 1000.0) : 0.0; };
    lines.push_back(Acore::StringFormat("camera diff: {} frames, {} rays; scalars exact in {} of {}", report.Frames,
        report.Rays, report.ScalarsExact, report.Frames));
    lines.push_back(Acore::StringFormat("  CPU (Vision::Render, one thread): {:.3f} ms a frame, {:.2f} M rays/s",
        report.CpuMs / frames, raysPerSecond(report.CpuMs) / 1e6));
    if (report.Device)
    {
        lines.push_back(Acore::StringFormat("  GPU: {:.3f} ms a frame in the kernels ({:.2f} M rays/s, {:.2f} ms for "
            "the {} frames in one launch); upload {:.2f} ms, download {:.2f} ms", report.Gpu.KernelMs / frames,
            raysPerSecond(report.Gpu.KernelMs) / 1e6, report.Gpu.KernelMs, report.Frames, report.Gpu.UploadMs,
            report.Gpu.DownloadMs));
        lines.push_back(Acore::StringFormat("  GPU stack: {} entries (the scenes' worst case {}); {} pixels "
            "overflowed it{}", report.Gpu.StackSize, report.Gpu.StackDepth, report.Gpu.Overflows,
            report.Gpu.Overflows ? ": GATE FAILED" : ""));
        FormatTally(lines, "GPU vs CPU", report.GpuTally);
    }
    else
        lines.push_back("  GPU: not run: " + report.DeviceError);
    if (report.Emulated)
    {
        lines.push_back(Acore::StringFormat("  emulated (the kernel's code on the host, one thread): {:.3f} ms a frame",
            report.EmulatedMs / frames));
        lines.push_back(Acore::StringFormat("  emulated stack: {} pixels overflowed it{}", report.EmulatedOverflows,
            report.EmulatedOverflows ? ": GATE FAILED" : ""));
        FormatTally(lines, "emulated vs CPU", report.EmulatedTally);
    }
    return lines;
}
