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

#include "AnimusForge.h"
#include "GpuRuntime.h"
#include "ProbeBake.h"
#include "SeatEncoder.h"
#include "WarmCaches.h"
#include "Forge.h"
#include "AnimusHooks.h"
#include "Config.h"
#include "Log.h"
#include "MapMgr.h"
#include "MapUpdater.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <cstdio>
#include <boost/json/object.hpp>
#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
    /// How long an idle or paused world thread sleeps per tick: an idle sim would otherwise spin a core.
    constexpr std::chrono::milliseconds IDLE_SLEEP{ 50 };

    /// How long a cancelled or skipped learner gets to save its checkpoint before it is interrupted.
    constexpr std::chrono::seconds LEARNER_STOP_GRACE{ 15 };

    void LogInfo(std::string const& line)
    {
        LOG_INFO("module.animus", "{}", line);
    }

    void LogWarn(std::string const& line)
    {
        LOG_WARN("module.animus", "{}", line);
    }

    /// The worldserver's resident memory, MB (0 when /proc is not there).
    uint64 ResidentMb()
    {
        std::ifstream status("/proc/self/status");
        for (std::string line; std::getline(status, line);)
            if (line.rfind("VmRSS:", 0) == 0)
                return uint64(std::strtoull(line.c_str() + 6, nullptr, 10) / 1024);

        return 0;
    }

    /// How much of the machine's memory is in use, percent (0 when /proc is not there).
    uint32 MemoryUsedPercent()
    {
        uint64 total = 0;
        uint64 available = 0;
        std::ifstream meminfo("/proc/meminfo");
        for (std::string line; std::getline(meminfo, line);)
        {
            if (line.rfind("MemTotal:", 0) == 0)
                total = std::strtoull(line.c_str() + 9, nullptr, 10);
            else if (line.rfind("MemAvailable:", 0) == 0)
                available = std::strtoull(line.c_str() + 13, nullptr, 10);
        }

        return total ? uint32(100 - std::min<uint64>(100, available * 100 / total)) : 0;
    }
}

AnimusForge::Forge* AnimusForge::Forge::Instance()
{
    static Forge instance;
    return &instance;
}

void AnimusForge::Forge::OnStartup()
{
    _config.Load();
    Animus::Curriculum::ProbeBake::Store::Configure(_config.ProbeBaked, _config.ProbeDir,
        _config.ProbeCacheGrids);
    // Before anything HIP starts, here or in the learner spawned later, which inherits it.
    Animus::Gpu::PrepareEnvironment();
    _fastConfig = _config.FastProfile(_config.FastBudget);
    _progressInterval = _config.ProgressInterval;

    if (!_config.Enable)
    {
        LOG_INFO("module.animus", "Animus Forge is disabled (AnimusForge.Enable = 0)");
        return;
    }

    if (_config.GpuObserve && _config.Policy == "remote")
    {
        std::string why;
        if (Animus::Gpu::Load(_config.LearnerPython, _config.LearnerWorkDir, why))
            LOG_INFO("module.animus", "Device library loaded: observations can be written on the GPU");
        else
            LOG_WARN("module.animus", "No device library, observations stay on the CPU: {}", why);
    }

    LOG_INFO("module.animus", "GPU mode: {}", _config.GpuSummary);

    if (_config.Cluster == ForgeConfig::ClusterRole::Host)
        _cluster.Listen(_config.ClusterControlPort);
    else if (_config.Cluster == ForgeConfig::ClusterRole::Worker)
        _cluster.Join(_config.ClusterHost, _config.ClusterDataPort, _config.ClusterAdvertise);

    // Every world table the curriculum reads on first use, read now: the database pools are sealed right after
    // this and an episode must never query.
    Animus::Curriculum::WarmCaches();

    // Open the socket now, so a learner started by hand can connect as soon as a plan starts. Not on a worker: its
    // sim listens on TCP for the host's learner once ordered onto a scenario, and never has a learner of its own.
    if (_config.IsRemote() && _config.Cluster != ForgeConfig::ClusterRole::Worker)
        _server.Listen(_config.SocketPath);

    LOG_INFO("module.animus", "Animus Forge is idle. Type `forge start` on the console to train AnimusForge.Queue, "
        "or `forge help` for every command.");
    CommandStatus(LogInfo);
}

void AnimusForge::Forge::OnWorldPrologue(uint32 diff)
{
    _applyTick = false;

    if (!_config.Enable || !_pool)
        return;

    // Only a running scenario has envs on maps. A pause or a cancel recorded since the last tick is applied by
    // OnUpdate below, so the state read here is the one the last tick ended in.
    if (_state != State::Training && _state != State::Running)
        return;

    // Half-batch: the groups' maps take turns, one per world tick, and a group's maps tick with the time of both
    // (MapMgr::ForgeTickDiff), so its envs' clocks move a whole TickMs. Otherwise the one group ticks every tick.
    _turn = _halfBatch ? _nextTurn : 0;
    _nextTurn = _halfBatch ? (_turn + 1) % 2 : 0;
    if (_turn >= _pool->GroupCount())
    {
        // Half-batch fell back to one group (GroupsKeepToTheirMaps): this world tick is the empty half.
        _decisionTick = false;
        return;
    }

    // Game time accrues every tick, whether or not the policy chose on this one: an episode's clock, and
    // everything the library measures against it, is in game milliseconds and does not care how often anyone
    // decides. It is advanced before the maps tick because the terminal check that follows them reads it.
    _pool->AdvanceClock(_turn, _halfBatch ? 2 * diff : diff);

    // Above TicksPerDecision = 1 the world runs several times between decisions. The intervening ticks move
    // splines, auras and the fight at the finer step and are otherwise silent -- no observation, no action, no
    // learner. That is the whole point: movement wants a fast world, the policy does not want a faster decision.
    // Counting ticks rather than accumulating milliseconds keeps a decision exactly TicksPerDecision ticks
    // whatever the tick rounds to.
    _decisionTick = ++_ticksSinceDecision >= RunConfig().TicksPerDecision;
    if (_decisionTick)
        _pool->BeginDecision(_turn);

    // The actions of the group's last decision are applied by the maps of this tick, and of no other.
    _applyTick = _actionsPending[_turn];
    _actionsPending[_turn] = false;
}

bool AnimusForge::Forge::IsMapFrozen(Map const& map) const
{
    if (!_halfBatch || !_pool || (_state != State::Training && _state != State::Running))
        return false;

    int32 const group = _pool->GroupOfMap(map);
    return group >= 0 && uint32(group) != _turn;
}

void AnimusForge::Forge::OnMapPrologue(Map& map)
{
    if (_applyTick && _pool)
        _pool->ApplyActionsForMap(map);
}

void AnimusForge::Forge::OnMapEpilogue(Map& map)
{
    if (_decisionTick && _pool)
        _pool->ObserveMap(map);
}

void AnimusForge::Forge::OnUpdate(uint32 diff)
{
    if (!_config.Enable)
        return;

    // Everything between the end of the last decision and here is the world tick: the map update above all.
    auto const tickStarted = std::chrono::steady_clock::now();
    if (_lastUpdateEnd && _state != State::Idle && _state != State::Paused)
        _worldNs += uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(tickStarted - *_lastUpdateEnd).count());
    _tickLearnerNs = 0;

    PollExport();
    PollCluster();
    ApplyRequest();

    // A worker ordered onto another scenario starts it once the one it was running has been torn down above.
    if (_clusterOrder && _state == State::Idle && _request == Request::None)
    {
        _requested = std::move(*_clusterOrder);
        _clusterOrder.reset();
        _request = Request::Start;
    }

    if (_pauseRequested && (_state == State::Training || _state == State::Running))
    {
        _pauseRequested = false;
        _pausedFrom = _state;
        _state = State::Paused;
        LOG_INFO("module.animus", "Paused {}: the sim is frozen{}. `forge resume` continues, `forge cancel` stops.",
            _current, _plan.Remote() ? " and the learner waits" : "");
    }

    // A decision the prologue opened on the strength of the last tick's state: the maps have already scored and
    // observed into the pool, so it is closed here whatever this tick decides to do instead of finishing it.
    auto const abandonDecision = [this]()
    {
        _decisionTick = false;
        if (_pool)
            _pool->FinishCollect();
    };

    if (_state == State::Paused)
    {
        abandonDecision();
        HoldWhilePaused();
        return;
    }

    if (_state == State::Idle)
    {
        abandonDecision();
        std::this_thread::sleep_for(IDLE_SLEEP);
        return;
    }

    ForgeConfig const& run = RunConfig();

    // The forge core sizes its tick from the same keys; a different tick means a worldserver built before they
    // were one, and every reward scaled per decision would be off.
    if (diff != _config.WorldTickMs() && !ForgeCore::Playtest() && !_tickMismatchLogged)
    {
        _tickMismatchLogged = true;
        LOG_ERROR("module.animus", "The world ticks {} ms, but AnimusForge.DecisionMs {} over TicksPerDecision {}{} "
            "wants {} ms: rebuild the worldserver (./forge.sh --build)", diff, run.DecisionMs, run.TicksPerDecision,
            _config.HalvesTick() ? ", halved for AnimusForge.HalfBatch," : "", _config.WorldTickMs());
    }

    // The clock and the count of ticks to a decision are the prologue's, before the maps tick: what is left here
    // is the decision itself, on a world thread the maps have rejoined.
    bool const decided = _decisionTick;
    // A decision of the whole pool is every group's: in half-batch, the second half's ends it.
    bool const counted = decided && _pool && _turn + 1 >= _pool->GroupCount();
    if (decided)
    {
        _decisionTick = false;
        _ticksSinceDecision = 0;

        // _ticks counts decisions, not world updates: it is the denominator of every per-decision figure in the
        // report (EnvStepsPerSecond, the ms-per-tick buckets) and of the bench's measurement window.
        if (counted)
        {
            ++_ticks;
            MaybeReport();
        }

        if (_plan.Remote())
            RemoteDecision(_turn);
        else
            LocalDecision(_turn);

        // What the pool spent this decision on, totalled for the report.
        if (_pool)
        {
            Animus::EnvPool::CollectTiming const& collect = _pool->LastCollect();
            _collect.RewardNs += collect.RewardNs;
            _collect.ObserveNs += collect.ObserveNs;
            _collect.FinalObserveNs += collect.FinalObserveNs;
            _collect.ResetNs += collect.ResetNs;
            _collect.ResetCreateNs += collect.ResetCreateNs;
            _collect.ResetPlaceNs += collect.ResetPlaceNs;
            _collect.ResetConfigureNs += collect.ResetConfigureNs;
            _collect.ResetDestroyNs += collect.ResetDestroyNs;
            _collect.ResetEncounterNs += collect.ResetEncounterNs;
            _collect.ResetScatterNs += collect.ResetScatterNs;
            _collect.ResetStockNs += collect.ResetStockNs;
            _collect.ResetPrepareNs += collect.ResetPrepareNs;
            _collect.ResetSeatsNs += collect.ResetSeatsNs;
            _collect.ResetDespawnNs += collect.ResetDespawnNs;
            _collect.ResetScenarioNs += collect.ResetScenarioNs;
            _collect.Reused = collect.Reused;       // the pool's count is cumulative already
            _collect.ApplyNs += collect.ApplyNs;
            _collect.Observes += collect.Observes;
            _collect.Resets += collect.Resets;
            _collect.MapResets += collect.MapResets;
            _collect.MapResetNs += collect.MapResetNs;
        }
    }

    // What is left of this module's time in the tick, once the waiting on the learner is taken out.
    auto const tickEnded = std::chrono::steady_clock::now();
    uint64 const inModule =
        uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnded - tickStarted).count());
    _simNs += inModule > _tickLearnerNs ? inModule - _tickLearnerNs : 0;
    _lastUpdateEnd = tickEnded;

    if (_benching && counted)
        BenchTick();
}

