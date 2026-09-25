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

#ifndef ANIMUS_LIB_BOT_FACTORY_H
#define ANIMUS_LIB_BOT_FACTORY_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>

class Map;
class Player;
class WorldSession;
struct Position;

/*
 * Bots are players without a client or a character row, built with public core APIs only, so the same code runs on the
 * forge core and on a stock AzerothCore.
 *
 * Mirrors the create path (CharacterHandler: new Player -> MotionMaster::Initialize -> Create) and the in-world half
 * of the login path (HandlePlayerLoginFromDB: SetPlayer -> SetMover -> ObjectAccessor::AddObject ->
 * Map::AddPlayerToMap), skipping every step that reads or writes the character database and every step whose only
 * effect is a packet to the bot's own client. Real players can see bots (a game master watching a stage), so a bot also
 * gets an empty social list (whispers and invites read it unchecked) and a character cache entry (name queries answer
 * from it, else clients show "Unknown").
 */
namespace Animus::BotFactory
{
    struct BotSpec
    {
        /// Unique among the server's players; real character names cannot contain digits, so a name with one never
        /// collides with a player's.
        std::string Name;
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Gender = 0;
        uint8 Level = 1;
        uint32 AccountId = 0;
        /// The battleground instance this bot joins, and on whose side. 0 = none, and the bot is placed the
        /// ordinary way. A battleground map is reached only through this: MapInstanced asks the player which
        /// battleground it was invited to.
        uint32 BattlegroundId = 0;
        uint32 BattlegroundType = 0;
        uint8 BattlegroundTeam = 0;
        /// The difficulty a new instance is opened at when this bot is the first in (Difficulty: DUNGEON_*, RAID_*).
        uint8 DungeonDifficulty = 0;
        uint8 RaidDifficulty = 0;

        /// Player GUID counter to create the bot with; 0 = a new one. A scenario that rebuilds bots
        /// reuses a fixed set: the core keeps some per-GUID state for the life of the server (e.g.
        /// InstanceSaveMgr's bind storage), so a new GUID per rebuild would grow memory forever.
        /// The previous bot with the GUID must be destroyed first.
        ObjectGuid::LowType GuidLow = 0;
    };

    /// Build a socketless session and a freshly created (never saved) character.
    ///
    /// The session is deliberately NOT registered with WorldSessionMgr: a socketless session is
    /// deleted there on the next update, logging its player out with a save. The bot is driven by
    /// Map::Update (MapSessionFilter + Player::Update) once it is on a map. Autosave is disabled.
    /// Returns nullptr on failure. The player is not in the world yet.
    ///
    /// `session` reuses the socketless session of a bot destroyed with keepSession, so a scenario that
    /// rebuilds its bots every episode does not create and delete a session each time.
    Player* Create(BotSpec const& spec, WorldSession* session = nullptr);

    /// Put a Create()d bot into its own new instance of `mapId` at `pos`, the server-side
    /// equivalent of logging in there. Returns the map, or nullptr on failure.
    Map* PlaceInNewInstance(Player* bot, uint32 mapId, Position const& pos);

    /// Put a Create()d bot on continent `mapId` (not instanceable, shared) at `pos`. Returns the map, or nullptr on
    /// failure.
    Map* PlaceOnContinent(Player* bot, uint32 mapId, Position const& pos);

    /// Put a Create()d bot into an existing map at `pos`. Returns false on failure.
    bool PlaceInMap(Player* bot, Map* map, Position const& pos);

    /// Teleport a placed bot to `pos` on its own map (an instance too), acknowledging for the client it has not.
    bool TeleportWithinMap(Player* bot, Position const& pos);

    /// Log the bot out without saving and drop its instance bind. Deletes the session unless
    /// keepSession, in which case it is returned for the next Create.
    WorldSession* Destroy(Player* bot, bool keepSession = false);

    /// Delete a Create()d bot that was never placed on a map (and its session).
    void DestroyUnplaced(Player* bot);
}

#endif
