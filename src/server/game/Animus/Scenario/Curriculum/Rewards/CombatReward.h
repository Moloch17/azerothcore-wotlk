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

#ifndef ANIMUS_LIB_CURRICULUM_COMBAT_REWARD_H
#define ANIMUS_LIB_CURRICULUM_COMBAT_REWARD_H

#include "CurriculumTuning.h"
#include "Define.h"
#include <algorithm>

namespace Animus::Curriculum
{
    struct SeatState;

    /// What the dungeon stages' fights share: the approach range and the difficulty scale.
    namespace CombatReward
    {
        /// The range the approach shaping aims for: the spec's melee or ranged range.
        [[nodiscard]] float DesiredRange(SeatState const& seat, CurriculumTuning::DuelTuning const& duel);

        /// The factor a fight's outcome terms carry for its difficulty tier: 1 + step x tier (Difficulty.TierScale).
        /// A win is multiplied by it and a loss divided by it, so the score stays comparable across the ladder.
        [[nodiscard]] inline float TierScale(float step, uint32 tier)
        {
            return 1.0f + std::max(0.0f, step) * float(tier);
        }
    }
}

#endif
