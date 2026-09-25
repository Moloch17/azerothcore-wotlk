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

#include "BotFactory.h"
#include "CharacterCache.h"
#include "GameTime.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SocialMgr.h"
#include "Transport.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    /// Player has no setter for m_social on a stock core; LoadFromDB assigns it from the login query. Explicit
    /// instantiation ignores access checks, so this names the private member without a core change.
    template <typename Tag, typename Tag::Type Member>
    struct PrivateMember
    {
        friend typename Tag::Type Access(Tag) { return Member; }
    };

    struct PlayerSocialTag
    {
        using Type = PlayerSocial* Player::*;
        friend Type Access(PlayerSocialTag);
    };

    template struct PrivateMember<PlayerSocialTag, &Player::m_social>;

    /// CharacterCreateInfo keeps its fields protected (it is filled from CMSG_CHAR_CREATE); a
    /// derived type may set them.
    class BotCreateInfo : public CharacterCreateInfo
    {
    public:
        explicit BotCreateInfo(Animus::BotFactory::BotSpec const& spec)
        {
            Name = spec.Name;
            Race = spec.Race;
            Class = spec.Class;
            Gender = spec.Gender;
        }
    };
}

Player* Animus::BotFactory::Create(BotSpec const& spec, WorldSession* session)
{
    bool const ownSession = !session;
    if (ownSession)
    {
        // accountFlags 0: no collector's edition voucher mail (Player::Create's only DB write).
        session = new WorldSession(spec.AccountId, std::string(spec.Name), 0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);

        // Default permissions for the security level, in memory. Must precede new Player, whose
        // constructor checks a permission and would otherwise run a sync login DB query.
        session->InitRBACDataForTest();

        // No account or character rows exist: on the forge core, logout, play time and instance binds write nothing.
        session->SetSimSession(true);
    }

    Player* bot = new Player(session);
    bot->GetMotionMaster()->Initialize();

    BotCreateInfo info(spec);
    ObjectGuid::LowType const guidLow = spec.GuidLow
        ? spec.GuidLow : sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!bot->Create(guidLow, &info))
    {
        LOG_ERROR("module.animus", "Player::Create failed for bot {} (race {}, class {})", spec.Name, spec.Race,
            spec.Class);
        delete bot;
        if (ownSession)
            delete session;
        return nullptr;
    }

    sInstanceSaveMgr->PlayerCreateBoundInstancesMaps(bot->GetGUID());
    // A null result gives an empty list. LogoutPlayer removes it again.
    bot->*Access(PlayerSocialTag{}) = sSocialMgr->LoadFromDB(nullptr, bot->GetGUID());
    session->SetPlayer(bot);

    // Never save: 0 disables the autosave countdown in Player::Update.
    bot->SetSaveTimer(0);

    // SetLevel, not GiveLevel: GiveLevel sends level-reward mail.
    if (spec.Level && bot->GetLevel() != spec.Level)
    {
        bot->SetLevel(spec.Level, false);
        bot->InitStatsForLevel(true);
        bot->InitTalentForLevel();

        // Raise weapon and defense skill caps to the new level (5 per level), then fill them, as
        // a character who levelled normally would have. Left at level 1 values, every swing would
        // roll against a skill of 5 and mostly miss.
        bot->UpdateSkillsForLevel();
        bot->UpdateSkillsToMaxSkillsForLevel();
    }

    bot->SetCanModifyStats(true);
    bot->UpdateAllStats();
    bot->SetFullHealth();

    sCharacterCache->AddCharacterCacheEntry(bot->GetGUID(), spec.AccountId, spec.Name, spec.Gender, spec.Race,
        spec.Class, bot->GetLevel());

    return bot;
}

Map* Animus::BotFactory::PlaceInNewInstance(Player* bot, uint32 mapId, Position const& pos)
{
    // A groupless player with no bind for this map always gets a brand new instance. A battleground map gives
    // the sim its own copy too (MapInstanced::CreateSimBattleground), which is how the flag stages get Warsong
    // Gulch's real ground.
    Map* map = sMapMgr->CreateMap(mapId, bot);
    if (!map || !(map->IsDungeon() || map->IsBattlegroundOrArena()))
    {
        LOG_ERROR("module.animus", "Map {} did not produce an instance for bot {}", mapId, bot->GetName());
        return nullptr;
    }

    return PlaceInMap(bot, map, pos) ? map : nullptr;
}

