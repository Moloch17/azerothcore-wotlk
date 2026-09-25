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

#include "InstanceBosses.h"

namespace
{
    using Animus::Curriculum::BossRow;

    // Map ids: Ragefire Chasm 389, the Deadmines 36, Scarlet Monastery 189, Stratholme 329, Utgarde Keep 574,
    // Karazhan 532, Naxxramas 533, Molten Core 409, Blackwing Lair 469, the Temple of Ahn'Qiraj 531.
    constexpr uint8 DUNGEON_NORMAL = 0;
    constexpr uint8 DUNGEON_HEROIC = 1;
    constexpr uint8 RAID_10 = 0;
    constexpr uint8 RAID_25 = 1;

    /// Five-man dungeons at five level bands: four bosses each so a class climbs a band at a time.
    std::vector<BossRow> const DUNGEON = {
        { .MapId = 389, .Entry = 11517, .Level = 15, .Difficulty = DUNGEON_NORMAL, .Name = "Oggleflint" },
        { .MapId = 389, .Entry = 11520, .Level = 15, .Difficulty = DUNGEON_NORMAL, .Name = "Taragaman the Hungerer" },
        { .MapId = 389, .Entry = 11518, .Level = 15, .Difficulty = DUNGEON_NORMAL, .Name = "Jergosh the Invoker" },
        { .MapId = 389, .Entry = 11519, .Level = 15, .Difficulty = DUNGEON_NORMAL, .Name = "Bazzalan" },
        { .MapId = 36, .Entry = 644, .DataId = 0, .Level = 20, .Difficulty = DUNGEON_NORMAL, .Name = "Rhahk'Zor" },
        { .MapId = 36, .Entry = 643, .Level = 20, .Difficulty = DUNGEON_NORMAL, .Name = "Sneed" },
        { .MapId = 36, .Entry = 1763, .Level = 20, .Difficulty = DUNGEON_NORMAL, .Name = "Gilnid" },
        { .MapId = 36, .Entry = 639, .Level = 20, .Difficulty = DUNGEON_NORMAL, .Name = "Edwin VanCleef" },
        { .MapId = 189, .Entry = 3983, .Level = 35, .Difficulty = DUNGEON_NORMAL, .Name = "Interrogator Vishas" },
        { .MapId = 189, .Entry = 4543, .Level = 35, .Difficulty = DUNGEON_NORMAL, .Name = "Bloodmage Thalnos" },
        { .MapId = 189, .Entry = 6487, .Level = 38, .Difficulty = DUNGEON_NORMAL, .Name = "Arcanist Doan" },
        { .MapId = 189, .Entry = 3975, .Level = 40, .Difficulty = DUNGEON_NORMAL, .Name = "Herod" },
        { .MapId = 329, .Entry = 10436, .Level = 60, .Difficulty = DUNGEON_NORMAL, .Name = "Baroness Anastari" },
        { .MapId = 329, .Entry = 10437, .Level = 60, .Difficulty = DUNGEON_NORMAL, .Name = "Nerub'enkan" },
        { .MapId = 329, .Entry = 10438, .Level = 60, .Difficulty = DUNGEON_NORMAL, .Name = "Maleki the Pallid" },
        { .MapId = 329, .Entry = 10439, .Level = 60, .Difficulty = DUNGEON_NORMAL, .Name = "Ramstein the Gorger" },
        { .MapId = 574, .Entry = 23953, .DataId = 0, .Level = 80, .Difficulty = DUNGEON_HEROIC,
            .Name = "Prince Keleseth" },
        { .MapId = 574, .Entry = 24200, .DataId = 1, .Level = 80, .Difficulty = DUNGEON_HEROIC, .Keep = { 24201 },
            .Name = "Skarvald and Dalronn" },
        { .MapId = 574, .Entry = 23954, .DataId = 2, .Level = 80, .Difficulty = DUNGEON_HEROIC,
            .Name = "Ingvar the Plunderer" },
    };