void AnimusForge::Forge::OnShutdown()
{
    // The core's damage, heal and spell calls stop feeding the pool before it goes.
    Animus::Hooks::SetActivePool(nullptr);

    // Closing the socket is what tells the learner to save and exit; give it time to do so.
    if (_learner.IsRunning())
        _learner.ExpectExit();

    // An export cut short by the shutdown is expected, not a failure.
    if (_export.IsRunning())
        _export.ExpectExit();

    _server.Shutdown();
    _learner.Stop(std::chrono::seconds(10));
    _export.Stop(std::chrono::seconds(10));

    if (_pool)
        _pool->Teardown();

    _pool.reset();
    _scenario.reset();
    _state = State::Idle;
}

void AnimusForge::Forge::ApplyRequest()
{
    Request const request = _request;
    _request = Request::None;

    switch (request)
    {
        case Request::None:
            break;
        case Request::Start:
            _plan = std::move(_requested);
            _requested = {};
            _plan.Index = 0;
            LOG_DEBUG("module.animus", "Plan: {} scenario{} with policy {}", _plan.Entries.size(),
                _plan.Entries.size() == 1 ? "" : "s", _plan.Policy);
            if (!StartCurrent())
            {
                _plan.Entries[_plan.Index].Result = Outcome::Failed;
                TeardownScenario(true);
                EndPlan("its first scenario failed to start");
            }
            break;
        case Request::Cancel:
        {
            if (_state == State::Idle)
                break;
            // Only a learner that is running or connected has a run to save.
            bool const learnerSaves = _plan.Remote() && (_learner.IsRunning() || _server.HasClient());
            if (_benching)
                BenchEnd();
            _plan.Entries[_plan.Index].Result = Outcome::Cancelled;
            TeardownScenario(true);
            EndPlan(learnerSaves ? "cancelled; the learner saved latest.pt, `forge resume` continues it" : "cancelled");
            break;
        }
        case Request::Skip:
            if (_state != State::Idle)
                FinishCurrent(Outcome::Skipped);
            break;
    }
}

void AnimusForge::Forge::HoldWhilePaused()
{
    // Nothing ticks while paused: maps, episode clocks and the learner all wait, so a paused episode resumes
    // exactly where it stopped. Console commands still run here.
    while (_state == State::Paused && !World::IsStopped())
    {
        if (_resumeRequested)
        {
            _resumeRequested = false;
            _state = _pausedFrom;
            _lastReport = std::chrono::steady_clock::now();
            _rateTime = _lastReport;
            _rateTicks = _ticks;
            _rateEpisodes = _pool ? _pool->CompletedEpisodes() : 0;
            if (_lastAct)
                _lastAct = _lastReport;
            LOG_INFO("module.animus", "Resumed {}", _current);
            return;
        }

        if (_request != Request::None)
        {
            ApplyRequest();
            return;
        }

        _learner.Poll();
        Pump();
        std::this_thread::sleep_for(IDLE_SLEEP);
    }
}

void AnimusForge::Forge::ReportProbeTables(std::string const& scenario) const
{
    // Only counted, never made: the tables ship with the forge, and a grid without one is measured live.
    namespace Bake = Animus::Curriculum::ProbeBake;
    Animus::Curriculum::StageDefinition const* stage = Animus::Curriculum::FindStage(scenario);
    if (!Bake::Store::Baked() || !stage)
        return;

    std::vector<Bake::GridRef> const grids = Bake::StageGrids(*stage);
    uint32 shipped = 0;
    std::error_code error;
    for (Bake::GridRef const& grid : grids)
        shipped += std::filesystem::exists(Bake::Store::FileFor(grid.MapId, grid.X, grid.Y), error) ? 1 : 0;
    if (shipped == grids.size())
        LOG_INFO("module.animus", "Ground probe for {}: all {} grids have tables", scenario, grids.size());
    else
        LOG_WARN("module.animus", "Ground probe for {}: {} of {} grids have tables in {}; seats on the others use the "
            "live probe. Bake them with `forge probestage {}` and ship the files.", scenario, shipped, grids.size(),
            Bake::Store::Dir(), scenario);
}

bool AnimusForge::Forge::StartCurrent()
{
    // A stage none of this run's classes can play (the stealth drill in a run of classes that cannot stealth) is
    // skipped rather than failed: the plan moves to the next entry, and that entry's learner seeds from the stage
    // before the skipped one (its seed chain walks past a checkpoint that lacks a layout). A plan whose remaining
    // entries are all skipped ends here, as it would after its last scenario.
    while (!_scenario)
    {
        PlanEntry const& skipped = _plan.Entries[_plan.Index];
        std::unique_ptr<Animus::Scenario> scenario = Animus::CreateScenario(skipped.Scenario,
            RunConfig().Stage(skipped.Scenario));
        if (!scenario)
        {
            LOG_ERROR("module.animus", "Unknown scenario '{}'", skipped.Scenario);
            return false;
        }
        if (scenario->Playable())
        {
            _scenario = std::move(scenario);
            ReportProbeTables(skipped.Scenario);
            break;
        }

        LOG_INFO("module.animus", "Skipping {}: none of this run's classes can play it", skipped.Scenario);
        _plan.Entries[_plan.Index].Result = Outcome::Skipped;
        if (_plan.Index + 1 >= _plan.Entries.size())
        {
            EndPlan("every scenario has ended");
            return true;
        }
        ++_plan.Index;
    }

    PlanEntry const& entry = _plan.Entries[_plan.Index];
    ForgeConfig const& config = RunConfig();
    _current = entry.Scenario;

    std::string const position = _plan.Entries.size() > 1
        ? Acore::StringFormat(" ({} of {})", _plan.Index + 1, _plan.Entries.size()) : "";

    LOG_INFO("module.animus", "Starting {}{}{} with policy {}{}", entry.Scenario, position,
        entry.Resume ? ", resuming its latest checkpoint" : "", _plan.Policy, _plan.Fast
        ? Acore::StringFormat(" (fast: {}; in {})", FastSummary(), config.OutputDir) : "");

    // Reject a local policy name before building anything, rather than on the first decision.
    if (!_plan.Remote() && !KnowsPolicy(_plan.Policy))
    {
        LOG_ERROR("module.animus", "Scenario {} has no policy '{}'", _scenario->Name(), _plan.Policy);
        return false;
    }

    // A benchmark trial runs at its own map update thread count; every other entry leaves the pool alone.
    if (entry.MapThreads)
        ApplyMapThreads(entry.MapThreads);

    _pool = std::make_unique<Animus::EnvPool>(*_scenario, config.Stage(entry.Scenario));
    if (!_pool->Setup())
        return false;

    _learnerStarted = false;
    if (_plan.Remote())
    {
        if (!_server.Listen(config.SocketPath))
            return false;

        // A learner that cannot be started is not fatal: the sim keeps waiting on the socket, so one started by
        // hand still works (and `forge cancel` gives up).
        // A cluster host's workers run the same scenario, and its learner trains on their sims as well as this one --
        // a bench's learner trials too, so `forge bench` on a host measures the cluster (a worker already running the
        // scenario carries on from one trial to the next).
        ForgeConfig learnerConfig = config;
        learnerConfig.LearnerRanks = PoolRanks(config.LearnerRanks);
        if (_config.Cluster == ForgeConfig::ClusterRole::Host)
        {
            _cluster.Poll();
            _cluster.TakeRegistrations();       // the workers registered by now are in this scenario from the start
            learnerConfig.ClusterSims = _cluster.WorkerSims();
            _clusterSims = learnerConfig.ClusterSims;
            _clusterStart = Acore::StringFormat("START {} 0 {}", entry.Scenario, _plan.Fast ? 1 : 0);
            _cluster.Broadcast(Acore::StringFormat("START {} {} {}", entry.Scenario, entry.Resume ? 1 : 0,
                _plan.Fast ? 1 : 0));
            if (!learnerConfig.ClusterSims.empty())
                LOG_INFO("module.animus", "Cluster: {} worker{} run{} {} too", learnerConfig.ClusterSims.size(),
                    learnerConfig.ClusterSims.size() == 1 ? "" : "s", learnerConfig.ClusterSims.size() == 1 ? "s" : "",
                    entry.Scenario);
        }

        if (config.LearnerAutoStart)
        {
            _learnerStarted = _learner.Start(learnerConfig, entry.Scenario, entry.Resume);
            if (!_learnerStarted)
                LOG_ERROR("module.animus", "Learner auto-start failed; start it by hand: {}",
                    LearnerProcess::ManualCommand(learnerConfig, entry.Scenario, entry.Resume));
        }
        else if (_config.Cluster == ForgeConfig::ClusterRole::Worker)
            LOG_INFO("module.animus", "Cluster: waiting for the host's learner on {}", config.SocketPath);
        else
            LOG_INFO("module.animus", "Waiting for a learner started by hand: {}",
                LearnerProcess::ManualCommand(learnerConfig, entry.Scenario, entry.Resume));
    }

    _pool->ResetAll();

    // Half-batch: two halves, provided no map holds envs of both (a continent's replicas are dealt contiguous
    // blocks, so the split has to fall on a replica boundary). Otherwise one group, which ticks on every other
    // world tick: the world still runs at half a decision, and the decisions are what they are without it.
    _halfBatch = _config.HalvesTick();
    _pool->SetGroups(_halfBatch ? _pool->NumEnvs() / 2 : _pool->NumEnvs());
    if (_halfBatch && (_pool->NumEnvs() < 2 || !_pool->GroupsKeepToTheirMaps()))
    {
        LOG_ERROR("module.animus", "AnimusForge.HalfBatch: {} envs of {} do not split into two halves on separate "
            "maps (make AnimusForge.ContinentReplicas even and a divisor of the envs); running them as one group",
            _pool->NumEnvs(), entry.Scenario);
        _pool->SetGroups(_pool->NumEnvs());
    }
    _turn = _nextTurn = 0;
    _awaitingAnswer[0] = _awaitingAnswer[1] = false;
    _actionsPending[0] = _actionsPending[1] = false;

    auto const now = std::chrono::steady_clock::now();
    _ticks = 0;
    _ticksSinceDecision = 0;
    _decisions = 0;
    _worldNs = 0;
    _simNs = 0;
    _learnerNs = 0;
    _tickLearnerNs = 0;
    _lastUpdateEnd.reset();
    _rateWorldNs = 0;
    _rateSimNs = 0;
    _rateLearnerNs = 0;
    _collect = Animus::EnvPool::CollectTiming();
    _rateCollect = Animus::EnvPool::CollectTiming();
    _collectMs = SimSnapshot::CollectMs();
    _mapTasks = SimSnapshot::MapTasksMs();
    _scenarioStarted = now;
    _lastReport = now;
    _lastAct.reset();
    _rateTime = now;
    _rateTicks = 0;
    _rateEpisodes = 0;
    _ticksPerSecond = 0.0;
    _episodesPerSecond = 0.0;
    _worldMsPerTick = 0.0;
    _simMsPerTick = 0.0;
    _learnerMsPerTick = 0.0;
    _monitor.Begin(entry.Scenario);

    // From here on the core's damage, heal and spell calls feed the pool.
    Animus::Hooks::SetActivePool(_pool.get());
    _state = _plan.Remote() ? State::Training : State::Running;
    return true;
}

