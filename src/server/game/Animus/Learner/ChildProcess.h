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

#ifndef MOD_ANIMUS_FORGE_CHILD_PROCESS_H
#define MOD_ANIMUS_FORGE_CHILD_PROCESS_H

#include <chrono>
#include <string>
#include <sys/types.h>
#include <vector>

namespace AnimusForge
{
    /// A helper program (the Python learner, a model export) run as a child of the worldserver.
    ///
    /// Output goes to a log file; nothing blocks while it runs. Poll() reaps it and logs how it ended once.
    class ChildProcess
    {
    public:
        /// `name` labels the log lines ("Learner", "Export").
        explicit ChildProcess(std::string name) : _name(std::move(name)) { }
        ~ChildProcess();

        ChildProcess(ChildProcess const&) = delete;
        ChildProcess& operator=(ChildProcess const&) = delete;

        /// Spawn `args` (args[0] is looked up on PATH) in `workDir`, appending stdout and stderr to `logFile`.
        /// Returns false, with the reason logged, if it cannot be started or one is already running.
        bool Start(std::vector<std::string> args, std::string const& workDir, std::string const& logFile);

        /// Reap the child if it has exited, logging how it ended once. Cheap; safe to call often.
        void Poll();

        [[nodiscard]] bool IsRunning() const { return _pid > 0; }
        [[nodiscard]] pid_t Pid() const { return _pid; }
        [[nodiscard]] std::string const& LogFile() const { return _logFile; }

        /// The last started child has exited with status 0.
        [[nodiscard]] bool FinishedCleanly() const { return _pid <= 0 && _exited && _exitedCleanly; }

        /// The exit code of the last started child, once it has exited normally; -1 otherwise.
        [[nodiscard]] int ExitCode() const { return _pid <= 0 && _exited ? _exitCode : -1; }

        /// The last started child has exited some other way (and was not stopped on purpose).
        [[nodiscard]] bool FailedUnexpectedly() const { return _pid <= 0 && _exited && !_exitedCleanly && !_stopping; }

        /// Seconds since the running child was started (0 when none is running).
        [[nodiscard]] double RunningSeconds() const;

        /// Mark the coming exit as expected (a cancel): it is logged as stopped rather than as a failure.
        void ExpectExit() { _stopping = true; }

        /// Wait up to `grace` for a voluntary exit, then SIGINT (Python saves on KeyboardInterrupt), then SIGKILL.
        /// `grace` applies to each of the three phases, so a child that ignores everything blocks for 3 x grace.
        void Stop(std::chrono::milliseconds grace);

    private:
        bool WaitForExit(std::chrono::milliseconds timeout);
        void ReportExit(int status);

        std::string _name;
        pid_t _pid = -1;
        bool _exited = false;
        bool _exitedCleanly = false;
        int _exitCode = -1;
        bool _stopping = false;
        std::string _logFile;
        std::chrono::steady_clock::time_point _startedAt;
    };
}

#endif
