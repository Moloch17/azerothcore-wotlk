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

#include "WorldActions.h"
#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GearStats.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Log.h"
#include "Supplies.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Animus::Curriculum;
    using Animus::Curriculum::WorldActions::NodeKind;

    /// The rank spells of the three gathering professions, as a player learns them: the skill each unlocks is the
    /// rank's cap (75, 150, 225, 300, 375, 450). The rank spell doubles as the gathering cast (Herb Gathering, Mining,
    /// Skinning open the node through SPELL_EFFECT_OPEN_LOCK / SPELL_EFFECT_SKINNING).
    struct Profession
    {
        uint32 Skill;
        std::array<uint32, 6> Ranks;
        uint32 Tool;            // the item the skill needs in the bags (0: none)
    };

    constexpr std::array<Profession, 3> PROFESSIONS = { {
        { SKILL_HERBALISM, { 2366, 2368, 3570, 11993, 28695, 50300 }, 0 },
        { SKILL_MINING, { 2575, 2576, 3564, 10248, 29354, 50310 }, 2901 },        // Mining Pick
        { SKILL_SKINNING, { 8613, 8617, 8618, 10768, 32678, 50305 }, 7005 },     // Skinning Knife
    } };

    constexpr std::array<uint16, 6> RANK_CAPS = { 75, 150, 225, 300, 375, 450 };

    Profession const* ProfessionOf(uint32 skill)
    {
        for (Profession const& profession : PROFESSIONS)
            if (profession.Skill == skill)
                return &profession;
        return nullptr;
    }

    uint32 SkillOfLock(uint32 lockId, NodeKind& kind)
    {
        kind = NodeKind::None;
        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return 0;

        for (uint32 j = 0; j < MAX_LOCK_CASE; ++j)
        {
            if (lock->Type[j] != LOCK_KEY_SKILL)
                continue;
            if (lock->Index[j] == LOCKTYPE_HERBALISM)
            {
                kind = NodeKind::Herb;
                return lock->Skill[j];
            }
            if (lock->Index[j] == LOCKTYPE_MINING)
            {
                kind = NodeKind::Mine;
                return lock->Skill[j];
            }
        }

        return 0;
    }

    SpellInfo const* HighestKnown(Player const* bot, Profession const& profession)
    {
        SpellInfo const* best = nullptr;
        for (uint32 rank : profession.Ranks)
            if (bot->HasSpell(rank))
                best = sSpellMgr->GetSpellInfo(rank);
        return best;
    }

    bool StartCast(Player* bot, SpellInfo const* info, SpellCastTargets& targets)
    {
        if (!info || !bot->IsAlive() || bot->IsNonMeleeSpellCast(false))
            return false;

        // A gathering cast is a cast: standing still first, as the mount cast does (TravelBlock::Apply).
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        return spell->prepare(&targets) == SPELL_CAST_OK;
    }

    /// Every item in the backpack and the bags.
    template <typename Visit>
    void ForEachBagItem(Player* bot, Visit visit)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                visit(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        visit(item);
    }

    bool IsJunk(ItemTemplate const* proto)
    {
        return proto && proto->Quality == ITEM_QUALITY_POOR && proto->SellPrice > 0;
    }

    /// Take every slot of `loot` the bot may have, once it is the bot's open loot.
    void TakeAll(Player* bot, Loot* loot, uint32& items, uint32& copper)
    {
        uint32 const slots = loot->GetMaxSlotInLootFor(bot);
        for (uint32 slot = 0; slot < slots; ++slot)
        {
            InventoryResult msg = EQUIP_ERR_OK;
            if (bot->StoreLootItem(uint8(slot), loot, msg))
                ++items;
        }

        // The money, as HandleLootMoneyOpcode does for a player alone (a group would split it).
        if (loot->gold)
        {
            bot->ModifyMoney(int32(loot->gold));
            copper += loot->gold;
            loot->gold = 0;
            loot->NotifyMoneyRemoved();
        }
    }
}

