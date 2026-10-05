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

#include "ClientMovement.h"
#include "AreaDefines.h"
#include "Battleground.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraDefines.h"
#include "Transport.h"
#include "Vehicle.h"

char const* ClientMovement::RefusalName(Refusal refusal)
{
    switch (refusal)
    {
        case Refusal::None: return "none";
        case Refusal::InvalidPosition: return "invalid position";
        case Refusal::SplineInProgress: return "a core spline in progress";
        case Refusal::DisableMove: return "movement disabled";
        case Refusal::DoubleJump: return "double jump (anticheat)";
        case Refusal::Anticheat: return "anticheat";
        case Refusal::TransportTeleported: return "transport packet from before a teleport";
        case Refusal::TransportCoords: return "transport coordinates invalid";
        case Refusal::Rooted: return "rooted";
        default: return "?";
    }
}

ClientMovement::Refusal ClientMovement::Verify(MovementInfo const& movementInfo, Player* plrMover, Unit* mover,
    Opcodes opcode, Client& client)
{
    if (!movementInfo.pos.IsPositionValid())
    {
        if (plrMover)
        {
            sScriptMgr->AnticheatUpdateMovementInfo(plrMover, movementInfo);
        }

        return Refusal::InvalidPosition;
    }

    if (!mover->movespline->Finalized())
    {
        if (!mover->movespline->isBoarding() || (opcode != CMSG_FORCE_MOVE_UNROOT_ACK && opcode != CMSG_FORCE_MOVE_ROOT_ACK))
            return Refusal::SplineInProgress;
    }

    // Xinef: do not allow to move with UNIT_FLAG_DISABLE_MOVE
    if (mover->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
    {
        // Xinef: skip moving packets
        if (movementInfo.HasMovementFlag(MOVEMENTFLAG_MASK_MOVING))
        {
            if (plrMover)
            {
                sScriptMgr->AnticheatUpdateMovementInfo(plrMover, movementInfo);
            }
            return Refusal::DisableMove;
        }
    }

    bool jumpopcode = false;
    if (opcode == MSG_MOVE_JUMP)
    {
        jumpopcode = true;
        if (plrMover && !sScriptMgr->AnticheatHandleDoubleJump(plrMover, mover))
        {
            client.Kick(plrMover);
            return Refusal::DoubleJump;
        }
    }

    /* start some hack detection */
    if (plrMover && !sScriptMgr->AnticheatCheckMovementInfo(plrMover, movementInfo, mover, jumpopcode))
    {
        client.Kick(plrMover);
        return Refusal::Anticheat;
    }

    if (movementInfo.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
    {
        // We were teleported, skip packets that were broadcast before teleport
        if (movementInfo.pos.GetExactDist2d(mover) > SIZE_OF_GRIDS)
        {
            if (plrMover)
            {
                sScriptMgr->AnticheatUpdateMovementInfo(plrMover, movementInfo);
                //LOG_INFO("anticheat", "MovementHandler:: 2 We were teleported, skip packets that were broadcast before teleport");
            }
            return Refusal::TransportTeleported;
        }

        if (!Acore::IsValidMapCoord(movementInfo.pos.GetPositionX() + movementInfo.transport.pos.GetPositionX(), movementInfo.pos.GetPositionY() + movementInfo.transport.pos.GetPositionY(),
            movementInfo.pos.GetPositionZ() + movementInfo.transport.pos.GetPositionZ(), movementInfo.pos.GetOrientation() + movementInfo.transport.pos.GetOrientation()))
        {
            if (plrMover)
            {
                sScriptMgr->AnticheatUpdateMovementInfo(plrMover, movementInfo);
            }

            return Refusal::TransportCoords;
        }
    }

    // rooted mover sent packet without root or moving AND root - ignore, due to client crash possibility
    if (opcode != CMSG_FORCE_MOVE_UNROOT_ACK)
        if (mover->IsRooted() && (!movementInfo.HasMovementFlag(MOVEMENTFLAG_ROOT) || movementInfo.HasMovementFlag(MOVEMENTFLAG_MASK_MOVING)))
            return Refusal::Rooted;

    return Refusal::None;
}

ClientMovement::Refusal ClientMovement::Apply(Player* controller, MovementInfo& movementInfo, Unit* mover,
    Player* plrMover, Opcodes opcode, Client& client)
{
    if (Refusal const refusal = Verify(movementInfo, plrMover, mover, opcode, client); refusal != Refusal::None)
        return refusal;

    if (mover->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) || (mover->IsCreature() && mover->IsImmobilizedState()))
    {
        movementInfo.pos.Relocate(mover->GetPositionX(), mover->GetPositionY(), mover->GetPositionZ());

        if (mover->IsCreature())
        {
            movementInfo.transport.guid = mover->m_movementInfo.transport.guid;
            movementInfo.transport.pos.Relocate(mover->m_movementInfo.transport.pos.GetPositionX(), mover->m_movementInfo.transport.pos.GetPositionY(), mover->m_movementInfo.transport.pos.GetPositionZ());
            movementInfo.transport.seat = mover->m_movementInfo.transport.seat;
        }
    }

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == MSG_MOVE_FALL_LAND && plrMover && !plrMover->IsInFlight())
    {
        plrMover->HandleFall(movementInfo);

        sScriptMgr->AnticheatSetJumpingbyOpcode(plrMover, false);
    }

    // interrupt parachutes upon falling or landing in water
    if (opcode == MSG_MOVE_FALL_LAND || opcode == MSG_MOVE_START_SWIM)
    {
        mover->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_LANDING); // Parachutes

        if (plrMover)
        {
            sScriptMgr->AnticheatSetJumpingbyOpcode(plrMover, false);
        }
    }

    if (plrMover && ((movementInfo.flags & MOVEMENTFLAG_SWIMMING) != 0) != plrMover->IsInWater())
    {
        // now client not include swimming flag in case jumping under water
        plrMover->SetInWater(!plrMover->IsInWater() || plrMover->GetMap()->IsUnderWater(plrMover->GetPhaseMask(), movementInfo.pos.GetPositionX(),
            movementInfo.pos.GetPositionY(), movementInfo.pos.GetPositionZ(), plrMover->GetCollisionHeight()));
    }

    if (plrMover)//Hook for OnPlayerMove
    {
        sScriptMgr->OnPlayerMove(plrMover, movementInfo, opcode);
    }

    if (movementInfo.GetMovementFlags() & MOVEMENTFLAG_MASK_MOVING_OR_TURN)
    {
        if (mover->IsStandState())
            mover->SetStandState(UNIT_STAND_STATE_STAND);
        mover->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
    }

    Relocate(controller, movementInfo, mover, client);

    if (plrMover && opcode != CMSG_MOVE_KNOCK_BACK_ACK)
        plrMover->UpdateFallInformationIfNeed(movementInfo, opcode);

    return Refusal::None;
}

