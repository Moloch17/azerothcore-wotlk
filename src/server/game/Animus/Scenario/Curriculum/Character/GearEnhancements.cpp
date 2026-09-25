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

/*
 * Finishing a gear set as players finish theirs: enchants, gems, death knight runes, rogue poisons and a hunter's
 * quiver (see GearBuilder). Relics are equipped from their own pool in GearBuilder.cpp.
 */

#include "GearBuilder.h"
#include "ActionCatalog.h"
#include "DBCStores.h"
#include "GearStats.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>

namespace
{
    using Animus::Curriculum::StatProfile;
    using namespace Animus::Curriculum::GearStats;

    enum RuneSpells : uint32
    {
        SPELL_RUNE_OF_THE_FALLEN_CRUSADER   = 53344,
        SPELL_RUNE_OF_RAZORICE              = 53343,
        SPELL_RUNE_OF_THE_STONESKIN_GARGOYLE = 62158,
    };

    /// First ranks of the shaman weapon imbues.
    enum ImbueSpells : uint32
    {
        SPELL_ROCKBITER_WEAPON              = 8017,
        SPELL_FLAMETONGUE_WEAPON            = 8024,
        SPELL_WINDFURY_WEAPON               = 8232,
        SPELL_EARTHLIVING_WEAPON            = 51730,
    };

    constexpr uint32 IMBUE_DURATION_MS = 1800 * IN_MILLISECONDS;    // 30 minutes, as the spells last

    /// Levels at which every slot is enchanted and every socket filled: the expansion level caps, where characters
    /// are geared for dungeons. While levelling, each item is enchanted and socketed with this chance.
    constexpr std::array<uint8, 2> FULLY_ENHANCED_LEVELS = { 70, 80 };
    constexpr int32 LEVELLING_ENHANCE_CHANCE = 50;

    /// Candidates within this much enchanting skill of the best one are equally likely.
    constexpr uint16 ENCHANT_SKILL_SPREAD = 20;

    constexpr uint32 POISON_DURATION_MS = 3600 * IN_MILLISECONDS;   // Spell::EffectEnchantItemTmp
    /// Percent of rogues with Crippling Poison in the off hand instead of Deadly Poison: a slowed opponent is one
    /// that cannot run from, or catch up with, the rogue, and a rogue that never has it never learns to use it.
    constexpr int32 CRIPPLING_POISON_CHANCE = 50;
    constexpr uint8 GEM_EPIC_LEVEL = 80;

    /// The enchanting skill of the enchants a player of `level` can buy: the enchanting skill a character of the
    /// level could have (75 per 15 levels to 300 at 60, then 7.5 per level), plus the top enchants at the cap.
    uint16 EnchantSkillCap(uint8 level)
    {
        if (level <= 60)
            return uint16(level * 5);
        if (level < 80)
            return uint16(level <= 70 ? 300 + (level - 60) * 7.5f : 375 + (level - 70) * 7.5f);
        return 480;
    }

    uint32 EnchantOf(SpellInfo const* spell, uint32 effect)
    {
        for (SpellEffectInfo const& info : spell->GetEffects())
            if (info.Effect == effect && info.MiscValue > 0)
                return uint32(info.MiscValue);

        return 0;
    }

    /// An item's on-use spell with an enchant effect of `effect`.
    SpellInfo const* EnchantingSpellOf(ItemTemplate const& proto, uint32 effect)
    {
        for (_Spell const& spell : proto.Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId); info && EnchantOf(info, effect))
                    return info;

        return nullptr;
    }

    bool FullyEnhanced(uint8 level)
    {
        return std::find(FULLY_ENHANCED_LEVELS.begin(), FULLY_ENHANCED_LEVELS.end(), level)
            != FULLY_ENHANCED_LEVELS.end();
    }

    void SetEnchantment(Player* bot, Item* item, EnchantmentSlot slot, uint32 enchantId, uint32 durationMs = 0,
        uint32 charges = 0)
    {
        bot->ApplyEnchantment(item, slot, false);
        item->SetEnchantment(slot, enchantId, durationMs, charges, bot->GetGUID());
        bot->ApplyEnchantment(item, slot, true);
    }

    /// Meta gems: only their stats count (their equip effects are the gem's point, not a proc to judge).
    StatVerdict MetaGemVerdict(StatProfile profile, SpellItemEnchantmentEntry const* enchantment)
    {
        StatVerdict verdict;
        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
            if (enchantment->type[i] == ITEM_ENCHANTMENT_TYPE_STAT)
                verdict.Add(profile, enchantment->spellid[i]);

        return verdict;
    }
}

