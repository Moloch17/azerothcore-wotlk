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

#include "SpawnArea.h"
#include "Creature.h"
#include "Log.h"
#include "Player.h"
#include <list>

namespace
{
    /// Creatures within this radius of the bot that the scenario did not spawn are removed.
    constexpr float SPAWN_AREA_CLEAR_RADIUS = 60.0f;
}

void Animus::SpawnArea::Clear(Player* bot)
{
    // alive = false: every creature in range, dead or alive.
    std::list<Creature*> creatures;
    bot->GetDeadCreatureListInGrid(creatures, SPAWN_AREA_CLEAR_RADIUS, false);

    for (Creature* creature : creatures)
        creature->DespawnOrUnsummon(0ms, Seconds(WEEK));

    if (!creatures.empty())
        LOG_WARN("module.animus", "Removed {} creatures from the spawn area around {}", creatures.size(),
            bot->GetName());
}
