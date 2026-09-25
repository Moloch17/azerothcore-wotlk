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

#ifndef ACORE_MAPMANAGER_H
#define ACORE_MAPMANAGER_H

#include "Common.h"
#include "Define.h"
#include "Map.h"
#include <atomic>
#include "MapInstanced.h"
#include "MapUpdater.h"
#include "Object.h"
#include "Timer.h"

class Transport;
class StaticTransport;
class MotionTransport;
struct TransportCreatureProto;

class MapMgr
{
public:
    static MapMgr* instance();

    Map* CreateBaseMap(uint32 mapId);
    Map* FindBaseNonInstanceMap(uint32 mapId) const;

    /// One of several Map objects for the same continent, so the sim can spread its envs over map objects --
    /// and so over map tasks -- instead of crowding one. Replica 0 is the base map itself; a higher index is a
    /// child of it with an instance id of its own, created on first use.
    ///
    /// A replica costs only its own grids, spawns and nav query: terrain comes from the base (GridTerrainLoader
    /// gives any map with an instance id its parent's), and so do the collision tree and the navmesh
    /// (MapCollisionData). Its own nav query is the point -- one per map object is what makes concurrent
    /// pathfinding safe. The base's grids are all loaded before the first replica, because a replica's grid
    /// asks the base for terrain that must already be there.
    ///
    /// World thread only: it takes the manager's lock, adds to its containers and loads a continent's grids.
    Map* CreateContinentReplica(uint32 mapId, uint32 index);
    Map* CreateMap(uint32 mapId, Player* player);
    Map* FindMap(uint32 mapId, uint32 instanceId) const;

    Map* FindBaseMap(uint32 mapId) const // pussywizard: need this public for movemaps (mmaps)
    {
        MapMapType::const_iterator iter = i_maps.find(mapId);
        return (iter == i_maps.end() ? nullptr : iter->second);
    }

    [[nodiscard]] uint32 GetAreaId(uint32 phaseMask, uint32 mapid, float x, float y, float z) const
    {
        Map const* m = const_cast<MapMgr*>(this)->CreateBaseMap(mapid);
        return m->GetAreaId(phaseMask, x, y, z);
    }
    [[nodiscard]] uint32 GetAreaId(uint32 phaseMask, uint32 mapid, Position const& pos) const { return GetAreaId(phaseMask, mapid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()); }
    [[nodiscard]] uint32 GetAreaId(uint32 phaseMask, WorldLocation const& loc) const { return GetAreaId(phaseMask, loc.GetMapId(), loc); }

    [[nodiscard]] uint32 GetZoneId(uint32 phaseMask, uint32 mapid, float x, float y, float z) const
    {
        Map const* m = const_cast<MapMgr*>(this)->CreateBaseMap(mapid);
        return m->GetZoneId(phaseMask, x, y, z);
    }
    [[nodiscard]] uint32 GetZoneId(uint32 phaseMask, uint32 mapid, Position const& pos) const { return GetZoneId(phaseMask, mapid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()); }
    [[nodiscard]] uint32 GetZoneId(uint32 phaseMask, WorldLocation const& loc) const { return GetZoneId(phaseMask, loc.GetMapId(), loc); }

    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, uint32 mapid, float x, float y, float z)
    {
        Map const* m = const_cast<MapMgr*>(this)->CreateBaseMap(mapid);
        m->GetZoneAndAreaId(phaseMask, zoneid, areaid, x, y, z);
    }

    void Initialize(void);
    void Update(uint32);

    /// Stock spread map work over MapUpdateInterval with a round robin; the sim ticks every map every
    /// tick, so the interval has nothing left to drive. Kept for the config reload that sets it.
    void SetMapUpdateInterval(uint32 /*t*/) { }

    //void LoadGrid(int mapid, int instId, float x, float y, WorldObject const* obj, bool no_unload = false);
    void UnloadAll();

    static bool ExistMapAndVMap(uint32 mapid, float x, float y);
    static bool IsValidMAP(uint32 mapid, bool startUp);

    static bool IsValidMapCoord(uint32 mapid, Position const& pos)
    {
        return IsValidMapCoord(mapid, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation());
    }

    static bool IsValidMapCoord(uint32 mapid, float x, float y)
    {
        return IsValidMAP(mapid, false) && Acore::IsValidMapCoord(x, y);
    }

    static bool IsValidMapCoord(uint32 mapid, float x, float y, float z)
    {
        return IsValidMAP(mapid, false) && Acore::IsValidMapCoord(x, y, z);
    }

    static bool IsValidMapCoord(uint32 mapid, float x, float y, float z, float o)
    {
        return IsValidMAP(mapid, false) && Acore::IsValidMapCoord(x, y, z, o);
    }

    static bool IsValidMapCoord(WorldLocation const& loc)
    {
        return IsValidMapCoord(loc.GetMapId(), loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ(), loc.GetOrientation());
    }

    // modulos a radian orientation to the range of 0..2PI
    static float NormalizeOrientation(float o)
    {
        // fmod only supports positive numbers. Thus we have
        // to emulate negative numbers
        if (o < 0)
        {
            float mod = o * -1;
            mod = std::fmod(mod, 2.0f * static_cast<float>(M_PI));
            mod = -mod + 2.0f * static_cast<float>(M_PI);
            return mod;
        }
        return std::fmod(o, 2.0f * static_cast<float>(M_PI));
    }

    /**
    * @name GetInstanceIDs
    * @return vector of instance IDs
    */
    std::vector<bool> GetInstanceIDs()
    {
        return _instanceIds;
    }

    void DoDelayedMovesAndRemoves();

    /// An instance was just destroyed (MapInstanced::Update, on a map thread): ask for a heap trim.
    void NoteInstanceDestroyed();

    /// Every map's Map::UpdateTiming summed, refreshed at the end of each Update() while no map thread runs.
    /// Cumulative; readers diff two snapshots. Instances are included through their container, continent
    /// replicas through i_replicaById.
    [[nodiscard]] Map::UpdateTiming const& GetUpdateTiming() const { return _updateTiming; }

    /// The map tasks of every Update(), accumulated: how many ran, their summed wall time, each tick's longest
    /// task, and the wall time of the whole schedule-and-join. Sum over wall is the parallelism actually had;
    /// longest over wall says whether one map is the critical path. The last tick's slowest map, and the CPUs
    /// its tasks started on, name the straggler and show where the pinned workers ran.
    struct TaskTiming
    {
        uint64 Ticks = 0;
        uint64 Tasks = 0;
        uint64 SumNs = 0;
        uint64 LongestNs = 0;           ///< sum over ticks of that tick's longest task
        uint64 WallNs = 0;
        uint32 SlowestMapId = 0;        ///< last tick
        uint32 SlowestInstanceId = 0;
        int32 SlowestCpu = -1;
        uint64 CpuMask = 0;             ///< last tick: CPUs 0-63 on which a task started
    };

    [[nodiscard]] TaskTiming const& GetTaskTiming() const { return _taskTiming; }

    /// The diff `map` ticks with on this world tick of `diff`, or 0 to leave it out: the forge's half-batch
    /// (AnimusForge.HalfBatch) freezes the maps of the half that is deciding, and a frozen map gets the world time it
    /// missed on its next tick. Every other map ticks every world tick, with `diff`.
    uint32 ForgeTickDiff(Map& map, uint32 diff);

    Map::EnterState PlayerCannotEnter(uint32 mapid, Player* player, bool loginCheck = false);
    void InitializeVisibilityDistanceInfo();

    /* statistics */
    void GetNumInstances(uint32& dungeons, uint32& battlegrounds, uint32& arenas);
    void GetNumPlayersInInstances(uint32& dungeons, uint32& battlegrounds, uint32& arenas, uint32& spectators);

    // Instance ID management
    void InitInstanceIds();
    void RegisterInstanceId(uint32 instanceId);
    uint32 GenerateInstanceId();

    MapUpdater* GetMapUpdater() { return &m_updater; }

    template<typename Worker>
    void DoForAllMaps(Worker&& worker);

    template<typename Worker>
    void DoForAllMapsWithMapId(uint32 mapId, Worker&& worker);

