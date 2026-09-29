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
    obs[OBS_CALLS_LEFT] = float(view.CallsLeft) / float(RAID_CALLS);
    obs[OBS_BY_EVENT] = view.ByEvent ? 1.0f : 0.0f;
    obs[OBS_RAID] = view.Raid ? 1.0f : 0.0f;
    obs[OBS_EPISODE_TIME] = view.EpisodeTime;
    obs[OBS_OWN_STANDING] = view.OwnStanding;
    obs[OBS_ENEMY_STANDING] = view.EnemyStanding;
    obs[OBS_OWN_HEALTH] = view.OwnHealth;
    obs[OBS_ENEMY_HEALTH] = view.EnemyHealth;
    obs[OBS_OWN_SCORE] = view.OwnScore;
    obs[OBS_ENEMY_SCORE] = view.EnemyScore;
    obs[OBS_HAS_OBJECTIVE] = view.HasObjective ? 1.0f : 0.0f;
    obs[OBS_SINCE_CALL] = view.SinceCall;
    obs[OBS_HAS_FOCUS] = view.HasFocus ? 1.0f : 0.0f;
    obs[OBS_ADDRESS_FIRST + uint32(view.Address)] = 1.0f;
    obs[OBS_ADDRESS_GROUP] = float(view.AddressGroup) / float(RAID_GROUPS);
    for (uint32 objective = 0; objective < view.Objectives.size(); ++objective)
        obs[OBS_OBJECTIVE_FIRST + objective] = view.Objectives[objective] ? 1.0f : 0.0f;
    obs[OBS_POSTURE_FIRST + uint32(view.Posture)] = 1.0f;
    obs[OBS_RALLY_FIRST + uint32(view.Rally)] = 1.0f;
    obs[OBS_ANCHOR_FIRST + uint32(view.Anchor)] = 1.0f;
    obs[OBS_OFFSET_FIRST + uint32(view.Offset)] = 1.0f;
    obs[OBS_RING_FIRST + uint32(view.Ring)] = 1.0f;
    obs[OBS_PLACE_VALID] = view.PlaceValid ? 1.0f : 0.0f;
    obs[OBS_PLACE_DISTANCE] = view.PlaceDistance;

    // Nothing is sayable off the director's turn: its action there changes nothing (DirectorEncounter::Call).
    bool const speak = mask && view.MayCall;
    bool const individual = view.Address != OrderSource::Side;
    for (uint32 slot = 0; slot < view.SeatCount && slot < DIRECTOR_SEATS; ++slot)
    {
        DirectorView::SeatSlot const& seat = view.Seats[slot];
        float* out = obs + OBS_SEAT_FIRST + slot * SEAT_FEATURES;
        out[SEAT_PRESENT] = seat.Present ? 1.0f : 0.0f;
        if (!seat.Present)
            continue;

        out[SEAT_ALIVE] = seat.Alive ? 1.0f : 0.0f;
        out[SEAT_HEALTH] = seat.Health;
        out[SEAT_POWER] = seat.Power;
        seat.Apt.WriteBrief(out + SEAT_APTITUDE_FIRST);
        out[SEAT_IN_COMBAT] = seat.InCombat ? 1.0f : 0.0f;
        out[SEAT_CASTING] = seat.Casting ? 1.0f : 0.0f;
        out[SEAT_SPREAD] = seat.Spread;
        out[SEAT_BEARING_SIN] = seat.BearingSin;
        out[SEAT_BEARING_COS] = seat.BearingCos;
        out[SEAT_TO_FOCUS] = seat.ToFocus;
        out[SEAT_ON_FOCUS] = seat.OnFocus ? 1.0f : 0.0f;
        out[SEAT_AT_PLACE] = seat.AtPlace ? 1.0f : 0.0f;
        out[SEAT_GROUP] = float(seat.Group) / float(RAID_GROUPS);
        out[SEAT_ADDRESSED] = seat.Addressed ? 1.0f : 0.0f;
        out[SEAT_ATTACKED] = seat.Attacked;
        out[SEAT_ORDER_FIRST + uint32(seat.Order)] = 1.0f;
        out[SEAT_ORDER_AGE] = seat.OrderAge;

        // A living member can be addressed, and healed by an order to whoever is addressed.
        if (speak && seat.Alive)
        {
            mask[ACTION_ADDRESS_MEMBER_FIRST + slot] = 1;
            if (individual)
                mask[ACTION_HEAL_FIRST + slot] = 1;
        }
    }

    for (uint32 slot = 0; slot < view.EnemyCount && slot < PACK_SLOTS; ++slot)
    {
        DirectorView::EnemySlot const& enemy = view.Enemies[slot];
        float* out = obs + OBS_ENEMY_FIRST + slot * ENEMY_FEATURES;
        out[ENEMY_PRESENT] = enemy.Present ? 1.0f : 0.0f;
        if (!enemy.Present)
            continue;

        out[ENEMY_ALIVE] = enemy.Alive ? 1.0f : 0.0f;
        out[ENEMY_HEALTH] = enemy.Health;
        enemy.Apt.WriteBrief(out + ENEMY_APTITUDE_FIRST);
        out[ENEMY_IN_COMBAT] = enemy.InCombat ? 1.0f : 0.0f;
        out[ENEMY_CASTING] = enemy.Casting ? 1.0f : 0.0f;
        out[ENEMY_SPREAD] = enemy.Spread;
        out[ENEMY_IS_FOCUS] = enemy.IsFocus ? 1.0f : 0.0f;
        out[ENEMY_SEEN] = enemy.Seen ? 1.0f : 0.0f;
        out[ENEMY_UNSEEN_TIME] = enemy.UnseenTime;
        out[ENEMY_BEARING_SIN] = enemy.BearingSin;
        out[ENEMY_BEARING_COS] = enemy.BearingCos;
        out[ENEMY_ON_SEAT] = enemy.OnSeat ? 1.0f : 0.0f;
        out[ENEMY_ORDERED] = enemy.Ordered;

        // Calling a dead enemy is not a call, and the seats could not act on it. Focus goes to the side or to
        // whoever is addressed; tank, interrupt and control are duties, so only to someone addressed.
        if (speak && enemy.Alive)
        {
            mask[ACTION_FOCUS_FIRST + slot] = 1;
            if (individual)
            {
                mask[ACTION_TANK_FIRST + slot] = 1;
                mask[ACTION_INTERRUPT_FIRST + slot] = 1;
                mask[ACTION_CONTROL_FIRST + slot] = 1;
            }
        }
    }

    if (!speak)
        return;

    // Posture and shape are the side's, and always sayable; so is addressing the side, or a group it has.
    std::fill(mask + ACTION_POSTURE_FIRST, mask + ACTION_POSTURE_FIRST + TEAM_POSTURE_COUNT, uint8(1));
    std::fill(mask + ACTION_RALLY_FIRST, mask + ACTION_RALLY_FIRST + TEAM_RALLY_COUNT, uint8(1));
    mask[ACTION_ADDRESS_SIDE] = 1;
    for (uint32 group = 0; view.Raid && group < view.Groups && group < RAID_GROUPS; ++group)
        mask[ACTION_ADDRESS_GROUP_FIRST + group] = 1;

    if (view.PlacesAllowed)
    {
        std::fill(mask + ACTION_ANCHOR_FIRST, mask + ACTION_ANCHOR_FIRST + PLACE_ANCHOR_COUNT, uint8(1));
        std::fill(mask + ACTION_OFFSET_FIRST, mask + ACTION_OFFSET_FIRST + PLACE_OFFSET_COUNT, uint8(1));
        std::fill(mask + ACTION_RING_FIRST, mask + ACTION_RING_FIRST + PLACE_RING_COUNT, uint8(1));
        mask[ACTION_GO_TO] = view.PlaceValid ? 1 : 0;
    }
    else
        mask[ACTION_RALLY_FIRST + uint32(TeamRally::Point)] = 0;    // Rally::Point needs a place

    for (uint32 objective = 0; objective < view.Objectives.size(); ++objective)
        if (view.Objectives[objective])
            mask[ACTION_OBJECTIVE_FIRST + objective] = 1;
}

