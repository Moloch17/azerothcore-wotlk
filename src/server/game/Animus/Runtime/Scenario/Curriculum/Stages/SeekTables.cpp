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

#include "SeekTables.h"
#include <algorithm>
#include <iterator>

/*
 * **How these tables were authored** (offline, 2026-10-10; .agents/plans/general-search/tools/: navscan.py,
 * annotate.py, scene.py, rooms3d.py -- the navmesh and the baked scene read only to author, never a bot input):
 *
 * 1. The map's navmesh (the extracted mmtiles, DT_POLYREF64) scanned on a half-yard grid: every ground polygon's
 *    detail triangles rasterised, one cell-storey per floor (storeys a yard apart kept apart: the Deadmines stack).
 *    Magma, slime and water polygons are left out.
 * 2. Each cell's floor read from the baked scene (Runtime/Vision/BakedScene: the vmaps' and terrain's collision
 *    triangles, the terrain heightfield where no model floors a cell -- the Deadmines' cove), a cell under a deadly
 *    liquid (Ragefire's lava) dropped, and one with no floor within 1.5 yd of the navmesh's dropped.
 * 3. The cell-storeys reachable from the entrance (four-way, floors within a yard) eroded by ERODE cells so the narrow
 *    passages part -- 6 yd for Ragefire's cave (a yard and a half, the Stockades', parts nothing in a cave 12-20 yd
 *    wide), 4 yd for the Deadmines' tunnels -- the parts flooded as regions and grown back over the rim, nearest
 *    region first. Regions under 250 sq yd (Ragefire) / 200 sq yd (the Deadmines) are left out.
 * 4. The HALLWAY is the chain of regions from the entrance that every other region hangs off: Ragefire's entrance
 *    passage, its first cavern, the descending passage and the lower cavern round the lava; the Deadmines' entrance
 *    hall and the mine tunnel down to its first junction. Its cells a yard and a half or more off any wall, sampled
 *    every 3 yd where the scene's floor agrees with the navmesh's within a yard, are the hallway points (the entrance
 *    first, areatrigger_teleport 2230 / 78).
 * 5. Each other region is a room. A cave region winds and slopes, so its Floor is not the hull of the whole region:
 *    it is the convex hull (cut to eight corners) of the region's cells a yard and a half off any wall within a
 *    radius of a seed cell, within 1.5 yd of the seed's floor, the radius shrunk from 40 yd until at least 97 of
 *    every 100 of the hull's 1-yd samples lie on the region's own cells near that floor; seeds are tried from the
 *    region's centroid outward, 6 yd apart, and the largest polygon wins. FloorZ is the seed's scene floor, Centre
 *    the seed, Opening the middle of the cells shared with the region it is entered from (the bordering region
 *    nearest the entrance by walk), Walk the eight-way Dijkstra distance from the entrance to the Centre over the
 *    scan's cells. Names are <map>_<region id>_<word>: the word says what the region is (a cavern of 1000 sq yd or
 *    more, a chamber of 400, a nook with one way in, else a passage; a few named by hand). None ends in _back or
 *    holds _end_: the Stockades' back-room rules (BackRoomName, RoomEntryBackMult, SeekDraw::IsHardRoom) are inert
 *    here by construction.
 * 6. Front (the doorway and room rungs' set) is every room whose Opening is onto the hallway. Ragefire has four;
 *    the Deadmines is held out and plays the deep rung's sweep over every room, so its Front is never drawn.
 *
 * **Checked** (rooms3d.py --validate, the one-off run of the StockadeRoomsDataTest rule, no test): every 1-yd sample
 * of every Floor against the baked scene. Ragefire: 18,786 samples, 86 with no floor within Seek.FloorTolerance
 * (2 yd) of FloorZ, 540 without Seek.Clearance (0.8 yd) of room at knee height along every axis (the cave's
 * stalagmites and rubble; Place tries Seek.Attempts spots and falls back to the centre). The Deadmines: 11,716
 * samples, 39 and 487. Ragefire's cave openings are wide borders, not 3-yd doorways: the trap drill's jamb search
 * there mostly finds no jamb and the episode starts as usual.
 *
 * Regions: Ragefire 36 of 250 sq yd or more (8 hallway, 28 rooms); the Deadmines 43 of 200 sq yd or more (10 hallway,
 * 33 rooms), the cove's outer shore (y below -870) left out.
 */

