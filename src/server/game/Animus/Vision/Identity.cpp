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

#include "Identity.h"
#include "SharedDefines.h"

namespace
{
    using namespace Animus::Vision;

    Class UnitClass(EntityFacts const& facts)
    {
        if (facts.Dead)
            return facts.LootableByMe ? Class::LootableCorpse : Class::Corpse;
        if (facts.Player)
            return facts.Reaction < 0 ? Class::HostilePlayer : Class::FriendlyPlayer;
        if (facts.Reaction < 0)
            return Class::HostileCreature;
        if (facts.QuestMark)
            return Class::QuestGiver;
        if (facts.Vendor)
            return Class::Vendor;
        if (facts.Trainer)
            return Class::Trainer;
        return facts.Reaction > 0 ? Class::FriendlyCreature : Class::NeutralCreature;
    }

    Class ObjectClass(EntityFacts const& facts)
    {
        switch (facts.ObjectType)
        {
            case GAMEOBJECT_TYPE_DOOR:
            case GAMEOBJECT_TYPE_BUTTON:
                return Class::Door;
            case GAMEOBJECT_TYPE_MAILBOX:
                return Class::Mailbox;
            default:
                break;
        }
        if (facts.QuestMark)
            return Class::QuestGiver;
        if (facts.ObjectType == GAMEOBJECT_TYPE_CHEST && facts.LockSkill == LOCKTYPE_HERBALISM)
            return Class::Herb;
        if (facts.ObjectType == GAMEOBJECT_TYPE_CHEST && facts.LockSkill == LOCKTYPE_MINING)
            return Class::Ore;
        if (facts.QuestRelevant)
            return Class::QuestObject;
        if (facts.ObjectType == GAMEOBJECT_TYPE_CHEST)
            return Class::Chest;
        return facts.Usable ? Class::UsableObject : Class::OtherObject;
    }
}

Animus::Vision::Identity Animus::Vision::Classify(EntityFacts const& facts)
{
    Identity identity;
    if (facts.GameObject)
    {
        identity.What = ObjectClass(facts);
        identity.Quest = facts.QuestMark || facts.QuestRelevant;
        identity.Lootable = facts.Lootable;
        identity.Usable = facts.Usable;
        return identity;
    }
    identity.What = UnitClass(facts);
    identity.Quest = !facts.Dead && (facts.QuestMark || facts.QuestTarget);
    identity.Lootable = facts.Dead && facts.LootableByMe;
    identity.Usable = !facts.Dead && facts.Interactive && facts.Reaction >= 0;
    return identity;
}
