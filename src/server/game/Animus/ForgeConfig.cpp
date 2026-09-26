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

#include "ForgeConfig.h"
#include "BotAccounts.h"
#include "Config.h"
#include "DBCEnums.h"
#include "Log.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    /// The learner's directory (apps/forge/python in the source tree), baked in at configure time as
    /// FORGE_PYTHON_DIR. Valid wherever the source tree exists at the path it was built from: native builds and
    /// the bind-mounted Docker services, not the runtime images, which set AnimusForge.Learner.WorkDir.
    fs::path DefaultLearnerWorkDir()
    {
#ifdef FORGE_PYTHON_DIR
        return fs::path(FORGE_PYTHON_DIR);
#else
        return fs::path("apps/forge/python");
#endif
    }

    /// The directory of the worldserver config file this server loaded: what every relative path key is relative to.
    fs::path ConfigDir()
    {
        fs::path const file = sConfigMgr->GetFilename();
        fs::path const dir = file.has_parent_path() ? file.parent_path() : fs::path(".");
        std::error_code error;
        fs::path const absolute = fs::absolute(dir, error);
        return error ? dir : absolute;
    }

    /// A path key's value: as given when absolute, else under `base`.
    fs::path Resolve(fs::path const& value, fs::path const& base)
    {
        return (value.is_relative() ? base / value : value).lexically_normal();
    }

    /// A comma-separated config list, whitespace removed, empty entries dropped.
    std::vector<std::string> GetList(std::string const& key, std::string const& fallback = "")
    {
        std::vector<std::string> entries;
        std::string const value = sConfigMgr->GetOption<std::string>(key, fallback);
        for (std::string_view name : Acore::Tokenize(value, ',', false))
        {
            std::string entry(name);
            entry.erase(std::remove_if(entry.begin(), entry.end(), [](unsigned char c) { return std::isspace(c); }),
                entry.end());

            if (!entry.empty())
                entries.push_back(std::move(entry));
        }

        return entries;
    }
}

namespace
{
    /// A comma-separated config list of positive numbers, in order, without repeats; invalid entries are logged.
    std::vector<uint32> GetNumberList(std::string const& key, std::string const& fallback)
    {
        std::vector<uint32> numbers;
        for (std::string const& entry : GetList(key, fallback))
        {
            uint32 number = 0;
            auto const [end, error] = std::from_chars(entry.data(), entry.data() + entry.size(), number);
            if (error != std::errc() || end != entry.data() + entry.size() || !number)
            {
                LOG_ERROR("module.animus", "{} = '{}' is not a positive number; skipped", key, entry);
                continue;
            }

            if (std::find(numbers.begin(), numbers.end(), number) == numbers.end())
                numbers.push_back(number);
        }

        return numbers;
    }
}

namespace
{
    struct Gpu
    {
        uint32 Index = 0;
        uint32 Units = 0;       // compute units (AMD) or streaming multiprocessors (NVIDIA)
        std::string Name;
    };

    /// The GPUs the learner's own torch sees (ROCm or CUDA: what it will run on, which a count of /sys would get
    /// wrong -- ROCm hides GPUs it does not support). Asked once per process, the first time a config loads in a
    /// mode that needs it; the cards do not change while the server runs. Empty when torch sees none or cannot be
    /// asked.
    std::vector<Gpu> const& DetectGpus(std::string const& python, std::string const& workDir)
    {
        static std::optional<std::vector<Gpu>> found;
        if (found)
            return *found;

        found.emplace();
        std::string const command = "cd '" + workDir + "' && '" + python + "' -c 'import torch; n = "
            "torch.cuda.device_count() if torch.cuda.is_available() else 0; [print(i, torch.cuda.get_device_properties"
            "(i).multi_processor_count, torch.cuda.get_device_properties(i).name, sep=chr(9)) for i in range(n)]' "
            "2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return *found;

        char line[512];
        while (std::fgets(line, sizeof(line), pipe))
        {
            std::vector<std::string_view> const fields = Acore::Tokenize(line, '\t', true);
            if (fields.size() < 3)
                continue;
            Optional<uint32> const index = Acore::StringTo<uint32>(fields[0]);
            Optional<uint32> const units = Acore::StringTo<uint32>(fields[1]);
            if (!index || !units)
                continue;
            std::string name(fields[2]);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
                name.pop_back();
            found->push_back({ *index, *units, std::move(name) });
        }
        pclose(pipe);
        return *found;
    }
}

