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

#include "ClassAssets.h"
#include <mutex>
#include "ObjectMgr.h"
#include "Random.h"
#include <algorithm>
#include <array>

namespace
{
    /// A class's kit, talents and catalog, owned apart from the assets so they outlive any one lookup.
    struct SharedClassAssets
    {
        std::unique_ptr<Animus::Curriculum::ClassKit> Kit;
        std::unique_ptr<Animus::Curriculum::TalentBuilder> Talents;
        std::unique_ptr<Animus::Curriculum::ActionCatalog> Catalog;
    };
}

Animus::Curriculum::ClassAssets const& Animus::Curriculum::ClassAssets::For(
    ClassProfile const& profile)
{
    static std::map<uint8, SharedClassAssets> classes;
    static std::map<ClassProfile const*, ClassAssets> assets;
    // Warmed at startup (WarmCaches), so this is a lookup; built here only for a profile the warm-up missed, and
    // rebuilds run on the map threads. The entries never move once made.
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);

    if (auto const itr = assets.find(&profile); itr != assets.end())
        return itr->second;

    SharedClassAssets& shared = classes[profile.Class];
    if (!shared.Kit)
    {
        shared.Kit = std::make_unique<ClassKit>(profile.Class);
        shared.Talents = std::make_unique<TalentBuilder>(profile.Class);
        shared.Catalog = std::make_unique<ActionCatalog>(profile.Class, *shared.Kit, *shared.Talents);
    }

    ClassAssets& entry = assets[&profile];
    entry.Profile = &profile;
    for (uint8 race : PLAYABLE_RACES)
        if (sObjectMgr->GetPlayerInfo(race, profile.Class))
            entry.Races.push_back(race);

    entry.Kit = shared.Kit.get();
    entry.Talents = shared.Talents.get();
    entry.Catalog = shared.Catalog.get();
    entry.Gear = std::make_unique<GearBuilder>(profile, *shared.Kit);

    // What each spec is normally capable of, from its standard build at the cap and with no character in hand.
    // Composition reads this; a seat reads its own.
    entry.SpecAptitudes.reserve(profile.Specs.size());
    for (SpecProfile const& spec : profile.Specs)
        entry.SpecAptitudes.push_back(Aptitude::Of(entry,
            shared.Talents->Standard(spec.Name, spec.TabPage, TalentBuilder::MAX_POINTS), nullptr));

    return entry;
}

std::vector<uint8> Animus::Curriculum::ClassAssets::SpecsMeeting(AptitudeDemand demand) const
{
    std::vector<uint8> found;
    for (uint8 i = 0; i < uint8(SpecAptitudes.size()); ++i)
        if (demand.MetBy(SpecAptitudes[i]))
            found.push_back(i);

    return found;
}

bool Animus::Curriculum::ClassAssets::CanMeet(AptitudeDemand demand) const
{
    return std::any_of(SpecAptitudes.begin(), SpecAptitudes.end(),
        [demand](Aptitude const& aptitude) { return demand.MetBy(aptitude); });
}

Animus::Curriculum::ClassProfile const* Animus::Curriculum::ClassAssets::FindProfile(uint8 playerClass)
{
    for (ClassProfile const& profile : ClassProfiles())
        if (profile.Class == playerClass)
            return &profile;

    return nullptr;
}

std::vector<uint8> Animus::Curriculum::ClassAssets::ClassesFor(uint8 level, AptitudeDemand demand)
{
    // The level and race checks are cheap and come first, because For() builds a profile's assets and that takes
    // seconds. Both callers -- a scripted owner and a scripted enemy player -- only run in stages that warmed
    // every class at construction, so by the time this is asked For() is a map lookup.
    std::vector<uint8> classes;
    for (ClassProfile const& profile : ClassProfiles())
    {
        if (ClassKit::MinLevelOf(profile.Class) > level)
            continue;

        if (!std::any_of(PLAYABLE_RACES.begin(), PLAYABLE_RACES.end(),
            [&profile](uint8 race) { return sObjectMgr->GetPlayerInfo(race, profile.Class) != nullptr; }))
            continue;

        if (demand.Any() && !For(profile).CanMeet(demand))
            continue;

        classes.push_back(profile.Class);
    }

    return classes;
}

uint8 Animus::Curriculum::DrawSpec(ClassAssets const& assets, AptitudeDemand demand)
{
    ClassProfile const* profile = assets.Profile;
    if (!profile || profile->Specs.empty())
        return 0;

    // A class asked for something none of its builds can do falls back to any of them, which is what a caller
    // wants when the composition cannot be filled exactly -- a party wanting somebody to hold the pull out of a
    // run with no class that can.
    std::vector<uint8> const meeting = assets.SpecsMeeting(demand);
    if (meeting.empty())
        return uint8(urand(0, uint32(profile->Specs.size()) - 1));

    return meeting[urand(0, uint32(meeting.size()) - 1)];
}
