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

#ifndef ANIMUS_LIB_CURRICULUM_INSTANCE_BOSSES_H
#define ANIMUS_LIB_CURRICULUM_INSTANCE_BOSSES_H

#include "Define.h"
#include "StageDefinition.h"

#include <vector>

namespace Animus::Curriculum
{
    /// One rung of an instance ladder: a real dungeon or raid boss, fought in its own instance by the core's own
    /// script. The table is data, not code: the entries come from the core's script headers (molten_core.h,
    /// naxxramas.h, karazhan.h, ...), and nothing here is a position. Where the boss stands comes from its spawn in
    /// the world database, and where the raid stands comes from the server's own path from the instance entrance
    /// to it (InstanceEncounter::EngagePoint).
    ///
    /// Bosses that need an event, a door sequence, a key, mind control or a vehicle are left out on purpose:
    /// Majordomo, Ragnaros, Razorgore, Vaelastrasz, Nefarian, the Twin Emperors, C'Thun, Gothik, Thaddius, the Four
    /// Horsemen, Sapphiron, Kel'Thuzad, Razuvious, the Opera, Chess, Mr. Smite, Chromaggus's door. A row whose
    /// creature template or spawn the world database lacks is dropped at startup with a log line, never a crash.
    struct BossRow
    {
        uint32 MapId = 0;
        uint32 Entry = 0;                   // the boss creature
        int32 DataId = -1;                  // the instance script's boss index (SetBossState); -1 = none
        uint8 Level = 60;                   // the seats' level for this rung
        uint8 Difficulty = 0;               // Difficulty: DUNGEON_DIFFICULTY_*, RAID_DIFFICULTY_*MAN_NORMAL
        /// Creatures around the boss that ARE the encounter and are never cleared as trash: Lucifron's protectors,
        /// Garr's Firesworn, Moroes's guests.
        std::vector<uint32> Keep{};
        /// Leave the trash around the boss standing: the pull to the boss is part of the fight.
        bool Trash = false;
        char const* Name = "";
    };

    /// The ladder an arena's `Instance` names, in rung order.
    [[nodiscard]] std::vector<BossRow> const& InstanceLadderRows(InstanceLadder ladder);
}

#endif
