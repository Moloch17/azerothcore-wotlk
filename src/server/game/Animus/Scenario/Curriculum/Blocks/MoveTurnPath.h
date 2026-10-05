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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_TURN_PATH_H
#define ANIMUS_LIB_CURRICULUM_MOVE_TURN_PATH_H

#include <algorithm>
#include <cmath>
#include <vector>

/// **A turn walked as one run** (MoveBlock Steer, movement-smooth A2). A turn under a held bearing swings the
/// heading TURN_RATE a decision, more than the keep check's slack, so every decision of it relaunched the spline: an
/// about-turn was four restarts in a second, the largest source left. The whole remaining turn is laid out at launch
/// instead -- one leg a decision at the heading StepTurn will have given the seat by then, then straight on -- so the
/// feet are where the per-decision steps would have put them and the run is launched once. Pure, so it is tested on
/// its own (MoveTurnPathTest).
namespace Animus::Curriculum::MoveTurnPath
{
    struct Leg
    {
        float Heading = 0.0f;       // radians, 0..2pi
        float Length = 0.0f;        // yards
    };

    /// The legs of a run launched at `heading` with `turnLeft` radians of a turn still to come (signed, left
    /// positive, as SeatView::TurnLeft after this decision's step), stepping at most `rate` a decision: one leg of
    /// `spacing` yards (the seat's speed over a decision) per decision while the turn lasts, then one straight leg,
    /// `reach` yards in all and cut short there. A run with no turn left is one straight leg.
    [[nodiscard]] inline std::vector<Leg> Legs(float heading, float turnLeft, float rate, float spacing, float reach)
    {
        auto const wrap = [](float angle)
        {
            float const twoPi = 2.0f * float(M_PI);
            float const wrapped = std::fmod(angle, twoPi);
            return wrapped < 0.0f ? wrapped + twoPi : wrapped;
        };

        std::vector<Leg> legs;
        if (reach <= 0.0f)
            return legs;
        float left = std::fabs(turnLeft) < 1e-4f ? 0.0f : turnLeft;
        if (rate <= 0.0f || spacing <= 0.0f)
            left = 0.0f;

        float course = heading;
        float walked = 0.0f;
        while (walked < reach)
        {
            bool const turning = left != 0.0f;
            float const length = turning ? std::min(spacing, reach - walked) : reach - walked;
            legs.push_back({ wrap(course), length });
            walked += length;
            if (!turning)
                break;
            // The step StepTurn takes at the next decision, the same float arithmetic.
            float const step = std::clamp(left, -rate, rate);
            course += step;
            left -= step;
            if (std::fabs(left) < 1e-4f)
                left = 0.0f;
        }
        return legs;
    }

    struct AirLeg
    {
        float Heading = 0.0f;       // radians, 0..2pi
        float Pitch = 0.0f;         // radians, up positive
        float Length = 0.0f;        // yards along the leg, not over the ground
    };

    /// The legs of a run through the water or the air (movement-smooth A2 off the ground): as Legs, with a pitch
    /// still to come stepped beside the turn -- `pitchLeft` radians from `pitch` (SeatView::PitchTarget less
    /// SeatView::Pitch, after this decision's step) at most `pitchRate` a decision, StepPitch's arithmetic -- one leg
    /// a decision while either lasts, then one straight leg at the heading and pitch they end on. A run with neither
    /// left is one straight leg.
    [[nodiscard]] inline std::vector<AirLeg> AirLegs(float heading, float turnLeft, float turnRate, float pitch,
        float pitchLeft, float pitchRate, float spacing, float reach)
    {
        auto const wrap = [](float angle)
        {
            float const twoPi = 2.0f * float(M_PI);
            float const wrapped = std::fmod(angle, twoPi);
            return wrapped < 0.0f ? wrapped + twoPi : wrapped;
        };

        std::vector<AirLeg> legs;
        if (reach <= 0.0f)
            return legs;
        float turn = std::fabs(turnLeft) < 1e-4f || turnRate <= 0.0f ? 0.0f : turnLeft;
        float tilt = std::fabs(pitchLeft) < 1e-4f || pitchRate <= 0.0f ? 0.0f : pitchLeft;
        if (spacing <= 0.0f)
            turn = tilt = 0.0f;

        float course = heading;
        float climb = pitch;
        float walked = 0.0f;
        while (walked < reach)
        {
            bool const turning = turn != 0.0f || tilt != 0.0f;
            float const length = turning ? std::min(spacing, reach - walked) : reach - walked;
            legs.push_back({ wrap(course), climb, length });
            walked += length;
            if (!turning)
                break;
            float const step = std::clamp(turn, -turnRate, turnRate);
            course += step;
            turn -= step;
            if (std::fabs(turn) < 1e-4f)
                turn = 0.0f;
            float const lift = std::clamp(tilt, -pitchRate, pitchRate);
            climb += lift;
            tilt -= lift;
            if (std::fabs(tilt) < 1e-4f)
                tilt = 0.0f;
        }
        return legs;
    }
}

#endif
