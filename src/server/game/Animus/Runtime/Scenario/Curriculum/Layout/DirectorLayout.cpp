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

#include "DirectorLayout.h"
#include "StringFormat.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;

    char const* ObjectKindName(uint32 kind)
    {
        switch (kind)
        {
            case DirectorLayout::OBJECT_DOOR:       return "door";
            case DirectorLayout::OBJECT_LEVER:      return "lever";
            case DirectorLayout::OBJECT_GOOBER:     return "goober";
            case DirectorLayout::OBJECT_KEY_CHEST:  return "key_chest";
            default:                                return "unknown";
        }
    }

    std::string SeatFeatureName(uint32 feature)
    {
        using namespace DirectorLayout;
        if (feature >= SEAT_APTITUDE_FIRST && feature < SEAT_APTITUDE_FIRST + Aptitude::BRIEF_COUNT)
            return Acore::StringFormat("apt{}", feature - SEAT_APTITUDE_FIRST);
        if (feature >= SEAT_ROLE_FIRST && feature < SEAT_ROLE_FIRST + 3)
        {
            static char const* const roles[] = { "tank", "healer", "damage" };
            return roles[feature - SEAT_ROLE_FIRST];
        }
        if (feature >= SEAT_ORDER_FIRST && feature < SEAT_ORDER_FIRST + ORDER_KIND_COUNT)
            return Acore::StringFormat("order_{}", OrderKindName(OrderKind(feature - SEAT_ORDER_FIRST)));
        if (feature >= SEAT_GOAL_FIRST && feature < SEAT_GOAL_FIRST + GOAL_COUNT)
            return Acore::StringFormat("goal_{}", GoalName(SeatGoal(feature - SEAT_GOAL_FIRST)));
        switch (feature)
        {
            case SEAT_PRESENT:      return "present";
            case SEAT_ALIVE:        return "alive";
            case SEAT_HEALTH:       return "health";
            case SEAT_POWER:        return "power";
            case SEAT_IN_COMBAT:    return "in_combat";
            case SEAT_CASTING:      return "casting";
            case SEAT_MOVING:       return "moving";
            case SEAT_SPREAD:       return "spread";
            case SEAT_BEARING_SIN:  return "bearing_sin";
            case SEAT_BEARING_COS:  return "bearing_cos";
            case SEAT_LEADER:       return "leader";
            case SEAT_CARRIES_KEY:  return "carries_key";
            case SEAT_ADDRESSED:    return "addressed";
            case SEAT_ATTACKED:     return "attacked";
            case SEAT_ORDER_AGE:    return "order_age";
            case SEAT_ORDER_NEAR:   return "order_near";
            case SEAT_ORDER_DONE:   return "order_done";
            default:                return "unknown";
        }
    }

    char const* EnemyFeatureName(uint32 feature)
    {
        using namespace DirectorLayout;
        switch (feature)
        {
            case ENEMY_PRESENT:     return "present";
            case ENEMY_ALIVE:       return "alive";
            case ENEMY_HEALTH:      return "health";
            case ENEMY_IN_COMBAT:   return "in_combat";
            case ENEMY_CASTING:     return "casting";
            case ENEMY_ELITE:       return "elite";
            case ENEMY_BOSS:        return "boss";
            case ENEMY_LEVEL:       return "level";
            case ENEMY_DISTANCE:    return "distance";
            case ENEMY_BEARING_SIN: return "bearing_sin";
            case ENEMY_BEARING_COS: return "bearing_cos";
            case ENEMY_ON_PARTY:    return "on_party";
            case ENEMY_ORDERED:     return "ordered";
            default:                return "unknown";
        }
    }

    std::string ObjectFeatureName(uint32 feature)
    {
        using namespace DirectorLayout;
        if (feature >= OBJECT_KIND_FIRST && feature < OBJECT_KIND_FIRST + OBJECT_KINDS)
            return Acore::StringFormat("kind_{}", ObjectKindName(feature - OBJECT_KIND_FIRST));
        switch (feature)
        {
            case OBJECT_PRESENT:        return "present";
            case OBJECT_OPEN:           return "open";
            case OBJECT_NEEDS_KEY:      return "needs_key";
            case OBJECT_KEY_HELD:       return "key_held";
            case OBJECT_DISTANCE:       return "distance";
            case OBJECT_BEARING_SIN:    return "bearing_sin";
            case OBJECT_BEARING_COS:    return "bearing_cos";
            case OBJECT_IS_GATE:        return "is_gate";
            case OBJECT_ORDERED:        return "ordered";
            default:                    return "unknown";
        }
    }
}

