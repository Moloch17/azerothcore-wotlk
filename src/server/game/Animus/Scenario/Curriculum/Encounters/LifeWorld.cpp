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

#include "LifeWorld.h"
#include "MapMgr.h"
#include <set>
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Env.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum::LifeWorld;

    constexpr std::array<Band, BAND_COUNT> BANDS = { { { 15, 20 }, { 35, 40 }, { 58, 60 } } };

    constexpr float CELL_YARDS = 100.0f;
    constexpr uint32 MAP_EASTERN_KINGDOMS = 0;
    constexpr uint32 MAP_KALIMDOR = 1;

    /// The playable races of each side as a race mask (1 << (race - 1)): what quest_template.AllowableRaces holds.
    constexpr uint32 ALLIANCE_RACES = (1u << (RACE_HUMAN - 1)) | (1u << (RACE_DWARF - 1)) | (1u << (RACE_NIGHTELF - 1))
        | (1u << (RACE_GNOME - 1)) | (1u << (RACE_DRAENEI - 1));
    constexpr uint32 HORDE_RACES = (1u << (RACE_ORC - 1)) | (1u << (RACE_UNDEAD_PLAYER - 1)) | (1u << (RACE_TAUREN - 1))
        | (1u << (RACE_TROLL - 1)) | (1u << (RACE_BLOODELF - 1));

    /// How far a quest may reach, per band: the turn-in from the giver, and each objective's place from the
    /// giver. A 300 s episode on foot covers about 2,000 yards; the upper bands ride, and their quests are given
    /// further from where they are done.
    constexpr std::array<float, 3> QUEST_REACH = { 800.0f, 1200.0f, 1600.0f };
    /// How close an objective's spawns count as one place.
    constexpr float OBJECTIVE_CLUSTER_YARDS = 60.0f;
    /// A POI point's place gets its height from the creature spawns around it.
    constexpr float POI_HEIGHT_RADIUS = 60.0f;

    bool IsContinent(uint32 map)
    {
        return map == MAP_EASTERN_KINGDOMS || map == MAP_KALIMDOR;
    }

    uint64 CellKey(uint32 map, int32 cx, int32 cy)
    {
        return (uint64(map) << 48) | (uint64(uint32(cx) & 0xFFFFFF) << 24) | uint64(uint32(cy) & 0xFFFFFF);
    }

    int32 CellOf(float v)
    {
        return int32(std::floor(v / CELL_YARDS));
    }
}

Animus::Curriculum::LifeWorld::Band const& Animus::Curriculum::LifeWorld::BandAt(uint32 index)
{
    return BANDS[std::min<uint32>(index, BAND_COUNT - 1)];
}

uint32 Animus::Curriculum::LifeWorld::BandOf(uint8 level)
{
    for (uint32 band = 0; band < BAND_COUNT; ++band)
        if (level <= BANDS[band].Max)
            return band;
    return BAND_COUNT - 1;
}

Animus::Curriculum::LifeWorld::Side Animus::Curriculum::LifeWorld::SideOf(TeamId team)
{
    return team == TEAM_ALLIANCE ? Side::Alliance : team == TEAM_HORDE ? Side::Horde : Side::Any;
}

TeamId Animus::Curriculum::LifeWorld::TeamOf(Side side)
{
    return side == Side::Alliance ? TEAM_ALLIANCE : side == Side::Horde ? TEAM_HORDE : TEAM_NEUTRAL;
}

char const* Animus::Curriculum::LifeWorld::SideName(Side side)
{
    return side == Side::Alliance ? "alliance" : side == Side::Horde ? "horde" : "any";
}

// SpawnIndex ---------------------------------------------------------------------------------------------------------

Animus::Curriculum::LifeWorld::SpawnIndex const& Animus::Curriculum::LifeWorld::SpawnIndex::Instance()
{
    static SpawnIndex const index;
    return index;
}

