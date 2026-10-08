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
 * The curriculum: every stage the forge can train, each extending one earlier stage and seeding from it, keeping the
 * base's blocks it needs and adding its own.
 *
 * **The movement curriculum** (.agents/plans/movement-curriculum/, approved 2026-10-05): seven stages, M1-M7, in the
 * order a player learns to move -- controls, ground, verticality, water, long routes, riding and flight, company --
 * every one of them movement alone, on the player controller (the move block's keys and mouse), on real terrain. The
 * first curriculum (stage1_move ... stage21_ship) is archived: its definitions on the git tag `curriculum-v1`, its
 * code and learner configs (deleted from the tree 2026-10-07) on `pre-cleanup-2026-10-07`, its runs in
 * var/animus-forge/shared/archive/curriculum-v1-2026-10-05/.
 *
 *   movement   move1_controls ─ move2_seek (perception-goals P1: the compass split, a hidden object found by sight)
 *              ─ move3_interact (dungeon-curriculum M3: the named object, levers and doors, a key item; the sight block)
 *              ─ move4_follow (dungeon-curriculum I5: a party keeps with a leader through an empty dungeon)
 *   combat     move3_interact ─ combat1_fight ─ combat2_packs ─ combat3_survive (dungeon-curriculum C1-C3, Ragefire Chasm)
 *   party      combat3_survive (+ move4_follow's party frames, merged by name) ─ group1_roles (dungeon-curriculum G1)
 *              ─ dungeon2_ragefire ─ dungeon3_deadmines (dungeon-curriculum D2, D3: real dungeon wings; Wailing
 *              Caverns held out; group2_corridor and dungeon1_pulls retired 2026-10-08, vision-only movement)
 *
 * Every movement stage runs 50 ms world ticks (AnimusForge.Stage.<name>.TicksPerDecision in the conf template): the
 * controller's mouse-look facing rule and its heartbeat are checked once a world tick, so a coarser tick would leave
 * the server's facing staler than a client's.
 *
 * A stage's episodes are its arenas (see ArenaDefinition): each episode draws one by weight, so a stage can mix
 * situations, or several kinds of ground, over the union of their blocks.
 *
 * Adding a stage is one entry here (plus new blocks or encounters only if it needs new features) and a learner
 * config, configs/<name>.yaml -- or configs/<class>/<name>.yaml for a run of a single class, which is how a
 * per-class curriculum sets its own floors.
 */

#include "StageDefinition.h"
#include "InstanceBosses.h"
#include "Log.h"
#include "AreaDefines.h"
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;

    // The highest role a party drill can fix (ArenaDefinition::DrillRole: 1 tank, 2 healer, 3 damage).
    constexpr uint8 DRILL_DAMAGE = 3;

    /// M1's ground: the Stockades (map 34), an instance of its own for every env, its creatures cleared on the env's
    /// first build (StageScenario's SpawnArea::Clear), so the dungeon is empty. The seat stands where the entrance's
    /// area trigger puts a player (areatrigger_teleport 101: 54.23, 0.28, -18.34, facing 6.26); behind it is the
    /// portal, which only a client's CMSG_AREATRIGGER uses and the player controller never sends, so backing into it
    /// leaves nothing. Ahead a ramp drops to the entrance hallway's floor (z -25.6 from x 76), which runs straight
    /// east, about sixteen yards wide, to its end wall a little past x 172 (forge controller probe, 2026-10-05).
    Position StockadeEntrance()
    {
        return { 54.23f, 0.28f, -18.34f, 6.26f };
    }

    /// **The Stockades' hallways** (M1, perception-goals REDESIGN §1): where the seat may spawn and where its object
    /// may stand -- the entrance (StockadeEntrance, first), then every point of a 3-yd grid over the hallways: the
    /// entrance hallway with its crossing and ramps, the two wing corridors down to their hubs' doors, and the
    /// hallway's end past the crossing (M2's hall_end), x 54 on (the ramp's top behind the entrance, up to the portal,
    /// is left out). The facing is drawn each episode, so the table's is 0.
    ///
    /// **How they were authored** (offline, 2026-10-06): the M2 room table's scan (StockadeRoomsDataTest's
    /// AuthoringScan: the navmesh read only to author, never a bot input), its walkable cells eroded by a yard and a
    /// half (.agents/plans/perception-goals/tools/rooms.py), the hallway region (region 0) and hall_end (r32) kept,
    /// and their eroded cores sampled every 3 yd (tools/hallway.py), each point at the vmap floor the scan read there
    /// and only where the navmesh's floor agrees within a yard: so every point is a yard and a half or more off any
    /// wall, on the floor a body stands on.
    ///
    /// **Validated** by StockadeHallwaysDataTest against the map's floors (the vmaps, with FORGE_VISION_DATA): every
    /// point has a vmap floor within a quarter yard of its height, and a knee-height ray of Seek.Clearance yards along
    /// each axis meets nothing, so an object stands there clear of the walls.
    std::vector<Position> StockadeHallways()
    {
        return {
            { 54.23f, 0.28f, -18.34f, 6.26f }, { 54.0f, 0.0f, -18.26f, 0.0f }, { 54.0f, 3.0f, -18.26f, 0.0f },
            { 57.0f, 0.0f, -19.40f, 0.0f }, { 57.0f, 3.0f, -19.40f, 0.0f }, { 60.0f, 0.0f, -20.53f, 0.0f },
            { 60.0f, 3.0f, -20.53f, 0.0f }, { 63.0f, 0.0f, -21.65f, 0.0f }, { 63.0f, 3.0f, -21.65f, 0.0f },
            { 66.0f, 0.0f, -22.75f, 0.0f }, { 66.0f, 3.0f, -22.75f, 0.0f }, { 69.0f, 0.0f, -23.92f, 0.0f },
            { 69.0f, 3.0f, -23.92f, 0.0f }, { 72.0f, 0.0f, -25.03f, 0.0f }, { 72.0f, 3.0f, -25.03f, 0.0f },
            { 75.0f, 0.0f, -25.61f, 0.0f }, { 75.0f, 3.0f, -25.61f, 0.0f }, { 78.0f, 0.0f, -25.61f, 0.0f },
            { 78.0f, 3.0f, -25.61f, 0.0f }, { 81.0f, 0.0f, -25.61f, 0.0f }, { 81.0f, 3.0f, -25.61f, 0.0f },
            { 84.0f, 0.0f, -25.61f, 0.0f }, { 84.0f, 3.0f, -25.61f, 0.0f }, { 87.0f, 0.0f, -25.61f, 0.0f },
            { 87.0f, 3.0f, -25.61f, 0.0f }, { 90.0f, 0.0f, -25.61f, 0.0f }, { 90.0f, 3.0f, -25.61f, 0.0f },
            { 93.0f, 0.0f, -25.61f, 0.0f }, { 93.0f, 3.0f, -25.61f, 0.0f }, { 96.0f, 0.0f, -25.61f, 0.0f },
            { 96.0f, 3.0f, -25.61f, 0.0f }, { 99.0f, 0.0f, -25.61f, 0.0f }, { 99.0f, 3.0f, -25.61f, 0.0f },
            { 102.0f, 0.0f, -25.61f, 0.0f }, { 102.0f, 3.0f, -25.61f, 0.0f }, { 105.0f, 0.0f, -25.61f, 0.0f },
            { 105.0f, 3.0f, -25.61f, 0.0f }, { 108.0f, 0.0f, -25.61f, 0.0f }, { 108.0f, 3.0f, -25.61f, 0.0f },
            { 111.0f, 0.0f, -25.61f, 0.0f }, { 111.0f, 3.0f, -25.61f, 0.0f }, { 114.0f, -87.0f, -33.94f, 0.0f },
            { 114.0f, -84.0f, -33.94f, 0.0f }, { 114.0f, 0.0f, -25.61f, 0.0f }, { 114.0f, 3.0f, -25.61f, 0.0f },
            { 117.0f, -87.0f, -33.94f, 0.0f }, { 117.0f, -84.0f, -33.94f, 0.0f }, { 117.0f, -81.0f, -33.94f, 0.0f },
            { 117.0f, -78.0f, -33.94f, 0.0f }, { 117.0f, 0.0f, -25.61f, 0.0f }, { 117.0f, 3.0f, -25.61f, 0.0f },
            { 120.0f, -81.0f, -33.94f, 0.0f }, { 120.0f, -78.0f, -33.94f, 0.0f }, { 120.0f, -75.0f, -33.94f, 0.0f },
            { 120.0f, -72.0f, -33.94f, 0.0f }, { 120.0f, -69.0f, -33.94f, 0.0f }, { 120.0f, 0.0f, -25.61f, 0.0f },
            { 120.0f, 3.0f, -25.61f, 0.0f }, { 123.0f, -75.0f, -33.94f, 0.0f }, { 123.0f, -72.0f, -33.94f, 0.0f },
            { 123.0f, -69.0f, -33.94f, 0.0f }, { 123.0f, -66.0f, -33.94f, 0.0f }, { 123.0f, -63.0f, -33.94f, 0.0f },
            { 123.0f, -60.0f, -33.94f, 0.0f }, { 123.0f, -57.0f, -33.94f, 0.0f }, { 123.0f, 0.0f, -25.61f, 0.0f },
            { 123.0f, 3.0f, -25.61f, 0.0f }, { 126.0f, -66.0f, -33.94f, 0.0f }, { 126.0f, -63.0f, -33.94f, 0.0f },
            { 126.0f, -60.0f, -33.94f, 0.0f }, { 126.0f, -57.0f, -33.94f, 0.0f }, { 126.0f, -54.0f, -33.94f, 0.0f },
            { 126.0f, -51.0f, -33.94f, 0.0f }, { 126.0f, -48.0f, -33.94f, 0.0f }, { 126.0f, -45.0f, -33.94f, 0.0f },
            { 126.0f, -42.0f, -33.94f, 0.0f }, { 126.0f, -33.0f, -33.94f, 0.0f }, { 126.0f, -30.0f, -33.00f, 0.0f },
            { 126.0f, -24.0f, -30.70f, 0.0f }, { 126.0f, -21.0f, -29.60f, 0.0f }, { 126.0f, -12.0f, -26.21f, 0.0f },
            { 126.0f, 0.0f, -25.61f, 0.0f }, { 126.0f, 3.0f, -25.61f, 0.0f }, { 126.0f, 9.0f, -25.61f, 0.0f },
            { 126.0f, 12.0f, -25.66f, 0.0f }, { 126.0f, 24.0f, -30.15f, 0.0f }, { 126.0f, 33.0f, -33.51f, 0.0f },
            { 126.0f, 36.0f, -33.94f, 0.0f }, { 129.0f, -57.0f, -33.94f, 0.0f }, { 129.0f, -54.0f, -33.94f, 0.0f },
            { 129.0f, -51.0f, -33.94f, 0.0f }, { 129.0f, -48.0f, -33.94f, 0.0f }, { 129.0f, -45.0f, -33.94f, 0.0f },
            { 129.0f, -42.0f, -33.94f, 0.0f }, { 129.0f, -39.0f, -33.94f, 0.0f }, { 129.0f, -36.0f, -33.94f, 0.0f },
            { 129.0f, -33.0f, -33.94f, 0.0f }, { 129.0f, -30.0f, -32.98f, 0.0f }, { 129.0f, -27.0f, -31.84f, 0.0f },
            { 129.0f, -24.0f, -30.70f, 0.0f }, { 129.0f, -21.0f, -29.60f, 0.0f }, { 129.0f, -18.0f, -28.48f, 0.0f },
            { 129.0f, -15.0f, -27.32f, 0.0f }, { 129.0f, -12.0f, -26.21f, 0.0f }, { 129.0f, -9.0f, -25.61f, 0.0f },
            { 129.0f, -6.0f, -25.61f, 0.0f }, { 129.0f, -3.0f, -25.61f, 0.0f }, { 129.0f, 0.0f, -25.61f, 0.0f },
            { 129.0f, 3.0f, -25.61f, 0.0f }, { 129.0f, 6.0f, -25.61f, 0.0f }, { 129.0f, 9.0f, -25.61f, 0.0f },
            { 129.0f, 12.0f, -25.66f, 0.0f }, { 129.0f, 15.0f, -26.77f, 0.0f }, { 129.0f, 18.0f, -27.92f, 0.0f },
            { 129.0f, 21.0f, -29.04f, 0.0f }, { 129.0f, 24.0f, -30.15f, 0.0f }, { 129.0f, 27.0f, -31.26f, 0.0f },
            { 129.0f, 30.0f, -32.43f, 0.0f }, { 129.0f, 33.0f, -33.54f, 0.0f }, { 129.0f, 36.0f, -33.94f, 0.0f },
            { 129.0f, 39.0f, -33.94f, 0.0f }, { 129.0f, 42.0f, -33.94f, 0.0f }, { 129.0f, 45.0f, -33.94f, 0.0f },
            { 129.0f, 48.0f, -33.94f, 0.0f }, { 129.0f, 51.0f, -33.94f, 0.0f }, { 129.0f, 54.0f, -33.94f, 0.0f },
            { 132.0f, -24.0f, -30.70f, 0.0f }, { 132.0f, -21.0f, -29.60f, 0.0f }, { 132.0f, -12.0f, -26.21f, 0.0f },
            { 132.0f, -9.0f, -25.61f, 0.0f }, { 132.0f, 0.0f, -25.61f, 0.0f }, { 132.0f, 3.0f, -25.61f, 0.0f },
            { 132.0f, 12.0f, -25.66f, 0.0f }, { 132.0f, 21.0f, -29.04f, 0.0f }, { 132.0f, 24.0f, -30.15f, 0.0f },
            { 132.0f, 33.0f, -33.57f, 0.0f }, { 132.0f, 36.0f, -33.94f, 0.0f }, { 132.0f, 42.0f, -33.94f, 0.0f },
            { 132.0f, 45.0f, -33.94f, 0.0f }, { 132.0f, 48.0f, -33.94f, 0.0f }, { 132.0f, 51.0f, -33.94f, 0.0f },
            { 132.0f, 54.0f, -33.94f, 0.0f }, { 132.0f, 57.0f, -33.94f, 0.0f }, { 132.0f, 60.0f, -33.94f, 0.0f },
            { 132.0f, 63.0f, -33.94f, 0.0f }, { 132.0f, 66.0f, -33.94f, 0.0f }, { 135.0f, 0.0f, -25.61f, 0.0f },
            { 135.0f, 3.0f, -25.61f, 0.0f }, { 135.0f, 57.0f, -33.94f, 0.0f }, { 135.0f, 60.0f, -33.94f, 0.0f },
            { 135.0f, 63.0f, -33.94f, 0.0f }, { 135.0f, 66.0f, -33.94f, 0.0f }, { 135.0f, 69.0f, -33.94f, 0.0f },
            { 135.0f, 72.0f, -33.94f, 0.0f }, { 135.0f, 75.0f, -33.94f, 0.0f }, { 138.0f, 0.0f, -25.61f, 0.0f },
            { 138.0f, 3.0f, -25.61f, 0.0f }, { 138.0f, 72.0f, -33.94f, 0.0f }, { 138.0f, 75.0f, -33.94f, 0.0f },
            { 138.0f, 78.0f, -33.94f, 0.0f }, { 138.0f, 81.0f, -33.94f, 0.0f }, { 141.0f, 0.0f, -25.61f, 0.0f },
            { 141.0f, 3.0f, -25.61f, 0.0f }, { 141.0f, 78.0f, -33.94f, 0.0f }, { 141.0f, 81.0f, -33.94f, 0.0f },
            { 141.0f, 84.0f, -33.94f, 0.0f }, { 141.0f, 87.0f, -33.94f, 0.0f }, { 141.0f, 90.0f, -33.94f, 0.0f },
            { 144.0f, 84.0f, -33.94f, 0.0f }, { 144.0f, 87.0f, -33.94f, 0.0f }, { 150.0f, -3.0f, -25.61f, 0.0f },
            { 150.0f, 0.0f, -25.61f, 0.0f }, { 150.0f, 3.0f, -25.61f, 0.0f }, { 150.0f, 6.0f, -25.61f, 0.0f },
            { 153.0f, -3.0f, -25.61f, 0.0f }, { 153.0f, 0.0f, -25.61f, 0.0f }, { 153.0f, 3.0f, -25.61f, 0.0f },
            { 153.0f, 6.0f, -25.61f, 0.0f }, { 156.0f, -3.0f, -25.61f, 0.0f }, { 156.0f, 0.0f, -25.61f, 0.0f },
            { 156.0f, 3.0f, -25.61f, 0.0f }, { 156.0f, 6.0f, -25.61f, 0.0f }, { 159.0f, -3.0f, -25.61f, 0.0f },
            { 159.0f, 0.0f, -25.61f, 0.0f }, { 159.0f, 3.0f, -25.61f, 0.0f }, { 159.0f, 6.0f, -25.61f, 0.0f },
            { 162.0f, -3.0f, -25.61f, 0.0f }, { 162.0f, 0.0f, -25.61f, 0.0f }, { 162.0f, 3.0f, -25.61f, 0.0f },
            { 162.0f, 6.0f, -25.61f, 0.0f }, { 165.0f, -3.0f, -25.61f, 0.0f }, { 165.0f, 0.0f, -25.61f, 0.0f },
            { 165.0f, 3.0f, -25.61f, 0.0f }, { 165.0f, 6.0f, -25.61f, 0.0f }, { 168.0f, -3.0f, -25.61f, 0.0f },
            { 168.0f, 0.0f, -25.61f, 0.0f }, { 168.0f, 3.0f, -25.61f, 0.0f }, { 168.0f, 6.0f, -25.61f, 0.0f },
            { 171.0f, -3.0f, -25.61f, 0.0f }, { 171.0f, 0.0f, -25.61f, 0.0f }, { 171.0f, 3.0f, -25.61f, 0.0f },
            { 171.0f, 6.0f, -25.61f, 0.0f }
        };
    }

    /// **The Stockades' rooms** (M2 seek): every room off the hallways, 39 of them -- the eight cells along the
    /// entrance hallway (four front cells, each with a back room behind it), the hallway's end past the crossing, and
    /// in each wing four cells (front and back) along its corridor, its round hub, and three end rooms (front and back)
    /// off the hub. The entrance hallway runs along +x (north); the "west" wing is the +y one, the "east" the -y.
    ///
    /// **How they were authored** (offline, 2026-10-06; the live sim was training): the map's navmesh (mmaps 034, read
    /// only to author, never a bot input) scanned on a half-yard grid over x 40-210, y -160-160, with each
    /// walkable cell's floor height read from the vmaps (StockadeRoomsDataTest's scan); the walkable cells eroded by a
    /// yard and a half so the doorways part, the parts flooded and grown back, the entrance hallway with its crossing
    /// and ramps being the one left over. Each room's Floor is the convex hull of its eroded cells cut to at most eight
    /// corners (a corner's removal only ever shrinks the hull), so it stays a yard and a half or more off the walls;
    /// FloorZ is
    /// the median of its cells' vmap floor; Opening the middle of the cells it shares with the region it is entered
    /// from; Centre its walkable cell nearest its middle; Walk the walking distance from the spawn to the Centre, an
    /// eight-way Dijkstra over the scan's cells. Every 1-yd sample of every Floor was on the room's own walkable cells.
    ///
    /// **Validated** by StockadeRoomsDataTest against the map's floors (the vmaps, with FORGE_VISION_DATA): every 1-yd
    /// sample of every room has a floor within Seek.FloorTolerance of FloorZ (2,951 of them on 2026-10-06; the worst
    /// 0.8 yd off, in the two hubs, whose floors are not flat), and every sample but 18 in the hubs has a knee-height
    /// yard of room along each axis. An object stands on the floor the vmaps give at its spot, not at FloorZ.
    std::vector<SeekRoom> StockadeRooms()
    {
        // In order of walking distance from the spawn (their depth ranks them by Walk, not by this order).
        std::vector<SeekRoom> rooms = {
            // hall_east_1: 260 sq yd, 43 yd from the spawn, opening onto the hallway
            { "hall_east_1", -26.52f, { 84.5f, -5.5f }, { 84.5f, -14.5f }, 43.0f,
                { { 79.0f, -22.0f }, { 79.5f, -22.0f }, { 84.5f, -21.5f }, { 90.5f, -20.5f },
                  { 90.5f, -8.0f }, { 80.0f, -7.5f }, { 79.0f, -7.5f } } },
            // hall_west_1: 260 sq yd, 43 yd from the spawn, opening onto the hallway
            { "hall_west_1", -26.52f, { 84.5f, 6.6f }, { 85.0f, 16.0f }, 43.0f,
                { { 79.0f, 9.5f }, { 90.5f, 9.5f }, { 90.5f, 23.5f }, { 90.0f, 23.5f },
                  { 84.0f, 23.0f }, { 79.0f, 21.5f } } },
            // hall_west_1_back: 124 sq yd, 57 yd from the spawn, opening onto hall_west_1
            { "hall_west_1_back", -26.52f, { 82.5f, 25.6f }, { 84.5f, 30.0f }, 57.0f,
                { { 79.0f, 28.0f }, { 90.5f, 28.0f }, { 90.5f, 32.5f }, { 79.0f, 32.5f } } },
            // hall_east_1_back: 125 sq yd, 58 yd from the spawn, opening onto hall_east_1
            { "hall_east_1_back", -26.52f, { 86.8f, -23.8f }, { 85.0f, -29.0f }, 58.0f,
                { { 79.0f, -31.0f }, { 90.5f, -31.0f }, { 90.5f, -26.5f }, { 82.0f, -26.5f },
                  { 79.0f, -27.0f } } },
            // hall_east_2: 263 sq yd, 65 yd from the spawn, opening onto the hallway
            { "hall_east_2", -26.52f, { 107.1f, -5.6f }, { 106.5f, -14.5f }, 65.0f,
                { { 101.0f, -22.0f }, { 102.0f, -22.0f }, { 107.5f, -21.5f }, { 112.5f, -20.0f },
                  { 112.5f, -7.5f }, { 101.0f, -7.5f } } },
            // hall_west_2: 261 sq yd, 66 yd from the spawn, opening onto the hallway
            { "hall_west_2", -26.52f, { 106.9f, 6.6f }, { 107.0f, 16.0f }, 66.0f,
                { { 101.0f, 9.5f }, { 112.5f, 9.5f }, { 112.5f, 23.5f }, { 111.5f, 23.5f },
                  { 107.0f, 23.0f }, { 101.0f, 21.5f } } },
            // hall_west_2_back: 123 sq yd, 79 yd from the spawn, opening onto hall_west_2
            { "hall_west_2_back", -26.52f, { 106.9f, 25.6f }, { 106.5f, 30.0f }, 79.0f,
                { { 101.0f, 28.0f }, { 112.5f, 28.0f }, { 112.5f, 32.5f }, { 101.0f, 32.5f } } },
            // hall_east_2_back: 122 sq yd, 80 yd from the spawn, opening onto hall_east_2
            { "hall_east_2_back", -26.52f, { 108.8f, -24.0f }, { 107.0f, -29.0f }, 80.0f,
                { { 101.0f, -31.0f }, { 112.5f, -31.0f }, { 112.5f, -26.5f }, { 107.5f, -26.5f },
                  { 101.0f, -27.0f } } },
            // hall_end: 406 sq yd, 106 yd from the spawn, opening onto the hallway
            { "hall_end", -25.61f, { 146.0f, 0.5f }, { 160.0f, 0.5f }, 106.0f,
                { { 148.5f, -5.0f }, { 172.0f, -5.0f }, { 172.0f, 6.5f }, { 148.5f, 6.5f } } },
            // east_south_1: 232 sq yd, 124 yd from the spawn, opening onto the hallway
            { "east_south_1", -34.86f, { 122.0f, -43.0f }, { 113.0f, -41.5f }, 124.0f,
                { { 106.0f, -40.5f }, { 107.5f, -45.5f }, { 108.5f, -46.0f }, { 117.0f, -47.5f },
                  { 119.0f, -47.5f }, { 120.0f, -37.5f }, { 110.5f, -36.5f }, { 106.0f, -36.5f } } },
            // west_south_1: 300 sq yd, 126 yd from the spawn, opening onto the hallway
            { "west_south_1", -34.86f, { 124.0f, 45.8f }, { 115.0f, 47.0f }, 126.0f,
                { { 108.0f, 40.5f }, { 114.0f, 40.0f }, { 121.0f, 40.0f }, { 121.5f, 42.0f },
                  { 122.5f, 52.5f }, { 111.0f, 54.5f }, { 108.5f, 54.0f }, { 108.0f, 50.5f } } },
            // west_north_1: 234 sq yd, 127 yd from the spawn, opening onto the hallway
            { "west_north_1", -34.86f, { 136.1f, 44.5f }, { 145.0f, 43.0f }, 127.0f,
                { { 138.0f, 38.5f }, { 146.5f, 38.0f }, { 152.0f, 38.0f }, { 152.0f, 43.0f },
                  { 151.0f, 47.0f }, { 149.5f, 47.5f }, { 141.5f, 49.0f }, { 139.0f, 49.0f } } },
            // east_north_1: 303 sq yd, 128 yd from the spawn, opening onto the hallway
            { "east_north_1", -34.86f, { 134.2f, -44.6f }, { 143.5f, -45.5f }, 128.0f,
                { { 135.5f, -51.0f }, { 140.5f, -52.0f }, { 149.5f, -53.5f }, { 150.0f, -50.0f },
                  { 151.0f, -39.0f }, { 142.5f, -38.5f }, { 137.0f, -38.5f }, { 135.5f, -50.0f } } },
            // east_south_1_back: 96 sq yd, 139 yd from the spawn, opening onto east_south_1
            { "east_south_1_back", -34.86f, { 103.5f, -40.5f }, { 99.0f, -40.0f }, 139.0f,
                { { 96.0f, -43.5f }, { 97.5f, -44.0f }, { 100.5f, -44.5f }, { 101.5f, -39.5f },
                  { 101.5f, -36.5f }, { 99.5f, -36.0f }, { 97.0f, -36.0f }, { 96.5f, -37.5f } } },
            // west_south_1_back: 158 sq yd, 140 yd from the spawn, opening onto west_south_1
            { "west_south_1_back", -34.86f, { 105.9f, 49.5f }, { 101.0f, 48.5f }, 140.0f,
                { { 98.0f, 41.0f }, { 102.5f, 41.0f }, { 103.5f, 49.0f }, { 104.0f, 55.5f },
                  { 103.0f, 56.0f }, { 100.0f, 56.5f }, { 99.5f, 56.0f }, { 98.0f, 43.5f } } },
            // west_north_1_back: 96 sq yd, 142 yd from the spawn, opening onto west_north_1
            { "west_north_1_back", -34.86f, { 155.0f, 42.2f }, { 159.5f, 41.5f }, 142.0f,
                { { 157.0f, 37.5f }, { 161.5f, 37.5f }, { 162.0f, 44.0f }, { 162.0f, 45.0f },
                  { 161.0f, 45.5f }, { 158.0f, 46.0f }, { 157.5f, 45.0f }, { 157.0f, 41.5f } } },
            // east_north_1_back: 155 sq yd, 143 yd from the spawn, opening onto east_north_1
            { "east_north_1_back", -34.86f, { 153.2f, -45.5f }, { 157.5f, -47.0f }, 143.0f,
                { { 154.5f, -54.5f }, { 157.0f, -55.0f }, { 158.5f, -55.0f }, { 159.0f, -52.0f },
                  { 160.5f, -40.5f }, { 160.5f, -39.5f }, { 156.0f, -39.5f }, { 155.0f, -47.0f } } },
            // east_south_2: 227 sq yd, 148 yd from the spawn, opening onto the hallway
            { "east_south_2", -34.86f, { 118.0f, -63.8f }, { 109.0f, -61.0f }, 148.0f,
                { { 103.0f, -64.0f }, { 113.5f, -67.5f }, { 114.0f, -67.0f }, { 115.5f, -62.5f },
                  { 116.5f, -58.0f }, { 115.0f, -57.5f }, { 103.5f, -55.0f }, { 102.5f, -59.5f } } },
            // east_north_2: 304 sq yd, 149 yd from the spawn, opening onto the hallway
            { "east_north_2", -34.86f, { 129.1f, -67.0f }, { 138.5f, -70.0f }, 149.0f,
                { { 131.0f, -74.0f }, { 143.0f, -78.5f }, { 143.5f, -78.0f }, { 145.0f, -72.0f },
                  { 146.5f, -64.5f }, { 135.0f, -62.0f }, { 133.5f, -62.0f }, { 130.5f, -72.0f } } },
            // west_south_2: 300 sq yd, 151 yd from the spawn, opening onto the hallway
            { "west_south_2", -34.86f, { 128.9f, 68.8f }, { 119.5f, 71.5f }, 151.0f,
                { { 112.0f, 66.5f }, { 112.5f, 66.0f }, { 123.0f, 63.5f }, { 125.0f, 64.0f },
                  { 128.0f, 75.0f }, { 116.5f, 79.5f }, { 114.0f, 79.0f }, { 112.5f, 73.5f } } },
            // west_north_2: 234 sq yd, 151 yd from the spawn, opening onto the hallway
            { "west_north_2", -34.86f, { 140.2f, 65.2f }, { 149.0f, 62.5f }, 151.0f,
                { { 141.5f, 59.5f }, { 154.0f, 56.5f }, { 155.5f, 57.0f }, { 155.5f, 65.0f },
                  { 155.0f, 65.5f }, { 145.0f, 69.0f }, { 144.0f, 68.5f }, { 141.5f, 60.5f } } },
            // east_south_2_back: 93 sq yd, 163 yd from the spawn, opening onto east_south_2
            { "east_south_2_back", -34.86f, { 99.8f, -58.4f }, { 95.0f, -57.0f }, 163.0f,
                { { 92.5f, -60.0f }, { 96.0f, -61.0f }, { 97.5f, -57.5f }, { 98.0f, -54.0f },
                  { 97.5f, -53.5f }, { 94.5f, -53.0f }, { 93.5f, -55.0f }, { 92.5f, -59.0f } } },
            // east_north_2_back: 154 sq yd, 164 yd from the spawn, opening onto east_north_2
            { "east_north_2_back", -34.86f, { 147.5f, -72.6f }, { 152.0f, -73.5f }, 164.0f,
                { { 147.5f, -80.0f }, { 150.5f, -81.0f }, { 151.5f, -81.0f }, { 153.0f, -77.0f },
                  { 156.0f, -67.0f }, { 155.0f, -66.5f }, { 151.5f, -66.0f }, { 147.5f, -79.5f } } },
            // west_south_2_back: 154 sq yd, 165 yd from the spawn, opening onto west_south_2
            { "west_south_2_back", -34.86f, { 110.4f, 72.2f }, { 106.5f, 75.0f }, 165.0f,
                { { 103.5f, 68.0f }, { 107.0f, 67.5f }, { 109.0f, 74.5f }, { 110.5f, 81.5f },
                  { 108.0f, 82.5f }, { 106.5f, 82.5f }, { 106.0f, 81.0f }, { 102.5f, 69.0f } } },
            // west_north_2_back: 92 sq yd, 167 yd from the spawn, opening onto west_north_2
            { "west_north_2_back", -34.86f, { 158.8f, 59.5f }, { 163.0f, 58.5f }, 167.0f,
                { { 160.0f, 55.5f }, { 161.0f, 55.0f }, { 164.0f, 54.5f }, { 165.5f, 59.5f },
                  { 165.5f, 61.5f }, { 162.5f, 62.5f }, { 162.0f, 62.5f }, { 160.5f, 58.5f } } },
            // east_hub: 772 sq yd, 184 yd from the spawn, opening onto the hallway
            { "east_hub", -35.19f, { 113.0f, -91.5f }, { 105.0f, -106.0f }, 184.0f,
                { { 95.5f, -116.5f }, { 101.5f, -119.5f }, { 110.0f, -118.5f }, { 117.5f, -112.5f },
                  { 117.5f, -101.0f }, { 109.0f, -93.0f }, { 99.5f, -93.0f }, { 92.0f, -102.5f } } },
            // west_hub: 763 sq yd, 187 yd from the spawn, opening onto the hallway
            { "west_hub", -35.19f, { 145.2f, 92.8f }, { 153.0f, 107.5f }, 187.0f,
                { { 140.5f, 104.0f }, { 143.5f, 97.0f }, { 153.0f, 94.5f }, { 164.0f, 98.5f },
                  { 166.5f, 104.0f }, { 163.0f, 118.0f }, { 148.0f, 120.5f }, { 142.0f, 116.5f } } },
            // east_end_1: 264 sq yd, 202 yd from the spawn, opening onto east_hub
            { "east_end_1", -33.94f, { 120.2f, -114.2f }, { 128.5f, -118.5f }, 202.0f,
                { { 120.5f, -120.5f }, { 132.0f, -126.5f }, { 132.5f, -126.5f }, { 135.0f, -122.0f },
                  { 135.5f, -116.0f }, { 125.5f, -111.0f }, { 124.0f, -113.0f }, { 120.5f, -120.0f } } },
            // east_end_2: 267 sq yd, 203 yd from the spawn, opening onto east_hub
            { "east_end_2", -33.94f, { 89.8f, -98.2f }, { 81.5f, -93.5f }, 203.0f,
                { { 74.0f, -96.0f }, { 85.0f, -101.5f }, { 89.5f, -93.0f }, { 90.0f, -92.0f },
                  { 89.5f, -91.5f }, { 77.5f, -85.5f }, { 75.0f, -90.0f } } },
            // west_end_1: 264 sq yd, 205 yd from the spawn, opening onto west_hub
            { "west_end_1", -33.94f, { 138.0f, 115.5f }, { 129.5f, 120.5f }, 205.0f,
                { { 122.0f, 118.0f }, { 127.5f, 115.0f }, { 132.5f, 112.5f }, { 133.0f, 112.5f },
                  { 135.5f, 117.0f }, { 138.0f, 122.0f }, { 126.0f, 128.0f }, { 123.0f, 123.5f } } },
            // west_end_3: 265 sq yd, 206 yd from the spawn, opening onto west_hub
            { "west_end_3", -33.94f, { 168.5f, 99.5f }, { 177.0f, 95.0f }, 206.0f,
                { { 169.0f, 93.0f }, { 180.5f, 87.0f }, { 183.5f, 91.5f }, { 184.0f, 97.5f },
                  { 180.5f, 99.5f }, { 174.5f, 102.5f }, { 173.5f, 102.5f }, { 168.5f, 94.0f } } },
            // east_end_3: 262 sq yd, 213 yd from the spawn, opening onto east_hub
            { "east_end_3", -33.94f, { 97.0f, -121.5f }, { 92.5f, -129.5f }, 213.0f,
                { { 85.0f, -133.5f }, { 89.0f, -136.5f }, { 95.0f, -137.0f }, { 100.0f, -127.0f },
                  { 99.0f, -125.5f }, { 91.5f, -121.5f }, { 90.5f, -122.0f }, { 85.0f, -132.5f } } },
            // west_end_2: 267 sq yd, 216 yd from the spawn, opening onto west_hub
            { "west_end_2", -33.94f, { 161.2f, 122.8f }, { 166.0f, 131.0f }, 216.0f,
                { { 158.5f, 127.5f }, { 162.5f, 125.0f }, { 167.5f, 123.0f }, { 173.5f, 134.0f },
                  { 173.0f, 135.5f }, { 169.0f, 137.5f }, { 163.5f, 139.0f }, { 158.0f, 128.5f } } },
            // east_end_1_back: 124 sq yd, 217 yd from the spawn, opening onto east_end_1
            { "east_end_1_back", -33.94f, { 136.5f, -124.6f }, { 141.0f, -125.5f }, 217.0f,
                { { 137.5f, -129.5f }, { 140.0f, -131.0f }, { 140.5f, -131.0f }, { 145.5f, -121.5f },
                  { 142.5f, -120.0f }, { 141.5f, -120.0f }, { 138.5f, -124.5f }, { 137.5f, -128.0f } } },
            // east_end_2_back: 121 sq yd, 218 yd from the spawn, opening onto east_end_2
            { "east_end_2_back", -33.94f, { 73.1f, -88.9f }, { 69.0f, -87.0f }, 218.0f,
                { { 65.0f, -91.0f }, { 68.0f, -92.5f }, { 68.5f, -92.5f }, { 70.0f, -90.5f },
                  { 71.5f, -88.0f }, { 73.0f, -83.0f }, { 70.5f, -81.5f }, { 69.5f, -82.0f } } },
            // west_end_1_back: 121 sq yd, 221 yd from the spawn, opening onto west_end_1
            { "west_end_1_back", -33.94f, { 121.1f, 124.6f }, { 117.0f, 127.0f }, 221.0f,
                { { 113.0f, 123.0f }, { 115.5f, 121.5f }, { 116.5f, 121.5f }, { 119.5f, 125.5f },
                  { 121.0f, 130.5f }, { 121.0f, 131.0f }, { 118.0f, 132.5f }, { 114.0f, 125.0f } } },
            // west_end_3_back: 122 sq yd, 221 yd from the spawn, opening onto west_end_3
            { "west_end_3_back", -33.94f, { 185.4f, 90.4f }, { 189.0f, 88.5f }, 221.0f,
                { { 185.5f, 84.0f }, { 187.5f, 83.0f }, { 188.5f, 83.5f }, { 193.0f, 91.5f },
                  { 193.0f, 92.5f }, { 190.5f, 94.0f }, { 189.5f, 94.0f }, { 186.5f, 89.5f } } },
            // east_end_3_back: 124 sq yd, 229 yd from the spawn, opening onto east_end_3
            { "east_end_3_back", -33.94f, { 90.5f, -139.2f }, { 86.0f, -142.5f }, 229.0f,
                { { 80.5f, -141.5f }, { 88.0f, -145.5f }, { 90.0f, -146.0f }, { 90.5f, -145.5f },
                  { 92.0f, -142.5f }, { 87.0f, -140.0f }, { 82.5f, -138.5f }, { 81.5f, -139.0f } } },
            // west_end_2_back: 123 sq yd, 231 yd from the spawn, opening onto west_end_2
            { "west_end_2_back", -33.94f, { 168.6f, 140.5f }, { 172.5f, 143.5f }, 231.0f,
                { { 166.5f, 144.5f }, { 171.0f, 141.5f }, { 176.0f, 140.0f }, { 176.5f, 140.0f },
                  { 177.5f, 142.0f }, { 177.0f, 143.5f }, { 169.0f, 147.5f }, { 168.0f, 147.0f } } }
        };
        // The front cells (SeekRoom::Front, the seek ladder's doorway and room rungs): each room opening onto the
        // hallway but the two hubs -- the four cells along the entrance hallway, its end, and the wings' four cells
        // each. The back rooms, the hubs and the end rooms are the deep rung's.
        static char const* const FRONT[] = { "hall_east_1", "hall_west_1", "hall_east_2", "hall_west_2", "hall_end",
            "east_south_1", "east_north_1", "east_south_2", "east_north_2", "west_south_1", "west_north_1",
            "west_south_2", "west_north_2" };
        for (SeekRoom& room : rooms)
            room.Front = std::any_of(std::begin(FRONT), std::end(FRONT), [&room](char const* name)
            {
                return room.Name == name;
            });
        return rooms;
    }

    /// **The seek stage's objects**: real gameobject_template entries whose displays have collision models in the
    /// vmaps' GameObjectModels.dtree (so the camera's rays and the player controller meet them), each a thing a player
    /// would look for in a cell, and none of them part of the Stockades' own spawns. Type 5 (generic) where the world
    /// has one, so nothing about them can be used, looted or opened; the strongbox is a type 3 chest with no quest and
    /// no flags. Height is the model's bounding box height (dtree) times the template's size (all 1): the objective
    /// point is the object's centre. Radius is the box's largest half-extent: the camera's flag reaches its radius and
    /// a quarter yard more (at most a yard; Vision::ObjectiveRadiusFor), so it sits on the object. Looked up in
    /// acore_world on 2026-10-06 (MySQL is sealed after startup, so nothing is queried at runtime).
    std::vector<SeekObject> SeekObjects()
    {
        return {
            { 144111, "chest", 1.32f, 0.78f },      // Smite's Chest, display 259 Treasurechest01.m2, 1.2 x 1.5 yd
            { 179972, "crate", 1.24f, 0.66f },      // Stormwind Crate 01, display 31 Stormwindcrate01.m2, 1.2 x 1.3 yd
            { 179967, "barrel", 0.99f, 0.50f },     // Barrel 01, display 32 Barrel01.m2, 0.9 x 1.0 yd
            { 180660, "sack", 0.94f, 0.55f },       // Sack of Gold, display 6484 Sack01_01.m2, 1.1 x 1.1 yd
            { 2039, "strongbox", 0.61f, 0.58f },    // Hidden Strongbox (type 3), display 10 Chest01.m2, 0.8 x 1.2 yd
        };
    }

    /// **M1's evaluation** (perception-goals REDESIGN §1): a fixed set of (spawn, object) pairs over the hallways,
    /// indexes into StockadeHallways(), each played with the compass and without (SightDraw::EvaluationPick); the
    /// object of pair i is the pool's object i mod 5. Twenty-four in sight of the spawn and eight just round a corner
    /// (a doorframe, the crossing into a wing): a quarter, the training's CornerShare at the top rungs.
    ///
    /// **Authored** (offline, 2026-10-06) by StockadeHallwaysDataTest's AuthoringPairs: the spawns spread over the
    /// table (the entrance first, then a stride of 97 points), each object drawn as SightDraw::Place draws it, over
    /// the map's vmaps cast as the camera casts. **Validated** there too (EveryEvaluationPairIsWhatItSays): each pair
    /// is what it says from a small body's eye and a tall one's (1 and 2.4 yd), for every object of the pool.
    std::vector<SightPair> StockadeSightPairs()
    {
        return {
            { 0, 27, false },       // (54.2 0.3) -> (93 0), 38.8 yd: the entrance, down the ramp
            { 97, 41, false },      // (129 -30) -> (114 -87), 58.9 yd
            { 194, 170, false },    // (168 -3) -> (150 -3), 18.0 yd
            { 89, 58, false },      // (129 -54) -> (123 -75), 21.8 yd
            { 186, 109, false },    // (162 -3) -> (129 6), 34.2 yd
            { 81, 161, false },     // (126 0) -> (141 0), 15.0 yd
            { 178, 162, false },    // (156 -3) -> (141 3), 16.2 yd
            { 73, 129, false },     // (126 -48) -> (132 -9), 39.5 yd
            { 170, 194, false },    // (150 -3) -> (168 -3), 18.0 yd
            { 65, 5, false },       // (123 0) -> (60 0), 63.0 yd
            { 162, 7, false },      // (141 3) -> (63 0), 78.1 yd
            { 57, 11, false },      // (120 3) -> (69 0), 51.1 yd
            { 154, 138, false },    // (135 75) -> (132 45), 30.1 yd
            { 49, 35, false },      // (117 0) -> (105 0), 12.0 yd
            { 146, 13, false },     // (135 0) -> (72 0), 63.0 yd
            { 41, 75, false },      // (114 -87) -> (126 -42), 46.6 yd
            { 138, 151, false },    // (132 45) -> (135 66), 21.2 yd
            { 33, 187, false },     // (102 0) -> (162 0), 60.0 yd
            { 130, 179, false },    // (132 0) -> (156 0), 24.0 yd
            { 25, 34, false },      // (90 0) -> (102 3), 12.4 yd
            { 122, 117, false },    // (129 45) -> (129 30), 15.0 yd
            { 17, 179, false },     // (78 0) -> (156 0), 78.0 yd
            { 114, 119, false },    // (129 21) -> (129 36), 15.0 yd
            { 9, 0, false },        // (66 0) -> (54.2 0.3), 11.8 yd: back up the ramp to the entrance
            { 106, 97, true },      // (129 -3) -> (129 -30), 27.0 yd
            { 195, 162, true },     // (168 0) -> (141 3), 27.2 yd
            { 187, 109, true },     // (162 0) -> (129 6), 33.5 yd
            { 179, 162, true },     // (156 0) -> (141 3), 15.3 yd
            { 171, 106, true },     // (150 0) -> (129 -3), 21.2 yd
            { 66, 180, true },      // (123 3) -> (156 3), 33.0 yd
            { 163, 133, true },     // (141 78) -> (132 21), 57.7 yd
            { 42, 77, true },       // (114 -84) -> (126 -30), 55.3 yd
        };
    }

    /// **M3's sites** (dungeon-curriculum M3, InteractSite): the Deadmines' own doors and what opens each, with the
    /// floor either side. Three lever doors -- the Factory Door (13965, lever 101831), the Foundry Door (16399, lever
    /// 101834) and the Mast Room Door (16400, lever 101832), each lever a few yards from its door on one side, linked
    /// to it by the map's own script (smart_scripts: the lever's state change activates the door) -- and the Iron Clad
    /// Door (16397), which the cannon (16398, lock 83: the Defias Gunpowder, item 5397, whose spell 6250 opens it)
    /// blows open from the tunnel; that door's own lever (101833) stands on the cove's side, out of the seat's reach.
    /// The doors' locks (85, 202) take no key a hand has: only their lever, or the cannon, opens them.
    ///
    /// Near is the opener's side (the seat's spawns and the distinguish and key rungs' spots), Far the floor behind the
    /// shut door (the switch rung's spots); a 3-yd grid, each point 1.5 yd or more off every wall, 3 yd or more off the
    /// lever, lock and door.
    ///
    /// **How they were authored** (offline, 2026-10-06): DeadminesSitesDataTest's AuthoringScan (the navmesh read only
    /// to author, never a bot input) on every storey, then .agents/plans/dungeon-curriculum/tools/sites.py: the
    /// door's storey within 28 yd, cells where the vmap floor agrees with the navmesh within a yard, eroded 1.5 yd,
    /// the shut door's footprint taken out, and flooded from either side of the door -- no cell is reached from both
    /// (no way round the door within the site); seven Mast Room points the data test found within Seek.Clearance of a
    /// vmap wall the navmesh's erosion missed are left out. **Validated** by DeadminesSitesDataTest against the map's vmaps
    /// (FORGE_VISION_DATA): every point on a vmap floor within a quarter yard and clear of walls at knee height, and
    /// no line of sight from a near point's eye to a far point that does not pass through the shut door's leaf.
    std::vector<InteractSite> DeadminesSites()
    {
        return {
            { "factory", 13965, 101831, 0,
                {
                    { -195.0f, -483.0f, 54.05f, 0.0f }, { -192.0f, -483.0f, 54.05f, 0.0f },
                    { -192.0f, -480.0f, 54.04f, 0.0f }, { -192.0f, -465.0f, 54.26f, 0.0f },
                    { -189.0f, -483.0f, 54.05f, 0.0f }, { -189.0f, -480.0f, 54.05f, 0.0f },
                    { -189.0f, -477.0f, 54.05f, 0.0f }, { -189.0f, -474.0f, 54.05f, 0.0f },
                    { -189.0f, -471.0f, 54.05f, 0.0f }, { -189.0f, -468.0f, 54.12f, 0.0f },
                    { -189.0f, -465.0f, 54.21f, 0.0f }, { -186.0f, -483.0f, 54.05f, 0.0f },
                    { -186.0f, -480.0f, 54.04f, 0.0f }
                },
                {
                    { -210.0f, -462.0f, 53.82f, 0.0f }, { -210.0f, -441.0f, 54.12f, 0.0f },
                    { -207.0f, -459.0f, 53.87f, 0.0f }, { -207.0f, -444.0f, 53.97f, 0.0f },
                    { -204.0f, -459.0f, 54.11f, 0.0f }, { -204.0f, -456.0f, 54.23f, 0.0f },
                    { -204.0f, -453.0f, 54.22f, 0.0f }, { -204.0f, -450.0f, 54.07f, 0.0f },
                    { -204.0f, -447.0f, 53.89f, 0.0f }, { -204.0f, -444.0f, 53.56f, 0.0f },
                    { -201.0f, -459.0f, 54.34f, 0.0f }, { -201.0f, -456.0f, 54.34f, 0.0f },
                    { -201.0f, -453.0f, 54.34f, 0.0f }, { -201.0f, -450.0f, 54.22f, 0.0f },
                    { -201.0f, -447.0f, 54.30f, 0.0f }, { -201.0f, -444.0f, 53.79f, 0.0f },
                    { -201.0f, -441.0f, 53.40f, 0.0f }, { -201.0f, -438.0f, 53.10f, 0.0f },
                    { -201.0f, -435.0f, 53.60f, 0.0f }, { -198.0f, -450.0f, 54.50f, 0.0f },
                    { -198.0f, -444.0f, 53.92f, 0.0f }, { -198.0f, -441.0f, 53.42f, 0.0f },
                    { -198.0f, -438.0f, 53.27f, 0.0f }, { -198.0f, -435.0f, 53.46f, 0.0f },
                    { -195.0f, -450.0f, 54.48f, 0.0f }, { -195.0f, -444.0f, 53.89f, 0.0f },
                    { -195.0f, -441.0f, 53.52f, 0.0f }, { -195.0f, -438.0f, 53.40f, 0.0f },
                    { -195.0f, -435.0f, 53.34f, 0.0f }, { -195.0f, -432.0f, 53.77f, 0.0f },
                    { -192.0f, -447.0f, 54.37f, 0.0f }, { -192.0f, -444.0f, 53.99f, 0.0f },
                    { -192.0f, -441.0f, 53.51f, 0.0f }, { -192.0f, -438.0f, 53.38f, 0.0f },
                    { -192.0f, -435.0f, 53.31f, 0.0f }, { -192.0f, -432.0f, 53.63f, 0.0f },
                    { -189.0f, -450.0f, 54.57f, 0.0f }, { -189.0f, -441.0f, 53.39f, 0.0f },
                    { -189.0f, -438.0f, 53.36f, 0.0f }, { -189.0f, -435.0f, 53.39f, 0.0f },
                    { -189.0f, -432.0f, 53.74f, 0.0f }, { -186.0f, -450.0f, 54.62f, 0.0f },
                    { -186.0f, -441.0f, 53.88f, 0.0f }, { -186.0f, -438.0f, 53.68f, 0.0f },
                    { -186.0f, -435.0f, 53.72f, 0.0f }, { -186.0f, -432.0f, 53.79f, 0.0f },
                    { -183.0f, -456.0f, 54.66f, 0.0f }, { -183.0f, -444.0f, 54.62f, 0.0f },
                    { -183.0f, -441.0f, 54.25f, 0.0f }, { -183.0f, -438.0f, 54.14f, 0.0f },
                    { -183.0f, -435.0f, 54.20f, 0.0f }, { -180.0f, -435.0f, 54.92f, 0.0f }
                },
            },
            { "foundry", 16399, 101834, 0,
                {
                    { -159.0f, -585.0f, 19.31f, 0.0f }, { -159.0f, -582.0f, 19.31f, 0.0f },
                    { -159.0f, -579.0f, 19.31f, 0.0f }, { -159.0f, -576.0f, 19.31f, 0.0f },
                    { -156.0f, -585.0f, 19.31f, 0.0f }, { -156.0f, -582.0f, 19.31f, 0.0f },
                    { -156.0f, -579.0f, 19.31f, 0.0f }, { -156.0f, -576.0f, 19.31f, 0.0f },
                    { -153.0f, -585.0f, 19.32f, 0.0f }, { -153.0f, -582.0f, 19.32f, 0.0f },
                    { -153.0f, -579.0f, 19.32f, 0.0f }, { -153.0f, -576.0f, 19.32f, 0.0f },
                    { -150.0f, -585.0f, 19.32f, 0.0f }, { -150.0f, -582.0f, 19.32f, 0.0f },
                    { -150.0f, -579.0f, 19.32f, 0.0f }, { -150.0f, -576.0f, 19.32f, 0.0f },
                    { -150.0f, -573.0f, 19.32f, 0.0f }, { -147.0f, -585.0f, 18.86f, 0.0f },
                    { -147.0f, -576.0f, 18.92f, 0.0f }, { -147.0f, -573.0f, 19.25f, 0.0f },
                    { -144.0f, -576.0f, 18.99f, 0.0f }, { -144.0f, -573.0f, 19.17f, 0.0f }
                },
                {
                    { -192.0f, -579.0f, 20.98f, 0.0f }, { -192.0f, -576.0f, 20.98f, 0.0f },
                    { -192.0f, -573.0f, 20.98f, 0.0f }, { -192.0f, -570.0f, 20.98f, 0.0f },
                    { -189.0f, -579.0f, 20.98f, 0.0f }, { -189.0f, -576.0f, 20.98f, 0.0f },
                    { -189.0f, -573.0f, 20.98f, 0.0f }, { -189.0f, -570.0f, 20.98f, 0.0f },
                    { -189.0f, -567.0f, 20.50f, 0.0f }, { -186.0f, -579.0f, 20.98f, 0.0f },
                    { -186.0f, -576.0f, 20.98f, 0.0f }, { -186.0f, -573.0f, 20.56f, 0.0f },
                    { -186.0f, -567.0f, 19.31f, 0.0f }, { -186.0f, -564.0f, 19.31f, 0.0f },
                    { -186.0f, -561.0f, 19.31f, 0.0f }, { -183.0f, -582.0f, 19.31f, 0.0f },
                    { -183.0f, -579.0f, 19.31f, 0.0f }, { -183.0f, -576.0f, 19.31f, 0.0f },
                    { -183.0f, -573.0f, 19.31f, 0.0f }, { -183.0f, -570.0f, 19.31f, 0.0f },
                    { -183.0f, -567.0f, 19.31f, 0.0f }, { -183.0f, -564.0f, 19.31f, 0.0f },
                    { -183.0f, -561.0f, 19.31f, 0.0f }, { -180.0f, -585.0f, 19.32f, 0.0f },
                    { -180.0f, -582.0f, 19.31f, 0.0f }, { -180.0f, -579.0f, 19.31f, 0.0f },
                    { -180.0f, -576.0f, 19.31f, 0.0f }, { -180.0f, -573.0f, 19.31f, 0.0f },
                    { -180.0f, -570.0f, 19.31f, 0.0f }, { -180.0f, -567.0f, 19.31f, 0.0f },
                    { -180.0f, -564.0f, 19.31f, 0.0f }, { -180.0f, -561.0f, 19.31f, 0.0f },
                    { -180.0f, -558.0f, 19.31f, 0.0f }, { -177.0f, -585.0f, 19.32f, 0.0f },
                    { -177.0f, -582.0f, 19.31f, 0.0f }, { -177.0f, -579.0f, 19.31f, 0.0f },
                    { -177.0f, -576.0f, 19.31f, 0.0f }, { -177.0f, -573.0f, 19.31f, 0.0f },
                    { -177.0f, -570.0f, 19.31f, 0.0f }, { -177.0f, -567.0f, 19.31f, 0.0f },
                    { -177.0f, -564.0f, 19.31f, 0.0f }, { -177.0f, -561.0f, 19.31f, 0.0f },
                    { -174.0f, -585.0f, 19.32f, 0.0f }, { -174.0f, -573.0f, 19.31f, 0.0f },
                    { -174.0f, -570.0f, 19.31f, 0.0f }, { -174.0f, -567.0f, 19.31f, 0.0f }
                },
            },
            { "mast_room", 16400, 101832, 0,
                {
                    { -297.0f, -555.0f, 49.45f, 0.0f }, { -297.0f, -552.0f, 49.45f, 0.0f },
                    { -297.0f, -549.0f, 49.44f, 0.0f }, { -297.0f, -546.0f, 49.44f, 0.0f },
                    { -294.0f, -561.0f, 48.99f, 0.0f }, { -294.0f, -558.0f, 48.95f, 0.0f },
                    { -294.0f, -555.0f, 49.45f, 0.0f }, { -294.0f, -552.0f, 49.45f, 0.0f },
                    { -294.0f, -549.0f, 49.44f, 0.0f }, { -294.0f, -546.0f, 49.44f, 0.0f },
                    { -291.0f, -561.0f, 48.86f, 0.0f }, { -291.0f, -555.0f, 49.45f, 0.0f },
                    { -291.0f, -552.0f, 49.45f, 0.0f }, { -291.0f, -549.0f, 49.45f, 0.0f },
                    { -291.0f, -546.0f, 49.45f, 0.0f }, { -288.0f, -561.0f, 49.01f, 0.0f },
                    { -288.0f, -558.0f, 48.96f, 0.0f }, { -288.0f, -555.0f, 49.45f, 0.0f },
                    { -288.0f, -552.0f, 49.45f, 0.0f }, { -288.0f, -549.0f, 49.44f, 0.0f },
                    { -288.0f, -546.0f, 49.44f, 0.0f }, { -285.0f, -561.0f, 49.23f, 0.0f },
                    { -285.0f, -558.0f, 49.12f, 0.0f }, { -285.0f, -555.0f, 49.45f, 0.0f },
                    { -285.0f, -552.0f, 49.45f, 0.0f }, { -285.0f, -546.0f, 49.44f, 0.0f }
                },
                {
                    { -300.0f, -531.0f, 49.41f, 0.0f }, { -300.0f, -528.0f, 49.35f, 0.0f },
                    { -297.0f, -531.0f, 49.45f, 0.0f }, { -297.0f, -528.0f, 49.28f, 0.0f },
                    { -297.0f, -525.0f, 49.47f, 0.0f }, { -291.0f, -528.0f, 49.75f, 0.0f },
                    { -291.0f, -525.0f, 49.64f, 0.0f }, { -291.0f, -522.0f, 49.54f, 0.0f },
                    { -291.0f, -519.0f, 49.54f, 0.0f }, { -291.0f, -516.0f, 49.54f, 0.0f },
                    { -291.0f, -513.0f, 49.68f, 0.0f }, { -288.0f, -516.0f, 49.54f, 0.0f },
                    { -288.0f, -513.0f, 49.68f, 0.0f }, { -285.0f, -531.0f, 49.25f, 0.0f },
                    { -285.0f, -516.0f, 48.49f, 0.0f }, { -282.0f, -531.0f, 49.12f, 0.0f },
                    { -282.0f, -528.0f, 48.89f, 0.0f }, { -282.0f, -525.0f, 48.82f, 0.0f },
                    { -282.0f, -522.0f, 48.75f, 0.0f }, { -282.0f, -519.0f, 48.73f, 0.0f },
                    { -282.0f, -516.0f, 49.01f, 0.0f }, { -282.0f, -513.0f, 49.29f, 0.0f },
                    { -279.0f, -531.0f, 49.27f, 0.0f }, { -279.0f, -528.0f, 49.11f, 0.0f },
                    { -279.0f, -519.0f, 49.24f, 0.0f }, { -279.0f, -516.0f, 49.52f, 0.0f },
                    { -276.0f, -522.0f, 49.91f, 0.0f }, { -276.0f, -519.0f, 49.73f, 0.0f },
                    { -276.0f, -516.0f, 49.49f, 0.0f }, { -273.0f, -522.0f, 49.95f, 0.0f }
                },
            },
            { "iron_clad", 16397, 16398, 5397,
                {
                    { -114.0f, -654.0f, 7.48f, 0.0f }, { -111.0f, -654.0f, 7.19f, 0.0f },
                    { -111.0f, -645.0f, 8.86f, 0.0f }, { -108.0f, -654.0f, 7.42f, 0.0f },
                    { -108.0f, -651.0f, 7.01f, 0.0f }, { -108.0f, -648.0f, 7.51f, 0.0f },
                    { -108.0f, -645.0f, 8.04f, 0.0f }, { -105.0f, -657.0f, 7.42f, 0.0f },
                    { -105.0f, -654.0f, 7.42f, 0.0f }, { -105.0f, -651.0f, 6.81f, 0.0f },
                    { -105.0f, -648.0f, 7.00f, 0.0f }, { -105.0f, -645.0f, 7.76f, 0.0f },
                    { -102.0f, -660.0f, 7.42f, 0.0f }, { -102.0f, -657.0f, 7.42f, 0.0f },
                    { -102.0f, -654.0f, 7.42f, 0.0f }, { -102.0f, -651.0f, 6.90f, 0.0f },
                    { -102.0f, -648.0f, 6.95f, 0.0f }, { -99.0f, -660.0f, 7.42f, 0.0f },
                    { -99.0f, -657.0f, 7.42f, 0.0f }, { -99.0f, -654.0f, 7.42f, 0.0f },
                    { -99.0f, -651.0f, 6.96f, 0.0f }
                },
                {
                    { -105.0f, -684.0f, 7.43f, 0.0f }, { -102.0f, -693.0f, 8.36f, 0.0f },
                    { -102.0f, -690.0f, 8.07f, 0.0f }, { -102.0f, -684.0f, 7.42f, 0.0f },
                    { -99.0f, -693.0f, 8.31f, 0.0f }, { -99.0f, -690.0f, 8.09f, 0.0f },
                    { -99.0f, -687.0f, 7.88f, 0.0f }, { -99.0f, -684.0f, 7.43f, 0.0f },
                    { -99.0f, -678.0f, 7.42f, 0.0f }, { -96.0f, -693.0f, 8.32f, 0.0f },
                    { -96.0f, -690.0f, 8.11f, 0.0f }, { -96.0f, -687.0f, 7.90f, 0.0f },
                    { -96.0f, -684.0f, 7.43f, 0.0f }, { -96.0f, -681.0f, 7.42f, 0.0f },
                    { -96.0f, -678.0f, 7.42f, 0.0f }, { -93.0f, -693.0f, 8.33f, 0.0f },
                    { -93.0f, -690.0f, 8.12f, 0.0f }, { -93.0f, -687.0f, 7.92f, 0.0f },
                    { -93.0f, -684.0f, 7.43f, 0.0f }, { -93.0f, -681.0f, 7.42f, 0.0f },
                    { -90.0f, -690.0f, 8.13f, 0.0f }, { -90.0f, -681.0f, 7.42f, 0.0f }
                },
            },
        };
    }

    /// Where the interact stage's seats are first put, before an episode's site is drawn (the instance is cleared
    /// round it, InteractEncounter puts the seat at its site): the Foundry Door's near side, mid-dungeon.
    Position DeadminesMiddle()
    {
        return { -150.0f, -576.0f, 19.32f, 3.14f };
    }

    /// **Ragefire Chasm's entrance** (the combat stages, C1-C3): where its areatrigger (2230, from Orgrimmar's Cleft of
    /// Shadow) puts a player, and where a seat that died there comes back (dungeon-curriculum I4).
    Position RagefireEntrance()
    {
        return { 3.81f, -14.82f, -17.84f, 4.39f };
    }

    /// **The dungeon stages' blocks** (dungeon-curriculum G2, D1-D3): G1's -- the camera, its entities, the mental map,
    /// the sight list with the combat block's nameplate columns and the player, pet and target frames, the party frames
    /// (PartyFrames revision 2: the one source of the members' state, their minimap dots and targets), duel, pet and
    /// gauntlet (eat, drink, rest) -- and a whole dungeon's: the pack block (whose slots, in a sight stage, are what
    /// the seat saw: StageScenario::ViewSeat). No crowd block: its pack ahead, its overflow and the nearest object are
    /// read off the server's lists within a radius, through walls -- the sight list names what a player sees, and its
    /// interact and use-item presses open the doors and fire the cannon. No party or support block: the party frames
    /// are the members' one source, and a heal goes as the client sends it.
    std::vector<BlockId> DungeonBlocks()
    {
        using enum BlockId;
        return { Core, Move, Duel, Pet, Pack, Gauntlet, Vision, Entities, Map, Sight, PartyFrames, Combat, Goal };
    }

    /// The "human" stand-in's share of the dungeon stages' training episodes (I7, from G2 on), percent: a fifth of
    /// the runs have it in one seat, leading or following, in any role.
    constexpr int32 DUNGEON_STAND_IN_SHARE = 20;

    /// Every stage, every base before the stages that extend it.
    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        // M1 -- controls, seen through the camera (the user's design, 2026-10-05; redesigned 2026-10-06, perception-
        // goals REDESIGN §1): straight lines and stopping on a point, at every race and class at level 1 (a death
        // knight at its 55: a level 1 kit is one or two spells, so the lesson is the movement alone), on the camera,
        // so M2 begins with a camera that already works.
        //
        // The same empty Stockades. Each episode the seat stands at a random point of the hallways (StockadeHallways,
        // the entrance among them), facing a random way, and one real object of M2's pool (SeekObjects: the class and
        // flag M2 shows) stands at another, 10 to 120 yd off, in sight of the seat's eye; later rungs put some of them
        // just round a corner (SightEncounter). Reach it as fast as possible and stop beside it: the straight line is
        // the way, the time cost (Markers.StepCost each decision until the stop) the price of anything else, and
        // Arrive is paid once, on the stop.
        //
        // The compass (perception-goals P1: the objective's bearing and distance) is there at the first rung, and is
        // withheld for more and more of the episodes as the shaping fade steps (0, 0.25, 0.6, 0.9): by the top rung
        // the seat goes to what it sees. Wall and Stuck are charged from the first step, at a small fixed price.
        //
        // Core and the goal block are the layout's frame; Move the lesson, the compass and the camera its two ways of
        // knowing where. Nothing to fight, so no duel block. The evaluation plays a fixed set of (spawn, object) pairs
        // over the hallways (StockadeSightPairs), each with the compass and without.
        stages.push_back({
            .Name = "move1_controls",
            .Suffix = "_controls",
            .Extends = "",
            .Summary = "an empty Stockades: from a random hallway point to a real object in sight, as fast as "
                "possible, and stop beside it; the compass withheld more often each rung",
            .Blocks = { Core, Move, Compass, Vision, Goal },
            .Arenas = {
                { .Name = "hallway", .Weight = 1, .Against = Opposition::Sight, .EpisodeSeconds = 60,
                    .SpawnPoints = StockadeHallways(), .MapId = MAP_STORMWIND_STOCKADE, .Objects = SeekObjects(),
                    .SightPairs = StockadeSightPairs() },
            },
            .MapId = MAP_STORMWIND_STOCKADE,
            .SpawnPoints = { StockadeEntrance() },
            .Level = 1,
        });

        // M2 -- seek (the user, 2026-10-06: "M2 seek also happens inside the same Stockades instance. It has to find
        // an object in a room, and that object and room is randomized."; perception-goals plan §4, REDESIGN §2). The
        // same empty Stockades; each episode the seat stands at a random point of the hallways (StockadeHallways, the
        // entrance among them, as M1), facing a random way, and one real object -- a chest, crate, barrel, sack or
        // strongbox -- stands where the placement ladder says. No compass: the camera's objective flag shows it only
        // in line of sight, standing in for a quest object's glow. Found is stopping within three yards of it
        // (interaction range). Its memory is the GRU's and its mental map's (the map block, REDESIGN §3).
        //
        // **The ladder** (SeekDraw::Rung, the shaping fade's rungs): in the hallway in sight of the spawn, just inside
        // a front cell's opening, anywhere in a front cell, then deep (the back rooms, hubs and end rooms), each rung
        // keeping a tenth of the one below; episodes of 90, 120, 200 and 300 s by rung (Seek.RungSeconds*). An
        // evaluation plays 78 episodes at the training rung (each of its rooms in turn, the objects cycled); the held
        // out "sweep" arena is every (room, object) pair once, 39 x 5 = 195 episodes at the top rung, for the stage's
        // end (eval.heldout).
        //
        // **The deep rung's clock, 300 s**: a greedy sweep from the entrance through every room's centre walks 1,595
        // yd, 228 s at run speed (the scan's Dijkstra distances, nearest unvisited room next); a seat that sees into a
        // room from its door needs less, one that backtracks more. 300 s is 1.3 times the sweep, and is the episode
        // clock's own scale (EPISODE_TIME_SCALE_MS), so the clock feature never saturates.
        stages.push_back({
            .Name = "move2_seek",
            .Suffix = "_seek",
            .Extends = "move1_controls",
            .Summary = "the same empty Stockades: one object in its hallways or one of its 39 rooms, found by sight "
                "with no compass and stopped beside, deeper each rung",
            .Blocks = { Core, Move, Vision, Map, Goal },
            .Arenas = {
                { .Name = "rooms", .Weight = 1, .Against = Opposition::Seek, .EpisodeSeconds = 300,
                    .SpawnPoints = StockadeHallways(), .MapId = MAP_STORMWIND_STOCKADE,
                    .Rooms = StockadeRooms(), .Objects = SeekObjects(), .SeekRadius = 3.0f },
                { .Name = "sweep", .Weight = 1, .Against = Opposition::Seek, .EvalOnly = true, .EpisodeSeconds = 300,
                    .SpawnPoints = StockadeHallways(), .MapId = MAP_STORMWIND_STOCKADE,
                    .Rooms = StockadeRooms(), .Objects = SeekObjects(), .SeekRadius = 3.0f },
            },
            .MapId = MAP_STORMWIND_STOCKADE,
            .SpawnPoints = { StockadeEntrance() },
            .Level = 1,
        });

        // M3 -- interact (dungeon-curriculum M3): in an empty Deadmines, at the Deadmines band (17-20, every race and
        // class; a death knight at its own minimum), telling objects apart and using the dungeon's own levers, doors and
        // locks -- through the sight block's entity presses alone (I1), as a client sends them. No looting. The goal
        // names the object by its kind (the sight block's named row: class and template entry, and the task), never its
        // place; no compass and no objective flag. Its memory is the GRU's, the mental map's and entity memory's (I2).
        //
        // **The ladder** (InteractDraw::Rung, the shaping fade's rungs 1, 0.5, 0), each episode at one of the four sites
        // (DeadminesSites): distinguish -- the named object among two to four decoys of other kinds of the seek pool,
        // all in sight of the seat; switch -- the named object behind its shut door, the lever on the seat's side
        // opens it; key -- the cannon, named, opened only by the gunpowder the seat carries from the start (used on it,
        // its script blows the Iron Clad Door). Each rung keeps a tenth of the one below; episodes of 60, 120 and 90 s.
        // An evaluation plays the training rung with its seed's site, spawn, spots and kinds; the held-out "sweep"
        // plays every rung in turn.
        //
        // Paid: Arrive (Outcome) for the right object reached or its lock given the key; DoorOpened (Outcome) for the
        // switch rung's door opened by the seat's own lever press; WrongObject (Cost) for a decoy taken, each once; a
        // refused press is the sight block's price (a locked door pressed, the key on the wrong thing); Stuck, Wall
        // and the step cost as M2's; Sighting (Shaping) once, the first frame that lists the named object.
        stages.push_back({
            .Name = "move3_interact",
            .Suffix = "_interact",
            .Extends = "move2_seek",
            .Summary = "an empty Deadmines: the object the goal names among decoys, behind a door its lever opens, or "
                "the cannon fired with the gunpowder carried from the start -- all through the sight block's presses",
            .Blocks = { Core, Move, Vision, Map, Sight, Goal },
            .Arenas = {
                { .Name = "sites", .Weight = 1, .Against = Opposition::Interact, .EpisodeSeconds = 120,
                    .MapId = MAP_DEADMINES, .Objects = SeekObjects(), .SeekRadius = 3.0f,
                    .Sites = DeadminesSites() },
                { .Name = "sweep", .Weight = 1, .Against = Opposition::Interact, .EvalOnly = true,
                    .EpisodeSeconds = 120, .MapId = MAP_DEADMINES, .Objects = SeekObjects(), .SeekRadius = 3.0f,
                    .Sites = DeadminesSites() },
            },
            .MapId = MAP_DEADMINES,
            .SpawnPoints = { DeadminesMiddle() },
            .MinLevel = 17,
            .FocusLevelFirst = 17,
            .FocusLevelLast = 20,
            .FocusChance = 100,
        });

        // M4 -- follow (dungeon-curriculum Part 2, I5): a party of five -- a leader and four learned followers -- in an
        // empty Ragefire Chasm or Deadmines (cleared whole, as the Stockades: SpawnArea::ClearMap, and their doors,
        // levers and chests removed: ObjectPool::ClearOwn), at the dungeon's level band (the dungeon finder's). The
        // leader walks the dungeon's route from the door through each boss's place in turn, stopping at each; the
        // followers keep 3-10 yd from it, out of its way, through its doors and drops, waiting when it stops and
        // regrouping after (PartyFollowEncounter). Deaths are rare here, but a follower that dies rises at the
        // entrance after Respawn.DelayMs and walks back (I4, EntranceRespawn): the episode never ends on one.
        //
        // **The ladder** (the shaping fade's rungs; the stage has no shaping, so the fade is the ladder alone): a
        // slow, steady leader (walking, long stops), then running, then stopping unannounced, then stopping
        // unannounced more often and sometimes stepping back first.
        //
        // Core and the goal block are the frame; Move the lesson; the camera (with its entity list) and the mental
        // map how the leader is seen and the way remembered; the party frames block what a player's UI shows of the
        // party -- the frames, always, and the minimap's dots within its radius. No compass: the leader is not an
        // objective point. The evaluation plays both dungeons' fixed routes from seeded episodes at the training rung.
        stages.push_back({
            .Name = "move4_follow",
            .Suffix = "_follow",
            .Extends = "move2_seek",
            .Summary = "an empty Ragefire Chasm or Deadmines: four followers keep with a leader walking the dungeon's "
                "route from the door, through its doors and drops, waiting and regrouping when it stops; a death "
                "rises at the entrance and walks back",
            .Blocks = { Core, Move, Vision, Map, PartyFrames, Goal },
            .Arenas = {
                { .Name = "ragefire", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::PartyFollow,
                    .PartySize = GROUP_MEMBERS, .EpisodeSeconds = 300, .MapId = MAP_RAGEFIRE_CHASM },
                { .Name = "deadmines", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::PartyFollow,
                    .PartySize = GROUP_MEMBERS, .EpisodeSeconds = 300, .MapId = MAP_DEADMINES },
            },
            .MapId = MAP_RAGEFIRE_CHASM,
        });

        // **The combat stages** (dungeon-curriculum C1-C3, approved 2026-10-06): fighting on a cleared Ragefire Chasm
        // (map 389, every creature removed on the env's first build: SpawnArea::ClearMap), every race and class at the
        // dungeon's level band, 13-18 (a death knight at its own 55, against creatures of its level), one seat. The
        // seat starts at one of the dungeon's own creature spawn points, its corridors, within a walk of the entrance
        // (CombatEncounter::Corridors), and what it fights is spawned there for it (CombatEncounter, CombatDraw::
        // PlanPull at its class and build's rung).
        //
        // **What it perceives** (I3) is what a player does: the camera, the entities it shows and the ones it remembers
        // (the vision, entities, map and sight blocks), each visible one's cast bar, crowd control and threat (the
        // combat block's per-target columns on the sight list), the player and pet frames, and the target frame's
        // threat (the combat block, revision 1; a party's members are the party frames block's). **What
        // it does**: the move block's keys and mouse (controller-only), selecting by sight (the sight block's presses)
        // and casting at the selection as the client does; nothing situational masked. A death is never the end: it
        // comes back alive at the entrance after a short delay and walks back (I4, ArenaDefinition::RespawnAtEntrance).
        //
        // Seeded from move3_interact (the plan's base): the camera, the map, the movement and the sight list carry --
        // its slots widened by the combat columns, which start at zero, and its named row (revision 2), present and 0
        // here where nothing is named; the duel, pet and combat blocks start fresh.
        //
        // C1 -- fight: one creature at a time, the next two seconds after each kill, over a 150 s episode; a caster
        // from rung 1, an elite from rung 4. `guard` stands a passive friend by the seat that every creature goes for
        // first: taunting it off and healing it are the drill, for the classes that can. Outcome: kills, survived;
        // Cost: damage taken (small), the time a kill takes, a death.
        stages.push_back({
            .Name = "combat1_fight",
            .Suffix = "_fight",
            .Extends = "move3_interact",
            .Summary = "a cleared Ragefire Chasm: one creature at a time, found by sight and killed with the class's "
                "kit; in a share of them a friend to taunt off and heal",
            .Blocks = { Core, Move, Duel, Pet, Vision, Entities, Map, Sight, Combat, Goal },
            .Arenas = {
                { .Name = "fight", .Weight = 3, .Against = Opposition::Combat, .EpisodeSeconds = 150,
                    .Combat = CombatDrill::Fight, .RespawnAtEntrance = true },
                { .Name = "guard", .Weight = 1, .Against = Opposition::Combat, .EpisodeSeconds = 150,
                    .Combat = CombatDrill::Fight, .Ally = true, .RespawnAtEntrance = true },
            },
            .MapId = MAP_RAGEFIRE_CHASM,
            .SpawnPoints = { RagefireEntrance() },
            .FocusLevelFirst = 13,
            .FocusLevelLast = 18,
            .FocusChance = 100,
        });

        // C2 -- packs: packs of two to four on the same ground -- a caster from rung 1, linked from rung 2, fire
        // underfoot in a third of them (always in `fire`) -- with the next pack standing further on: pulling it before
        // this one is down is an extra pull. Focus, interrupts, crowd control, line of sight, out of the fire, one pack
        // at a time. Outcome: packs cleared, interrupts landed, survived; Cost: extra pulls, fire damage, damage taken.
        stages.push_back({
            .Name = "combat2_packs",
            .Suffix = "_packs",
            .Extends = "combat1_fight",
            .Summary = "a cleared Ragefire Chasm: packs of 2-4, casters, linked, fire underfoot, the next pack further "
                "on; clear them one at a time",
            .Blocks = { Core, Move, Duel, Pet, Vision, Entities, Map, Sight, Combat, Goal },
            .Arenas = {
                { .Name = "packs", .Weight = 2, .Against = Opposition::Combat, .EpisodeSeconds = 240,
                    .Combat = CombatDrill::Packs, .RespawnAtEntrance = true },
                { .Name = "fire", .Weight = 1, .Against = Opposition::Combat, .EpisodeSeconds = 240, .Hazards = true,
                    .Combat = CombatDrill::Packs, .RespawnAtEntrance = true },
            },
            .MapId = MAP_RAGEFIRE_CHASM,
            .SpawnPoints = { RagefireEntrance() },
            .FocusLevelFirst = 13,
            .FocusLevelLast = 18,
            .FocusChance = 100,
        });

        // C3 -- survive: packs that can kill (three or four, two levels up on the rung's), pull after pull, the next
        // waiting until the seat goes for it; food and drink stocked (the gauntlet block: eat, drink, rest until
        // ready), so resting between pulls and backing off are the seat's own choices. A death comes back at the
        // entrance and walks back, and the pack is still there to clear: never "give up". Outcome: survived (its
        // purpose), packs cleared, interrupts landed; every second dead or away from the fight a Cost (Away).
        stages.push_back({
            .Name = "combat3_survive",
            .Suffix = "_survive",
            .Extends = "combat2_packs",
            .Summary = "a cleared Ragefire Chasm: packs that can kill, pull after pull; rest between them, back off, "
                "and after a death come back from the entrance and finish them",
            .Blocks = { Core, Move, Duel, Pet, Gauntlet, Vision, Entities, Map, Sight, Combat, Goal },
            .Arenas = {
                { .Name = "survive", .Weight = 1, .Against = Opposition::Combat, .EpisodeSeconds = 360,
                    .Combat = CombatDrill::Survive, .RespawnAtEntrance = true },
            },
            .MapId = MAP_RAGEFIRE_CHASM,
            .SpawnPoints = { RagefireEntrance() },
            .FocusLevelFirst = 13,
            .FocusLevelLast = 18,
            .FocusChance = 100,
        });

        // **The party stages** (dungeon-curriculum G1-): parties of five on real dungeon ground, perceived as the
        // combat stages perceive (the camera, the sight list, the target frame's threat) with the party frames
        // (PartyFrames revision 2: every member's state, its minimap dot and its target -- the one source of it).
        //
        // G1 -- roles (the archived curriculum's stage6 drills, on the combat stages' cleared Ragefire Chasm at its
        // band, 13-18, every race and class): one role drilled an episode, the drilled role in seat 0 -- its class and
        // build drawn among those whose spec plays it (StageScenario's DrillRole makeup: a role is read off the build,
        // and every class drills every role its builds can) -- and a proper party round it, learned seats, partners
        // from the I7 pool (never seat 0) and the "human" stand-in in a share of the episodes. Pack after pack for the
        // episode's clock (RolesEncounter):
        // - tank_hold: the tank holds every enemy (DrillHold);
        // - heal_keep: the healer keeps everyone up through packs of twice their health, longer than its mana bar
        //   (DrillKeep);
        // - damage_discipline: a damage dealer kills the tank's target without taking an enemy off it (DrillFocus);
        // - pull: the tank pulls one pack of a camp at a time, the packs closer each rung (PullClean, PullExtra).
        // Each drill's lesson is the drilled seat's own Outcome (the old curriculum held its drills only once they
        // were), tier-scaled; Clear and Survived every seat's; Death, Away and the clock at full price. A death comes
        // back alive at the entrance and walks back to the party (I4); the episode ends on its clock alone.
        //
        // Seeded from combat3_survive (the seed chain), with move4_follow's party frames merged in by name (the
        // overseer's ruling: bootstrap.seed_merges, the second source).
        stages.push_back({
            .Name = "group1_roles",
            .Suffix = "_roles",
            .Extends = "combat3_survive",
            .Merges = { "move4_follow" },
            .Summary = "a party of five on a cleared Ragefire Chasm drilling one role an episode: the tank holds every "
                "enemy, the healer keeps everyone up within its mana, a damage dealer kills the tank's target without "
                "pulling it, the tank pulls one pack at a time",
            .Blocks = { Core, Move, Duel, Pet, Gauntlet, Vision, Entities, Map, Sight, PartyFrames, Combat, Goal },
            .Arenas = {
                { .Name = "tank_hold", .Weight = 2, .Seats = SeatPlan::Party, .Against = Opposition::Roles,
                    .PartyGroup = true, .EpisodeSeconds = 240, .ProperParty = true,
                    .DrillRole = ROLE_TANK, .Roles = RolesDrill::Hold, .RespawnAtEntrance = true },
                { .Name = "heal_keep", .Weight = 2, .Seats = SeatPlan::Party, .Against = Opposition::Roles,
                    .PartyGroup = true, .EpisodeSeconds = 300, .ProperParty = true,
                    .DrillRole = ROLE_HEALER, .Roles = RolesDrill::Keep, .RespawnAtEntrance = true },
                { .Name = "damage_discipline", .Weight = 2, .Seats = SeatPlan::Party, .Against = Opposition::Roles,
                    .PartyGroup = true, .EpisodeSeconds = 240, .ProperParty = true,
                    .DrillRole = ROLE_DAMAGE, .Roles = RolesDrill::Focus, .RespawnAtEntrance = true },
                { .Name = "pull", .Weight = 2, .Seats = SeatPlan::Party, .Against = Opposition::Roles,
                    .PartyGroup = true, .EpisodeSeconds = 360, .ProperParty = true,
                    .DrillRole = ROLE_TANK, .Roles = RolesDrill::Pull, .RespawnAtEntrance = true },
            },
            .MapId = MAP_RAGEFIRE_CHASM,
            .SpawnPoints = { RagefireEntrance() },
            .FocusLevelFirst = 13,
            .FocusLevelLast = 18,
            .FocusChance = 100,
        });

        // **The party stages** (dungeon-curriculum D2, D3, 2026-10-07): five seats on a real dungeon's own ground, its
        // packs and patrols where the world database stands them (InstanceEncounter, InstanceLadder::Wing; a fresh
        // instance every run), every race and class at the dungeon's levels, seeing what a player sees (the camera, the
        // sight list, the party and target frames) and moving on the player controller. Nothing ends at a death: a
        // seat that dies is alive again at the entrance after Respawn.DelayMs and walks back (I4); a wipe is scored,
        // and only the run's last allowed wipe (Instance.WingWipes, and the ladder's spare ones) ends it. No looting:
        // what a lock takes (the Deadmines' gunpowder) is carried from the door. The "human" stand-in plays a fifth of
        // every stage's training runs, leading or following, in any role (I7; ArenaDefinition::StandInShare), and the
        // learner's with_human arm measures the party with it beside the party without (H). Nothing tells the seats
        // the way: no route, waypoint or path hint exists for them or for the rewards (decision 0019). The corridor
        // (group2_corridor) and the pull drill (dungeon1_pulls) were retired with the field route they stood on.
        //
        // D2 -- Ragefire: the door to Bazzalan, a full clear -- every pull and side boss, deaths and rejoins in play,
        // the levels and spare wipes stepping down the whole dungeon's ladder. Outcome, tier-scaled by the ladder's
        // rung: the bosses (Kill: Instance.WingMidBoss, WingBoss), the full clear (Clear: Instance.WingClear), the
        // kills, a pull started ready; Cost: wipes (the second ends the run), deaths, standing about, straying, the
        // clock (StepCost: Instance.WingClock; Timeout: the share of the dungeon left).
        //
        // **Held out from here on: Wailing Caverns** (the WING ladder's row 2, door to Lord Serpentis), the arena
        // `heldout` -- never drawn in training (EvalOnly: ArenaDrawWeights), played only by the learner's
        // eval.heldout, so a party that learned to run dungeons is told from one that learned these routes.
        stages.push_back({
            .Name = "dungeon2_ragefire",
            .Suffix = "_ragefire",
            .Extends = "group1_roles",
            .Summary = "Ragefire Chasm from the door to Bazzalan: a full clear, every pull and side boss, the ladder "
                "stepping down to its own levels; Wailing Caverns held out",
            .Blocks = DungeonBlocks(),
            .Arenas = {
                { .Name = "dungeon", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Wing, .InstanceRow = 0, .EpisodeSeconds = 7200,
                    .StandInShare = DUNGEON_STAND_IN_SHARE },
                { .Name = "heldout", .Weight = 0, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Wing, .InstanceRow = 2, .EvalOnly = true,
                    .EpisodeSeconds = 10800 },
            },
        });

        // D3 -- the Deadmines: the door to VanCleef at levels 17-20 (the bar's band, LevelFirst/LevelLast), its doors
        // and levers by a real interact and the Iron Clad Door by the cannon and the gunpowder carried from the door
        // (the sight block's presses, through the session's handlers; nothing opens by itself), every side boss. **The
        // bar: 70% or more of the evaluation's runs cleared with at most one wipe** (the second wipe ends a run), with
        // no help (an evaluation is at the ladder's last rung). Measured per boss (boss_* columns) and by role (deaths_tank,
        // _healer, _damage: the seat's place in the party, read off its build). Wailing Caverns held out, as D2.
        stages.push_back({
            .Name = "dungeon3_deadmines",
            .Suffix = "_deadmines",
            .Extends = "dungeon2_ragefire",
            .Summary = "the Deadmines from the door to VanCleef at 17-20: doors, levers and the cannon by real "
                "presses, every side boss; the bar is 70% clears with at most one wipe; Wailing Caverns held out",
            .Blocks = DungeonBlocks(),
            .Arenas = {
                { .Name = "dungeon", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Wing, .InstanceRow = 1, .EpisodeSeconds = 14400,
                    .StandInShare = DUNGEON_STAND_IN_SHARE, .LevelFirst = 17, .LevelLast = 20 },
                { .Name = "heldout", .Weight = 0, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Wing, .InstanceRow = 2, .EvalOnly = true,
                    .EpisodeSeconds = 10800 },
            },
        });

        return stages;
    }

    /// Why `arena` cannot be played with `stage`'s blocks, or empty.
    std::string ArenaProblem(StageDefinition const& stage, ArenaDefinition const& arena)
    {
        bool const instance = arena.Against == Opposition::Instance;
        if (instance != (arena.Instance != InstanceLadder::None))
            return "an instance ladder goes with fighting in an instance, and only with that";
        if (instance && (arena.InstanceRow < 0 || std::size_t(arena.InstanceRow)
            >= InstanceLadderRows(arena.Instance).size()))
            return "an instance arena runs a row of its ladder";
        if (instance && !stage.Has(BlockId::Pack))
            return "an instance needs the pack block";
        // The seat picks its own target from what it sees (StageScenario::CurrentTarget): no encounter selects one.
        if (instance && !stage.Has(BlockId::Sight))
            return "an instance needs the sight block: the seat selects its own target";
        if (instance && arena.Seats != SeatPlan::Party)
            return "an instance is fought by a party";
        // Its members are read through the party frames.
        if (arena.PartyGroup && !stage.Has(BlockId::PartyFrames))
            return "a party group needs the party frames block";
        // A group running a dungeon: five learned seats.
        bool const dungeonGroup = arena.Against == Opposition::Instance && arena.Seats == SeatPlan::Party;
        // And a proper party drilling on a dungeon's ground (the roles stage, G1: Opposition::Roles).
        bool const roles = arena.Against == Opposition::Roles;
        bool const drillGroup = arena.ProperParty && roles && arena.Seats == SeatPlan::Party;
        if (arena.PartyGroup && !dungeonGroup && !drillGroup)
            return "a party group is a dungeon's or a drill's";
        if (arena.ProperParty && !(drillGroup && arena.PartyGroup))
            return "a proper party is drawn for a party drill on a dungeon's ground (a whole dungeon draws its own)";
        if (arena.DrillRole > DRILL_DAMAGE || (arena.DrillRole && !arena.ProperParty))
            return "a drilled role (1 tank, 2 healer, 3 damage) is a proper party's";
        // The drilled seat is paid its lesson by RolesEncounter alone (PartyEncounter pays no drill weighting).
        if (arena.DrillRole && !roles)
            return "a drilled role is a roles arena's";
        if (arena.InstanceRow >= 0 && !instance)
            return "only an instance arena pins a row of its ladder";
        // The dungeon curriculum's party stages (D2, D3): the level band and the stand-in's share.
        bool const wing = arena.Instance == InstanceLadder::Wing;
        if ((arena.LevelFirst || arena.LevelLast) && !wing)
            return "a level band is a dungeon wing's (InstanceLadder::Wing)";
        if (arena.LevelFirst > arena.LevelLast || (arena.LevelFirst == 0) != (arena.LevelLast == 0))
            return "a level band is its first and last level, in order";
        if (arena.StandInShare > 100 || (arena.StandInShare > 0 && arena.Seats != SeatPlan::Party))
            return "the stand-in's share is a percentage of a party's training episodes";
        // The seek stage: one seat in a dungeon of rooms, an object to find in one of them, nothing to fight; it finds
        // the object by sight, so it carries the camera and not the compass.
        bool const seek = arena.Against == Opposition::Seek;
        if (seek && (arena.Seats != SeatPlan::Solo))
            return "a seek arena is one seat on its own";
        if (seek && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Vision)))
            return "a seek arena is walked with the move block and searched with the camera (the vision block)";
        if (seek && stage.Has(BlockId::Compass))
            return "a seek arena's object is found by sight: the compass would point at it";
        if (seek && (arena.Rooms.empty() || arena.Objects.empty() || arena.SeekRadius <= 0.0f))
            return "a seek arena needs rooms to hide its object in, objects to hide and a radius to find one within";
        // M1's object in the hallways: one seat, on foot and dry, an object of the pool put down at one of the arena's
        // hallway points (its SpawnPoints, which it spawns at too); walked to with the move block, pointed at by the
        // compass (withheld more often each rung) and seen with the camera.
        bool const sight = arena.Against == Opposition::Sight;
        if (sight && (arena.Seats != SeatPlan::Solo || !arena.Rooms.empty()))
            return "a sight arena is one seat on its own, with no room of its own";
        if (sight && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Compass) || !stage.Has(BlockId::Vision)))
            return "a sight arena is walked with the move block, pointed at by the compass and seen with the camera";
        if (sight && (arena.Objects.empty() || arena.SpawnPoints.size() < 2))
            return "a sight arena needs objects to put down and hallway points to spawn at and put them on";
        for (SightPair const& pair : arena.SightPairs)
            if (!sight || pair.Spawn >= arena.SpawnPoints.size() || pair.Object >= arena.SpawnPoints.size()
                || pair.Spawn == pair.Object)
                return "a sight pair is a sight arena's: two different points of its SpawnPoints";

        // M3's sites: one seat on its own, on foot and dry, acting on what it sees through the sight block; the goal
        // names what it is after, so nothing points at it -- no compass, no marker of its own.
        bool const interact = arena.Against == Opposition::Interact;
        if (interact && (arena.Seats != SeatPlan::Solo || !arena.Rooms.empty()))
            return "an interact arena is one seat on its own, with no room of its own";
        if (interact && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Vision) || !stage.Has(BlockId::Sight)))
            return "an interact arena is walked with the move block, seen with the camera and acted on with the sight "
                "block";
        if (interact && stage.Has(BlockId::Compass))
            return "an interact arena's object is found by what the goal names: the compass would point at it";
        if (interact && (arena.Sites.empty() || arena.Objects.size() < 3 || arena.SeekRadius <= 0.0f
            || !arena.MapId))
            return "an interact arena needs its map, sites, at least three kinds of object (one named, two decoys) "
                "and a radius to reach one within";
        for (InteractSite const& site : arena.Sites)
            if (!interact || site.Name.empty() || !site.Door || !site.Opener || site.Near.empty()
                || (!site.Key && site.Far.empty()))
                return "an interact site is an interact arena's: named, a door and its opener, its near side, and the "
                    "far side a lever's door shuts off";
        if (interact && (std::none_of(arena.Sites.begin(), arena.Sites.end(),
            [](InteractSite const& site) { return site.Key == 0; })
            || std::none_of(arena.Sites.begin(), arena.Sites.end(),
            [](InteractSite const& site) { return site.Key != 0; })))
            return "an interact arena's ladder needs a site a lever opens and one a key item opens";
        // The party follow (M4): a party of learned followers and a leader in the owner's slot, in a dungeon of its
        // own (its door is the spawn, its bosses' places the route), nothing to fight; the leader is found by sight,
        // the minimap and memory, so the camera and the party frames, and never the compass.
        bool const partyFollow = arena.Against == Opposition::PartyFollow;
        if (partyFollow && (arena.Seats != SeatPlan::Party || !arena.PartySize || arena.PartySize > GROUP_MEMBERS
            || arena.PartyGroup || !arena.MapId))
            return "a party follow is a party of 1 to GROUP_MEMBERS followers and its leader, on a dungeon's map, "
                "with nothing to fight";
        if (partyFollow && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Vision)
            || !stage.Has(BlockId::PartyFrames)))
            return "a party follow is walked with the move block and the leader found with the camera and the party "
                "frames";
        if (partyFollow && stage.Has(BlockId::Compass))
            return "a party follow's leader is no objective point: the compass would point at it";
        if (arena.PartySize && (arena.Seats != SeatPlan::Party || arena.PartySize > GROUP_SEATS))
            return "a party size is a party's, 1 to GROUP_SEATS";

        // The combat stages: one seat on a dungeon's own ground, seeing what it fights (the sight block and the combat
        // block's columns on it) and coming back at the entrance after a death.
        // The roles stage (G1, RolesEncounter): a proper party of five, grouped, on a dungeon's own ground, drilling
        // the role its drill is about in seat 0, by sight with the party frames, its dead back at the entrance.
        if (roles != (arena.Roles != RolesDrill::None))
            return "a roles drill goes with a party drilling on a cleared dungeon (Opposition::Roles), and only with "
                "that";
        if (roles && (arena.Seats != SeatPlan::Party || arena.PartySize || !arena.ProperParty || !arena.PartyGroup
            || !arena.RespawnAtEntrance
            || arena.DrillRole != DrilledRole(arena.Roles)))
            return "a roles arena is a proper party of five in a core group, its drilled "
                "role the drill's, its dead back at the entrance";
        if (roles && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Vision) || !stage.Has(BlockId::Sight)
            || !stage.Has(BlockId::Combat) || !stage.Has(BlockId::PartyFrames) || !stage.Has(BlockId::Gauntlet)))
            return "a roles arena is fought by sight with the party frames: the move, vision, sight, combat, party "
                "frames and gauntlet blocks";
        if (roles && (arena.MapId ? arena.MapId : stage.MapId) == 0)
            return "a roles arena is on a dungeon's own map";
        bool const combat = arena.Against == Opposition::Combat;
        if (combat != (arena.Combat != CombatDrill::None))
            return "a combat drill goes with fighting on a cleared dungeon (Opposition::Combat), and only with that";
        if (combat && arena.Seats != SeatPlan::Solo)
            return "a combat arena is one seat on its own";
        if (combat && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Vision) || !stage.Has(BlockId::Sight)
            || !stage.Has(BlockId::Combat)))
            return "a combat arena is fought by sight: the move, vision, sight and combat blocks";
        if (combat && arena.Combat == CombatDrill::Survive && !stage.Has(BlockId::Gauntlet))
            return "surviving pull after pull rests between them with the gauntlet block";
        if (arena.Ally && (!combat || arena.Combat != CombatDrill::Fight))
            return "a friend to guard is a single fight's (CombatDrill::Fight)";
        if (combat && (arena.MapId ? arena.MapId : stage.MapId) == 0)
            return "a combat arena is on a dungeon's own map";
        if (arena.RespawnAtEntrance && (arena.MapId ? arena.MapId : stage.MapId) == 0)
            return "a seat comes back at an instance's entrance, so the arena is on an instance's map";

        if (!seek && !arena.Rooms.empty())
            return "only a seek arena has rooms";
        if (!seek && !sight && !interact && !arena.Objects.empty())
            return "only a seek, a sight or an interact arena has objects";
        for (SeekRoom const& room : arena.Rooms)
            if (room.Floor.size() < 3 || room.Name.empty())
                return "a seek room is named and its floor is a polygon";

        // An arena on a map of its own stands on its own ground: the stage's points are on the stage's map. An
        // encounter that finds its own spawn (an instance's door, a quest giver, a node field, an inn) needs none.
        bool const ownSpawn = instance || arena.Against == Opposition::PartyFollow;
        if (arena.MapId && arena.MapId != stage.MapId && arena.SpawnPoints.empty() && !ownSpawn)
            return "an arena on a map of its own needs its own spawn points";

        return {};
    }

    /// Why `stage` cannot be used, or empty. `valid` holds the stages accepted so far.
    std::string Problem(StageDefinition const& stage, std::vector<StageDefinition> const& valid)
    {
        if (stage.Blocks.size() < 2 || stage.Blocks[0] != BlockId::Core || stage.Blocks[1] != BlockId::Move)
            return "its blocks must start with core, then move";

        for (std::size_t i = 0; i < stage.Blocks.size(); ++i)
            if (std::find(stage.Blocks.begin() + i + 1, stage.Blocks.end(), stage.Blocks[i]) != stage.Blocks.end())
                return "a block is listed twice";

        // The mental map is written from the camera's frames (perception-goals REDESIGN §3), after them.
        if (stage.Has(BlockId::Map) && !stage.Has(BlockId::Vision))
            return "a mental map is written from the camera's frames: it needs the vision block";
        // Entity memory is written from the camera's entity list, by the entities block before the sight block reads
        // it (dungeon-curriculum I2).
        if (stage.Has(BlockId::Sight))
        {
            auto const entities = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::Entities);
            auto const sight = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::Sight);
            if (!stage.Has(BlockId::Vision) || entities == stage.Blocks.end() || sight < entities)
                return "the sight block reads what the camera's entity list wrote: it needs the vision block, after it";
        }
        // ... and a combat stage with a party reads its members there: the combat block keeps only the player frame and
        // the pet frame (its revision 1), so without the party frames block its party would be unseen.
        if (stage.Has(BlockId::Combat) && stage.SeatCount() > 1 && !stage.Has(BlockId::PartyFrames))
            return "a combat stage with a party reads its members' frames in the party frames block";
        // The combat block's per-target columns ride on the sight list (dungeon-curriculum I3).
        if (stage.Has(BlockId::Combat))
        {
            auto const sight = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::Sight);
            auto const combat = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::Combat);
            if (sight == stage.Blocks.end() || combat < sight)
                return "the combat block's columns ride on the sight list: it needs the sight block, after it";
        }

        // The base only has to exist: seeding maps the base's blocks to this stage's by name (stage.json spans), so a
        // stage may drop base blocks it does not need and several stages may share a base.
        auto const earlier = [&valid](std::string const& name)
        {
            return std::any_of(valid.begin(), valid.end(), [&name](StageDefinition const& other)
            {
                return other.Name == name;
            });
        };

        if (!stage.Extends.empty() && !earlier(stage.Extends))
            return "it extends " + stage.Extends + ", which is not an earlier valid stage";

        for (std::string const& merge : stage.Merges)
        {
            if (stage.Extends.empty())
                return "a merge needs a stage it extends (the trunk)";
            if (merge == stage.Extends || std::count(stage.Merges.begin(), stage.Merges.end(), merge) > 1)
                return "it merges " + merge + " twice";
            if (!earlier(merge))
                return "it merges " + merge + ", which is not an earlier valid stage";
        }

        // A stage with nothing to fight carries no duel block (the movement stages); one that fights needs it.
        bool const fights = stage.AnyArena([](ArenaDefinition const& arena)
        {
            return arena.Against != Opposition::Seek && arena.Against != Opposition::Sight
                && arena.Against != Opposition::Interact && arena.Against != Opposition::PartyFollow;
        });
        if (fights && !stage.Has(BlockId::Duel))
            return "a stage that fights something needs the duel block";

        if (stage.Arenas.empty() || stage.Arenas.size() > MAX_ARENAS)
            return "it needs 1 to " + std::to_string(MAX_ARENAS) + " arenas";
        // Held-out content is measured beside what the stage trains on: a stage of nothing but held-out arenas would
        // have to train on one, and a held-out arena is never trained (Wailing Caverns).
        if (std::all_of(stage.Arenas.begin(), stage.Arenas.end(), [](ArenaDefinition const& arena)
            {
                return arena.EvalOnly;
            }))
            return "it trains on none of its arenas: every one is held out";

        for (std::size_t i = 0; i < stage.Arenas.size(); ++i)
        {
            ArenaDefinition const& arena = stage.Arenas[i];
            if (arena.Name.empty())
                return "an arena has no name";

            for (std::size_t j = i + 1; j < stage.Arenas.size(); ++j)
                if (stage.Arenas[j].Name == arena.Name)
                    return "arena " + arena.Name + " is listed twice";

            if (std::string const problem = ArenaProblem(stage, arena); !problem.empty())
                return "arena " + arena.Name + ": " + problem;
        }

        return {};
    }
}