float Animus::Curriculum::WorldActions::GearScore(ItemTemplate const* proto, StatProfile stats)
{
    if (!proto)
        return 0.0f;

    // The item level carries the budget; the stats say whether the budget was spent on this build. A stat the
    // profile never wears (spirit on a rogue) counts against the item, so a higher-level wrong item does not win.
    float score = float(proto->ItemLevel);
    for (uint32 i = 0; i < proto->StatsCount; ++i)
    {
        int32 const preference = GearStats::StatPreference(stats, proto->ItemStat[i].ItemStatType);
        float const value = float(proto->ItemStat[i].ItemStatValue);
        if (preference > 0)
            score += value;
        else if (preference < 0)
            score -= value;
    }

    if (stats == StatProfile::Tank)
        score += float(proto->Armor) / 20.0f;
    if (proto->Class == ITEM_CLASS_WEAPON)
        score += proto->getDPS() * (stats == StatProfile::Caster || stats == StatProfile::Healer ? 0.2f : 2.0f);

    return score;
}

bool Animus::Curriculum::WorldActions::CanWear(Player const* bot, ItemTemplate const* proto)
{
    if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
        return false;
    if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    return bot->FindEquipSlot(proto, NULL_SLOT, false) != NULL_SLOT;
}

Item* Animus::Curriculum::WorldActions::BestUpgrade(Player* bot, StatProfile stats)
{
    Item* best = nullptr;
    float bestGain = 0.0f;
    ForEachBagItem(bot, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!CanWear(bot, proto))
            return;

        // The equip check itself, so that what this offers is what EquipUpgrade can do: proficiency, a two-hander
        // against an off-hand, a weapon swap in combat, a cast in progress.
        uint16 dest = 0;
        if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
            return;
        uint8 const slot = uint8(dest & 255);
        if (slot == NULL_SLOT)
            return;

        Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        float const gain = GearScore(proto, stats) - (worn ? GearScore(worn->GetTemplate(), stats) : 0.0f);
        if (gain > bestGain)
        {
            bestGain = gain;
            best = item;
        }
    });

    return best;
}

bool Animus::Curriculum::WorldActions::EquipUpgrade(Player* bot, StatProfile stats)
{
    Item* item = BestUpgrade(bot, stats);
    if (!item)
        return false;

    uint16 dest = 0;
    if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
        return false;

    // As the client's drag onto the character sheet: the worn item comes back to the bag slot.
    bot->SwapItem(item->GetPos(), dest);
    bot->AutoUnequipOffhandIfNeed();
    return true;
}

