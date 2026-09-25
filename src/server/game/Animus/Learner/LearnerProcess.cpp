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

#include "LearnerProcess.h"
#include "CpuPlacement.h"
#include "ForgeConfig.h"
#include "Log.h"
#include "MapMgr.h"
#include "StringConvert.h"
#include <algorithm>
#include <arpa/inet.h>
#include <filesystem>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace
{
    /// Where rank 0 meets the others (torch.distributed's rendezvous): a port free on loopback now. Another program
    /// could take it before rank 0 binds it; the run then fails to start and says so, rather than joining another's.
    uint16 FreeLoopbackPort()
    {
        int const probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe < 0)
            return 29500;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        uint16 port = 29500;
        if (bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0
            && getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
        close(probe);
        return port;
    }

    /// Rank `rank`'s device for one of the learner's two jobs, `device` being AnimusForge.Learner.TrainDevice or
    /// RolloutDevice. Set: that device, or with several ranks each its own GPU counted from there ("cuda:1" and 2
    /// ranks: cuda:1 and cuda:2; a device that is not a GPU is every rank's). Auto (empty): the rank-th of the GPUs
    /// the GPU mode counted (the largest ones, whatever torch numbers them), or with none counted cuda:<rank> for
    /// several ranks and the stage config's own device for one. The learner falls back to the first GPU for one the
    /// machine does not have, and then reduces over gloo.
    std::string RankDevice(AnimusForge::ForgeConfig const& config, std::string const& device, uint32 rank)
    {
        if (device.empty())
        {
            if (!config.Gpus.empty())
                return "cuda:" + std::to_string(config.Gpus[rank % config.Gpus.size()]);
            return config.LearnerRanks > 1 ? "cuda:" + std::to_string(rank) : std::string();
        }
        if (config.LearnerRanks <= 1 || device.rfind("cuda", 0) != 0)
            return device;

        uint32 first = 0;
        if (std::size_t const colon = device.find(':'); colon != std::string::npos)
            first = Acore::StringTo<uint32>(device.substr(colon + 1)).value_or(0);
        return "cuda:" + std::to_string(first + rank);
    }

    std::vector<std::string> LearnerArgs(AnimusForge::ForgeConfig const& config, std::string const& scenario,
        bool resume, uint32 rank, uint16 port)
    {
        // The run is named after the scenario, so a shared config (AnimusForge.Learner.Config) still gives every
        // scenario its own runs/<scenario>/. The sim decides where runs and layouts go (AnimusForge.OutputDir).
        std::vector<std::string> args =
        {
            config.LearnerPython, "-u", "-m", "animus.train",
            "--config", config.LearnerConfigFor(scenario),
            "--socket", config.SocketPath,
            "--run-name", scenario,
            "--runs-dir", config.RunsDir().string(),
            "--layouts-dir", config.LayoutsDir().string(),
        };

        if (resume)
            args.emplace_back("--resume");

        auto const set = [&args](std::string const& value)
        {
            args.emplace_back("--set");
            args.emplace_back(value);
        };

        // The learner's torch shares the machine with the map update threads; 0 leaves torch's own default.
        if (config.LearnerTorchThreads)
            set("torch_threads=" + std::to_string(config.LearnerTorchThreads));

        // A cluster host's learner trains on its workers' sims as well as this one (the learner's ClusterEnv).
        if (!config.ClusterSims.empty())
        {
            std::string sims;
            for (std::string const& sim : config.ClusterSims)
                sims += (sims.empty() ? "" : ", ") + ("'" + sim + "'");
            set("cluster_sims=[" + sims + "]");
        }

        // Data-parallel learners: which rank this is, and where the ranks meet.
        if (config.LearnerRanks > 1)
        {
            set("rank=" + std::to_string(rank));
            set("ranks=" + std::to_string(config.LearnerRanks));
            set("dist_address=127.0.0.1:" + std::to_string(port));
        }

        // Before AnimusForge.Learner.Args, whose --set comes later and wins.
        if (std::string const train = RankDevice(config, config.LearnerTrainDevice, rank); !train.empty())
            set("train_device=" + train);
        if (std::string const rollout = RankDevice(config, config.LearnerRolloutDevice, rank); !rollout.empty())
            set("rollout_device=" + rollout);

        args.insert(args.end(), config.LearnerArgs.begin(), config.LearnerArgs.end());
        return args;
    }
}