Animus::Curriculum::LifeWorld::SpawnIndex::SpawnIndex()
{
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!IsContinent(data.mapid) || !(data.phaseMask & 1))
            continue;
        Spawn spawn;
        spawn.SpawnId = spawnId;
        spawn.Entry = data.id;
        spawn.Map = data.mapid;
        spawn.Pos.Relocate(data.posX, data.posY, data.posZ, data.orientation);
        _creatures.push_back(spawn);
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!IsContinent(data.mapid) || !(data.phaseMask & 1))
            continue;
        Spawn spawn;
        spawn.SpawnId = spawnId;
        spawn.Entry = data.id;
        spawn.Map = data.mapid;
        spawn.Pos.Relocate(data.posX, data.posY, data.posZ, data.orientation);
        spawn.Object = true;
        spawn.Rotation[0] = data.rotation.x;
        spawn.Rotation[1] = data.rotation.y;
        spawn.Rotation[2] = data.rotation.z;
        spawn.Rotation[3] = data.rotation.w;
        _objects.push_back(spawn);
    }

    for (Spawn const& spawn : _creatures)
    {
        _creatureCells[CellKey(spawn.Map, CellOf(spawn.Pos.GetPositionX()), CellOf(spawn.Pos.GetPositionY()))]
            .push_back(&spawn);
        _byEntry[spawn.Entry].push_back(&spawn);
    }
    for (Spawn const& spawn : _objects)
    {
        _objectCells[CellKey(spawn.Map, CellOf(spawn.Pos.GetPositionX()), CellOf(spawn.Pos.GetPositionY()))]
            .push_back(&spawn);
        _objectsByEntry[spawn.Entry].push_back(&spawn);
    }

    LOG_INFO("module.animus", "Life world: {} creature and {} gameobject spawns on the continents indexed",
        _creatures.size(), _objects.size());
}

void Animus::Curriculum::LifeWorld::SpawnIndex::Near(Cells const& cells, uint32 map, float x, float y, float radius,
    std::vector<Spawn const*>& out) const
{
    out.clear();
    int32 const reach = int32(std::ceil(radius / CELL_YARDS));
    int32 const cx = CellOf(x);
    int32 const cy = CellOf(y);
    for (int32 dx = -reach; dx <= reach; ++dx)
        for (int32 dy = -reach; dy <= reach; ++dy)
        {
            auto const cell = cells.find(CellKey(map, cx + dx, cy + dy));
            if (cell == cells.end())
                continue;
            for (Spawn const* spawn : cell->second)
                if (spawn->Pos.GetExactDist2d(x, y) <= radius)
                    out.push_back(spawn);
        }

    std::sort(out.begin(), out.end(), [x, y](Spawn const* a, Spawn const* b)
    {
        return a->Pos.GetExactDist2d(x, y) < b->Pos.GetExactDist2d(x, y);
    });
}

void Animus::Curriculum::LifeWorld::SpawnIndex::CreaturesNear(uint32 map, float x, float y, float radius,
    std::vector<Spawn const*>& out) const
{
    Near(_creatureCells, map, x, y, radius, out);
}

void Animus::Curriculum::LifeWorld::SpawnIndex::ObjectsNear(uint32 map, float x, float y, float radius,
    std::vector<Spawn const*>& out) const
{
    Near(_objectCells, map, x, y, radius, out);
}

std::vector<Animus::Curriculum::LifeWorld::Spawn const*> const&
Animus::Curriculum::LifeWorld::SpawnIndex::CreaturesOfEntry(uint32 entry) const
{
    static std::vector<Spawn const*> const none;
    auto const found = _byEntry.find(entry);
    return found == _byEntry.end() ? none : found->second;
}

// QuestSet -----------------------------------------------------------------------------------------------------------

Animus::Curriculum::LifeWorld::QuestSet const& Animus::Curriculum::LifeWorld::QuestSet::Instance()
{
    static QuestSet const set;
    return set;
}

std::vector<Animus::Curriculum::LifeWorld::Spawn const*> const&
Animus::Curriculum::LifeWorld::SpawnIndex::ObjectsOfEntry(uint32 entry) const
{
    static std::vector<Spawn const*> const none;
    auto const found = _objectsByEntry.find(entry);
    return found == _objectsByEntry.end() ? none : found->second;
}

