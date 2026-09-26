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

/*
 * The forge's console commands (see Hooks/ForgeCommandScript.cpp for the command table). Every command runs on the
 * world thread; the ones that start or stop scenarios only record a request that OnUpdate applies.
 */

#include "AnimusForge.h"
#include "ClassAssets.h"
#include "Config.h"
#include "CpuPlacement.h"
#include "Log.h"
#include "MapMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr char const* ARCHIVE_DIR = "_archive";
    constexpr char const* EXPORT_LOG = "animus-export.log";

    /// How many of its last points a `noisy` build improvises, when the command asks for one. The curriculum
    /// draws this per character (Characters.TalentNoisePoints); the command shows the middle of that range.
    constexpr uint32 TALENTS_NOISE_POINTS = 3;

    /// A step budget written as `20M`, `20m`, `500K` or a plain count, for `forge fast <steps>`.
    ///
    /// A bare number below this is not a budget: `forge fast 20` is far more likely to be a mistyped scenario
    /// than a request to train twenty steps, and treating it as a budget would silently swallow the argument.
    constexpr uint64 SMALLEST_BUDGET = 1000;

    std::optional<uint64> ParseBudget(std::string const& word)
    {
        if (word.empty())
            return std::nullopt;

        uint64 scale = 1;
        std::string digits = word;
        switch (char const suffix = digits.back())
        {
            case 'K': case 'k': scale = 1000; break;
            case 'M': case 'm': scale = 1000000; break;
            default:
                if (!std::isdigit(static_cast<unsigned char>(suffix)))
                    return std::nullopt;
                break;
        }

        if (scale > 1)
            digits.pop_back();
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; }))
            return std::nullopt;

        uint64 value = 0;
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc() || end != digits.data() + digits.size())
            return std::nullopt;

        value *= scale;
        return value >= SMALLEST_BUDGET ? std::optional<uint64>(value) : std::nullopt;
    }

    std::string Join(std::vector<std::string> const& names, char const* separator = ", ")
    {
        std::string text;
        for (std::string const& name : names)
            text += (text.empty() ? "" : separator) + name;

        return text;
    }

    uintmax_t SizeOf(fs::path const& path)
    {
        std::error_code error;
        if (fs::is_regular_file(path, error))
            return fs::file_size(path, error);

        uintmax_t total = 0;
        for (fs::recursive_directory_iterator it(path, error), end; !error && it != end; it.increment(error))
            if (it->is_regular_file(error))
                total += it->file_size(error);

        return total;
    }

    /// The file the AnimusForge.* keys are read from last: a legacy <config dir>/modules/mod_animus_forge.conf
    /// when one exists (the server reads it after worldserver.conf, so it wins), else worldserver.conf itself.
    fs::path ModuleConfigFile()
    {
        fs::path const worldserver = sConfigMgr->GetFilename();
        fs::path const dir = worldserver.has_parent_path() ? worldserver.parent_path() : fs::path(".");
        fs::path const legacy = dir / "modules" / "mod_animus_forge.conf";
        std::error_code error;
        return fs::is_regular_file(legacy, error) ? legacy : worldserver;
    }

    /// Set `key` to `value` in a config file, keeping every comment and every other key; the file is backed up to
    /// <file>.before-bench first, and a key the file does not have is appended. False (with a reply) on failure.
    bool WriteConfigValue(fs::path const& file, std::string const& key, std::string const& value,
        AnimusForge::LineSink const& out)
    {
        std::error_code error;
        if (!fs::is_regular_file(file, error))
        {
            out(Acore::StringFormat("  {} does not exist; set {} = {} by hand.", file.string(), key, value));
            return false;
        }

        std::vector<std::string> lines;
        {
            std::ifstream input(file);
            for (std::string line; std::getline(input, line);)
                lines.push_back(line);
            if (!input.eof() && input.fail())
            {
                out(Acore::StringFormat("  Could not read {}.", file.string()));
                return false;
            }
        }

        bool replaced = false;
        for (std::string& line : lines)
        {
            std::size_t const start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            std::size_t const equals = line.find('=', start);
            if (equals == std::string::npos)
                continue;

            std::string name = line.substr(start, equals - start);
            name.erase(name.find_last_not_of(" \t") + 1);
            if (name != key)
                continue;

            line = key + " = " + value;
            replaced = true;
        }

        if (!replaced)
            lines.push_back(key + " = " + value);

        fs::copy_file(file, fs::path(file).concat(".before-bench"), fs::copy_options::overwrite_existing, error);
        if (error)
            out(Acore::StringFormat("  Could not back {} up ({}); writing it anyway.", file.string(),
                error.message()));

        fs::path const partial = fs::path(file).concat(".partial");
        {
            std::ofstream output(partial, std::ios::trunc);
            for (std::string const& line : lines)
                output << line << "\n";
            if (!output)
            {
                out(Acore::StringFormat("  Could not write {}.", partial.string()));
                return false;
            }
        }

        fs::rename(partial, file, error);
        if (error)
        {
            out(Acore::StringFormat("  Could not replace {} ({}).", file.string(), error.message()));
            return false;
        }

        out(Acore::StringFormat("  {}: {} = {}{}", file.string(), key, value,
            replaced ? "" : " (added at the end)"));
        return true;
    }

    std::string Bytes(uintmax_t bytes)
    {
        if (bytes >= uintmax_t(1) << 30)
            return Acore::StringFormat("{:.1f} GB", double(bytes) / double(uintmax_t(1) << 30));
        if (bytes >= uintmax_t(1) << 20)
            return Acore::StringFormat("{:.1f} MB", double(bytes) / double(uintmax_t(1) << 20));
        if (bytes >= uintmax_t(1) << 10)
            return Acore::StringFormat("{:.1f} KB", double(bytes) / double(uintmax_t(1) << 10));

        return Acore::StringFormat("{} B", bytes);
    }

    /// A run name is one plain path component: no separators, no "." or "..", not the archive.
    bool IsRunName(std::string const& name)
    {
        return !name.empty() && name != "." && name != ".." && name != ARCHIVE_DIR
            && name.find_first_of("/\\") == std::string::npos;
    }

    /// Delete `path` (a file or a directory tree) and say so. False only when it existed and could not be removed.
    bool RemovePath(fs::path const& path, AnimusForge::LineSink const& out)
    {
        std::error_code error;
        if (!fs::exists(path, error))
            return true;

        uintmax_t const size = SizeOf(path);
        fs::remove_all(path, error);
        if (error)
        {
            out(Acore::StringFormat("  Could not remove {}: {}", path.string(), error.message()));
            return false;
        }

        out(Acore::StringFormat("  Removed {} ({})", path.string(), Bytes(size)));
        LOG_INFO("module.animus", "forge clean removed {} ({})", path.string(), Bytes(size));
        return true;
    }

    std::string Age(fs::path const& path)
    {
        std::error_code error;
        auto const written = fs::last_write_time(path, error);
        if (error)
            return "-";

        double const seconds = std::chrono::duration<double>(fs::file_time_type::clock::now() - written).count();
        return AnimusForge::Format::Duration(seconds) + " ago";
    }
}

