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

#ifndef _MAP_UPDATER_H_INCLUDED
#define _MAP_UPDATER_H_INCLUDED

#include "Define.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Map;

/// The sim host's map scheduler.
///
/// A persistent pool of pinned worker threads and one fixed task array per tick. Tasks are pushed by
/// the world thread (base maps, grid preloads) and by map tasks themselves (an instance container
/// pushes its children while it runs), claimed with one atomic increment, and never heap-allocated.
/// Workers spin briefly for the next task, then park on a condition variable until the next push.
/// wait() makes the calling thread a worker too and returns when every pushed task has finished.
///
/// A map task is the whole of a map's tick: Map::Update followed by its own Map::DelayedUpdate, so
/// nothing about a map is left for the world thread to do serially after the join.
///
/// activate() / deactivate() may be called repeatedly between ticks: the module switches thread
/// counts while benchmarking.
///
/// Placement (Acore::CpuPlacement::Order): the thread that calls activate(), the world thread, which runs tasks
/// too in wait(), takes the first CPU and worker k the (k + 1)-th, so the pool fills one die's physical cores, then
/// their SMT siblings, before it crosses to another die's L3. Pinned by CPU number instead, on a two-die 9950X3D,
/// every map task ran 40% slower above 8 threads.
class MapUpdater
{
public:
    MapUpdater();
    ~MapUpdater();

    void activate(std::size_t num_threads);
    void deactivate();
    [[nodiscard]] bool activated() const { return !_workers.empty(); }

    /// The CPUs the world thread and the workers are pinned to (empty before activate()).
    [[nodiscard]] std::vector<int> const& PoolCpus() const { return _poolCpus; }

    /// A map's full tick: Update(diff, s_diff) then DelayedUpdate(diff). Callable from any thread.
    void schedule_update(Map& map, uint32 diff, uint32 s_diff);
    /// The same tick run here and now, for the callers that update a map without a worker (no threads
    /// configured). The sim's per-map halves of a decision hang off this, so every path that ticks a map
    /// goes through it and none of them can be forgotten.
    static void RunMapTick(Map& map, uint32 diff, uint32 s_diff);
    /// Create the base map and load all of its grids (PreloadAllNonInstancedMapGrids).
    void schedule_map_preload(uint32 mapid);
    /// Run tasks on the calling thread until every pushed task, including those pushed meanwhile, is done.
    void wait();

private:
    struct Task
    {
        Map* map = nullptr;          ///< nullptr: a preload of mapId
        uint32 mapId = 0;
        uint32 diff = 0;
        uint32 s_diff = 0;
    };

    /// More than the largest env count the module allows plus every base map, with room to spare.
    static constexpr uint32 MaxTasks = 16384;

    void Push(Task const& task);
    bool RunOne();
    static void Run(Task const& task);
    void WorkerThread(uint32 index);
    static void PinToCpu(uint32 index);

    std::unique_ptr<Task[]> _tasks;
    std::unique_ptr<std::atomic<bool>[]> _ready;    ///< slot written and publishable
    // One cache line each. _next is written by every claim and _pending by every finish, while idle workers spin
    // reading _next and _count: sharing a line, every claim and finish would pull it away from every spinner.
    alignas(64) std::atomic<uint32> _count{ 0 };    ///< slots taken this tick
    alignas(64) std::atomic<uint32> _next{ 0 };     ///< slots claimed this tick
    alignas(64) std::atomic<uint32> _pending{ 0 };  ///< pushed and not yet finished
    alignas(64) std::atomic<uint32> _parked{ 0 };   ///< workers waiting on the condition variable
    alignas(64) std::atomic<bool> _stop{ false };
    std::vector<int> _poolCpus;
    std::mutex _parkLock;
    std::condition_variable _parkCv;
    std::vector<std::thread> _workers;
};

#endif //_MAP_UPDATER_H_INCLUDED
