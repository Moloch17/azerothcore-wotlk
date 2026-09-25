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

#include "CompanionBlock.h"
#include "ContextBlock.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "MoveBlock.h"
#include "FlagBlock.h"
#include "OrderBlock.h"
#include "GauntletBlock.h"
#include "HostilesBlock.h"
#include "PackBlock.h"
#include "PartyBlock.h"
#include "PetBlock.h"
#include "PvpBlock.h"
#include "SupportBlock.h"
#include "TravelBlock.h"
#include "WorldBlock.h"

Animus::Curriculum::Block const& Animus::Curriculum::GetBlock(BlockId id)
{
    static CoreBlock const core;
    static MoveBlock const move;
    static DuelBlock const duel;
    static PackBlock const pack;
    static GauntletBlock const gauntlet;
    static CompanionBlock const companion;
    static PartyBlock const party;
    static PvpBlock const pvp;
    static ContextBlock const context;
    static HostilesBlock const hostiles;
    static PetBlock const pet;
    static TravelBlock const travel;
    static FlagBlock const flag;
    static SupportBlock const support;
    static OrderBlock const order;
    static WorldBlock const world;

    // In BlockId order.
    static std::array<Block const*, BLOCK_COUNT> const blocks =
    {
        &core, &move, &duel, &pack, &gauntlet, &companion, &party, &pvp, &context, &hostiles, &pet, &travel,
        &flag, &support, &order, &world
    };

    return *blocks[std::size_t(id)];
}
