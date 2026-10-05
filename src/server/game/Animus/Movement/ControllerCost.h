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

#ifndef ANIMUS_MOVEMENT_CONTROLLER_COST_H
#define ANIMUS_MOVEMENT_CONTROLLER_COST_H

#include <atomic>
#include <cstdint>

/// What the player controller costs the sim, for the status line (player-controller C8): thread time in the seats'
/// world-tick work (the server's orders answered, the body stepped, the reports applied -- PlayerLink and its
/// ClientMovement::Apply are inside it) and how many seat ticks that was. Every map thread adds to it, once per env
/// tick, so the atomics are touched a few hundred times a second, not per seat.
namespace Animus::Movement::ControllerCost
{
    inline std::atomic<uint64_t> Ns{ 0 };
    inline std::atomic<uint64_t> SeatTicks{ 0 };

    inline void Add(uint64_t ns, uint64_t seatTicks)
    {
        Ns.fetch_add(ns, std::memory_order_relaxed);
        SeatTicks.fetch_add(seatTicks, std::memory_order_relaxed);
    }
}

#endif