bool AnimusForge::Forge::Enabled(LineSink const& out) const
{
    if (_config.Enable)
        return true;

    out("Animus Forge is disabled (AnimusForge.Enable = 0)");
    return false;
}

bool AnimusForge::Forge::ValidScenario(std::string const& scenario, LineSink const& out) const
{
    std::vector<std::string> const names = Animus::ScenarioNames();
    if (std::find(names.begin(), names.end(), scenario) != names.end())
        return true;

    out(Acore::StringFormat("Unknown scenario '{}'. Available: {}", scenario, Join(names)));
    return false;
}

void AnimusForge::Forge::ReportWorkers(LineSink const& out) const
{
    if (_config.Cluster != ForgeConfig::ClusterRole::Host)
        return;

    std::vector<ClusterLink::WorkerStatus> const workers = _cluster.Workers();
    out(Acore::StringFormat("Cluster: {} worker{} connected", workers.size(), workers.size() == 1 ? "" : "s"));
    for (ClusterLink::WorkerStatus const& worker : workers)
        out(worker.Progress.empty() ? Acore::StringFormat("  {}: no report yet", worker.Sim)
            : Acore::StringFormat("  {}: {} ({:.0f} s ago)", worker.Sim, worker.Progress, worker.SecondsAgo));
}

void AnimusForge::Forge::CommandStatus(LineSink const& out)
{
    if (!Enabled(out))
        return;

    if (_state != State::Idle)
    {
        _learner.Poll();
        _monitor.Report(RunConfig(), Snapshot(false), PlanRows(_plan, true), out, out, false);
    }
    else
    {
        out(_request == Request::Start ? "Forge: idle, starting a plan on the next tick" : "Forge: idle");

        TextTable table({ { "Setting" }, { "Value" } });
        table.AddRow({ "queue (forge start)", Join(DefaultQueue()) });
        table.AddRow({ "queue (forge fast)", Join(FastQueue()) });
        table.AddRow({ "policy", _config.Policy });
        table.AddRow({ "envs", Acore::StringFormat("{}, a decision every {} ms, {} s episodes", _config.Envs,
            _config.DecisionMs, _config.EpisodeSeconds) });

        if (_config.IsRemote())
        {
            table.AddRow({ "learner", _config.LearnerAutoStart ? "started with each scenario" : "started by hand" });
            table.AddRow({ "GPU mode", _config.GpuSummary });
            auto const setting = [](std::string const& value) { return value.empty() ? std::string("auto") : value; };
            table.AddRow({ "learner placement", Acore::StringFormat("update {}, rollouts {}, cpus {}; the sim's "
                "map update on cpus {}", setting(_config.LearnerTrainDevice), setting(_config.LearnerRolloutDevice),
                setting(_config.LearnerCpus), Acore::CpuPlacement::Describe(sMapMgr->GetMapUpdater()->PoolCpus())) });
            table.AddRow({ "learner socket", _config.SocketPath });
            table.AddRow({ "learner log", _config.LearnerLogFile });
        }

        table.AddRow({ "runs", _config.RunsDir().string() });
        table.AddRow({ "models", _config.ModelDir });
        table.AddRow({ "fast test run (forge fast)", FastSummary() + "; in " + _fastConfig.OutputDir });
        table.AddRow({ "progress report", _progressInterval ? Acore::StringFormat("every {} and at each stage's end",
            Format::Duration(_progressInterval)) : "`forge status`, and at each stage's end" });

        if (_lastPlan)
        {
            std::vector<std::string> outcomes;
            for (PlanEntry const& entry : _lastPlan->Entries)
                outcomes.push_back(entry.Scenario + " " + OutcomeName(entry.Result));

            table.AddRow({ _lastPlan->Fast ? "last plan (fast)" : "last plan", Join(outcomes) });
        }

        table.Write(out, "  ");
    }

    if (_export.IsRunning())
        out(Acore::StringFormat("  Export of {} running for {} (pid {}); output in {}", _exportScenario,
            Format::Duration(_export.RunningSeconds()), _export.Pid(), _export.LogFile()));

    if (_pauseRequested)
        out("  Pause requested: it takes effect after the current decision.");

    if (_request == Request::Cancel)
        out("  Cancel requested: it takes effect as soon as the learner is disconnected.");
    else if (_request == Request::Skip)
        out("  Skip requested: it takes effect as soon as the learner is disconnected.");

    ReportWorkers(out);
}

