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

#include "EntityActions.h"
#include "Bag.h"
#include "CastWatch.h"
#include "Creature.h"
#include "GameObject.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

namespace
{
    namespace Ea = Animus::Curriculum::EntityActions;

    /// The session's own handlers, by opcode.
    class HandlerPort final : public Ea::ClientPort
    {
    public:
        void Send(Player* bot, WorldPacket& packet) override
        {
            WorldSession* session = bot ? bot->GetSession() : nullptr;
            if (!session)
                return;
            packet.rpos(0);
            switch (packet.GetOpcode())
            {
                case CMSG_SET_SELECTION: session->HandleSetSelectionOpcode(packet); break;
                case CMSG_ATTACKSWING:   session->HandleAttackSwingOpcode(packet); break;
                case CMSG_GAMEOBJ_USE:   session->HandleGameObjectUseOpcode(packet); break;
                case CMSG_GOSSIP_HELLO:  session->HandleGossipHelloOpcode(packet); break;
                case CMSG_USE_ITEM:      session->HandleUseItemOpcode(packet); break;
                case CMSG_CAST_SPELL:    session->HandleCastSpellOpcode(packet); break;
                default:                 break;
            }
        }
    };

    /// The client's cast count: one more for every cast it sends.
    uint8 NextCastCount()
    {
        thread_local uint8 count = 0;
        return ++count;
    }

    /// A refused cast's cause, for its price.
    Ea::Refusal CastRefusal(uint32 failed)
    {
        switch (SpellCastResult(failed - 1))
        {
            case SPELL_FAILED_OUT_OF_RANGE:
            case SPELL_FAILED_TOO_CLOSE:
                return Ea::Refusal::Reach;
            case SPELL_FAILED_LINE_OF_SIGHT:
                return Ea::Refusal::Sight;
            default:
                return Ea::Refusal::Cast;
        }
    }

    bool TakesObjects(SpellInfo const* info)
    {
        return (info->GetExplicitTargetMask() & TARGET_FLAG_GAMEOBJECT) != 0;
    }

    bool TakesUnits(SpellInfo const* info)
    {
        return (info->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK) != 0;
    }

    /// The use spell an item carries, if any.
    SpellInfo const* UseSpellOf(Item const* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return nullptr;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return sSpellMgr->GetSpellInfo(uint32(spell.SpellId));
        return nullptr;
    }
}

char const* Animus::Curriculum::EntityActions::RefusalName(Refusal refusal)
{
    switch (refusal)
    {
        case Refusal::None:     return "none";
        case Refusal::Gone:     return "gone";
        case Refusal::Kind:     return "kind";
        case Refusal::Reach:    return "reach";
        case Refusal::Sight:    return "sight";
        case Refusal::Loot:     return "loot";
        case Refusal::NoItem:   return "no_item";
        case Refusal::NoTarget: return "no_target";
        case Refusal::Cast:     return "cast";
        case Refusal::Locked:   return "locked";
        case Refusal::Count:    break;
    }
    return "unknown";
}

Animus::Curriculum::EntityActions::ClientPort& Animus::Curriculum::EntityActions::SessionPort()
{
    static HandlerPort port;
    return port;
}

bool Animus::Curriculum::EntityActions::AtClient(Player* bot, WorldObject const* object)
{
    if (!bot || !object || !bot->IsInWorld() || !object->IsInWorld() || object->GetMap() != bot->GetMap())
        return false;
    if (object == bot)
        return true;
    return bot->IsWithinDist(object, bot->GetSightRange(object), false) && bot->CanSeeOrDetect(object);
}

WorldObject* Animus::Curriculum::EntityActions::ResolveAtClient(Player* bot, ObjectGuid guid)
{
    if (!bot || !guid || !bot->IsInWorld())
        return nullptr;
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    return AtClient(bot, object) ? object : nullptr;
}

WorldPacket Animus::Curriculum::EntityActions::SetSelection(ObjectGuid guid)
{
    WorldPacket packet(CMSG_SET_SELECTION, 8);
    packet << guid;
    return packet;
}

WorldPacket Animus::Curriculum::EntityActions::AttackSwing(ObjectGuid guid)
{
    WorldPacket packet(CMSG_ATTACKSWING, 8);
    packet << guid;
    return packet;
}

WorldPacket Animus::Curriculum::EntityActions::GameObjectUse(ObjectGuid guid)
{
    WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
    packet << guid;
    return packet;
}

