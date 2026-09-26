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

#include "GpuRuntime.h"
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>

namespace
{
    std::once_flag g_once;
    ForgeGpuApi const* g_api = nullptr;
    std::string g_why;

    /// Where torch keeps its shared libraries, asked of the learner's own interpreter.
    std::string TorchLibDir(std::string const& python, std::string const& workDir)
    {
        std::string const command = "cd '" + workDir + "' && '" + python + "' -c 'import os, torch; "
            "print(os.path.join(os.path.dirname(torch.__file__), \"lib\") if torch.version.hip else \"\")' 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return {};
        char line[4096] = {};
        std::string dir = std::fgets(line, sizeof(line), pipe) ? line : "";
        pclose(pipe);
        while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r'))
            dir.pop_back();
        return dir;
    }

    void DoLoad(std::string const& python, std::string const& workDir)
    {
        Animus::Gpu::PrepareEnvironment();

        std::string const torchLib = TorchLibDir(python, workDir);
        if (torchLib.empty())
        {
            g_why = "the learner's torch has no HIP runtime (or no torch at " + python + ")";
            return;
        }

        // The runtime first, by its full path and with its symbols global: the device library needs its symbols
        // and names it only by soname, which this already-loaded object satisfies.
        std::string const runtime = (std::filesystem::path(torchLib) / "libamdhip64.so").string();
        if (!dlopen(runtime.c_str(), RTLD_NOW | RTLD_GLOBAL))
        {
            g_why = "cannot load " + runtime + ": " + dlerror();
            return;
        }

        std::error_code error;
        std::filesystem::path const self = std::filesystem::read_symlink("/proc/self/exe", error);
        std::string const library = (self.parent_path() / "libforge-gpu.so").string();
        void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            g_why = "cannot load " + library + " (built only where hipcc is): " + dlerror();
            return;
        }

        auto const get = reinterpret_cast<ForgeGpuApi const* (*)()>(dlsym(handle, "ForgeGpuGetApi"));
        ForgeGpuApi const* api = get ? get() : nullptr;
        if (!api || api->Version != FORGE_GPU_API_VERSION)
        {
            g_why = library + " is from another build (API version " + std::to_string(api ? api->Version : 0)
                + ", this worldserver wants " + std::to_string(FORGE_GPU_API_VERSION) + ")";
            return;
        }
        g_api = api;
    }
}

namespace Animus::Gpu
{
    void PrepareEnvironment()
    {
        // Not overwritten when already set: whoever set it knows their ROCm.
        setenv("HSA_ENABLE_IPC_MODE_LEGACY", "0", 0);
    }

    bool Load(std::string const& python, std::string const& workDir, std::string& why)
    {
        std::call_once(g_once, [&] { DoLoad(python, workDir); });
        why = g_why;
        return g_api != nullptr;
    }

    ForgeGpuApi const* Api()
    {
        return g_api;
    }
}