std::vector<Position> Animus::Curriculum::SeekTables::RagefireHallways()
{
    // 192 points: the entrance, then the hallway regions' cores every 3 yd (the facing is drawn each episode).
    return {
            { 3.81f, -14.82f, -17.84f, 4.39f },
            { -294.0f, -48.0f, -60.93f, 0.0f }, { -288.0f, -54.0f, -60.89f, 0.0f }, { -288.0f, -51.0f, -60.93f, 0.0f },
            { -285.0f, -63.0f, -60.44f, 0.0f }, { -285.0f, -60.0f, -60.44f, 0.0f }, { -285.0f, -57.0f, -60.52f, 0.0f },
            { -285.0f, -54.0f, -60.81f, 0.0f }, { -285.0f, -51.0f, -60.93f, 0.0f }, { -282.0f, -63.0f, -60.44f, 0.0f },
            { -282.0f, -60.0f, -60.44f, 0.0f }, { -282.0f, -57.0f, -60.44f, 0.0f }, { -282.0f, -54.0f, -60.73f, 0.0f },
            { -282.0f, -51.0f, -60.93f, 0.0f }, { -279.0f, -63.0f, -60.44f, 0.0f }, { -279.0f, -60.0f, -60.44f, 0.0f },
            { -279.0f, -57.0f, -60.44f, 0.0f }, { -279.0f, -54.0f, -60.64f, 0.0f }, { -279.0f, -51.0f, -60.92f, 0.0f },
            { -279.0f, -48.0f, -60.93f, 0.0f }, { -276.0f, -60.0f, -60.44f, 0.0f }, { -276.0f, -57.0f, -60.44f, 0.0f },
            { -276.0f, -54.0f, -60.58f, 0.0f }, { -276.0f, -51.0f, -60.86f, 0.0f }, { -276.0f, -48.0f, -60.93f, 0.0f },
            { -273.0f, -60.0f, -60.44f, 0.0f }, { -273.0f, -57.0f, -60.44f, 0.0f }, { -273.0f, -54.0f, -60.52f, 0.0f },
            { -273.0f, -51.0f, -60.81f, 0.0f }, { -273.0f, -48.0f, -60.93f, 0.0f }, { -273.0f, -45.0f, -60.93f, 0.0f },
            { -273.0f, -42.0f, -60.81f, 0.0f }, { -270.0f, -57.0f, -60.44f, 0.0f }, { -270.0f, -54.0f, -60.51f, 0.0f },
            { -270.0f, -51.0f, -60.77f, 0.0f }, { -270.0f, -48.0f, -60.93f, 0.0f }, { -270.0f, -45.0f, -60.93f, 0.0f },
            { -270.0f, -42.0f, -60.83f, 0.0f }, { -270.0f, -39.0f, -60.69f, 0.0f }, { -270.0f, -36.0f, -60.69f, 0.0f },
            { -267.0f, -54.0f, -60.52f, 0.0f }, { -267.0f, -51.0f, -60.74f, 0.0f }, { -267.0f, -48.0f, -60.93f, 0.0f },
            { -267.0f, -45.0f, -60.93f, 0.0f }, { -267.0f, -42.0f, -60.84f, 0.0f }, { -267.0f, -39.0f, -60.70f, 0.0f },
            { -267.0f, -36.0f, -60.69f, 0.0f }, { -267.0f, -33.0f, -60.69f, 0.0f }, { -264.0f, -54.0f, -60.54f, 0.0f },
            { -264.0f, -51.0f, -60.76f, 0.0f }, { -264.0f, -48.0f, -60.93f, 0.0f }, { -264.0f, -45.0f, -60.93f, 0.0f },
            { -264.0f, -42.0f, -60.86f, 0.0f }, { -264.0f, -39.0f, -60.72f, 0.0f }, { -264.0f, -36.0f, -60.69f, 0.0f },
            { -264.0f, -33.0f, -60.69f, 0.0f }, { -264.0f, -30.0f, -60.69f, 0.0f }, { -264.0f, -27.0f, -60.69f, 0.0f },
            { -261.0f, -45.0f, -60.93f, 0.0f }, { -261.0f, -42.0f, -60.89f, 0.0f }, { -261.0f, -39.0f, -60.75f, 0.0f },
            { -261.0f, -36.0f, -60.69f, 0.0f }, { -261.0f, -33.0f, -60.69f, 0.0f }, { -261.0f, -30.0f, -60.69f, 0.0f },
            { -261.0f, -27.0f, -60.69f, 0.0f }, { -258.0f, -42.0f, -60.92f, 0.0f }, { -258.0f, -39.0f, -60.77f, 0.0f },
            { -258.0f, -36.0f, -60.69f, 0.0f }, { -258.0f, -33.0f, -60.66f, 0.0f }, { -255.0f, -42.0f, -60.81f, 0.0f },
            { -255.0f, -39.0f, -60.62f, 0.0f }, { -255.0f, -36.0f, -60.44f, 0.0f }, { -255.0f, -33.0f, -60.19f, 0.0f },
            { -252.0f, -39.0f, -60.22f, 0.0f }, { -252.0f, -36.0f, -60.06f, 0.0f }, { -252.0f, -33.0f, -59.96f, 0.0f },
            { -249.0f, -36.0f, -59.67f, 0.0f }, { -246.0f, -36.0f, -58.73f, 0.0f }, { -243.0f, -36.0f, -57.81f, 0.0f },
            { -240.0f, -36.0f, -57.38f, 0.0f }, { -237.0f, -36.0f, -56.52f, 0.0f }, { -234.0f, -36.0f, -56.52f, 0.0f },
            { -231.0f, -36.0f, -56.52f, 0.0f }, { -228.0f, -36.0f, -56.18f, 0.0f }, { -225.0f, -36.0f, -55.80f, 0.0f },
            { -222.0f, -36.0f, -55.28f, 0.0f }, { -219.0f, -36.0f, -54.62f, 0.0f }, { -216.0f, -36.0f, -53.74f, 0.0f },
            { -213.0f, -36.0f, -52.87f, 0.0f }, { -213.0f, -33.0f, -52.72f, 0.0f }, { -210.0f, -39.0f, -51.85f, 0.0f },
            { -210.0f, -36.0f, -51.87f, 0.0f }, { -210.0f, -33.0f, -51.77f, 0.0f }, { -207.0f, -36.0f, -50.90f, 0.0f },
            { -207.0f, -33.0f, -50.85f, 0.0f }, { -204.0f, -36.0f, -49.65f, 0.0f }, { -204.0f, -33.0f, -49.67f, 0.0f },
            { -201.0f, -36.0f, -48.40f, 0.0f }, { -201.0f, -33.0f, -48.40f, 0.0f }, { -198.0f, -33.0f, -47.26f, 0.0f },
            { -195.0f, -33.0f, -46.42f, 0.0f }, { -192.0f, -33.0f, -45.58f, 0.0f }, { -189.0f, -33.0f, -45.22f, 0.0f },
            { -156.0f, -33.0f, -37.72f, 0.0f }, { -153.0f, -33.0f, -36.55f, 0.0f }, { -57.0f, -36.0f, -19.83f, 0.0f },
            { -54.0f, -36.0f, -20.70f, 0.0f }, { -54.0f, -33.0f, -20.60f, 0.0f }, { -54.0f, -30.0f, -20.50f, 0.0f },
            { -51.0f, -39.0f, -21.27f, 0.0f }, { -51.0f, -36.0f, -21.08f, 0.0f }, { -51.0f, -33.0f, -20.98f, 0.0f },
            { -51.0f, -30.0f, -20.79f, 0.0f }, { -48.0f, -39.0f, -21.71f, 0.0f }, { -48.0f, -36.0f, -21.48f, 0.0f },
            { -48.0f, -33.0f, -21.29f, 0.0f }, { -48.0f, -30.0f, -21.08f, 0.0f }, { -45.0f, -39.0f, -21.78f, 0.0f },
            { -45.0f, -36.0f, -21.63f, 0.0f }, { -45.0f, -33.0f, -21.62f, 0.0f }, { -45.0f, -30.0f, -21.55f, 0.0f },
            { -42.0f, -39.0f, -21.75f, 0.0f }, { -42.0f, -36.0f, -21.62f, 0.0f }, { -42.0f, -33.0f, -21.62f, 0.0f },
            { -42.0f, -30.0f, -21.62f, 0.0f }, { -42.0f, -27.0f, -21.62f, 0.0f }, { -42.0f, -24.0f, -21.62f, 0.0f },
            { -39.0f, -45.0f, -21.86f, 0.0f }, { -39.0f, -42.0f, -21.86f, 0.0f }, { -39.0f, -39.0f, -21.72f, 0.0f },
            { -39.0f, -36.0f, -21.62f, 0.0f }, { -39.0f, -33.0f, -21.62f, 0.0f }, { -39.0f, -30.0f, -21.62f, 0.0f },
            { -39.0f, -27.0f, -21.62f, 0.0f }, { -39.0f, -24.0f, -21.62f, 0.0f }, { -39.0f, -21.0f, -21.62f, 0.0f },
            { -36.0f, -54.0f, -21.37f, 0.0f }, { -36.0f, -51.0f, -21.56f, 0.0f }, { -36.0f, -48.0f, -21.82f, 0.0f },
            { -36.0f, -45.0f, -21.86f, 0.0f }, { -36.0f, -42.0f, -21.85f, 0.0f }, { -36.0f, -39.0f, -21.70f, 0.0f },
            { -36.0f, -36.0f, -21.62f, 0.0f }, { -36.0f, -33.0f, -21.62f, 0.0f }, { -36.0f, -30.0f, -21.62f, 0.0f },
            { -36.0f, -27.0f, -21.62f, 0.0f }, { -36.0f, -24.0f, -21.62f, 0.0f }, { -33.0f, -54.0f, -21.37f, 0.0f },
            { -33.0f, -51.0f, -21.56f, 0.0f }, { -33.0f, -48.0f, -21.85f, 0.0f }, { -33.0f, -45.0f, -21.86f, 0.0f },
            { -33.0f, -42.0f, -21.83f, 0.0f }, { -33.0f, -39.0f, -21.68f, 0.0f }, { -33.0f, -36.0f, -21.62f, 0.0f },
            { -33.0f, -33.0f, -21.62f, 0.0f }, { -30.0f, -57.0f, -21.37f, 0.0f }, { -30.0f, -54.0f, -21.37f, 0.0f },
            { -30.0f, -51.0f, -21.60f, 0.0f }, { -30.0f, -48.0f, -21.86f, 0.0f }, { -30.0f, -45.0f, -21.86f, 0.0f },
            { -30.0f, -42.0f, -21.82f, 0.0f }, { -30.0f, -39.0f, -21.67f, 0.0f }, { -27.0f, -60.0f, -21.37f, 0.0f },
            { -27.0f, -57.0f, -21.37f, 0.0f }, { -27.0f, -54.0f, -21.38f, 0.0f }, { -27.0f, -51.0f, -21.65f, 0.0f },
            { -27.0f, -48.0f, -21.86f, 0.0f }, { -24.0f, -60.0f, -21.37f, 0.0f }, { -24.0f, -57.0f, -21.37f, 0.0f },
            { -24.0f, -54.0f, -21.44f, 0.0f }, { -24.0f, -51.0f, -21.72f, 0.0f }, { -24.0f, -48.0f, -21.86f, 0.0f },
            { -21.0f, -63.0f, -21.38f, 0.0f }, { -21.0f, -60.0f, -21.37f, 0.0f }, { -21.0f, -57.0f, -21.37f, 0.0f },
            { -21.0f, -54.0f, -21.52f, 0.0f }, { -21.0f, -51.0f, -21.80f, 0.0f }, { -21.0f, -48.0f, -21.86f, 0.0f },
            { -18.0f, -63.0f, -21.37f, 0.0f }, { -18.0f, -60.0f, -21.37f, 0.0f }, { -18.0f, -57.0f, -21.37f, 0.0f },
            { -18.0f, -54.0f, -21.60f, 0.0f }, { -18.0f, -51.0f, -21.86f, 0.0f }, { -18.0f, -48.0f, -21.86f, 0.0f },
            { -15.0f, -51.0f, -21.86f, 0.0f }, { -15.0f, -48.0f, -21.86f, 0.0f }, { -12.0f, -48.0f, -21.86f, 0.0f },
            { -9.0f, -48.0f, -21.86f, 0.0f }, { -9.0f, -45.0f, -21.85f, 0.0f }, { 3.0f, -18.0f, -18.75f, 0.0f },
            { 3.0f, -15.0f, -17.84f, 0.0f }, { 3.0f, -12.0f, -16.89f, 0.0f }
    };
}

