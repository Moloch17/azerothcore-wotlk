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

#ifndef ANIMUS_LIB_CURRICULUM_GAUNTLET_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_GAUNTLET_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// Lasting through many fights: pull timing, eating and drinking, and what food and drink is left. Actions: eat,
    /// drink. (The class's sustain spells -- heals, absorbs, friendly dispels -- are core actions.) A layout with this
    /// block also acts between pulls, without a target.
    class GauntletBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_PULLS_CLEARED           = 0,    // / 10
            OBS_PULL_ACTIVE             = 1,
            OBS_QUIET_TIME              = 2,    // time since the last fight ended / 20 s; 0 during a fight
            OBS_PULL_TIME               = 3,    // time into the current pull / 60 s
            OBS_ELITE_PULL              = 4,
            OBS_EATING                  = 5,
            OBS_DRINKING                = 6,
            OBS_FOOD_LEFT               = 7,    // / the food stocked
            OBS_DRINK_LEFT              = 8,
            OBS_PULL_ARRIVAL            = 9,    // alone: an unengaged pull comes to the bot in this / 30 s; else 0
            OBS_NEXT_PULL               = 10,   // between pulls: the next one spawns in this / 20 s
            OBS_GLOBAL_COUNT            = 11
        };

        enum Action : uint32
        {
            ACTION_EAT                  = 0,
            ACTION_DRINK                = 1,
            /// Eat and drink, decision after decision, until health and mana are back, something interrupts it or
            /// Options.RestMaxMs runs out: one press for a whole break between pulls (SeatOption).
            ACTION_REST_UNTIL_READY     = 2,
            ACTION_COUNT                = 3     // the sustain spells are core actions, cast on the bot itself
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Gauntlet; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

    private:
        /// One decision of a rest: eat or drink, whichever the seat is short of.
        void Rest(SeatView& view, SeatActionResult& result) const;
    };
}

#endif