void AnimusForge::Forge::CommandScenarios(LineSink const& out)
{
    TextTable table({ { "Scenario" }, { "Run" }, { "Env steps", TextTable::Align::Right },
        { "Best score", TextTable::Align::Right }, { "Last written", TextTable::Align::Right } });

    for (std::string const& name : Animus::ScenarioNames())
    {
        fs::path const run = _config.RunsDir() / name;
        std::error_code error;

        std::string state = "-";
        std::string steps = "-";
        std::string best = "-";
        std::string written = "-";

        ProgressFile finished;
        if (_state != State::Idle && name == _current)
            state = StateName();
        else if (finished.Load(run / "finished.json"))
            state = "finished (" + finished.Text("reason") + ")";
        else if (fs::exists(run / "latest.pt", error))
            state = "checkpoint (resumable)";

        ProgressFile progress;
        if (progress.Load(run / "progress.json"))
        {
            steps = Format::OrDash(progress.Number("env_steps"), Format::Compact) + " / "
                + Format::OrDash(progress.Number("total_env_steps"), Format::Compact);
            best = Format::OrDash(progress.Number("best_score"), Format::Metric);
            written = Age(run / "progress.json");
        }
        else if (fs::exists(run / "latest.pt", error))
            written = Age(run / "latest.pt");

        table.AddRow({ name, state, steps, best, written });
    }

    out(Acore::StringFormat("Scenarios (runs in {}):", _config.RunsDir().string()));
    table.Write(out, "  ");

    uint32 models = 0;
    std::error_code error;
    for (fs::directory_iterator it(_config.ModelDir, error), end; !error && it != end; it.increment(error))
        if (it->path().extension() == ".amdl")
            ++models;

    out(Acore::StringFormat("  {} exported model{} in {}", models, models == 1 ? "" : "s", _config.ModelDir));
}

bool AnimusForge::Forge::CommandStart(std::vector<std::string> scenarios, LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state != State::Idle || _request == Request::Start)
    {
        out(Acore::StringFormat("{} is {}: `forge cancel` it first.", _current, StateName()));
        return false;
    }

    // Without names: AnimusForge.Queue (every curriculum stage when empty), leaving out the stages that already
    // advanced (AnimusForge.Queue.SkipFinished), so a restarted server carries on where training stopped. Named
    // scenarios always train.
    bool const fromQueue = scenarios.empty();
    if (fromQueue)
    {
        for (std::string const& scenario : DefaultQueue())
        {
            if (_config.QueueSkipFinished && _config.IsRemote() && RunAdvanced(_config, scenario))
                out(Acore::StringFormat("  Skipping {}: its run already finished and moved on (runs/{}/finished.json;"
                    " move the run away or set AnimusForge.Queue.SkipFinished = 0 to train it again).", scenario,
                    scenario));
            else
                scenarios.push_back(scenario);
        }
    }

    if (scenarios.empty())
    {
        out(fromQueue ? "Nothing to start: every queued scenario already finished (AnimusForge.Queue.SkipFinished)."
            : "Nothing to start: name the scenarios (`forge start <scenario> [scenario ...]`).");
        return false;
    }

    Plan plan;
    plan.Policy = _config.Policy;
    plan.LocalEpisodes = _config.QueueLocalEpisodes;
    for (std::string const& scenario : scenarios)
    {
        if (!ValidScenario(scenario, out))
            return false;

        plan.Entries.push_back({ scenario, false });
    }

    _requested = std::move(plan);
    _request = Request::Start;

    out(Acore::StringFormat("Starting {} with policy {}.", Join(scenarios), _config.Policy));
    WarnSeedOrder(_config, scenarios, out);
    if (_config.IsRemote())
        out("  Each scenario trains from scratch: an earlier run in runs/<scenario>/ is moved to runs/_archive/.");

    return true;
}

