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

#ifndef ANIMUS_LIB_CURRICULUM_ENCOUNTERS_H
#define ANIMUS_LIB_CURRICULUM_ENCOUNTERS_H

#include "BotSlot.h"
#include "InstanceBosses.h"
#include "Encounter.h"
#include "EntranceRespawn.h"
#include "Env.h"
#include "ObjectGuid.h"
#include "RewardLedger.h"
#include "SeatView.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "WingRun.h"
#include "SeenPlaces.h"
// The nav stages' encounter (decision 0036) lives in its own header, as the sight and seek encounters do.
#include "PointNavEncounter.h"
#include "DirectorLayout.h"
#include "DirectorOrders.h"
#include "WingTeacher.h"
#include <map>
#include <unordered_set>
#include <mutex>
#include <set>
#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

class Battleground;
struct CreatureData;
class Group;
class Map;
class WorldObject;

/*
 * The encounters a StageDefinition can ask for (see Encounter). Each keeps its state per env, sized at construction.
 */
namespace Animus::Curriculum
{

    /// How much an enemy matters to a party, for the order of the enemy slots (PACK_SLOTS, the first
    /// NAMED_ENEMY_SLOTS of them named by the other blocks): the tank's target, then what is on a player, what else
    /// fights, what stands, what is gone. One order per env, so a slot is the same enemy to every seat. Coarse on
    /// purpose: within a fighting rank the encounters keep the slot an enemy had, so the slots do not reshuffle
    /// whenever someone takes a step (a distance order did, and it confused the seeded policies, 2026-10-03).
    enum class EnemyRank : uint8
    {
        TankTarget,
        OnPlayer,
        Fighting,
        Standing,
        Gone
    };

    [[nodiscard]] EnemyRank RankEnemy(Unit const* enemy, Unit const* tank);

    /// The owner and the seats form a real core group every episode (a sim group: it lives only in memory), so party
    /// spells, auras and group heals work as in play. Each seat sees its three teammates and is rewarded for them.
    class PartyEncounter final : public Encounter
    {
    public:
        PartyEncounter(StageScenario& scenario, uint32 envs);

        /// The party's living tank seat, or null.
        [[nodiscard]] Player* Tank(Env const& env) const;
        /// The seat that leads the party this episode (StageDefinition::LeaderAnyRole: drawn from the whole party;
        /// else seat 0), and its bot.
        [[nodiscard]] uint32 LeaderSeat(Env const& env) const { return _envs[env.Index].Leader; }
        /// The leader's role this episode (DungeonRole: 1 tank, 2 healer, 3 damage; 0 unknown).
        [[nodiscard]] uint32 LeaderRole(Env const& env) const;
        /// The others keep with the leader (StageDefinition::LeaderAnyRole): a seat's goal is the living leader; the
        /// leader's own is the next encounter's (the pack it takes the party to, the next site of the clear way).
        [[nodiscard]] bool RouteGoal(Env const& env, uint32 seat, Position& goal) const override;

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeRebuild(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void Teardown(Env& env) override;

    private:
        struct SeatParty
        {
            uint64 TeammateDamageTaken = 0;
            uint64 TeammateHealing = 0;
            uint64 ThreatOnTeammates = 0;
            uint32 TeammatesDied = 0;
            std::array<bool, MAX_SEATS> TeammateDeathSeen{};
            uint64 ActiveMs = 0;                // the seat last dealt damage or healed (Raid.Idle)
            uint32 IdleMs = 0;                  // in a fight with an enemy in reach and nothing done (Raid.Idle)
            // The roles' readings (party, no owner): the tank's enemies held against those on the party, in
            // enemy-decisions; a damage dealer's damage on the tank's target against all it dealt, and its time with
            // an enemy taken off the tank.
            uint64 EnemiesHeld = 0;
            uint64 EnemiesOnParty = 0;
            uint64 TankTargetDamage = 0;
            uint64 DamageDealt = 0;
            uint32 PulledOffMs = 0;
            // The seat's group while the seat fights, member by member and decision by decision: present, and of
            // that alive above half health (group_kept_share, a healer's effectiveness).
            uint32 GroupMemberMs = 0;
            uint32 GroupKeptMs = 0;
            uint32 TankFightMs = 0;             // the tank alive and in a fight ...
            uint32 TankModeMs = 0;              // ... and of that, in its tanking stance, form or aura
            // The leader and the tank (StageDefinition::LeaderAnyRole): the pulls this seat started, those out of
            // turn (and this decision's, to pay), and its quiet time alive beside the leader against all of it.
            uint32 Pulls = 0;
            uint32 OutOfTurn = 0;
            uint32 NewOutOfTurn = 0;
            uint32 QuietMs = 0;
            uint32 KeptMs = 0;
        };

        struct EnvParty
        {
            Group* PartyGroup = nullptr;
            std::array<SeatParty, MAX_SEATS> Seats;
            /// The leader's seat, drawn at Build; the creatures in the fight now, and whether one was the last
            /// decision (a pull is a fight starting from none: what joins one under way is not judged here).
            uint32 Leader = 0;
            std::vector<ObjectGuid> Engaged;
            bool Fighting = false;
        };

        /// The leader's rules this decision (BeforeRewards): a pull starting judged in or out of turn, and the fight's
        /// creatures followed.
        void JudgePulls(Env& env);

        /// The seat index of teammate slot `slot` (0..PARTY_MEMBERS-1) of `seat`: the other seats in order.
        /// The first seat of the group `seat` is in: a party is one group.
        [[nodiscard]] static uint32 GroupFirstSeat(uint32 seat) { return seat / GROUP_SEATS * GROUP_SEATS; }
        void Disband(Env& env);
        /// In the spec's tanking stance, form or aura: Defensive Stance, Bear or Dire Bear Form, Righteous Fury, Frost
        /// Presence.
        [[nodiscard]] static bool InTankingStance(Player const* bot);
        /// What a healer's keeping-up pay is worth now: in full under a Protect goal (or with no goal), else
        /// Party.HealOffGoal of it.
        [[nodiscard]] float HealShare(SeatState const& seat) const;
        /// Each role paid for its own part, and idling charged (Raid.*).
        void RewardRole(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger);

        std::vector<EnvParty> _envs;
    };

