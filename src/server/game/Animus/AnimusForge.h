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

#ifndef MOD_ANIMUS_FORGE_H
#define MOD_ANIMUS_FORGE_H

#include "ChildProcess.h"
#include "ClusterLink.h"
#include "Map.h"
#include "MapMgr.h"
#include "EnvPool.h"
#include "ForgeConfig.h"
#include "SeatEncoder.h"
#include "LearnerProcess.h"
#include "LockstepServer.h"
#include "Progress.h"
#include "Scenario.h"
#include "TextTable.h"
#include <chrono>
#include <memory>
#include <optional>

namespace AnimusForge
{
    /// Module root: owns the scenario, the env pool and the learner connection, and runs one decision step every
    /// AnimusForge.TicksPerDecision world ticks while a plan runs (a decision is AnimusForge.DecisionMs of game time;
    /// at the default of one tick per decision the two are the same thing).
    ///
    /// The sim starts idle; console commands (scripts/Commands/cs_forge.cpp) start, pause, resume, skip and cancel
    /// plans. A command only records a request: OnUpdate applies it at the start of a tick, never in the middle of
    /// a decision. While the world thread waits on the learner (accepting it, or waiting for its ACT) it keeps
    /// running console commands, so the console answers within a fraction of a second throughout.
    class Forge
    {
    public:
        static Forge* Instance();

        void OnStartup();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// The world tick, before the maps are scheduled: the episode clock, and whether this tick ends a
        /// decision. A decision's scoring and observation run inside the map tasks that follow
        /// (OnMapEpilogue), so both have to be settled before the first of them starts.
        void OnWorldPrologue(uint32 diff);

        /// Inside a map's task, on the thread updating it: the envs on this map take the last decision's
        /// actions before its tick (OnMapPrologue) and are scored and observed after it (OnMapEpilogue).
        /// Both do nothing for a map that holds no env, which is every map in playtest mode.
        void OnMapPrologue(Map& map);
        void OnMapEpilogue(Map& map);

        /// Half-batch (AnimusForge.HalfBatch): whether `map` sits out this world tick because it holds envs of the
        /// half that is not ticking -- the one the learner is deciding. Called from map tasks (MapMgr::ForgeTickDiff,
        /// for the instances a container schedules), while nothing writes the state it reads.
        [[nodiscard]] bool IsMapFrozen(Map const& map) const;

        /// Console commands, run on the world thread. Each writes its reply to `out` and returns false when it
        /// refuses (the reply says why).
        void CommandStatus(LineSink const& out);
        void CommandScenarios(LineSink const& out);
        bool CommandStart(std::vector<std::string> scenarios, LineSink const& out);
        bool CommandFast(std::vector<std::string> scenarios, LineSink const& out);
        bool CommandResume(std::vector<std::string> scenarios, LineSink const& out);
        bool CommandPause(LineSink const& out);
        bool CommandCancel(LineSink const& out);
        bool CommandSkip(LineSink const& out);
        bool CommandRun(std::string const& scenario, std::string const& policy, uint32 episodes, LineSink const& out);
        /// `forge bench [scenario]`: time the sim at every AnimusForge.Bench.Threads x Envs pair, then the best few
        /// with the learner, and report what runs fastest. `forge bench apply` writes the winner into the configs.
        bool CommandBench(std::string const& scenario, LineSink const& out);

        /// `forge talents <class> [spec] [points] [plan]`: print a build the curriculum would give a
        /// character of that class, tree by tree. Builds nothing and trains nothing.
        bool CommandTalents(std::string const& playerClass, std::string const& spec, uint32 points,
            std::string const& plan, LineSink const& out);
        bool CommandBenchApply(LineSink const& out);
        bool CommandExport(std::string scenario, std::string const& checkpoint, LineSink const& out);
        bool CommandClean(std::string const& target, std::string const& scenario, LineSink const& out);
        void CommandProgress(std::optional<uint32> seconds, LineSink const& out);

    private:
        enum class State : uint8
        {
            Idle,
            Training,       // remote policy: lock-step with the learner
            Running,        // local policy: scripted or random actions
            Paused,
        };