bool AnimusForge::Forge::CommandFast(std::vector<std::string> scenarios, LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state != State::Idle || _request == Request::Start)
    {
        out(Acore::StringFormat("{} is {}: `forge cancel` it first.", _current, StateName()));
        return false;
    }

    // A leading step count is this run's budget: `forge fast 30M`, `forge fast 30M stage8_duel`. Only the first
    // word is considered, and only if it parses as one, so a scenario name is never eaten by mistake.
    uint64 budget = _config.FastBudget;
    if (!scenarios.empty())
        if (std::optional<uint64> const named = ParseBudget(scenarios.front()))
        {
            budget = *named;
            scenarios.erase(scenarios.begin());
        }

    // The budget is per invocation, so the fast profile is rebuilt around it; ConfigFor(plan) hands the learner
    // this one.
    _fastConfig = _config.FastProfile(budget);

    // Without names: the whole fast queue (AnimusForge.Fast.Queue, else every curriculum stage), every stage trained
    // again from scratch in order, so one `forge fast` is a full run of the curriculum with nothing skipped.
    if (scenarios.empty())
        scenarios = FastQueue();

    if (scenarios.empty())
    {
        out("Nothing to start: AnimusForge.Fast.Queue names no scenarios and the curriculum has no stages.");
        return false;
    }

    Plan plan;
    plan.Policy = _fastConfig.Policy;
    plan.Fast = true;
    plan.Budget = budget;
    for (std::string const& scenario : scenarios)
    {
        if (!ValidScenario(scenario, out))
            return false;

        plan.Entries.push_back({ scenario, false });
    }

    _requested = std::move(plan);
    _request = Request::Start;

    out(Acore::StringFormat("Fast run of {}: {}.", Join(scenarios), FastSummary()));
    out(Acore::StringFormat("  Learner settings from {} over each stage's config. Every stage trains its whole "
        "budget: convergence is off, so none of them stops early when its score flattens.",
        _fastConfig.FastLearnerOverlay));
    out(Acore::StringFormat("  Runs, layouts and models go to {}: each scenario trains from scratch there (an earlier "
        "fast run of it is archived), seeding from the fast runs of the stages it builds on. The runs in {} are not "
        "touched.", _fastConfig.OutputDir, _config.RunsDir().string()));
    WarnSeedOrder(_fastConfig, scenarios, out);
    return true;
}

bool AnimusForge::Forge::CommandResume(std::vector<std::string> scenarios, LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state == State::Paused)
    {
        if (!scenarios.empty() && (scenarios.size() > 1 || scenarios.front() != _current))
        {
            out(Acore::StringFormat("{} is paused: `forge resume` continues it; `forge cancel` it to run something "
                "else.", _current));
            return false;
        }

        _resumeRequested = true;
        out(Acore::StringFormat("Resuming {}.", _current));
        return true;
    }

    // The learner of a running scenario died: start a new one from its last checkpoint without rebuilding the envs.
    if (_state == State::Training && !_server.HasClient() && !_learner.IsRunning() && _config.LearnerAutoStart)
    {
        if (!scenarios.empty() && (scenarios.size() > 1 || scenarios.front() != _current))
        {
            out(Acore::StringFormat("{} is waiting for its learner: `forge resume` restarts it, `forge cancel` stops "
                "the plan.", _current));
            return false;
        }

        std::error_code error;
        bool const resume = fs::exists(RunConfig().RunsDir() / _current / "latest.pt", error);
        _plan.Entries[_plan.Index].Resume = resume;
        // As the scenario started it: as many ranks, and a cluster host's workers' sims.
        ForgeConfig learnerConfig = RunConfig();
        learnerConfig.LearnerRanks = PoolRanks(learnerConfig.LearnerRanks);
        learnerConfig.ClusterSims = _clusterSims;
        _learnerStarted = _learner.Start(learnerConfig, _current, resume);
        if (!_learnerStarted)
        {
            out(Acore::StringFormat("Could not restart the learner for {}; see the server log.", _current));
            return false;
        }

        out(Acore::StringFormat("Restarted the learner for {}{}.", _current,
            resume ? " from its latest checkpoint" : " from scratch (it had no checkpoint yet)"));
        return true;
    }

    if (_state != State::Idle || _request == Request::Start)
    {
        out(Acore::StringFormat("{} is {}: `forge pause` pauses it, `forge cancel` stops it.", _current,
            StateName()));
        return false;
    }

    Plan plan;
    plan.Policy = _config.Policy;

    if (scenarios.empty())
    {
        if (!_lastPlan || !_lastPlan->Remote())
        {
            out("Nothing to resume since the server started: name it (`forge resume <scenario> [scenario ...]`).");
            return false;
        }

        // A fast test run resumes as one, in its own output directory.
        plan.Policy = _lastPlan->Policy;
        plan.Fast = _lastPlan->Fast;

        // The first scenario of the last plan that did not end on its own resumes; the ones after it follow.
        std::size_t first = _lastPlan->Entries.size();
        for (std::size_t i = 0; i < _lastPlan->Entries.size(); ++i)
        {
            Outcome const outcome = _lastPlan->Entries[i].Result;
            if (outcome != Outcome::Done && outcome != Outcome::Skipped)
            {
                first = i;
                break;
            }
        }

        if (first == _lastPlan->Entries.size())
        {
            out("Every scenario of the last plan ended: name one to resume (`forge resume <scenario>`).");
            return false;
        }

        for (std::size_t i = first; i < _lastPlan->Entries.size(); ++i)
            plan.Entries.push_back({ _lastPlan->Entries[i].Scenario, i == first });
    }
    else
    {
        if (!_config.IsRemote())
        {
            out(Acore::StringFormat("Resume continues a training run, but AnimusForge.Policy is '{}'.",
                _config.Policy));
            return false;
        }

        for (std::string const& scenario : scenarios)
        {
            if (!ValidScenario(scenario, out))
                return false;

            plan.Entries.push_back({ scenario, plan.Entries.empty() });
        }
    }

    std::string const& resumed = plan.Entries.front().Scenario;
    std::error_code error;
    if (!fs::exists(ConfigFor(plan).RunsDir() / resumed / "latest.pt", error))
    {
        out(Acore::StringFormat("{} has no runs/{}/latest.pt to resume; `forge start {}` trains it from scratch.",
            resumed, resumed, resumed));
        return false;
    }

    std::vector<std::string> names;
    for (PlanEntry const& entry : plan.Entries)
        names.push_back(entry.Scenario);

    bool const fast = plan.Fast;

    _requested = std::move(plan);
    _request = Request::Start;

    out(Acore::StringFormat("Resuming {}{} from its latest checkpoint{}.", fast ? "the fast test run of " : "",
        names.front(), names.size() > 1
        ? Acore::StringFormat(", then {} from scratch", Join(std::vector<std::string>(names.begin() + 1, names.end())))
        : ""));
    return true;
}

