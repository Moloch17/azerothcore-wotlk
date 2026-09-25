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

#ifndef ANIMUS_LIB_CURRICULUM_STAGE_SCENARIO_H
#define ANIMUS_LIB_CURRICULUM_STAGE_SCENARIO_H

#include "CurriculumTuning.h"
#include "DirectorLayout.h"
#include "Encounter.h"
#include "EpisodeInfoTable.h"
#include "Layout.h"
#include "Position.h"
#include "Scenario.h"
#include "StageDefinition.h"
#include "StageSettings.h"
#include "StageState.h"
#include <memory>
#include <optional>

namespace Animus::Curriculum
{
    class DirectorEncounter;
    class OwnerEncounter;
    class PartyEncounter;

    /// One curriculum stage (see StageDefinition) for every class of StageSettings::Classes, as layouts of one
    /// policy.
    ///
    /// Each learned agent of an env is a seat: every episode it becomes a new character of a class -- a race the
    /// class allows, random gender, level (1-80, 55-80 for death knights), one of its builds with that build's
    /// standard talents and glyphs, the trainer spells of the level, random level-appropriate gear including
    /// trinkets, enchants and gems, and potions, bandages and stones it must learn to use. A seat's layout is its
    /// class's (see Layout), padded to the largest layout's on the wire; the learner shares one trunk between all
    /// layouts.
    ///
    /// Every episode is one of the stage's arenas (see ArenaDefinition), drawn by weight after the evaluation reseed.
    /// The scenario builds the seats and drives the encounters the arena uses (see Encounter); the critic state is
    /// class-agnostic (every seat's and enemy's essentials, the owner, the pull timing and the arena).
    class StageScenario final : public Scenario
    {
    public:
        /// Class-agnostic critic state, per seat and per enemy slot.
        enum StateGlobal : uint32
        {
            STATE_EPISODE_TIME          = 0,
            STATE_PULL_ACTIVE           = 1,
            STATE_PULLS_CLEARED         = 2,    // / 10
            STATE_NEXT_PULL             = 3,    // time until the next pull / 20 s
            STATE_ELITE_PULL            = 4,
            STATE_LINKED_PULL           = 5,
            STATE_OWNER_PRESENT         = 6,
            STATE_OWNER_ALIVE           = 7,
            STATE_OWNER_HEALTH          = 8,
            STATE_OWNER_MANA            = 9,
            STATE_OWNER_X               = 10,   // relative to the spawn point, / 40
            STATE_OWNER_Y               = 11,
            STATE_OWNER_IN_COMBAT       = 12,
            STATE_TIER                  = 13,   // the fight's difficulty tier or the pull's rung, over the top one
            STATE_ARENA_FIRST           = 14,   // one-hot: the episode's arena (MAX_ARENAS columns)
            STATE_GLOBAL_COUNT          = 14 + MAX_ARENAS
        };

        enum StateSeat : uint32
        {
            STATE_SEAT_PRESENT          = 0,
            STATE_SEAT_ALIVE            = 1,
            STATE_SEAT_HEALTH           = 2,
            STATE_SEAT_MANA             = 3,
            STATE_SEAT_OTHER_POWER      = 4,    // rage, energy or runic power as a fraction
            STATE_SEAT_LEVEL            = 5,    // / 80
            STATE_SEAT_APTITUDE_FIRST   = 6,    // the six-number brief of what its build can do
            STATE_SEAT_CLASS_FIRST      = 12,    // one-hot over PLAYABLE_CLASSES
            STATE_SEAT_IN_COMBAT        = 22,
            STATE_SEAT_CASTING          = 23,
            STATE_SEAT_X                = 24,   // relative to the spawn point, / 40
            STATE_SEAT_Y                = 25,
            STATE_SEAT_FEATURES         = 26
        };