void Animus::Curriculum::DirectorLayout::Observe(DirectorView const& view, float* obs, uint8* mask)
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    if (mask)
    {
        std::fill(mask, mask + ACTION_COUNT, uint8(0));
        // Holding is always allowed: a director with nothing to add says nothing.
        mask[ACTION_HOLD] = 1;
    }
    if (!view.Active)
        return;

    obs[OBS_ACTIVE] = 1.0f;
    obs[OBS_MAY_CALL] = view.MayCall ? 1.0f : 0.0f;
    obs[OBS_CALLS_LEFT] = float(view.CallsLeft) / float(std::max<uint32>(1, view.CallsBudget));
    obs[OBS_BY_EVENT] = view.ByEvent ? 1.0f : 0.0f;
    obs[OBS_EPISODE_TIME] = view.EpisodeTime;
    obs[OBS_PARTY_ALIVE] = view.PartyAlive;
    obs[OBS_PARTY_HEALTH] = view.PartyHealth;
    obs[OBS_PARTY_POWER] = view.PartyPower;
    obs[OBS_IN_COMBAT] = view.InCombat ? 1.0f : 0.0f;
    obs[OBS_ENEMIES_ALIVE] = float(view.EnemiesAlive) / float(DIRECTOR_ENEMIES);
    obs[OBS_ENEMIES_ON_PARTY] = float(view.EnemiesOnParty) / float(DIRECTOR_ENEMIES);
    obs[OBS_SINCE_CALL] = std::min(1.0f, view.SinceCall / ORDER_AGE_SCALE);
    obs[OBS_ORDERS_STANDING] = float(view.OrdersStanding) / float(DIRECTOR_SEATS);
    obs[OBS_KEY_HELD] = view.KeyHeld ? 1.0f : 0.0f;
    obs[OBS_GATE_AHEAD] = view.GateAhead ? 1.0f : 0.0f;
    obs[OBS_POSTURE_FIRST + std::min<uint32>(uint32(view.Posture), TEAM_POSTURE_COUNT - 1)] = 1.0f;
    if (view.Address < DIRECTOR_SEATS)
        obs[OBS_ADDRESS_FIRST + view.Address] = 1.0f;

    for (uint32 slot = 0; slot < DIRECTOR_SEATS; ++slot)
    {
        DirectorView::SeatSlot const& seat = view.Seats[slot];
        float* out = obs + OBS_SEAT_FIRST + slot * SEAT_FEATURES;
        if (!seat.Present)
            continue;
        out[SEAT_PRESENT] = 1.0f;
        out[SEAT_ALIVE] = seat.Alive ? 1.0f : 0.0f;
        out[SEAT_HEALTH] = seat.Health;
        out[SEAT_POWER] = seat.Power;
        seat.Apt.WriteBrief(out + SEAT_APTITUDE_FIRST);
        out[SEAT_IN_COMBAT] = seat.InCombat ? 1.0f : 0.0f;
        out[SEAT_CASTING] = seat.Casting ? 1.0f : 0.0f;
        out[SEAT_MOVING] = seat.Moving ? 1.0f : 0.0f;
        out[SEAT_SPREAD] = std::min(1.0f, seat.Spread / DISTANCE_SCALE);
        out[SEAT_BEARING_SIN] = seat.BearingSin;
        out[SEAT_BEARING_COS] = seat.BearingCos;
        out[SEAT_LEADER] = seat.Leader ? 1.0f : 0.0f;
        if (seat.Role >= 1 && seat.Role <= 3)
            out[SEAT_ROLE_FIRST + seat.Role - 1] = 1.0f;
        out[SEAT_CARRIES_KEY] = seat.CarriesKey ? 1.0f : 0.0f;
        out[SEAT_ADDRESSED] = seat.Addressed ? 1.0f : 0.0f;
        out[SEAT_ATTACKED] = seat.Attacked;
        out[SEAT_ORDER_FIRST + std::min<uint32>(uint32(seat.Order), ORDER_KIND_COUNT - 1)] = 1.0f;
        out[SEAT_ORDER_AGE] = std::min(1.0f, seat.OrderAge / ORDER_AGE_SCALE);
        out[SEAT_ORDER_NEAR] = seat.OrderNear ? 1.0f : 0.0f;
        out[SEAT_ORDER_DONE] = seat.OrderDone ? 1.0f : 0.0f;
        if (seat.Goal != NO_GOAL)
        {
            int32 const kind = GoalKindOf(seat.Goal);
            if (kind >= 0 && kind < int32(GOAL_COUNT))
                out[SEAT_GOAL_FIRST + uint32(kind)] = 1.0f;
        }
    }

    for (uint32 slot = 0; slot < DIRECTOR_ENEMIES; ++slot)
    {
        DirectorView::EnemySlot const& enemy = view.Enemies[slot];
        float* out = obs + OBS_ENEMY_FIRST + slot * ENEMY_FEATURES;
        if (!enemy.Present)
            continue;
        out[ENEMY_PRESENT] = 1.0f;
        out[ENEMY_ALIVE] = enemy.Alive ? 1.0f : 0.0f;
        out[ENEMY_HEALTH] = enemy.Health;
        out[ENEMY_IN_COMBAT] = enemy.InCombat ? 1.0f : 0.0f;
        out[ENEMY_CASTING] = enemy.Casting ? 1.0f : 0.0f;
        out[ENEMY_ELITE] = enemy.Elite ? 1.0f : 0.0f;
        out[ENEMY_BOSS] = enemy.Boss ? 1.0f : 0.0f;
        out[ENEMY_LEVEL] = std::clamp(enemy.Level, -1.0f, 1.0f);
        out[ENEMY_DISTANCE] = std::min(1.0f, enemy.Distance / DISTANCE_SCALE);
        out[ENEMY_BEARING_SIN] = enemy.BearingSin;
        out[ENEMY_BEARING_COS] = enemy.BearingCos;
        out[ENEMY_ON_PARTY] = enemy.OnParty ? 1.0f : 0.0f;
        out[ENEMY_ORDERED] = enemy.Ordered;
    }

    for (uint32 slot = 0; slot < DIRECTOR_OBJECTS; ++slot)
    {
        DirectorView::ObjectSlot const& object = view.Objects[slot];
        float* out = obs + OBS_OBJECT_FIRST + slot * OBJECT_FEATURES;
        if (!object.Present)
            continue;
        out[OBJECT_PRESENT] = 1.0f;
        out[OBJECT_KIND_FIRST + std::min<uint32>(uint32(object.Kind), OBJECT_KINDS - 1)] = 1.0f;
        out[OBJECT_OPEN] = object.Open ? 1.0f : 0.0f;
        out[OBJECT_NEEDS_KEY] = object.NeedsKey ? 1.0f : 0.0f;
        out[OBJECT_KEY_HELD] = object.KeyHeld ? 1.0f : 0.0f;
        out[OBJECT_DISTANCE] = std::min(1.0f, object.Distance / DISTANCE_SCALE);
        out[OBJECT_BEARING_SIN] = object.BearingSin;
        out[OBJECT_BEARING_COS] = object.BearingCos;
        out[OBJECT_IS_GATE] = object.IsGate ? 1.0f : 0.0f;
        out[OBJECT_ORDERED] = object.Ordered;
    }

    // The mask: on its turn, every field it could edit about something that is there (a slot present); a bad
    // order is priced, never masked (the user: intent, not masks). Off its turn, hold alone.
    if (!mask || !view.MayCall)
        return;
    for (uint32 posture = 0; posture < TEAM_POSTURE_COUNT; ++posture)
        mask[ACTION_POSTURE_FIRST + posture] = 1;
    bool const addressed = view.Address < DIRECTOR_SEATS && view.Seats[view.Address].Present;
    for (uint32 slot = 0; slot < DIRECTOR_SEATS; ++slot)
        if (view.Seats[slot].Present)
        {
            mask[ACTION_ADDRESS_FIRST + slot] = 1;
            mask[ACTION_HEAL_FIRST + slot] = addressed ? 1 : 0;
            mask[ACTION_FOLLOW_FIRST + slot] = addressed ? 1 : 0;
            mask[ACTION_ASSIST_FIRST + slot] = addressed ? 1 : 0;
        }
    mask[ACTION_CLEAR] = addressed ? 1 : 0;
    mask[ACTION_REST] = addressed ? 1 : 0;
    mask[ACTION_STAY] = addressed ? 1 : 0;
    for (uint32 slot = 0; slot < DIRECTOR_ENEMIES; ++slot)
        if (view.Enemies[slot].Present && addressed)
        {
            mask[ACTION_FOCUS_FIRST + slot] = 1;
            mask[ACTION_PULL_FIRST + slot] = 1;
            mask[ACTION_CONTROL_FIRST + slot] = 1;
            mask[ACTION_INTERRUPT_FIRST + slot] = 1;
            mask[ACTION_TAUNT_FIRST + slot] = 1;
        }
    for (uint32 slot = 0; slot < DIRECTOR_OBJECTS; ++slot)
        if (view.Objects[slot].Present && addressed)
        {
            mask[ACTION_LOOT_FIRST + slot] = 1;
            mask[ACTION_USE_FIRST + slot] = 1;
        }
}

