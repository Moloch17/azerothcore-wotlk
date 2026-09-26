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

#ifndef ANIMUS_LIB_SCENARIO_H
#define ANIMUS_LIB_SCENARIO_H

#include "Define.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Animus
{
    struct Env;
    struct StageSettings;

    /// One kind of agent: its observation features and actions (a class/role, say). An agent of a layout fills
    /// only the first ObsDim features and NumActions mask entries of its padded row.
    struct LayoutSpec
    {
        std::string Name;
        uint32 ObsDim = 0;
        uint32 NumActions = 0;
    };

    /// Fixed tensor shapes a scenario exposes to the learner.
    struct ScenarioSpec
    {
        uint32 AgentsPerEnv = 1;
        uint32 ObsDim = 0;          // the largest layout's: every agent's observation row is padded to it
        uint32 StateDim = 0;
        uint32 NumActions = 0;      // the largest layout's: every agent's mask row is padded to it
        uint32 EpisodeInfoDim = 0;  // per agent
        uint32 GoalCount = 0;       // goals a policy may pursue (Curriculum::SeatGoal); 0 = the scenario has none
        uint32 LongestEpisodeSeconds = 0;   // when some episodes run longer than StageSettings::EpisodeSeconds
        std::vector<LayoutSpec> Layouts;    // empty = one layout named after the scenario, ObsDim x NumActions
    };

    /// A training scenario: how an env is built, reset, observed, acted on and scored.
    ///
    /// All calls happen on the world thread, outside MapMgr::Update. Buffers are pre-sized by
    /// EnvPool: per-agent arrays hold AgentsPerEnv rows in agent order.
    class Scenario
    {
    public:
        virtual ~Scenario() = default;

        [[nodiscard]] virtual char const* Name() const = 0;
        [[nodiscard]] virtual ScenarioSpec Spec() const = 0;

        /// Whether this run can play the scenario at all. A curriculum stage restricted to the classes that can
        /// do its thing (the stealth drill) has no layout in a run of other classes; a queue skips it rather than
        /// halting on it, and the stage after it seeds from the one before.
        [[nodiscard]] virtual bool Playable() const { return true; }

        /// Characters kept across an episode boundary instead of rebuilt (StageScenario::ReuseSeat), cumulative:
        /// `forge status` reports the rate beside the episodes rebuilt per decision.
        [[nodiscard]] virtual uint64 CharactersReused() const { return 0; }
        /// Whether every episode reset keeps its env on the map it is on, with nothing shared built or torn down but
        /// through ResetDefer: then the pool may reset it on the thread updating that map (EnvPool). False unless a
        /// scenario has made sure.
        [[nodiscard]] virtual bool ResetsStayOnMap() const { return false; }

        /// Once at startup: create bots and targets and place them. env.MapId/InstanceId, Bots and
        /// Targets must be filled in. Returns false if the env cannot be built.
        virtual bool Setup(Env& env) = 0;

        /// Start a new episode in place. EnvPool has already cleared the episode clock and stats.
        virtual void Reset(Env& env) = 0;

        /// actions: [AgentsPerEnv] chosen action per agent. Masked actions may still arrive from a
        /// misbehaving client and must be ignored safely.
        virtual void ApplyActions(Env& env, int32 const* actions) = 0;

        /// goals: [AgentsPerEnv] the goal each agent is pursuing (0..GoalCount-1, or Curriculum::NO_GOAL), sent with
        /// the actions by a policy that has a goal head. Called before ApplyActions. A goal is scored, reported and
        /// shown to teammates; it never masks an action, so a goal out of range is simply ignored.
        virtual void ApplyGoals(Env& /*env*/, int32 const* /*goals*/) { }

        /// obs: [AgentsPerEnv * ObsDim], state: [StateDim], mask: [AgentsPerEnv * NumActions]. `mask` is null for an
        /// ended episode's final observation, which needs no actions: skip the (costly) cast checks then.
        virtual void Observe(Env& env, float* obs, float* state, uint8* mask) = 0;

        /// layout: [AgentsPerEnv] index into Spec().Layouts of each agent's current layout. Called after Observe,
        /// and for an ended episode before its reset. Constant within an episode.
        virtual void AgentLayouts(Env const& /*env*/, uint16* layout) const
        {
            std::fill(layout, layout + Spec().AgentsPerEnv, uint16(0));
        }

        /// present: [AgentsPerEnv] 1 when the agent has a character this episode, 0 for a seat left empty (it only
        /// has the no-op and earns nothing, so the learner does not train on it). Constant within an episode.
        virtual void AgentPresence(Env const& /*env*/, uint8* present) const
        {
            std::fill(present, present + Spec().AgentsPerEnv, uint8(1));
        }

        /// reward: [AgentsPerEnv], from env.StepStats (cleared by EnvPool afterwards).
        virtual void Reward(Env& env, float* reward) = 0;

        /// Whether `agent` is the other side of a self-play episode: evaluation can have a scripted policy play it
        /// (MODE_FLAG_SCRIPTED_OPPONENTS) to score the learner against a fixed opponent.
        [[nodiscard]] virtual bool IsOpponentSeat(Env const& /*env*/, uint32 /*agent*/) const { return false; }

        /// True if the episode reached a terminal state (e.g. every agent died). Checked at each
        /// decision; the episode time limit ends it as a truncation otherwise.
        [[nodiscard]] virtual bool IsTerminal(Env const& /*env*/) const { return false; }

        /// info: [AgentsPerEnv * EpisodeInfoDim] totals per agent for the episode that just ended.
        virtual void EpisodeInfo(Env const& env, float* info) const = 0;

        /// One name per EpisodeInfo column; sent to the learner in SPEC and used in local reports.
        [[nodiscard]] virtual std::vector<std::string> EpisodeInfoNames() const = 0;

        /// Scripted baseline for local policies other than "random" (the forge's AnimusForge.Policy, the stage viewer's
        /// policy), for one agent of `layout` (obs and mask are its row). Returns false if the scenario does not know
        /// the policy.
        virtual bool ScriptedAction(std::string const& policy, float const* obs, uint8 const* mask, uint16 layout,
            int32& action) const = 0;

        /// How often training episodes should draw each layout of Spec().Layouts, in layout order (the learner's
        /// WEIGHTS message). Weights are relative, so all-ones is the even draw a scenario starts with; an empty
        /// vector restores it. Evaluation episodes are unaffected: they spread their seeds over the layouts evenly.
        /// Scenarios that have one layout, or draw none, need not implement it.
        virtual void SetLayoutWeights(std::vector<float> const& /*weights*/) { }

        /// Once at shutdown: remove bots (without saving) and targets.
        virtual void Teardown(Env& env) = 0;
    };

    /// Build the scenario `name`, or nullptr if no such scenario exists.
    std::unique_ptr<Scenario> CreateScenario(std::string const& name, StageSettings const& settings);

    /// Names of every registered scenario.
    std::vector<std::string> ScenarioNames();
}

#endif
