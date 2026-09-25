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

#ifndef ANIMUS_WARM_CACHES_H
#define ANIMUS_WARM_CACHES_H

namespace Animus::Curriculum
{
    /// Touch every table the curriculum reads from the world database on first use (consumables, gear sources,
    /// enchant groups, opponents, world creatures, quests, and each class's kit, talents and catalog), so that
    /// all of it is in memory before the database pools are sealed and no episode ever queries.
    void WarmCaches();

    /// GearBuilder.cpp's part of the above: its file-local enchant groups and the gear stats tables.
    void WarmGearCaches();
}

#endif
