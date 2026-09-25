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

#include "GearBuilder.h"
#include "WarmCaches.h"
#include "ClassKit.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GearStats.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Supplies.h"
#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using Animus::Curriculum::StatProfile;
    using namespace Animus::Curriculum::GearStats;

    constexpr uint32 EQUIP_ATTEMPTS = 6;
    constexpr uint32 AMMO_COUNT = 1000;

    struct ItemLevelAnchor
    {
        uint8 Level;
        uint16 Low;
        uint16 High;
    };

    /// The item levels a player of a level wears, between anchors linearly: while levelling, quest and dungeon
    /// gear a few item levels above the level; Outland greens and blues from 58 to 70; Northrend gear from 70;
    /// heroic-dungeon gear at 80 (normal dungeon blues to the first heroic and badge pieces). Raid and top-end PvP
    /// gear sits above these bands.
    constexpr std::array<ItemLevelAnchor, 16> ITEM_LEVEL_ANCHORS =
    {{
        {  1,   1,   8 },
        { 10,   8,  20 },
        { 20,  18,  30 },
        { 30,  28,  38 },
        { 40,  38,  50 },
        { 50,  48,  60 },
        { 57,  55,  66 },
        { 58,  58,  88 },
        { 60,  78,  96 },
        { 65,  92, 110 },
        { 68, 105, 125 },
        { 70, 110, 130 },
        { 72, 135, 160 },
        { 75, 150, 175 },
        { 78, 165, 190 },
        { 80, 180, 213 },
    }};

    /// Levels at which epics are worn: the expansion level caps, where heroic dungeons and badge vendors hand them
    /// out. Elsewhere the bands would reach the previous expansion's raid epics (level 72: Black Temple, Sunwell).
    constexpr std::array<uint8, 2> EPIC_LEVELS = { 70, 80 };

    /// How often a candidate is picked, by where it comes from: players gear up mostly in dungeons and from quests,
    /// much less from world drops, vendors and crafting.
    constexpr uint8 WEIGHT_DUNGEON = 4;
    constexpr uint8 WEIGHT_QUEST = 3;
    constexpr uint8 WEIGHT_OTHER = 1;

    /// How far below the band's low end a slot may reach when nothing in the band fits, in order.
    constexpr std::array<uint16, 4> ITEM_LEVEL_WIDENING = { 0, 10, 25, 1000 };

    std::pair<uint16, uint16> ItemLevelBand(uint8 level)
    {
        ItemLevelAnchor const* below = &ITEM_LEVEL_ANCHORS.front();
        for (ItemLevelAnchor const& anchor : ITEM_LEVEL_ANCHORS)
        {
            if (anchor.Level == level)
                return { anchor.Low, anchor.High };

            if (anchor.Level > level)
            {
                float const t = float(level - below->Level) / float(anchor.Level - below->Level);
                auto const lerp = [t](uint16 a, uint16 b)
                {
                    return uint16(float(a) + (float(b) - float(a)) * t + 0.5f);
                };
                return { lerp(below->Low, anchor.Low), lerp(below->High, anchor.High) };
            }

            below = &anchor;
        }

        return { ITEM_LEVEL_ANCHORS.back().Low, ITEM_LEVEL_ANCHORS.back().High };
    }

    bool HasResilience(ItemTemplate const& proto)
    {
        for (uint32 i = 0; i < proto.StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            if (proto.ItemStat[i].ItemStatType == ITEM_MOD_RESILIENCE_RATING && proto.ItemStat[i].ItemStatValue > 0)
                return true;

        return false;
    }

    /// item_enchantment_template: random property / suffix group -> ids. Loaded once.
    std::unordered_map<uint32, std::vector<uint32>> const& RandomEnchantGroups()
    {
        static std::unordered_map<uint32, std::vector<uint32>> const groups = []()
        {
            std::unordered_map<uint32, std::vector<uint32>> result;
            if (QueryResult query = WorldDatabase.Query("SELECT entry, ench FROM item_enchantment_template"))
            {
                do
                {
                    Field* fields = query->Fetch();
                    result[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
                } while (query->NextRow());
            }

            return result;
        }();

        return groups;
    }

    std::unordered_set<uint32> UsableSkills(uint8 playerClass)
    {
        std::unordered_set<uint32> skills;
        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
            if (!line || (line->categoryId != SKILL_CATEGORY_WEAPON && line->categoryId != SKILL_CATEGORY_ARMOR))
                continue;

            for (uint8 race = RACE_HUMAN; race <= RACE_DRAENEI; ++race)
            {
                if (sObjectMgr->GetPlayerInfo(race, playerClass) && GetSkillRaceClassInfo(line->id, race, playerClass))
                {
                    skills.insert(line->id);
                    break;
                }
            }
        }

        return skills;
    }

    uint8 RequiredLevelOf(ItemTemplate const* proto)
    {
        if (proto->RequiredLevel)
            return uint8(std::min<uint32>(proto->RequiredLevel, DEFAULT_MAX_LEVEL));

        return uint8(std::clamp<uint32>(proto->ItemLevel > 5 ? proto->ItemLevel - 5 : 1, 1, DEFAULT_MAX_LEVEL));
    }
}

