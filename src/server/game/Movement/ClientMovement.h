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

#ifndef ACORE_CLIENT_MOVEMENT_H
#define ACORE_CLIENT_MOVEMENT_H

#include "Define.h"
#include "Opcodes.h"

class Player;
class Unit;
struct MovementInfo;

/// **A client's movement report, applied** (player-controller plan §5A). WorldSession::ProcessMovementInfo,
/// VerifyMovementInfo and HandleMoverRelocation, moved here unchanged except for what made them a session's: the
/// controlling player and the two things only a session has -- its clock sync and the kick -- are parameters. The
/// packet handlers still call them through WorldSession (same behaviour), and a sessionless bot driven by the
/// player controller calls them directly, so its moves go through the server's own movement rules: verification, the
/// anticheat hooks, fall damage, the landing interrupts, relocation (zone, area, grid, visibility) and OnPlayerMove.
/// The relay to watching clients stays the caller's.
namespace ClientMovement
{
    /// Why a report was not applied: each `return false` of the old VerifyMovementInfo, by name.
    enum class Refusal : uint8
    {
        None = 0,
        InvalidPosition,        // the position is not a valid one
        SplineInProgress,       // a core spline moves the unit (a charge, a knockback, a fear)
        DisableMove,            // UNIT_FLAG_DISABLE_MOVE and the report moves
        DoubleJump,             // AnticheatHandleDoubleJump refused it (the client is kicked)
        Anticheat,              // AnticheatCheckMovementInfo refused it (the client is kicked)
        TransportTeleported,    // on a transport, from far away: a packet from before a teleport
        TransportCoords,        // on a transport, at invalid coordinates
        Rooted,                 // rooted, and the report does not say so or moves
        Count
    };

    [[nodiscard]] char const* RefusalName(Refusal refusal);

    /// What only the reporting client has: the clock its report's time is synchronised to, and being kicked.
    class Client
    {
    public:
        virtual ~Client() = default;
        /// Turn the report's time into server time (WorldSession::SynchronizeMovement for a real client).
        virtual void Synchronize(MovementInfo& movementInfo) = 0;
        /// The anticheat hooks kick the mover's client (WorldSession::KickPlayer for a real one).
        virtual void Kick(Player* plrMover) = 0;
    };

    /// WorldSession::VerifyMovementInfo.
    [[nodiscard]] Refusal Verify(MovementInfo const& movementInfo, Player* plrMover, Unit* mover, Opcodes opcode,
        Client& client);
    /// WorldSession::ProcessMovementInfo: verify, then apply (fall damage, interrupts, OnPlayerMove, relocation,
    /// fall information). `controller` is the reporting session's player (WorldSession::_player).
    [[nodiscard]] Refusal Apply(Player* controller, MovementInfo& movementInfo, Unit* mover, Player* plrMover,
        Opcodes opcode, Client& client);
    /// WorldSession::HandleMoverRelocation.
    void Relocate(Player* controller, MovementInfo& movementInfo, Unit* mover, Client& client);
}

#endif
