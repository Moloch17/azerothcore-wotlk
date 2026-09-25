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

#ifndef ANIMUS_LIB_BOT_ACCOUNTS_H
#define ANIMUS_LIB_BOT_ACCOUNTS_H

#include "Define.h"

/*
 * Account ids of in-memory bots. They exist only in memory, but every bot needs an account id of its own, and they
 * must never collide with each other or with a real realm's accounts. Each kind of bot gets its own range.
 */
namespace Animus::BotAccounts
{
    /// Far above anything a real realm allocates.
    constexpr uint32 BASE = 0x7F000000;

    /// Learned agents per env (a party's or a raid's seats) and bots per agent (two sessions alternate across
    /// rebuilds). Must cover Curriculum::MAX_SEATS, which StageScenario asserts.
    constexpr uint32 SEATS_PER_ENV = 40;
    constexpr uint32 SESSIONS_PER_BOT = 2;

    constexpr uint32 OWNER_OFFSET = 100000;
    constexpr uint32 OPPONENT_OFFSET = 300000;
    constexpr uint32 AMBUSHER_OFFSET = 500000;
    constexpr uint32 AMBUSHERS_PER_ENV = 2;

    /// Most envs whose seat accounts stay below the owner range (the owner and opponent ranges hold more).
    constexpr uint32 MAX_ENVS = OWNER_OFFSET / (SEATS_PER_ENV * SESSIONS_PER_BOT);
    static_assert(MAX_ENVS >= 1024, "seat accounts leave too few envs for the sim to run");
    static_assert(MAX_ENVS * SESSIONS_PER_BOT <= OPPONENT_OFFSET - OWNER_OFFSET, "owner accounts overlap opponents'");
    static_assert(MAX_ENVS * SESSIONS_PER_BOT <= AMBUSHER_OFFSET - OPPONENT_OFFSET,
        "opponent accounts overlap ambushers'");

    /// A learned agent's bot: env, seat, session slot.
    [[nodiscard]] constexpr uint32 Seat(uint32 env, uint32 seat, uint8 session)
    {
        return BASE + env * SEATS_PER_ENV * SESSIONS_PER_BOT + seat * SESSIONS_PER_BOT + session;
    }

    /// A scripted owner (companion and party stages).
    [[nodiscard]] constexpr uint32 Owner(uint32 env, uint8 session)
    {
        return BASE + OWNER_OFFSET + env * SESSIONS_PER_BOT + session;
    }

    /// A scripted enemy player (PvP stage).
    [[nodiscard]] constexpr uint32 Opponent(uint32 env, uint8 session)
    {
        return BASE + OPPONENT_OFFSET + env * SESSIONS_PER_BOT + session;
    }

    /// A scripted enemy player ambushing the owner: env, ambusher, session slot.
    [[nodiscard]] constexpr uint32 Ambusher(uint32 env, uint32 ambusher, uint8 session)
    {
        return BASE + AMBUSHER_OFFSET + env * AMBUSHERS_PER_ENV * SESSIONS_PER_BOT + ambusher * SESSIONS_PER_BOT
            + session;
    }

    /// Short-lived probe characters that discover a class's spells, one per race, below every other range.
    [[nodiscard]] constexpr uint32 Probe(uint8 race)
    {
        return BASE - 1 - race;
    }
}

#endif
