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

#include "MapMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GridDefines.h"
#include "GridTerrainLoader.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "LFGMgr.h"
#include "Language.h"
#include "Log.h"
#include "MapInstanced.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "Transport.h"
#include "World.h"
#include "WorldPacket.h"
#include <atomic>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace
{
    /// How often a heap trim may run after instances were destroyed.
    constexpr uint32 ForgeTrimInterval = 10 * IN_MILLISECONDS;

    /// A non-instanceable base map (a continent) is skippable while empty. A MapInstanced is a
    /// *container* keyed by map id and never has players itself -- they are in its child instances.
    /// Skipping containers on !HavePlayers() would skip every instance on the server and, worse,
    /// stop CanUnload() running, so finished instances would never be destroyed. Containers are
    /// never skipped.
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

MapMgr::MapMgr()
{
    i_timer[3].SetInterval(sWorld->getIntConfig(CONFIG_INTERVAL_MAPUPDATE));
    mapUpdateStep = 0;
    _nextInstanceId = 0;
}

MapMgr::~MapMgr()
{
}

MapMgr* MapMgr::instance()
{
    static MapMgr instance;
    return &instance;
}

void MapMgr::Initialize()
{
    int num_threads(sWorld->getIntConfig(CONFIG_NUMTHREADS));

    // Start mtmaps if needed
    if (num_threads > 0)
        m_updater.activate(num_threads);
}

void MapMgr::InitializeVisibilityDistanceInfo()
{
    for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        (*iter).second->InitVisibilityDistance();
}

Map* MapMgr::CreateBaseMap(uint32 id)
{
    Map* map = FindBaseMap(id);

    if (!map)
    {
        std::lock_guard<std::mutex> guard(Lock);

        map = FindBaseMap(id);
        if (!map) // pussywizard: check again after acquiring mutex
        {
            MapEntry const* entry = sMapStore.LookupEntry(id);
            ASSERT(entry);

            if (entry->Instanceable())
                map = new MapInstanced(id);
            else
                map = new Map(id, 0, REGULAR_DIFFICULTY);

            i_maps[id] = map;

            if (!entry->Instanceable())
            {
                map->LoadRespawnTimes();
                map->LoadCorpseData();
            }

            map->OnCreateMap();
        }
    }

    ASSERT(map);
    return map;
}

Map* MapMgr::FindBaseNonInstanceMap(uint32 mapId) const
{
    Map* map = FindBaseMap(mapId);
    if (map && map->Instanceable())
        return nullptr;
    return map;
}

Map* MapMgr::CreateMap(uint32 id, Player* player)
{
    Map* m = CreateBaseMap(id);

    if (m && m->Instanceable())
        m = ((MapInstanced*)m)->CreateInstanceForPlayer(id, player);

    return m;
}

Map* MapMgr::FindMap(uint32 mapid, uint32 instanceId) const
{
    Map* map = FindBaseMap(mapid);
    if (!map)
        return nullptr;

    if (!map->Instanceable())
        return instanceId == 0 ? map : nullptr;

    return ((MapInstanced*)map)->FindInstanceMap(instanceId);
}

Map::EnterState MapMgr::PlayerCannotEnter(uint32 mapid, Player* player, bool loginCheck)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapid);
    if (!entry)
        return Map::CANNOT_ENTER_NO_ENTRY;

    if (!entry->IsDungeon())
        return Map::CAN_ENTER;

    InstanceTemplate const* instance = sObjectMgr->GetInstanceTemplate(mapid);
    if (!instance)
        return Map::CANNOT_ENTER_UNINSTANCED_DUNGEON;

    Difficulty targetDifficulty, requestedDifficulty;
    targetDifficulty = requestedDifficulty = player->GetDifficulty(entry->IsRaid());
    // Get the highest available difficulty if current setting is higher than the instance allows
    MapDifficulty const* mapDiff = GetDownscaledMapDifficultyData(entry->MapID, targetDifficulty);
    if (!mapDiff)
    {
        player->SendTransferAborted(mapid, TRANSFER_ABORT_DIFFICULTY, requestedDifficulty);
        return Map::CANNOT_ENTER_DIFFICULTY_UNAVAILABLE;
    }

    //Bypass checks for GMs
    if (player->IsGameMaster())
        return Map::CAN_ENTER;

    char const* mapName = entry->name[player->GetSession()->GetSessionDbcLocale()];

    if (!sScriptMgr->OnPlayerCanEnterMap(player, entry, instance, mapDiff, loginCheck))
        return Map::CANNOT_ENTER_UNSPECIFIED_REASON;

    Group* group = player->GetGroup();
    if (entry->IsRaid())
    {
        // can only enter in a raid group
        if ((!group || !group->isRaidGroup()) && !sWorld->getBoolConfig(CONFIG_INSTANCE_IGNORE_RAID))
        {
            // probably there must be special opcode, because client has this string constant in GlobalStrings.lua
            /// @todo: this is not a good place to send the message
            player->GetSession()->SendAreaTriggerMessage(LANG_INSTANCE_RAID_GROUP_ONLY, mapName);
            LOG_DEBUG("maps", "MAP: Player '{}' must be in a raid group to enter instance '{}'", player->GetName(), mapName);
            return Map::CANNOT_ENTER_NOT_IN_RAID;
        }
    }

    // xinef: dont allow LFG Group to enter other instance that is selected
    if (group)
        if (group->isLFGGroup())
            if (!sLFGMgr->inLfgDungeonMap(group->GetGUID(), mapid, targetDifficulty))
            {
                player->SendTransferAborted(mapid, TRANSFER_ABORT_MAP_NOT_ALLOWED);
                return Map::CANNOT_ENTER_UNSPECIFIED_REASON;
            }

    if (!player->IsAlive())
    {
        if (player->HasCorpse())
        {
            // let enter in ghost mode in instance that connected to inner instance with corpse
            uint32 corpseMap = player->GetCorpseLocation().GetMapId();
            do
            {
                if (corpseMap == mapid)
                    break;

                InstanceTemplate const* corpseInstance = sObjectMgr->GetInstanceTemplate(corpseMap);
                corpseMap = corpseInstance ? corpseInstance->Parent : 0;
            } while (corpseMap);

            if (!corpseMap)
            {
                WorldPacket data(SMSG_CORPSE_NOT_IN_INSTANCE, 0);
                player->SendDirectMessage(&data);
                LOG_DEBUG("maps", "MAP: Player '{}' does not have a corpse in instance '{}' and cannot enter.", player->GetName(), mapName);
                return Map::CANNOT_ENTER_CORPSE_IN_DIFFERENT_INSTANCE;
            }
            LOG_DEBUG("maps", "MAP: Player '{}' has corpse in instance '{}' and can enter.", player->GetName(), mapName);
        }
        else
        {
            LOG_DEBUG("maps", "Map::PlayerCannotEnter - player '{}' is dead but does not have a corpse!", player->GetName());
            return Map::CANNOT_ENTER_CORPSE_IN_DIFFERENT_INSTANCE;
        }
    }

    // if map exists - check for being full, etc.
    if (!loginCheck) // for login this is done by the calling function
    {
        uint32 destInstId = sInstanceSaveMgr->PlayerGetDestinationInstanceId(player, mapid, targetDifficulty);
        if (destInstId)
            if (Map* boundMap = FindMap(mapid, destInstId))
                if (Map::EnterState denyReason = boundMap->CannotEnter(player, loginCheck))
                    return denyReason;
    }

    // players are only allowed to enter 5 instances per hour
    if (entry->IsNonRaidDungeon() && (!group || !group->isLFGGroup() || !group->IsLfgRandomInstance()))
    {
        uint32 instaceIdToCheck = 0;
        if (InstanceSave* save = sInstanceSaveMgr->PlayerGetInstanceSave(player->GetGUID(), mapid, player->GetDifficulty(entry->IsRaid())))
            instaceIdToCheck = save->GetInstanceId();

        // instaceIdToCheck can be 0 if save not found - means no bind so the instance is new
        if (!player->CheckInstanceCount(instaceIdToCheck))
        {
            player->SendTransferAborted(mapid, TRANSFER_ABORT_TOO_MANY_INSTANCES);
            return Map::CANNOT_ENTER_TOO_MANY_INSTANCES;
        }
    }

    //Other requirements
    return player->Satisfy(sObjectMgr->GetAccessRequirement(mapid, targetDifficulty), mapid, true) ? Map::CAN_ENTER : Map::CANNOT_ENTER_UNSPECIFIED_REASON;
}