int32 Animus::Curriculum::GearStats::StatPreference(StatProfile profile, uint32 stat)
{
    bool const physical = profile == StatProfile::StrengthMelee || profile == StatProfile::AgilityMelee;
    bool const spell = profile == StatProfile::Caster || profile == StatProfile::Healer;

    switch (stat)
    {
        case ITEM_MOD_STRENGTH:
            return physical || profile == StatProfile::Tank ? 1 : (profile == StatProfile::Ranged ? 0 : -1);
        case ITEM_MOD_AGILITY:
            return spell ? -1 : 1;
        case ITEM_MOD_STAMINA:
            return profile == StatProfile::Tank ? 1 : 0;
        case ITEM_MOD_INTELLECT:
            // Enhancement mail is agility and intellect (154 of the 157 level 80 agility mail pieces).
            return spell ? 1 : (profile == StatProfile::Ranged || profile == StatProfile::AgilityMelee ? 0 : -1);
        case ITEM_MOD_SPIRIT:
            return profile == StatProfile::Healer ? 1 : (profile == StatProfile::Caster ? 0 : -1);
        case ITEM_MOD_ATTACK_POWER:
        case ITEM_MOD_ARMOR_PENETRATION_RATING:
            return spell ? -1 : (profile == StatProfile::Tank ? 0 : 1);
        case ITEM_MOD_RANGED_ATTACK_POWER:
        case ITEM_MOD_HIT_RANGED_RATING:
        case ITEM_MOD_CRIT_RANGED_RATING:
        case ITEM_MOD_HASTE_RANGED_RATING:
            return profile == StatProfile::Ranged ? 1 : (spell ? -1 : 0);
        case ITEM_MOD_HIT_MELEE_RATING:
        case ITEM_MOD_CRIT_MELEE_RATING:
        case ITEM_MOD_HASTE_MELEE_RATING:
            return spell ? -1 : (profile == StatProfile::Tank && stat != ITEM_MOD_HIT_MELEE_RATING ? 0 : 1);
        case ITEM_MOD_EXPERTISE_RATING:
            return spell || profile == StatProfile::Ranged ? -1 : 1;
        case ITEM_MOD_HIT_RATING:
            return profile == StatProfile::Healer ? -1 : 1;
        case ITEM_MOD_CRIT_RATING:
        case ITEM_MOD_HASTE_RATING:
            return profile == StatProfile::Tank ? 0 : 1;
        case ITEM_MOD_HIT_SPELL_RATING:
            return profile == StatProfile::Caster ? 1 : -1;
        case ITEM_MOD_CRIT_SPELL_RATING:
        case ITEM_MOD_HASTE_SPELL_RATING:
        case ITEM_MOD_SPELL_POWER:
        case ITEM_MOD_SPELL_DAMAGE_DONE:
        case ITEM_MOD_SPELL_HEALING_DONE:
        case ITEM_MOD_SPELL_PENETRATION:
            return spell ? 1 : -1;
        case ITEM_MOD_MANA_REGENERATION:
            return profile == StatProfile::Healer ? 1 : (profile == StatProfile::Caster ? 0 : -1);
        case ITEM_MOD_DEFENSE_SKILL_RATING:
        case ITEM_MOD_DODGE_RATING:
        case ITEM_MOD_PARRY_RATING:
        case ITEM_MOD_BLOCK_RATING:
        case ITEM_MOD_BLOCK_VALUE:
            return profile == StatProfile::Tank ? 1 : -1;
        default:
            return 0;
    }
}

