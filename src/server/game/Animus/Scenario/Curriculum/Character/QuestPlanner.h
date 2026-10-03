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

#ifndef ANIMUS_LIB_CURRICULUM_QUEST_PLANNER_H
#define ANIMUS_LIB_CURRICULUM_QUEST_PLANNER_H

#include "Define.h"
#include "Position.h"
#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

class Player;
class Quest;

namespace Animus::Curriculum
{
    /// **Quests as typed objectives** (long-horizon plan, Component A). A quest row says what it wants in a handful
    /// of columns -- creatures or objects with counts, items with counts, an area trigger, a start item -- and the
    /// rest of the world database says where those are and what yields them. The planner reads both once and turns
    /// every quest into objectives a bot can plan: kill these, loot this item from those creatures or from those
    /// chests, buy it from that vendor, use that object, use this item on that creature, deliver to that NPC, go to
    /// that place. The forge's life stages draw quests from it; the module reads a real character's quest log
    /// through it. Both see the same objectives.
    enum class ObjectiveKind : uint8
    {
        Kill,                   // creatures to kill (RequiredNpcOrGo > 0)
        CollectFromCreature,    // an item looted from creatures (creature_loot_template, QuestRequired)
        CollectFromObject,      // an item looted from a chest or node (gameobject_loot_template)
        Buy,                    // an item a vendor sells (npc_vendor)
        UseObject,              // objects to use (RequiredNpcOrGo < 0)
        UseItemOn,              // a quest item used on creatures for credit (QUEST_SPECIAL_FLAGS_CAST)
        Deliver,                // nothing to gather: take it to the quest ender
        Explore,                // reach a place (areatrigger_involvedrelation)
        Count
    };

    constexpr uint32 OBJECTIVE_KIND_COUNT = uint32(ObjectiveKind::Count);
    [[nodiscard]] std::string_view ObjectiveKindName(ObjectiveKind kind);

    struct PlannedObjective
    {
        ObjectiveKind Kind = ObjectiveKind::Kill;
        /// What is counted: the creature or object entry (Kill, UseObject, UseItemOn), the item (collect, Buy), the
        /// area trigger (Explore), the ender (Deliver).
        uint32 Entry = 0;
        uint32 Count = 1;
        uint32 UseItem = 0;                 // UseItemOn: the item to use
        int32 Slot = -1;                    // which RequiredNpcOrGo / RequiredItemId column it is
        /// Where it comes from: creature entries (Kill, CollectFromCreature, UseItemOn, Buy's vendors, Deliver's
        /// ender), object entries (CollectFromObject, UseObject).
        std::vector<uint32> Sources;
        bool SourcesAreObjects = false;
        /// Where the world map points for it (quest_poi): the centroid of each area, on PoiMap. The height is not
        /// in the table; a reader puts the place on the ground itself. Explore's place is the trigger's own, with
        /// a height and its radius.
        std::vector<Position> Places;
        uint32 PoiMap = 0;
        float Radius = 0.0f;
    };

    struct QuestPlan
    {
        uint32 Id = 0;
        std::vector<PlannedObjective> Objectives;
        /// The quests that must be rewarded before this one can be taken (every positive prevQuests entry, and
        /// theirs): what the sim grants a seat to put a chained quest in reach.
        std::vector<uint32> Prerequisites;
        uint32 Next = 0;                    // the chain's next quest (RewardNextQuest, else NextQuestId)
        /// Where the world map shows the turn-in (quest_poi objective -1), for a reader with no spawn table.
        std::vector<Position> TurnInPlaces;
        uint32 TurnInMap = 0;
        bool Supported = false;
        char const* Unsupported = "";       // why not, when not
    };

    class QuestPlanner
    {
    public:
        static QuestPlanner const& Instance();

        /// The plan of `quest`, or null for a quest id the database does not have.
        [[nodiscard]] QuestPlan const* Plan(uint32 quest) const;
        [[nodiscard]] std::size_t Size() const { return _plans.size(); }

        /// How much of an objective `player` has done, 0 to 1 (1 once the quest is complete or rewarded).
        [[nodiscard]] static float Progress(Player const* player, QuestPlan const& plan, uint32 objective);

        /// Mark every prerequisite of `plan` rewarded on `player`, so the quest can be offered: the sim's way of
        /// putting a chained quest in reach without playing the chain (the plan's "grant prerequisites").
        static void GrantPrerequisites(Player* player, QuestPlan const& plan);

    private:
        QuestPlanner();

        std::unordered_map<uint32, QuestPlan> _plans;
        std::array<uint32, OBJECTIVE_KIND_COUNT> _kindCounts{};
    };

    /// Whether a quest is held out of training (a tenth of them, by id): drawn only in evaluation, so the eval
    /// reads how well quests the policy never saw are done.
    [[nodiscard]] bool IsHeldOutQuest(uint32 quest);
    /// Whether a zone is held out of training (a tenth of them, by id): every quest given there is evaluation only,
    /// so the eval also reads how quests go in places the policy never trained in.
    [[nodiscard]] bool IsHeldOutZone(uint32 zone);
}

#endif
