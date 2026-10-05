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

#ifndef ANIMUS_MOVEMENT_UNIT_BODY_H
#define ANIMUS_MOVEMENT_UNIT_BODY_H

#include "PlayerController.h"

class Unit;

/// The player controller's view of a core Unit (player-controller plan §2.1): the speeds in force -- mount, form,
/// snare and buffs included, read every time -- and the collision body. The one place the controller's pure types
/// are filled from the core.
namespace Animus::Movement
{
    /// The unit's own speeds now (Unit::GetSpeed per move type), whether it can fly (a flying mount or form in a flyable
    /// zone: MOVEMENTFLAG_CAN_FLY), fall slowly (feather fall, slow fall, hover) or walk on water.
    [[nodiscard]] Speeds SpeedsOf(Unit const* unit);
    /// Its collision cylinder (Unit::GetCollisionRadius / GetCollisionHeight).
    [[nodiscard]] Body ShapeOf(Unit const* unit);
}

#endif