namespace
{
    /// Well-known weapon and armor procs, by enchantment name, that suit a profile.
    bool ProcSuits(Animus::Curriculum::StatProfile profile, std::string_view name)
    {
        using Animus::Curriculum::StatProfile;

        auto const any = [name](std::initializer_list<std::string_view> names)
        {
            return std::any_of(names.begin(), names.end(), [name](std::string_view n) { return name == n; });
        };

        switch (profile)
        {
            case StatProfile::StrengthMelee:
            case StatProfile::AgilityMelee:
                return any({ "Crusader", "Mongoose", "Berserking", "Executioner", "Fiery Weapon", "Icebreaker",
                    "Lifestealing", "Unholy Weapon", "Battlemaster" });
            case StatProfile::Tank:
                return any({ "Mongoose", "Blade Ward", "Blood Draining", "Crusader" });
            case StatProfile::Caster:
                return any({ "Black Magic", "Deathfrost", "Spellsurge" });
            case StatProfile::Healer:
                return any({ "Spellsurge", "Lifeward" });
            case StatProfile::Ranged:
                return false;
        }

        return false;
    }
}

Animus::Curriculum::GearStats::StatVerdict Animus::Curriculum::GearStats::EnchantmentVerdict(
    StatProfile profile, SpellItemEnchantmentEntry const* enchantment)
{
    StatVerdict verdict;
    if (!enchantment)
        return verdict;

    bool const physical = profile == StatProfile::StrengthMelee || profile == StatProfile::AgilityMelee
        || profile == StatProfile::Ranged || profile == StatProfile::Tank;

    for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
    {
        switch (enchantment->type[i])
        {
            case ITEM_ENCHANTMENT_TYPE_NONE:
                break;
            case ITEM_ENCHANTMENT_TYPE_STAT:
                verdict.Add(profile, enchantment->spellid[i]);
                break;
            case ITEM_ENCHANTMENT_TYPE_DAMAGE:
                (physical ? verdict.Wanted : verdict.Forbidden) = true;
                break;
            case ITEM_ENCHANTMENT_TYPE_RESISTANCE:
                // spellid is the school: 0 armor.
                (profile == StatProfile::Tank && enchantment->spellid[i] == 0
                    ? verdict.Wanted : verdict.Forbidden) = true;
                break;
            case ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL:
            case ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL:
                (ProcSuits(profile, enchantment->description[LOCALE_enUS]) ? verdict.Wanted : verdict.Forbidden) = true;
                break;
            default:
                verdict.Forbidden = true;
                break;
        }
    }

    return verdict;
}

Animus::Curriculum::GearStats::StatVerdict Animus::Curriculum::GearStats::RandomEnchantmentVerdict(
    StatProfile profile, std::array<uint32, 5> const& enchantments)
{
    StatVerdict verdict;
    for (uint32 enchantmentId : enchantments)
    {
        SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantmentId);
        if (!enchantment)
            continue;

        for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
            if (enchantment->type[i] == ITEM_ENCHANTMENT_TYPE_STAT)
                verdict.Add(profile, enchantment->spellid[i]);
    }

    return verdict;
}

std::unordered_map<uint32, uint8> const& Animus::Curriculum::GearStats::ItemSources()
{
    static std::unordered_map<uint32, uint8> const sources = []()
    {
        std::unordered_map<uint32, uint8> result;

        // Creatures spawned in five-player dungeons, by loot id.
        std::unordered_set<uint32> dungeonLoot;
        if (QueryResult query = WorldDatabase.Query("SELECT DISTINCT id, map FROM creature"))
        {
            do
            {
                Field* fields = query->Fetch();
                MapEntry const* map = sMapStore.LookupEntry(fields[1].Get<uint32>());
                CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(fields[0].Get<uint32>());
                if (map && map->IsNonRaidDungeon() && creature && creature->lootid)
                    dungeonLoot.insert(creature->lootid);
            } while (query->NextRow());
        }

        // Their loot, one level of references deep (boss loot lives in reference tables).
        std::unordered_set<uint32> references;
        if (QueryResult query = WorldDatabase.Query("SELECT Entry, Item, Reference FROM creature_loot_template"))
        {
            do
            {
                Field* fields = query->Fetch();
                if (!dungeonLoot.contains(fields[0].Get<uint32>()))
                    continue;

                if (uint32 const reference = fields[2].Get<uint32>())
                    references.insert(reference);
                else
                    result[fields[1].Get<uint32>()] |= SOURCE_DUNGEON;
            } while (query->NextRow());
        }

        if (QueryResult query = WorldDatabase.Query(
            "SELECT Entry, Item FROM reference_loot_template WHERE Reference = 0"))
        {
            do
            {
                Field* fields = query->Fetch();
                if (references.contains(fields[0].Get<uint32>()))
                    result[fields[1].Get<uint32>()] |= SOURCE_DUNGEON;
            } while (query->NextRow());
        }

        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            for (uint32 item : quest->RewardItemId)
                if (item)
                    result[item] |= SOURCE_QUEST;
            for (uint32 item : quest->RewardChoiceItemId)
                if (item)
                    result[item] |= SOURCE_QUEST;
        }

        LOG_DEBUG("module.animus", "Gear: {} items from dungeons and quests", result.size());
        return result;
    }();

    return sources;
}

