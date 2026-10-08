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

#ifndef ANIMUS_LIB_CURRICULUM_ENTRANCE_RESPAWN_H
#define ANIMUS_LIB_CURRICULUM_ENTRANCE_RESPAWN_H

#include "Define.h"
#include <algorithm>

class Player;
struct Position;

namespace Animus::Curriculum
{
    struct SeatState;

    /// **Death in an instance** (dungeon-curriculum I4; the user, 2026-10-06: no graveyard). A seat that dies is out
    /// for a delay (Respawn.DelayMs), then is alive again -- full health, full power -- at the instance's entrance,
    /// and plays on: the episode does not end, no ghost walks, no corpse is run back to, and nothing teleports it to
    /// the party. It walks back on the player controller, by its own map and memory, and has rejoined once it is
    /// within Respawn.RejoinYards of the party (its leader, or the living party's centroid while the leader is down).
    ///
    /// This is the per-seat clock and the measures, with no world in it: it is fed a death and a walk back.
    /// RiseAtEntrance does the standing up.
    struct RespawnClock
    {
        enum class Step : uint8
        {
            None,
            Died,           // the seat went down this decision
            Rise,           // the delay is over: stand it up at the entrance now (then Risen)
            Rejoined,       // it is back with the party
        };

        bool Out = false;               // dead, waiting to rise
        uint32 DeadSinceMs = 0;
        bool Rejoining = false;         // risen, walking back
        uint32 RoseAtMs = 0;
        uint32 Deaths = 0;
        uint32 Rises = 0;
        uint32 Rejoins = 0;
        uint32 RejoinMsTotal = 0;       // over the rejoins: the mean is RejoinMsTotal / Rejoins
        uint32 OutMsTotal = 0;          // time spent dead

        /// One decision: `alive` now, `partyYards` from the party (negative when there is no party to be with).
        Step Note(uint32 nowMs, bool alive, float partyYards, uint32 delayMs, float rejoinYards)
        {
            if (!alive)
            {
                if (!Out)
                {
                    Out = true;
                    Rejoining = false;
                    DeadSinceMs = std::max<uint32>(1, nowMs);
                    ++Deaths;
                    return Step::Died;
                }
                return nowMs >= DeadSinceMs + delayMs ? Step::Rise : Step::None;
            }

            // Stood up by anything else (a friend's resurrection): it rises where it lies, and walks back from there.
            if (Out)
                Risen(nowMs);
            if (Rejoining && partyYards >= 0.0f && partyYards <= rejoinYards)
            {
                Rejoining = false;
                ++Rejoins;
                RejoinMsTotal += nowMs - std::min(nowMs, RoseAtMs);
                return Step::Rejoined;
            }
            return Step::None;
        }

        /// The seat stood up at `nowMs`.
        void Risen(uint32 nowMs)
        {
            if (Out)
                OutMsTotal += nowMs - std::min(nowMs, DeadSinceMs);
            Out = false;
            DeadSinceMs = 0;
            Rejoining = true;
            RoseAtMs = std::max<uint32>(1, nowMs);
            ++Rises;
        }

        /// The mean seconds from rising to rejoining, over the rejoins (0 with none).
        [[nodiscard]] float RejoinSeconds() const
        {
            return Rejoins ? float(RejoinMsTotal) / float(Rejoins) / 1000.0f : 0.0f;
        }

        /// The share of rises that rejoined (1 with no rise: nothing was left to rejoin).
        [[nodiscard]] float RejoinedShare() const
        {
            return Rises ? float(Rejoins) / float(Rises) : 1.0f;
        }
    };

    /// **A wipe in a whole dungeon** (InstanceEncounter, I4 wired into the wing runs): nobody standing counts as one
    /// wipe, once, until somebody stands again -- risen at the entrance by its RespawnClock like any death, never stood
    /// up at the door all together by a teleport. The run ends past its allowance of wipes.
    struct WipeLatch
    {
        bool Counted = false;

        /// One decision: whether anybody of the party is alive, whether the dungeon is cleared. True when a new
        /// wipe is counted now.
        bool Note(bool anyoneAlive, bool cleared)
        {
            if (anyoneAlive || cleared)
            {
                Counted = false;
                return false;
            }
            if (Counted)
                return false;
            Counted = true;
            return true;
        }
    };

    /// Stand `bot` up at `entrance` as RespawnClock's Rise asks: alive, full health and power, out of combat, no keys
    /// held, its player controller started again from where the server put it (the body would otherwise walk on from
    /// where it died). False when the move failed; it is alive either way.
    bool RiseAtEntrance(Player* bot, SeatState& seat, Position const& entrance, uint32 nowMs);

    /// The seat's last frame forgotten (RiseAtEntrance does it first): its entity list empty and its sight slots naming
    /// nothing, until the camera casts again where it stands now.
    void ForgetFrame(SeatState& seat);
}

#endif