std::vector<std::string> Animus::Curriculum::DirectorLayout::ActionNames()
{
    std::vector<std::string> names(ACTION_COUNT);
    names[ACTION_HOLD] = "hold";
    for (uint32 posture = 0; posture < TEAM_POSTURE_COUNT; ++posture)
        names[ACTION_POSTURE_FIRST + posture] = Acore::StringFormat("posture_{}", PostureName(TeamPosture(posture)));
    for (uint32 slot = 0; slot < DIRECTOR_SEATS; ++slot)
    {
        names[ACTION_ADDRESS_FIRST + slot] = Acore::StringFormat("address_member{}", slot);
        names[ACTION_HEAL_FIRST + slot] = Acore::StringFormat("order_heal_member{}", slot);
        names[ACTION_FOLLOW_FIRST + slot] = Acore::StringFormat("order_follow_member{}", slot);
        names[ACTION_ASSIST_FIRST + slot] = Acore::StringFormat("order_assist_member{}", slot);
    }
    names[ACTION_CLEAR] = "order_clear";
    names[ACTION_REST] = "order_rest";
    names[ACTION_STAY] = "order_stay";
    for (uint32 slot = 0; slot < DIRECTOR_ENEMIES; ++slot)
    {
        names[ACTION_FOCUS_FIRST + slot] = Acore::StringFormat("order_focus_enemy{}", slot);
        names[ACTION_PULL_FIRST + slot] = Acore::StringFormat("order_pull_enemy{}", slot);
        names[ACTION_CONTROL_FIRST + slot] = Acore::StringFormat("order_control_enemy{}", slot);
        names[ACTION_INTERRUPT_FIRST + slot] = Acore::StringFormat("order_interrupt_enemy{}", slot);
        names[ACTION_TAUNT_FIRST + slot] = Acore::StringFormat("order_taunt_enemy{}", slot);
    }
    for (uint32 slot = 0; slot < DIRECTOR_OBJECTS; ++slot)
    {
        names[ACTION_LOOT_FIRST + slot] = Acore::StringFormat("order_loot_object{}", slot);
        names[ACTION_USE_FIRST + slot] = Acore::StringFormat("order_use_object{}", slot);
    }
    return names;
}