void AnimusForge::Forge::TeardownScenario(bool stopLearner)
{
    // The core's damage, heal and spell calls stop feeding the pool before it goes.
    Animus::Hooks::SetActivePool(nullptr);

    if (stopLearner && _learner.IsRunning())
    {
        _learner.ExpectExit();
        _server.DropClient();
        LOG_INFO("module.animus", "Waiting for the learner (pid {}) to save and exit...", _learner.Pid());
        _learner.Stop(LEARNER_STOP_GRACE);
    }
    else
        _server.DropClient();

    if (_pool)
        _pool->Teardown();

    _pool.reset();
    _scenario.reset();
    _learnerStarted = false;
    _pauseRequested = false;
    _resumeRequested = false;
    _lastAct.reset();
}

char const* AnimusForge::Forge::OutcomeName(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::None:        return "not started";
        case Outcome::Done:        return "done";
        case Outcome::Skipped:     return "skipped";
        case Outcome::Failed:      return "failed";
        case Outcome::Cancelled:   return "cancelled";
    }

    return "unknown";
}

void AnimusForge::Forge::FinishCurrent(Outcome outcome)
{
    PlanEntry& entry = _plan.Entries[_plan.Index];
    entry.Result = outcome;

    ReportStageEnd();
    LOG_INFO("module.animus", "{} {} after {}{}", entry.Scenario, OutcomeName(outcome),
        Format::Duration(std::chrono::duration<double>(std::chrono::steady_clock::now() - _scenarioStarted).count()),
        _plan.Entries.size() > 1 ? Acore::StringFormat(" ({} of {})", _plan.Index + 1, _plan.Entries.size()) : "");

    // A learner that finished its run has already exited; any other ending stops it. A benchmark trial is the
    // exception that ends well with its learner still training -- its budget is one no trial ever reaches -- so
    // that one is stopped and waited for here, or the next trial finds it running and cannot start its own.
    TeardownScenario(outcome != Outcome::Done || _benching);

    if (++_plan.Index >= _plan.Entries.size())
    {
        _plan.Index = uint32(_plan.Entries.size()) - 1;
        EndPlan("every scenario has ended");
        return;
    }

    if (!StartCurrent())
    {
        _plan.Entries[_plan.Index].Result = Outcome::Failed;
        TeardownScenario(true);
        EndPlan("the next scenario failed to start");
    }
}

void AnimusForge::Forge::EndPlan(char const* reason)
{
    _state = State::Idle;
    _lastPlan = _plan;

    LOG_INFO("module.animus", "Plan ended: {}. The sim is idle.", reason);

    if (_config.Cluster == ForgeConfig::ClusterRole::Host)
    {
        _cluster.Broadcast("STOP");
        _clusterSims.clear();
        _clusterStart.clear();
    }

    if (_benching)
    {
        BenchPlanEnded();
        return;
    }

    if (_plan.Entries.size() > 1)
    {
        TextTable table({ { "#", TextTable::Align::Right }, { "Scenario" }, { "Outcome" } });
        for (std::size_t i = 0; i < _plan.Entries.size(); ++i)
            table.AddRow({ std::to_string(i + 1), _plan.Entries[i].Scenario, OutcomeName(_plan.Entries[i].Result) });

        table.Write(LogInfo, "  ");
    }
}

bool AnimusForge::Forge::LearnerFinished() const
{
    return _plan.Remote() && _learnerStarted && _learner.FinishedCleanly();
}

std::vector<std::string> AnimusForge::Forge::DefaultQueue() const
{
    if (!_config.Queue.empty())
        return _config.Queue;

    std::vector<std::string> stages;
    for (Animus::Curriculum::StageDefinition const& stage : Animus::Curriculum::CurriculumStages())
        if (stage.InDefaultQueue)
            stages.push_back(stage.Name);

    return stages;
}

std::vector<std::string> AnimusForge::Forge::FastQueue() const
{
    if (!_config.FastQueue.empty())
        return _config.FastQueue;

    std::vector<std::string> stages;
    for (Animus::Curriculum::StageDefinition const& stage : Animus::Curriculum::CurriculumStages())
        stages.push_back(stage.Name);

    return stages;
}

bool AnimusForge::Forge::RunAdvanced(ForgeConfig const& config, std::string const& scenario) const
{
    ProgressFile finished;
    if (!finished.Load(config.RunsDir() / scenario / "finished.json"))
        return false;

    // Every finished run advanced: there are no stage targets. A finished.json from before that, with
    // "advanced": false, is a run that was judged and failed under rules that no longer exist.
    std::optional<double> const advanced = finished.Number("advanced");
    return !advanced || *advanced != 0.0;
}

/// What the learner will actually seed from, which is the checkpoint and not the verdict.
///
/// animus.config.resolved_init_from walks the seed chain and takes the first best.pt that exists, whether or not
/// that stage finished -- an interrupted run does not write finished.json. RunAdvanced answers a different question
/// (did this stage finish), and using it here warned that a parent would not be seeded from whenever its run had
/// merely been cancelled, while the learner went on to seed from it: every `forge start stage19_duo_led` this
/// session printed that warning and then seeded from stage15_arena's best.pt in the next breath. A warning that is
/// usually wrong teaches operators to skip them.
bool AnimusForge::Forge::RunSeedable(ForgeConfig const& config, std::string const& scenario) const
{
    std::error_code error;
    return std::filesystem::exists(config.RunsDir() / scenario / "best.pt", error);
}

void AnimusForge::Forge::WarnSeedOrder(ForgeConfig const& config, std::vector<std::string> const& scenarios,
    LineSink const& out) const
{
    // A stage seeds from the closest stage it extends that has a checkpoint (and a merge from its other parents).
    for (std::size_t index = 0; index < scenarios.size(); ++index)
    {
        Animus::Curriculum::StageDefinition const* stage = Animus::Curriculum::FindStage(scenarios[index]);
        if (!stage || stage->Extends.empty())
            continue;

        std::vector<std::string> parents = { stage->Extends };
        parents.insert(parents.end(), stage->Merges.begin(), stage->Merges.end());
        for (std::string const& parent : parents)
        {
            // Trained earlier in this plan: it will have a checkpoint by the time this stage starts.
            if (std::find(scenarios.begin(), scenarios.begin() + index, parent) != scenarios.begin() + index)
                continue;

            if (RunSeedable(config, parent))
            {
                // It will be seeded from. Worth saying only that the run never finished (it was cancelled), which
                // is a reason to read this stage's scores carefully and not a reason to retrain anything.
                //
                // It does not say which checkpoint: that is the learner's to decide (TrainConfig.seed_from, and
                // the run's own seed_from file), and it said "best" here while the learner took latest.pt.
                if (!RunAdvanced(config, parent))
                    out(Acore::StringFormat("  {} seeds from {}, whose run did not finish (it was cancelled).",
                        stage->Name, parent));
                continue;
            }

            if (std::find(scenarios.begin() + index + 1, scenarios.end(), parent) != scenarios.end())
                out(Acore::StringFormat("  Warning: {} comes before {}, which it builds on and seeds from; it will "
                    "not seed from it. List {} first.", stage->Name, parent, parent));
            else
                out(Acore::StringFormat("  Warning: {} builds on {}, which has no checkpoint in {}; it will not "
                    "seed from it and starts from scratch. Train {} first.", stage->Name, parent,
                    config.RunsDir().string(), parent));
        }
    }
}

void AnimusForge::Forge::Pump()
{
    if (_pumping)
        return;

    _pumping = true;
    sWorld->ProcessCliCommands();
    PollExport();
    PollCluster();
    MaybeReport();
    _pumping = false;
}

void AnimusForge::Forge::PollCluster()
{
    _cluster.Poll();

    // Host: a worker of the running scenario that dropped out and is back goes straight back to it, and the
    // learner takes its envs in again between rollouts. One the learner does not know joins the next scenario.
    if (_config.Cluster == ForgeConfig::ClusterRole::Host)
    {
        for (std::string const& sim : _cluster.TakeRegistrations())
        {
            bool const running = _state == State::Training && !_clusterStart.empty();
            if (running && std::find(_clusterSims.begin(), _clusterSims.end(), sim) != _clusterSims.end())
            {
                LOG_INFO("module.animus", "Cluster: {} is back; ordering it onto {} again", sim, _current);
                _cluster.SendTo(sim, _clusterStart);
            }
            else if (running)
                LOG_INFO("module.animus", "Cluster: {} joins the next scenario this host starts", sim);
        }
        return;
    }

    if (_config.Cluster != ForgeConfig::ClusterRole::Worker)
        return;

    // The host's console shows every worker: what it runs, how fast, and where a decision's time goes.
    auto const now = std::chrono::steady_clock::now();
    if (now >= _nextClusterReport)
    {
        _nextClusterReport = now + std::chrono::seconds(5);
        _cluster.Report(Acore::StringFormat("state={} scenario={} envs={} env_steps_per_s={:.0f} decision_ms={:.1f} "
            "world_ms={:.1f} sim_ms={:.1f} learner_ms={:.1f}", StateName(), _current.empty() ? "-" : _current,
            _pool ? _pool->NumEnvs() : 0, _ticksPerSecond * double(_pool ? _pool->NumEnvs() : 0),
            _ticksPerSecond > 0.0 ? 1000.0 / _ticksPerSecond : 0.0, _worldMsPerTick, _simMsPerTick,
            _learnerMsPerTick));
    }

    // Orders only ask: the scenario is torn down and started from OnUpdate, never from inside a wait on the learner
    // (Pump runs there), exactly as a console command's are.
    while (std::optional<std::string> order = _cluster.NextOrder())
    {
        char scenario[128] = {};
        unsigned resume = 0;
        unsigned fast = 0;
        if (std::sscanf(order->c_str(), "START %127s %u %u", scenario, &resume, &fast) >= 1)
        {
            // Already running it (only the link to the host was lost): its sim goes on, and the learner reconnects.
            if (_state != State::Idle && _current == scenario && _plan.Fast == (fast != 0) && !_clusterOrder)
            {
                LOG_INFO("module.animus", "Cluster: the host orders {}, which this worker is running already",
                    scenario);
                continue;
            }
            LOG_INFO("module.animus", "Cluster: the host orders {}{}", scenario, fast ? " (fast)" : "");
            _clusterOrder = WorkerPlan(scenario, resume != 0, fast != 0);
            if (_state != State::Idle)
                _request = Request::Cancel;
        }
        else if (*order == "STOP")
        {
            LOG_INFO("module.animus", "Cluster: the host's plan ended");
            _clusterOrder.reset();
            if (_state != State::Idle)
                _request = Request::Cancel;
        }
    }
}

