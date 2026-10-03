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

#ifndef ANIMUS_LIB_DEATH_BLOCK_H
#define ANIMUS_LIB_DEATH_BLOCK_H

#include "Block.h"

class Player;
class WorldObject;

namespace Animus::Curriculum
{
    /// **After dying** (next-run plan, Wave 6): what a player does once it is dead in the open world. Release the
    /// spirit (to the nearest graveyard on the same map, else where it fell), run the ghost back to the corpse,
    /// and rise there -- where it chooses, once the reclaim delay is over -- or take the spirit healer's
    /// resurrection (with its sickness) instead; or wait, unreleased, for a friend's resurrection and accept it.
    /// Releasing is offered only where the arena lets death run on (ArenaDefinition::DeathRuns); elsewhere the
    /// encounters stand the dead up as before. A dead seat observes this block (and the goal block) alone.
    class DeathBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_DEAD                = 0,    // dead in its body (not released)
            OBS_GHOST               = 1,    // released: a ghost
            OBS_CORPSE_DISTANCE     = 2,    // / 100 yd, clamped
            OBS_CORPSE_SIN          = 3,    // the corpse's bearing from the facing
            OBS_CORPSE_COS          = 4,
            OBS_RESURRECT_OFFERED   = 5,    // a friend's resurrection is waiting to be accepted
            OBS_DEAD_TIME           = 6,    // log(1 + seconds dead) / log(1 + 600)
            OBS_SICKNESS            = 7,    // resurrection sickness on the seat
            OBS_HOSTILES_AT_CORPSE  = 8,    // hostile creatures within 20 yd of the corpse / 4, clamped
            OBS_RECLAIM_READY       = 9,    // a ghost in reach of its corpse with the reclaim delay over
            OBS_DEATH_RUNS          = 10,   // the arena lets death run on (releasing is possible)
            OBS_GRAVEYARD_DISTANCE  = 11,   // a ghost's yards to the nearest graveyard on the map / 100, clamped
            OBS_COUNT               = 12
        };

        enum Action : uint32
        {
            ACTION_RELEASE          = 0,    // release the spirit
            ACTION_ACCEPT           = 1,    // accept a friend's resurrection
            ACTION_RUN_TO_CORPSE    = 2,    // a ghost runs back to its corpse
            ACTION_RISE_AT_CORPSE   = 3,    // a ghost in reach of its corpse rises there
            ACTION_SPIRIT_HEALER    = 4,    // a ghost at the graveyard takes the spirit healer's resurrection
            ACTION_COUNT            = 5
        };

        /// Resurrection sickness.
        static constexpr uint32 SICKNESS_SPELL = 15007;
        /// Hostiles this near the corpse are observed; a rise is safe with none within its aggro radius and this.
        static constexpr float CORPSE_HOSTILE_RANGE = 20.0f;
        static constexpr float SAFE_RISE_MARGIN = 5.0f;
        static constexpr float SAFE_RISE_SEARCH = 50.0f;
        /// The spirit healer is taken within this of the nearest graveyard.
        static constexpr float SPIRIT_HEALER_RANGE = 30.0f;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] bool IsMovement(uint32 local) const override { return local == ACTION_RUN_TO_CORPSE; }
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// Living hostile creatures within `range` of `at` (aggroOnly: only those whose aggro radius for `bot`, plus
        /// SAFE_RISE_MARGIN, reaches it).
        [[nodiscard]] static uint32 HostilesNear(WorldObject const* at, Player* bot, float range, bool aggroOnly);

        /// Whether `action` can be taken now.
        [[nodiscard]] static bool IsAllowed(SeatView const& view, uint32 action);
    };
}

#endif