bool Animus::Curriculum::WorldActions::IsLootable(Player const* bot, Creature const* creature)
{
    if (!creature || creature->IsAlive() || !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
        return false;
    if (creature->loot.isLooted())
        return false;
    return !creature->hasLootRecipient() || creature->isTappedBy(bot);
}

bool Animus::Curriculum::WorldActions::IsSkinnable(Player const* bot, Creature const* creature)
{
    if (!creature || creature->IsAlive() || !creature->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        return false;

    uint32 const skill = creature->GetCreatureTemplate()->GetRequiredLootSkill();
    if (!bot->HasSkill(skill))
        return false;

    // The requirement Spell::CheckCast applies to a skinning cast.
    int32 const skillValue = bot->GetSkillValue(skill);
    int32 const level = creature->GetLevel();
    int32 const required = skillValue < 100 ? (level - 10) * 10 : level * 5;
    return skillValue >= required;
}

bool Animus::Curriculum::WorldActions::LootAll(Player* bot, Unit* source, uint32& items, uint32& copper)
{
    items = 0;
    copper = 0;
    Creature* creature = source ? source->ToCreature() : nullptr;
    if (!creature || !bot->IsAlive())
        return false;

    ObjectGuid const guid = creature->GetGUID();
    bool const skinning = bot->GetLootGUID() == guid && creature->loot.loot_type == LOOT_SKINNING;
    if (!skinning)
    {
        if (!IsLootable(bot, creature) || !bot->IsWithinDistInMap(creature, INTERACT_YARDS))
            return false;
        if (bot->GetLootGUID() != guid)
            bot->SendLoot(guid, LOOT_CORPSE);
        if (bot->GetLootGUID() != guid)
            return false;
    }

    TakeAll(bot, &creature->loot, items, copper);
    bot->GetSession()->DoLootRelease(guid);
    return items > 0 || copper > 0;
}

bool Animus::Curriculum::WorldActions::LootAll(Player* bot, GameObject* source, uint32& items, uint32& copper)
{
    items = 0;
    copper = 0;
    if (!source || !bot->IsAlive())
        return false;

    ObjectGuid const guid = source->GetGUID();
    // A node is opened by the gathering cast (Spell::EffectOpenLock -> GameObject::Use -> SendLoot); this takes
    // what it opened. A chest that needs no skill is opened here.
    if (bot->GetLootGUID() != guid)
    {
        if (NodeOf(source) != NodeKind::None || !bot->IsWithinDistInMap(source, INTERACT_YARDS))
            return false;
        bot->SendLoot(guid, LOOT_CORPSE);
        if (bot->GetLootGUID() != guid)
            return false;
    }

    TakeAll(bot, &source->loot, items, copper);
    bot->GetSession()->DoLootRelease(guid);
    return items > 0 || copper > 0;
}

uint32 Animus::Curriculum::WorldActions::OfferedQuest(Player* bot, Creature const* giver)
{
    if (!giver || !giver->IsAlive() || !giver->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
        return 0;

    QuestRelationBounds const quests = sObjectMgr->GetCreatureQuestRelationBounds(giver->GetEntry());
    for (auto it = quests.first; it != quests.second; ++it)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(it->second);
        if (!quest || bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE)
            continue;
        if (bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            return quest->GetQuestId();
    }

    return 0;
}

uint32 Animus::Curriculum::WorldActions::TurnInQuest(Player* bot, Creature const* giver)
{
    if (!giver || !giver->IsAlive() || !giver->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
        return 0;

    QuestRelationBounds const quests = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(giver->GetEntry());
    for (auto it = quests.first; it != quests.second; ++it)
        if (bot->GetQuestStatus(it->second) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(it->second))
            return it->second;

    return 0;
}

uint32 Animus::Curriculum::WorldActions::TakeQuest(Player* bot, Creature* giver)
{
    uint32 const questId = OfferedQuest(bot, giver);
    if (!questId || !bot->IsWithinDistInMap(giver, INTERACT_YARDS))
        return 0;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    bot->AddQuestAndCheckCompletion(quest, giver);
    return questId;
}

uint32 Animus::Curriculum::WorldActions::TurnIn(Player* bot, Creature* giver, StatProfile stats)
{
    uint32 const questId = TurnInQuest(bot, giver);
    if (!questId || !bot->IsWithinDistInMap(giver, INTERACT_YARDS))
        return 0;

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    // The reward: the choice that rates highest for the build among those the bot can wear, else the first.
    uint32 choice = 0;
    float best = -1.0f;
    for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
        if (!proto || !CanWear(bot, proto))
            continue;
        float const score = GearScore(proto, stats);
        if (score > best)
        {
            best = score;
            choice = i;
        }
    }

    // A full bag refuses the reward: the press was wasted, not the quest.
    if (!bot->CanRewardQuest(quest, choice, false))
        return 0;

    bot->RewardQuest(quest, choice, giver, true);
    return questId;
}

float Animus::Curriculum::WorldActions::QuestProgress(Player const* bot, uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return 0.0f;

    QuestStatus const status = bot->GetQuestStatus(questId);
    if (status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED)
        return 1.0f;
    if (status != QUEST_STATUS_INCOMPLETE)
        return 0.0f;

    uint32 have = 0;
    uint32 need = 0;
    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        if (quest->RequiredNpcOrGo[i] == 0 || quest->RequiredNpcOrGoCount[i] == 0)
            continue;
        need += quest->RequiredNpcOrGoCount[i];
        have += std::min<uint32>(quest->RequiredNpcOrGoCount[i],
            const_cast<Player*>(bot)->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]));
    }
    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        if (quest->RequiredItemId[i] == 0 || quest->RequiredItemCount[i] == 0)
            continue;
        need += quest->RequiredItemCount[i];
        have += std::min<uint32>(quest->RequiredItemCount[i], bot->GetItemCount(quest->RequiredItemId[i]));
    }

    return need ? float(have) / float(need) : 0.0f;
}

Animus::Curriculum::WorldActions::NodeKind Animus::Curriculum::WorldActions::NodeOf(GameObject const* object)
{
    if (!object || object->GetGoType() != GAMEOBJECT_TYPE_CHEST)
        return NodeKind::None;

    NodeKind kind = NodeKind::None;
    SkillOfLock(object->GetGOInfo()->GetLockId(), kind);
    return kind;
}

