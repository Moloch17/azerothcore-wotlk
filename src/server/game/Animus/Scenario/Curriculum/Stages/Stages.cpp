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

    /// The end of the entrance hallway, on its centre line, two yards short of the end wall: about 116 yards from the
    /// entrance, some sixteen seconds at run speed.
    Position StockadeHallwayEnd()
    {
        return { 170.0f, 1.0f, -25.61f, 0.0f };
    }

    /// Every stage, every base before the stages that extend it.
    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        // M1 -- controls (the user's design, 2026-10-05): stopping exactly on a marker. An empty Stockades, the seat at
        // the entrance, and one objective that never changes, at the end of the entrance hallway: reach it as fast
        // as possible and stop on it, within a yard. The straight line is the way, so a stop, a detour or a turn
        // there and back is time lost: the time cost (Markers.StepCost each decision until the stop) is the price of
        // all of them, and Arrive is paid once, on the stop.
        //
        // Every class and race, at level 1 (a death knight at its 55): a level 1 kit is one or two spells, so the
        // lesson is the movement alone, and every class's layout learns it from the start.
        //
        // Core and the goal block stay as the layout's frame (the character and its kit, the goal head the learner
        // sizes from the goal block); Move is the whole lesson, and the compass (perception-goals P1: the objective's
        // bearing and distance, split from the move block) says where the mark is. Nothing to fight, so no duel block. One place and one
        // objective: there is no held-out ground, and the evaluation is the training task itself.
        stages.push_back({
            .Name = "move1_controls",
            .Suffix = "_controls",
            .Extends = "",
            .Summary = "an empty Stockades: from the entrance to the end of the hallway as fast as possible, and stop "
                "within a yard of the mark",
            .Blocks = { Core, Move, Compass, Vision, Goal },
            .Arenas = {
                { .Name = "hallway", .Weight = 1, .Against = Opposition::Markers, .EpisodeSeconds = 60,
                    .SpawnPoints = { StockadeEntrance() }, .MapId = MAP_STORMWIND_STOCKADE,
                    .Objective = StockadeHallwayEnd(), .ObjectiveRadius = 1.0f },
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
