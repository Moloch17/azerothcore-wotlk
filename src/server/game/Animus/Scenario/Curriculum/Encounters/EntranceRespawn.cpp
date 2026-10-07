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

#include "EntranceRespawn.h"
#include "BotFactory.h"
#include "Player.h"
#include "StageScenario.h"
#include "StageState.h"

bool Animus::Curriculum::RiseAtEntrance(Player* bot, SeatState& seat, Position const& entrance, uint32 nowMs)
{
    // What it saw is what it saw before it died, somewhere else: nothing is in view at the entrance until its camera
    // casts a frame there (dungeon-curriculum I3 -- the enemy list is the frame's, never the server's).
    ForgetFrame(seat);
    if (!bot || !bot->IsInWorld())
        return false;

    // Alive and whole where it lies, out of any fight -- no resurrection sickness, no corpse: it never released.
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f, false);
    bot->SetFullHealth();
    for (Powers power : { POWER_MANA, POWER_ENERGY })
        if (bot->GetMaxPower(power))
            bot->SetPower(power, bot->GetMaxPower(power));
    bot->CombatStopWithPets(true);

    // A client that stands up again holds no keys: the ones held when it died would walk it on at once.
    float const faceTurn = seat.Controls.Held.FaceTurnApplied;
    seat.Controls.Held = Movement::ControlState();
    seat.Controls.Held.FaceTurnApplied = faceTurn;

    bool const moved = BotFactory::TeleportWithinMap(bot, entrance);
    // The controller takes its body from where the server has it now, as at an episode's start; the Teleport order
    // the move queued is drained next tick and only takes the same body again.
    StageScenario::StartMover(seat, bot, nowMs);
    return moved;
}

void Animus::Curriculum::ForgetFrame(SeatState& seat)
{
    seat.Seen = Vision::SeenList();
    seat.SightGuids.fill(0);
}
