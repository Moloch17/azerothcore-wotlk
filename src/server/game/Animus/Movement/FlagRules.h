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

#ifndef ANIMUS_MOVEMENT_FLAG_RULES_H
#define ANIMUS_MOVEMENT_FLAG_RULES_H

#include <cstdint>

/// **The movement flags a server takes off a client's report** (player-controller C4, strip parity): the
/// REMOVE_VIOLATING_FLAGS rules of WorldSession::ReadMovementInfo, moved here unchanged and in their order (each sees
/// the flags the ones before it left), so a real client's packet and the forge's controller report (PlayerLink) are
/// stripped by the same code -- training never accepts a flag the realm drops. Pure, so it is tested on its own.
namespace Animus::Movement
{
    /// What the rules ask of the player (WorldSession::ReadMovementInfo's GetPlayer() and GetSecurity()).
    struct FlagFacts
    {
        bool HoverAura = false;             // HasHoverAura
        bool WaterWalkAura = false;         // HasWaterWalkAura
        bool GhostAura = false;             // HasGhostAura
        bool FeatherFallAura = false;       // HasFeatherFallAura
        bool FlyAura = false;               // the mover's HasFlyAura or HasIncreaseMountedFlightSpeedAura
        bool Privileged = false;            // GetSecurity() != SEC_PLAYER (a GM may fly without the aura)
        bool SplineRunning = false;         // the player's movespline initialised and not finalised
    };

    namespace FlagBit
    {
        constexpr uint32_t FORWARD = 0x00000001;
        constexpr uint32_t BACKWARD = 0x00000002;
        constexpr uint32_t STRAFE_LEFT = 0x00000004;
        constexpr uint32_t STRAFE_RIGHT = 0x00000008;
        constexpr uint32_t LEFT = 0x00000010;
        constexpr uint32_t RIGHT = 0x00000020;
        constexpr uint32_t PITCH_UP = 0x00000040;
        constexpr uint32_t PITCH_DOWN = 0x00000080;
        constexpr uint32_t DISABLE_GRAVITY = 0x00000400;
        constexpr uint32_t ROOT = 0x00000800;
        constexpr uint32_t FALLING = 0x00001000;
        constexpr uint32_t ASCENDING = 0x00400000;
        constexpr uint32_t DESCENDING = 0x00800000;
        constexpr uint32_t CAN_FLY = 0x01000000;
        constexpr uint32_t FLYING = 0x02000000;
        constexpr uint32_t SPLINE_ENABLED = 0x08000000;
        constexpr uint32_t WATERWALKING = 0x10000000;
        constexpr uint32_t FALLING_SLOW = 0x20000000;
        constexpr uint32_t HOVER = 0x40000000;
    }

    /// `flags` with every violating flag removed, rule by rule in ReadMovementInfo's order.
    [[nodiscard]] constexpr uint32_t SanitizeFlags(uint32_t flags, FlagFacts const& facts)
    {
        namespace F = FlagBit;
        auto has = [&flags](uint32_t mask) { return (flags & mask) != 0; };
        auto both = [&flags](uint32_t a, uint32_t b) { return (flags & a) != 0 && (flags & b) != 0; };

        // A client's ROOT is never valid: the server's own SendMoveRoot is what a root rests on.
        if (has(F::ROOT))
            flags &= ~F::ROOT;
        if (has(F::HOVER) && !facts.HoverAura)
            flags &= ~F::HOVER;
        if (both(F::ASCENDING, F::DESCENDING))
            flags &= ~(F::ASCENDING | F::DESCENDING);
        if (both(F::LEFT, F::RIGHT))
            flags &= ~(F::LEFT | F::RIGHT);
        if (both(F::STRAFE_LEFT, F::STRAFE_RIGHT))
            flags &= ~(F::STRAFE_LEFT | F::STRAFE_RIGHT);
        if (both(F::PITCH_UP, F::PITCH_DOWN))
            flags &= ~(F::PITCH_UP | F::PITCH_DOWN);
        if (both(F::FORWARD, F::BACKWARD))
            flags &= ~(F::FORWARD | F::BACKWARD);
        if (has(F::WATERWALKING) && !facts.WaterWalkAura && !facts.GhostAura)
            flags &= ~F::WATERWALKING;
        if (has(F::FALLING_SLOW) && !facts.FeatherFallAura)
            flags &= ~F::FALLING_SLOW;
        if (has(F::FLYING | F::CAN_FLY) && !facts.Privileged && !facts.FlyAura)
            flags &= ~(F::FLYING | F::CAN_FLY);
        if (has(F::CAN_FLY | F::DISABLE_GRAVITY) && has(F::FALLING))
            flags &= ~F::FALLING;
        if (has(F::SPLINE_ENABLED) && !facts.SplineRunning)
            flags &= ~F::SPLINE_ENABLED;
        return flags;
    }
}

#endif