        enum StateEnemy : uint32
        {
            STATE_ENEMY_PRESENT         = 0,
            STATE_ENEMY_ALIVE           = 1,
            STATE_ENEMY_HEALTH          = 2,
            STATE_ENEMY_X               = 3,
            STATE_ENEMY_Y               = 4,
            STATE_ENEMY_CASTING         = 5,
            STATE_ENEMY_ELITE           = 6,
            STATE_ENEMY_LEVEL_DIFF      = 7,    // (its level - seat 0's) / 5
            STATE_ENEMY_IN_COMBAT       = 8,
            STATE_ENEMY_ON_OWNER        = 9,    // its victim is the owner
            // Who it is fighting, told in a raid's terms rather than a seat-wide one-hot: at forty seats that was
            // 160 columns over four enemies, nearly all of them zero, and what a critic needs is which kind of seat
            // and which group, not which index.
            STATE_ENEMY_ON_SEAT         = 10,   // its victim is a learned seat at all
            STATE_ENEMY_SEAT_INDEX      = 11,   // ... that seat / MAX_SEATS
            STATE_ENEMY_SEAT_GROUP_FIRST = 12,  // ... one-hot over RAID_GROUPS
            STATE_ENEMY_SEAT_APTITUDE_FIRST = 12 + RAID_GROUPS, // ... the brief of what its build can do
            STATE_ENEMY_MAX_HEALTH      = 18 + RAID_GROUPS, // its max health / seat 0's / 4, clamped
            STATE_ENEMY_DAMAGE_MODIFIER = 19 + RAID_GROUPS, // / 2
            STATE_ENEMY_ARMOR           = 20 + RAID_GROUPS, // share of seat 0's physical hits its armor takes off
            STATE_ENEMY_RUN_SPEED       = 21 + RAID_GROUPS, // / 2
            STATE_ENEMY_TYPE_FIRST      = 22 + RAID_GROUPS, // one-hot over Encoding::OPPONENT_TYPES (7)
            STATE_ENEMY_FEATURES        = 29 + RAID_GROUPS
        };

        StageScenario(StageSettings const& settings, StageDefinition const& stage);
        ~StageScenario() override;

        [[nodiscard]] char const* Name() const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        /// The far side of a self-play arena: its seats, and the director commanding them.
        [[nodiscard]] bool IsOpponentSeat(Env const& env, uint32 agent) const override;
        [[nodiscard]] ScenarioSpec Spec() const override { return _spec; }

        bool Setup(Env& env) override;
        void Reset(Env& env) override;
        void ApplyActions(Env& env, int32 const* actions) override;
        void ApplyGoals(Env& env, int32 const* goals) override;
        void Observe(Env& env, float* obs, float* state, uint8* mask) override;
        void AgentLayouts(Env const& env, uint16* layout) const override;
        void AgentPresence(Env const& env, uint8* present) const override;
        void Reward(Env& env, float* reward) override;
        void EpisodeInfo(Env const& env, float* info) const override;
        [[nodiscard]] std::vector<std::string> EpisodeInfoNames() const override { return _info.Names(); }
        bool ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask, uint16 layout,
            int32& action) const override;
        void SetLayoutWeights(std::vector<float> const& weights) override;
        void Teardown(Env& env) override;

        // For the encounters.
        [[nodiscard]] StageDefinition const& Stage() const { return _stage; }
        /// The env's current episode's arena.
        [[nodiscard]] ArenaDefinition const& Arena(Env const& env) const;