Animus::Curriculum::WorldActions::NodeKind Animus::Curriculum::WorldActions::NodeOfEntry(uint32 entry)
{
    GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
    if (!info || info->type != GAMEOBJECT_TYPE_CHEST)
        return NodeKind::None;

    NodeKind kind = NodeKind::None;
    SkillOfLock(info->GetLockId(), kind);
    return kind;
}

bool Animus::Curriculum::WorldActions::CanGather(Player const* bot, GameObject const* node)
{
    if (!node || !node->isSpawned())
        return false;

    NodeKind kind = NodeKind::None;
    uint32 const required = SkillOfLock(node->GetGOInfo()->GetLockId(), kind);
    if (kind == NodeKind::None)
        return false;

    uint32 const skill = kind == NodeKind::Herb ? SKILL_HERBALISM : SKILL_MINING;
    return bot->HasSkill(skill) && bot->GetSkillValue(skill) >= required && GatherSpell(bot, kind);
}

SpellInfo const* Animus::Curriculum::WorldActions::GatherSpell(Player const* bot, NodeKind kind)
{
    switch (kind)
    {
        case NodeKind::Herb: return HighestKnown(bot, PROFESSIONS[0]);
        case NodeKind::Mine: return HighestKnown(bot, PROFESSIONS[1]);
        default:             return nullptr;
    }
}

SpellInfo const* Animus::Curriculum::WorldActions::SkinningSpell(Player const* bot)
{
    return HighestKnown(bot, PROFESSIONS[2]);
}

bool Animus::Curriculum::WorldActions::Gather(Player* bot, GameObject* node)
{
    if (!CanGather(bot, node) || !bot->IsWithinDistInMap(node, INTERACT_YARDS))
        return false;

    SpellCastTargets targets;
    targets.SetGOTarget(node);
    return StartCast(bot, GatherSpell(bot, NodeOf(node)), targets);
}

bool Animus::Curriculum::WorldActions::Skin(Player* bot, Creature* corpse)
{
    if (!IsSkinnable(bot, corpse) || !bot->IsWithinDistInMap(corpse, INTERACT_YARDS))
        return false;

    SpellCastTargets targets;
    targets.SetUnitTarget(corpse);
    return StartCast(bot, SkinningSpell(bot), targets);
}

void Animus::Curriculum::WorldActions::LearnProfession(Player* bot, uint32 skill, uint16 value)
{
    Profession const* profession = ProfessionOf(skill);
    if (!profession)
        return;

    uint32 rank = 0;
    for (; rank < RANK_CAPS.size(); ++rank)
    {
        if (!bot->HasSpell(profession->Ranks[rank]))
            bot->learnSpell(profession->Ranks[rank]);
        if (value <= RANK_CAPS[rank])
            break;
    }

    uint16 const cap = RANK_CAPS[std::min<uint32>(rank, RANK_CAPS.size() - 1)];
    bot->SetSkill(uint16(skill), bot->GetSkillStep(uint16(skill)), std::min(value, cap), cap);
    if (profession->Tool && !bot->GetItemCount(profession->Tool))
        StoreInBags(bot, profession->Tool, 1);
}

uint32 Animus::Curriculum::WorldActions::JunkValue(Player const* bot)
{
    uint32 value = 0;
    ForEachBagItem(const_cast<Player*>(bot), [&value](Item* item)
    {
        if (IsJunk(item->GetTemplate()))
            value += item->GetTemplate()->SellPrice * item->GetCount();
    });
    return value;
}

uint32 Animus::Curriculum::WorldActions::SellJunk(Player* bot, Creature* vendor)
{
    if (!vendor || !bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR))
        return 0;
    if (vendor->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR))
        return 0;

    // HandleSellItemOpcode, without the buyback slot: what a player sells here is gone.
    std::vector<Item*> junk;
    ForEachBagItem(bot, [&junk](Item* item) { if (IsJunk(item->GetTemplate()) && !item->IsNotEmptyBag()) junk.push_back(item); });

    uint32 made = 0;
    for (Item* item : junk)
    {
        uint32 const money = item->GetTemplate()->SellPrice * item->GetCount();
        if (bot->GetMoney() >= MAX_MONEY_AMOUNT - money)
            break;
        bot->ModifyMoney(int32(money));
        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        made += money;
    }

    return made;
}