std::vector<Animus::Curriculum::SeekRoom> Animus::Curriculum::SeekTables::RagefireRooms()
{
    // In order of walking distance from the entrance (SeekDraw::Depths ranks by Walk, not by this order).
    std::vector<SeekRoom> rooms = {
            // rf_24_chamber: 922 sq yd, 325 yd from the entrance, opening onto the hallway (front)
            { "rf_24_chamber", -61.80f, { -245.9f, -5.7f }, { -217.5f, -11.0f }, 325.0f,
                { { -228.0f, -15.5f }, { -224.5f, -18.5f }, { -212.0f, -18.5f }, { -210.5f, -17.0f },
                  { -198.5f, 0.0f }, { -200.0f, 2.0f }, { -219.5f, 1.5f }, { -231.0f, -10.0f } } },
            // rf_19_nook: 442 sq yd, 328 yd from the entrance, opening onto the hallway (front)
            { "rf_19_nook", -61.90f, { -270.0f, -82.9f }, { -268.0f, -94.5f }, 328.0f,
                { { -279.5f, -92.5f }, { -277.0f, -102.0f }, { -275.5f, -103.5f }, { -271.5f, -104.5f },
                  { -270.5f, -104.5f }, { -263.5f, -103.0f }, { -260.5f, -99.0f }, { -261.0f, -89.0f } } },
            // rf_10_passage: 262 sq yd, 335 yd from the entrance, opening onto the hallway (front)
            { "rf_10_passage", -59.27f, { -300.8f, -33.2f }, { -305.5f, -24.0f }, 335.0f,
                { { -313.0f, -24.0f }, { -304.0f, -33.0f }, { -300.0f, -32.5f }, { -298.0f, -30.0f },
                  { -301.0f, -20.0f }, { -303.0f, -19.5f }, { -308.5f, -19.0f }, { -311.5f, -20.5f } } },
            // rf_17_chamber: 678 sq yd, 371 yd from the entrance, opening onto r11
            { "rf_17_chamber", -46.14f, { -302.2f, 0.6f }, { -289.0f, 12.0f }, 371.0f,
                { { -298.0f, 8.0f }, { -295.0f, 4.0f }, { -292.5f, 3.0f }, { -286.0f, 2.5f },
                  { -281.5f, 5.5f }, { -280.0f, 14.5f }, { -294.5f, 14.5f }, { -298.0f, 13.0f } } },
            // rf_38_cavern: 1397 sq yd, 403 yd from the entrance, opening onto the hallway (front)
            { "rf_38_cavern", -58.13f, { -181.0f, -65.1f }, { -150.0f, -47.5f }, 403.0f,
                { { -162.0f, -47.5f }, { -161.5f, -49.0f }, { -150.0f, -59.5f }, { -147.0f, -59.0f },
                  { -143.0f, -57.0f }, { -145.0f, -49.0f }, { -159.0f, -40.0f }, { -161.5f, -44.5f } } },
            // rf_36_nook: 860 sq yd, 425 yd from the entrance, opening onto rf_24_chamber
            { "rf_36_nook", -38.76f, { -172.9f, 5.0f }, { -153.5f, 30.0f }, 425.0f,
                { { -160.5f, 30.5f }, { -149.5f, 12.0f }, { -140.5f, 29.5f }, { -140.5f, 38.5f },
                  { -146.0f, 44.0f }, { -152.5f, 46.0f }, { -157.0f, 44.0f }, { -160.5f, 36.5f } } },
            // rf_21_cavern: 1718 sq yd, 428 yd from the entrance, opening onto rf_17_chamber
            { "rf_21_cavern", -43.92f, { -264.7f, 12.2f }, { -226.5f, 5.5f }, 428.0f,
                { { -240.0f, 2.5f }, { -233.0f, 1.5f }, { -220.0f, 2.0f }, { -218.5f, 9.0f },
                  { -219.5f, 12.5f }, { -224.5f, 14.0f }, { -237.5f, 13.5f }, { -239.5f, 10.5f } } },
            // rf_28_chamber: 575 sq yd, 478 yd from the entrance, opening onto rf_21_cavern
            { "rf_28_chamber", -31.92f, { -187.6f, 14.8f }, { -179.0f, 12.0f }, 478.0f,
                { { -188.0f, 12.5f }, { -187.0f, 4.5f }, { -174.0f, 8.0f }, { -172.0f, 10.0f },
                  { -172.0f, 12.5f }, { -173.5f, 18.5f }, { -180.0f, 18.5f }, { -188.0f, 17.5f } } },
            // rf_32_chamber: 546 sq yd, 487 yd from the entrance, opening onto rf_28_chamber
            { "rf_32_chamber", -30.00f, { -172.3f, 12.1f }, { -170.0f, 11.0f }, 487.0f,
                { { -173.0f, 14.5f }, { -171.5f, 10.0f }, { -167.5f, 8.0f }, { -166.5f, 8.0f },
                  { -164.5f, 12.0f }, { -167.0f, 20.5f }, { -172.0f, 19.0f }, { -173.0f, 18.5f } } },
            // rf_40_chamber: 400 sq yd, 524 yd from the entrance, opening onto rf_32_chamber
            { "rf_40_chamber", -20.36f, { -141.8f, 8.8f }, { -132.5f, 2.0f }, 524.0f,
                { { -142.5f, 5.0f }, { -134.0f, 1.5f }, { -124.0f, 3.0f }, { -116.5f, 9.0f },
                  { -116.5f, 10.5f }, { -122.5f, 16.5f }, { -134.5f, 14.5f }, { -141.0f, 12.5f } } },
            // rf_41_cavern: 3154 sq yd, 572 yd from the entrance, opening onto rf_40_chamber
            { "rf_41_cavern", -18.39f, { -119.1f, 13.1f }, { -108.0f, 47.5f }, 572.0f,
                { { -117.0f, 27.5f }, { -108.0f, 25.5f }, { -91.5f, 33.0f }, { -86.0f, 47.5f },
                  { -89.0f, 58.5f }, { -101.5f, 68.5f }, { -112.0f, 69.0f }, { -120.5f, 55.5f } } },
            // rf_37_chamber: 743 sq yd, 610 yd from the entrance, opening onto rf_41_cavern
            { "rf_37_chamber", -20.91f, { -132.1f, 74.4f }, { -138.0f, 80.5f }, 610.0f,
                { { -150.5f, 77.5f }, { -148.0f, 68.0f }, { -134.5f, 69.5f }, { -130.0f, 74.0f },
                  { -128.0f, 81.0f }, { -129.5f, 98.5f }, { -149.0f, 83.5f }, { -150.5f, 81.0f } } },
            // rf_31_chamber: 423 sq yd, 623 yd from the entrance, opening onto rf_37_chamber
            { "rf_31_chamber", -21.18f, { -149.3f, 69.2f }, { -161.0f, 71.5f }, 623.0f,
                { { -175.0f, 71.5f }, { -174.5f, 68.0f }, { -150.0f, 63.0f }, { -148.5f, 68.0f },
                  { -148.5f, 73.0f }, { -151.0f, 80.0f }, { -173.0f, 78.5f }, { -174.5f, 74.5f } } },
            // rf_43_chamber: 417 sq yd, 662 yd from the entrance, opening onto rf_37_chamber
            { "rf_43_chamber", -7.51f, { -112.0f, 103.0f }, { -91.5f, 102.5f }, 662.0f,
                { { -96.0f, 102.0f }, { -83.5f, 89.5f }, { -80.5f, 88.5f }, { -75.5f, 94.5f },
                  { -74.0f, 99.0f }, { -80.0f, 102.5f }, { -82.5f, 103.0f }, { -96.0f, 103.0f } } },
            // rf_23_cavern: 1733 sq yd, 665 yd from the entrance, opening onto r30
            { "rf_23_cavern", -24.68f, { -184.5f, 78.2f }, { -194.5f, 91.5f }, 665.0f,
                { { -218.5f, 81.5f }, { -215.0f, 76.0f }, { -195.0f, 77.0f }, { -188.0f, 81.5f },
                  { -186.5f, 102.0f }, { -188.0f, 110.0f }, { -203.0f, 116.0f }, { -219.5f, 97.5f } } },
            // rf_44_chamber: 586 sq yd, 692 yd from the entrance, opening onto rf_43_chamber
            { "rf_44_chamber", -6.63f, { -75.1f, 90.3f }, { -68.0f, 85.5f }, 692.0f,
                { { -78.0f, 86.5f }, { -77.0f, 84.5f }, { -71.0f, 75.5f }, { -67.0f, 88.5f },
                  { -68.0f, 93.0f }, { -68.5f, 93.5f }, { -76.0f, 93.5f }, { -78.0f, 91.5f } } },
            // rf_27_cavern: 1078 sq yd, 727 yd from the entrance, opening onto rf_23_cavern
            { "rf_27_cavern", -25.22f, { -195.8f, 129.5f }, { -190.5f, 155.0f }, 727.0f,
                { { -204.0f, 151.5f }, { -193.5f, 141.5f }, { -190.5f, 141.0f }, { -184.5f, 145.0f },
                  { -177.5f, 160.0f }, { -181.0f, 165.0f }, { -193.5f, 165.0f }, { -204.0f, 156.0f } } },
            // rf_18_cavern: 1487 sq yd, 737 yd from the entrance, opening onto rf_23_cavern
            { "rf_18_cavern", -25.02f, { -239.1f, 93.0f }, { -264.5f, 96.5f }, 737.0f,
                { { -282.0f, 93.5f }, { -262.5f, 79.0f }, { -247.5f, 91.0f }, { -247.0f, 95.5f },
                  { -256.5f, 112.5f }, { -261.0f, 114.0f }, { -268.5f, 110.5f }, { -282.0f, 97.5f } } },
            // rf_20_cavern: 1354 sq yd, 767 yd from the entrance, opening onto rf_27_cavern
            { "rf_20_cavern", -18.72f, { -214.8f, 153.8f }, { -243.5f, 153.0f }, 767.0f,
                { { -258.0f, 146.5f }, { -246.0f, 137.5f }, { -235.5f, 141.0f }, { -229.0f, 147.5f },
                  { -228.0f, 156.0f }, { -236.0f, 167.0f }, { -248.5f, 167.5f }, { -259.0f, 155.0f } } },
            // rf_14_cavern: 1122 sq yd, 808 yd from the entrance, opening onto rf_18_cavern
            { "rf_14_cavern", -25.43f, { -298.1f, 109.4f }, { -287.0f, 148.0f }, 808.0f,
                { { -304.5f, 144.0f }, { -301.0f, 137.0f }, { -288.0f, 137.5f }, { -280.0f, 145.0f },
                  { -280.0f, 151.0f }, { -285.0f, 160.5f }, { -298.5f, 161.5f }, { -303.5f, 154.0f } } },
            // rf_22_cavern: 1682 sq yd, 813 yd from the entrance, opening onto rf_27_cavern
            { "rf_22_cavern", -24.91f, { -187.1f, 198.9f }, { -230.0f, 210.0f }, 813.0f,
                { { -245.0f, 204.5f }, { -238.5f, 196.5f }, { -223.0f, 196.0f }, { -214.5f, 213.0f },
                  { -216.0f, 217.0f }, { -230.0f, 226.0f }, { -239.5f, 222.5f }, { -245.5f, 213.5f } } },
            // rf_25_cavern: 1635 sq yd, 819 yd from the entrance, opening onto rf_44_chamber
            { "rf_25_cavern", -12.94f, { -115.3f, 39.4f }, { -187.0f, 56.0f }, 819.0f,
                { { -218.5f, 53.5f }, { -206.5f, 46.5f }, { -201.5f, 47.0f }, { -182.5f, 55.5f },
                  { -180.5f, 62.0f }, { -209.0f, 70.5f }, { -209.5f, 70.5f }, { -218.5f, 57.5f } } },
            // rf_15_cavern: 1513 sq yd, 851 yd from the entrance, opening onto rf_22_cavern
            { "rf_15_cavern", -25.82f, { -259.2f, 210.1f }, { -283.5f, 204.0f }, 851.0f,
                { { -306.5f, 197.5f }, { -299.0f, 190.0f }, { -290.0f, 190.5f }, { -267.0f, 213.0f },
                  { -273.0f, 225.0f }, { -277.5f, 226.5f }, { -298.0f, 223.0f }, { -305.5f, 213.5f } } },
            // rf_9_cavern: 2007 sq yd, 908 yd from the entrance, opening onto rf_22_cavern
            { "rf_9_cavern", -12.42f, { -251.6f, 239.4f }, { -307.5f, 253.0f }, 908.0f,
                { { -319.5f, 246.0f }, { -311.5f, 240.0f }, { -299.0f, 242.0f }, { -295.0f, 247.5f },
                  { -299.0f, 262.0f }, { -308.0f, 263.0f }, { -311.0f, 262.0f }, { -321.0f, 255.0f } } },
            // rf_8_deep: 3746 sq yd, 932 yd from the entrance, opening onto r12
            { "rf_8_deep", -21.98f, { -306.2f, 222.9f }, { -361.5f, 209.5f }, 932.0f,
                { { -388.5f, 197.5f }, { -375.0f, 180.5f }, { -359.0f, 178.0f }, { -333.5f, 203.0f },
                  { -331.5f, 216.0f }, { -334.0f, 222.5f }, { -371.0f, 226.5f }, { -382.5f, 213.5f } } },
            // rf_7_cavern: 1570 sq yd, 962 yd from the entrance, opening onto rf_9_cavern
            { "rf_7_cavern", -5.26f, { -336.8f, 255.5f }, { -362.5f, 252.0f }, 962.0f,
                { { -394.5f, 235.0f }, { -384.5f, 232.5f }, { -374.5f, 236.0f }, { -350.0f, 252.5f },
                  { -350.5f, 262.0f }, { -373.5f, 272.0f }, { -380.5f, 270.5f }, { -396.0f, 255.0f } } },
            // rf_0_chamber: 494 sq yd, 1041 yd from the entrance, opening onto r2
            { "rf_0_chamber", 5.26f, { -404.1f, 213.1f }, { -410.0f, 190.0f }, 1041.0f,
                { { -413.5f, 182.0f }, { -412.5f, 179.0f }, { -407.5f, 179.0f }, { -403.0f, 180.0f },
                  { -402.5f, 183.0f }, { -402.5f, 197.0f }, { -409.0f, 199.0f }, { -413.5f, 198.5f } } },
            // rf_1_cavern: 1488 sq yd, 1081 yd from the entrance, opening onto rf_0_chamber
            { "rf_1_cavern", 7.74f, { -405.8f, 179.1f }, { -396.5f, 149.0f }, 1081.0f,
                { { -411.0f, 171.0f }, { -408.5f, 150.5f }, { -393.5f, 133.5f }, { -388.5f, 130.5f },
                  { -379.0f, 131.5f }, { -369.5f, 144.5f }, { -373.0f, 152.0f }, { -400.0f, 176.5f } } },
    };
    static char const* const FRONT[] = { "rf_10_passage", "rf_19_nook", "rf_24_chamber", "rf_38_cavern" };
    for (SeekRoom& room : rooms)
        room.Front = std::any_of(std::begin(FRONT), std::end(FRONT), [&room](char const* name)
        {
            return room.Name == name;
        });
    return rooms;
}

