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
    /// stages). It is the env's ally 0: a seat of its own in the scenario's owner slot, played through its row by the
    /// learner's frozen checkpoint (there is no scripted owner).
    class OwnerEncounter final : public Encounter
    {
    public:
        OwnerEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] Player* Find(Env const& env) const;
        /// Whether this episode's owner is built (a cast seat in the scenario's owner slot).
        [[nodiscard]] bool IsCast(Env const& env) const { return _envs[env.Index].Cast; }

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        void OnRecovered(Env& env, int32 who) override;
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
            bool Cast = false;                  // this episode's owner is a seat in the scenario's owner slot
            uint8 Class = 0;
            Aptitude Apt;
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
            mutable uint32 DenseAt = 0;         // the yard of the field route it was nearest at its last view
            /// Off the route out of a fight -- fallen into a cavern, kited away -- its own field way back to it,
            /// planned at DetourMs and again every DETOUR_REPLAN_MS.
            mutable std::vector<Position> Detour;
            mutable uint32 DetourMs = 0;
            /// Taken off the route (past 6 yards from it) and not yet back on it (within 4): the way back is the
            /// detour until then, so a seat at the edge does not swap the two every decision (movement-smooth A8).
            mutable bool OffRoute = false;
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
            /// The dungeon map's layout (SeenPlaces::Layout): ground nodes, unordered, no creature on them -- what a
            /// player's dungeon map draws; a sight stage's goal places in SeenAndLayout.
            std::vector<SeenPlaces::Point> MapLayout;
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

        [[nodiscard]] bool Wing(Env const& env) const;
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
        /// A corridor run (G2): the packs before its first cleared and the party set down short of it, as a cell's
        /// start (StartAt); its packs to clear in route order. False, nothing changed, when the route has no packs.
        bool StartCorridor(Env& env, Map* map, WingPlan const& plan, std::vector<ObjectGuid::LowType> const& counted);
        /// The route packs fighting the party this decision, for the chain pull (EnvInstance::Drawn).
        void UpdateDrawnPacks(Env& env, EnvInstance& fight);
        /// A sight stage's goal places in a dungeon (SeenPlaces): what `seat` saw and remembers, its map's frontier,
        /// the layout (SeenAndLayout), the leader -- never a pack's or boss's live position or the route's order.
        void SeenWorld(Env const& env, uint32 seat, SeatView& view) const;
        /// Whether a whole dungeon's run is a full clear: the last boss dead and every creature the clear counts.
        [[nodiscard]] static bool FullClear(EnvInstance const& fight);
        /// The run's success as the stage counts it: a drill's pack pulled and killed alone, a corridor cleared, a
        /// whole dungeon's last boss dead (the `cleared` column, the stand-in split's and the videos' outcome).
        [[nodiscard]] static bool Succeeded(EnvInstance const& fight);
        void NoteDrill(uint32 rung, bool clean);
        void RewardWing(Env& env, uint32 seat, Player* bot, RewardLedger& ledger);
        /// Instance.WingTrace: follow the fight under way, and log what a wipe ended.
        void TraceWing(Env& env, EnvInstance& fight, bool fighting);
        void LogWipe(Env const& env, EnvInstance const& fight) const;

        [[nodiscard]] std::vector<BossRow const*> const& Rows(Env const& env) const;
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
}

#endif
