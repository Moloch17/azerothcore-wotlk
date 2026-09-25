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

#include "WorldBlock.h"
#include "Log.h"
#include "Creature.h"
#include "GameObject.h"
#include "Layout.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "Supplies.h"
#include "WorldActions.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    /// (present, distance, bearing) of a thing at `where`, in the seat's own frame.
    void Place(SeatView const& view, WorldObject const* thing, float* obs, uint32 first)
    {
        Player const* bot = view.Bot;
        obs[first] = 1.0f;
        obs[first + 1] = std::min(1.0f, bot->GetExactDist2d(thing) / WorldBlock::RANGE);
        float const relative = bot->GetAngle(thing->GetPositionX(), thing->GetPositionY()) - view.Facing;
        float const bearing = std::atan2(std::sin(relative), std::cos(relative));
        obs[first + 2] = std::sin(bearing);
        obs[first + 3] = std::cos(bearing);
    }

    bool InReach(Player const* bot, WorldObject const* thing)
    {
        return thing && bot->IsWithinDistInMap(thing, WorldActions::INTERACT_YARDS);
    }

    StatProfile StatsOf(SeatView const& view)
    {
        return view.L && view.L->Profile ? view.L->Profile->Specs[view.Spec].Stats : StatProfile::StrengthMelee;
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        WorldView const& world = view.World;
        if (!world.Active || !bot->IsAlive())
            return false;

        bool const casting = bot->IsNonMeleeSpellCast(false);
        switch (action)
        {
            case WorldBlock::ACTION_INTERACT:
                if (casting)
                    return false;
                return (world.Giver && (world.GiverOffers || world.GiverTurnIn) && InReach(bot, world.Giver))
                    || (world.Node && world.NodeOpenable && InReach(bot, world.Node))
                    || (world.Corpse && InReach(bot, world.Corpse));
            case WorldBlock::ACTION_LOOT_ALL:
                return !bot->GetLootGUID().IsEmpty() || (world.Corpse && !world.CorpseSkinnable
                    && InReach(bot, world.Corpse));
            case WorldBlock::ACTION_EQUIP_UPGRADE:
                return !bot->IsInCombat() && WorldActions::BestUpgrade(bot, StatsOf(view)) != nullptr;
            case WorldBlock::ACTION_SELL_JUNK:
                return world.Vendor && InReach(bot, world.Vendor) && WorldActions::JunkValue(bot) > 0;
            case WorldBlock::ACTION_REPAIR:
                return world.Vendor && world.VendorRepairs && InReach(bot, world.Vendor) && bot->GetMoney() > 0
                    && WorldActions::Durability(bot) < 1.0f;
            case WorldBlock::ACTION_BUY_SUPPLIES:
            {
                if (!world.Vendor || !InReach(bot, world.Vendor) || !bot->GetMoney())
                    return false;
                uint32 food = 0, drink = 0;
                WorldActions::CountSupplies(bot, food, drink);
                bool const wantsDrink = bot->GetMaxPower(POWER_MANA) > 0;
                return (food < CONSUMABLE_COUNT || (wantsDrink && drink < CONSUMABLE_COUNT))
                    && WorldActions::SellsSupplies(bot, world.Vendor);
            }
            default:
                return false;
        }
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::WorldBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

std::string Animus::Curriculum::WorldBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "interact", "loot_all", "equip_upgrade", "sell_junk", "repair", "buy_supplies",
    };

    return local < NAMES.size() ? NAMES[local] : std::string();
}