        enum class Request : uint8
        {
            None,
            Start,
            Cancel,
            Skip,
        };

        /// How a plan's scenario ended.
        enum class Outcome : uint8
        {
            None,           // pending or running
            Done,           // its learner finished and moved on, or its local episodes ran
            Skipped,
            Failed,         // it could not start or run
            Cancelled,
        };

        /// "done", "below target", ...; "not started" for Outcome::None.
        [[nodiscard]] static char const* OutcomeName(Outcome outcome);

        struct PlanEntry
        {
            std::string Scenario;
            bool Resume = false;
            Outcome Result = Outcome::None;
            /// Settings this entry runs with, when they are not the plan's (a benchmark trial's envs and learner).
            std::optional<ForgeConfig> Config = std::nullopt;
            /// MapUpdate.Threads for this entry; 0 = leave the pool as it is (every entry but a benchmark trial's).
            uint32 MapThreads = 0;
        };

        /// One `forge bench` trial: settings, and what they ran at.
        struct BenchTrial
        {
            uint32 MapThreads = 0;
            uint32 Envs = 0;
            uint32 TorchThreads = 0;        // learner trials only; 0 = torch's own default
            bool Learner = false;           // false: a local policy, so the sim alone is timed
            uint32 Agents = 1;              // seats per env of the benchmarked scenario
            double EnvStepsPerSecond = 0.0;
            double WorkerEnvStepsPerSecond = 0.0;   // a cluster host's learner trials: the workers' own, as reported
            double WorldMsPerTick = 0.0;    // map update and the rest of the world tick
            double SimMsPerTick = 0.0;      // observing, rewarding and applying actions
            double LearnerMsPerTick = 0.0;  // blocked on the learner
            uint64 MemoryMb = 0;            // the worldserver's resident memory at the end of the trial
            uint32 Groups = 1;              // groups the pool was sent in: 2 when half-batch applied, else 1
            double ObjectsMs = 0.0;         // UpdateNonPlayerObjects, thread time per decision over every map
            double ResetMs = 0.0;           // rebuilding ended episodes on the world thread, per decision
            double SpawnUpdates = 0.0;      // world spawns updated per decision, over every map
            double UnseenSpawns = 0.0;      // world spawns left alone per decision (no player shares their phase)
            double OtherUpdates = 0.0;      // other non-player objects updated per decision
            double SpawnMs = 0.0;           // ... and their thread time per decision
            double OtherMs = 0.0;
            double SendMs = 0.0;            // SendObjectUpdates, thread time per decision
            double TasksPerUpdate = 0.0;    // map tasks per map update (MapMgr::TaskTiming), and per update:
            double TaskSumMs = 0.0;         // ... their summed wall time
            double TaskLongestMs = 0.0;     // ... the longest one
            double TaskWallMs = 0.0;        // ... the whole schedule-and-join
            int32 SlowestCpu = -1;          // the last update's longest task ran here
            uint64 CpuMask = 0;             // and its tasks started on these
            uint32 WarmupTicks = 0;         // the window this trial ran: the sim grid and the learner phase differ
            uint32 MeasureTicks = 0;
            bool Measured = false;
            std::string Note;               // why it was not measured
        };

        /// Scenarios run one after another.
        struct Plan
        {
            std::vector<PlanEntry> Entries;
            uint32 Index = 0;
            std::string Policy;             // "remote" trains; anything else runs locally
            uint64 LocalEpisodes = 0;       // local policy: episodes per scenario (0 = until cancelled)
            bool Fast = false;              // `forge fast`: trained with ForgeConfig::FastProfile
            /// Env steps each stage of a fast run trains for (`forge fast <steps>`); 0 = AnimusForge.Fast.Budget.
            uint64 Budget = 0;

            [[nodiscard]] bool Remote() const { return Policy == "remote"; }
        };

        Forge() = default;

        void ApplyRequest();
        void HoldWhilePaused();

        /// Build and start the plan's current scenario. False (logged) when it cannot start.
        bool StartCurrent();