    /// Ten-man raids: Karazhan at 70, then Naxxramas at 80. Karazhan's Attumen is met as Midnight, which is the
    /// creature the room has before he rides in.
    std::vector<BossRow> const RAID10 = {
        { .MapId = 532, .Entry = 16151, .DataId = 0, .Level = 70, .Difficulty = RAID_10, .Keep = { 15550, 16152 },
            .Name = "Attumen the Huntsman" },
        { .MapId = 532, .Entry = 15687, .DataId = 1, .Level = 70, .Difficulty = RAID_10,
            .Keep = { 17007, 19872, 19873, 19874, 19875, 19876 }, .Name = "Moroes" },
        { .MapId = 532, .Entry = 16457, .DataId = 2, .Level = 70, .Difficulty = RAID_10, .Name = "Maiden of Virtue" },
        { .MapId = 532, .Entry = 15691, .DataId = 5, .Level = 70, .Difficulty = RAID_10, .Name = "The Curator" },
        { .MapId = 532, .Entry = 16524, .DataId = 6, .Level = 70, .Difficulty = RAID_10, .Name = "Shade of Aran" },
        { .MapId = 532, .Entry = 15690, .DataId = 10, .Level = 70, .Difficulty = RAID_10, .Name = "Prince Malchezaar" },
        { .MapId = 533, .Entry = 15956, .DataId = 6, .Level = 80, .Difficulty = RAID_10, .Keep = { 16573 },
            .Name = "Anub'Rekhan" },
        { .MapId = 533, .Entry = 15953, .DataId = 7, .Level = 80, .Difficulty = RAID_10, .Keep = { 16506, 16505 },
            .Name = "Grand Widow Faerlina" },
        { .MapId = 533, .Entry = 15952, .DataId = 8, .Level = 80, .Difficulty = RAID_10, .Name = "Maexxna" },
        { .MapId = 533, .Entry = 15954, .DataId = 3, .Level = 80, .Difficulty = RAID_10, .Name = "Noth the Plaguebringer" },
        { .MapId = 533, .Entry = 15936, .DataId = 4, .Level = 80, .Difficulty = RAID_10, .Name = "Heigan the Unclean" },
        { .MapId = 533, .Entry = 16011, .DataId = 5, .Level = 80, .Difficulty = RAID_10, .Name = "Loatheb" },
        { .MapId = 533, .Entry = 16028, .DataId = 0, .Level = 80, .Difficulty = RAID_10, .Name = "Patchwerk" },
        { .MapId = 533, .Entry = 15931, .DataId = 1, .Level = 80, .Difficulty = RAID_10, .Name = "Grobbulus" },
        { .MapId = 533, .Entry = 15932, .DataId = 2, .Level = 80, .Difficulty = RAID_10, .Name = "Gluth" },
    };

    std::vector<BossRow> const RAID25 = {
        { .MapId = 533, .Entry = 15956, .DataId = 6, .Level = 80, .Difficulty = RAID_25, .Keep = { 16573 },
            .Name = "Anub'Rekhan" },
        { .MapId = 533, .Entry = 15953, .DataId = 7, .Level = 80, .Difficulty = RAID_25, .Keep = { 16506, 16505 },
            .Name = "Grand Widow Faerlina" },
        { .MapId = 533, .Entry = 15952, .DataId = 8, .Level = 80, .Difficulty = RAID_25, .Name = "Maexxna" },
        { .MapId = 533, .Entry = 15954, .DataId = 3, .Level = 80, .Difficulty = RAID_25, .Name = "Noth the Plaguebringer" },
        { .MapId = 533, .Entry = 15936, .DataId = 4, .Level = 80, .Difficulty = RAID_25, .Name = "Heigan the Unclean" },
        { .MapId = 533, .Entry = 16011, .DataId = 5, .Level = 80, .Difficulty = RAID_25, .Name = "Loatheb" },
        { .MapId = 533, .Entry = 16028, .DataId = 0, .Level = 80, .Difficulty = RAID_25, .Name = "Patchwerk" },
        { .MapId = 533, .Entry = 15931, .DataId = 1, .Level = 80, .Difficulty = RAID_25, .Name = "Grobbulus" },
        { .MapId = 533, .Entry = 15932, .DataId = 2, .Level = 80, .Difficulty = RAID_25, .Name = "Gluth" },
    };

