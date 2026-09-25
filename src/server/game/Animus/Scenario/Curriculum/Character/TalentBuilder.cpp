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

#include "TalentBuilder.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpecBuilds.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <map>

namespace
{
    constexpr uint8 GLYPH_SLOTS = MAX_GLYPH_SLOT_INDEX;

    /// Player::InitGlyphsForLevel: the level a glyph slot opens at.
    uint8 GlyphSlotLevel(uint8 slot)
    {
        switch (slot)
        {
            case 0:
            case 1:  return 15;
            case 2:  return 50;
            case 3:  return 30;
            case 4:  return 70;
            default: return 80;
        }
    }

    /// The glyph a "Glyph of X" item teaches: its spell's APPLY_GLYPH effect.
    uint32 GlyphPropertiesOf(ItemTemplate const& proto)
    {
        for (_Spell const& spell : proto.Spells)
            if (SpellInfo const* info = spell.SpellId > 0 ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr)
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.Effect == SPELL_EFFECT_APPLY_GLYPH && effect.MiscValue > 0)
                        return uint32(effect.MiscValue);

        return 0;
    }
}

Animus::Curriculum::TalentBuilder::TalentBuilder(uint8 playerClass)
{
    uint32 const classMask = 1 << (playerClass - 1);

    std::map<uint32, uint8> tabPages;   // TalentTabID -> tab page
    for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
        if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(i))
            if (tab->ClassMask & classMask)
                tabPages[tab->TalentTabID] = uint8(tab->tabpage);

    std::vector<TalentEntry const*> entries;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        if (TalentEntry const* entry = sTalentStore.LookupEntry(i))
            if (tabPages.contains(entry->TalentTab))
                entries.push_back(entry);

    std::sort(entries.begin(), entries.end(), [&](TalentEntry const* a, TalentEntry const* b)
    {
        uint8 const tabA = tabPages[a->TalentTab];
        uint8 const tabB = tabPages[b->TalentTab];
        if (tabA != tabB)
            return tabA < tabB;
        return a->Row != b->Row ? a->Row < b->Row : a->Col < b->Col;
    });

    for (TalentEntry const* entry : entries)
    {
        Talent talent;
        talent.TalentId = entry->TalentID;
        talent.Tab = tabPages[entry->TalentTab];
        talent.Row = entry->Row;
        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
        {
            talent.RankSpells[rank] = entry->RankID[rank];
            if (entry->RankID[rank])
                talent.MaxRank = rank + 1;
        }

        if (SpellInfo const* first = sSpellMgr->GetSpellInfo(talent.RankSpells[0]))
            talent.Name = first->SpellName[LOCALE_enUS];

        _talents.push_back(talent);
    }

    // Prerequisites by index. Player::LearnTalent accepts the prerequisite at rank index DependsOnRank
    // or higher, i.e. DependsOnRank + 1 ranks.
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (!entries[i]->DependsOn)
            continue;

        for (size_t j = 0; j < entries.size(); ++j)
        {
            if (entries[j]->TalentID == entries[i]->DependsOn)
            {
                _talents[i].DependsOn = int32(j);
                _talents[i].DependsOnRanks = uint8(entries[i]->DependsOnRank + 1);
                break;
            }
        }
    }

    // Glyph items of the class, by name.
    std::map<std::string, ItemTemplate const*> glyphItems;
    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
        if (proto.Class == ITEM_CLASS_GLYPH && (proto.AllowableClass & classMask))
            glyphItems.emplace(proto.Name1, &proto);

    auto const resolveGlyphs = [&](std::vector<char const*> const& names, char const* spec)
    {
        std::vector<Glyph> glyphs;
        for (char const* name : names)
        {
            auto const item = glyphItems.find(std::string("Glyph of ") + name);
            uint32 const properties = item != glyphItems.end() ? GlyphPropertiesOf(*item->second) : 0;
            if (!properties || !sGlyphPropertiesStore.LookupEntry(properties))
            {
                LOG_WARN("module.animus", "Class {} {}: no glyph \"Glyph of {}\"", playerClass, spec, name);
                continue;
            }

            glyphs.push_back({ properties, uint8(std::min<uint32>(item->second->RequiredLevel, DEFAULT_MAX_LEVEL)) });
        }

        return glyphs;
    };

    for (SpecBuild const& specBuild : SpecBuilds())
    {
        if (specBuild.Class != playerClass)
            continue;

        SpecData& data = _specs[std::string(specBuild.Spec)];
        for (TalentPick const& pick : specBuild.Talents)
        {
            auto const talent = std::find_if(_talents.begin(), _talents.end(),
                [&pick](Talent const& t) { return t.Tab == pick.Tab && t.Name == pick.Name; });
            if (talent == _talents.end())
            {
                LOG_WARN("module.animus", "Class {} {}: no talent \"{}\" in tree {}", playerClass, specBuild.Spec,
                    pick.Name, pick.Tab);
                continue;
            }

            data.Picks.push_back({ uint32(talent - _talents.begin()), pick.Ranks });
        }

        // A list names the whole 71-point build: 53 to 61 in the spec's own tree, where its last row and the
        // ability the spec is built around live, and the rest in a support tree. It is spent as written.

        data.Majors = resolveGlyphs(specBuild.MajorGlyphs, specBuild.Spec.data());
        data.Minors = resolveGlyphs(specBuild.MinorGlyphs, specBuild.Spec.data());
    }
}

