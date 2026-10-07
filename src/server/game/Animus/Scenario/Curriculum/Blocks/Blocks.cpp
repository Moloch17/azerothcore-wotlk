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

/*
 * Every block implementation, by BlockId. Adding a block is a new BlockId, its class, and one entry here.
 */

#include "CombatBlock.h"
#include "CompassBlock.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "EntitiesBlock.h"
#include "MoveBlock.h"
#include "GoalBlock.h"
#include "GauntletBlock.h"
#include "Layout.h"
#include "MapBlock.h"
#include "PackBlock.h"
#include "PartyFramesBlock.h"
#include "PetBlock.h"
#include "SightBlock.h"
#include "VisionBlock.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

void Animus::Curriculum::DescribeSeatSets(Layout const& layout, boost::json::array& sets)
{
    auto const segment = [](uint32 first, uint32 stride)
    {
        boost::json::object part;
        part["first"] = first;
        part["stride"] = stride;
        return part;
    };
    auto const pointer = [](uint32 first, uint32 count)
    {
        boost::json::object part;
        part["first"] = first;
        part["count"] = count;
        return part;
    };
    auto const add = [&sets](char const* name, uint32 slots, uint32 present, boost::json::array segments,
        boost::json::array pointers)
    {
        boost::json::object set;
        set["name"] = name;
        set["slots"] = slots;
        set["present"] = present;
        set["segments"] = std::move(segments);
        set["pointers"] = std::move(pointers);
        sets.emplace_back(std::move(set));
    };

    if (layout.Has(BlockId::Pack))
    {
        BlockSlice const& pack = layout.Slice(BlockId::Pack);
        boost::json::array segments{ segment(pack.ObsFirst + PackBlock::OBS_GLOBAL_COUNT, PackBlock::SLOT_FEATURES) };
        add("enemies", PACK_SLOTS, PackBlock::SLOT_PRESENT, std::move(segments),
            boost::json::array{ pointer(pack.ActionFirst + PackBlock::ACTION_SLOT_FIRST, PACK_SLOTS) });
    }
}

Animus::Curriculum::Block const& Animus::Curriculum::GetBlock(BlockId id)
{
    static CoreBlock const core;
    static MoveBlock const move;
    static CompassBlock const compass;
    static DuelBlock const duel;
    static PackBlock const pack;
    static GauntletBlock const gauntlet;
    static PetBlock const pet;
    static VisionBlock const vision;
    static EntitiesBlock const entities;
    static MapBlock const map;
    static SightBlock const sight;
    static PartyFramesBlock const partyFrames;
    static CombatBlock const combat;
    static GoalBlock const goal;

    // Keyed by id: the ids have gaps (the deleted blocks' numbers are not reused).
    static std::array<Block const*, BLOCK_COUNT> const blocks = [&]
    {
        std::array<Block const*, BLOCK_COUNT> table{};
        table[std::size_t(BlockId::Core)] = &core;
        table[std::size_t(BlockId::Move)] = &move;
        table[std::size_t(BlockId::Compass)] = &compass;
        table[std::size_t(BlockId::Duel)] = &duel;
        table[std::size_t(BlockId::Pack)] = &pack;
        table[std::size_t(BlockId::Gauntlet)] = &gauntlet;
        table[std::size_t(BlockId::Pet)] = &pet;
        table[std::size_t(BlockId::Vision)] = &vision;
        table[std::size_t(BlockId::Entities)] = &entities;
        table[std::size_t(BlockId::Map)] = &map;
        table[std::size_t(BlockId::Sight)] = &sight;
        table[std::size_t(BlockId::PartyFrames)] = &partyFrames;
        table[std::size_t(BlockId::Combat)] = &combat;
        table[std::size_t(BlockId::Goal)] = &goal;
        return table;
    }();

    ASSERT(std::size_t(id) < BLOCK_COUNT && blocks[std::size_t(id)], "No block has id {}", uint32(id));
    return *blocks[std::size_t(id)];
}