WorldPacket Animus::Curriculum::EntityActions::GossipHello(ObjectGuid guid)
{
    WorldPacket packet(CMSG_GOSSIP_HELLO, 8);
    packet << guid;
    return packet;
}

WorldPacket Animus::Curriculum::EntityActions::UseItem(uint8 bag, uint8 slot, ObjectGuid item, uint32 spellId,
    uint8 castCount, SpellCastTargets& targets)
{
    // HandleUseItemOpcode: bag, slot, cast count, spell, item, glyph index, cast flags, then the targets.
    WorldPacket packet(CMSG_USE_ITEM, 1 + 1 + 1 + 4 + 8 + 4 + 1 + 16);
    packet << bag << slot << castCount << spellId << item << uint32(0) << uint8(0);
    targets.Write(packet);
    return packet;
}

WorldPacket Animus::Curriculum::EntityActions::CastSpell(uint32 spellId, uint8 castCount, SpellCastTargets& targets)
{
    // HandleCastSpellOpcode: cast count, spell, cast flags, then the targets.
    WorldPacket packet(CMSG_CAST_SPELL, 1 + 4 + 1 + 16);
    packet << castCount << spellId << uint8(0);
    targets.Write(packet);
    return packet;
}

bool Animus::Curriculum::EntityActions::OpensLoot(uint32 goType)
{
    return goType == GAMEOBJECT_TYPE_CHEST || goType == GAMEOBJECT_TYPE_FISHINGNODE
        || goType == GAMEOBJECT_TYPE_FISHINGHOLE;
}

bool Animus::Curriculum::EntityActions::LockedToHand(uint32 goType, uint32 lockId)
{
    return lockId && (goType == GAMEOBJECT_TYPE_DOOR || goType == GAMEOBJECT_TYPE_BUTTON
        || goType == GAMEOBJECT_TYPE_GOOBER);
}

Animus::Curriculum::EntityActions::Refusal Animus::Curriculum::EntityActions::JudgeObjectUse(ObjectFacts const& facts)
{
    if (OpensLoot(facts.Type))
        return Refusal::Loot;
    if (!facts.Selectable)
        return Refusal::Kind;
    // The server's handler opens a locked door to any hand (GameObject::Use takes no lock); the client never sends it.
    if (facts.Locked)
        return Refusal::Locked;
    if (facts.Distance > facts.Reach)
        return Refusal::Reach;
    return Refusal::None;
}

Animus::Curriculum::EntityActions::Refusal Animus::Curriculum::EntityActions::JudgeUnitInteract(UnitFacts const& facts)
{
    // A corpse is only ever looted: never opened.
    if (facts.Dead)
        return Refusal::Loot;
    if (facts.Player || facts.Hostile || !facts.NpcFlags)
        return Refusal::Kind;
    if (facts.Distance > facts.Reach)
        return Refusal::Reach;
    return Refusal::None;
}

Item* Animus::Curriculum::EntityActions::KeyItemFor(Player* bot, WorldObject const* object, SpellInfo const*& spell)
{
    spell = nullptr;
    if (!bot || !object)
        return nullptr;
    bool const isObject = object->ToGameObject() != nullptr;
    auto const fits = [&](Item* item) -> bool
    {
        SpellInfo const* info = UseSpellOf(item);
        if (!info || !(isObject ? TakesObjects(info) : TakesUnits(info)))
            return false;
        spell = info;
        return true;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); item && fits(item))
            return item;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(uint8(slot)); item && fits(item))
                    return item;
    return nullptr;
}

Animus::Curriculum::EntityActions::CastOutcome Animus::Curriculum::EntityActions::CastThroughClient(Player* bot,
    SpellInfo const* info, SpellCastTargets& targets, ClientPort& port)
{
    CastOutcome outcome;
    if (!bot || !info || !bot->GetSession())
        return outcome;
    WorldPacket packet = CastSpell(info->Id, NextCastCount(), targets);
    std::size_t const queued = bot->SpellQueue.size();
    Movement::ScopedCastWatch watch(bot->GetSession());
    port.Send(bot, packet);
    if (watch.Watch().Failures)
    {
        outcome.Failed = uint32(watch.Watch().Result) + 1;
        return outcome;
    }
    outcome.Sent = true;
    outcome.Queued = bot->SpellQueue.size() > queued;
    return outcome;
}

