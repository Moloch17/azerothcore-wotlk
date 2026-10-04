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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_KEEP_H
#define ANIMUS_LIB_CURRICULUM_MOVE_KEEP_H

#include "Define.h"
#include <algorithm>
#include <cmath>

/// **When a held bearing's run is left alone** (MoveBlock Steer, movement-smooth A1). A spline relaunched over a
/// running one is a hitch a client draws in the stride; the keep check used to be "half an 8-yard step left", which on
/// foot restarted every ~750 ms and in water or the air every decision. Now a run is kept while it has a couple of
/// decisions of travel left at its own speed and still goes where the seat wants, and a fresh run reaches about three
/// seconds of travel. Pure, so it is tested on its own (MoveKeepTest).
namespace Animus::Curriculum::MoveKeep
{
    constexpr float REACH_SECONDS = 3.0f;       // a fresh run reaches this much travel at the seat's speed ...
    constexpr float MIN_REACH = 8.0f;           // ... and never less than the old step
    constexpr uint32 KEEP_DECISIONS = 2;        // a run is kept while this many decisions of travel are left
    constexpr float HEADING_SLACK = 0.35f;      // about 20 degrees: a path bent round a rock
    constexpr float PITCH_SLACK = 0.2f;         // about 11 degrees of climb or dive
    constexpr float FACING_SLACK = 0.1f;        // under 6 degrees of the facing wanted

    /// How far a fresh run reaches, at `speed` yards a second.
    [[nodiscard]] inline float Reach(float speed)
    {
        return std::max(MIN_REACH, std::max(0.0f, speed) * REACH_SECONDS);
    }

    /// Whether a run still under way is kept: `remaining` yards left at `velocity` yards a second last at least
    /// KEEP_DECISIONS decisions of `decisionMs`, and its course, climb and the seat's facing are within their slack
    /// of what is wanted (errors in radians, any sign). A run with no velocity is never kept.
    [[nodiscard]] inline bool KeepRun(float remaining, float velocity, uint32 decisionMs, float headingError,
        float pitchError, float facingError)
    {
        if (velocity <= 0.0f || decisionMs == 0)
            return false;
        float const leftMs = remaining / velocity * 1000.0f;
        return leftMs >= float(KEEP_DECISIONS * decisionMs) && std::fabs(headingError) < HEADING_SLACK
            && std::fabs(pitchError) < PITCH_SLACK && std::fabs(facingError) < FACING_SLACK;
    }
}

#endif