uint32 Animus::Curriculum::ArenaDefinition::SeatCount() const
{
    switch (Seats)
    {
        // A party is a whole group of learned seats (the dungeon, 2026-09-30), or PartySize of them.
        case SeatPlan::Party:  return PartySize ? PartySize : GROUP_SEATS;
        case SeatPlan::Solo:   break;
    }

    return 1;
}

std::vector<uint32> Animus::Curriculum::ArenaDrawWeights(std::vector<ArenaDefinition> const& arenas,
    std::vector<uint32> const& weights, std::vector<uint32> const& finals, bool evaluating, float progress)
{
    float const along = evaluating ? 1.0f : std::clamp(progress, 0.0f, 1.0f);
    std::vector<uint32> out(arenas.size(), 0);
    for (std::size_t arena = 0; arena < arenas.size(); ++arena)
    {
        ArenaDefinition const& definition = arenas[arena];
        // Held out (Wailing Caverns): played only by an evaluation pinned to it, never drawn -- in training least of
        // all, which is what holding it out means.
        if (definition.EvalOnly)
            continue;
        float const from = float(arena < weights.size() ? weights[arena] : definition.Weight);
        float const to = float(arena < finals.size() ? finals[arena] : from);
        out[arena] = uint32(std::lround(100.0f * (from + (to - from) * along)));
    }
    return out;
}