std::unordered_set<uint32> const& Animus::Curriculum::GearStats::ObtainableItems()
{
    static std::unordered_set<uint32> const items = []()
    {
        std::unordered_set<uint32> result;

        // Column types differ between tables (npc_vendor.item is signed: negative = vendor reference),
        // so every id is read as a signed 64-bit value.
        for (char const* sql :
        {
            "SELECT CAST(Item AS SIGNED) FROM creature_loot_template WHERE Reference = 0",
            "SELECT CAST(Item AS SIGNED) FROM reference_loot_template WHERE Reference = 0",
            "SELECT CAST(Item AS SIGNED) FROM gameobject_loot_template WHERE Reference = 0",
            "SELECT CAST(Item AS SIGNED) FROM item_loot_template WHERE Reference = 0",
            "SELECT CAST(item AS SIGNED) FROM npc_vendor",
            "SELECT CAST(RewardItem1 AS SIGNED) FROM quest_template UNION SELECT CAST(RewardItem2 AS SIGNED) FROM "
                "quest_template UNION SELECT CAST(RewardItem3 AS SIGNED) FROM quest_template UNION SELECT "
                "CAST(RewardItem4 AS SIGNED) FROM quest_template",
            "SELECT CAST(RewardChoiceItemID1 AS SIGNED) FROM quest_template "
                "UNION SELECT CAST(RewardChoiceItemID2 AS SIGNED) FROM quest_template "
                "UNION SELECT CAST(RewardChoiceItemID3 AS SIGNED) FROM quest_template "
                "UNION SELECT CAST(RewardChoiceItemID4 AS SIGNED) FROM quest_template "
                "UNION SELECT CAST(RewardChoiceItemID5 AS SIGNED) FROM quest_template "
                "UNION SELECT CAST(RewardChoiceItemID6 AS SIGNED) FROM quest_template",
        })
        {
            if (QueryResult query = WorldDatabase.Query(sql))
            {
                do
                {
                    int64 const itemId = query->Fetch()[0].Get<int64>();
                    if (itemId > 0)
                        result.insert(uint32(itemId));
                } while (query->NextRow());
            }
        }

        for (uint32 spellId = 0; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM || effect.Effect == SPELL_EFFECT_CREATE_ITEM_2)
                        && effect.ItemType)
                        result.insert(effect.ItemType);

        LOG_DEBUG("module.animus", "Gear: {} obtainable items", result.size());
        return result;
    }();

    return items;
}

SpellInfo const* Animus::Curriculum::GearStats::ItemUseSpell(ItemTemplate const* proto)
{
    if (!proto)
        return nullptr;

    for (_Spell const& spell : proto->Spells)
        if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            return sSpellMgr->GetSpellInfo(spell.SpellId);

    return nullptr;
}

Animus::Curriculum::GearBuilder::GearBuilder(ClassProfile const& profile, ClassKit const& kit)
    : _kit(kit), _class(profile.Class)
{
    for (SpecProfile const& spec : profile.Specs)
    {
        if (!_pools.contains(spec.Stats))
        {
            BuildPools(spec.Stats);
            BuildEnhancements(spec.Stats);
        }
    }

    if (_class != CLASS_HUNTER && _class != CLASS_ROGUE)
        return;

    std::unordered_set<uint32> const& obtainable = ObtainableItems();
    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (!obtainable.contains(itemId) || proto.Quality > ITEM_QUALITY_EPIC || proto.RequiredSkill)
            continue;

        if (_class == CLASS_HUNTER && proto.Class == ITEM_CLASS_PROJECTILE)
        {
            if (proto.SubClass == ITEM_SUBCLASS_ARROW)
                _arrows.emplace_back(RequiredLevelOf(&proto), itemId);
            else if (proto.SubClass == ITEM_SUBCLASS_BULLET)
                _bullets.emplace_back(RequiredLevelOf(&proto), itemId);
        }
        else if (_class == CLASS_HUNTER && proto.Class == ITEM_CLASS_QUIVER
            && (proto.SubClass == ITEM_SUBCLASS_QUIVER || proto.SubClass == ITEM_SUBCLASS_AMMO_POUCH))
            _quivers.emplace_back(RequiredLevelOf(&proto), itemId);
        else if (_class == CLASS_ROGUE && proto.Class == ITEM_CLASS_CONSUMABLE)
        {
            // Rogue poisons, by name: the enchant their use spell puts on a weapon.
            std::vector<std::pair<uint8, uint32>>* poisons = proto.Name1.starts_with("Instant Poison")
                ? &_instantPoisons : proto.Name1.starts_with("Deadly Poison") ? &_deadlyPoisons
                : proto.Name1.starts_with("Crippling Poison") ? &_cripplingPoisons : nullptr;
            if (!poisons)
                continue;

            for (_Spell const& spell : proto.Spells)
            {
                SpellInfo const* info = spell.SpellId > 0 ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr;
                if (!info)
                    continue;

                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY && effect.MiscValue > 0)
                        poisons->emplace_back(RequiredLevelOf(&proto), uint32(effect.MiscValue));
            }
        }
    }

    for (auto* list : { &_arrows, &_bullets, &_quivers, &_instantPoisons, &_deadlyPoisons, &_cripplingPoisons })
        std::sort(list->begin(), list->end());
}