Animus::Curriculum::LifeWorld::QuestSet::QuestSet()
{
    SpawnIndex const& spawns = SpawnIndex::Instance();
    QuestPlanner const& planner = QuestPlanner::Instance();

    // Quest -> the creatures that start and end it, from the relation tables (which run creature -> quest).
    std::unordered_map<uint32, std::vector<uint32>> starters;
    std::unordered_map<uint32, std::vector<uint32>> enders;
    for (auto const& [creature, quest] : *sObjectMgr->GetCreatureQuestRelationMap())
        starters[quest].push_back(creature);
    for (auto const& [creature, quest] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        enders[quest].push_back(creature);

    auto const nearestSpawn = [&spawns](std::vector<uint32> const& entries, Position const* near, uint32 map,
        float reach, bool objects) -> Spawn const*
    {
        Spawn const* best = nullptr;
        for (uint32 entry : entries)
            for (Spawn const* spawn : objects ? spawns.ObjectsOfEntry(entry) : spawns.CreaturesOfEntry(entry))
            {
                if (near && (spawn->Map != map || spawn->Pos.GetExactDist2d(near) > reach))
                    continue;
                if (!best || (near && spawn->Pos.GetExactDist2d(near) < best->Pos.GetExactDist2d(near)))
                    best = spawn;
            }
        return best;
    };

    // An objective's place: the spawn of its sources, within reach of the giver, with the most of them around it
    // (the nearest to the giver among equals), and those spawns. A real spawn point, so something is always there.
    auto const placeOf = [&spawns](std::vector<uint32> const& entries, bool objects, Spawn const& giver, float reach,
        ObjectivePlace& place)
    {
        std::vector<Spawn const*> near;
        for (uint32 entry : entries)
            for (Spawn const* spawn : objects ? spawns.ObjectsOfEntry(entry) : spawns.CreaturesOfEntry(entry))
                if (spawn->Map == giver.Map && spawn->Pos.GetExactDist2d(&giver.Pos) <= reach)
                    near.push_back(spawn);
        if (near.empty())
            return false;
        Spawn const* best = nullptr;
        std::size_t bestAround = 0;
        for (Spawn const* spawn : near)
        {
            std::size_t around = 0;
            for (Spawn const* other : near)
                if (other->Pos.GetExactDist2d(&spawn->Pos) <= OBJECTIVE_CLUSTER_YARDS)
                    ++around;
            if (!best || around > bestAround || (around == bestAround
                && spawn->Pos.GetExactDist2d(&giver.Pos) < best->Pos.GetExactDist2d(&giver.Pos)))
            {
                best = spawn;
                bestAround = around;
            }
        }
        place.Where.Relocate(best->Pos);
        for (Spawn const* spawn : near)
            if (spawn->Pos.GetExactDist2d(&best->Pos) <= OBJECTIVE_CLUSTER_YARDS)
                place.Spawns.push_back(spawn);
        return true;
    };

    std::array<uint32, BAND_COUNT> reached = {};
    std::array<uint32, BAND_COUNT> enderFar = {};
    std::array<uint32, BAND_COUNT> objectiveFar = {};
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
    {
        // What the planner could make objectives of, and nothing that needs a repeat, a day, a flag, a skill, a
        // class or a reputation. A prerequisite is fine: the episode grants it (QuestPlanner::GrantPrerequisites).
        QuestPlan const* plan = planner.Plan(id);
        if (!plan || !plan->Supported)
            continue;
        if (quest->HasFlag(QUEST_FLAGS_DAILY | QUEST_FLAGS_WEEKLY | QUEST_FLAGS_TRACKING | QUEST_FLAGS_UNAVAILABLE
            | QUEST_FLAGS_FLAGS_PVP | QUEST_FLAGS_RAID))
            continue;
        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_REPEATABLE | QUEST_SPECIAL_FLAGS_MONTHLY
            | QUEST_SPECIAL_FLAGS_DF_QUEST))
            continue;
        if (quest->GetRequiredSkill() || quest->GetRequiredClasses() || quest->GetRequiredMinRepFaction()
            || quest->IsSeasonal() || quest->GetQuestLevel() <= 0)
            continue;

        uint32 const level = uint32(quest->GetQuestLevel());
        uint32 band = BAND_COUNT;
        for (uint32 b = 0; b < BAND_COUNT; ++b)
            if (level >= BANDS[b].Min && level <= BANDS[b].Max && quest->GetMinLevel() <= BANDS[b].Max)
                band = b;
        if (band == BAND_COUNT)
            continue;

        uint32 const races = quest->GetAllowableRaces();
        Side side = Side::Any;
        if (races)
        {
            bool const alliance = races & ALLIANCE_RACES;
            bool const horde = races & HORDE_RACES;
            if (alliance && !horde)
                side = Side::Alliance;
            else if (horde && !alliance)
                side = Side::Horde;
            else if (!alliance && !horde)
                continue;
        }

        auto const starter = starters.find(id);
        auto const ender = enders.find(id);
        if (starter == starters.end() || ender == enders.end())
            continue;
        float const reach = QUEST_REACH[band];
        Spawn const* giver = nearestSpawn(starter->second, nullptr, 0, reach, false);
        if (!giver)
            continue;
        ++reached[band];
        Spawn const* turnIn = nearestSpawn(ender->second, &giver->Pos, giver->Map, reach, false);
        if (!turnIn)
        {
            ++enderFar[band];
            continue;
        }

        // Each objective's place, by kind.
        QuestCandidate candidate;
        bool reachable = true;
        for (PlannedObjective const& objective : plan->Objectives)
        {
            ObjectivePlace place;
            switch (objective.Kind)
            {
                case ObjectiveKind::Kill:
                case ObjectiveKind::CollectFromCreature:
                case ObjectiveKind::UseItemOn:
                case ObjectiveKind::CollectFromObject:
                case ObjectiveKind::UseObject:
                    reachable = placeOf(objective.Sources, objective.SourcesAreObjects, *giver, reach, place);
                    break;
                case ObjectiveKind::Buy:
                    if (Spawn const* vendor = nearestSpawn(objective.Sources, &giver->Pos, giver->Map, reach, false))
                    {
                        place.Where.Relocate(vendor->Pos);
                        place.Spawns = { vendor };
                    }
                    else
                        reachable = false;
                    break;
                case ObjectiveKind::Deliver:
                    place.Where.Relocate(turnIn->Pos);
                    break;
                case ObjectiveKind::Explore:
                    reachable = !objective.Places.empty() && objective.PoiMap == giver->Map
                        && objective.Places.front().GetExactDist2d(&giver->Pos) <= reach;
                    if (reachable)
                    {
                        place.Where.Relocate(objective.Places.front());
                        place.Radius = objective.Radius;
                    }
                    break;
                case ObjectiveKind::Count:
                    reachable = false;
                    break;
            }
            if (!reachable)
                break;
            candidate.Places.push_back(std::move(place));
            if (objective.Kind == ObjectiveKind::CollectFromCreature || objective.Kind == ObjectiveKind::CollectFromObject
                || objective.Kind == ObjectiveKind::Buy)
                candidate.Collect = true;
        }
        if (!reachable || candidate.Places.empty())
        {
            ++objectiveFar[band];
            continue;
        }

        candidate.Id = id;
        candidate.MinLevel = uint32(quest->GetMinLevel());
        candidate.Band = band;
        candidate.For = side;
        candidate.Giver = giver;
        candidate.Ender = turnIn;
        candidate.Plan = plan;
        candidate.Zone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, giver->Map, giver->Pos.GetPositionX(),
            giver->Pos.GetPositionY(), giver->Pos.GetPositionZ());
        candidate.HeldOut = IsHeldOutQuest(id) || IsHeldOutZone(candidate.Zone);
        _quests.push_back(std::move(candidate));
    }

    std::sort(_quests.begin(), _quests.end(), [](QuestCandidate const& a, QuestCandidate const& b)
    {
        return a.Id < b.Id;
    });

    // Held-out zones must not empty a band for a side: a zone whose hold-out would leave a (band, side) with fewer
    // than MIN_TRAINING quests in training stays in training. The zones held out are logged once, so an evaluation's
    // held-out numbers can be read against them.
    {
        constexpr uint32 MIN_TRAINING = 30;
        auto const trainingCount = [this](uint32 band, Side side)
        {
            return std::count_if(_quests.begin(), _quests.end(), [&](QuestCandidate const& q)
            {
                return q.Band == band && (q.For == side || q.For == Side::Any) && !q.HeldOut;
            });
        };
        std::set<uint32> heldZones;
        for (QuestCandidate const& q : _quests)
            if (IsHeldOutZone(q.Zone))
                heldZones.insert(q.Zone);
        std::set<uint32> kept;
        for (uint32 zone : heldZones)
            for (uint32 band = 0; band < BAND_COUNT && !kept.count(zone); ++band)
                for (Side side : { Side::Alliance, Side::Horde })
                    if (trainingCount(band, side) < MIN_TRAINING
                        && std::any_of(_quests.begin(), _quests.end(), [&](QuestCandidate const& q)
                            { return q.Zone == zone && q.Band == band && (q.For == side || q.For == Side::Any); }))
                    {
                        kept.insert(zone);
                        break;
                    }
        for (QuestCandidate& q : _quests)
            if (kept.count(q.Zone))
                q.HeldOut = IsHeldOutQuest(q.Id);
        std::string held;
        for (uint32 zone : heldZones)
            if (!kept.count(zone))
                held += Acore::StringFormat("{}{}", held.empty() ? "" : ", ", zone);
        LOG_INFO("module.animus", "Life world: zones held out of training: {} ({} kept to fill a band)",
            held.empty() ? "none" : held, kept.size());
    }

    // Chains: a quest's next is linked when it is a candidate too, on the same map (both are drawn together).
    std::unordered_map<uint32, QuestCandidate*> byId;
    for (QuestCandidate& quest : _quests)
        byId[quest.Id] = &quest;
    uint32 chained = 0;
    for (QuestCandidate& quest : _quests)
        if (auto const next = byId.find(quest.Plan->Next); next != byId.end() && next->second != &quest
            && next->second->Giver->Map == quest.Giver->Map && next->second->Band == quest.Band)
        {
            quest.Next = next->second;
            ++chained;
        }

    for (QuestCandidate const& quest : _quests)
    {
        auto const add = [this, &quest](Side side)
        {
            _byBandAndSide[{ quest.Band, side }].push_back(&quest);
            if (!quest.HeldOut)
                _trainingByBandAndSide[{ quest.Band, side }].push_back(&quest);
        };
        add(quest.For);
        if (quest.For == Side::Any)
        {
            add(Side::Alliance);
            add(Side::Horde);
        }
    }

    for (uint32 band = 0; band < BAND_COUNT; ++band)
        LOG_INFO("module.animus", "Life world: band {}-{} has {} alliance and {} horde quests of {} eligible "
            "({} turn-ins and {} objectives beyond {:.0f} yards)", BANDS[band].Min, BANDS[band].Max,
            For(band, Side::Alliance, true).size(), For(band, Side::Horde, true).size(), reached[band],
            enderFar[band], objectiveFar[band], QUEST_REACH[band]);
    LOG_INFO("module.animus", "Life world: {} quests, {} with a next quest in the set", _quests.size(), chained);
}