bool Animus::Curriculum::StageDefinition::Has(BlockId block) const
{
    return std::find(Blocks.begin(), Blocks.end(), block) != Blocks.end();
}

uint32 Animus::Curriculum::StageDefinition::SeatCount() const
{
    uint32 seats = 1;
    for (ArenaDefinition const& arena : Arenas)
        seats = std::max(seats, arena.SeatCount());

    return seats;
}

namespace
{
    std::vector<std::string>& LeftOut()
    {
        static std::vector<std::string> problems;
        return problems;
    }
}

std::vector<Animus::Curriculum::StageDefinition> const& Animus::Curriculum::CurriculumStages()
{
    static std::vector<StageDefinition> const stages = []()
    {
        std::vector<StageDefinition> valid;
        for (StageDefinition& stage : Definitions())
        {
            // A camera brings its entity list, right after it (perception-goals 1b): no stage names it on its own.
            auto const vision = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::Vision);
            if (vision != stage.Blocks.end() && !stage.Has(BlockId::Entities))
                stage.Blocks.insert(vision + 1, BlockId::Entities);

            if (std::string const problem = Problem(stage, valid); !problem.empty())
            {
                LOG_ERROR("module.animus", "Stage {} is left out: {}", stage.Name, problem);
                LeftOut().push_back(stage.Name + ": " + problem);
                continue;
            }

            valid.push_back(std::move(stage));
        }

        return valid;
    }();

    return stages;
}

