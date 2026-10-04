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
#include "Unit.h"
#include <algorithm>

void Animus::Curriculum::DirectorRules::PrepareTurn(DirectorOrders& orders, uint32 step, Unit* const* members,
    uint32 memberCount, Unit* const* enemies, uint32 enemyCount, uint32 clockDecisions, float lowHealth)
{
    bool event = false;
    for (uint32 slot = 0; slot < memberCount && slot < orders.WasAlive.size(); ++slot)
    {
        Unit const* member = members[slot];
        bool const alive = member && member->IsAlive();
        bool const low = alive && member->GetHealthPct() / 100.0f < lowHealth;
        event = event || (orders.WasAlive[slot] && !alive) || (low && !orders.WasLow[slot]);
        orders.WasAlive[slot] = alive ? 1 : 0;
        orders.WasLow[slot] = low ? 1 : 0;
    }
    uint32 living = 0;
    bool focusAlive = false;
    for (uint32 slot = 0; slot < enemyCount; ++slot)
        if (enemies[slot] && enemies[slot]->IsAlive())
        {
            ++living;
            focusAlive = focusAlive || enemies[slot]->GetGUID() == orders.Focus;
        }
    event = event || living > orders.EnemiesAlive || (orders.FocusWasAlive && !focusAlive);
    orders.EnemiesAlive = living;
    orders.FocusWasAlive = focusAlive;

    if (orders.CallsLeft)
        return;
    bool const clock = step >= orders.NextClock;
    if (!clock && !event)
        return;
    orders.CallsLeft = memberCount > GROUP_SEATS ? DirectorLayout::RAID_CALLS : DirectorLayout::GROUP_CALLS;
    orders.ByEvent = !clock;
    orders.NextClock = step + std::max<uint32>(1, clockDecisions);
    ++orders.Turns;
}

bool Animus::Curriculum::DirectorRules::Addressed(DirectorOrders const& orders, uint32 slot)
{
    switch (orders.Address)
    {
        case OrderSource::Side:   return true;
        case OrderSource::Group:  return slot / GROUP_SEATS == orders.AddressGroup;
        case OrderSource::Member: return slot == orders.AddressMember;
        default:                  return false;
    }
}

void Animus::Curriculum::DirectorRules::Order(DirectorOrders& orders, uint32 step, uint32 memberCount,
    OrderKind kind, ObjectGuid target, uint32 objective)
{
    for (uint32 slot = 0; slot < memberCount && slot < orders.Members.size(); ++slot)
    {
        if (!Addressed(orders, slot))
            continue;
        DirectorOrders::MemberOrder& member = orders.Members[slot];
        if (member.Kind != OrderKind::None && member.Source > orders.Address)
            continue;
        if (member.Kind != OrderKind::None)
        {
            ++orders.Replaced;
            if (member.Source == orders.Address && step < member.IssuedStep + orders.HoldSteps)
                ++orders.Churned;
        }
        member.Kind = kind;
        member.Target = target;
        member.Objective = objective;
        member.Source = orders.Address;
        member.IssuedStep = step;
        ++orders.MemberOrders;
    }
    orders.Changed(step);
}

void Animus::Curriculum::DirectorRules::Apply(DirectorOrders& orders, uint32 step, int32 action,
    Unit* const* members, uint32 memberCount, ObjectGuid const* callable, uint32 enemyCount, bool turns)
{
    using namespace DirectorLayout;
    if (action < 0)
        return;
    if (turns)
    {
        if (!orders.CallsLeft)
            return;
        if (action == int32(ACTION_HOLD))
        {
            orders.CallsLeft = 0;
            return;
        }
        --orders.CallsLeft;
        ++orders.Calls;
    }
    else if (action == int32(ACTION_HOLD))
        return;

    uint32 const local = uint32(action);
    auto const set = [&](auto& field, auto value)
    {
        if (field != value)
        {
            field = value;
            orders.Changed(step);
        }
    };

    if (local < ACTION_RALLY_FIRST)
        return set(orders.Posture, TeamPosture(local - ACTION_POSTURE_FIRST));
    if (local < ACTION_ANCHOR_FIRST)
        return set(orders.Rally, TeamRally(local - ACTION_RALLY_FIRST));
    if (local < ACTION_OFFSET_FIRST)
        return set(orders.Anchor, PlaceAnchor(local - ACTION_ANCHOR_FIRST));
    if (local < ACTION_RING_FIRST)
        return set(orders.Offset, PlaceOffset(local - ACTION_OFFSET_FIRST));
    if (local < ACTION_ADDRESS_SIDE)
        return set(orders.Ring, PlaceRing(local - ACTION_RING_FIRST));

    if (local == ACTION_ADDRESS_SIDE)
    {
        orders.Address = OrderSource::Side;
        return;
    }
    if (local < ACTION_ADDRESS_MEMBER_FIRST)
    {
        orders.Address = OrderSource::Group;
        orders.AddressGroup = local - ACTION_ADDRESS_GROUP_FIRST;
        return;
    }
    if (local < ACTION_FOCUS_FIRST)
    {
        uint32 const slot = local - ACTION_ADDRESS_MEMBER_FIRST;
        if (slot < memberCount)
        {
            orders.Address = OrderSource::Member;
            orders.AddressMember = slot;
        }
        return;
    }

    auto const enemyAt = [&](uint32 slot) { return slot < enemyCount ? callable[slot] : ObjectGuid::Empty; };
    if (local < ACTION_TANK_FIRST)
    {
        ObjectGuid const enemy = enemyAt(local - ACTION_FOCUS_FIRST);
        if (!enemy)
            return;
        if (orders.Address == OrderSource::Side)
            return set(orders.Focus, enemy);
        return Order(orders, step, memberCount, OrderKind::Focus, enemy, 0);
    }
    if (local < ACTION_HEAL_FIRST)
    {
        uint32 const family = (local - ACTION_TANK_FIRST) / NAMED_ENEMY_SLOTS;
        OrderKind const kind = family == 0 ? OrderKind::Tank : family == 1 ? OrderKind::Interrupt : OrderKind::Control;
        ObjectGuid const enemy = enemyAt((local - ACTION_TANK_FIRST) % NAMED_ENEMY_SLOTS);
        if (enemy && orders.Address != OrderSource::Side)
            Order(orders, step, memberCount, kind, enemy, 0);
        return;
    }
    if (local < ACTION_GO_TO)
    {
        uint32 const slot = local - ACTION_HEAL_FIRST;
        Unit const* member = slot < memberCount ? members[slot] : nullptr;
        if (member && member->IsAlive() && orders.Address != OrderSource::Side)
            Order(orders, step, memberCount, OrderKind::Heal, member->GetGUID(), 0);
        return;
    }
    if (local == ACTION_GO_TO)
        return Order(orders, step, memberCount, OrderKind::GoTo, ObjectGuid::Empty, 0);
    if (local < ACTION_COUNT)
        Order(orders, step, memberCount, OrderKind::Objective, ObjectGuid::Empty, local - ACTION_OBJECTIVE_FIRST);
}