bool AnimusForge::Forge::CommandPause(LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state == State::Paused || _pauseRequested)
    {
        out(Acore::StringFormat("{} is already paused; `forge resume` continues it.", _current));
        return false;
    }

    if (_state == State::Idle)
    {
        out("Nothing is running.");
        return false;
    }

    _pauseRequested = true;
    out(Acore::StringFormat("Pausing {} after the current decision.", _current));
    return true;
}

bool AnimusForge::Forge::CommandCancel(LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state == State::Idle && _request == Request::Start)
    {
        _request = Request::None;
        _requested = {};
        out("Cancelled the plan before it started.");
        return true;
    }

    if (_state == State::Idle)
    {
        out("Nothing is running.");
        return false;
    }

    _request = Request::Cancel;
    bool const learnerSaves = _plan.Remote() && (_learner.IsRunning() || _server.HasClient());
    out(Acore::StringFormat("Cancelling {}{}.", _current,
        learnerSaves ? ": the learner saves its latest checkpoint, then the sim goes idle" : ""));
    return true;
}

bool AnimusForge::Forge::CommandSkip(LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state == State::Idle)
    {
        out("Nothing is running.");
        return false;
    }

    _request = Request::Skip;
    std::size_t const next = _plan.Index + 1;
    out(Acore::StringFormat("Skipping {}{}; {}.", _current,
        _plan.Remote() ? " (the learner saves its latest checkpoint)" : "",
        next < _plan.Entries.size() ? "next: " + _plan.Entries[next].Scenario
            : "it is the last one, so the plan ends"));
    return true;
}

bool AnimusForge::Forge::CommandRun(std::string const& scenario, std::string const& policy, uint32 episodes,
    LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state != State::Idle || _request == Request::Start)
    {
        out(Acore::StringFormat("{} is {}: `forge cancel` it first.", _current, StateName()));
        return false;
    }

    if (!ValidScenario(scenario, out))
        return false;

    if (policy == "remote")
    {
        out(Acore::StringFormat("`forge start {}` trains it with the learner; `forge run` takes a local policy.",
            scenario));
        return false;
    }

    Plan plan;
    plan.Policy = policy;
    plan.LocalEpisodes = episodes;
    plan.Entries.push_back({ scenario, false });

    _requested = std::move(plan);
    _request = Request::Start;

    out(Acore::StringFormat("Running {} with policy {} {}.", scenario, policy,
        episodes ? Acore::StringFormat("for {} episodes", Format::Count(episodes)) : "until `forge cancel`"));
    return true;
}

