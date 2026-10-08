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

#ifndef ANIMUS_LIB_CURRICULUM_WING_RUN_H
#define ANIMUS_LIB_CURRICULUM_WING_RUN_H

#include <cstdint>

/// **The party stages' bookkeeping of a dungeon run** (dungeon-curriculum D2, D3; InstanceEncounter): the pure
/// parts -- the tier a run's outcome is scaled by, the party's leader, who strays and who is away -- with no core
/// types.
namespace Animus::Curriculum::WingRun
{
    /// The difficulty ladder's rungs (StageScenario::WING_RUNGS, 9) a tier spans: a run's outcome terms scale with
    /// 1 + Difficulty.TierScale x its tier, rung 0 (the easiest, eight levels up) tier 0 and the evaluation's rung 8
    /// (the dungeon's own levels, no wipes spared) tier 4, so a ladder that steps down raises what a clear pays rather
    /// than the score falling as it gets harder (animus-tier-scaled-outcomes).
    constexpr uint32_t RUNGS_PER_TIER = 2;

    [[nodiscard]] constexpr uint32_t TierOfRung(uint32_t rung)
    {
        return rung / RUNGS_PER_TIER;
    }

    /// **The leader a party keeps with** ("stay with the leader"): the "human" stand-in's seat when it leads (I7: it
    /// sits in seat 0, the group's leader), else the tank's; -1 for none.
    [[nodiscard]] constexpr int32_t LeaderSeat(int32_t standInSeat, bool standInLeads, int32_t tankSeat)
    {
        return standInLeads && standInSeat >= 0 ? standInSeat : tankSeat;
    }

    /// **Whether a seat pays Lost this decision** (Instance.WingStray): alive, not the leader, the leader alive, and
    /// further than `strayYards` from it -- and not walking back from the entrance after a rise, which Away prices
    /// (the same seconds are never charged twice, and coming back is never a stray).
    [[nodiscard]] constexpr bool Strays(bool alive, bool isLeader, bool walkingBack, bool leaderAlive, float yards,
        float strayYards)
    {
        return alive && !isLeader && !walkingBack && leaderAlive && yards > strayYards;
    }

    /// **Whether a seat pays Away this decision** (Instance.WingAway): dead, or risen at the entrance and not yet back
    /// with the party (as C3's Combat.Away; never a reward for coming back, which would pay dying).
    [[nodiscard]] constexpr bool Away(bool alive, bool walkingBack)
    {
        return !alive || walkingBack;
    }
}

#endif
