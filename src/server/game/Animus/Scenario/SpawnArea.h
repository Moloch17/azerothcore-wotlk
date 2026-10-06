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

#ifndef ANIMUS_LIB_SPAWN_AREA_H
#define ANIMUS_LIB_SPAWN_AREA_H

class Map;
class Player;

namespace Animus::SpawnArea
{
    /// Remove every creature near the bot that a scenario did not spawn (the spawn point's own creatures).
    void Clear(Player* bot);

    /// Remove every creature a map spawns from the database within `radius` yards of `bot`, loading those grids first,
    /// so creatures further off do not appear later as the grids load. An instance a stage uses as empty ground (M1's
    /// Stockades) is cleared this way: Clear's 60 yards left the hallway's far half full of its mobs, which killed the
    /// level 1 seats (2026-10-05).
    void ClearMap(Player* bot, float radius);
}

#endif