AnimusForge::Forge::Plan AnimusForge::Forge::WorkerPlan(std::string const& scenario, bool resume, bool fast) const
{
    // A fast run's scenarios are built from the fast profile (its classes, levels, envs): the host's learner refuses a
    // worker whose sim is not the same scenario as its own.
    ForgeConfig config = fast ? _fastConfig : _config;
    config.Policy = "remote";
    config.LearnerAutoStart = false;
    config.SocketPath = Acore::StringFormat("tcp://0.0.0.0:{}", _config.ClusterDataPort);
    config.LearnerRanks = 1;        // a worker's sim is one rank's, whichever of the host's learners that is

    Plan plan;
    plan.Policy = "remote";
    plan.Fast = fast;
    PlanEntry entry;
    entry.Scenario = scenario;
    entry.Resume = resume;
    entry.Config = std::move(config);
    plan.Entries.push_back(std::move(entry));
    return plan;
}

void AnimusForge::Forge::PollExport()
{
    if (!_export.IsRunning())
        return;

    _export.Poll();
    if (_export.FinishedCleanly())
        LOG_INFO("module.animus", "Export of {} finished: models in {}", _exportScenario, _exportModelDir);
}

void AnimusForge::Forge::MaybeReport()
{
    if (!_progressInterval || (_state != State::Training && _state != State::Running))
        return;

    auto const now = std::chrono::steady_clock::now();
    if (now - _lastReport < std::chrono::seconds(_progressInterval))
        return;

    _lastReport = now;
    _learner.Poll();
    _monitor.Report(RunConfig(), Snapshot(true), PlanRows(_plan, true), LogInfo, LogWarn, true);
}

void AnimusForge::Forge::ReportStageEnd()
{
    // The report `forge status` shows, once more as the stage ends: its final evaluation and the plan so far.
    _learner.Poll();
    _monitor.Report(RunConfig(), Snapshot(false), PlanRows(_plan, true), LogInfo, LogWarn, false);
}

uint32 AnimusForge::Forge::ConfiguredMapThreads()
{
    return uint32(std::max<int32>(0, sConfigMgr->GetOption<int32>("MapUpdate.Threads", 1)));
}

void AnimusForge::Forge::ApplyMapThreads(uint32 threads)
{
    MapUpdater* updater = sMapMgr->GetMapUpdater();
    if (!updater)
        return;

    // Between decisions, with no map update running: deactivate joins the pool's threads, activate starts new ones.
    if (updater->activated())
        updater->deactivate();

    if (threads)
        updater->activate(threads);

    LOG_DEBUG("module.animus", "Map update threads: {}", threads);
}

AnimusForge::Forge::Plan AnimusForge::Forge::BenchPlan(std::string const& scenario,
    std::vector<BenchTrial> const& trials) const
{
    Plan plan;
    plan.Policy = _config.Bench.Policy;
    for (BenchTrial const& trial : trials)
    {
        PlanEntry entry;
        entry.Scenario = scenario;
        entry.Config = _config.BenchProfile(trial.Envs, trial.Learner, trial.TorchThreads);
        entry.MapThreads = trial.MapThreads;
        plan.Entries.push_back(std::move(entry));
    }

    // Every trial carries its own settings; the plan's policy is only what `forge status` shows.
    if (!trials.empty() && trials.front().Learner)
        plan.Policy = "remote";

    return plan;
}

void AnimusForge::Forge::BenchTick()
{
    // Trials dropped before they ran (out of memory) keep no plan entry: walk past them.
    while (_benchTrial < _benchTrials.size() && !_benchTrials[_benchTrial].Note.empty())
        ++_benchTrial;

    if (_benchTrial >= _benchTrials.size())
        return;

    BenchTrial& trial = _benchTrials[_benchTrial];
    ForgeConfig const& config = RunConfig();

    // The learner phase times a handful of settings rather than a grid, so it can afford the longer window its
    // start-up and its updates need; the sim-only grid runs on the short one.
    uint32 const warmupTicks = _benchLearnerPhase ? _config.Bench.LearnerWarmupTicks : _config.Bench.WarmupTicks;
    uint32 const measureTicks = _benchLearnerPhase ? _config.Bench.LearnerMeasureTicks : _config.Bench.MeasureTicks;

    // The warm-up covers the first episodes and, for a learner trial, its start-up and first update.
    if (_ticks == warmupTicks)
    {
        _benchMeasuredFrom = std::chrono::steady_clock::now();
        _benchWorldNs = _worldNs;
        _benchSimNs = _simNs;
        _benchLearnerNs = _learnerNs;
        _benchObjectsNs = sMapMgr->GetUpdateTiming().ObjectsNs;
        _benchResetNs = _collect.ResetNs;
        _benchMapTiming = sMapMgr->GetUpdateTiming();
        _benchTaskTiming = sMapMgr->GetTaskTiming();
        return;
    }

    if (_ticks < uint64(warmupTicks) + measureTicks)
        return;

    double const seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - _benchMeasuredFrom).count();
    double const ticks = double(measureTicks);
    uint32 const agents = _pool ? _pool->Spec().AgentsPerEnv : 1;

    trial.Agents = agents;
    trial.Envs = config.Envs;
    trial.EnvStepsPerSecond = seconds > 0.0 ? ticks * double(config.Envs) * double(agents) / seconds : 0.0;
    // A host's learner trial: its workers' own env steps, as they last reported them.
    trial.WorkerEnvStepsPerSecond = 0.0;
    if (trial.Learner && _config.Cluster == ForgeConfig::ClusterRole::Host)
        for (ClusterLink::WorkerStatus const& worker : _cluster.Workers())
        {
            std::size_t const at = worker.Progress.find("env_steps_per_s=");
            if (at != std::string::npos)
                trial.WorkerEnvStepsPerSecond += std::strtod(worker.Progress.c_str() + at + 16, nullptr);
        }
    trial.WorldMsPerTick = double(_worldNs - _benchWorldNs) / ticks / 1e6;
    trial.SimMsPerTick = double(_simNs - _benchSimNs) / ticks / 1e6;
    trial.LearnerMsPerTick = double(_learnerNs - _benchLearnerNs) / ticks / 1e6;
    trial.MemoryMb = ResidentMb();
    trial.Groups = _pool ? _pool->GroupCount() : 1;

    // Instances destroyed during the window take their share out of the map totals: clamp rather than wrap.
    uint64 const objectsNs = sMapMgr->GetUpdateTiming().ObjectsNs;
    trial.ObjectsMs = double(objectsNs - std::min(objectsNs, _benchObjectsNs)) / ticks / 1e6;
    trial.ResetMs = double(_collect.ResetNs - std::min(_collect.ResetNs, _benchResetNs)) / ticks / 1e6;
    {
        Map::UpdateTiming const& now = sMapMgr->GetUpdateTiming();
        auto const since = [](uint64 later, uint64 earlier) { return double(later - std::min(later, earlier)); };
        trial.SpawnUpdates = since(now.SpawnUpdates, _benchMapTiming.SpawnUpdates) / ticks;
        trial.UnseenSpawns = since(now.UnseenSpawns, _benchMapTiming.UnseenSpawns) / ticks;
        trial.OtherUpdates = since(now.OtherUpdates, _benchMapTiming.OtherUpdates) / ticks;
        trial.SpawnMs = since(now.SpawnNs, _benchMapTiming.SpawnNs) / ticks / 1e6;
        trial.OtherMs = since(now.OtherNs, _benchMapTiming.OtherNs) / ticks / 1e6;
        trial.SendMs = since(now.SendUpdatesNs, _benchMapTiming.SendUpdatesNs) / ticks / 1e6;
    }

    MapMgr::TaskTiming const& tasks = sMapMgr->GetTaskTiming();
    if (uint64 const updates = tasks.Ticks - _benchTaskTiming.Ticks)
    {
        double const perUpdate = double(updates) * 1e6;
        trial.TasksPerUpdate = double(tasks.Tasks - _benchTaskTiming.Tasks) / double(updates);
        trial.TaskSumMs = double(tasks.SumNs - _benchTaskTiming.SumNs) / perUpdate;
        trial.TaskLongestMs = double(tasks.LongestNs - _benchTaskTiming.LongestNs) / perUpdate;
        trial.TaskWallMs = double(tasks.WallNs - _benchTaskTiming.WallNs) / perUpdate;
    }
    trial.SlowestCpu = tasks.SlowestCpu;
    trial.CpuMask = tasks.CpuMask;

    trial.WarmupTicks = warmupTicks;
    trial.MeasureTicks = measureTicks;
    trial.Measured = true;

    LOG_INFO("module.animus", "Bench {} of {}: {} threads, {} envs{} -> {:.0f} env steps/s (world {:.1f} ms, sim "
        "{:.1f} ms, learner {:.1f} ms per decision; {} MB)", _benchTrial + 1, _benchTrials.size(), trial.MapThreads,
        trial.Envs, trial.Learner ? Acore::StringFormat(", learner (torch threads {})",
            trial.TorchThreads ? std::to_string(trial.TorchThreads) : "default") : "", trial.EnvStepsPerSecond,
        trial.WorldMsPerTick, trial.SimMsPerTick, trial.LearnerMsPerTick, trial.MemoryMb);
    LOG_INFO("module.animus", "Bench {} of {}: map tasks {:.1f} per update, sum {:.2f} ms over {:.2f} ms wall, "
        "longest {:.2f} ms (last on cpu {}), cpus {}; objects {:.2f} ms, resets {:.2f} ms per decision",
        _benchTrial + 1, _benchTrials.size(), trial.TasksPerUpdate, trial.TaskSumMs, trial.TaskWallMs,
        trial.TaskLongestMs, trial.SlowestCpu, Format::Cpus(trial.CpuMask), trial.ObjectsMs, trial.ResetMs);
    LOG_INFO("module.animus", "Bench {} of {}: objects are {:.0f} world spawns ({:.2f} ms; {:.0f} unseen, left alone), "
        "{:.0f} others ({:.2f} ms) and sending updates ({:.2f} ms) per decision", _benchTrial + 1, _benchTrials.size(),
        trial.SpawnUpdates, trial.SpawnMs, trial.UnseenSpawns, trial.OtherUpdates, trial.OtherMs, trial.SendMs);

    // Skip what is left of this thread count once the machine is running out of memory: bigger envs only cost more.
    // The trials go from the plan too, so their envs are never built. Trial i is plan entry _plan.Index + i -
    // _benchTrial, so both are walked from the back.
    if (uint32 const used = MemoryUsedPercent(); used > _config.Bench.MaxMemoryPercent)
    {
        for (std::size_t i = _benchTrials.size(); i-- > _benchTrial + 1;)
        {
            BenchTrial& later = _benchTrials[i];
            if (later.MapThreads != trial.MapThreads || later.Envs <= trial.Envs || !later.Note.empty())
                continue;

            later.Note = Acore::StringFormat("skipped: memory {}% used", used);
            std::size_t const entry = _plan.Index + (i - _benchTrial);
            if (entry < _plan.Entries.size())
                _plan.Entries.erase(_plan.Entries.begin() + entry);
        }
    }

    ++_benchTrial;
    FinishCurrent(Outcome::Done);
}

