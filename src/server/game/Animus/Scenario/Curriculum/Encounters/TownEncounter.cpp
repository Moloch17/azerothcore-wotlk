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

#include "LifeEncounter.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "GearStats.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "Supplies.h"
#include "WorldActions.h"
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace Animus::Curriculum;
    using Animus::Env;

    constexpr uint32 SALT_INN = 23;
    constexpr uint32 SALT_JUNK = 29;
    constexpr uint32 SALT_UPGRADE = 31;
    constexpr uint32 JUNK_KINDS = 3;                // different grey items in the bags at the start
    constexpr uint32 UPGRADES = 2;                  // better items in the bags at the start
    constexpr float DAMAGE_SHARE = 0.5;             // of the gear's durability lost at the start
    constexpr uint32 SUPPLIES_KEPT = 1;             // food and drink left at the start
    /// The NPCs a town episode copies into the phase: whoever a player trades with.
    constexpr uint32 TOWN_NPC_FLAGS = UNIT_NPC_FLAG_VENDOR_MASK | UNIT_NPC_FLAG_VENDOR_FOOD | UNIT_NPC_FLAG_REPAIR
        | UNIT_NPC_FLAG_INNKEEPER;
    /// How far a missing repairer or food vendor is fetched from, and how far from the inn it is stood.
    constexpr float MISSING_TRADER_REACH = 1500.0f;
    constexpr float MISSING_TRADER_YARDS = 8.0f;

    /// Whether the creature template's vendor list has food or drink a character of `level` can use.
    bool SellsFood(uint32 entry, uint8 level)
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            return false;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (proto && proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD
                && proto->RequiredLevel <= level && !item->ExtendedCost)
                return true;
        }
        return false;
    }

    /// Grey items with a vendor price, once: what the seat arrives with.
    std::vector<uint32> const& JunkItems()
    {
        static std::vector<uint32> const junk = []
        {
            std::vector<uint32> out;
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                if (proto.Quality == ITEM_QUALITY_POOR && proto.SellPrice > 0 && proto.RequiredLevel <= 1
                    && (proto.Class == ITEM_CLASS_MISC || proto.Class == ITEM_CLASS_TRADE_GOODS)
                    && proto.MaxCount == 0 && proto.Bonding == NO_BIND)
                    out.push_back(entry);
            std::sort(out.begin(), out.end());
            return out;
        }();
        return junk;
    }

    /// Wearable items a player can get, once: the upgrades are drawn from them.
    std::vector<ItemTemplate const*> const& WearableItems()
    {
        static std::vector<ItemTemplate const*> const items = []
        {
            std::vector<ItemTemplate const*> out;
            std::unordered_set<uint32> const& obtainable = GearStats::ObtainableItems();
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                if ((proto.Class == ITEM_CLASS_ARMOR || proto.Class == ITEM_CLASS_WEAPON)
                    && proto.Quality >= ITEM_QUALITY_UNCOMMON && proto.Quality <= ITEM_QUALITY_RARE
                    && obtainable.contains(entry))
                    out.push_back(&proto);
            std::sort(out.begin(), out.end(), [](ItemTemplate const* a, ItemTemplate const* b)
            {
                return a->ItemId < b->ItemId;
            });
            return out;
        }();
        return items;
    }

    /// An item that rates higher than what `bot` wears in a random slot, for its build: the first found from a
    /// seeded start in the wearable list.
    ItemTemplate const* FindUpgrade(Env const& env, Player* bot, StatProfile stats, uint32 salt)
    {
        std::vector<ItemTemplate const*> const& items = WearableItems();
        if (items.empty())
            return nullptr;

        uint32 const start = LifeWorld::Draw(env, uint32(items.size()), salt);
        for (uint32 i = 0; i < items.size(); ++i)
        {
            ItemTemplate const* proto = items[(start + i) % items.size()];
            if (proto->RequiredLevel > bot->GetLevel() || proto->RequiredLevel + 10 < bot->GetLevel()
                || !WorldActions::CanWear(bot, proto))
                continue;
            uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot == NULL_SLOT)
                continue;
            Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            float const gain = WorldActions::GearScore(proto, stats)
                - (worn ? WorldActions::GearScore(worn->GetTemplate(), stats) : 0.0f);
            if (gain > 5.0f)
                return proto;
        }
        return nullptr;
    }
}

Animus::Curriculum::TownEncounter::TownEncounter(StageScenario& scenario, uint32 envs)
    : LifeEncounter(scenario, envs, "town"), _towns(envs)
{
    JunkItems();
    WearableItems();
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::TownEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::Progress, RewardTerm::Death, RewardTerm::Wasted,
        RewardTerm::TownSold, RewardTerm::TownRepaired, RewardTerm::TownStocked, RewardTerm::TownEquipped,
        RewardTerm::TownDone };
}

