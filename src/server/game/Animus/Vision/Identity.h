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

#ifndef ANIMUS_VISION_IDENTITY_H
#define ANIMUS_VISION_IDENTITY_H

#include "Camera.h"
#include <cstdint>

/// **What a thing in view is, to this seat** (perception-goals 1a and 1b): the facts a player's client shows about a
/// unit or a game object -- its nameplate's colour, the quest mark over it, the sparkle on a corpse or a chest, the
/// cursor over it, its tooltip's quest line -- and the semantic class and the entity list's flags that follow from
/// them. Pure: MapVisionWorld reads the facts off the core (FactsOf), the tests write them by hand.
///
/// **The UI rule** (perception-goals amendment 7): every fact that depends on the character is this seat's own -- a
/// quest giver's mark only when the seat's client would draw one for it, a quest object only while one of its quests
/// needs it, a corpse lootable only when this seat may loot it. Nothing about what other players see leaks in.
namespace Animus::Vision
{
    struct EntityFacts
    {
        bool GameObject = false;

        // A unit's.
        bool Player = false;
        bool Dead = false;
        /// A dead creature with loot this seat may take: the client's sparkle (UNIT_DYNFLAG_LOOTABLE as the core sends
        /// it to this seat).
        bool LootableByMe = false;
        /// -1 hostile to the seat, 0 neutral, 1 friendly: the nameplate's colour.
        int8_t Reaction = 0;
        /// A creature that sells, or trains (its npc flags).
        bool Vendor = false;
        bool Trainer = false;
        /// A creature the seat can talk to or use (any npc flag), when not hostile.
        bool Interactive = false;
        /// A creature this seat's quests need killed (its tooltip's objective line).
        bool QuestTarget = false;

        // Either's.
        /// A quest mark this seat's client draws over it: a yellow or blue "!" or a "?" (QuestGiverStatus for this
        /// player from DIALOG_STATUS_INCOMPLETE up; the low-level and unavailable marks the client hides by default
        /// are not marks).
        bool QuestMark = false;

        // A game object's.
        /// Its GameobjectTypes.
        uint32_t ObjectType = 0;
        /// The skill its lock asks for (LockType: 2 herbalism, 3 mining), 0 for none.
        uint32_t LockSkill = 0;
        /// One of this seat's quests needs it (GameObject::ActivateToQuest for this player: the client's glow).
        bool QuestRelevant = false;
        /// The seat's cursor turns to a cog over it: a kind that can be used, selectable, its use not withheld.
        bool Usable = false;
        /// A chest whose loot is there to take.
        bool Lootable = false;
    };

    /// The class and the entity list's flags of one entity.
    struct Identity
    {
        Class What = Class::OtherObject;
        bool Quest = false;
        bool Lootable = false;
        bool Usable = false;
    };

    /// The class, by these rules, first match wins:
    /// - a unit: dead -> lootable corpse (LootableByMe) or corpse; a player -> hostile or friendly player; hostile
    ///   -> hostile creature; a quest mark -> quest giver; vendor; trainer; neutral or friendly creature;
    /// - a game object: a door or button -> door; mailbox; a quest mark -> quest giver; a chest locked by
    ///   herbalism -> herb, by mining -> ore; quest relevant -> quest object; a chest -> chest; usable -> usable
    ///   object; else other object.
    /// Quest is a quest mark, a quest target or quest relevance; Lootable a corpse's sparkle or a chest's loot;
    /// Usable an interactive unit not hostile, or a usable game object.
    [[nodiscard]] Identity Classify(EntityFacts const& facts);

    /// An entity of a frame, as the entity list reads it (perception-goals 1b): what it is to the seat, its template
    /// (a creature's or game object's entry; 0 for a player), its level and health (a game object's 0 and 1), how its
    /// nameplate reads, and its middle (the list's distance and direction are to it).
    struct EntityInfo
    {
        Identity Id;
        uint32_t Entry = 0;
        bool GameObject = false;
        float Level = 0.0f;
        float Health = 1.0f;
        int8_t Reaction = 0;
        Vec3 Centre;
    };
}

#endif
