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
 *   movement   move1_controls ─ move2_ground ─ move3_vertical ─ move4_water ─ move5_routes ─ move6_mounted
 *              ─ move7_follow
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

    /// Open, flat ground on Kalimdor for the controls stage (M1): the Barrens' scrub and Mulgore's grass, from the
    /// first curriculum's training ground (curriculum-v1 KalimdorGround), every point stood on with `forge rays` then.
    /// Its Durotar and Dustwallow points are left out: they are canyon, rock and broken shore, M2's ground.
    std::vector<Position> KalimdorFlats()
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
            // Mulgore (its other point, (-1210, -93), is on the ridge the broken list climbs)
            { -1225.2f, 106.6f, 131.4f, 0.0f },
        };
    }

    /// Kalimdor's control ground (curriculum-v1 KalimdorControl): three regions in no training list, each point with
    /// five of its eight bearings open for forty yards. The plan named the Durotar flats for this; the Durotar points
    /// the first curriculum validated are canyon and rock, so the open control ground it held out stands in until
    /// flat Durotar points are stood on.
    std::vector<Position> KalimdorFlatsControl()
    {
        return {
            { -1637.9f, 3082.9f, 31.9f, 0.0f },   { -1168.4f, 2713.1f, 112.1f, 0.0f },
            { -561.0f, 2069.0f, 90.0f, 0.0f },    { 4012.0f, -788.0f, 286.0f, 0.0f },
            { 1969.6f, -2339.0f, 89.4f, 0.0f },   { 1813.0f, -2424.0f, 93.0f, 0.0f },
            { 1965.0f, -2559.0f, 86.0f, 0.0f },
        };
    }

    /// Nagrand's plateaus (map 530; curriculum-v1 NagrandControl): the Throne of the Elements' grass trains, the
    /// two points far off to the east and south are held out.
    std::vector<Position> NagrandPlateaus()
    {
        return {
            { -850.6f, 6517.2f, 172.6f, 0.0f },   { -842.4f, 6578.1f, 172.7f, 0.0f },
            { -652.9f, 6576.9f, 170.4f, 0.0f },   { -685.5f, 6609.0f, 176.6f, 0.0f },
        };
    }

    std::vector<Position> NagrandPlateausControl()
    {
        return {
            { -533.9f, 8870.4f, 209.0f, 0.0f },
            // UNVERIFIED as open plateau ground: at z -93.8 it is far below Nagrand's grass. It was a flight
            // stage's take-off point; the §7.5 dry check, before M1's first run, keeps or replaces it.
            { -974.2f, 8136.0f, -93.8f, 0.0f },
        };
    }

    /// Broken ground on Kalimdor for the ground stage (M2; curriculum-v1 BrokenGround): chosen by local relief and then
    /// stood on with `forge rays` -- at least ~4.5 yd of clearance, short reaches on several bearings, so there is
    /// something to walk round. The cliff feet at the end are low ground under a plateau 30-50 yd up whose top the
    /// route reaches by one ramp at 1.3-1.8x the straight line.
    std::vector<Position> KalimdorBroken()
    {
        return {
            // Mulgore/Barrens ridge, relief 78 over a 179 yard span
            { -1401.0f, -85.0f, 159.0f, 0.0f },   { -1286.0f, 107.0f, 130.9f, 0.0f },
            // Barrens ridge, relief 64 over 200
            { -454.0f, -2419.0f, 93.0f, 0.0f },   { -373.0f, -2323.0f, 94.0f, 0.0f },
            // Durotar: canyon and rock
            { -49.4f, -4313.6f, 68.7f, 0.0f },    { -107.5f, -4302.0f, 61.7f, 0.0f },
            { 642.0f, -4185.0f, 15.0f, 0.0f },    { 633.0f, -4298.0f, 18.0f, 0.0f },
            // Dustwallow Marsh: broken shore (the markers' walking way never swims: TravelPlaceRules::DryOnly)
            { -2631.0f, -3607.0f, 42.0f, 0.0f },  { -2751.0f, -3660.0f, 39.0f, 0.0f },
            { -2851.0f, -3650.0f, 33.0f, 0.0f },  { -2987.0f, -3940.0f, 39.0f, 0.0f },
            // Cliff feet
            { -2032.2f, -3618.1f, 22.3f, 0.0f },  { -2563.7f, -3798.6f, 7.0f, 0.0f },
            { 190.8f, -4516.5f, 27.1f, 0.0f },    { 479.5f, -4658.7f, 41.7f, 0.0f },
        };
    }

    /// The southern Barrens escarpment, relief 47, in no training list (curriculum-v1 BrokenControl).
    std::vector<Position> KalimdorBrokenControl()
    {
        return {
            { -623.5f, -3166.8f, 91.7f, 0.0f },   { -405.9f, -3207.1f, 186.5f, 0.0f },
            { -441.9f, -3162.0f, 210.3f, 0.0f },
        };
    }

    /// Cliff feet under plateaus 30-50 yd up whose tops the route reaches by one ramp at 1.3-1.8x the straight line
    /// (curriculum-v1 BrokenGround's last four): the vertical stage's climbs.
    std::vector<Position> CliffFeet()
    {
        return {
            { -2032.2f, -3618.1f, 22.3f, 0.0f },  // southern Barrens, plateau +40..50
            { -2563.7f, -3798.6f, 7.0f, 0.0f },   // Barrens/Dustwallow edge, +41..49
            { 190.8f, -4516.5f, 27.1f, 0.0f },    // Durotar canyon, +30..37
            { 479.5f, -4658.7f, 41.7f, 0.0f },    // Durotar canyon, +28..34
        };
    }

    /// The foot of the southern Barrens escarpment, whose top LedgeTopsControl holds out (curriculum-v1 BrokenControl).
    std::vector<Position> CliffFeetControl()
    {
        return {
            { -623.5f, -3166.8f, 91.7f, 0.0f },
        };
    }

    /// Plateau tops above the cliff feet (curriculum-v1 LedgeTops), stood on with `forge rays` facing the edge; the way
    /// round 1.9-12x the straight line, drops of 11-44 yd.
    std::vector<Position> LedgeTops()
    {
        return {
            { -2063.9f, -3645.5f, 66.1f, 0.0f },   // southern Barrens, above (-2032, -3618): 44 yd
            { -2094.8f, -3644.6f, 72.4f, 0.0f },   // beside it: 11 yd
            { 394.1f, -4599.2f, 76.2f, 0.0f },     // Durotar canyon, above (480, -4659): 23 yd
            { 85.4f, -4543.8f, 58.4f, 0.0f },      // Durotar canyon: 18 yd
            { -519.0f, -4076.9f, 69.9f, 0.0f },    // southern Barrens: 27 yd
            { -2379.6f, 459.2f, 76.8f, 0.0f },     // Mulgore: 16-25 yd
            { -4052.7f, -2145.5f, 90.2f, 0.0f },   // Thousand Needles: 40 yd
            { -4449.9f, -2914.0f, 40.0f, 0.0f },   // Thousand Needles: 16-18 yd
        };
    }

    /// The southern Barrens escarpment's top (curriculum-v1 LedgeControl); the deep ones kill without Slow Fall.
    std::vector<Position> LedgeTopsControl()
    {
        return {
            { -545.9f, -3054.0f, 138.1f, 0.0f },   // 46 yd
            { -515.9f, -3149.0f, 161.5f, 0.0f },   // 67 yd
            { -481.2f, -3249.9f, 164.5f, 0.0f },   // 70 yd
        };
    }

    /// Inns on Kalimdor (curriculum-v1 Inns): on the mesh, inside by their WMO group, z on the floor; clearance 1.7-6.6
    /// yd. Feralas and Thousand Needles are among the plan's arenas.
    std::vector<Position> Inns()
    {
        return {
            { -3182.4f, -2920.8f, 33.56f, 0.0f },  // Brackenwall Village
            { -4461.9f, 242.6f, 39.11f, 0.0f },    // Feralas
            { -4622.3f, -3172.1f, 34.81f, 0.0f },  // Mudsprocket
            { -2366.7f, -346.0f, -8.96f, 0.0f },   // Mulgore
            { -1051.4f, -3653.8f, 23.88f, 0.0f },  // The Barrens
            { -5477.9f, -2460.3f, 89.28f, 0.0f },  // Thousand Needles
            { 6688.0f, -4670.1f, 721.69f, 0.0f },  // Winterspring
        };
    }

    /// Rooms no training episode stands in (curriculum-v1 InnsControl): Tanaris is the plan's held-out ground; Desolace
    /// never once built an episode in the first curriculum (too tight for its objectives) and is left out.
    std::vector<Position> InnsControl()
    {
        return {
            { -3615.5f, -4467.3f, 21.10f, 0.0f },  // Theramore Isle
            { -7162.1f, -3845.9f, 9.51f, 0.0f },   // Tanaris
        };
    }

    /// The banks of the Barrens oases (curriculum-v1 OasisShore), on the shore, not in the pool: a crossing whose dry
    /// way round is the longer one.
    std::vector<Position> OasisShore()
    {
        return {
            { -3923.0f, -2981.0f, 31.0f, 0.0f }, { -3952.0f, -2947.0f, 40.0f, 0.0f },
            { -3964.0f, -3068.0f, 39.0f, 0.0f }, { -3879.0f, -3004.0f, 37.0f, 0.0f },
            { -4048.0f, -3051.0f, 43.0f, 0.0f }, { -3985.0f, -2911.0f, 37.0f, 0.0f },
        };
    }

    /// The far side of the same pond, held out (curriculum-v1 OasisControl): no second pond wide enough was found.
    std::vector<Position> OasisControl()
    {
        return {
            { -4017.0f, -3086.0f, 37.0f, 0.0f }, { -3926.0f, -2911.0f, 39.0f, 0.0f },
        };
    }

    /// Stonebull Lake's banks in Mulgore (curriculum-v1 StonebullShore): 33 yd deep in the middle, banks that slope in
    /// at the water line, chosen from the map tiles so a tenth to a third of 20-120 yd draws land on a bed under 6-40
    /// yd of water.
    std::vector<Position> StonebullShore()
    {
        return {
            { -1946.0f, -558.0f, -11.9f, 0.0f }, { -1954.0f, -521.0f, -11.1f, 0.0f },
            { -2192.0f, -712.0f, -14.5f, 0.0f }, { -2192.0f, -571.0f, -14.9f, 0.0f },
            { -2196.0f, -175.0f, -13.1f, 0.0f }, { -2254.0f, -137.0f, -10.8f, 0.0f },
        };
    }

    /// Lake Elune'ara in Moonglade, held out from the lakebeds (curriculum-v1 EluneAraShore): up to 66 yd deep.
    std::vector<Position> EluneAraShore()
    {
        return {
            { 7675.0f, -2775.0f, 454.5f, 0.0f }, { 7508.0f, -2617.0f, 453.3f, 0.0f },
        };
    }

    /// Outland's ground to take off from (curriculum-v1 OutlandGround): Hellfire's broken flats, Zangarmarsh's mushroom
    /// basins and Shadowmoon's ridges. Terokkar's two points are held out (the plan's held-out flight ground).
    std::vector<Position> OutlandGround()
    {
        return {
            { 170.0f, 2589.0f, 93.0f, 0.0f },     { 169.0f, 2708.0f, 101.0f, 0.0f },     // Hellfire Peninsula
            { -3260.0f, 2690.0f, 85.0f, 0.0f },   { -3293.0f, 2832.0f, 125.0f, 0.0f },   // Zangarmarsh
            { -3631.0f, 3741.0f, 298.0f, 0.0f },  { -3721.0f, 3746.0f, 284.0f, 0.0f },   // Shadowmoon Valley
        };
    }

    std::vector<Position> OutlandControl()
    {
        return {
            { -1750.0f, 5154.0f, -37.0f, 0.0f },  { -1730.0f, 5282.0f, -32.0f, 0.0f },   // Terokkar Forest
        };
    }

    /// Every stage, every base before the stages that extend it.
    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        // M1 -- controls: what a new player learns in the first minute. Forward, turning, strafing, and stopping
        // where it meant to: a marker 5 to 60 yards off, anywhere round (behind included, by the last rung), stopped
        // on inside a radius that tightens from four yards to half a yard (the user's "stop exactly on the marker",
        // 2026-10-05), then the next, three to eight an episode. Open ground only: the straight line is the way.
        //
        // Core and the goal block stay as the layout's frame (the character and its kit, the goal head the learner
        // sizes from the goal block); Move is the whole lesson. Nothing to fight, so no duel block.
        stages.push_back({
            .Name = "move1_controls",
            .Suffix = "_controls",
            .Extends = "",
            .Summary = "markers on open ground, 5 to 60 yd off and anywhere round: get there and stop exactly on them",
            .Blocks = { Core, Move, Goal },
            .Arenas = {
                { .Name = "plains", .Weight = 3, .Against = Opposition::Markers, .EpisodeSeconds = 120 },
                { .Name = "nagrand", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 120,
                    .SpawnPoints = NagrandPlateaus(), .MapId = MAP_OUTLAND,
                    .HeldOutSpawnPoints = NagrandPlateausControl() },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorFlats(),
            .HeldOutSpawnPoints = KalimdorFlatsControl(),
        });

        // M2 -- ground: real ground, slopes the client can and cannot climb, rocks, ridges, cliff feet and their
        // ramps, getting round things and not getting stuck. Markers 20 to 120 yards off with something in the way:
        // the walking way is 1.0 to 1.6 times the straight line and more as the ladder climbs, so the straight line
        // is often not walkable. Stopping on them as M1 taught, inside a yard. Stuck and Wall are charged as noise
        // prices (MarkerGround.*), and the progress shaping follows the route (a training signal, never seen).
        //
        // Kalimdor only for now: the plan's forests and fenced farms (Ashenvale, Duskwood, Westfall, Goldshire) and
        // its Eastern Kingdoms hills need points stood on first (§7.5's dry check); Hillsbrad, its held-out ground,
        // likewise -- the first curriculum's Hillsbrad points are map 560's, not Eastern Kingdoms'.
        stages.push_back({
            .Name = "move2_ground",
            .Suffix = "_ground",
            .Extends = "move1_controls",
            .Summary = "markers on broken ground with something in the way: find the way round, do not get stuck",
            .Blocks = { Core, Move, Goal },
            .Arenas = {
                { .Name = "broken", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 150,
                    .Course = MarkerCourse::Ground },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorBroken(),
            .HeldOutSpawnPoints = KalimdorBrokenControl(),
        });

        // M3 -- verticality: up and down. Markers above the seat from a cliff foot (the way up a ramp, a stair or a
        // jump), below it from a ledge top (the drop is the shortcut, the way round the safe one, at least LedgeDetour
        // times the line with no drop past SafeDrop), or on another floor of an inn. The height window climbs from a
        // step or two to 15-45 yd (MarkerVertical.*), so the drops come to cost health and then to kill. Movement
        // only (user, 2026-10-05): no interactions, no closed doors -- a marker the player controller cannot walk to
        // is never placed, and a closed door is a wall to it.
        stages.push_back({
            .Name = "move3_vertical",
            .Suffix = "_vertical",
            .Extends = "move2_ground",
            .Summary = "markers above, below and on other floors: steps, jumps, safe drops and the long way round",
            .Blocks = { Core, Move, Goal },
            .Arenas = {
                { .Name = "climb", .Weight = 2, .Against = Opposition::Markers, .EpisodeSeconds = 150,
                    .Course = MarkerCourse::Vertical, .SpawnPoints = CliffFeet(),
                    .HeldOutSpawnPoints = CliffFeetControl() },
                { .Name = "ledges", .Weight = 2, .Against = Opposition::Markers, .EpisodeSeconds = 150,
                    .Course = MarkerCourse::Vertical, .SpawnPoints = LedgeTops(),
                    .HeldOutSpawnPoints = LedgeTopsControl(), .Ledges = true },
                { .Name = "rooms", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 90,
                    .Course = MarkerCourse::Vertical, .Indoors = true, .SpawnPoints = Inns(),
                    .HeldOutSpawnPoints = InnsControl(), .SpawnScatter = 4.0f },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorBroken(),
            .HeldOutSpawnPoints = KalimdorBrokenControl(),
        });

        // M4 -- water: getting in, swimming, the surface and the swim jump, diving to the bed, breath, getting out
        // onto banks (a bank taller than a step takes the swim jump), and choosing between swimming and going round.
        // A crossing at the Barrens oases (the dry way round always the longer, both ways ones the controller makes),
        // lakebeds in Stonebull Lake down to forty yards as the ladder climbs, and a chain of four to six lakebeds
        // longer than a breath. Drowning is a cost at full price; a drowned seat dies (Markers.Death).
        //
        // Kalimdor only until the dry check: the plan's Loch Modan, coasts, Stormwind's canals and Zoram Strand need
        // points stood on, and its held-out Lake Everstill likewise; Lake Elune'ara is the held-out lake here.
        stages.push_back({
            .Name = "move4_water",
            .Suffix = "_water",
            .Extends = "move3_vertical",
            .Summary = "markers across water, on lakebeds and in chains longer than a breath: swim, dive, climb out",
            .Blocks = { Core, Move, Goal },
            .Arenas = {
                { .Name = "crossing", .Weight = 2, .Against = Opposition::Markers, .EpisodeSeconds = 150,
                    .Course = MarkerCourse::Water, .Water = true, .SpawnPoints = OasisShore(),
                    .HeldOutSpawnPoints = OasisControl() },
                { .Name = "lakebed", .Weight = 2, .Against = Opposition::Markers, .EpisodeSeconds = 150,
                    .Course = MarkerCourse::Water, .SpawnPoints = StonebullShore(),
                    .HeldOutSpawnPoints = EluneAraShore(), .Underwater = true },
                { .Name = "chain", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 240,
                    .Course = MarkerCourse::Water, .SpawnPoints = StonebullShore(),
                    .HeldOutSpawnPoints = EluneAraShore(), .Underwater = true, .Checkpoints = true },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorBroken(),
            .HeldOutSpawnPoints = KalimdorBrokenControl(),
        });

        // M5 -- long routes: one trip an episode, 150 to 600 yards across mixed ground, where the way is not visible
        // from the start -- round lakes, through canyons and over ridges, out of a pocket the straight line walks
        // into. The detour climbs from 1.3 to 3.0 times the straight line; the route planner plans the whole way
        // (a training signal only), and the controller has to be able to walk or swim every yard of it. This is
        // where the perception plan's local map and route tiers (.agents/plans/local-map-perception/) earn their
        // keep; until they land the stage runs on today's ground probe and the marker's bearing, and is expected to
        // plateau lower.
        //
        // Kalimdor's validated ground until the dry check: the plan's canyons and valleys (Thousand Needles,
        // Desolace, Badlands), cities on foot and Stranglethorn need points stood on; Feralas, its held-out ground,
        // likewise. Human trips (AnimusForge.Human.Trips) join once capture data exists.
        stages.push_back({
            .Name = "move5_routes",
            .Suffix = "_routes",
            .Extends = "move4_water",
            .Summary = "one long trip, 150 to 600 yd, the way not visible from the start: find it and arrive",
            .Blocks = { Core, Move, Goal },
            .Arenas = {
                { .Name = "canyons", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 420,
                    .Course = MarkerCourse::Routes, .SpawnPoints = KalimdorBroken(),
                    .HeldOutSpawnPoints = KalimdorBrokenControl() },
                { .Name = "open", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 420,
                    .Course = MarkerCourse::Routes, .SpawnPoints = KalimdorFlats(),
                    .HeldOutSpawnPoints = KalimdorFlatsControl() },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorBroken(),
            .HeldOutSpawnPoints = KalimdorBrokenControl(),
        });

        // M6 -- riding and flight: mounting when it pays, steering at mount speed, dismounting where it must, and
        // flying -- take off, climb over what is in the way, cruise, land on the marker. A ride across Kalimdor's
        // flats (150-500 yd, the ground mount from level 20), flights in Outland (200-900 yd, a flying mount from
        // 60, under open sky), and air-only markers the ground route does not reach (a ground mount there is only
        // useless, never masked: ground_mount_on_air_leg). Mounting is
        // a cast the seat must stand still for, and a hit interrupts it (ruling f: the game's own rules). The travel
        // block joins the layout here, with the mounts; arriving is landing and stopping on the marker.
        //
        // Outland's held-out ground is Terokkar (the plan's); Northrend's flight-only plateaus need points stood on.
        stages.push_back({
            .Name = "move6_mounted",
            .Suffix = "_mounted",
            .Extends = "move5_routes",
            .Summary = "trips worth mounting for: ride, fly over what is in the way, land on the marker",
            .Blocks = { Core, Move, Travel, Goal },
            .Arenas = {
                { .Name = "ride", .Weight = 2, .Against = Opposition::Markers, .EpisodeSeconds = 240,
                    .Course = MarkerCourse::Mounted, .SpawnPoints = KalimdorFlats(), .MinLevel = 20,
                    .HeldOutSpawnPoints = KalimdorFlatsControl() },
                { .Name = "flight", .Weight = 2, .Against = Opposition::Markers, .EpisodeSeconds = 240,
                    .Course = MarkerCourse::Mounted, .Flying = true, .SpawnPoints = OutlandGround(),
                    .MapId = MAP_OUTLAND, .MinLevel = 60, .HeldOutSpawnPoints = OutlandControl() },
                { .Name = "flight_air", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 240,
                    .Course = MarkerCourse::Mounted, .Flying = true, .AirOnly = true, .SpawnPoints = OutlandGround(),
                    .MapId = MAP_OUTLAND, .MinLevel = 60, .HeldOutSpawnPoints = OutlandControl() },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorFlats(),
            .HeldOutSpawnPoints = KalimdorFlatsControl(),
        });

        // M7 -- company: moving with someone. Keep within three to ten yards of a leader for the whole episode,
        // catching up after falling behind, not crowding it, going where it went, and keeping clear of hostile
        // camps on the way -- no combat yet. The leader is an agent in the owner's slot moved by the player
        // controller and reported as a client, never a spline: on the early rungs the seek helper's keys walk it
        // over trips of the ground (at a walk, then running, the trips lengthening), and from Follow.CastFromRung
        // half the training episodes give it to a frozen M6 checkpoint (cast.agents.leader) that rides, swims and
        // jumps as it likes. Recorded human trips replace both once capture data exists (plan §8.4).
        //
        // Movement only, in the forge: the realm's companions are parked (user, 2026-10-05).
        stages.push_back({
            .Name = "move7_follow",
            .Suffix = "_follow",
            .Extends = "move6_mounted",
            .Summary = "keep within 3-10 yd of a moving leader: fall behind, catch up, go where it went",
            .Blocks = { Core, Move, Travel, Goal },
            .Arenas = {
                { .Name = "open", .Weight = 1, .Against = Opposition::Follow, .EpisodeSeconds = 180,
                    .SpawnPoints = KalimdorFlats(), .HeldOutSpawnPoints = KalimdorFlatsControl() },
                { .Name = "broken", .Weight = 1, .Against = Opposition::Follow, .EpisodeSeconds = 180,
                    .SpawnPoints = KalimdorBroken(), .HeldOutSpawnPoints = KalimdorBrokenControl() },
            },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = KalimdorFlats(),
            .HeldOutSpawnPoints = KalimdorFlatsControl(),
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

        // A stage with nothing to fight carries no duel block (the movement stages); one that fights needs it.
        bool const fights = stage.AnyArena([](ArenaDefinition const& arena)
        {
            return arena.Against != Opposition::Markers && arena.Against != Opposition::Follow;
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
