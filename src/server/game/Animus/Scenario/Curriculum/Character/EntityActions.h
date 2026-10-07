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

#ifndef ANIMUS_LIB_CURRICULUM_ENTITY_ACTIONS_H
#define ANIMUS_LIB_CURRICULUM_ENTITY_ACTIONS_H

#include "ObjectGuid.h"
#include "WorldPacket.h"
#include <functional>

class Item;
class Player;
class SpellCastTargets;
class SpellInfo;
class WorldObject;

namespace Animus::Curriculum
{
    struct SeatActionResult;
}

/// **Acting on what the seat sees** (dungeon-curriculum I1): a press on an entity of the sight block's list -- one the
/// seat sees now or remembers -- sent as the client sends it, as a packet through the seat's session's own handler:
/// - **select**: CMSG_SET_SELECTION (HandleSetSelectionOpcode), and, while swinging, CMSG_ATTACKSWING at a hostile
///   new target, as the client switches its auto attack;
/// - **interact**: a game object, CMSG_GAMEOBJ_USE (HandleGameObjectUseOpcode); a creature, CMSG_GOSSIP_HELLO
///   (HandleGossipHelloOpcode, the interaction every npc flag opens with);
/// - **use an item on it**: the key item the seat carries for it -- an item whose use spell takes a game object, or a
///   unit, as the entity is -- CMSG_USE_ITEM with the entity as the cast's target (HandleUseItemOpcode);
/// - **assist**: CMSG_SET_SELECTION to the unit's own target, as /assist reads it off the unit's target field;
/// - **focus**: the client's focus (a client-side unit, no packet): the friend beneficial spells go to.
/// A spell goes as CMSG_CAST_SPELL (HandleCastSpellOpcode, CastThroughClient) at the selection, or the focus.
///
/// **What is judged before the packet**, as the client knows it: the entity is still there to it (AtClient: on its
/// map, in the world, detectable, within the visibility distance -- a remembered entity may not be), the kind of
/// thing the press takes, and the handler's own interaction distance, which the handler returns on silently. A press
/// refused for any of these sends nothing and is priced (StageScenario::JudgePress, Actions.Aimless.ActRefused): an
/// action on a remembered entity is a legal press that the world judges, never masked. A cast refused by the
/// server's own checks comes back through the cast watch (CastWatch) and is priced the same.
///
/// **No looting** (the user, 2026-10-06): there is no loot press, no loot window is ever opened, and nothing that
/// opens one is sent -- a chest, a herb or ore node, a fishing node or hole, or a corpse is refused before any packet
/// (Refusal::Loot), interact and item use alike.
namespace Animus::Curriculum::EntityActions
{
    enum class Press : uint8
    {
        Select,
        Interact,
        UseItem,
        Assist,
        Focus,
        Count
    };

    /// Why a press did nothing. None: it was sent (or, a focus, taken).
    enum class Refusal : uint8
    {
        None,
        Gone,           // not there to the client: despawned, another map, out of its sight, undetectable
        Kind,           // not a thing this press takes (select a game object, gossip with a hostile or a player)
        Reach,          // beyond the interaction distance, or the cast's range
        Sight,          // the cast's line of sight
        Loot,           // a press that would open a loot window
        NoItem,         // no item the seat carries is used on such a thing
        NoTarget,       // assist: the unit has no target, or one the client does not have
        Cast,           // the cast was refused otherwise (its SpellCastResult)
        /// A door, button or goober with a lock (a door its lever opens, the Deadmines' cannon that takes the
        /// gunpowder): the client sends no use for a locked thing, it shows the lock (M3 interact).
        Locked,
        Count
    };
    constexpr uint32 REFUSALS = uint32(Refusal::Count);
    [[nodiscard]] char const* RefusalName(Refusal refusal);

    /// Where a press's packet goes: the seat's session's handler (SessionPort), or a test's recorder.
    class ClientPort
    {
    public:
        virtual ~ClientPort() = default;
        virtual void Send(Player* bot, WorldPacket& packet) = 0;
    };
    /// The session's own handlers, by opcode (the opcodes the presses send; any other is dropped).
    [[nodiscard]] ClientPort& SessionPort();

    /// Finds the object a press names, as the seat's client has it; null when it does not (AtClient).
    using Resolver = std::function<WorldObject*(Player* bot, ObjectGuid guid)>;
    [[nodiscard]] WorldObject* ResolveAtClient(Player* bot, ObjectGuid guid);
    /// Whether the seat's client has `object`: on its map, in the world, detectable, within the visibility distance.
    [[nodiscard]] bool AtClient(Player* bot, WorldObject const* object);