/// The sim host's map tick.
///
/// Skips maps that have nobody on them: the sim still creates the continent maps at startup, and
/// they would otherwise tick forever with no players.
///
/// The stock 4-step round robin (mapUpdateStep 0-3 with the `full` flag) is reproduced exactly.
/// It is what decides whether a map gets the accumulated timer or a session-only update, so bots
/// observe the same update cadence they would on a stock server.
///
/// Destroying an instance frees tens of megabytes of small allocations, and the allocator keeps
/// that in the process arenas rather than returning it: a sim that builds and drops pools of a
/// hundred instances (a scenario change, `forge bench`) reads as tens of gigabytes still held. So
/// a destroyed instance asks for a trim, at most one every ForgeTrimInterval, on the world thread
/// with no map updating.
void MapMgr::Update(uint32 diff)
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

    // After the updaters are done and before the next tick schedules any: no map is being updated here.
    ForgeTrimHeap(diff);

    // Roll every map's phase timers into one figure the status line can diff between reports.
    Map::UpdateTiming total;
    for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end(); ++iter)
    {
        total += iter->second->GetUpdateTiming();
        if (MapInstanced* container = iter->second->ToMapInstanced())
            for (auto const& [id, instance] : container->GetInstancedMaps())
                total += instance->GetUpdateTiming();
    }
    _updateTiming = total;
}

