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

#include "Progress.h"
#include "ForgeConfig.h"
#include "StringFormat.h"
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/version.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>

namespace
{
    /// What the sim's share of a decision went on. `observe` carries the mask, `final observe` is the same work
    /// without it, so their per-call difference is what mask building costs.
    std::string SimPartsNote(AnimusForge::SimSnapshot::CollectMs const& collect)
    {
        // Observe, reward and apply run inside the map tasks, so they are summed thread time already counted in
        // the world figure above, not time of their own on top of it. Reset is the world thread's, and is.
        return Acore::StringFormat("reward {:.2f} ms, final observe {:.2f} ms, apply {:.2f} ms, map-thread resets "
            "{:.2f} ms ({:.2f} episodes) (thread time inside the map update, not on top of it), reset {:.2f} ms on the "
            "world thread ({:.2f} episodes rebuilt per decision, {:.2f} characters reused)", collect.Reward,
            collect.FinalObserve, collect.Apply, collect.MapReset, collect.MapResetsPerTick, collect.Reset,
            collect.ResetsPerTick, collect.ReusedPerTick);
    }

    /// The rest of the observation's blocks after the largest, as "name ms", largest first.
    std::string ObserveBlocksNote(std::vector<std::pair<std::string, double>> const& blocks)
    {
        std::string note;
        for (std::size_t i = 1; i < blocks.size(); ++i)
            if (blocks[i].second >= 0.005)
                note += Acore::StringFormat("{}{} {:.2f}", note.empty() ? "" : ", ", blocks[i].first, blocks[i].second);
        return note + " ms (thread time per decision, every map)";
    }

    /// The map update as tasks: sum against wall is the parallelism it had, longest against wall whether one map
    /// is the critical path, and the CPUs say where the pinned workers actually ran.
    std::string MapTasksNote(AnimusForge::SimSnapshot::MapTasksMs const& tasks)
    {
        return Acore::StringFormat("{:.1f} tasks, sum {:.2f} ms ({:.1f}x wall), longest {:.2f} ms (last: map {} "
            "instance {} on cpu {}), cpus {} (per map update)", tasks.Tasks, tasks.Sum,
            tasks.Wall > 0.0 ? tasks.Sum / tasks.Wall : 0.0, tasks.Longest, tasks.SlowestMapId,
            tasks.SlowestInstanceId, tasks.SlowestCpu, AnimusForge::Format::Cpus(tasks.CpuMask));
    }

    /// Weight of the newest interval in the step rate average.
    constexpr double RATE_EMA_ALPHA = 0.3;

    /// Warn when the learner has not answered a STEP for this long.
    constexpr double STALL_SECONDS = 120.0;

    /// Warn when the step rate of the last interval falls this far below the run's average.
    constexpr double RATE_DROP_FRACTION = 0.30;

    /// Warn when entropy falls under this fraction of its first reported value.
    constexpr double ENTROPY_COLLAPSE_FRACTION = 0.25;

    /// Warn when the policy moves this much per update.
    constexpr double APPROX_KL_LIMIT = 0.05;
    constexpr double CLIP_FRACTION_LIMIT = 0.30;

    /// Clock slack when telling this run's progress.json from an earlier run's, in seconds.
    constexpr double STALE_SLACK = 5.0;

    /// Warn when the best score is still under the baseline after this many evaluations.
    constexpr double BELOW_BASELINE_EVALS = 2.0;

    /// Episode means shown in the report, when the scenario has them, in this order.
    constexpr char const* EPISODE_COLUMNS[] =
    {
        "dps", "killed", "died", "time_to_kill", "damage_taken", "kills", "pulls_cleared", "wipes", "owner_deaths",
        "healing", "owner_healing",
    };

    double UnixNow()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// "~14:32" today, "~Tue 14:32" within a week, "~Sep 21 14:32" after that.
    std::string ClockAfter(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0)
            return "";

        std::time_t const at = std::time(nullptr) + std::time_t(seconds);
        std::tm local{};
        localtime_r(&at, &local);

