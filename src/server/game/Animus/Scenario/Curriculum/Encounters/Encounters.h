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
#include "DifficultyLadder.h"
#include "InstanceBosses.h"
#include "LifeEncounter.h"
#include "Encounter.h"
#include "DirectorLayout.h"
#include "DirectorOrders.h"
#include "Env.h"
#include "HumanPools.h"
#include "ObjectGuid.h"
#include "RewardLedger.h"
#include "RoutePlanner.h"
#include "ScriptedPlayer.h"
#include "SeatView.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include <unordered_set>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <array>
#include <functional>
#include <map>
#include <string>
#include <mutex>
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
    /// What stopping a cast was worth, as a multiple of the stage's own Interrupt weight: a heal undoes damage
    /// already dealt, an area spell would have hit everyone, a long cast was a large part of the caster's output
    /// (IncomingSpell::Prevented). Never below 1 -- the flat term is how a class finds interrupting at all. Shared
    /// so the duel and the pack price the same prevented cast the same way.
    [[nodiscard]] float PreventedScale(float heal, float area, float longCast, uint8 prevented);

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

    /// Scripted enemy players: the PvP stages' opponent and the ambushers.
    namespace EnemyPlayers
    {
        /// Give `enemy` the other side's faction and flag both for PvP, which players need to attack each other.
        void MakeEnemies(Player* player, Player* enemy);
        /// Flag a player for PvP (zone updates can drop the flag).
        void Flag(Player* player);

        /// A bot slot's names and account ids, per session slot.
        struct Naming
        {
            std::function<std::string(uint8)> Name;
            std::function<uint32(uint8)> Account;
        };

        struct Spawned
        {
            Player* Bot = nullptr;
            uint8 Class = 0;
            Aptitude Apt;
        };

        /// Rebuild `slot` as an enemy player at `level` of a random class and role (tuning's chances), with a
        /// standard build, kit and PvP gear, 40-50 yd from `near` in `map`. Bot is null on failure.
        Spawned Create(BotSlot& slot, Naming const& naming, uint8 level, CurriculumTuning::OpponentTuning const& tuning,
            Player* near, Map* map, uint32 mapId, ScriptedPlayer::State& state);
    }

    /// A same-level creature spawned out of aggro range, which fights back. Reward: CombatReward::OneOnOne.
    ///
    /// Its difficulty adapts per class/role (CurriculumTuning::DifficultyTuning): once a class/role wins most of its
    /// fights at a tier, its opponents come from the next one -- a level or more above it, then elites. A fight that
    /// simple play wins every time teaches nothing a plan would add. An evaluation spreads its seeds over every tier
    /// instead, so two checkpoints meet the same fights.
    /// A hazard drill with nothing to fight: fire lands under each seat every few seconds and stays, so the only
    /// thing that hurts is standing still. A trap gameobject is the one ground hazard with no unit behind it, which
    /// is what lets the stage have fire without an enemy (see HazardEncounter.cpp for why it is placed there).
    class HazardEncounter final : public Encounter
    {
    public:
        HazardEncounter(StageScenario& scenario, uint32 envs);

        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeRebuild(Env& env) override;
        /// The stage ending: its patches and casters go with it (as a reset clears them), or they outlive the stage on
        /// the continent replicas, which the next stage keeps.
        void Teardown(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        struct EnvHazards
        {
            ObjectGuid Emitter;             // the invisible trigger that lays the ground
            uint32 Spell = 0;               // the persistent area aura it casts, drawn per episode for the level
            uint32 Placed = 0;              // patches this episode, for the episode info
            uint32 NextMs = 0;              // episode time the next patch is due
        };

        /// Remove whatever is still burning.
        void Clear(Env& env);

        std::vector<EnvHazards> _envs;
    };

    /// The rotation drill (Opposition::Dummy, ArenaDefinition::Drill): the kit against the duel pool's creatures with
    /// their health scaled to outlive the episode, standing still or wandering with more to switch to, hitting back,
    /// or with the seat bleeding. Paid for output against the dummy's own health, and for mana kept.
    class DummyEncounter final : public Encounter
    {
    public:
        DummyEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        struct EnvDummies
        {
            DummyDrill Drill = DummyDrill::Still;
            uint8 Level = 1;
            uint32 BaseHealth = 1;          // the main dummy's health before scaling: output is measured in it
            uint32 Adds = 0;
            uint32 NextAddMs = 0;
            uint32 NextBleedMs = 0;
            float Output = 0.0f;            // damage dealt, in kills' worth of BaseHealth
            float ManaKept = 0.0f;
            bool KillPaid = false;
            bool DeathPaid = false;
            bool EndPaid = false;
        };

        /// One dummy near the seat, into env.Targets; the first of an episode (`main`) sets the base health.
        Creature* Spawn(Env& env, Map* map, Player* bot, bool main);
        [[nodiscard]] static bool TimeIsUp(Env const& env);

        std::vector<EnvDummies> _envs;
    };

    class CreatureEncounter final : public Encounter
    {
    public:
        CreatureEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void WriteState(Env const& env, float* state) const override;

        /// Class `layout` built as `spec`'s current training tier.
        [[nodiscard]] uint32 Tier(uint16 layout, uint8 spec) const { return _ladder.Tier(layout, spec); }

    private:
        struct EnvFight
        {
            uint8 Tier = 0;
            bool Elite = false;
            uint16 Layout = 0;
            uint8 Spec = 0;             // ... and the build it drew, which has its own rung
            bool Counts = false;        // a training fight at its class/build's current tier: its outcome moves it
            bool Recorded = false;      // the outcome is in
            ObjectGuid PendingInterrupt;// a casting opponent the seat just cast an interrupt at
            uint32 Interrupts = 0;      // landed this episode: the duel paid for these but never reported them
            uint32 ControlMs = 0;       // the opponent held out of the fight (not paid here, only measured)
        };

        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;

        /// The episode's time limit is reached.
        [[nodiscard]] static bool TimeIsUp(Env const& env);

        std::vector<EnvFight> _envs;
        DifficultyLadder _ladder;
    };

    /// Packs of creatures (casters included, often linked): one pack, or the gauntlet's pull after pull with breaks
    /// between. Pulls are shared by every seat: whether one was cleared is decided once per decision, before the
    /// seats' rewards, and a cleared gauntlet pull is removed after them. With an owner, pulls spawn around it, and
    /// after a pull everyone who died stands up again (a pull that killed everyone is cleared away).
    class PullsEncounter final : public Encounter
    {
    public:
        PullsEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void AfterRewards(Env& env) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

        /// Whether an enemy is held out of the fight: stunned, incapacitated, asleep, polymorphed, feared, or rooted
        /// out of melee reach of what it was fighting and not casting at it.
        [[nodiscard]] static bool Controlled(Unit const* enemy);

    private:
        /// Most creatures a pull spawns: a solo or gauntlet pull's pack, and a party's (a drill's single pack, a
        /// camp's packs together). What the enemy slots were when these were tuned (PACK_SLOTS 4, MAX_TARGETS 8);
        /// the slots grew so a seat sees a dungeon's pulls, not so the drills' pulls grow with them.
        static constexpr uint32 PACK_SPAWN_MAX = 4;
        static constexpr uint32 PARTY_SPAWN_MAX = 8;
        static constexpr uint32 CAMP_PACKS = 4;     // most packs a camp stands (two creatures each: PARTY_SPAWN_MAX)

        struct SeatPull
        {
            uint32 Interrupts = 0;
            ObjectGuid PendingInterrupt;        // a casting enemy the seat just cast an interrupt at
            uint64 PullDamageTaken = 0;
            uint32 FoodItem = 0;
            uint32 DrinkItem = 0;
            uint32 FoodUsed = 0;
            uint32 DrinkUsed = 0;
            uint32 SustainCasts = 0;
            // Recovery between pulls (solo gauntlet).
            float ReadyHealth = 1.0f;           // health fraction the decision before (kept through an engage) ...
            float ReadyMana = 1.0f;             // ... and mana fraction (1 without mana)
            float EngageHealthSum = 0.0f;       // over the pulls engaged
            float EngageManaSum = 0.0f;
            float BuffCoverageSum = 0.0f;       // buff coverage when each pull was engaged (SupportBlock::BuffCoverage)
            uint32 PullsEngaged = 0;
            uint32 PullsStartedLow = 0;         // engaged below half health or 30% mana
            uint32 RestMs = 0;                  // eating or drinking
            uint32 FoodFailed = 0;
            uint32 DrinkFailed = 0;
            uint32 MealsCutShort = 0;           // food or drink ended early with health or mana still to restore
            int32 FoodLeftMs = -1;              // the food aura's remaining time last decision; -1 without one ...
            int32 DrinkLeftMs = -1;             // ... and the drink's
            /// CombatTally::PreparationMs when the current pull spawned: what the seat prepared for this pull is
            /// what its stall grace is refunded for (PullTuning::PreparationRefundMaxMs). Counted over the episode,
            /// a gauntlet seat carried one pull's buffing into the grace of every pull after it.
            uint32 PreparationBaseMs = 0;
            uint32 ControlMs = 0;               // enemy-time kept out of the fight by crowd control (solo gauntlet)
            float PullControlPaid = 0.0f;       // ... and the control reward paid for the current pull
            /// Crowd control priced as the damage it prevents (single pack). What holding an enemy out of the fight
            /// saves is that enemy's own damage rate, measured over the time it was alive and free to act; an enemy
            /// that has not been free for Pulls.ControlRateMinMs yet is estimated from the pull's measured mean, and
            /// a pull with nothing measured from Pulls.ControlFallbackDps -- so a pre-pull Sap, which never lets its
            /// target swing at all, is still paid for what it prevents.
            std::array<uint64, MAX_TARGETS> SlotDamage{};   // damage each enemy slot dealt this seat, this pull ...
            std::array<uint32, MAX_TARGETS> SlotFreeMs{};   // ... and how long it was alive and free to act
            /// Wall time of the pull with at least one add held, which is what extends the overtime grace. Enemy-time
            /// (ControlMs) would let two adds held at once buy twice the grace for the same delay.
            uint32 ControlledMs = 0;
            float ControlPrevented = 0.0f;      // damage prevented this episode, in the seat's maximum healths
        };

        struct EnvPulls
        {
            bool Linked = false;
            uint32 PackSize = 0;                // creatures in the episode's first pull
            uint32 PullKills = 0;               // dead enemies of the current pull
            uint32 Kills = 0;
            uint32 PullStartMs = 0;
            bool PullEngaged = false;           // a creature of the current pull entered combat ...
            uint32 PullEngageMs = 0;            // ... at this episode time: the fast clear bonuses count from here
            bool PullCleared = false;          // decided once per decision, before the seats' rewards
            uint32 NewKills = 0;                // ... and the kills since the last decision
            uint32 PullsCleared = 0;
            uint32 QuietSinceMs = 0;            // episode time the last pull ended
            uint32 NextPullMs = 0;              // spawn the next pull at this episode time
            uint32 ArriveMs = 0;                // solo gauntlet: an unengaged pull comes to the seat at this time
            bool Arrived = false;               // ... and has been sent
            uint32 PullsArrived = 0;            // pulls that came to the seat before it engaged them
            bool EliteOrHigher = false;
            uint32 Rung = 0;                    // single pack: its ladder rung ...
            uint16 RungLayout = 0;              // ... for this class
            uint8 RungSpec = 0;                 // ... built this way, which is what the rung is kept against
            bool RungCounts = false;            // ... a training pack at the class/build's own rung
            bool RungRecorded = false;          // ... whose outcome is in
            uint32 Wipes = 0;                   // owner stages: pulls that killed everyone and were cleared away
            bool OwnerDied = false;             // owner stages: the owner died this episode (it stands up again)
            bool AwaitingRevive = false;        // owner stages: someone dead waits for a resurrection (Recover)
            std::array<SeatPull, MAX_SEATS> Seats;
            // A camp's packs (PullSchedule::Camp): which pack each creature stands with, and how each was fought.
            std::vector<std::pair<ObjectGuid, uint8>> CampMembers;
            uint8 CampPacks = 0;
            std::array<bool, CAMP_PACKS> CampMixed{};       // ... in a fight beside another pack at some point
            std::array<bool, CAMP_PACKS> CampCleared{};
            uint32 CampNewClean = 0;            // packs killed on their own since the last decision
            uint32 CampFighting = 0;            // packs in the fight this decision
            uint32 CampPeak = 0;                // ... and the most at once this episode
            uint32 CampClean = 0;               // packs killed on their own this episode
            uint32 CampQuietMs = 0;             // episode time nothing of the camp was last in a fight
            int8 PatrolPack = -1;               // the camp's patrol, as a pack number; -1 without one ...
            Position PatrolA;                   // ... walking between its first pack and its last
            Position PatrolB;
            bool PatrolToB = false;
            // A party's run (GroupRun): its rung on the run ladder and what the rung gave it.
            uint32 RunRung = 0;
            uint32 RunLength = 0;               // pulls; 0 = the whole planned run
            uint32 RunWipes = 0;                // wipes it may stand up from
            uint32 RunLevels = 0;               // above the seat's, on every pull
            bool RunCounts = false;
            bool RunRecorded = false;
        };

        /// A party's planned run (PullSchedule::Sequence with a party of its own): the group stage's corridor, on
        /// its own ladder.
        [[nodiscard]] bool GroupRun(Env const& env) const
        {
            ArenaDefinition const& arena = _scenario.Arena(env);
            return arena.Schedule == PullSchedule::Sequence && arena.PartyGroup && arena.Seats == SeatPlan::Party;
        }
        /// The pulls in this env's planned run.
        [[nodiscard]] uint32 RunLength(Env const& env) const;
        /// A party's run ended: won (every pull cleared) or lost; it moves the run ladder once.
        void RecordRun(Env& env, bool won);

        /// A proper party's single pack (the roles stage's tank, healer and damage drills): up to PARTY_SPAWN_MAX
        /// creatures, ordered as a camp's.
        [[nodiscard]] bool PartyDrill(Env const& env) const
        {
            ArenaDefinition const& arena = _scenario.Arena(env);
            return arena.ProperParty && arena.Schedule == PullSchedule::SinglePack;
        }

        /// A camp of packs (PullSchedule::Camp): the pull drill.
        [[nodiscard]] bool Camp(Env const& env) const
        {
            return _scenario.Arena(env).Schedule == PullSchedule::Camp;
        }

        /// Tally a camp after the step: which packs are fighting, which fought beside another, which were killed on
        /// their own.
        void UpdateCamp(Env& env);
        /// Order a camp's slots for the next observation: what is fighting first, then the nearest to the tank, the
        /// dead last, with the seats' selections and per-slot tallies moved along. After the rewards, which read
        /// this step's per-slot stats in the order they were made in, and before the seats observe.
        void OrderCamp(Env& env);

        /// Whether the env's episode is pull after pull (else a single pack).
        [[nodiscard]] bool Gauntlet(Env const& env) const
        {
            PullSchedule const schedule = _scenario.Arena(env).Schedule;
            return schedule == PullSchedule::Gauntlet || schedule == PullSchedule::Sequence;
        }

        /// Seats that fight as a group: beside an owner, or a party or raid of their own. The dead stand up between
        /// pulls (Recover), and the group's win is lasting with no wipe (GauntletOwnerTerms).
        [[nodiscard]] bool Grouped(Env const& env) const
        {
            ArenaDefinition const& arena = _scenario.Arena(env);
            return arena.Owner || arena.PartyGroup;
        }

        /// Pull after pull with no owner and no group: won by lasting (PullTuning::SoloGauntlet*).
        [[nodiscard]] bool SoloGauntlet(Env const& env) const
        {
            return Gauntlet(env) && !Grouped(env);
        }

        /// A gauntlet nobody starts the pulls of (no owner to engage them): each pull walks to the seats after its
        /// arrival wait.
        [[nodiscard]] bool PullsArrive(Env const& env) const
        {
            return Gauntlet(env) && !_scenario.Arena(env).Owner;
        }

        /// A known run of pulls in a fixed order (PullSchedule::Sequence): the same fights, in the same order, every
        /// episode, so what is left to learn is the plan -- what to spend early, what to save for the end.
        [[nodiscard]] bool Sequence(Env const& env) const
        {
            return _scenario.Arena(env).Schedule == PullSchedule::Sequence;
        }

        /// A solo gauntlet's per-decision terms: its survival counted as the kill, stall, spacing and control.
        void GauntletAloneTerms(Env& env, SeatState& seat, SeatPull& pull, Player* bot, RewardLedger& ledger);
        /// An owner arena's: its win counted as the kill, and control.
        void GauntletOwnerTerms(Env& env, SeatState& seat, SeatPull& pull, Player* bot, RewardLedger& ledger);
        /// Crowd control that keeps pack members other than the seat's target out of an engaged pull, while another
        /// member is alive: `perSecond` per enemy-second, up to `perPull` a pull.
        void ControlTerm(Env& env, SeatState const& seat, SeatPull& pull, float perSecond, float perPull,
            RewardLedger& ledger);
        /// The stall grace earned by preparing for the current pull (SeatPull::PreparationBaseMs).
        [[nodiscard]] static uint32 PreparationRefundMs(CurriculumTuning::PullTuning const& tuning,
            CombatTally const& tally, SeatPull const& pull);
        /// A single pack's control, priced as the damage it prevents rather than as time held. Tracks what each enemy
        /// slot deals while it is free to act, credits every held add its own rate over the decision, and pays
        /// Pulls.SinglePackControl times that -- in maximum healths, over health now, so control is worth more the
        /// less health there is to lose. Also accumulates the wall time held, which extends the overtime grace.
        void ControlPreventedTerm(Env& env, SeatState const& seat, SeatPull& pull, Player const* bot,
            AgentStats const& step, RewardLedger& ledger);
        /// A solo gauntlet's pull nobody engaged in time walks over to the seat.
        void SendPull(Env& env);
        /// Food and drink stocked, each: Pulls.GauntletSupplies alone, else CONSUMABLE_COUNT.
        [[nodiscard]] uint32 Supplies(Env const& env) const;
        /// Eating and drinking this decision: time spent resting, meals ended with something left to restore.
        static void TrackRest(uint32 decisionMs, SeatPull& pull, Player const* bot);
        /// Whether the env's episode is one pack on its own (no owner): won on the clear, lost on a death or the clock.
        [[nodiscard]] bool SinglePack(Env const& env) const;
        /// Whether any arena of the stage is: its supplies, episode info columns.
        [[nodiscard]] bool AnyGauntlet() const;
        /// The single pack's top rung: Pulls.MaxTier, no higher than the ladder has.
        /// The top rung this env's arena may draw: its pin when it has one, else Pulls.MaxTier.
        [[nodiscard]] uint32 MaxRung(Env const& env) const;
        bool SpawnPull(Env& env, Map* map);
        /// A camp of `packs` packs of `size`, `elites` of them with an elite, `levels` above the seat, each `spacing`
        /// yards or more on from the one before (PullSchedule::Camp); `linked`, the second and third are one pull;
        /// `patrol`, a pair more walks from the first to the last and back.
        bool SpawnCamp(Env& env, Map* map, uint32 packs, uint32 size, uint32 elites, uint32 levels, float spacing,
            bool linked, bool patrol);
        /// A pull is up: its clock, its arrival, and the seats' per-pull tallies start again.
        void StartPull(Env& env);
        /// The seat drawn as the party's tank (StageState's DungeonRole) while it is up; nullptr without one.
        [[nodiscard]] Player* PartyTank(Env const& env) const;
        /// The field is empty: schedule the next pull and restart the seats' target selection.
        void EndPull(Env& env, EnvPulls& pulls);
        void Recover(Env& env);

        std::vector<EnvPulls> _envs;
        DifficultyLadder _ladder;
        DifficultyLadder _campLadder;       // a camp's rungs are a different ladder from a pack's
        DifficultyLadder _groupLadder;      // ... and a party's run another
    };

    /// A player of a random class and role near the seats' level, whom the seats fight for (companion and party
    /// stages). It is the env's ally 0. Scripted (ScriptedPlayer::UpdateMember), or in a cast-owner arena a seat
    /// of its own in the scenario's owner slot, played through its row by the learner's frozen checkpoint.
    class OwnerEncounter final : public Encounter
    {
    public:
        OwnerEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] Player* Find(Env const& env) const;
        /// Whether this episode's owner is played through its row rather than by the script.
        [[nodiscard]] bool IsCast(Env const& env) const { return _envs[env.Index].Cast; }

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        void OnRecovered(Env& env, int32 who) override;
        void OnPullStarting(Env& env) override;
        void Deactivate(Env& env) override;
        void Teardown(Env& env) override;

    private:
        /// The owner as a seat in the scenario's owner slot (ArenaDefinition::OwnerCast).
        bool BuildCast(Env& env, Map* map, uint8 level);

        struct SeatOwner
        {
            uint64 Healing = 0;                 // effective healing the seat did on the owner
            uint64 ThreatOnBot = 0;             // enemy-decisions spent attacking the seat
            bool DeathSeen = false;             // the seat has paid for the owner's current death
        };

        struct EnvOwner
        {
            BotSlot Bot;                        // the scripted owner's character (a cast owner's is its seat's)
            bool Cast = false;                  // this episode's owner is a seat in the scenario's owner slot
            uint8 Class = 0;
            Aptitude Apt;
            ScriptedPlayer::State Script;
            bool Died = false;
            uint32 Deaths = 0;
            bool DeathCounted = false;
            uint64 DamageTaken = 0;
            uint64 ThreatOnOwner = 0;
            uint32 StepEnemiesOnOwner = 0;      // living enemies in combat attacking the owner, this decision
            std::array<SeatOwner, MAX_SEATS> Seats;
        };

        std::vector<EnvOwner> _envs;
    };

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
        void OnRecovered(Env& env, int32 who) override;
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
        /// The first seat of the group `seat` is in: a party is one group, a raid is RAID_GROUPS of them.
        [[nodiscard]] static uint32 GroupFirstSeat(uint32 seat) { return seat / GROUP_SEATS * GROUP_SEATS; }
        void Disband(Env& env);
        /// In the spec's tanking stance, form or aura: Defensive Stance, Bear or Dire Bear Form, Righteous Fury, Frost
        /// Presence.
        [[nodiscard]] static bool InTankingStance(Player const* bot);
        /// What a healer's keeping-up pay is worth now: in full under a Protect goal (or with no goal), else
        /// Party.HealOffGoal of it.
        [[nodiscard]] float HealShare(SeatState const& seat) const;
        /// Each role paid for its own part, and idling charged (Raid.*).
        void RewardRole(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger, bool raid);

        std::vector<EnvParty> _envs;
    };

    /// A real dungeon or raid boss in its own instance (Opposition::Instance, ArenaDefinition::Instance): the
    /// rung is a row of InstanceBosses, which fixes the map, the level and the difficulty; the seats spawn at the
    /// instance's front door and are taken to the boss along the server's own path; the boss fights with the core's
    /// script. Won when the boss dies, lost when every seat is dead, when the script evades, or on the clock. The
    /// reward is CombatReward::OneOnOne against the boss for every seat, plus progress credit on a lost fight.
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

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
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
            uint32 DeadSinceMs = 0;             // out of a fight and dead since (Instance.WingRiseMs); 0: not
            uint32 Walk = 0;                    // the route point the seat walks to next; back to 0 at the door
            uint32 EngagesPaid = 0;             // EnvInstance::ReadyEngages paid for (the tank)
            mutable uint32 DenseAt = 0;         // the yard of the field route it was nearest at its last view
            /// Off the route out of a fight -- fallen into a cavern, kited away -- its own field way back to it,
            /// planned at DetourMs and again every DETOUR_REPLAN_MS.
            mutable std::vector<Position> Detour;
            mutable uint32 DetourMs = 0;
            /// Taken off the route (past 6 yards from it) and not yet back on it (within 4): the way back is the
            /// detour until then, so a seat at the edge does not swap the two every decision (movement-smooth A8).
            mutable bool OffRoute = false;
            uint32 FoodItem = 0;                // what it eats and drinks between pulls (Instance.WingSupplies)
            uint32 DrinkItem = 0;
        };

        struct EnvInstance
        {
            BossRow const* Row = nullptr;
            uint32 MapId = 0;
            uint32 Entry = 0;
            uint32 Tier = 0;
            bool Counts = false;
            uint16 Layout = 0;
            uint8 Spec = 0;
            ObjectGuid Boss;
            uint32 BossHealth = 1;
            float HealthLeft = 1.0f;
            bool Engaged = false;
            uint32 EngageMs = 0;
            bool BossDead = false;
            bool Wiped = false;
            bool Evaded = false;
            bool Recorded = false;
            bool Announced = false;             // the owner was told a pull is starting (OnPullStarting)
            uint32 TrashCleared = 0;
            /// A whole wing (InstanceLadder::Wing): the route from the door to the boss and the next point on it, the
            /// trash killed, the wipes, and the creatures watched for dying.
            std::vector<Position> Route;
            std::vector<float> RouteRemain;     // per route point: yards along the route from it to the end
            /// The route a yard at a time, as the layered field walks it (FieldRoute), and the yard of each route
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
            /// The run's rung of the support ladder and the wipes it stands up at the door, fixed when the
            /// run is drawn; and whether it is an evaluation's, which the running route share leaves out.
            uint32 Rung = 0;                    // the ladder's rung the run was drawn on (StageScenario::WING_RUNGS)
            bool Probe = false;                 // no script, no hints: a measure of the policy (Instance.WingProbe)
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
            bool Scripted = false;              // a seat of this run was played by the script (WingScript)
            uint32 LastMs = 0;                  // the run's clock at its last update, and its level, for its log line
            uint32 Level = 0;
            uint32 StuckLoggedMs = 0;           // when the next "Wing stuck" line may be written
            uint32 Rises = 0;                   // seats that rose at the door and ran back (WingRiseMs)
            /// The crowd past the pack's slots (CrowdBlock): on the tank, elites, the tank itself, the next enemies,
            /// and the nearest pack not in the fight.
            uint32 OnTank = 0;
            uint32 Elites = 0;
            ObjectGuid Tank;
            std::vector<ObjectGuid> Overflow;
            std::vector<ObjectGuid> Objects;    // what the party can use near it (CrowdBlock::ACTION_USE_OBJECT)
            /// Every closed door near the party, locked or not, as discs: an advance's run ends at one (A8).
            std::vector<RouteShortcut::Door> ClosedDoors;
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

        [[nodiscard]] bool Wing(Env const& env) const;
        /// A creature a full clear kills (Instance.WingFullClear): alive, hostile to the party, not a critter, a
        /// civilian, a totem, a pet or a summon, and attackable.
        [[nodiscard]] static bool Hostile(Player const* seat, Creature const* creature);
        /// A lever, a button, a goober (the Deadmines' cannon) or a closed door, spawned, ready and not locked.
        [[nodiscard]] static bool Usable(GameObject const* object);
        /// A whole dungeon's way through: its route points every Instance.WingWaypointYards (the last one the boss),
        /// the field route a yard at a time with the yard of each point, and the spawns of the creatures it can reach.
        /// A pack of the field route, in the order the route reaches it: where it is fought from, the route's yard
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
        /// The door-to-boss plan, once per boss: over the layered field where it covers the dungeon (FieldRoute),
        /// else the server's navmesh as before.
        [[nodiscard]] WingPlan WingRoute(Env const& env, Map* map, Player* seat, Creature* boss) const;
        /// An advance's run cut short at the first closed door on it (RouteShortcut::CutAtDoors, A8).
        static void CutAdvanceAtDoors(SeatView& view, EnvInstance const& fight);
        /// The plan over the layered field, from the door (`seat`) through `bosses` in order (the last one last);
        /// WingPlan::Field false when the field cannot walk that far.
        [[nodiscard]] WingPlan FieldWingRoute(Env const& env, Map* map, Player* seat, Creature* boss,
            std::vector<Position> const& bosses) const;
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
        void NoteDrill(uint32 rung, bool clean);
        /// The level range a dungeon is run at: the dungeon finder's target range for its map and difficulty.
        [[nodiscard]] static std::pair<uint32, uint32> DungeonLevels(BossRow const& row);
        void RewardWing(Env& env, uint32 seat, Player* bot, RewardLedger& ledger);
        /// Instance.WingTrace: follow the fight under way, and log what a wipe ended.
        void TraceWing(Env& env, EnvInstance& fight, bool fighting);
        void LogWipe(Env const& env, EnvInstance const& fight) const;

        [[nodiscard]] std::vector<BossRow const*> const& Rows(Env const& env) const;
        [[nodiscard]] static CreatureData const* FindSpawn(BossRow const& row);
        [[nodiscard]] Creature* FindBoss(Map* map, BossRow const& row, WorldObject const* anchor) const;
        [[nodiscard]] Position EngagePoint(Env const& env, Map* map, Player* seat, Creature* boss) const;
        [[nodiscard]] float TierScale(Env const& env) const;
        [[nodiscard]] static bool TimeIsUp(Env const& env);

        std::vector<EnvInstance> _envs;
        std::map<InstanceLadder, std::vector<BossRow const*>> _rows;   // per ladder, the rows the database fields
        DifficultyLadder _ladder;
        /// The pull drill's ladder (Instance.PullRung*): the rung and the newest rung's drills, clean or not.
        std::mutex _drillLock;
        uint32 _drillRung = 0;
        std::deque<bool> _drillRuns;
    };

    /// An enemy player: one played by a script (Opposition::ScriptedPlayer), or the other seat (self-play,
    /// Opposition::MirrorSeat), as the env's arena says. Reward: CombatReward::OneOnOne against it.
    class OpponentEncounter final : public Encounter
    {
    public:
        OpponentEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Deactivate(Env& env) override;
        void Teardown(Env& env) override;

    private:
        struct EnvOpponent
        {
            BotSlot Bot;
            uint8 Class = 0;
            Aptitude Apt;
            ScriptedPlayer::State Script;
            /// Per seat: a casting opponent the seat just cast an interrupt at, how many it has landed, and how long
            /// the opponent has been held out of the fight. A scripted player casts and heals -- 3 interruptible
            /// casts an episode in stage14_pvp -- so stopping one matters at least as much as it does against a
            /// creature, and until now none of it was paid or even counted here.
            std::array<ObjectGuid, MAX_SEATS> PendingInterrupt{};
            std::array<uint32, MAX_SEATS> Interrupts{};
            std::array<uint32, MAX_SEATS> ControlMs{};
        };

        /// Whether the seats fight each other rather than a scripted player: one a side in a Mirror arena,
        /// TeamSeats of them a side in a Teams arena. A Teams arena read as anything else spawns a scripted
        /// opponent and points every seat at it, which is a gang-up, not a match.
        [[nodiscard]] bool Mirror(Env const& env) const
        {
            SeatPlan const seats = _scenario.Arena(env).Seats;
            return seats == SeatPlan::Mirror || seats == SeatPlan::Teams;
        }
        [[nodiscard]] bool Flag(Env const& env) const
        {
            return _scenario.Arena(env).Against == Opposition::Flag;
        }
        void TrackInterrupt(Env& env, uint32 seat, Unit const* opponent, RewardLedger& ledger);
        /// Count time out of the hunter's sight, and pay for the moment contact breaks.
        void TrackHiding(Env& env, uint32 seat, Player* bot, Player const* hunter, RewardLedger& ledger);
        void TrackStalking(Env& env, uint32 seat, Player* bot, Player const* quarry, bool seen,
            RewardLedger& ledger);
        [[nodiscard]] Player* Find(Env const& env, uint32 seat) const;
        /// The seats of the side `seat` fights, in that side's own seat order, capped at the slots a seat can
        /// observe. The order has to be stable across a match: target selection indexes it.
        uint32 EnemySeats(Env const& env, uint32 seat, std::array<uint32, PACK_SLOTS>& out) const;
        bool RebuildScripted(Env& env, Player* bot, Map* map);

        std::vector<EnvOpponent> _envs;
    };

    /// Scripted enemy players who ambush the owner (ArenaDefinition::Ambushers): beside pulls they arrive at a random
    /// time and take enemy slots the pulls leave free; against Opposition::Ambush one of them is the whole fight from
    /// the start. They attack the owner while it lives, then the nearest seat. The pvp block sees the first living one.
    /// Reward: beside pulls, every seat is paid per ambusher killed (the pulls and the owner pay the rest); alone,
    /// CombatReward::OneOnOne against it.
    class AmbushEncounter final : public Encounter
    {
    public:
        AmbushEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeRebuild(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Deactivate(Env& env) override;
        void Teardown(Env& env) override;

    private:
        struct Ambusher
        {
            BotSlot Bot;
            uint8 Class = 0;
            Aptitude Apt;
            ScriptedPlayer::State Script;
            bool KillCounted = false;
        };

        struct EnvAmbush
        {
            std::array<Ambusher, MAX_AMBUSHERS> Ambushers;
            uint32 Count = 0;                   // ambushers this episode
            uint32 ArriveMs = 0;                // episode time they arrive (beside pulls)
            bool Arrived = false;
            uint32 Killed = 0;
            uint32 StepKills = 0;               // killed since the last decision
        };

        /// Whether the env's episode is the ambush alone (no pulls).
        [[nodiscard]] bool Alone(Env const& env) const { return _scenario.Arena(env).Against == Opposition::Ambush; }
        [[nodiscard]] Player* Find(Env const& env, uint32 ambusher) const;
        /// The first living ambusher that arrived, or null.
        [[nodiscard]] Player* FirstAlive(Env const& env, uint32* index = nullptr) const;
        bool Arrive(Env& env, Map* map);
        void RemoveBots(Env& env);

        std::vector<EnvAmbush> _envs;
    };

    /// A place to get to: on the ground a reachable spot 60-320 yd away by path, in a flying arena a spot 350-700 yd
    /// away. Reward: potential shaping on the distance left, arriving (on the ground; faster pays more), damage taken
    /// (falls), death. The episode ends on arriving or dying.
    /// What TravelEncounter::FindPlace is asked for beyond the distance. At namespace scope rather than nested,
    /// because a nested struct's default member initialisers are not usable in the enclosing class's default
    /// arguments until the enclosing class is complete.
    struct TravelPlaceRules
    {
        /// The detour band the trip should fall in, by the walking path over the straight line: -1 for any, 0
        /// under DetourEasy, 1 from DetourEasy to DetourHard, 2 from DetourHard up to the generator's ceiling.
        /// Insisted on for the first half of the attempts, then let go, so an arena whose ground offers no long
        /// way round still builds an episode.
        int32 Band = -1;
        float DetourEasy = 1.15f;
        float DetourHard = 1.4f;
        /// The place must be reachable by air only: no complete ground route, or one longer than AirDetour times
        /// the straight line (ArenaDefinition::AirOnly).
        bool AirOnly = false;
        float AirDetour = 2.5f;
        /// The place must be below a ledge (ArenaDefinition::Ledges): DropMin to DropMax yards under the seat,
        /// its ground route round complete but at least LedgeDetour times the straight line, and the straight
        /// line crossing one edge the seat can drop off (TravelEncounter::LedgeOnLine).
        bool Ledge = false;
        float LedgeDetour = 2.0f;
        float DropMin = 5.0f;
        float DropMax = 80.0f;
        /// The place must be on a lakebed under DepthMin to DepthMax yards of water (ArenaDefinition::Underwater).
        bool Underwater = false;
        float DepthMin = 6.0f;
        float DepthMax = 40.0f;
        /// Judge this one place rather than search for one (a human trip's end): a single try at At, probed from its
        /// own height, under every other rule above. The band is not insisted on.
        bool Fixed = false;
        Position At;
        /// Only bearings within ArcHalf radians either side of ArcCentre (world yaw) are drawn; pi or more is the whole
        /// circle, which is every arena but the markers' (whose ladder widens the arc from ahead to behind).
        float ArcCentre = 0.0f;
        float ArcHalf = float(M_PI);
        /// The ceiling on the walking way over the straight line, when it should be lower than the generator's own
        /// (0 keeps it): a marker in the open is one the straight line reaches.
        float MaxDetour = 0.0f;
        /// The floor on it (0 none), insisted on for the first half of the attempts and let go after, as Band is: a
        /// marker with something in the way (the ground stage's detour ladder).
        float MinDetour = 0.0f;
        /// The walking way may not swim: planned on NAV_GROUND alone (a movement stage before the water one).
        bool DryOnly = false;
    };

    class TravelEncounter final : public Encounter
    {
    public:
        TravelEncounter(StageScenario& scenario, uint32 envs, StageSettings const& settings);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        /// A training reset may start at a human trip's start or a human hard spot (AnimusForge.Human.*): the seat's
        /// spawn is set here, before the seats are placed, and Build validates what it was given.
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

        /// A place `nearest`-`furthest` yd from `bot` on ground that is not water; on foot (`flying` false) one it can
        /// walk to by a path not much longer than the straight line. False if none was found. `walk`, when given,
        /// takes the length of that path -- the straight line when there is none (a flying arena).
        /// A place `nearest` to `furthest` away that the seat can get to and stand on. `across` inverts the detour
        /// test for a water arena: instead of refusing an objective whose path is much longer than the straight
        /// line, it insists on one, and checks that what lies between is water rather than a cliff. `across` also
        /// requires a dry way round to exist at all -- water is only worth getting into when there is a choice --
        /// and reports its length in `dry`, which is what the way round costs on foot.
        ///
        /// `budgetSeconds` is what makes the whole thing honest: a place is only accepted if the path to it can
        /// be covered in that long at the speed this character actually has. Reachable and reachable-in-time are
        /// different claims, and only the first was ever checked. 0 means no budget, for a placement that is a
        /// feature of the arena rather than a trip against a clock.
        ///
        /// `shortcut`, if given, reports that the accepted place's path was not a path. PathGenerator answers
        /// PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH on a missing tile, a hole in the mesh, or a start too far
        /// from it -- and what it returns then is BuildShortcut's two-point straight line, whose length is the
        /// distance as the crow flies. Tested for PATHFIND_NORMAL alone, as this function has always tested it,
        /// that reads as a clean route with a detour of exactly 1.0, and the feasibility budget it is measured
        /// against means nothing. Opponents::Walkable has always checked the flag; here it was missed.
        ///
        /// `rules` says what kind of trip is wanted beyond its length: the detour band it should fall in, and
        /// whether it must be reachable by air alone (TravelPlaceRules).
        static bool FindPlace(Player* bot, Map* map, float nearest, float furthest, bool flying, Position& place,
            float budgetSeconds, float* walk = nullptr, bool across = false, float* dry = nullptr,
            bool indoors = false, bool* shortcut = nullptr, TravelPlaceRules const& rules = TravelPlaceRules(),
            float* ledgeDrop = nullptr, float* diveDepth = nullptr);
        /// Whether the straight line from `bot` to (x, y) passes through water.
        static bool CrossesWater(Player const* bot, Map* map, Position const& place, float x, float y);
        /// Whether the straight line from `bot` to the place crosses one edge the seat can drop off -- the
        /// approach walkable, then a fall of at most `rules.DropMax` onto ground the way on from which reaches
        /// the place at an ordinary detour. `drop` is the height of that edge.
        static bool LedgeOnLine(Player const* bot, Map* map, float x, float y, float z, TravelPlaceRules const& rules,
            float& drop);

    private:
        struct EnvTravel
        {
            bool Indoors = false;           // the arena is inside a building: placement and arrival both change
            bool AirOnly = false;           // the arena is air-only: placement, the ground mount and arrival change
            bool Ledge = false;             // the objective is below a ledge on the straight line (a ledge arena that found one)
            float LedgeDrop = 0.0f;         // and how high that edge is
            bool Dive = false;              // the objective is on a lakebed (a dive arena that found one)
            float DiveDepth = 0.0f;         // and how much water stands over it
            bool Chain = false;             // the objective is a chain (ArenaDefinition::Checkpoints)
            uint32 Checkpoints = 0;         // objectives reached so far; ArriveMs is the first of them
            bool ChainBroken = false;       // a next leg was wanted and none could be placed: nothing more to reach
            int32 Band = -1;                // the detour band the trip was drawn for (TravelPlaceRules::Band); -1 none
            bool Crossing = false;          // the objective was placed across water (a water arena that found one)
            float DryDistance = 0.0f;       // yards of the way round on foot, water excluded; 0 = no dry route
            bool HasObjective = false;
            Position Objective;
            float StartDistance = 0.0f;         // yards on the ground at the start
            float WalkDistance = 0.0f;          // yards of path to the objective: what covering it on foot costs
            /// How much of the episode's clock the trip needs at this character's own speed -- path length over
            /// speed over episode length. The feasibility cap in FindPlace is a ceiling on this, and reporting it
            /// is how the distribution under that ceiling stays visible rather than assumed.
            float TripShare = 0.0f;
            float LastDistance = -1.0f;         // shaping: yards at the last reward; < 0 = none yet
            /// The closest the seat ever got to the objective this episode, and how far into the clock that was.
            ///
            /// objective_distance_at_end says where a failure stopped, which turns out to say very little. A seat
            /// that touched seven yards forty seconds in and then wandered off has a steering fault; one that
            /// never closed past fifty has a routing fault, or was sent somewhere it cannot reach. Those are
            /// different bugs with different fixes and the end distance cannot tell them apart -- both of them
            /// finish sixty yards out.
            /// The way to the objective, and when it was last planned.
            ///
            /// Held per env rather than per seat because the objective is the env's: travel is one trip that
            /// one seat makes. It is what Progress is shaped on -- the distance along this, not the distance
            /// through the hillside between here and there.
            Route Way;
            uint32 WayMs = 0;
            bool WayFailed = false;         // a route was wanted and none was found
            /// The trip was measured against a straight line rather than a route -- see FindPlace. Reported and
            /// not yet acted on: the first question is how many episodes this is.
            bool Shortcut = false;
            bool DryShortcut = false;
            float Nearest = -1.0f;
            uint32 NearestMs = 0;
            float NearestX = 0.0f;              // and where the seat was standing when it was that close
            float NearestY = 0.0f;
            /// Yards the seat has actually covered, summed decision by decision -- as against WalkDistance, which
            /// is the length of the path it was *given* and says nothing about whether the legs turned. A seat
            /// that times out 96 yards short of a 106 yard trip either never moved or moved in circles, and only
            /// these two numbers together tell those apart.
            float Travelled = 0.0f;
            float LastX = 0.0f;                 // where it was at the last reward, for the sum above
            float LastY = 0.0f;
            bool HasLastPos = false;            // ... or nothing yet, so the first decision adds no jump
            /// The last second, for OBS_CLOSE_RATE toward the objective: when the mark was set, how far from the
            /// objective the seat was then, and the rate. OBS_MOVE_RATE is the scenario's now
            /// (StageScenario::TrackMotion), measured for every seat in every arena.
            uint32 MarkMs = 0;
            float MarkDistance = -1.0f;
            float CloseRate = 0.0f;
            /// The longest stretch without gaining on the objective, and how many such stretches there were
            /// (stall_seconds, stalls). A stall is not a stop: a seat pacing a bank at full speed gains nothing
            /// and is stalled, and a seat waiting out a mount cast gains nothing for two seconds and is not.
            float StallBest = -1.0f;            // the least route distance yet; < 0 = none yet
            uint32 StallSinceMs = 0;            // when it last improved
            uint32 StallLongestMs = 0;
            uint32 Stalls = 0;
            bool Stalling = false;              // the current stretch has been counted
            bool Arrived = false;
            uint32 ArriveMs = 0;
            uint32 MountedMs = 0;               // episode time spent mounted
            uint32 FlyingMountMs = 0;           // ... of it on a flying mount, in the air or not
            uint32 FlyingMs = 0;                // ... on a flying mount in the air
            bool KnowsFlyer = false;
            /// Why the flying mount was refused at the start of the episode, as a SpellCastResult.
            ///
            /// could_mount_flying was reported for the whole life of the flight stage while CouldMountFlyer was
            /// never once assigned, so the column read false whatever happened. stage7_flight then ran its full
            /// thirty million steps with flew at exactly 0.0000 -- every character level 67 and knowing a flying
            /// mount -- and the one number that would have said so was a constant.
            uint32 FlyerRefusal = 0;
            /// The zone and area the seat believed it was in when that was asked. SpellInfo::CheckLocation
            /// tests the player's own cached ids, not its coordinates, so a stale or unresolved zone refuses a
            /// flying mount in the middle of Outland.
            uint32 FlyerZone = 0;
            uint32 FlyerArea = 0;            // the seat knows a flying mount spell at all
            bool CouldMountFlyer = false;       // ... and the mask would have offered it at the start
            bool Flew = false;                  // the seat rode a flying mount at some point this episode
            float FlightSpeedSeen = 0.0f;       // the fastest MOVE_FLIGHT speed it had while on one
            double HeightSum = 0.0;             // height above ground while on one, summed over the samples
            uint32 HeightSamples = 0;
            // Ground covered while aloft, against the time it took: what the spline actually flies at, which no
            // metric taken from GetSpeed() can answer and no policy can confound.
            Position LastPos;
            bool LastAloft = false;
            double FlightDistance = 0.0;
            uint32 FlightMs = 0;
            float FlightPeakYps = 0.0f;         // the fastest single step while aloft: what the spline can do
            uint32 AloftSteps = 0;              // decisions aloft, and how many of them had the FLYING flag set
            uint32 AloftFlagged = 0;
            uint32 LastRewardMs = 0;
            /// Human play (BeforeLevel): the pool trip this reset was handed (an index into its arena's pool, -1
            /// none), or that it starts at a hard spot -- and, once Build has validated them, whether the episode
            /// actually got its trip from the pool (human_trip) or its start from a spot (human_hard_start).
            int32 HumanTrip = -1;
            bool HumanHard = false;
            bool FromHumanTrip = false;
            bool FromHardStart = false;
        };

        /// The trip distance band an arena draws its objectives in, yards on the ground.
        static void TripBand(ArenaDefinition const& arena, CurriculumTuning::TravelTuning const& tuning, float& least,
            float& most);
        /// Whether a seat may stand at (x, y, z) to start an episode of `arena`: on ground (or, where the arena is
        /// wet, in water) that the probe finds within a step of z, on the navmesh, dry where the arena is dry and
        /// inside a building where the arena is indoors. `z` is moved onto the ground found.
        static bool StandsAt(Map* map, uint32 phaseMask, float x, float y, float& z, ArenaDefinition const& arena);
        /// Load the human pools for every travel arena the stage trains (constructor): parse, filter, check each
        /// start stands, create the grids they reach, and log what was kept.
        void LoadHumanPools(StageSettings const& settings);
        /// A human start that did not hold up: the seats go back to the episode's own spawn point, as if it had never
        /// been handed one.
        void AbandonHumanStart(Env& env, Map* map, EnvTravel& travel);

        /// Re-plan the way to the objective if it has gone stale, and say whether it was re-planned.
        ///
        /// Movement first, then the clock, for the reason the ground probe gives: at seven yards a second a
        /// timer alone goes stale inside the first corner. A route that cannot be found leaves WayFailed set
        /// and the straight line standing, which is worse shaping but not no shaping.
        bool RefreshWay(EnvTravel& travel, Player* bot, float stray, float refreshSeconds, float corner,
            uint32 nowMs);
        /// A chain arena's next objective (ArenaDefinition::Checkpoints), drawn from where the seat stands now
        /// and within what is left of the clock; false when none can be placed.
        bool NextLeg(Env const& env, EnvTravel& travel, Player* bot) const;

        /// Yards to the objective along the way there, or the straight line where there is no way.
        static float WayDistance(EnvTravel const& travel, Player const* bot);

        /// How much of the walk the trip saved, 0 (no faster than walking, or slower) to 1. Mounting is worth what
        /// it saves: nothing over a hop too short to pay for the cast, most of it over a long haul.
        [[nodiscard]] static float Saved(EnvTravel const& travel);

        std::vector<EnvTravel> _envs;

    public:
        /// The shaping route's plans since the start (RefreshWay): count, time, and how many failed or fell short.
        static inline std::atomic<uint64> WayPlans{ 0 };
        static inline std::atomic<uint64> WayPlanNs{ 0 };
        static inline std::atomic<uint64> WayPlansFailed{ 0 };
        static inline std::atomic<uint64> WayPlansPartial{ 0 };
        /// Objective searches (FindPlace): how many, their tries, the paths they planned, their time, and how many
        /// found nothing.
        static inline std::atomic<uint64> PlaceSearches{ 0 };
        static inline std::atomic<uint64> PlaceAttempts{ 0 };
        static inline std::atomic<uint64> PlacePaths{ 0 };
        static inline std::atomic<uint64> PlaceNs{ 0 };
        static inline std::atomic<uint64> PlaceFailed{ 0 };
        /// Human play: trips and hard starts handed to resets (BeforeLevel), those that held up, and those that fell
        /// back to the arena's own draw.
        static inline std::atomic<uint64> HumanTripsDrawn{ 0 };
        static inline std::atomic<uint64> HumanTripsUsed{ 0 };
        static inline std::atomic<uint64> HumanTripsFellBack{ 0 };
        static inline std::atomic<uint64> HardStartsDrawn{ 0 };
        static inline std::atomic<uint64> HardStartsUsed{ 0 };
        static inline std::atomic<uint64> HardStartsFellBack{ 0 };

    private:

        /// Trips found before, per (arena, spawn point, detour band), for training resets to draw from instead of
        /// searching again (Build): the search plans a route per attempt and was ~1.2 ms of every stage1_move reset.
        struct PooledTrip
        {
            Position Objective;
            float Walk = 0.0f;
            float DryDistance = 0.0f;
            float LedgeDrop = 0.0f;
            float DiveDepth = 0.0f;
            int32 Band = -1;
            bool Shortcut = false;
            bool DryShortcut = false;
            bool Crossing = false;
            bool AirOnly = false;
            bool Ledge = false;
            bool Dive = false;
        };
        bool _pooling = false;          // AnimusForge.TravelPools

        /// Human play, per arena of the stage (by index; empty for one that takes none): the trips and the hard spots
        /// it may use, filtered once at construction, and the spots' cumulative weights.
        struct HumanArena
        {
            std::vector<HumanPools::Trip> Trips;
            std::vector<HumanPools::HardSpot> Spots;
            std::vector<uint64> SpotWeights;
        };
        std::vector<HumanArena> _human;
        float _humanTripShare = 0.0f;   // AnimusForge.Human.TripShare
        float _hardSpotShare = 0.0f;    // AnimusForge.Human.HardSpotShare
        std::mutex _poolLock;           // resets run on the map threads
        std::unordered_map<uint64, std::vector<PooledTrip>> _pools;
    };

    /// The side's director: what the team holds to, who it concentrates on, what shape it takes, and whose turn
    /// the next duty is. Written into every seat's SeatView::TeamOrder, read by the order block, and binding on
    /// nobody -- an order is advice.
    ///
    /// Scripted for now, and deliberately legible: the lowest enemy is the focus, the duty goes round the side in
    /// turn, and the rally follows the objective. A seat learns that following a sensible order pays before a
    /// learned director has to discover what a sensible order is -- the same order the curriculum already uses
    /// when a scripted enemy player comes before a learned one.
    class DirectorEncounter final : public Encounter
    {
    public:
        DirectorEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;

        /// What compliance shaping this seat was paid this decision (RewardTerm::OrderMatch). The director's own
        /// reward takes it back out: it is paid the mean of its side's rewards, and a director that could earn
        /// from the shaping would learn to call whoever its seats were already fighting -- to look busy rather
        /// than to lead.
        [[nodiscard]] float ShapingPaid(Env const& env, uint32 seat) const;
        /// Shaping a seat was paid for following its order another way (Goals.Progress on a primary the order set):
        /// taken off the director's reward with the rest, so it cannot pay itself by ordering what is easy.
        void AddShaping(Env const& env, uint32 seat, float paid);
        /// The seat's primary goal as its director's standing order says it (OrderGoals.h), under a learned
        /// director; NO_GOAL when there is none (no order, a side-wide one, or one no goal says).
        [[nodiscard]] int32 MemberGoal(Env const& env, uint32 seat) const;
        /// What the side's director owes for the member orders it replaced since this was last asked
        /// (Director.OrderChange, Director.OrderChurn); taken off its reward once a decision.
        [[nodiscard]] float OrderCost(Env& env, uint32 side);

        /// What the agent commanding `side` sees. Built from the seats and the enemy side, then offered to every
        /// other active encounter (Encounter::ViewDirector) for the objective it alone knows.
        void ViewSide(Env const& env, uint32 side, DirectorLayout::DirectorView& view) const;

        /// One call from the learned director of `side`: the action names the single field of the standing order
        /// it changes, and everything else keeps what it was. An action out of range, or one naming a slot that
        /// is not there, changes nothing -- a masked action may still arrive -- and so does any action off the
        /// director's turn (PrepareTurn).
        void Call(Env& env, uint32 side, int32 action);

        /// Before the director's observation: whether it gets a turn now -- on its clock (Director.ClockDecisions)
        /// or on an event (a member down or newly below a quarter of its health, a new enemy, the focus dead) --
        /// with a budget of calls (four for a group, eight for a raid) it keeps until it holds or spends them.
        void PrepareTurn(Env& env, uint32 side);

    private:
        /// The standing order and the director's turn (DirectorOrders, shared with the module), and what this
        /// encounter keeps besides: the place it resolved and the measurements.
        struct SideOrder : DirectorOrders
        {
            /// Replaced and Churned already charged (OrderCost).
            uint32 ReplacedPaid = 0;
            uint32 ChurnedPaid = 0;
            /// Where the side was sent, rebuilt every decision from the anchor, offset and ring so a place hung on
            /// the focus or on the side's own centre follows them as they move. HasPlace is derived, never set on
            /// its own.
            Position Place;
            bool HasPlace = false;
            /// Whether the call is worth following, which is upstream of whether it is followed: decisions with
            /// a living enemy to call, those whose call was one, those whose call was the most hurt of them,
            /// and what picking at random among the living would have scored.
            uint32 Decisions = 0;
            uint32 FocusAlive = 0;
            uint32 FocusLowest = 0;
            float ChanceSum = 0.0f;
            /// Whether the director could see what it was calling, and how much of the enemy it could see at
            /// all. Without these a blind director and a bad one read the same: order_focus_lowest falls in
            /// both cases and nothing says which.
            uint32 FocusUnseen = 0;
            float SeenSum = 0.0f;
            /// The place: decisions one stood, seats that reached it, and how far the side was from it.
            uint32 PlaceCalled = 0;
            uint32 PlaceReached = 0;
            float PlaceDistanceSum = 0.0f;
            /// Per seat, when it was last paid for arriving, and whether it was inside the radius last
            /// decision. Arriving is paid on the crossing and only then: a seat standing in the right spot
            /// earns nothing for going on standing there, and one stepping in and out of the edge earns once.
            std::array<uint32, MAX_SEATS> PlacePaidMs{};
            std::array<uint8, MAX_SEATS> WasAtPlace{};
        };

        /// What a side remembers of one enemy slot. Keyed by slot rather than by guid: SideSeats hands back
        /// stable seat indices for the episode and ViewSide already walks them, so an array indexed the same
        /// way costs nothing and keeps an allocation off the per-decision path. The guid is kept only to
        /// notice a slot being reassigned.
        struct EnemyMemory
        {
            ObjectGuid Guid;
            Position LastSeen;
            uint32 LastSeenMs = 0;
            float Health = 0.0f;
            Aptitude Apt;
            bool Alive = false;
            bool Known = false;         // the side has seen it at least once
        };

        /// One side's picture of the enemy: what it remembers, and what it can see this decision.
        ///
        /// The seen mask is worked out once a decision in Update rather than inside ViewSide, which runs per
        /// side per observe: SideCanSee is a loop over the side's seats, so asking it per enemy slot inside
        /// the view would be forty CanSeeOrDetect calls a side a decision in a ten-a-side match, on the world
        /// thread.
        struct SideKnowledge
        {
            std::array<EnemyMemory, NAMED_ENEMY_SLOTS> Enemies{};
            std::array<uint8, NAMED_ENEMY_SLOTS> Seen{};
        };

        struct EnvDirector
        {
            std::array<SideOrder, TEAM_COUNT> Sides;
            std::array<SideKnowledge, TEAM_COUNT> Knowledge;
            uint32 Steps = 0;
            /// Per seat, the shaping paid this decision; cleared before every decision's rewards.
            std::array<float, MAX_SEATS> Shaping{};
        };

        /// One side's orders, from what its seats and the enemy's are doing. The scripted director; a learned one
        /// is told what to say instead (Call).
        void Command(Env& env, uint32 side);
        /// The enemies of `side` in slot order: the other side's seats in a match, the env's target slots (the
        /// pack, the boss) against creatures. Slots the side's seats select between, so a call means the same
        /// enemy as a seat's own choice.
        uint32 Enemies(Env const& env, uint32 side, std::array<Unit*, NAMED_ENEMY_SLOTS>& out) const;
        /// The seats of `side` a director commands, in seat order: up to a raid.
        uint32 Members(Env const& env, uint32 side, std::array<uint32, DirectorLayout::DIRECTOR_SEATS>& out) const;
        /// The member slot of `seat` on its side (the director's own slot order), or DIRECTOR_SEATS when none.
        [[nodiscard]] uint32 SlotOf(Env const& env, uint32 seat) const;
        /// Whether the env's arena has the director learn rather than follow the script.
        [[nodiscard]] bool Learned(Env const& env) const;
        /// Refresh what the side can see and what it remembers, once per decision before anything reads it.
        void Observe(Env& env, uint32 side);
        /// Rebuild SideOrder::Place from its anchor, offset and ring, and say whether it landed anywhere the
        /// side could stand.
        void ResolvePlace(Env& env, uint32 side);
        /// Pay a seat for arriving where it was sent, on the crossing and no more often than the cooldown.
        void RewardPlace(Env& env, uint32 seat, Player* bot, RewardLedger& ledger);
        /// Drop what the side is being asked for once it cannot be done: a call at a corpse is not a call.
        void Forget(Env& env, uint32 side);
        /// Tally what the side's standing call is worth this decision, scripted or learned.
        void Measure(Env& env, uint32 side);

        std::vector<EnvDirector> _envs;
    };

    /// Warsong Gulch's rules between the two mirror seats: each has a flag at its base; touching the other's takes it
    /// (and dismounts the carrier, who cannot mount while carrying), touching one's own dropped flag returns it, and
    /// carrying the other's home while one's own is there captures it. A carrier who dies drops the flag where it fell;
    /// a dropped flag goes home on its own after a while. The dead stand up at their base after a wave. First to
    /// Flag.CapturesToWin ends the match. The seat's travel objective is where its side needs it next.
    class FlagEncounter final : public Encounter
    {
    public:
        FlagEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeSeats(Env& env, uint8 level) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        /// What a seat heads for, in the order a player would pick it.
        enum class Goal : uint8 { None, CaptureHome, ReturnOwn, TakeEnemy, PickUpEnemy, ChaseCarrier };

        struct Side
        {
            Position Base;
            SeatView::FlagState State = SeatView::FlagState::AtBase;   // this side's own flag
            Position Dropped;
            uint32 DroppedMs = 0;
            uint32 Captures = 0;
            uint32 Pickups = 0;
            uint32 Returns = 0;
            uint32 CarrierKills = 0;
            uint32 Deaths = 0;
            /// The seat carrying this side's flag, NO_SEAT when nobody is. With one seat a side this was the
            /// other seat by construction; with ten it has to be said.
            uint32 CarriedBy = NO_SEAT;
            // This decision's events, paid to every seat of the side: Update clears them at its top, so all ten
            // read the same decision rather than the first to be rewarded taking them.
            uint32 StepCaptures = 0;
            uint32 StepPickups = 0;
            uint32 StepReturns = 0;
            uint32 StepCarrierKills = 0;
            uint32 StepLost = 0;
        };

        /// What belongs to a seat rather than to its side. These sat on Side while a side was one seat.
        struct SeatFlagState
        {
            bool Dead = false;
            uint32 RespawnMs = 0;
            uint32 StepDeaths = 0;              // charged to the seat that died, not to its side
            uint32 ReachSteps = 0;              // decisions with a flag close enough to use
            uint32 Steps = 0;                   // decisions, to divide it by
            float LastDistance = -1.0f;         // shaping toward the current goal; < 0 = none yet
            Goal LastGoal = Goal::None;
        };

        struct EnvFlags
        {
            Battleground* Match = nullptr;             // the scripted battleground, when the arena runs one
            std::array<Group*, TEAM_COUNT> Groups{};    // a side is a group, so its healers can reach it
            std::array<Side, TEAM_COUNT> Sides;
            std::array<SeatFlagState, TEAM_MATCH_SEATS> Seats;
            bool Built = false;
        };

        /// Which side a seat plays for: seats 0..TEAM_SEATS-1 are side 0, the rest side 1. A Mirror arena has
        /// one seat a side and lands on 0 and 1 as it always did.
        [[nodiscard]] uint32 SideOf(Env const& env, uint32 seat) const;
        /// Make each side a group, so party and raid spells reach a team-mate. Disband undoes it.
        void FormTeams(Env& env);
        void Disband(Env& env);
        /// The real Warsong Gulch for this env, made before its seats so they can be told to join it. Null for
        /// an arena that plays the flag rules itself (stage 11's one on one).
        [[nodiscard]] Battleground* Match(Env const& env) const;
        [[nodiscard]] Battleground* MatchFor(Env const& env) const override { return Match(env); }
        /// Take the match down with the episode that was it.
        void EndMatch(Env& env);
        /// The stage ending: the match and the groups go before the seats' bots are destroyed. Left standing, the
        /// battleground kept pointers to them and its next update touched a freed player (a segfault in
        /// Battleground::_ProcessJoin -> Player::ResetAllPowers once a sweep moved past stage25_warsong).
        void Teardown(Env& env) override;
        /// Read the script's score and flag state back into the side view the seats and rewards use.
        void ReadMatch(Env& env);

        /// Where seat `seat` should go now, and why.
        [[nodiscard]] Goal CurrentGoal(Env const& env, uint32 seat, Position& place) const;

        std::vector<EnvFlags> _envs;
    };
}

#endif