void Animus::Curriculum::GearBuilder::BuildPools(StatProfile stats)
{
    Pools& pools = _pools[stats];

    std::unordered_set<uint32> const& obtainable = ObtainableItems();
    std::unordered_map<uint32, std::vector<uint32>> const& enchantGroups = RandomEnchantGroups();
    std::unordered_set<uint32> const skills = UsableSkills(_class);
    uint32 const classMask = 1 << (_class - 1);

    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (!obtainable.contains(itemId) || proto.Quality < ITEM_QUALITY_NORMAL || proto.Quality > ITEM_QUALITY_EPIC)
            continue;

        if (!(proto.AllowableClass & classMask) || proto.RequiredSkill || proto.RequiredSpell || proto.RequiredHonorRank
            || proto.RequiredCityRank || proto.RequiredReputationFaction || proto.Duration)
            continue;

        if (uint32 const skill = proto.GetSkill(); skill && !skills.contains(skill))
            continue;

        std::vector<Pool> targets;
        bool const armor = proto.Class == ITEM_CLASS_ARMOR;
        bool const weapon = proto.Class == ITEM_CLASS_WEAPON;
        bool const bodyArmor = armor && proto.SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH
            && proto.SubClass <= ITEM_SUBCLASS_ARMOR_PLATE;

        switch (proto.InventoryType)
        {
            case INVTYPE_HEAD:      if (bodyArmor) targets = { POOL_HEAD }; break;
            case INVTYPE_SHOULDERS: if (bodyArmor) targets = { POOL_SHOULDERS }; break;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:      if (bodyArmor) targets = { POOL_CHEST }; break;
            case INVTYPE_WAIST:     if (bodyArmor) targets = { POOL_WAIST }; break;
            case INVTYPE_LEGS:      if (bodyArmor) targets = { POOL_LEGS }; break;
            case INVTYPE_FEET:      if (bodyArmor) targets = { POOL_FEET }; break;
            case INVTYPE_WRISTS:    if (bodyArmor) targets = { POOL_WRISTS }; break;
            case INVTYPE_HANDS:     if (bodyArmor) targets = { POOL_HANDS }; break;
            case INVTYPE_NECK:      if (armor) targets = { POOL_NECK }; break;
            case INVTYPE_FINGER:    if (armor) targets = { POOL_FINGER }; break;
            case INVTYPE_TRINKET:   if (armor) targets = { POOL_TRINKET }; break;
            case INVTYPE_CLOAK:     if (armor) targets = { POOL_BACK }; break;
            case INVTYPE_SHIELD:
                if (armor && proto.SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                    targets = { POOL_SHIELD };
                break;
            case INVTYPE_HOLDABLE:  targets = { POOL_HELD }; break;
            case INVTYPE_RELIC:     if (armor) targets = { POOL_RELIC }; break;
            case INVTYPE_2HWEAPON:
                if (weapon && proto.SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                    targets = { POOL_TWO_HAND };
                break;
            case INVTYPE_WEAPON:        if (weapon) targets = { POOL_MAIN_HAND, POOL_OFF_HAND }; break;
            case INVTYPE_WEAPONMAINHAND: if (weapon) targets = { POOL_MAIN_HAND }; break;
            case INVTYPE_WEAPONOFFHAND: if (weapon) targets = { POOL_OFF_HAND }; break;
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
                if (weapon && proto.SubClass == ITEM_SUBCLASS_WEAPON_WAND)
                    targets = { POOL_WAND };
                else if (weapon && proto.SubClass != ITEM_SUBCLASS_WEAPON_THROWN)
                    targets = { POOL_RANGED };
                break;
            default:
                break;
        }

        if (targets.empty())
            continue;

        Candidate candidate;
        candidate.ItemId = itemId;
        candidate.ReqLevel = RequiredLevelOf(&proto);
        candidate.ItemLevel = uint16(proto.ItemLevel);
        candidate.SubClass = proto.SubClass;
        candidate.Pvp = HasResilience(proto);
        candidate.Epic = proto.Quality == ITEM_QUALITY_EPIC;
        if (auto const source = ItemSources().find(itemId); source != ItemSources().end())
            candidate.Weight = (source->second & SOURCE_DUNGEON) ? WEIGHT_DUNGEON : WEIGHT_QUEST;
        else
            candidate.Weight = WEIGHT_OTHER;

        StatVerdict fixed;
        for (uint32 i = 0; i < proto.StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            if (proto.ItemStat[i].ItemStatValue > 0)
                fixed.Add(stats, proto.ItemStat[i].ItemStatType);

        if (fixed.Forbidden)
            continue;

        // Random stats: keep the properties/suffixes that suit the profile. An item whose random stats
        // never do is still usable for its armor or weapon damage, without them.
        int32 const randomGroup = proto.RandomProperty ? proto.RandomProperty : proto.RandomSuffix;
        if (randomGroup > 0)
        {
            if (auto const group = enchantGroups.find(uint32(randomGroup)); group != enchantGroups.end())
            {
                for (uint32 id : group->second)
                {
                    StatVerdict verdict;
                    if (proto.RandomProperty)
                    {
                        if (ItemRandomPropertiesEntry const* property = sItemRandomPropertiesStore.LookupEntry(id))
                            verdict = RandomEnchantmentVerdict(stats, property->Enchantment);
                    }
                    else if (ItemRandomSuffixEntry const* suffix = sItemRandomSuffixStore.LookupEntry(id))
                        verdict = RandomEnchantmentVerdict(stats, suffix->Enchantment);

                    if (verdict.Suits())
                        candidate.RandomIds.push_back(proto.RandomProperty ? int32(id) : -int32(id));
                }
            }
        }

        bool const onUse = std::any_of(std::begin(proto.Spells), std::end(proto.Spells),
            [](_Spell const& spell) { return spell.SpellId > 0; });

        candidate.Stats = fixed.Wanted || !candidate.RandomIds.empty()
            || ((proto.InventoryType == INVTYPE_TRINKET || proto.InventoryType == INVTYPE_RELIC) && onUse);

        // Jewelry and trinkets are only worth their stats or effects.
        bool const statsOnly = proto.InventoryType == INVTYPE_NECK || proto.InventoryType == INVTYPE_FINGER
            || proto.InventoryType == INVTYPE_TRINKET || proto.InventoryType == INVTYPE_RELIC;
        if (statsOnly && !candidate.Stats)
            continue;

        for (Pool pool : targets)
            pools[pool].push_back(candidate);
    }

    LOG_DEBUG("module.animus", "Gear pools for class {} profile {}: {} two-handers, {} one-handers, {} chests, "
        "{} trinkets", _class, uint32(stats), pools[POOL_TWO_HAND].size(), pools[POOL_MAIN_HAND].size(),
        pools[POOL_CHEST].size(), pools[POOL_TRINKET].size());
}

uint64 Animus::Curriculum::GearBuilder::WindowKey(Pool pool, uint8 level, StatProfile stats, int32 subclass,
    bool needStats, bool pvp)
{
    // subclass is -1 for "any", so it is stored one above itself to keep the key unsigned.
    return uint64(uint8(pool)) | (uint64(level) << 8) | (uint64(uint8(stats)) << 16)
        | (uint64(uint8(subclass + 1)) << 24) | (uint64(needStats) << 32) | (uint64(pvp) << 33);
}

std::vector<Animus::Curriculum::GearBuilder::Candidate const*> const& Animus::Curriculum::GearBuilder::Window(
    Pool pool, uint8 level, StatProfile stats, int32 subclass, bool needStats, bool pvp) const
{
    // Nineteen slots, each asking up to twice for stats and up to four times down the armor fallbacks, and each
    // ask walking a whole pool four times as the item level band widens -- per character, per episode, for an
    // answer that depends on nothing that changed since the last character of the same level and spec.
    uint64 const key = WindowKey(pool, level, stats, subclass, needStats, pvp);
    if (auto const cached = _windows.find(key); cached != _windows.end())
        return cached->second;

    std::vector<Candidate const*>& found = _windows[key];
    auto const pools = _pools.find(stats);
    if (pools == _pools.end())
        return found;

    // The level's item level band first, then reaching further below it. Never above the band: a character of
    // the level does not wear raid or top-end PvP gear.
    auto const [low, high] = ItemLevelBand(level);
    bool const epics = std::find(EPIC_LEVELS.begin(), EPIC_LEVELS.end(), level) != EPIC_LEVELS.end();
    for (uint16 widening : ITEM_LEVEL_WIDENING)
    {
        for (Candidate const& candidate : pools->second[pool])
        {
            if (candidate.ReqLevel > level || candidate.ItemLevel > high || candidate.ItemLevel + widening < low)
                continue;

            if ((subclass >= 0 && candidate.SubClass != uint32(subclass)) || (needStats && !candidate.Stats)
                || (candidate.Pvp && !pvp) || (candidate.Epic && !epics))
                continue;

            found.push_back(&candidate);
        }

        if (!found.empty())
            break;
    }

    return found;
}

bool Animus::Curriculum::GearBuilder::EquipFromPool(Player* bot, uint8 slot, Pool pool, StatProfile stats,
    bool pvp, int32 subclass) const
{
    uint8 const level = bot->GetLevel();

    // Body armor falls back to lighter armor types; everything falls back to items without the
    // profile's stats before leaving the slot empty.
    std::vector<int32> subclasses = { subclass };
    if (subclass > ITEM_SUBCLASS_ARMOR_CLOTH && IsBodyArmorPool(pool))
        for (int32 lighter = subclass - 1; lighter >= int32(ITEM_SUBCLASS_ARMOR_CLOTH); --lighter)
            subclasses.push_back(lighter);

    // Candidates are weighted by source, and by closeness to the middle of the level's band: most characters wear
    // typical gear for their level, few the best or worst of it.
    auto const [low, high] = ItemLevelBand(level);
    float const center = (float(low) + float(high)) / 2.0f;
    float const halfWidth = std::max(1.0f, (float(high) - float(low)) / 2.0f);

    for (bool needStats : { true, false })
    {
        for (int32 armorSubclass : subclasses)
        {
            std::vector<Candidate const*> candidates = Window(pool, level, stats, armorSubclass, needStats, pvp);
            std::vector<double> weights;
            weights.reserve(candidates.size());
            for (Candidate const* c : candidates)
                weights.push_back(c->Weight / (1.0 + std::abs(float(c->ItemLevel) - center) / halfWidth));

            for (uint32 attempt = 0; attempt < EQUIP_ATTEMPTS && !candidates.empty(); ++attempt)
            {
                uint32 const pick = urandweighted(weights.size(), weights.data());
                Candidate const* candidate = candidates[pick];
                candidates.erase(candidates.begin() + pick);
                weights.erase(weights.begin() + pick);

                int32 const randomId = candidate->RandomIds.empty() ? 0
                    : candidate->RandomIds[urand(0, uint32(candidate->RandomIds.size()) - 1)];

                Item* item = Item::CreateItem(candidate->ItemId, 1, bot, false, randomId);
                if (!item)
                    continue;

                uint16 dest = 0;
                if (bot->CanEquipItem(slot, dest, item, false) != EQUIP_ERR_OK)
                {
                    delete item;
                    continue;
                }

                bot->EquipItem(dest, item, true);
                return true;
            }
        }
    }

    return false;
}

void Animus::Curriculum::GearBuilder::LearnProficiencies(Player* bot)
{
    for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
        if (!line || (line->categoryId != SKILL_CATEGORY_WEAPON && line->categoryId != SKILL_CATEGORY_ARMOR))
            continue;

        if (!bot->HasSkill(line->id) && GetSkillRaceClassInfo(line->id, bot->getRace(), bot->getClass()))
            bot->LearnDefaultSkill(line->id, 0);
    }

    bot->UpdateSkillsToMaxSkillsForLevel();
}

bool Animus::Curriculum::GearBuilder::EquipWeapons(Player* bot, SpecProfile const& spec, WeaponLayout layout,
    bool pvp) const
{
    StatProfile const stats = spec.Stats;

    switch (layout)
    {
        case WeaponLayout::TwoHand:
            return EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_TWO_HAND, stats, pvp);
        case WeaponLayout::Staff:
            return EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_TWO_HAND, stats, pvp, ITEM_SUBCLASS_WEAPON_STAFF);
        case WeaponLayout::DualWield:
        case WeaponLayout::DualWieldDaggers:
        {
            int32 const subclass = layout == WeaponLayout::DualWieldDaggers ? int32(ITEM_SUBCLASS_WEAPON_DAGGER) : -1;
            if (!bot->CanDualWield()
                || !EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats, pvp, subclass))
                return false;
            EquipFromPool(bot, EQUIPMENT_SLOT_OFFHAND, POOL_OFF_HAND, stats, pvp, subclass);
            return true;
        }
        case WeaponLayout::OneHand:
            return EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats, pvp);
        case WeaponLayout::OneHandShield:
            if (!EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats, pvp))
                return false;
            EquipFromPool(bot, EQUIPMENT_SLOT_OFFHAND, POOL_SHIELD, stats, pvp);
            return true;
        case WeaponLayout::OneHandHeld:
            if (!EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats, pvp))
                return false;
            EquipFromPool(bot, EQUIPMENT_SLOT_OFFHAND, POOL_HELD, stats, pvp);
            return true;
        case WeaponLayout::TwoHandRanged:
        {
            bool const ranged = EquipFromPool(bot, EQUIPMENT_SLOT_RANGED, POOL_RANGED, stats, pvp);
            bool const melee = EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_TWO_HAND, stats, pvp);
            return ranged || melee;
        }
    }

    return false;
}