void AnimusForge::Forge::BenchPlanEnded()
{
    // Phase 1 is over: the best few settings run again with the learner, which is what training actually costs.
    if (!_benchLearnerPhase && _config.Bench.LearnerTop)
    {
        std::vector<BenchTrial> best;
        for (BenchTrial const& trial : _benchTrials)
            if (trial.Measured)
                best.push_back(trial);

        std::sort(best.begin(), best.end(), [](BenchTrial const& left, BenchTrial const& right)
        {
            return left.EnvStepsPerSecond > right.EnvStepsPerSecond;
        });
        best.resize(std::min<std::size_t>(best.size(), _config.Bench.LearnerTop));

        std::vector<BenchTrial> learnerTrials;
        for (BenchTrial const& trial : best)
            for (uint32 threads : _config.Bench.LearnerTorchThreads)
            {
                BenchTrial next = trial;
                next.Learner = true;
                next.TorchThreads = threads;
                next.Measured = false;
                next.EnvStepsPerSecond = 0.0;
                next.Note.clear();
                learnerTrials.push_back(next);
            }

        if (!learnerTrials.empty())
        {
            _benchLearnerPhase = true;
            _benchTrials.insert(_benchTrials.end(), learnerTrials.begin(), learnerTrials.end());
            _benchTrial = _benchTrials.size() - learnerTrials.size();

            LOG_INFO("module.animus", "Bench: the {} fastest settings run again with the learner", best.size());
            _requested = BenchPlan(_benchScenario, learnerTrials);
            _request = Request::Start;
            return;
        }
    }

    BenchReport(LogInfo);
    BenchSave();
    BenchEnd();
}

void AnimusForge::Forge::BenchEnd()
{
    _benching = false;
    Map::DetailedObjectTiming.store(false, std::memory_order_relaxed);
    _benchLearnerPhase = false;
    ApplyMapThreads(ConfiguredMapThreads());
}

void AnimusForge::Forge::BenchReport(LineSink const& out) const
{
    out(Acore::StringFormat("Benchmark of {} ({} decisions timed per trial, {} ms per decision):", _benchScenario,
        _config.Bench.MeasureTicks, _config.DecisionMs));

    TextTable table({ { "Threads", TextTable::Align::Right }, { "Envs", TextTable::Align::Right }, { "Learner" },
        { "Env steps/s", TextTable::Align::Right }, { "World ms", TextTable::Align::Right },
        { "Sim ms", TextTable::Align::Right }, { "Learner ms", TextTable::Align::Right },
        { "Memory MB", TextTable::Align::Right }, { "Tasks", TextTable::Align::Right },
        { "Longest ms", TextTable::Align::Right }, { "Parallel", TextTable::Align::Right }, { "CPUs" } });

    BenchTrial const* winner = nullptr;
    bool unsplitTrials = false;
    for (BenchTrial const& trial : _benchTrials)
    {
        std::string const learner = !trial.Learner ? "-"
            : trial.TorchThreads ? Acore::StringFormat("torch {}", trial.TorchThreads) : "torch default";

        if (!trial.Measured)
        {
            table.AddRow({ std::to_string(trial.MapThreads), std::to_string(trial.Envs), learner,
                trial.Note.empty() ? "not run" : trial.Note, "", "", "", "", "", "", "", "" });
            continue;
        }

        // A half-batch trial whose envs could not be split ran as one group, in lock step: marked, as it measures
        // something other than the configuration it names.
        bool const unsplit = _config.HalfBatch && trial.Groups < 2;
        unsplitTrials |= unsplit;
        table.AddRow({ std::to_string(trial.MapThreads), std::to_string(trial.Envs) + (unsplit ? " *" : ""), learner,
            Acore::StringFormat("{:.0f}", trial.EnvStepsPerSecond),
            Acore::StringFormat("{:.1f}", trial.WorldMsPerTick), Acore::StringFormat("{:.1f}", trial.SimMsPerTick),
            Acore::StringFormat("{:.1f}", trial.LearnerMsPerTick), std::to_string(trial.MemoryMb),
            Acore::StringFormat("{:.1f}", trial.TasksPerUpdate), Acore::StringFormat("{:.2f}", trial.TaskLongestMs),
            Acore::StringFormat("{:.1f}x", trial.TaskWallMs > 0.0 ? trial.TaskSumMs / trial.TaskWallMs : 0.0),
            Format::Cpus(trial.CpuMask) });

        // The learner phase is what training costs, so it decides once it has run.
        bool const better = !winner || (trial.Learner && !winner->Learner)
            || (trial.Learner == winner->Learner && trial.EnvStepsPerSecond > winner->EnvStepsPerSecond);
        if (better)
            winner = &trial;
    }

    table.Write(out, "  ");
    double workers = 0.0;
    for (BenchTrial const& trial : _benchTrials)
        workers = std::max(workers, trial.WorkerEnvStepsPerSecond);
    if (workers > 0.0)
        for (BenchTrial const& trial : _benchTrials)
            if (trial.Measured && trial.Learner)
                out(Acore::StringFormat("  cluster, {} threads / {} envs with the learner: {:.0f} env steps/s here + "
                    "{:.0f} on the workers = {:.0f}", trial.MapThreads, trial.Envs, trial.EnvStepsPerSecond,
                    trial.WorkerEnvStepsPerSecond, trial.EnvStepsPerSecond + trial.WorkerEnvStepsPerSecond));
    if (unsplitTrials)
        out(Acore::StringFormat("* ran as one group, not in halves: those envs do not split into two halves on "
            "separate maps with AnimusForge.ContinentReplicas = {} (each replica holds at most 31 envs; make the "
            "replicas even, a divisor of the envs, and enough of them).", _config.ContinentReplicas));

    if (!winner)
    {
        out("No trial was measured.");
        return;
    }

    out(Acore::StringFormat("Fastest: MapUpdate.Threads = {}, AnimusForge.Envs = {}{} at {:.0f} env steps/s "
        "({:.1f}x the {} threads / {} envs you run now).", winner->MapThreads, winner->Envs,
        winner->Learner && winner->TorchThreads
            ? Acore::StringFormat(", AnimusForge.Learner.TorchThreads = {}", winner->TorchThreads) : "",
        winner->EnvStepsPerSecond, [&]
        {
            for (BenchTrial const& trial : _benchTrials)
                if (trial.Measured && trial.Learner == winner->Learner && trial.MapThreads == ConfiguredMapThreads()
                    && trial.Envs == _config.Envs && trial.EnvStepsPerSecond > 0.0)
                    return winner->EnvStepsPerSecond / trial.EnvStepsPerSecond;

            return 1.0;
        }(), ConfiguredMapThreads(), _config.Envs));

    if (winner->Envs != _config.Envs)
        out(Acore::StringFormat("Note: {} envs instead of {} changes what the learner sees in one update (its batch "
            "is rollout_length x envs x seats), not only the speed.", winner->Envs, _config.Envs));

    out("`forge bench apply` writes these into your configs (the thread count needs a restart).");
}

void AnimusForge::Forge::BenchSave() const
{
    namespace fs = std::filesystem;

    boost::json::object file;
    file["scenario"] = _benchScenario;
    file["decision_ms"] = _config.DecisionMs;
    file["ticks_per_decision"] = _config.TicksPerDecision;
    file["measure_ticks"] = _config.Bench.MeasureTicks;
    file["warmup_ticks"] = _config.Bench.WarmupTicks;
    file["learner_measure_ticks"] = _config.Bench.LearnerMeasureTicks;
    file["learner_warmup_ticks"] = _config.Bench.LearnerWarmupTicks;
    file["cores"] = uint32(std::thread::hardware_concurrency());
    file["configured_threads"] = ConfiguredMapThreads();
    file["configured_envs"] = _config.Envs;

    boost::json::array& trials = file["trials"].emplace_array();
    boost::json::object const* bestEntry = nullptr;
    double best = 0.0;
    bool bestLearner = false;
    for (BenchTrial const& trial : _benchTrials)
    {
        boost::json::object& entry = trials.emplace_back(boost::json::object()).get_object();
        entry["map_threads"] = trial.MapThreads;
        entry["envs"] = trial.Envs;
        entry["agents"] = trial.Agents;
        entry["learner"] = trial.Learner;
        entry["torch_threads"] = trial.TorchThreads;
        entry["measured"] = trial.Measured;
        entry["env_steps_per_second"] = trial.EnvStepsPerSecond;
        entry["world_ms"] = trial.WorldMsPerTick;
        entry["sim_ms"] = trial.SimMsPerTick;
        entry["learner_ms"] = trial.LearnerMsPerTick;
        entry["memory_mb"] = trial.MemoryMb;
        entry["objects_ms"] = trial.ObjectsMs;
        entry["reset_ms"] = trial.ResetMs;
        entry["spawn_updates"] = trial.SpawnUpdates;
        entry["unseen_spawns"] = trial.UnseenSpawns;
        entry["other_updates"] = trial.OtherUpdates;
        entry["spawn_ms"] = trial.SpawnMs;
        entry["other_ms"] = trial.OtherMs;
        entry["send_ms"] = trial.SendMs;
        entry["tasks_per_update"] = trial.TasksPerUpdate;
        entry["task_sum_ms"] = trial.TaskSumMs;
        entry["task_longest_ms"] = trial.TaskLongestMs;
        entry["task_wall_ms"] = trial.TaskWallMs;
        entry["slowest_cpu"] = trial.SlowestCpu;
        entry["cpus"] = Format::Cpus(trial.CpuMask);
        entry["warmup_ticks"] = trial.WarmupTicks;
        entry["measure_ticks"] = trial.MeasureTicks;
        if (!trial.Note.empty())
            entry["note"] = trial.Note;

        if (trial.Measured && ((trial.Learner && !bestLearner) || (trial.Learner == bestLearner
            && trial.EnvStepsPerSecond > best)))
        {
            best = trial.EnvStepsPerSecond;
            bestLearner = trial.Learner;
            bestEntry = &entry;
        }
    }

    if (bestEntry)
        file["best"] = *bestEntry;

    std::error_code error;
    fs::create_directories(_config.Bench.OutputDir, error);
    fs::path const path = fs::path(_config.Bench.OutputDir) / "bench.json";
    std::ofstream out(path, std::ios::trunc);
    out << boost::json::serialize(file);
    if (!out)
        LOG_ERROR("module.animus", "Could not write {}", path.string());
    else
        LOG_INFO("module.animus", "Bench results: {}", path.string());
}