std::vector<Position> Animus::Curriculum::SeekTables::DeadminesHallways()
{
    // 106 points: the entrance, then the hallway regions' cores every 3 yd.
    return {
            { -16.4f, -383.07f, 61.78f, 1.86f },
            { -198.0f, -441.0f, 53.42f, 0.0f }, { -198.0f, -438.0f, 53.27f, 0.0f }, { -198.0f, -435.0f, 53.46f, 0.0f },
            { -198.0f, -432.0f, 53.95f, 0.0f }, { -198.0f, -429.0f, 54.18f, 0.0f }, { -195.0f, -441.0f, 53.52f, 0.0f },
            { -195.0f, -438.0f, 53.40f, 0.0f }, { -195.0f, -435.0f, 53.34f, 0.0f }, { -195.0f, -432.0f, 53.77f, 0.0f },
            { -195.0f, -429.0f, 54.01f, 0.0f }, { -192.0f, -438.0f, 53.38f, 0.0f }, { -192.0f, -435.0f, 53.31f, 0.0f },
            { -192.0f, -432.0f, 53.63f, 0.0f }, { -192.0f, -429.0f, 53.88f, 0.0f }, { -192.0f, -426.0f, 54.03f, 0.0f },
            { -189.0f, -438.0f, 53.36f, 0.0f }, { -189.0f, -435.0f, 53.39f, 0.0f }, { -189.0f, -432.0f, 53.74f, 0.0f },
            { -189.0f, -429.0f, 53.84f, 0.0f }, { -189.0f, -426.0f, 54.00f, 0.0f }, { -186.0f, -438.0f, 53.67f, 0.0f },
            { -186.0f, -435.0f, 53.72f, 0.0f }, { -186.0f, -432.0f, 53.79f, 0.0f }, { -186.0f, -429.0f, 53.90f, 0.0f },
            { -186.0f, -426.0f, 54.00f, 0.0f }, { -186.0f, -423.0f, 54.22f, 0.0f }, { -183.0f, -441.0f, 54.25f, 0.0f },
            { -183.0f, -438.0f, 54.14f, 0.0f }, { -183.0f, -435.0f, 54.20f, 0.0f }, { -183.0f, -432.0f, 54.31f, 0.0f },
            { -183.0f, -429.0f, 54.42f, 0.0f }, { -159.0f, -399.0f, 56.33f, 0.0f }, { -156.0f, -399.0f, 56.40f, 0.0f },
            { -153.0f, -399.0f, 56.59f, 0.0f }, { -150.0f, -402.0f, 56.87f, 0.0f }, { -147.0f, -402.0f, 57.05f, 0.0f },
            { -144.0f, -402.0f, 57.55f, 0.0f }, { -141.0f, -402.0f, 58.18f, 0.0f }, { -138.0f, -408.0f, 58.42f, 0.0f },
            { -138.0f, -405.0f, 58.38f, 0.0f }, { -138.0f, -402.0f, 58.47f, 0.0f }, { -135.0f, -414.0f, 58.28f, 0.0f },
            { -135.0f, -411.0f, 58.13f, 0.0f }, { -135.0f, -408.0f, 58.09f, 0.0f }, { -135.0f, -405.0f, 57.92f, 0.0f },
            { -135.0f, -402.0f, 58.21f, 0.0f }, { -123.0f, -417.0f, 57.72f, 0.0f }, { -123.0f, -414.0f, 58.22f, 0.0f },
            { -123.0f, -411.0f, 58.03f, 0.0f }, { -123.0f, -390.0f, 58.89f, 0.0f }, { -123.0f, -387.0f, 59.15f, 0.0f },
            { -123.0f, -384.0f, 59.27f, 0.0f }, { -120.0f, -399.0f, 57.53f, 0.0f }, { -120.0f, -396.0f, 57.22f, 0.0f },
            { -120.0f, -393.0f, 57.01f, 0.0f }, { -120.0f, -390.0f, 58.47f, 0.0f }, { -120.0f, -387.0f, 58.88f, 0.0f },
            { -120.0f, -384.0f, 59.33f, 0.0f }, { -117.0f, -393.0f, 56.51f, 0.0f }, { -111.0f, -381.0f, 57.55f, 0.0f },
            { -111.0f, -378.0f, 58.18f, 0.0f }, { -108.0f, -381.0f, 57.08f, 0.0f }, { -108.0f, -378.0f, 57.64f, 0.0f },
            { -105.0f, -399.0f, 59.29f, 0.0f }, { -105.0f, -393.0f, 57.48f, 0.0f }, { -105.0f, -390.0f, 57.32f, 0.0f },
            { -105.0f, -387.0f, 57.16f, 0.0f }, { -105.0f, -384.0f, 57.12f, 0.0f }, { -105.0f, -381.0f, 57.24f, 0.0f },
            { -105.0f, -378.0f, 57.69f, 0.0f }, { -102.0f, -399.0f, 58.78f, 0.0f }, { -102.0f, -396.0f, 58.10f, 0.0f },
            { -102.0f, -393.0f, 57.58f, 0.0f }, { -102.0f, -390.0f, 57.68f, 0.0f }, { -102.0f, -387.0f, 57.85f, 0.0f },
            { -102.0f, -384.0f, 57.95f, 0.0f }, { -102.0f, -381.0f, 57.78f, 0.0f }, { -102.0f, -378.0f, 57.92f, 0.0f },
            { -99.0f, -399.0f, 58.49f, 0.0f }, { -99.0f, -378.0f, 58.38f, 0.0f }, { -96.0f, -399.0f, 58.06f, 0.0f },
            { -84.0f, -375.0f, 56.54f, 0.0f }, { -81.0f, -378.0f, 56.03f, 0.0f }, { -81.0f, -375.0f, 55.88f, 0.0f },
            { -78.0f, -378.0f, 55.20f, 0.0f }, { -78.0f, -375.0f, 55.28f, 0.0f }, { -75.0f, -378.0f, 55.02f, 0.0f },
            { -75.0f, -375.0f, 54.76f, 0.0f }, { -66.0f, -399.0f, 54.44f, 0.0f }, { -66.0f, -396.0f, 54.57f, 0.0f },
            { -63.0f, -399.0f, 53.92f, 0.0f }, { -63.0f, -396.0f, 54.05f, 0.0f }, { -60.0f, -402.0f, 53.76f, 0.0f },
            { -60.0f, -399.0f, 53.66f, 0.0f }, { -60.0f, -396.0f, 53.83f, 0.0f }, { -60.0f, -381.0f, 53.94f, 0.0f },
            { -60.0f, -378.0f, 53.98f, 0.0f }, { -57.0f, -402.0f, 54.20f, 0.0f }, { -57.0f, -384.0f, 54.20f, 0.0f },
            { -57.0f, -381.0f, 54.07f, 0.0f }, { -48.0f, -378.0f, 55.26f, 0.0f }, { -48.0f, -375.0f, 55.48f, 0.0f },
            { -45.0f, -378.0f, 55.51f, 0.0f }, { -45.0f, -375.0f, 55.24f, 0.0f }, { -42.0f, -375.0f, 55.60f, 0.0f }
    };
}