void Animus::Curriculum::GearBuilder::Equip(Player* bot, SpecProfile const& spec, bool pvp) const
{
    // Starting outfit, the previous episode's set, bags and backpack contents.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    StatProfile const stats = spec.Stats;
    int32 const armor = int32(_kit.ArmorSubclass(bot->GetLevel()));

    EquipFromPool(bot, EQUIPMENT_SLOT_HEAD, POOL_HEAD, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_NECK, POOL_NECK, stats, pvp);
    EquipFromPool(bot, EQUIPMENT_SLOT_SHOULDERS, POOL_SHOULDERS, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_CHEST, POOL_CHEST, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_WAIST, POOL_WAIST, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_LEGS, POOL_LEGS, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_FEET, POOL_FEET, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_WRISTS, POOL_WRISTS, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_HANDS, POOL_HANDS, stats, pvp, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_FINGER1, POOL_FINGER, stats, pvp);
    EquipFromPool(bot, EQUIPMENT_SLOT_FINGER2, POOL_FINGER, stats, pvp);
    EquipFromPool(bot, EQUIPMENT_SLOT_TRINKET1, POOL_TRINKET, stats, pvp);
    EquipFromPool(bot, EQUIPMENT_SLOT_TRINKET2, POOL_TRINKET, stats, pvp);
    EquipFromPool(bot, EQUIPMENT_SLOT_BACK, POOL_BACK, stats, pvp);

    for (WeaponLayout layout : spec.Weapons)
    {
        if (EquipWeapons(bot, spec, layout, pvp))
            break;

        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED })
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    }

    if (spec.Wand && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        EquipFromPool(bot, EQUIPMENT_SLOT_RANGED, POOL_WAND, stats, pvp);

    // Paladins, shamans, druids and death knights carry a relic in the ranged slot.
    if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        EquipFromPool(bot, EQUIPMENT_SLOT_RANGED, POOL_RELIC, stats, pvp);

    EquipQuiver(bot);
    StoreAmmo(bot);
    _kit.StoreReagents(bot);
    Enhance(bot, spec);
}

void Animus::Curriculum::GearBuilder::StoreAmmo(Player* bot) const
{
    Item const* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged)
        return;

    std::vector<std::pair<uint8, uint32>> const* ammo = nullptr;
    switch (ranged->GetTemplate()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            ammo = &_arrows;
            break;
        case ITEM_SUBCLASS_WEAPON_GUN:
            ammo = &_bullets;
            break;
        default:
            return;
    }

    // The best ammo the level allows.
    uint32 itemId = 0;
    for (auto const& [reqLevel, id] : *ammo)
        if (reqLevel <= bot->GetLevel())
            itemId = id;

    if (itemId && StoreInBags(bot, itemId, AMMO_COUNT))
        bot->SetAmmo(itemId);
}

void Animus::Curriculum::WarmGearCaches()
{
    RandomEnchantGroups();
    GearStats::ItemSources();
    GearStats::ObtainableItems();
}