        /// The arena's pinned pack rung, or -1 when the ladder is free to climb (ArenaDefinition::MaxRung).
        [[nodiscard]] int32 ArenaMaxRung(Env const& env) const;
        /// Whether the env's current episode uses `encounter`.
        [[nodiscard]] bool Uses(Env const& env, Encounter const& encounter) const;
        /// The encounters the env's current episode uses, in build order.
        [[nodiscard]] std::vector<Encounter*> const& ActiveEncounters(Env const& env) const;
        [[nodiscard]] CurriculumTuning const& Tuning() const { return _tuning; }
        /// Which side a seat plays for. A Teams arena splits its seats down the middle; anything else has one
        /// seat a side, which is what a Mirror is.
        [[nodiscard]] uint32 SideOf(Env const& env, uint32 seat) const;
        [[nodiscard]] Position const& SpawnPoint() const { return _spawnPoint; }
        /// Where the env's seats start: the stage's spawn point for the env (StageDefinition::SpawnPoints), else
        /// SpawnPoint().
        /// The ground this episode may stand on: the arena's if it has its own, else the stage's -- and the
        /// control ground instead when the episode is being scored (StageDefinition::HeldOutSpawnPoints).
        [[nodiscard]] std::vector<Position> const& SpawnGroundFor(Env const& env) const;
        [[nodiscard]] Position const& SpawnPointFor(Env const& env) const;
        /// Move each seat off the spawn point by up to ArenaDefinition::SpawnScatter yards and turn it a random
        /// way. Does nothing at all for an arena that leaves SpawnScatter at 0, which is every arena that has
        /// not asked for it. Called once the seats are in the world and again whenever a failed build moves them
        /// to another point, always before the encounters build: the objective is placed from where the seat
        /// ends up, so scattering afterwards would measure the trip from somewhere the seat is not.
        void ScatterSeats(Env const& env, Map* map) const;
        /// Whether the envs share a continent (each in its own phase) rather than each having an instance.
        [[nodiscard]] bool OnContinent() const { return _continent; }
        /// The phase of the env's seats and everything they meet on a continent.
        [[nodiscard]] static uint32 EnvPhase(Env const& env);
        [[nodiscard]] uint32 SpawnMapId() const { return _spawnMapId; }
        /// The map this episode's seats are placed on: what an encounter fixed (EnvState::EpisodeMapId), else the
        /// stage's.
        [[nodiscard]] uint32 EpisodeMapId(Env const& env) const;
        [[nodiscard]] uint32 SeatCount() const { return _seatCount; }
        /// Whether some arena of the stage plays its owner as an agent (ArenaDefinition::OwnerCast): one more row
        /// on the wire, after the seats and the directors, in every episode of the stage.
        [[nodiscard]] bool HasCastOwner() const { return _castOwner; }
        /// The owner's agent index (valid when HasCastOwner).
        [[nodiscard]] uint32 OwnerAgent() const { return _seatCount + (HasDirectors() ? TEAM_COUNT : 0); }
        /// Whether this episode's owner is played through its row: a cast-owner arena, not an evaluation, and
        /// not one of the episodes Owner.CastScriptedShare keeps scripted.
        [[nodiscard]] bool CastOwnerActive(Env const& env) const;
        /// Build the owner as a seat in the owner's agent slot: a class and build of the run meeting `demand`,
        /// at `level`, placed at `start`; null when nothing could be built. The caller sets its faction and
        /// records it as the env's ally.
        Player* BuildOwnerSeat(Env& env, Map*& map, uint8 level, Position const& start, AptitudeDemand demand);
        /// Release the owner's seat: its character goes and its slot reads empty.
        void ReleaseOwnerSeat(Env& env);
        /// Whether the run carries the two director agents at all (some arena of the stage has a learned
        /// director), and whether the env's current episode is actually using them.
        [[nodiscard]] bool HasDirectors() const { return _directorLayout != NO_LAYOUT; }
        [[nodiscard]] bool DirectorsActive(Env const& env) const;
        /// The agent index that commands `side`, or NO_SEAT when the run has no directors.
        [[nodiscard]] uint32 DirectorAgent(uint32 side) const
        {
            return HasDirectors() ? _seatCount + side : NO_SEAT;
        }
        /// The seats of `side`, in seat order, and how many there are (at most TEAM_SEATS).
        uint32 SideSeats(Env const& env, uint32 side, std::array<uint32, TEAM_SEATS>& out) const;
        /// Whether a side can see `unit` at all: any one of its living seats can.
        ///
        /// A side's knowledge is the union of its members', which is a notion the scenario did not have --
        /// ViewSeat filters what one seat sees (StageScenario.cpp, the `hidden` lambda) and nothing filtered
        /// anything per side. A director commands a side, so this is the visibility its observation is built
        /// from. Dead seats are excluded: a side that wiped should not go on spotting.
        [[nodiscard]] bool SideCanSee(Env const& env, uint32 side, Unit const* unit) const;
        /// Decision interval / 50 ms: per-decision reward terms are tuned per 50 ms and scaled by this, so they mean
        /// the same per second at any StageSettings::DecisionMs.
        [[nodiscard]] float DecisionScale() const { return _decisionScale; }
        /// The decision interval, in ms of game time.
        [[nodiscard]] uint32 DecisionMs() const { return _decisionMs; }

        /// Every class/role layout of the run, by Layout::Index (the index AgentLayouts reports).
        [[nodiscard]] std::vector<Layout> const& Layouts() const { return _layouts; }
        [[nodiscard]] bool Playable() const override { return !_layouts.empty(); }
        [[nodiscard]] uint64 CharactersReused() const override { return _reused; }

        /// How many (class, role) pairs the run can field, which is what an evaluation spreads its seeds over.
        /// The difficulty ladder divides by the same number, so every pair meets every rung.
        [[nodiscard]] uint32 CastingCount() const { return uint32(Castings(AptitudeDemand::Anything()).size()); }

        [[nodiscard]] EnvState& Data(Env const& env);
        [[nodiscard]] EnvState const& Data(Env const& env) const;

        /// Seat `seat`'s bot, in or out of the world (see BotSlot::Active).
        [[nodiscard]] Player* SeatBot(Env const& env, uint32 seat) const;
        /// What SeatReward last resolved as this seat's target (SeatState::CurrentTargetGuid), for the const readers
        /// -- episode info, state -- that cannot ask the encounters again. Falls back to the first target slot.
        [[nodiscard]] Unit* SeatTarget(Env const& env, uint32 seat) const;