void Animus::Curriculum::TownEncounter::AddMoreEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("town_inn", [this](Env const& env, uint32) { return float(_towns[env.Index].Inn); });
    table.Add("town_npcs", [this](Env const& env, uint32) { return float(_towns[env.Index].Npcs); });
    table.Add("copper_sold", [this](Env const& env, uint32) { return float(_towns[env.Index].CopperSold); });
    table.Add("junk_at_start", [this](Env const& env, uint32) { return float(_towns[env.Index].JunkAtStart); });
    table.Add("sold_out", [this](Env const& env, uint32) { return _towns[env.Index].SoldOut ? 1.0f : 0.0f; });
    table.Add("repaired", [this](Env const& env, uint32) { return _towns[env.Index].Repaired ? 1.0f : 0.0f; });
    table.Add("copper_repaired", [this](Env const& env, uint32) { return float(_towns[env.Index].CopperRepaired); });
    table.Add("stocked", [this](Env const& env, uint32) { return _towns[env.Index].Stocked ? 1.0f : 0.0f; });
    table.Add("supplies_bought", [this](Env const& env, uint32) { return float(_towns[env.Index].SuppliesBought); });
    table.Add("upgrades_equipped", [this](Env const& env, uint32) { return float(_towns[env.Index].Equipped); });
    table.Add("upgrades_given", [this](Env const& env, uint32) { return float(_towns[env.Index].Upgrades); });
}

bool Animus::Curriculum::TownEncounter::Place(Env& env, EnvLife& life)
{
    EnvTown& town = _towns[env.Index];
    town = EnvTown();

    std::vector<uint32> const& inns = LifeWorld::TownInns(life.Tier, life.Side);
    LifeWorld::SpawnIndex const& spawns = LifeWorld::SpawnIndex::Instance();
    std::vector<LifeWorld::Spawn const*> places;
    for (uint32 inn : inns)
        for (LifeWorld::Spawn const* spawn : spawns.CreaturesOfEntry(inn))
            places.push_back(spawn);
    if (places.empty())
        return false;

    LifeWorld::Spawn const* inn = places[LifeWorld::Draw(env, uint32(places.size()), SALT_INN)];
    town.Inn = inn->Entry;
    EnvState& data = _scenario.Data(env);
    data.EpisodeMapId = inn->Map;
    data.HasEpisodeMap = true;
    data.EpisodeSpawn.Relocate(inn->Pos);
    data.HasEpisodeSpawn = true;
    return true;
}

bool Animus::Curriculum::TownEncounter::Build(Env& env, Map* map, uint8 level)
{
    EnvLife& life = _envs[env.Index];
    EnvTown& town = _towns[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    if (!town.Inn || !bot || !map)
        return false;

    // The town's traders around the inn.
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    std::vector<LifeWorld::Spawn const*> spawns;
    LifeWorld::SpawnIndex::Instance().CreaturesNear(map->GetId(), bot->GetPositionX(), bot->GetPositionY(),
        tuning.TownRadius, spawns);
    for (LifeWorld::Spawn const* spawn : spawns)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn->Entry);
        if (!info || !(info->npcflag & TOWN_NPC_FLAGS) || !LifeWorld::IsWorldCreature(*spawn, true))
            continue;
        if (Summon(env, life, map, *spawn))
            ++town.Npcs;
    }
    if (!town.Npcs)
        return false;

    // A town is only a town with someone to repair and someone selling food and drink: when the inn's radius
    // holds neither, the nearest such trader of the map is copied in beside the inn.
    bool repairer = false;
    bool supplier = false;
    for (ObjectGuid const& guid : life.Spawned)
        if (Creature* npc = guid.IsCreature() ? ObjectAccessor::GetCreature(*bot, guid) : nullptr)
        {
            repairer = repairer || npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR);
            supplier = supplier || WorldActions::SellsSupplies(bot, npc);
        }
    if (!repairer || !supplier)
    {
        std::vector<LifeWorld::Spawn const*> far;
        LifeWorld::SpawnIndex::Instance().CreaturesNear(map->GetId(), bot->GetPositionX(), bot->GetPositionY(),
            MISSING_TRADER_REACH, far);
        for (LifeWorld::Spawn const* spawn : far)
        {
            if (repairer && supplier)
                break;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn->Entry);
            if (!info || !LifeWorld::IsWorldCreature(*spawn, true))
                continue;
            bool const wantRepairer = !repairer && (info->npcflag & UNIT_NPC_FLAG_REPAIR);
            bool const wantSupplier = !supplier && (info->npcflag & UNIT_NPC_FLAG_VENDOR_MASK)
                && SellsFood(spawn->Entry, level);
            if (!wantRepairer && !wantSupplier)
                continue;
            LifeWorld::Spawn beside = *spawn;
            float const side = repairer ? -1.0f : 1.0f;
            beside.Pos.Relocate(bot->GetPositionX() + side * MISSING_TRADER_YARDS, bot->GetPositionY(),
                bot->GetPositionZ(), bot->GetOrientation());
            if (Creature* npc = Summon(env, life, map, beside))
            {
                ++town.Npcs;
                repairer = repairer || wantRepairer;
                supplier = supplier || wantSupplier;
                LOG_INFO("module.animus", "{}: env {}: {} ({}) brought to the inn ({} / {})", _scenario.Name(),
                    env.Index, npc->GetName(), npc->GetEntry(), wantRepairer ? "repairs" : "-",
                    wantSupplier ? "food" : "-");
            }
        }
    }

    // What the seat arrives with: gold for the level, junk, damaged gear, one food and one drink, upgrades.
    bot->SetMoney(uint32(level) * uint32(level) * tuning.TownCopperPerLevelSquared);
    std::vector<uint32> const& junk = JunkItems();
    for (uint32 kind = 0; kind < JUNK_KINDS && !junk.empty(); ++kind)
        StoreInBags(bot, junk[LifeWorld::Draw(env, uint32(junk.size()), SALT_JUNK + kind)], 1 + kind);
    town.JunkAtStart = WorldActions::JunkValue(bot);

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DurabilityLoss(item, DAMAGE_SHARE);

    WorldActions::DropSupplies(bot, SUPPLIES_KEPT);

    SeatState const& seat = _scenario.Data(env).Seats[0];
    StatProfile const stats = seat.L ? seat.L->Profile->Specs[seat.Spec].Stats : StatProfile::StrengthMelee;
    for (uint32 i = 0; i < UPGRADES; ++i)
        if (ItemTemplate const* proto = FindUpgrade(env, bot, stats, SALT_UPGRADE + i))
            if (StoreInBags(bot, proto->ItemId, 1))
            {
                // Only an upgrade the seat can put on counts: the template says wearable, the item says equippable.
                Item* stored = bot->GetItemByEntry(proto->ItemId);
                uint16 dest = 0;
                if (stored && bot->CanEquipItem(NULL_SLOT, dest, stored, true) == EQUIP_ERR_OK)
                    ++town.Upgrades;
                else
                    bot->DestroyItemCount(proto->ItemId, 1, true);
            }

    return true;
}

