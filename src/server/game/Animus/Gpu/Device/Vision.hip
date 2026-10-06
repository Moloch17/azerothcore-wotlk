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

#include "../DeviceApi.h"
#include "DeviceRuntime.h"
#include "../VisionDevice.h"

/// The camera's kernels (camera-vision.GPU.md, G2; perception-goals P2): one thread a cast pixel, every frame of the
/// launch in one grid (the frame on y, its pixels on x); then the entity slots -- each cast pixel counted under its
/// entity number, each frame's slots assigned by one thread -- then one thread a canonical pixel for the frames cast
/// smaller (byte 4 made the slot on the way), and the cast frames' own byte 4 made the slot. Every step is
/// VisionDevice.h's, shared with the host.
namespace
{
    using namespace Animus::GpuVision;
    namespace Vi = Animus::Vision;

    /// Measured on gfx1100 (camera-vision.GPU.md, G2.5): the launch bound is what matters -- without it the
    /// compiler sizes the kernel for blocks of 1024 and it ran six to eight times slower -- then 8 x 8 tiles over
    /// row strips (a fifth or so). 32 or 128 threads, and forcing 12 or 16 waves a SIMD (fewer registers, more
    /// spills), measured the same within the noise of a GPU shared with the learner. HIP 5.7 has no wave64 on
    /// gfx1100.
    constexpr unsigned int THREADS = 64;
    /// A block casts a TILE_W x TILE_H tile of its frame (neighbouring rays meet the same nodes and triangles, so
    /// a wave's lanes walk the trees together), the tiles in row order; a tile past the frame's edge idles there.
    constexpr uint32_t TILE_W = 8;
    constexpr uint32_t TILE_H = THREADS / TILE_W;

    __device__ uint8_t* CastFrame(ForgeVisionLaunch const& launch, FrameRequest const& request)
    {
        return Scaled(request) ? launch.Scratch + request.ScratchOffset : launch.Image + request.ImageOffset;
    }

    template <int Stack>
    __global__ void __launch_bounds__(THREADS) CastKernel(ForgeVisionLaunch launch)
    {
        FrameRequest const& request = launch.Requests[blockIdx.y];
        uint32_t const tilesX = (request.CastWidth + TILE_W - 1) / TILE_W;
        uint32_t const tilesY = (request.CastHeight + TILE_H - 1) / TILE_H;
        if (blockIdx.x >= tilesX * tilesY)
            return;
        uint32_t const row = (blockIdx.x / tilesX) * TILE_H + threadIdx.x / TILE_W;
        uint32_t const col = (blockIdx.x % tilesX) * TILE_W + threadIdx.x % TILE_W;
        if (row >= request.CastHeight || col >= request.CastWidth)
            return;
        uint32_t const pixel = row * request.CastWidth + col;
        FrameSight const sight = SightOf(request, launch.Units, launch.Boxes, launch.Doors);
        if (CastPixel<Stack>(request, launch.Scenes[request.Scene], sight, row, col,
            CastFrame(launch, request) + uint64_t(pixel) * Vi::BYTES_PER_PIXEL))
            atomicAdd(launch.Overflows, 1u);
    }

    /// Every frame's counts zeroed: COUNTS_PER_FRAME threads a frame.
    __global__ void __launch_bounds__(THREADS) ClearKernel(ForgeVisionLaunch launch)
    {
        uint32_t const number = blockIdx.x * blockDim.x + threadIdx.x;
        if (number >= COUNTS_PER_FRAME)
            return;
        launch.Counts[uint64_t(blockIdx.y) * COUNTS_PER_FRAME + number] = Vi::SlotStat();
    }

    /// The reduction (perception-goals amendment 11): each cast pixel adds itself to its entity number's count.
    __global__ void __launch_bounds__(THREADS) CountKernel(ForgeVisionLaunch launch)
    {
        FrameRequest const& request = launch.Requests[blockIdx.y];
        uint32_t const pixel = blockIdx.x * blockDim.x + threadIdx.x;
        if (pixel >= request.CastWidth * request.CastHeight)
            return;
        uint32_t const row = pixel / request.CastWidth;
        uint32_t const col = pixel % request.CastWidth;
        uint8_t const number = NumberAt(request, CastFrame(launch, request), row, col);
        if (!number)
            return;
        Vi::SlotStat& count = launch.Counts[uint64_t(request.Frame) * COUNTS_PER_FRAME + number];
        atomicAdd(&count.Pixels, 1u);
        atomicAdd(&count.SumRow, row);
        atomicAdd(&count.SumCol, col);
    }