std::vector<std::string> Animus::Curriculum::DirectorLayout::ActionNames()
{
    std::vector<std::string> names(ACTION_COUNT);
    names[ACTION_HOLD] = "hold";
    for (uint32 posture = 0; posture < TEAM_POSTURE_COUNT; ++posture)
        names[ACTION_POSTURE_FIRST + posture] = Acore::StringFormat("posture_{}",
            PostureName(TeamPosture(posture)));
    for (uint32 rally = 0; rally < TEAM_RALLY_COUNT; ++rally)
        names[ACTION_RALLY_FIRST + rally] = Acore::StringFormat("rally_{}", RallyName(TeamRally(rally)));
    for (uint32 anchor = 0; anchor < PLACE_ANCHOR_COUNT; ++anchor)
        names[ACTION_ANCHOR_FIRST + anchor] = Acore::StringFormat("anchor_{}", AnchorName(PlaceAnchor(anchor)));
    for (uint32 offset = 0; offset < PLACE_OFFSET_COUNT; ++offset)
        names[ACTION_OFFSET_FIRST + offset] = Acore::StringFormat("offset_{}", OffsetName(PlaceOffset(offset)));
    for (uint32 ring = 0; ring < PLACE_RING_COUNT; ++ring)
        names[ACTION_RING_FIRST + ring] = Acore::StringFormat("ring_{}", RingName(PlaceRing(ring)));
    names[ACTION_ADDRESS_SIDE] = "address_side";
    for (uint32 group = 0; group < RAID_GROUPS; ++group)
        names[ACTION_ADDRESS_GROUP_FIRST + group] = Acore::StringFormat("address_group_{}", group);
    for (uint32 slot = 0; slot < DIRECTOR_SEATS; ++slot)
    {
        names[ACTION_ADDRESS_MEMBER_FIRST + slot] = Acore::StringFormat("address_member_{}", slot);
        names[ACTION_HEAL_FIRST + slot] = Acore::StringFormat("heal_{}", slot);
    }
    for (uint32 slot = 0; slot < PACK_SLOTS; ++slot)
    {
        names[ACTION_FOCUS_FIRST + slot] = Acore::StringFormat("focus_{}", slot);
        names[ACTION_TANK_FIRST + slot] = Acore::StringFormat("tank_{}", slot);
        names[ACTION_INTERRUPT_FIRST + slot] = Acore::StringFormat("interrupt_{}", slot);
        names[ACTION_CONTROL_FIRST + slot] = Acore::StringFormat("control_{}", slot);
    }
    names[ACTION_GO_TO] = "go_to";
    for (uint32 objective = 0; objective < 4; ++objective)
        names[ACTION_OBJECTIVE_FIRST + objective] = Acore::StringFormat("objective_{}", objective);
    return names;
}

