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

#ifndef ANIMUS_LIB_CURRICULUM_WORLD_COORDINATOR_H
#define ANIMUS_LIB_CURRICULUM_WORLD_COORDINATOR_H

#include "Define.h"
#include "Position.h"
#include <vector>

namespace Animus::Curriculum
{
    /// **The world coordinator** (long-horizon plan, Component F): rule-based, so hundreds of bots in one zone do not
    /// all pile onto the same creatures. A group working a place claims it for a while (refreshed while it stays);
    /// other groups see the claim in their journal and are sent elsewhere when elsewhere will do. Nothing is
    /// learned here and nothing is forbidden: poaching a claimed place is charged in training (Life.Poach), and the
    /// journal says which places are taken, so the policy learns to go where it is not.
    class WorldCoordinator
    {
    public:
        struct Claim
        {
            uint32 Owner = 0;               // the claiming group
            Position Where;
            float Radius = 0.0f;
            uint64 UntilMs = 0;
        };

        /// `owner` is working at `where`: claim (or keep) it until `nowMs + holdMs`.
        void Stake(uint32 owner, Position const& where, float radius, uint64 nowMs, uint32 holdMs);
        /// Whether `at` lies in a place another group holds.
        [[nodiscard]] bool ClaimedByOther(uint32 owner, Position const& at, uint64 nowMs) const;
        /// Of `candidates`, where `owner` should go: the first no other group holds, else the first.
        [[nodiscard]] Position const* Assign(uint32 owner, std::vector<Position> const& candidates, uint64 nowMs) const;
        /// Drop lapsed claims.
        void Expire(uint64 nowMs);
        void Clear() { _claims.clear(); }
        [[nodiscard]] std::vector<Claim> const& Claims() const { return _claims; }

    private:
        std::vector<Claim> _claims;
    };
}

#endif
