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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_PRICE_H
#define ANIMUS_LIB_CURRICULUM_MOVE_PRICE_H

#include "Define.h"
#include <algorithm>
#include <cmath>

/// **What steering that does not commit costs** (Actions.Jitter, Effort, Fidget; movement-smooth C). The jitter
/// charge was a count inside a hard window: any turn back within JitterWindowMs cost one reversal whether it undid 15
/// degrees or 135 and whether it came 250 ms or 1499 ms later, and a bearing swung round cost half as much per degree
/// as a turn. Now a reversal is priced by the angle it undoes, in quarter turns for every axis alike, and weighed by
/// how recent the choice it undoes was, e^(-dt/tau) -- no edge to time a press against. Pure, so it is tested on its
/// own (MovePriceTest).
namespace Animus::Curriculum::MovePrice
{
    constexpr float QUARTER_TURN = float(M_PI) / 2.0f;     // the unit every steering reversal is priced in
    /// The columns count reversals as they always did, within the old 1500 ms window, so runs before and after the
    /// decay compare; a reversal past it but within WEAVE_MS is a weave, the slow wobble the decay now prices.
    constexpr uint64 COUNT_MS = 1500;
    constexpr uint64 WEAVE_MS = 4000;

    /// Which column a reversal `sinceMs` after the choice it undoes is counted in: 1 a reversal, 2 a weave, 0 none.
    [[nodiscard]] inline uint32 CountAs(uint64 sinceMs)
    {
        return sinceMs < COUNT_MS ? 1 : sinceMs < WEAVE_MS ? 2 : 0;
    }

    /// How much a choice `sinceMs` ago still weighs: 1 now, e^-1 at `decayMs`. Nothing with no decay.
    [[nodiscard]] inline float Recency(uint64 sinceMs, uint32 decayMs)
    {
        return decayMs ? std::exp(-float(sinceMs) / float(decayMs)) : 0.0f;
    }

    /// The angle a steering choice `now` (signed radians) takes back from the one before it, `previous`, in quarter
    /// turns: nothing unless they go opposite ways, then the smaller of the two -- undoing 15 of a 45 degree turn
    /// undoes 15. A turn about (half a turn or more) is not a reversal of anything.
    [[nodiscard]] inline float Undone(float previous, float now)
    {
        if (previous == 0.0f || now == 0.0f || (previous > 0.0f) == (now > 0.0f)
            || std::fabs(now) >= float(M_PI) - 0.01f)
            return 0.0f;
        return std::min(std::fabs(previous), std::fabs(now)) / QUARTER_TURN;
    }

    /// A bearing `apart` steps of `count` round from the last, in quarter turns: a neighbour of eight is half of one,
    /// a reversal two. At least what a turn of the same angle costs.
    [[nodiscard]] inline float BearingSwing(uint32 apart, uint32 count)
    {
        if (!count)
            return 0.0f;
        apart %= count;
        uint32 const steps = std::min(apart, count - apart);
        return float(steps) * (2.0f * float(M_PI) / float(count)) / QUARTER_TURN;
    }

    /// The effort a steering press costs, as a share of a full press: in proportion to its angle up to `full`, so a
    /// fine correction is not priced like a swing.
    [[nodiscard]] inline float EffortOf(float angle, float full)
    {
        return full > 0.0f ? std::min(1.0f, std::fabs(angle) / full) : 1.0f;
    }

    /// Whether a condition held `heldMs` has held long enough to be charged (`graceMs`): a seat that runs into the
    /// band it wants and stops within the grace is not fidgeting, and a range that flickers at its edge is not charged
    /// on every flicker.
    [[nodiscard]] inline bool Settled(uint32 heldMs, uint32 graceMs)
    {
        return heldMs >= graceMs;
    }
}

#endif