    /// A whole dungeon wing, run by a party (Opposition::Instance, InstanceLadder::Wing): the arena's pinned row of
    /// InstanceBosses fixes the map, the level and the difficulty; the party starts at the instance's front door with
    /// the trash alive and finds its own way to the last boss: no route, waypoint or path hint exists for the seats or
    /// the rewards (vision-only movement, decision 0019). Won when the last boss dies, lost on the last wipe or on the
    /// clock.
    class InstanceEncounter final : public Encounter
    {
    public:
        InstanceEncounter(StageScenario& scenario, uint32 envs);

        /// The level range a dungeon is run at: the dungeon finder's target range for its map and difficulty.
        [[nodiscard]] static std::pair<uint32, uint32> DungeonLevels(BossRow const& row);
        /// The world database's spawn of a row's boss (the first on its map), or null.
        [[nodiscard]] static CreatureData const* FindSpawn(BossRow const& row);

        /// **The teacher's view of the way** (TeacherTuning, StageScenario::TeachSeat), worked out once a decision an
        /// env: the checkpoint the tank walks to -- the goal, or the first gate before it still shut -- the way's next
        /// object to open and how, the seat that opens it (a Heavy Door: the tank; the barrel: the seat nearest it;
        /// the cannon: the seat holding the gunpowder, else back to the barrel), and the next pack's member to pull.
        struct TeacherWay
        {
            bool Valid = false;
            uint32 Lower = 0;           // the first leg of the line the seats are looked for on
            uint32 Target = 0;          // the checkpoint the tank walks to
            bool AtGate = false;        // ... a gate still shut
            ObjectGuid Object;          // the way's next object to open (empty: none -- a boss's door, nothing shut)
            WingTeacher::Opens ObjectOpens = WingTeacher::Opens::Click;
            ObjectGuid Opener;          // the seat that opens it
            ObjectGuid Pull;            // the next pack's nearest member not in a fight, while no gate is before it
        };
        [[nodiscard]] TeacherWay const& Teacher(Env const& env, Map* map);
        /// The teacher's share this decision (per leg, PointNavEncounter::LegTau): 0 in an evaluation, a teacher-free
        /// trip, with the teacher off or no way.
        [[nodiscard]] float TeacherTau(Env const& env);
        [[nodiscard]] bool TeacherFree(Env const& env) const { return _envs[env.Index].TeacherFree; }
        /// The party's time with no progress (a kill, a site cleared): the teacher's ready-check valve.
        [[nodiscard]] float NoProgressSeconds(Env const& env) const
        {
            EnvInstance const& fight = _envs[env.Index];
            return float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, fight.ProgressMs)) / 1000.0f;
        }
        /// The next step along the recorded line for a seat: the tank's toward the way's target, another's toward
        /// the tank (TeacherWay::Lower .. Target). False without a line.
        [[nodiscard]] bool TeacherStep(Env const& env, Player const* bot, Player const* tank, bool isTank,
            Position& step);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        /// Where the party goes next (a run with the clear way, ArenaDefinition::RunClear): the first site of it on
        /// from the run's start with a living creature of the dungeon, at that creature; past the last, the clear
        /// way's end. The leader's (the others' is the leader, PartyEncounter's, asked first).
        [[nodiscard]] bool RouteGoal(Env const& env, uint32 seat, Position& goal) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        struct SeatInstance
        {
            bool OutcomePaid = false;
            // A wing's terms paid so far (kills, wipes), and whether its death has been.
            uint32 KillsPaid = 0;
            uint32 BossKillsPaid = 0;
            uint32 GatesPaid = 0;               // gates of the way opened by the party's hand, paid (GateOpened)
            uint32 WipesPaid = 0;
            bool DeathPaid = false;
            /// Death and the rise at the entrance (dungeon-curriculum I4, EntranceRespawn): out for Respawn.DelayMs,
            /// then alive at the entrance and walking back, rejoined within Respawn.RejoinYards of the party.
            RespawnClock Clock;
            uint32 EngagesPaid = 0;             // EnvInstance::ReadyEngages paid for (ReadyPull, every seat)
            /// The seat's line against the recorded one (the path_* measures): yards off the leg's recorded line,
            /// summed over its living decisions, those decisions, those PATH_OFF_YARDS or more off, and the yards it
            /// walked (a jump past PATH_JUMP_YARDS -- a rise, a teleport -- not counted).
            float PathSum = 0.0f;
            uint32 PathSteps = 0;
            uint32 PathOff = 0;
            float PathNow = -1.0f;      // yards from the recorded line at the last decision (-1: not measured)
            float Travel = 0.0f;
            bool HasLast = false;
            Position Last;
            /// The party stages' terms paid so far (2026-10-07): the full clear; and the seat's deaths this run (by
            /// role: deaths_tank, ...).
            bool FullClearPaid = false;
            uint32 Deaths = 0;
            /// The frontier of its own mental map (SeenPlaces::Frontier), refreshed every FRONTIER_MS: its goal places'
            /// way on in a sight stage.
            mutable std::vector<SeenPlaces::Point> Frontier;
            mutable uint32 FrontierMs = 0;
            mutable bool FrontierReady = false;
            uint32 FoodItem = 0;                // what it eats and drinks between pulls (Instance.WingSupplies)
            uint32 DrinkItem = 0;
        };

