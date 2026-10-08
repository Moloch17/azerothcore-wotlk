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

#ifndef ANIMUS_LIB_CURRICULUM_TALENT_BUILDER_H
#define ANIMUS_LIB_CURRICULUM_TALENT_BUILDER_H

#include "Define.h"
#include <array>
#include <map>
#include <string>
#include <vector>

class Player;

namespace Animus::Curriculum
{
    /// A class's three talent trees, the standard builds and glyphs of its specs (SpecBuilds), and random builds.
    ///
    /// A standard build spends points one at a time, each on the first talent of the spec's list that still wants
    /// ranks and can take a point right now (row requirement: 5 points per row in the tree; prerequisite talent at
    /// its required rank), so a low-level character has the talents players take first. A random build spends each
    /// point on a talent that can take one, drawn with the row as its weight so the build walks down the tree far
    /// enough to reach its last row rather than filling the cheap rows first. Points a standard build cannot place
    /// are spent randomly.
    ///
    /// A spec's list names the whole 71-point build and is spent as written: 53 to 61 points in its own tree,
    /// which is where its last row and the ability the spec is built around are, and the rest in a support tree.
    /// SPEC_TREE_POINTS is what the last row needs, not a ceiling -- a random build uses it to split its points
    /// the way a real one does.
    class TalentBuilder
    {
    public:
        static constexpr uint32 SPEC_TREE_POINTS = 51;  // the points the last row of a tree needs to unlock
        /// Every point a character can spend, which is what a level 80 has. Used where a build has to be described
        /// without a character to describe it against -- a spec's standard build, for composition.
        static constexpr uint32 MAX_POINTS = 71;
        static constexpr uint32 TREE_COUNT = 3;

        struct Talent
        {
            uint32 TalentId = 0;
            uint8 Tab = 0;                          // tab page 0-2
            uint32 Row = 0;
            uint8 MaxRank = 0;
            std::array<uint32, 5> RankSpells{};
            std::string Name;                       // the first rank's spell name
            int32 DependsOn = -1;                   // index into Talents()
            uint8 DependsOnRanks = 0;               // ranks the prerequisite needs
        };

        /// One learn step: LearnTalent(TalentId, Rank) with Rank 0-based.
        struct Step
        {
            uint32 Index = 0;                       // into Talents()
            uint8 Rank = 0;
        };

        struct Build
        {
            std::vector<uint8> Ranks;               // per talent
            std::vector<Step> Order;                // valid learning order
            std::array<uint32, TREE_COUNT> TreePoints{};
        };

        explicit TalentBuilder(uint8 playerClass);

        [[nodiscard]] std::vector<Talent> const& Talents() const { return _talents; }

        [[nodiscard]] Build Random(uint8 specTab, uint32 points) const;

        /// The spec's standard build (SpecBuilds) for `points`; random for a spec without one.
        [[nodiscard]] Build Standard(std::string const& spec, uint8 specTab, uint32 points) const;

        /// The spec's standard build with its last `move` points spent at random instead: a build a player might
        /// have, off the beaten path. `move` is clamped to the points there are.
        [[nodiscard]] Build Noisy(std::string const& spec, uint8 specTab, uint32 points, uint32 move) const;

        /// Fills the bot's unlocked glyph slots with the spec's glyphs its level may use, best first.
        void ApplyGlyphs(Player* bot, std::string const& spec) const;

        /// Learns the build on a bot with no talents. Returns the points left unspent (0 when the build
        /// and Player::LearnTalent agree).
        uint32 Apply(Player* bot, Build const& build) const;

    private:
        struct Pick
        {
            uint32 Index = 0;                       // into Talents()
            uint8 Ranks = 0;
        };

        struct Glyph
        {
            uint32 PropertiesId = 0;                // GlyphProperties.dbc
            uint8 ReqLevel = 0;                     // the glyph item's
        };

        struct SpecData
        {
            std::vector<Pick> Picks;
            std::vector<Glyph> Majors;
            std::vector<Glyph> Minors;
        };

        /// Spend `points` in the trees of `treeMask`, drawn with the row as the weight so a build walks down a
        /// tree rather than filling its cheap rows first.
        void Spend(Build& build, uint32 treeMask, uint32 points) const;
        [[nodiscard]] bool CanTake(Build const& build, uint32 index) const;

        std::vector<Talent> _talents;               // by tab, row, column
        std::map<std::string, SpecData> _specs;     // by SpecProfile::Name
    };
}

#endif
