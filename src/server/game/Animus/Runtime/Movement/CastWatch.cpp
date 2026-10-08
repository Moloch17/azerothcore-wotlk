/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#include "CastWatch.h"
#include "WorldPacket.h"

Animus::Movement::CastWatch*& Animus::Movement::WatchedCast()
{
    thread_local CastWatch* watch = nullptr;
    return watch;
}

void Animus::Movement::NoteCastFailed(WorldSession const* session, WorldPacket const& packet)
{
    CastWatch* watch = WatchedCast();
    if (!watch || watch->Session != session)
        return;
    // SMSG_CAST_FAILED: the cast count (uint8), the spell (uint32), the result (uint8), then the result's own data.
    if (packet.size() < 6)
        return;
    uint8 const* bytes = packet.contents();
    ++watch->Failures;
    watch->Spell = uint32(bytes[1]) | (uint32(bytes[2]) << 8) | (uint32(bytes[3]) << 16) | (uint32(bytes[4]) << 24);
    watch->Result = bytes[5];
}
