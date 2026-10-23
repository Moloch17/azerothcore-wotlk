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
 *   nav        nav1_sight ─ nav2_detour ─ nav3_vertical ─ nav4_beyond ─ nav5_water ─ nav6_follow (the vision movement
 *              curriculum, decision 0036: a point goal over a traversability bake, the route learned on a costmap and
 *              a planner; a second root, seeded from nothing before it; dungeons only in this build, Wailing Caverns
 *              held out of every stage)
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
#include "SeekTables.h"
#include "Log.h"
#include "AreaDefines.h"
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;

    // The highest role a party drill can fix (ArenaDefinition::DrillRole: 1 tank, 2 healer, 3 damage).
    constexpr uint8 DRILL_DAMAGE = 3;

    /// **The Deadmines' run** (dm1, 2026-10-22; the curriculum before it is the tag curriculum-nav-v1): the empty
    /// dungeon from its door to its exit trigger (areatrigger 121, "Deadmines Instance End", radius 7), its own doors
    /// live: the Factory, Mast Room and Foundry doors open as their dead bosses would have left them, the Iron Clad
    /// Door shut and opened the player's way (the Defias Gunpowder in the bags used on the Defias Cannon, whose script
    /// 1639800 blows the door), the Heavy Doors as the world has them. The bake is the nav stages' (36-deadmines.trav,
    /// arena `deadmines`, no box); the start is drawn back along the route from the exit by rung (PairKind 6).
    ArenaDefinition DeadminesRun()
    {
        ArenaDefinition arena{ .Name = "deadmines", .Weight = 1, .Against = Opposition::PointNav,
            .EpisodeSeconds = 600, .MapId = MAP_DEADMINES, .RespawnAtEntrance = true };
        arena.HasRunGoal = true;
        arena.RunGoal = { -110.76f, -1009.19f, 34.80f, 0.0f };
        arena.RunGoalRadius = 5.0f;
        arena.HasRunGate = true;
        arena.RunGate = { -100.5f, -668.8f, 7.4f, 0.0f };
        arena.RunGateSeconds = 60.0f;
        // The way a full clear walks (the user, 2026-10-23: the shortest way to the exit skipped the ship): every
        // site the dungeon's creatures spawn at, in order, then the exit.
        arena.RunClear = true;
        arena.LiveObjects = true;
        arena.LiveOpen = { 13965, 16400, 16399 };   // the Factory, Mast Room and Foundry doors
        arena.LiveShut = { 16397 };                 // the Iron Clad Door
        arena.LiveItems = { 5397 };                 // the Defias Gunpowder
        return arena;
    }

    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        // DM1 -- navigate the empty Deadmines (the user, 2026-10-22): from its door to its exit, opening the Iron Clad
        // Door with the cannon on the way. No camera (StageDefinition::Camera): the dungeon is memorised (the shared
        // memory surveyed from the bake), what is in it now is what is in line of sight in the view cone the look
        // head turns, and what was seen is remembered (the entity and object memories). Every race and class at
        // every level, as the nav stages had them. Combat, and dungeons with their creatures, come after.
        stages.push_back({
            .Name = "dm1_navigate",
            .Suffix = "_nav",
            .Extends = "",
            .Summary = "the empty Deadmines, door to exit: a memorised dungeon, line of sight and memory for what is "
                "in it, the Iron Clad Door opened with the gunpowder and the cannon",
            .Blocks = { Core, Move, Vision, Map, MemoryEntities, Sight, Goal },
            .Arenas = { DeadminesRun() },
            .MapId = MAP_DEADMINES,
            .SpawnPoints = { Position(-16.4f, -383.07f, 61.78f, 1.86f) },
            .EvenLevels = true,
            .MapTiers = 2,
            .SharedMemoryShare = 1.0f,
            .NoBackpedal = true,
            .NoSpells = true,
            .Camera = false,
            .PairKind = 6,
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
        // The party follow (M4): a party of learned followers and their group's leader (a player in the owner's slot,
        // walked by the encounter), in a dungeon of its own (its door is the spawn, its bosses' places the route),
        // nothing to fight; the leader is found by sight, the minimap and memory, so the camera and the party frames,
        // and never the compass. The encounter forms the group itself (arena.PartyGroup stays off).
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

        // The nav stages (decision 0036, PointNavEncounter): one seat on its own in a cleared instance, a point goal
        // over the dungeon's traversability bake, seen with the camera and planned on the mental map (two tiers:
        // StageDefinition::MapTiers), whose route is the goal's only observation (map block revision 7: no compass,
        // which would give the straight line away); its start drawn from the bake, its dead rising at the start.
        bool const pointNav = arena.Against == Opposition::PointNav;
        if (pointNav && (arena.Seats != SeatPlan::Solo || !arena.Rooms.empty() || !arena.Objects.empty()))
            return "a nav arena is one seat on its own, with no room or object of its own";
        if (pointNav && (!stage.Has(BlockId::Move) || !stage.Has(BlockId::Vision) || !stage.Has(BlockId::Map)))
            return "a nav arena is walked with the move block, seen with the camera and planned on the mental map";
        if (pointNav && stage.Has(BlockId::Compass))
            return "a nav arena's goal is the route on the mental map: the compass would give the straight line away";
        if (pointNav && (!arena.OwnMap() || !arena.RespawnAtEntrance))
            return "a nav arena is on a dungeon's own map (or a tower's) and its dead rise at the start";
        if (pointNav && stage.MapTiers != 2)
            return "a nav arena's planner reads both tiers of the mental map (MapTiers 2)";
        // A dungeon run (PairKind 6) has its fixed goal; live objects are a run's (its shut doors on the way), and
        // their keys and doors are pressed through the sight block.
        if (pointNav && stage.PairKind == 6 && (!arena.HasRunGoal || arena.RunGoalRadius <= 0.0f))
            return "a dungeon run (PairKind 6) names its goal and the goal's radius";
        if (arena.LiveObjects && (!pointNav || stage.PairKind != 6 || !stage.Has(BlockId::Sight)))
            return "live objects are a dungeon run's (PairKind 6), opened through the sight block";
        // A bake box is the bake's, and a labels-only arena's PointNav (nav6's labeller) bakes the dungeon too.
        if ((arena.SightOnly || arena.Revisit) && !pointNav)
            return "a sight-only draw and a revisit price are a nav arena's";
        if (arena.HasBox && !pointNav && !arena.LabelsOnly)
            return "a bake box is a nav arena's or a labels-only arena's (PointNav bakes both)";
        if (arena.HasBox && (arena.BoxMinX >= arena.BoxMaxX || arena.BoxMinY >= arena.BoxMaxY))
            return "a bake box is its min and max x and y, in order";
        // PointNav as a labeller beside the party follow (nav6): the leader's believed position is the point the
        // map block's route is planned to, so the two-tier map is needed as in a nav arena, and nothing else of
        // PointNav's.
        if (arena.LabelsOnly && (!partyFollow || !stage.Has(BlockId::Map) || stage.MapTiers != 2))
            return "labels-only PointNav rides on a party follow, with both tiers of the mental map";

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
        if (arena.RespawnAtEntrance && !arena.Tower && (arena.MapId ? arena.MapId : stage.MapId) == 0)
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
        bool const ownSpawn = instance || arena.Against == Opposition::PartyFollow
            || arena.Against == Opposition::PointNav;     // the nav stages: every reachable cell of the bake
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
        // The coverage block samples the mental map's tile counters (general search): it needs the map block.
        if (stage.Has(BlockId::Coverage) && !stage.Has(BlockId::Map))
            return "a coverage block samples the mental map's tiles: it needs the map block";
        // The map block's crop has one tier, or two for the nav stages (the 8-yd coarse tier, revision 5).
        if (stage.MapTiers < 1 || stage.MapTiers > 2 || (stage.MapTiers == 2 && !stage.Has(BlockId::Map)))
            return "the mental map's tiers are 1 or 2, and two tiers need the map block";
        // The remembered entities are written from the camera's entity list (Vision::ObjectMemory): the entities block
        // observes first.
        if (stage.Has(BlockId::MemoryEntities))
        {
            auto const entities = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::Entities);
            auto const memory = std::find(stage.Blocks.begin(), stage.Blocks.end(), BlockId::MemoryEntities);
            if (!stage.Has(BlockId::Vision) || entities == stage.Blocks.end() || memory < entities)
                return "the memory entities block reads what the camera's entity list wrote: it needs the vision block, "
                    "after it";
        }
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
                && arena.Against != Opposition::Interact && arena.Against != Opposition::PartyFollow
                && arena.Against != Opposition::PointNav;
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
    // Builds the table (and fills LeftOut) on first use; the table itself is not needed here.
    (void) CurriculumStages();
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
            if (arena.OwnMap())
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