float Animus::Curriculum::WorldActions::Durability(Player const* bot)
{
    uint32 current = 0;
    uint32 max = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            continue;
        current += item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        max += item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    }

    return max ? float(current) / float(max) : 1.0f;
}

uint32 Animus::Curriculum::WorldActions::Repair(Player* bot, Creature* repairer)
{
    if (!repairer || !bot->GetNPCIfCanInteractWith(repairer->GetGUID(), UNIT_NPC_FLAG_REPAIR))
        return 0;

    // HandleRepairItemOpcode's "repair all" at a plain merchant: no reputation discount, no guild bank.
    return bot->DurabilityRepairAll(true, 1.0f, false);
}

namespace
{
    /// The food and drink slots of a vendor the bot can use, best (highest required level) first.
    struct Supply
    {
        uint32 Slot = 0;
        uint32 Item = 0;
        uint8 ReqLevel = 0;
    };

    void SuppliesOf(Player const* bot, Creature const* vendor, Supply& food, Supply& drink)
    {
        VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
        if (!items)
            return;

        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* entry = items->GetItem(slot);
            ItemTemplate const* proto = entry ? sObjectMgr->GetItemTemplate(entry->item) : nullptr;
            if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD
                || proto->RequiredLevel > bot->GetLevel() || entry->ExtendedCost || proto->RequiredSkill)
                continue;

            SpellInfo const* onUse = GearStats::ItemUseSpell(proto);
            if (!onUse)
                continue;

            Supply* which = onUse->HasAura(SPELL_AURA_MOD_REGEN) ? &food
                : onUse->HasAura(SPELL_AURA_MOD_POWER_REGEN) ? &drink : nullptr;
            if (which && (!which->Item || proto->RequiredLevel > which->ReqLevel))
                *which = { slot, proto->ItemId, uint8(proto->RequiredLevel) };
        }
    }
}

bool Animus::Curriculum::WorldActions::SellsSupplies(Player const* bot, Creature const* vendor)
{
    Supply food, drink;
    SuppliesOf(bot, vendor, food, drink);
    return food.Item || (drink.Item && bot->GetMaxPower(POWER_MANA) > 0);
}

uint32 Animus::Curriculum::WorldActions::BuySupplies(Player* bot, Creature* vendor, uint32 count)
{
    if (!vendor || !bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR))
        return 0;

    Supply food, drink;
    SuppliesOf(bot, vendor, food, drink);
    if (bot->GetMaxPower(POWER_MANA) == 0)
        drink = Supply();

    uint32 bought = 0;
    for (Supply const& supply : { food, drink })
    {
        if (!supply.Item)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(supply.Item);
        uint32 const have = bot->GetItemCount(supply.Item);
        if (have >= count)
            continue;

        // BuyItemFromVendorSlot buys `count` lots of the template's BuyCount.
        uint32 const lot = std::max<uint32>(1, proto->BuyCount);
        uint32 const lots = (count - have + lot - 1) / lot;
        // Its return value says whether the vendor's stock changed (false for an unlimited item), not whether
        // the purchase went through: the bags say that.
        uint32 const before = bot->GetItemCount(supply.Item);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), supply.Slot, supply.Item, uint8(std::min<uint32>(lots, 5)),
            NULL_BAG, NULL_SLOT);
        uint32 const after = bot->GetItemCount(supply.Item);
        bool const ok = after > before;
        if (ok)
            bought += after - before;
    }

    return bought;
}

void Animus::Curriculum::WorldActions::CountSupplies(Player const* bot, uint32& food, uint32& drink)
{
    food = 0;
    drink = 0;
    ForEachBagItem(const_cast<Player*>(bot), [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD)
            return;
        SpellInfo const* onUse = GearStats::ItemUseSpell(proto);
        if (!onUse)
            return;
        if (onUse->HasAura(SPELL_AURA_MOD_REGEN))
            food += item->GetCount();
        else if (onUse->HasAura(SPELL_AURA_MOD_POWER_REGEN))
            drink += item->GetCount();
    });
}