bool Animus::Curriculum::TalentBuilder::CanTake(Build const& build, uint32 index) const
{
    Talent const& talent = _talents[index];
    return build.Ranks[index] < talent.MaxRank && build.TreePoints[talent.Tab] >= talent.Row * MAX_TALENT_RANK
        && (talent.DependsOn < 0 || build.Ranks[talent.DependsOn] >= talent.DependsOnRanks);
}

Animus::Curriculum::TalentBuilder::Build Animus::Curriculum::TalentBuilder::Standard(std::string const& spec,
    uint8 specTab, uint32 points) const
{
    auto const data = _specs.find(spec);
    if (data == _specs.end())
        return Random(specTab, points);

    Build build;
    build.Ranks.assign(_talents.size(), 0);

    std::vector<uint8> wanted(_talents.size(), 0);
    for (Pick const& pick : data->second.Picks)
        wanted[pick.Index] = std::min<uint8>(uint8(wanted[pick.Index] + pick.Ranks), _talents[pick.Index].MaxRank);

    while (build.Order.size() < points)
    {
        auto const next = std::find_if(data->second.Picks.begin(), data->second.Picks.end(),
            [&](Pick const& pick)
            {
                return build.Ranks[pick.Index] < wanted[pick.Index] && CanTake(build, pick.Index);
            });
        if (next == data->second.Picks.end())
            break;

        build.Order.push_back({ next->Index, build.Ranks[next->Index] });
        ++build.Ranks[next->Index];
        ++build.TreePoints[_talents[next->Index].Tab];
    }

    // Only if the list cannot place them: its own tree first, then the others.
    uint32 const otherTrees = ((1u << TREE_COUNT) - 1) & ~(1u << specTab);
    Spend(build, 1u << specTab, points - uint32(build.Order.size()));
    Spend(build, otherTrees, points - uint32(build.Order.size()));
    return build;
}

Animus::Curriculum::TalentBuilder::Build Animus::Curriculum::TalentBuilder::Noisy(std::string const& spec,
    uint8 specTab, uint32 points, uint32 move) const
{
    // The standard build stops short, and the rest is spent as a random build spends: the early picks (the ones a
    // player takes first) are kept, the tail is somebody's own idea. Spend caps the spec tree like Standard does.
    move = std::min(move, points);
    Build build = Standard(spec, specTab, points - move);

    uint32 const otherTrees = ((1u << TREE_COUNT) - 1) & ~(1u << specTab);
    Spend(build, 1u << specTab, points - uint32(build.Order.size()));
    Spend(build, otherTrees, points - uint32(build.Order.size()));
    return build;
}