    /// The classic forty-man raids at 60. Naxxramas has no forty-man in 3.3.5.
    std::vector<BossRow> const RAID40 = {
        { .MapId = 409, .Entry = 12118, .DataId = 0, .Level = 60, .Difficulty = RAID_10, .Keep = { 12119 },
            .Name = "Lucifron" },
        { .MapId = 409, .Entry = 11982, .DataId = 1, .Level = 60, .Difficulty = RAID_10, .Name = "Magmadar" },
        { .MapId = 409, .Entry = 12259, .DataId = 2, .Level = 60, .Difficulty = RAID_10, .Keep = { 11661 },
            .Name = "Gehennas" },
        { .MapId = 409, .Entry = 12057, .DataId = 3, .Level = 60, .Difficulty = RAID_10, .Keep = { 12099 },
            .Name = "Garr" },
        { .MapId = 409, .Entry = 12264, .DataId = 4, .Level = 60, .Difficulty = RAID_10, .Name = "Shazzrah" },
        { .MapId = 409, .Entry = 12056, .DataId = 5, .Level = 60, .Difficulty = RAID_10, .Name = "Baron Geddon" },
        { .MapId = 409, .Entry = 12098, .DataId = 6, .Level = 60, .Difficulty = RAID_10, .Keep = { 11662 },
            .Name = "Sulfuron Harbinger" },
        { .MapId = 409, .Entry = 11988, .DataId = 7, .Level = 60, .Difficulty = RAID_10, .Keep = { 11672 },
            .Name = "Golemagg the Incinerator" },
        { .MapId = 469, .Entry = 12017, .DataId = 2, .Level = 60, .Difficulty = RAID_10, .Name = "Broodlord Lashlayer" },
        { .MapId = 469, .Entry = 11983, .DataId = 3, .Level = 60, .Difficulty = RAID_10, .Name = "Firemaw" },
        { .MapId = 469, .Entry = 14601, .DataId = 4, .Level = 60, .Difficulty = RAID_10, .Name = "Ebonroc" },
        { .MapId = 469, .Entry = 11981, .DataId = 5, .Level = 60, .Difficulty = RAID_10, .Name = "Flamegor" },
        { .MapId = 531, .Entry = 15263, .DataId = 1, .Level = 60, .Difficulty = RAID_10, .Name = "The Prophet Skeram" },
        { .MapId = 531, .Entry = 15516, .DataId = 3, .Level = 60, .Difficulty = RAID_10, .Keep = { 15984 },
            .Name = "Battleguard Sartura" },
        { .MapId = 531, .Entry = 15510, .DataId = 4, .Level = 60, .Difficulty = RAID_10, .Name = "Fankriss the Unyielding" },
        { .MapId = 531, .Entry = 15509, .DataId = 6, .Level = 60, .Difficulty = RAID_10, .Name = "Princess Huhuran" },
    };

    std::vector<BossRow> const NONE;
}

std::vector<Animus::Curriculum::BossRow> const& Animus::Curriculum::InstanceLadderRows(InstanceLadder ladder)
{
    switch (ladder)
    {
        case InstanceLadder::Dungeon: return DUNGEON;
        case InstanceLadder::Raid10:  return RAID10;
        case InstanceLadder::Raid25:  return RAID25;
        case InstanceLadder::Raid40:  return RAID40;
        case InstanceLadder::None:    break;
    }

    return NONE;
}
