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

#include "BotSlot.h"
#include "Player.h"
#include "SharedDefines.h"
#include "DBCStores.h"
#include "Player.h"
#include "Position.h"
#include "WorldSession.h"

Animus::BotSlot::BotSlot() = default;
Animus::BotSlot::~BotSlot() = default;
Animus::BotSlot::BotSlot(BotSlot&&) noexcept = default;
Animus::BotSlot& Animus::BotSlot::operator=(BotSlot&&) noexcept = default;

Player* Animus::BotSlot::Active() const
{
    WorldSession* session = _sessions[_active].get();
    return session ? session->GetPlayer() : nullptr;
}

void Animus::BotSlot::Begin()
{
    _previous = Active();
    _created = false;
}

uint8 Animus::BotSlot::NextSession() const
{
    return _previous ? uint8(1 - _active) : _active;
}

Player* Animus::BotSlot::CreateNext(BotFactory::BotSpec spec, Map*& map, uint32 mapId, Position const& start)
{
    uint8 const session = NextSession();
    spec.GuidLow = _guids[session];

    Player* bot = BotFactory::Create(spec, _sessions[session].get());
    if (!bot)
        return nullptr;

    // Told before it is placed: the map it lands on is the battleground's, and MapInstanced reaches that
    // through the invitation this records.
    if (spec.BattlegroundId)
        bot->SetBattlegroundId(spec.BattlegroundId, BattlegroundTypeId(spec.BattlegroundType), 0, true, false,
            TeamId(spec.BattlegroundTeam));
    // Likewise the difficulty: MapInstanced opens a new instance at the first player's.
    bot->SetDungeonDifficulty(Difficulty(spec.DungeonDifficulty));
    bot->SetRaidDifficulty(Difficulty(spec.RaidDifficulty));

    // Create made a session if the slot had none; the slot owns it either way.
    if (_sessions[session].get() != bot->GetSession())
        _sessions[session].reset(bot->GetSession());
    _guids[session] = bot->GetGUID().GetCounter();

    // No map yet: a new instance of an instanceable map, else the continent every env shares.
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
    bool const placed = map ? BotFactory::PlaceInMap(bot, map, start)
        : (map = mapEntry && !mapEntry->Instanceable() ? BotFactory::PlaceOnContinent(bot, mapId, start)
            : BotFactory::PlaceInNewInstance(bot, mapId, start)) != nullptr;
    if (!placed)
    {
        // DestroyUnplaced deletes the session too.
        [[maybe_unused]] WorldSession* released = _sessions[session].release();
        BotFactory::DestroyUnplaced(bot);
        return nullptr;
    }

    _created = true;
    return bot;
}

void Animus::BotSlot::Promote()
{
    uint8 const next = NextSession();

    // The session stays with the slot for the next rebuild.
    if (_previous)
        BotFactory::Destroy(_previous, true);

    if (_created)
        _active = next;

    _previous = nullptr;
    _created = false;
}

void Animus::BotSlot::Abort()
{
    if (_created)
    {
        // CreateNext put the new bot on NextSession(); the session stays with the slot for the next rebuild.
        if (Player* bot = _sessions[NextSession()]->GetPlayer())
            BotFactory::Destroy(bot, true);
    }

    _previous = nullptr;
    _created = false;
}

void Animus::BotSlot::Destroy()
{
    if (Player* bot = Active())
    {
        // Destroy deletes the session.
        [[maybe_unused]] WorldSession* released = _sessions[_active].release();
        BotFactory::Destroy(bot);
    }

    _previous = nullptr;
    _created = false;
}
