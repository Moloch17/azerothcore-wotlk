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

/// The camera's kernels (camera-vision.GPU.md, G2): one thread a cast pixel, every frame of the launch in one grid
/// (the frame on y, its pixels on x), then one thread a canonical pixel for the frames cast smaller. Every step is
/// VisionDevice.h's, shared with the host.
namespace
{
    using namespace Animus::GpuVision;

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
        uint8_t* target = Scaled(request) ? launch.Scratch + request.ScratchOffset
            : launch.Image + request.ImageOffset;
        CastPixel(request, launch.Scenes[request.Scene], launch.Units, row, col,
            target + uint64_t(pixel) * Animus::Vision::BYTES_PER_PIXEL);
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
            pixel % request.Width);
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
    CastKernel<<<cast, block, 0, Stream()>>>(*launch);
    if (int const failed = Check(hipGetLastError(), "CastKernel"))
        return failed;
    dim3 const upscale((launch->MaxPixels + THREADS - 1) / THREADS, launch->RequestCount);
    UpscaleKernel<<<upscale, block, 0, Stream()>>>(*launch);
    return Check(hipGetLastError(), "UpscaleKernel");
}