AnimusForge::SimSnapshot AnimusForge::Forge::Snapshot(bool advanceRates)
{
    SimSnapshot sim;
    auto const now = std::chrono::steady_clock::now();

    sim.Scenario = _current;
    sim.State = StateName() + (_plan.Fast && _state != State::Idle ? " (fast)" : "");
    sim.PlanPosition = _plan.Index + 1;
    sim.PlanSize = uint32(_plan.Entries.size());
    sim.Remote = _plan.Remote();
    sim.Decisions = _decisions;
    sim.EpisodeLimit = _plan.Remote() ? 0 : _plan.LocalEpisodes;
    sim.ScenarioSeconds = std::chrono::duration<double>(now - _scenarioStarted).count();

    if (_pool)
    {
        sim.Envs = _pool->NumEnvs();
        sim.AgentsPerEnv = _pool->Spec().AgentsPerEnv;
        sim.Episodes = _pool->CompletedEpisodes();
        sim.EpisodeMeans = _pool->LastEpisodeMeans();
        sim.EpisodeMeansCount = _pool->LastEpisodeMeansCount();
    }

    // Rates over the time since the last periodic report; a status in between shows the rate so far.
    double const seconds = std::chrono::duration<double>(now - _rateTime).count();
    uint64 const ticks = _ticks - std::min(_ticks, _rateTicks);
    if (seconds >= 1.0 && _state != State::Paused)
    {
        _ticksPerSecond = double(ticks) / seconds;
        _episodesPerSecond = double(sim.Episodes - std::min(sim.Episodes, _rateEpisodes)) / seconds;

        // Where those decisions' wall time went, per decision.
        if (ticks)
        {
            double const perTick = double(ticks) * 1e6;
            _worldMsPerTick = double(_worldNs - std::min(_worldNs, _rateWorldNs)) / perTick;
            _simMsPerTick = double(_simNs - std::min(_simNs, _rateSimNs)) / perTick;
            _learnerMsPerTick = double(_learnerNs - std::min(_learnerNs, _rateLearnerNs)) / perTick;

            auto const since = [](uint64 now, uint64 then) { return double(now - std::min(now, then)); };
            _collectMs.Reward = since(_collect.RewardNs, _rateCollect.RewardNs) / perTick;
            _collectMs.Observe = since(_collect.ObserveNs, _rateCollect.ObserveNs) / perTick;
            _collectMs.FinalObserve = since(_collect.FinalObserveNs, _rateCollect.FinalObserveNs) / perTick;
            _collectMs.Reset = since(_collect.ResetNs, _rateCollect.ResetNs) / perTick;
            _collectMs.MapReset = since(_collect.MapResetNs, _rateCollect.MapResetNs) / perTick;
            _observeBlockMs.clear();
            for (std::size_t slot = 0; slot < _rateObserveNs.size(); ++slot)
            {
                uint64 const now = Animus::Curriculum::SeatEncoder::ObserveNs[slot].load(std::memory_order_relaxed);
                double const ms = double(now - std::min(now, _rateObserveNs[slot])) / 1e6 / double(ticks);
                if (ms > 0.0)
                {
                    namespace Curriculum = Animus::Curriculum;
                    std::string const name = slot == Curriculum::SeatEncoder::OBSERVE_VIEW ? std::string("view")
                        : slot == Curriculum::SeatEncoder::OBSERVE_PROBE ? std::string("move.probe")
                        : slot == Curriculum::SeatEncoder::OBSERVE_PROBE_MARCH ? std::string("probe.marches")
                        : slot == Curriculum::SeatEncoder::OBSERVE_PROBE_RAYS ? std::string("probe.rays")
                        : std::string(Curriculum::BlockName(Curriculum::BlockId(slot)));
                    _observeBlockMs.emplace_back(name, ms);
                }
            }
            std::sort(_observeBlockMs.begin(), _observeBlockMs.end(),
                [](auto const& a, auto const& b) { return a.second > b.second; });
            _collectMs.ResetCreate = since(_collect.ResetCreateNs, _rateCollect.ResetCreateNs) / perTick;
            _collectMs.ResetPlace = since(_collect.ResetPlaceNs, _rateCollect.ResetPlaceNs) / perTick;
            _collectMs.ResetConfigure = since(_collect.ResetConfigureNs, _rateCollect.ResetConfigureNs) / perTick;
            _collectMs.ResetDestroy = since(_collect.ResetDestroyNs, _rateCollect.ResetDestroyNs) / perTick;
            _collectMs.ResetEncounter = since(_collect.ResetEncounterNs, _rateCollect.ResetEncounterNs) / perTick;
            _collectMs.ResetScatter = since(_collect.ResetScatterNs, _rateCollect.ResetScatterNs) / perTick;
            _collectMs.ResetStock = since(_collect.ResetStockNs, _rateCollect.ResetStockNs) / perTick;
            _collectMs.ResetPrepare = since(_collect.ResetPrepareNs, _rateCollect.ResetPrepareNs) / perTick;
            _collectMs.ResetSeats = since(_collect.ResetSeatsNs, _rateCollect.ResetSeatsNs) / perTick;
            _collectMs.ResetDespawn = since(_collect.ResetDespawnNs, _rateCollect.ResetDespawnNs) / perTick;
            _collectMs.ResetScenario = since(_collect.ResetScenarioNs, _rateCollect.ResetScenarioNs) / perTick;

            Map::UpdateTiming const& mapTiming = sMapMgr->GetUpdateTiming();
            _worldMs.Sessions = since(mapTiming.SessionsNs, _rateMapTiming.SessionsNs) / perTick;
            _worldMs.Players = since(mapTiming.PlayersNs, _rateMapTiming.PlayersNs) / perTick;
            _worldMs.Objects = since(mapTiming.ObjectsNs, _rateMapTiming.ObjectsNs) / perTick;
            _worldMs.Scripts = since(mapTiming.ScriptsNs, _rateMapTiming.ScriptsNs) / perTick;
            _worldMs.Relocation = since(mapTiming.RelocationNs, _rateMapTiming.RelocationNs) / perTick;
            _worldMs.Visibility = since(mapTiming.VisibilityNs, _rateMapTiming.VisibilityNs) / perTick;
            _worldMs.Delayed = since(mapTiming.DelayedNs, _rateMapTiming.DelayedNs) / perTick;

            MapMgr::TaskTiming const& taskTiming = sMapMgr->GetTaskTiming();
            if (uint64 const updates = taskTiming.Ticks - std::min(taskTiming.Ticks, _rateTaskTiming.Ticks))
            {
                double const perUpdate = double(updates) * 1e6;
                _mapTasks.Tasks = since(taskTiming.Tasks, _rateTaskTiming.Tasks) / double(updates);
                _mapTasks.Sum = since(taskTiming.SumNs, _rateTaskTiming.SumNs) / perUpdate;
                _mapTasks.Longest = since(taskTiming.LongestNs, _rateTaskTiming.LongestNs) / perUpdate;
                _mapTasks.Wall = since(taskTiming.WallNs, _rateTaskTiming.WallNs) / perUpdate;
            }
            _mapTasks.SlowestMapId = taskTiming.SlowestMapId;
            _mapTasks.SlowestInstanceId = taskTiming.SlowestInstanceId;
            _mapTasks.SlowestCpu = taskTiming.SlowestCpu;
            _mapTasks.CpuMask = taskTiming.CpuMask;
            _collectMs.Apply = since(_collect.ApplyNs, _rateCollect.ApplyNs) / perTick;
            _collectMs.ResetsPerTick = since(_collect.Resets, _rateCollect.Resets) / double(ticks);
            _collectMs.MapResetsPerTick = since(_collect.MapResets, _rateCollect.MapResets) / double(ticks);
            _collectMs.ReusedPerTick = since(_collect.Reused, _rateCollect.Reused) / double(ticks);
        }
    }

    if (advanceRates)
    {
        _rateTime = now;
        _rateTicks = _ticks;
        _rateEpisodes = sim.Episodes;
        _rateWorldNs = _worldNs;
        _rateSimNs = _simNs;
        _rateLearnerNs = _learnerNs;
        _rateCollect = _collect;
        for (std::size_t slot = 0; slot < _rateObserveNs.size(); ++slot)
            _rateObserveNs[slot] = Animus::Curriculum::SeatEncoder::ObserveNs[slot].load(std::memory_order_relaxed);
        _rateMapTiming = sMapMgr->GetUpdateTiming();
        _rateTaskTiming = sMapMgr->GetTaskTiming();
    }

    sim.TicksPerSecond = _ticksPerSecond;
    sim.ObserveBlocks = _observeBlockMs;
    if (Animus::Curriculum::ProbeBake::Store::Baked())
    {
        namespace Store = Animus::Curriculum::ProbeBake::Store;
        uint64 const reads = Store::Reads.load(std::memory_order_relaxed);
        uint64 const fallbacks = Store::Fallbacks.load(std::memory_order_relaxed);
        sim.ProbeNote = Acore::StringFormat("baked: {} reads, {} grid tables held ({:.0f} MB), {} measured live "
            "where no table answered ({:.1f}%)", reads, Store::Loaded(), double(Store::Bytes()) / (1024.0 * 1024.0),
            fallbacks, reads + fallbacks ? 100.0 * double(fallbacks) / double(reads + fallbacks) : 0.0);
    }
    sim.EpisodesPerSecond = _episodesPerSecond;
    sim.EnvStepsPerSecond = _ticksPerSecond * double(sim.Envs) * double(sim.AgentsPerEnv);
    sim.WorldMsPerTick = _worldMsPerTick;
    sim.SimMsPerTick = _simMsPerTick;
    sim.LearnerMsPerTick = _learnerMsPerTick;
    sim.Collect = _collectMs;
    sim.World = _worldMs;
    sim.MapTasks = _mapTasks;

    sim.LearnerRunning = _learner.IsRunning();
    sim.LearnerPid = _learner.IsRunning() ? int32(_learner.Pid()) : -1;
    sim.LearnerConnected = _server.HasClient();
    sim.LearnerFailed = _learnerStarted && _learner.FailedUnexpectedly();
    sim.SecondsSinceAct = _lastAct ? std::chrono::duration<double>(now - *_lastAct).count() : -1.0;
    return sim;
}

std::vector<AnimusForge::PlanRow> AnimusForge::Forge::PlanRows(Plan const& plan, bool live) const
{
    std::vector<PlanRow> rows;
    for (std::size_t i = 0; i < plan.Entries.size(); ++i)
    {
        PlanEntry const& entry = plan.Entries[i];

        PlanRow row;
        row.Scenario = entry.Scenario;
        row.Resume = entry.Resume;
        row.Current = live && i == plan.Index;
        row.Pending = entry.Result == Outcome::None && !row.Current;
        row.Status = entry.Result != Outcome::None ? OutcomeName(entry.Result) : row.Current ? StateName() : "pending";
        rows.push_back(std::move(row));
    }

    return rows;
}

std::string AnimusForge::Forge::StateName() const
{
    switch (_state)
    {
        case State::Idle:
            return "idle";
        case State::Training:
            return _server.HasClient() ? "training" : "waiting for learner";
        case State::Running:
            return "running " + _plan.Policy;
        case State::Paused:
            return "paused";
    }

    return "unknown";
}

void AnimusForge::Forge::WaitedForLearner(std::chrono::steady_clock::time_point from)
{
    uint64 const waited = uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - from).count());
    _learnerNs += waited;
    _tickLearnerNs += waited;
}

void AnimusForge::Forge::LocalDecision(uint32 group)
{
    _pool->FinishCollect(group);

    if (_plan.LocalEpisodes && _pool->CompletedEpisodes() >= _plan.LocalEpisodes)
    {
        FinishCurrent(Outcome::Done);
        return;
    }

    auto const [begin, count] = _pool->GroupRange(group);
    if (!_pool->ChooseLocalActions(_plan.Policy, false, begin, count))
    {
        LOG_ERROR("module.animus", "Scenario {} could not choose actions with policy '{}'", _current, _plan.Policy);
        FinishCurrent(Outcome::Failed);
        return;
    }

    // The maps apply them at the start of the group's next tick, each its own envs': the actions land in the very
    // update they would have if they had been applied here, and every env's share runs on its map's thread.
    _actionsPending[group] = true;
}

