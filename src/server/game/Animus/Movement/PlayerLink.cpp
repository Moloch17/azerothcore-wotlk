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

#include "PlayerLink.h"
#include "Forge.h"
#include "GameTime.h"
#include "Log.h"
#include "MoveSpline.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    using Animus::Movement::PlayerLink;

    /// A sessionless bot's side of ClientMovement: its reports are timed on the server's own clock (GameTime), and it
    /// is never kicked -- a report the anticheat would kick for is refused and logged.
    class BotMovementClient final : public ClientMovement::Client
    {
    public:
        void Synchronize(MovementInfo& movementInfo) override
        {
            movementInfo.time = uint32(GameTime::GetGameTimeMS().count());
        }

        void Kick(Player* plrMover) override
        {
            LOG_ERROR("module.animus", "Player controller: the anticheat would kick {}; a bot is not kicked, its "
                "report is refused", plrMover ? plrMover->GetName() : std::string("?"));
        }
    };

    /// When each refusal reason was last logged (game ms), for every seat together.
    std::array<std::atomic<uint64>, size_t(ClientMovement::Refusal::Count)> LastLogged{};

    /// A speed ack's move type (the handler's switch), or MAX_MOVE_TYPE for another opcode.
    UnitMoveType SpeedAckType(uint16 opcode)
    {
        switch (opcode)
        {
            case CMSG_FORCE_WALK_SPEED_CHANGE_ACK: return MOVE_WALK;
            case CMSG_FORCE_RUN_SPEED_CHANGE_ACK: return MOVE_RUN;
            case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK: return MOVE_RUN_BACK;
            case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK: return MOVE_SWIM;
            case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK: return MOVE_SWIM_BACK;
            case CMSG_FORCE_TURN_RATE_CHANGE_ACK: return MOVE_TURN_RATE;
            case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK: return MOVE_FLIGHT;
            case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK: return MOVE_FLIGHT_BACK;
            case CMSG_FORCE_PITCH_RATE_CHANGE_ACK: return MOVE_PITCH_RATE;
            default: return UnitMoveType(MAX_MOVE_TYPE);
        }
    }
}