std::vector<Animus::Curriculum::LifeWorld::QuestCandidate const*> const&
Animus::Curriculum::LifeWorld::QuestSet::For(uint32 band, Side side, bool evaluating) const
{
    static std::vector<QuestCandidate const*> const none;
    auto const& map = evaluating ? _byBandAndSide : _trainingByBandAndSide;
    auto const found = map.find({ band, side });
    return found == map.end() ? none : found->second;
}

// Grounds and towns --------------------------------------------------------------------------------------------------

std::vector<Animus::Curriculum::LifeWorld::Ground> const& Animus::Curriculum::LifeWorld::GatherGrounds(uint32 band)
{
    // The three densest 400-yard cells of herb and ore spawns per zone (acore_world, 2026-09-24), each the average
    // of its nodes. Band 0: the Barrens, Westfall, Loch Modan. Band 1: Thousand Needles, Stranglethorn, Arathi,
    // Feralas. Band 2: Un'Goro, Winterspring, the Eastern Plaguelands, Silithus, the Burning Steppes.
    static std::vector<Ground> const grounds[BAND_COUNT] =
    {
        {
            { 1, -4160.0f, -2235.0f, 76.0f }, { 1, -615.0f, -2178.0f, 86.0f }, { 1, 157.0f, -1854.0f, 94.0f },
            { 0, -10518.0f, 1917.0f, 17.0f }, { 0, -9846.0f, 1422.0f, 41.0f }, { 0, -11280.0f, 1504.0f, 44.0f },
            { 0, -4988.0f, -3848.0f, 316.0f }, { 0, -4984.0f, -2997.0f, 323.0f },
        },
        {
            { 1, -4981.0f, -2261.0f, -53.0f }, { 1, -5016.0f, -972.0f, -4.0f }, { 1, -5462.0f, -1740.0f, -16.0f },
            { 0, -12298.0f, -1020.0f, 21.0f }, { 0, -12999.0f, -576.0f, 47.0f }, { 0, -11808.0f, 188.0f, 27.0f },
            { 0, -1768.0f, -3350.0f, 47.0f }, { 0, -907.0f, -3858.0f, 132.0f },
            { 1, -5067.0f, 1752.0f, 78.0f }, { 1, -5032.0f, 1353.0f, 60.0f },
        },
        {
            { 1, -6297.0f, -1451.0f, -253.0f }, { 1, -8129.0f, -1420.0f, -248.0f }, { 1, -7775.0f, -582.0f, -259.0f },
            { 1, 5356.0f, -4919.0f, 802.0f }, { 1, 6677.0f, -5279.0f, 763.0f }, { 1, 5740.0f, -4993.0f, 810.0f },
            { 0, 1819.0f, -3825.0f, 134.0f }, { 0, 3002.0f, -4635.0f, 105.0f }, { 0, 2585.0f, -3766.0f, 160.0f },
            { 1, -7756.0f, 1823.0f, 16.0f }, { 1, -6619.0f, 213.0f, 20.0f },
            { 0, -7858.0f, -2622.0f, 161.0f }, { 0, -8193.0f, -1807.0f, 148.0f },
        },
    };

    return grounds[std::min<uint32>(band, BAND_COUNT - 1)];
}