void Animus::Curriculum::TalentBuilder::ApplyGlyphs(Player* bot, std::string const& spec) const
{
    auto const data = _specs.find(spec);
    if (data == _specs.end())
        return;

    uint8 const level = bot->GetLevel();
    std::vector<uint32> used;
    for (uint8 slot = 0; slot < GLYPH_SLOTS; ++slot)
    {
        GlyphSlotEntry const* slotEntry = sGlyphSlotStore.LookupEntry(bot->GetGlyphSlot(slot));
        if (!slotEntry || level < GlyphSlotLevel(slot))
            continue;

        // TypeFlags: 0 major, 1 minor (GlyphSlot.dbc and GlyphProperties.dbc agree).
        std::vector<Glyph> const& glyphs = slotEntry->TypeFlags ? data->second.Minors : data->second.Majors;
        for (Glyph const& glyph : glyphs)
        {
            GlyphPropertiesEntry const* properties = sGlyphPropertiesStore.LookupEntry(glyph.PropertiesId);
            if (glyph.ReqLevel > level || !properties || properties->TypeFlags != slotEntry->TypeFlags
                || std::find(used.begin(), used.end(), glyph.PropertiesId) != used.end())
                continue;

            bot->CastSpell(bot, properties->SpellId, true);
            bot->SetGlyph(slot, glyph.PropertiesId, false);
            used.push_back(glyph.PropertiesId);
            break;
        }
    }
}

Animus::Curriculum::TalentBuilder::Build Animus::Curriculum::TalentBuilder::Random(uint8 specTab,
    uint32 points) const
{
    Build build;
    build.Ranks.assign(_talents.size(), 0);

    uint32 const specPoints = std::min(points, SPEC_TREE_POINTS);
    uint32 const otherTrees = ((1u << TREE_COUNT) - 1) & ~(1u << specTab);

    Spend(build, 1u << specTab, specPoints);
    Spend(build, otherTrees, points - uint32(build.Order.size()));

    // Only if the other trees could not take the rest: back to the spec tree, rather than leave points unspent.
    Spend(build, 1u << specTab, points - uint32(build.Order.size()));

    return build;
}

void Animus::Curriculum::TalentBuilder::Spend(Build& build, uint32 treeMask, uint32 points) const
{
    std::vector<uint32> candidates;
    std::vector<uint32> weights;
    for (uint32 point = 0; point < points; ++point)
    {
        candidates.clear();
        weights.clear();
        uint32 total = 0;
        for (uint32 i = 0; i < _talents.size(); ++i)
        {
            if (!(treeMask & (1u << _talents[i].Tab)) || !CanTake(build, i))
                continue;

            // Deeper rows are likelier, so a build walks down its tree instead of filling the cheap rows first and
            // arriving at the last row with no points left: a player spends the 5 a row needs and moves on.
            candidates.push_back(i);
            weights.push_back(_talents[i].Row + 1);
            total += _talents[i].Row + 1;
        }

        if (candidates.empty())
            return;

        uint32 roll = urand(0, total - 1);
        std::size_t pick = 0;
        while (pick + 1 < candidates.size() && roll >= weights[pick])
        {
            roll -= weights[pick];
            ++pick;
        }

        uint32 const chosen = candidates[pick];
        build.Order.push_back({ chosen, build.Ranks[chosen] });
        ++build.Ranks[chosen];
        ++build.TreePoints[_talents[chosen].Tab];
    }
}

uint32 Animus::Curriculum::TalentBuilder::Apply(Player* bot, Build const& build) const
{
    for (Step const& step : build.Order)
        bot->LearnTalent(_talents[step.Index].TalentId, step.Rank);

    return bot->GetFreeTalentPoints();
}