bool Animus::Movement::PlayerLink::Apply(Report const& report)
{
    Player* bot = _bot;
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
        return false;

    MovementInfo info;
    info.guid = bot->GetGUID();
    info.flags = report.Flags;
    info.flags2 = 0;
    info.pos.Relocate(report.X, report.Y, report.Z, report.Yaw);
    info.transport.Reset();
    info.pitch = report.Pitch;
    info.fallTime = report.FallMs;
    info.jump.zspeed = report.JumpZSpeed;
    info.jump.sinAngle = report.JumpSin;
    info.jump.cosAngle = report.JumpCos;
    info.jump.xyspeed = report.JumpXYSpeed;
    // The flags a server strips from any client's report (ReadMovementInfo's rules, shared: FlagRules.h).
    bot->GetSession()->SanitizeMovementFlags(info);
    Opcodes const opcode = Opcodes(report.Opcode);

    // An ack, as its handler takes it before the movement it carries is applied.
    switch (opcode)
    {
        case MSG_MOVE_TELEPORT_ACK:
            // A near teleport waits for this (Player::TeleportTo sets the semaphore and sends MSG_MOVE_TELEPORT_ACK).
            // The sim's own teleports acknowledge themselves (BotFactory::TeleportWithinMap); a spell's (Blink, a
            // charge's leap, a summon) is acknowledged here, through the handler a client's ack goes to.
            if (bot->IsBeingTeleportedNear())
            {
                WorldPacket ack(MSG_MOVE_TELEPORT_ACK);
                ack << bot->GetPackGUID();
                ack << uint32(report.Counter) << uint32(0);
                bot->GetSession()->HandleMoveTeleportAck(ack);
            }
            return true;
        case CMSG_FORCE_MOVE_ROOT_ACK:
            if (bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_ROOT))
                return true;
            break;
        case CMSG_FORCE_MOVE_UNROOT_ACK:
            if (!bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_ROOT))
                return true;
            break;
        case CMSG_MOVE_HOVER_ACK:
        case CMSG_MOVE_FEATHER_FALL_ACK:
        case CMSG_MOVE_WATER_WALK_ACK:
        case CMSG_MOVE_SET_CAN_FLY_ACK:
        case CMSG_MOVE_GRAVITY_DISABLE_ACK:
        case CMSG_MOVE_GRAVITY_ENABLE_ACK:
            // HandleMovementFlagChangeToggleAck.
            sScriptMgr->AnticheatSetCanFlybyServer(bot, info.HasMovementFlag(MOVEMENTFLAG_CAN_FLY));
            bot->m_movementInfo.flags = info.GetMovementFlags();
            break;
        default:
            if (UnitMoveType const type = SpeedAckType(report.Opcode); type != UnitMoveType(MAX_MOVE_TYPE))
            {
                // HandleForceSpeedChangeAck's bookkeeping: one ack per order, so the count stays the orders unanswered.
                sScriptMgr->AnticheatSetUnderACKmount(bot);
                if (bot->m_forced_speed_changes[type] > 0)
                    --bot->m_forced_speed_changes[type];
            }
            break;
    }

    BotMovementClient client;
    uint32 const health = bot->GetHealth();
    bool const wasAlive = bot->IsAlive();
    ClientMovement::Refusal const refusal = ClientMovement::Apply(bot, info, bot, bot, opcode, client);
    if (refusal != ClientMovement::Refusal::None)
    {
        Refuse(refusal, report);
        return false;
    }

    if (opcode == CMSG_MOVE_SET_CAN_FLY_ACK && bot->GetPendingFlightChange() == report.Counter)
        bot->SetPendingFlightChange(false);
    if (opcode == CMSG_MOVE_KNOCK_BACK_ACK)
    {
        if (bot->IsFreeFlying())
            bot->SetCanFly(true);
        bot->SetCanTeleport(true);
    }

    // A landing's cost (Player::HandleFall, inside Apply), for the fall columns.
    if (opcode == MSG_MOVE_FALL_LAND)
    {
        ++_memory.Landings;
        if (bot->GetMaxHealth() && bot->GetHealth() < health)
            _memory.FallDamage += float(health - bot->GetHealth()) / float(bot->GetMaxHealth());
    }
    // A death in the movement itself is the fall's: a landing that killed, or the core's kill of a body under the
    // map's floor (ClientMovement::Apply: DAMAGE_FALL_TO_VOID, PLAYER_FLAGS_IS_OUT_OF_BOUNDS), which no landing
    // reports.
    if (wasAlive && !bot->IsAlive())
    {
        bool const fellOut = bot->HasPlayerFlag(PLAYER_FLAGS_IS_OUT_OF_BOUNDS);
        if (opcode == MSG_MOVE_FALL_LAND || fellOut)
            ++_memory.FallDeaths;
        if (fellOut)
            ++_memory.VoidDeaths;
    }

    // Watching clients see it as they see a player's (C5): the relay each handler makes, only while a real client
    // is connected (ForgeCore::HasClients) -- training builds nothing.
    if (ForgeCore::HasClients())
        Relay(report, info, opcode);

    ++Applied;
    _memory.GoodX = report.X;
    _memory.GoodY = report.Y;
    _memory.GoodZ = report.Z;
    _memory.GoodYaw = report.Yaw;
    _memory.HasGood = true;
    _memory.InvalidStreak = 0;
    return true;
}

void Animus::Movement::PlayerLink::Refuse(ClientMovement::Refusal refusal, Report const& report)
{
    size_t const index = size_t(refusal);
    ++Refused[index];
    uint64 const now = uint64(GameTime::GetGameTimeMS().count());
    uint64 last = LastLogged[index].load(std::memory_order_relaxed);
    if ((!last || now - last >= REFUSAL_LOG_MS) && LastLogged[index].compare_exchange_strong(last, now))
        LOG_WARN("module.animus", "Player controller: {}'s report 0x{:X} at ({:.2f}, {:.2f}, {:.2f}) refused: {} "
            "({} so far; logged at most once a minute)", _bot->GetName(), report.Opcode, report.X, report.Y, report.Z,
            ClientMovement::RefusalName(refusal), Refused[index].load(std::memory_order_relaxed));

    if (refusal != ClientMovement::Refusal::InvalidPosition)
        return;
    // An invalid position is a bug in the controller or the world query: never kicked in the sim, and after a streak
    // the seat is put back where the server last accepted it.
    if (++_memory.InvalidStreak >= UNSTICK_AFTER && _memory.HasGood)
    {
        LOG_ERROR("module.animus", "Player controller: {} refused for an invalid position {} times in a row; put back "
            "at ({:.2f}, {:.2f}, {:.2f})", _bot->GetName(), _memory.InvalidStreak, _memory.GoodX, _memory.GoodY,
            _memory.GoodZ);
        _memory.InvalidStreak = 0;
        ++Unsticks;
        _bot->NearTeleportTo(_memory.GoodX, _memory.GoodY, _memory.GoodZ, _memory.GoodYaw);
    }
}

