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

#include "FlagBlock.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float DISTANCE_SCALE = 200.0f;
    constexpr float SCORE_SCALE = 3.0f;

    void WritePlace(Player const* bot, Position const& place, float* out)
    {
        float const bearing = bot->GetRelativeAngle(&place);
        out[0] = std::min(1.0f, bot->GetExactDist2d(&place) / DISTANCE_SCALE);
        out[1] = std::sin(bearing);
        out[2] = std::cos(bearing);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::FlagBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::FlagBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    SeatView::FlagMatch const& flags = view.Flags;
    if (!flags.Active)
        return;

    using State = SeatView::FlagState;
    Player const* bot = view.Bot;

    obs[OBS_CARRYING] = flags.Enemy == State::Carried ? 1.0f : 0.0f;
    obs[OBS_OWN_AT_BASE] = flags.Own == State::AtBase ? 1.0f : 0.0f;
    obs[OBS_OWN_CARRIED] = flags.Own == State::Carried ? 1.0f : 0.0f;
    obs[OBS_OWN_DROPPED] = flags.Own == State::Dropped ? 1.0f : 0.0f;
    obs[OBS_ENEMY_AT_BASE] = flags.Enemy == State::AtBase ? 1.0f : 0.0f;
    obs[OBS_ENEMY_DROPPED] = flags.Enemy == State::Dropped ? 1.0f : 0.0f;

    WritePlace(bot, flags.OwnBase, obs + OBS_OWN_BASE_DISTANCE);
    WritePlace(bot, flags.EnemyBase, obs + OBS_ENEMY_BASE_DISTANCE);

    Position const* dropped = nullptr;
    if (flags.Own == State::Dropped)
        dropped = &flags.OwnDropped;
    if (flags.Enemy == State::Dropped
        && (!dropped || bot->GetExactDist2d(&flags.EnemyDropped) < bot->GetExactDist2d(dropped)))
        dropped = &flags.EnemyDropped;
    if (dropped)
        WritePlace(bot, *dropped, obs + OBS_DROPPED_DISTANCE);

    obs[OBS_CAN_TAKE] = flags.Usable ? 1.0f : 0.0f;
    if (mask)
        mask[ACTION_TAKE_FLAG] = flags.Usable ? 1 : 0;

    obs[OBS_OWN_SCORE] = std::min(1.0f, float(flags.OwnScore) / SCORE_SCALE);
    obs[OBS_ENEMY_SCORE] = std::min(1.0f, float(flags.EnemyScore) / SCORE_SCALE);
}

void Animus::Curriculum::FlagBlock::Apply(SeatView& view, uint32 local, SeatActionResult& /*result*/) const
{
    if (local != ACTION_TAKE_FLAG || !view.Flags.Usable)
        return;

    Player* bot = view.Bot;
    GameObject* flag = bot ? ObjectAccessor::GetGameObject(*bot, view.Flags.Usable) : nullptr;
    if (!flag)
        return;

    // What a player's click does: the battleground's own handler decides whether this is a pickup, a return or
    // nothing at all, which is the point of going through the object rather than reimplementing the rules.
    flag->Use(bot);
}