        struct EnvInstance
        {
            BossRow const* Row = nullptr;
            uint32 MapId = 0;
            uint32 Entry = 0;
            uint32 Tier = 0;
            ObjectGuid Boss;
            uint32 BossHealth = 1;
            float HealthLeft = 1.0f;
            bool Engaged = false;
            uint32 EngageMs = 0;
            bool BossDead = false;
            bool Wiped = false;
            bool Evaded = false;
            /// A whole wing (InstanceLadder::Wing): the trash killed, the wipes, and the creatures watched for dying.
            uint32 TrashKills = 0;
            uint32 BossKills = 0;               // dungeon bosses killed on the way (of the trash kills)
            /// The last decision the dungeon went forward (a kill) or anything fought the party (Instance.WingStall).
            uint32 ProgressMs = 0;
            uint32 ProgressSeen = 0;            // kills at ProgressMs
            uint32 Wipes = 0;
            /// The run's rung of the difficulty ladder and the wipes it stands up at the door, fixed when the
            /// run is drawn; and whether it is an evaluation's, which the running share leaves out.
            uint32 Rung = 0;                    // the ladder's rung the run was drawn on (StageScenario::WING_RUNGS)
            bool Probe = false;                 // a measure of the policy at the rung (Instance.WingProbe)
            uint32 WipesAllowed = 1;
            bool Evaluating = false;
            /// The fight under way, for the wipe's log line (Instance.WingTrace): when it began, what had been killed
            /// by then, the most enemies on the party at once and who they were, and each death in order.
            struct FightTrace
            {
                bool InFight = false;
                uint32 StartMs = 0;
                uint32 KillsAtStart = 0;
                uint32 PeakEngaged = 0;
                uint32 PeakElites = 0;
                uint32 PeakOnTank = 0;
                std::string PeakEntries;
                std::string Deaths;
                std::array<bool, MAX_SEATS> Dead{};
                std::array<uint8, MAX_SEATS> Mana{};    // each seat's mana share when last seen alive
            } Trace;
            /// The party is in a fight this decision, and how many hostile creatures are on it.
            bool Fighting = false;
            /// Fights the party started ready -- every living seat at Instance.WingReadyShare of its health and mana --
            /// which the tank is paid for (Instance.WingEngage).
            uint32 ReadyEngages = 0;
            uint32 OnParty = 0;
            float CrowdSeconds = 0.0f;          // seconds the party had more than a pack on it
            uint32 HostileTotal = 0;            // the instance's creatures a full clear kills, at the start
            uint32 LastMs = 0;                  // the run's clock at its last update, and its level, for its log line
            uint32 Level = 0;
            uint32 StuckLoggedMs = 0;           // when the next "Wing stuck" line may be written
            uint32 Rises = 0;                   // seats that rose at the entrance and walked back (Respawn.*)
            uint32 Rejoins = 0;                 // ... and reached the party again
            uint32 RejoinMsTotal = 0;           // ... in this long altogether
            /// The instance's entrance, where the dead rise: the door the run came in by.
            Position Entrance;
            WipeLatch Wipe;                     // the party is down and its wipe counted, until somebody stands
            /// The crowd past the pack's slots (CrowdBlock): on the tank, elites, the tank itself, the next enemies,
            /// and the nearest pack not in the fight.
            ObjectGuid Tank;
            std::vector<ObjectGuid> Overflow;
            std::vector<ObjectGuid> Objects;    // what the party can use near it (CrowdBlock::ACTION_USE_OBJECT)
            mutable std::vector<ObjectGuid> Used;   // what a seat has used this run: each thing once
            /// **The party's own hand on the way's gates** (the director, 2026-10-26): each object a seat pressed
            /// this run, when, and whether it was an opener -- a button or a lever, the cannon (an item used on it)
            /// -- and per gate of the way whether its opening was paid (GateOpened). A gate counts as the party's
            /// when it opens within GATE_PRESS_MS of a press on the gate itself or, a door, of a press on an opener
            /// (the cannon's blows the Iron Clad Door, a lever swings a boss's door). A boss's door that opened on
            /// its boss's death is the kill's (2026-10-27: every Factory Door opening was paid as the party's, any
            /// press within ten seconds counting).
            struct Press
            {
                ObjectGuid Guid;
                uint32 Ms = 0;
                bool Opener = false;
            };
            std::vector<Press> Pressed;
            std::vector<uint8> GatePaid;
            uint32 GatesOpened = 0;             // paid to every seat (SeatInstance::GatesPaid)
            ObjectGuid Approached;              // the thing the tank has been near, unused, since ApproachedMs
            uint32 ApproachedMs = 0;
            bool EndLogged = false;             // the "Wing time" line is written once, when the clock runs out
            uint32 LastKillMs = 0;              // when the party last killed something
            bool HasAhead = false;
            Position Ahead;
            uint32 AheadSize = 0;
            /// The nearest creature out of the fight past the pack ahead (CrowdBlock's second pack): what pulling the
            /// one ahead may bring with it.
            bool HasSecond = false;
            Position Second;
            /// Creatures seen in the slots or past them and not yet counted dead; a kill moves from here to Counted,
            /// which keeps it from being watched (and counted) again. Every decision walks Watched, so it holds only
            /// what can still die rather than everything the run has ever seen.
            std::unordered_set<ObjectGuid> Watched;
            std::unordered_set<ObjectGuid> Counted;
            /// The dungeon bosses killed on the way, by entry (the per-boss measures, boss_<name>); the last boss is
            /// BossDead.
            std::vector<uint32> BossesKilled;
            std::array<SeatInstance, MAX_SEATS> Seats;
            /// The clear way (a RunClear arena's, PointNav's): the checkpoint the run started at (WingReverse: the
            /// rung's, everything spawned before it killed first, and how many that was), and the next one with a
            /// living creature, the party's goal (GoalAt) while HasGoal.
            PointNavEncounter::ClearWay const* Way = nullptr;
            int32 StartSite = 0;
            bool StartDrawn = false;            // a drawn start (Instance.WingStarts): not a whole run
            /// The teacher (TeacherTuning): a teacher-free leg trip (it measures the policy alone), the decision the
            /// way's teacher view was worked out for, that view, and the seat named to loot the barrel.
            bool TeacherFree = false;
            uint32 TeacherStamp = UINT32_MAX;
            TeacherWay Teacher;
            ObjectGuid LootOpener;
            uint32 GatePaidBossAlive = 0;       // door gates paid with a boss spawned before them alive (invariant)
            uint32 Killed = 0;
            int32 GoalSite = 0;
            bool HasGoal = false;
            Position GoalAt;
            /// A leg of the clear way (ArenaDefinition::WingLegs): the checkpoint it goes to, whether it is done (its
            /// pack dead, its gate open, the party at the checkpoint), whether its outcome still counts toward its
            /// rate, and how many legs the way has. A whole run has LegTrip false.
            bool LegTrip = false;
            int32 Leg = -1;
            uint32 LegCount = 0;
            bool LegDone = false;
            bool LegCounted = false;
            bool LegMissLogged = false;         // the leg-missed line written (at its last decision)
            uint32 LegDoneMs = 0;               // the leg's clock when it was done
        };

