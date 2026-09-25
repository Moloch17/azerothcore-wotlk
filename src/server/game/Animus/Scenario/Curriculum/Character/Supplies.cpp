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

#include "Supplies.h"
#include "DatabaseEnv.h"
#include "GearStats.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PetTalents.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "WorldCreatures.h"
#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <unordered_set>

namespace
{
    enum SupplySpells : uint32
    {
        SPELL_TAME_BEAST            = 1515,
    };

    /// Flasks at the expansion level caps, where characters are geared for dungeons; while levelling an elixir now
    /// and then.
    constexpr std::array<uint8, 2> FLASK_LEVELS = { 70, 80 };
    constexpr int32 LEVELLING_ELIXIR_CHANCE = 50;

    bool EnergizesMana(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_ENERGIZE && effect.MiscValue == POWER_MANA)
                return true;

        return false;
    }

    /// Whether a flask's or elixir's auras suit a profile, judged as the item stats they stand for.
    bool BuffSuits(SpellInfo const* info, Animus::Curriculum::StatProfile stats)
    {
        using namespace Animus::Curriculum::GearStats;
        using Animus::Curriculum::StatProfile;

        StatVerdict verdict;
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_APPLY_AURA)
                continue;

            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_STAT:
                    switch (effect.MiscValue)
                    {
                        case STAT_STRENGTH:  verdict.Add(stats, ITEM_MOD_STRENGTH); break;
                        case STAT_AGILITY:   verdict.Add(stats, ITEM_MOD_AGILITY); break;
                        case STAT_STAMINA:   verdict.Add(stats, ITEM_MOD_STAMINA); break;
                        case STAT_INTELLECT: verdict.Add(stats, ITEM_MOD_INTELLECT); break;
                        case STAT_SPIRIT:    verdict.Add(stats, ITEM_MOD_SPIRIT); break;
                        default:             verdict.Wanted = true; break;     // all stats
                    }
                    break;
                case SPELL_AURA_MOD_ATTACK_POWER:
                    verdict.Add(stats, ITEM_MOD_ATTACK_POWER);
                    break;
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                    verdict.Add(stats, ITEM_MOD_RANGED_ATTACK_POWER);
                    break;
                case SPELL_AURA_MOD_DAMAGE_DONE:
                    // Spell damage (magic schools), or all damage.
                    if (effect.MiscValue & SPELL_SCHOOL_MASK_MAGIC)
                        verdict.Add(stats, ITEM_MOD_SPELL_POWER);
                    break;
                case SPELL_AURA_MOD_HEALING_DONE:
                    verdict.Add(stats, ITEM_MOD_SPELL_HEALING_DONE);
                    break;
                case SPELL_AURA_MOD_POWER_REGEN:
                    if (effect.MiscValue == POWER_MANA)
                        verdict.Add(stats, ITEM_MOD_MANA_REGENERATION);
                    break;
                case SPELL_AURA_MOD_INCREASE_HEALTH:
                    (stats == StatProfile::Tank ? verdict.Wanted : verdict.Forbidden) = true;
                    break;
                case SPELL_AURA_MOD_RATING:
                    for (uint32 rating = CR_DEFENSE_SKILL; rating < MAX_COMBAT_RATING; ++rating)
                    {
                        if (!(effect.MiscValue & (1 << rating)))
                            continue;

                        if (rating <= CR_HASTE_SPELL)
                            verdict.Add(stats, rating + ITEM_MOD_DEFENSE_SKILL_RATING - CR_DEFENSE_SKILL);
                        else if (rating == CR_EXPERTISE)
                            verdict.Add(stats, ITEM_MOD_EXPERTISE_RATING);
                        else if (rating == CR_ARMOR_PENETRATION)
                            verdict.Add(stats, ITEM_MOD_ARMOR_PENETRATION_RATING);
                    }
                    break;
                default:
                    break;
            }
        }

        return verdict.Suits();
    }
}

Animus::Curriculum::ConsumablePool const& Animus::Curriculum::ConsumablePool::Instance()
{
    static ConsumablePool const pool;
    return pool;
}

