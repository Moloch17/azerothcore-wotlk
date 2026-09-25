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

    /// Rank `rank`'s device: its own GPU counted from AnimusForge.Learner.Device ("cuda:1" and 2 ranks: cuda:1 and
    /// cuda:2; empty: cuda:0, cuda:1, ...), or the one device for every rank when that is not a GPU. The learner
    /// falls back to the first GPU for one the machine does not have, and then reduces over gloo.
    std::string RankDevice(std::string const& device, uint32 rank, uint32 ranks)
    {
        if (ranks <= 1)
            return device;
        if (!device.empty() && device.rfind("cuda", 0) != 0)
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
        std::string const device = RankDevice(config.LearnerDevice, rank, config.LearnerRanks);
        if (!device.empty())
            for (char const* key : { "train_device=", "rollout_device=" })
                set(key + device);

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

    // Off the map update's cores. Unpinned, torch's threads landed on the CPUs the map tasks run on and the map
    // update went from 3.9 to 5.4 ms per decision with the learner attached. Each rank gets whole cores of what is
    // left, so two ranks' threads do not share one. A pool that covers every core leaves them alone.
    std::vector<int> const& pool = sMapMgr->GetMapUpdater()->PoolCpus();
    std::vector<int> const away = pool.empty() ? std::vector<int>() : Acore::CpuPlacement::AwayFrom(pool);
    std::vector<std::vector<int>> const slices = Acore::CpuPlacement::Split(away, ranks);

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
        if (!away.empty())
        {
            std::vector<int> const& cpus = slices[rank];
            if (Acore::CpuPlacement::PinProcess(process.Pid(), cpus))
                LOG_INFO("module.animus", "{} (pid {}) on cpus {}, away from the map update's cores",
                    ranks > 1 ? "Learner rank " + std::to_string(rank) : std::string("Learner"), process.Pid(),
                    Acore::CpuPlacement::Describe(cpus));
            else
                LOG_WARN("module.animus", "Could not pin the learner (pid {}) to cpus {}", process.Pid(),
                    Acore::CpuPlacement::Describe(cpus));
        }

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