        /// A backwards wing run's start (ArenaDefinition::WingReverse): the rung's checkpoint of the clear way,
        /// everything the dungeon spawns before it killed (its bosses' scripts open their doors), the arena's shut
        /// doors opened when the start is past the gate, and the party stood at the last site before it.
        void StartReverse(Env& env, EnvInstance& fight, Map* map, Player* seat);
        /// The run started at checkpoint `start` of the clear way, the party stood round checkpoint `standAt`:
        /// everything spawned before `start` killed, the gates before it opened (the arena's shut doors past the
        /// way's gate, and every recorded gate's own object).
        void StartFrom(Env& env, EnvInstance& fight, Map* map, Player* seat, uint32 start, uint32 standAt);
        /// A leg of the clear way for this episode (ArenaDefinition::WingLegs), or a whole run: drawn against the
        /// rung's leg rates (PointNavEncounter::LegsLearnedAt, DrawLegAt), started at the checkpoint before, its clock
        /// the leg's.
        void PlanLeg(Env& env, EnvInstance& fight, Map* map, Player* seat);
        /// A drawn start (Instance.WingStarts): the run from a checkpoint drawn by its failure rate, never between a
        /// boss's site and its door (StartsBlocked), the clock the recorded time that remains.
        void PlanDrawnStart(Env& env, EnvInstance& fight, Map* map, Player* seat);
        /// Per checkpoint, whether a start there is blocked: after a dungeon boss's site up to the checkpoint past
        /// its door (the door opens with the boss's death, which a start deals outside its script).
        [[nodiscard]] static std::vector<uint8> StartsBlocked(PointNavEncounter::ClearWay const& way, Map* map);
        /// Whether a dungeon boss spawned at a checkpoint before `k` is alive.
        [[nodiscard]] static bool BossAliveBefore(PointNavEncounter::ClearWay const& way, uint32 k, Map* map);
        /// Whether a leg's objective is met: its checkpoint's pack dead, its gate's object open, a living seat at the
        /// checkpoint.
        [[nodiscard]] bool LegMet(Env const& env, EnvInstance const& fight, Map* map, Player const* seat) const;
        /// The party's goal on (GoalSite, GoalAt): the first checkpoint from GoalSite with a living creature.
        void AdvanceGoal(Env& env, EnvInstance& fight, Map* map, Player const* seat);