        /// Tear the running scenario down. With `stopLearner` the learner is disconnected and waited for (it saves
        /// latest.pt when the socket closes); a learner that finished by itself is already gone.
        void TeardownScenario(bool stopLearner);

        /// The current scenario ended with `outcome`: tear it down and start the next one, or finish the plan.
        void FinishCurrent(Outcome outcome);

        /// The plan stops here (finished, cancelled or failed); the sim goes idle.
        void EndPlan(char const* reason);

        /// The running scenario's auto-started learner finished its run and moved on (exit 0: every class converged,
        /// or its step limit reached).
        [[nodiscard]] bool LearnerFinished() const;

        /// AnimusForge.Queue, or every curriculum stage in order when it is empty.
        [[nodiscard]] std::vector<std::string> DefaultQueue() const;
        /// AnimusForge.Fast.Queue, or every curriculum stage in order (pilots too) when it is empty.
        [[nodiscard]] std::vector<std::string> FastQueue() const;

        /// The run of `scenario` finished (<RunsDir>/<scenario>/finished.json): it converged or reached its budget.
        [[nodiscard]] bool RunAdvanced(ForgeConfig const& config, std::string const& scenario) const;
        /// Whether a stage has a checkpoint the learner would seed from, which is not the same question.
        [[nodiscard]] bool RunSeedable(ForgeConfig const& config, std::string const& scenario) const;

        /// Warn about stages listed before the stage they extend and seed from (unless that one already advanced).
        void WarnSeedOrder(ForgeConfig const& config, std::vector<std::string> const& scenarios,
            LineSink const& out) const;

        /// The settings a plan runs with: the configured ones, or the fast profile for `forge fast`.
        [[nodiscard]] ForgeConfig const& ConfigFor(Plan const& plan) const { return plan.Fast ? _fastConfig : _config; }

        /// The settings of the running (or last started) plan: a benchmark trial's own, else the plan's.
        [[nodiscard]] ForgeConfig const& RunConfig() const
        {
            PlanEntry const& entry = _plan.Entries[_plan.Index];
            return entry.Config ? *entry.Config : ConfigFor(_plan);
        }

        /// The world thread was blocked on the learner from `from` until now.
        void WaitedForLearner(std::chrono::steady_clock::time_point from);

        /// Set the map update pool's thread count (a benchmark trial's, or the configured one again). Between ticks
        /// only: it joins the pool's threads and starts new ones.
        void ApplyMapThreads(uint32 threads);
        /// MapUpdate.Threads as configured, which every trial is restored to when the benchmark ends.
        [[nodiscard]] static uint32 ConfiguredMapThreads();

        /// Every trial the benchmark still has to run, as a plan (phase 1: the sim alone).
        [[nodiscard]] Plan BenchPlan(std::string const& scenario, std::vector<BenchTrial> const& trials) const;
        /// The running trial: nothing until its warm-up is over, then its timing, then the next trial.
        void BenchTick();
        /// The benchmark's plan ended: start the learner phase, or report and stop.
        void BenchPlanEnded();
        void BenchReport(LineSink const& out) const;
        /// <bench>/bench.json: every trial, the winner and the machine, for `forge bench apply`.
        void BenchSave() const;
        /// Give up on the benchmark: the map update pool goes back to the configured thread count.
        void BenchEnd();

        /// Console commands, the export process and the periodic report, while the world thread waits.
        void Pump();
        void PollExport();
        void MaybeReport();

        /// Cluster: take the registrations and orders that have come in; a worker acts on its host's orders.
        void PollCluster();
        /// A worker's plan for the scenario its host ordered: remote policy, no learner of its own, its sim on TCP.
        [[nodiscard]] Plan WorkerPlan(std::string const& scenario, bool resume, bool fast) const;

