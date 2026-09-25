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

#ifndef ANIMUS_LIB_CURRICULUM_WORLD_ACTIONS_H
#define ANIMUS_LIB_CURRICULUM_WORLD_ACTIONS_H

#include "ClassProfile.h"
#include "Define.h"

class Creature;
class GameObject;
class Item;
class Player;
class SpellInfo;
class Unit;
struct ItemTemplate;

namespace Animus::Curriculum
{
    struct WorldView;
}

/*
 * What a player does in the world besides fighting, as the client's handlers do it: loot a corpse, take and turn in
 * a quest, gather a node, skin a kill, put on a better item, sell what is junk, repair, buy food and drink. The
 * bodies of the opcode handlers (HandleSellItemOpcode, HandleLootMoneyOpcode, the quest giver handlers) with the
 * packets left out, so the sim's WorldBlock and the live companion (mod-animus) act through one set of functions.
 *
 * Nothing here decides *when*: that is the policy's. What is a lookup -- which reward is the better item, which
 * spell opens this node -- is scripted here on purpose.
 */
namespace Animus::Curriculum::WorldActions
{
    /// How near a player has to stand to use a corpse, a giver, a vendor or a node (the client's own).
    constexpr float INTERACT_YARDS = 5.0f;

    /// What a gathering node is, from its lock (LOCKTYPE_HERBALISM, LOCKTYPE_MINING).
    enum class NodeKind : uint8 { None, Herb, Mine };

    /// A rating of an item for a build's stats: the item level, plus what its stats are worth to the profile and
    /// minus what they are not (GearStats::StatPreference). A lookup, not a policy: EQUIP_UPGRADE puts on whatever
    /// rates highest, and a quest turn-in takes the reward that does.
    [[nodiscard]] float GearScore(ItemTemplate const* proto, StatProfile stats);

    /// Whether `bot` could wear `proto` at all: class, level, proficiency (Player::CanUseItem) and a slot for it.
    [[nodiscard]] bool CanWear(Player const* bot, ItemTemplate const* proto);

    /// The item in the bags that would rate higher than what is worn in its slot, best first; null for none.
    [[nodiscard]] Item* BestUpgrade(Player* bot, StatProfile stats);
    /// Put BestUpgrade on. True when something was equipped.
    bool EquipUpgrade(Player* bot, StatProfile stats);

    /// A dead creature the bot may loot: killed with loot left, tapped by the bot (or its group).
    [[nodiscard]] bool IsLootable(Player const* bot, Creature const* creature);
    /// A looted corpse the bot could skin: skinnable, and the bot has the skill for its level.
    [[nodiscard]] bool IsSkinnable(Player const* bot, Creature const* creature);
    /// Take everything from `source` (a corpse or an opened node): opens the loot as the client would, stores every
    /// item that fits and the money, releases it. `items` and `copper` report what was taken. False when nothing
    /// could be taken at all (nothing there, or the bags full).
    bool LootAll(Player* bot, Unit* source, uint32& items, uint32& copper);
    bool LootAll(Player* bot, GameObject* source, uint32& items, uint32& copper);

    /// A quest `giver` offers that the bot can take now (Player::CanTakeQuest and CanAddQuest), or 0.
    [[nodiscard]] uint32 OfferedQuest(Player* bot, Creature const* giver);
    /// A quest the bot has completed that `giver` takes back, or 0.
    [[nodiscard]] uint32 TurnInQuest(Player* bot, Creature const* giver);
    /// Accept OfferedQuest from `giver`. Returns the quest id, 0 when none.
    uint32 TakeQuest(Player* bot, Creature* giver);
    /// Hand TurnInQuest in, choosing the reward that rates highest for `stats` (GearScore) among those the bot can
    /// wear, the first otherwise. Returns the quest id, 0 when none was turned in (a full bag refuses a reward).
    uint32 TurnIn(Player* bot, Creature* giver, StatProfile stats);
    /// Objective progress of quest `questId` in the bot's log, 0 to 1 over its kill and item counts.
    [[nodiscard]] float QuestProgress(Player const* bot, uint32 questId);

    /// What kind of node `object` is, or None for an object that is not a gathering node.
    [[nodiscard]] NodeKind NodeOf(GameObject const* object);
    /// The same of a gameobject template, for a spawn that is not in the world yet.
    [[nodiscard]] NodeKind NodeOfEntry(uint32 entry);
    /// Whether the bot's skill opens `node` (the lock's skill against the bot's).
    [[nodiscard]] bool CanGather(Player const* bot, GameObject const* node);
    /// The gathering spell the bot knows for `kind` (the highest rank), or null.
    [[nodiscard]] SpellInfo const* GatherSpell(Player const* bot, NodeKind kind);
    [[nodiscard]] SpellInfo const* SkinningSpell(Player const* bot);
    /// Start gathering `node` (a cast; the loot opens when it finishes). True when the cast started.
    bool Gather(Player* bot, GameObject* node);
    /// Start skinning `corpse`. True when the cast started.
    bool Skin(Player* bot, Creature* corpse);
    /// Give the bot a gathering profession at `value` skill (the rank spells a player of that skill has), and the
    /// tool it needs in its bags (a mining pick, a skinning knife).
    void LearnProfession(Player* bot, uint32 skill, uint16 value);

    /// Vendor value of the junk (grey items) in the bags, in copper.
    [[nodiscard]] uint32 JunkValue(Player const* bot);
    /// Sell every grey item to `vendor`, as CMSG_SELL_ITEM does. Returns the copper made.
    uint32 SellJunk(Player* bot, Creature* vendor);
    /// The worn gear's durability, 0 to 1 (1 with nothing that wears).
    [[nodiscard]] float Durability(Player const* bot);
    /// Repair everything at `repairer`, as CMSG_REPAIR_ITEM does. Returns the copper spent (0: nothing to repair,
    /// or could not pay).
    uint32 Repair(Player* bot, Creature* repairer);
    /// Whether `vendor` sells food or drink the bot could use.
    [[nodiscard]] bool SellsSupplies(Player const* bot, Creature const* vendor);
    /// Buy food and drink from `vendor` up to `count` of each (the best it sells that the bot can use), as
    /// CMSG_BUY_ITEM does. Returns how many items were bought.
    uint32 BuySupplies(Player* bot, Creature* vendor, uint32 count);
    /// Food and drink in the bags, each.
    void CountSupplies(Player const* bot, uint32& food, uint32& drink);
    /// Throw food and drink away down to `keep` of each (a town episode starts nearly out).
    void DropSupplies(Player* bot, uint32 keep);

    /// What is around the bot within `radius`, for the WorldBlock: the nearest corpse it may loot (or skin), the
    /// nearest quest giver it has business with, the nearest gathering node, the nearest vendor. The quest fields
    /// are the caller's (the encounter knows the episode's quest; the live module reads the log). Sets Active.
    void Sense(Player* bot, float radius, WorldView& world);
}

#endif