AnimusForge::LearnerProcess::LearnerProcess()
{
    _ranks.push_back(std::make_unique<ChildProcess>("Learner"));
}

std::string AnimusForge::LearnerProcess::RankLogFile(std::string const& logFile, uint32 rank)
{
    if (!rank)
        return logFile;

    std::filesystem::path path = logFile;
    std::string const extension = path.extension().string();
    path.replace_extension();
    return path.string() + ".rank" + std::to_string(rank) + extension;
}

bool AnimusForge::LearnerProcess::Start(ForgeConfig const& config, std::string const& scenario, bool resume)
{
    namespace fs = std::filesystem;

    // Non-throwing checks: this also runs from console commands (`forge resume`).
    std::error_code error;
    fs::path const workDir = config.LearnerWorkDir;
    if (!fs::is_directory(workDir, error) || !fs::exists(workDir / "animus" / "train.py", error))
    {
        LOG_ERROR("module.animus", "Learner directory '{}' does not contain animus/train.py. Set "
            "AnimusForge.Learner.WorkDir to apps/forge/python, or AnimusForge.Learner.AutoStart = 0 "
            "to start the learner yourself.", workDir.string());
        return false;
    }

    fs::path const configPath = config.LearnerConfigFor(scenario);
    if (!fs::exists(configPath, error))
    {
        LOG_ERROR("module.animus", "Learner config '{}' does not exist (scenario {}). Create it or set "
            "AnimusForge.Learner.Config.", configPath.string(), scenario);
        return false;
    }

    uint32 const ranks = std::max<uint32>(1, config.LearnerRanks);
    while (_ranks.size() < ranks)
        _ranks.push_back(std::make_unique<ChildProcess>("Learner rank " + std::to_string(_ranks.size())));
    _started = ranks;

    // Off the map update's cores (AnimusForge.Learner.Cpus = auto). Unpinned, torch's threads landed on the CPUs the
    // map tasks run on and the map update went from 3.9 to 5.4 ms per decision with the learner attached. Each rank
    // gets whole cores of what is left, so two ranks' threads do not share one. A pool that covers every core leaves
    // them alone. Named CPUs are shared out the same way, whatever the map update uses.
    std::string cpuError;
    std::vector<int> cpus = Acore::CpuPlacement::Parse(config.LearnerCpus, cpuError);
    if (!cpuError.empty())
        LOG_ERROR("module.animus", "AnimusForge.Learner.Cpus: {}{}", cpuError,
            cpus.empty() ? "; placing it itself" : "");
    bool const named = !cpus.empty();
    if (!named)
    {
        std::vector<int> const& pool = sMapMgr->GetMapUpdater()->PoolCpus();
        if (!pool.empty())
            cpus = Acore::CpuPlacement::AwayFrom(pool);
    }
    std::vector<std::vector<int>> const slices = Acore::CpuPlacement::Split(cpus, ranks);

    uint16 const port = ranks > 1 ? FreeLoopbackPort() : 0;
    for (uint32 rank = 0; rank < ranks; ++rank)
    {
        ChildProcess& process = *_ranks[rank];
        std::string const logFile = RankLogFile(config.LearnerLogFile, rank);
        if (!process.Start(LearnerArgs(config, scenario, resume, rank, port), workDir.string(), logFile))
        {
            // A data-parallel run is all of its ranks or none: the ones started would wait for this one for ever.
            for (uint32 started = 0; started < rank; ++started)
            {
                _ranks[started]->ExpectExit();
                _ranks[started]->Stop(std::chrono::seconds(2));
            }
            return false;
        }

        // Set before the interpreter has started any thread of its own, so every thread it starts inherits it.
        std::string const train = RankDevice(config, config.LearnerTrainDevice, rank);
        std::string const rollout = RankDevice(config, config.LearnerRolloutDevice, rank);
        std::string const devices = Acore::StringFormat("update on {}, rollouts on {}", train.empty()
            ? "the stage config's device" : train, rollout.empty() ? "the stage config's device" : rollout);
        std::string const name = ranks > 1 ? "Learner rank " + std::to_string(rank) : std::string("Learner");
        if (cpus.empty())
            LOG_INFO("module.animus", "{} (pid {}): {}", name, process.Pid(), devices);
        else if (Acore::CpuPlacement::PinProcess(process.Pid(), slices[rank]))
            LOG_INFO("module.animus", "{} (pid {}): {}; on cpus {}{}", name, process.Pid(), devices,
                Acore::CpuPlacement::Describe(slices[rank]), named ? "" : ", away from the map update's cores");
        else
            LOG_WARN("module.animus", "Could not pin the learner (pid {}) to cpus {}", process.Pid(),
                Acore::CpuPlacement::Describe(slices[rank]));

        LOG_DEBUG("module.animus", "Started learner rank {} of {} (pid {}) for {}{}: config {}; output in {}", rank,
            ranks, process.Pid(), scenario, resume ? ", resuming latest.pt" : "", configPath.string(), logFile);
    }

    if (ranks > 1)
        LOG_INFO("module.animus", "Learner: {} data-parallel ranks, meeting on 127.0.0.1:{}; rank k logs to {}",
            ranks, port, RankLogFile(config.LearnerLogFile, 1));
    return true;
}

