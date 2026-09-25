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

#include "SeatCharacter.h"
#include "ClassAssets.h"
#include "Layout.h"
#include "Player.h"
#include "Random.h"
#include "SpellChecks.h"
#include "Supplies.h"
#include "TravelBlock.h"

Animus::Curriculum::SeatCharacter::Built Animus::Curriculum::SeatCharacter::Configure(Player* bot,
    Layout const& layout, uint8 specIndex, bool pvp, TalentPlan plan, uint32 noisePoints)
{
    ClassAssets const& assets = *layout.Assets;
    SpecProfile const& spec = layout.Profile->Specs[specIndex];

    // Every talent learned, every spell in the kit and every item equipped below recomputes the character's stats
    // on its own, and at level 80 that is some seventy talents, a kit walked twice and nineteen slots -- hundreds
    // of full recomputes to reach the one state that matters, per character, per episode. This is the core's own
    // pattern for applying a pile of modifiers at once (Player::_ApplyAllStatBonuses): hold the recompute, apply
    // everything, then recompute once at the end. The UpdateAllStats() that closes this function is that one.
    bot->SetCanModifyStats(false);

    GearBuilder::LearnProficiencies(bot);

    Built built;
    uint32 const points = bot->GetFreeTalentPoints();
    built.Plan = plan;
    switch (plan)
    {
        case TalentPlan::Noisy:
            built.Build = assets.Talents->Noisy(spec.Name, spec.TabPage, points, noisePoints);
            break;
        case TalentPlan::Random:
            built.Build = assets.Talents->Random(spec.TabPage, points);
            break;
        case TalentPlan::Standard:
            built.Build = assets.Talents->Standard(spec.Name, spec.TabPage, points);
            break;
    }
    built.UnspentTalentPoints = assets.Talents->Apply(bot, built.Build);

    assets.Kit->Learn(bot);
    if (layout.Has(BlockId::Travel))
        TravelBlock::LearnRiding(bot);
    assets.Talents->ApplyGlyphs(bot, spec.Name);
    assets.Gear->Equip(bot, spec, pvp);

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++built.EquippedItems;

    bot->SetCanModifyStats(true);
    bot->UpdateAllStats();
    bot->SetFullHealth();
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));
    bot->SetPower(POWER_RAGE, 0);
    bot->SetPower(POWER_RUNIC_POWER, 0);
    return built;
}

bool Animus::Curriculum::SeatCharacter::GivePet(Player* bot, std::vector<uint32> const& stable)
{
    enum : uint32
    {
        SPELL_SUMMON_IMP            = 688,
        SPELL_SUMMON_VOIDWALKER     = 697,
        SPELL_SUMMON_SUCCUBUS       = 712,
        SPELL_SUMMON_FELHUNTER      = 691,
        SPELL_SUMMON_FELGUARD       = 30146,
        SPELL_RAISE_DEAD            = 46584,
        SPELL_MASTER_OF_GHOULS      = 52143,
        SPELL_SUMMON_WATER_ELEMENTAL = 31687,
        SPELL_GLYPH_ETERNAL_WATER   = 70937,
    };

    if (bot->GetPetGUID())
        return true;

    // The summon a player would already have cast; triggered, so it takes no cast time and no reagent, and its
    // cooldown is cleared again: arriving with the pet out leaves the summon ready.
    auto const summon = [bot](uint32 spell)
    {
        bot->CastSpell(bot, spell, TRIGGERED_FULL_MASK);
        bot->RemoveSpellCooldown(spell, true);
        return bot->GetPetGUID() != ObjectGuid::Empty;
    };

    switch (bot->getClass())
    {
        case CLASS_HUNTER:
            return !stable.empty() && CallHunterBeast(bot, stable[urand(0, uint32(stable.size()) - 1)]);
        case CLASS_WARLOCK:
        {
            std::vector<uint32> known;
            for (uint32 spell : { SPELL_SUMMON_IMP, SPELL_SUMMON_VOIDWALKER, SPELL_SUMMON_SUCCUBUS,
                SPELL_SUMMON_FELHUNTER, SPELL_SUMMON_FELGUARD })
                if (bot->HasSpell(spell))
                    known.push_back(spell);

            return !known.empty() && summon(known[urand(0, uint32(known.size()) - 1)]);
        }
        case CLASS_DEATH_KNIGHT:
            return bot->HasSpell(SPELL_RAISE_DEAD) && bot->HasAura(SPELL_MASTER_OF_GHOULS) && summon(SPELL_RAISE_DEAD);
        case CLASS_MAGE:
            return bot->HasSpell(SPELL_SUMMON_WATER_ELEMENTAL) && bot->HasAura(SPELL_GLYPH_ETERNAL_WATER)
                && summon(SPELL_SUMMON_WATER_ELEMENTAL);
        default:
            return false;
    }
}

std::vector<uint32> Animus::Curriculum::SeatCharacter::PrepareFighter(Player* bot, Layout const& layout,
    Aptitude const& aptitude)
{
    using namespace SpellChecks;

    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    std::vector<uint32> stable = layout.Profile->Class == CLASS_HUNTER
        ? StablePool::Instance().Random(STABLE_SLOTS) : std::vector<uint32>();

    // A warrior needs a stance and only a first login casts one. Which stance follows from the build rather than
    // from a label: one carrying a taunt and real mitigation is going to be standing in front of things.
    if (layout.Profile->Class == CLASS_WARRIOR)
        bot->CastSpell(bot, AptitudeDemand::HoldsThePull().MetBy(aptitude) && aptitude[Aptitude::TAUNT] > 0.0f
            && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);

    return stable;
}
