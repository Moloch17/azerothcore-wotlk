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
#include "Position.h"
#include <string>
#include <string_view>
#include <vector>

namespace Animus::Curriculum
{
    /// Who the learned agents of an env are.
    enum class SeatPlan : uint8
    {
        Solo,           // one seat
        Party,          // one group (1-GROUP_SEATS with a character each episode): a tank, a healer and damage
        Mirror,         // two seats that fight each other (self-play)
        Raid,           // the arena's seats as RAID_GROUPS groups of GROUP_SEATS: a tank and a healer per group
        Teams,          // TEAM_COUNT sides of TEAM_SEATS against each other (self-play), each side a group
    };

    /// What the seats fight.
    enum class Opposition : uint8
    {
        Creature,       // one same-level creature spawned out of aggro range
        Pulls,          // packs of creatures (see PullSchedule)
        ScriptedPlayer, // an enemy player played by a script
        MirrorSeat,     // the other seat (SeatPlan::Mirror)
        Ambush,         // only ambushers: scripted enemy players attacking the owner (ArenaDefinition::Ambushers)
        Travel,         // a place to get to (ArenaDefinition::Flying for one best reached in the air)
        Flag,           // Warsong Gulch's rules between the two mirror seats: take the other's flag home
        Hazards,        // nothing to fight: ground to get off (HazardEncounter)
        Instance,       // a real dungeon or raid boss in its own instance (InstanceEncounter, ArenaDefinition::Instance)
        Quest,          // a quest of the level band, giver to turn-in, in the world's own zone (QuestEncounter)
        Gather,         // a field of the band's herb and ore nodes, with the zone's creatures (GatherEncounter)
        Town,           // a town's traders: sell, repair, restock, dress (TownEncounter)
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
    };

    enum class PullSchedule : uint8
    {
        None,
        SinglePack,     // one pack; the episode ends when it is cleared
        Gauntlet,       // pull after pull with a break between, until the episode ends
        Sequence,       // a known run of pulls in a fixed order, the same every episode, won by clearing the last
    };

    /// Most arenas a stage can mix (the critic state has one column per arena).
    constexpr uint32 MAX_ARENAS = 12;

    /// Most ambushers an arena can have; they take enemy slots the pulls leave free.
    constexpr uint32 MAX_AMBUSHERS = 2;

