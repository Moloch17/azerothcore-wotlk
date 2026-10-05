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
 *   movement   move1_controls ─ ... ─ move7_follow       (M2-M7 land one at a time)
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

    /// Open, flat ground on Kalimdor for the controls stage (M1): the Barrens' scrub and Mulgore's grass, from the first
    /// curriculum's training ground (curriculum-v1 KalimdorGround), every point stood on with `forge rays` then. The
    /// Durotar and Dustwallow points of that list are left out: they are canyon, rock and broken shore, M2's ground.
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

        // The movement stages' markers: one seat on its own with nothing else in the episode, on the stage's ground.
        // The travel arena's kinds of ground (water, rooms, ledges, lakebeds, the air) are not markers' yet: each
        // comes with the stage that asks for it.
        bool const markers = arena.Against == Opposition::Markers;
        if (markers && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0
            || arena.Schedule != PullSchedule::None || arena.Directed))
            return "a marker arena is one seat on its own, with nothing to fight and no one to follow";
        if (markers && (arena.OnFoot || arena.Flying || arena.AirOnly || arena.Water || arena.Indoors || arena.Ledges
            || arena.Underwater || arena.Checkpoints))
            return "a marker arena takes none of the travel arena's kinds of ground yet";
        if (markers && !stage.Has(BlockId::Move))
            return "markers are walked to with the move block";

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
            return arena.Against != Opposition::Markers;
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