bool AnimusForge::Forge::CommandTalents(std::string const& playerClass, std::string const& spec, uint32 points,
    std::string const& plan, LineSink const& out)
{
    using namespace Animus::Curriculum;

    if (!Enabled(out))
        return false;

    auto const profile = std::find_if(ClassProfiles().begin(), ClassProfiles().end(),
        [&playerClass](ClassProfile const& candidate) { return candidate.Name == playerClass; });
    if (profile == ClassProfiles().end())
    {
        std::vector<std::string> names;
        for (ClassProfile const& candidate : ClassProfiles())
            names.push_back(candidate.Name);

        out(Acore::StringFormat("Unknown class '{}'. Available: {}", playerClass, Join(names)));
        return false;
    }

    // Building the assets is what a stage does on its first episode of this class: trainer spells, gear
    // pools, the talent trees. It is cached from here on, so asking twice is cheap.
    ClassAssets const& assets = ClassAssets::For(*profile);
    if (!assets.Talents || profile->Specs.empty())
    {
        out(Acore::StringFormat("{} has no talent trees to show", playerClass));
        return false;
    }

    SpecProfile const* chosen = &profile->Specs.front();
    if (!spec.empty())
    {
        auto const named = std::find_if(profile->Specs.begin(), profile->Specs.end(),
            [&spec](SpecProfile const& candidate) { return candidate.Name == spec; });
        if (named == profile->Specs.end())
        {
            std::vector<std::string> names;
            for (SpecProfile const& candidate : profile->Specs)
                names.push_back(candidate.Name);

            out(Acore::StringFormat("{} has no spec '{}'. Available: {}", playerClass, spec, Join(names)));
            return false;
        }

        chosen = &*named;
    }

    TalentBuilder const& talents = *assets.Talents;
    TalentBuilder::Build build;
    if (plan == "random")
        build = talents.Random(chosen->TabPage, points);
    else if (plan == "noisy")
        build = talents.Noisy(chosen->Name, chosen->TabPage, points, TALENTS_NOISE_POINTS);
    else if (plan.empty() || plan == "standard")
        build = talents.Standard(chosen->Name, chosen->TabPage, points);
    else
    {
        out(Acore::StringFormat("Unknown plan '{}'. Available: standard, noisy, random", plan));
        return false;
    }

    out(Acore::StringFormat("{} {} ({} plan): {} points", playerClass, chosen->Name,
        plan.empty() ? "standard" : plan, points));

    TextTable table({ { "Tree" }, { "Row", TextTable::Align::Right }, { "Talent" },
        { "Ranks", TextTable::Align::Right } });
    uint32 spent = 0;
    for (uint8 tab = 0; tab < TalentBuilder::TREE_COUNT; ++tab)
    {
        for (TalentBuilder::Talent const& talent : talents.Talents())
        {
            uint32 const index = uint32(&talent - talents.Talents().data());
            if (talent.Tab != tab || !build.Ranks[index])
                continue;

            spent += build.Ranks[index];
            table.AddRow({ tab == chosen->TabPage ? Acore::StringFormat("{} (spec)", tab) : std::to_string(tab),
                std::to_string(talent.Row + 1), talent.Name,
                Acore::StringFormat("{}/{}", build.Ranks[index], talent.MaxRank) });
        }
    }

    table.Write(out, "  ");

    std::vector<std::string> trees;
    for (uint8 tab = 0; tab < TalentBuilder::TREE_COUNT; ++tab)
        trees.push_back(Acore::StringFormat("{}{}", build.TreePoints[tab], tab == chosen->TabPage ? " (spec)" : ""));

    out(Acore::StringFormat("  {} points placed as {}; {} left over. The last row of a tree needs {} points in "
        "it.", spent, Join(trees, " / "), points - std::min(points, spent), TalentBuilder::SPEC_TREE_POINTS - 1));
    return true;
}

bool AnimusForge::Forge::CommandBench(std::string const& scenario, LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_state != State::Idle || _request == Request::Start)
    {
        out(Acore::StringFormat("{} is {}: `forge cancel` it first.", _current, StateName()));
        return false;
    }

    std::string const benchScenario = scenario.empty() ? _config.Bench.Scenario : scenario;
    if (!ValidScenario(benchScenario, out))
        return false;

    if (!KnowsPolicy(_config.Bench.Policy) && _config.Bench.Policy != "random")
        out(Acore::StringFormat("  AnimusForge.Bench.Policy '{}' may not exist for {}; a trial that cannot run it is "
            "reported as failed.", _config.Bench.Policy, benchScenario));

    // Every thread count against every env count, smaller envs first: a memory-heavy trial then only skips the
    // bigger ones of its thread count.
    _benchTrials.clear();
    std::vector<uint32> envs = _config.Bench.Envs;
    std::sort(envs.begin(), envs.end());
    for (uint32 threads : _config.Bench.Threads)
        for (uint32 count : envs)
        {
            if (count > _config.Bench.MaxEnvs)
                continue;

            BenchTrial trial;
            trial.MapThreads = threads;
            trial.Envs = count;
            _benchTrials.push_back(trial);
        }

    if (_benchTrials.empty())
    {
        out("Nothing to benchmark: AnimusForge.Bench.Threads and .Envs are empty (or over .MaxEnvs).");
        return false;
    }

    _benching = true;
    Map::DetailedObjectTiming.store(true, std::memory_order_relaxed);
    _benchLearnerPhase = false;
    _benchTrial = 0;
    _benchScenario = benchScenario;

    _requested = BenchPlan(benchScenario, _benchTrials);
    _request = Request::Start;

    uint32 const perTrial = (_config.Bench.WarmupTicks + _config.Bench.MeasureTicks) * _config.DecisionMs / 1000;
    out(Acore::StringFormat("Benchmarking {} with policy {}: {} trials of about {} s of game time each, then the {} "
        "fastest again with the learner. `forge cancel` stops it.", benchScenario, _config.Bench.Policy,
        _benchTrials.size(), perTrial, _config.Bench.LearnerTop));
    out("  Nothing is trained: every trial runs in the bench output directory and real runs are untouched.");
    return true;
}

