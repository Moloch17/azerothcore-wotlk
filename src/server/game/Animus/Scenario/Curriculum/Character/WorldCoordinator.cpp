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

#include "WorldCoordinator.h"
#include <algorithm>

void Animus::Curriculum::WorldCoordinator::Stake(uint32 owner, Position const& where, float radius, uint64 nowMs,
    uint32 holdMs)
{
    for (Claim& claim : _claims)
        if (claim.Owner == owner && claim.Where.GetExactDist2d(&where) <= std::max(claim.Radius, radius))
        {
            claim.UntilMs = std::max(claim.UntilMs, nowMs + holdMs);
            return;
        }
    _claims.push_back({ owner, where, radius, nowMs + holdMs });
}

bool Animus::Curriculum::WorldCoordinator::ClaimedByOther(uint32 owner, Position const& at, uint64 nowMs) const
{
    return std::any_of(_claims.begin(), _claims.end(), [&](Claim const& claim)
    {
        return claim.Owner != owner && claim.UntilMs > nowMs && claim.Where.GetExactDist2d(&at) <= claim.Radius;
    });
}

Position const* Animus::Curriculum::WorldCoordinator::Assign(uint32 owner,
    std::vector<Position> const& candidates, uint64 nowMs) const
{
    for (Position const& place : candidates)
        if (!ClaimedByOther(owner, place, nowMs))
            return &place;
    return candidates.empty() ? nullptr : &candidates.front();
}

void Animus::Curriculum::WorldCoordinator::Expire(uint64 nowMs)
{
    std::erase_if(_claims, [nowMs](Claim const& claim) { return claim.UntilMs <= nowMs; });
}