void AnimusForge::ForgeConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("AnimusForge.Enable", true);

    Queue = GetList("AnimusForge.Queue");
    QueueSkipFinished = sConfigMgr->GetOption<bool>("AnimusForge.Queue.SkipFinished", true);
    QueueLocalEpisodes = sConfigMgr->GetOption<uint32>("AnimusForge.Queue.LocalEpisodes", 0);
    Classes = GetList("AnimusForge.Classes");

    Envs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Envs", 64));
    ContinentReplicas = sConfigMgr->GetOption<uint32>("AnimusForge.ContinentReplicas", 0);
    // AnimusForge.Stage.<name>.Envs
    StageEnvs.clear();
    for (std::string const& key : sConfigMgr->GetKeysByString("AnimusForge.Stage."))
    {
        std::string const rest = key.substr(std::string("AnimusForge.Stage.").size());
        std::size_t const dot = rest.rfind(".Envs");
        if (dot == std::string::npos || dot + 5 != rest.size())
            continue;
        StageEnvs[rest.substr(0, dot)] = sConfigMgr->GetOption<uint32>(key, Envs);
    }
    if (Envs > Animus::BotAccounts::MAX_ENVS)
    {
        LOG_ERROR("module.animus", "AnimusForge.Envs = {} is more than bot account ids allow; using {}", Envs,
            Animus::BotAccounts::MAX_ENVS);
        Envs = Animus::BotAccounts::MAX_ENVS;
    }
    DecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.DecisionMs", 250));
    TicksPerDecision = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.TicksPerDecision", 1));
    HalfBatch = sConfigMgr->GetOption<bool>("AnimusForge.HalfBatch", false);
    if (TicksPerDecision > DecisionMs)
    {
        LOG_ERROR("module.animus", "AnimusForge.TicksPerDecision = {} is more than AnimusForge.DecisionMs = {} ms, "
            "which would need a sub-millisecond tick; using {}", TicksPerDecision, DecisionMs, DecisionMs);
        TicksPerDecision = DecisionMs;
    }
    if (DecisionMs % TicksPerDecision)
    {
        // The core ticks at the truncated quotient, so a remainder would make a decision cover slightly less game
        // time than DecisionMs claims -- and DecisionMs is what every reward scale is written against.
        uint32 const rounded = DecisionMs - (DecisionMs % TicksPerDecision);
        LOG_ERROR("module.animus", "AnimusForge.DecisionMs = {} ms does not divide into {} ticks; using {} ms so a "
            "decision is exactly {} ticks of {} ms", DecisionMs, TicksPerDecision, rounded, TicksPerDecision,
            rounded / TicksPerDecision);
        DecisionMs = rounded;
    }
    if (HalfBatch && TicksPerDecision != 1)
        LOG_ERROR("module.animus", "AnimusForge.HalfBatch needs AnimusForge.TicksPerDecision = 1 (it is {}); the "
            "halves do not take turns", TicksPerDecision);
    if (HalvesTick() && DecisionMs % 2)
    {
        // Each half's maps tick every other world tick with the time of both, so a world tick has to be exactly half.
        LOG_ERROR("module.animus", "AnimusForge.HalfBatch halves AnimusForge.DecisionMs = {} ms, which is odd; using "
            "{} ms", DecisionMs, DecisionMs - 1);
        DecisionMs -= 1;
    }
    EpisodeSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.EpisodeSeconds", 60));

    Policy = sConfigMgr->GetOption<std::string>("AnimusForge.Policy", "remote");
    ReportEpisodes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.ReportEpisodes", 256));
    // Relative path keys are relative to the directory of the worldserver config file, whatever the server's working
    // directory; empty ones take the defaults below.
    fs::path const configDir = ConfigDir();

    SocketPath = Resolve(sConfigMgr->GetOption<std::string>("AnimusForge.Socket", "/tmp/animus-forge.sock"),
        configDir).string();

    LearnerAutoStart = sConfigMgr->GetOption<bool>("AnimusForge.Learner.AutoStart", true);

    fs::path workDir = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.WorkDir", "");
    workDir = workDir.empty() ? DefaultLearnerWorkDir() : Resolve(workDir, configDir);
    LearnerWorkDir = workDir.string();

    fs::path const outputDir = sConfigMgr->GetOption<std::string>("AnimusForge.OutputDir", "");
    OutputDir = (outputDir.empty() ? workDir : Resolve(outputDir, configDir)).lexically_normal().string();

    std::string const probeSource = sConfigMgr->GetOption<std::string>("AnimusForge.Probe.Source", "baked");
    ProbeBaked = probeSource != "live";
    if (ProbeBaked && probeSource != "baked")
        LOG_ERROR("server.loading", "AnimusForge.Probe.Source = \"{}\" is neither live nor baked: reading baked",
            probeSource);
    fs::path const probeDir = sConfigMgr->GetOption<std::string>("AnimusForge.Probe.Dir", "");
    // The tables ship with the forge, beside its models: baked ahead of time (`forge probestage`), never by a
    // running server.
    ProbeDir = (probeDir.empty() ? DefaultLearnerWorkDir().parent_path() / "probes" : Resolve(probeDir, configDir))
        .lexically_normal().string();
    ProbeCacheGrids = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Probe.CacheGrids", 64));

    LearnerPython = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Python", "");
    if (LearnerPython.empty())
    {
        fs::path const venvPython = workDir / ".venv" / "bin" / "python";
        std::error_code error;
        LearnerPython = fs::exists(venvPython, error) ? venvPython.string() : "python3";
    }
    else if (LearnerPython.find('/') != std::string::npos)
        LearnerPython = Resolve(LearnerPython, configDir).string();     // a bare name ("python3") is found on PATH

    // Set: relative to the config directory. Empty: configs/<scenario>.yaml in the work directory (LearnerConfigFor).
    fs::path const learnerConfig = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Config", "");
    LearnerConfig = learnerConfig.empty() ? std::string() : Resolve(learnerConfig, configDir).string();

    LearnerArgs.clear();
    std::istringstream extraArgs(sConfigMgr->GetOption<std::string>("AnimusForge.Learner.Args", ""));
    for (std::string arg; extraArgs >> arg;)
        LearnerArgs.push_back(arg);

    fs::path const logFile = sConfigMgr->GetOption<std::string>("AnimusForge.Learner.LogFile", "");
    if (logFile.empty())
    {
        // Beside the server's own logs (LogsDir is the core's key, resolved as the core resolves it).
        fs::path logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
        LearnerLogFile = (logsDir / "animus-learner.log").string();
    }
    else
        LearnerLogFile = Resolve(logFile, configDir).string();

    // Exported models stay in the forge's own folder; copying them to a game server is done by hand.
    fs::path const modelDir = sConfigMgr->GetOption<std::string>("AnimusForge.ModelDir", "");
    ModelDir = (modelDir.empty() ? DefaultLearnerWorkDir().parent_path() / "models" : Resolve(modelDir, configDir)).lexically_normal().string();

    ProgressInterval = sConfigMgr->GetOption<uint32>("AnimusForge.Progress.Interval", 0);

    FastEnvs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Fast.Envs", 16));
    FastBudget = std::max<uint64>(1000, sConfigMgr->GetOption<uint64>("AnimusForge.Fast.Budget", 20000000));
    FastQueue = GetList("AnimusForge.Fast.Queue", "");

    fs::path fastOutputDir = sConfigMgr->GetOption<std::string>("AnimusForge.Fast.OutputDir", "fast");
    if (fastOutputDir.empty())
        fastOutputDir = "fast";
    // Nested in the resolved AnimusForge.OutputDir when relative (the one path key that is not config-dir relative).
    fastOutputDir = Resolve(fastOutputDir, OutputDir);

    // Fast runs must stay out of the real runs: they archive what they replace, and `forge clean fast` deletes it all.
    fs::path const inside = fs::path(OutputDir).lexically_relative(fastOutputDir);
    if (!inside.empty() && *inside.begin() != "..")
    {
        LOG_ERROR("module.animus", "AnimusForge.Fast.OutputDir '{}' is or contains AnimusForge.OutputDir; fast runs go "
            "to {}/fast instead", fastOutputDir.string(), OutputDir);
        fastOutputDir = fs::path(OutputDir) / "fast";
    }
    FastOutputDir = fastOutputDir.string();

    // Set: relative to the config directory. Empty: configs/fast.yaml in the work directory.
    fs::path const fastOverlay = sConfigMgr->GetOption<std::string>("AnimusForge.Fast.Learner.Overlay", "");
    FastLearnerOverlay = (fastOverlay.empty() ? workDir / "configs" / "fast.yaml" : Resolve(fastOverlay, configDir))
        .lexically_normal().string();

    FastLearnerArgs.clear();
    std::istringstream fastArgs(sConfigMgr->GetOption<std::string>("AnimusForge.Fast.Learner.Args", ""));
    for (std::string arg; fastArgs >> arg;)
        FastLearnerArgs.push_back(arg);

    LearnerTorchThreads = sConfigMgr->GetOption<uint32>("AnimusForge.Learner.TorchThreads", 0);
    // "auto" and "" alike leave the choice to the GPU mode (TrainDevice / RolloutDevice) or the map update (Cpus).
    auto const placement = [](std::string const& key)
    {
        std::string value = sConfigMgr->GetOption<std::string>(key, "auto");
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }),
            value.end());
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return lower == "auto" ? std::string() : value;
    };
    LearnerTrainDevice = placement("AnimusForge.Learner.TrainDevice");
    LearnerRolloutDevice = placement("AnimusForge.Learner.RolloutDevice");
    LearnerCpus = placement("AnimusForge.Learner.Cpus");

    std::string role = sConfigMgr->GetOption<std::string>("AnimusForge.Cluster.Role", "standalone");
    std::transform(role.begin(), role.end(), role.begin(), [](unsigned char c) { return std::tolower(c); });
    Cluster = role == "host" ? ClusterRole::Host : role == "worker" ? ClusterRole::Worker : ClusterRole::Standalone;
    if (role != "host" && role != "worker" && role != "standalone")
        LOG_ERROR("module.animus", "AnimusForge.Cluster.Role = '{}' is not standalone, host or worker; standalone",
            role);
    ClusterHost = sConfigMgr->GetOption<std::string>("AnimusForge.Cluster.Host", "");
    ClusterControlPort = uint16(sConfigMgr->GetOption<uint32>("AnimusForge.Cluster.ControlPort", 7700));
    ClusterDataPort = uint16(sConfigMgr->GetOption<uint32>("AnimusForge.Cluster.DataPort", 7701));
    ClusterAdvertise = sConfigMgr->GetOption<std::string>("AnimusForge.Cluster.Advertise", "");
    if (Cluster == ClusterRole::Worker && ClusterHost.empty())
    {
        LOG_ERROR("module.animus", "AnimusForge.Cluster.Role = worker needs AnimusForge.Cluster.Host; standalone");
        Cluster = ClusterRole::Standalone;
    }

    Bench = BenchSettings();
    Bench.Scenario = sConfigMgr->GetOption<std::string>("AnimusForge.Bench.Scenario", "stage8_duel");
    Bench.Policy = sConfigMgr->GetOption<std::string>("AnimusForge.Bench.Policy", "fight");
    Bench.Threads = GetNumberList("AnimusForge.Bench.Threads", "4, 8, 12, 16");
    Bench.Envs = GetNumberList("AnimusForge.Bench.Envs", "64, 128, 192");
    Bench.MaxEnvs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Bench.MaxEnvs", 256));
    Bench.WarmupTicks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Bench.WarmupTicks", 128));
    Bench.MeasureTicks = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AnimusForge.Bench.MeasureTicks", 384));
    Bench.LearnerWarmupTicks = std::max<uint32>(1,
        sConfigMgr->GetOption<uint32>("AnimusForge.Bench.LearnerWarmupTicks", 384));
    Bench.LearnerMeasureTicks = std::max<uint32>(1,
        sConfigMgr->GetOption<uint32>("AnimusForge.Bench.LearnerMeasureTicks", 768));
    Bench.MaxMemoryPercent = std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("AnimusForge.Bench.MaxMemoryPercent", 80), 1, 100);
    Bench.LearnerTop = sConfigMgr->GetOption<uint32>("AnimusForge.Bench.LearnerTop", 2);
    // 0 means torch's own default, so this list keeps zeroes: it is read as "the default" rather than "no threads".
    Bench.LearnerTorchThreads.clear();
    for (std::string const& entry : GetList("AnimusForge.Bench.LearnerTorchThreads", "0, 8"))
    {
        uint32 number = 0;
        auto const [end, error] = std::from_chars(entry.data(), entry.data() + entry.size(), number);
        if (error == std::errc() && end == entry.data() + entry.size()
            && std::find(Bench.LearnerTorchThreads.begin(), Bench.LearnerTorchThreads.end(), number)
                == Bench.LearnerTorchThreads.end())
            Bench.LearnerTorchThreads.push_back(number);
    }
    if (Bench.LearnerTorchThreads.empty())
        Bench.LearnerTorchThreads.push_back(0);

    Bench.OutputDir = (fs::path(OutputDir) / "bench").lexically_normal().string();

    if (Bench.Threads.empty())
        Bench.Threads.push_back(std::max<uint32>(1, sConfigMgr->GetOption<uint32>("MapUpdate.Threads", 1)));
    if (Bench.Envs.empty())
        Bench.Envs.push_back(Envs);

    SpawnMapId = sConfigMgr->GetOption<uint32>("AnimusForge.SpawnPoint.MapId", 560);
    SpawnPosition.Relocate(
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.X", 2741.9f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.Y", 1315.2f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.Z", 14.0f),
        sConfigMgr->GetOption<float>("AnimusForge.SpawnPoint.O", 2.96f));

    // Last: the mode needs the policy, the cluster role and the learner's paths, and adjusts Envs and LearnerArgs.
    ApplyGpuMode();
}

