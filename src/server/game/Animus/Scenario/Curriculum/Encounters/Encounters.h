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
#include "DifficultyLadder.h"
#include "InstanceBosses.h"
#include "LifeEncounter.h"
#include "Encounter.h"
#include "DirectorLayout.h"
#include "Env.h"
#include "ObjectGuid.h"
#include "RewardLedger.h"
#include "RoutePlanner.h"
#include "ScriptedPlayer.h"
#include "SeatView.h"
#include "StageDefinition.h"
#include "StageScenario.h"
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
        };

        /// Whether the env's episode is pull after pull (else a single pack).
        [[nodiscard]] bool Gauntlet(Env const& env) const
        {
            PullSchedule const schedule = _scenario.Arena(env).Schedule;
            return schedule == PullSchedule::Gauntlet || schedule == PullSchedule::Sequence;
        }

        /// Pull after pull with no owner: won by lasting (PullTuning::SoloGauntlet*).
        [[nodiscard]] bool SoloGauntlet(Env const& env) const
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
        /// The field is empty: schedule the next pull and restart the seats' target selection.
        void EndPull(Env& env, EnvPulls& pulls);
        void Recover(Env& env);

        std::vector<EnvPulls> _envs;
        DifficultyLadder _ladder;
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
        InstanceEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeLevel(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;

    private:
        struct SeatInstance
        {
            bool OutcomePaid = false;
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
            std::array<SeatInstance, MAX_SEATS> Seats;
        };

        [[nodiscard]] std::vector<BossRow const*> const& Rows(Env const& env) const;
        [[nodiscard]] static CreatureData const* FindSpawn(BossRow const& row);
        [[nodiscard]] Creature* FindBoss(Map* map, BossRow const& row, WorldObject const* anchor) const;
        [[nodiscard]] Position EngagePoint(Env const& env, Map* map, Player* seat, Creature* boss) const;
        [[nodiscard]] float TierScale(Env const& env) const;
        [[nodiscard]] static bool TimeIsUp(Env const& env);

        std::vector<EnvInstance> _envs;
        std::map<InstanceLadder, std::vector<BossRow const*>> _rows;   // per ladder, the rows the database fields
        DifficultyLadder _ladder;
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
    };

    class TravelEncounter final : public Encounter
    {
    public:
        TravelEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
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
        };

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

        /// What the agent commanding `side` sees. Built from the seats and the enemy side, then offered to every
        /// other active encounter (Encounter::ViewDirector) for the objective it alone knows.
        void ViewSide(Env const& env, uint32 side, DirectorLayout::DirectorView& view) const;

        /// One call from the learned director of `side`: the action names the single field of the standing order
        /// it changes, and everything else keeps what it was. An action out of range, or one naming a slot that
        /// is not there, changes nothing -- a masked action may still arrive.
        void Call(Env& env, uint32 side, int32 action);

    private:
        struct SideOrder
        {
            TeamPosture Posture = TeamPosture::Attack;
            TeamRally Rally = TeamRally::None;
            /// Where the side was sent, rebuilt every decision from the three fields below so a place hung
            /// on the focus or on the side's own centre follows them as they move. HasPlace is derived, never
            /// set on its own: two independent ways to say "there is a place" is how they drift apart.
            Position Place;
            bool HasPlace = false;
            PlaceAnchor Anchor = PlaceAnchor::TeamCentre;
            /// Toward, not At. At the side's own centre a place is just where the side already is, which
            /// makes Rally::Point a synonym for Rally::Stack and gives the director two actions that say the
            /// same thing -- measured on the first run with places: the side averaged 7.9 yards from the
            /// called place against a radius of 8. Toward the enemy at the near ring is a push, which nothing
            /// else in the vocabulary says.
            PlaceOffset Offset = PlaceOffset::Toward;
            PlaceRing Ring = PlaceRing::Near;
            ObjectGuid Focus;
            uint32 Duty = NO_SEAT;              // the seat that owes the next interrupt or control
            uint32 Changes = 0;                 // how often the call moved, for the episode info
            uint32 CalledStep = 0;              // the decision the order last changed on
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
            std::array<EnemyMemory, PACK_SLOTS> Enemies{};
            std::array<uint8, PACK_SLOTS> Seen{};
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
        /// Note that the order changed, for order_changes and the director's own "how long has this stood".
        void Changed(SideOrder& order, uint32 steps) const;

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
        /// Read the script's score and flag state back into the side view the seats and rewards use.
        void ReadMatch(Env& env);

        /// Where seat `seat` should go now, and why.
        [[nodiscard]] Goal CurrentGoal(Env const& env, uint32 seat, Position& place) const;

        std::vector<EnvFlags> _envs;
    };
}

#endif