void Animus::Curriculum::GearBuilder::BuildEnhancements(StatProfile stats)
{
    std::vector<EnchantCandidate>& enchants = _enchants[stats];
    std::vector<GemCandidate>& gems = _gems[stats];
    std::unordered_set<uint32> const& obtainable = ObtainableItems();

    auto const addEnchant = [&](SpellInfo const* spell, uint16 skill)
    {
        uint32 const enchantId = EnchantOf(spell, SPELL_EFFECT_ENCHANT_ITEM);
        SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchantment || enchantment->requiredSkill || !EnchantmentVerdict(stats, enchantment).Suits())
            return;

        enchants.push_back({ spell, enchantId, skill });
    };

    // Enchanting recipes, by the skill at which they turn yellow.
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        if (!ability || ability->SkillLine != SKILL_ENCHANTING)
            continue;

        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(ability->Spell))
            if (EnchantOf(spell, SPELL_EFFECT_ENCHANT_ITEM))
                addEnchant(spell, uint16(ability->TrivialSkillLineRankLow));
    }

    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (!obtainable.contains(itemId) || proto.RequiredSkill || proto.Quality > ITEM_QUALITY_EPIC)
            continue;

        // Enchanting items: armor kits, leg armor, arcanums, shoulder inscriptions.
        if (proto.Class == ITEM_CLASS_CONSUMABLE && proto.SubClass == ITEM_SUBCLASS_ITEM_ENHANCEMENT)
        {
            if (SpellInfo const* spell = EnchantingSpellOf(proto, SPELL_EFFECT_ENCHANT_ITEM))
                addEnchant(spell, EnchantSkillCap(uint8(std::min<uint32>(proto.RequiredLevel, 79))));
            continue;
        }

        if (proto.Class != ITEM_CLASS_GEM || !proto.GemProperties || proto.HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE)
            || proto.ItemLimitCategory || proto.Quality < ITEM_QUALITY_UNCOMMON)
            continue;

        GemPropertiesEntry const* properties = sGemPropertiesStore.LookupEntry(proto.GemProperties);
        SpellItemEnchantmentEntry const* enchantment = properties
            ? sSpellItemEnchantmentStore.LookupEntry(properties->spellitemenchantement) : nullptr;
        if (!enchantment || enchantment->requiredSkill)
            continue;

        bool const meta = properties->color & SOCKET_COLOR_META;
        if (!(meta ? MetaGemVerdict(stats, enchantment) : EnchantmentVerdict(stats, enchantment)).Suits())
            continue;

        gems.push_back({ enchantment->ID, properties->color, uint8(std::min<uint32>(proto.RequiredLevel, 80)),
            uint16(proto.ItemLevel), uint8(proto.Quality) });
    }

    LOG_DEBUG("module.animus", "Enhancements for class {} profile {}: {} enchants, {} gems", _class, uint32(stats),
        enchants.size(), gems.size());
}

void Animus::Curriculum::GearBuilder::Enhance(Player* bot, SpecProfile const& spec) const
{
    uint8 const level = bot->GetLevel();
    bool const full = FullyEnhanced(level);

    // Meta gems go in last: whether they activate depends on the gems in every other socket.
    std::vector<std::pair<Item*, uint8>> metas;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || (!full && !roll_chance_i(LEVELLING_ENHANCE_CHANCE)))
            continue;

        EnchantItem(bot, item, spec.Stats);
        SocketItem(bot, item, spec.Stats, metas);
    }

    for (auto const& [item, socket] : metas)
        bot->ApplyEnchantment(item, EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socket), true);

    Runeforge(bot, spec);
    ApplyPoisons(bot);
    ApplyImbues(bot, spec);
}

