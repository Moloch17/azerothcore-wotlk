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
 * **A class learns to walk before it learns to fight.** The first four stages have nothing to kill in them, and
 * they are first on purpose: a seat now steers itself -- eight bearings under a held yaw and pitch, with the
 * ground read along each of them -- and where a seat puts its feet is not something only some stages are about.
 * Everything after them inherits legs that already work, and a stage that used to be about fighting while moving
 * badly is now only about fighting.
 *
 *   move ─ dodge ─ travel ─ flight        the feet: ground, fire underfoot, the mount, the air
 *      ├─ indoor, jump ─ glide, dive ─ breathe   drills off the feet, by name: rooms, ledges, lakebeds
 *        ─ duel ─ pack ─ gauntlet ─ endurance        alone, against things that fight back
 *        ─ pvp ─ evade ─ hide ─ stealth ─ arena      against people
 *        ─ companion ─ party ─ tanking ─ triage      beside others, still nobody commanding
 *        ─ flag ─ warsong ─ duo_led                  an objective, and then a director
 *
 * It is one line, deliberately. A branch is cheaper to train but it ends in several checkpoints, and everything
 * a leaf teaches is thrown away unless the stage exported from is downstream of it -- which is how the drills,
 * the raids and the team stages came to be a dead end. A linear chain ends in one leaf that carries everything.
 *
 * Scenario names carry the stage's number (stage1_move ... stage27_crossroads), model names only its suffix
 * (_move). The move stage is the first: nothing seeds it.
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

    /// The banks of Stonebull Lake in Mulgore, for the dive drill (stage4_dive).
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

    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        stages.push_back({
            .Name = "stage1_move",
            .Suffix = "_move",
            .Extends = "",
            .Summary = "a place 40-160 yd away on foot: hold a bearing, read the ground, and cross it",
            .Blocks = { Core, Move, Travel, Duel },
            // Mounting is masked here, not merely unpaid. What is left is everything a player does before it can
            // ride -- Sprint, Dash, Travel Form, Aspect of the Cheetah, and simply not stopping -- and those are
            // worth learning on their own, because a mount is barred in combat, indoors and at low level, which is
            // most of when a bot actually has to get somewhere.
            //
            // Three kinds of ground, because "cross it" is a different problem on each and a policy taught only on
            // the flats learns to hold forward. Open ground is the lesson at its simplest; broken ground is where
            // the terrain probe earns its place, since a seat that cannot see the cliff it is walking into is only
            // being steered by the pathfinder; and water is where the seat has to decide whether to get in at all,
            // and then swim in three dimensions once it has.
            .Arenas = {
                // Both clocks are the same. They were 120 and 150 over identical ground, which made `open` the
                // harder arena of the two while being the one described as the simpler lesson -- and if both must
                // reach every objective, a shorter clock is a handicap with nothing to teach in it. The
                // difference between these two arenas is the terrain, which is what it was always meant to be.
                { .Name = "open", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true },
                // Ground that is actually broken. Until now neither arena declared spawn points, so both fell
                // through to the stage list and ran on *the same terrain*: "broken ground is where the terrain
                // probe earns its place" described an arena identical to the open one, and the measured detour
                // said so -- 1.25 against 1.21, which is the same trip.
                //
                // These cells were chosen by local relief -- the standard deviation of creature-spawn z within a
                // 250-unit cell -- and then, the part that was missing, stood on.
                //
                // Relief on its own selects for the thing that breaks a bot. It is a measure of how much the
                // ground moves, so it ranks crevices, ledges and cliff faces highest, and several of the points
                // it produced were places a character cannot turn round in: 0.47 yards of clearance on the
                // Mulgore ridge, 0.99 in Durotar, 2.13 and 1.45 at the two held-out escarpment points. They are
                // on the navmesh, so a route out of them exists and the episode builds -- it simply cannot be
                // walked. At six million steps those two held-out points alone were 43% of every timeout in the
                // stage: 85 failures out of 198, every one with a complete route, none getting within a hundred
                // yards of its objective.
                //
                // Every point here is now measured with `forge rays` as well: at least ~4.5 yards of clearance,
                // so there is room to turn, and still short reaches on several bearings, so there is still
                // something to walk around. Rough ground a character can stand on, which is what the arena
                // wanted in the first place. The ridges carry a relief of 78 and 64 against ground whose z
                // barely moves, and the Durotar canyons and the Dustwallow shore are what this file already
                // calls "canyon and rock" and "marsh and broken shore".
                { .Name = "broken", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true,
                    .SpawnPoints = {
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
                        // Cliff feet: low ground under a plateau 30-50 yd up, whose top the route reaches by one
                        // ramp at 1.3-1.8x the straight line. The first format-6 run (2026-09-23, 22M steps)
                        // arrived 0.96 on the held-out escarpment below and lost 50 of its 80 failures at its
                        // cliff foot: the seat ran at the objective's bearing, reached the wall under it, and
                        // paced there for the rest of the clock, because nothing it had trained on had taught it
                        // to back off and follow the wall to the way up. None of the lists above had that shape.
                        // Each of these was found from the creature spawns' relief, then stood on with `forge
                        // rays` (floor found, no water) and routed to its three nearest plateau points with
                        // `forge route`. The escarpment itself stays held out.
                        { -2032.2f, -3618.1f, 22.3f, 0.0f },  // southern Barrens, plateau +40..50, 1.47-2.31x
                        { -2563.7f, -3798.6f, 7.0f, 0.0f },   // Barrens/Dustwallow edge, +41..49, 1.03-1.36x
                        { 190.8f, -4516.5f, 27.1f, 0.0f },    // Durotar canyon, +30..37, 1.76-2.2x
                        { 479.5f, -4658.7f, 41.7f, 0.0f },    // Durotar canyon, +28..34, 1.3-1.77x
                    },
                    // The southern Barrens escarpment, relief 47, in no training list. Rougher ground held back
                    // for scoring, on the same argument as the stage's own control: if `arrived` here tracks
                    // `arrived` on the ridges, the seat is reading terrain rather than remembering places.
                                        .HeldOutSpawnPoints = {
                        { -623.5f, -3166.8f, 91.7f, 0.0f },   { -405.9f, -3207.1f, 186.5f, 0.0f },
                        { -441.9f, -3162.0f, 210.3f, 0.0f },
                    } },
                // The banks of the Barrens oases -- Lushwater to the north, Stagnant to the south -- because the
                // stage's own spawn points have no water within reach, and a water arena that finds no crossing
                // quietly becomes a second open arena (the first run of this stage reported crossing 0.0 over all
                // 415 of its water episodes). These are on the shore, not in the pool. Taken from the land creatures
                // the oases are ringed with (Kolkar centaurs), so the ground under each one is real. The pools
                // themselves are shallow -- the surface is z 30.2 over a bed at 27-28, under three yards at the
                // deepest, measured from the map tiles (var/lakes) -- which is why the dive drills go elsewhere.
                { .Name = "water", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true, .Water = true,
                    .SpawnPoints = {
                        { -3923.0f, -2981.0f, 31.0f, 0.0f }, { -3952.0f, -2947.0f, 40.0f, 0.0f },
                        { -3964.0f, -3068.0f, 39.0f, 0.0f }, { -3879.0f, -3004.0f, 37.0f, 0.0f },
                        { -4048.0f, -3051.0f, 43.0f, 0.0f }, { -3985.0f, -2911.0f, 37.0f, 0.0f },
                    },
                    // The far side of the same pond, kept back for scoring. Weaker control than the stage's own:
                    // Azshara and Teldrassil are ground this policy has never seen, while these are banks of the
                    // water it trains on, approached from the other side. It is what the ground allows -- of four
                    // bodies of water measured, only this one was wide enough for the way round to be worth
                    // avoiding, so a second pond to hold out does not exist yet. What it does test is whether the
                    // seat crosses water it has not launched from before; what it cannot test is a different lake.
                    .HeldOutSpawnPoints = {
                        { -4017.0f, -3086.0f, 37.0f, 0.0f }, { -3926.0f, -2911.0f, 39.0f, 0.0f },
                    } },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
        });

        // Inside, where the walls are close enough to matter.
        //
        // Named 1b rather than renumbering the scenarios behind it: stage numbers are cosmetic here --
        // nothing parses them and training order comes from AnimusForge.Queue -- and the name says where it
        // belongs without the churn.
        //
        // Not in the default queue: a side branch off stage1_move, trained by name, and stage5_dodge extends
        // stage1_move directly. It is the stage that tests what the sixteen rays, the fifteen-degree turn, the
        // clearance term and the jump were built for -- a doorway off the objective's axis, which open country
        // never asks for. It is in the default queue: every stage is ended by the same
        // convergence rule, so a drill a first run does poorly at is read from its report, not held back.
        stages.push_back({
            .Name = "stage2_indoor",
            .Suffix = "_indoor",
            .Extends = "stage1_move",
            .Summary = "a place 8-40 yd away inside a building: read the walls, keep off them, and find the door",
            .Blocks = { Core, Move, Travel, Duel },
            .Arenas = {
                // Short trips and a short clock: an inn is twenty to thirty yards across, so an outdoor arena's
                // first step would already be through an outside wall.
                { .Name = "rooms", .Against = Opposition::Travel, .EpisodeSeconds = 90,
                    .OnFoot = true, .Indoors = true, .SpawnScatter = 4.0f },
            },
            .MapId = MAP_KALIMDOR,
            // Every inn on Kalimdor that areatrigger_tavern names, spread across regions for the same reason the
            // ground list is: a policy that sees four rooms learns four rooms.
            // Every one of these was stood on before it was written down, with `forge rays`, and the ones that
            // are not here are why the command exists.
            //
            // The first draft of this list was the centre of each row of areatrigger_tavern, on the reasoning
            // that the table names and bounds every inn in the world. It does -- but an areatrigger's centre is
            // a point in a volume, not a place on a floor, and of the twelve taken that way three were on no
            // navmesh at all and four were in buildings whose WMO group is flagged outdoors. Astranaar's centre
            // sits in the gap between two storeys; the Barrens and Durotar ones landed on isolated discs of mesh
            // about thirty yards across, open ground rather than rooms. The four flagged outdoors are the night
            // elf inns -- Auberdine, Dolanaar, Astranaar -- which really are open-sided, and FindPlace refuses to
            // put an objective in one, so a seat spawned there would have had nowhere to be sent.
            //
            // What is left is every tavern on Kalimdor that is on the mesh and whose group says it is inside,
            // with z corrected from the trigger's centre to the floor Map::GetHeight finds under it -- as much
            // as ten yards down in one case. Clearance at each is 0.71 to 6.63 yards, which is the point: these
            // are rooms a seat can touch two walls in.
            .SpawnPoints = {
                { -3182.4f, -2920.8f, 33.56f, 0.0f },  // Brackenwall Village, clearance 1.70
                { -4461.9f, 242.6f, 39.11f, 0.0f },    // Feralas, 2.85
                { -4622.3f, -3172.1f, 34.81f, 0.0f },  // Mudsprocket, 2.84
                { -2366.7f, -346.0f, -8.96f, 0.0f },   // Mulgore, 2.29
                { -1051.4f, -3653.8f, 23.88f, 0.0f },  // The Barrens, 2.71
                { -5477.9f, -2460.3f, 89.28f, 0.0f },  // Thousand Needles, 5.36
                { 6688.0f, -4670.1f, 721.69f, 0.0f },  // Winterspring, 6.63
            },
            // Rooms no training episode stands in, for the same reason every other stage holds ground back.
            // Desolace is the tightest room found anywhere on the map at 0.71 yards of clearance, which makes it
            // the one worth scoring on.
            //
            // It has never once been scored on. Grouping all 14336 evaluation episodes the first indoor run ever
            // recorded by where they ran: Tanaris 7314, Theramore 7022, Desolace 0. The reset draws it as often
            // as the others and no episode has ever started there, which with SPAWN_ATTEMPTS re-rolling a point
            // that cannot build (StageScenario, "Somewhere else in the list") means every Desolace draw became a
            // Theramore or a Tanaris one in silence. So this stage's gate has been read off two rooms while
            // reporting three -- and the two are not one task: Theramore arrives 0.9980 in 1.7 s, Tanaris 0.8947
            // in 13.9 s, so 0.9453 is a trivial room averaged with a hard one.
            //
            // The columns `spawn_drawn` and `spawn_point` now say this outright, and what to do about Desolace
            // waits on them rather than on a guess: a room whose draws all re-roll wants either a shorter
            // objective range than Travel's 8-40 yards or a different room, and which of those it is depends on
            // whether FindPlace is failing on the distance or on the walls.
            .HeldOutSpawnPoints = {
                { -1596.2f, 3145.3f, 62.53f, 0.0f },   // Desolace, clearance 0.71
                { -3615.5f, -4467.3f, 21.10f, 0.0f },  // Theramore Isle, 3.43
                { -7162.1f, -3845.9f, 9.51f, 0.0f },   // Tanaris, 5.91
            },
        });

        // Down, where the way round is long and the way down is a fall.
        //
        // The first format-6 run of stage1_move lost fifty of its eighty held-out failures at one cliff foot, under
        // an objective forty yards up: the mirror of that is a seat above a ledge with the objective below, and
        // the jump is the move that makes the difference. Until format 7 it could not: the landing test raycast
        // along the navmesh and clipped at every lip. Now a jump drops, the fall after it is the core's own with
        // the core's own damage -- nothing to fourteen yards, lethal past about seventy for a full-health
        // character -- and the seat is told how far down the landing is (OBS_JUMP_DROP) and nothing else. What
        // a fall costs is the seat's to learn, and it differs by class.
        //
        // So two drills, both by name. This one masks Slow Fall and Levitate (FeatherFallMasked): every class
        // learns the bare price of a drop, including the price of taking one that is worth it. The one after it
        // gives the classes that have one the button back. The objective always has a way round on foot, at
        // least LedgeDetour times the straight line, so a class that will not drop still arrives.
        stages.push_back({
            .Name = "stage3_jump",
            .Suffix = "_jump",
            .Extends = "stage1_move",
            .Summary = "a place 20-120 yd away below a ledge: drop off it with a jump, or take the long way round",
            .Blocks = { Core, Move, Travel, Duel },
            .Arenas = {
                { .Name = "ledges", .Against = Opposition::Travel, .EpisodeSeconds = 120,
                    .OnFoot = true, .Ledges = true },
            },
            .MapId = MAP_KALIMDOR,
            // Plateau tops above the ground the broken arena trains its cliff feet on, found from the relief in
            // the creature spawns and stood on with `forge rays` facing the edge; the foot below each was routed
            // to with `forge route`. Whether a spawn point offers a ledge trip is what the `ledge` column reports,
            // and a top that never does is a top to replace.
            //
            // Each was routed down to its nearest lower spawns first: the way round is 1.9-12x the straight line
            // for all of these, with drops of 11-44 yd. Four tops picked by relief alone were dropped when the
            // route came back at 1.0x -- a slope the seat can walk down is not a ledge -- and one had no floor.
            .SpawnPoints = {
                { -2063.9f, -3645.5f, 66.1f, 0.0f },   // southern Barrens, above (-2032, -3618): 44 yd, 2.2-3.5x
                { -2094.8f, -3644.6f, 72.4f, 0.0f },   // beside it: 11 yd, 1.9-3.4x
                { 394.1f, -4599.2f, 76.2f, 0.0f },     // Durotar canyon, above (480, -4659): 23 yd, 3.3-4.2x
                { 85.4f, -4543.8f, 58.4f, 0.0f },      // Durotar canyon: 18 yd, 4.7x
                { -519.0f, -4076.9f, 69.9f, 0.0f },    // southern Barrens: 27 yd, 6.6x
                { -2379.6f, 459.2f, 76.8f, 0.0f },     // Mulgore: 16-25 yd, 3.5-4.7x
                { -4052.7f, -2145.5f, 90.2f, 0.0f },   // Thousand Needles: 40 yd, 6.5x
                { -4449.9f, -2914.0f, 40.0f, 0.0f },   // Thousand Needles: 16-18 yd, 8.6-12.5x
            },
            // The southern Barrens escarpment's top, the same ground stage1_move holds out at its foot. Two of
            // stage1_move's own control tops sit above walkable slopes (1.0x) and are no use here; these three
            // are above real edges, and the deep ones are past the fall that kills without Slow Fall.
            .HeldOutSpawnPoints = {
                { -545.9f, -3054.0f, 138.1f, 0.0f },   // 46 yd, 1.9-2.7x
                { -515.9f, -3149.0f, 161.5f, 0.0f },   // 67 yd, 5.3-5.7x
                { -481.2f, -3249.9f, 164.5f, 0.0f },   // 70 yd, 2.9x
            },
        });

        // Down again, into the water this time.
        //
        // stage1_move's water arena taught one decision, swim across or walk round, at the surface: nothing ever
        // asked the seat to go under. Here the objective is on the bed of a lake, under six to forty yards of
        // water, and arriving means standing on it. The breath is the core's own (WaterBreath.Timer, three
        // minutes) and so is the drowning after it, a fifth of the seat's health a second; what the seat sees is
        // how much of its breath is spent (OBS_SUBMERGED_TIME) and how deep the place is, and what it learns is
        // when to come up. Unending Breath and Water Breathing are open: the two classes that have one learn
        // when a cast is worth it, and the rest learn the bare price of the dive.
        //
        // A third of the episodes are longer than a breath. One dive is free against the core's three-minute
        // breath, so a single lakebed never asks the seat to come up; the `chain` arena's lakebeds come as a
        // chain (ArenaDefinition::Checkpoints), thirty to sixty yards on from each other, for four minutes. A
        // seat that stays down for the whole chain runs out of air at three minutes and drowns before the
        // clock; one that surfaces between legs, or casts Unending Breath, Water Breathing or Aquatic Form
        // first, does not. The outcome is being alive at the end, and checkpoints, breaths, breathing_casts and
        // aquatic_seconds say how each class managed it.
        stages.push_back({
            .Name = "stage4_dive",
            .Suffix = "_dive",
            .Extends = "stage1_move",
            .Summary = "a place 20-120 yd away on a lakebed under 6-40 yd of water, or a chain of them longer than "
                "a breath: swim down to it, and come up for air",
            .Blocks = { Core, Move, Travel, Duel },
            .Arenas = {
                { .Name = "depths", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 150,
                    .OnFoot = true, .Underwater = true },
                { .Name = "chain", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 240,
                    .OnFoot = true, .Underwater = true, .Checkpoints = true },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = StonebullShore(),
            .HeldOutSpawnPoints = EluneAraShore(),
        });

        // Something on the ground, in every pull. Hazards exist already -- the pack ladder draws a hazard caster
        // from rung 3 and self-play produces them by the spell (stage19_arena measured 3.1 s of hazard an episode,
        // stage4_gauntlet 1.1 s) -- but a class/role that stalls below rung 3 never meets one, and a second an
        // episode is thin to learn from. Here every pull has one, at stage 2's difficulty, so walking out of it is
        // the thing being learned rather than a detail of a harder fight.
        stages.push_back({
            .Name = "stage5_dodge",
            .Suffix = "_dodge",
            .Extends = "stage1_move",
            .Summary = "nothing to fight, only ground to get off: fire lands underfoot and stays",
            // No pack, no pet and no support: there is nothing here to fight, heal or send anything at, and a block
            // whose actions are masked for a whole stage is exploration the stage cannot afford. Travel stays,
            // though, because dropping it would throw away the objective-bearing columns stage 1 just trained --
            // block-wise seeding starts a re-added block from scratch.
            .Blocks = { Core, Move, Travel, Duel },
            // Nothing is spawned to fight. Fire lands under each seat every few seconds and lingers, so standing
            // still is the only thing that hurts and moving is the only way to spend less. With a pack in the
            // arena the hazard was one charge among many and its numbers could not be read on their own; with the
            // pack gone, hazard_seconds and hazard_damage are the whole stage.
            .Arenas = { { .Name = "hazards", .Against = Opposition::Hazards, .Schedule = PullSchedule::None,
                .EpisodeSeconds = 120 } },
        });

        // Travel: getting somewhere, off the duel. Characters of 20 and up ride; the policy learns when a trip is worth
        // a mount's cast time, and to arrive on foot, ready to fight.
        //
        // The Barrens, not the arena the fighting stages spawn in: a trip needs open, pathable ground in every
        // direction for a few hundred yards, and that arena is a corner pocket with none -- the nearest walkable
        // ground outside it is 350 yd off and 50 yd up a hillside, past the objective search's reach, so no episode
        // could ever be built there. The envs share the continent, each in its own phase, spread over the flats.
        stages.push_back({
            .Name = "stage6_travel",
            .Suffix = "_travel",
            .Extends = "stage5_dodge",
            .Summary = "a place 60-320 yd away by path: mount when it pays, get there, arrive on foot",
            .Blocks = { Core, Move, Travel, Duel },
            // The lesson is the trip -- whether to mount, when a ride pays for its cast -- on top of the steering
            // stage 1 taught. It used to offer ACTION_FOLLOW_ROUTE here, a pathfound leg at a time, on the
            // argument that steering the same eight yards three hundred times teaches nothing new; but a route
            // walked by the engine is the engine navigating, and the policy pressed it in most of its episodes.
            // The seat walks the whole trip itself now.
            .Arenas = { { .Name = "travel", .Against = Opposition::Travel, .EpisodeSeconds = 150 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
            .MinLevel = 20,
        });

        // Flight: Outland's Nagrand, where flying mounts fly (a battleground never allows them). The envs share the
        // continent, each in its own phase, spread over open ground.
        stages.push_back({
            .Name = "stage7_flight",
            .Suffix = "_flight",
            .Extends = "stage6_travel",
            .Summary = "a place 350-700 yd away in Nagrand: take off, fly over what is in the way, land, dismount",
            .Blocks = { Core, Move, Travel, Duel },
            // Two arenas. `flight` places its objective anywhere the height probe finds dry ground, which in
            // Nagrand is nearly always walkable, and 700 yards at run speed is 100 s of a 180 s clock: a ground
            // ride arrives often enough that flying stays optional, and nine of ten class heads never found the
            // flying mount. `flight_air` is where the wings are the way: an objective the ground route does not
            // reach (a plateau, a floating island), the ground mount masked, and arrival measured at the
            // objective's own height so the cliff foot under it does not count.
            .Arenas = {
                { .Name = "flight", .Weight = 2, .Against = Opposition::Travel, .EpisodeSeconds = 180,
                    .Flying = true },
                { .Name = "flight_air", .Weight = 1, .Against = Opposition::Travel, .EpisodeSeconds = 180,
                    .Flying = true, .AirOnly = true },
            },
            .MapId = MAP_OUTLAND,
            // Four Outland regions to take off from, rather than one: Hellfire's broken flats, Zangarmarsh's
            // mushroom basins, Shadowmoon's ridges at nearly 300 yards of altitude, and Terokkar's low forest.
            // A flight starts and ends on the ground, so where it does is part of the lesson.
            .SpawnPoints = {
                // Hellfire Peninsula
                { 170.0f, 2589.0f, 93.0f, 0.0f },     { 169.0f, 2708.0f, 101.0f, 0.0f },
                // Zangarmarsh
                { -3260.0f, 2690.0f, 85.0f, 0.0f },   { -3293.0f, 2832.0f, 125.0f, 0.0f },
                // Shadowmoon Valley
                { -3631.0f, 3741.0f, 298.0f, 0.0f },  { -3721.0f, 3746.0f, 284.0f, 0.0f },
                // Terokkar Forest
                { -1750.0f, 5154.0f, -37.0f, 0.0f },  { -1730.0f, 5282.0f, -32.0f, 0.0f },
            },
            // THE CONTROL GROUND for flight, and it has to be ground a flying mount may actually leave.
            //
            // It used to be Eversong Woods and the Draenei isles, chosen because map 530 carries them as well as
            // Outland and they are therefore a different continent rather than a different corner. They are also
            // not flyable: AreaTableEntry::IsFlyable is `flags & AREA_FLAG_OUTLAND` and those zones do not carry
            // it, so SpellInfo::CheckLocation refuses every flying mount there with SPELL_FAILED_INCORRECT_AREA.
            //
            // Evaluation spawns on held-out ground. So every one of the 2048 evaluation episodes began somewhere
            // the seat could not take off, `flew` read exactly 0.0000 at every checkpoint, and the stage ran its
            // whole thirty million steps against a gate of flew >= 0.35 that nothing could ever have met. The
            // refusal code was identical on all 2048 episodes, which is what named it.
            //
            // Nagrand instead: Outland, so flyable, and none of the four zones this stage trains in (Hellfire,
            // Zangarmarsh, Shadowmoon, Terokkar). The separation is a zone rather than a continent, which is what
            // the requirement to fly allows. Every point stood on with `forge rays`.
            .HeldOutSpawnPoints = {
                { -850.6f, 6517.2f, 172.6f, 0.0f },   { -842.4f, 6578.1f, 172.7f, 0.0f },
                { -652.9f, 6576.9f, 170.4f, 0.0f },   { -685.5f, 6609.0f, 176.6f, 0.0f },
                { -533.9f, 8870.4f, 209.0f, 0.0f },   { -974.2f, 8136.0f, -93.8f, 0.0f },
            },
            .MinLevel = 60,
        });

        stages.push_back({
            .Name = "stage8_duel",
            .Suffix = "_duel",
            .Extends = "stage7_flight",
            .Summary = "a same-level creature out of aggro range: close in and kill it fast, taking little damage",
            // The main line runs through the whole movement block rather than beside it, so the combat root starts
            // with legs that already work. Travel stays for the same reason it did in Part I; the pet block is new
            // here, because this is the first stage with anything to send a pet at.
            .Blocks = { Core, Move, Travel, Duel, Pet },
            // 90 s: running out of time is a lost fight (Duel.Timeout), and a healer or tank against a creature with
            // twice the usual health needs half a minute to kill it after a few seconds of closing in.
            .Arenas = {
                { .Name = "duel", .Weight = 3, .Against = Opposition::Creature, .EpisodeSeconds = 90 },
                // A lake: the opponent stands in the water, on the Barrens oases stage1_move swims, so a share of
                // every class's fights are swimming ones -- reach, casting, the pet and whether to go in at all
                // are all different wet. The scenario's swim_seconds says how much of the fight was.
                { .Name = "lake", .Weight = 1, .Against = Opposition::Creature, .EpisodeSeconds = 90,
                    .Water = true,
                    .SpawnPoints = {
                { -3923.0f, -2981.0f, 31.0f, 0.0f }, { -3952.0f, -2947.0f, 40.0f, 0.0f },
                { -3964.0f, -3068.0f, 39.0f, 0.0f }, { -3879.0f, -3004.0f, 37.0f, 0.0f },
                { -4048.0f, -3051.0f, 43.0f, 0.0f }, { -3985.0f, -2911.0f, 37.0f, 0.0f },
                    },
                    .HeldOutSpawnPoints = {
                { -4017.0f, -3086.0f, 37.0f, 0.0f }, { -3926.0f, -2911.0f, 39.0f, 0.0f },
                    } },
            },
            // Ground, because until now every fight in the curriculum happened on the same square yard: the
            // combat stages take the host's single spawn point inside a per-env instance, so a duel's terrain was
            // one place, every episode, for the whole run. The same five regions the feet were taught on, and the
            // same control ground, so `clean_kill` at the gate is a claim about the fight rather than about a spot
            // the seat has stood on a million times.
            //
            // This is the combat root and the first of its kind: it moves a fight off an instance and onto a
            // shared continent, where envs are separated by phase rather than by instance. Proven at 128 envs by
            // the travel stages, but not yet by anything that spawns an opponent -- so it wants a standalone run
            // before stages 6 to 17 follow it.
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
        });

        stages.push_back({
            .Name = "stage9_pack",
            .Suffix = "_pack",
            .Extends = "stage8_duel",
            .Summary = "a pack of 2-4, casters included, usually linked: targets, interrupts, crowd control",
            .Blocks = { Core, Move, Duel, Pet, Pack },
            // 150 s: running out of time is a lost fight (Pulls.Timeout), and a pack is up to four of the duel's
            // creatures. stage1_duel's policy took 17 s a kill and its baseline 23 s, so four take 70-90 s before the
            // approach; the duel's 90 s (or the host's 60) would lose packs to the clock that play could win.
            .Arenas = { { .Name = "pack", .Against = Opposition::Pulls, .Schedule = PullSchedule::SinglePack,
                .EpisodeSeconds = 150 } },
        });

        stages.push_back({
            .Name = "stage10_gauntlet",
            .Suffix = "_gauntlet",
            .Extends = "stage9_pack",
            .Summary = "pull after pull with short breaks: heals, food and drink",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Support },
            // 450 s: pull after pull is the point. At the host's 60 s a break of 8-20 s before each pull left two or
            // three of them, with nothing to recover for. Pulls come to the seat when it waits too long and come
            // sooner as it clears them, so seven and a half minutes hold eight or more, and the solo gauntlet is won
            // by lasting to the end with Pulls.SoloGauntletWinPulls cleared (PullTuning::SoloGauntlet*).
            .Arenas = { { .Name = "gauntlet", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .EpisodeSeconds = 450 } },
        });

        // A planned run: the same eight pulls in the same order every episode, ending on an elite pack two levels
        // above. Nothing about the fights is left to learn -- stage 3 taught them -- so what is left is the plan:
        // what to spend on the opener, what to keep for the last pull, when the breather is a rest and when it is a
        // chance to get ahead. Won by clearing the last pull alive; the clock running out is a loss however far it
        // got.
        stages.push_back({
            .Name = "stage11_endurance",
            .Suffix = "_endurance",
            .Extends = "stage10_gauntlet",
            .Summary = "a known run of eight pulls, won by finishing it",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Support },
            .Arenas = { { .Name = "endurance", .Against = Opposition::Pulls, .Schedule = PullSchedule::Sequence,
                .EpisodeSeconds = 900 } },
        });

        // The PvP branch: off the endurance run, without the PvE blocks it would never fill. Self-play from the
        // first PvP stage: two learned seats of any classes, the far side played by the live policy or by a frozen
        // earlier checkpoint (the learner's cast league), so the opponent is always something that learned to
        // fight rather than a script. The scripted enemy player survives only as the evaluation yardstick and in
        // the evade, hide and stealth drills, whose lesson is escaping a hunter that searches.
        stages.push_back({
            .Name = "stage12_pvp",
            .Suffix = "_pvp",
            .Extends = "stage11_endurance",
            .Summary = "self-play one-on-one: two learned seats of any classes",
            .Blocks = { Core, Move, Duel, Pet, Pvp },
            .Arenas = { { .Name = "arena_1v1", .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                .Pvp = true } },
        });

        // Two drills about not fighting, between the scripted duel and self-play. Everything up to here rewards
        // winning the fight in front of it, so a losing fight is a class of situation the policy has never been
        // paid to handle: it dies with its cooldowns up. Both sit on the trunk order but only the first is on the
        // trunk, and both are played by all eighteen class/roles.
        stages.push_back({
            .Name = "stage13_evade",
            .Suffix = "_evade",
            .Extends = "stage12_pvp",
            .Summary = "a fight it cannot win: break away, break line of sight, and live to the end of it",
            .Blocks = { Core, Move, Duel, Pet, Pvp },
            // Ten levels up. Six was chosen against the open-field arena and stopped being a losing fight once
            // the spawn had cover: the `fight` baseline, which never tries to hide, went from winning 0.188 of
            // these to winning 0.447, because terrain blocks the scripted opponent's casting as readily as it
            // hides the seat. A drill about leaving a fight has to be a fight worth leaving.
            //
            // The win condition is being alive at 120 s, which is what makes running, cover and the escape
            // cooldown the only policy that scores. Nothing pays time spent hidden -- that would be farmed by
            // walking to the far corner at t=0 -- only the transition out of contact (RewardTerm::BrokeContact,
            // once per break with a cooldown).
            .Arenas = { { .Name = "evade", .Against = Opposition::ScriptedPlayer, .Pvp = true,
                .EpisodeSeconds = 120, .OpponentLevelBonus = 10 } },
            // Cover is the whole point, and the default spawn is open field: the first run of this stage read
            // exactly 0.000 contact breaks for twelve of the eighteen class/roles, because on flat ground
            // nothing but stealth can break line of sight. These are walkable ground inside Durnholde Keep
            // (walls, towers, two levels) and among the Southshore farms (buildings, fences, trees), on the
            // same instance map the PvP line already fights on.
            .MapId = 560,
            .SpawnPoints = HillsbradGround(),
            .HeldOutSpawnPoints = HillsbradControl(),
        });

        // Hiding, for every class and every race. Stealth is one way to do it and the rarest -- four of the
        // eighteen class/roles have a stealth aura in their kit -- but it is not the lesson. The lesson is
        // becoming unseen and staying unseen, which every class can do with terrain, with distance, and with
        // whatever its kit and its race give it: Blink, Disengage, Feign Death, Invisibility, Sprint, and
        // Shadowmeld for any night elf. So this stage is played by all eighteen, graded on the outcome rather
        // than on which button produced it.
        stages.push_back({
            .Name = "stage14_hide",
            .Suffix = "_hide",
            .Extends = "stage13_evade",
            .Summary = "get out of sight and stay there, and hide again after being found",
            .Blocks = { Core, Move, Duel, Pet, Pvp },
            // Six levels up rather than the evade drill's ten. The fight is winnable often enough that hiding
            // is a choice rather than the only move left, which is the difference between this stage and the
            // one before it: stage 16 is about leaving a fight that is lost, this one is about not being found
            // once you have.
            .Arenas = { { .Name = "hide", .Against = Opposition::ScriptedPlayer, .Pvp = true,
                .EpisodeSeconds = 120, .OpponentLevelBonus = 6 } },
            // Cover is the whole point, and the default spawn is open field: the first run of this stage read
            // exactly 0.000 contact breaks for twelve of the eighteen class/roles, because on flat ground
            // nothing but stealth can break line of sight. These are walkable ground inside Durnholde Keep
            // (walls, towers, two levels) and among the Southshore farms (buildings, fences, trees), on the
            // same instance map the PvP line already fights on.
            .MapId = 560,
            .SpawnPoints = HillsbradGround(),
            .HeldOutSpawnPoints = HillsbradControl(),
        });

        // Stealth, which is not the same lesson as hiding and is why it is a stage of its own. Hiding is not
        // being found: every class does it, with terrain and distance and whatever its kit gives it, and
        // Shadowmeld counts there. Stealth is being *close* and not found -- crossing the ground to someone
        // who is looking for you and arriving inside strike range with the opener still in hand. Shadowmeld
        // cannot do that, because it breaks the moment you move; only a real stealth aura can.
        //
        // So this stage is restricted (NeedsStealth): only the classes whose kit can really stealth play it. It
        // used to be forced to be a leaf for that reason, and the arena stage reached past it -- but that made the
        // rule wrong in the case it matters most. In a druid-only or rogue-only run every layout stealths, the
        // checkpoint is not partial at all, and there is nothing to protect against; the check that matters now
        // lives in animus.bootstrap, where the run's actual layouts are known. So the chain runs through it, and
        // a feral druid's Prowl reaches the arena and the flag instead of dying here.
        stages.push_back({
            .Name = "stage15_stealth",
            .Suffix = "_stealth",
            .Extends = "stage14_hide",
            .Summary = "close on a stronger enemy unseen, hold there in strike range, and open from it",
            .NeedsStealth = true,
            .Blocks = { Core, Move, Duel, Pet, Pvp },
            // Six levels up, as the hide stage: the fight has to be one the opener decides, so that getting
            // into position is worth the time it costs rather than a flourish before a fight that was winnable
            // anyway.
            .Arenas = { { .Name = "stealth", .Against = Opposition::ScriptedPlayer, .Pvp = true,
                .EpisodeSeconds = 120, .OpponentLevelBonus = 6 } },
            // The same cover the other two drills use: an approach needs something to come round.
            .MapId = 560,
            .SpawnPoints = HillsbradGround(),
            .HeldOutSpawnPoints = HillsbradControl(),
        });

        stages.push_back({
            .Name = "stage16_companion",
            .Suffix = "_companion",
            .Extends = "stage15_stealth",
            // The pack, gauntlet and support blocks were trained across stages 6-8 and then dropped by the
            // PvP line this stage extends, so without this merge they would start from zero here and three
            // stages of training would be spent again. A merge seeds exactly the blocks the extended stage
            // does not have.
            .Merges = { "stage11_endurance" },
            .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Support },
            // 450 s, as the solo gauntlet: without its own length the arena took the host's 60 s, two or three pulls
            // with nothing to recover for and no win to reach (Pulls.OwnerWinPulls).
            .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .Owner = true, .OwnerCast = true, .EpisodeSeconds = 450 } },
        });

        stages.push_back({
            .Name = "stage17_party",
            .Suffix = "_party",
            .Extends = "stage16_companion",
            .Summary = "four learned seats and the scripted owner against elite-heavy pulls",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "party", .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true, .EpisodeSeconds = 450 } },
        });

        // Holding what the group pulls. Tanks exist in stages 5, 8, 13 and 14, but the stage is won by the clear,
        // so a tank that loses an add to the healer and takes it back is scored the same as one that never lost it.
        // Here seat 0 always holds the pull (ArenaDefinition::SeatAptitudes) and the pulls are a party's, so what the
        // episode is about is the threat table -- which the seat can now read (Encoding::ThreatShare).
        //
        // Read its scores knowing that the hazard charge lands about four times harder on a tank than on a ranged
        // seat (stage19_arena: paladin_tank -0.834 an episode against priest_dps -0.198), because a tank cannot walk
        // out of what it is holding an enemy in. That is Hazards.Standing being tuned for a seat with a choice.
        stages.push_back({
            .Name = "stage18_tanking",
            .Suffix = "_tanking",
            .Extends = "stage17_party",
            .Summary = "a fixed tank seat beside its group: hold what the pull brings, and keep it off the others",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "tanking", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true, .EpisodeSeconds = 300,
                .SeatAptitudes = { AptitudeDemand::HoldsThePull() } } },
        });

        // Keeping a group up when the damage outruns one heal. Stage 5 has healers, but its win is the clear and
        // stage 4 measured the seat putting 13% of its healing into the owner: triage is never the episode.
        //
        // Before reading its numbers, know that stage 5 carries a resurrection exploit this drill inherits: nothing
        // clears m_resurrectGUID, so one landed Rebirth makes every later death of that ally an instant free
        // resurrect that pays RewardTerm::Revive again (stage9_companion measured druid_dps at 38.4 revives an
        // episode, 88% of its return). A forced healer seat will find it faster than anything else in the
        // curriculum. Fix that before trusting a triage score.
        stages.push_back({
            .Name = "stage19_triage",
            .Suffix = "_triage",
            .Extends = "stage18_tanking",
            .Summary = "a fixed healer seat beside its group: keep the hurt one up, and spend mana to do it",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "triage", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true, .EpisodeSeconds = 300,
                .SeatAptitudes = { AptitudeDemand::KeepsThemUp() } } },
        });

        // Warsong Gulch's rules between two learned seats (self-play): take the other side's flag home, return one's
        // own, stop the carrier. Mounting between the bases and being dismounted by the flag come from travel; the
        // fight from the arena.
        //
        // The Barrens, for stage 9's reason: the second base is placed by the same objective search, 100-180 yd from
        // the first, and only open ground has room for it.
        // Life outside the fight, learned in the sim. A quest of the seat's level band -- its giver, the world's own
        // creatures around its objectives, its turn-in -- copied out of the spawn tables into the env's phase
        // (QuestEncounter, LifeWorld), so a seat takes it, does it and hands it in with what it learned to fight
        // with. The rung is the band (15-20, 35-40, 58-60), the side is drawn with it and the race follows. It
        // extends the endurance run, since a quest is a run of small fights with walking between them, and merges
        // the travel stage for the mount and the objective the trips were steered to.
        stages.push_back({
            .Name = "stage20_quest",
            .Suffix = "_quest",
            .Extends = "stage11_endurance",
            .Merges = { "stage6_travel" },
            .Summary = "a quest of the level band: take it, do it, hand it in",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Support, World },
            .Arenas = { { .Name = "quest", .Against = Opposition::Quest, .EpisodeSeconds = 300 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .MinLevel = 15,
        });

        // A field of the band's herb and ore nodes, with the zone's own creatures among them, and the seat holding
        // the gathering professions at the band's skill (GatherEncounter): find the node, open it, take what is in
        // it, skin what it killed, and do not die to what lives there. The episode is scored by what was gathered
        // before the clock.
        stages.push_back({
            .Name = "stage21_gather",
            .Suffix = "_gather",
            .Extends = "stage20_quest",
            .Summary = "herbs and ore of the band's zones, with what lives among them",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Support, World },
            .Arenas = { { .Name = "gather", .Against = Opposition::Gather, .EpisodeSeconds = 240 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .MinLevel = 15,
        });

        // A town of the seat's side, its traders copied into the phase around the inn (TownEncounter). The seat
        // arrives with junk in its bags, half its durability gone, one food and one drink, and two better items it
        // has not put on: sell, repair, restock, dress. Won when all four are done before the clock.
        stages.push_back({
            .Name = "stage22_town",
            .Suffix = "_town",
            .Extends = "stage21_gather",
            .Summary = "a town: sell the junk, repair, restock, put the better item on",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Support, World },
            .Arenas = { { .Name = "town", .Against = Opposition::Town, .EpisodeSeconds = 120 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .MinLevel = 15,
        });

        // The first real instance: a party of four learned seats and their cast owner against a dungeon's own
        // scripted bosses, in the dungeon (InstanceEncounter). The rungs are the bosses of five dungeons across the
        // level bands -- Ragefire Chasm at 15 through heroic Utgarde Keep at 80 -- so the rung fixes the level as
        // well as the fight, and a class climbs a band at a time. The seats spawn at the front door and are taken
        // to the boss along the server's own path; the trash they never pulled is cleared, the boss's own adds stay.
        // The party gauntlet on the host map is kept as a control arena at a tenth of the episodes: the same
        // policy is graded on real bosses and on the pool encounter it has always been graded on.
        stages.push_back({
            .Name = "stage23_dungeon",
            .Suffix = "_dungeon",
            .Extends = "stage19_triage",
            .Summary = "a party and its owner against real dungeon bosses, in their instances",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = {
                { .Name = "dungeon", .Weight = 9, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .Owner = true, .OwnerCast = true, .PartyGroup = true, .Instance = InstanceLadder::Dungeon,
                    .EpisodeSeconds = 300 },
                { .Name = "dungeon_control", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true,
                    .EpisodeSeconds = 300 },
            },
        });

        stages.push_back({
            .Name = "stage24_flag",
            .Suffix = "_flag",
            .Extends = "stage19_triage",
            // stage6_travel for the travel block, stage12_pvp for the pvp block: this stage extends the
            // party line, which has neither, and a flag match is a fight between two seats before it is
            // anything else.
            .Merges = { "stage6_travel", "stage12_pvp" },
            .Summary = "capture the flag one-on-one: bases 100-180 yd apart, first to three captures",
            .Blocks = { Core, Move, Duel, Pet, Pvp, Travel, Flag },
            .Arenas = { { .Name = "flag", .Seats = SeatPlan::Mirror, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 300 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorGround(),
            .HeldOutSpawnPoints = KalimdorControl(),
            .MinLevel = 20,
        });

        stages.push_back({
            .Name = "stage25_warsong",
            .Suffix = "_warsong",
            .Extends = "stage24_flag",
            // Ten a side is a group: the party block was trained at stages 15-17 and the flag line it extends
            // does not carry it.
            .Merges = { "stage19_triage" },
            .Summary = "ten against ten for the flag: escort the carrier, hold the base, stop theirs",
            .Blocks = { Core, Move, Duel, Pet, Pvp, Travel, Flag, Party },
            .Arenas = { { .Name = "warsong", .Seats = SeatPlan::Teams, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 420 } },
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

        // The raid branch: MAX_SEATS learned seats as RAID_GROUPS groups of GROUP_SEATS, each group with its own
        // tank and healer (SeatPlan::Raid). A raid is not a bigger party -- it is many seats around one large enemy,
        // which is why the opponents are an elite and its adds rather than a pack per seat, and why the mechanics a
        // seat can now read (a cast worth interrupting, something on the ground, where it stands on the threat
        // table) matter far more here than they do alone.
        //
        // NOT in the default queue, and not runnable at the usual env count: 40 seats an env is 40 bots an env, so
        // AnimusForge.Envs has to come down roughly in proportion (a few dozen envs, not 128) before either of these
        // is started. Train by name: `forge start stage28_raid_single`.
        stages.push_back({
            .Name = "stage28_raid_single",
            .Suffix = "_raid",
            .Extends = "stage19_triage",
            .Summary = "a raid of eight groups against one elite and its adds, won or lost as the single pack is",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "raid_single", .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::SinglePack, .EpisodeSeconds = 300 } },
            .InDefaultQueue = false,
        });

        // The raid's endurance: pull after pull with recovery between, which is what a wing of a raid instance is
        // before the boss of it. Seeded from the single fight, as the gauntlet is from the pack.
        stages.push_back({
            .Name = "stage29_raid_gauntlet",
            .Suffix = "_raidrun",
            .Extends = "stage28_raid_single",
            .Summary = "a raid clearing pull after pull, recovering between them",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "raid_gauntlet", .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .EpisodeSeconds = 600 } },
            .InDefaultQueue = false,
        });

        // Two a side, and a director. The seats already fight one on one from the arena; what is new is being
        // told what the pair is doing -- concentrate on that one, you take the next interrupt -- and learning
        // that following it pays. The director is scripted here and deliberately legible: the lowest enemy is the
        // focus, the duty goes round the side in turn. A learned director comes next, and meets seats that
        // already know how to be commanded rather than seats that have never heard an order.
        stages.push_back({
            .Name = "stage26_duo_led",
            .Suffix = "_duo",
            .Extends = "stage25_warsong",
            // The pack and support blocks, trained at stages 6-8; the flag line it extends dropped both.
            .Merges = { "stage11_endurance" },
            .Summary = "two against two, told who to kill and whose turn it is: follow the call",
            .Blocks = { Core, Move, Duel, Pack, Pet, Pvp, Context, Hostiles, Support, Order },
            .Arenas = { { .Name = "duo", .Seats = SeatPlan::Teams, .Against = Opposition::MirrorSeat,
                .Pvp = true, .EpisodeSeconds = 180, .Directed = true, .DirectorLearned = true,
                .Places = true, .TeamSeats = 2 } },
            .MinLevel = 20,
        });

        // The crossroads: both branches join. It extends the party (the trunk and every PvE block), takes the pvp
        // block from the arena, and each parent teaches the arenas it trained on. Two new situations need PvE
        // and PvP in one
        // episode: an ambush of the owner in the middle of the gauntlet, and a lone enemy player attacking the owner.
        // Every PvE arena plays long episodes; the one-on-ones stay short.
        stages.push_back({
            .Name = "stage27_crossroads",
            .Suffix = "_crossroads",
            .Extends = "stage19_triage",
            // The leaf of every other branch, so nothing trained in the queue is left behind: the PvP line
            // through warsong, the movement line through flight. The PvE line arrives by extension.
            .Merges = {
                "stage26_duo_led", "stage25_warsong", "stage23_dungeon", "stage22_town", "stage21_gather",
                "stage20_quest", "stage12_pvp", "stage7_flight", "stage16_companion", "stage10_gauntlet", "stage8_duel",
            },
            .Summary = "PvE, PvP and life in one policy: every earlier situation, an ambush mid-gauntlet, a ganked owner",
            .Blocks = { Core, Move, Travel, Duel, Pet, Pack, Gauntlet, Companion, Party, Pvp, Context, Hostiles,
                Support, World },
            .Arenas = {
                { .Name = "companion", .Weight = 20, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .Owner = true, .OwnerCast = true, .EpisodeSeconds = 300 },
                { .Name = "party", .Weight = 20, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .OwnerCast = true, .PartyGroup = true, .EpisodeSeconds = 300 },
                { .Name = "arena_1v1", .Weight = 25, .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 60 },
                { .Name = "gauntlet", .Weight = 10, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .EpisodeSeconds = 450 },
                { .Name = "duel", .Weight = 5, .Against = Opposition::Creature, .EpisodeSeconds = 60 },
                { .Name = "ambush", .Weight = 15, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .Owner = true, .OwnerCast = true, .EpisodeSeconds = 300, .Ambushers = 2 },
                { .Name = "escort_duel", .Weight = 5, .Against = Opposition::Ambush, .Owner = true, .OwnerCast = true,
                    .EpisodeSeconds = 90, .Ambushers = 1 },
                // The shipped model has met a scripted boss: the dungeon ladder, in its instances.
                { .Name = "dungeon", .Weight = 10, .Seats = SeatPlan::Party, .Against = Opposition::Instance,
                    .Owner = true, .OwnerCast = true, .PartyGroup = true, .Instance = InstanceLadder::Dungeon,
                    .EpisodeSeconds = 300 },
                // ... and lived a little: a quest, a field of nodes, a town, so the life it learned ships too.
                { .Name = "quest", .Weight = 5, .Against = Opposition::Quest, .EpisodeSeconds = 300 },
                { .Name = "gather", .Weight = 3, .Against = Opposition::Gather, .EpisodeSeconds = 240 },
                { .Name = "town", .Weight = 2, .Against = Opposition::Town, .EpisodeSeconds = 120 },
            },
        });

        // The real raids, by name, each seeded from the one before and all from the dungeon: ten seats in Karazhan
        // and Naxxramas, twenty-five in Naxxramas, forty in Molten Core, Blackwing Lair and the Temple of
        // Ahn'Qiraj. No owner (forty seats leave no slot for one): seat 0 leads the raid group. Each keeps the
        // synthetic single pack at its own seat count as a control arena. Not in the default queue: forty seats an
        // env is forty bots an env, so AnimusForge.Stage.<name>.Envs sets each stage's own env count (32, 16, 8).
        stages.push_back({
            .Name = "stage30_raid10",
            .Suffix = "_raid10",
            .Extends = "stage23_dungeon",
            .Summary = "ten seats against Karazhan's and Naxxramas's bosses, in their raids",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = {
                { .Name = "raid", .Weight = 9, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid10, .RaidSeats = 10, .EpisodeSeconds = 360 },
                { .Name = "raid_control", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::SinglePack, .RaidSeats = 10, .EpisodeSeconds = 300 },
            },
            .InDefaultQueue = false,
        });

        stages.push_back({
            .Name = "stage31_raid25",
            .Suffix = "_raid25",
            .Extends = "stage30_raid10",
            .Summary = "twenty-five seats against Naxxramas's bosses",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = {
                { .Name = "raid", .Weight = 9, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid25, .RaidSeats = 25, .EpisodeSeconds = 420 },
                { .Name = "raid_control", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::SinglePack, .RaidSeats = 25, .EpisodeSeconds = 300 },
            },
            .InDefaultQueue = false,
        });

        stages.push_back({
            .Name = "stage32_raid40",
            .Suffix = "_raid40",
            .Extends = "stage31_raid25",
            .Summary = "forty seats against the classic raids' bosses: Molten Core, Blackwing Lair, Ahn'Qiraj",
            .Blocks = { Core, Move, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = {
                { .Name = "raid", .Weight = 9, .Seats = SeatPlan::Raid, .Against = Opposition::Instance,
                    .PartyGroup = true, .Instance = InstanceLadder::Raid40, .RaidSeats = 40, .EpisodeSeconds = 480 },
                { .Name = "raid_control", .Weight = 1, .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::SinglePack, .RaidSeats = 40, .EpisodeSeconds = 300 },
            },
            .InDefaultQueue = false,
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
        bool const raidGroup = instance && arena.Seats == SeatPlan::Raid;
        if (arena.PartyGroup && !stage.Has(BlockId::Party))
            return "a party group needs the party block";
        if (arena.PartyGroup && !raidGroup && (!arena.Owner || arena.Seats != SeatPlan::Party))
            return "a party group needs an owner and party seats, unless it is a raid in an instance";
        if (arena.OwnerCast && !arena.Owner)
            return "a cast owner is still an owner: the arena has to have one";
        if (arena.OwnerCast && stage.SeatCount() + TEAM_COUNT + 1 > MAX_SEATS)
            return "a cast owner needs a seat slot past the seats and the directors, and a raid has none to spare";
        // Self-play: one seat a side in a Mirror, TEAM_SEATS of them in a Teams arena, and a team match is a
        // flag match -- there is nothing else for two learned sides of ten to be playing.
        bool const selfPlay = arena.Seats == SeatPlan::Mirror || arena.Seats == SeatPlan::Teams;
        if (selfPlay != (arena.Against == Opposition::MirrorSeat || flag))
            return "self-play seats go with fighting the mirror seat or a flag match, and only with them";
        if (arena.Seats == SeatPlan::Teams && !flag && arena.Against != Opposition::MirrorSeat)
            return "team seats fight the other team, at a flag or in an arena";
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
        if (arena.Directed && arena.Seats != SeatPlan::Teams)
            return "a director commands a side, so its arena needs team seats";
        if (arena.Ambushers > MAX_AMBUSHERS)
            return "at most " + std::to_string(MAX_AMBUSHERS) + " ambushers";
        if (arena.Ambushers > 0 && !(pulls || ambushOnly))
            return "ambushers join pulls, or are the whole fight (Opposition::Ambush)";
        if (arena.Ambushers > 0 && (!arena.Owner || !stage.Has(BlockId::Pack)))
            return "ambushers attack an owner and take enemy slots (the pack block)";
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
        if (arena.OpponentLevelBonus != 0 && arena.Against != Opposition::ScriptedPlayer)
            return "only a scripted enemy player takes a level bonus";
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
        if (life && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None))
            return "a life arena is one seat on its own, with no pulls";
        if (stage.Has(BlockId::World) && !stage.AnyArena([](ArenaDefinition const& other)
            {
                return other.Against == Opposition::Quest || other.Against == Opposition::Gather
                    || other.Against == Opposition::Town;
            }))
            return "the world block wants a life arena to be read in";

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
        case SeatPlan::Teams:  return std::min(TeamSeats, TEAM_SEATS) * TEAM_COUNT;
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
                continue;
            }

            valid.push_back(std::move(stage));
        }

        return valid;
    }();

    return stages;
}

Animus::Curriculum::StageDefinition const* Animus::Curriculum::FindStage(std::string_view name)
{
    for (StageDefinition const& stage : CurriculumStages())
        if (stage.Name == name)
            return &stage;

    return nullptr;
}
