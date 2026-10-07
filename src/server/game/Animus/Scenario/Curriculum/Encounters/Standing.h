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

#ifndef ANIMUS_LIB_CURRICULUM_STANDING_H
#define ANIMUS_LIB_CURRICULUM_STANDING_H

#include "Define.h"
#include "UnitDefines.h"
#include <algorithm>

/*
 * What the movement stages' encounters (sight, seek, interact, the party follow) read of a seat's feet: whether it has
 * stopped, what pressing into a wall costs, and where its distance to a leader falls. Pure, so it is tested alone.
 */
namespace Animus::Curriculum::Standing
{
    /// What pressing into a wall costs over a decision: `wallSeconds` of it (the controller's count, slides
    /// included), the unit having moved `moved` yards against the `asked` its held keys ask over the decision.
    /// Nothing while moved / asked is at least `slideShare` (a slide that makes good progress is the right way
    /// round a corner); in proportion to the shortfall below it, the full `price` a second at no movement.
    [[nodiscard]] inline float WallCharge(float wallSeconds, float moved, float asked, float price, float slideShare)
    {
        if (wallSeconds <= 0.0f || price <= 0.0f)
            return 0.0f;
        float const share = std::clamp(slideShare, 0.0f, 1.0f);
        float const ratio = asked > 1e-4f ? std::clamp(moved / asked, 0.0f, 1.0f) : 0.0f;
        float const blocked = share > 0.0f ? std::clamp((share - ratio) / share, 0.0f, 1.0f) : 0.0f;
        return price * wallSeconds * blocked;
    }

    /// Whether a seat is stopped, as arriving means it, read from the server's applied state (player-controller
    /// §5A.1: what is judged is what the server holds; the client sends MOVE_STOP at once, so at a stop the two
    /// agree): `movementFlags` is the unit's MovementInfo flags -- no forward, back, strafe, pitch, ascend or
    /// descend, not falling (a jump is), not swimming or flying -- and the unit's position moved under
    /// `stopMoved` yards since the last decision. A turn (MOVEMENTFLAG_LEFT/RIGHT, or the mouse's facing alone)
    /// does not count against it: turning on the spot is standing still (ratified 2026-10-05).
    [[nodiscard]] inline bool Stopped(uint32 movementFlags, float movedYards, float stopMoved)
    {
        constexpr uint32 MOVING = MOVEMENTFLAG_MASK_MOVING | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING;
        return (movementFlags & MOVING) == 0 && movedYards < stopMoved;
    }

    /// Where a distance to a leader falls: 0 too close, 1 in the band, 2 behind, 3 lost (past `lostYards`).
    [[nodiscard]] inline uint32 Band(float distance, float bandMin, float bandMax, float lostYards)
    {
        if (distance < bandMin)
            return 0;
        if (distance <= bandMax)
            return 1;
        return distance <= lostYards ? 2 : 3;
    }
}

#endif
