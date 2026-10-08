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
#include "Kinematics.h"
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Animus
{
    struct Env;
    struct StageSettings;

    /// One kind of agent: its observation features and actions (a class/role, say). An agent of a layout fills
    /// only the first ObsDim features and NumActions mask entries of its padded row.
    /// A start a dungeon wing's training run may take instead of the door (Go-Explore, the learner's EXPLORE_STARTS):
    /// the arena and row, the route's packs cleared (24 bits a word, route order), the party's yard / 16, the weight.
    struct ExploreStart
    {
        uint32 Arena = 0;
        uint32 Tier = 0;
        std::array<uint32, 4> Packs{};
        uint32 Yard = 0;
        float Weight = 0.0f;
    };

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
        /// Per agent, the bytes of its camera image (Vision::ImageBytes) when the stage has a vision block, else 0:
        /// the STEP's image section (camera-vision.BYTES.md, protocol 21).
        uint32 ImageBytes = 0;
        /// Per agent, the look head's choices ACT carries after the actions and goals (Vision::FreeLook::HEADS) when
        /// the stage has a vision block, else 0: ACT is then protocol 21's (camera-vision.FREELOOK.md C).
        uint32 LookHeads = 0;
        /// Per agent, the bytes of its mental map's crop (Vision::CROP_BYTES) when the stage has a map block, else 0:
        /// the STEP's map section (perception-goals REDESIGN §3, protocol 24).
        uint32 MapBytes = 0;
        std::vector<LayoutSpec> Layouts;    // empty = one layout named after the scenario, ObsDim x NumActions
    };

    /// A training scenario: how an env is built, reset, observed, acted on and scored.
    ///
    /// Buffers are pre-sized by EnvPool: per-agent arrays hold AgentsPerEnv rows in agent order. Reset, Reward,
    /// IsTerminal, Observe, ApplyActions, ApplyGoals, ApplyLook and SubTick of an env run on the map thread that
    /// updates its map (EnvPool::ResetMapEnvs, ApplyActionsForMap, ObserveMap); the rest run on the world thread,
    /// outside MapMgr::Update.
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

        /// Every world tick, after ApplyActions on a tick that has a decision's actions (`decided`), with the game
        /// milliseconds the tick moved: what the world does between decisions when TicksPerDecision is above 1, and
        /// what clients are shown (movement-smooth A2). No observation, no action, no reward.
        virtual void SubTick(Env& /*env*/, uint32 /*diffMs*/, bool /*decided*/) { }

        /// goals: [AgentsPerEnv] the goal each agent is pursuing (0..GoalCount-1, or Curriculum::NO_GOAL), sent with
        /// the actions by a policy that has a goal head. Called before ApplyActions. A goal is scored, reported and
        /// shown to teammates; it never masks an action, so a goal out of range is simply ignored.
        virtual void ApplyGoals(Env& /*env*/, int32 const* /*goals*/) { }

        /// look: [AgentsPerEnv * Spec().LookHeads] each agent's look head choice (Vision::FreeLook), in range (ACT
        /// refuses any other). Called before ApplyActions, only when LookHeads > 0. Only agents whose layout has a
        /// camera take theirs; every other row is a placeholder and is never applied. Looking is free: nothing
        /// here may reach what prices, tallies or judges an action.
        virtual void ApplyLook(Env& /*env*/, int32 const* /*look*/) { }

        /// The size agent `agent`'s camera casts its frames at this episode (the camera audit's "render"), or 0 x 0
        /// for an agent with no camera.
        [[nodiscard]] virtual std::pair<uint32, uint32> CameraRenderSize(Env const& /*env*/, uint32 /*agent*/) const
        {
            return { 0, 0 };
        }

        /// The observation column where layout `layout`'s entity list starts (the entities block: ENTITY_SLOTS slots
        /// of EntitiesBlock::ENTITY_FEATURES), or -1 for a layout without a camera. The audit and the evaluation
        /// videos read the listed entities back from an observation row to draw them over the image.
        [[nodiscard]] virtual int32 EntitiesFirst(uint16 /*layout*/) const { return -1; }

        /// obs: [AgentsPerEnv * ObsDim], state: [StateDim], mask: [AgentsPerEnv * NumActions]. `mask` is null for an
        /// ended episode's final observation, which needs no actions: skip the (costly) cast checks then. image:
        /// [AgentsPerEnv * Spec().ImageBytes] each agent's camera image, null when ImageBytes is 0. map:
        /// [AgentsPerEnv * Spec().MapBytes] each agent's mental map crop, null when MapBytes is 0.
        virtual void Observe(Env& env, float* obs, float* state, uint8* mask, uint8* image, uint8* map) = 0;

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

        /// kinematics: [AgentsPerEnv * Kinematics::SAMPLE_DIM] each agent's body as it stands after Observe
        /// (Kinematics.h, protocol 20): what the learner's style reward and realism score read. Called with
        /// AgentLayouts, on the thread updating the env's map; zeros for an agent without a body, and for every agent
        /// of a scenario that has no bodies to describe.
        virtual void AgentKinematics(Env const& /*env*/, float* kinematics) const
        {
            std::fill(kinematics, kinematics + Spec().AgentsPerEnv * Kinematics::SAMPLE_DIM, 0.0f);
        }

        /// reward: [AgentsPerEnv], from env.StepStats (cleared by EnvPool afterwards).
        virtual void Reward(Env& env, float* reward) = 0;

        /// True if the episode reached a terminal state (e.g. every agent died). Checked at each
        /// decision; the episode time limit ends it as a truncation otherwise.
        [[nodiscard]] virtual bool IsTerminal(Env const& /*env*/) const { return false; }

        /// info: [AgentsPerEnv * EpisodeInfoDim] totals per agent for the episode that just ended.
        virtual void EpisodeInfo(Env const& env, float* info) const = 0;

        /// One name per EpisodeInfo column; sent to the learner in SPEC and used in local reports.
        [[nodiscard]] virtual std::vector<std::string> EpisodeInfoNames() const = 0;

        /// How many seeds an evaluation's placements cycle through before the next rung starts (seed i plays pair
        /// i mod this, rung i / this): the evaluation videos (Vision::EvalVideoSeeds) spread their picks by it.
        [[nodiscard]] virtual uint32 EvaluationPairs() const { return 1; }

        /// A seat's place in its party, for the evaluation videos' choice of whom to film in a party (Vision::
        /// EvalVideoAgent): 1 tank, 2 healer, 3 damage, read off its build; 0 for none (no party, or no place drawn).
        [[nodiscard]] virtual uint32 FilmedRole(Env const& /*env*/, uint32 /*agent*/) const { return 0; }

        /// How often training episodes should draw each layout of Spec().Layouts, in layout order (the learner's
        /// WEIGHTS message). Weights are relative, so all-ones is the even draw a scenario starts with; an empty
        /// vector restores it. Evaluation episodes are unaffected: they spread their seeds over the layouts evenly.
        /// Scenarios that have one layout, or draw none, need not implement it.
        virtual void SetLayoutWeights(std::vector<float> const& /*weights*/) { }
        /// How far through its budget the stage's training is, 0 to 1 (the learner's PROGRESS).
        virtual void SetStageProgress(float /*progress*/) { }
        /// What shaping rewards are paid times (the learner's fade ladder, PROGRESS from protocol 18): 1 = as tuned.
        virtual void SetShapingScale(float /*scale*/) { }
        /// What noise prices are paid times (the learner's cost ladder, PROGRESS from protocol 19): 1 = as tuned.
        virtual void SetCostScale(float /*scale*/) { }
        /// The arena the next evaluation plays (index + 1; 0 = the stage's own draw): a held-out arena only (MODE's
        /// arena, protocol 18). False, and nothing changes, for one the scenario does not hold out.
        virtual bool PinEvaluationArena(uint32 pin) { return pin == 0; }
        /// Whether the learner plays the "human" stand-in's row with a frozen partner (MODE_FLAG_STAND_IN): an
        /// evaluation then has the stand-in in every episode that has a party (the learner's "with the human
        /// stand-in" arm), training in its share of them; without it every party is all the learner's. Scenarios
        /// without a stand-in ignore it.
        virtual void SetStandIn(bool /*standIn*/) { }
        /// The cells training runs of a dungeon wing start from, `share` of the time (EXPLORE_STARTS); replaces the
        /// last table. Scenarios without wings ignore it.
        virtual void SetExploreStarts(float /*share*/, std::vector<ExploreStart> /*starts*/) { }

        /// A cluster's shared curriculum state (StageScenario's dungeon ladder): a worker's runs since its last
        /// report, as a PROGRESS field (empty for none); the host folds every worker's into its own and sends the
        /// workers what it decided ("RUNG <n>"), which they follow instead of deciding for themselves.
        [[nodiscard]] virtual std::string TakeClusterTally() { return {}; }
        virtual void AddClusterTally(std::string const& /*tally*/) { }
        [[nodiscard]] virtual int32 ClusterRung() const { return -1; }
        virtual void FollowClusterRung(uint32 /*rung*/) { }
        /// The rung of the cluster's dungeon ladder that has collapsed (WingLadder's alarm, the host's), or -1.
        [[nodiscard]] virtual int32 ClusterLadderCollapsed() const { return -1; }

        /// Once at shutdown: remove bots (without saving) and targets.
        virtual void Teardown(Env& env) = 0;
    };

    /// Build the scenario `name`, or nullptr if no such scenario exists.
    std::unique_ptr<Scenario> CreateScenario(std::string const& name, StageSettings const& settings);

    /// Names of every registered scenario.
    std::vector<std::string> ScenarioNames();
}

#endif