Animus::Curriculum::EntityActions::Refusal Animus::Curriculum::EntityActions::Apply(Press press, Player* bot,
    uint64 guid, ObjectGuid& focus, SeatActionResult& result, ClientPort& port, Resolver const& resolve)
{
    auto const refuse = [&result](Refusal refusal)
    {
        result.ActRefused = uint8(refusal);
        return refusal;
    };
    if (!bot || !guid)
        return refuse(Refusal::Gone);
    WorldObject* object = resolve(bot, ObjectGuid(guid));
    if (!object)
        return refuse(Refusal::Gone);
    result.ActedOn = object->GetGUID();
    result.ActPress = uint8(press);
    Unit* unit = object->ToUnit();
    GameObject* go = object->ToGameObject();

    switch (press)
    {
        case Press::Select:
        {
            if (!unit)
                return refuse(Refusal::Kind);
            WorldPacket packet = SetSelection(unit->GetGUID());
            port.Send(bot, packet);
            ++result.Selections;
            // Swinging already: the client turns its auto attack onto a hostile new target.
            if (bot->GetVictim() && bot->GetVictim() != unit && unit->IsAlive() && bot->IsValidAttackTarget(unit))
            {
                WorldPacket swing = AttackSwing(unit->GetGUID());
                port.Send(bot, swing);
            }
            return Refusal::None;
        }
        case Press::Assist:
        {
            if (!unit)
                return refuse(Refusal::Kind);
            ObjectGuid const its = unit->GetTarget();
            if (!its || !resolve(bot, its))
                return refuse(Refusal::NoTarget);
            WorldPacket packet = SetSelection(its);
            port.Send(bot, packet);
            ++result.Selections;
            return Refusal::None;
        }
        case Press::Focus:
        {
            if (!unit)
                return refuse(Refusal::Kind);
            focus = unit->GetGUID();
            ++result.Selections;
            return Refusal::None;
        }
        case Press::Interact:
        {
            if (go)
            {
                ObjectFacts facts;
                facts.Type = go->GetGoType();
                facts.Selectable = !go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
                facts.Reach = go->GetInteractionDistance();
                facts.Distance = go->IsWithinDistInMap(bot) ? 0.0f : facts.Reach + 1.0f;
                facts.Locked = LockedToHand(go->GetGoType(), go->GetGOInfo()->GetLockId());
                if (Refusal const refusal = JudgeObjectUse(facts); refusal != Refusal::None)
                    return refuse(refusal);
                WorldPacket packet = GameObjectUse(go->GetGUID());
                port.Send(bot, packet);
                ++result.Interactions;
                return Refusal::None;
            }
            if (!unit)
                return refuse(Refusal::Kind);
            UnitFacts facts;
            facts.Player = unit->IsPlayer();
            facts.Dead = !unit->IsAlive();
            facts.Hostile = bot->IsHostileTo(unit);
            facts.NpcFlags = unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE;
            facts.Reach = INTERACTION_DISTANCE;
            facts.Distance = bot->IsWithinDistInMap(unit, INTERACTION_DISTANCE) ? 0.0f : INTERACTION_DISTANCE + 1.0f;
            if (Refusal const refusal = JudgeUnitInteract(facts); refusal != Refusal::None)
                return refuse(refusal);
            WorldPacket packet = GossipHello(unit->GetGUID());
            port.Send(bot, packet);
            ++result.Interactions;
            return Refusal::None;
        }
        case Press::UseItem:
        {
            // Nothing that opens loot: a chest's lock, a node, a corpse.
            if ((go && OpensLoot(go->GetGoType())) || (unit && !unit->IsAlive()))
                return refuse(Refusal::Loot);
            SpellInfo const* spell = nullptr;
            Item* item = KeyItemFor(bot, object, spell);
            if (!item || !spell)
                return refuse(Refusal::NoItem);
            SpellCastTargets targets;
            if (go)
                targets.SetGOTarget(go);
            else
                targets.SetUnitTarget(unit);
            WorldPacket packet = UseItem(item->GetBagSlot(), item->GetSlot(), item->GetGUID(), spell->Id,
                NextCastCount(), targets);
            Movement::ScopedCastWatch watch(bot->GetSession());
            port.Send(bot, packet);
            if (watch.Watch().Failures)
                return refuse(CastRefusal(uint32(watch.Watch().Result) + 1));
            ++result.ItemUses;
            ++result.Interactions;
            return Refusal::None;
        }
        case Press::Count:
            break;
    }
    return refuse(Refusal::Kind);
}
