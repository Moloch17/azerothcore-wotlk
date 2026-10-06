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

#ifndef ANIMUS_GPU_VISION_DIFF_H
#define ANIMUS_GPU_VISION_DIFF_H

#include "VisionCaster.h"
#include "VisionGpu.h"
#include <array>
#include <map>
#include <string>
#include <vector>

/// **The CPU caster against the GPU's** (camera-vision.GPU.md, G2's differential test; `forge camera diff` and the
/// data harness in the tests): the same frames cast by Vision::Render and by the kernel (on the device, and the
/// same kernel code on the host -- "emulated" -- which tells the device's arithmetic from the port's), compared ray
/// by ray at the size each was cast at.
///
/// A pixel is identical within the tolerances when its class and objective bit are the same, its distance and height
/// codes within 1 and its normal within 2. A mismatched pixel is an edge pixel when it lies on a class boundary: for
/// a class mismatch, a neighbour (of the 8) has the other caster's class; for any other, a neighbour has another
/// class in the CPU's frame. The gate (amendment 6): >= 99.9% of the non-edge pixels identical, edge mismatches
/// < ~1% of all, the upscale bit-exact and the scalars exact.
namespace Animus::GpuVision
{
    /// One frame to compare: the seat, its camera (RenderWidth x RenderHeight drawn), its units and objective.
    struct DiffFrame
    {
        Vision::Pose Pose;
        Vision::CameraState Camera;
        std::vector<Vision::UnitShape> Units;
        bool HasObjective = false;
        Vision::Vec3 Objective;
    };

    /// N frames round (x, y, z): positions jittered within `radius` (each set on the floor there, when there is
    /// one), yaw, camera yaw, pitch and zoom random, the render size cycling through RenderSizes and the canonical
    /// size, a few random units round the feet (and the seat's own, which a ray never sees) and mostly an
    /// objective.
    [[nodiscard]] std::vector<DiffFrame> RandomFrames(Vision::VisionWorld const& world,
        Vision::Settings const& settings, float x, float y, float z, uint32_t count, float radius);

    struct DiffTally
    {
        uint64_t Pixels = 0;
        uint64_t Identical = 0;
        uint64_t EdgeMismatches = 0;    // mismatches on a class boundary
        uint64_t Class = 0;             // mismatches by cause (a pixel counts under each it has)
        uint64_t Objective = 0;
        uint64_t Distance = 0;
        uint64_t Height = 0;
        uint64_t Normal = 0;
        uint64_t ExactBytes = 0;        // pixels whose five bytes are the same
        std::array<uint64_t, Vision::CLASSES> CpuClasses{};  // the CPU's pixels by class
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> ClassPairs;  // (CPU class, other class) of class mismatches
        std::map<std::pair<uint32_t, uint32_t>, std::pair<uint64_t, uint64_t>> BySize;  // (w, h): pixels, identical
        uint32_t UpscaleExact = 0;      // frames whose canonical image is exactly Upscale of their cast frame
        /// The first few mismatches, for a look: "frame f (row, col) w x h: CPU b0 .. b4, other b0 .. b4".
        std::vector<std::string> Samples;
        uint32_t Frame = 0;             // the frame CompareFrame is given (RunDiff sets it)
        uint32_t Frames = 0;

        [[nodiscard]] uint64_t NonEdge() const { return Pixels - EdgeMismatches; }
        [[nodiscard]] double NonEdgeShare() const
        {
            return NonEdge() ? double(Identical) / double(NonEdge()) : 1.0;
        }
    };

    struct DiffReport
    {
        uint32_t Frames = 0;
        uint64_t Rays = 0;              // cast pixels, every frame
        uint32_t ScalarsExact = 0;
        double CpuMs = 0.0;             // Vision::Render, all frames
        bool Device = false;
        std::string DeviceError;
        CastTiming Gpu;
        DiffTally GpuTally;
        bool Emulated = false;
        double EmulatedMs = 0.0;
        uint32_t EmulatedOverflows = 0;
        DiffTally EmulatedTally;
    };

    /// Casts `frames` on the CPU (over `world`, as the seat's camera does) and on the renderer's scene `scene`
    /// (already synced from the same map), and compares them. `emulate` adds the host-run kernel's column;
    /// `repeats` casts the launch that many times on the device and keeps the fastest kernel time (a shared GPU
    /// is noisy), comparing the first.
    [[nodiscard]] DiffReport RunDiff(Renderer& renderer, int32_t scene, Vision::VisionWorld const& world,
        uint32_t phaseMask, Vision::Settings const& settings, std::vector<DiffFrame> const& frames, bool emulate,
        uint32_t repeats = 1);

    /// Compares one cast frame (w x h, row 0 on top) of the CPU's with another's, into `tally`.
    void CompareFrame(uint8_t const* cpu, uint8_t const* other, uint32_t w, uint32_t h, DiffTally& tally);

    /// The report as lines for a console.
    [[nodiscard]] std::vector<std::string> FormatDiff(DiffReport const& report);
}

#endif