void Animus::Movement::PlayerLink::Relay(Report const& report, MovementInfo& info, Opcodes opcode)
{
    Player* bot = _bot;
    WorldSession* session = bot->GetSession();
    Opcodes relay = opcode;
    switch (opcode)
    {
        case CMSG_FORCE_MOVE_ROOT_ACK: relay = MSG_MOVE_ROOT; break;                    // HandleMoveRootAck
        case CMSG_FORCE_MOVE_UNROOT_ACK: relay = MSG_MOVE_UNROOT; break;
        case CMSG_MOVE_KNOCK_BACK_ACK:                                                  // HandleMoveKnockBackAck
        {
            WorldPacket data(MSG_MOVE_KNOCK_BACK, 66);
            session->WriteMovementInfo(&data, &info);
            data << info.jump.sinAngle;
            data << info.jump.cosAngle;
            data << info.jump.xyspeed;
            data << info.jump.zspeed;
            bot->SendMessageToSet(&data, false);
            ++Relayed;
            return;
        }
        case CMSG_MOVE_HOVER_ACK: relay = MSG_MOVE_HOVER; break;                      // ...FlagChangeToggleAck
        case CMSG_MOVE_FEATHER_FALL_ACK: relay = MSG_MOVE_FEATHER_FALL; break;
        case CMSG_MOVE_WATER_WALK_ACK: relay = MSG_MOVE_WATER_WALK; break;
        case CMSG_MOVE_SET_CAN_FLY_ACK: relay = MSG_MOVE_UPDATE_CAN_FLY; break;
        case CMSG_MOVE_GRAVITY_DISABLE_ACK:
        case CMSG_MOVE_GRAVITY_ENABLE_ACK: relay = MSG_MOVE_GRAVITY_CHNG; break;
        default:
            if (UnitMoveType const type = SpeedAckType(report.Opcode); type != UnitMoveType(MAX_MOVE_TYPE))
            {
                // HandleForceSpeedChangeAck: MSG_MOVE_SET_*_SPEED with the new speed.
                WorldPacket data(SetSpeed2Opc_table[type][static_cast<size_t>(SpeedOpcodeIndex::ACK_RESPONSE)], 18);
                session->WriteMovementInfo(&data, &info);
                data << report.Speed;
                bot->SendMessageToSet(&data, false);
                ++Relayed;
                return;
            }
            break;          // a movement opcode: relayed as itself (HandleMovementOpcodes)
    }
    WorldPacket data(relay, 64);
    session->WriteMovementInfo(&data, &info);
    bot->SendMessageToSet(&data, false);
    ++Relayed;
}

Animus::Movement::ServerState Animus::Movement::PlayerLink::State() const
{
    ServerState state;
    Player const* bot = _bot;
    if (!bot)
    {
        state.Imposed = true;
        return state;
    }
    state.X = bot->GetPositionX();
    state.Y = bot->GetPositionY();
    state.Z = bot->GetPositionZ();
    state.Yaw = bot->GetOrientation();
    // What a client is made to yield to (a root, a stun, a fear or confuse, a spline, a teleport under way, a charm),
    // read off the unit every tick: the core sets these at once, while its root flag waits for the client's ack. A
    // corpse does not move; a ghost does.
    state.Imposed = !bot->IsInWorld() || bot->IsBeingTeleported()
        || bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING)
        || bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) || !bot->movespline->Finalized() || bot->IsCharmed()
        || (bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST));
    return state;
}

void Animus::Movement::QueueOrder(Animus::Client::Inbox& inbox, WorldPacket const& packet, uint64 self)
{
    ++PlayerLink::OrderPackets;
    if (std::optional<Animus::Client::Order> order = Animus::Client::Decode(packet.GetOpcode(), packet.contents(),
            packet.size(), self))
        inbox.Push(*order);
}