bool AnimusForge::Forge::CommandBenchApply(LineSink const& out)
{
    namespace fs = std::filesystem;

    if (!Enabled(out))
        return false;

    if (_benching)
    {
        out("The benchmark is still running: `forge cancel` or wait for it to finish.");
        return false;
    }

    fs::path const path = fs::path(_config.Bench.OutputDir) / "bench.json";
    std::error_code error;
    if (!fs::exists(path, error))
    {
        out(Acore::StringFormat("No benchmark results in {}: run `forge bench` first.", path.string()));
        return false;
    }

    std::ifstream file(path);
    std::string const text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    boost::json::value parsed = boost::json::parse(text, error);
    if (error || !parsed.is_object() || !parsed.as_object().contains("best"))
    {
        out(Acore::StringFormat("{} has no winning trial; run `forge bench` again.", path.string()));
        return false;
    }

    boost::json::object const& best = parsed.as_object().at("best").as_object();
    auto const number = [&best](char const* key) -> uint32
    {
        auto const value = best.if_contains(key);
        return value && value->is_int64() ? uint32(value->as_int64()) : 0;
    };

    uint32 const threads = number("map_threads");
    uint32 const envs = number("envs");
    uint32 const torchThreads = number("torch_threads");
    if (!threads || !envs)
    {
        out(Acore::StringFormat("{} does not name a thread and env count.", path.string()));
        return false;
    }

    bool ok = WriteConfigValue(sConfigMgr->GetFilename(), "MapUpdate.Threads", std::to_string(threads), out);
    ok = WriteConfigValue(ModuleConfigFile(), "AnimusForge.Envs", std::to_string(envs), out) && ok;
    if (torchThreads)
        ok = WriteConfigValue(ModuleConfigFile(), "AnimusForge.Learner.TorchThreads", std::to_string(torchThreads),
            out) && ok;

    if (!ok)
        return false;

    out(Acore::StringFormat("Applied: MapUpdate.Threads = {}, AnimusForge.Envs = {}{}.", threads, envs,
        torchThreads ? Acore::StringFormat(", AnimusForge.Learner.TorchThreads = {}", torchThreads) : ""));
    out("  The thread count takes effect when the worldserver restarts; the env count at the next `forge start`.");
    return true;
}

bool AnimusForge::Forge::CommandExport(std::string scenario, std::string const& checkpoint, LineSink const& out)
{
    if (!Enabled(out))
        return false;

    if (_export.IsRunning())
    {
        out(Acore::StringFormat("The export of {} is still running (pid {}).", _exportScenario, _export.Pid()));
        return false;
    }

    // Without a name: the current or last scenario, from the fast output directory when that plan was a fast one.
    ForgeConfig const* config = &_config;
    if (scenario.empty())
    {
        if (_state != State::Idle)
        {
            scenario = _current;
            config = &RunConfig();
        }
        else if (_lastPlan)
        {
            scenario = _lastPlan->Entries[_lastPlan->Index].Scenario;
            config = &ConfigFor(*_lastPlan);
        }
    }

    if (scenario.empty())
    {
        out("Which scenario? `forge export <scenario> [best|latest]`");
        return false;
    }

    if (!IsRunName(scenario))
    {
        out(Acore::StringFormat("'{}' is not a scenario name.", scenario));
        return false;
    }

    if (!checkpoint.empty() && checkpoint != "best" && checkpoint != "latest")
    {
        out("The checkpoint is `best` (the best evaluation) or `latest` (the last save).");
        return false;
    }

    fs::path const run = config->RunsDir() / scenario;
    std::error_code error;
    fs::path file = run / ((checkpoint.empty() ? "best" : checkpoint) + ".pt");
    if (checkpoint.empty() && !fs::exists(file, error))
        file = run / "latest.pt";

    if (!fs::exists(file, error))
    {
        out(Acore::StringFormat("{} has no {} to export.", scenario, checkpoint.empty()
            ? "best.pt or latest.pt" : (run / (checkpoint + ".pt")).string()));
        return false;
    }

    fs::create_directories(config->ModelDir, error);
    if (error)
    {
        out(Acore::StringFormat("Cannot create {}: {}", config->ModelDir, error.message()));
        return false;
    }

    fs::path const log = fs::path(_config.LearnerLogFile).parent_path() / EXPORT_LOG;
    std::vector<std::string> args =
    {
        _config.LearnerPython, "-u", "-m", "animus.export", "--checkpoint", file.string(), "--out", config->ModelDir,
        // stage.json there names the models, and each model's layout manifest is copied beside it.
        "--layouts-dir", config->LayoutsDir().string(),
    };

    if (!_export.Start(std::move(args), _config.LearnerWorkDir, log.string()))
    {
        out("Could not start the export; see the server log.");
        return false;
    }

    _exportScenario = scenario;
    _exportModelDir = config->ModelDir;
    out(Acore::StringFormat("Exporting {} ({}) to {}; output in {}.", scenario, file.filename().string(),
        config->ModelDir, log.string()));
    out("  Models stay in the forge's folder: copy them to a game server by hand.");
    return true;
}

