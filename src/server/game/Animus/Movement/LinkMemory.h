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

#ifndef ANIMUS_MOVEMENT_LINK_MEMORY_H
#define ANIMUS_MOVEMENT_LINK_MEMORY_H

#include <cstdint>

namespace Animus::Movement
{
    /// What a seat's link remembers between ticks: the last position the server accepted (where an unstick goes), and
    /// how many reports in a row were refused for an invalid position.
    struct LinkMemory
    {
        float GoodX = 0.0f;
        float GoodY = 0.0f;
        float GoodZ = 0.0f;
        float GoodYaw = 0.0f;
        bool HasGood = false;
        uint32_t InvalidStreak = 0;
        /// Landings the server took (MSG_MOVE_FALL_LAND), what they cost (share of maximum health, Player::HandleFall)
        /// and how many killed; for the fall columns, counted since the link began.
        uint32_t Landings = 0;
        float FallDamage = 0.0f;
        uint32_t FallDeaths = 0;
        uint32_t VoidDeaths = 0;        // ... of which the core's kill under the map's floor (DAMAGE_FALL_TO_VOID)
    };
}

#endif