std::vector<uint32> const& Animus::Curriculum::LifeWorld::TownInns(uint32 band, Side side)
{
    // Innkeeper entries: the Crossroads (3934) and Goldshire (295); Camp Taurajo (7714), Menethil Harbor (1464)
    // and neutral Ratchet (6791); Orgrimmar (6929), the Undercity (6741), Stormwind (6740), Ironforge (5111).
    static std::vector<uint32> const horde[BAND_COUNT] = { { 3934 }, { 7714, 6791 }, { 6929, 6741 } };
    static std::vector<uint32> const alliance[BAND_COUNT] = { { 295 }, { 1464, 6791 }, { 6740, 5111 } };
    uint32 const b = std::min<uint32>(band, BAND_COUNT - 1);
    return side == Side::Horde ? horde[b] : alliance[b];
}

// Summoning ----------------------------------------------------------------------------------------------------------

Creature* Animus::Curriculum::LifeWorld::Summon(Map* map, uint32 phase, Spawn const& spawn)
{
    if (!map || spawn.Object)
        return nullptr;

    map->LoadGrid(spawn.Pos.GetPositionX(), spawn.Pos.GetPositionY());
    TempSummon* creature = map->SummonCreature(spawn.Entry, spawn.Pos);
    if (!creature)
        return nullptr;

    // Into the env's own phase (the world's copy stays in phase 1), standing where the table stands it.
    creature->SetPhaseMask(phase, true);
    creature->SetHomePosition(spawn.Pos);
    return creature;
}

