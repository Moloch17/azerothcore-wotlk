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
 * The curriculum. Every stage extends one earlier stage and seeds from it, keeping the base's blocks it needs and
 * adding its own.
 *
 * **Four phases, in order: movement, classes, parties and raids, PvP -- then one stage that ships.** A class learns
 * to walk before it fights, to fight alone before it fights beside others, and to play the world before it plays
 * against people; the last stage replays all of it, because the stages that learned nothing over their seed in the
 * second full run were evidence that a later stage overwrites an earlier one.
 *
 *   movement   move ─ travel                                          every ground on foot; the mount, the air
 *   classes    ─ rotation ─ duel ─ pack ─ gauntlet ─ life             the kit, a fight, many, the world alone
 *   parties    ─ companion ─ party ─ dungeon ─ world_group ─ raid_pulls ─ raids
 *   pvp        ─ duel_pvp ─ escape ─ stealth ─ arena ─ flag ─ warsong ─ world_shared
 *   ship       ─ ship                                                 every phase at once
 *
 * It is one line, deliberately. A branch is cheaper to train but it ends in several checkpoints, and everything a
 * leaf teaches is thrown away unless the stage exported from is downstream of it -- which is how the drills, the raids
 * and the team stages came to be a dead end. A linear chain ends in one leaf that carries everything. A drill that
 * bought nothing as a stage of its own (tanking, triage, hide, endurance, the dodge field) is an arena of the stage it
 * serves instead: it trains beside the lesson rather than before the stage that overwrites it.
 *
 * Scenario names carry the stage's number (stage1_move ... stage21_ship), model names only its suffix (_move). The
 * move stage is the first: nothing seeds it.
 *
 * A stage's episodes are its arenas (see ArenaDefinition): each episode draws one by weight, so a stage can mix
 * PvE and PvP situations, or three kinds of ground, over the union of their blocks.
 *
 * Adding a stage is one entry here (plus new blocks or encounters only if it needs new features) and a learner
 * config, configs/<name>.yaml -- or configs/<class>/<name>.yaml for a run of a single class, which is how a
 * per-class curriculum sets its own floors.
 */

