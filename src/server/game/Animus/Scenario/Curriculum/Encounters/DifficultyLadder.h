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

#ifndef ANIMUS_LIB_CURRICULUM_DIFFICULTY_LADDER_H
#define ANIMUS_LIB_CURRICULUM_DIFFICULTY_LADDER_H

#include "ClassProfile.h"
#include "Define.h"
#include <mutex>
#include <string>
#include <vector>

namespace Animus
{
    struct Env;
}

namespace Animus::Curriculum
{
    class StageScenario;

    /// Difficulty that adapts per class and role (CurriculumTuning::DifficultyTuning): each of them fights at its
    /// own rung, 0 up to the encounter's top one. After Window fights at a rung it moves up one once it won
    /// RaiseAbove of them, down one below LowerBelow; ReviewChance of its training fights are at a lower rung and
    /// StretchChance at the next one up, neither of which count, so nothing is forgotten and nothing is met for the
    /// first time in an evaluation. A fight that simple play wins every time teaches nothing a plan would add.
    ///
    /// The rung is per (class, role) and not per model, even though a model is now a whole class. A paladin that
    /// tanks well and heals badly would otherwise share one rung between them: the tank's wins would carry it up
    /// and the healer would drown at a difficulty it never earned, which is the failure this granularity exists to
    /// prevent. Its own class is still what it learns from; only the pacing is split.
    ///
    /// An evaluation spreads its seeds over the rungs instead (every class over every rung), so two checkpoints
    /// meet the same fights, and a stage viewer's forced tier (StageScenario::ForceTier) wins over both. Rungs start
    /// at 0 with the worldserver.
    class DifficultyLadder
    {
    public:
        /// `what` names the ladder in the log ("difficulty tier", "pack rung").
        DifficultyLadder(StageScenario const& scenario, std::string what);

        struct Pick
        {
            uint32 Tier = 0;
            bool Counts = false;        // a training fight at this class and build's own rung: its outcome moves it
        };

        /// The rung of an episode of class `layout` built as spec `spec`, on a ladder whose top rung is `maxTier`.
        /// World thread (it rolls the review chance).
        [[nodiscard]] Pick Draw(Env const& env, uint16 layout, uint8 spec, uint32 maxTier) const;

        /// A fight drawn with Counts ended: count it, and move that class and build once its window is full. Any
        /// thread.
        void Record(uint16 layout, uint8 spec, uint32 tier, bool won, uint32 maxTier);

        /// Class `layout` built as `spec`'s current rung.
        [[nodiscard]] uint32 Tier(uint16 layout, uint8 spec) const;

    private:
        struct LayoutTier
        {
            uint32 Tier = 0;
            uint32 Fights = 0;          // at this rung, since it was reached
            uint32 Wins = 0;
        };

        /// Row of (layout, spec) in _tiers, which is layout-major: MAX_SPECS rungs per class.
        ///
        /// Keyed on the build, not on a role, and it has to be: a paladin healer and a paladin tank shared a rung
        /// under one model per class, and the tank dragged it up while the healer drowned. Two builds in one role
        /// have the same problem -- a feral cat and a balance druid are both "damage" and are not equally hard to
        /// win with -- which a role could never separate and a spec does.
        [[nodiscard]] std::size_t Row(uint16 layout, uint8 spec) const
        {
            return std::size_t(layout) * MAX_SPECS + std::size_t(std::min<uint32>(spec, MAX_SPECS - 1));
        }

        StageScenario const& _scenario;
        std::string _what;
        mutable std::mutex _lock;       // envs finish on map update threads
        std::vector<LayoutTier> _tiers;
    };
}

#endif