bool AnimusForge::Forge::CommandClean(std::string const& target, std::string const& scenario, LineSink const& out)
{
    if (!Enabled(out))
        return false;

    fs::path const runs = _config.RunsDir();
    fs::path const exportLog = fs::path(_config.LearnerLogFile).parent_path() / EXPORT_LOG;
    bool const busy = _state != State::Idle || _request == Request::Start;

    auto const cleanArchive = [&]()
    {
        // A learner that is starting a fresh run moves the old one into the archive.
        if (_learner.IsRunning() && !_server.HasClient())
        {
            out("A learner is starting and may be archiving its run: try again once it has connected.");
            return false;
        }

        return RemovePath(runs / ARCHIVE_DIR, out);
    };

    auto const cleanExports = [&]()
    {
        if (_export.IsRunning())
        {
            out("An export is running: try again when it has finished.");
            return false;
        }

        bool ok = true;
        std::error_code error;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(_config.ModelDir, error), end; !error && it != end; it.increment(error))
            if (it->is_regular_file(error) && (it->path().extension() == ".amdl" || it->path().extension() == ".json"))
                files.push_back(it->path());

        for (fs::path const& file : files)
            ok = RemovePath(file, out) && ok;

        return ok;
    };

    auto const cleanFast = [&]()
    {
        if ((_state != State::Idle && _plan.Fast) || (_request == Request::Start && _requested.Fast))
        {
            out("A fast test run is running or about to: `forge cancel` it first.");
            return false;
        }

        if (_export.IsRunning() && _exportModelDir == _fastConfig.ModelDir)
        {
            out("An export of a fast run is running: try again when it has finished.");
            return false;
        }

        return RemovePath(_fastConfig.OutputDir, out);
    };

    auto const cleanLogs = [&]()
    {
        if (_learner.IsRunning() || _export.IsRunning())
        {
            out("The learner or an export is still writing its log: try again when it has stopped.");
            return false;
        }

        bool learner = RemovePath(_config.LearnerLogFile, out);
        for (uint32 rank = 1; rank < 16; ++rank)
            learner = RemovePath(LearnerProcess::RankLogFile(_config.LearnerLogFile, rank), out) && learner;
        return RemovePath(exportLog, out) && learner;
    };

    bool ok = true;
    if (target == "archive")
        ok = cleanArchive();
    else if (target == "scenario")
    {
        if (!IsRunName(scenario))
        {
            out("Which run? `forge clean scenario <scenario>` removes runs/<scenario>/.");
            return false;
        }

        bool const requested = std::any_of(_requested.Entries.begin(), _requested.Entries.end(),
            [&scenario](PlanEntry const& entry) { return entry.Scenario == scenario; });
        if ((_state != State::Idle && scenario == _current) || (_request == Request::Start && requested))
        {
            out(Acore::StringFormat("{} is running or about to: `forge cancel` it first.", scenario));
            return false;
        }

        ok = RemovePath(runs / scenario, out);
    }
    else if (target == "exports")
        ok = cleanExports();
    else if (target == "fast")
        ok = cleanFast();
    else if (target == "logs")
        ok = cleanLogs();
    else if (target == "all")
    {
        if (busy || _export.IsRunning() || _learner.IsRunning())
        {
            out("`forge clean all` needs an idle sim with no learner or export running.");
            return false;
        }

        std::error_code error;
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(runs, error), end; !error && it != end; it.increment(error))
            if (it->is_directory(error))
                dirs.push_back(it->path());

        for (fs::path const& dir : dirs)
            ok = RemovePath(dir, out) && ok;

        ok = cleanExports() && ok;
        ok = cleanFast() && ok;
        ok = cleanLogs() && ok;
    }
    else
    {
        out("Usage: forge clean archive | scenario <scenario> | exports | fast | logs | all");
        return false;
    }

    out(ok ? "Clean finished." : "Clean finished with errors.");
    return ok;
}

std::string AnimusForge::Forge::FastSummary() const
{
    // The total is worth saying out loud: a fast run of the whole curriculum at its default budget is a day or
    // more of training, and it used to be minutes. Nobody should start one by accident.
    uint64 const budget = _fastConfig.FastBudget;
    std::size_t const stages = FastQueue().size();
    return Acore::StringFormat("{} envs, {}, {}, {} steps a stage{}", _fastConfig.Envs,
        _fastConfig.Level ? Acore::StringFormat("level {}", _fastConfig.Level) : std::string("random levels"),
        _fastConfig.Classes.empty() ? "every class" : Join(_fastConfig.Classes),
        Format::Count(budget),
        stages ? Acore::StringFormat(" ({} over {} stages)", Format::Count(budget * stages), stages)
            : std::string());
}

void AnimusForge::Forge::CommandProgress(std::optional<uint32> seconds, LineSink const& out)
{
    if (seconds)
    {
        _progressInterval = *seconds;

        // The next tick reports right away, then every interval.
        if (_progressInterval)
            _lastReport = std::chrono::steady_clock::now() - std::chrono::seconds(_progressInterval);
    }

    out(_progressInterval ? Acore::StringFormat("Progress report every {} while a scenario runs.",
        Format::Duration(_progressInterval)) : "Periodic progress report off: `forge status` shows it on demand, and each stage's end prints it.");
}
