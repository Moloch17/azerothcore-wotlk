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
#include <hip/hip_runtime.h>
#include <cstring>
#include <string>

namespace
{
    hipStream_t g_stream = nullptr;
    int g_device = -1;
    thread_local std::string t_error;

    int Check(hipError_t result, char const* what)
    {
        if (result == hipSuccess)
            return 0;
        t_error = std::string(what) + ": " + hipGetErrorString(result);
        return int(result);
    }

    int Init(int device)
    {
        if (int const failed = Check(hipSetDevice(device), "hipSetDevice"))
            return failed;
        if (g_stream && g_device == device)
            return 0;
        g_device = device;
        // Non-blocking: the library's work must not wait on, or hold up, work the default stream carries.
        return Check(hipStreamCreateWithFlags(&g_stream, hipStreamNonBlocking), "hipStreamCreateWithFlags");
    }

    int Alloc(void** pointer, size_t bytes)
    {
        return Check(hipMalloc(pointer, bytes), "hipMalloc");
    }

    int Free(void* pointer)
    {
        return Check(hipFree(pointer), "hipFree");
    }

    int Export(void* pointer, void* handle)
    {
        hipIpcMemHandle_t exported;
        if (int const failed = Check(hipIpcGetMemHandle(&exported, pointer), "hipIpcGetMemHandle"))
            return failed;
        static_assert(sizeof(exported) == FORGE_GPU_HANDLE_BYTES);
        std::memcpy(handle, &exported, sizeof(exported));
        return 0;
    }

    int CopyToDevice(void* device, void const* host, size_t bytes)
    {
        return Check(hipMemcpyAsync(device, host, bytes, hipMemcpyHostToDevice, g_stream), "hipMemcpyAsync H2D");
    }

    int CopyToHost(void* host, void const* device, size_t bytes)
    {
        return Check(hipMemcpyAsync(host, device, bytes, hipMemcpyDeviceToHost, g_stream), "hipMemcpyAsync D2H");
    }

    int Synchronize()
    {
        return Check(hipStreamSynchronize(g_stream), "hipStreamSynchronize");
    }

    int AllocHost(void** pointer, size_t bytes)
    {
        return Check(hipHostMalloc(pointer, bytes, hipHostMallocDefault), "hipHostMalloc");
    }

    int FreeHost(void* pointer)
    {
        return Check(hipHostFree(pointer), "hipHostFree");
    }

    char const* LastError()
    {
        return t_error.c_str();
    }

    ForgeGpuApi const API = {
        FORGE_GPU_API_VERSION,
        Init,
        Alloc,
        Free,
        Export,
        CopyToDevice,
        CopyToHost,
        Synchronize,
        AllocHost,
        FreeHost,
        LastError,
    };
}

extern "C" __attribute__((visibility("default"))) ForgeGpuApi const* ForgeGpuGetApi()
{
    return &API;
}