void Animus::Curriculum::GearBuilder::ApplyImbues(Player* bot, SpecProfile const& spec) const
{
    if (_class != CLASS_SHAMAN)
        return;

    auto const imbue = [bot](Item* weapon, uint32 firstRank)
    {
        SpellInfo const* spell = weapon && weapon->GetTemplate()->Class == ITEM_CLASS_WEAPON
            ? ActionCatalog::KnownRank(bot, firstRank) : nullptr;
        if (!spell || !weapon->IsFitToSpellRequirements(spell))
            return false;

        uint32 const enchantId = EnchantOf(spell, SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY);
        if (!sSpellItemEnchantmentStore.LookupEntry(enchantId))
            return false;

        SetEnchantment(bot, weapon, TEMP_ENCHANTMENT_SLOT, enchantId, IMBUE_DURATION_MS);
        return true;
    };

    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

    switch (spec.Stats)
    {
        case StatProfile::AgilityMelee:     // enhancement: Windfury in the main hand, Flametongue in the off hand
            if (!imbue(mainHand, SPELL_WINDFURY_WEAPON))
                imbue(mainHand, SPELL_FLAMETONGUE_WEAPON);
            if (!imbue(offHand, SPELL_FLAMETONGUE_WEAPON))
                imbue(offHand, SPELL_ROCKBITER_WEAPON);
            break;
        case StatProfile::Healer:           // restoration
            if (!imbue(mainHand, SPELL_EARTHLIVING_WEAPON))
                imbue(mainHand, SPELL_FLAMETONGUE_WEAPON);
            break;
        default:                            // elemental
            if (!imbue(mainHand, SPELL_FLAMETONGUE_WEAPON))
                imbue(mainHand, SPELL_ROCKBITER_WEAPON);
            break;
    }
}

void Animus::Curriculum::GearBuilder::EnchantItem(Player* bot, Item* item, StatProfile stats) const
{
    auto const candidates = _enchants.find(stats);
    if (candidates == _enchants.end())
        return;

    ItemTemplate const* proto = item->GetTemplate();
    uint32 const itemRequiredLevel = proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel;
    uint8 const level = bot->GetLevel();
    uint16 const cap = EnchantSkillCap(level);

    std::vector<EnchantCandidate const*> fitting;
    uint16 best = 0;
    for (EnchantCandidate const& candidate : candidates->second)
    {
        SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(candidate.EnchantId);
        if (candidate.Skill > cap || enchantment->requiredLevel > level
            || candidate.Spell->BaseLevel > itemRequiredLevel || !item->IsFitToSpellRequirements(candidate.Spell))
            continue;

        fitting.push_back(&candidate);
        best = std::max(best, candidate.Skill);
    }

    std::erase_if(fitting, [best](EnchantCandidate const* c) { return c->Skill + ENCHANT_SKILL_SPREAD < best; });
    if (!fitting.empty())
        SetEnchantment(bot, item, PERM_ENCHANTMENT_SLOT, fitting[urand(0, uint32(fitting.size()) - 1)]->EnchantId);
}

void Animus::Curriculum::GearBuilder::SocketItem(Player* bot, Item* item, StatProfile stats,
    std::vector<std::pair<Item*, uint8>>& metas) const
{
    auto const candidates = _gems.find(stats);
    ItemTemplate const* proto = item->GetTemplate();
    if (candidates == _gems.end() || !item->HasSocket())
        return;

    uint8 const level = bot->GetLevel();
    for (uint8 socket = 0; socket < MAX_ITEM_PROTO_SOCKETS; ++socket)
    {
        uint32 const color = proto->Socket[socket].Color;
        if (!color)
            continue;

        // The best gems of the socket's color the level allows; failing that, the best of any color that fits.
        auto const pick = [&](bool matchColor) -> GemCandidate const*
        {
            std::vector<GemCandidate const*> found;
            uint16 bestItemLevel = 0;
            for (GemCandidate const& gem : candidates->second)
            {
                bool const metaGem = gem.Color & SOCKET_COLOR_META;
                if (gem.ReqLevel > level || gem.ItemLevel > level
                    || (gem.Quality == ITEM_QUALITY_EPIC && level < GEM_EPIC_LEVEL)
                    || metaGem != bool(color & SOCKET_COLOR_META) || (matchColor && !(gem.Color & color)))
                    continue;

                if (gem.ItemLevel > bestItemLevel)
                {
                    bestItemLevel = gem.ItemLevel;
                    found.clear();
                }

                if (gem.ItemLevel == bestItemLevel)
                    found.push_back(&gem);
            }

            return found.empty() ? nullptr : found[urand(0, uint32(found.size()) - 1)];
        };

        GemCandidate const* gem = pick(true);
        if (!gem)
            gem = pick(false);
        if (!gem)
            continue;

        EnchantmentSlot const slot = EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socket);
        if (color & SOCKET_COLOR_META)
        {
            item->SetEnchantment(slot, gem->EnchantId, 0, 0, bot->GetGUID());
            metas.emplace_back(item, socket);
        }
        else
            SetEnchantment(bot, item, slot, gem->EnchantId);
    }

    if (proto->socketBonus && item->GemsFitSockets())
        SetEnchantment(bot, item, BONUS_ENCHANTMENT_SLOT, proto->socketBonus);
}

