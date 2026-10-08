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
#include <atomic>

namespace
{
    std::atomic<uint32> ForgeTickMs{ 0 };
}

namespace ForgeCore
{
    bool HasClients()
    {
        // The forge opens no world listener and registers no sim session with the session manager, so there is
        // never a real client to build a packet for.
        return false;
    }

    void SetTickMs(uint32 tickMs)
    {
        ForgeTickMs.store(tickMs, std::memory_order_relaxed);
    }

    uint32 TickMs()
    {
        return ForgeTickMs.load(std::memory_order_relaxed);
    }
}