#include "StageDefinition.h"
#include "Log.h"
#include "AreaDefines.h"
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;

    /// Kalimdor's training ground, shared by every stage that walks it.
    ///
    /// Five regions rather than one, because a spawn point is drawn per episode and what a policy never stands on
    /// it cannot learn to read. Until 2026-09-21 this was eight points in the Barrens, picked by env index, so an
    /// env stood on the same patch of scrub for its whole life and 128 of them saw eight places between them --
    /// which a network can fit instead of learning to read what is in front of it. The Barrens is flat scrub,
    /// Durotar canyon and rock, Mulgore open rolling grass, Dustwallow marsh and broken shore.
    /// Ground the stage's own arenas spawn on.
    ///
    /// Five points that used to be in these lists are not any more, all of them off the navmesh and all found
    /// by standing on them with `forge rays`. They were survivable while a placement that could not be routed
    /// fell back to a straight line: the episode was built, it was measured against the crow's flight, and it
    /// arrived about half the time. Now that the fallback is refused, no objective can be found from them at
    /// all, and four spawn attempts in a row from one is a dead plan -- which is how they were noticed.
    std::vector<Position> KalimdorGround()
    {
        return {
            // The Barrens
            { -872.0f, -2642.0f, 92.0f, 0.0f },   { -2298.0f, -1948.0f, 96.0f, 0.0f },
            { -1967.0f, -2544.0f, 94.0f, 0.0f },  { -2605.0f, -2286.0f, 92.0f, 0.0f },
            { -609.0f, -1614.0f, 94.0f, 0.0f },   { -881.0f, -3221.0f, 92.0f, 0.0f },
            { -3077.0f, -1786.0f, 92.0f, 0.0f },  { -3115.0f, -2352.0f, 94.0f, 0.0f },
            // Northern Barrens
            { -652.0f, -2060.0f, 87.0f, 0.0f },   { -767.0f, -2062.0f, 81.0f, 0.0f },
            { -579.6f, -2070.5f, 54.9f, 0.0f },   { -2068.0f, -2106.0f, 93.0f, 0.0f },
            { -1942.0f, -1985.0f, 92.0f, 0.0f },  { -1991.0f, -2090.0f, 92.0f, 0.0f },
            // Durotar
            { -49.4f, -4313.6f, 68.7f, 0.0f },
            { -107.5f, -4302.0f, 61.7f, 0.0f },    { 642.0f, -4185.0f, 15.0f, 0.0f },
            { 633.0f, -4298.0f, 18.0f, 0.0f },
            // Mulgore
            { -1225.2f, 106.6f, 131.4f, 0.0f },
            { -1210.0f, -93.0f, 163.0f, 0.0f },
            // Dustwallow Marsh
            { -2631.0f, -3607.0f, 42.0f, 0.0f },  { -2751.0f, -3660.0f, 39.0f, 0.0f },
            { -2851.0f, -3650.0f, 33.0f, 0.0f },  { -2987.0f, -3940.0f, 39.0f, 0.0f },
        };
    }

    /// The banks of Stonebull Lake in Mulgore, for the move stage's lakebeds (its depths and chain arenas).
    ///
    /// A dive needs water six to forty yards deep within reach of a shore the seat can stand on, and the Barrens
    /// oases stage1_move swims in are under three yards deep everywhere (see the water arena's note): every dive
    /// episode built there fell back to a plain trip. Stonebull is 33 yd deep in the middle with banks that slope
    /// in at the water line (surface z -15.0, ground here -11 to -15), and nothing worse than prairie wolves and
    /// stalkers on its shores. Each point was chosen from the map tiles (var/lakes/sim.py) as dry ground from which
    /// a tenth to a third of uniformly drawn 20-120 yd objectives land on a bed under 6-40 yd of water, so the
    /// placer's 192 attempts find one every time; the ground z is the tile's own.
    std::vector<Position> StonebullShore()
    {
        return {
            { -1946.0f, -558.0f, -11.9f, 0.0f }, { -1954.0f, -521.0f, -11.1f, 0.0f },
            { -2192.0f, -712.0f, -14.5f, 0.0f }, { -2192.0f, -571.0f, -14.9f, 0.0f },
            { -2196.0f, -175.0f, -13.1f, 0.0f }, { -2254.0f, -137.0f, -10.8f, 0.0f },
        };
    }

    /// The shore of Lake Elune'ara in Moonglade, held out from the dive drills: a lake the seat never trained on,
    /// with the same kind of bed (up to 66 yd deep, so the 6-40 yd band is a ring some way in) and only critters
    /// on its banks. Chosen the same way as StonebullShore: two dry points scoring 0.44 and 0.41, on the south and
    /// west banks.
    std::vector<Position> EluneAraShore()
    {
        return {
            { 7675.0f, -2775.0f, 454.5f, 0.0f }, { 7508.0f, -2617.0f, 453.3f, 0.0f },
        };
    }

    /// Map 560's training ground, shared by the drills that fight people on it (evade, hide, stealth).
    ///
    /// One instance is a small world, so the split is by district rather than by region: the southern approaches
    /// train, the northern farmland is kept back. Weaker separation than Kalimdor's -- a seat could carry
    /// something across a thousand yards that it could not carry across a continent -- and it is what the map
    /// allows. What it still tests is ground the weights were never updated against.
    std::vector<Position> HillsbradGround()
    {
        return {
            { 2161.0f, 232.0f, 57.0f, 0.0f },   { 2168.0f, 129.0f, 78.0f, 0.0f },
            { 2067.0f, 109.0f, 55.0f, 0.0f },   { 2161.0f, 36.0f, 62.0f, 0.0f },
            { 2637.0f, 717.0f, 57.0f, 0.0f },   { 2528.0f, 708.0f, 56.0f, 0.0f },
        };
    }

    /// THE CONTROL GROUND on map 560: the northern farmland, which no training episode of these stages stands on.
    std::vector<Position> HillsbradControl()
    {
        return {
            { 1803.0f, 1071.0f, 12.0f, 0.0f },  { 1907.0f, 1093.0f, 23.0f, 0.0f },
            { 1822.0f, 983.0f, 14.0f, 0.0f },   { 1884.0f, 968.0f, 14.0f, 0.0f },
        };
    }

    /// THE CONTROL GROUND on Kalimdor: where scored episodes stand, and where training never does.
    ///
    /// Three regions in no training list, chosen for being as far from water as the map allows -- 1,500 to 5,000
    /// yards from the nearest water-dwelling creature, which is the best proxy for "dry" the world data offers.
    /// The first attempt used Teldrassil and the Azshara coast and the seats swam: 21 seconds an episode in the
    /// open arena and 33 in the broken one, against 0.03 on the ground they train on. A control that is wet where
    /// training is dry measures the coastline, not the policy.
    ///
    /// No weight is ever updated against this ground, so `arrived` and `saved` at a gate are claims about the seat
    /// rather than about how many times it has seen the Barrens. If these track the training numbers the seat is
    /// reading terrain; if they fall away, it had learned the places.
    std::vector<Position> KalimdorControl()
    {
        // Ground the stage is scored on and never trains on -- and, until it was stood on, the worst of the
        // three lists. Five of its nine points could not be used: three were off the navmesh outright, and two
        // were pockets of 1.57 and 2.26 yards. A control list made of ground a character cannot walk measures
        // the ground rather than the policy, and it was doing so in the direction that makes generalisation
        // look worse than it is.
        //
        // The second thing this list wants is openness, which took a wrong turn to learn. The first set of
        // replacements was chosen the way `broken`'s points are -- roomy but hemmed in -- and the open arena,
        // which has no spawn points of its own and falls through to this list, promptly got worse: 0.1264 of
        // its episodes timed out, then 0.1694. Clearance says there is room to turn round; it says nothing
        // about whether there is anywhere to go. Every point here now has at least five of its eight bearings
        // running the full forty yards, as well as five yards of clearance and a floor underneath, all
        // measured with `forge rays`.
        return {
            // Northern highlands, the far side of the map from every training region
            { -1637.9f, 3082.9f, 31.9f, 0.0f },   { -1168.4f, 2713.1f, 112.1f, 0.0f },
            { -561.0f, 2069.0f, 90.0f, 0.0f },
            // Eastern high ground
            // (3871, -1025, 242) was here and is not: it failed to build an encounter often enough to be a
            // recurring error in the log, and a control list is the last place to keep a point that sometimes
            // cannot produce an episode.
            { 4012.0f, -788.0f, 286.0f, 0.0f },
            // Mid-east plains
            { 1969.6f, -2339.0f, 89.4f, 0.0f },   { 1813.0f, -2424.0f, 93.0f, 0.0f },
            { 1965.0f, -2559.0f, 86.0f, 0.0f },
        };
    }

    /// Broken ground for the move stage's `broken` arena (and the ship stage's walk).
    ///
    /// These cells were chosen by local relief -- the standard deviation of creature-spawn z within a 250-unit cell --
    /// and then, the part that was missing, stood on. Relief on its own selects for the thing that breaks a bot: it
    /// ranks crevices, ledges and cliff faces highest, and several points it produced were places a character cannot
    /// turn round in (0.47 yards of clearance on the Mulgore ridge). At six million steps two such held-out points
    /// were 43% of every timeout in the stage. Every point here is measured with `forge rays`: at least ~4.5 yards of
    /// clearance, and still short reaches on several bearings, so there is something to walk around.
    ///
    /// The cliff feet at the end are low ground under a plateau 30-50 yd up, whose top the route reaches by one ramp
    /// at 1.3-1.8x the straight line: the first format-6 run lost 50 of its 80 held-out failures pacing at a cliff
    /// foot, because nothing it had trained on had taught it to back off and follow the wall to the way up.
    std::vector<Position> BrokenGround()
    {
        return {
            // Mulgore/Barrens ridge, relief 78 over a 179 yard span
            { -1401.0f, -85.0f, 159.0f, 0.0f },
            { -1286.0f, 107.0f, 130.9f, 0.0f },
            // Barrens ridge, relief 64 over 200
            { -454.0f, -2419.0f, 93.0f, 0.0f },
            { -373.0f, -2323.0f, 94.0f, 0.0f },
            // Durotar: canyon and rock
            { -49.4f, -4313.6f, 68.7f, 0.0f },
            { -107.5f, -4302.0f, 61.7f, 0.0f },    { 642.0f, -4185.0f, 15.0f, 0.0f },
            { 633.0f, -4298.0f, 18.0f, 0.0f },
            // Dustwallow Marsh: broken shore
            { -2631.0f, -3607.0f, 42.0f, 0.0f },  { -2751.0f, -3660.0f, 39.0f, 0.0f },
            { -2851.0f, -3650.0f, 33.0f, 0.0f },  { -2987.0f, -3940.0f, 39.0f, 0.0f },
            // Cliff feet
            { -2032.2f, -3618.1f, 22.3f, 0.0f },  // southern Barrens, plateau +40..50, 1.47-2.31x
            { -2563.7f, -3798.6f, 7.0f, 0.0f },   // Barrens/Dustwallow edge, +41..49, 1.03-1.36x
            { 190.8f, -4516.5f, 27.1f, 0.0f },    // Durotar canyon, +30..37, 1.76-2.2x
            { 479.5f, -4658.7f, 41.7f, 0.0f },    // Durotar canyon, +28..34, 1.3-1.77x
        };
    }

    /// The southern Barrens escarpment, relief 47, in no training list: rougher ground held back for scoring.
    std::vector<Position> BrokenControl()
    {
        return {
            { -623.5f, -3166.8f, 91.7f, 0.0f },   { -405.9f, -3207.1f, 186.5f, 0.0f },
            { -441.9f, -3162.0f, 210.3f, 0.0f },
        };
    }

    /// The banks of the Barrens oases -- Lushwater to the north, Stagnant to the south -- on the shore, not in the
    /// pool, taken from the land creatures the oases are ringed with (Kolkar centaurs). The pools are shallow (the
    /// surface is z 30.2 over a bed at 27-28, measured from the map tiles in var/lakes), which is why the dives go
    /// elsewhere. A water crossing and a fight in the lake both stand here.
    std::vector<Position> OasisShore()
    {
        return {
            { -3923.0f, -2981.0f, 31.0f, 0.0f }, { -3952.0f, -2947.0f, 40.0f, 0.0f },
            { -3964.0f, -3068.0f, 39.0f, 0.0f }, { -3879.0f, -3004.0f, 37.0f, 0.0f },
            { -4048.0f, -3051.0f, 43.0f, 0.0f }, { -3985.0f, -2911.0f, 37.0f, 0.0f },
        };
    }

    /// The far side of the same pond, kept back for scoring: of four bodies of water measured, only this one was wide
    /// enough for the way round to be worth avoiding, so a second pond to hold out does not exist yet.
    std::vector<Position> OasisControl()
    {
        return {
            { -4017.0f, -3086.0f, 37.0f, 0.0f }, { -3926.0f, -2911.0f, 39.0f, 0.0f },
        };
    }

    /// Every inn on Kalimdor that areatrigger_tavern names, is on the mesh and whose WMO group says it is inside,
    /// with z corrected from the trigger's centre to the floor Map::GetHeight finds under it; each stood on with
    /// `forge rays`. The night elf inns are open-sided and flagged outdoors, and FindPlace refuses an objective in one.
    /// Clearance 0.71 to 6.63 yards: rooms a seat can touch two walls in.
    std::vector<Position> Inns()
    {
        return {
            { -3182.4f, -2920.8f, 33.56f, 0.0f },  // Brackenwall Village, clearance 1.70
            { -4461.9f, 242.6f, 39.11f, 0.0f },    // Feralas, 2.85
            { -4622.3f, -3172.1f, 34.81f, 0.0f },  // Mudsprocket, 2.84
            { -2366.7f, -346.0f, -8.96f, 0.0f },   // Mulgore, 2.29
            { -1051.4f, -3653.8f, 23.88f, 0.0f },  // The Barrens, 2.71
            { -5477.9f, -2460.3f, 89.28f, 0.0f },  // Thousand Needles, 5.36
            { 6688.0f, -4670.1f, 721.69f, 0.0f },  // Winterspring, 6.63
        };
    }

    /// Rooms no training episode stands in. Desolace is the tightest room found anywhere on the map, and the reset
    /// has never once built an episode there (spawn_drawn and spawn_point say so): its draws all re-roll, which wants
    /// either a shorter objective range than Travel's 8-40 yards or a different room.
    std::vector<Position> InnsControl()
    {
        return {
            { -1596.2f, 3145.3f, 62.53f, 0.0f },   // Desolace, clearance 0.71
            { -3615.5f, -4467.3f, 21.10f, 0.0f },  // Theramore Isle, 3.43
            { -7162.1f, -3845.9f, 9.51f, 0.0f },   // Tanaris, 5.91
        };
    }

    /// Plateau tops above the ground the broken arena trains its cliff feet on, stood on with `forge rays` facing the
    /// edge; the foot below each was routed to with `forge route`. The way round is 1.9-12x the straight line for all
    /// of these, with drops of 11-44 yd; tops whose route came back at 1.0x (a walkable slope) were dropped.
    std::vector<Position> LedgeTops()
    {
        return {
            { -2063.9f, -3645.5f, 66.1f, 0.0f },   // southern Barrens, above (-2032, -3618): 44 yd, 2.2-3.5x
            { -2094.8f, -3644.6f, 72.4f, 0.0f },   // beside it: 11 yd, 1.9-3.4x
            { 394.1f, -4599.2f, 76.2f, 0.0f },     // Durotar canyon, above (480, -4659): 23 yd, 3.3-4.2x
            { 85.4f, -4543.8f, 58.4f, 0.0f },      // Durotar canyon: 18 yd, 4.7x
            { -519.0f, -4076.9f, 69.9f, 0.0f },    // southern Barrens: 27 yd, 6.6x
            { -2379.6f, 459.2f, 76.8f, 0.0f },     // Mulgore: 16-25 yd, 3.5-4.7x
            { -4052.7f, -2145.5f, 90.2f, 0.0f },   // Thousand Needles: 40 yd, 6.5x
            { -4449.9f, -2914.0f, 40.0f, 0.0f },   // Thousand Needles: 16-18 yd, 8.6-12.5x
        };
    }

    /// The southern Barrens escarpment's top, the ground the broken arena holds out at its foot; the deep ones are
    /// past the fall that kills without Slow Fall.
    std::vector<Position> LedgeControl()
    {
        return {
            { -545.9f, -3054.0f, 138.1f, 0.0f },   // 46 yd, 1.9-2.7x
            { -515.9f, -3149.0f, 161.5f, 0.0f },   // 67 yd, 5.3-5.7x
            { -481.2f, -3249.9f, 164.5f, 0.0f },   // 70 yd, 2.9x
        };
    }

    /// Four Outland regions to take off from: Hellfire's broken flats, Zangarmarsh's mushroom basins, Shadowmoon's
    /// ridges at nearly 300 yards of altitude, and Terokkar's low forest.
    std::vector<Position> OutlandGround()
    {
        return {
            // Hellfire Peninsula
            { 170.0f, 2589.0f, 93.0f, 0.0f },     { 169.0f, 2708.0f, 101.0f, 0.0f },
            // Zangarmarsh
            { -3260.0f, 2690.0f, 85.0f, 0.0f },   { -3293.0f, 2832.0f, 125.0f, 0.0f },
            // Shadowmoon Valley
            { -3631.0f, 3741.0f, 298.0f, 0.0f },  { -3721.0f, 3746.0f, 284.0f, 0.0f },
            // Terokkar Forest
            { -1750.0f, 5154.0f, -37.0f, 0.0f },  { -1730.0f, 5282.0f, -32.0f, 0.0f },
        };
    }

    /// THE CONTROL GROUND for flight, and it has to be ground a flying mount may leave. It used to be Eversong Woods
    /// and the Draenei isles, which are not flyable (AreaTableEntry::IsFlyable is `flags & AREA_FLAG_OUTLAND`): every
    /// one of 2048 evaluation episodes began where the seat could not take off, and `flew` read exactly 0 for the
    /// whole stage. Nagrand instead: Outland, so flyable, and none of the four training zones.
    std::vector<Position> NagrandControl()
    {
        return {
            { -850.6f, 6517.2f, 172.6f, 0.0f },   { -842.4f, 6578.1f, 172.7f, 0.0f },
            { -652.9f, 6576.9f, 170.4f, 0.0f },   { -685.5f, 6609.0f, 176.6f, 0.0f },
            { -533.9f, 8870.4f, 209.0f, 0.0f },   { -974.2f, 8136.0f, -93.8f, 0.0f },
        };
    }

    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        // ---------------------------------------------------------------------------------------------------------
        // Phase 1: movement. Nothing to kill; the feet, on every kind of ground, then the mount and the air.
        // ---------------------------------------------------------------------------------------------------------

        // Every terrain at once, every update. The first run trained these as four stages (move, indoor, jump, dive),
        // each extending move and none extending another, so what one taught the next never saw: the chain ended in
        // one of them and the rest were a dead end. As arenas of one stage every seat meets open ground, broken
        // ground, water, rooms, ledges and lakebeds in the same stretch of training.
        //
        // Mounting is masked (OnFoot), not merely unpaid: what is left is everything a player does before it can
        // ride -- Sprint, Dash, Travel Form, Aspect of the Cheetah, and not stopping -- and a mount is barred in
        // combat, indoors and at low level, which is most of when a bot actually has to get somewhere.
        stages.push_back({
            .Name = "stage1_move",
            .Suffix = "_move",
            .Extends = "",
            .Summary = "a place to get to on foot, on every kind of ground: open, broken, across water, inside, "
                "below a ledge, on a lakebed",
            .Blocks = { Core, Move, Travel, Duel, Forecast, Goal },
            .Arenas = {
                // Both clocks are the same: the difference between open and broken is the terrain.
                { .Name = "open", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true },
                { .Name = "broken", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true, .SpawnPoints = BrokenGround(), .HeldOutSpawnPoints = BrokenControl() },
                // Whether to get in at all -- swimming is ~4.7 yd/s against 7 running -- and then swimming in three
                // dimensions once in.
                { .Name = "water", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true, .Water = true, .SpawnPoints = OasisShore(),
                    .HeldOutSpawnPoints = OasisControl() },
                // A doorway off the objective's axis, which open country never asks for: short trips and a short
                // clock, since an inn is twenty to thirty yards across. SpawnScatter varies the opening view.
                { .Name = "rooms", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 90,
                    .OnFoot = true, .Indoors = true, .SpawnPoints = Inns(), .HeldOutSpawnPoints = InnsControl(),
                    .SpawnScatter = 4.0f },
                // Down, where the way round is long and the way down is a fall. The jump drops, the fall after it is
                // the core's own with the core's own damage, and the seat is told how far down the landing is
                // (OBS_JUMP_DROP) and nothing else: what a fall costs is the seat's to learn, and it differs by
                // class. The objective always has a way round on foot, at least LedgeDetour times the straight line.
                { .Name = "ledges", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 120,
                    .OnFoot = true, .SpawnPoints = LedgeTops(), .HeldOutSpawnPoints = LedgeControl(),
                    .Ledges = true },
                // Down into the water: an objective on the bed of a lake under six to forty yards of it, arriving
                // means standing on it, and the breath is the core's own. The chain's lakebeds come thirty to sixty
                // yards on from each other for four minutes -- longer than a breath -- so coming up, or a breathing
                // spell first, is a decision with a price on both sides.
                { .Name = "depths", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true, .SpawnPoints = StonebullShore(), .HeldOutSpawnPoints = EluneAraShore(),
                    .Underwater = true },
                { .Name = "chain", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 240,
                    .OnFoot = true, .SpawnPoints = StonebullShore(), .HeldOutSpawnPoints = EluneAraShore(),
                    .Underwater = true, .Checkpoints = true },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
        });

        // Getting somewhere far, now with a mount and wings: the ground trip in the Barrens, and flights in Outland
        // (the flying arenas are on a map of their own, ArenaDefinition::MapId, since only Outland flies here). The
        // dodge drill that used to sit before this stage is gone: fire with nothing to fight is not a decision, and
        // it stayed flat at -0.1; stepping out of fire is now an arena of the pack stage, under a pack.
        stages.push_back({
            .Name = "stage2_travel",
            .Suffix = "_travel",
            .Extends = "stage1_move",
            .Summary = "a place far off: mount when it pays, fly over what is in the way, arrive on foot",
            .Blocks = { Core, Move, Travel, Duel, Forecast, Goal },
            .Arenas = {
                // 60-320 yd by path. The seat walks the whole trip itself: a route walked by the engine is the
                // engine navigating, and the policy pressed it in most of its episodes when it was offered.
                { .Name = "travel", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .MinLevel = 20 },
                // Nagrand-sized trips, 350-700 yd. `flight` places its objective anywhere dry, which a ground ride
                // often reaches; `flight_air` only where the ground route does not (a plateau, a floating island),
                // with the ground mount masked, so the wings are the way.
                { .Name = "flight", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 180,
                    .Flying = true, .SpawnPoints = OutlandGround(), .MapId = MAP_OUTLAND, .MinLevel = 60,
                    .HeldOutSpawnPoints = NagrandControl() },
                { .Name = "flight_air", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 180,
                    .Flying = true, .AirOnly = true, .SpawnPoints = OutlandGround(), .MapId = MAP_OUTLAND,
                    .MinLevel = 60, .HeldOutSpawnPoints = NagrandControl() },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
        });

        // ---------------------------------------------------------------------------------------------------------
        // Phase 2: classes. One character playing its class: its kit, then fights, then its life in the world.
        // ---------------------------------------------------------------------------------------------------------

        // The kit with nothing fighting back (DummyEncounter). The first place Component H's intent judgement is the
        // whole lesson: there is no fight to survive and no clock to beat, only output and what it cost, so every
        // class learns its buttons with purpose before it learns to live through a fight. Every class plays every
        // drill: a dummy standing still, dummies wandering with more arriving to switch to, one that hits back (the
        // tank's drill, and anyone's under pressure), and damage landing on the seat while it works (the healer's
        // drill on itself -- healing others comes with the party phase, where there is someone to heal).
        stages.push_back({
            .Name = "stage3_rotation",
            .Suffix = "_rotation",
            .Extends = "stage2_travel",
            .Summary = "the kit against dummies: still, moving with adds, hitting back, or with the seat bleeding",
            // Travel stays, as it did through the dodge drill: dropping it would throw away the objective-bearing
            // columns the movement phase trained (block-wise seeding starts a re-added block from scratch).
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Forecast, Goal },
            .Arenas = {
                { .Name = "still", .Weight = 2, .Against = Opposition::Dummy, .EpisodeSeconds = 60,
                    .Drill = DummyDrill::Still },
                { .Name = "moving", .Weight = 2, .Against = Opposition::Dummy, .EpisodeSeconds = 90,
                    .Drill = DummyDrill::Moving },
                { .Name = "hitting", .Weight = 1, .Against = Opposition::Dummy, .EpisodeSeconds = 90,
                    .Drill = DummyDrill::Hitting },
                { .Name = "bleeding", .Weight = 1, .Against = Opposition::Dummy, .EpisodeSeconds = 60,
                    .Drill = DummyDrill::Bleeding },
                // Commanded goals (next-run plan, 3.4): the sim names the goal and pays for reaching it, so the kit
                // is learned as the means to a stated end before any goal is the seat's own choice.
                { .Name = "commanded", .Weight = 1, .Against = Opposition::Dummy, .EpisodeSeconds = 90,
                    .Drill = DummyDrill::Moving, .CommandedGoals = true },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
        });

        // One creature that fights back, on the ground the feet were taught on: the combat root. 90 s: running out
        // of time is a lost fight (Duel.Timeout), and a healer or tank against a creature with twice the usual health
        // needs half a minute to kill it after a few seconds of closing in. A share of fights are in a lake: reach,
        // casting, the pet and whether to go in at all are all different wet.
        stages.push_back({
            .Name = "stage4_duel",
            .Suffix = "_duel",
            .Extends = "stage3_rotation",
            .Summary = "a same-level creature out of aggro range: close in and kill it fast, taking little damage",
            .Blocks = { Core, Move, Travel, Duel, Pet, Forecast, Goal },
            .Arenas = {
                { .Name = "duel", .Weight = 3, .Against = Opposition::Creature, .EpisodeSeconds = 90 },
                { .Name = "lake", .Weight = 1, .Against = Opposition::Creature, .EpisodeSeconds = 90,
                    .Water = true, .SpawnPoints = OasisShore(), .HeldOutSpawnPoints = OasisControl() },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
        });

        // A pack of 2-4, casters included, usually linked: targets, interrupts, crowd control. A third of the pulls
        // put fire on the ground (ArenaDefinition::Hazards: a hazard caster in every pull), which is where stepping
        // out of it is learned now -- under a pack, where it is a trade against the fight, not alone in a field.
        // 150 s: four of the duel's creatures take 70-90 s before the approach.
        stages.push_back({
            .Name = "stage5_pack",
            .Suffix = "_pack",
            .Extends = "stage4_duel",
            // The pack block's slots, trained on the rotation drill's switching and dropped by the duel.
            .Merges = { "stage3_rotation" },
            .Summary = "a pack of 2-4, casters included, usually linked, and fire underfoot in a third of them",
            .Blocks = { Core, Move, Duel, Pet, Pack, Forecast, Goal },
            .Arenas = {
                { .Name = "pack", .Weight = 2, .Against = Opposition::Pulls, .Schedule = PullSchedule::SinglePack,
                    .EpisodeSeconds = 150 },
                { .Name = "hazards", .Weight = 1, .Against = Opposition::Pulls, .Schedule = PullSchedule::SinglePack,
                    .EpisodeSeconds = 150, .Hazards = true },
                // Commanded goals: fight this one, hold that one, recover -- given, then reached.
                { .Name = "commanded", .Weight = 1, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::SinglePack, .EpisodeSeconds = 150, .CommandedGoals = true },
            },
        });

        // Pull after pull with short breaks: heals, food and drink. 450 s holds eight or more pulls, and the solo
        // gauntlet is won by lasting to the end with Pulls.SoloGauntletWinPulls cleared. A third of the episodes are
        // the endurance run: the same eight pulls in the same order, ending on an elite pack two levels up -- a plan
        // rather than a fight, won by clearing the last pull alive. It was a stage of its own and was still rising
        // at its budget; as an arena it trains alongside the gauntlet it grew out of.
        stages.push_back({
            .Name = "stage6_gauntlet",
            .Suffix = "_gauntlet",
            .Extends = "stage5_pack",
            .Summary = "pull after pull with short breaks, and a known run of eight won by finishing it",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Support, Forecast, Goal },
            .Arenas = {
                { .Name = "gauntlet", .Weight = 2, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .EpisodeSeconds = 450 },
                { .Name = "endurance", .Weight = 1, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Sequence, .EpisodeSeconds = 900 },
                // Commanded goals between and during pulls: Recover and Rest given in the breaks as often as Fight.
                { .Name = "commanded", .Weight = 1, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .EpisodeSeconds = 450, .CommandedGoals = true },
            },
        });

        // Life outside the fight, one character playing its class in the world: a chain of 1-3 quests of its level
        // band (giver, objectives, turn-in, in the world's own zone), a field of herbs and ore with what lives among
        // them, and a town to sell, repair, restock and dress in. The end of the classes phase: a quest is a run of
        // small fights with walking between them, so it extends the gauntlet and merges the travel stage for the
        // mount and the objective-bearing columns. The quest arena is weighted up: it was the worst of the three.
        stages.push_back({
            .Name = "stage7_life",
            .Suffix = "_life",
            .Extends = "stage6_gauntlet",
            .Merges = { "stage2_travel" },
            .Summary = "a chain of quests of the level band, a field of herbs and ore, a town's traders",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Support, World, Forecast, Goal },
            .Arenas = {
                { .Name = "quest", .Weight = 3, .Against = Opposition::Quest, .EpisodeSeconds = 600 },
                { .Name = "gather", .Weight = 1, .Against = Opposition::Gather, .EpisodeSeconds = 240 },
                { .Name = "town", .Weight = 1, .Against = Opposition::Town, .EpisodeSeconds = 120 },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .MinLevel = 15,
        });

        // ---------------------------------------------------------------------------------------------------------
        // Phase 3: parties and raids. Beside others, and then commanded.
        // ---------------------------------------------------------------------------------------------------------

        // The gauntlet beside a cast owner: follow it (Component H's path-trailing follow), assist, guard and heal
        // it. 450 s, as the solo gauntlet: two or three pulls would leave nothing to recover for and no win to reach.
        stages.push_back({
            .Name = "stage8_companion",
            .Suffix = "_companion",
            .Extends = "stage7_life",
            .Summary = "the gauntlet beside an owner: follow, assist, guard and heal it",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Support, Forecast, Goal },
            .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .Owner = true, .OwnerCast = true, .EpisodeSeconds = 450 } },
        });

        // A group under a director, and its drills as arenas of it. The tanking and triage drills were stages of
        // their own and their best checkpoints were their seeds (0M): the next stage overwrote whatever they taught.
        // Here a third of the episodes fix a tank seat that has to hold what the pull brings (the threat table is
        // the episode), and a third a healer seat that has to keep the hurt one up and spend mana to do it.
        //
        // The hazard charge lands about four times harder on a tank than on a ranged seat, because a tank cannot walk
        // out of what it is holding an enemy in: that is Hazards.Standing tuned for a seat with a choice.
        stages.push_back({
            .Name = "stage9_party",
            .Suffix = "_party",
            .Extends = "stage8_companion",
            .Summary = "a party and its owner under a director, with a fixed tank or a fixed healer in two thirds",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support, Order, Forecast, Goal },
            .Arenas = {
                { .Name = "party", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true,
                    .EpisodeSeconds = 450, .Directed = true, .DirectorLearned = true },
                { .Name = "tanking", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true,
                    .EpisodeSeconds = 300, .SeatAptitudes = { AptitudeDemand::HoldsThePull() },
                    .Directed = true, .DirectorLearned = true },
                { .Name = "triage", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true,
                    .EpisodeSeconds = 300, .SeatAptitudes = { AptitudeDemand::KeepsThemUp() },
                    .Directed = true, .DirectorLearned = true },
            },
        });

        // The first real instance: a party and its owner against a dungeon's own scripted bosses, in the dungeon
        // (InstanceEncounter), the rungs five dungeons across the level bands. The party gauntlet on the host map
        // stays as a control arena at a tenth of the episodes.
        stages.push_back({
            .Name = "stage10_dungeon",
            .Suffix = "_dungeon",
            .Extends = "stage9_party",
            .Summary = "a party and its owner against real dungeon bosses, in their instances",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support, Order, Forecast, Goal },
            .Arenas = {
                { .Name = "dungeon", .Weight = 9, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .Owner = true, .OwnerCast = true, .PartyGroup = true, .Instance = InstanceLadder::Dungeon,
                    .EpisodeSeconds = 300, .Directed = true, .DirectorLearned = true },
                { .Name = "dungeon_control", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true,
                    .EpisodeSeconds = 300, .Directed = true, .DirectorLearned = true },
            },
        });

        // A group questing in the world (Component F): two to four seats with a director, a chain of quests in a real
        // zone, the journal shared -- where one member saw what the quest wants, all of them know. Kill and loot
        // credit is the group's (a real group), and the director sends members to the objectives.
        stages.push_back({
            .Name = "stage11_world_group",
            .Suffix = "_world_group",
            .Extends = "stage10_dungeon",
            // The world and travel blocks, from the life stage the party line does not carry.
            .Merges = { "stage7_life" },
            .Summary = "a group on a quest chain in the world, under a director",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Party, Support, World, Order, Forecast, Goal },
            .Arenas = { { .Name = "world_group", .Seats = SeatPlan::Party, .Against = Opposition::Quest,
                .PartyGroup = true, .EpisodeSeconds = 600, .Directed = true, .DirectorLearned = true } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .MinLevel = 15,
        });

        // The raid, before the real raids: MAX_SEATS learned seats as RAID_GROUPS groups of GROUP_SEATS, each group
        // with its own tank and healer, one director over all of them addressing groups and members. A raid is not a
        // bigger party -- it is many seats around one large enemy, which is why the opponents are an elite and its
        // adds, and why the mechanics a seat can read (a cast worth interrupting, something on the ground, where it
        // stands on the threat table) matter far more here. One fight, and a run of pulls with recovery between.
        //
        // Forty seats an env is forty bots an env: AnimusForge.Stage.stage12_raid_pulls.Envs brings the env count
        // down in proportion.
        stages.push_back({
            .Name = "stage12_raid_pulls",
            .Suffix = "_raid_pulls",
            .Extends = "stage11_world_group",
            // The companion block, which the world group dropped (it has no owner).
            .Merges = { "stage10_dungeon" },
            .Summary = "a raid of eight groups against one elite and its adds, and a run of raid pulls",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support, Order, Forecast, Goal },
            .Arenas = {
                // A raid group (PartyGroup): the party encounter's teammate terms and columns, the dead standing up
                // between pulls, and a wipe -- not seat 0's death -- ending the episode.
                { .Name = "raid_single", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::SinglePack, .PartyGroup = true, .EpisodeSeconds = 300,
                    .Directed = true, .DirectorLearned = true },
                { .Name = "raid_gauntlet", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .PartyGroup = true, .EpisodeSeconds = 600,
                    .Directed = true, .DirectorLearned = true },
            },
        });

        // The real raids, as arenas of one stage: ten seats in Karazhan and Naxxramas, twenty-five in Naxxramas,
        // forty in Molten Core, Blackwing Lair and the Temple of Ahn'Qiraj. No owner (forty seats leave no slot for
        // one): seat 0 leads. Weighted against their cost -- a forty-seat episode is four ten-seat ones -- so each
        // size gets a fair share of the stage's bots rather than of its episodes. A ten-seat synthetic single pack
        // stays as the control arena. The env count is set for the forty (Stage.stage13_raids.Envs).
        stages.push_back({
            .Name = "stage13_raids",
            .Suffix = "_raids",
            .Extends = "stage12_raid_pulls",
            .Summary = "raids of ten, twenty-five and forty seats against their bosses, in their instances",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support, Order, Forecast, Goal },
            .Arenas = {
                { .Name = "raid10", .Weight = 4, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid10, .RaidSeats = 10, .EpisodeSeconds = 360,
                    .Directed = true, .DirectorLearned = true },
                { .Name = "raid25", .Weight = 2, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid25, .RaidSeats = 25, .EpisodeSeconds = 420,
                    .Directed = true, .DirectorLearned = true },
                { .Name = "raid40", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid40, .RaidSeats = 40, .EpisodeSeconds = 480,
                    .Directed = true, .DirectorLearned = true },
                { .Name = "raid_control", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::SinglePack, .RaidSeats = 10, .EpisodeSeconds = 300,
                    .Directed = true, .DirectorLearned = true },
            },
        });

        // ---------------------------------------------------------------------------------------------------------
        // Phase 4: PvP. Against people -- learned ones from the first stage (the cast league), scripted ones only in
        // the drills about getting away. Last before shipping, so the ship stage replays everything before it.
        // ---------------------------------------------------------------------------------------------------------

        // Self-play one-on-one: two learned seats of any classes, the far side played by the live policy or by a
        // frozen earlier checkpoint (the league), so the opponent is always something that learned to fight.
        stages.push_back({
            .Name = "stage14_duel_pvp",
            .Suffix = "_duel_pvp",
            .Extends = "stage13_raids",
            .Summary = "self-play one-on-one: two learned seats of any classes",
            .Blocks = { Core, Move, Duel, Pet, Pvp, Forecast, Goal },
            .Arenas = { { .Name = "arena_1v1", .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                .Pvp = true } },
        });

        // Not fighting: a fight it may not win, against a scripted player that searches. Everything before rewards
        // winning the fight in front of it, so a losing fight is a situation the policy has never been paid to handle:
        // it dies with its cooldowns up. Two drills as arenas (evade and hide were separate stages, and hide's best
        // checkpoint was its seed): break away and live to the end of it, or get out of sight and stay there. Every
        // class plays both, graded on the outcome rather than the button -- terrain, distance, Blink, Disengage,
        // Feign Death, Invisibility, Sprint, Shadowmeld. The opponent is from ten levels below to ten above, so
        // whether to leave at all is part of the lesson. Cover is the whole point: Durnholde Keep and the Southshore
        // farms, on the instance map the PvP line fights on.
        stages.push_back({
            .Name = "stage15_escape",
            .Suffix = "_escape",
            .Extends = "stage14_duel_pvp",
            .Summary = "a fight it may not win: break away and live, or get out of sight and stay there",
            .Blocks = { Core, Move, Duel, Pet, Pvp, Forecast, Goal },
            .Arenas = {
                { .Name = "evade", .Against = Opposition::ScriptedPlayer, .Pvp = true, .EpisodeSeconds = 120,
                    .OpponentLevelRange = 10 },
                { .Name = "hide", .Against = Opposition::ScriptedPlayer, .Pvp = true, .EpisodeSeconds = 120,
                    .OpponentLevelRange = 10 },
            },
            .MapId = 560,
            .SpawnPoints = HillsbradGround(),
            .HeldOutSpawnPoints = HillsbradControl(),
        });

        // Stealth, which is not hiding: being *close* and not found -- crossing the ground to someone looking for you
        // and arriving inside strike range with the opener in hand. Shadowmeld cannot, since it breaks on moving;
        // only a real stealth aura can. Restricted (NeedsStealth) to the classes whose kit can stealth; the learner's
        // bootstrap steps over it for the rest and merges its layouts forward. From level with the seat to six up:
        // stronger, so getting into position is worth its time, but close enough that a good opener decides it.
        stages.push_back({
            .Name = "stage16_stealth",
            .Suffix = "_stealth",
            .Extends = "stage15_escape",
            .Summary = "close on a stronger enemy unseen, hold there in strike range, and open from it",
            .NeedsStealth = true,
            .Blocks = { Core, Move, Duel, Pet, Pvp, Forecast, Goal },
            .Arenas = { { .Name = "stealth", .Against = Opposition::ScriptedPlayer, .Pvp = true,
                .EpisodeSeconds = 120, .OpponentLevelBonus = 3, .OpponentLevelRange = 3 } },
            .MapId = 560,
            .SpawnPoints = HillsbradGround(),
            .HeldOutSpawnPoints = HillsbradControl(),
        });

        // Arena teams under a director: two, three and five a side (it was two only). The seats already fight one on
        // one; what is new is being told what the side is doing -- concentrate on that one, you take the next
        // interrupt, go there -- and learning that following it pays. Places are on: an arena has cover worth
        // sending someone to.
        stages.push_back({
            .Name = "stage17_arena",
            .Suffix = "_arena",
            .Extends = "stage16_stealth",
            // The pack, support and order blocks, trained through the party phase; the pvp line dropped them.
            .Merges = { "stage13_raids" },
            .Summary = "two, three and five a side, under a director: follow the call",
            .Blocks = { Core, Move, Duel, Pack, Pet, Pvp, Context, Hostiles, Support, Order, Forecast, Goal },
            .Arenas = {
                { .Name = "arena_2v2", .Weight = 2, .Seats = SeatPlan::Teams, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 180, .Directed = true, .DirectorLearned = true, .Places = true,
                    .TeamSeats = 2 },
                { .Name = "arena_3v3", .Weight = 2, .Seats = SeatPlan::Teams, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 180, .Directed = true, .DirectorLearned = true, .Places = true,
                    .TeamSeats = 3 },
                { .Name = "arena_5v5", .Weight = 1, .Seats = SeatPlan::Teams, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 180, .Directed = true, .DirectorLearned = true, .Places = true,
                    .TeamSeats = 5 },
            },
            .MinLevel = 20,
        });

        // Warsong Gulch's rules one on one: take the other side's flag home, return one's own, stop the carrier.
        // Mounting between the bases and being dismounted by the flag come from travel; the fight from the arena.
        // The Barrens, since the second base is placed by the objective search 100-180 yd from the first.
        stages.push_back({
            .Name = "stage18_flag",
            .Suffix = "_flag",
            .Extends = "stage17_arena",
            // The travel block, which the pvp line does not carry.
            .Merges = { "stage2_travel" },
            .Summary = "capture the flag one-on-one: bases 100-180 yd apart, first to three captures",
            .Blocks = { Core, Move, Duel, Pet, Pvp, Travel, Flag, Forecast, Goal },
            .Arenas = { { .Name = "flag", .Seats = SeatPlan::Mirror, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 300 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
            .MinLevel = 20,
        });

        // Ten against ten in the real battleground, each side under its director: escort the carrier, hold the
        // base, stop theirs.
        stages.push_back({
            .Name = "stage19_warsong",
            .Suffix = "_warsong",
            .Extends = "stage18_flag",
            // Ten a side is a group: the party and order blocks, which the flag line does not carry.
            .Merges = { "stage13_raids" },
            .Summary = "ten against ten for the flag: escort the carrier, hold the base, stop theirs",
            .Blocks = { Core, Move, Duel, Pet, Pvp, Travel, Flag, Party, Order, Forecast, Goal },
            .Arenas = { { .Name = "warsong", .Seats = SeatPlan::Teams, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 420, .Directed = true, .DirectorLearned = true } },
            .MapId = MAP_WARSONG_GULCH,
            // Silverwing Hold and the Warsong Lumber Mill, as game_graveyard 769 and 770 put them: the real
            // battleground's own arrival points, which are also where its flags stand.
            .SpawnPoints = { { 1523.8f, 1481.8f, 352.0f, 3.1416f } },
            .FlagBases = {
                { 1523.8f, 1481.8f, 352.0f, 3.1416f },
                { 933.3f, 1433.7f, 345.5f, 0.1516f },
            },
            .MinLevel = 20,
        });

        // The world with other people in it (Component F): two directed pairs and two solos questing in one zone,
        // every group half the time on the first group's quest, the zone's creatures shared. The coordinator's claims
        // show in the journal, and credit taken in a place another group holds is charged (Life.Poach): what it
        // teaches is going where the others are not. A quarter of the episodes add the world's other people: hostile
        // players who arrive mid-quest and gank whoever they find.
        stages.push_back({
            .Name = "stage20_world_shared",
            .Suffix = "_world_shared",
            .Extends = "stage19_warsong",
            // The world group for the life and group blocks, the arena for the hostiles block.
            .Merges = { "stage11_world_group", "stage17_arena" },
            .Summary = "groups and solos questing in one zone, sharing its creatures, and ganked by hostile players",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Party, Pvp, Hostiles, Support, World, Order,
                Forecast, Goal },
            .Arenas = {
                { .Name = "world_shared", .Weight = 3, .Seats = SeatPlan::Teams, .Against = Opposition::Quest,
                    .PartyGroup = true, .EpisodeSeconds = 600, .Directed = true, .DirectorLearned = true,
                    .TeamSeats = 2, .LoneSeats = 2 },
                { .Name = "world_gank", .Weight = 1, .Seats = SeatPlan::Teams, .Against = Opposition::Quest,
                    .PartyGroup = true, .EpisodeSeconds = 600, .Ambushers = 2, .Directed = true,
                    .DirectorLearned = true, .TeamSeats = 2, .LoneSeats = 2 },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .MinLevel = 15,
        });

        // ---------------------------------------------------------------------------------------------------------
        // Ship: every phase at once, the checkpoint that ships.
        // ---------------------------------------------------------------------------------------------------------

        // With PvP last, the stages right before shipping are all PvP, and the drills whose best checkpoint was their
        // seed are evidence that a later stage overwrites an earlier one. So the last stage replays every phase and is
        // judged on all of them: its evaluation reports per phase (the eval config's `phases`), and the gate is each
        // phase within noise of its own stage. It merges the last stage of each earlier phase -- the PvP phase
        // arrives by extension -- so each phase's arenas can be distilled from the model that trained them.
        //
        // Sixteen arenas (MAX_ARENAS), each phase's representatives rather than every drill: movement's trip, flight
        // and water crossing (the earliest lesson, the likeliest overwritten); the classes' duel, gauntlet, quest and
        // town; the companion, the directed party, the dungeon and a ten-seat raid; one-on-one, arena teams, the
        // escape drill and the shared world with its ganks; and the ganked owner, which needs PvE and PvP in one
        // episode. Left out: gathering (the quest's journal work covers finding and taking things), the rotation
        // drill (every fight replays the kit), ledges and rooms, the flag and Warsong, and the 25- and 40-seat raids
        // -- those last two are close to this stage in the chain and have their own stage evaluations. Arenas on
        // other maps than the host's say so (MapId). The raid makes this a ten-seat stage: the smaller arenas leave
        // the rest of the seats empty.
        stages.push_back({
            .Name = "stage21_ship",
            .Suffix = "_ship",
            .Extends = "stage20_world_shared",
            .Merges = { "stage2_travel", "stage7_life", "stage13_raids", "stage17_arena" },
            .Summary = "every phase in one policy: the trip, the fight, the quest, the party, the raid and the arena",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Companion, Party, Pvp, Context, Hostiles,
                Support, World, Order, Forecast, Goal },
            .Arenas = {
                // Movement
                { .Name = "travel", .Weight = 3, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .SpawnPoints = KalimdorGround(), .MapId = MAP_KALIMDOR, .MinLevel = 20,
                    .HeldOutSpawnPoints = KalimdorControl() },
                { .Name = "flight", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 180,
                    .Flying = true, .SpawnPoints = OutlandGround(), .MapId = MAP_OUTLAND, .MinLevel = 60,
                    .HeldOutSpawnPoints = NagrandControl() },
                // The move stage's water crossing on foot: the earliest lesson and the likeliest to be overwritten,
                // and swimming comes up all the time in live play.
                { .Name = "water", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true, .Water = true, .SpawnPoints = OasisShore(), .MapId = MAP_KALIMDOR,
                    .HeldOutSpawnPoints = OasisControl() },
                // Classes
                { .Name = "duel", .Weight = 3, .Against = Opposition::Creature, .EpisodeSeconds = 90,
                    .SpawnPoints = KalimdorGround(), .MapId = MAP_KALIMDOR, .HeldOutSpawnPoints = KalimdorControl() },
                { .Name = "gauntlet", .Weight = 3, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .EpisodeSeconds = 450 },
                { .Name = "quest", .Weight = 4, .Against = Opposition::Quest, .EpisodeSeconds = 600 },
                // Parties and raids
                { .Name = "companion", .Weight = 4, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .EpisodeSeconds = 300 },
                { .Name = "party", .Weight = 4, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true,
                    .EpisodeSeconds = 300, .Directed = true, .DirectorLearned = true },
                { .Name = "dungeon", .Weight = 4, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .Owner = true, .OwnerCast = true, .PartyGroup = true, .Instance = InstanceLadder::Dungeon,
                    .EpisodeSeconds = 300, .Directed = true, .DirectorLearned = true },
                { .Name = "raid10", .Weight = 2, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid10, .RaidSeats = 10, .EpisodeSeconds = 360,
                    .Directed = true, .DirectorLearned = true },
                // PvP
                { .Name = "arena_1v1", .Weight = 4, .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 60 },
                { .Name = "arena_2v2", .Weight = 2, .Seats = SeatPlan::Teams, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 180, .Directed = true, .DirectorLearned = true, .Places = true,
                    .TeamSeats = 2, .MinLevel = 20 },
                { .Name = "escape", .Weight = 1, .Against = Opposition::ScriptedPlayer, .Pvp = true,
                    .EpisodeSeconds = 120, .SpawnPoints = HillsbradGround(), .MapId = 560,
                    .HeldOutSpawnPoints = HillsbradControl(), .OpponentLevelRange = 10 },
                { .Name = "world_shared", .Weight = 2, .Seats = SeatPlan::Teams, .Against = Opposition::Quest,
                    .PartyGroup = true, .EpisodeSeconds = 600, .Ambushers = 1, .Directed = true,
                    .DirectorLearned = true, .TeamSeats = 2, .LoneSeats = 2 },
                // PvE and PvP in one episode: the owner ganked in the middle of the gauntlet.
                { .Name = "ambush", .Weight = 2, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .Owner = true, .OwnerCast = true, .EpisodeSeconds = 300, .Ambushers = 2 },
                { .Name = "town", .Weight = 1, .Against = Opposition::Town, .EpisodeSeconds = 120 },
            },
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
        if (arena.PartyGroup && !raidGroup && !worldGroup && (!arena.Owner || arena.Seats != SeatPlan::Party))
            return "a party group needs an owner and party seats, unless it is a raid or a quest";
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
        if (travel && !stage.Has(BlockId::Travel))
            return "travel needs the travel block";
        if (travel && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0))
            return "travel is one seat on its own";
        if ((arena.OpponentLevelBonus != 0 || arena.OpponentLevelRange != 0)
            && arena.Against != Opposition::ScriptedPlayer)
            return "only a scripted enemy player takes a level bonus or range";
        if (arena.OpponentLevelRange < 0)
            return "a level range is how far either way, not negative";
        if (arena.Flying && !travel)
            return "only a travel arena flies";
        if (arena.Indoors && !travel)
            return "only a travel arena can be indoors: being inside changes where an objective may be put and "
                "what reaching it means, and nothing else asks either question";
        if (arena.Indoors && arena.Flying)
            return "an arena is indoors or it flies, not both";
        if (arena.Indoors && arena.Water)
            return "an interior arena has no crossing to offer: water wants an objective across a lake";
        if (arena.Ledges && !travel)
            return "only a travel arena has ledges: an objective below a drop is a place to get to";
        if (arena.Ledges && (arena.Flying || arena.Indoors || arena.Water))
            return "a ledge arena is on foot outdoors: the drop is the shortcut and the ramp is the way round, which "
                "wings, a roof or a lake would each make a different question";
        if (arena.Water && !travel && arena.Against != Opposition::Creature)
            return "water is a travel arena's crossing or a creature arena's lake; nothing else reads it";
        if (arena.Underwater && !travel)
            return "only a travel arena dives: an objective on a lakebed is a place to get to";
        if (arena.Underwater && (arena.Flying || arena.Indoors || arena.Ledges || arena.Water))
            return "a dive arena is its own trip: the objective is on the bed, not across the lake, and neither "
                "wings, a roof nor a ledge belong to it";
        if (arena.Checkpoints && !(travel && arena.Underwater))
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

        if (!stage.Has(BlockId::Duel))
            return "every stage fights something that fights back, which needs the duel block";

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
        // returned when MAX_SEATS was 4 and is what it has to keep returning now that MAX_SEATS is a raid.
        case SeatPlan::Party:  return GROUP_MEMBERS;
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
