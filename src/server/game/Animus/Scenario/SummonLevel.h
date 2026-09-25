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

#ifndef ANIMUS_LIB_SUMMON_LEVEL_H
#define ANIMUS_LIB_SUMMON_LEVEL_H

#include "Define.h"

namespace Animus
{
    /// Level forced onto the next creature whose level is selected on this thread; 0 = no override.
    ///
    /// Set it immediately around a Map::SummonCreature call: summoning runs Creature::SelectLevel
    /// synchronously, where the OnBeforeCreatureSelectLevel hook applies it. Level drives the
    /// creature's base stats (health, armor) as well as the attack tables, which is why this is
    /// done at selection time rather than with SetLevel afterwards.
    inline thread_local uint8 PendingSummonLevel = 0;
}

#endif
