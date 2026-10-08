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

#ifndef ACORE_FORGE_CORE_H
#define ACORE_FORGE_CORE_H

#include "Define.h"

/// The sim host's few process-wide switches.
///
/// The forge is unconditional: it is always the sim host, always built this way.
namespace ForgeCore
{
    /// Whether any real client session exists right now: never, the forge opens no listener. Sim sessions are
    /// never registered with the session manager, so this is the cheap answer to "is there anybody to build a
    /// packet for": the packet builders skip their work while it is false, and behave as stock while it is true.
    AC_GAME_API bool HasClients();

    /// The world tick (game ms) the running stage wants, set by the module when a stage starts and cleared (0) when
    /// the plan ends: AnimusForge.Stage.<name>.TicksPerDecision cuts one stage's decision finer than the rest.
    /// ForgeMain's update loop reads it every tick and falls back to the configured tick at 0.
    AC_GAME_API void SetTickMs(uint32 tickMs);
    AC_GAME_API uint32 TickMs();
}

#endif
