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

#ifndef ANIMUS_LIB_BOT_SLOT_H
#define ANIMUS_LIB_BOT_SLOT_H

#include "BotFactory.h"
#include "ObjectGuid.h"
#include <array>
#include <memory>

class Map;
class Player;
class WorldSession;
struct Position;

namespace Animus
{
    /// One bot that is rebuilt every episode (a learned seat, a scripted owner or opponent).
    ///
    /// Two sessions and two GUIDs alternate: the next bot is created and placed on the idle session before the old
    /// one leaves, so an instance never loses its last player, and neither sessions nor GUIDs are allocated per
    /// rebuild (the core keeps some per-GUID state for the life of the server).
    ///
    ///     slot.Begin();                                   // remember the current bot
    ///     spec.AccountId = BotAccounts::Owner(env, slot.NextSession());
    ///     Player* bot = slot.CreateNext(spec, map, mapId, start);
    ///     ...                                             // configure the new bot
    ///     slot.Promote();                                 // the old bot goes, the new one is the slot's bot
    class BotSlot
    {
    public:
        BotSlot();
        ~BotSlot();
        BotSlot(BotSlot&&) noexcept;
        BotSlot& operator=(BotSlot&&) noexcept;
        BotSlot(BotSlot const&) = delete;
        BotSlot& operator=(BotSlot const&) = delete;

        /// The slot's bot, through its session rather than ObjectAccessor: a bot in the middle of a far teleport is
        /// out of the world but still the slot's, and still has to be destroyed.
        [[nodiscard]] Player* Active() const;

        /// Start a rebuild: the current bot stays until Promote.
        void Begin();

        /// The session slot the next bot goes on: the idle one when a bot exists, else the active one. Account ids
        /// and names derived from it keep the two bots apart.
        [[nodiscard]] uint8 NextSession() const;

        /// Create the next bot on NextSession() with that session's GUID and place it: into `map`, or into a new
        /// instance of `mapId` when `map` is null (which then points at it). Null on failure, with nothing changed.
        Player* CreateNext(BotFactory::BotSpec spec, Map*& map, uint32 mapId, Position const& start);

        /// End the rebuild: the bot remembered by Begin is destroyed (its session kept), and the bot CreateNext made,
        /// if any, becomes the slot's bot.
        void Promote();

        /// Cancel the rebuild: the bot CreateNext made, if any, is destroyed (its session kept) and the bot remembered
        /// by Begin stays the slot's bot.
        void Abort();

        /// Destroy the slot's bot and its session (teardown).
        void Destroy();

    private:
        std::array<std::unique_ptr<WorldSession>, 2> _sessions;
        std::array<ObjectGuid::LowType, 2> _guids{};
        uint8 _active = 0;
        Player* _previous = nullptr;
        bool _created = false;
    };
}

#endif