    /// One situation an episode of a stage can be: who the seats are, what they fight, and how long it lasts. Every
    /// episode of a stage draws one of its arenas by weight, so one stage (and one policy) can train PvE and PvP
    /// together. A stage with a single arena is a stage of one situation.
    struct ArenaDefinition
    {
        std::string Name;               // unique in the stage: episode info, stage.json, tuning keys
        uint32 Weight = 1;              // share of episodes; <TuningPrefix>Arena.<stage>.<name>.Weight
        SeatPlan Seats = SeatPlan::Solo;
        Opposition Against = Opposition::Creature;
        PullSchedule Schedule = PullSchedule::None;
        bool Owner = false;             // an owner the seats fight for
        /// The owner is an agent of its own: one more row on the wire, after the seats (and the directors), which
        /// the learner plays from a frozen checkpoint (its cast, stage.json `cast`) and never trains. A share of
        /// the episodes (Owner.CastScriptedShare) keeps the scripted owner, which wanders and engages on a timer
        /// -- the shape the companion's follow lesson was built on -- and every evaluation does: the yardstick
        /// stays the owner it always was. Ignored unless Owner.
        bool OwnerCast = false;
        bool PartyGroup = false;        // the owner and seats form a core group
        bool Pvp = false;               // against players: resilience gear, no resurrecting oneself
        /// Opposition::Instance: the boss ladder this arena climbs. The rung fixes the map, the seats' level and
        /// the difficulty; the stage's MapId and SpawnPoints are not used by this arena.
        InstanceLadder Instance = InstanceLadder::None;
        /// SeatPlan::Raid: how many seats the raid has (a multiple of GROUP_SEATS up to MAX_SEATS); 0 = MAX_SEATS.
        uint32 RaidSeats = 0;
        uint32 EpisodeSeconds = 0;      // episode length; 0 = StageSettings::EpisodeSeconds
        /// Most scripted enemy players that ambush the owner (1 to this many, MAX_AMBUSHERS at most): mid-episode
        /// beside pulls, or from the start against Opposition::Ambush. 0 = none.
        uint32 Ambushers = 0;
        /// What the first seats must be able to do (entry i is seat i); the rest are drawn as usual. A drill stage
        /// fixes the seat it is about -- one that has to hold what it pulls, one that has to keep a group up --
        /// where the ordinary party asks for nothing in particular and the lesson is smeared over whoever
        /// happened to turn up.
        std::vector<AptitudeDemand> SeatAptitudes{};
        /// A director commands each side: one more agent a side, choosing the team's posture, the enemy it
        /// concentrates on, the shape it takes and whose turn the next duty is. Off by default -- a solo arena
        /// would pay for an agent with nothing to say.
        bool Directed = false;
        /// The director is an agent that learns rather than the scripted one (DirectorEncounter). Costs
        /// TEAM_COUNT more agents an env across the whole stage -- the spec is fixed, so a stage with one
        /// learned-directed arena carries the pair in every episode and marks them absent where they are not
        /// used. Ignored unless Directed.
        bool DirectorLearned = false;
        /// The director may name a place to go to (PlaceAnchor, PlaceOffset, PlaceRing). Off by default: the
        /// thirteen actions stay masked in an arena with nowhere worth sending anyone, so no stage pays
        /// exploration for a vocabulary it cannot use. Ignored unless Directed.
        bool Places = false;
        /// Seats a side in a Teams arena: 2 and 3 are the arena formats, 10 a battleground side. Ignored by
        /// every other seat plan.
        uint32 TeamSeats = TEAM_SEATS;
        /// Every pull contains a creature that puts something on the ground (OpponentPool::RandomHazardCaster),
        /// whatever rung the ladder is on. The pack ladder only reaches hazards at rung 3, so a class/role that
        /// stalls below it never meets one; this makes stepping out of a hazard learnable on its own.
        bool Hazards = false;
        /// Pin the pack ladder instead of letting it climb: -1 leaves it to Pulls.MaxTier, 0 and up hold every
        /// class/role at that rung for training and evaluation alike (DifficultyLadder::Draw takes it as the cap,
        /// and a cap of 0 leaves review and stretch draws nowhere to go).
        ///
        /// A drill wants one variable. With the ladder climbing, the thing being drilled and the difficulty of
        /// everything around it move together, and a metric that rises can mean either "it is not learning" or
        /// "there is more of it to meet" -- stage3_hazards spent 7M steps with its hazard seconds rising against a
        /// rising rung and neither reading could be ruled out. Overridden per arena by
        /// `<TuningPrefix>Arena.<stage>.<arena>.MaxRung`.
        int32 MaxRung = -1;
        /// Travel: the objective is far enough that flying beats riding (the stage's map must allow flight).
        bool Flying = false;
        /// Travel: no mount may be summoned, so the trip is made on the seat's own legs. What is left to learn
        /// is what a player does before it can ride: the speed cooldowns (Sprint, Dash, Travel Form, Aspect of
        /// the Cheetah), not stopping, and not wandering off the path. Mounting is masked, not merely unpaid,
        /// because a masked action cannot be explored into and the lesson stays clean.
        bool OnFoot = false;
        /// Travel: the objective may sit across water, and is chosen so that the way round is longer than the way
        /// through. On a creature arena instead (stage8_duel's `lake`): the opponent stands in the water, so the
        /// fight is a swimming one for whoever goes in after it.
        /// Travel: the objective may sit across water, and is chosen so that the way round is longer than the way
        /// through. Every other travel arena refuses an objective anywhere near water, which is why nothing in the
        /// curriculum had ever had to swim.
        ///
        /// Water is the one piece of ground that asks a question before it asks for a skill: swimming is about
        /// 4.7 yards a second against 7 running, so crossing pays only when the straight line saves more than
        /// about a third of the distance -- and what a build can do in water (a druid's Aquatic Form, a shaman's
        /// Water Walking) changes the answer.
        bool Water = false;
        /// Where this arena's envs start, when its ground is not the stage's. An arena is drawn per episode but
        /// the stage's list cannot give an arena that needs particular ground -- water, most of all -- what it
        /// needs. These are used in place of the stage's when the episode is this arena's; empty means the stage's.
        /// **The episode happens inside a building.** Everything that has to change about placing an objective
        /// and calling it reached, in one flag.
        ///
        /// Outdoors, an objective is found by probing sixty yards above the seat and searching a hundred and
        /// twenty down, because ground a long way up or down is still ground and the broken arena's ridges span
        /// seventy yards of relief. Inside a two-storey inn the same probe returns the roof. And arrival is
        /// two-dimensional, which is right on a slope and wrong under a staircase: a seat on the ground floor
        /// stands six yards from an objective on the floor above and has arrived at nothing.
        ///
        /// So an interior arena probes from the seat's own height, keeps the objective on a floor it could stand
        /// on, and adds a storey's worth of vertical tolerance to arriving. None of it touches an arena that
        /// leaves this false.
        bool Indoors = false;
        /// Travel, flying: the place can only be reached by air. FindPlace refuses a candidate the ground route
        /// reaches within Travel.AirDetour of the straight line, the ground mount is masked, and arriving means
        /// standing within Travel.AirArriveRise yards of the objective's own height. A spawn point with no such
        /// place in reach builds an ordinary flight instead and reports air_only 0, as a water arena that finds no
        /// crossing reports crossing 0: the shortfall is the ground's, and the gate can name it.
        ///
        /// Without this a flying arena never needs its wings. A flight objective is placed anywhere on dry
        /// ground the height probe finds, which in Nagrand is nearly always walkable, and 700 yards at run speed
        /// is 100 s of a 180 s clock: a ground ride arrives often enough that flying stays optional, and nine of
        /// ten class heads never found the flying mount. Only a Flying arena may set this.
        bool AirOnly = false;
        std::vector<Position> SpawnPoints{};
        /// Ground kept back for evaluation: training never stands here. Empty means the arena has no control of
        /// its own, and evaluation runs on the same ground training does -- which measures nothing about whether
        /// the policy learned to read terrain or merely learned these particular banks.
        std::vector<Position> HeldOutSpawnPoints{};
        /// Levels added to the scripted enemy player's own, on top of Opponent.LevelSpread. A drill about
        /// getting away needs a fight the seat cannot win; every other arena wants an even match and leaves
        /// this at 0. Ignored unless the opposition is a scripted player.
        int32 OpponentLevelBonus = 0;
        /// Yards of validated random offset applied to each seat's start, with a random facing to go with it.
        /// 0 leaves the seat exactly on the spawn point facing due east, which is what every arena did and what
        /// every arena that leaves this alone keeps doing.
        ///
        /// A spawn point is one pose, not one place. Drawing the objective at a uniform bearing varies the task
        /// but not the view the episode opens on, so a policy sees as many opening views as the stage has points
        /// -- seven in stage2_indoor's training, two in the evaluation that actually runs. "Read the walls from
        /// this spot" is a smaller thing to learn than "read the walls", and the gap between them is the whole
        /// claim an indoor drill makes.
        ///
        /// The offset goes through TravelEncounter::FindPlace, so it is on the mesh, reachable, and inside the
        /// building when the arena is Indoors. A room too tight to hold one keeps the spawn point and still
        /// takes the facing: the scatter is an improvement where it fits, never a reason to lose an episode.
        float SpawnScatter = 0.0f;
        /// Travel, on foot: the objective is below a ledge. FindPlace puts it Travel.LedgeDropMin to LedgeDropMax
        /// yards under the seat, where the ground route round is complete but at least Travel.LedgeDetour times
        /// the straight line and the straight line itself crosses one edge the seat can drop off, so the jump is
        /// the shortcut and the ramp is the safe way (TravelEncounter::LedgeOnLine). A spawn point with no such
        /// place in reach builds an ordinary trip and reports `ledge` 0, as a water arena reports `crossing` 0.
        /// Arriving means the objective's own floor (ARRIVE_SAME_FLOOR), or the lip above it would count.
        bool Ledges = false;
        /// Travel, on foot: the objective is on the bed of a lake, under Travel.DiveDepthMin to DiveDepthMax yards
        /// of water, Travel.DiveMin to DiveMax yards away. Arriving is standing on the bed beside it, which means
        /// swimming down, and the deep ones cannot be reached on one breath: the core's breath timer and its
        /// drowning damage (a fifth of the seat's health a second once the breath is spent) are the price, and
        /// what the seat learns. A spawn point with no water that deep in reach builds an ordinary trip and
        /// reports `dive` 0, as a water arena reports `crossing` 0.
        bool Underwater = false;
        /// Travel: the objective is a chain. Reaching one draws the next from where the seat stands, Travel.ChainMin
        /// to ChainMax yards on, of the same kind as the arena's (a dive arena chains lakebeds), and the episode
        /// runs to its clock rather than ending on arrival: what is measured is how many were reached and whether
        /// the seat is alive at the end. Built for the breath: a chain of lakebeds keeps a seat under water for
        /// longer than one breath lasts, so that coming up for air, or making the breath free with a spell, is a
        /// decision with a price on both sides. A leg no place can be found for leaves the seat with nothing more
        /// to reach for the rest of the clock (chain_broken).
        bool Checkpoints = false;

        [[nodiscard]] uint32 SeatCount() const;
    };

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
        /// Where a flag arena's bases are, one per side. Empty: the second base is searched for, BaseMin-BaseMax
        /// from the first, which is what a stage with no map of its own has to do. Warsong Gulch has real ones.
        std::vector<Position> FlagBases{};
        /// The lowest level its characters may be (flying needs 60), raising a host's fixed level too.
        uint8 MinLevel = 0;

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
}

#endif
