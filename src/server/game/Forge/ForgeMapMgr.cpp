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

/*
 * The sim host's map tick: MapMgr::ForgeUpdate and MapInstanced::ForgeUpdate.
 *
 * Each is called from the top of its stock counterpart, which returns immediately afterwards.
 * The upstream bodies are left in place untouched but never run.
 *
 * Purpose: skip maps that have nobody on them. An instance-only sim still creates the continent
 * maps at startup, and they then tick forever with no players.
 *
 * The stock 4-step round robin (mapUpdateStep 0-3 with the `full` flag) is reproduced exactly.
 * It is what decides whether a map gets the accumulated timer or a session-only update, so bots
 * observe the same update cadence they would on a stock server.
 *
 * The one thing that must not be "optimised": a MapInstanced is a *container* keyed by map id,
 * and it never has players itself -- they are in its child instances. Skipping containers on
 * !HavePlayers() would skip every instance on the server and, worse, stop CanUnload() running,
 * so finished instances would never be destroyed. For a sim that cycles instances that is an
 * unbounded leak. Hence: containers always recurse, CanUnload always runs, and only the child's
 * Update() is skipped while it is empty.
 */

#include "Map.h"
#include "MapInstanced.h"
#include "MapMgr.h"
#include "MapUpdater.h"

namespace
{
    /// A non-instanceable base map (a continent) is skippable while empty. Containers never are.
    inline bool ForgeMapIsIdle(Map const* map)
    {
        return !map->Instanceable() && !map->HavePlayers();
    }

    /// Stock's round-robin selector: which map kind gets a full update on this step.
    /// 0 = continents, 1 = battlegrounds/arenas, 2 = dungeons, 3 = none.
    inline bool ForgeMapMatchesStep(Map const* map, uint8 step)
    {
        switch (step)
        {
            case 0:  return !map->IsBattlegroundOrArena() && !map->IsDungeon();
            case 1:  return map->IsBattlegroundOrArena();
            case 2:  return map->IsDungeon();
            default: return false;
        }
    }
}

void MapMgr::ForgeUpdate(uint32 diff)
{
    for (uint8 i = 0; i < 4; ++i)
        i_timer[i].Update(diff);

    // Stock schedules an LFG update here. The sim has no dungeon finder, so it is dropped.

    for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end(); ++iter)
    {
        Map* map = iter->second;

        if (ForgeMapIsIdle(map))
            continue;

        bool const full = mapUpdateStep < 3 && ForgeMapMatchesStep(map, mapUpdateStep);

        if (m_updater.activated())
            m_updater.schedule_update(*map, uint32(full ? i_timer[mapUpdateStep].GetCurrent() : 0), diff);
        else
            map->Update(uint32(full ? i_timer[mapUpdateStep].GetCurrent() : 0), diff);
    }

    if (m_updater.activated())
        m_updater.wait();

    if (mapUpdateStep < 3)
    {
        for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        {
            Map* map = iter->second;

            if (ForgeMapIsIdle(map))
                continue;

            if (ForgeMapMatchesStep(map, mapUpdateStep))
                map->DelayedUpdate(uint32(i_timer[mapUpdateStep].GetCurrent()));
        }

        i_timer[mapUpdateStep].SetCurrent(0);
        ++mapUpdateStep;
    }

    if (mapUpdateStep == 3 && i_timer[3].Passed())
    {
        mapUpdateStep = 0;
        i_timer[3].SetCurrent(0);
    }
}

void MapInstanced::ForgeUpdate(const uint32 t, const uint32 s_diff)
{
    // take care of loaded GridMaps (when unused, unload it!)
    Map::Update(t, s_diff, false);

    InstancedMaps::iterator i = m_InstancedMaps.begin();

    while (i != m_InstancedMaps.end())
    {
        // CanUnload() runs every tick regardless of occupancy: it is what decrements the unload
        // timer, and skipping it would leak every instance the sim ever creates.
        if (i->second->CanUnload(t))
        {
            if (!DestroyInstance(i))                             // iterator incremented
            {
                //m_unloadTimer
            }
        }
        else
        {
            // An empty instance still exists (it is inside its unload delay, or waiting for the
            // bots to zone in), but nothing in it needs simulating yet.
            if (!i->second->HavePlayers())
            {
                ++i;
                continue;
            }

            // update only here, because it may schedule some bad things before delete
            if (sMapMgr->GetMapUpdater()->activated())
                sMapMgr->GetMapUpdater()->schedule_update(*i->second, t, s_diff);
            else
                i->second->Update(t, s_diff);
            ++i;
        }
    }
}
