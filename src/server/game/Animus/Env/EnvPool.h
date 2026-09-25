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

#ifndef ANIMUS_LIB_ENV_POOL_H
#define ANIMUS_LIB_ENV_POOL_H

#include "Env.h"
#include "Scenario.h"
#include <string>
#include <unordered_map>
#include <vector>

class Spell;
class SpellInfo;
class Unit;
enum DamageEffectType : uint8;

namespace Animus
{
    struct StageSettings;

    /// EpisodeSeed of an episode that was not an evaluation episode.
    constexpr uint32 NO_EPISODE_SEED = 0xFFFFFFFF;

    /// Every env of one scenario, plus the flat structure-of-arrays buffers a host reads and writes: the forge's bridge
    /// sends them to the learner (its STEP payload layout, env-major), the stage viewer plays them locally.
    ///
    /// Its hooks (RecordDamage, ...) are fed by the library's scripts while the pool is registered (PoolRegistry).
    class EnvPool
    {
    public:
        EnvPool(Scenario& scenario, StageSettings const& settings);

        bool Setup();
        void Teardown();

        /// Every world tick: advance each env's episode clock.
        void AdvanceClock(uint32 diff);

        /// Reset every env and write fresh observations with zero reward and done.
        void ResetAll();

        /// Decision step: score the transition that just ended, auto-reset finished envs, observe.
        void Collect();

        /// Fill Actions from a local policy ("random" or a scenario scripted policy): every agent's, or only the
        /// opponent seats' (Scenario::IsOpponentSeat), keeping the other actions.
        bool ChooseLocalActions(std::string const& policy, bool opponentsOnly = false);

        void ApplyActions();

        /// Evaluation (the forge's MODE message): hand seed indexes 0..episodes-1 to envs as they reset, each env
        /// rebuilt right after reseeding the world thread's random numbers from (seedBase, index)
        /// (CoreHooks::SeedRandom). With a baseline policy name, EvalBaseline() tells the caller to run it
        /// instead of the learner's actions -- only on the opponent seats when EvalOpponentsOnly(). Takes effect at
        /// the next reset; call ResetAll to start every env on it.
        void SetEvaluation(bool enabled, uint32 seedBase, uint32 episodes, std::string const& baseline,
            bool opponentsOnly = false);
        [[nodiscard]] bool IsEvaluating() const { return _evaluating; }

        /// How often training episodes draw each of the scenario's layouts (the forge's WEIGHTS message), in layout
        /// order; empty restores the even draw. Takes effect as envs reset; evaluation episodes are never weighted.
        void SetLayoutWeights(std::vector<float> const& weights) { _scenario.SetLayoutWeights(weights); }

        /// Replaying lost evaluation episodes (the forge's REPLAY message): `fraction` of training resets rebuild one
        /// of `seeds` -- evaluation seed indexes of `seedBase` -- from the very random numbers the evaluation built it
        /// from, so the same character meets the same opponent; the fight itself rolls afresh. Replaces the seeds
        /// before; no seeds or a fraction of 0 stops it. Evaluation episodes are never replays, and a replay reports
        /// as a training episode (NO_EPISODE_SEED).
        void SetReplay(uint32 seedBase, float fraction, std::vector<uint32> seeds);
        [[nodiscard]] uint64 ReplayedEpisodes() const { return _replayed; }

        [[nodiscard]] std::string const& EvalBaseline() const { return _evalBaseline; }
        [[nodiscard]] bool EvalOpponentsOnly() const { return _evalOpponentsOnly; }

        /// Damage hook, called from map worker threads. Only touches the stats of the env whose
        /// instance the calling thread is updating. `spell` is the spell that dealt it, when the hook knows (null for
        /// melee swings, and for spell damage it could not match).
        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type,
            SpellInfo const* spell);

        /// Heal hook, called from map threads with the health actually gained. Counts healing an agent
        /// (or its pets) does on itself, its env's allies and its other agents.
        void RecordHeal(Unit const* healer, Unit const* receiver, uint32 gain, bool periodic = false);

        /// Heal hook, called from map threads with the healing before it is applied (overhealing included): what an
        /// agent cast on itself, its env's allies and agents, for the overheal share.
        void RecordHealCast(Unit const* healer, Unit const* receiver, uint32 heal);

        /// Spell hooks, called from map threads when an agent's cast-time spell finishes casting or is
        /// cancelled before it does. Triggered spells and channels are not counted.
        void RecordCastCompleted(Unit const* caster, Spell* spell);
        void RecordCastCancelled(Unit const* caster, Spell* spell, bool bySelf);

        [[nodiscard]] Scenario const& GetScenario() const { return _scenario; }
        [[nodiscard]] ScenarioSpec const& Spec() const { return _spec; }
        [[nodiscard]] uint32 NumEnvs() const { return static_cast<uint32>(_envs.size()); }
        [[nodiscard]] Env const& GetEnv(uint32 index) const { return _envs[index]; }

        /// Episodes finished by every env since Setup.
        [[nodiscard]] uint64 CompletedEpisodes() const;

