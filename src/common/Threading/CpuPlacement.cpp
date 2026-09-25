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

#include "CpuPlacement.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <tuple>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace
{
    /// The first line of a /sys file, or empty.
    std::string ReadLine(std::string const& path)
    {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        return line;
    }

    /// "32768K" -> 32768, "96M" -> 98304: the L3 sizes in /sys are in K, sometimes in M.
    uint64 KiB(std::string const& text)
    {
        if (text.empty())
            return 0;

        uint64 value = std::strtoull(text.c_str(), nullptr, 10);
        if (text.back() == 'M')
            value *= 1024;
        return value;
    }

    /// The first CPU of a /sys CPU list ("8-15,24-31" -> 8): the name of the group it describes.
    int FirstOfList(std::string const& list)
    {
        return list.empty() ? -1 : std::atoi(list.c_str());
    }

    struct Topology
    {
        std::vector<int> Order;
        std::vector<std::pair<int, int>> CoreOf;     // (cpu, the first CPU of its physical core)
    };

    Topology Compute()
    {
        std::vector<int> allowed;
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0)
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                if (CPU_ISSET(cpu, &set))
                    allowed.push_back(cpu);
#endif
        if (allowed.empty())
            return {};

        struct Cpu
        {
            int Id;
            uint64 CacheKiB;    // the size of its L3, largest first
            int Cache;          // which L3: the first CPU sharing it
            int Sibling;        // 0 for a core's first hardware thread, 1 for the next
            int Core;
        };

        std::vector<Cpu> cpus;
        for (int id : allowed)
        {
            std::string const base = Acore::StringFormat("/sys/devices/system/cpu/cpu{}/", id);
            std::string const siblings = ReadLine(base + "topology/thread_siblings_list");

            Cpu cpu;
            cpu.Id = id;
            cpu.CacheKiB = KiB(ReadLine(base + "cache/index3/size"));
            cpu.Cache = FirstOfList(ReadLine(base + "cache/index3/shared_cpu_list"));
            cpu.Core = FirstOfList(siblings);
            cpu.Sibling = cpu.Core >= 0 && cpu.Core != id ? 1 : 0;
            cpus.push_back(cpu);
        }

        std::stable_sort(cpus.begin(), cpus.end(), [](Cpu const& a, Cpu const& b)
        {
            return std::make_tuple(-int64(a.CacheKiB), a.Cache, a.Sibling, a.Core)
                < std::make_tuple(-int64(b.CacheKiB), b.Cache, b.Sibling, b.Core);
        });

        Topology topology;
        for (Cpu const& cpu : cpus)
        {
            topology.Order.push_back(cpu.Id);
            topology.CoreOf.emplace_back(cpu.Id, cpu.Core >= 0 ? cpu.Core : cpu.Id);
        }
        return topology;
    }

    Topology const& Cached()
    {
        static Topology const topology = Compute();
        return topology;
    }

    int CoreOf(int cpu)
    {
        for (auto const& [id, core] : Cached().CoreOf)
            if (id == cpu)
                return core;
        return cpu;
    }
}

std::vector<int> const& Acore::CpuPlacement::Order()
{
    return Cached().Order;
}

std::vector<int> Acore::CpuPlacement::AwayFrom(std::vector<int> const& used)
{
    std::vector<int> taken;
    for (int cpu : used)
        taken.push_back(CoreOf(cpu));

    std::vector<int> away;
    for (int cpu : Order())
        if (std::find(taken.begin(), taken.end(), CoreOf(cpu)) == taken.end())
            away.push_back(cpu);
    return away;
}

bool Acore::CpuPlacement::PinThisThread([[maybe_unused]] int cpu)
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    return false;
#endif
}

bool Acore::CpuPlacement::PinProcess([[maybe_unused]] int pid, [[maybe_unused]] std::vector<int> const& cpus)
{
#if defined(__linux__)
    if (cpus.empty())
        return false;

    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus)
        CPU_SET(cpu, &set);
    return sched_setaffinity(pid, sizeof(set), &set) == 0;
#else
    return false;
#endif
}

std::string Acore::CpuPlacement::Describe(std::vector<int> cpus)
{
    std::sort(cpus.begin(), cpus.end());

    std::string text;
    for (std::size_t i = 0; i < cpus.size();)
    {
        std::size_t last = i;
        while (last + 1 < cpus.size() && cpus[last + 1] == cpus[last] + 1)
            ++last;

        if (!text.empty())
            text += ',';
        text += last == i ? std::to_string(cpus[i]) : Acore::StringFormat("{}-{}", cpus[i], cpus[last]);
        i = last + 1;
    }
    return text.empty() ? "-" : text;
}