GameObject* Animus::Curriculum::LifeWorld::SummonObject(Map* map, uint32 phase, Spawn const& spawn)
{
    if (!map || !spawn.Object)
        return nullptr;

    map->LoadGrid(spawn.Pos.GetPositionX(), spawn.Pos.GetPositionY());
    // No respawn: a gathered node is gone for the episode, and what is left is removed with it (Despawn).
    GameObject* object = map->SummonGameObject(spawn.Entry, spawn.Pos, spawn.Rotation[0], spawn.Rotation[1],
        spawn.Rotation[2], spawn.Rotation[3], 0);
    if (!object)
        return nullptr;

    object->SetPhaseMask(phase, true);
    return object;
}

void Animus::Curriculum::LifeWorld::Despawn(Map* map, std::vector<ObjectGuid>& spawned)
{
    if (map)
        for (ObjectGuid const& guid : spawned)
        {
            if (guid.IsGameObject())
            {
                if (GameObject* object = map->GetGameObject(guid))
                    object->Delete();
            }
            else if (Creature* creature = map->GetCreature(guid))
                creature->DespawnOrUnsummon();
        }

    spawned.clear();
}

bool Animus::Curriculum::LifeWorld::IsWorldCreature(Spawn const& spawn, bool npcs)
{
    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn.Entry);
    if (!info || spawn.Object)
        return false;
    // Nothing that is a trigger, a vehicle or a totem; the NPCs of a town only when asked for.
    if (info->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER) || info->type == CREATURE_TYPE_TOTEM
        || info->VehicleId)
        return false;
    return npcs || info->npcflag == 0;
}

float Animus::Curriculum::LifeWorld::GroundZ(Map* map, uint32 phase, float x, float y, float z)
{
    if (!map)
        return z;
    map->LoadGrid(x, y);
    float const ground = map->GetHeight(phase, x, y, z + 50.0f, true, 200.0f);
    return ground > INVALID_HEIGHT ? ground : z;
}

uint32 Animus::Curriculum::LifeWorld::Draw(Env const& env, uint32 count, uint32 salt)
{
    if (count <= 1)
        return 0;
    if (env.EpisodeSeedIndex == 0xFFFFFFFF)
        return urand(0, count - 1);
    // A seeded episode draws the same thing for the same seed, whatever else the sim did in between.
    uint64 h = (uint64(env.EpisodeSeedIndex) + 1) * 0x9E3779B97F4A7C15ull + uint64(salt) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 29;
    return uint32(h % count);
}