std::vector<Animus::Curriculum::SeekRoom> Animus::Curriculum::SeekTables::DeadminesRooms()
{
    // In order of walking distance from the entrance. Held out: the sweep plays every room, so Front is drawn never;
    // the one room opening onto the hallway is marked for the episode info's sake.
    std::vector<SeekRoom> rooms = {
            // dm_11_passage: 303 sq yd, 227 yd from the entrance, opening onto the hallway (front)
            { "dm_11_passage", 54.23f, { -191.3f, -451.1f }, { -203.5f, -452.5f }, 227.0f,
                { { -208.5f, -461.0f }, { -206.5f, -462.0f }, { -202.5f, -462.0f }, { -194.0f, -453.5f },
                  { -194.0f, -450.0f }, { -199.5f, -449.0f }, { -202.0f, -449.0f }, { -205.5f, -450.0f } } },
            // dm_13_lumber: 764 sq yd, 262 yd from the entrance, opening onto dm_11_passage
            { "dm_13_lumber", 53.51f, { -191.1f, -462.4f }, { -191.5f, -492.0f }, 262.0f,
                { { -198.5f, -486.5f }, { -195.0f, -501.0f }, { -191.5f, -504.5f }, { -183.0f, -503.0f },
                  { -179.5f, -499.0f }, { -179.0f, -488.0f }, { -188.0f, -478.5f }, { -196.0f, -479.0f } } },
            // dm_20_mill: 297 sq yd, 308 yd from the entrance, opening onto dm_13_lumber
            { "dm_20_mill", 52.25f, { -164.6f, -508.0f }, { -154.0f, -521.0f }, 308.0f,
                { { -156.5f, -526.5f }, { -151.0f, -528.0f }, { -149.0f, -527.0f }, { -149.0f, -522.0f },
                  { -150.0f, -520.0f }, { -153.5f, -514.5f }, { -155.0f, -513.5f }, { -156.0f, -513.5f } } },
            // dm_7_cavern: 1302 sq yd, 320 yd from the entrance, opening onto dm_13_lumber
            { "dm_7_cavern", 49.29f, { -193.3f, -503.4f }, { -232.5f, -477.5f }, 320.0f,
                { { -250.5f, -477.5f }, { -249.0f, -484.5f }, { -245.5f, -489.5f }, { -227.0f, -494.0f },
                  { -219.0f, -489.0f }, { -221.5f, -485.0f }, { -229.5f, -478.5f }, { -244.5f, -474.0f } } },
            // dm_1_cavern: 1492 sq yd, 378 yd from the entrance, opening onto dm_7_cavern
            { "dm_1_cavern", 50.12f, { -264.3f, -482.2f }, { -288.0f, -499.5f }, 378.0f,
                { { -298.0f, -509.0f }, { -294.0f, -512.0f }, { -288.0f, -513.5f }, { -281.5f, -511.5f },
                  { -276.0f, -493.5f }, { -279.5f, -488.5f }, { -285.5f, -486.0f }, { -298.0f, -490.0f } } },
            // dm_2_chamber: 647 sq yd, 431 yd from the entrance, opening onto dm_1_cavern
            { "dm_2_chamber", 49.45f, { -290.2f, -538.3f }, { -289.0f, -555.0f }, 431.0f,
                { { -297.5f, -585.0f }, { -287.5f, -574.5f }, { -282.5f, -553.5f }, { -283.5f, -546.0f },
                  { -289.0f, -538.5f }, { -292.0f, -538.5f }, { -297.5f, -545.5f }, { -298.0f, -553.0f } } },
            // dm_0_passage: 210 sq yd, 483 yd from the entrance, opening onto dm_2_chamber
            { "dm_0_passage", 47.40f, { -298.2f, -586.0f }, { -295.0f, -601.0f }, 483.0f,
                { { -298.0f, -607.0f }, { -294.0f, -608.5f }, { -286.5f, -602.0f }, { -286.5f, -601.5f },
                  { -287.5f, -600.0f }, { -288.5f, -599.0f }, { -292.5f, -595.5f }, { -298.0f, -592.5f } } },
            // dm_6_chamber: 530 sq yd, 518 yd from the entrance, opening onto r4
            { "dm_6_chamber", 50.77f, { -277.8f, -583.5f }, { -260.5f, -573.5f }, 518.0f,
                { { -275.5f, -579.0f }, { -272.5f, -584.0f }, { -256.5f, -588.0f }, { -250.0f, -585.5f },
                  { -245.0f, -577.0f }, { -248.0f, -571.5f }, { -263.0f, -573.0f }, { -276.0f, -575.5f } } },
            // dm_9_nook: 451 sq yd, 678 yd from the entrance, opening onto dm_8_forge
            { "dm_9_nook", 20.98f, { -207.1f, -587.6f }, { -206.0f, -594.5f }, 678.0f,
                { { -214.0f, -592.5f }, { -206.5f, -601.5f }, { -200.5f, -601.0f }, { -197.5f, -587.0f },
                  { -199.5f, -585.0f }, { -211.5f, -586.0f }, { -213.5f, -588.5f }, { -214.0f, -590.0f } } },
            // dm_8_forge: 4860 sq yd, 700 yd from the entrance, opening onto dm_6_chamber
            { "dm_8_forge", 20.98f, { -241.2f, -578.6f }, { -197.5f, -568.0f }, 700.0f,
                { { -208.0f, -572.5f }, { -199.5f, -579.5f }, { -194.5f, -579.5f }, { -187.5f, -574.5f },
                  { -186.5f, -568.0f }, { -194.5f, -560.0f }, { -203.0f, -557.5f }, { -207.5f, -561.5f } } },
            // dm_19_foundry: 647 sq yd, 732 yd from the entrance, opening onto dm_8_forge
            { "dm_19_foundry", 19.31f, { -166.3f, -580.4f }, { -156.5f, -578.0f }, 732.0f,
                { { -166.5f, -581.5f }, { -152.0f, -589.5f }, { -136.5f, -586.5f }, { -134.5f, -578.0f },
                  { -135.5f, -572.0f }, { -139.0f, -570.5f }, { -152.0f, -570.0f }, { -166.5f, -579.5f } } },
            // dm_22_chamber: 929 sq yd, 779 yd from the entrance, opening onto dm_19_foundry
            { "dm_22_chamber", 13.35f, { -131.7f, -589.7f }, { -132.5f, -612.0f }, 779.0f,
                { { -137.0f, -625.0f }, { -130.0f, -625.5f }, { -120.5f, -619.0f }, { -119.5f, -616.5f },
                  { -123.5f, -611.5f }, { -132.5f, -605.5f }, { -135.5f, -606.5f }, { -137.0f, -614.5f } } },
            // dm_26_passage: 260 sq yd, 812 yd from the entrance, opening onto dm_22_chamber
            { "dm_26_passage", 13.13f, { -126.2f, -631.7f }, { -125.0f, -642.0f }, 812.0f,
                { { -130.5f, -640.0f }, { -130.0f, -640.5f }, { -122.0f, -644.0f }, { -120.5f, -644.0f },
                  { -120.0f, -639.0f }, { -122.0f, -637.0f }, { -125.0f, -636.0f }, { -130.0f, -639.0f } } },
            // dm_31_chamber: 525 sq yd, 826 yd from the entrance, opening onto dm_26_passage
            { "dm_31_chamber", 7.79f, { -113.2f, -642.2f }, { -103.0f, -644.5f }, 826.0f,
                { { -114.0f, -658.0f }, { -101.0f, -670.5f }, { -99.0f, -669.0f }, { -95.5f, -661.5f },
                  { -95.0f, -653.5f }, { -104.5f, -640.5f }, { -110.0f, -641.0f }, { -114.0f, -646.5f } } },
            // dm_35_cannon: 1045 sq yd, 872 yd from the entrance, opening onto dm_31_chamber
            { "dm_35_cannon", 8.29f, { -100.1f, -670.3f }, { -103.5f, -692.0f }, 872.0f,
                { { -106.5f, -684.0f }, { -102.5f, -723.0f }, { -92.0f, -726.0f }, { -88.0f, -724.0f },
                  { -89.0f, -679.5f }, { -99.0f, -669.5f }, { -101.0f, -671.0f }, { -106.5f, -678.5f } } },
            // dm_41_chamber: 421 sq yd, 876 yd from the entrance, opening onto dm_35_cannon
            { "dm_41_chamber", 2.73f, { -87.4f, -679.2f }, { -81.5f, -684.5f }, 876.0f,
                { { -86.0f, -687.5f }, { -82.0f, -691.5f }, { -81.5f, -691.5f }, { -70.5f, -683.5f },
                  { -70.5f, -681.0f }, { -82.0f, -677.0f }, { -83.5f, -678.5f }, { -86.0f, -687.0f } } },
            // dm_25_cavern: 1550 sq yd, 879 yd from the entrance, opening onto dm_35_cannon
            { "dm_25_cavern", 9.75f, { -110.7f, -685.1f }, { -120.0f, -682.5f }, 879.0f,
                { { -126.0f, -693.5f }, { -125.0f, -698.0f }, { -121.5f, -698.0f }, { -115.5f, -695.5f },
                  { -114.5f, -691.5f }, { -115.0f, -687.5f }, { -118.0f, -682.5f }, { -126.0f, -683.0f } } },
            // dm_36_passage: 292 sq yd, 879 yd from the entrance, opening onto dm_41_chamber
            { "dm_36_passage", 2.51f, { -84.2f, -689.8f }, { -86.5f, -688.0f }, 879.0f,
                { { -101.5f, -693.0f }, { -98.5f, -698.5f }, { -95.5f, -701.0f }, { -92.0f, -700.5f },
                  { -91.0f, -700.0f }, { -82.0f, -692.0f }, { -86.5f, -687.5f }, { -101.5f, -692.5f } } },
            // dm_53_chamber: 556 sq yd, 917 yd from the entrance, opening onto dm_41_chamber
            { "dm_53_chamber", 7.41f, { -62.6f, -676.3f }, { -38.5f, -683.0f }, 917.0f,
                { { -50.0f, -680.0f }, { -48.0f, -681.5f }, { -42.5f, -684.5f }, { -40.5f, -684.5f },
                  { -36.5f, -683.0f }, { -39.5f, -680.5f }, { -48.5f, -677.5f }, { -49.0f, -677.5f } } },
            // dm_54_nook: 710 sq yd, 951 yd from the entrance, opening onto dm_53_chamber
            { "dm_54_nook", 2.89f, { -40.2f, -688.2f }, { -9.5f, -694.0f }, 951.0f,
                { { -21.5f, -694.0f }, { -16.5f, -695.0f }, { 2.0f, -695.5f }, { 2.5f, -694.0f },
                  { 1.5f, -693.0f }, { 0.5f, -692.5f }, { -18.5f, -692.0f }, { -21.0f, -692.0f } } },
            // dm_50_tunnel: 564 sq yd, 952 yd from the entrance, opening onto dm_35_cannon
            { "dm_50_tunnel", 8.98f, { -77.5f, -729.4f }, { -47.0f, -726.0f }, 952.0f,
                { { -69.5f, -733.5f }, { -59.5f, -734.5f }, { -32.0f, -730.5f }, { -29.5f, -726.0f },
                  { -37.5f, -721.5f }, { -46.5f, -721.0f }, { -59.0f, -723.0f }, { -70.5f, -729.0f } } },
            // dm_15_chamber: 704 sq yd, 976 yd from the entrance, opening onto dm_25_cavern
            { "dm_15_chamber", 6.42f, { -170.9f, -686.6f }, { -180.0f, -715.5f }, 976.0f,
                { { -186.0f, -721.0f }, { -183.5f, -721.0f }, { -182.5f, -720.5f }, { -180.0f, -719.0f },
                  { -180.0f, -713.0f }, { -182.5f, -712.5f }, { -184.5f, -713.0f }, { -186.0f, -713.5f } } },
            // dm_14_cavern: 1116 sq yd, 985 yd from the entrance, opening onto dm_15_chamber
            { "dm_14_cavern", 5.45f, { -182.5f, -721.5f }, { -187.5f, -730.0f }, 985.0f,
                { { -188.5f, -737.0f }, { -184.0f, -730.5f }, { -183.5f, -729.5f }, { -183.5f, -723.5f },
                  { -185.0f, -722.5f }, { -186.0f, -722.5f }, { -187.5f, -728.5f }, { -188.5f, -735.0f } } },
            // dm_55_dock: 1911 sq yd, 1010 yd from the entrance, opening onto dm_50_tunnel
            { "dm_55_dock", 9.03f, { -30.5f, -728.4f }, { 0.5f, -756.5f }, 1010.0f,
                { { -13.0f, -748.0f }, { -8.0f, -770.0f }, { 0.5f, -772.5f }, { 4.0f, -772.0f },
                  { 8.5f, -768.0f }, { 8.5f, -752.5f }, { -2.0f, -743.0f }, { -5.0f, -741.5f } } },
            // dm_44_chamber: 555 sq yd, 1110 yd from the entrance, opening onto dm_55_dock
            { "dm_44_chamber", 17.59f, { -30.7f, -794.9f }, { -71.5f, -780.0f }, 1110.0f,
                { { -94.0f, -784.0f }, { -90.0f, -788.0f }, { -44.5f, -786.0f }, { -44.0f, -785.0f },
                  { -48.0f, -782.5f }, { -57.5f, -780.0f }, { -75.5f, -779.5f }, { -87.5f, -782.0f } } },
            // dm_45_cove: 822 sq yd, 1153 yd from the entrance, opening onto dm_55_dock
            { "dm_45_cove", 16.84f, { -17.6f, -825.5f }, { -71.0f, -848.0f }, 1153.0f,
                { { -77.5f, -860.0f }, { -67.5f, -863.5f }, { -59.5f, -859.0f }, { -57.5f, -856.5f },
                  { -57.0f, -855.5f }, { -62.0f, -847.5f }, { -76.5f, -846.5f }, { -77.5f, -856.0f } } },
            // dm_23_chamber: 722 sq yd, 1159 yd from the entrance, opening onto dm_44_chamber
            { "dm_23_chamber", 17.18f, { -92.1f, -786.4f }, { -120.5f, -790.5f }, 1159.0f,
                { { -131.5f, -793.5f }, { -125.0f, -803.5f }, { -123.0f, -804.0f }, { -116.5f, -802.5f },
                  { -108.5f, -796.5f }, { -106.5f, -790.5f }, { -110.0f, -783.5f }, { -131.0f, -787.5f } } },
            // dm_38_chamber: 624 sq yd, 1175 yd from the entrance, opening onto dm_23_chamber
            { "dm_38_chamber", 26.40f, { -101.2f, -780.2f }, { -84.0f, -784.0f }, 1175.0f,
                { { -93.0f, -788.0f }, { -87.0f, -793.5f }, { -76.5f, -790.5f }, { -74.0f, -784.0f },
                  { -74.5f, -781.5f }, { -82.0f, -775.5f }, { -90.0f, -776.0f }, { -93.5f, -785.0f } } },
            // dm_37_shore: 367 sq yd, 1190 yd from the entrance, opening onto dm_45_cove
            { "dm_37_shore", 16.92f, { -77.9f, -857.3f }, { -108.0f, -848.5f }, 1190.0f,
                { { -111.0f, -846.5f }, { -105.0f, -850.5f }, { -92.0f, -856.5f }, { -82.5f, -859.0f },
                  { -78.0f, -859.5f }, { -77.5f, -855.0f }, { -80.5f, -852.0f }, { -107.5f, -843.5f } } },
            // dm_24_shore: 295 sq yd, 1197 yd from the entrance, opening onto dm_23_chamber
            { "dm_24_shore", 16.92f, { -128.5f, -813.0f }, { -123.0f, -828.0f }, 1197.0f,
                { { -127.5f, -827.0f }, { -127.0f, -833.5f }, { -118.5f, -841.5f }, { -116.5f, -843.0f },
                  { -112.0f, -844.5f }, { -109.5f, -842.5f }, { -121.5f, -826.0f }, { -127.5f, -818.5f } } },
            // dm_42_bow: 585 sq yd, 1223 yd from the entrance, opening onto dm_38_chamber
            { "dm_42_bow", 39.00f, { -98.8f, -803.0f }, { -75.0f, -789.5f }, 1223.0f,
                { { -85.0f, -797.0f }, { -83.0f, -798.5f }, { -63.0f, -797.0f }, { -53.5f, -792.0f },
                  { -53.5f, -788.0f }, { -63.0f, -786.5f }, { -73.5f, -786.5f }, { -82.0f, -788.5f } } },
            // dm_43_deck: 1086 sq yd, 1280 yd from the entrance, opening onto r51
            { "dm_43_deck", 41.37f, { -56.4f, -808.8f }, { -62.0f, -817.0f }, 1280.0f,
                { { -75.5f, -820.0f }, { -73.0f, -825.5f }, { -62.0f, -831.0f }, { -55.0f, -829.0f },
                  { -55.0f, -811.5f }, { -57.5f, -807.0f }, { -68.0f, -804.5f }, { -73.0f, -808.5f } } },
            // dm_34_nook: 266 sq yd, 1324 yd from the entrance, opening onto dm_43_deck
            { "dm_34_nook", 37.85f, { -92.1f, -823.8f }, { -105.5f, -817.0f }, 1324.0f,
                { { -106.0f, -823.5f }, { -102.5f, -826.5f }, { -100.0f, -825.0f }, { -97.0f, -812.5f },
                  { -97.5f, -811.5f }, { -100.5f, -811.5f }, { -104.5f, -813.5f }, { -106.0f, -817.0f } } },
    };
    for (SeekRoom& room : rooms)
        room.Front = room.Name == "dm_11_passage";
    return rooms;
}