void Animus::Curriculum::WorldActions::DropSupplies(Player* bot, uint32 keep)
{
    std::vector<std::pair<uint32, uint32>> drop;    // (item, count to destroy)
    std::unordered_map<uint32, uint32> have;
    ForEachBagItem(bot, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD)
            return;
        SpellInfo const* onUse = GearStats::ItemUseSpell(proto);
        if (onUse && (onUse->HasAura(SPELL_AURA_MOD_REGEN) || onUse->HasAura(SPELL_AURA_MOD_POWER_REGEN)))
            have[proto->ItemId] += item->GetCount();
    });
    for (auto const& [item, count] : have)
        if (count > keep)
            drop.emplace_back(item, count - keep);
    for (auto const& [item, count] : drop)
        bot->DestroyItemCount(item, count, true);
}

void Animus::Curriculum::WorldActions::Sense(Player* bot, float radius, WorldView& world)
{
    world = WorldView();
    world.Active = true;
    if (!bot || !bot->IsInWorld())
        return;

    std::list<Creature*> creatures;
    Acore::AllWorldObjectsInRange check(bot, radius);
    Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> creatureSearcher(bot, creatures, check);
    Cell::VisitObjects(bot, creatureSearcher, radius);

    float corpseDistance = 0.0f, giverDistance = 0.0f, vendorDistance = 0.0f;
    bool vendorBusiness = false;
    bool const junk = JunkValue(bot) > 0;
    bool const worn = bot->GetMoney() > 0 && Durability(bot) < 1.0f;
    uint32 foodCount = 0, drinkCount = 0;
    CountSupplies(bot, foodCount, drinkCount);
    bool const short_ = foodCount < CONSUMABLE_COUNT
        || (bot->GetMaxPower(POWER_MANA) > 0 && drinkCount < CONSUMABLE_COUNT);
    bool corpseLootable = false;
    for (Creature* creature : creatures)
    {
        float const distance = bot->GetExactDist2d(creature);
        if (!creature->IsAlive())
        {
            // A corpse to loot beats one to skin; the nearest of each kind.
            bool const lootable = IsLootable(bot, creature);
            bool const skinnable = !lootable && IsSkinnable(bot, creature);
            if (!lootable && !skinnable)
                continue;
            if (!world.Corpse || (lootable && !corpseLootable) || (lootable == corpseLootable && distance < corpseDistance))
            {
                world.Corpse = creature;
                world.CorpseSkinnable = skinnable;
                corpseLootable = lootable;
                corpseDistance = distance;
                world.CorpseQuestItem = false;
                for (LootItem const& item : creature->loot.quest_items)
                    if (bot->HasQuestForItem(item.itemid))
                        world.CorpseQuestItem = true;
            }
            continue;
        }

        if (creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) && (!world.Giver || distance < giverDistance))
        {
            bool const turnIn = TurnInQuest(bot, creature) != 0;
            bool const offers = !turnIn && OfferedQuest(bot, creature) != 0;
            if (turnIn || offers)
            {
                world.Giver = creature;
                world.GiverOffers = offers;
                world.GiverTurnIn = turnIn;
                giverDistance = distance;
            }
        }
        if (creature->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_VENDOR_MASK | UNIT_NPC_FLAG_REPAIR))
            && creature->GetReactionTo(bot) > REP_UNFRIENDLY)
        {
            // The vendor slot goes to the nearest trader with business to do: one that buys when there is junk,
            // repairs when something is worn, sells food or drink when the bags run low. Without any, the nearest.
            bool const repairs = creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR);
            bool const sells = creature->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_VENDOR_MASK))
                && !creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR);
            bool const business = (junk && sells) || (worn && repairs) || (short_ && SellsSupplies(bot, creature));
            if (!world.Vendor || (business && !vendorBusiness)
                || (business == vendorBusiness && distance < vendorDistance))
            {
                world.Vendor = creature;
                world.VendorRepairs = repairs;
                vendorDistance = distance;
                vendorBusiness = business;
            }
        }
    }

    std::list<GameObject*> objects;
    Acore::GameObjectListSearcher<Acore::AllWorldObjectsInRange> objectSearcher(bot, objects, check);
    Cell::VisitObjects(bot, objectSearcher, radius);
    float nodeDistance = 0.0f;
    for (GameObject* object : objects)
    {
        NodeKind const kind = NodeOf(object);
        if (kind == NodeKind::None || !object->isSpawned())
            continue;
        float const distance = bot->GetExactDist2d(object);
        if (!world.Node || distance < nodeDistance)
        {
            world.Node = object;
            world.NodeKind = kind;
            world.NodeOpenable = CanGather(bot, object);
            nodeDistance = distance;
        }
    }
}
