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

#ifndef ANIMUS_LIB_CURRICULUM_SEAT_ENCODER_H
#define ANIMUS_LIB_CURRICULUM_SEAT_ENCODER_H

#include "Layout.h"
#include "SeatView.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

/*
 * A class/role policy's inputs and outputs in the world: a bot's observation row and action mask, and what each action
 * does, by handing each block its slice (see Block).
 */
namespace Animus::Curriculum::SeatEncoder
{
    /// Whether the layout also acts without a target (between gauntlet pulls: food, drink, sustain spells; travel;
    /// the world outside a fight).
    [[nodiscard]] inline bool ActsWithoutTarget(Layout const& layout)
    {
        return layout.Has(BlockId::Gauntlet) || layout.Has(BlockId::Travel) || layout.Has(BlockId::World);
    }

    /// Write the layout's observation (view.L->ObsDim values) and action mask (view.L->NumActions). Action 0 is always
    /// allowed and the character features are always written; the rest only for a living bot with a target (or a
    /// layout that acts without one).
    /// `obs` and `mask` (when given) must arrive zeroed, with mask[0] set: StageScenario::ObserveSeat does it.
    void Observe(SeatView const& view, float* obs, uint8* mask);

    /// Thread time spent observing, summed over every seat on every map thread: per block (BlockId::Core is the
    /// character), and at OBSERVE_VIEW what the seat does before its blocks (StageScenario::ObserveSeat: the
    /// liquid check, target and motion tracking, the memory, SeatView). `forge status` shows where it goes.
    constexpr std::size_t OBSERVE_VIEW = BLOCK_COUNT;
    /// Parts of the move block's time (inside "move", not beside it): the ground probe's refresh as a whole, its
    /// height marches, and its navmesh raycasts.
    constexpr std::size_t OBSERVE_PROBE = BLOCK_COUNT + 1;
    constexpr std::size_t OBSERVE_PROBE_MARCH = BLOCK_COUNT + 2;
    constexpr std::size_t OBSERVE_PROBE_RAYS = BLOCK_COUNT + 3;
    constexpr std::size_t OBSERVE_SLOTS = BLOCK_COUNT + 4;

    /// One map thread's share of it, on cache lines of its own: shared counters that every map thread added to
    /// bounced the same lines between cores every block of every seat. Written by its thread alone (a relaxed store,
    /// no atomic add) and summed when `forge status` reads them (ObserveTotal).
    struct alignas(64) ObserveTally
    {
        std::array<std::atomic<uint64>, OBSERVE_SLOTS> Ns{};
    };

    inline std::mutex ObserveTalliesLock;
    inline std::vector<ObserveTally*> ObserveTallies;     // every thread's, kept for the process's life

    inline ObserveTally& ThreadObserveTally()
    {
        thread_local ObserveTally* tally = []
        {
            ObserveTally* created = new ObserveTally();
            std::lock_guard<std::mutex> guard(ObserveTalliesLock);
            ObserveTallies.push_back(created);
            return created;
        }();
        return *tally;
    }

    inline void AddObserve(std::size_t slot, uint64 ns)
    {
        std::atomic<uint64>& own = ThreadObserveTally().Ns[slot];
        own.store(own.load(std::memory_order_relaxed) + ns, std::memory_order_relaxed);
    }

    /// Every thread's time in `slot` so far.
    [[nodiscard]] inline uint64 ObserveTotal(std::size_t slot)
    {
        std::lock_guard<std::mutex> guard(ObserveTalliesLock);
        uint64 total = 0;
        for (ObserveTally const* tally : ObserveTallies)
            total += tally->Ns[slot].load(std::memory_order_relaxed);
        return total;
    }

    /// Adds the time since `mark` to `slot` and moves `mark` to now.
    inline void ChargeObserve(std::size_t slot, std::chrono::steady_clock::time_point& mark)
    {
        auto const now = std::chrono::steady_clock::now();
        AddObserve(slot, uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count()));
        mark = now;
    }

    /// Apply `action` as the client would. Masked or out-of-range actions do nothing.
    void Apply(SeatView& view, int32 action, SeatActionResult& result);
}

#endif