void ClientMovement::Relocate(Player* controller, MovementInfo& movementInfo, Unit* mover, Client& client)
{
    client.Synchronize(movementInfo);

    mover->UpdatePosition(movementInfo.pos);
    mover->m_movementInfo = movementInfo;

    if (mover->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
    {
        // if we boarded a transport, add us to it (generalized for both players and creatures)
        if (!mover->GetTransport())
        {
            if (Transport* transport = mover->GetMap()->GetTransport(movementInfo.transport.guid))
            {
                mover->SetTransport(transport);
                transport->AddPassenger(mover);
            }
        }
        else if (mover->GetTransport()->GetGUID() != movementInfo.transport.guid)
        {
            // Switching transports
            bool foundNewTransport = false;
            mover->GetTransport()->RemovePassenger(mover);
            if (Transport* transport = mover->GetMap()->GetTransport(movementInfo.transport.guid))
            {
                foundNewTransport = true;
                mover->SetTransport(transport);
                transport->AddPassenger(mover);
            }

            if (!foundNewTransport)
            {
                mover->SetTransport(nullptr);
                movementInfo.transport.Reset();
            }
        }

        if (!mover->GetTransport() && !mover->GetVehicle())
        {
            GameObject* go = mover->GetMap()->GetGameObject(movementInfo.transport.guid);
            if (!go || go->GetGoType() != GAMEOBJECT_TYPE_TRANSPORT)
                movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        }
    }
    else
    {
        // if we were on a transport, leave (handles both players and creatures)
        if (Transport* transport = mover->GetTransport())
        {
            if (mover->IsPlayer())
                sScriptMgr->AnticheatSetUnderACKmount(mover->ToPlayer()); // just for safe

            transport->RemovePassenger(mover);
            mover->SetTransport(nullptr);
            movementInfo.transport.Reset();
        }
    }

    // Some vehicles allow the passenger to turn by himself
    if (Vehicle* vehicle = mover->GetVehicle())
    {
        if (VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(mover))
        {
            if (seat->m_flags & VEHICLE_SEAT_FLAG_ALLOW_TURNING && movementInfo.pos.GetOrientation() != mover->GetOrientation())
            {
                mover->SetOrientation(movementInfo.pos.GetOrientation());
                mover->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TURNING);
            }
        }
    }

    if (Player* plrMover = mover->ToPlayer()) // nothing is charmed, or player charmed
    {
        if (plrMover->IsSitState() && (movementInfo.flags & (MOVEMENTFLAG_MASK_MOVING | MOVEMENTFLAG_MASK_TURNING)))
            plrMover->SetStandState(UNIT_STAND_STATE_STAND);

        if (movementInfo.pos.GetPositionZ() < plrMover->GetMap()->GetMinHeight(movementInfo.pos.GetPositionX(), movementInfo.pos.GetPositionY()))
        {
            if (!plrMover->GetBattleground() || !plrMover->GetBattleground()->HandlePlayerUnderMap(controller))
            {
                if (plrMover->IsAlive())
                {
                    // The Oculus under map case is handled by areatrigger (5001) and should not kill the player
                    if (plrMover->GetMapId() == MAP_THE_OCULUS)
                        return;

                    plrMover->SetPlayerFlag(PLAYER_FLAGS_IS_OUT_OF_BOUNDS);
                    plrMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, controller->GetMaxHealth());
                    // player can be alive if GM
                    if (plrMover->IsAlive())
                        plrMover->KillPlayer();
                }
                // Rescue only released ghosts: teleporting an unreleased body would move the corpse
                // out of instances (e.g. Eye of Eternity platform destruction, issue #25757).
                else if (plrMover->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !plrMover->HasPlayerFlag(PLAYER_FLAGS_IS_OUT_OF_BOUNDS))
                    plrMover->RepopAtGraveyard();
            }
        }
    }
}