void Animus::Curriculum::TownEncounter::Update(Env& env)
{
    EnvLife& life = _envs[env.Index];
    EnvTown& town = _towns[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !bot->IsAlive())
        return;

    town.SoldOut = WorldActions::JunkValue(bot) == 0;
    town.Repaired = WorldActions::Durability(bot) >= 1.0f;
    uint32 food = 0, drink = 0;
    WorldActions::CountSupplies(bot, food, drink);
    bool const wantsDrink = bot->GetMaxPower(POWER_MANA) > 0;
    town.Stocked = food >= CONSUMABLE_COUNT && (!wantsDrink || drink >= CONSUMABLE_COUNT);

    // The nearest trader the seat still has business with.
    bool const business = !town.SoldOut || !town.Repaired || !town.Stocked;
    if (business)
    {
        WorldView sense;
        WorldActions::Sense(bot, _scenario.Tuning().Life.SenseRange, sense);
        if (sense.Vendor)
            SetWaypoint(life, 1, sense.Vendor->GetPosition());
        else
            ClearWaypoint(life);
    }
    else
        ClearWaypoint(life);
}

void Animus::Curriculum::TownEncounter::Sensed(Env const& /*env*/, EnvLife const& /*life*/, SeatView& /*view*/) const
{
}

void Animus::Curriculum::TownEncounter::Account(Env& env, EnvLife& /*life*/, SeatActionResult const& result)
{
    EnvTown& town = _towns[env.Index];
    town.CopperSold += result.CopperSold;
    town.Repairs += result.Repairs;
    town.CopperRepaired += result.CopperRepaired;
    town.SuppliesBought += result.SuppliesBought;
    town.Equipped += result.Equipped;
}

void Animus::Curriculum::TownEncounter::RewardMore(Env& env, EnvLife& life, Player* /*bot*/, RewardLedger& ledger)
{
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    EnvTown& town = _towns[env.Index];

    // Sold: the share of the starting junk's value realised (a partial sale pays its part).
    if (town.CopperSold > town.SoldPaidCopper && town.JunkAtStart)
    {
        ledger.Add(RewardTerm::TownSold, tuning.TownSold * float(town.CopperSold - town.SoldPaidCopper)
            / float(town.JunkAtStart));
        town.SoldPaidCopper = town.CopperSold;
    }
    if (town.Repaired && !town.RepairPaid)
    {
        town.RepairPaid = true;
        ledger.Add(RewardTerm::TownRepaired, tuning.TownRepaired);
    }
    if (town.Stocked && !town.StockPaid)
    {
        town.StockPaid = true;
        ledger.Add(RewardTerm::TownStocked, tuning.TownStocked);
    }
    if (town.Equipped > town.EquippedPaid)
    {
        ledger.Add(RewardTerm::TownEquipped, tuning.TownEquipped * float(town.Equipped - town.EquippedPaid));
        town.EquippedPaid = town.Equipped;
    }
    if (!life.OutcomePaid && Finished(env, life))
        ledger.Add(RewardTerm::TownDone, tuning.TownDone);
}

bool Animus::Curriculum::TownEncounter::Finished(Env const& env, EnvLife const& /*life*/) const
{
    EnvTown const& town = _towns[env.Index];
    return town.SoldOut && town.Repaired && town.Stocked && town.Equipped >= town.Upgrades;
}
