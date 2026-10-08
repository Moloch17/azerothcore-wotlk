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

#include "UnitBody.h"
#include "Unit.h"
#include "SpellAuraDefines.h"

Animus::Movement::Speeds Animus::Movement::SpeedsOf(Unit const* unit)
{
    Speeds speeds;
    if (!unit)
        return speeds;
    speeds.Walk = unit->GetSpeed(MOVE_WALK);
    speeds.Run = unit->GetSpeed(MOVE_RUN);
    speeds.RunBack = unit->GetSpeed(MOVE_RUN_BACK);
    speeds.Swim = unit->GetSpeed(MOVE_SWIM);
    speeds.SwimBack = unit->GetSpeed(MOVE_SWIM_BACK);
    speeds.Flight = unit->GetSpeed(MOVE_FLIGHT);
    speeds.FlightBack = unit->GetSpeed(MOVE_FLIGHT_BACK);
    speeds.TurnRate = unit->GetSpeed(MOVE_TURN_RATE);
    speeds.PitchRate = unit->GetSpeed(MOVE_PITCH_RATE);
    speeds.CanFly = unit->CanFly();
    speeds.SlowFall = unit->HasAuraType(SPELL_AURA_FEATHER_FALL) || unit->HasAuraType(SPELL_AURA_HOVER);
    speeds.WaterWalk = unit->HasWaterWalkAura();
    return speeds;
}

Animus::Movement::Body Animus::Movement::ShapeOf(Unit const* unit)
{
    Body shape;
    if (!unit)
        return shape;
    shape.Radius = unit->GetCollisionRadius();
    shape.Height = unit->GetCollisionHeight();
    return shape;
}