std::string AnimusForge::LearnerProcess::ManualCommand(ForgeConfig const& config, std::string const& scenario,
    bool resume)
{
    // Several ranks: every one in the background, then wait for them all (in a subshell, so each runs in the dir).
    uint32 const ranks = std::max<uint32>(1, config.LearnerRanks);
    std::string command = "cd " + config.LearnerWorkDir + " &&" + (ranks > 1 ? " (" : "");
    for (uint32 rank = 0; rank < ranks; ++rank)
    {
        for (std::string const& arg : LearnerArgs(config, scenario, resume, rank, 29500))
            command += " " + arg;
        if (ranks > 1)
            command += " &";
    }

    if (ranks > 1)
        command += " wait)";
    return command;
}

void AnimusForge::LearnerProcess::Poll()
{
    for (std::unique_ptr<ChildProcess> const& process : _ranks)
        process->Poll();

    // A data-parallel run is all of its ranks or none: one that died leaves the others blocked in a collective for
    // it for ever, holding the sim on their envs. Stopping them too lets the sim see the learner gone, and `forge
    // resume` start the run again from its last checkpoint.
    if (_started > 1 && FailedUnexpectedly() && IsRunning())
    {
        LOG_WARN("module.animus", "A learner rank failed; stopping the others (the run resumes from latest.pt)");
        ExpectExit();
        Stop(std::chrono::seconds(2));
    }
}

bool AnimusForge::LearnerProcess::IsRunning() const
{
    return std::any_of(_ranks.begin(), _ranks.end(), [](auto const& process) { return process->IsRunning(); });
}

bool AnimusForge::LearnerProcess::FinishedCleanly() const
{
    return std::all_of(_ranks.begin(), _ranks.begin() + _started,
        [](auto const& process) { return process->FinishedCleanly(); });
}

bool AnimusForge::LearnerProcess::FailedUnexpectedly() const
{
    return std::any_of(_ranks.begin(), _ranks.begin() + _started,
        [](auto const& process) { return process->FailedUnexpectedly(); });
}

void AnimusForge::LearnerProcess::ExpectExit()
{
    for (std::unique_ptr<ChildProcess> const& process : _ranks)
        process->ExpectExit();
}

void AnimusForge::LearnerProcess::Stop(std::chrono::milliseconds grace)
{
    for (std::unique_ptr<ChildProcess> const& process : _ranks)
        process->Stop(grace);
}