        /// The scripted owner, or null (no owner in the env's arena, or none built).
        [[nodiscard]] Player* Owner(Env const& env) const;

        /// The party's living tank seat, or null (no party in the env's arena).
        [[nodiscard]] Player* PartyTank(Env const& env) const;

        /// Get a seat's bot ready to fight something that fights back: no XP, a hunter's stable, a warrior's stance.
        void PrepareFighter(Player* bot, SeatState& seat) const;

        /// Every seat, and the owner, stood up again after a pull: tell every encounter (see Encounter::OnRecovered).
        void NotifyRecovered(Env& env, int32 who);

        /// A pull is about to spawn: tell every encounter (see Encounter::OnPullStarting).
        void NotifyPullStarting(Env& env);

        /// Whether seat `seat` is dead with no resurrection of its own left to wait for (Tuning().Resurrection).
        [[nodiscard]] bool DeadForGood(Env const& env, uint32 seat) const;

        /// Whether seat `seat`'s bot is alive and knows a resurrection spell it could cast on an ally.
        [[nodiscard]] bool SeatCanResurrect(Env const& env, uint32 seat) const;

        /// The name of spec `spec` of class `layout` ("feral_bear"), for logs and reports; "?" if there is no such
        /// spec. A spec is the name of a talent template, which is a real thing about a build -- unlike a role,
        /// which was a name for what somebody expected the build to be for.
        [[nodiscard]] std::string SpecName(uint16 layout, uint8 spec) const;

    private:
        /// One thing a seat can be: a class, and one of its specs.
        struct Casting
        {
            Layout const* L = nullptr;
            uint8 Spec = 0;
        };

        /// What a seat may be: every (class, spec) the run can field, narrowed to those meeting `demand` when the
        /// arena's composition asked for something. A run whose classes have no build that meets it falls back to
        /// all of them (StageSettings::Classes may leave classes out, and a run of rogues and mages has nobody who
        /// can hold a pull).
        [[nodiscard]] std::vector<Casting> Castings(AptitudeDemand demand) const;

        /// The class and build `seat` plays this episode. An evaluation episode takes both from its seed index, so
        /// the seeds spread evenly over the (class, spec) pairs -- one model per class, but a paladin's healing
        /// build is still scored on its own share of the seeds. A training episode draws one, weighted by
        /// SetLayoutWeights unless `weighted` is off (a cast owner, which learns nothing from the draw).
        [[nodiscard]] Casting DrawCasting(Env const& env, uint32 seat, AptitudeDemand demand,
            bool weighted = true) const;

        /// How often a training episode draws this class with this build, relative to the others; 1 without weights.
        [[nodiscard]] float Weight(Layout const& layout, uint8 spec) const;
        void AddCoreEpisodeInfo();
        void WriteStageFiles(StageSettings const& settings) const;

        bool Rebuild(Env& env);
        /// The next episode's arena: drawn by weight (no draw for a single arena, so its random numbers are as before).
        [[nodiscard]] uint32 DrawArena() const;
        /// The encounters arena `arena` uses, in build order and in reward order.
        [[nodiscard]] std::vector<Encounter*> const& ActiveRewardOrder(Env const& env) const;
        /// Create and place seat `seat`'s next character (its layout is set). `map` is null for the env's first bot.
        Player* BuildSeat(Env& env, uint32 seat, Map*& map, uint8 level, Position const& start);
        /// The seat's current character made ready for a new episode in place of a rebuild (Characters.ReuseEpisodes):
        /// alive, full, unbuffed, cooldowns clear, pet away, moved to `start`. Null when it cannot be (then BuildSeat).
        Player* ReuseSeat(Env& env, uint32 seat, Position const& start);
        void Configure(Player* bot, SeatState& seat, bool pvp) const;
        /// Every seat's potions, bandages, stones and flask for the episode (after the encounters are built).
        void StockSeats(Env& env);
        /// Pet classes start with their pet out Characters.PetOutChance percent of the time (after StockSeats).
        void GivePets(Env& env);
        /// Dead players with a resurrection request accept it, as a client does; the reviving seat is credited.
        void AcceptResurrections(Env& env);

