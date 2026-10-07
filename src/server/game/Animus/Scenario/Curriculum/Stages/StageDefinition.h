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

#ifndef ANIMUS_LIB_CURRICULUM_STAGE_DEFINITION_H
#define ANIMUS_LIB_CURRICULUM_STAGE_DEFINITION_H

#include "Block.h"
#include "Aptitude.h"
#include "ClassProfile.h"
#include "SeenPlaces.h"
#include "Position.h"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Animus::Curriculum
{
    /// Who the learned agents of an env are.
    enum class SeatPlan : uint8
    {
        Solo,           // one seat
        Party,          // one group (1-GROUP_SEATS with a character each episode): a tank, a healer and damage
        Raid,           // the arena's seats as RAID_GROUPS groups of GROUP_SEATS: a tank and a healer per group
    };

    /// What the seats fight.
    enum class Opposition : uint8
    {
        Instance,       // a real dungeon or raid boss in its own instance (InstanceEncounter, ArenaDefinition::Instance)
        /// Nothing to fight: one real object in one of a dungeon's rooms, found by sight and stopped beside
        /// (SeekEncounter, ArenaDefinition::Rooms and Objects) -- M2 seek.
        Seek,
        /// Nothing to fight: one real object along a dungeon's hallways, in sight of the spawn (or just round a
        /// corner), stopped beside, with a compass the ladder withholds more and more often (SightEncounter,
        /// ArenaDefinition::Objects and SightPairs) -- M1 controls, redesigned.
        Sight,
        /// Nothing to fight: the object the goal names, among decoys, behind a door its lever opens, or the lock its
        /// key item opens, in an empty dungeon (InteractEncounter, ArenaDefinition::Sites and Objects) -- M3
        /// interact.
        Interact,
        /// Nothing to fight: a party keeps with a leader walking a dungeon's route from the door, in an emptied
        /// instance (PartyFollowEncounter) -- M4 follow. The leader is in the owner's slot, moved by the player
        /// controller; a seat that dies rises at the instance's entrance and walks back (EntranceRespawn).
        PartyFollow,
        /// Creatures on a cleared dungeon's own ground, fought by a seat that sees them (CombatEncounter, the drill in
        /// ArenaDefinition::Combat) -- the combat stages C1-C3 of the dungeon curriculum.
        Combat,
        /// A party of five drilling one role on a cleared dungeon's own ground (RolesEncounter, the drill in
        /// ArenaDefinition::Roles) -- G1 of the dungeon curriculum.
        Roles,
    };

    /// What a combat arena's creatures are (Opposition::Combat, CombatEncounter): the dungeon curriculum's C1-C3.
    enum class CombatDrill : uint8
    {
        None,
        Fight,          // C1: one creature at a time, the next after each kill
        Packs,          // C2: packs of 2-4 (casters, linked, fire underfoot), the next pack standing further on
        Survive,        // C3: packs that can kill, pull after pull; rest between them, and come back after a death
    };

    /// What a roles arena drills (Opposition::Roles, RolesEncounter): G1's four, one role's lesson an episode, the
    /// drilled role in seat 0 (ArenaDefinition::DrillRole).
    enum class RolesDrill : uint8
    {
        None,
        Hold,           // tank_hold: the tank holds every enemy, pack after pack
        Keep,           // heal_keep: the healer keeps everyone up through fights longer than its mana bar
        Focus,          // damage_discipline: a damage dealer kills the tank's target without taking an enemy off it
        Pull,           // pull: the tank pulls one pack of a camp at a time
    };

    /// **A room of the seek stage** (ArenaDefinition::Rooms): a floor area the object may be put on, written once in
    /// the stage definition from the map. Floor is a convex polygon (x, y corners in order), kept a yard and a half in
    /// from the walls; FloorZ the floor's height, Opening the middle of its doorway onto the room or hallway it is
    /// entered from, Centre a point inside on the floor, and Walk the walking distance from the stage's spawn to it
    /// (yards): its depth, which the room ladder orders the rooms by.
    struct SeekRoom
    {
        std::string Name;
        float FloorZ = 0.0f;
        std::pair<float, float> Opening{};
        std::pair<float, float> Centre{};
        float Walk = 0.0f;
        std::vector<std::pair<float, float>> Floor{};
        /// A front cell: its opening is onto the hallway and it is no hub -- the seek ladder's doorway and room rungs
        /// draw these, its deep rung the rest (SeekDraw::RungRooms).
        bool Front = false;
    };

    /// **An object the seek stage hides** (ArenaDefinition::Objects): a gameobject_template entry whose display has a
    /// collision model (vmaps' GameObjectModels.dtree), so the camera's rays meet it; Kind names it in the episode
    /// info, and Height is the model's height times the template's size, yards: the objective point is its centre.
    /// Radius is its bounding radius (the model box's largest half-extent times the size): the camera flags it within
    /// Vision::ObjectiveRadiusFor(Radius) of its centre, not the default yard.
    struct SeekObject
    {
        uint32 Entry = 0;
        std::string Kind;
        float Height = 1.0f;
        float Radius = 0.5f;
    };

    /// **One of the sight stage's evaluation episodes** (ArenaDefinition::SightPairs): the seat spawns at
    /// SpawnPoints[Spawn] and the object stands at SpawnPoints[Object] (the arena's hallway points are both its spawns
    /// and the object's places). Corner: the object is not in sight of the spawn, only of a point a few yards' walk
    /// from it.
    struct SightPair
    {
        uint16 Spawn = 0;
        uint16 Object = 0;
        bool Corner = false;
    };

    /// **A site of the interact stage** (ArenaDefinition::Sites, M3): one of the map's own doors and what opens it --
    /// a lever beside it (a button whose use the map's script links to the door), or a lock that takes a key item
    /// (the Deadmines' cannon and its gunpowder, which blows the Iron Clad Door) -- and the floor either side of it:
    /// Near, the opener's side, where the seat stands and the distinguish rung's objects go; Far, behind the shut
    /// door, where the switch rung's object goes. Written once from the map (an authoring scan of the navmesh and the
    /// vmaps, offline) and checked against the map's data by a GTest (DeadminesSitesDataTest).
    struct InteractSite
    {
        std::string Name;
        uint32 Door = 0;                    // the door's gameobject entry (one of the map's own spawns)
        uint32 Opener = 0;                  // what opens it: a lever's (button's) or the lock's (goober's) entry
        uint32 Key = 0;                     // the item the opener's lock takes; 0 for a lever
        std::vector<Position> Near{};
        std::vector<Position> Far{};
    };

    /// Which real-instance ladder an arena climbs (InstanceBosses.cpp): five-man dungeons across the level bands, or
    /// the ten-, twenty-five- and forty-man raids.
    enum class InstanceLadder : uint8
    {
        None,
        Dungeon,
        Raid10,
        Raid25,
        Raid40,
        /// Whole dungeon wings (next-run plan 5.3): from the wing's door to its last boss, the trash alive.
        Wing,
    };

    /// Most arenas a stage can mix (the critic state has one column per arena).
    constexpr uint32 MAX_ARENAS = 16;

    /// One situation an episode of a stage can be: who the seats are, what they fight, and how long it lasts. Every
    /// episode of a stage draws one of its arenas by weight, so one stage (and one policy) can train PvE and PvP
    /// together. A stage with a single arena is a stage of one situation.
    struct ArenaDefinition
    {
        std::string Name;               // unique in the stage: episode info, stage.json, tuning keys
        uint32 Weight = 1;              // share of episodes; <TuningPrefix>Arena.<stage>.<name>.Weight
        /// The share at the end of the stage's budget, reached linearly from Weight as training goes (the
        /// learner's PROGRESS): an arena weighted up as the skills it needs come in -- whole dungeon wings in the
        /// party stage. -1 keeps Weight throughout. Evaluation draws by the final weights.
        /// <TuningPrefix>Arena.<stage>.<name>.WeightFinal
        int32 WeightFinal = -1;
        SeatPlan Seats = SeatPlan::Solo;
        Opposition Against = Opposition::Instance;
        bool PartyGroup = false;        // the seats form a core group (PartyEncounter)
        /// Opposition::Instance: the boss ladder this arena climbs. The rung fixes the map, the seats' level and
        /// the difficulty; the stage's MapId and SpawnPoints are not used by this arena.
        InstanceLadder Instance = InstanceLadder::None;
        /// The ladder's row this arena always runs (Ragefire Chasm, the Deadmines: a stage each); -1 = the class's own
        /// rung on the ladder.
        int8 InstanceRow = -1;
        /// InstanceLadder::Wing: one pull a run instead of the whole dungeon -- the party a little way back along the
        /// route from one pack, the packs before it cleared, the run over when that pack is dead or a second one
        /// joins (Instance.Pull*). Training only, unless the stage is all drills (EvaluatesDrills: dungeon1_pulls,
        /// whose evaluation drills the pack its seed names).
        bool PullDrill = false;
        /// Played only when an evaluation pins it (the learner's eval.heldout, MODE's arena): never drawn in training
        /// nor in an ordinary evaluation, whatever its weight. Content a stage is measured on and never trained on --
        /// a dungeon it has not seen -- so a policy that memorised its own route is told from one that learned to run
        /// dungeons (peak-play W2).
        bool EvalOnly = false;
        /// SeatPlan::Raid: how many seats the raid has (a multiple of GROUP_SEATS up to MAX_SEATS); 0 = MAX_SEATS.
        uint32 RaidSeats = 0;
        /// SeatPlan::Party: how many learned seats the party has, every episode (1 to GROUP_SEATS); 0 = the party's
        /// own draw (Party.SizeWeight*, or a whole group). The party follow's four followers beside its leader.
        uint32 PartySize = 0;
        uint32 EpisodeSeconds = 0;      // episode length; 0 = StageSettings::EpisodeSeconds
        /// A full party of five drawn as a dungeon's is (StageScenario::FitsDungeonRole): a tank, a healer and three
        /// damage dealers by what their specs are geared for.
        bool ProperParty = false;
        /// The role this arena drills (DungeonRole: 1 tank, 2 healer, 3 damage; 0 none): that seat is seat 0 -- the
        /// one whose class and build climbs the pack ladder -- and its role's terms are weighted Raid.DrillWeight.
        uint8 DrillRole = 0;
        /// Every pull contains a creature that puts something on the ground (OpponentPool::RandomHazardCaster),
        /// whatever rung the ladder is on. The pack ladder only reaches hazards at rung 3, so a class/role that
        /// stalls below it never meets one; this makes stepping out of a hazard learnable on its own.
        bool Hazards = false;
        /// **Commanded goals** (next-run plan, 3.4): the sim gives the seat its primary goal -- a random one of those
        /// the goal block offers, every COMMAND_EVERY decisions or when it ends -- as a director's order is given, so
        /// the learner holds it without its goal head being trained on it. Paid only by the goal's own terms beside
        /// the stage's: the fast loop learns to follow a goal before the slow loop learns to choose one.
        bool CommandedGoals = false;
        /// **Death runs on** (next-run plan, Wave 6): a seat that dies is not stood up and does not end the episode.
        /// It releases, runs its ghost back to the corpse and rises there, takes the spirit healer's resurrection, or
        /// waits for a friend's (DeathBlock, which the stage must carry). Open-world arenas only: a release inside an
        /// instance would take the ghost to another map.
        bool DeathRuns = false;
        /// Where this arena's envs start, when its ground is not the stage's: used in place of the stage's when the
        /// episode is this arena's; empty means the stage's.
        std::vector<Position> SpawnPoints{};
        /// The map this arena's episodes are on, when it is not the stage's (0 = the stage's). A stage can then mix
        /// ground on several maps, which is what lets one stage replay a whole phase. An arena on a map of its own
        /// stands on its own SpawnPoints.
        uint32 MapId = 0;
        /// The lowest level this arena's characters may be, over the stage's MinLevel.
        uint8 MinLevel = 0;
        /// Ground kept back for evaluation: training never stands here. Empty means the arena has no control of
        /// its own, and evaluation runs on the same ground training does -- which measures nothing about whether
        /// the policy learned to read terrain or merely learned these particular banks.
        std::vector<Position> HeldOutSpawnPoints{};
        /// Opposition::Seek: the rooms an object may be hidden in (one drawn an episode, by the room ladder), the
        /// objects (one drawn an episode, uniformly), and how near the object a stop finds it, yards (interaction
        /// range).
        std::vector<SeekRoom> Rooms{};
        std::vector<SeekObject> Objects{};
        float SeekRadius = 3.0f;
        /// Opposition::Sight: the evaluation's fixed (spawn, object) pairs, indexes into SpawnPoints, each played with
        /// and without the compass (SightDraw::EvaluationPick). Objects is the pool the object is drawn from.
        std::vector<SightPair> SightPairs{};
        /// Opposition::Interact: the map's doors with what opens them and the floor either side (InteractSite).
        /// Objects is the pool the named object and its decoys are drawn from, SeekRadius how near reaching one is.
        std::vector<InteractSite> Sites{};
        /// Opposition::Combat: what the creatures are (CombatDrill), and whether a friendly fighter stands with the seat
        /// for each creature to go for first -- the drill's taunt and its heals on someone else (C1's `guard`).
        CombatDrill Combat = CombatDrill::None;
        bool Ally = false;
        /// Opposition::Roles: the role drilled (RolesDrill; its DungeonRole in DrillRole: seat 0's).
        RolesDrill Roles = RolesDrill::None;
        /// **A death brings the seat back alive at the instance's entrance** (dungeon-curriculum I4; the user,
        /// 2026-10-06: no graveyard, ghost or corpse run): out for Respawn.DelayMs, then alive with full health and
        /// power at the entrance (EntranceRespawn's RespawnClock and RiseAtEntrance), to walk back on the controller.
        /// The episode goes on (StageScenario::DeadForGood is never true). An instanced arena's, never with DeathRuns.
        bool RespawnAtEntrance = false;
        /// InstanceLadder::Wing: **a corridor** (dungeon-curriculum G2): a run is this many of the route's packs in
        /// route order -- the packs before the first cleared as a party that came from the door left them, the
        /// party set down short of the first, the run won when every one of them is cleared. Pull, fight, rest, ready,
        /// next. The first pack is drawn each training run and taken from the seed in an evaluation. 0: the whole
        /// dungeon.
        uint32 CorridorPacks = 0;
        /// A party arena's share of training episodes with the "human" stand-in in one seat (I7), percent; -1 keeps
        /// StandIn.Share. Overridden by `<TuningPrefix>Arena.<stage>.<arena>.StandInShare`. Evaluations play it only
        /// in the learner's with_human arm, as before.
        int32 StandInShare = -1;
        /// InstanceLadder::Wing: the levels its runs are drawn in before the ladder's lift (the bar's band: the
        /// Deadmines at 17-20), in place of the dungeon finder's range. 0: the dungeon finder's.
        uint8 LevelFirst = 0;
        uint8 LevelLast = 0;

        [[nodiscard]] uint32 SeatCount() const;
    };

    /// Whether a stage's evaluations play its pull drills (ArenaDefinition::PullDrill): a stage whose every trained
    /// arena is a drill (dungeon1_pulls) is measured on its drills, where a stage that also runs the whole dungeon is
    /// measured on that and keeps its drills for training.
    [[nodiscard]] bool EvaluatesDrills(std::vector<ArenaDefinition> const& arenas);

    /// **The arena draw's weights** (StageScenario::DrawArena): each arena's share, linear from its weight to its final
    /// weight over the budget (`progress`; an evaluation draws by the final ones). A held-out arena
    /// (ArenaDefinition::EvalOnly: Wailing Caverns) is never drawn this way, in training or evaluation -- only an
    /// evaluation pinned to it plays it -- and a pull drill only in a stage that EvaluatesDrills.
    [[nodiscard]] std::vector<uint32> ArenaDrawWeights(std::vector<ArenaDefinition> const& arenas,
        std::vector<uint32> const& weights, std::vector<uint32> const& finals, bool evaluating, float progress);

    /// One curriculum stage: its own scenario (`stage1_duel`, ...), its blocks and the arenas its episodes are.
    ///
    /// A stage extends one earlier stage, whose best model seeds it: the base's blocks this stage keeps are seeded
    /// block by block (their features and actions may move), dropped ones are left behind and new ones start fresh.
    /// Several stages may extend the same base, so the curriculum is a tree. A merge stage also lists other earlier
    /// stages (Merges): the blocks only they have are seeded from them, and the learner can distill each of their
    /// arenas from their models, joining branches of the tree again.
    struct StageDefinition
    {
        std::string Name;               // the scenario name
        std::string Suffix;             // added to a class/role's name for the stage's models (warrior_dps_duel)
        std::string Extends;            // the stage it builds on and seeds from (the trunk); empty for the first
        std::vector<std::string> Merges{}; // further stages it seeds the blocks only they have from
        std::string Summary;
        /// Played only by the classes whose own kit can make them stealthed (StageScenario's CanStealth, asked of
        /// ClassKit so the answer is true of every member of the class rather than of one race of it).
        ///
        /// A restricted stage's checkpoint holds only the layouts it played, so seeding from it can leave the rest
        /// of a run starting from random weights. That used to be prevented here, by refusing to let anything
        /// extend or merge a restricted stage at all -- which also made the rule wrong in the case it matters
        /// most: in a run of one class that can stealth, every layout plays the stage and there is nothing
        /// partial about the checkpoint. The rule now lives where the actual layouts are known
        /// (animus.bootstrap), which refuses loudly rather than fresh-initialising in silence, so a stage like
        /// this can sit in the middle of a chain when the run it is in allows it.
        bool NeedsStealth = false;
        std::vector<BlockId> Blocks;    // in layout order: every block any of its arenas needs
        std::vector<ArenaDefinition> Arenas;
        bool InDefaultQueue = true;     // trained by an empty AnimusForge.Queue (false: only when named)
        /// Where its envs are: 0 = the host's StageSettings::SpawnMapId and SpawnPosition. A continent (not
        /// instanceable) is shared by every env, so each env gets its own phase; one of SpawnPoints is drawn for
        /// each episode, so a seat sees all of this ground rather than the one patch its env index picked out.
        uint32 MapId = 0;
        std::vector<Position> SpawnPoints{};
        /// The control ground: where evaluation episodes stand, and where training never does.
        ///
        /// A seeded evaluation on the ground training uses cannot tell a policy that reads terrain from one that
        /// has learned these particular places -- it randomises the episode, not the world. Scoring the gates
        /// here instead, on ground no weight has ever been updated against, is what makes `arrived` and `saved`
        /// claims about the policy rather than about the map.
        std::vector<Position> HeldOutSpawnPoints{};
        /// The lowest level its characters may be (flying needs 60), raising a host's fixed level too.
        uint8 MinLevel = 0;
        /// A band most of a stage's training characters are drawn in (FocusChance percent of them; the rest at any
        /// level as usual): the class stages train at the dungeons' levels first, where a kit is small and every
        /// spell in it matters. 0 = none. Evaluation spreads over every level as before.
        uint8 FocusLevelFirst = 0;
        uint8 FocusLevelLast = 0;
        uint8 FocusChance = 0;
        /// Every character of the stage at this level, training and evaluation alike, raised to its class's own
        /// minimum (a death knight's 55). 0 = drawn as usual. The user's M1 (2026-10-05): level 1, so a class's kit is
        /// one or two spells and the lesson is the movement alone.
        uint8 Level = 0;
        /// Where a sight stage's goal places come from in a dungeon (SeenPlaces): what the seat saw and its map's
        /// frontier alone (the default: the user, 2026-10-07, "only let the bots know what they've discovered"), or
        /// with the dungeon map's layout nodes too. Never a live pack's or boss's position. Overridden by
        /// `<TuningPrefix>Stage.<name>.GoalPlaces` (0 seen and layout, 1 seen only).
        SeenPlaces::Source GoalPlaces = SeenPlaces::Source::SeenOnly;

        [[nodiscard]] bool Has(BlockId block) const;
        /// Seats per env: the largest arena's.
        [[nodiscard]] uint32 SeatCount() const;
        /// Whether any arena of the stage satisfies `predicate` (encounters, info columns and pools it needs).
        template <typename Predicate>
        [[nodiscard]] bool AnyArena(Predicate predicate) const
        {
            for (ArenaDefinition const& arena : Arenas)
                if (predicate(arena))
                    return true;
            return false;
        }
    };

    /// Every curriculum stage, every base before the stages that extend it. Invalid definitions (an unknown or later
    /// base, a repeated block, parts that need a missing block) are logged and left out.
    [[nodiscard]] std::vector<StageDefinition> const& CurriculumStages();

    [[nodiscard]] StageDefinition const* FindStage(std::string_view name);

    /// Why each stage CurriculumStages left out was left out ("<name>: <problem>"); empty when every definition is
    /// valid. The forge refuses to start training while it is not empty: a stage left out with only a log line is how
    /// two open-world stages once went missing from a queue unnoticed.
    [[nodiscard]] std::vector<std::string> const& CurriculumProblems();
}

#endif
