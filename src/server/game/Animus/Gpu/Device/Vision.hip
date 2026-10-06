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

    constexpr unsigned int THREADS = 64;

    __global__ void CastKernel(ForgeVisionLaunch launch)
    {
        FrameRequest const& request = launch.Requests[blockIdx.y];
        uint32_t const pixel = blockIdx.x * blockDim.x + threadIdx.x;
        if (pixel >= request.CastWidth * request.CastHeight)
            return;
        uint32_t const row = pixel / request.CastWidth;
        uint32_t const col = pixel % request.CastWidth;
        uint8_t* target = Scaled(request) ? launch.Scratch + request.ScratchOffset
            : launch.Image + request.ImageOffset;
        CastPixel(request, launch.Scenes[request.Scene], launch.Units, row, col,
            target + uint64_t(pixel) * Animus::Vision::BYTES_PER_PIXEL);
    }

    __global__ void UpscaleKernel(ForgeVisionLaunch launch)
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
    dim3 const cast((launch->MaxCastPixels + THREADS - 1) / THREADS, launch->RequestCount);
    CastKernel<<<cast, block, 0, Stream()>>>(*launch);
    if (int const failed = Check(hipGetLastError(), "CastKernel"))
        return failed;
    dim3 const upscale((launch->MaxPixels + THREADS - 1) / THREADS, launch->RequestCount);
    UpscaleKernel<<<upscale, block, 0, Stream()>>>(*launch);
    return Check(hipGetLastError(), "UpscaleKernel");
}
