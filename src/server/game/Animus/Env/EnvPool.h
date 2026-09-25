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
#include <atomic>
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

        /// Every world tick its maps ticked: advance a group's episode clocks by the game time they ticked.
        void AdvanceClock(uint32 group, uint32 diff);

        /// Half-batch (AnimusForge.HalfBatch): envs [0, split) are group 0 and the rest group 1, and the sim ticks one
        /// group's maps while the learner decides the other's. split = NumEnvs(), the default, is one group.
        void SetGroups(uint32 split);
        [[nodiscard]] uint32 GroupCount() const { return _split < NumEnvs() ? 2 : 1; }
        /// (first env, env count) of a group.
        [[nodiscard]] std::pair<uint32, uint32> GroupRange(uint32 group) const
        {
            uint32 const split = std::min(_split, NumEnvs());
            return group == 0 ? std::make_pair(0u, split) : std::make_pair(split, NumEnvs() - split);
        }
        /// The group whose envs are on `map`, or -1 for a map without envs. Read by map tasks while the maps update,
        /// when nothing moves an env between maps.
        [[nodiscard]] int32 GroupOfMap(Map const& map) const;
        /// No map holds envs of both groups, which freezing a group's maps needs. Continent replicas are dealt
        /// contiguous blocks, so a split on a replica boundary passes; an instance holds one env.
        [[nodiscard]] bool GroupsKeepToTheirMaps() const;

        /// Reset every env and write fresh observations with zero reward and done.
        void ResetAll();

        /// A decision in three parts, so the scoring and the observation run on the threads that update
        /// the maps instead of on the world thread.
        ///
        /// BeginDecision opens the decision on the world thread before the maps tick: it clears the marks
        /// and the per-env timings the other parts fill. ApplyActionsForMap and ObserveMap run on the
        /// thread updating that map, before and after its tick, and touch only the envs that live on it,
        /// so two maps never meet. FinishCollect closes the decision on the world thread once the maps
        /// have joined, and does what has to stay serial: the ended episodes' info and their reset.
        ///
        /// An env whose map never ticked is observed by FinishCollect itself, so a map the scheduler did
        /// not run costs correctness nothing.
        ///
        /// Each takes the group deciding (0 with one group): a half-batch decision is two, one per half.
        void BeginDecision(uint32 group);
        void ApplyActionsForMap(Map const& map);
        void ObserveMap(Map const& map);
        void FinishCollect(uint32 group);
        /// Close whichever decisions are open.
        void FinishCollect();

        /// Whether a decision is open: BeginDecision ran and FinishCollect has not. A host that abandons
        /// a decision (a pause, a learner that reconnects) still has to close it.
        [[nodiscard]] bool DecisionOpen() const { return _decisionOpen[0] || _decisionOpen[1]; }

        /// Fill Actions from a local policy ("random" or a scenario scripted policy): every agent's, or only the
        /// opponent seats' (Scenario::IsOpponentSeat), keeping the other actions. Only envs [begin, begin + count).
        bool ChooseLocalActions(std::string const& policy, bool opponentsOnly = false, uint32 begin = 0,
            uint32 count = UINT32_MAX);

        /// Evaluation (the forge's MODE message): hand seed indexes firstSeed..firstSeed+episodes-1 to envs as they
        /// reset (a cluster's sims each play their own run of one evaluation's seeds), each env
        /// rebuilt right after reseeding the world thread's random numbers from (seedBase, index)
        /// (CoreHooks::SeedRandom). With a baseline policy name, EvalBaseline() tells the caller to run it
        /// instead of the learner's actions -- only on the opponent seats when EvalOpponentsOnly(). Takes effect at
        /// the next reset; call ResetAll to start every env on it.
        void SetEvaluation(bool enabled, uint32 seedBase, uint32 episodes, std::string const& baseline,
            bool opponentsOnly = false, uint32 firstSeed = 0);
        [[nodiscard]] bool IsEvaluating() const { return _evaluating; }
        /// Data-parallel learners each play their own run of an evaluation's seeds on their own envs: after
        /// SetEvaluation, `runs[r]` = (first seed index, episodes) for the envs whose `rangeOfEnv` entry is r.
        void SetEvaluationRuns(std::vector<std::pair<uint32, uint32>> const& runs, std::vector<uint32> rangeOfEnv);

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

        /// Where a decision's pool time went, for the host's report: the parts that can be worked on
        /// separately. Filled by FinishCollect, so a host may read it once the decision is closed.
        ///
        /// Reward, Observe, FinalObserve and Apply are summed thread time from the map tasks -- the maps run
        /// at once, so their total is more than the wall clock they took, and it is already inside the world
        /// figure rather than on top of it. Reset is the world thread's own.
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

        /// One env's decision, less what must run on the world thread: the reward, the step stats, the
        /// terminal check, and the observation (the final one when the episode ended, whose next episode
        /// FinishEnv observes after building it). Runs on the thread updating the env's map.
        void ObserveEnv(Env& env);
        /// An ended episode's serial half: its info, its report, the next episode, and that one's first
        /// observation. World thread only -- it builds characters and puts them on a map.
        void FinishEnv(Env& env);

        /// _envMapKey of an env that is not in _mapEnvs yet.
        static constexpr uint64 NOT_FILED = UI64LIT(0xFFFFFFFFFFFFFFFF);

        /// Envs are grouped by this, so a map task can find its own: one key per Map, since an instance id
        /// is unique and a continent's replicas each have their own.
        [[nodiscard]] static uint64 MapKey(uint32 mapId, uint32 instanceId)
        {
            return (uint64(mapId) << 32) | instanceId;
        }
        [[nodiscard]] static uint64 MapKey(Map const& map);

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

        /// MapKey -> the envs living on that map, in env order. Maintained like _agents, and read by the
        /// map threads while they hold a decision's half.
        std::unordered_map<uint64, std::vector<uint32>> _mapEnvs;
        std::vector<uint64> _envMapKey;         // per env: the key it is filed under, to move it when it changes

        /// A decision is open between BeginDecision and FinishCollect.
        uint32 _split = UINT32_MAX;             // first env of group 1; NumEnvs() or more is one group (SetGroups)
        bool _decisionOpen[2] = { false, false };
        /// What the maps spent applying this decision's actions, summed across them (they run at once, so it
        /// is more than the wall clock they took). Taken by the next FinishCollect, which is the decision the
        /// actions came from however many ticks apart the two are.
        std::atomic<uint64> _applyNs{ 0 };
        /// Per env, so two map threads never write the same line; summed into _collect by FinishCollect.
        std::vector<CollectTiming> _envCollect;
        /// Per env: its map ticked this decision and observed it. What is left is observed serially.
        std::vector<uint8> _observed;
        /// The stray-env warning is worth one line, not one per decision.
        bool _strayLogged = false;

        bool _evaluating = false;
        uint32 _evalSeedBase = 0;
        uint32 _evalEpisodes = 0;
        struct EvalRun
        {
            uint32 Next = 0;
            uint32 End = 0;
        };
        std::vector<EvalRun> _evalRuns;             // SetEvaluationRuns; empty = the one run above
        std::vector<uint32> _evalRunOfEnv;
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