void AnimusForge::ForgeConfig::ApplyGpuMode()
{
    std::string mode = sConfigMgr->GetOption<std::string>("AnimusForge.Gpu.Mode", "auto");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::tolower(c); });
    GpuModeSetting = mode == "single" ? GpuMode::Single : mode == "multi" ? GpuMode::Multi : GpuMode::Auto;
    if (mode != "auto" && mode != "single" && mode != "multi")
        LOG_ERROR("module.animus", "AnimusForge.Gpu.Mode = '{}' is not auto, single or multi; auto", mode);

    uint32 const multiLearners = std::min<uint32>(sConfigMgr->GetOption<uint32>("AnimusForge.Gpu.Multi.Learners", 0),
        16);

    // Only a learner of this sim's own needs the GPUs counted: a cluster worker runs none, and a local policy none.
    bool const learnerHere = IsRemote() && Cluster != ClusterRole::Worker;
    Gpus.clear();
    std::string found = "not counted";
    // Counted in every mode: besides auto's choice, it puts the learners on the largest GPUs whatever torch numbers
    // them (an integrated GPU can come first).
    if (learnerHere)
    {
        std::vector<Gpu> const& gpus = DetectGpus(LearnerPython, LearnerWorkDir);
        uint32 largest = 0;
        for (Gpu const& gpu : gpus)
            largest = std::max(largest, gpu.Units);

        // An integrated GPU beside a discrete card would be the slowest rank, and every rank waits for the slowest.
        std::vector<std::string> names;
        for (Gpu const& gpu : gpus)
        {
            if (2 * gpu.Units < largest)
                continue;
            Gpus.push_back(gpu.Index);
            names.push_back(Acore::StringFormat("cuda:{} {}", gpu.Index, gpu.Name));
        }

        found = Gpus.empty() ? "no GPU found" : Acore::StringFormat("{} GPU{} found: ", Gpus.size(),
            Gpus.size() == 1 ? "" : "s");
        for (std::size_t i = 0; i < names.size(); ++i)
            found += (i ? ", " : "") + names[i];
        if (gpus.size() > Gpus.size())
            found += Acore::StringFormat(" ({} smaller left out)", gpus.size() - Gpus.size());
    }

    MultiGpu = learnerHere && (GpuModeSetting == GpuMode::Multi
        || (GpuModeSetting == GpuMode::Auto && Gpus.size() > 1));
    LearnerRanks = MultiGpu ? std::clamp<uint32>(multiLearners ? multiLearners : uint32(Gpus.size()), 1, 16) : 1;

    // The mode's own values. Envs: Single.Envs, or Multi.Envs, or by default AnimusForge.Envs (and each stage's own
    // envs) for every learner, so each GPU gets the batch one GPU would.
    std::string const prefix = MultiGpu ? "AnimusForge.Gpu.Multi." : "AnimusForge.Gpu.Single.";
    uint32 const envs = sConfigMgr->GetOption<uint32>(prefix + "Envs", 0);
    if (envs)
        Envs = envs;
    else if (MultiGpu)
    {
        Envs *= LearnerRanks;
        for (auto& [stage, stageEnvs] : StageEnvs)
            stageEnvs *= LearnerRanks;
    }
    if (Envs > Animus::BotAccounts::MAX_ENVS)
    {
        LOG_ERROR("module.animus", "{} envs for the GPU mode is more than bot account ids allow; using {}", Envs,
            Animus::BotAccounts::MAX_ENVS);
        Envs = Animus::BotAccounts::MAX_ENVS;
    }

    // Minibatches, then the mode's own learner args: after AnimusForge.Learner.Args, so the mode's --set wins.
    std::vector<std::string> args;
    if (uint32 const minibatches = sConfigMgr->GetOption<uint32>(prefix + "Minibatches", 0))
        args = { "--set", Acore::StringFormat("mappo.minibatches={}", minibatches) };
    args.insert(args.begin(), LearnerArgs.begin(), LearnerArgs.end());
    std::istringstream modeArgs(sConfigMgr->GetOption<std::string>(prefix + "LearnerArgs", ""));
    for (std::string arg; modeArgs >> arg;)
        args.push_back(arg);
    LearnerArgs = std::move(args);

    char const* const setting = GpuModeSetting == GpuMode::Auto ? "auto" : GpuModeSetting == GpuMode::Single
        ? "single" : "multi";
    GpuSummary = !learnerHere ? Acore::StringFormat("{} (no learner on this sim)", setting)
        : Acore::StringFormat("{}{}: {} learner{}, {} envs ({})", MultiGpu ? "multi" : "single",
            GpuModeSetting == GpuMode::Auto ? " (auto)" : "", LearnerRanks, LearnerRanks == 1 ? "" : "s", Envs, found);
}

