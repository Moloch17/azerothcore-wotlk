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

#ifndef ANIMUS_MOVEMENT_REPORT_CADENCE_H
#define ANIMUS_MOVEMENT_REPORT_CADENCE_H

#include "PlayerController.h"
#include <cstdint>
#include <vector>

/// **When a controlled seat reports its movement** (player-controller plan §5A.1): as the 3.3.5a client does, read out
/// of Wow.exe in C0c (.agents/plans/player-controller/client-constants.md §5). A movement packet the moment a held
/// control or the mode changes, and a heartbeat 500 ms after the last movement packet of any kind while moving.
/// The server credits position only from these reports, exactly as for a player.
namespace Animus::Movement::Cadence
{
    /// A heartbeat is due this long after the last movement packet of any kind: every send reschedules it (0x71f0c0
    /// calls 0x6e9b70: next = now + 0x1f4).
    constexpr uint32_t HEARTBEAT_MS = 500;
    /// ... while any of these is set (0x6f0b0f: test [mover+0x44], 0xc0100f): moving, falling, ascending or
    /// descending. Turning on the spot alone is not moving: it reports its start and stop, no heartbeats.
    constexpr uint32_t HEARTBEAT_FLAGS = 0x00c0100f;

    /// The client's movement opcodes (the core's values, Server/Protocol/Opcodes.h), mirrored to stay core-free.
    namespace Op
    {
        constexpr uint16_t START_FORWARD = 0x0B5;
        constexpr uint16_t START_BACKWARD = 0x0B6;
        constexpr uint16_t STOP = 0x0B7;
        constexpr uint16_t START_STRAFE_LEFT = 0x0B8;
        constexpr uint16_t START_STRAFE_RIGHT = 0x0B9;
        constexpr uint16_t STOP_STRAFE = 0x0BA;
        constexpr uint16_t JUMP = 0x0BB;
        constexpr uint16_t START_TURN_LEFT = 0x0BC;
        constexpr uint16_t START_TURN_RIGHT = 0x0BD;
        constexpr uint16_t STOP_TURN = 0x0BE;
        constexpr uint16_t START_PITCH_UP = 0x0BF;
        constexpr uint16_t START_PITCH_DOWN = 0x0C0;
        constexpr uint16_t STOP_PITCH = 0x0C1;
        constexpr uint16_t SET_RUN_MODE = 0x0C2;
        constexpr uint16_t SET_WALK_MODE = 0x0C3;
        constexpr uint16_t FALL_LAND = 0x0C9;
        constexpr uint16_t START_SWIM = 0x0CA;
        constexpr uint16_t STOP_SWIM = 0x0CB;
        constexpr uint16_t SET_FACING = 0x0DA;
        constexpr uint16_t SET_PITCH = 0x0DB;
        constexpr uint16_t HEARTBEAT = 0x0EE;
        constexpr uint16_t START_ASCEND = 0x359;
        constexpr uint16_t STOP_ASCEND = 0x35A;   // the client stops a descent with it too (no STOP_DESCEND in 3.3.5a)
        constexpr uint16_t START_DESCEND = 0x3A7;
    }

    /// The opcodes a client sends this tick for the change from `before` to `after` (MovementFlags of the body and its
    /// controls), with `jumped` / `landed` from the step. In the client's order of checks (fn 0x6ef860). Empty when
    /// nothing a client reports changed.
    [[nodiscard]] inline std::vector<uint16_t> Changes(uint32_t before, uint32_t after, bool jumped, bool landed)
    {
        std::vector<uint16_t> ops;
        auto changed = [before, after](uint32_t mask) { return (before & mask) != (after & mask); };

        if (changed(Flag::FORWARD | Flag::BACKWARD))
            ops.push_back((after & Flag::FORWARD) ? Op::START_FORWARD : (after & Flag::BACKWARD) ? Op::START_BACKWARD
                : Op::STOP);
        if (changed(Flag::STRAFE_LEFT | Flag::STRAFE_RIGHT))
            ops.push_back((after & Flag::STRAFE_LEFT) ? Op::START_STRAFE_LEFT
                : (after & Flag::STRAFE_RIGHT) ? Op::START_STRAFE_RIGHT : Op::STOP_STRAFE);
        if (changed(Flag::ASCENDING | Flag::DESCENDING))
            ops.push_back((after & Flag::ASCENDING) ? Op::START_ASCEND : (after & Flag::DESCENDING) ? Op::START_DESCEND
                : Op::STOP_ASCEND);
        if (jumped)
            ops.push_back(Op::JUMP);
        if (changed(Flag::LEFT | Flag::RIGHT))
            ops.push_back((after & Flag::LEFT) ? Op::START_TURN_LEFT : (after & Flag::RIGHT) ? Op::START_TURN_RIGHT
                : Op::STOP_TURN);
        if (changed(Flag::WALKING))
            ops.push_back((after & Flag::WALKING) ? Op::SET_WALK_MODE : Op::SET_RUN_MODE);
        if (changed(Flag::SWIMMING))
            ops.push_back((after & Flag::SWIMMING) ? Op::START_SWIM : Op::STOP_SWIM);
        if (changed(Flag::PITCH_UP | Flag::PITCH_DOWN))
            ops.push_back((after & Flag::PITCH_UP) ? Op::START_PITCH_UP : (after & Flag::PITCH_DOWN) ? Op::START_PITCH_DOWN
                : Op::STOP_PITCH);
        if (landed)
            ops.push_back(Op::FALL_LAND);
        return ops;
    }

    /// Whether a heartbeat is due at `nowMs`, the last movement packet having gone at `lastSendMs`.
    [[nodiscard]] inline bool HeartbeatDue(uint32_t flags, uint32_t nowMs, uint32_t lastSendMs)
    {
        return (flags & HEARTBEAT_FLAGS) != 0 && nowMs - lastSendMs >= HEARTBEAT_MS;
    }
}

#endif
