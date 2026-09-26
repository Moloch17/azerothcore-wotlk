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
    /// Parts of the core block's time: its per-action features (known rank, cooldown, auras on the seat and its
    /// target) and its per-action masks (IsActionAllowed); the rest of "core" is the character's own stats.
    constexpr std::size_t OBSERVE_CORE_FEATURES = BLOCK_COUNT + 4;
    constexpr std::size_t OBSERVE_CORE_MASKS = BLOCK_COUNT + 5;
    constexpr std::size_t OBSERVE_SLOTS = BLOCK_COUNT + 6;
    inline std::array<std::atomic<uint64>, OBSERVE_SLOTS> ObserveNs{};

    /// Adds the time since `mark` to `slot` and moves `mark` to now.
    inline void ChargeObserve(std::size_t slot, std::chrono::steady_clock::time_point& mark)
    {
        auto const now = std::chrono::steady_clock::now();
        ObserveNs[slot].fetch_add(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count()),
            std::memory_order_relaxed);
        mark = now;
    }

    /// Apply `action` as the client would. Masked or out-of-range actions do nothing.
    void Apply(SeatView& view, int32 action, SeatActionResult& result);
}

#endif
