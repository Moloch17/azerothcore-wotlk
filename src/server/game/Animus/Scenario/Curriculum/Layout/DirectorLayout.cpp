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
    obs[OBS_HAS_DUTY] = view.HasDuty ? 1.0f : 0.0f;
    obs[OBS_POSTURE_FIRST + uint32(view.Posture)] = 1.0f;
    obs[OBS_RALLY_FIRST + uint32(view.Rally)] = 1.0f;
    obs[OBS_ANCHOR_FIRST + uint32(view.Anchor)] = 1.0f;
    obs[OBS_OFFSET_FIRST + uint32(view.Offset)] = 1.0f;
    obs[OBS_RING_FIRST + uint32(view.Ring)] = 1.0f;
    obs[OBS_PLACE_VALID] = view.PlaceValid ? 1.0f : 0.0f;
    obs[OBS_PLACE_DISTANCE] = view.PlaceDistance;

    for (uint32 slot = 0; slot < view.SeatCount && slot < TEAM_SEATS; ++slot)
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
        out[SEAT_IS_DUTY] = seat.IsDuty ? 1.0f : 0.0f;
        out[SEAT_AT_PLACE] = seat.AtPlace ? 1.0f : 0.0f;

        // Only a seat that is there to be given the duty may be given it.
        if (mask && seat.Alive)
            mask[ACTION_DUTY_FIRST + slot] = 1;
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

        // Calling a dead enemy is not a call, and the seats could not act on it.
        if (mask && enemy.Alive)
            mask[ACTION_FOCUS_FIRST + slot] = 1;
    }

    // Posture and shape are always sayable: they are about the side, which is always there.
    if (mask)
    {
        std::fill(mask + ACTION_POSTURE_FIRST, mask + ACTION_POSTURE_FIRST + TEAM_POSTURE_COUNT, uint8(1));
        std::fill(mask + ACTION_RALLY_FIRST, mask + ACTION_RALLY_FIRST + TEAM_RALLY_COUNT, uint8(1));

        // Naming a place is offered only where the arena wants one. Everywhere else the thirteen actions stay
        // masked, so a stage with nowhere to send anyone never spends exploration discovering that.
        if (view.PlacesAllowed)
        {
            std::fill(mask + ACTION_ANCHOR_FIRST, mask + ACTION_ANCHOR_FIRST + PLACE_ANCHOR_COUNT, uint8(1));
            std::fill(mask + ACTION_OFFSET_FIRST, mask + ACTION_OFFSET_FIRST + PLACE_OFFSET_COUNT, uint8(1));
            std::fill(mask + ACTION_RING_FIRST, mask + ACTION_RING_FIRST + PLACE_RING_COUNT, uint8(1));
        }
        else
        {
            // Rally::Point means nothing without a place to point at.
            mask[ACTION_RALLY_FIRST + uint32(TeamRally::Point)] = 0;
        }
    }
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
    for (uint32 slot = 0; slot < PACK_SLOTS; ++slot)
        names[ACTION_FOCUS_FIRST + slot] = Acore::StringFormat("focus_{}", slot);
    for (uint32 slot = 0; slot < TEAM_SEATS; ++slot)
        names[ACTION_DUTY_FIRST + slot] = Acore::StringFormat("duty_{}", slot);

    return names;
}

char const* Animus::Curriculum::DirectorLayout::Name()
{
    return "director";
}
