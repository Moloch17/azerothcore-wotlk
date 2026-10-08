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

#ifndef ANIMUS_LIB_CURRICULUM_GEAR_STATS_H
#define ANIMUS_LIB_CURRICULUM_GEAR_STATS_H

#include "ClassProfile.h"
#include "Define.h"
#include <array>
#include <unordered_map>
#include <unordered_set>

class SpellInfo;
struct ItemTemplate;
struct SpellItemEnchantmentEntry;

/// What suits a spec's gear: shared by gear, enchants, gems and consumables (GearBuilder*.cpp).
namespace Animus::Curriculum::GearStats
{
    /// Stat preference of a profile for an ITEM_MOD_* stat: 1 wanted, -1 never on this spec's gear, 0 indifferent.
    [[nodiscard]] int32 StatPreference(StatProfile profile, uint32 stat);

    struct StatVerdict
    {
        bool Wanted = false;
        bool Forbidden = false;

        void Add(StatProfile profile, uint32 stat)
        {
            int32 const preference = StatPreference(profile, stat);
            Wanted |= preference > 0;
            Forbidden |= preference < 0;
        }

        [[nodiscard]] bool Suits() const { return Wanted && !Forbidden; }
    };

    /// An enchantment's effects: stats as above; weapon damage for physical profiles; armor for tanks; the
    /// well-known weapon procs of each profile. Anything else (resistances, profession bonuses) is forbidden.
    [[nodiscard]] StatVerdict EnchantmentVerdict(StatProfile profile, SpellItemEnchantmentEntry const* enchantment);

    /// Random property / suffix enchantments together (ItemRandomProperties/ItemRandomSuffix entries): stats only.
    [[nodiscard]] StatVerdict RandomEnchantmentVerdict(StatProfile profile, std::array<uint32, 5> const& enchantments);

    /// The spell an item casts when used (the first on-use spell of any of its spell slots), or null. The one lookup
    /// every consumer uses, so an item is never stocked by one rule and refused by another.
    [[nodiscard]] SpellInfo const* ItemUseSpell(ItemTemplate const* proto);

    /// Items a player can actually get: loot, vendors, quest rewards and crafting. Loaded once (world thread).
    [[nodiscard]] std::unordered_set<uint32> const& ObtainableItems();

    enum ItemSource : uint8
    {
        SOURCE_DUNGEON  = 0x01,     // dropped by a creature in a five-player dungeon
        SOURCE_QUEST    = 0x02,     // a quest reward
    };

    /// ItemSource bits of items from dungeons and quests, where players get most of their gear. Loaded once.
    [[nodiscard]] std::unordered_map<uint32, uint8> const& ItemSources();
}

#endif
