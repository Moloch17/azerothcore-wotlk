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

#ifndef MOD_ANIMUS_FORGE_PROGRESS_H
#define MOD_ANIMUS_FORGE_PROGRESS_H

#include "Define.h"
#include "TextTable.h"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AnimusForge
{
    struct ForgeConfig;

    /// runs/<run>/progress.json as the learner last wrote it (python/animus/progress.py): one flat JSON object of
    /// numbers, strings and nulls. Also reads the top-level values of finished.json.
    class ProgressFile
    {
    public:
        /// False when the file is missing or is not a JSON object.
        bool Load(std::filesystem::path const& path);

        /// Parse a JSON object's top-level numbers, booleans (as 1 and 0) and strings. Nested objects and arrays are
        /// skipped; null values are left out.
        bool Parse(std::string const& text);

        [[nodiscard]] std::optional<double> Number(std::string const& key) const;
        [[nodiscard]] std::string Text(std::string const& key) const;

    private:
        std::unordered_map<std::string, double> _numbers;
        std::unordered_map<std::string, std::string> _strings;
    };

    /// What the sim knows about the running scenario, gathered by Forge for a report.
    struct SimSnapshot
    {
        std::string Scenario;
        std::string State;                  // "training", "paused", "running greedy", ...
        uint32 PlanPosition = 0;            // 1-based
        uint32 PlanSize = 0;
        bool Remote = true;
        uint32 Envs = 0;
        uint32 AgentsPerEnv = 0;
        uint64 Decisions = 0;
        uint64 Episodes = 0;
        uint64 EpisodeLimit = 0;            // local runs: stop after this many episodes (0 = none)
        double ScenarioSeconds = 0.0;       // wall time since the scenario started
        double TicksPerSecond = 0.0;        // sim ticks per wall second over the last interval
        double EpisodesPerSecond = 0.0;     // over the last interval
        double EnvStepsPerSecond = 0.0;     // ticks x envs x agents per wall second
        // Where a decision's wall time goes, ms (the parts add up to one decision):
        double WorldMsPerTick = 0.0;        // the map update and the rest of the world tick
        double SimMsPerTick = 0.0;          // observing, rewarding and applying actions
        double LearnerMsPerTick = 0.0;      // blocked on the learner

        /// What the sim's own share went on, ms per decision (they add up to SimMsPerTick, less the bridge's
        /// own work): which of these dominates is what says where to spend effort.
        struct CollectMs
        {
            double Reward = 0.0;
            double Observe = 0.0;           // the running episodes' observation and mask
            double FinalObserve = 0.0;      // the ended ones' last observation, which has no mask
            double Reset = 0.0;             // building the next episode's characters
            double ResetCreate = 0.0;       // ... of which creating and placing them
            double ResetPlace = 0.0;        // ... phase, position, talent points
            double ResetConfigure = 0.0;    // ... talents, kit, gear
            double ResetDestroy = 0.0;      // ... destroying the previous seats
            double ResetEncounter = 0.0;    // ... building the encounters
            double ResetScatter = 0.0;      // ... spreading the seats
            double ResetStock = 0.0;        // ... supplies and pets
            double ResetPrepare = 0.0;      // ... the draws and the encounters' episode resets
            double ResetScenario = 0.0;     // ... Scenario::Reset as a whole
            double ResetDespawn = 0.0;      // ... the previous episode's targets despawned
            double ResetSeats = 0.0;        // ... the seats' loop as a whole
            double Apply = 0.0;             // the actions the learner sent
            double ResetsPerTick = 0.0;     // episodes rebuilt per decision
            double ReusedPerTick = 0.0;     // characters kept across those episodes instead of rebuilt, per decision
        };

        CollectMs Collect;

        /// Where the core's map update went, ms per decision summed over every map (Map::UpdateTiming).
        struct WorldMs
        {
            double Sessions = 0.0;
            double Players = 0.0;
            double Objects = 0.0;
            double Scripts = 0.0;
            double Relocation = 0.0;
            double Visibility = 0.0;
            double Delayed = 0.0;
        };

        WorldMs World;

        /// The map update as tasks on the updater (MapMgr::TaskTiming), per map update rather than per decision.
        /// Sum over Wall is the parallelism the update actually had; Longest near Wall means one map is the
        /// critical path. Slowest* and Cpus describe the last update only.
        struct MapTasksMs
        {
            double Tasks = 0.0;
            double Sum = 0.0;
            double Longest = 0.0;
            double Wall = 0.0;
            uint32 SlowestMapId = 0;
            uint32 SlowestInstanceId = 0;
            int32 SlowestCpu = -1;
            uint64 CpuMask = 0;
        };

        MapTasksMs MapTasks;
        /// Observation thread time per decision by block (and "view", the seat's work before its blocks), largest
        /// first.
        std::vector<std::pair<std::string, double>> ObserveBlocks;
        bool LearnerRunning = false;
        int32 LearnerPid = -1;
        bool LearnerConnected = false;
        bool LearnerFailed = false;
        double SecondsSinceAct = -1.0;      // < 0: no ACT yet from this learner
        uint64 EpisodeMeansCount = 0;
        std::vector<std::pair<std::string, double>> EpisodeMeans;
    };

    /// One scenario of the current plan, for the plan table.
    struct PlanRow
    {
        std::string Scenario;
        std::string Status;                 // "done", "training", "pending", "skipped", "failed", ...
        bool Current = false;
        bool Pending = false;               // not started yet
        bool Resume = false;
    };

    /// Builds the progress report: `forge status` and the periodic report while a scenario runs.
    ///
    /// Remembers the previous periodic report of the running scenario for trends, the step rate for ETAs and the
    /// first entropy for collapse warnings. Only a periodic report advances that state, so `forge status` in
    /// between does not shorten the trend window.
    class ProgressMonitor
    {
    public:
        /// A new scenario started (or resumed): forget the previous one's trends.
        void Begin(std::string const& scenario);

        /// Write the report. Warnings go to `warn` (one line each), everything else to `info`.
        void Report(ForgeConfig const& config, SimSnapshot const& sim, std::vector<PlanRow> const& plan,
            LineSink const& info, LineSink const& warn, bool periodic);

    private:
        struct Sample
        {
            double UnixTime = 0.0;
            double EnvSteps = 0.0;
        };

        struct Previous
        {
            std::optional<double> Reward;
            std::optional<double> Entropy;
            std::optional<double> Rate;
        };

        [[nodiscard]] std::optional<double> StepRate(ProgressFile const& progress) const;
        /// ConfiguredTotalEnvSteps, cached until the next scenario begins.
        [[nodiscard]] std::optional<uint64> ConfiguredSteps(ForgeConfig const& config,
            std::string const& scenario) const;
        void Advance(ProgressFile const& progress);

        void ReportTraining(ForgeConfig const& config, SimSnapshot const& sim, ProgressFile const* progress,
            LineSink const& info, std::vector<std::string>& warnings) const;
        void ReportLocal(SimSnapshot const& sim, LineSink const& info) const;
        void ReportPlan(ForgeConfig const& config, std::vector<PlanRow> const& plan, ProgressFile const* current,
            LineSink const& info) const;

        std::string _scenario;
        std::optional<Sample> _lastSample;
        std::optional<double> _rateEma;
        std::optional<double> _firstEntropy;
        Previous _previous;

        /// ConfiguredTotalEnvSteps per scenario of the plan table, read once per running scenario.
        mutable std::unordered_map<std::string, std::optional<uint64>> _configuredSteps;
    };

    /// runs/<scenario>/progress.json under the learner directory.
    std::filesystem::path ProgressPath(ForgeConfig const& config, std::string const& scenario);

    /// total_env_steps a scenario's learner will train for: the last --set total_env_steps= in
    /// the learner arguments, else the key in an --overlay file (the fast profile's), else in its YAML config or the
    /// configs it extends. Empty when none says.
    std::optional<uint64> ConfiguredTotalEnvSteps(ForgeConfig const& config, std::string const& scenario);
}

#endif
