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

#ifndef ANIMUS_LIB_CURRICULUM_LAYOUT_H
#define ANIMUS_LIB_CURRICULUM_LAYOUT_H

#include "ActionCatalog.h"
#include "Block.h"
#include "ClassAssets.h"
#include "ClassProfile.h"
#include <optional>
#include <string>
#include <vector>

/*
 * What a class/role policy sees and does at a curriculum stage: the stage's blocks, one after the other, each with its
 * slice of the observation row and the action mask. A layout's manifest (Manifest) records everything its meaning
 * depends on and is exported with the layout's model; whatever plays the model must build the same manifest.
 */
namespace Animus::Curriculum
{
    struct StageDefinition;

    struct Layout
    {
        uint16 Index = 0;                           // position among the layouts of a run (the learner's id)
        StageDefinition const* Stage = nullptr;
        ClassProfile const* Profile = nullptr;
        ClassAssets const* Assets = nullptr;
        uint32 ObsDim = 0;
        uint32 NumActions = 0;
        /// The side's commander rather than a character: no class, no role, no catalog, and its own fixed
        /// observation row and action space (DirectorLayout). One of these exists per run that has a directed
        /// arena, shared by both sides and seeded down the stage chain like any other layout.
        bool Director = false;
        std::vector<BlockId> Blocks;                // the stage's blocks, in layout order
        std::array<BlockSlice, BLOCK_COUNT> Slices{};
        std::vector<ActionCatalog::Action> AllyRevives; // resurrections and the soulstone (Catalog().Revives())
        /// The layout's long buffs (ActionCatalog::IsLongBuff) as buffs a unit can have at once: chains that share a
        /// spell group (Blessings of Might and Kings, Fortitude and Prayer of Fortitude) are one group. Each group
        /// lists every rank's spell id. Buff coverage (a friend's share of groups up) reads them.
        std::vector<std::vector<uint32>> BuffGroups;
        /// Per action: Block::ModeGroupOf (as uint8), resolved once for SeatMemory.
        std::vector<uint8> ModeGroups;

        /// The layout of `profile` at `stage` (Index 0). Builds the profile's assets on first use.
        [[nodiscard]] static Layout Build(ClassProfile const& profile, StageDefinition const& stage);

        /// The director's layout at `stage` (Index 0). Carries no blocks: what a director sees and says is one
        /// fixed thing in every scenario, which is the whole point of having one network for all of them.
        [[nodiscard]] static Layout BuildDirector(StageDefinition const& stage);

        [[nodiscard]] bool Has(BlockId block) const { return (_blockMask >> uint32(block)) & 1; }
        [[nodiscard]] BlockSlice const& Slice(BlockId block) const { return Slices[std::size_t(block)]; }
        [[nodiscard]] ActionCatalog const& Catalog() const { return *Assets->Catalog; }
        [[nodiscard]] uint8 PlayerClass() const { return Profile ? Profile->Class : 0; }

        /// The block whose actions contain `action`, if any.
        [[nodiscard]] std::optional<BlockId> BlockOfAction(uint32 action) const;

        /// The model name of this layout: the class/role plus the stage suffix (warrior_dps_party).
        [[nodiscard]] std::string ModelName() const;

        /// Everything the layout's meaning depends on, as compact JSON: the stage, class/role, sizes and specs, then
        /// per block its name, observation and action spans and its own entries (spells, talents, slot counts).
        [[nodiscard]] std::string Manifest() const;

        /// Every action's name, by layout action index (Block::ActionName).
        [[nodiscard]] std::vector<std::string> ActionNames() const;

    private:
        uint32 _blockMask = 0;
    };

    /// A manifest spell list: the first rank of every action.
    [[nodiscard]] boost::json::array SpellList(std::vector<ActionCatalog::Action> const& actions);

    /// A manifest span of a row: [first, count].
    [[nodiscard]] boost::json::array Span(uint32 first, uint32 count);
}

#endif
