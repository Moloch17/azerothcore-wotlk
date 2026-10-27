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

#ifndef ANIMUS_LIB_CURRICULUM_HINT_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_HINT_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **The teacher's suggestion** (dm_clear's teacher, the user 2026-10-27: "a teacher again that slowly withdraws"):
    /// what the scripted teacher would press and where it would head this decision, for the learner to imitate, and
    /// whether the teacher played the decision -- never for the policy to read: the learner keeps these columns out
    /// of its networks (the "hint" manifest flag) and reads them for its imitation terms only. The scenario writes
    /// them after the seat's row is observed (StageScenario::WriteHints); the block itself writes zeros, so a model
    /// played anywhere else -- an evaluation, the module -- sees nothing here.
    class HintBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_ACTION          = 0,    // the suggested action's index in the layout + 1 (0: no hint)
            OBS_WEIGHT          = 1,    // how much the learner imitates it (0: not at all)
            OBS_SCRIPTED        = 2,    // 1: the teacher played this decision, not the policy (out of the PPO terms)
            OBS_HEADING         = 3,    // the teacher's heading, radians from the body's facing in (-pi, pi]
            OBS_HEADING_WEIGHT  = 4,    // how much the learner imitates the heading (0: not at all)
            OBS_COUNT           = 5
        };

        [[nodiscard]] BlockSize Size(Layout const& /*layout*/) const override { return { OBS_COUNT, 0 }; }
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
    };

    /// The director's hint columns, after its own row (DirectorLayout::OBS_COUNT): the suggested call + 1, its weight,
    /// and whether the teacher played the director's decision; written by StageScenario::WriteHints.
    constexpr uint32 DIRECTOR_HINT_COUNT = 3;
}

#endif
