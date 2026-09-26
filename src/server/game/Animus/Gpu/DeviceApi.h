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

#ifndef ANIMUS_GPU_DEVICE_API_H
#define ANIMUS_GPU_DEVICE_API_H

#include <cstddef>
#include <cstdint>

/// The one boundary between the worldserver and its device code.
///
/// The device code is libforge-gpu.so (Gpu/Device/*.hip), built with hipcc and linked against no HIP runtime: its
/// HIP symbols are resolved when the worldserver loads it, from the runtime the learner's torch ships, which the
/// worldserver loads first (Gpu::Runtime). That is what lets the two processes share device memory -- a buffer
/// exported by one HIP runtime can only be opened by the same one (measured: HIP 5.7 to torch's ROCm 6.4 fails,
/// 6.4 to 6.4 works with HSA_ENABLE_IPC_MODE_LEGACY=0) -- and what keeps the worldserver itself free of HIP: a
/// server without a GPU, or an end user's CPU-only bot, never loads either library.
///
/// So this header is plain C: no HIP types cross it. Every call returns 0 on success; LastError says why not.
/// Compiled with HIP 5.7's headers against a 6.4 runtime, the device side keeps to the calls whose ABI did not
/// change between them (allocation, copies, streams, IPC, kernel launches) -- never hipGetDeviceProperties.
extern "C"
{
    /// Bump when a member is added or changed: the loader refuses a library built against another.
    constexpr uint32_t FORGE_GPU_API_VERSION = 1;
    /// hipIpcMemHandle_t's size: the handle a learner opens a buffer by.
    constexpr size_t FORGE_GPU_HANDLE_BYTES = 64;

    struct ForgeGpuApi
    {
        uint32_t Version;

        /// Make `device` current for the calling thread, and create the library's stream on it the first time.
        int (*Init)(int device);
        int (*Alloc)(void** pointer, size_t bytes);
        int (*Free)(void* pointer);
        /// The IPC handle of a buffer Alloc returned, FORGE_GPU_HANDLE_BYTES written to `handle`.
        int (*Export)(void* pointer, void* handle);
        /// Copies on the library's stream, in order with its kernels; Synchronize waits for all of them.
        int (*CopyToDevice)(void* device, void const* host, size_t bytes);
        int (*CopyToHost)(void* host, void const* device, size_t bytes);
        int (*Synchronize)();
        /// Page-locked host memory, for copies that do not stage through a bounce buffer.
        int (*AllocHost)(void** pointer, size_t bytes);
        int (*FreeHost)(void* pointer);
        char const* (*LastError)();
    };

    /// The library's one exported symbol.
    ForgeGpuApi const* ForgeGpuGetApi();
}

#endif
