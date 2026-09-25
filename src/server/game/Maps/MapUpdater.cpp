/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
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

#include "MapUpdater.h"
#include "AnimusForge.h"
#include "DatabaseEnv.h"
#include "Errors.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include <algorithm>
#include <chrono>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace
{
    inline void CpuRelax()
    {
#if defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }
}

MapUpdater::MapUpdater()
    : _tasks(std::make_unique<Task[]>(MaxTasks)), _ready(std::make_unique<std::atomic<bool>[]>(MaxTasks))
{
    for (uint32 i = 0; i < MaxTasks; ++i)
        _ready[i].store(false, std::memory_order_relaxed);
}

MapUpdater::~MapUpdater()
{
    if (activated())
        deactivate();
}

void MapUpdater::activate(std::size_t num_threads)
{
    // A pool that was deactivated starts again here (the module switches thread counts between decisions).
    _stop.store(false, std::memory_order_release);

    _workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
        _workers.emplace_back(&MapUpdater::WorkerThread, this, uint32(i));
}

void MapUpdater::deactivate()
{
    wait();

    {
        std::lock_guard<std::mutex> guard(_parkLock);
        _stop.store(true, std::memory_order_release);
    }
    _parkCv.notify_all();

    for (std::thread& worker : _workers)
        if (worker.joinable())
            worker.join();

    // Joined threads are not workers: activated() has to say so, and activate() must not keep them around.
    _workers.clear();
}

void MapUpdater::Push(Task const& task)
{
    _pending.fetch_add(1, std::memory_order_acq_rel);

    uint32 const slot = _count.fetch_add(1, std::memory_order_acq_rel);
    ASSERT(slot < MaxTasks, "MapUpdater: more than {} tasks in one tick", MaxTasks);

    _tasks[slot] = task;
    _ready[slot].store(true, std::memory_order_release);

    if (_parked.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> guard(_parkLock);
        _parkCv.notify_all();
    }
}

void MapUpdater::schedule_update(Map& map, uint32 diff, uint32 s_diff)
{
    Task task;
    task.map = &map;
    task.diff = diff;
    task.s_diff = s_diff;
    Push(task);
}

void MapUpdater::schedule_map_preload(uint32 mapid)
{
    Task task;
    task.mapId = mapid;
    Push(task);
}

void MapUpdater::RunMapTick(Map& map, uint32 diff, uint32 s_diff)
{
    // The sim's envs on this map, on this thread: they take the last decision's actions before the tick and are
    // scored and observed after it, so the only part of a decision left for the world thread is the part that has
    // to be serial. A map with no env of its own pays one branch for each.
    auto const start = std::chrono::steady_clock::now();
    Map::TaskSample sample;
#if defined(__linux__)
    sample.Cpu = sched_getcpu();
#endif

    sAnimusForge->OnMapPrologue(map);
    map.Update(diff, s_diff);
    map.DelayedUpdate(diff);
    sAnimusForge->OnMapEpilogue(map);

    // Never zero for a task that ran: zero is how MapMgr tells a map that was not ticked.
    sample.Ns = std::max<uint64>(1, uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()));
    map.SetTaskSample(sample);
}

void MapUpdater::Run(Task const& task)
{
    if (task.map)
    {
        RunMapTick(*task.map, task.diff, task.s_diff);
        return;
    }

    Map* map = sMapMgr->CreateBaseMap(task.mapId);
    LOG_INFO("server.loading", ">> Loading All Grids For Map {} ({})", map->GetId(), map->GetMapName());
    map->LoadAllGrids();
}

bool MapUpdater::RunOne()
{
    uint32 slot = _next.load(std::memory_order_acquire);
    for (;;)
    {
        if (slot >= _count.load(std::memory_order_acquire))
            return false;
        if (_next.compare_exchange_weak(slot, slot + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }

    // The slot was taken by a pusher that has not written it yet: it is a few instructions away.
    while (!_ready[slot].load(std::memory_order_acquire))
        CpuRelax();

    Task const task = _tasks[slot];
    _ready[slot].store(false, std::memory_order_relaxed);

    Run(task);

    _pending.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

void MapUpdater::wait()
{
    // The calling thread works too, then spins on the stragglers: the join is one atomic read.
    for (;;)
    {
        if (RunOne())
            continue;
        if (_pending.load(std::memory_order_acquire) == 0)
            break;
        CpuRelax();
    }

    // Every task is finished and none can be claimed (next >= count). Rewind for the next tick: count first,
    // so a worker that reads the rewound next also reads a rewound count.
    //
    // The load-bearing invariant: a slot is claimed (next advanced) strictly before its task runs, and
    // _pending is decremented only after the task ran, so a claimed-but-unrun slot keeps _pending above
    // zero and this rewind cannot happen underneath a worker still waiting on that slot's _ready flag.
    _count.store(0, std::memory_order_release);
    _next.store(0, std::memory_order_release);
}

void MapUpdater::PinToCpu([[maybe_unused]] uint32 index)
{
#if defined(__linux__)
    // Worker k takes the k-th CPU this process may run on, so a container's cpuset is honoured and every
    // worker keeps its cache. The world thread and the learner stay unpinned.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        return;

    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed))
            cpus.push_back(cpu);

    if (cpus.empty())
        return;

    cpu_set_t mine;
    CPU_ZERO(&mine);
    CPU_SET(cpus[index % cpus.size()], &mine);
    pthread_setaffinity_np(pthread_self(), sizeof(mine), &mine);
#endif
}

void MapUpdater::WorkerThread(uint32 index)
{
    PinToCpu(index);

    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    // How long a worker spins for the next task before parking. A tick's tasks arrive within microseconds of
    // each other; between ticks the learner exchange can take milliseconds, and that is what parking is for.
    constexpr uint32 SpinRounds = 4000;

    while (!_stop.load(std::memory_order_acquire))
    {
        if (RunOne())
            continue;

        bool found = false;
        for (uint32 round = 0; round < SpinRounds && !found; ++round)
        {
            CpuRelax();
            found = _next.load(std::memory_order_acquire) < _count.load(std::memory_order_acquire);
        }
        if (found)
            continue;

        std::unique_lock<std::mutex> lock(_parkLock);
        _parked.fetch_add(1, std::memory_order_acq_rel);
        _parkCv.wait(lock, [this]
        {
            return _stop.load(std::memory_order_acquire)
                || _next.load(std::memory_order_acquire) < _count.load(std::memory_order_acquire);
        });
        _parked.fetch_sub(1, std::memory_order_acq_rel);
    }
}