        char buffer[32];
        char const* format = seconds < 20 * 3600 ? "~%H:%M" : seconds < 6 * 86400 ? "~%a %H:%M" : "~%b %d %H:%M";
        std::strftime(buffer, sizeof(buffer), format, &local);
        return buffer;
    }

    /// "+4.0%" change from `before` to `now`, or "" when either is missing.
    std::string Change(std::optional<double> now, std::optional<double> before)
    {
        if (!now || !before || !std::isfinite(*now) || !std::isfinite(*before) || std::abs(*before) < 1e-12)
            return "";

        return AnimusForge::Format::Percent((*now - *before) / std::abs(*before), true) + " vs last";
    }
}

bool AnimusForge::ProgressFile::Load(std::filesystem::path const& path)
{
    std::ifstream file(path);
    if (!file)
        return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    return Parse(buffer.str());
}

bool AnimusForge::ProgressFile::Parse(std::string const& text)
{
    _numbers.clear();
    _strings.clear();

    boost::json::parse_options options;
#if BOOST_VERSION >= 108200
    options.allow_infinity_and_nan = true;  // Python's json.dumps writes NaN and Infinity
#endif

    boost::json::error_code error;
    boost::json::value const document = boost::json::parse(text, error, {}, options);
    if (error || !document.is_object())
        return false;

    // Top-level numbers, booleans (as 1 and 0) and strings; nested objects, arrays and nulls are left out.
    for (auto const& [key, value] : document.get_object())
    {
        std::string const name(key);
        if (value.is_string())
            _strings[name] = std::string(value.get_string());
        else if (value.is_bool())
            _numbers[name] = value.get_bool() ? 1.0 : 0.0;
        else if (value.is_number())
            _numbers[name] = value.to_number<double>();
    }

    return true;
}

std::optional<double> AnimusForge::ProgressFile::Number(std::string const& key) const
{
    auto const itr = _numbers.find(key);
    if (itr == _numbers.end())
        return std::nullopt;

    return itr->second;
}

std::string AnimusForge::ProgressFile::Text(std::string const& key) const
{
    auto const itr = _strings.find(key);
    return itr == _strings.end() ? std::string() : itr->second;
}

std::filesystem::path AnimusForge::ProgressPath(ForgeConfig const& config, std::string const& scenario)
{
    return config.RunsDir() / scenario / "progress.json";
}

namespace
{
    /// Most `extends:` links followed: a longer chain is a loop.
    constexpr uint32 MAX_EXTENDS_DEPTH = 16;

