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

#ifndef ANIMUS_LIB_CURRICULUM_WORLD_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_WORLD_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// The world outside a fight: the nearest corpse to loot, quest giver to talk to, node to gather, vendor to
    /// trade with, and the seat's own bags, gold, gear and supplies -- and six things a player does about them.
    ///
    /// **One action, the world decides what it means.** INTERACT is the right-click: on a giver it takes or hands
    /// in a quest, on a node it gathers, on a corpse it loots (or skins a looted one). What the policy learns is to
    /// stand at the right thing at the right time, which is the whole skill; which handler runs is a lookup. The
    /// other five are the buttons a client has: loot all, put on the better item, sell the greys, repair, buy
    /// food and drink. What "better" means is GearScore, scripted on purpose (WorldActions).
    ///
    /// The observations are filled by the life encounters in the sim (SeatView::World) and by the live module's
    /// life service for a companion: the same features from the real world, no phasing.
    class WorldBlock final : public Block
    {
    public:
        /// Each thing is (present, distance / RANGE, bearing sin, bearing cos in the seat's own frame) and what it
        /// is; "in reach" is within WorldActions::INTERACT_YARDS.
        enum Obs : uint32
        {
            OBS_CORPSE                  = 0,
            OBS_CORPSE_DISTANCE         = 1,
            OBS_CORPSE_SIN              = 2,
            OBS_CORPSE_COS              = 3,
            OBS_CORPSE_QUEST_ITEM       = 4,    // the corpse holds an item a quest of the seat's wants
            OBS_CORPSE_SKINNABLE        = 5,    // looted, and the seat can skin it
            OBS_CORPSE_IN_REACH         = 6,
            OBS_GIVER                   = 7,
            OBS_GIVER_DISTANCE          = 8,
            OBS_GIVER_SIN               = 9,
            OBS_GIVER_COS               = 10,
            OBS_GIVER_OFFERS            = 11,   // it offers a quest the seat can take
            OBS_GIVER_TURN_IN           = 12,   // it takes a quest the seat has completed
            OBS_GIVER_IN_REACH          = 13,
            OBS_NODE                    = 14,
            OBS_NODE_DISTANCE           = 15,
            OBS_NODE_SIN                = 16,
            OBS_NODE_COS                = 17,
            OBS_NODE_HERB               = 18,
            OBS_NODE_MINE               = 19,
            OBS_NODE_OPENABLE           = 20,   // the seat's skill opens it
            OBS_NODE_IN_REACH           = 21,
            OBS_VENDOR                  = 22,
            OBS_VENDOR_DISTANCE         = 23,
            OBS_VENDOR_SIN              = 24,
            OBS_VENDOR_COS              = 25,
            OBS_VENDOR_REPAIRS          = 26,
            OBS_VENDOR_IN_REACH         = 27,
            OBS_QUEST_NONE              = 28,   // the episode's quest: not taken, taken, complete
            OBS_QUEST_ACTIVE            = 29,
            OBS_QUEST_COMPLETE          = 30,
            OBS_QUEST_PROGRESS          = 31,   // objectives done, 0 to 1
            OBS_BAG_FREE                = 32,   // free bag slots / 16
            OBS_GOLD                    = 33,   // log10(copper + 1) / 7
            OBS_DURABILITY              = 34,   // the worn gear's, 0 to 1
            OBS_HAS_UPGRADE             = 35,   // something in the bags rates higher than what is worn
            OBS_HAS_JUNK                = 36,   // greys to sell
            OBS_FOOD                    = 37,   // food carried / CONSUMABLE_COUNT
            OBS_DRINK                   = 38,
            OBS_LOOT_OPEN               = 39,   // a loot window is open (a node just gathered, a corpse opened)
            OBS_CASTING                 = 40,   // a gathering or skinning cast is running
            OBS_COUNT                   = 41
        };

        enum Action : uint32
        {
            ACTION_INTERACT             = 0,    // the nearest thing in reach: giver, node, corpse
            ACTION_LOOT_ALL             = 1,
            ACTION_EQUIP_UPGRADE        = 2,
            ACTION_SELL_JUNK            = 3,
            ACTION_REPAIR               = 4,
            ACTION_BUY_SUPPLIES         = 5,
            ACTION_COUNT                = 6
        };

        /// How far a thing is reported out to: the distance feature saturates here.
        static constexpr float RANGE = 100.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::World; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
    };
}

#endif
