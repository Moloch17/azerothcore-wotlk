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

#ifndef ANIMUS_LIB_CURRICULUM_BUILD_RETRY_H
#define ANIMUS_LIB_CURRICULUM_BUILD_RETRY_H

#include <cstdint>

namespace Animus::Curriculum
{
    /// Builds of an env's first episode tried at a stage's setup before the stage is given up on. A failed build in
    /// training ends that episode and the next reset draws again (StageScenario::Reset); at setup one unlucky draw --
    /// an arena, a spawn point and its four retries all refusing -- used to end the whole plan (move3_vertical,
    /// 2026-10-05). Each try draws the arena and the spawn afresh, so only a stage that keeps failing fails.
    constexpr uint32_t SETUP_BUILD_ATTEMPTS = 8;

    /// Call `build` until it succeeds or `attempts` tries have failed, `onFailure(try)` after each failure; returns
    /// whether a try succeeded. Pure, for BuildRetryTest.
    template <typename Build, typename OnFailure>
    bool RetryBuild(uint32_t attempts, Build&& build, OnFailure&& onFailure)
    {
        for (uint32_t attempt = 0; attempt < attempts; ++attempt)
        {
            if (build())
                return true;
            onFailure(attempt);
        }
        return false;
    }
}

#endif