Animus::StageSettings AnimusForge::ForgeConfig::Stage(std::string const& scenario) const
{
    Animus::StageSettings stage;
    stage.Envs = Envs;
    if (auto const own = StageEnvs.find(scenario); own != StageEnvs.end())
        stage.Envs = std::min(std::max<uint32>(1, own->second), Animus::BotAccounts::MAX_ENVS);
    stage.DecisionMs = DecisionMs;
    stage.EpisodeSeconds = EpisodeSeconds;
    stage.ReportEpisodes = ReportEpisodes;
    stage.Classes = Classes;
    stage.SpawnMapId = SpawnMapId;
    stage.SpawnPosition = SpawnPosition;
    stage.Level = Level;
    stage.ContinentReplicas = ContinentReplicas;
    stage.TuningPrefix = "AnimusForge.Curriculum.";
    stage.LayoutsDir = LayoutsDir().string();
    return stage;
}

fs::path AnimusForge::ForgeConfig::RunsDir() const
{
    return fs::path(OutputDir) / "runs";
}

fs::path AnimusForge::ForgeConfig::LayoutsDir() const
{
    return fs::path(OutputDir) / "layouts";
}

AnimusForge::ForgeConfig AnimusForge::ForgeConfig::BenchProfile(uint32 envs, bool remote, uint32 torchThreads) const
{
    ForgeConfig bench = *this;
    bench.Policy = remote ? "remote" : Bench.Policy;
    bench.Envs = std::min(std::max<uint32>(1, envs), Animus::BotAccounts::MAX_ENVS);
    bench.LearnerTorchThreads = torchThreads;
    bench.OutputDir = Bench.OutputDir;
    bench.ModelDir = (fs::path(Bench.OutputDir) / "models").string();

    // A timed run only has to train: no evaluation, no seeding from other runs, no distillation, and a budget it
    // never reaches. AnimusForge.Learner.Args still win over these (--set is applied in order).
    //
    // The evaluation interval is pushed out of reach rather than set to 0: a stage whose target has gates needs an
    // interval to check them at, and the learner refuses the pair at startup. With eval.at_start off and a budget
    // nothing reaches, an interval this large is the same as none.
    bench.LearnerArgs = { "--set", "eval.every_env_steps=1000000000000", "--set", "eval.at_start=false",
        "--set", "init_from=[]",
        "--set", "merge_from=[]", "--set", "distill.teachers=\"\"", "--set", "total_env_steps=1000000000000",
        "--set", "checkpoint_every=1000000", "--set", "convergence.patience=0" };
    bench.LearnerArgs.insert(bench.LearnerArgs.end(), LearnerArgs.begin(), LearnerArgs.end());
    return bench;
}