        /// A creature a full clear kills: alive, hostile to the party, not a critter, a
        /// civilian, a totem, a pet or a summon, and attackable.
        [[nodiscard]] static bool Hostile(Player const* seat, Creature const* creature);
        /// A lever, a button, a goober (the Deadmines' cannon) or a closed door, spawned, ready and not locked.
        [[nodiscard]] static bool Usable(GameObject const* object);
    public:
        /// **The party's objects and keys** (the director, 2026-10-26): what the party can use near it this decision
        /// (a door, a lever, the cannon, a key chest), the way's next gate's object (empty when none, past the last,
        /// or not in the instance), the key item an object's lock takes (0 none), and whether a seat carries a key
        /// item of the map.
        [[nodiscard]] std::vector<ObjectGuid> const& PartyObjects(Env const& env) const
        {
            return _envs[env.Index].Objects;
        }
        [[nodiscard]] ObjectGuid NextGate(Env const& env) const;
        [[nodiscard]] static uint32 KeyOfLock(GameObject const* object);
        [[nodiscard]] static bool CarriesKey(Player const* bot, uint32 mapId);
        /// A seat's press on an object, noted with the decision's clock (GateOpened; the director's Use orders).
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        static constexpr uint32 GATE_PRESS_MS = 10000;
    private:
        /// The items the locks of a map's game objects take (LOCK_KEY_ITEM): each seat carries them from the door.
        [[nodiscard]] static std::vector<uint32> const& KeyItems(uint32 mapId);
        /// The dungeon's enemies and what the party can use, once per decision (UpdateEnemies calls it).
        void UpdateWingEnemies(Env& env, EnvInstance& fight);
        /// The dead rise at the entrance after Respawn.DelayMs and walk back (I4); the rejoins counted.
        void RiseDead(Env& env, EnvInstance& fight);
        /// A sight stage's goal places in a dungeon (SeenPlaces): what `seat` saw and remembers, its map's frontier,
        /// the leader within the minimap's range -- never a pack's or boss's live position or a route.
        void SeenWorld(Env const& env, uint32 seat, SeatView& view) const;
        /// Whether a whole dungeon's run is a full clear: the last boss dead and every creature the clear counts.
        [[nodiscard]] static bool FullClear(EnvInstance const& fight);
        /// The run's success as the stage counts it: the last boss dead (the `cleared` column, the stand-in split's and
        /// the videos' outcome).
        [[nodiscard]] static bool Succeeded(EnvInstance const& fight);
        /// The share of the dungeon cleared: the creatures killed (the last boss among them) over those a full clear
        /// counts, +1 for the boss; 0 before the instance is counted. No route and no navmesh: what the clock-out cost
        /// (Instance.WingTimeout), the ladder's progress and `wing_cleared_share` are measured by.
        [[nodiscard]] static float ClearedShare(EnvInstance const& fight);
        /// Instance.WingTrace: follow the fight under way, and log what a wipe ended.
        void TraceWing(Env& env, EnvInstance& fight, bool fighting);
        void LogWipe(Env const& env, EnvInstance const& fight) const;