std::vector<std::string> const& Animus::Curriculum::CurriculumProblems()
{
    CurriculumStages();
    return LeftOut();
}

std::map<uint32, std::vector<std::string>> Animus::Curriculum::StageMaps(uint32 fallbackMap)
{
    std::map<uint32, std::vector<std::string>> maps;
    auto const add = [&maps](uint32 mapId, StageDefinition const& stage)
    {
        std::vector<std::string>& names = maps[mapId];
        if (std::find(names.begin(), names.end(), stage.Name) == names.end())
            names.push_back(stage.Name);
    };
    for (StageDefinition const& stage : CurriculumStages())
    {
        if (stage.MapId)
            add(stage.MapId, stage);
        bool placedByStage = false;
        for (ArenaDefinition const& arena : stage.Arenas)
        {
            if (arena.Instance != InstanceLadder::None)
            {
                std::vector<BossRow> const& rows = InstanceLadderRows(arena.Instance);
                if (arena.InstanceRow >= 0 && std::size_t(arena.InstanceRow) < rows.size())
                    add(rows[std::size_t(arena.InstanceRow)].MapId, stage);
                continue;
            }
            if (arena.MapId)
                add(arena.MapId, stage);
            else
                placedByStage = true;
        }
        if (!stage.MapId && (placedByStage || stage.Arenas.empty()))
            add(fallbackMap, stage);
    }
    return maps;
}

Animus::Curriculum::StageDefinition const* Animus::Curriculum::FindStage(std::string_view name)
{
    for (StageDefinition const& stage : CurriculumStages())
        if (stage.Name == name)
            return &stage;

    return nullptr;
}
