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

#ifndef ANIMUS_LIB_KINEMATICS_H
#define ANIMUS_LIB_KINEMATICS_H

#include "Define.h"
#include <algorithm>

namespace Animus
{
    /// The kinematic sample a STEP carries per agent (protocol 20): how its body stands and moves, the same SAMPLE the
    /// human capture is resampled into (apps/forge/python/animus/human/FORMAT.md section 3, motion.py), so the
    /// learner's style reward and realism score compare a bot with players like for like:
    ///
    ///     [t_seconds, x, y, z, yaw, pitch, mode, mounted, speed, in_combat]
    ///
    /// An agent without a body (a director, an empty seat, a bot not in the world) sends zeros.
    namespace Kinematics
    {
        constexpr uint32 SAMPLE_DIM = 10;

        enum Column : uint32
        {
            T = 0, X, Y, Z, YAW, PITCH, MODE, MOUNTED, SPEED, IN_COMBAT
        };

        /// motion.MODE_*: what the body is moving through.
        enum class Mode : uint8
        {
            Ground = 0,
            Swimming = 1,
            Flying = 2,
            Airborne = 3,       // a jump or a fall on its way
        };

        struct Body
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
            float Yaw = 0.0f;           // the unit's orientation, radians
            float Pitch = 0.0f;         // where the seat looks up or down, radians
            Mode Motion = Mode::Ground;
            bool Mounted = false;
            float Speed = 0.0f;         // the forward speed in force, yards a second (mounted included)
            bool InCombat = false;
        };

        /// The mode of a body: a jump or a fall in flight is airborne whatever it is over; then water; then flight,
        /// which is a flying mount off the ground (a seat on one is flagged flying from the moment it mounts, so the
        /// flag alone would call a ride across a field a flight).
        [[nodiscard]] inline Mode ModeOf(bool jumpingOrFalling, bool inWater, bool flyingAloft)
        {
            if (jumpingOrFalling && !flyingAloft)
                return Mode::Airborne;
            if (inWater)
                return Mode::Swimming;
            if (flyingAloft)
                return Mode::Flying;
            return Mode::Ground;
        }

        /// Write `body` at episode time `seconds` into `out` [SAMPLE_DIM].
        inline void Write(float seconds, Body const& body, float* out)
        {
            out[T] = std::max(0.0f, seconds);
            out[X] = body.X;
            out[Y] = body.Y;
            out[Z] = body.Z;
            out[YAW] = body.Yaw;
            out[PITCH] = body.Pitch;
            out[MODE] = float(uint8(body.Motion));
            out[MOUNTED] = body.Mounted ? 1.0f : 0.0f;
            out[SPEED] = std::max(0.0f, body.Speed);
            out[IN_COMBAT] = body.InCombat ? 1.0f : 0.0f;
        }

        /// No body: zeros.
        inline void Clear(float* out)
        {
            std::fill(out, out + SAMPLE_DIM, 0.0f);
        }
    }
}

#endif
