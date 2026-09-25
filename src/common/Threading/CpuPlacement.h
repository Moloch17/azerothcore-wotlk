/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef _CPU_PLACEMENT_H
#define _CPU_PLACEMENT_H

#include "Define.h"
#include <cstddef>
#include <string>
#include <vector>

/// Where the sim host's threads go on a machine whose cores do not share one last-level cache.
///
/// On a multi-die CPU (a two-CCD Ryzen, say) a pool pinned to "the k-th CPU" spreads across dies as soon as it is
/// larger than one of them, and every task that crosses pays for the other die's L3. Order() lists the CPUs this
/// process may run on so that a pool taking them from the front stays together: the die with the largest L3 first
/// (the V-cache die on an X3D part), each of its physical cores before any SMT sibling, then the next die the same
/// way. Without the topology in /sys, it is the allowed CPUs in number order.
namespace Acore::CpuPlacement
{
    /// The allowed CPUs, pool-first as above. Read once, then cached.
    AC_COMMON_API std::vector<int> const& Order();

    /// The allowed CPUs that share no physical core with any of `used`: where a second workload (the learner) can
    /// run beside a pinned pool without taking hardware threads from its cores. Empty if the pool covers every core.
    AC_COMMON_API std::vector<int> AwayFrom(std::vector<int> const& used);

    /// `cpus` dealt into `parts` slices of whole physical cores (a core's SMT siblings stay together), in the order
    /// the cores first appear in `cpus`, so a slice keeps to one die where it can. Fewer cores than parts: every
    /// slice is all of `cpus`.
    AC_COMMON_API std::vector<std::vector<int>> Split(std::vector<int> const& cpus, std::size_t parts);

    /// Pin the calling thread to `cpu`. False, and nothing changed, if the kernel refuses.
    AC_COMMON_API bool PinThisThread(int cpu);

    /// Restrict process `pid` (its threads started from now on inherit it) to `cpus`.
    AC_COMMON_API bool PinProcess(int pid, std::vector<int> const& cpus);

    /// "0-7,16-23" for a list of CPUs, for log lines.
    AC_COMMON_API std::string Describe(std::vector<int> cpus);
}

#endif
