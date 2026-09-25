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
/// The forge is unconditional: it is always the sim host, always built this way. The one sanctioned
/// exception is playtest mode, which exists so a human can log a real client in and confirm the world
/// is joinable and behaves: the game clock follows the wall clock, the world listener starts, Warden
/// runs and the database stays open. Everything else in the fork is the same in both modes.
namespace ForgeCore
{
    /// Read Forge.Playtest once, after the configs are loaded and before anything asks. Never re-read.
    AC_GAME_API void LoadSettings();

    /// A real client can log in: wall clock, listener, Warden, database open.
    AC_GAME_API bool Playtest();

    /// Whether any real client session exists right now. Sim sessions are never registered with the
    /// session manager, so this is the cheap answer to "is there anybody to build a packet for": the
    /// packet builders skip their work while it is false, and behave as stock while it is true.
    AC_GAME_API bool HasClients();
}

#endif