void Animus::Curriculum::GearBuilder::Runeforge(Player* bot, SpecProfile const& spec) const
{
    if (_class != CLASS_DEATH_KNIGHT)
        return;

    auto const rune = [bot](Item* weapon, uint32 spellId)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!weapon || !spell || !weapon->IsFitToSpellRequirements(spell))
            return false;

        SetEnchantment(bot, weapon, PERM_ENCHANTMENT_SLOT, EnchantOf(spell, SPELL_EFFECT_ENCHANT_ITEM));
        return true;
    };

    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

    // Tanks: the Stoneskin Gargoyle (two-handers only). Damage dealers: the Fallen Crusader, Razorice in the off hand.
    if (spec.Stats != StatProfile::Tank || !rune(mainHand, SPELL_RUNE_OF_THE_STONESKIN_GARGOYLE))
        rune(mainHand, SPELL_RUNE_OF_THE_FALLEN_CRUSADER);
    rune(offHand, SPELL_RUNE_OF_RAZORICE);
}

void Animus::Curriculum::GearBuilder::ApplyPoisons(Player* bot) const
{
    if (_class != CLASS_ROGUE)
        return;

    auto const best = [level = bot->GetLevel()](std::vector<std::pair<uint8, uint32>> const& poisons)
    {
        uint32 enchantId = 0;
        for (auto const& [reqLevel, id] : poisons)
            if (reqLevel <= level)
                enchantId = id;
        return enchantId;
    };

    // Instant Poison in the main hand, Deadly Poison in the off hand (or the main hand, before Instant Poison); half
    // the rogues that can have it put Crippling Poison in the off hand instead.
    uint32 const instant = best(_instantPoisons);
    uint32 const crippling = best(_cripplingPoisons);
    uint32 const deadly = best(_deadlyPoisons);
    uint32 const offHandPoison = crippling && roll_chance_i(CRIPPLING_POISON_CHANCE) ? crippling : deadly;
    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

    auto const poison = [bot](Item* weapon, uint32 enchantId)
    {
        SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (weapon && enchantment && weapon->GetTemplate()->Class == ITEM_CLASS_WEAPON)
            SetEnchantment(bot, weapon, TEMP_ENCHANTMENT_SLOT, enchantId, POISON_DURATION_MS, enchantment->charges);
    };

    poison(mainHand, instant ? instant : deadly);
    poison(offHand, offHandPoison ? offHandPoison : instant);
}

void Animus::Curriculum::GearBuilder::EquipQuiver(Player* bot) const
{
    Item const* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (_class != CLASS_HUNTER || !ranged)
        return;

    uint32 const subclass = ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_GUN
        ? ITEM_SUBCLASS_AMMO_POUCH : ITEM_SUBCLASS_QUIVER;

    uint32 itemId = 0;
    for (auto const& [reqLevel, id] : _quivers)
        if (reqLevel <= bot->GetLevel() && sObjectMgr->GetItemTemplate(id)->SubClass == subclass)
            itemId = id;

    uint16 dest = 0;
    if (itemId && bot->CanEquipNewItem(NULL_SLOT, dest, itemId, false) == EQUIP_ERR_OK)
        bot->EquipNewItem(dest, itemId, true);
}
