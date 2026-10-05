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

#ifndef ANIMUS_MOVEMENT_PLAYER_LINK_H
#define ANIMUS_MOVEMENT_PLAYER_LINK_H

#include "Client.h"
#include "LinkMemory.h"
#include "ClientOrders.h"
#include "Opcodes.h"
#include "ClientMovement.h"
#include "Define.h"
#include <array>
#include <atomic>

class Player;
struct MovementInfo;
class WorldPacket;

/// **The forge's server, to the player controller** (player-controller C4, §5A): a seat's reports are applied by the
/// server's own movement handling (ClientMovement::Apply, the core of WorldSession::HandleMovementOpcodes), its acks
/// as the ack handlers apply them, and what the server holds or imposes is read back off the Player. The realm has its
/// own ServerLink over the packet path; the client logic in front of both is the same (Client).
namespace Animus::Movement
{
    class PlayerLink final : public ServerLink
    {
    public:
        /// Refused for an invalid position this many times in a row, the seat is put back where the server last
        /// accepted it (a bug, logged loudly, never a kick).
        static constexpr uint32 UNSTICK_AFTER = 20;
        /// A refusal reason is logged at most once in this long (per reason, for every seat together).
        static constexpr uint32 REFUSAL_LOG_MS = 60000;

        PlayerLink(Player* bot, LinkMemory& memory) : _bot(bot), _memory(memory) { }

        bool Apply(Report const& report) override;
        [[nodiscard]] ServerState State() const override;

        /// Reports applied and refused, by reason, for every seat (the status line, C8).
        static inline std::atomic<uint64> Applied{ 0 };
        static inline std::array<std::atomic<uint64>, size_t(ClientMovement::Refusal::Count)> Refused{};
        static inline std::atomic<uint64> Unsticks{ 0 };
        /// The sim sessions' movement-order packets the hook kept (QueueOrder): its hits, for the status line (C8).
        static inline std::atomic<uint64> OrderPackets{ 0 };

        /// Reports relayed to watching clients (only while one is connected).
        static inline std::atomic<uint64> Relayed{ 0 };

    private:
        void Refuse(ClientMovement::Refusal refusal, Report const& report);
        /// Send what the report's handler sends the players around the mover (SendMessageToSet; a bot has no client
        /// of its own to leave out).
        void Relay(Report const& report, MovementInfo& info, Opcodes opcode);

        Player* _bot;
        LinkMemory& _memory;
    };

    /// Read one of the server's movement orders out of a packet sent to a sim session (WorldSession::SendPacket's
    /// filter has already picked the opcode) with the shared decoder (Animus::Client::Decode) and queue it in the
    /// session's inbox. Reads only; builds nothing. `self` is the session player's raw GUID.
    void QueueOrder(Animus::Client::Inbox& inbox, WorldPacket const& packet, uint64 self);
}

#endif
