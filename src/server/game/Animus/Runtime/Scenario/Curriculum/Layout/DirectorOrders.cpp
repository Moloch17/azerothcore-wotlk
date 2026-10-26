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

#include "DirectorOrders.h"
#include <algorithm>

Animus::Curriculum::DirectorRules::Call Animus::Curriculum::DirectorRules::Decode(int32 action)
{
    using namespace DirectorLayout;
    Call call;
    if (action < 0 || action >= int32(ACTION_COUNT))
        return call;
    uint32 const local = uint32(action);
    auto const in = [local](uint32 first, uint32 count, uint32& index)
    {
        if (local < first || local >= first + count)
            return false;
        index = local - first;
        return true;
    };
    uint32 index = 0;
    if (local == ACTION_HOLD)
        call.Kind = Call::Hold;
    else if (in(ACTION_POSTURE_FIRST, TEAM_POSTURE_COUNT, index))
    {
        call.Kind = Call::Posture;
        call.Stance = TeamPosture(index);
    }
    else if (in(ACTION_ADDRESS_FIRST, DIRECTOR_SEATS, index))
    {
        call.Kind = Call::Address;
        call.Member = index;
    }
    else if (local == ACTION_CLEAR)
        call.Kind = Call::Clear;
    else if (local == ACTION_REST)
    {
        call.Kind = Call::Order;
        call.Verb = OrderKind::Rest;
    }
    else
    {
        call.Kind = Call::Order;
        if (in(ACTION_FOCUS_FIRST, DIRECTOR_ENEMIES, index))
            call.Verb = OrderKind::Focus;
        else if (in(ACTION_PULL_FIRST, DIRECTOR_ENEMIES, index))
            call.Verb = OrderKind::Pull;
        else if (in(ACTION_HEAL_FIRST, DIRECTOR_SEATS, index))
            call.Verb = OrderKind::Heal;
        else if (in(ACTION_FOLLOW_FIRST, DIRECTOR_SEATS, index))
            call.Verb = OrderKind::Follow;
        else if (in(ACTION_LOOT_FIRST, DIRECTOR_OBJECTS, index))
            call.Verb = OrderKind::Loot;
        else if (in(ACTION_USE_FIRST, DIRECTOR_OBJECTS, index))
            call.Verb = OrderKind::Use;
        else
            call.Kind = Call::None;
        call.Slot = index;
    }
    return call;
}

void Animus::Curriculum::DirectorRules::PrepareTurn(DirectorOrders& orders, uint32 step, bool const* alive,
    bool const* low, uint32 count, uint32 enemiesOnParty, uint32 clockDecisions, uint32 calls)
{
    bool event = orders.Event;
    orders.Event = false;
    for (uint32 slot = 0; slot < count && slot < orders.WasAlive.size(); ++slot)
    {
        event = event || (orders.WasAlive[slot] && !alive[slot]) || (low[slot] && !orders.WasLow[slot]);
        orders.WasAlive[slot] = alive[slot] ? 1 : 0;
        orders.WasLow[slot] = low[slot] ? 1 : 0;
    }
    event = event || enemiesOnParty > orders.EnemiesOnParty;
    orders.EnemiesOnParty = enemiesOnParty;

    if (orders.CallsLeft)
        return;
    bool const clock = step >= orders.NextClock;
    if (!clock && !event)
        return;
    orders.CallsLeft = std::max<uint32>(1, calls);
    orders.ByEvent = !clock;
    orders.NextClock = step + std::max<uint32>(1, clockDecisions);
    ++orders.Turns;
}

bool Animus::Curriculum::DirectorRules::Spend(DirectorOrders& orders, bool hold)
{
    if (!orders.CallsLeft)
        return false;
    if (hold)
    {
        orders.CallsLeft = 0;
        return false;
    }
    --orders.CallsLeft;
    ++orders.Calls;
    return true;
}

bool Animus::Curriculum::DirectorRules::Order(DirectorOrders& orders, uint32 step, uint32 ms, uint32 member,
    OrderKind kind, ObjectGuid target, uint32 holdSteps)
{
    if (member >= orders.Members.size())
        return false;
    DirectorOrders::MemberOrder& order = orders.Members[member];
    bool const replaced = order.Kind != OrderKind::None;
    if (replaced)
    {
        ++orders.Replaced;
        if (step < order.IssuedStep + holdSteps)
            ++orders.Churned;
    }
    order = DirectorOrders::MemberOrder();
    order.Kind = kind;
    order.Target = target;
    order.IssuedMs = ms;
    order.IssuedStep = step;
    if (kind != OrderKind::None)
    {
        ++orders.Orders;
        ++orders.Given[uint32(kind)];
    }
    orders.LastCallMs = ms;
    return replaced;
}