uint32 AnimusForge::Forge::PoolRanks(uint32 wanted) const
{
    return std::clamp<uint32>(wanted, 1, std::max<uint32>(1, _pool->NumEnvs() / _pool->GroupCount()));
}

void AnimusForge::Forge::RemoteDecision(uint32 group)
{
    // While the world thread waits on the learner: answer console commands, report progress, and stop waiting
    // when a command needs this scenario to end.
    auto const onIdle = [this]()
    {
        _learner.Poll();
        Pump();
        return _request == Request::None;
    };

    // Waiting for a learner to connect holds no decision, so a pause applies right away (OnUpdate pauses next tick).
    auto const onAccepting = [this, &onIdle]()
    {
        return onIdle() && !_pauseRequested && !LearnerFinished();
    };

    if (!_server.HasClient())
    {
        // Blocks until the learner connects; returns false on shutdown, a cancel or skip, or when the scenario's
        // learner has finished its run.
        auto const waitFrom = std::chrono::steady_clock::now();
        _ranks = PoolRanks(RunConfig().LearnerRanks);
        bool const connected = _server.AcceptClients(_ranks, onAccepting);
        WaitedForLearner(waitFrom);

        if (!connected)
        {
            if (LearnerFinished())
                FinishCurrent(Outcome::Done);
            return;
        }

        // Every rank's SPEC, then every rank's device offer, then every answer: a learner answers its offer as soon as
        // it has its SPEC, but data-parallel learners also wait on each other once they have it, so no rank may be
        // kept waiting for its SPEC or its offer behind another's answer.
        for (uint32 rank = 0; rank < _ranks; ++rank)
            if (!SendSpec(rank))
                return;
        for (uint32 rank = 0; rank < _ranks; ++rank)
            if (!OfferDevice(rank))
                return;
        for (uint32 rank = 0; rank < _ranks; ++rank)
            if (!AwaitDeviceAnswer(rank))
                return;

        // A new learner starts from fresh training episodes; whatever ran unobserved is discarded. An evaluation the
        // previous learner left unfinished ends here, or its baseline would keep replacing this learner's actions.
        // Its replay seeds were its evaluations' losses: this learner sends its own.
        _pool->SetEvaluation(false, 0, 0, {});
        _pool->SetReplay(0, 0.0f, {});
        _pool->ResetAll();
        if (!SendEveryGroup())
            return;
    }
    else
    {
        _pool->FinishCollect(group);
        if (!SendStep(group))
            return;
    }

    // The group whose maps tick next needs its answer before they do. In half-batch that is the other half, whose
    // STEP went out a world tick ago and which the learner has been deciding while this half's maps ticked; so the
    // wait here is only what is left of it. With one group it is this STEP's answer, the plain lock step.
    uint32 target = _halfBatch && _nextTurn < _pool->GroupCount() ? _nextTurn : 0;
    if (!_awaitingAnswer[target])
        return;

    std::size_t const actionBytes = _pool->Actions.size() * sizeof(int32);
    std::size_t const weightBytes = sizeof(WeightsHeader) + _pool->Spec().Layouts.size() * sizeof(float);
    std::size_t const replayBytes = sizeof(ReplayHeader) + MAX_REPLAY_SEEDS * sizeof(uint32);
    uint32 const agents = _pool->Spec().AgentsPerEnv;

    // Every learner's answer for the group awaited, rank by rank: an ACT for its share, or a MODE, WEIGHTS or REPLAY
    // first. A mode switch resets every env and answers with every group's fresh STEP before the ACT -- once every
    // rank has asked for it, since data-parallel learners evaluate on the same update. Weights and replay seeds are
    // applied without an answer.
    std::vector<ModeMsg> modes;
    for (uint32 rank = 0; rank < _ranks;)
    {
        _server.Use(rank);
        MsgType type;
        std::vector<char> payload;
        auto const waitFrom = std::chrono::steady_clock::now();
        bool const received = _server.ReceiveAny(type, payload,
            std::max({ sizeof(ActHeader) + 2 * actionBytes, sizeof(ModeMsg), weightBytes, replayBytes }), onIdle);
        WaitedForLearner(waitFrom);
        if (!received)
        {
            _server.DropClient();
            return;
        }

        // ACT names the envs it answers for -- the group awaited, as a learner answers STEPs in the order they went
        // out -- in the learner's own numbering, then carries their actions, and the goals after them when the
        // policy has a goal head.
        ActHeader act{};
        if (type == MsgType::Act && payload.size() >= sizeof(act))
            std::memcpy(&act, payload.data(), sizeof(act));
        RankRows const rows = RankGroup(rank, target);
        std::size_t const groupBytes = std::size_t(rows.Count) * agents * sizeof(int32);
        std::size_t const body = payload.size() - std::min(payload.size(), sizeof(act));
        if (type == MsgType::Act && modes.empty() && act.EnvBegin == rows.Local && act.EnvCount == rows.Count
            && (body == groupBytes || body == 2 * groupBytes))
        {
            std::size_t const first = std::size_t(rows.Global) * agents;
            char const* actions = payload.data() + sizeof(act);
            std::memcpy(_pool->Actions.data() + first, actions, groupBytes);
            if (body == 2 * groupBytes)
                std::memcpy(_pool->Goals.data() + first, actions + groupBytes, groupBytes);
            else
                std::fill_n(_pool->Goals.begin() + first, groupBytes / sizeof(int32), -1);

            _lastAct = std::chrono::steady_clock::now();
            ++rank;
            continue;
        }

        if (type == MsgType::Mode && payload.size() == sizeof(ModeMsg) && modes.size() == rank)
        {
            ModeMsg mode{};
            std::memcpy(&mode, payload.data(), sizeof(mode));
            modes.push_back(mode);
            if (++rank < _ranks)
                continue;

            // A MODE comes where every learner holds every group's STEP (between whole decisions), so nothing they
            // have not read is lost: every env starts again, and every group's fresh STEP goes out.
            if (!ApplyModes(modes))
            {
                _server.DropClient();
                return;
            }
            _pool->ResetAll();
            if (!SendEveryGroup())
                return;

            target = 0;
            modes.clear();
            rank = 0;
            continue;
        }

        if (type == MsgType::Weights && payload.size() >= sizeof(WeightsHeader))
        {
            WeightsHeader header{};
            std::memcpy(&header, payload.data(), sizeof(header));
            if (payload.size() != sizeof(WeightsHeader) + header.Count * sizeof(float))
            {
                LOG_ERROR("module.animus", "Learner sent WEIGHTS with {} bytes for {} weights", payload.size(),
                    header.Count);
                _server.DropClient();
                return;
            }

            std::vector<float> weights(header.Count);
            if (header.Count)
                std::memcpy(weights.data(), payload.data() + sizeof(WeightsHeader), header.Count * sizeof(float));

            // Takes effect as envs reset; the episodes already running keep the classes they were built with.
            _pool->SetLayoutWeights(weights);
            continue;
        }

        if (type == MsgType::Replay && payload.size() >= sizeof(ReplayHeader))
        {
            ReplayHeader header{};
            std::memcpy(&header, payload.data(), sizeof(header));
            if (header.Count > MAX_REPLAY_SEEDS
                || payload.size() != sizeof(ReplayHeader) + header.Count * sizeof(uint32))
            {
                LOG_ERROR("module.animus", "Learner sent REPLAY with {} bytes for {} seeds", payload.size(),
                    header.Count);
                _server.DropClient();
                return;
            }

            std::vector<uint32> seeds(header.Count);
            if (header.Count)
                std::memcpy(seeds.data(), payload.data() + sizeof(ReplayHeader), header.Count * sizeof(uint32));

            // Takes effect as envs reset, like the weights.
            _pool->SetReplay(header.SeedBase, header.Fraction, std::move(seeds));
            continue;
        }

        // CLOSE (the client is already gone), a protocol error, or learners out of step (one evaluating while
        // another trains): the next decision waits for new learners.
        if (type != MsgType::Close)
            LOG_ERROR("module.animus", "Learner rank {} sent message type {} with {} bytes where ACT, MODE, WEIGHTS or "
                "REPLAY was expected", rank, uint32(type), payload.size());

        _server.DropClient();
        return;
    }
    _awaitingAnswer[target] = false;

    // Scoring a scripted baseline on the evaluation seeds: its actions replace the learner's (only the opponent
    // seats' when the learner plays against it).
    auto const [begin, count] = _pool->GroupRange(target);
    if (!_pool->EvalBaseline().empty()
        && !_pool->ChooseLocalActions(_pool->EvalBaseline(), _pool->EvalOpponentsOnly(), begin, count))
    {
        LOG_ERROR("module.animus", "Scenario {} could not run baseline '{}'; dropping the learner", _current,
            _pool->EvalBaseline());
        _server.DropClient();
        return;
    }

    // The maps apply them at the start of the group's next tick, each its own envs': the actions land in the very
    // update they would have if they had been applied here, and every env's share runs on its map's thread.
    _actionsPending[target] = true;
}

bool AnimusForge::Forge::KnowsPolicy(std::string const& policy) const
{
    if (policy == "random")
        return true;

    // Only a built scenario can answer, and `forge bench` asks while the forge is idle, where there is none:
    // say yes rather than crash on it. A trial that turns out not to know the policy reports as failed.
    if (!_scenario)
        return true;

    // ScriptedAction answers whether the scenario has the policy; a blank row is enough to ask.
    Animus::ScenarioSpec const spec = _scenario->Spec();
    std::vector<float> obs(spec.ObsDim, 0.0f);
    std::vector<uint8> mask(spec.NumActions, 0);
    int32 action = 0;
    return _scenario->ScriptedAction(policy, obs.data(), mask.data(), 0, action);
}

bool AnimusForge::Forge::ApplyMode(ModeMsg const& mode)
{
    std::string const baseline(mode.Baseline, strnlen(mode.Baseline, POLICY_NAME_SIZE));

    if (mode.Mode > 1)
    {
        LOG_ERROR("module.animus", "Learner asked for unknown mode {}", mode.Mode);
        return false;
    }

    if (mode.Mode == 1 && !baseline.empty() && !KnowsPolicy(baseline))
    {
        LOG_ERROR("module.animus", "Learner asked for baseline '{}', which scenario {} does not have", baseline,
            _scenario->Name());
        return false;
    }

    bool const opponentsOnly = (mode.Flags & MODE_FLAG_SCRIPTED_OPPONENTS) != 0;
    _pool->SetEvaluation(mode.Mode == 1, mode.SeedBase, mode.Episodes, baseline, opponentsOnly, mode.FirstSeed);

    if (mode.Mode == 1)
        LOG_DEBUG("module.animus", "Evaluation: {} seeded episodes from seed {}, policy {}", mode.Episodes,
            mode.SeedBase, baseline.empty() ? "learner" : opponentsOnly ? "learner against " + baseline : baseline);
    else
        LOG_DEBUG("module.animus", "Evaluation finished; training");

    return true;
}

