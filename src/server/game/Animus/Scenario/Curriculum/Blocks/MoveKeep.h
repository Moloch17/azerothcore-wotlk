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
#include "UnitDefines.h"
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
    constexpr float NOTHING_TO_WALK = 0.5f;     // a path to the edge shorter than this: the seat is already there
    constexpr float SPEED_SLACK = 0.05f;        // a run launched at a speed more than 5% off the seat's is relaunched
    constexpr float SWIM_ENTER = 0.75f;         // water this deep (of the seat's height) is swum ...
    constexpr float SWIM_LEAVE = 0.4f;          // ... and is walked again only once it is this shallow

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

    /// Whether a run whose drawn head is `shown` (radians) is relaunched so a watching client draws `decided`: more
    /// than `threshold` radians apart, either way round. The same rule the realm's MoveBlock keeps (A3).
    [[nodiscard]] inline bool FacingRelaunch(float shown, float decided, float threshold)
    {
        float const apart = std::fabs(std::remainder(decided - shown, 2.0f * float(M_PI)));
        return apart > threshold;
    }

    /// Whether a run whose end is as far as the ground lets the bearing go -- its path came back incomplete, the
    /// point it was aimed at unreachable -- is kept to that end (movement-smooth A7). Relaunching it only finds the
    /// same end nearer, so the time-left rule would restart it every decision of its last two; it is let go only
    /// when its course or climb no longer fits, or it has stopped.
    [[nodiscard]] inline bool KeepEdgeRun(float velocity, float headingError, float pitchError)
    {
        return velocity > 0.0f && std::fabs(headingError) < HEADING_SLACK && std::fabs(pitchError) < PITCH_SLACK;
    }

    /// Whether a run launched at `launched` yards a second is relaunched because the seat now moves at `now`: a
    /// sprint, a slow, a mount or a form change (movement-smooth A9). A spline keeps the speed it was launched with.
    [[nodiscard]] inline bool SpeedChanged(float launched, float now)
    {
        return launched > 0.0f && std::fabs(now - launched) > SPEED_SLACK * launched;
    }

    /// Whether the seat is steered as a swimmer (movement-smooth A9): the core says it is in the water and the water
    /// is deep enough to swim, or it was swimming and the water has not yet got shallow enough to walk. At the shore
    /// the core's in-water flag flickers as the seat bobs, and each flicker switched a straight swim for a pathfound
    /// walk and back -- a relaunch each time. Depth and height in yards.
    [[nodiscard]] inline bool SwimMode(bool wasSwimming, bool coreInWater, float depth, float height)
    {
        if (!coreInWater && depth < SWIM_LEAVE * height)
            return false;
        if (coreInWater && depth >= SWIM_ENTER * height)
            return true;
        return wasSwimming;
    }

    /// The speed a run Steer launches moves at, from how Steer moves the seat: flying, swimming by SwimMode's
    /// hysteresis (not the core's in-water flag, which flickers at the shore), or on foot; backwards for a bearing
    /// behind it. Every run is launched at this speed explicitly, so the path type and the speed always agree
    /// (movement-smooth A9).
    [[nodiscard]] inline UnitMoveType SteerMoveType(bool flying, bool swimming, bool back)
    {
        if (flying)
            return back ? MOVE_FLIGHT_BACK : MOVE_FLIGHT;
        if (swimming)
            return back ? MOVE_SWIM_BACK : MOVE_SWIM;
        return back ? MOVE_RUN_BACK : MOVE_RUN;
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

    /// **Why a run under way was relaunched** (restart-causes): stage1's seats relaunched their run ~68 times a minute
    /// with almost none of it on a turn (2026-10-04, turn_restarts 0.6), so spline_restarts is split by what let the
    /// run go. Press*: a movement press changed what the seat wants, by kind -- a bearing, a whole turn, a facing mode,
    /// a pitch, or anything else the move block takes (HALT, a jump); stage2's argmax evaluations still had ~18 of
    /// these a minute (2026-10-04), so the split says which presses are worth pricing. Stop: the run was stopped, not
    /// relaunched -- Unit::StopMoving launches a Stop spline that is born finished, so a stop changed the spline id
    /// like a relaunch and was counted as one (most of the first split's "other"). Shown: a watching client's stale
    /// head. Speed: a sprint, slow, mount or form. Course and Climb: the run no longer goes where the seat wants.
    /// Time: under KEEP_DECISIONS of travel left; TimeCapped the same for a run launched short by CappedReach. Other:
    /// a run launched by anything but the move block (a follow, an advance, a death walk).
    enum class Relaunch : uint8
    {
        None,
        PressBearing,
        PressTurn,
        PressFacing,
        PressPitch,
        PressOther,
        Stop,
        Shown,
        Speed,
        Course,
        Climb,
        Time,
        TimeCapped,
        Other,
        Count
    };

    [[nodiscard]] inline bool IsPress(Relaunch cause)
    {
        return cause >= Relaunch::PressBearing && cause <= Relaunch::PressOther;
    }

    [[nodiscard]] inline char const* RelaunchName(Relaunch cause)
    {
        switch (cause)
        {
            case Relaunch::PressBearing:    return "press_bearing";
            case Relaunch::PressTurn:       return "press_turn";
            case Relaunch::PressFacing:     return "press_facing";
            case Relaunch::PressPitch:      return "press_pitch";
            case Relaunch::PressOther:      return "press_other";
            case Relaunch::Stop:            return "stop";
            case Relaunch::Shown:           return "shown";
            case Relaunch::Speed:           return "speed";
            case Relaunch::Course:          return "course";
            case Relaunch::Climb:           return "climb";
            case Relaunch::Time:            return "time";
            case Relaunch::TimeCapped:      return "time_capped";
            case Relaunch::Other:           return "other";
            default:                        return "none";
        }
    }

    /// What a decision that changed a seat's running spline is counted as: a stop when the spline is finished after
    /// it (a Stop spline is launched done), else what Steer's keep said let the run go, else the movement press
    /// applied this decision (a jump launches outside the keep), else another block's launch.
    [[nodiscard]] inline Relaunch Recorded(bool stoppedAfter, Relaunch launchCause, Relaunch pressKind)
    {
        if (stoppedAfter)
            return Relaunch::Stop;
        if (launchCause != Relaunch::None)
            return launchCause;
        if (IsPress(pressKind))
            return pressKind;
        return Relaunch::Other;
    }

    /// What let a run go that Steer's keep did not keep, in the order the keep asks: the two forced relaunches, then
    /// course and climb (the seat wants somewhere else), then time (it is running out). `capped` is whether the run
    /// was launched with a reach CappedReach cut short. None only when KeepRun would have kept it.
    [[nodiscard]] inline Relaunch WhyRelaunched(bool shownStale, bool resped, float remaining, float velocity,
        uint32 decisionMs, float headingError, float pitchError, bool capped)
    {
        if (shownStale)
            return Relaunch::Shown;
        if (resped)
            return Relaunch::Speed;
        if (std::fabs(headingError) >= HEADING_SLACK)
            return Relaunch::Course;
        if (std::fabs(pitchError) >= PITCH_SLACK)
            return Relaunch::Climb;
        if (KeepRun(remaining, velocity, decisionMs, headingError, pitchError))
            return Relaunch::None;
        return capped ? Relaunch::TimeCapped : Relaunch::Time;
    }
}

#endif