void MapMgr::NoteInstanceDestroyed()
{
    _destroyedInstances.fetch_add(1);
}

/// Give the heap freed by destroyed instances back to the OS.
void MapMgr::ForgeTrimHeap(uint32 diff)
{
    _trimCountdown = diff < _trimCountdown ? _trimCountdown - diff : 0;
    if (_trimCountdown || !_destroyedInstances.exchange(0))
        return;

    _trimCountdown = ForgeTrimInterval;
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

void MapMgr::DoDelayedMovesAndRemoves()
{
}

bool MapMgr::ExistMapAndVMap(uint32 mapid, float x, float y)
{
    GridCoord p = Acore::ComputeGridCoord(x, y);
    return GridTerrainLoader::ExistMap(mapid, p.x_coord, p.y_coord) && GridTerrainLoader::ExistVMap(mapid, p.x_coord, p.y_coord);
}

bool MapMgr::IsValidMAP(uint32 mapid, bool startUp)
{
    MapEntry const* mEntry = sMapStore.LookupEntry(mapid);

    if (startUp)
    {
        return mEntry != nullptr;
    }
    else
    {
        return mEntry && (!mEntry->IsDungeon() || sObjectMgr->GetInstanceTemplate(mapid));
    }

    /// @todo: add check for battleground template
}

void MapMgr::UnloadAll()
{
    for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end();)
    {
        iter->second->UnloadAll();
        delete iter->second;
        i_maps.erase(iter++);
    }

    if (m_updater.activated())
        m_updater.deactivate();
}

void MapMgr::GetNumInstances(uint32& dungeons, uint32& battlegrounds, uint32& arenas)
{
    for (MapMapType::iterator itr = i_maps.begin(); itr != i_maps.end(); ++itr)
    {
        Map* map = itr->second;
        if (!map->Instanceable())
            continue;
        MapInstanced::InstancedMaps& maps = ((MapInstanced*)map)->GetInstancedMaps();
        for (MapInstanced::InstancedMaps::iterator mitr = maps.begin(); mitr != maps.end(); ++mitr)
        {
            if (mitr->second->IsDungeon()) dungeons++;
            else if (mitr->second->IsBattleground()) battlegrounds++;
            else if (mitr->second->IsBattleArena()) arenas++;
        }
    }
}

void MapMgr::GetNumPlayersInInstances(uint32& dungeons, uint32& battlegrounds, uint32& arenas, uint32& spectators)
{
    for (MapMapType::iterator itr = i_maps.begin(); itr != i_maps.end(); ++itr)
    {
        Map* map = itr->second;
        if (!map->Instanceable())
            continue;
        MapInstanced::InstancedMaps& maps = ((MapInstanced*)map)->GetInstancedMaps();
        for (MapInstanced::InstancedMaps::iterator mitr = maps.begin(); mitr != maps.end(); ++mitr)
        {
            if (mitr->second->IsDungeon()) dungeons += ((InstanceMap*)mitr->second)->GetPlayers().getSize();
            else if (mitr->second->IsBattleground()) battlegrounds += ((InstanceMap*)mitr->second)->GetPlayers().getSize();
            else if (mitr->second->IsBattleArena())
            {
                uint32 spect = 0;
                if (BattlegroundMap* bgmap = mitr->second->ToBattlegroundMap())
                    if (Battleground* bg = bgmap->GetBG())
                        spect = bg->GetSpectators().size();

                arenas += ((InstanceMap*)mitr->second)->GetPlayers().getSize() - spect;
                spectators += spect;
            }
        }
    }
}

void MapMgr::InitInstanceIds()
{
    _nextInstanceId = 1;

    QueryResult result = CharacterDatabase.Query("SELECT MAX(id) FROM instance");
    if (result)
    {
        uint32 maxId = (*result)[0].Get<uint32>();
        _instanceIds.resize(maxId + 1);
    }
}

void MapMgr::RegisterInstanceId(uint32 instanceId)
{
    // Allocation was done in InitInstanceIds()
    _instanceIds[instanceId] = true;

    // Instances are pulled in ascending order from db and _nextInstanceId is initialized with 1,
    // so if the instance id is used, increment
    if (_nextInstanceId == instanceId)
        ++_nextInstanceId;
}

uint32 MapMgr::GenerateInstanceId()
{
    if (sToCloud9Sidecar->ClusterModeEnabled())
        return sToCloud9Sidecar->GenerateInstanceGuid();

    uint32 newInstanceId = _nextInstanceId;

    // find the lowest available id starting from the current _nextInstanceId
    while (_nextInstanceId < 0xFFFFFFFF && ++_nextInstanceId < _instanceIds.size() && _instanceIds[_nextInstanceId]);

    if (_nextInstanceId == 0xFFFFFFFF)
    {
        LOG_ERROR("server.worldserver", "Instance ID overflow!! Can't continue, shutting down server. ");
        World::StopNow(ERROR_EXIT_CODE);
    }

    return newInstanceId;
}
