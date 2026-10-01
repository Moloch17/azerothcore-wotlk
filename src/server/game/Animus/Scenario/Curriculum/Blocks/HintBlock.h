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
    /// A suggested action for the learner to imitate while the support lasts (Instance.WingHint), never for the
    /// policy to read: the learner keeps these columns out of its networks (their adapter weights stay zero) and
    /// trains toward the suggestion with a weight that falls with the support. The scenario writes them after the
    /// seat's row is observed (StageScenario::ObserveSeat); the block itself writes nothing, so a model played anywhere
    /// else -- an evaluation, the module -- sees zeros.
    class HintBlock final : public Block
    {
    public:
        enum Feature : uint32
        {
            OBS_ACTION  = 0,    // the suggested action's index in the layout + 1 (0: none; 1: the no-op, waiting)
            OBS_WEIGHT  = 1,    // how much the learner imitates it (0: not at all)
            OBS_SCRIPTED = 2,   // 1: the script played this decision (Instance.WingScript), not the policy
            OBS_COUNT   = 3
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Hint; }
        [[nodiscard]] BlockSize Size(Layout const& /*layout*/) const override { return { OBS_COUNT, 0 }; }
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& /*view*/, float* /*obs*/, uint8* /*mask*/) const override { }
    };
}

#endif
