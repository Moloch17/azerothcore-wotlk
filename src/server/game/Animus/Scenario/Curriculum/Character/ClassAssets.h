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

#ifndef ANIMUS_LIB_CURRICULUM_CLASS_ASSETS_H
#define ANIMUS_LIB_CURRICULUM_CLASS_ASSETS_H

#include "ActionCatalog.h"
#include "ClassKit.h"
#include "ClassProfile.h"
#include "Aptitude.h"
#include "GearBuilder.h"
#include "TalentBuilder.h"
#include <map>
#include <memory>
#include <vector>

namespace Animus::Curriculum
{
    /// Everything needed to build and play a character of one class: its kit, talents and action catalog, gear for
    /// every spec it has, and the races that can be it.
    ///
    /// Built once per class and shared by every scenario and scripted player that needs it: item pools and trainer
    /// data take a few seconds each. The kit, the talents and the catalog were always shared by the class's roles;
    /// now the gear is too, because one GearBuilder covers every spec in the profile it was given.
    struct ClassAssets
    {
        ClassProfile const* Profile = nullptr;
        std::vector<uint8> Races;
        ClassKit const* Kit = nullptr;
        TalentBuilder const* Talents = nullptr;
        ActionCatalog const* Catalog = nullptr;
        std::unique_ptr<GearBuilder> Gear;
        /// What each of the class's specs can do, read off its standard build at the level cap -- one entry per
        /// Profile->Specs, in the same order.
        ///
        /// Composition has to choose a character before there is a character to measure, so it measures the
        /// template instead: this is "what a build down this tree is normally capable of", which is exactly the
        /// question "who can hold this pull" is really asking. What a *seat* observes is its own Aptitude, read
        /// off the talents and gear it actually got, which may be nothing like the template.
        std::vector<Aptitude> SpecAptitudes;

        /// The assets of a profile of ClassProfiles(), built on first use (world thread only).
        static ClassAssets const& For(ClassProfile const& profile);

        /// The profile of `playerClass`, if it is a class that is played.
        static ClassProfile const* FindProfile(uint8 playerClass);

        /// The indices of Profile->Specs whose standard build meets `demand`, in order; empty if none do.
        [[nodiscard]] std::vector<uint8> SpecsMeeting(AptitudeDemand demand) const;
        /// Whether any of them does.
        [[nodiscard]] bool CanMeet(AptitudeDemand demand) const;

        /// Classes a player of `level` can be with a spec that meets `demand`.
        static std::vector<uint8> ClassesFor(uint8 level, AptitudeDemand demand);
    };

    /// A random spec of `assets` whose standard build meets `demand`, as an index into Profile->Specs. A class that
    /// has none falls back to any of its specs.
    [[nodiscard]] uint8 DrawSpec(ClassAssets const& assets, AptitudeDemand demand);
}

#endif
