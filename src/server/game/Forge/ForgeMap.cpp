/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
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

/*
 * Map::ForgeSendObjectUpdates -- object updates without packets.
 *
 * Stock Map::SendObjectUpdates walks every object whose update fields changed this tick and, for
 * each one, builds a values-update block for itself and for every player that can see it, then
 * assembles and sends one packet per player. The sim host has no listener, so no session ever has
 * a socket and every one of those blocks is thrown away inside WorldSession::SendPacket. With bots
 * all visible to each other and health/power changing on every regen tick, that is roughly
 * changed objects x visible players of wasted work per tick.
 *
 * The only state the builders change is the object's update bookkeeping: every BuildUpdate
 * override (WorldObject, Item, MotionTransport, StaticTransport) ends in ClearUpdateMask. So this
 * drains the queue and clears each mask directly, which keeps field-change tracking correct --
 * the next change re-queues the object exactly as before -- without building anything.
 *
 * Other packet builders skipped for the same reason carry a "Forge: no client sockets" comment.
 */

#include "Map.h"
#include "Object.h"

void Map::ForgeSendObjectUpdates()
{
    while (!_updateObjects.empty())
    {
        Object* obj = *_updateObjects.begin();
        ASSERT(obj->IsInWorld());

        _updateObjects.erase(_updateObjects.begin());
        obj->ClearUpdateMask(false);
    }
}