private:
    typedef std::unordered_map<uint32, Map*> MapMapType;
    typedef std::vector<bool> InstanceIds;

    MapMgr();
    ~MapMgr();

    MapMgr(MapMgr const&);
    MapMgr& operator=(MapMgr const&);

    /// Give the heap freed by destroyed instances back to the OS, at most once per trim interval.
    void ForgeTrimHeap(uint32 diff);

    Map::UpdateTiming _updateTiming;
    TaskTiming _taskTiming;

    /// Instances destroyed since the last trim, and the countdown to the next one.
    std::atomic<uint32> _destroyedInstances{ 0 };
    uint32 _trimCountdown{ 10 * IN_MILLISECONDS };

    /// Continent replicas by map id, in index order, index 0 being the base map in i_maps.
    typedef std::unordered_map<uint32, std::vector<Map*>> ReplicaMapType;
    /// The same maps keyed by map id and instance id, for FindMap, which is asked on every hook.
    typedef std::unordered_map<uint64, Map*> ReplicaByIdType;

    [[nodiscard]] static uint64 ReplicaKey(uint32 mapId, uint32 instanceId)
    {
        return (uint64(mapId) << 32) | instanceId;
    }

    std::mutex Lock;
    MapMapType i_maps;
    ReplicaMapType i_replicas;
    ReplicaByIdType i_replicaById;

    InstanceIds _instanceIds;
    uint32 _nextInstanceId;
    MapUpdater m_updater;
};

template<typename Worker>
void MapMgr::DoForAllMaps(Worker&& worker)
{
    std::lock_guard<std::mutex> guard(Lock);

    for (auto& mapPair : i_maps)
    {
        Map* map = mapPair.second;
        if (MapInstanced* mapInstanced = map->ToMapInstanced())
        {
            MapInstanced::InstancedMaps& instances = mapInstanced->GetInstancedMaps();
            for (auto& instancePair : instances)
                worker(instancePair.second);
        }
        else
            worker(map);
    }

    // A continent replica is a world of its own: whatever is done to every map is done to it too, or game
    // events and world spawns would reach the base continent only.
    for (auto& replica : i_replicaById)
        worker(replica.second);
}

template<typename Worker>
inline void MapMgr::DoForAllMapsWithMapId(uint32 mapId, Worker&& worker)
{
    std::lock_guard<std::mutex> guard(Lock);

    auto itr = i_maps.find(mapId);
    if (itr != i_maps.end())
    {
        Map* map = itr->second;
        if (MapInstanced* mapInstanced = map->ToMapInstanced())
        {
            MapInstanced::InstancedMaps& instances = mapInstanced->GetInstancedMaps();
            for (auto& p : instances)
                worker(p.second);
        }
        else
            worker(map);
    }

    auto const replicas = i_replicas.find(mapId);
    if (replicas != i_replicas.end())
        for (std::size_t index = 1; index < replicas->second.size(); ++index)
            worker(replicas->second[index]);
}

#define sMapMgr MapMgr::instance()

#endif
