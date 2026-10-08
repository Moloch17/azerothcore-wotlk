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

#ifndef ANIMUS_LIB_CURRICULUM_SUPPLIES_H
#define ANIMUS_LIB_CURRICULUM_SUPPLIES_H

#include "ClassProfile.h"
#include "Define.h"
#include <map>
#include <utility>
#include <vector>

class Player;

/*
 * What a class/role character carries beyond its kit and gear: food and drink, potions, bandages, a warlock's
 * healthstones and soulstone, the flask or elixir it drank before the fight, and a hunter's stable of beasts to call.
 */
namespace Animus::Curriculum
{
    /// How many of each food and drink a character carries.
    constexpr uint32 CONSUMABLE_COUNT = 5;

    /// Hunters get Tame Beast and Call Pet at this level.
    constexpr uint8 HUNTER_PET_LEVEL = 10;

    /// What a character brings to a fight, by item entry (0: none).
    struct BattleSupplies
    {
        uint32 HealthPotion = 0;
        uint32 ManaPotion = 0;
        uint32 Healthstone = 0;
        uint32 Bandage = 0;
        uint32 Soulstone = 0;
    };

    /// Consumables a player can get, by the level (or First Aid skill) they need: vendor-sold food (health
    /// regeneration) and drink (mana regeneration), healing and mana potions, bandages, warlock healthstones and
    /// soulstones, and flasks and elixirs.
    class ConsumablePool
    {
    public:
        static ConsumablePool const& Instance();

        /// The best food / drink a character of `level` can use, or 0.
        [[nodiscard]] uint32 Food(uint8 level) const { return Best(_food, level); }
        [[nodiscard]] uint32 Drink(uint8 level) const { return Best(_drink, level); }

        /// The best potions, bandage (for the First Aid skill of the level) and conjured stones of `level`. Mana
        /// potions only for mana users, healthstones for warlocks and the parties they are in, soulstones for
        /// warlocks.
        [[nodiscard]] BattleSupplies Supplies(uint8 level, bool usesMana, bool warlock, bool warlockInParty) const;

        /// The spell of the flask (at the level caps) or elixir (while levelling, now and then) a character of
        /// `level` with `stats` gear drinks before a fight, or 0.
        [[nodiscard]] uint32 BuffSpell(uint8 level, StatProfile stats) const;

        /// The First Aid skill a character of `level` has (as a player who kept it up would).
        [[nodiscard]] static uint16 FirstAidSkill(uint8 level);

    private:
        struct Buff
        {
            uint8 ReqLevel = 0;
            uint16 ItemLevel = 0;
            uint32 SpellId = 0;
            bool Flask = false;
        };

        ConsumablePool();

        [[nodiscard]] static uint32 Best(std::vector<std::pair<uint8, uint32>> const& items, uint8 level);

        std::vector<std::pair<uint8, uint32>> _food;     // (required level, item), sorted
        std::vector<std::pair<uint8, uint32>> _drink;
        std::vector<std::pair<uint8, uint32>> _healthPotions;
        std::vector<std::pair<uint8, uint32>> _manaPotions;
        std::vector<std::pair<uint8, uint32>> _healthstones;
        std::vector<std::pair<uint8, uint32>> _soulstones;
        std::vector<std::pair<uint16, uint32>> _bandages;   // (First Aid skill, item), sorted
        std::map<StatProfile, std::vector<Buff>> _buffs;
    };

    /// Tameable, non-exotic beasts spawned somewhere in the world, by family.
    class StablePool
    {
    public:
        static StablePool const& Instance();

        /// Beast entries of `count` different random families: a hunter's stable. Fewer if the world has fewer
        /// families.
        [[nodiscard]] std::vector<uint32> Random(uint32 count) const;

    private:
        StablePool();

        std::vector<std::vector<uint32>> _beastsByFamily;
    };

    /// Put `count` new items in the bot's bags, all or nothing; false when they do not fit. Unlike
    /// Player::StoreNewItemInBestSlots, a full bag is not logged as an error.
    bool StoreInBags(Player* bot, uint32 itemId, uint32 count);

    /// Top the bot's food and drink up to `count` each; 0 skips one.
    void StockConsumables(Player* bot, uint32 food, uint32 drink, uint32 count = CONSUMABLE_COUNT);

    /// Put the supplies in the bot's bags (potions and bandages CONSUMABLE_COUNT each, one healthstone and one
    /// soulstone), teach it the First Aid its bandage needs, and drink its flask or elixir.
    void StockBattleSupplies(Player* bot, BattleSupplies const& supplies, StatProfile stats);

    /// Hunters: bring the stabled beast `entry` out as the bot's pet (Call Pet for a pet that only exists in memory;
    /// Call Pet itself loads pets from the database). Needs level 10 and no pet out. Returns false if no pet was
    /// created.
    bool CallHunterBeast(Player* bot, uint32 entry);

    /// Whether a hunter can call a stable beast now: old enough, and without a living pet (a dead one is dismissed
    /// first, as a player does before calling another).
    [[nodiscard]] bool CanCallHunterBeast(Player* bot);
}

#endif