std::vector<std::string> Animus::Curriculum::DirectorLayout::ColumnNames()
{
    std::vector<std::string> names;
    names.reserve(OBS_COUNT);
    static char const* const globals[] = { "active", "may_call", "calls_left", "by_event", "episode_time",
        "party_alive", "party_health", "party_power", "in_combat", "enemies_alive", "enemies_on_party", "since_call",
        "orders_standing", "key_held", "gate_ahead" };
    for (char const* name : globals)
        names.emplace_back(name);
    for (uint32 posture = 0; posture < TEAM_POSTURE_COUNT; ++posture)
        names.emplace_back(Acore::StringFormat("posture_{}", PostureName(TeamPosture(posture))));
    for (uint32 slot = 0; slot < DIRECTOR_SEATS; ++slot)
        names.emplace_back(Acore::StringFormat("address_member{}", slot));
    for (uint32 slot = 0; slot < DIRECTOR_SEATS; ++slot)
        for (uint32 feature = 0; feature < SEAT_FEATURES; ++feature)
            names.emplace_back(Acore::StringFormat("member{}_{}", slot, SeatFeatureName(feature)));
    for (uint32 slot = 0; slot < DIRECTOR_ENEMIES; ++slot)
        for (uint32 feature = 0; feature < ENEMY_FEATURES; ++feature)
            names.emplace_back(Acore::StringFormat("enemy{}_{}", slot, EnemyFeatureName(feature)));
    for (uint32 slot = 0; slot < DIRECTOR_OBJECTS; ++slot)
        for (uint32 feature = 0; feature < OBJECT_FEATURES; ++feature)
            names.emplace_back(Acore::StringFormat("object{}_{}", slot, ObjectFeatureName(feature)));
    return names;
}