    // The packets, as the 3.3.5a client writes them.
    [[nodiscard]] WorldPacket SetSelection(ObjectGuid guid);
    [[nodiscard]] WorldPacket AttackSwing(ObjectGuid guid);
    [[nodiscard]] WorldPacket GameObjectUse(ObjectGuid guid);
    [[nodiscard]] WorldPacket GossipHello(ObjectGuid guid);
    [[nodiscard]] WorldPacket UseItem(uint8 bag, uint8 slot, ObjectGuid item, uint32 spellId, uint8 castCount,
        SpellCastTargets& targets);
    [[nodiscard]] WorldPacket CastSpell(uint32 spellId, uint8 castCount, SpellCastTargets& targets);

    /// Game object kinds whose use opens a loot window (a chest -- herbs and ore are chests -- a fishing node or hole).
    [[nodiscard]] bool OpensLoot(uint32 goType);

    /// A game object's use, judged from what the client knows of it.
    struct ObjectFacts
    {
        uint32 Type = 0;
        bool Selectable = true;     // no GO_FLAG_NOT_SELECTABLE
        float Distance = 0.0f;      // from the seat, as IsWithinDistInMap measures it
        float Reach = 0.0f;         // GameObject::GetInteractionDistance
        bool Locked = false;        // a door, button or goober whose template names a lock (LockedToHand)
    };
    /// Whether a game object of `goType` with lock `lockId` is shut to a hand on it: a door, a button or a goober
    /// with any lock. Its lever, or the item its lock takes, opens it; a chest's lock is the loot refusal's.
    [[nodiscard]] bool LockedToHand(uint32 goType, uint32 lockId);
    [[nodiscard]] Refusal JudgeObjectUse(ObjectFacts const& facts);

    /// A creature's (or player's) interaction, judged from what the client knows of it (GetNPCIfCanInteractWith).
    struct UnitFacts
    {
        bool Player = false;
        bool Dead = false;
        bool Hostile = false;
        bool NpcFlags = false;
        float Distance = 0.0f;
        float Reach = 0.0f;         // INTERACTION_DISTANCE
    };
    [[nodiscard]] Refusal JudgeUnitInteract(UnitFacts const& facts);

    /// The item the seat would use on `object`: one in its bags, inventory or keyring whose use spell takes a game
    /// object (for a game object) or a unit (for a unit); null when it carries none. Its use spell into `spell`.
    [[nodiscard]] Item* KeyItemFor(Player* bot, WorldObject const* object, SpellInfo const*& spell);

    /// What a cast sent as the client sends it came to.
    struct CastOutcome
    {
        bool Sent = false;          // through the handler: started, or queued for the end of the cooldown
        bool Queued = false;        // ... queued (the client's spell queue)
        uint32 Failed = 0;          // a refusal's SpellCastResult + 1; 0 none
    };
    /// CMSG_CAST_SPELL for `info` at `targets` through the seat's session's handler, its refusal read back.
    CastOutcome CastThroughClient(Player* bot, SpellInfo const* info, SpellCastTargets& targets,
        ClientPort& port = SessionPort());
    /// CMSG_USE_ITEM for `item` (its use spell `spell`) at `targets` through the seat's session's handler, its refusal
    /// read back: eating, drinking, a key on a lock -- an item used as the client uses it.
    CastOutcome UseItemThroughClient(Player* bot, Item* item, SpellInfo const* spell, SpellCastTargets& targets,
        ClientPort& port = SessionPort());
    /// CMSG_ATTACKSWING at `target` through the seat's session's handler: the auto attack, started as the client
    /// starts it. False when there was nothing to send it through.
    bool StartAttackThroughClient(Player* bot, ObjectGuid target, ClientPort& port = SessionPort());
    /// The pet bar's Attack (CMSG_PET_ACTION, COMMAND_ATTACK) at `target` for the seat's pet, through the session's
    /// handler -- which takes it for the first controlled unit only, as the client's pet bar is. False with no pet.
    bool PetAttackThroughClient(Player* bot, ObjectGuid target, ClientPort& port = SessionPort());
    [[nodiscard]] WorldPacket PetAction(ObjectGuid pet, uint32 data, ObjectGuid target);

    /// **A press** on the entity `guid` (raw): judged as above, then sent. `focus` is the seat's client focus (Focus
    /// sets it). Result: Interactions, Selections and ItemUses for what was sent, ActRefused for a refusal; ActedOn
    /// and ActPress the entity and the press, sent or refused, once it was found.
    Refusal Apply(Press press, Player* bot, uint64 guid, ObjectGuid& focus, SeatActionResult& result,
        ClientPort& port = SessionPort(), Resolver const& resolve = ResolveAtClient);
}

#endif
