/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
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

#include "WarmCaches.h"
#include "ClassAssets.h"
#include "ClassProfile.h"
#include "LifeWorld.h"
#include "Log.h"
#include "Opponents.h"
#include "Supplies.h"
#include "Timer.h"
#include "WorldCreatures.h"

void Animus::Curriculum::WarmCaches()
{
    uint32 const started = getMSTime();

    ConsumablePool::Instance();
    WarmGearCaches();
    Opponents::OpponentPool::Instance();
    WorldCreatures::SpawnedIds();
    WorldCreatures::WaypointWalkerIds();
    LifeWorld::QuestSet::Instance();

    for (ClassProfile const& profile : ClassProfiles())
        ClassAssets::For(profile);

    LOG_INFO("module.animus", "Curriculum caches warmed in {} ms: the world database is not read again", GetMSTimeDiffToNow(started));
}
