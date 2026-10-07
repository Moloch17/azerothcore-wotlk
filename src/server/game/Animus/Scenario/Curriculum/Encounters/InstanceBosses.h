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
    /// One row of a boss table: a real dungeon boss, fought in its own instance by the core's own script. The table is
    /// data, not code, and nothing here is a position: where the boss stands comes from its spawn in the world
    /// database. A row whose creature template or spawn the world database lacks is dropped at startup with a log
    /// line, never a crash.
    struct BossRow
    {
        uint32 MapId = 0;
        uint32 Entry = 0;                   // the boss creature
        uint8 Level = 60;                   // the seats' level for this rung
        uint8 Difficulty = 0;               // Difficulty: DUNGEON_DIFFICULTY_*, RAID_DIFFICULTY_*MAN_NORMAL
        char const* Name = "";
    };

    /// The ladder an arena's `Instance` names, in row order.
    [[nodiscard]] std::vector<BossRow> const& InstanceLadderRows(InstanceLadder ladder);

    /// The bosses of the dungeons the party follow stage walks (Ragefire Chasm, the Deadmines), in route order.
    [[nodiscard]] std::vector<BossRow> const& FollowBosses();

    /// **A dungeon's bosses, by name** (dungeon-curriculum D2, D3: "per boss"): every boss a whole dungeon's route
    /// passes -- side bosses included, the rares and an escort's boss left out -- with the short name its episode info
    /// column has (InstanceEncounter's boss_<name>: killed this run). Data from the world database's creature_template
    /// (looked up 2026-10-07; MySQL is sealed after startup).
    struct WingBoss
    {
        uint32 MapId = 0;
        uint32 Entry = 0;
        char const* Name = "";
    };

    [[nodiscard]] std::vector<WingBoss> const& WingBosses();
}

#endif
