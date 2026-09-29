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

#ifndef ANIMUS_LIB_FORECAST_BLOCK_H
#define ANIMUS_LIB_FORECAST_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **What is about to happen** (Component P, layer 1): facts about the next few seconds the sim can compute, as
    /// the journal's are facts about the past. The cues a player reads before acting -- the big cast about to land
    /// on it or on a friend, the interrupt window closing, how close it is to pulling aggro off the tank, where the
    /// owner will be, how long its mana and its target will last -- so that a heal started before the hit, a
    /// defensive before the cast, an interrupt timed to the cast and a step back before the pull are learnable.
    ///
    /// No actions. A block of its own rather than more core globals, so that seeding from a checkpoint without it
    /// keeps every other block's weights (animus.bootstrap) and only this one starts from nothing.
    class ForecastBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            /// Enemy casts in progress that will land on the seat: their estimated damage as a share of its health
            /// / 2, and how soon the first lands / 3 s (1 when none).
            OBS_INCOMING_SELF           = 0,
            OBS_INCOMING_SELF_SOON      = 1,
            /// The same for the friend worst off (the owner and each teammate): the share of that friend's health.
            OBS_INCOMING_ALLY           = 2,
            OBS_INCOMING_ALLY_SOON      = 3,
            /// The interruptible enemy cast finishing soonest: time left / 3 s (1 when none), and that there is one.
            OBS_INTERRUPT_WINDOW        = 4,
            OBS_INTERRUPT_PRESENT       = 5,
            /// Threat: the seat's share of the top threat on the enemy nearest to turning on it (1.1 in melee, 1.3
            /// at range takes aggro), / 1.5; and, on the enemies attacking the seat, the most threat a friend holds
            /// as a share of the seat's -- how close the seat is to losing what it holds -- / 1.5.
            OBS_THREAT_PULL             = 6,
            OBS_THREAT_HOLD             = 7,
            /// Where the owner will be in 1.5 s at its current movement: distance / 40 and bearing from the seat.
            OBS_OWNER_AHEAD_DISTANCE    = 8,
            OBS_OWNER_AHEAD_SIN         = 9,
            OBS_OWNER_AHEAD_COS         = 10,
            /// The same for the teammate who will then be furthest from the seat.
            OBS_TEAMMATE_AHEAD_DISTANCE = 11,
            OBS_TEAMMATE_AHEAD_SIN      = 12,
            OBS_TEAMMATE_AHEAD_COS      = 13,
            /// Seconds of mana left at the rate it has been spent, and until the target dies at the rate it has
            /// been falling (SeatMemory), each / 60 s. The next pull's clock is the gauntlet block's
            /// (GauntletBlock::OBS_NEXT_PULL), so it is not repeated here.
            OBS_MANA_SECONDS            = 14,
            OBS_TARGET_SECONDS          = 15,
            OBS_COUNT
        };

        /// How far ahead the owner's position is extrapolated.
        static constexpr float OWNER_AHEAD_SECONDS = 1.5f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Forecast; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };
}

#endif
