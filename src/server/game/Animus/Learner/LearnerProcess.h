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
#include <string>

namespace AnimusForge
{
    struct ForgeConfig;

    /// The Python learner as a child process of the worldserver.
    ///
    /// Lifetime is tied to the learner socket rather than to signals: when the server shuts down, a run is
    /// cancelled (or the server crashes) the socket closes, the learner's train loop sees the disconnect, saves
    /// latest.pt and exits by itself. Stop() only escalates if it does not.
    class LearnerProcess : public ChildProcess
    {
    public:
        LearnerProcess() : ChildProcess("Learner") { }

        /// Validate the setup and spawn the learner for `scenario`: training from scratch, or with `resume`
        /// continuing runs/<scenario>/latest.pt. Returns false (with the reason logged) if the working directory
        /// or config is missing or the process cannot be started.
        bool Start(ForgeConfig const& config, std::string const& scenario, bool resume);

        /// The command line to start the learner by hand, for error messages.
        [[nodiscard]] static std::string ManualCommand(ForgeConfig const& config, std::string const& scenario,
            bool resume);
    };
}

#endif
