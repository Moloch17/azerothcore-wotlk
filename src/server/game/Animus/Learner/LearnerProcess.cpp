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
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    std::vector<std::string> LearnerArgs(AnimusForge::ForgeConfig const& config, std::string const& scenario,
        bool resume)
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

        // The learner's torch shares the machine with the map update threads; 0 leaves torch's own default.
        if (config.LearnerTorchThreads)
        {
            args.emplace_back("--set");
            args.emplace_back("torch_threads=" + std::to_string(config.LearnerTorchThreads));
        }

        // A cluster host's learner trains on its workers' sims as well as this one (the learner's ClusterEnv).
        if (!config.ClusterSims.empty())
        {
            std::string sims;
            for (std::string const& sim : config.ClusterSims)
                sims += (sims.empty() ? "" : ", ") + ("'" + sim + "'");
            args.emplace_back("--set");
            args.emplace_back("cluster_sims=[" + sims + "]");
        }

        // Before AnimusForge.Learner.Args, whose --set comes later and wins.
        if (!config.LearnerDevice.empty())
        {
            for (char const* key : { "train_device=", "rollout_device=" })
            {
                args.emplace_back("--set");
                args.emplace_back(key + config.LearnerDevice);
            }
        }

        args.insert(args.end(), config.LearnerArgs.begin(), config.LearnerArgs.end());
        return args;
    }
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

    if (!ChildProcess::Start(LearnerArgs(config, scenario, resume), workDir.string(), config.LearnerLogFile))
        return false;

    // Off the map update's cores. Unpinned, torch's threads landed on the CPUs the map tasks run on and the map
    // update went from 3.9 to 5.4 ms per decision with the learner attached. Set before the interpreter has started
    // any thread of its own, so every thread it starts inherits it. A pool that covers every core leaves it alone.
    std::vector<int> const away = Acore::CpuPlacement::AwayFrom(sMapMgr->GetMapUpdater()->PoolCpus());
    if (!sMapMgr->GetMapUpdater()->PoolCpus().empty() && !away.empty())
    {
        if (Acore::CpuPlacement::PinProcess(Pid(), away))
            LOG_INFO("module.animus", "Learner (pid {}) on cpus {}, away from the map update's cores", Pid(),
                Acore::CpuPlacement::Describe(away));
        else
            LOG_WARN("module.animus", "Could not pin the learner (pid {}) to cpus {}", Pid(),
                Acore::CpuPlacement::Describe(away));
    }

    LOG_DEBUG("module.animus", "Started learner (pid {}) for {}{}: config {}; output in {}", Pid(), scenario,
        resume ? ", resuming latest.pt" : "", configPath.string(), config.LearnerLogFile);
    return true;
}

std::string AnimusForge::LearnerProcess::ManualCommand(ForgeConfig const& config, std::string const& scenario,
    bool resume)
{
    std::string command = "cd " + config.LearnerWorkDir + " &&";
    for (std::string const& arg : LearnerArgs(config, scenario, resume))
        command += " " + arg;

    return command;
}