        /// Where a decision's pool time went, for the host's report: the parts of Collect and ApplyActions that
        /// can be worked on separately. Filled every decision, so a host may read it after ApplyActions.
        ///
        /// ObserveNs covers the observation and the mask of the running episodes; FinalObserveNs the ended ones'
        /// last observation, which is the same work without the mask, so comparing the two per call is the
        /// cheapest read on what mask building costs.
        struct CollectTiming
        {
            uint64 RewardNs = 0;
            uint64 ObserveNs = 0;           // ... and the step stats roll-up and terminal check before it
            uint64 FinalObserveNs = 0;      // ... and the ended episodes' info
            uint64 ResetNs = 0;             // building the next episode: characters, gear, the encounter
            uint64 ResetCreateNs = 0;       // ... of which: creating and placing the seats' characters
            uint64 ResetPlaceNs = 0;        // ... phase, position data and talent points on the map
            uint64 ResetConfigureNs = 0;    // ... talents, kit, gear
            uint64 ResetDestroyNs = 0;      // ... destroying the previous seats
            uint64 ApplyNs = 0;             // ApplyActions, which is a decision's other half
            uint32 Observes = 0;            // envs observed (one per env per decision)
            uint32 Resets = 0;              // episodes that ended and were rebuilt
            uint64 Reused = 0;              // characters kept across those resets instead of rebuilt (cumulative)
        };

        [[nodiscard]] CollectTiming const& LastCollect() const { return _collect; }

        /// Mean episode info of the last StageSettings::ReportEpisodes finished episodes, by column name, and how many
        /// episodes that was (0 before the first batch).
        [[nodiscard]] std::vector<std::pair<std::string, double>> const& LastEpisodeMeans() const
        {
            return _lastEpisodeMeans;
        }
        [[nodiscard]] uint64 LastEpisodeMeansCount() const { return _lastEpisodeMeansCount; }

        std::vector<float> Obs;
        std::vector<float> State;
        std::vector<uint8> Mask;
        std::vector<float> Rewards;
        std::vector<uint8> Done;
        std::vector<uint8> Terminated;
        std::vector<float> FinalObs;
        std::vector<float> FinalState;
        std::vector<uint16> Layout;             // per agent: index into Spec().Layouts
        std::vector<uint8> Present;             // per agent: 1 = has a character this episode
        std::vector<float> EpisodeInfo;         // per agent
        std::vector<uint32> EpisodeSeed;        // per env: seed index of the episode that just ended
        std::vector<int32> Actions;
        /// The goal each agent is pursuing, in agent order, as the learner sent it (Curriculum::NO_GOAL for none).
        /// A host fills it before ApplyActions; a policy without goals leaves it alone.
        std::vector<int32> Goals;

    private:
        struct AgentSlot
        {
            uint32 Env;
            uint32 Agent;
        };

        /// A hit on an agent or an ally (`victimSlot`): credit each agent whose own damage-taken reductions on the
        /// victim made it smaller with what they prevented.
        void RecordPrevented(AgentSlot const& victimSlot, bool victimIsAgent, Unit const* victim, uint32 damage);

        void ResetEnv(Env& env);
        /// Write the env's per-agent layout and presence rows (after its observation).
        void DescribeAgents(Env const& env);
        /// A non-agent's cast was cancelled: note it on its env when it is one of the env's targets.
        void RecordTargetInterrupted(Unit const* caster, Spell* spell, bool bySelf);
        /// Map the env's bots, allies and instance to it (world thread, while no map updates).
        void IndexEnv(Env const& env);
        void ReportEpisode(uint32 envIndex);

        Scenario& _scenario;
        ScenarioSpec _spec;
        uint32 _episodeLengthMs;
        uint32 _reportEpisodes;

        std::vector<Env> _envs;

        /// Bot GUID -> env/agent. Built in Setup and changed only when a reset rebuilds an env's bots,
        /// on the world thread while no map updates, so the concurrent lookups from map threads need
        /// no lock.
        std::unordered_map<ObjectGuid, AgentSlot> _agents;

        /// Ally GUID -> env and index in Env::Allies. Maintained like _agents.
        std::unordered_map<ObjectGuid, AgentSlot> _allies;

        /// Instance id -> env index, for hooks about units that are not agents (the env's targets). Maintained like
        /// _agents.
        std::unordered_map<uint32, uint32> _envByInstance;

        bool _evaluating = false;
        uint32 _evalSeedBase = 0;
        uint32 _evalEpisodes = 0;
        uint32 _evalNextSeed = 0;
        std::string _evalBaseline;
        bool _evalOpponentsOnly = false;
        std::vector<uint32> _envSeed;           // per env: seed index of the running episode

        uint32 _replaySeedBase = 0;
        float _replayFraction = 0.0f;
        std::vector<uint32> _replaySeeds;
        uint64 _replayed = 0;                   // training resets that rebuilt a replay seed

        CollectTiming _collect;

        std::vector<double> _reportInfoSum;
        uint32 _reportedEpisodes = 0;
        std::vector<std::pair<std::string, double>> _lastEpisodeMeans;
        uint64 _lastEpisodeMeansCount = 0;
    };
}

#endif