boost::json::object Animus::Curriculum::DirectorLayout::SetDescriptor()
{
    // The two sets and the actions scored per slot, for the learner's set encoder and pointer heads.
    auto const set = [](uint32 first, uint32 slots, uint32 width)
    {
        boost::json::object entry;
        entry["first"] = first;
        entry["slots"] = slots;
        entry["width"] = width;
        entry["present"] = 0;
        return entry;
    };
    auto const pointer = [](uint32 first, char const* over)
    {
        boost::json::object entry;
        entry["first"] = first;
        entry["over"] = over;
        return entry;
    };
    boost::json::object descriptor;
    descriptor["globals"] = uint32(OBS_GLOBAL_COUNT);
    descriptor["seats"] = set(OBS_SEAT_FIRST, DIRECTOR_SEATS, SEAT_FEATURES);
    descriptor["enemies"] = set(OBS_ENEMY_FIRST, PACK_SLOTS, ENEMY_FEATURES);
    boost::json::array pointers;
    pointers.push_back(pointer(ACTION_ADDRESS_MEMBER_FIRST, "seats"));
    pointers.push_back(pointer(ACTION_HEAL_FIRST, "seats"));
    pointers.push_back(pointer(ACTION_FOCUS_FIRST, "enemies"));
    pointers.push_back(pointer(ACTION_TANK_FIRST, "enemies"));
    pointers.push_back(pointer(ACTION_INTERRUPT_FIRST, "enemies"));
    pointers.push_back(pointer(ACTION_CONTROL_FIRST, "enemies"));
    descriptor["pointers"] = std::move(pointers);
    descriptor["may_call"] = uint32(MAY_CALL_COLUMN);
    return descriptor;
}

char const* Animus::Curriculum::DirectorLayout::Name()
{
    return "director";
}
