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

#include "ClassKit.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Supplies.h"
#include <algorithm>
#include <set>

namespace
{
    struct QuestSpell
    {
        uint8 Class;
        uint32 SpellId;
        uint8 ReqLevel;
    };

    /// Spells the class quests give rather than the trainers.
    constexpr std::array<QuestSpell, 7> QUEST_SPELLS =
    {{
        { CLASS_WARRIOR,        71,     10 },   // Defensive Stance
        { CLASS_WARRIOR,        2458,   30 },   // Berserker Stance
        { CLASS_DRUID,          5487,   10 },   // Bear Form
        { CLASS_WARLOCK,        697,    10 },   // Summon Voidwalker
        { CLASS_WARLOCK,        712,    20 },   // Summon Succubus
        { CLASS_WARLOCK,        691,    30 },   // Summon Felhunter
        { CLASS_DEATH_KNIGHT,   46584,  55 },   // Raise Dead
    }};

    struct ClassReagent
    {
        uint8 Class;
        uint32 ItemId;
        uint32 Count;
        uint8 ReqLevel;
    };

    constexpr std::array<ClassReagent, 12> CLASS_REAGENTS =
    {{
        { CLASS_SHAMAN,         5175,   1,  1 },    // Earth Totem
        { CLASS_SHAMAN,         5176,   1,  1 },    // Fire Totem
        { CLASS_SHAMAN,         5177,   1,  1 },    // Water Totem
        { CLASS_SHAMAN,         5178,   1,  1 },    // Air Totem
        { CLASS_SHAMAN,         17030,  5,  30 },   // Ankh (Reincarnation)
        // Water Breathing and Water Walking consume one each (Spell.dbc reagents 17057 and 17058). Without a stack
        // the cast fails its reagent check, and the breathe drill's shamans were never once offered either: the
        // catalog listed them and the mask refused them on every decision of every episode.
        { CLASS_SHAMAN,         17057,  20, 22 },   // Shiny Fish Scales (Water Breathing)
        { CLASS_SHAMAN,         17058,  20, 28 },   // Fish Oil (Water Walking)
        // Soul Shards do not stack: 20 filled the 16-slot backpack, and no potion, bandage, healthstone or soulstone
        // fit after them. A pack fight's summons and Soul Fires need a few; Drain Soul makes more into free slots.
        { CLASS_WARLOCK,        6265,   5,  1 },    // Soul Shard
        { CLASS_DEATH_KNIGHT,   37201,  20, 55 },   // Corpse Dust (Raise Dead without a corpse)
        { CLASS_ROGUE,          5140,   20, 22 },   // Flash Powder (Vanish)
        // Slow Fall and Levitate consume one each. Without a stack the cast fails its reagent check until the glyph
        // that waives it is slotted, and that is the mage's second minor glyph (level 50) and the priest's third
        // (level 70): every seat below those levels had no feather-fall button at all.
        { CLASS_MAGE,           17056,  20, 12 },   // Light Feather (Slow Fall)
        { CLASS_PRIEST,         17056,  20, 34 },   // Light Feather (Levitate)
    }};

    constexpr uint8 DEATH_KNIGHT_START_LEVEL = 55;

    /// A trainer spell with a LEARN_SPELL effect teaches the effect's spell; others are learned as is.
    std::vector<uint32> TaughtSpells(uint32 trainerSpellId)
    {
        std::vector<uint32> taught;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(trainerSpellId))
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    taught.push_back(effect.TriggerSpell);

        if (taught.empty())
            taught.push_back(trainerSpellId);

        return taught;
    }
}

Animus::Curriculum::ClassKit::ClassKit(uint8 playerClass) : _class(playerClass)
{
    std::set<uint32> seen;

    // Every class trainer of the class (Type 0 = class trainer, Requirement = class id).
    if (QueryResult result = WorldDatabase.Query("SELECT ts.SpellId, ts.ReqLevel, ts.ReqAbility1, ts.ReqAbility2, "
        "ts.ReqAbility3 FROM trainer t JOIN trainer_spell ts ON ts.TrainerId = t.Id WHERE t.Type = 0 AND "
        "t.Requirement = {}", playerClass))
    {
        do
        {
            Field* fields = result->Fetch();
            for (uint32 spellId : TaughtSpells(fields[0].Get<uint32>()))
            {
                if (!sSpellMgr->GetSpellInfo(spellId) || !seen.insert(spellId).second)
                    continue;

                KitSpell spell;
                spell.SpellId = spellId;
                spell.ReqLevel = fields[1].Get<uint8>();
                spell.ReqAbility = { fields[2].Get<uint32>(), fields[3].Get<uint32>(), fields[4].Get<uint32>() };
                _spells.push_back(spell);
            }
        } while (result->NextRow());
    }

    for (QuestSpell const& quest : QUEST_SPELLS)
        if (quest.Class == playerClass && sSpellMgr->GetSpellInfo(quest.SpellId) && seen.insert(quest.SpellId).second)
            _spells.push_back({ quest.SpellId, quest.ReqLevel, {} });

    std::stable_sort(_spells.begin(), _spells.end(),
        [](KitSpell const& a, KitSpell const& b) { return a.ReqLevel < b.ReqLevel; });

    for (ClassReagent const& reagent : CLASS_REAGENTS)
        if (reagent.Class == playerClass)
            _reagents.push_back({ reagent.ItemId, reagent.Count, reagent.ReqLevel });

    if (_spells.empty())
        LOG_ERROR("module.animus", "No trainer spells found for class {}; bots will only know their starting spells",
            playerClass);
}

uint8 Animus::Curriculum::ClassKit::MinLevelOf(uint8 playerClass)
{
    return playerClass == CLASS_DEATH_KNIGHT ? DEATH_KNIGHT_START_LEVEL : 1;
}

void Animus::Curriculum::ClassKit::Learn(Player* bot) const
{
    uint8 const level = bot->GetLevel();

    // Two passes: a rank whose required ability is itself a kit spell of the same level may come
    // before it in the list.
    for (uint32 pass = 0; pass < 2; ++pass)
    {
        for (KitSpell const& spell : _spells)
        {
            if (spell.ReqLevel > level || bot->HasSpell(spell.SpellId))
                continue;

            bool const abilitiesKnown = std::all_of(spell.ReqAbility.begin(), spell.ReqAbility.end(),
                [bot](uint32 ability) { return !ability || bot->HasSpell(ability); });

            if (abilitiesKnown)
                bot->learnSpell(spell.SpellId);
        }
    }
}

void Animus::Curriculum::ClassKit::StoreReagents(Player* bot) const
{
    for (Reagent const& reagent : _reagents)
    {
        if (reagent.ReqLevel > bot->GetLevel())
            continue;

        // Some reagents do not stack (Soul Shard), and a store is all-or-nothing: add one at a time
        // until the bags are full.
        uint32 stored = 0;
        while (stored < reagent.Count && StoreInBags(bot, reagent.ItemId, 1))
            ++stored;

        if (!stored)
            LOG_WARN("module.animus", "Could not give reagent {} to {}", reagent.ItemId, bot->GetName());
    }
}

uint32 Animus::Curriculum::ClassKit::ArmorSubclass(uint8 level) const
{
    switch (_class)
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
            return level >= 40 ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
        case CLASS_DEATH_KNIGHT:
            return ITEM_SUBCLASS_ARMOR_PLATE;
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return level >= 40 ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        default:
            return ITEM_SUBCLASS_ARMOR_CLOTH;
    }
}
