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

#include "QuestPlanner.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include <algorithm>
#include <unordered_set>

namespace
{
    using namespace Animus::Curriculum;

    /// quest_poi's ObjectiveIndex for a quest column: the creature/object columns are 0-3, the item columns 4-9.
    constexpr int32 POI_ITEM_FIRST = 4;

    using Multimap = std::unordered_map<uint32, std::vector<uint32>>;

    void Add(Multimap& map, uint32 key, uint32 value)
    {
        std::vector<uint32>& values = map[key];
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }

    /// Item -> the creature entries whose loot has it for a quest.
    Multimap CreatureDroppers()
    {
        Multimap lootCreatures;
        for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
            if (creature.lootid)
                Add(lootCreatures, creature.lootid, entry);

        Multimap droppers;
        if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item FROM creature_loot_template WHERE QuestRequired = 1"))
            do
            {
                Field* fields = result->Fetch();
                if (auto const found = lootCreatures.find(fields[0].Get<uint32>()); found != lootCreatures.end())
                    for (uint32 entry : found->second)
                        Add(droppers, fields[1].Get<uint32>(), entry);
            } while (result->NextRow());
        return droppers;
    }

    /// Item -> the gameobject entries (chests, nodes, quest objects) whose loot has it.
    Multimap ObjectDroppers()
    {
        Multimap lootObjects;
        for (auto const& [entry, object] : *sObjectMgr->GetGameObjectTemplates())
            if (uint32 const loot = object.GetLootId())
                Add(lootObjects, loot, entry);

        Multimap droppers;
        if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item FROM gameobject_loot_template"))
            do
            {
                Field* fields = result->Fetch();
                if (auto const found = lootObjects.find(fields[0].Get<uint32>()); found != lootObjects.end())
                    for (uint32 entry : found->second)
                        Add(droppers, fields[1].Get<uint32>(), entry);
            } while (result->NextRow());
        return droppers;
    }

    Multimap Vendors()
    {
        Multimap vendors;
        if (QueryResult result = WorldDatabase.Query("SELECT entry, item FROM npc_vendor WHERE item > 0"))
            do
            {
                Field* fields = result->Fetch();
                Add(vendors, fields[1].Get<uint32>(), fields[0].Get<uint32>());
            } while (result->NextRow());
        return vendors;
    }

    Multimap QuestTriggers()
    {
        Multimap triggers;
        if (QueryResult result = WorldDatabase.Query("SELECT quest, id FROM areatrigger_involvedrelation"))
            do
            {
                Field* fields = result->Fetch();
                Add(triggers, fields[0].Get<uint32>(), fields[1].Get<uint32>());
            } while (result->NextRow());
        return triggers;
    }

    /// The centroids of the quest's POI areas for one objective column (any objective area when none is marked
    /// for it), on their map.
    void Places(uint32 quest, int32 poiIndex, PlannedObjective& objective)
    {
        QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(quest);
        if (!pois)
            return;

        auto const take = [&objective](QuestPOI const& poi)
        {
            if (poi.points.empty())
                return;
            float x = 0.0f;
            float y = 0.0f;
            for (QuestPOIPoint const& point : poi.points)
            {
                x += float(point.x);
                y += float(point.y);
            }
            objective.PoiMap = poi.MapId;
            objective.Places.emplace_back(x / float(poi.points.size()), y / float(poi.points.size()), 0.0f);
        };

        for (QuestPOI const& poi : *pois)
            if (poi.ObjectiveIndex == poiIndex)
                take(poi);
        if (objective.Places.empty() && poiIndex >= 0)
            for (QuestPOI const& poi : *pois)
                if (poi.ObjectiveIndex >= 0)
                    take(poi);
    }

    void Prerequisites(Quest const* quest, std::vector<uint32>& out, std::unordered_set<uint32>& seen)
    {
        for (int32 previous : quest->prevQuests)
        {
            if (previous <= 0 || !seen.insert(uint32(previous)).second)
                continue;
            if (Quest const* before = sObjectMgr->GetQuestTemplate(uint32(previous)))
            {
                Prerequisites(before, out, seen);
                out.push_back(uint32(previous));
            }
        }
    }
}

std::string_view Animus::Curriculum::ObjectiveKindName(ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Kill:                return "kill";
        case ObjectiveKind::CollectFromCreature: return "collect_creature";
        case ObjectiveKind::CollectFromObject:   return "collect_object";
        case ObjectiveKind::Buy:                 return "buy";
        case ObjectiveKind::UseObject:           return "use_object";
        case ObjectiveKind::UseItemOn:           return "use_item_on";
        case ObjectiveKind::Deliver:             return "deliver";
        case ObjectiveKind::Explore:             return "explore";
        case ObjectiveKind::Count:               break;
    }
    return "unknown";
}

