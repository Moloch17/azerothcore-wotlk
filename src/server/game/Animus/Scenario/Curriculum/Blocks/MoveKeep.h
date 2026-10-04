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

    /// How far a fresh run reaches, at `speed` yards a second.
    [[nodiscard]] inline float Reach(float speed)
    {
        return std::max(MIN_REACH, std::max(0.0f, speed) * REACH_SECONDS);
    }

    /// How far a fresh run reaches while the held bearing has `optionLeftMs` to go: no further than the seat would
    /// travel before the option lapses plus KEEP_DECISIONS decisions, so a bearing let go does not coast on for the
    /// rest of a three-second run it never asked for -- into a pack or off a ledge -- and the credit for stopping
    /// stays with the decision that stopped. Never below MIN_REACH while the option runs; a refresh extends the
    /// option, so a seat walking on never hitches.
    [[nodiscard]] inline float CappedReach(float speed, uint64 optionLeftMs, uint32 decisionMs)
    {
        float const left = std::max(0.0f, speed) * float(optionLeftMs + uint64(KEEP_DECISIONS) * decisionMs) / 1000.0f;
        return std::min(Reach(speed), std::max(MIN_REACH, left));
    }

    /// Whether a run still under way when its held bearing lapsed has more than KEEP_DECISIONS decisions of travel
    /// left, and so is stopped rather than left to coast.
    [[nodiscard]] inline bool CoastsTooFar(float remaining, float velocity, uint32 decisionMs)
    {
        return velocity > 0.0f && remaining / velocity * 1000.0f > float(KEEP_DECISIONS * decisionMs);
    }

    /// Whether a run still under way is kept: `remaining` yards left at `velocity` yards a second last at least
    /// KEEP_DECISIONS decisions of `decisionMs`, and its course and climb are within their slack of what is wanted
    /// (errors in radians, any sign). A run with no velocity is never kept. Facing is not asked: it is turned on
    /// the run itself (Encoding::ReaimRun, movement-smooth A3).
    [[nodiscard]] inline bool KeepRun(float remaining, float velocity, uint32 decisionMs, float headingError,
        float pitchError)
    {
        if (velocity <= 0.0f || decisionMs == 0)
            return false;
        float const leftMs = remaining / velocity * 1000.0f;
        return leftMs >= float(KEEP_DECISIONS * decisionMs) && std::fabs(headingError) < HEADING_SLACK
            && std::fabs(pitchError) < PITCH_SLACK;
    }
}

#endif
