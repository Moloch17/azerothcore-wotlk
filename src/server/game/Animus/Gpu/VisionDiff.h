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
#include <span>
#include <string>
#include <vector>

/// **The CPU caster against the GPU's** (camera-vision.GPU.md, G2's differential test; `forge camera diff` and the
/// data harness in the tests): the same frames cast by Vision::Render and by the kernel (on the device, and the
/// same kernel code on the host -- "emulated" -- which tells the device's arithmetic from the port's), compared ray
/// by ray at the size each was cast at.
///
/// A pixel is identical within the tolerances when its class and objective bit are the same, its slot names the same
/// entity in both frames' lists (perception-goals P2: class and identity exactly), its distance and height codes
/// within 1 and its normal within 2. A mismatched pixel is an edge pixel when it lies on a class boundary: for a class
/// mismatch, a neighbour (of the 8) has the other caster's class; for any other, a neighbour has another class in
/// the CPU's frame. The gate (amendment 6): >= 99.9% of the non-edge pixels identical, edge mismatches < ~1% of all,
/// the upscale bit-exact and the scalars exact; and (P2) no class or identity mismatch off an edge, and every
/// frame's entity list -- the GPU's reduction -- equal to the CPU's, entity for entity and count for count.
namespace Animus::GpuVision
{
    /// One frame to compare: the seat, its camera (RenderWidth x RenderHeight drawn), its entities -- units, boxes
    /// and known doors, numbered nearest first -- and its objective.
    struct DiffFrame
    {
        Vision::Pose Pose;
        Vision::CameraState Camera;
        std::vector<Vision::UnitShape> Units;
        std::vector<Vision::BoxShape> Boxes;
        std::vector<Vision::DoorShape> Doors;
        bool HasObjective = false;
        Vision::Vec3 Objective;

        [[nodiscard]] Vision::Sight View() const
        {
            Vision::Sight sight;
            sight.Units = Units;
            sight.Boxes = Boxes;
            sight.Doors = Doors;
            return sight;
        }
        float ObjectiveRadius = Vision::OBJECTIVE_RADIUS;   // the flag's (an object's ObjectiveRadiusFor, or 1 yd)
    };

    /// N frames round (x, y, z): positions jittered within `radius` (each set on the floor there, when there is
    /// one), yaw, camera yaw, pitch and zoom random, the render size cycling through RenderSizes and the canonical
    /// size, up to `units` random units round the feet (and the seat's own, which a ray never sees), up to `boxes`
    /// colliderless game objects (boxes of random size, turned at random; a quarter of them open doors, the band
    /// Vision::OpenDoorBox leaves, of the door class), the `doors` the scene has, all of them
    /// numbered nearest the head first as GatherSight numbers them, and mostly an objective.
    [[nodiscard]] std::vector<DiffFrame> RandomFrames(Vision::VisionWorld const& world,
        Vision::Settings const& settings, float x, float y, float z, uint32_t count, float radius,
        uint32_t units = 6, uint32_t boxes = 4, std::span<Vision::DoorShape const> doors = {});

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
        uint64_t Identity = 0;          // mismatches by cause: the slot names another entity (or none)
        uint64_t OffEdgeIdentity = 0;   // class or identity mismatches off any edge (the P2 gate: none)
        uint32_t SlotTablesExact = 0;   // frames whose entity list equals the CPU's
        uint64_t Listed = 0;            // the CPU's listed entities, every frame
        uint32_t FullLists = 0;         // frames whose CPU list is full (ENTITY_SLOTS: the cap at work)
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

    /// Every collision game object of the renderer's scene `scene` (its door records) as a frame could know them:
    /// each one's GameObjectModel and position, its class cycling through the door and the game object classes, so
    /// a diff checks that both casters name the object behind a dynamic-tree hit alike.
    [[nodiscard]] std::vector<Vision::DoorShape> KnownDoors(Renderer const& renderer, int32_t scene);

    /// Casts `frames` on the CPU (over `world`, as the seat's camera does) and on the renderer's scene `scene`
    /// (already synced from the same map), and compares them. `emulate` adds the host-run kernel's column;
    /// `repeats` casts the launch that many times on the device and keeps the fastest kernel time (a shared GPU
    /// is noisy), comparing the first.
    [[nodiscard]] DiffReport RunDiff(Renderer& renderer, int32_t scene, Vision::VisionWorld const& world,
        uint32_t phaseMask, Vision::Settings const& settings, std::vector<DiffFrame> const& frames, bool emulate,
        uint32_t repeats = 1);

    /// Compares one cast frame (w x h, row 0 on top) of the CPU's with another's, and their entity lists, into
    /// `tally`.
    void CompareFrame(uint8_t const* cpu, uint8_t const* other, uint32_t w, uint32_t h,
        Vision::FrameSlots const& cpuSlots, Vision::FrameSlots const& otherSlots, DiffTally& tally);

    /// The report as lines for a console.
    [[nodiscard]] std::vector<std::string> FormatDiff(DiffReport const& report);
}

#endif