bool AnimusForge::Forge::SendSpec(uint32 rank)
{
    Animus::ScenarioSpec const spec = _pool->Spec();
    _server.Use(rank);

    SpecMsg msg{};
    msg.Version = PROTOCOL_VERSION;
    msg.NumEnvs = RankEnvs(rank);
    msg.AgentsPerEnv = spec.AgentsPerEnv;
    msg.ObsDim = spec.ObsDim;
    msg.StateDim = spec.StateDim;
    msg.NumActions = spec.NumActions;
    msg.EpisodeInfoDim = spec.EpisodeInfoDim;
    msg.GoalCount = spec.GoalCount;
    // The learner reads a step as tick_ms * decision_ticks, which is AnimusForge.DecisionMs however the two are
    // split; the split itself is what tells it how finely the world moved underneath a decision.
    msg.TickMs = RunConfig().TickMs();
    msg.DecisionTicks = RunConfig().TicksPerDecision;
    // The longest episode the scenario can have: the learner sizes evaluation windows by it.
    msg.EpisodeSeconds = std::max(RunConfig().EpisodeSeconds, spec.LongestEpisodeSeconds);
    msg.EnvGroups = _pool->GroupCount();
    std::strncpy(msg.Scenario, _scenario->Name(), SCENARIO_NAME_SIZE - 1);

    uint32 const layoutCount = uint32(spec.Layouts.size());
    std::vector<LayoutMsg> layouts(layoutCount);
    for (uint32 i = 0; i < layoutCount; ++i)
    {
        layouts[i].ObsDim = spec.Layouts[i].ObsDim;
        layouts[i].NumActions = spec.Layouts[i].NumActions;
        std::strncpy(layouts[i].Name, spec.Layouts[i].Name.c_str(), LAYOUT_NAME_SIZE - 1);
    }

    std::string names;
    for (std::string const& name : _scenario->EpisodeInfoNames())
        names += (names.empty() ? "" : ",") + name;

    return _server.Send(MsgType::Spec, { { &msg, sizeof(msg) }, { &layoutCount, sizeof(layoutCount) },
        { layouts.data(), layouts.size() * sizeof(LayoutMsg) }, { names.data(), names.size() } });
}

bool AnimusForge::Forge::OfferDevice(uint32 rank)
{
    if (_rankDevices.size() < _ranks)
        _rankDevices.resize(_ranks);
    RankDevice& device = _rankDevices[rank];
    ForgeGpuApi const* gpu = Animus::Gpu::Api();

    // A learner reconnecting gets fresh buffers; whatever it had opened it closed when it went.
    if (gpu)
        for (void* pointer : { device.Obs, device.State, device.Mask })
            if (pointer)
                gpu->Free(pointer);
    device = RankDevice();
    if (!gpu)
        return true;

    Animus::ScenarioSpec const spec = _pool->Spec();
    uint32 const envs = RankEnvs(rank);
    std::size_t const obsBytes = std::size_t(envs) * spec.AgentsPerEnv * spec.ObsDim * sizeof(float);
    std::size_t const stateBytes = std::size_t(envs) * spec.StateDim * sizeof(float);
    std::size_t const maskBytes = std::size_t(envs) * spec.AgentsPerEnv * spec.NumActions * sizeof(uint8);
    // The GPU the rank's learner trains on: the one it can open the buffers on.
    std::vector<uint32> const& gpus = _config.Gpus;
    device.Device = int(rank < gpus.size() ? gpus[rank] : 0);

    DeviceMsg msg{};
    msg.Device = uint32(device.Device);
    msg.Envs = envs;
    if (gpu->Init(device.Device) || gpu->Alloc(&device.Obs, std::max<std::size_t>(obsBytes, 4))
        || gpu->Alloc(&device.State, std::max<std::size_t>(stateBytes, 4))
        || gpu->Alloc(&device.Mask, std::max<std::size_t>(maskBytes, 4))
        || gpu->Export(device.Obs, msg.ObsHandle) || gpu->Export(device.State, msg.StateHandle)
        || gpu->Export(device.Mask, msg.MaskHandle))
    {
        LOG_WARN("module.animus", "Rank {}: no device buffers, the learner gets obs over the socket: {}", rank,
            gpu->LastError());
        for (void* pointer : { device.Obs, device.State, device.Mask })
            if (pointer)
                gpu->Free(pointer);
        device = RankDevice();
        return true;
    }

    _server.Use(rank);
    device.Offered = true;
    device.Bytes = obsBytes + stateBytes + maskBytes;
    return _server.Send(MsgType::Device, { { &msg, sizeof(msg) } });
}

bool AnimusForge::Forge::AwaitDeviceAnswer(uint32 rank)
{
    if (rank >= _rankDevices.size() || !_rankDevices[rank].Offered)
        return true;
    RankDevice& device = _rankDevices[rank];
    ForgeGpuApi const* gpu = Animus::Gpu::Api();

    _server.Use(rank);
    DeviceAckMsg ack{};
    MsgType type = MsgType::Close;
    if (!_server.Receive(type, &ack, sizeof(ack)) || type != MsgType::DeviceAck)
        return false;

    device.Offered = false;
    device.On = ack.Accepted != 0;
    LOG_INFO("module.animus", "Rank {}: {}", rank, device.On
        ? Acore::StringFormat("obs, state and mask in device memory on GPU {} ({:.1f} MB)", device.Device,
            double(device.Bytes) / (1024.0 * 1024.0))
        : std::string("the learner declined device buffers; obs over the socket"));
    if (!device.On)
    {
        for (void* pointer : { device.Obs, device.State, device.Mask })
            gpu->Free(pointer);
        device = RankDevice();
    }
    return true;
}

bool AnimusForge::Forge::UploadRows(uint32 rank, uint32 begin, uint32 local, uint32 count)
{
    ForgeGpuApi const* gpu = Animus::Gpu::Api();
    RankDevice const& device = _rankDevices[rank];
    uint32 const envs = std::max<uint32>(1, _pool->NumEnvs());
    auto copy = [&](void* target, auto const& vec) -> bool
    {
        std::size_t const perEnv = vec.size() / envs * sizeof(vec[0]);
        return gpu->CopyToDevice(static_cast<char*>(target) + std::size_t(local) * perEnv,
            reinterpret_cast<char const*>(vec.data()) + std::size_t(begin) * perEnv, std::size_t(count) * perEnv) == 0;
    };
    if (gpu->Init(device.Device) == 0 && copy(device.Obs, _pool->Obs) && copy(device.State, _pool->State)
        && copy(device.Mask, _pool->Mask) && gpu->Synchronize() == 0)
        return true;
    LOG_ERROR("module.animus", "Rank {}: writing obs to the device failed: {}", rank, gpu->LastError());
    return false;
}

bool AnimusForge::Forge::SendEveryGroup()
{
    _nextTurn = 0;
    _actionsPending[0] = _actionsPending[1] = false;
    for (uint32 group = 0; group < _pool->GroupCount(); ++group)
        if (!SendStep(group))
            return false;
    return true;
}

bool AnimusForge::Forge::SendStep(uint32 group)
{
    _awaitingAnswer[group] = true;
    uint64 const decision = _decisions++;
    for (uint32 rank = 0; rank < std::max<uint32>(1, _server.Clients()); ++rank)
    {
        // Each learner gets its share of the group, numbered as its own envs.
        RankRows const rows = RankGroup(rank, group);
        _server.Use(rank);
        StepHeader header{ decision, rows.Local, rows.Count };

        // Every array is env-major, so a share's rows are one contiguous run of each.
        uint32 const envs = std::max<uint32>(1, _pool->NumEnvs());
        uint32 const begin = rows.Global;
        uint32 const count = rows.Count;
        auto chunk = [begin, count, envs](auto const& vec)
        {
            std::size_t const perEnv = vec.size() / envs;
            return Chunk{ vec.data() + std::size_t(begin) * perEnv, std::size_t(count) * perEnv * sizeof(vec[0]) };
        };

        // The ended envs' last observation and state, gathered: a few rows a decision, where every row went before.
        auto ended = [this, begin, count, envs](std::vector<float> const& vec, std::vector<float>& rows)
        {
            std::size_t const perEnv = vec.size() / envs;
            rows.clear();
            for (uint32 env = begin; env < begin + count; ++env)
                if (_pool->Done[env])
                    rows.insert(rows.end(), vec.begin() + std::ptrdiff_t(env * perEnv),
                        vec.begin() + std::ptrdiff_t((env + 1) * perEnv));
            return Chunk{ rows.data(), rows.size() * sizeof(float) };
        };

        // With device buffers the obs, state and mask are in them before the STEP says they are there.
        bool const device = rank < _rankDevices.size() && _rankDevices[rank].On;
        if (device && !UploadRows(rank, begin, rows.Local, count))
            return false;
        Chunk const none{ nullptr, 0 };

        if (!_server.Send(MsgType::Step,
            {
                { &header, sizeof(header) },
                device ? none : chunk(_pool->Obs),
                device ? none : chunk(_pool->State),
                device ? none : chunk(_pool->Mask),
                chunk(_pool->Layout),
                chunk(_pool->Present),
                chunk(_pool->Rewards),
                chunk(_pool->Done),
                chunk(_pool->Terminated),
                ended(_pool->FinalObs, _endedObs),
                ended(_pool->FinalState, _endedState),
                chunk(_pool->EpisodeInfo),
                chunk(_pool->EpisodeSeed),
            }))
            return false;
    }
    return true;
}

AnimusForge::Forge::RankRows AnimusForge::Forge::RankGroup(uint32 rank, uint32 group) const
{
    RankRows rows;
    for (uint32 g = 0; g <= group; ++g)
    {
        auto const [begin, count] = _pool->GroupRange(g);
        uint64 const first = begin + uint64(count) * rank / _ranks;
        uint64 const last = begin + uint64(count) * (rank + 1) / _ranks;
        if (g < group)
            rows.Local += uint32(last - first);
        else
        {
            rows.Global = uint32(first);
            rows.Count = uint32(last - first);
        }
    }
    return rows;
}

uint32 AnimusForge::Forge::RankEnvs(uint32 rank) const
{
    uint32 envs = 0;
    for (uint32 group = 0; group < _pool->GroupCount(); ++group)
        envs += RankGroup(rank, group).Count;
    return envs;
}

bool AnimusForge::Forge::ApplyModes(std::vector<ModeMsg> const& modes)
{
    for (ModeMsg const& mode : modes)
    {
        if (mode.Mode != modes.front().Mode || mode.SeedBase != modes.front().SeedBase
            || mode.Flags != modes.front().Flags || std::strncmp(mode.Baseline, modes.front().Baseline,
                POLICY_NAME_SIZE) != 0)
        {
            LOG_ERROR("module.animus", "Data-parallel learners asked for different modes on the same decision");
            return false;
        }
    }

    if (!ApplyMode(modes.front()))
        return false;
    if (modes.size() == 1)
        return true;

    // Each learner's envs play its own run of the evaluation's seeds.
    std::vector<std::pair<uint32, uint32>> runs;
    std::vector<uint32> runOfEnv(_pool->NumEnvs(), 0);
    for (uint32 rank = 0; rank < modes.size(); ++rank)
    {
        runs.emplace_back(modes[rank].FirstSeed, modes[rank].Episodes);
        for (uint32 group = 0; group < _pool->GroupCount(); ++group)
        {
            RankRows const rows = RankGroup(rank, group);
            std::fill_n(runOfEnv.begin() + rows.Global, rows.Count, rank);
        }
    }
    _pool->SetEvaluationRuns(runs, std::move(runOfEnv));
    return true;
}