        [[nodiscard]] std::vector<BossRow const*> const& Rows(Env const& env) const;
        [[nodiscard]] Creature* FindBoss(Map* map, BossRow const& row, WorldObject const* anchor) const;
        [[nodiscard]] float TierScale(Env const& env) const;
        [[nodiscard]] static bool TimeIsUp(Env const& env);
        /// Whether the episode is over: a wipe or the clock, else a leg's objective, else the last boss's death.
        [[nodiscard]] static bool Over(Env const& env, EnvInstance const& fight);
        void LogLegMissed(Env const& env, EnvInstance const& fight, Map* map, Player const* seat) const;

        std::vector<EnvInstance> _envs;
        /// The recorded line flattened for the teacher (TeacherStep): its points in order, the yards along it, and
        /// each leg's first and one-past-last point; per way, built once.
        struct TeacherLine
        {
            std::vector<Position> Points;
            std::vector<float> Along;
            std::vector<uint32> LegFirst;
            std::vector<uint32> LegEnd;
        };
        std::map<PointNavEncounter::ClearWay const*, TeacherLine> _teacherLines;
        std::mutex _teacherLineLock;
        [[nodiscard]] TeacherLine const& LineOf(PointNavEncounter::ClearWay const& way);
        /// The legs whose start has been logged (LogLegStart), (rung, leg) as rung * 1000 + leg, once each: envs run on
        /// map threads at once.
        std::set<uint32> _legStartLogged;
        std::mutex _legStartLock;
        /// A leg's start, once a leg a rung: its gate's object and whether it is shut, and the bosses before it alive.
        void LogLegStart(Env const& env, EnvInstance const& fight, Map* map);
        std::map<InstanceLadder, std::vector<BossRow const*>> _rows;   // per ladder, the rows the database fields
    };

    /// **The director** (restored 2026-10-26: "we do want a director for long term planning and group coordination"):
    /// one learned agent a party, after the seats, that observes the party, the enemies and the objects near it as
    /// sets (DirectorLayout) and edits a standing order on its turns (DirectorOrders). Each seat reads its own order
    /// through the order block and is paid for carrying it out: OrderDone when it is, OrderFailed when it lapses,
    /// LootUnordered for a key chest looted unbidden. The director is paid what following achieved -- the party's
    /// mean reward less those terms -- less its own churn and the orders it could see were impossible.
    class DirectorEncounter final : public Encounter
    {
    public:
        DirectorEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void Update(Env& env) override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;

        /// Whether the director commands this episode: the arena is a party group's, with a seat to command.
        [[nodiscard]] bool Active(Env const& env) const;
        /// Whether this decision is the director's turn to speak, decided before it sees that it is (ObserveDirector).
        void PrepareTurn(Env& env);
        void ViewDirector(Env& env, DirectorLayout::DirectorView& view);
        /// One call, this decision's action of the director's row (ApplyActions, before the seats act on it).
        void Call(Env& env, int32 action);
        /// The director's row's reward, from the seats' (StageScenario::Reward): their mean less the order terms
        /// paid them this decision, less the director's own churn and impossible orders.
        [[nodiscard]] float DirectorReward(Env& env, float const* seatRewards);
        /// **The director's teacher** (TeacherTuning, Teacher.Director): a scripted director's call this decision --
        /// on its turn, the most urgent order the party lacks (a heal for the one hurt, the gunpowder used, the barrel
        /// looted, the tank's target focused, the next pack pulled, a straggler called back), its member addressed
        /// first -- written into the director's hint columns after its row (`obs`, `mask` final), and played with
        /// probability `tau` (segments as the seats'). Nothing at tau 0.
        void Teach(Env& env, float* obs, uint8 const* mask, float tau);
        [[nodiscard]] bool TeacherPlays(Env const& env) const { return _envs[env.Index].TeacherPlays; }
        [[nodiscard]] int32 TeacherAction(Env const& env) const
        {
            return std::max(_envs[env.Index].TeacherAction, int32(DirectorLayout::ACTION_HOLD));
        }

    private:
        struct EnvDirector
        {
            DirectorOrders Orders;
            uint32 Step = 0;                    // decisions this episode
            /// What each seat was paid in order terms this decision (taken off the director's reward), and what the
            /// seat owes: a key chest looted unbidden.
            std::array<float, MAX_SEATS> CompliancePaid{};
            std::array<uint8, MAX_SEATS> LootUnorderedDue{};
            std::array<uint8, MAX_SEATS> DoneFlash{};       // the order carried out, for the next observation
            uint32 ChurnDue = 0;
            uint32 ImpossibleDue = 0;
            /// The slots the director's last view named, so a call's slot resolves to what it saw.
            std::array<ObjectGuid, DirectorLayout::DIRECTOR_ENEMIES> EnemySlots{};
            std::array<ObjectGuid, DirectorLayout::DIRECTOR_OBJECTS> ObjectSlots{};
            /// Each order's bookkeeping at issue: whether the seat carried a key then, the object's state then.
            std::array<uint8, MAX_SEATS> HadKey{};
            std::array<uint32, MAX_SEATS> ObjectStateAtIssue{};
            std::array<uint8, MAX_SEATS> Pressed{};         // the seat pressed its order's object since the order
            /// Orders priced by their number (Director.OrderIssued, DonePerMinute): the orders charged so far, each
            /// seat's tokens for OrderDone pay, and the orders carried out that no token paid.
            uint32 OrdersCharged = 0;
            /// The director's teacher (Teach): its call this decision (-1 none), whether it plays it, its segment, and
            /// the episode's tallies.
            int32 TeacherAction = -1;
            bool TeacherPlays = false;
            uint32 TeacherSegmentLeft = 0;
            bool TeacherSegmentPlays = false;
            uint32 TeacherDecisions = 0;
            uint32 TeacherPlayed = 0;
            std::array<float, MAX_SEATS> DoneTokens{};
            uint32 DoneUnpaid = 0;
            RewardLedger Rewards;               // the director's own terms, for its columns
        };
        [[nodiscard]] static uint32 ObjectState(GameObject const* object);
        [[nodiscard]] bool Ready(Player const* bot) const;
        void Judge(Env& env, uint32 seat, Player* bot);
        void Finish(Env& env, uint32 seat, bool done);
        std::vector<EnvDirector> _envs;
    };
}

#endif