void Animus::Curriculum::WorldBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    WorldView const& world = view.World;

    if (world.Corpse)
    {
        Place(view, world.Corpse, obs, OBS_CORPSE);
        obs[OBS_CORPSE_QUEST_ITEM] = world.CorpseQuestItem ? 1.0f : 0.0f;
        obs[OBS_CORPSE_SKINNABLE] = world.CorpseSkinnable ? 1.0f : 0.0f;
        obs[OBS_CORPSE_IN_REACH] = InReach(bot, world.Corpse) ? 1.0f : 0.0f;
    }
    if (world.Giver)
    {
        Place(view, world.Giver, obs, OBS_GIVER);
        obs[OBS_GIVER_OFFERS] = world.GiverOffers ? 1.0f : 0.0f;
        obs[OBS_GIVER_TURN_IN] = world.GiverTurnIn ? 1.0f : 0.0f;
        obs[OBS_GIVER_IN_REACH] = InReach(bot, world.Giver) ? 1.0f : 0.0f;
    }
    if (world.Node)
    {
        Place(view, world.Node, obs, OBS_NODE);
        obs[OBS_NODE_HERB] = world.NodeKind == WorldActions::NodeKind::Herb ? 1.0f : 0.0f;
        obs[OBS_NODE_MINE] = world.NodeKind == WorldActions::NodeKind::Mine ? 1.0f : 0.0f;
        obs[OBS_NODE_OPENABLE] = world.NodeOpenable ? 1.0f : 0.0f;
        obs[OBS_NODE_IN_REACH] = InReach(bot, world.Node) ? 1.0f : 0.0f;
    }
    if (world.Vendor)
    {
        Place(view, world.Vendor, obs, OBS_VENDOR);
        obs[OBS_VENDOR_REPAIRS] = world.VendorRepairs ? 1.0f : 0.0f;
        obs[OBS_VENDOR_IN_REACH] = InReach(bot, world.Vendor) ? 1.0f : 0.0f;
    }

    obs[OBS_QUEST_NONE] = world.QuestState == WorldView::QUEST_NONE ? 1.0f : 0.0f;
    obs[OBS_QUEST_ACTIVE] = world.QuestState == WorldView::QUEST_ACTIVE ? 1.0f : 0.0f;
    obs[OBS_QUEST_COMPLETE] = world.QuestState == WorldView::QUEST_COMPLETE ? 1.0f : 0.0f;
    obs[OBS_QUEST_PROGRESS] = world.QuestProgress;

    obs[OBS_BAG_FREE] = std::min(1.0f, float(bot->GetFreeInventorySpace()) / 16.0f);
    obs[OBS_GOLD] = std::min(1.0f, std::log10(float(bot->GetMoney()) + 1.0f) / 7.0f);
    obs[OBS_DURABILITY] = WorldActions::Durability(bot);
    obs[OBS_HAS_UPGRADE] = WorldActions::BestUpgrade(bot, StatsOf(view)) ? 1.0f : 0.0f;
    obs[OBS_HAS_JUNK] = WorldActions::JunkValue(bot) > 0 ? 1.0f : 0.0f;
    uint32 food = 0, drink = 0;
    WorldActions::CountSupplies(bot, food, drink);
    obs[OBS_FOOD] = std::min(1.0f, float(food) / float(CONSUMABLE_COUNT));
    obs[OBS_DRINK] = std::min(1.0f, float(drink) / float(CONSUMABLE_COUNT));
    obs[OBS_LOOT_OPEN] = bot->GetLootGUID().IsEmpty() ? 0.0f : 1.0f;
    obs[OBS_CASTING] = bot->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;

    for (uint32 action = 0; mask && action < ACTION_COUNT; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::WorldBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;
    WorldView const& world = view.World;
    StatProfile const stats = StatsOf(view);
    uint32 items = 0;
    uint32 copper = 0;

    switch (local)
    {
        case ACTION_INTERACT:
        {
            // The nearest thing in reach, and what a right-click on it does.
            struct Choice { WorldObject* Thing; float Distance; };
            Choice best{ nullptr, 0.0f };
            auto const consider = [&](WorldObject* thing)
            {
                if (!InReach(bot, thing))
                    return;
                float const distance = bot->GetExactDist2d(thing);
                if (!best.Thing || distance < best.Distance)
                    best = { thing, distance };
            };
            if (world.Giver && (world.GiverOffers || world.GiverTurnIn))
                consider(world.Giver);
            if (world.Node && world.NodeOpenable)
                consider(world.Node);
            if (world.Corpse)
                consider(world.Corpse);

            if (!best.Thing)
                return;
            ++result.Interactions;
            if (best.Thing == world.Giver)
            {
                if (world.GiverTurnIn && WorldActions::TurnIn(bot, world.Giver, stats))
                    result.QuestTurnedIn = true;
                else if (world.GiverOffers && WorldActions::TakeQuest(bot, world.Giver))
                    result.QuestAccepted = true;
                else
                    ++result.Wasted;
            }
            else if (best.Thing == world.Node)
            {
                if (WorldActions::Gather(bot, world.Node))
                    ++result.GatherCasts;
                else
                    ++result.Wasted;
            }
            else if (world.CorpseSkinnable)
            {
                if (WorldActions::Skin(bot, world.Corpse->ToCreature()))
                    ++result.GatherCasts;
                else
                    ++result.Wasted;
            }
            else if (WorldActions::LootAll(bot, world.Corpse, items, copper))
            {
                result.ItemsLooted += items;
                result.CopperLooted += copper;
                ++result.CorpsesLooted;
            }
            else
                ++result.Wasted;
            return;
        }
        case ACTION_LOOT_ALL:
        {
            // Whatever is open (a gathered node, a skinned or opened corpse), else the corpse in reach.
            ObjectGuid const open = bot->GetLootGUID();
            bool done = false;
            if (open.IsGameObject())
            {
                if (WorldActions::LootAll(bot, bot->GetMap()->GetGameObject(open), items, copper))
                {
                    done = true;
                    ++result.NodesLooted;
                }
            }
            else if (!open.IsEmpty())
            {
                if (WorldActions::LootAll(bot, ObjectAccessor::GetUnit(*bot, open), items, copper))
                {
                    done = true;
                    ++result.CorpsesLooted;
                }
            }
            else if (world.Corpse && WorldActions::LootAll(bot, world.Corpse, items, copper))
            {
                done = true;
                ++result.CorpsesLooted;
            }

            if (done)
            {
                result.ItemsLooted += items;
                result.CopperLooted += copper;
            }
            else
                ++result.Wasted;
            return;
        }
        case ACTION_EQUIP_UPGRADE:
            if (WorldActions::EquipUpgrade(bot, stats))
                ++result.Equipped;
            else
                ++result.Wasted;
            return;
        case ACTION_SELL_JUNK:
            if (uint32 const made = WorldActions::SellJunk(bot, world.Vendor))
                result.CopperSold += made;
            else
                ++result.Wasted;
            return;
        case ACTION_REPAIR:
        {
            float const before = WorldActions::Durability(bot);
            result.CopperRepaired += WorldActions::Repair(bot, world.Vendor);
            if (WorldActions::Durability(bot) > before)
                ++result.Repairs;
            else
                ++result.Wasted;
            return;
        }
        case ACTION_BUY_SUPPLIES:
            if (uint32 const bought = WorldActions::BuySupplies(bot, world.Vendor, CONSUMABLE_COUNT))
                result.SuppliesBought += bought;
            else
                ++result.Wasted;
            return;
        default:
            return;
    }
}
