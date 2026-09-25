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

#ifndef ANIMUS_RESET_TIMING_H
#define ANIMUS_RESET_TIMING_H

#include "Define.h"
#include <chrono>

namespace Animus
{
    /// Where an episode reset's time goes, accumulated by the scenario on the thread that resets and read by the
    /// env pool right after: creating the seats' characters and placing them in the map (BotSlot::CreateNext),
    /// phasing, position and talent setup, configuring talents/kit/gear, and destroying the previous seats.
    struct ResetTiming
    {
        uint64 CreateNs = 0;
        uint64 PlaceNs = 0;
        uint64 ConfigureNs = 0;
        uint64 DestroyNs = 0;
    };

    inline thread_local ResetTiming CurrentReset;

    /// Nanoseconds since `from`, and move `from` to now.
    inline uint64 ResetSinceNs(std::chrono::steady_clock::time_point& from)
    {
        auto const now = std::chrono::steady_clock::now();
        uint64 const ns = uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - from).count());
        from = now;
        return ns;
    }
}

#endif
