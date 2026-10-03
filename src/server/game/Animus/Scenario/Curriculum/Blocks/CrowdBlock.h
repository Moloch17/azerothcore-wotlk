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

#ifndef ANIMUS_LIB_CURRICULUM_CROWD_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_CROWD_BLOCK_H

#include "Block.h"

class GameObject;
class Player;

namespace Animus::Curriculum
{
    /// What is on the party past the pack block's PACK_SLOTS enemies, and the pack ahead: a whole dungeon's fights
    /// had a median of eight creatures on the party (2026-10-01), half of them out of sight of a seat that saw four.
    /// Counts over every creature on the party, the next CROWD_SLOTS of them one by one, and the nearest pack not yet
    /// in the fight. A separate block, so the blocks built on PACK_SLOTS keep their sizes and their seeded weights. No
    /// actions but one, using what opens the way on (a lever, the cannon): what it shows is fought through the pack
    /// block's slots, and stopped short of by the feet.
    class CrowdBlock final : public Block
    {
    public:
        enum Feature : uint32
        {
            OBS_PRESENT         = 0,    // the encounter reports a crowd (a whole dungeon)
            OBS_ON_PARTY        = 1,    // creatures whose victim is a player of the party, / 8
            OBS_ON_TANK         = 2,    // ... on the party's tank, / 8
            OBS_LOOSE           = 3,    // ... on anybody else, / 8
            OBS_ELITES          = 4,    // elites on the party, / 4
            OBS_UNSEEN          = 5,    // on the party past the pack block's slots, / 4
            OBS_AHEAD_DISTANCE  = 6,    // the nearest pack not in the fight, / 60 yd (1: none in sight)
            OBS_AHEAD_SIZE      = 7,    // how many stand with it, / 6
            OBS_AHEAD_SIN       = 8,    // its bearing in the seat's own frame
            OBS_AHEAD_COS       = 9,
            OBS_IS_TANK         = 10,   // this seat is the one the party's crowd is counted against
            OBS_OBJECT_PRESENT  = 11,   // the nearest thing the party can use (a lever, a button, the cannon, a door)
            OBS_OBJECT_DISTANCE = 12,   // / 40 yd
            OBS_OBJECT_SIN      = 13,
            OBS_OBJECT_COS      = 14,
            OBS_OBJECT_DOOR     = 15,   // it is a door (else a lever, a button or the like)
            OBS_TANK_PRESENT    = 16,   // the party's tank (the one the party block follows), alive, and not this seat
            OBS_TANK_DISTANCE   = 17,   // / 100 yd
            OBS_TANK_SIN        = 18,
            OBS_TANK_COS        = 19,
            OBS_TANK_TARGET_FIRST = 20, // one-hot over the pack block's slots: the enemy the tank is on
            OBS_BEHIND          = OBS_TANK_TARGET_FIRST + PACK_SLOTS,   // its place on the route is behind the party
            OBS_SLOT_FIRST      = OBS_BEHIND + 1
        };

        enum SlotFeature : uint32
        {
            SLOT_PRESENT        = 0,
            SLOT_HEALTH         = 1,
            SLOT_DISTANCE       = 2,    // / 40 yd
            SLOT_SIN            = 3,
            SLOT_COS            = 4,
            SLOT_IN_COMBAT      = 5,
            SLOT_ON_ME          = 6,
            SLOT_ON_TANK        = 7,
            SLOT_ELITE          = 8,
            SLOT_FEATURES       = 9
        };

        /// After the slots, so a block seeded from one without them keeps every column it had: what a pull of the
        /// pack ahead may bring (2026-10-03: the tank saw only the nearest pack, never the one behind it).
        enum TailFeature : uint32
        {
            OBS_TAIL_FIRST      = OBS_SLOT_FIRST + CROWD_SLOTS * SLOT_FEATURES,
            /// The nearest creature out of the fight past the pack ahead's reach: the second pack.
            OBS_SECOND_PRESENT  = OBS_TAIL_FIRST,
            OBS_SECOND_GAP      = OBS_TAIL_FIRST + 1,   // from the pack ahead, / 40 yd
            OBS_SECOND_DISTANCE = OBS_TAIL_FIRST + 2,   // from the seat, / 60 yd
            OBS_SECOND_SIN      = OBS_TAIL_FIRST + 3,
            OBS_SECOND_COS      = OBS_TAIL_FIRST + 4,
            /// Yards the seat stands outside the nearest aggro radius (Creature::GetAggroRange) of a creature of the
            /// pack ahead, and of any other out of the fight it can see, / 20 in -1..1: below 0 it would be pulled.
            OBS_AHEAD_MARGIN    = OBS_TAIL_FIRST + 5,
            OBS_SECOND_MARGIN   = OBS_TAIL_FIRST + 6,
            /// How long the party has gone without a kill, a step along the route or a fight, / 120 s, at most 1.
            OBS_STILL           = OBS_TAIL_FIRST + 7,
            OBS_COUNT           = OBS_TAIL_FIRST + 8
        };

        enum Action : uint32
        {
            /// Use the nearest usable thing within reach: pull a lever, press a button, fire the cannon, open a door.
            ACTION_USE_OBJECT   = 0,
            /// Walk the server's path to the seat's objective: the next point of the route, or for a seat other than
            /// the tank the route back towards it. Smooth, pathed movement the script uses and a seat can learn,
            /// where held bearings zig-zagged and walked into walls (2026-10-01).
            ACTION_ADVANCE      = 1,
            /// Walk the server's path to the nearest usable thing (a lever, the cannon).
            ACTION_APPROACH_OBJECT = 2,
            ACTION_COUNT        = 3
        };

        /// The item a lock is opened with (LOCK_KEY_ITEM), or 0: the Deadmines' cannon takes the Defias Gunpowder.
        [[nodiscard]] static uint32 KeyOf(GameObject const* object);
        /// Whether `bot` can use `object` as it stands: a key it needs is carried.
        [[nodiscard]] static bool CanUse(Player const* bot, GameObject const* object);

        [[nodiscard]] BlockId Id() const override { return BlockId::Crowd; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] bool IsMovement(uint32 local) const override
        {
            return local == ACTION_ADVANCE || local == ACTION_APPROACH_OBJECT;
        }
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
    };
}

#endif