    /// The top-level total_env_steps of a learner config, following `extends:` as the learner does (the file's own
    /// value wins over its base's).
    std::optional<uint64> YamlTotalEnvSteps(std::filesystem::path const& path, uint32 depth = 0)
    {
        std::ifstream file(path);
        if (!file || depth > MAX_EXTENDS_DEPTH)
            return std::nullopt;

        static std::regex const key(R"(^total_env_steps:\s*([0-9_]+))");
        static std::regex const extends(R"(^extends:\s*["']?([^"'\s#]+))");

        std::optional<uint64> own;
        std::optional<std::filesystem::path> base;
        for (std::string line; std::getline(file, line);)
        {
            std::smatch match;
            if (std::regex_search(line, match, key))
            {
                std::string digits = match[1].str();
                digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                own = std::strtoull(digits.c_str(), nullptr, 10);
            }
            else if (std::regex_search(line, match, extends))
                base = path.parent_path() / match[1].str();
        }

        if (own || !base)
            return own;

        return YamlTotalEnvSteps(*base, depth + 1);
    }
}

std::optional<uint64> AnimusForge::ConfiguredTotalEnvSteps(ForgeConfig const& config, std::string const& scenario)
{
    std::optional<uint64> total = YamlTotalEnvSteps(config.LearnerConfigFor(scenario));

    // An `--overlay` file (the fast profile's) is merged over the config, as it is in the learner.
    for (std::size_t i = 0; i + 1 < config.LearnerArgs.size(); ++i)
        if (config.LearnerArgs[i] == "--overlay")
            if (std::optional<uint64> const overlay = YamlTotalEnvSteps(config.LearnerArgs[i + 1]))
                total = overlay;

    // `--set total_env_steps=N` in AnimusForge.Learner.Args wins, as it does in the learner.
    for (std::size_t i = 0; i + 1 < config.LearnerArgs.size(); ++i)
        if (config.LearnerArgs[i] == "--set" && config.LearnerArgs[i + 1].starts_with("total_env_steps="))
            total = std::strtoull(config.LearnerArgs[i + 1].c_str() + sizeof("total_env_steps=") - 1, nullptr, 10);

    return total;
}

std::optional<uint64> AnimusForge::ProgressMonitor::ConfiguredSteps(ForgeConfig const& config,
    std::string const& scenario) const
{
    auto cached = _configuredSteps.find(scenario);
    if (cached == _configuredSteps.end())
        cached = _configuredSteps.emplace(scenario, ConfiguredTotalEnvSteps(config, scenario)).first;

    return cached->second;
}

void AnimusForge::ProgressMonitor::Begin(std::string const& scenario)
{
    _scenario = scenario;
    _lastSample.reset();
    _rateEma.reset();
    _firstEntropy.reset();
    _previous = {};
    _configuredSteps.clear();       // re-read the configs once per scenario: they may have been edited
}

std::optional<double> AnimusForge::ProgressMonitor::StepRate(ProgressFile const& progress) const
{
    if (_rateEma)
        return _rateEma;

    // Before two samples: the average since the learner started, evaluations and updates included.
    std::optional<double> const steps = progress.Number("env_steps");
    std::optional<double> const resumed = progress.Number("resumed_env_steps");
    std::optional<double> const started = progress.Number("started_at");
    std::optional<double> const updated = progress.Number("updated_at");
    if (steps && started && updated && *updated - *started > 1.0 && *steps > resumed.value_or(0.0))
        return (*steps - resumed.value_or(0.0)) / (*updated - *started);

    return progress.Number("env_steps_per_sec");
}

void AnimusForge::ProgressMonitor::Advance(ProgressFile const& progress)
{
    std::optional<double> const steps = progress.Number("env_steps");
    std::optional<double> const updated = progress.Number("updated_at");
    if (!steps || !updated)
        return;

    if (_lastSample && *updated > _lastSample->UnixTime && *steps > _lastSample->EnvSteps)
    {
        double const rate = (*steps - _lastSample->EnvSteps) / (*updated - _lastSample->UnixTime);
        _rateEma = _rateEma ? RATE_EMA_ALPHA * rate + (1.0 - RATE_EMA_ALPHA) * *_rateEma : rate;
    }

    if (!_lastSample || *steps != _lastSample->EnvSteps)
        _lastSample = Sample{ *updated, *steps };

    if (!_firstEntropy)
        _firstEntropy = progress.Number("entropy");

    _previous.Reward = progress.Number("reward_per_decision");
    _previous.Entropy = progress.Number("entropy");
    _previous.Rate = StepRate(progress);
}

void AnimusForge::ProgressMonitor::Report(ForgeConfig const& config, SimSnapshot const& sim,
    std::vector<PlanRow> const& plan, LineSink const& info, LineSink const& warn, bool periodic)
{
    if (sim.Scenario != _scenario)
        Begin(sim.Scenario);

    // A progress.json older than this scenario is the previous run's, left until the new learner archives it (or
    // until a learner started by hand connects): not this run's progress.
    ProgressFile progress;
    bool haveProgress = sim.Remote && progress.Load(ProgressPath(config, sim.Scenario));
    if (haveProgress && progress.Number("started_at").value_or(0.0) < UnixNow() - sim.ScenarioSeconds - STALE_SLACK)
        haveProgress = false;

    std::vector<std::string> warnings;

    if (sim.Remote)
    {
        // A run's rate warnings compare the newest interval with the average before it, so look before Advance.
        if (periodic && haveProgress && _rateEma && _lastSample)
        {
            std::optional<double> const steps = progress.Number("env_steps");
            std::optional<double> const updated = progress.Number("updated_at");
            if (steps && updated && *updated > _lastSample->UnixTime && *steps > _lastSample->EnvSteps)
            {
                double const rate = (*steps - _lastSample->EnvSteps) / (*updated - _lastSample->UnixTime);
                if (rate < (1.0 - RATE_DROP_FRACTION) * *_rateEma)
                    warnings.push_back(Acore::StringFormat("Step rate dropped to {} steps/s, {} below the run's "
                        "average of {}", Format::Count(uint64(rate)), Format::Percent(1.0 - rate / *_rateEma),
                        Format::Count(uint64(*_rateEma))));
            }
        }

        ReportTraining(config, sim, haveProgress ? &progress : nullptr, info, warnings);
    }
    else
        ReportLocal(sim, info);

    if (plan.size() > 1)
        ReportPlan(config, plan, haveProgress ? &progress : nullptr, info);

    for (std::string const& warning : warnings)
        warn("Warning: " + warning);

    if (periodic && haveProgress)
        Advance(progress);
}

void AnimusForge::ProgressMonitor::ReportTraining(ForgeConfig const& config, SimSnapshot const& sim,
    ProgressFile const* progress, LineSink const& info, std::vector<std::string>& warnings) const
{
    std::string header = Acore::StringFormat("Forge: {}", sim.Scenario);
    if (sim.PlanSize > 1)
        header += Acore::StringFormat(" ({} of {})", sim.PlanPosition, sim.PlanSize);
    header += " | " + sim.State;
    if (progress)
    {
        std::string const phase = progress->Text("phase");
        if (!phase.empty() && phase != "training")
            header += " | learner " + phase;
        if (std::optional<double> const update = progress->Number("update"))
            header += Acore::StringFormat(" | update {}", Format::Count(uint64(*update)));
    }
    header += " | " + Format::Duration(sim.ScenarioSeconds);
    info(header);

    TextTable table({ { "Metric" }, { "Value", TextTable::Align::Right }, { "Note" } });

    std::string learner = sim.LearnerConnected ? "connected" : sim.LearnerRunning ? "starting" : "not connected";
    std::string learnerNote;
    if (sim.LearnerPid > 0)
        learnerNote = Acore::StringFormat("pid {}", sim.LearnerPid);
    if (sim.LearnerConnected && sim.SecondsSinceAct >= 0)
        learnerNote += Acore::StringFormat("{}last answer {} ago", learnerNote.empty() ? "" : ", ",
            Format::Duration(sim.SecondsSinceAct));
    if (!sim.LearnerRunning && !sim.LearnerConnected && !config.LearnerAutoStart)
        learnerNote = "start it by hand (AnimusForge.Learner.AutoStart = 0)";
    table.AddRow({ "learner", learner, learnerNote });

    if (sim.LearnerConnected && sim.SecondsSinceAct > STALL_SECONDS)
        warnings.push_back(Acore::StringFormat("The learner has not answered for {} (stalled, or a very long update "
            "or evaluation); see {}", Format::Duration(sim.SecondsSinceAct), config.LearnerLogFile));

    if (sim.LearnerFailed && !sim.LearnerConnected)
        warnings.push_back(Acore::StringFormat("The learner exited unexpectedly; see {}. `forge resume` continues from "
            "its last checkpoint, `forge cancel` stops the plan", config.LearnerLogFile));

    table.AddRow({ "sim", Acore::StringFormat("{} env steps/s", Format::Count(uint64(sim.EnvStepsPerSecond))),
        Acore::StringFormat("{} envs x {} agents, {} decisions/s, {} decisions", sim.Envs, sim.AgentsPerEnv,
            Format::Count(uint64(sim.TicksPerSecond)), Format::Count(sim.Decisions)) });
    table.AddRow({ "per decision", Acore::StringFormat("{:.1f} ms",
        sim.WorldMsPerTick + sim.SimMsPerTick + sim.LearnerMsPerTick),
        Acore::StringFormat("world {:.1f} ms (map update), sim {:.1f} ms, learner {:.1f} ms", sim.WorldMsPerTick,
            sim.SimMsPerTick, sim.LearnerMsPerTick) });
    table.AddRow({ "sim parts", Acore::StringFormat("{:.2f} ms observe", sim.Collect.Observe),
        SimPartsNote(sim.Collect) });
    table.AddRow({ "reset parts", Acore::StringFormat("{:.2f} ms create", sim.Collect.ResetCreate),
        Acore::StringFormat("place {:.2f} ms, configure {:.2f} ms, destroy {:.2f} ms, encounters {:.2f} ms, "
            "scatter {:.2f} ms, stock {:.2f} ms; prepare {:.2f} ms, seats {:.2f} ms in all, despawn {:.2f} ms; "
            "scenario reset {:.2f} ms (per decision)", sim.Collect.ResetPlace, sim.Collect.ResetConfigure,
            sim.Collect.ResetDestroy, sim.Collect.ResetEncounter, sim.Collect.ResetScatter, sim.Collect.ResetStock,
            sim.Collect.ResetPrepare, sim.Collect.ResetSeats, sim.Collect.ResetDespawn, sim.Collect.ResetScenario) });
    table.AddRow({ "world parts", Acore::StringFormat("{:.2f} ms objects", sim.World.Objects),
        Acore::StringFormat("sessions {:.2f}, players {:.2f}, scripts {:.2f}, relocation {:.2f}, visibility {:.2f}, "
            "delayed {:.2f} ms (thread time per decision, every map)", sim.World.Sessions, sim.World.Players,
            sim.World.Scripts, sim.World.Relocation, sim.World.Visibility, sim.World.Delayed) });
    if (!sim.ObserveBlocks.empty())
        table.AddRow({ "observe blocks", Acore::StringFormat("{:.2f} ms {}", sim.ObserveBlocks.front().second,
            sim.ObserveBlocks.front().first), ObserveBlocksNote(sim.ObserveBlocks) });
    if (!sim.ProbeNote.empty())
        table.AddRow({ "ground probe", "", sim.ProbeNote });
    table.AddRow({ "map tasks", Acore::StringFormat("{:.2f} ms wall", sim.MapTasks.Wall),
        MapTasksNote(sim.MapTasks) });

    if (!progress)
    {
        table.AddRow({ "progress", "-", "no progress.json yet (the learner writes it after connecting)" });
        table.Write(info, "  ");
        return;
    }

    std::optional<double> const steps = progress->Number("env_steps");
    std::optional<double> const total = progress->Number("total_env_steps");
    std::optional<double> const rate = StepRate(*progress);

    if (steps && total && *total > 0)
        table.AddRow({ "env steps", Format::Compact(*steps) + " / " + Format::Compact(*total),
            Format::Percent(std::min(1.0, *steps / *total)) });

    if (rate)
        table.AddRow({ "step rate", Format::Count(uint64(*rate)) + " steps/s",
            _rateEma ? Change(rate, _previous.Rate) : "average since start" });

    std::optional<double> etaSteps;
    if (steps && total && rate && *rate > 0)
    {
        etaSteps = std::max(0.0, *total - *steps) / *rate;
        table.AddRow({ "ETA (step limit)", Format::Duration(*etaSteps), ClockAfter(*etaSteps) });
    }

    std::optional<double> const patience = progress->Number("patience");
    std::optional<double> const every = progress->Number("eval_every");
    std::optional<double> const sinceBest = progress->Number("evals_since_best");
    std::optional<double> const evals = progress->Number("evals");
    if (steps && total && rate && *rate > 0 && patience && *patience > 0 && every && *every > 0)
    {
        // The overall score's plateau is when the learning rates start to anneal; every class then has to show
        // the rest of its signals over `window` evaluations, so this is the earliest the stage can end.
        double const window = progress->Number("window").value_or(1.0);
        double const needed = std::max(window, *patience - sinceBest.value_or(0.0));
        double const stopAt = progress->Number("last_eval_env_steps").value_or(0.0) + needed * *every;
        if (stopAt < *total)
        {
            double const eta = std::max(0.0, stopAt - *steps) / *rate;
            table.AddRow({ "ETA (converged, earliest)", Format::Duration(eta),
                Acore::StringFormat("at {} if every class settles over the next {} eval{}", Format::Compact(stopAt),
                    uint32(needed), needed == 1 ? "" : "s") });
        }

        if (sinceBest && *sinceBest > 0 && *patience - *sinceBest <= 1 && evals && *evals > 0)
            warnings.push_back(Acore::StringFormat("Convergence: {} of {} evaluations without a new overall best; "
                "the learning rates anneal from here", uint32(*sinceBest), uint32(*patience)));
    }

    // Which classes have converged (held out of the training draw) and which is furthest from it.
    if (std::string const converged = progress->Text("converged_layouts"); !converged.empty())
        table.AddRow({ "converged", converged, "held at hold_share of the draw, adapter and head frozen" });
    if (std::string const weakest = progress->Text("weakest_layout"); !weakest.empty())
        table.AddRow({ "weakest", weakest, Acore::StringFormat("still missing: {}",
            progress->Text("weakest_missing")) });

    std::optional<double> const score = progress->Number("last_eval_score");
    std::optional<double> const best = progress->Number("best_score");
    std::optional<double> const baselineScore = progress->Number("baseline_score");
    if (evals && *evals > 0)
    {
        std::string note = Acore::StringFormat("{} evals", uint32(*evals));
        if (best)
            note += Acore::StringFormat(", best {} at {}", Format::Metric(*best),
                Format::Compact(progress->Number("best_env_steps").value_or(0.0)));
        if (sinceBest && patience && *patience > 0)
            note += Acore::StringFormat(", {}/{} without improvement", uint32(*sinceBest), uint32(*patience));
        table.AddRow({ "eval score", Format::OrDash(score, Format::Metric), note });

        if (baselineScore && best && std::abs(*baselineScore) > 1e-9)
        {
            table.AddRow({ "best vs baseline", Format::Percent((*best - *baselineScore) / std::abs(*baselineScore),
                true), Acore::StringFormat("{} {}", progress->Text("baseline"), Format::Metric(*baselineScore)) });

            if (*evals >= BELOW_BASELINE_EVALS && *best < *baselineScore)
                warnings.push_back(Acore::StringFormat("Best evaluation score {} is still below the {} baseline's {} "
                    "after {} evaluations", Format::Metric(*best), progress->Text("baseline"),
                    Format::Metric(*baselineScore), uint32(*evals)));
        }
    }
    else if (every && *every > 0 && steps)
        table.AddRow({ "eval score", "-", Acore::StringFormat("first evaluation at {}",
            Format::Compact(std::ceil((*steps + 1) / *every) * *every)) });

    std::optional<double> const reward = progress->Number("reward_per_decision");
    if (reward)
        table.AddRow({ "reward/decision", Format::Metric(*reward), Change(reward, _previous.Reward) });

    std::optional<double> const entropy = progress->Number("entropy");
    if (entropy)
    {
        std::string note = Change(entropy, _previous.Entropy);
        if (_firstEntropy && *_firstEntropy > 0)
        {
            note += Acore::StringFormat("{}{} of start", note.empty() ? "" : ", ",
                Format::Percent(*entropy / *_firstEntropy));

            if (*entropy < ENTROPY_COLLAPSE_FRACTION * *_firstEntropy)
                warnings.push_back(Acore::StringFormat("Entropy {} is under {} of its starting {}: the policy may "
                    "have collapsed", Format::Metric(*entropy), Format::Percent(ENTROPY_COLLAPSE_FRACTION),
                    Format::Metric(*_firstEntropy)));
        }
        table.AddRow({ "entropy", Format::Metric(*entropy), note });
    }

    if (std::optional<double> const valueLoss = progress->Number("value_loss"))
        table.AddRow({ "value loss", Format::Metric(*valueLoss), "" });

    std::optional<double> const kl = progress->Number("approx_kl");
    std::optional<double> const clip = progress->Number("clip_frac");
    if (kl || clip)
        table.AddRow({ "approx KL / clip", Format::OrDash(kl, Format::Metric) + " / " + Format::OrDash(clip,
            [](double v) { return Format::Percent(v); }), "" });

    if ((kl && *kl > APPROX_KL_LIMIT) || (clip && *clip > CLIP_FRACTION_LIMIT))
        warnings.push_back(Acore::StringFormat("Updates are large (approx KL {}, clip fraction {}): consider a lower "
            "learning rate", Format::OrDash(kl, Format::Metric), Format::OrDash(clip,
                [](double v) { return Format::Percent(v); })));

    if (std::optional<double> const episodes = progress->Number("episodes"))
        table.AddRow({ "episodes/update", Format::Count(uint64(*episodes)), "" });

    for (char const* column : EPISODE_COLUMNS)
        if (std::optional<double> const value = progress->Number(std::string("episode_") + column))
            table.AddRow({ std::string("  ") + column, Format::Metric(*value), "mean of the last update's episodes" });

    std::string const nonfinite = progress->Text("nonfinite");
    if (!nonfinite.empty())
        warnings.push_back("Non-finite values from the learner: " + nonfinite);

    table.Write(info, "  ");
}

void AnimusForge::ProgressMonitor::ReportLocal(SimSnapshot const& sim, LineSink const& info) const
{
    std::string header = Acore::StringFormat("Forge: {}", sim.Scenario);
    if (sim.PlanSize > 1)
        header += Acore::StringFormat(" ({} of {})", sim.PlanPosition, sim.PlanSize);
    info(header + " | " + sim.State + " | " + Format::Duration(sim.ScenarioSeconds));

    TextTable table({ { "Metric" }, { "Value", TextTable::Align::Right }, { "Note" } });
    table.AddRow({ "sim", Acore::StringFormat("{} env steps/s", Format::Count(uint64(sim.EnvStepsPerSecond))),
        Acore::StringFormat("{} envs x {} agents, {} decisions/s, {} decisions", sim.Envs, sim.AgentsPerEnv,
            Format::Count(uint64(sim.TicksPerSecond)), Format::Count(sim.Decisions)) });
    table.AddRow({ "per decision", Acore::StringFormat("{:.1f} ms",
        sim.WorldMsPerTick + sim.SimMsPerTick + sim.LearnerMsPerTick),
        Acore::StringFormat("world {:.1f} ms (map update), sim {:.1f} ms, learner {:.1f} ms", sim.WorldMsPerTick,
            sim.SimMsPerTick, sim.LearnerMsPerTick) });
    table.AddRow({ "sim parts", Acore::StringFormat("{:.2f} ms observe", sim.Collect.Observe),
        SimPartsNote(sim.Collect) });
    table.AddRow({ "reset parts", Acore::StringFormat("{:.2f} ms create", sim.Collect.ResetCreate),
        Acore::StringFormat("place {:.2f} ms, configure {:.2f} ms, destroy {:.2f} ms, encounters {:.2f} ms, "
            "scatter {:.2f} ms, stock {:.2f} ms; prepare {:.2f} ms, seats {:.2f} ms in all, despawn {:.2f} ms; "
            "scenario reset {:.2f} ms (per decision)", sim.Collect.ResetPlace, sim.Collect.ResetConfigure,
            sim.Collect.ResetDestroy, sim.Collect.ResetEncounter, sim.Collect.ResetScatter, sim.Collect.ResetStock,
            sim.Collect.ResetPrepare, sim.Collect.ResetSeats, sim.Collect.ResetDespawn, sim.Collect.ResetScenario) });
    table.AddRow({ "world parts", Acore::StringFormat("{:.2f} ms objects", sim.World.Objects),
        Acore::StringFormat("sessions {:.2f}, players {:.2f}, scripts {:.2f}, relocation {:.2f}, visibility {:.2f}, "
            "delayed {:.2f} ms (thread time per decision, every map)", sim.World.Sessions, sim.World.Players,
            sim.World.Scripts, sim.World.Relocation, sim.World.Visibility, sim.World.Delayed) });
    if (!sim.ObserveBlocks.empty())
        table.AddRow({ "observe blocks", Acore::StringFormat("{:.2f} ms {}", sim.ObserveBlocks.front().second,
            sim.ObserveBlocks.front().first), ObserveBlocksNote(sim.ObserveBlocks) });
    if (!sim.ProbeNote.empty())
        table.AddRow({ "ground probe", "", sim.ProbeNote });
    table.AddRow({ "map tasks", Acore::StringFormat("{:.2f} ms wall", sim.MapTasks.Wall),
        MapTasksNote(sim.MapTasks) });

    std::string episodes = Format::Count(sim.Episodes);
    std::string note = Acore::StringFormat("{:.1f}/s", sim.EpisodesPerSecond);
    if (sim.EpisodeLimit)
    {
        episodes += " / " + Format::Count(sim.EpisodeLimit);
        if (sim.EpisodesPerSecond > 0 && sim.Episodes < sim.EpisodeLimit)
        {
            double const eta = double(sim.EpisodeLimit - sim.Episodes) / sim.EpisodesPerSecond;
            note += Acore::StringFormat(", done in {} {}", Format::Duration(eta), ClockAfter(eta));
        }
    }
    table.AddRow({ "episodes", episodes, note });

    for (auto const& [name, value] : sim.EpisodeMeans)
        table.AddRow({ "  " + name, Format::Metric(value),
            Acore::StringFormat("mean of the last {} episodes", Format::Count(sim.EpisodeMeansCount)) });

    table.Write(info, "  ");
}

void AnimusForge::ProgressMonitor::ReportPlan(ForgeConfig const& config, std::vector<PlanRow> const& plan,
    ProgressFile const* current, LineSink const& info) const
{
    TextTable table({ { "#", TextTable::Align::Right }, { "Scenario" }, { "Status" },
        { "Env steps", TextTable::Align::Right }, { "Best score", TextTable::Align::Right },
        { "ETA", TextTable::Align::Right } });

    std::optional<double> const rate = current ? StepRate(*current) : std::nullopt;
    double remaining = 0.0;
    bool estimated = false;

    for (std::size_t i = 0; i < plan.size(); ++i)
    {
        PlanRow const& row = plan[i];
        std::string steps = "-";
        std::string best = "-";
        std::string eta = "";

        ProgressFile file;
        ProgressFile const* progress = row.Current ? current : nullptr;
        if (!row.Current && !row.Pending && file.Load(ProgressPath(config, row.Scenario)))
            progress = &file;

        if (progress)
        {
            std::optional<double> const done = progress->Number("env_steps");
            std::optional<double> const total = progress->Number("total_env_steps");
            steps = Format::OrDash(done, Format::Compact) + " / " + Format::OrDash(total, Format::Compact);
            best = Format::OrDash(progress->Number("best_score"), Format::Metric);

            if (row.Current && done && total && rate && *rate > 0)
            {
                double const seconds = std::max(0.0, *total - *done) / *rate;
                remaining += seconds;
                eta = Format::Duration(seconds);
            }
        }
        else if (std::optional<uint64> const configured = row.Pending ? ConfiguredSteps(config, row.Scenario)
            : std::nullopt)
        {
            steps = Format::Compact(double(*configured));
            if (rate && *rate > 0)
            {
                double const seconds = double(*configured) / *rate;
                remaining += seconds;
                estimated = true;
                eta = "~" + Format::Duration(seconds);
            }
        }

        table.AddRow({ std::to_string(i + 1), row.Scenario, row.Status + (row.Resume ? " (resume)" : ""), steps, best,
            eta });
    }

    info("  Plan:");
    table.Write(info, "  ");

    if (remaining > 0)
        info(Acore::StringFormat("  Plan ETA {}{} {}{}", estimated ? "~" : "", Format::Duration(remaining),
            ClockAfter(remaining), estimated ? " (~: full step limit at the current rate; converging ends sooner)" : ""));
}