        void LocalDecision(uint32 group);
        void RemoteDecision(uint32 group);
        bool SendSpec(uint32 rank);
        /// Host: each connected worker and what it last reported (forge status).
        void ReportWorkers(LineSink const& out) const;
        /// The GPU mode's learner count (ForgeConfig::LearnerRanks) as this pool can take it: every rank needs an env
        /// of every group. The learners started and the connections awaited both use it, so the two always agree.
        [[nodiscard]] uint32 PoolRanks(uint32 wanted) const;
        bool SendStep(uint32 group);
        /// Data-parallel learners (multi-GPU mode): rank `rank`'s share of group `group`, as the first
        /// env of the pool, how many, and the first env in that learner's own numbering (its envs are its share of
        /// each group, one after another).
        struct RankRows
        {
            uint32 Global = 0;
            uint32 Count = 0;
            uint32 Local = 0;
        };
        [[nodiscard]] RankRows RankGroup(uint32 rank, uint32 group) const;
        [[nodiscard]] uint32 RankEnvs(uint32 rank) const;
        /// Every rank's MODE is in (they evaluate on the same update): the first sets the mode, each its seed run.
        bool ApplyModes(std::vector<ModeMsg> const& modes);
        /// After a reset (a new learner, a MODE): every group's STEP, and group 0's maps tick next.
        bool SendEveryGroup();
        bool ApplyMode(ModeMsg const& mode);

        /// Whether the running scenario has the local policy `policy` ("random" or one of its scripted policies).
        [[nodiscard]] bool KnowsPolicy(std::string const& policy) const;

        [[nodiscard]] SimSnapshot Snapshot(bool advanceRates);
        [[nodiscard]] std::vector<PlanRow> PlanRows(Plan const& plan, bool live) const;
        [[nodiscard]] std::string StateName() const;
        /// The progress report at the end of the running stage (whatever AnimusForge.Progress.Interval is).
        void ReportStageEnd();
        /// The fast profile in a few words: envs, level and classes.
        [[nodiscard]] std::string FastSummary() const;
        [[nodiscard]] bool Enabled(LineSink const& out) const;
        [[nodiscard]] bool ValidScenario(std::string const& scenario, LineSink const& out) const;

        ForgeConfig _config;
        /// _config.FastProfile(budget): rebuilt when `forge fast <steps>` names a budget of its own, so
        /// ConfigFor(plan) hands the learner the budget that invocation asked for.
        ForgeConfig _fastConfig;
        std::unique_ptr<Animus::Scenario> _scenario;
        std::unique_ptr<Animus::EnvPool> _pool;
        LockstepServer _server;
        LearnerProcess _learner;
        ChildProcess _export{ "Export" };
        ProgressMonitor _monitor;

        State _state = State::Idle;
        State _pausedFrom = State::Idle;
        bool _pauseRequested = false;
        bool _resumeRequested = false;
        bool _pumping = false;
        bool _learnerStarted = false;      // the auto-started learner belongs to the running scenario

        Request _request = Request::None;
        Plan _requested;
        Plan _plan;
        std::optional<Plan> _lastPlan;     // the last plan that ended, for `forge resume` without arguments

        uint64 _ticks = 0;                  // decisions since the scenario started, not world updates
        uint32 _ticksSinceDecision = 0;     // world updates since the last decision (< TicksPerDecision)
        /// Half-batch (AnimusForge.HalfBatch with TicksPerDecision 1): the world ticks at half a decision and the
        /// pool's two groups' maps take turns; `_turn` is the group whose maps tick this world tick and decide at
        /// its end, `_nextTurn` the next one's. Without half-batch the one group ticks every world tick.
        ClusterLink _cluster;
        /// Worker: the scenario the host ordered, to start once the one running has been torn down.
        std::optional<Plan> _clusterOrder;
        /// Host: the workers' sims the running scenario's learner trains on, and the order that started them, for
        /// one that drops out and registers again to be sent straight back to it.
        std::vector<std::string> _clusterSims;
        /// Worker: when its next PROGRESS goes to the host.
        std::chrono::steady_clock::time_point _nextClusterReport{};
        std::string _clusterStart;
        bool _halfBatch = false;
        uint32 _turn = 0;
        uint32 _nextTurn = 0;
        /// A group's STEP went to the learner and its answer has not come back.
        bool _awaitingAnswer[2] = { false, false };
        uint32 _ranks = 1;                  // learners connected to this sim's pool
        /// This tick ends a decision: set by OnWorldPrologue, read by the map epilogues that score and observe
        /// on it, cleared when OnUpdate closes the decision.
        ///
        /// Plain bools although map workers read them: both are written before any map task is pushed and read
        /// only from inside a task, so the scheduler's release on publishing a task and the worker's acquire on
        /// claiming it publish them; the join in MapUpdater::wait() orders the next write after every read.
        bool _decisionTick = false;
        /// A decision has filled Actions and no map has applied them yet. The prologue hands it to _applyTick,
        /// so the maps of exactly one tick apply a decision's actions, whatever TicksPerDecision is.
        bool _actionsPending[2] = { false, false };        // per group
        bool _applyTick = false;
        uint64 _decisions = 0;
        /// SendStep's gather of the ended envs' final obs and state (protocol 14), kept to reuse the allocations.
        std::vector<float> _endedObs;
        std::vector<float> _endedState;
        bool _tickMismatchLogged = false;   // a world tick other than ForgeConfig::TickMs was reported once
        uint32 _progressInterval = 0;

