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

#ifndef ANIMUS_GPU_RUNTIME_H
#define ANIMUS_GPU_RUNTIME_H

#include "DeviceApi.h"
#include <string>

/// The worldserver's hold on its device code (DeviceApi.h), loaded at run time and only when asked for.
namespace Animus::Gpu
{
    /// Load the HIP runtime of the learner's torch -- `<python> -c 'import torch'` in `workDir` says where it is --
    /// and then libforge-gpu.so from beside the worldserver binary. Once: later calls return the first answer.
    /// False, with the reason in `why`, when there is no torch with a HIP runtime or no device library (a build
    /// without hipcc); the forge then does everything on the CPU as before.
    bool Load(std::string const& python, std::string const& workDir, std::string& why);

    /// The device calls, or nullptr before a successful Load.
    ForgeGpuApi const* Api();

    /// Set in the worldserver's environment before anything HIP starts, and inherited by the learner it spawns:
    /// the device-memory sharing the two processes need works only in ROCm's non-legacy IPC mode.
    void PrepareEnvironment();
}

#endif