Map* Animus::BotFactory::PlaceOnContinent(Player* bot, uint32 mapId, Position const& pos)
{
    Map* map = sMapMgr->CreateBaseMap(mapId);
    if (!map || map->Instanceable())
    {
        LOG_ERROR("module.animus", "Map {} is not a continent for bot {}", mapId, bot->GetName());
        return nullptr;
    }

    map->LoadGrid(pos.GetPositionX(), pos.GetPositionY());
    return PlaceInMap(bot, map, pos) ? map : nullptr;
}

bool Animus::BotFactory::PlaceInMap(Player* bot, Map* map, Position const& pos)
{
    // Player::Create parked the bot on its race's start continent; move it before entering.
    bot->ResetMap();
    bot->Relocate(pos);
    bot->SetMap(map);
    bot->SetFallInformation(GameTime::GetGameTime().count(), pos.GetPositionZ());
    bot->SetMover(bot);

    ObjectAccessor::AddObject(bot);

    if (!map->AddPlayerToMap(bot))
    {
        LOG_ERROR("module.animus", "Could not add bot {} to map {} instance {}", bot->GetName(), map->GetId(),
            map->GetInstanceId());
        ObjectAccessor::RemoveObject(bot);
        return false;
    }

    return true;
}

bool Animus::BotFactory::TeleportWithinMap(Player* bot, Position const& pos)
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported())
        return false;

    if (!bot->TeleportTo(bot->GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
        pos.GetOrientation()))
        return false;

    WorldPacket ack(MSG_MOVE_TELEPORT_ACK);
    ack << bot->GetPackGUID();
    ack << uint32(0) << uint32(0);
    bot->GetSession()->HandleMoveTeleportAck(ack);
    return true;
}

void Animus::BotFactory::DestroyUnplaced(Player* bot)
{
    WorldSession* session = bot->GetSession();

    // ~Unit asserts that every aura (passives included) is gone; this is what removing it from a map
    // would have done. A failed placement may have set the map already.
    bot->CleanupsBeforeDelete();
    if (bot->FindMap())
        bot->ResetMap();

    // LogoutPlayer would have dropped these.
    sCharacterCache->DeleteCharacterCacheEntry(bot->GetGUID(), bot->GetName());
    sSocialMgr->RemovePlayerSocial(bot->GetGUID());

    session->SetPlayer(nullptr);
    delete bot;
    delete session;
}

WorldSession* Animus::BotFactory::Destroy(Player* bot, bool keepSession)
{
    WorldSession* session = bot->GetSession();
    ObjectGuid const guid = bot->GetGUID();
    uint32 const mapId = bot->GetMapId();

    // A bot in the middle of a far teleport is on no map.
    Map* map = bot->FindMap();
    Difficulty const difficulty = map ? map->GetDifficulty() : REGULAR_DIFFICULTY;

    // Off its transport first: the transport keeps a pointer to every passenger.
    if (Transport* transport = bot->GetTransport())
        transport->RemovePassenger(bot, true);

    // A dead bot would be repopped at a graveyard (a far teleport) by LogoutPlayer.
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);

    // The pet goes first, unsaved: LogoutPlayer would save it to the character database. Totems and
    // guardians (trinket summons, ghouls, water elementals) go with it, while the bot is still in its map.
    if (map)
    {
        if (Pet* pet = bot->GetPet())
            bot->RemovePet(pet, PET_SAVE_AS_DELETED);

        bot->UnsummonAllTotems();
        bot->RemoveAllControlled();
    }

    if (bot->IsBeingTeleportedFar())
        LOG_WARN("module.animus", "Bot {} is being teleported to map {} ({}, {}, {}); logout completes the teleport "
            "first", bot->GetName(), bot->GetTeleportDest().GetMapId(), bot->GetTeleportDest().GetPositionX(),
            bot->GetTeleportDest().GetPositionY(), bot->GetTeleportDest().GetPositionZ());

    sCharacterCache->DeleteCharacterCacheEntry(guid, bot->GetName());

    // Removes the player from its map, drops its social list and deletes it; false = no SaveToDB.
    session->LogoutPlayer(false);

    // A sim session's bind was never written to the character database: nothing there to delete.
    sInstanceSaveMgr->PlayerUnbindInstance(guid, mapId, difficulty, false);

    if (keepSession)
        return session;

    delete session;
    return nullptr;
}
