/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
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

#include "Forge.h"
#include "Config.h"
#include "Log.h"
#include "WorldSessionMgr.h"

namespace
{
    bool ForgePlaytest = false;
}

namespace Forge
{
    void LoadSettings()
    {
        ForgePlaytest = sConfigMgr->GetOption<bool>("Forge.Playtest", false);
        if (ForgePlaytest)
            LOG_INFO("server.worldserver", "Forge.Playtest is on: wall clock, world listener, Warden and the database stay up for a real client");
    }

    bool Playtest()
    {
        return ForgePlaytest;
    }

    bool HasClients()
    {
        return sWorldSessionMgr->GetActiveSessionCount() > 0;
    }
}
