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

#include "PetTalents.h"
#include "Containers.h"
#include "DBCStores.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr uint32 PET_TALENT_TYPES = 3;      // CreatureFamilyEntry::petTalentType: ferocity, tenacity, cunning

    struct Pick
    {
        std::string_view Name;
        uint8 Ranks;
    };

    // The builds players took in 3.3.5, in the order they took the points: 16 points at level 80 fill each list's
    // first ten entries, and the rest are where Beast Mastery's four extra points go.
    constexpr std::array<Pick, 13> FEROCITY =
    { {
        { "Cobra Reflexes", 2 }, { "Great Stamina", 1 }, { "Spiked Collar", 3 }, { "Culling the Herd", 3 },
        { "Spider's Bite", 3 }, { "Call of the Wild", 1 }, { "Rabid", 1 }, { "Great Stamina", 2 },
        { "Wild Hunt", 1 }, { "Shark Attack", 2 }, { "Wild Hunt", 2 }, { "Lionhearted", 2 }, { "Great Stamina", 3 },
    } };

    constexpr std::array<Pick, 14> TENACITY =
    { {
        { "Great Stamina", 3 }, { "Spiked Collar", 3 }, { "Blood of the Rhino", 2 }, { "Guard Dog", 2 },
        { "Lionhearted", 2 }, { "Last Stand", 1 }, { "Taunt", 1 }, { "Natural Armor", 1 }, { "Silverback", 1 },
        { "Silverback", 2 }, { "Natural Armor", 2 }, { "Intervene", 1 }, { "Culling the Herd", 3 },
        { "Great Resistance", 3 },
    } };

    constexpr std::array<Pick, 15> CUNNING =
    { {
        { "Cobra Reflexes", 2 }, { "Great Stamina", 1 }, { "Owl's Focus", 2 }, { "Spiked Collar", 1 },
        { "Culling the Herd", 3 }, { "Cornered", 2 }, { "Spiked Collar", 3 }, { "Wolverine Bite", 1 },
        { "Bullheaded", 1 }, { "Wild Hunt", 1 }, { "Feeding Frenzy", 2 }, { "Wild Hunt", 2 },
        { "Roar of Recovery", 1 }, { "Great Stamina", 3 }, { "Carrion Feeder", 1 },
    } };

    constexpr std::array<std::string_view, 5> FAMILY_SPECIFIC = { "Dash", "Dive", "Charge", "Swoop", "Mobility" };

    struct Tree
    {
        std::map<std::string, TalentEntry const*, std::less<>> ByName;
        std::vector<TalentEntry const*> Talents;
    };

    std::string TalentName(TalentEntry const* talent)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(talent->RankID[0]);
        return info && info->SpellName[0] ? std::string(info->SpellName[0]) : std::string();
    }

    /// Every pet talent tree, by talent type, built once from the DBCs.
    std::array<Tree, PET_TALENT_TYPES> const& Trees()
    {
        static std::array<Tree, PET_TALENT_TYPES> const trees = []()
        {
            std::array<Tree, PET_TALENT_TYPES> built;
            for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
            {
                TalentEntry const* talent = sTalentStore.LookupEntry(i);
                TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
                if (!tab || !tab->petTalentMask || !talent->RankID[0])
                    continue;

                std::string const name = TalentName(talent);
                if (name.empty() || std::find(FAMILY_SPECIFIC.begin(), FAMILY_SPECIFIC.end(), name)
                    != FAMILY_SPECIFIC.end())
                    continue;

                for (uint32 type = 0; type < PET_TALENT_TYPES; ++type)
                {
                    if (!(tab->petTalentMask & (1u << type)))
                        continue;

                    built[type].ByName.emplace(name, talent);
                    built[type].Talents.push_back(talent);
                }
            }

            return built;
        }();

        return trees;
    }

    /// The pet's rank in `talent`: 0 when it has none.
    uint8 RankOf(Pet const* pet, TalentEntry const* talent)
    {
        for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            if (talent->RankID[rank] && pet->HasSpell(talent->RankID[rank]))
                return uint8(rank + 1);

        return 0;
    }

    /// One more rank of `talent`, if every rule allows it now.
    bool LearnNextRank(Player* owner, Pet* pet, TalentEntry const* talent)
    {
        uint32 const points = pet->GetFreeTalentPoints();
        uint8 const rank = RankOf(pet, talent);
        if (!points || rank >= MAX_PET_TALENT_RANK || !talent->RankID[rank])
            return false;

        owner->LearnPetTalent(pet->GetGUID(), talent->TalentID, rank);
        return pet->GetFreeTalentPoints() < points;
    }
}

uint32 Animus::Curriculum::PetTalents::Spend(Player* owner, Pet* pet)
{
    if (!owner || !pet || pet->getPetType() != HUNTER_PET)
        return 0;

    CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(pet->GetCreatureTemplate()->family);
    if (!family || family->petTalentType < 0 || uint32(family->petTalentType) >= PET_TALENT_TYPES)
        return pet->GetFreeTalentPoints();

    uint32 const type = uint32(family->petTalentType);
    Tree const& tree = Trees()[type];

    Pick const* first = nullptr;
    std::size_t count = 0;
    switch (type)
    {
        case 0: first = FEROCITY.data(); count = FEROCITY.size(); break;
        case 1: first = TENACITY.data(); count = TENACITY.size(); break;
        default: first = CUNNING.data(); count = CUNNING.size(); break;
    }

    // The standard build, in order: each pick raises its talent to its rank as far as the rules allow right now.
    for (std::size_t i = 0; i < count && pet->GetFreeTalentPoints(); ++i)
    {
        auto const talent = tree.ByName.find(first[i].Name);
        if (talent == tree.ByName.end())
            continue;

        while (RankOf(pet, talent->second) < first[i].Ranks && LearnNextRank(owner, pet, talent->second))
            ;
    }

    // What the list could not place: a random talent that takes a point, deeper rows first, until none does.
    std::vector<TalentEntry const*> candidates = tree.Talents;
    while (pet->GetFreeTalentPoints())
    {
        Acore::Containers::RandomShuffle(candidates);
        std::stable_sort(candidates.begin(), candidates.end(), [](TalentEntry const* a, TalentEntry const* b)
        {
            return a->Row > b->Row;
        });

        bool learned = false;
        for (TalentEntry const* talent : candidates)
        {
            if (LearnNextRank(owner, pet, talent))
            {
                learned = true;
                break;
            }
        }

        if (!learned)
            break;
    }

    return pet->GetFreeTalentPoints();
}