        /// What the seat's actions aim at: the dummy or creature, the selected pack enemy, the enemy player. Null
        /// between gauntlet pulls.
        [[nodiscard]] Unit* CurrentTarget(Env const& env, uint32 seat);
        /// Remember where the seat last saw its target, while it can see it.
        static void TrackTarget(Env const& env, SeatState& seat, Player* bot, Unit* target);
        /// The seat's view. Enemies the bot can neither see nor detect are left out of it, the target included
        /// (SeatView::HiddenTarget).
        [[nodiscard]] SeatView ViewSeat(Env const& env, uint32 seat, Player* bot, Unit* target) const;
        void ApplySeatAction(Env& env, uint32 seat, int32 action);
        /// Whether layout action `action` may not be pressed now (SeatMemory::Paced, Tuning().Actions).
        [[nodiscard]] bool Paced(Env const& env, SeatState const& seat, uint32 action) const;
        /// The seat pressed `action`: its memory, and the repeat charge.
        void Press(Env const& env, SeatState& seat, Player* bot, uint32 action, bool didSomething) const;
        void ObserveSeat(Env& env, uint32 seat, float* obs, uint8* mask);
        /// The row of the agent commanding `side`: what it sees of its side, the enemy and the standing order,
        /// and which calls it may make (DirectorLayout).
        void ObserveDirector(Env& env, uint32 side, float* obs, uint8* mask);
        [[nodiscard]] float SeatReward(Env& env, uint32 seat);
        /// Whether the seat's decision matched the goal it is pursuing (SeatGoal): damage for Fight, an enemy other
        /// than its target held for Control, healing or resting itself for Recover, healing or shielding the owner or
        /// a teammate for Protect, its spec's range for Position, a buff, summon or stealth out of combat for Prepare.
        [[nodiscard]] bool GoalHeld(Env const& env, uint32 seatIndex, Player* bot, Unit const* target) const;
        /// Before a seat's reward: what its absorbs on itself and its friends soaked since the last one (into the
        /// step's protection stats), and whether any friend is low.
        /// Count an enemy cast the seat could have interrupted, once per cast (interruptible_casts_seen).
    void TrackInterruptibleCast(Env const& env, SeatState& seat, Player* bot, Unit* target);

    /// The nearest hazard the seat is not standing in, cached and refreshed about once a second.
    void TrackHazards(Env const& env, SeatState& seat, Player* bot);
    /// Whether the seat's legs are getting anywhere (SeatState::MoveRate, CloseRate), for every arena.
    static void TrackMotion(Env const& env, SeatState& seat, Player const* bot, Unit const* target);

    void TrackSupport(Env& env, uint32 seatIndex, Player* bot);
        /// The per-decision bookkeeping SeatReward does before any encounter's terms (damage dealt and taken,
        /// the current target, support), for a row that is observed but not paid: the cast owner's.
        void TrackSeatStep(Env& env, uint32 seatIndex, Player* bot);
        void WriteState(Env const& env, float* state) const;

        StageDefinition const& _stage;
        CurriculumTuning _tuning;
        uint64 _reused = 0;                     // characters kept across episodes (CharactersReused)
        uint32 _spawnMapId;
        Position _spawnPoint;
        bool _continent = false;
        uint32 _seatCount = 1;
        bool _castOwner = false;            // some arena plays its owner as an agent (ArenaDefinition::OwnerCast)
        uint32 _level = 0;                  // StageSettings::Level: every character's level, 0 = random
        float _decisionScale = 1.0f;
        uint32 _decisionMs = 0;

        /// The director encounter, or null when no arena of the stage has one. Owned by _encounters.
        DirectorEncounter* _director = nullptr;

        std::vector<Layout> _layouts;
        /// The director layout's index in _layouts, or NO_LAYOUT when no arena of the stage has a learned
        /// director. The two director agents follow the seats: agent _seatCount + side commands side `side`.
        uint32 _directorLayout = NO_LAYOUT;

        /// Per layout, how often a training episode draws it (the learner's WEIGHTS message); empty = evenly.
        std::vector<float> _layoutWeights;
        ScenarioSpec _spec;
        EpisodeInfoTable _info;
        std::vector<EnvState> _data;

        /// Every encounter any arena uses, in build order: opponent, owner, party group, pulls, creature.
        std::vector<std::unique_ptr<Encounter>> _encounters;
        /// The same encounters in reward order: a reward may read what an earlier one recorded this decision.
        std::vector<Encounter*> _rewardOrder;
        /// Per arena: the encounters it uses, in build order and in reward order.
        std::vector<std::vector<Encounter*>> _arenaEncounters;
        std::vector<std::vector<Encounter*>> _arenaRewardOrder;
        /// Per arena: its share of episodes (<TuningPrefix>Arena.<stage>.<arena>.Weight) and episode length.
        std::vector<uint32> _arenaWeights;
        std::vector<uint32> _arenaEpisodeMs;
        std::vector<int32> _arenaMaxRung;       // -1: the ladder's own cap (Pulls.MaxTier)
        OwnerEncounter* _owner = nullptr;
        PartyEncounter* _party = nullptr;
    };
}

#endif