AnimusForge::ForgeConfig AnimusForge::ForgeConfig::FastProfile(uint64 budget) const
{
    ForgeConfig fast = *this;
    fast.Policy = "remote";
    fast.Envs = FastEnvs;
    fast.ReportEpisodes = std::min<uint32>(ReportEpisodes, 64);
    // The budget this profile was built for, so whatever reports the profile reports the budget in force and
    // not the configured default -- `forge fast 30M` saying "20,000,000 steps a stage" is a message that lies.
    fast.FastBudget = budget;

    // Level and Classes are deliberately NOT narrowed. A fast run used to train four classes at level 20,
    // which made it a rehearsal of a problem the real build never trains: the classes it skipped were the ones
    // whose faults a sweep is for finding. Only the budget and the env count are smaller now.
    fast.OutputDir = FastOutputDir;
    fast.ModelDir = (fs::path(FastOutputDir) / "models").string();

    // The overlay goes first, then the budget, then the operator's own --set: AnimusForge.Learner.Args and
    // AnimusForge.Fast.Learner.Args still win over both, because --set is applied in order.
    //
    // convergence.patience 0 is what makes the budget a budget: ConvergenceTracker.converged returns false at
    // once when patience is 0, so a stage trains every step it was given instead of stopping as soon as its
    // score flattens. A fast run is then a fixed, predictable sweep rather than a race of uneven lengths.
    fast.LearnerArgs = { "--overlay", FastLearnerOverlay,
        "--set", Acore::StringFormat("total_env_steps={}", budget),
        "--set", "convergence.patience=0" };
    fast.LearnerArgs.insert(fast.LearnerArgs.end(), LearnerArgs.begin(), LearnerArgs.end());
    fast.LearnerArgs.insert(fast.LearnerArgs.end(), FastLearnerArgs.begin(), FastLearnerArgs.end());
    return fast;
}

std::string AnimusForge::ForgeConfig::LearnerConfigFor(std::string const& scenario) const
{
    if (!LearnerConfig.empty())
    {
        fs::path const named(LearnerConfig);
        return (named.is_relative() ? fs::path(LearnerWorkDir) / named : named).string();
    }

    // A run of one class gets that class's own gates where it has written them: configs/<class>/<scenario>.yaml,
    // falling back to the shared configs/<scenario>.yaml. A curriculum trained per class needs per-class floors --
    // a resto druid and a bear are not held to the same clean-kill rate -- and an overlay could not do it, because
    // an overlay merges section by section and would give every stage in the queue the same `target`, when stage
    // one gates `arrived` and stage five gates `clean_kill`.
    fs::path const shared = fs::path("configs") / (scenario + ".yaml");
    fs::path config = shared;
    if (Classes.size() == 1)
    {
        fs::path const perClass = fs::path("configs") / Classes.front() / (scenario + ".yaml");
        std::error_code ec;
        if (fs::exists(fs::path(LearnerWorkDir) / perClass, ec))
            config = perClass;
    }

    return (fs::path(LearnerWorkDir) / config).string();
}