bool Animus::Curriculum::IsHeldOutQuest(uint32 quest)
{
    // A fixed hash rather than id % 10, so held-out quests are not all from the same stretch of ids.
    uint32 h = quest * 2654435761u;
    h ^= h >> 16;
    return h % 10 == 0;
}

Animus::Curriculum::QuestPlanner const& Animus::Curriculum::QuestPlanner::Instance()
{
    static QuestPlanner const planner;
    return planner;
}

Animus::Curriculum::QuestPlan const* Animus::Curriculum::QuestPlanner::Plan(uint32 quest) const
{
    auto const found = _plans.find(quest);
    return found == _plans.end() ? nullptr : &found->second;
}

Animus::Curriculum::QuestPlanner::QuestPlanner()
{
    Multimap const creatureDroppers = CreatureDroppers();
    Multimap const objectDroppers = ObjectDroppers();
    Multimap const vendors = Vendors();
    Multimap const triggers = QuestTriggers();

    Multimap enders;
    for (auto const& [creature, quest] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        Add(enders, quest, creature);

    std::unordered_map<std::string_view, uint32> refused;
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
    {
        QuestPlan plan;
        plan.Id = id;
        plan.Next = quest->GetNextQuestInChain() ? quest->GetNextQuestInChain() : quest->GetNextQuestId();
        std::unordered_set<uint32> seen;
        Prerequisites(quest, plan.Prerequisites, seen);
        {
            PlannedObjective turnIn;
            Places(id, -1, turnIn);
            plan.TurnInPlaces = std::move(turnIn.Places);
            plan.TurnInMap = turnIn.PoiMap;
        }

        auto const refuse = [&plan, &refused](char const* why)
        {
            plan.Supported = false;
            plan.Unsupported = why;
            ++refused[why];
        };

        bool const cast = quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_CAST);
        bool ok = true;
        if (quest->GetTimeAllowed())
            refuse("timed"), ok = false;
        else if (quest->GetPlayersSlain())
            refuse("player kills"), ok = false;
        else if (std::any_of(quest->prevQuests.begin(), quest->prevQuests.end(), [](int32 q) { return q < 0; }))
            refuse("needs another quest active"), ok = false;
        else if (quest->HasFlag(QUEST_FLAGS_DAILY | QUEST_FLAGS_WEEKLY | QUEST_FLAGS_UNAVAILABLE | QUEST_FLAGS_RAID))
            refuse("daily, weekly, raid or unavailable"), ok = false;

        // The creature and object columns.
        for (uint32 i = 0; ok && i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const what = quest->RequiredNpcOrGo[i];
            uint32 const count = quest->RequiredNpcOrGoCount[i];
            if (!what || !count)
                continue;

            PlannedObjective objective;
            objective.Slot = int32(i);
            objective.Count = count;
            if (what < 0)
            {
                objective.Kind = ObjectiveKind::UseObject;
                objective.Entry = uint32(-what);
                objective.Sources = { objective.Entry };
                objective.SourcesAreObjects = true;
            }
            else if (cast)
            {
                // Credit for a spell cast on the creature: only learnable when the quest hands the seat the item
                // that casts it.
                uint32 item = quest->GetSrcItemId();
                for (uint32 k = 0; !item && k < QUEST_SOURCE_ITEM_IDS_COUNT; ++k)
                    item = quest->ItemDrop[k];
                if (!item)
                {
                    refuse("script credit with no item");
                    ok = false;
                    break;
                }
                objective.Kind = ObjectiveKind::UseItemOn;
                objective.Entry = uint32(what);
                objective.UseItem = item;
                objective.Sources = { objective.Entry };
            }
            else
            {
                objective.Kind = ObjectiveKind::Kill;
                objective.Entry = uint32(what);
                objective.Sources = { objective.Entry };
            }
            Places(id, int32(i), objective);
            plan.Objectives.push_back(std::move(objective));
        }

        // The item columns.
        for (uint32 i = 0; ok && i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const item = quest->RequiredItemId[i];
            uint32 const count = quest->RequiredItemCount[i];
            if (!item || !count || item == quest->GetSrcItemId())
                continue;           // the start item is handed over with the quest

            PlannedObjective objective;
            objective.Slot = int32(i);
            objective.Count = count;
            objective.Entry = item;
            if (auto const found = creatureDroppers.find(item); found != creatureDroppers.end())
            {
                objective.Kind = ObjectiveKind::CollectFromCreature;
                objective.Sources = found->second;
            }
            else if (auto const chest = objectDroppers.find(item); chest != objectDroppers.end())
            {
                objective.Kind = ObjectiveKind::CollectFromObject;
                objective.Sources = chest->second;
                objective.SourcesAreObjects = true;
            }
            else if (auto const vendor = vendors.find(item); vendor != vendors.end())
            {
                objective.Kind = ObjectiveKind::Buy;
                objective.Sources = vendor->second;
            }
            else
            {
                refuse("an item with no source");
                ok = false;
                break;
            }
            Places(id, POI_ITEM_FIRST + int32(i), objective);
            plan.Objectives.push_back(std::move(objective));
        }

        // A place to reach.
        if (ok && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        {
            auto const found = triggers.find(id);
            AreaTrigger const* trigger = found != triggers.end() ? sObjectMgr->GetAreaTrigger(found->second.front())
                : nullptr;
            if (!trigger)
            {
                refuse("an event with no place");
                ok = false;
            }
            else
            {
                PlannedObjective objective;
                objective.Kind = ObjectiveKind::Explore;
                objective.Entry = trigger->entry;
                objective.PoiMap = trigger->map;
                objective.Places.emplace_back(trigger->x, trigger->y, trigger->z);
                objective.Radius = std::max(trigger->radius, 5.0f);
                plan.Objectives.push_back(std::move(objective));
            }
        }

        // Nothing to gather: a delivery to the ender (the quest's own turn-in).
        if (ok && plan.Objectives.empty())
        {
            auto const found = enders.find(id);
            if (found == enders.end())
            {
                refuse("no ender");
                ok = false;
            }
            else
            {
                PlannedObjective objective;
                objective.Kind = ObjectiveKind::Deliver;
                objective.Entry = found->second.front();
                objective.Sources = found->second;
                Places(id, -1, objective);
                plan.Objectives.push_back(std::move(objective));
            }
        }

        if (ok)
        {
            plan.Supported = true;
            for (PlannedObjective const& objective : plan.Objectives)
                ++_kindCounts[std::size_t(objective.Kind)];
        }
        _plans.emplace(id, std::move(plan));
    }

    uint32 supported = 0;
    for (auto const& [id, plan] : _plans)
        supported += plan.Supported ? 1 : 0;
    std::string kinds;
    for (uint32 k = 0; k < OBJECTIVE_KIND_COUNT; ++k)
        kinds += Acore::StringFormat("{}{} {}", k ? ", " : "", _kindCounts[k], ObjectiveKindName(ObjectiveKind(k)));
    std::string why;
    for (auto const& [reason, count] : refused)
        why += Acore::StringFormat("{}{} {}", why.empty() ? "" : ", ", count, reason);
    LOG_INFO("module.animus", "Quest planner: {} of {} quests planned ({}); not planned: {}", supported,
        _plans.size(), kinds, why);
}

float Animus::Curriculum::QuestPlanner::Progress(Player const* player, QuestPlan const& plan, uint32 index)
{
    if (!player || index >= plan.Objectives.size())
        return 0.0f;

    QuestStatus const status = player->GetQuestStatus(plan.Id);
    if (status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED || player->GetQuestRewardStatus(plan.Id))
        return 1.0f;
    if (status == QUEST_STATUS_NONE)
        return 0.0f;

    Quest const* quest = sObjectMgr->GetQuestTemplate(plan.Id);
    PlannedObjective const& objective = plan.Objectives[index];
    float const count = float(std::max<uint32>(1, objective.Count));
    switch (objective.Kind)
    {
        case ObjectiveKind::Kill:
        case ObjectiveKind::UseObject:
        case ObjectiveKind::UseItemOn:
            if (!quest || objective.Slot < 0)
                return 0.0f;
            return std::min(1.0f, float(const_cast<Player*>(player)->GetReqKillOrCastCurrentCount(plan.Id,
                quest->RequiredNpcOrGo[objective.Slot])) / count);
        case ObjectiveKind::CollectFromCreature:
        case ObjectiveKind::CollectFromObject:
        case ObjectiveKind::Buy:
            return std::min(1.0f, float(player->GetItemCount(objective.Entry, true)) / count);
        case ObjectiveKind::Deliver:
        case ObjectiveKind::Explore:
        case ObjectiveKind::Count:
            return 0.0f;
    }
    return 0.0f;
}

void Animus::Curriculum::QuestPlanner::GrantPrerequisites(Player* player, QuestPlan const& plan)
{
    if (!player)
        return;
    for (uint32 before : plan.Prerequisites)
        if (!player->GetQuestRewardStatus(before))
            player->SetRewardedQuest(before);
}
