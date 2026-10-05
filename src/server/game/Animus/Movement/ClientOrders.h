/*
 * This file is part of the Animus project, based on AzerothCore.
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

#ifndef ANIMUS_CLIENT_ORDERS_H
#define ANIMUS_CLIENT_ORDERS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

/// **The companion's client, the pure half** (player-controller realm port, R1): what the server tells a client about
/// its own movement and expects an answer to, read off the packets it sends, in the order it sent them; the clock the
/// client stamps its movement with; and the inbox those orders wait in until the companion's tick.
///
/// A real client is told of a root, a speed change, flight, water walking, feather fall, hover or a knockback by a
/// packet, changes its own movement state, and acknowledges with the counter the server sent. The stock core relies on
/// those acknowledgements (the relay to other players, m_forced_speed_changes, the can-fly and water-walk flags of a
/// client-controlled player are set only by the ack), so a companion -- a session with no socket -- answers them as a
/// client does. No core types here, so it is tested on its own (tests/ClientOrdersTest.cpp).
namespace Animus::Client
{
    /// The opcodes involved, the core's values (Server/Protocol/Opcodes.h), mirrored to stay core-free.
    namespace Op
    {
        // Server -> client.
        constexpr uint16_t MSG_MOVE_TELEPORT_ACK = 0x0C7;   // to the client: a near teleport waits for its answer
        constexpr uint16_t SMSG_MOVE_WATER_WALK = 0x0DE;
        constexpr uint16_t SMSG_MOVE_LAND_WALK = 0x0DF;
        constexpr uint16_t SMSG_FORCE_RUN_SPEED_CHANGE = 0x0E2;
        constexpr uint16_t SMSG_FORCE_RUN_BACK_SPEED_CHANGE = 0x0E4;
        constexpr uint16_t SMSG_FORCE_SWIM_SPEED_CHANGE = 0x0E6;
        constexpr uint16_t SMSG_FORCE_MOVE_ROOT = 0x0E8;
        constexpr uint16_t SMSG_FORCE_MOVE_UNROOT = 0x0EA;
        constexpr uint16_t SMSG_MOVE_KNOCK_BACK = 0x0EF;
        constexpr uint16_t SMSG_MOVE_FEATHER_FALL = 0x0F2;
        constexpr uint16_t SMSG_MOVE_NORMAL_FALL = 0x0F3;
        constexpr uint16_t SMSG_MOVE_SET_HOVER = 0x0F4;
        constexpr uint16_t SMSG_MOVE_UNSET_HOVER = 0x0F5;
        constexpr uint16_t SMSG_FORCE_WALK_SPEED_CHANGE = 0x2DA;
        constexpr uint16_t SMSG_FORCE_SWIM_BACK_SPEED_CHANGE = 0x2DC;
        constexpr uint16_t SMSG_FORCE_TURN_RATE_CHANGE = 0x2DE;
        constexpr uint16_t SMSG_MOVE_SET_CAN_FLY = 0x343;
        constexpr uint16_t SMSG_MOVE_UNSET_CAN_FLY = 0x344;
        constexpr uint16_t SMSG_FORCE_FLIGHT_SPEED_CHANGE = 0x381;
        constexpr uint16_t SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE = 0x383;
        constexpr uint16_t SMSG_TIME_SYNC_REQ = 0x390;
        constexpr uint16_t SMSG_FORCE_PITCH_RATE_CHANGE = 0x45C;
        constexpr uint16_t SMSG_MOVE_GRAVITY_DISABLE = 0x4CE;
        constexpr uint16_t SMSG_MOVE_GRAVITY_ENABLE = 0x4D0;

        // Client -> server.
        constexpr uint16_t CMSG_FORCE_RUN_SPEED_CHANGE_ACK = 0x0E3;
        constexpr uint16_t CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK = 0x0E5;
        constexpr uint16_t CMSG_FORCE_SWIM_SPEED_CHANGE_ACK = 0x0E7;
        constexpr uint16_t CMSG_FORCE_MOVE_ROOT_ACK = 0x0E9;
        constexpr uint16_t CMSG_FORCE_MOVE_UNROOT_ACK = 0x0EB;
        constexpr uint16_t CMSG_MOVE_KNOCK_BACK_ACK = 0x0F0;
        constexpr uint16_t CMSG_MOVE_HOVER_ACK = 0x0F6;
        constexpr uint16_t CMSG_MOVE_FEATHER_FALL_ACK = 0x2CF;
        constexpr uint16_t CMSG_MOVE_WATER_WALK_ACK = 0x2D0;
        constexpr uint16_t CMSG_FORCE_WALK_SPEED_CHANGE_ACK = 0x2DB;
        constexpr uint16_t CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK = 0x2DD;
        constexpr uint16_t CMSG_FORCE_TURN_RATE_CHANGE_ACK = 0x2DF;
        constexpr uint16_t CMSG_MOVE_SET_CAN_FLY_ACK = 0x345;
        constexpr uint16_t CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK = 0x382;
        constexpr uint16_t CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK = 0x384;
        constexpr uint16_t CMSG_TIME_SYNC_RESP = 0x391;
        constexpr uint16_t CMSG_FORCE_PITCH_RATE_CHANGE_ACK = 0x45D;
        constexpr uint16_t CMSG_MOVE_GRAVITY_DISABLE_ACK = 0x4CF;
        constexpr uint16_t CMSG_MOVE_GRAVITY_ENABLE_ACK = 0x4D1;
    }

    /// MovementInfo flag bits the acks touch, the core's values (Entities/Unit/UnitDefines.h).
    namespace MoveFlag
    {
        constexpr uint32_t DISABLE_GRAVITY = 0x00000400;
        constexpr uint32_t ROOT = 0x00000800;
        constexpr uint32_t FALLING = 0x00001000;
        constexpr uint32_t CAN_FLY = 0x01000000;
        constexpr uint32_t FLYING = 0x02000000;
        constexpr uint32_t WATERWALKING = 0x10000000;
        constexpr uint32_t FALLING_SLOW = 0x20000000;
        constexpr uint32_t HOVER = 0x40000000;
        /// MOVEMENTFLAG_MASK_MOVING: what a rooted client stops (the core's mask).
        constexpr uint32_t MASK_MOVING = 0x00000001 | 0x00000002 | 0x00000004 | 0x00000008 | 0x00000040 | 0x00000080
            | 0x00001000 | 0x00002000 | 0x00400000 | 0x00800000 | 0x04000000;
    }

    enum class OrderKind : uint8_t
    {
        TimeSync,       // SMSG_TIME_SYNC_REQ: answered with the client's clock
        Root,
        Unroot,
        Speed,          // one of the nine SMSG_FORCE_*_SPEED_CHANGE (SpeedType, Value)
        CanFly,
        UnsetCanFly,
        WaterWalk,
        LandWalk,
        FeatherFall,
        NormalFall,
        Hover,
        UnsetHover,
        GravityDisable,
        GravityEnable,
        Knockback,      // SMSG_MOVE_KNOCK_BACK (Cos, Sin, SpeedXY, SpeedZ)
        /// MSG_MOVE_TELEPORT_ACK to the client (Player::TeleportTo on the same map, SendTeleportAckPacket): the
        /// player has been put somewhere and the server waits for the same opcode back (HandleMoveTeleportAck)
        /// before it lets go of the near-teleport semaphore. The client takes its position from there.
        Teleport,
    };

    /// The core's UnitMoveType order (MOVE_WALK .. MOVE_PITCH_RATE), for a speed order.
    enum class SpeedType : uint8_t
    {
        Walk,
        Run,
        RunBack,
        Swim,
        SwimBack,
        TurnRate,
        Flight,
        FlightBack,
        PitchRate,
    };

    /// One thing the server told the client, as the client received it.
    struct Order
    {
        OrderKind Kind = OrderKind::TimeSync;
        uint16_t Opcode = 0;            // the server's opcode
        uint32_t Counter = 0;           // the movement counter (or the time-sync counter) to answer with
        SpeedType Speed = SpeedType::Run;
        float Value = 0.0f;             // the new speed
        /// A knockback as sent: the horizontal direction, its speed, and the vertical speed in the client's sign
        /// (negative is up: the server sends -speedZ).
        float Cos = 0.0f;
        float Sin = 0.0f;
        float SpeedXY = 0.0f;
        float SpeedZ = 0.0f;
        /// When it arrived: the client's own clock then (what a time-sync answer reports) and the steady time
        /// (the server's receive time of an answer sent at once).
        uint32_t ClientMs = 0;
        std::chrono::steady_clock::time_point ArrivedAt{};
    };

    /// Whether the server's opcode is one the client answers. Cheap: every packet to every session is asked.
    [[nodiscard]] constexpr bool Answers(uint16_t opcode)
    {
        switch (opcode)
        {
            case Op::SMSG_TIME_SYNC_REQ:
            case Op::SMSG_FORCE_MOVE_ROOT:
            case Op::SMSG_FORCE_MOVE_UNROOT:
            case Op::SMSG_FORCE_WALK_SPEED_CHANGE:
            case Op::SMSG_FORCE_RUN_SPEED_CHANGE:
            case Op::SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
            case Op::SMSG_FORCE_SWIM_SPEED_CHANGE:
            case Op::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE:
            case Op::SMSG_FORCE_TURN_RATE_CHANGE:
            case Op::SMSG_FORCE_FLIGHT_SPEED_CHANGE:
            case Op::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE:
            case Op::SMSG_FORCE_PITCH_RATE_CHANGE:
            case Op::SMSG_MOVE_SET_CAN_FLY:
            case Op::SMSG_MOVE_UNSET_CAN_FLY:
            case Op::SMSG_MOVE_WATER_WALK:
            case Op::SMSG_MOVE_LAND_WALK:
            case Op::SMSG_MOVE_FEATHER_FALL:
            case Op::SMSG_MOVE_NORMAL_FALL:
            case Op::SMSG_MOVE_SET_HOVER:
            case Op::SMSG_MOVE_UNSET_HOVER:
            case Op::SMSG_MOVE_GRAVITY_DISABLE:
            case Op::SMSG_MOVE_GRAVITY_ENABLE:
            case Op::SMSG_MOVE_KNOCK_BACK:
            case Op::MSG_MOVE_TELEPORT_ACK:
                return true;
            default:
                return false;
        }
    }

    namespace Detail
    {
        /// Reads a packet's bytes front to back; any read past the end fails the whole decode.
        class Reader
        {
        public:
            Reader(uint8_t const* data, std::size_t size) : _data(data), _size(size) { }

            template <typename T>
            bool Read(T& out)
            {
                if (!_data || _pos + sizeof(T) > _size)
                    return false;
                std::memcpy(&out, _data + _pos, sizeof(T));     // little-endian, as ByteBuffer writes on x86
                _pos += sizeof(T);
                return true;
            }

            /// A packed GUID (ObjectGuid::WriteAsPacked): a mask byte, then one byte for each set bit.
            bool ReadPackedGuid(uint64_t& out)
            {
                uint8_t mask = 0;
                if (!Read(mask))
                    return false;
                out = 0;
                for (int i = 0; i < 8; ++i)
                {
                    if (!(mask & (1 << i)))
                        continue;
                    uint8_t byte = 0;
                    if (!Read(byte))
                        return false;
                    out |= uint64_t(byte) << (i * 8);
                }
                return true;
            }

        private:
            uint8_t const* _data;
            std::size_t _size;
            std::size_t _pos = 0;
        };
    }

    /// The order a server packet carries, as the stock core writes it (Unit::SendSpeedToController, SendMoveRoot,
    /// SetCanFly / SetWaterWalking / SetFeatherFall / SetHover / SetDisableGravity, KnockbackFrom,
    /// WorldSession::SendTimeSync). Nothing for an opcode the client does not answer, a malformed packet, or one
    /// about another unit than `self` (the packed GUID's raw value, ObjectGuid::GetRawValue).
    [[nodiscard]] inline std::optional<Order> Decode(uint16_t opcode, uint8_t const* data, std::size_t size,
        uint64_t self)
    {
        if (!Answers(opcode))
            return std::nullopt;

        Detail::Reader in(data, size);
        Order order;
        order.Opcode = opcode;
        if (opcode == Op::SMSG_TIME_SYNC_REQ)
        {
            order.Kind = OrderKind::TimeSync;
            return in.Read(order.Counter) ? std::optional<Order>(order) : std::nullopt;
        }

        uint64_t guid = 0;
        if (!in.ReadPackedGuid(guid) || guid != self || !in.Read(order.Counter))
            return std::nullopt;

        auto speed = [&](SpeedType type, bool runFlag) -> std::optional<Order>
        {
            order.Kind = OrderKind::Speed;
            order.Speed = type;
            uint8_t unused = 0;
            if (runFlag && !in.Read(unused))
                return std::nullopt;
            return in.Read(order.Value) ? std::optional<Order>(order) : std::nullopt;
        };
        auto plain = [&](OrderKind kind) -> std::optional<Order>
        {
            order.Kind = kind;
            return order;
        };

        switch (opcode)
        {
            case Op::SMSG_FORCE_MOVE_ROOT: return plain(OrderKind::Root);
            case Op::SMSG_FORCE_MOVE_UNROOT: return plain(OrderKind::Unroot);
            case Op::SMSG_FORCE_WALK_SPEED_CHANGE: return speed(SpeedType::Walk, false);
            case Op::SMSG_FORCE_RUN_SPEED_CHANGE: return speed(SpeedType::Run, true);   // uint8(0) after the counter
            case Op::SMSG_FORCE_RUN_BACK_SPEED_CHANGE: return speed(SpeedType::RunBack, false);
            case Op::SMSG_FORCE_SWIM_SPEED_CHANGE: return speed(SpeedType::Swim, false);
            case Op::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE: return speed(SpeedType::SwimBack, false);
            case Op::SMSG_FORCE_TURN_RATE_CHANGE: return speed(SpeedType::TurnRate, false);
            case Op::SMSG_FORCE_FLIGHT_SPEED_CHANGE: return speed(SpeedType::Flight, false);
            case Op::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE: return speed(SpeedType::FlightBack, false);
            case Op::SMSG_FORCE_PITCH_RATE_CHANGE: return speed(SpeedType::PitchRate, false);
            case Op::SMSG_MOVE_SET_CAN_FLY: return plain(OrderKind::CanFly);
            case Op::SMSG_MOVE_UNSET_CAN_FLY: return plain(OrderKind::UnsetCanFly);
            case Op::SMSG_MOVE_WATER_WALK: return plain(OrderKind::WaterWalk);
            case Op::SMSG_MOVE_LAND_WALK: return plain(OrderKind::LandWalk);
            case Op::SMSG_MOVE_FEATHER_FALL: return plain(OrderKind::FeatherFall);
            case Op::SMSG_MOVE_NORMAL_FALL: return plain(OrderKind::NormalFall);
            case Op::SMSG_MOVE_SET_HOVER: return plain(OrderKind::Hover);
            case Op::SMSG_MOVE_UNSET_HOVER: return plain(OrderKind::UnsetHover);
            case Op::SMSG_MOVE_GRAVITY_DISABLE: return plain(OrderKind::GravityDisable);
            case Op::SMSG_MOVE_GRAVITY_ENABLE: return plain(OrderKind::GravityEnable);
            case Op::MSG_MOVE_TELEPORT_ACK: return plain(OrderKind::Teleport);    // its MovementInfo is not read
            case Op::SMSG_MOVE_KNOCK_BACK:
                order.Kind = OrderKind::Knockback;
                if (!in.Read(order.Cos) || !in.Read(order.Sin) || !in.Read(order.SpeedXY) || !in.Read(order.SpeedZ))
                    return std::nullopt;
                return order;
            default:
                return std::nullopt;
        }
    }

    /// The opcode the client answers an order with.
    [[nodiscard]] constexpr uint16_t AckOpcode(Order const& order)
    {
        switch (order.Kind)
        {
            case OrderKind::TimeSync: return Op::CMSG_TIME_SYNC_RESP;
            case OrderKind::Root: return Op::CMSG_FORCE_MOVE_ROOT_ACK;
            case OrderKind::Unroot: return Op::CMSG_FORCE_MOVE_UNROOT_ACK;
            case OrderKind::CanFly:
            case OrderKind::UnsetCanFly: return Op::CMSG_MOVE_SET_CAN_FLY_ACK;
            case OrderKind::WaterWalk:
            case OrderKind::LandWalk: return Op::CMSG_MOVE_WATER_WALK_ACK;
            case OrderKind::FeatherFall:
            case OrderKind::NormalFall: return Op::CMSG_MOVE_FEATHER_FALL_ACK;
            case OrderKind::Hover:
            case OrderKind::UnsetHover: return Op::CMSG_MOVE_HOVER_ACK;
            case OrderKind::GravityDisable: return Op::CMSG_MOVE_GRAVITY_DISABLE_ACK;
            case OrderKind::GravityEnable: return Op::CMSG_MOVE_GRAVITY_ENABLE_ACK;
            case OrderKind::Knockback: return Op::CMSG_MOVE_KNOCK_BACK_ACK;
            case OrderKind::Teleport: return Op::MSG_MOVE_TELEPORT_ACK;
            case OrderKind::Speed:
                switch (order.Speed)
                {
                    case SpeedType::Walk: return Op::CMSG_FORCE_WALK_SPEED_CHANGE_ACK;
                    case SpeedType::Run: return Op::CMSG_FORCE_RUN_SPEED_CHANGE_ACK;
                    case SpeedType::RunBack: return Op::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK;
                    case SpeedType::Swim: return Op::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK;
                    case SpeedType::SwimBack: return Op::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK;
                    case SpeedType::TurnRate: return Op::CMSG_FORCE_TURN_RATE_CHANGE_ACK;
                    case SpeedType::Flight: return Op::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK;
                    case SpeedType::FlightBack: return Op::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK;
                    case SpeedType::PitchRate: return Op::CMSG_FORCE_PITCH_RATE_CHANGE_ACK;
                }
                return 0;
        }
        return 0;
    }

    /// The flag acks (HandleMoveFlagChangeOpcode) end with a uint32 "is applied", except the gravity ones.
    [[nodiscard]] constexpr bool AckCarriesApplied(OrderKind kind)
    {
        switch (kind)
        {
            case OrderKind::CanFly:
            case OrderKind::UnsetCanFly:
            case OrderKind::WaterWalk:
            case OrderKind::LandWalk:
            case OrderKind::FeatherFall:
            case OrderKind::NormalFall:
            case OrderKind::Hover:
            case OrderKind::UnsetHover:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] constexpr bool Applies(OrderKind kind)
    {
        return kind == OrderKind::CanFly || kind == OrderKind::WaterWalk || kind == OrderKind::FeatherFall
            || kind == OrderKind::Hover;
    }

    /// The client's movement flags after it takes an order -- what its ack (and every later report) carries.
    /// A root stops every moving key and keeps the turn bits (the core's SendMoveRoot rule: ROOT never alongside
    /// MASK_MOVING); a knockback is a fall; the state orders set or clear their one flag; unset-can-fly also ends
    /// flight; a mover that can fly or has no gravity is never falling (the stock ReadMovementInfo strips FALLING
    /// beside CAN_FLY or DISABLE_GRAVITY). Speeds and time sync change no flag.
    ///
    /// The realm half of the ack logic: it moves into the bundled, shared client code once the forge's C4 defines
    /// the applier interface, so both sides take an order the same way.
    [[nodiscard]] constexpr uint32_t FlagsAfter(Order const& order, uint32_t flags)
    {
        switch (order.Kind)
        {
            case OrderKind::Root: return (flags & ~MoveFlag::MASK_MOVING) | MoveFlag::ROOT;
            case OrderKind::Unroot: return flags & ~MoveFlag::ROOT;
            case OrderKind::CanFly: return (flags & ~MoveFlag::FALLING) | MoveFlag::CAN_FLY;
            case OrderKind::UnsetCanFly: return flags & ~(MoveFlag::CAN_FLY | MoveFlag::FLYING);
            case OrderKind::WaterWalk: return flags | MoveFlag::WATERWALKING;
            case OrderKind::LandWalk: return flags & ~MoveFlag::WATERWALKING;
            case OrderKind::FeatherFall: return flags | MoveFlag::FALLING_SLOW;
            case OrderKind::NormalFall: return flags & ~MoveFlag::FALLING_SLOW;
            case OrderKind::Hover: return flags | MoveFlag::HOVER;
            case OrderKind::UnsetHover: return flags & ~MoveFlag::HOVER;
            case OrderKind::GravityDisable: return (flags & ~MoveFlag::FALLING) | MoveFlag::DISABLE_GRAVITY;
            case OrderKind::GravityEnable: return flags & ~MoveFlag::DISABLE_GRAVITY;
            case OrderKind::Knockback: return (flags & ~MoveFlag::ROOT) | MoveFlag::FALLING;
            default: return flags;
        }
    }

    /// The client's clock: milliseconds of its own steady clock, as GetTickCount is on a real one, starting at `base`
    /// (CLOCK_BASE by default) rather than 0, so a time is never 0 and never wraps soon after it starts. It is not the
    /// server's getMSTime: an offset subtracted from that would underflow in the server's first second. The core
    /// keeps a time-sync delta of 0 as "never synced" and only replaces it by one more than 25 ms away
    /// (ComputeNewClockDelta),
    /// so a companion's client is started a second ahead of the server's clock (CompanionClient) and its delta is
    /// always about -1000.
    class Clock
    {
    public:
        static constexpr uint32_t CLOCK_BASE = 1000000;

        explicit Clock(std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now(),
            uint32_t base = CLOCK_BASE) : _origin(origin), _base(base ? base : CLOCK_BASE) { }

        [[nodiscard]] uint32_t At(std::chrono::steady_clock::time_point when) const
        {
            auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(when - _origin).count();
            return _base + uint32_t(ms < 0 ? 0 : ms);
        }

        [[nodiscard]] uint32_t Now() const { return At(std::chrono::steady_clock::now()); }

    private:
        std::chrono::steady_clock::time_point _origin;
        uint32_t _base;
    };

    /// The orders waiting for the companion's tick: the "network" between the server's send and the client's
    /// handling. Pushed from whatever thread sends the packet (OnPacketSent: the world thread, map threads), drained
    /// by the companion's own tick. A mutex per companion, not a lock-free queue: a companion gets a handful of orders
    /// a minute, the lock is held for one push_back or one swap, and FIFO order across producers is exactly the order
    /// the pushes took the lock in -- which is the order the server sent them, since each is pushed inside its send.
    class Inbox
    {
    public:
        void Push(Order const& order)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _orders.push_back(order);
        }

        /// Everything waiting, oldest first, into `out` (cleared first); the inbox is left empty.
        void Drain(std::vector<Order>& out)
        {
            out.clear();
            std::lock_guard<std::mutex> lock(_mutex);
            out.swap(_orders);
        }

    private:
        std::mutex _mutex;
        std::vector<Order> _orders;
    };
}

#endif