        std::chrono::steady_clock::time_point _scenarioStarted;
        std::chrono::steady_clock::time_point _lastReport;
        std::optional<std::chrono::steady_clock::time_point> _lastAct;

        /// Where a tick's wall time goes, since the scenario started (ns). Reported per tick, and what the benchmark
        /// compares settings by.
        uint64 _worldNs = 0;            // between one decision and the next: the map update and the world tick
        uint64 _simNs = 0;              // this module's own work in the tick
        uint64 _learnerNs = 0;          // blocked on the learner (waiting for it to connect, or for its actions)
        uint64 _tickLearnerNs = 0;      // ... of the tick being processed
        std::optional<std::chrono::steady_clock::time_point> _lastUpdateEnd;

        std::vector<BenchTrial> _benchTrials;       // phase 1 and, once it is over, phase 2
        std::size_t _benchTrial = 0;                // the trial the running plan entry is
        bool _benching = false;
        bool _benchLearnerPhase = false;
        std::string _benchScenario;
        std::chrono::steady_clock::time_point _benchMeasuredFrom;
        uint64 _benchWorldNs = 0;       // the counters when the measurement started
        uint64 _benchSimNs = 0;
        uint64 _benchLearnerNs = 0;
        uint64 _benchObjectsNs = 0;
        uint64 _benchResetNs = 0;
        Map::UpdateTiming _benchMapTiming;
        MapMgr::TaskTiming _benchTaskTiming;

        std::chrono::steady_clock::time_point _rateTime;
        uint64 _rateTicks = 0;
        uint64 _rateEpisodes = 0;
        uint64 _rateWorldNs = 0;
        uint64 _rateSimNs = 0;
        uint64 _rateLearnerNs = 0;

        /// The pool's own per-decision timings, totalled since the scenario started and at the last rate window.
        Animus::EnvPool::CollectTiming _collect;
        Animus::EnvPool::CollectTiming _rateCollect;
        Map::UpdateTiming _rateMapTiming;
        MapMgr::TaskTiming _rateTaskTiming;
        SimSnapshot::WorldMs _worldMs;
        SimSnapshot::MapTasksMs _mapTasks;
        SimSnapshot::CollectMs _collectMs;
        /// SeatEncoder::ObserveNs at the last report, and per decision since it (for `forge status`).
        std::array<uint64, Animus::Curriculum::SeatEncoder::OBSERVE_SLOTS> _rateObserveNs{};
        std::vector<std::pair<std::string, double>> _observeBlockMs;
        double _ticksPerSecond = 0.0;
        double _episodesPerSecond = 0.0;
        double _worldMsPerTick = 0.0;
        double _simMsPerTick = 0.0;
        double _learnerMsPerTick = 0.0;

        std::string _exportScenario;
        std::string _exportModelDir;

        /// The scenario running, or the last one started.
        std::string _current;
    };
}

#define sAnimusForge AnimusForge::Forge::Instance()

#endif
