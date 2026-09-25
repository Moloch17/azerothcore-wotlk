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

#ifndef MOD_ANIMUS_FORGE_LEARNER_PROCESS_H
#define MOD_ANIMUS_FORGE_LEARNER_PROCESS_H

#include "ChildProcess.h"
#include "Define.h"
#include <memory>
#include <string>
#include <vector>

namespace AnimusForge
{
    struct ForgeConfig;

    /// The Python learner as child processes of the worldserver: one, or in multi-GPU mode (AnimusForge.Gpu.Mode)
    /// one per rank of a data-parallel run (animus.parallel), each on its own GPU, its own slice of the cores left
    /// over from the map update, and its own log. Rank 0 is the run's leader and writes animus-learner.log; rank k
    /// writes animus-learner.rank<k>.log beside it.
    ///
    /// Lifetime is tied to the learner socket rather than to signals: when the server shuts down, a run is
    /// cancelled (or the server crashes) the socket closes, the learners' train loops see the disconnect, save
    /// latest.pt and exit by themselves. Stop() only escalates if they do not.
    class LearnerProcess
    {
    public:
        LearnerProcess();

        /// Validate the setup and spawn the learner(s) for `scenario`: training from scratch, or with `resume`
        /// continuing runs/<scenario>/latest.pt. `config.LearnerRanks` is how many. Returns false (with the reason
        /// logged, and any rank already started stopped) if the working directory or config is missing or a
        /// process cannot be started.
        bool Start(ForgeConfig const& config, std::string const& scenario, bool resume);

        /// The command line(s) to start the learner(s) by hand, for error messages.
        [[nodiscard]] static std::string ManualCommand(ForgeConfig const& config, std::string const& scenario,
            bool resume);

        /// Rank k's log file, beside rank 0's `logFile`.
        [[nodiscard]] static std::string RankLogFile(std::string const& logFile, uint32 rank);

        /// Reap every rank that has exited, logging how each ended once.
        void Poll();

        /// Any rank is still running.
        [[nodiscard]] bool IsRunning() const;
        /// The leader's pid (rank 0), or -1.
        [[nodiscard]] pid_t Pid() const { return _ranks.front()->Pid(); }
        /// Every rank of the last start exited with status 0.
        [[nodiscard]] bool FinishedCleanly() const;
        /// A rank exited some other way (and was not stopped on purpose).
        [[nodiscard]] bool FailedUnexpectedly() const;

        void ExpectExit();
        /// ChildProcess::Stop on every rank in turn. They exit together once the socket is gone, so by the time the
        /// leader has, the others usually have too.
        void Stop(std::chrono::milliseconds grace);

    private:
        /// Rank 0 always; ranks 1.. for the last start's data-parallel run.
        std::vector<std::unique_ptr<ChildProcess>> _ranks;
        uint32 _started = 1;
    };
}

#endif