    /// One thread a frame: its slots, nearest first.
    __global__ void __launch_bounds__(THREADS) SlotKernel(ForgeVisionLaunch launch)
    {
        uint32_t const frame = blockIdx.x * blockDim.x + threadIdx.x;
        if (frame >= launch.RequestCount)
            return;
        AssignFrameSlots(launch.Counts + uint64_t(frame) * COUNTS_PER_FRAME, launch.Slots + uint64_t(frame)
            * Vi::ENTITY_SLOTS, launch.SlotOf + uint64_t(frame) * COUNTS_PER_FRAME);
    }

    __global__ void __launch_bounds__(THREADS) UpscaleKernel(ForgeVisionLaunch launch)
    {
        FrameRequest const& request = launch.Requests[blockIdx.y];
        if (!Scaled(request))
            return;
        uint32_t const pixel = blockIdx.x * blockDim.x + threadIdx.x;
        if (pixel >= request.Width * request.Height)
            return;
        UpscalePixel(launch.Scratch + request.ScratchOffset, request.CastWidth, request.CastHeight,
            launch.Image + request.ImageOffset, request.Width, request.Height, pixel / request.Width,
            pixel % request.Width, launch.SlotOf + uint64_t(request.Frame) * COUNTS_PER_FRAME);
    }

    /// The cast frames' own byte 4 made the slot (after the upscale has read the numbers).
    __global__ void __launch_bounds__(THREADS) RemapKernel(ForgeVisionLaunch launch)
    {
        FrameRequest const& request = launch.Requests[blockIdx.y];
        uint32_t const pixel = blockIdx.x * blockDim.x + threadIdx.x;
        if (pixel >= request.CastWidth * request.CastHeight)
            return;
        uint8_t& slot = CastFrame(launch, request)[uint64_t(pixel) * Vi::BYTES_PER_PIXEL + Vi::SLOT_BYTE];
        slot = launch.SlotOf[uint64_t(request.Frame) * COUNTS_PER_FRAME + slot];
    }
}

int ForgeGpuDevice::CastVision(ForgeVisionLaunch const* launch)
{
    if (!launch->RequestCount)
        return 0;
    dim3 const block(THREADS);
    // Enough tiles for the largest frame; MaxCastPixels' frame is no wider than the canonical image, so this
    // covers a cast frame of any shape up to it.
    uint32_t const tiles = ((launch->MaxCastWidth + TILE_W - 1) / TILE_W) * ((launch->MaxCastHeight + TILE_H - 1)
        / TILE_H);
    dim3 const cast(tiles, launch->RequestCount);
    static_assert(STACK_COUNT == 4, "one kernel a stack size");
    switch (StackIndexFor(launch->StackDepth))
    {
        case 0:
            CastKernel<STACK_SIZES[0]><<<cast, block, 0, Stream()>>>(*launch);
            break;
        case 1:
            CastKernel<STACK_SIZES[1]><<<cast, block, 0, Stream()>>>(*launch);
            break;
        case 2:
            CastKernel<STACK_SIZES[2]><<<cast, block, 0, Stream()>>>(*launch);
            break;
        default:
            CastKernel<STACK_SIZES[3]><<<cast, block, 0, Stream()>>>(*launch);
            break;
    }
    if (int const failed = Check(hipGetLastError(), "CastKernel"))
        return failed;

    dim3 const clear((COUNTS_PER_FRAME + THREADS - 1) / THREADS, launch->RequestCount);
    ClearKernel<<<clear, block, 0, Stream()>>>(*launch);
    if (int const failed = Check(hipGetLastError(), "ClearKernel"))
        return failed;
    dim3 const perCast((launch->MaxCastPixels + THREADS - 1) / THREADS, launch->RequestCount);
    CountKernel<<<perCast, block, 0, Stream()>>>(*launch);
    if (int const failed = Check(hipGetLastError(), "CountKernel"))
        return failed;
    SlotKernel<<<dim3((launch->RequestCount + THREADS - 1) / THREADS), block, 0, Stream()>>>(*launch);
    if (int const failed = Check(hipGetLastError(), "SlotKernel"))
        return failed;

    dim3 const upscale((launch->MaxPixels + THREADS - 1) / THREADS, launch->RequestCount);
    UpscaleKernel<<<upscale, block, 0, Stream()>>>(*launch);
    if (int const failed = Check(hipGetLastError(), "UpscaleKernel"))
        return failed;
    RemapKernel<<<perCast, block, 0, Stream()>>>(*launch);
    return Check(hipGetLastError(), "RemapKernel");
}
