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

#ifndef ANIMUS_LIB_CURRICULUM_SPEC_BUILDS_H
#define ANIMUS_LIB_CURRICULUM_SPEC_BUILDS_H

#include "Define.h"
#include <string_view>
#include <vector>

namespace Animus::Curriculum
{
    /// One talent of a standard build: its tree (tab page), name (the first rank's spell name) and ranks.
    struct TalentPick
    {
        uint8 Tab = 0;
        char const* Name = "";
        uint8 Ranks = 0;
    };

    /// The talents and glyphs players of a spec take, in the order they take them.
    struct SpecBuild
    {
        uint8 Class = 0;
        std::string_view Spec;                      // SpecProfile::Name
        std::vector<TalentPick> Talents;
        std::vector<char const*> MajorGlyphs;       // item names without "Glyph of ", best first
        std::vector<char const*> MinorGlyphs;
    };

    [[nodiscard]] std::vector<SpecBuild> const& SpecBuilds();
}

#endif
