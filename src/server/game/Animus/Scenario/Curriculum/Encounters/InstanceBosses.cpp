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

    // Map ids: Ragefire Chasm 389, the Deadmines 36, Scarlet Monastery 189, Stratholme 329, Utgarde Keep 574.
    constexpr uint8 DUNGEON_NORMAL = 0;
    constexpr uint8 DUNGEON_HEROIC = 1;

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
        { .MapId = 189, .Entry = 3983, .Level = 35, .Difficulty = DUNGEON_NORMAL, .Entrance = 45,
            .Name = "Interrogator Vishas" },
        { .MapId = 189, .Entry = 4543, .Level = 35, .Difficulty = DUNGEON_NORMAL, .Entrance = 45,
            .Name = "Bloodmage Thalnos" },
        { .MapId = 189, .Entry = 6487, .Level = 38, .Difficulty = DUNGEON_NORMAL, .Entrance = 614,
            .Name = "Arcanist Doan" },
        { .MapId = 189, .Entry = 3975, .Level = 40, .Difficulty = DUNGEON_NORMAL, .Entrance = 612, .Name = "Herod" },
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

    std::vector<BossRow> const NONE;
}

namespace
{
    /// Whole dungeons, entrance to last boss (InstanceLadder::Wing): each row names the last boss, the trash before
    /// it stays, and the party starts at the door. The seats' level is the dungeon's.
    ///
    /// One dungeon to begin with (2026-09-30): the Deadmines, the whole of it to VanCleef, six scripted bosses, and
    /// a navmesh path from the door to the last. Ragefire Chasm had no path from its door to its bosses as a boss
    /// rung (the party was stood in front of them instead): its wing's route is checked by the trace lines. The Scarlet Monastery wings (Graveyard to Thalnos, entrance 45; Library
    /// to Doan, 614; Armory to Herod, 612) and Utgarde Keep to Ingvar (DataId 2) come back once this one is learned.
    ///
    /// Ragefire Chasm came back first (2026-10-02, the Deadmines curriculum), as its own stage before the Deadmines':
    /// row 0, the door to Bazzalan, past Oggleflint, Taragaman and Jergosh. Each stage pins its row
    /// (ArenaDefinition::InstanceRow).
    std::vector<Animus::Curriculum::BossRow> const WING = {
        { .MapId = 389, .Entry = 11519, .Level = 16, .Difficulty = 0, .Trash = true,
            .Name = "Ragefire Chasm to Bazzalan" },
        { .MapId = 36, .Entry = 639, .Level = 20, .Difficulty = 0, .Trash = true,
            .Name = "the Deadmines to Edwin VanCleef" },
        // Held out (peak-play W2, ArenaDefinition::EvalOnly): Wailing Caverns (map 43, levels 15-25) to Lord Serpentis,
        // a fixed level-20 spawn on its main way, past Anacondra, Pythas and Cobrahn. Its last boss proper, Mutanus,
        // only comes with the Naralex escort, which no route can walk to.
        { .MapId = 43, .Entry = 3673, .Level = 20, .Difficulty = 0, .Trash = true,
            .Name = "Wailing Caverns to Lord Serpentis" },
    };
}

std::vector<Animus::Curriculum::WingBoss> const& Animus::Curriculum::WingBosses()
{
    static std::vector<WingBoss> const bosses = {
        // Ragefire Chasm (389): its four, in route order.
        { 389, 11517, "oggleflint" },
        { 389, 11520, "taragaman" },
        { 389, 11518, "jergosh" },
        { 389, 11519, "bazzalan" },
        // The Deadmines (36): Sneed in his Shredder, then out of it; Cookie the ship's side boss. Miner Johnson is a
        // rare spawn, and is no measure of a run.
        { 36, 644, "rhahkzor" },
        { 36, 642, "sneed_shredder" },
        { 36, 643, "sneed" },
        { 36, 1763, "gilnid" },
        { 36, 646, "smite" },
        { 36, 647, "greenskin" },
        { 36, 645, "cookie" },
        { 36, 639, "vancleef" },
        // Wailing Caverns (43, held out): the route's to Lord Serpentis. Mutanus comes only with the Naralex escort,
        // and the Deviate Faerie Dragon is a rare.
        { 43, 3671, "anacondra" },
        { 43, 3669, "cobrahn" },
        { 43, 3653, "kresh" },
        { 43, 3670, "pythas" },
        { 43, 3674, "skum" },
        { 43, 5775, "verdan" },
        { 43, 3673, "serpentis" },
    };
    return bosses;
}

std::vector<Animus::Curriculum::BossRow> const& Animus::Curriculum::InstanceLadderRows(InstanceLadder ladder)
{
    switch (ladder)
    {
        case InstanceLadder::Dungeon: return DUNGEON;
        case InstanceLadder::Wing:    return WING;
        case InstanceLadder::None:    break;
    }

    return NONE;
}
