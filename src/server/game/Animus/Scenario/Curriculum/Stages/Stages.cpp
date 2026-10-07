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
 * learner configs in apps/forge/python/configs/archive/, its runs in
 * var/animus-forge/shared/archive/curriculum-v1-2026-10-05/.
 *
 *   movement   move1_controls ─ move2_seek (perception-goals P1: the compass split, a hidden object found by sight)
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
#include "QuestPlanner.h"
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

        return stages;
    }

    /// Why `arena` cannot be played with `stage`'s blocks, or empty.
    std::string ArenaProblem(StageDefinition const& stage, ArenaDefinition const& arena)
    {
        bool const pulls = arena.Against == Opposition::Pulls;
        bool const ambushOnly = arena.Against == Opposition::Ambush;
        bool const flag = arena.Against == Opposition::Flag;
        bool const duelPlayer = arena.Against == Opposition::ScriptedPlayer || arena.Against == Opposition::MirrorSeat
            || flag;
        bool const player = duelPlayer || arena.Ambushers > 0;

        if (pulls != (arena.Schedule != PullSchedule::None))
            return "a pull schedule goes with pulls, and only with pulls";
        if (pulls && !stage.Has(BlockId::Pack))
            return "pulls need the pack block";
        if (arena.DeathRuns && (!stage.Has(BlockId::Death) || arena.Against == Opposition::Instance))
            return "death runs on in the open world, with the death block";
        if (arena.QuestDrill >= 0 && (arena.Against != Opposition::Quest || arena.Seats == SeatPlan::Teams
            || arena.QuestDrill >= int8(OBJECTIVE_KIND_COUNT)))
            return "an objective drill is a quest arena of one group, with an objective kind";
        if ((arena.Schedule == PullSchedule::Gauntlet || arena.Schedule == PullSchedule::Sequence)
            && !stage.Has(BlockId::Gauntlet))
            return "the gauntlet schedule needs the gauntlet block";
        bool const instance = arena.Against == Opposition::Instance;
        if (instance != (arena.Instance != InstanceLadder::None))
            return "an instance ladder goes with fighting in an instance, and only with that";
        if (instance && (!stage.Has(BlockId::Pack) || arena.Schedule != PullSchedule::None))
            return "an instance needs the pack block and no pull schedule";
        if (instance && arena.Seats != SeatPlan::Party && arena.Seats != SeatPlan::Raid)
            return "an instance is fought by a party or a raid";
        if (arena.RaidSeats && (arena.Seats != SeatPlan::Raid || arena.RaidSeats > MAX_SEATS
            || arena.RaidSeats % GROUP_SEATS))
            return "RaidSeats is a raid's seat count: a multiple of GROUP_SEATS, up to MAX_SEATS";
        if (arena.Owner && (!(pulls || ambushOnly || instance) || !stage.Has(BlockId::Companion)))
            return "an owner needs pulls, an ambush or an instance, and the companion block";
        // A raid is a group of its own, in an instance or against pulls (stage12's raid arenas): no owner.
        bool const raidGroup = arena.Seats == SeatPlan::Raid
            && (instance || arena.Against == Opposition::Pulls);
        if (arena.PartyGroup && !stage.Has(BlockId::Party))
            return "a party group needs the party block";
        // A group questing in the world (world_group, world_shared) is a party of its own, with no owner.
        bool const worldGroup = arena.Against == Opposition::Quest
            && (arena.Seats == SeatPlan::Party || arena.Seats == SeatPlan::Teams);
        // So is a group running a dungeon: five learned seats and no owner.
        bool const dungeonGroup = arena.Against == Opposition::Instance && arena.Seats == SeatPlan::Party;
        // And a proper party drilling against pulls (the roles and group stages).
        bool const drillGroup = arena.ProperParty && pulls && arena.Seats == SeatPlan::Party && !arena.Owner;
        if (arena.PartyGroup && !raidGroup && !worldGroup && !dungeonGroup && !drillGroup
            && (!arena.Owner || arena.Seats != SeatPlan::Party))
            return "a party group needs an owner and party seats, unless it is a raid, a quest, a dungeon or a drill";
        if (arena.ProperParty && !(drillGroup && arena.PartyGroup))
            return "a proper party is drawn for a party drill against pulls (a whole dungeon draws its own)";
        if (arena.DrillRole > DRILL_DAMAGE || (arena.DrillRole && !arena.ProperParty))
            return "a drilled role (1 tank, 2 healer, 3 damage) is a proper party's";
        if (arena.Schedule == PullSchedule::Camp && (!arena.ProperParty || !stage.Has(BlockId::Crowd)))
            return "a camp is a proper party's pull drill, read through the crowd block";
        if (arena.PackHealthPct != 100 && (!pulls || arena.PackHealthPct == 0))
            return "pack health is a percentage of a pull's creatures' own";
        if (arena.InstanceRow >= 0 && !instance)
            return "only an instance arena pins a row of its ladder";
        if (arena.EvalOnly && arena.PullDrill)
            return "a held-out arena is played by evaluations, which never play a pull drill";
        if (arena.OwnerCast && !arena.Owner)
            return "a cast owner is still an owner: the arena has to have one";
        if (arena.OwnerCast && stage.SeatCount() + TEAM_COUNT + 1 > MAX_SEATS)
            return "a cast owner needs a seat slot past the seats and the directors, and a raid has none to spare";
        // Self-play: one seat a side in a Mirror, TEAM_SEATS of them in a Teams arena, and a team match is a
        // flag match -- there is nothing else for two learned sides of ten to be playing.
        // Two groups sharing a zone (world_shared) are teams that do not fight each other.
        bool const sharedZone = arena.Seats == SeatPlan::Teams && arena.Against == Opposition::Quest;
        bool const selfPlay = (arena.Seats == SeatPlan::Mirror || arena.Seats == SeatPlan::Teams) && !sharedZone;
        if (selfPlay != (arena.Against == Opposition::MirrorSeat || flag))
            return "self-play seats go with fighting the mirror seat or a flag match, and only with them";
        if (arena.Seats == SeatPlan::Teams && !flag && arena.Against != Opposition::MirrorSeat && !sharedZone)
            return "team seats fight the other team, at a flag or in an arena, or share a zone questing";
        if (arena.Seats == SeatPlan::Teams && (arena.TeamSeats < 1 || arena.TeamSeats > TEAM_SEATS))
            return "a side is between one seat and TEAM_SEATS";
        if (arena.Directed && !stage.Has(BlockId::Order))
            return "a director needs the order block: its seats have to read what it asks";
        if (arena.OnFoot && arena.Against != Opposition::Travel)
            return "only a travel arena can be made on foot: there is nothing else a mount would be barred from";
        if (arena.OnFoot && arena.Flying)
            return "an arena is on foot or it flies, not both";
        if (arena.AirOnly && !arena.Flying)
            return "an air-only arena flies: AirOnly needs Flying";
        if (arena.Places && !arena.Directed)
            return "only a director names a place: the arena has to be directed";
        if (arena.DirectorLearned && !arena.Directed)
            return "a learned director is still a director: the arena has to be directed";
        if (arena.Directed && arena.Seats != SeatPlan::Teams && arena.Seats != SeatPlan::Party
            && arena.Seats != SeatPlan::Raid)
            return "a director commands a group: its arena needs team, party or raid seats";
        if (arena.Ambushers > MAX_AMBUSHERS)
            return "at most " + std::to_string(MAX_AMBUSHERS) + " ambushers";
        // A quest's ambushers are hostile players in the world: they gank whoever is questing, owner or not.
        bool const quest = arena.Against == Opposition::Quest;
        if (arena.Ambushers > 0 && !(pulls || ambushOnly || quest))
            return "ambushers join pulls or a quest, or are the whole fight (Opposition::Ambush)";
        if (arena.Ambushers > 0 && ((!arena.Owner && !quest) || !stage.Has(BlockId::Pack)))
            return "ambushers attack an owner (or anyone questing) and take enemy slots (the pack block)";
        if (ambushOnly && arena.Ambushers != 1)
            return "an ambush without pulls has exactly one ambusher (a one-on-one reward)";
        if (player && !stage.Has(BlockId::Pvp))
            return "fighting a player needs the pvp block";
        if (duelPlayer && !arena.Pvp)
            return "a one-on-one against a player is pvp";
        if (arena.Pvp && !player)
            return "a pvp arena fights a player";
        bool const travel = arena.Against == Opposition::Travel;
        // A marker arena takes some of the travel arena's kinds of ground (ledges, rooms, water, lakebeds, chains,
        // flight), each by its own course; which course may have which is the marker rules' further down.
        bool const markerGround = arena.Against == Opposition::Markers;
        bool const placed = travel || markerGround;
        if (travel && !stage.Has(BlockId::Travel))
            return "travel needs the travel block";
        if (travel && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0))
            return "travel is one seat on its own";
        if ((arena.OpponentLevelBonus != 0 || arena.OpponentLevelRange != 0)
            && arena.Against != Opposition::ScriptedPlayer)
            return "only a scripted enemy player takes a level bonus or range";
        if (arena.OpponentLevelRange < 0)
            return "a level range is how far either way, not negative";
        bool const mountedMarkers = arena.Against == Opposition::Markers && arena.Course == MarkerCourse::Mounted;
        if (arena.Flying && !travel && !mountedMarkers)
            return "only a travel arena flies";
        if (arena.Indoors && !placed)
            return "only a travel or marker arena can be indoors: being inside changes where an objective may be put "
                "and what reaching it means, and nothing else asks either question";
        if (arena.Indoors && arena.Flying)
            return "an arena is indoors or it flies, not both";
        if (arena.Indoors && arena.Water)
            return "an interior arena has no crossing to offer: water wants an objective across a lake";
        if (arena.Ledges && !placed)
            return "only a travel or marker arena has ledges: an objective below a drop is a place to get to";
        if (arena.Ledges && (arena.Flying || arena.Indoors || arena.Water))
            return "a ledge arena is on foot outdoors: the drop is the shortcut and the ramp is the way round, which "
                "wings, a roof or a lake would each make a different question";
        if (arena.Water && !placed && arena.Against != Opposition::Creature)
            return "water is a travel arena's crossing or a creature arena's lake; nothing else reads it";
        if (arena.Underwater && !placed)
            return "only a travel or marker arena dives: an objective on a lakebed is a place to get to";
        if (arena.Underwater && (arena.Flying || arena.Indoors || arena.Ledges || arena.Water))
            return "a dive arena is its own trip: the objective is on the bed, not across the lake, and neither "
                "wings, a roof nor a ledge belong to it";
        if (arena.Checkpoints && !(placed && arena.Underwater))
            return "only a dive arena chains: the next lakebed is drawn the way the first was, and no other kind of "
                "objective has a next one yet";
        if (flag && (!stage.Has(BlockId::Travel) || !stage.Has(BlockId::Flag)))
            return "a flag match needs the travel and flag blocks";
        bool const life = arena.Against == Opposition::Quest || arena.Against == Opposition::Gather
            || arena.Against == Opposition::Town;
        if (life && (!stage.Has(BlockId::World) || !stage.Has(BlockId::Travel) || !stage.Has(BlockId::Pack)))
            return "life outside the fight needs the world, travel and pack blocks";
        // A quest may be a group's (world_group, world_shared) and may be ganked (ambushers); gathering and a
        // town are one seat on its own.
        if (life && ((arena.Seats != SeatPlan::Solo && !worldGroup) || arena.Owner || arena.Pvp
            || (arena.Ambushers > 0 && arena.Against != Opposition::Quest) || arena.Schedule != PullSchedule::None))
            return "a life arena is one seat on its own (or a group questing), with no pulls";
        if (arena.LoneSeats && (!sharedZone || arena.LoneSeats > MAX_LONE_SEATS || arena.SeatCount() > MAX_SEATS))
            return "seats questing alone go beside two groups sharing a zone, at most MAX_LONE_SEATS of them";
        if (stage.Has(BlockId::World) && !stage.AnyArena([](ArenaDefinition const& other)
            {
                return other.Against == Opposition::Quest || other.Against == Opposition::Gather
                    || other.Against == Opposition::Town;
            }))
            return "the world block wants a life arena to be read in";

        // The movement stages' markers: one seat on its own with nothing else in the episode, on the stage's ground.
        // The travel arena's kinds of ground (water, rooms, ledges, lakebeds, the air) are not markers' yet: each
        // comes with the stage that asks for it.
        bool const markers = arena.Against == Opposition::Markers;
        if (markers && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None || arena.Directed))
            return "a marker arena is one seat on its own, with nothing to fight and no one to follow";
        if (markers && arena.OnFoot)
            return "a marker arena is on foot unless it is the mounted course's: OnFoot is the travel arena's";
        if (markers && (arena.Flying || arena.AirOnly) && arena.Course != MarkerCourse::Mounted)
            return "flying and air-only markers are the mounted course's";
        if (markers && arena.Course == MarkerCourse::Mounted && !stage.Has(BlockId::Travel))
            return "the mounted course mounts with the travel block";
        if (markers && (arena.Water || arena.Underwater || arena.Checkpoints) && arena.Course != MarkerCourse::Water)
            return "water, lakebeds and their chains are the water course's ground";
        if (markers && arena.Course == MarkerCourse::Water && arena.Water == arena.Underwater)
            return "a water marker arena is a crossing (Water) or lakebeds (Underwater)";
        if (markers && arena.Checkpoints && !arena.Underwater)
            return "a chain of markers is a chain of lakebeds";
        if (markers && (arena.Ledges || arena.Indoors) && arena.Course != MarkerCourse::Vertical)
            return "ledges and rooms are the vertical course's ground";
        if (markers && arena.Ledges && arena.Indoors)
            return "a marker arena is ledges or rooms, not both";
        if (markers && !stage.Has(BlockId::Move))
            return "markers are walked to with the move block";
        if (markers && !stage.Has(BlockId::Compass))
            return "a marker is found by its bearing and distance: the compass block";
        if (!markers && arena.Course != MarkerCourse::Open)
            return "only a marker arena has a course";

        // The follow stage: one seat, a leader in the owner's slot, nothing to fight and none of the travel arena's
        // kinds of ground.
        bool const follow = arena.Against == Opposition::Follow;
        if (follow && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None || arena.Directed || arena.OnFoot || arena.Flying || arena.AirOnly
            || arena.Water || arena.Indoors || arena.Ledges || arena.Underwater || arena.Checkpoints))
            return "a follow arena is one seat and a leader, with nothing to fight";
        if (follow && !stage.Has(BlockId::Move))
            return "a leader is followed with the move block";

        // The seek stage: one seat in a dungeon of rooms, an object to find in one of them, nothing to fight; it finds
        // the object by sight, so it carries the camera and not the compass.
        bool const seek = arena.Against == Opposition::Seek;
        if (seek && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None || arena.Directed || arena.Flying || arena.Water
            || arena.Underwater || arena.Checkpoints || arena.Objective))
            return "a seek arena is one seat on its own, on foot and dry, with no marker of its own";
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
        if (sight && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None || arena.Directed || arena.Flying || arena.Water
            || arena.Underwater || arena.Checkpoints || arena.Objective || !arena.Rooms.empty()))
            return "a sight arena is one seat on its own, on foot and dry, with no marker or room of its own";
        if (sight && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Compass) || !stage.Has(BlockId::Vision)))
            return "a sight arena is walked with the move block, pointed at by the compass and seen with the camera";
        if (sight && (arena.Objects.empty() || arena.SpawnPoints.size() < 2))
            return "a sight arena needs objects to put down and hallway points to spawn at and put them on";
        for (SightPair const& pair : arena.SightPairs)
            if (!sight || pair.Spawn >= arena.SpawnPoints.size() || pair.Object >= arena.SpawnPoints.size()
                || pair.Spawn == pair.Object)
                return "a sight pair is a sight arena's: two different points of its SpawnPoints";

        if (!seek && !arena.Rooms.empty())
            return "only a seek arena has rooms";
        if (!seek && !sight && !arena.Objects.empty())
            return "only a seek or a sight arena has objects";
        for (SeekRoom const& room : arena.Rooms)
            if (room.Floor.size() < 3 || room.Name.empty())
                return "a seek room is named and its floor is a polygon";

        bool const dummy = arena.Against == Opposition::Dummy;
        if (dummy && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None))
            return "the rotation drill is one seat on its own against its dummies";
        if (arena.Drill != DummyDrill::Still && !dummy)
            return "only a dummy arena has a drill";
        if (dummy && arena.Drill == DummyDrill::Moving && !stage.Has(BlockId::Pack))
            return "the moving drill's extra dummies need the pack block's slots";
        // An arena on a map of its own stands on its own ground: the stage's points are on the stage's map. An
        // encounter that finds its own spawn (an instance's door, a quest giver, a node field, an inn) needs none.
        bool const ownSpawn = instance || life;
        if (arena.MapId && arena.MapId != stage.MapId && arena.SpawnPoints.empty() && !ownSpawn)
            return "an arena on a map of its own needs its own spawn points";
        if (arena.MapId && arena.MapId != stage.MapId && flag)
            return "a flag match's bases are the stage's: it plays on the stage's map";

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
            return arena.Against != Opposition::Markers && arena.Against != Opposition::Follow
                && arena.Against != Opposition::Seek && arena.Against != Opposition::Sight;
        });
        if (fights && !stage.Has(BlockId::Duel))
            return "a stage that fights something needs the duel block";

        if (stage.Arenas.empty() || stage.Arenas.size() > MAX_ARENAS)
            return "it needs 1 to " + std::to_string(MAX_ARENAS) + " arenas";

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
        // A party is the owner and its companions: GROUP_MEMBERS learned seats beside it, which is what this
        // returned when MAX_SEATS was 4 and is what it has to keep returning now that MAX_SEATS is a raid. With no
        // owner it is a whole group of learned seats (the dungeon, 2026-09-30).
        case SeatPlan::Party:  return Owner ? GROUP_MEMBERS : GROUP_SEATS;
        case SeatPlan::Raid:   return RaidSeats ? RaidSeats : MAX_SEATS;
        case SeatPlan::Teams:  return std::min(TeamSeats, TEAM_SEATS) * TEAM_COUNT + LoneSeats;
        case SeatPlan::Mirror: return 2;
        case SeatPlan::Solo:   break;
    }

    return 1;
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

Animus::Curriculum::StageDefinition const* Animus::Curriculum::FindStage(std::string_view name)
{
    for (StageDefinition const& stage : CurriculumStages())
        if (stage.Name == name)
            return &stage;

    return nullptr;
}