Animus::Curriculum::ConsumablePool::ConsumablePool()
{
    std::unordered_set<uint32> const& obtainable = GearStats::ObtainableItems();
    std::unordered_set<uint32> sold;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT CAST(item AS SIGNED) FROM npc_vendor"))
    {
        do
        {
            if (int64 const item = result->Fetch()[0].Get<int64>(); item > 0)
                sold.insert(uint32(item));
        } while (result->NextRow());
    }

    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.Class != ITEM_CLASS_CONSUMABLE || proto.Map || proto.Area || !obtainable.contains(itemId))
            continue;

        uint8 const requiredLevel = uint8(std::min<uint32>(proto.RequiredLevel, DEFAULT_MAX_LEVEL));
        SpellInfo const* onUse = GearStats::ItemUseSpell(&proto);
        if (!onUse)
            continue;

        switch (proto.SubClass)
        {
            case ITEM_SUBCLASS_POTION:
                if (proto.RequiredSkill)
                    break;
                if (onUse->HasEffect(SPELL_EFFECT_HEAL))
                    _healthPotions.emplace_back(requiredLevel, itemId);
                else if (EnergizesMana(onUse))
                    _manaPotions.emplace_back(requiredLevel, itemId);
                break;
            case ITEM_SUBCLASS_BANDAGE:
                if (proto.RequiredSkill == SKILL_FIRST_AID
                    && (onUse->HasEffect(SPELL_EFFECT_HEAL) || onUse->HasAura(SPELL_AURA_PERIODIC_HEAL)))
                    _bandages.emplace_back(uint16(proto.RequiredSkillRank), itemId);
                break;
            case ITEM_SUBCLASS_CONSUMABLE:
                // Conjured by warlocks.
                if (proto.Name1.ends_with("Healthstone") && onUse->HasEffect(SPELL_EFFECT_HEAL))
                    _healthstones.emplace_back(requiredLevel, itemId);
                else if (proto.Name1.ends_with("Soulstone") && onUse->HasAura(SPELL_AURA_DUMMY))
                    _soulstones.emplace_back(requiredLevel, itemId);
                break;
            case ITEM_SUBCLASS_FLASK:
            case ITEM_SUBCLASS_ELIXIR:
                if (proto.RequiredSkill)
                    break;
                for (StatProfile stats : { StatProfile::StrengthMelee, StatProfile::AgilityMelee, StatProfile::Ranged,
                    StatProfile::Caster, StatProfile::Healer, StatProfile::Tank })
                    if (BuffSuits(onUse, stats))
                        _buffs[stats].push_back({ requiredLevel, uint16(proto.ItemLevel), onUse->Id,
                            proto.SubClass == ITEM_SUBCLASS_FLASK });
                break;
            default:
                break;
        }

        if (proto.SubClass != ITEM_SUBCLASS_FOOD || !sold.contains(itemId) || proto.RequiredSkill
            || proto.RequiredReputationFaction)
            continue;

        if (onUse->HasAura(SPELL_AURA_MOD_REGEN))
            _food.emplace_back(requiredLevel, itemId);
        else if (onUse->HasAura(SPELL_AURA_MOD_POWER_REGEN))
            _drink.emplace_back(requiredLevel, itemId);
    }

    for (auto* items : { &_food, &_drink, &_healthPotions, &_manaPotions, &_healthstones, &_soulstones })
        std::sort(items->begin(), items->end());
    std::sort(_bandages.begin(), _bandages.end());

    LOG_DEBUG("module.animus", "Consumables: {} foods, {} drinks sold by vendors; {} healing and {} mana potions, {} "
        "bandages, {} healthstones, {} soulstones", _food.size(), _drink.size(), _healthPotions.size(),
        _manaPotions.size(), _bandages.size(), _healthstones.size(), _soulstones.size());
}

Animus::Curriculum::BattleSupplies Animus::Curriculum::ConsumablePool::Supplies(uint8 level, bool usesMana,
    bool warlock, bool warlockInParty) const
{
    BattleSupplies supplies;
    supplies.HealthPotion = Best(_healthPotions, level);
    supplies.ManaPotion = usesMana ? Best(_manaPotions, level) : 0;
    supplies.Healthstone = warlock || warlockInParty ? Best(_healthstones, level) : 0;
    supplies.Soulstone = warlock ? Best(_soulstones, level) : 0;

    uint16 const firstAid = FirstAidSkill(level);
    for (auto const& [skill, itemId] : _bandages)
        if (skill <= firstAid)
            supplies.Bandage = itemId;

    return supplies;
}

uint32 Animus::Curriculum::ConsumablePool::BuffSpell(uint8 level, StatProfile stats) const
{
    auto const buffs = _buffs.find(stats);
    if (buffs == _buffs.end())
        return 0;

    bool const flask = std::find(FLASK_LEVELS.begin(), FLASK_LEVELS.end(), level) != FLASK_LEVELS.end();
    if (!flask && !roll_chance_i(LEVELLING_ELIXIR_CHANCE))
        return 0;

    // The newest of the kind the level allows; equally new ones are equally likely.
    std::vector<Buff const*> best;
    for (Buff const& buff : buffs->second)
    {
        if (buff.Flask != flask || buff.ReqLevel > level)
            continue;

        if (!best.empty() && std::tie(buff.ReqLevel, buff.ItemLevel) > std::tie(best[0]->ReqLevel, best[0]->ItemLevel))
            best.clear();

        if (best.empty() || std::tie(buff.ReqLevel, buff.ItemLevel) == std::tie(best[0]->ReqLevel, best[0]->ItemLevel))
            best.push_back(&buff);
    }

    return best.empty() ? 0 : best[urand(0, uint32(best.size()) - 1)]->SpellId;
}

uint16 Animus::Curriculum::ConsumablePool::FirstAidSkill(uint8 level)
{
    // 75 per 15 levels to 300 at 60, then 7.5 per level to 450 at 80.
    if (level <= 60)
        return uint16(level * 5);
    return uint16(std::min(450.0f, 300.0f + float(level - 60) * 7.5f));
}

