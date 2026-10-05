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

#ifndef ANIMUS_MOVEMENT_SEEK_H
#define ANIMUS_MOVEMENT_SEEK_H

#include "PlayerController.h"
#include <algorithm>
#include <cmath>

/// **The seek helper** (player-controller C9): the keys a scripted actor holds to get to a point -- turn toward it
/// with a mouse rate in proportion to how far off it is, hold forward once it is roughly ahead, let go on arrival. For
/// scripted actors only (a scripted leader, a scripted opponent driven by the controller); never a learning seat or a
/// companion, whose movement is the policy's. Pure, so it is tested on its own (SeekTest).
namespace Animus::Movement
{
    struct SeekTuning
    {
        float ArriveYards = 2.0f;           // let go of everything this close
        float AheadRadians = 0.7853982f;    // hold forward only within this of straight ahead (45 degrees)
        float SettleSeconds = 0.25f;        // the turn rate closes the error over this long
        float MaxTurnRate = 6.2831853f;     // rad/s, the mouse's fastest the policy has (360 degrees a second)
    };

    /// The controls that take `body` toward (x, y), from where it stands and how it faces now.
    [[nodiscard]] inline ControlState Seek(BodyState const& body, float x, float y, SeekTuning const& tuning = {})
    {
        ControlState control;
        float const dx = x - body.X;
        float const dy = y - body.Y;
        if (dx * dx + dy * dy <= tuning.ArriveYards * tuning.ArriveYards)
            return control;
        float const error = std::remainder(std::atan2(dy, dx) - body.Yaw, 6.2831853f);
        control.TurnRate = std::clamp(error / tuning.SettleSeconds, -tuning.MaxTurnRate, tuning.MaxTurnRate);
        control.Forward = std::fabs(error) <= tuning.AheadRadians ? 1 : 0;
        return control;
    }
}

#endif
