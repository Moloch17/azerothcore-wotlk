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

#ifndef ANIMUS_MOVEMENT_CAST_WATCH_H
#define ANIMUS_MOVEMENT_CAST_WATCH_H

#include "Define.h"

class WorldPacket;
class WorldSession;

/// **The server's answer to a cast a seat's client sent** (dungeon-curriculum I1): a seat casts as a client does, its
/// CMSG_CAST_SPELL or CMSG_USE_ITEM through the session's own handler, and the handler keeps the cast's result to
/// itself -- a refusal goes back to the client as SMSG_CAST_FAILED. A sim session has no client to send it to, so
/// while a seat's press is in its handler, a watch on that session catches the refusal (WorldSession::SendPacket's
/// sim branch, NoteCastFailed) and the press reads it back: a refused cast is priced by its cause, as a player sees
/// "Out of range" and learns. The watch is the pressing thread's own (a seat is pressed on its map's thread).
namespace Animus::Movement
{
    struct CastWatch
    {
        WorldSession const* Session = nullptr;
        uint32 Failures = 0;
        uint32 Spell = 0;           // the last refused cast's spell
        uint8 Result = 0;           // ... and its SpellCastResult
    };

    /// The watch this thread's press has open, or null.
    [[nodiscard]] CastWatch*& WatchedCast();

    /// SMSG_CAST_FAILED sent to `session` (a sim session's dropped packet): noted on the open watch over it.
    void NoteCastFailed(WorldSession const* session, WorldPacket const& packet);

    /// A watch over `session` for as long as it lives.
    class ScopedCastWatch
    {
    public:
        explicit ScopedCastWatch(WorldSession const* session) : _previous(WatchedCast())
        {
            _watch.Session = session;
            WatchedCast() = &_watch;
        }
        ~ScopedCastWatch() { WatchedCast() = _previous; }
        ScopedCastWatch(ScopedCastWatch const&) = delete;
        ScopedCastWatch& operator=(ScopedCastWatch const&) = delete;

        [[nodiscard]] CastWatch const& Watch() const { return _watch; }

    private:
        CastWatch _watch;
        CastWatch* _previous;
    };
}

#endif
