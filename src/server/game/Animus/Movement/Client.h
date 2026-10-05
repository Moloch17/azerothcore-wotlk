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

#ifndef ANIMUS_MOVEMENT_CLIENT_H
#define ANIMUS_MOVEMENT_CLIENT_H

#include "PlayerController.h"
#include "ClientOrders.h"
#include <cstdint>

/// **The controller as the client** (player-controller plan §5A, §5A.1): it moves the true body every tick under the
/// held controls, and tells the server about it as the 3.3.5a client does (ReportCadence) -- a change opcode when a
/// control or the mode changes, SET_FACING / SET_PITCH on the mouse-look's 0.1 rad rule, a heartbeat 500 ms after the
/// last packet while moving -- each at its exact moment inside the step, so the cadence does not depend on how long a
/// tick is. The server holds what was last reported, as for any player; the client yields to what the server imposes
/// (a root, a stun, a fear, a spline, a teleport) and answers the server's movement orders as a client does.
///
/// Pure: the server is a ServerLink, which the forge implements over ClientMovement::Apply (PlayerLink) and the realm
/// over its packet path, so one copy of this logic serves both; GTests use a fake (ClientTest).
namespace Animus::Movement
{
    /// One movement packet, as a client would send it (MovementInfo's fields, in the client's conventions).
    struct Report
    {
        uint16_t Opcode = 0;
        uint32_t TimeMs = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Yaw = 0.0f;
        float Pitch = 0.0f;
        uint32_t Flags = 0;
        uint32_t FallMs = 0;            // time since the fall began, while FALLING; 0 otherwise
        float JumpZSpeed = 0.0f;        // the client's: down positive (a jump's launch is -JUMP_SPEED)
        float JumpSin = 0.0f;
        float JumpCos = 1.0f;
        float JumpXYSpeed = 0.0f;
        // An acknowledgement's: the order's counter, a speed order's move type and value, whether a toggle applied.
        uint32_t Counter = 0;
        uint8_t MoveType = 0;
        float Speed = 0.0f;
        bool Applied = false;
    };

    /// What the server holds of the seat, read back after each report and every tick.
    struct ServerState
    {
        /// The server moves the unit itself or forbids it to move: rooted, stunned, confused, fleeing, a spline, a
        /// teleport under way, dead, charmed. The client yields: it stops, and takes the body from here.
        bool Imposed = false;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Yaw = 0.0f;
    };

    /// The server, to the client: apply a report (false: refused), and say what it holds.
    class ServerLink
    {
    public:
        virtual ~ServerLink() = default;
        virtual bool Apply(Report const& report) = 0;
        [[nodiscard]] virtual ServerState State() const = 0;
    };

    class Client
    {
    public:
        /// Further than this from what was last accepted, with nothing imposed: the server put the seat there (a
        /// teleport the client was not told of), and the client starts again from it.
        static constexpr float TELEPORT_YARDS = 5.0f;
        /// Flags the server's orders set and the client then carries (ClientOrders.h, FlagsAfter): flying, water
        /// walking, feather fall (FALLING_SLOW only under it: the core strips it otherwise), hover, no gravity.
        static constexpr uint32_t GRANTED_MASK = Flag::CAN_FLY | Flag::WATERWALKING | Flag::FALLING_SLOW | Flag::HOVER
            | Animus::Client::MoveFlag::DISABLE_GRAVITY;
        /// The flags a held control sets; a change of any of them is a packet at once.
        static constexpr uint32_t CONTROL_MASK = Flag::FORWARD | Flag::BACKWARD | Flag::STRAFE_LEFT | Flag::STRAFE_RIGHT
            | Flag::LEFT | Flag::RIGHT | Flag::PITCH_UP | Flag::PITCH_DOWN | Flag::ASCENDING | Flag::DESCENDING
            | Flag::WALKING;
        /// What a rooted client stops (the core's MOVEMENTFLAG_MASK_MOVING).
        static constexpr uint32_t MOVING_MASK = Animus::Client::MoveFlag::MASK_MOVING;

        struct Counters
        {
            uint64_t Reports = 0;           // packets sent, accepted or not
            uint64_t Refused = 0;           // ... refused by the server
            uint64_t Heartbeats = 0;
            uint64_t Facings = 0;           // SET_FACING and SET_PITCH
            uint64_t Changes = 0;           // change opcodes (start, stop, jump, land, swim, ...)
            uint64_t Acks = 0;              // answers to the server's orders
            uint64_t YieldTicks = 0;        // ticks the server imposed and the client yielded
            uint64_t Resyncs = 0;           // starts again from a position the server set on its own
        };

        /// The body as the client knows it: the truth, which self observations read (§5A.1 point 1).
        BodyState Body;
        Counters Counts;
        /// The last tick, over its sub-steps: yards moved, yards the held controls asked for, whether it met a wall;
        /// jumps taken and landings, and the highest fall landed.
        float TickMoved = 0.0f;
        float TickCommanded = 0.0f;
        bool TickWall = false;
        uint32_t TickJumps = 0;
        uint32_t TickLandings = 0;
        float TickFallHeight = 0.0f;

        /// Take the body where the server has it and report at once: an episode's start, after a teleport.
        void Start(ServerLink& link, Movement::Body const& shape, WorldQuery const& world, uint32_t nowMs);
        /// Answer one of the server's orders as a client does, in the order they were sent: take it (a root stops
        /// the body, a knockback launches it, a flag order is carried from then on) and acknowledge it with the flags
        /// the shared rule gives (Animus::Client::FlagsAfter, ClientOrders.h). A time sync is the realm's to answer.
        void Order(Animus::Client::Order const& order, ServerLink& link, Movement::Body const& shape,
            WorldQuery const& world, uint32_t nowMs);
        /// One world tick, ending at `nowMs`: step the body in sub-steps of at most MAX_SUBSTEP and report as the
        /// client would within them.
        void Tick(ControlState& control, Speeds const& speeds, Movement::Body const& shape, WorldQuery const& world,
            uint32_t diffMs, uint32_t nowMs, ServerLink& link);
        /// An episode's end: a last report, if the body is anywhere the server has not been told of, so the last
        /// stretch is credited (§5A.1 point 4).
        void Finish(ServerLink& link, uint32_t nowMs);

        [[nodiscard]] bool Started() const { return _started; }
        [[nodiscard]] bool Rooted() const { return _rooted; }
        [[nodiscard]] uint32_t Granted() const { return _granted; }
        [[nodiscard]] uint32_t LastReportMs() const { return _lastSendMs; }
        /// The flags the client reports for the body under `control` now.
        [[nodiscard]] uint32_t FlagsOf(ControlState const& control, Speeds const& speeds) const;

    private:
        [[nodiscard]] Report Snapshot(uint16_t opcode, uint32_t timeMs, uint32_t flags) const;
        bool Send(Report const& report, ServerLink& link);
        void TakeFromServer(ServerState const& state);

        Movement::Body const* _shape = nullptr;
        WorldQuery const* _world = nullptr;
        bool _started = false;
        bool _rooted = false;
        uint32_t _granted = 0;
        uint32_t _lastFlags = 0;
        uint32_t _lastSendMs = 0;
        float _reportedX = 0.0f;
        float _reportedY = 0.0f;
        float _reportedZ = 0.0f;
        float _reportedYaw = 0.0f;
        float _reportedPitch = 0.0f;
        // The fall under way's launch, carried by every report while FALLING (the client's jump info).
        float _jumpZSpeed = 0.0f;
        float _jumpSin = 0.0f;
        float _jumpCos = 1.0f;
        float _jumpXYSpeed = 0.0f;
    };
}

#endif
