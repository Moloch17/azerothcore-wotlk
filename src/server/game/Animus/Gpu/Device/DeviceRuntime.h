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

#ifndef ANIMUS_GPU_DEVICE_RUNTIME_H
#define ANIMUS_GPU_DEVICE_RUNTIME_H

/// The device library's own runtime, inside libforge-gpu.so only: HIP on AMD, and -- for the CUDA build that comes
/// with camera-vision.GPU.md's G0 -- CUDA under the same names, so the kernels and Runtime.hip are one source. Only
/// what the library uses is mapped; a HIP-only call added later must be mapped here too (or kept behind
/// FORGE_GPU_CUDA), which is what keeps nvcc able to build it.
#if defined(FORGE_GPU_CUDA)
#include <cuda_runtime.h>
using hipError_t = cudaError_t;
using hipStream_t = cudaStream_t;
using hipIpcMemHandle_t = cudaIpcMemHandle_t;
#define hipSuccess cudaSuccess
#define hipGetErrorString cudaGetErrorString
#define hipGetLastError cudaGetLastError
#define hipSetDevice cudaSetDevice
#define hipStreamCreateWithFlags cudaStreamCreateWithFlags
#define hipStreamNonBlocking cudaStreamNonBlocking
#define hipMalloc cudaMalloc
#define hipFree cudaFree
#define hipIpcGetMemHandle cudaIpcGetMemHandle
#define hipMemcpyAsync cudaMemcpyAsync
#define hipMemcpyHostToDevice cudaMemcpyHostToDevice
#define hipMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define hipStreamSynchronize cudaStreamSynchronize
#define hipHostMalloc(pointer, bytes, flags) cudaMallocHost(pointer, bytes)
#define hipHostMallocDefault 0
#define hipHostFree cudaFreeHost
#else
#include <hip/hip_runtime.h>
#endif

struct ForgeVisionLaunch;

/// Shared between the library's sources (hidden: only ForgeGpuGetApi is exported).
namespace ForgeGpuDevice
{
    /// The library's stream (Init creates it): every copy and kernel goes on it, in order.
    hipStream_t Stream();
    /// 0 for success, else the error's code, with `what` and its text kept for LastError.
    int Check(hipError_t result, char const* what);
    /// Vision.hip: DeviceApi's CastVision.
    int CastVision(ForgeVisionLaunch const* launch);
}

#endif
