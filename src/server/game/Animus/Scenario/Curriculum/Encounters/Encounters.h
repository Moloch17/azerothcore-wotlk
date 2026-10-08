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

#include "RouteShortcut.h"
#include "BotSlot.h"
#include "InstanceBosses.h"
#include "Encounter.h"
#include "EntranceRespawn.h"
#include "Env.h"
#include "ObjectGuid.h"
#include "RewardLedger.h"
#include "RoutePlanner.h"
#include "SeatView.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "WingRun.h"
#include "SeenPlaces.h"
#include <unordered_set>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
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

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeRebuild(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
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
        };

        struct EnvParty
        {
            Group* PartyGroup = nullptr;
            std::array<SeatParty, MAX_SEATS> Seats;
        };

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
    /// the trash alive, and the route to the last boss is its objective. Won when the last boss dies, lost on the last
    /// wipe or on the clock.
    class InstanceEncounter final : public Encounter
    {
    public:
        /// A wing's cells (Go-Explore): the route's packs as EXPLORE_PACK_WORDS words of EXPLORE_PACK_BITS (a float of
        /// the episode info holds 24 bits exactly), so a route's first 96 packs; the party's yard in buckets.
        static constexpr uint32 EXPLORE_PACK_WORDS = 4;
        static constexpr uint32 EXPLORE_PACK_BITS = 24;
        static constexpr uint32 EXPLORE_PACKS = EXPLORE_PACK_WORDS * EXPLORE_PACK_BITS;
        static constexpr uint32 EXPLORE_YARD_BUCKET = 16;
        static constexpr uint32 EXPLORE_MARKS = 8;

        InstanceEncounter(StageScenario& scenario, uint32 envs);

        /// The level range a dungeon is run at: the dungeon finder's target range for its map and difficulty.
        [[nodiscard]] static std::pair<uint32, uint32> DungeonLevels(BossRow const& row);
        /// The world database's spawn of a row's boss (the first on its map), or null.
        [[nodiscard]] static CreatureData const* FindSpawn(BossRow const& row);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        struct SeatInstance
        {
            bool OutcomePaid = false;
            // A wing's terms paid so far (kills, waypoints, wipes), and whether its death has been.
            uint32 KillsPaid = 0;
            uint32 BossKillsPaid = 0;
            uint32 WaypointsPaid = 0;
            uint32 WipesPaid = 0;
            bool DeathPaid = false;
            float Potential = 0.0f;             // the least route ahead it has reached (Instance.WingProgress)
            bool PotentialReady = false;
            /// Death and the rise at the entrance (dungeon-curriculum I4, EntranceRespawn): out for Respawn.DelayMs,
            /// then alive at the entrance and walking back, rejoined within Respawn.RejoinYards of the party.
            RespawnClock Clock;
            uint32 Walk = 0;                    // the route point the seat walks to next; back to 0 at the door
            uint32 EngagesPaid = 0;             // EnvInstance::ReadyEngages paid for (ReadyPull, every seat)
            /// The party stages' terms paid so far (2026-10-07): a corridor's packs cleared in route order, the chain
            /// pulls, the full clear; and the seat's deaths this run (by role: deaths_tank, ...).
            uint32 ClearsPaid = 0;
            uint32 ChainPaid = 0;
            bool FullClearPaid = false;
            uint32 Deaths = 0;
            mutable uint32 DenseAt = 0;         // the yard of the dense route it was nearest at its last view
            /// Off the route out of a fight -- fallen into a cavern, kited away -- its own way back to it,
            /// planned at DetourMs and again every DETOUR_REPLAN_MS.
            mutable std::vector<Position> Detour;
            /// Taken off the route (past 6 yards from it) and not yet back on it (within 4): the way back is the
            /// detour until then, so a seat at the edge does not swap the two every decision (movement-smooth A8).
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
            bool Recorded = false;
            /// A whole wing (InstanceLadder::Wing): the route from the door to the boss and the next point on it, the
            /// trash killed, the wipes, and the creatures watched for dying.
            std::vector<Position> Route;
            std::vector<float> RouteRemain;     // per route point: yards along the route from it to the end
            /// The route a yard at a time, a yard at a time, and the yard of each route
            /// point: what the crowd block's advance steps along. Empty when the route is the navmesh's.
            std::vector<Position> Dense;
            std::vector<uint32> RouteDense;
            std::vector<uint32> CornerAhead;    // WingPlan's
            std::vector<uint32> CornerBack;
            uint32 RouteNext = 0;
            uint32 TrashKills = 0;
            uint32 BossKills = 0;               // dungeon bosses killed on the way (of the trash kills)
            /// The last decision the dungeon went forward (a kill, a waypoint) or anything fought the party
            /// (Instance.WingStall).
            uint32 ProgressMs = 0;
            uint32 ProgressSeen = 0;            // kills + waypoints at ProgressMs
            uint32 Wipes = 0;
            /// The run's rung of the difficulty ladder and the wipes it stands up at the door, fixed when the
            /// run is drawn; and whether it is an evaluation's, which the running route share leaves out.
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
                uint32 PointAtStart = 0;
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
            /// The instance's entrance, where the dead rise: the door the run came in by, even for a run started
            /// part-way (Go-Explore), whose packs before its start are gone.
            Position Entrance;
            WipeLatch Wipe;                     // the party is down and its wipe counted, until somebody stands
            /// The crowd past the pack's slots (CrowdBlock): on the tank, elites, the tank itself, the next enemies,
            /// and the nearest pack not in the fight.
            ObjectGuid Tank;
            std::vector<ObjectGuid> Overflow;
            std::vector<ObjectGuid> Objects;    // what the party can use near it (CrowdBlock::ACTION_USE_OBJECT)
            mutable std::vector<ObjectGuid> Used;   // what a seat has used this run: each thing once
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
            /// A pull drill (ArenaDefinition::PullDrill): its rung, the pack it pulls and how far the nearest other
            /// stands from it, the route point at the pack (RouteNext goes no further), and how it ended -- the pack
            /// dead with nothing else in the fight, or a second pack joining (the first such creature's entry).
            bool Drill = false;
            uint32 DrillRung = 0;
            uint32 DrillPackIndex = 0;          // the route pack drilled (WingPlan::Packs' index)
            float DrillGap = 0.0f;
            uint32 DrillPoint = 0;
            std::vector<ObjectGuid> DrillPack;
            /// Every creature left alive and its pack (WingPlan::Packs' index, or one of its own past them): the first
            /// creature to fight the party makes its pack the drill's, whichever it is -- the party pulls what it
            /// pulls, and the lesson is one pack at a time.
            std::vector<std::pair<ObjectGuid, uint32>> DrillGroups;
            bool DrillLocked = false;
            bool DrillOther = false;            // the pack pulled was not the route's next
            bool DrillEngaged = false;
            bool DrillCleared = false;
            bool DrillExtra = false;
            uint32 DrillExtraEntry = 0;
            uint32 DrillPeak = 0;               // the most on the party at once
            /// Creatures seen in the slots or past them and not yet counted dead; a kill moves from here to Counted,
            /// which keeps it from being watched (and counted) again. Every decision walks Watched, so it holds only
            /// what can still die rather than everything the run has ever seen.
            std::unordered_set<ObjectGuid> Watched;
            std::unordered_set<ObjectGuid> Counted;
            /// The field route's packs in route order (WingPlan::Packs): where each stands on the route (its yard),
            /// its members' spawn ids, whether any member has ever been found (its grid loaded), and whether it is
            /// cleared -- found and none alive, or despawned by a drill; once cleared, cleared for the run
            /// (UpdateWingEnemies). The goal head's "next pack" places (View). Empty with a navmesh route.
            struct RoutePack
            {
                Position At;
                std::vector<ObjectGuid::LowType> Members;
                bool Cleared = false;
                bool Resolved = false;
            };
            std::vector<RoutePack> RoutePacks;
            /// Each route pack's members' spawn ids -> the pack (its RoutePacks index): what a creature fighting the
            /// party belongs to, for the chain pull.
            std::unordered_map<ObjectGuid::LowType, uint32> PackOf;
            /// A corridor run (ArenaDefinition::CorridorPacks, G2): its packs and which were cleared in route order.
            bool CorridorRun = false;
            WingRun::Corridor Corridor;
            /// The route packs the fight under way has drawn in, and the chain pulls of the run: a pack drawn into a
            /// fight another pack started (Instance.WingChainPull).
            WingRun::FightPacks Drawn;
            uint32 ChainPulls = 0;
            /// The fights started ready (ReadyEngages) the party is paid for: at most one a route pack.
            uint32 ReadyPaidCap = 0;
            /// The dungeon bosses killed on the way, by entry (the per-boss measures, boss_<name>); the last boss is
            /// BossDead.
            std::vector<uint32> BossesKilled;
            std::array<SeatInstance, MAX_SEATS> Seats;
            /// Go-Explore (the learner's EXPLORE_STARTS): a training run started from a cell instead of the door --
            /// its packs cleared and the party's yard / EXPLORE_YARD_BUCKET -- and the cells the run reached, a mark
            /// each time the set of cleared packs changed (the start's first), the last EXPLORE_MARKS of them with the
            /// run's clock: the episode info the learner's archive is built from.
            bool Started = false;
            std::array<uint32, EXPLORE_PACK_WORDS> StartPacks{};
            uint32 StartYard = 0;
            struct CellMark
            {
                std::array<uint32, EXPLORE_PACK_WORDS> Packs{};
                uint32 Yard = 0;
                uint32 Ms = 0;
            };
            std::vector<CellMark> Marks;
        };

        /// A creature a full clear kills (Instance.WingFullClear): alive, hostile to the party, not a critter, a
        /// civilian, a totem, a pet or a summon, and attackable.
        [[nodiscard]] static bool Hostile(Player const* seat, Creature const* creature);
        /// A lever, a button, a goober (the Deadmines' cannon) or a closed door, spawned, ready and not locked.
        [[nodiscard]] static bool Usable(GameObject const* object);
        /// The items the locks of a map's game objects take (LOCK_KEY_ITEM): each seat carries them from the door.
        [[nodiscard]] static std::vector<uint32> const& KeyItems(uint32 mapId);
        /// The dead rise at the entrance after Respawn.DelayMs and walk back (I4); the rejoins counted.
        void RiseDead(Env& env, EnvInstance& fight);
        /// A whole dungeon's way through: its route points every Instance.WingWaypointYards (the last one the boss),
        /// the route a yard at a time with the yard of each point, and the spawns of the creatures it can reach.
        /// A pack of the route, in the order the route reaches it: where it is fought from, the route's yard
        /// there, its creatures' spawns, and how far the nearest creature not cleared before it stands from it.
        struct WingPack
        {
            Position At;
            uint32 Yard = 0;
            std::vector<ObjectGuid::LowType> Members;
            float Gap = 0.0f;
        };
        struct WingPlan
        {
            std::vector<WingPack> Packs;        // every pack the field route walks to; empty with a navmesh route
            std::vector<Position> Route;
            std::vector<Position> Dense;
            std::vector<uint32> RouteDense;
            std::vector<ObjectGuid::LowType> Reachable;     // sorted; empty with a navmesh route: every creature
            bool Field = false;
            /// For each yard of Dense, the farthest yard on (and back) within RouteShortcut::REACH walked straight:
            /// the corners an advance runs through (movement-smooth A8). Empty with a navmesh route.
            std::vector<uint32> CornerAhead;
            std::vector<uint32> CornerBack;
        };
        /// The door-to-boss plan, once per boss: along the server's navmesh (PathGenerator).
        [[nodiscard]] WingPlan WingRoute(Env const& env, Map* map, Player* seat, Creature* boss) const;
        void UpdateWingEnemies(Env& env, EnvInstance& fight);
        /// The pull drill: a pack off the ladder, the packs before it cleared, the party set down short of it.
        /// False when the route has no packs to drill (a navmesh route); the run is the whole dungeon then.
        /// `counted`: the spawn ids HostileTotal counted (sorted); what a start despawns comes off it.
        bool StartDrill(Env& env, Map* map, WingPlan const& plan, std::vector<ObjectGuid::LowType> const& counted);
        /// A Go-Explore start (EnvInstance::Started): the cell's packs despawned -- a dungeon boss among them killed,
        /// so its script opens what its death opens -- and marked cleared, the party on the route at the cell's yard
        /// (back to clear ground), every pay latch at what the start already holds. False, nothing changed, for a
        /// cell this route cannot have.
        bool StartAt(Env& env, Map* map, WingPlan const& plan, std::vector<ObjectGuid::LowType> const& counted);
        /// The party to `start`, the `yard` of the field route: the wipes' spawn, every seat's route point and yard,
        /// and its waypoints paid up to RouteNext (an offset start is not paid the route it did not walk).
        void PlaceParty(Env& env, Position const& start, std::size_t yard);
        /// The run's cleared packs as a cell's words; a mark when they changed (UpdateWingEnemies).
        [[nodiscard]] static std::array<uint32, EXPLORE_PACK_WORDS> ClearedWords(EnvInstance const& fight);
        void MarkCell(Env const& env, EnvInstance& fight);
        /// The drill's pack dead with the fight over, or another creature fighting the party.
        void UpdateDrill(Env& env, EnvInstance& fight);
        /// A corridor run (G2): the packs before its first cleared and the party set down short of it, as a cell's
        /// start (StartAt); its packs to clear in route order. False, nothing changed, when the route has no packs.
        bool StartCorridor(Env& env, Map* map, WingPlan const& plan, std::vector<ObjectGuid::LowType> const& counted);
        /// The route packs fighting the party this decision, for the chain pull (EnvInstance::Drawn).
        void UpdateDrawnPacks(Env& env, EnvInstance& fight);
        /// A sight stage's goal places in a dungeon (SeenPlaces): what `seat` saw and remembers, its map's frontier,
        /// the leader within the minimap's range -- never a pack's or boss's live position or a route.
        void SeenWorld(Env const& env, uint32 seat, SeatView& view) const;
        /// Whether a whole dungeon's run is a full clear: the last boss dead and every creature the clear counts.
        [[nodiscard]] static bool FullClear(EnvInstance const& fight);
        /// The run's success as the stage counts it: a drill's pack pulled and killed alone, a corridor cleared, a
        /// whole dungeon's last boss dead (the `cleared` column, the stand-in split's and the videos' outcome).
        [[nodiscard]] static bool Succeeded(EnvInstance const& fight);
        void NoteDrill(uint32 rung, bool clean);
        /// Instance.WingTrace: follow the fight under way, and log what a wipe ended.
        void TraceWing(Env& env, EnvInstance& fight, bool fighting);
        void LogWipe(Env const& env, EnvInstance const& fight) const;

        [[nodiscard]] std::vector<BossRow const*> const& Rows(Env const& env) const;
        [[nodiscard]] Creature* FindBoss(Map* map, BossRow const& row, WorldObject const* anchor) const;
        [[nodiscard]] float TierScale(Env const& env) const;
        [[nodiscard]] static bool TimeIsUp(Env const& env);

        std::vector<EnvInstance> _envs;
        std::map<InstanceLadder, std::vector<BossRow const*>> _rows;   // per ladder, the rows the database fields
        /// The pull drill's ladder (Instance.PullRung*): the rung and the newest rung's drills, clean or not.
        std::mutex _drillLock;
        uint32 _drillRung = 0;
        std::deque<bool> _drillRuns;
    };
}

#endif