uint32 Animus::Curriculum::ConsumablePool::Best(std::vector<std::pair<uint8, uint32>> const& items, uint8 level)
{
    uint32 best = 0;
    for (auto const& [reqLevel, itemId] : items)
        if (reqLevel <= level)
            best = itemId;

    return best;
}

Animus::Curriculum::StablePool const& Animus::Curriculum::StablePool::Instance()
{
    static StablePool const pool;
    return pool;
}

Animus::Curriculum::StablePool::StablePool()
{
    std::unordered_set<uint32> const& spawned = WorldCreatures::SpawnedIds();
    std::unordered_set<uint32> const& walkers = WorldCreatures::WaypointWalkerIds();

    std::map<uint32, std::vector<uint32>> beastsByFamily;
    for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
        if (spawned.contains(entry) && !walkers.contains(entry) && info.IsTameable(false))
            beastsByFamily[info.family].push_back(entry);

    for (auto& [family, entries] : beastsByFamily)
        _beastsByFamily.push_back(std::move(entries));

    LOG_DEBUG("module.animus", "Stable: {} tameable beast families", _beastsByFamily.size());
}

std::vector<uint32> Animus::Curriculum::StablePool::Random(uint32 count) const
{
    std::vector<uint32> families(_beastsByFamily.size());
    for (uint32 i = 0; i < families.size(); ++i)
        families[i] = i;

    std::vector<uint32> stable;
    while (stable.size() < count && !families.empty())
    {
        uint32 const pick = urand(0, uint32(families.size()) - 1);
        std::vector<uint32> const& entries = _beastsByFamily[families[pick]];
        stable.push_back(entries[urand(0, uint32(entries.size()) - 1)]);
        families.erase(families.begin() + pick);
    }

    return stable;
}

bool Animus::Curriculum::StoreInBags(Player* bot, uint32 itemId, uint32 count)
{
    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK)
        return false;

    return bot->StoreNewItem(dest, itemId, true) != nullptr;
}

void Animus::Curriculum::StockBattleSupplies(Player* bot, BattleSupplies const& supplies, StatProfile stats)
{
    auto const stock = [bot](uint32 item, uint32 count)
    {
        for (uint32 have = item ? bot->GetItemCount(item) : count; have < count; ++have)
            if (!StoreInBags(bot, item, 1))
                break;
    };

    stock(supplies.HealthPotion, CONSUMABLE_COUNT);
    stock(supplies.ManaPotion, CONSUMABLE_COUNT);
    stock(supplies.Healthstone, 1);
    stock(supplies.Soulstone, 1);

    if (supplies.Bandage)
    {
        uint16 const firstAid = ConsumablePool::FirstAidSkill(bot->GetLevel());
        bot->SetSkill(SKILL_FIRST_AID, uint16(std::max(1, (firstAid + 74) / 75)), std::max<uint16>(1, firstAid),
            uint16(std::max(75, (firstAid + 74) / 75 * 75)));
        stock(supplies.Bandage, CONSUMABLE_COUNT);
    }

    if (uint32 const buff = ConsumablePool::Instance().BuffSpell(bot->GetLevel(), stats))
        bot->CastSpell(bot, buff, true);
}

void Animus::Curriculum::StockConsumables(Player* bot, uint32 food, uint32 drink, uint32 count)
{
    for (uint32 item : { food, drink })
    {
        if (!item)
            continue;

        for (uint32 carried = bot->GetItemCount(item); carried < count; ++carried)
            if (!StoreInBags(bot, item, 1))
                break;
    }
}

bool Animus::Curriculum::CanCallHunterBeast(Player* bot)
{
    if (bot->getClass() != CLASS_HUNTER || bot->GetLevel() < HUNTER_PET_LEVEL)
        return false;

    if (!bot->GetPetGUID())
        return true;

    Pet* pet = bot->GetPet();
    return pet && !pet->IsAlive();
}

bool Animus::Curriculum::CallHunterBeast(Player* bot, uint32 entry)
{
    if (!entry || !CanCallHunterBeast(bot))
        return false;

    // A dead pet is dismissed first: the stable holds one current pet, and a corpse still is it.
    if (Pet* dead = bot->GetPet(); dead && !dead->IsAlive())
        bot->RemovePet(dead, PET_SAVE_AS_DELETED);

    Pet* pet = bot->CreateTamedPetFrom(entry, SPELL_TAME_BEAST);
    if (!pet)
        return false;

    // Spell::EffectTameCreature without the database save: the bot and its pet are never saved.
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, bot->GetLevel());
    pet->GetMap()->AddToMap(pet->ToCreature(), true);
    bot->SetMinion(pet, true);

    // As a player's pet is: fed (a freshly tamed beast is unhappy and deals 75% damage; a happy one 125%) and with its
    // talent points spent.
    pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
    pet->InitTalentForLevel();
    PetTalents::Spend(bot, pet);
    bot->PetSpellInitialize();
    return true;
}