boost::json::object Animus::Curriculum::DirectorLayout::SetDescriptor()
{
    auto const set = [](uint32 first, uint32 slots, uint32 width)
    {
        boost::json::object entry;
        entry["first"] = first;
        entry["slots"] = slots;
        entry["width"] = width;
        entry["present"] = 0;
        return entry;
    };
    auto const pointer = [](uint32 first, char const* over, uint32 count)
    {
        boost::json::object entry;
        entry["first"] = first;
        entry["over"] = over;
        entry["count"] = count;
        return entry;
    };
    boost::json::object descriptor;
    descriptor["globals"] = uint32(OBS_GLOBAL_COUNT);
    descriptor["seats"] = set(OBS_SEAT_FIRST, DIRECTOR_SEATS, SEAT_FEATURES);
    descriptor["enemies"] = set(OBS_ENEMY_FIRST, DIRECTOR_ENEMIES, ENEMY_FEATURES);
    descriptor["objects"] = set(OBS_OBJECT_FIRST, DIRECTOR_OBJECTS, OBJECT_FEATURES);
    boost::json::array pointers;
    pointers.push_back(pointer(ACTION_ADDRESS_FIRST, "seats", DIRECTOR_SEATS));
    pointers.push_back(pointer(ACTION_HEAL_FIRST, "seats", DIRECTOR_SEATS));
    pointers.push_back(pointer(ACTION_FOLLOW_FIRST, "seats", DIRECTOR_SEATS));
    pointers.push_back(pointer(ACTION_ASSIST_FIRST, "seats", DIRECTOR_SEATS));
    pointers.push_back(pointer(ACTION_FOCUS_FIRST, "enemies", DIRECTOR_ENEMIES));
    pointers.push_back(pointer(ACTION_PULL_FIRST, "enemies", DIRECTOR_ENEMIES));
    pointers.push_back(pointer(ACTION_CONTROL_FIRST, "enemies", DIRECTOR_ENEMIES));
    pointers.push_back(pointer(ACTION_INTERRUPT_FIRST, "enemies", DIRECTOR_ENEMIES));
    pointers.push_back(pointer(ACTION_TAUNT_FIRST, "enemies", DIRECTOR_ENEMIES));
    pointers.push_back(pointer(ACTION_LOOT_FIRST, "objects", DIRECTOR_OBJECTS));
    pointers.push_back(pointer(ACTION_USE_FIRST, "objects", DIRECTOR_OBJECTS));
    descriptor["pointers"] = std::move(pointers);
    descriptor["may_call"] = uint32(MAY_CALL_COLUMN);
    descriptor["obs"] = uint32(OBS_COUNT);
    descriptor["actions"] = uint32(ACTION_COUNT);
    return descriptor;
}

char const* Animus::Curriculum::DirectorLayout::Name()
{
    return "director";
}
