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

#include "CrowdBlock.h"
#include "Creature.h"
#include "Player.h"
#include "SeatView.h"
#include <boost/json/object.hpp>
#include <cmath>

Animus::Curriculum::BlockSize Animus::Curriculum::CrowdBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_SLOT_FIRST + CROWD_SLOTS * SLOT_FEATURES, 0 };
}

void Animus::Curriculum::CrowdBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["slots"] = CROWD_SLOTS;
    block["slot_features"] = uint32(SLOT_FEATURES);
}

void Animus::Curriculum::CrowdBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    CrowdView const& crowd = view.Crowd;
    Player* bot = view.Bot;
    if (!crowd.Present || !bot)
        return;

    obs[OBS_PRESENT] = 1.0f;
    obs[OBS_ON_PARTY] = std::min(2.0f, float(crowd.OnParty) / 8.0f);
    obs[OBS_ON_TANK] = std::min(2.0f, float(crowd.OnTank) / 8.0f);
    obs[OBS_LOOSE] = std::min(2.0f, float(crowd.OnParty - std::min(crowd.OnParty, crowd.OnTank)) / 8.0f);
    obs[OBS_ELITES] = std::min(2.0f, float(crowd.Elites) / 4.0f);
    obs[OBS_UNSEEN] = std::min(2.0f, float(crowd.OnParty - std::min(crowd.OnParty, PACK_SLOTS)) / 4.0f);
    obs[OBS_IS_TANK] = crowd.Tank == bot ? 1.0f : 0.0f;
    obs[OBS_AHEAD_DISTANCE] = 1.0f;
    if (crowd.HasAhead)
    {
        obs[OBS_AHEAD_DISTANCE] = std::min(1.0f, bot->GetExactDist(&crowd.Ahead) / 60.0f);
        obs[OBS_AHEAD_SIZE] = std::min(2.0f, float(crowd.AheadSize) / 6.0f);
        float const angle = bot->GetRelativeAngle(&crowd.Ahead);
        obs[OBS_AHEAD_SIN] = std::sin(angle);
        obs[OBS_AHEAD_COS] = std::cos(angle);
    }

    for (uint32 slot = 0; slot < crowd.Count && slot < CROWD_SLOTS; ++slot)
    {
        Unit* unit = crowd.Units[slot];
        if (!unit || !unit->IsAlive() || !unit->IsInMap(bot))
            continue;
        float* features = obs + OBS_SLOT_FIRST + slot * SLOT_FEATURES;
        features[SLOT_PRESENT] = 1.0f;
        features[SLOT_HEALTH] = unit->GetHealthPct() / 100.0f;
        features[SLOT_DISTANCE] = std::min(1.0f, bot->GetExactDist(unit) / 40.0f);
        float const angle = bot->GetRelativeAngle(unit);
        features[SLOT_SIN] = std::sin(angle);
        features[SLOT_COS] = std::cos(angle);
        features[SLOT_IN_COMBAT] = unit->IsInCombat() ? 1.0f : 0.0f;
        Unit const* victim = unit->GetVictim();
        features[SLOT_ON_ME] = victim == bot ? 1.0f : 0.0f;
        features[SLOT_ON_TANK] = victim && victim == crowd.Tank ? 1.0f : 0.0f;
        Creature const* creature = unit->ToCreature();
        features[SLOT_ELITE] = creature && creature->isElite() ? 1.0f : 0.0f;
    }
}
