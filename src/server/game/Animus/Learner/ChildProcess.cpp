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

#include "ChildProcess.h"
#include "Log.h"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;

namespace
{
    /// Poll interval while waiting for the child to exit.
    constexpr std::chrono::milliseconds EXIT_POLL_INTERVAL{ 50 };
}

AnimusForge::ChildProcess::~ChildProcess()
{
    Stop(std::chrono::seconds(10));
}

bool AnimusForge::ChildProcess::Start(std::vector<std::string> args, std::string const& workDir,
    std::string const& logFile)
{
    if (IsRunning())
    {
        LOG_ERROR("module.animus", "{} is already running (pid {})", _name, _pid);
        return false;
    }

    if (args.empty())
        return false;

    std::error_code error;
    std::filesystem::path const logPath = logFile;
    if (logPath.has_parent_path())
        std::filesystem::create_directories(logPath.parent_path(), error);

    std::vector<char*> argv;
    for (std::string& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Run in the work directory, append both output streams to the log file, and do not leak the server's
    // descriptors (database connections, log files, the learner socket) into the child. The console is the server's
    // alone: the child reads no stdin and runs in its own process group, so Ctrl+C in the terminal reaches only the
    // server, which then stops the child itself.
    posix_spawn_file_actions_addchdir_np(&actions, workDir.c_str());
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    int const spawnError = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);

    if (spawnError)
    {
        LOG_ERROR("module.animus", "Could not start {} '{}': {}", _name, args[0], std::strerror(spawnError));
        return false;
    }

    _pid = pid;
    _logFile = logFile;
    _exited = false;
    _exitedCleanly = false;
    _exitCode = -1;
    _stopping = false;
    _startedAt = std::chrono::steady_clock::now();
    return true;
}

void AnimusForge::ChildProcess::Poll()
{
    if (_pid <= 0)
        return;

    int status = 0;
    if (::waitpid(_pid, &status, WNOHANG) == _pid)
        ReportExit(status);
}

double AnimusForge::ChildProcess::RunningSeconds() const
{
    if (_pid <= 0)
        return 0.0;

    return std::chrono::duration<double>(std::chrono::steady_clock::now() - _startedAt).count();
}

void AnimusForge::ChildProcess::Stop(std::chrono::milliseconds grace)
{
    if (_pid <= 0)
        return;

    if (WaitForExit(grace))
        return;

    LOG_WARN("module.animus", "{} (pid {}) still running; interrupting it", _name, _pid);
    ::kill(_pid, SIGINT);
    if (WaitForExit(grace))
        return;

    LOG_WARN("module.animus", "{} (pid {}) ignored SIGINT; killing it", _name, _pid);
    ::kill(_pid, SIGKILL);
    WaitForExit(grace);
}

bool AnimusForge::ChildProcess::WaitForExit(std::chrono::milliseconds timeout)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    while (_pid > 0)
    {
        Poll();
        if (_pid <= 0 || std::chrono::steady_clock::now() >= deadline)
            break;

        std::this_thread::sleep_for(EXIT_POLL_INTERVAL);
    }

    return _pid <= 0;
}

void AnimusForge::ChildProcess::ReportExit(int status)
{
    _exited = true;
    _exitedCleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    _exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (_exitedCleanly)
        LOG_DEBUG("module.animus", "{} (pid {}) finished; output in {}", _name, _pid, _logFile);
    else if (_stopping)
        LOG_DEBUG("module.animus", "{} (pid {}) stopped; output in {}", _name, _pid, _logFile);
    else if (WIFEXITED(status))
        LOG_ERROR("module.animus", "{} (pid {}) exited with code {}; see {}", _name, _pid, WEXITSTATUS(status),
            _logFile);
    else if (WIFSIGNALED(status))
        LOG_ERROR("module.animus", "{} (pid {}) killed by signal {}; see {}", _name, _pid, WTERMSIG(status), _logFile);

    _pid = -1;
}
