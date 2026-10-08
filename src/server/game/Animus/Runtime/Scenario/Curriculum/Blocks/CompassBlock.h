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

#ifndef ANIMUS_LIB_CURRICULUM_COMPASS_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_COMPASS_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// **Where the objective is** (perception-goals plan, P1: the compass split). Whether there is one, its bearing
    /// off the seat's facing and its distance at two scales: the columns the move block carried up to its revision 4,
    /// moved here so a stage can leave them out. All straight-line, from the seat to the mark: no walking way, no route
    /// (vision-only movement, decision 0019). M1 (move1_controls) carries the compass and is told where its mark is; M2 (move2_seek) does not,
    /// and has to find its objective with the camera, whose objective flag shows it only in line of sight.
    ///
    /// No actions. The column names are revision 4's move block's (stage.json obs_names), so a seed from a revision 4
    /// checkpoint lands the compass's weights here (bootstrap's by-name remap) and the move block's kept columns where
    /// the move block now has them.
    class CompassBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_OBJECTIVE = 0,              // there is one
            OBS_OBJECTIVE_BEARING_SIN,      // its bearing off the seat's facing, in (-pi, pi]
            OBS_OBJECTIVE_BEARING_COS,
            OBS_OBJECTIVE_DISTANCE,         // yards / 500, clamped
            /// The same distance over forty yards: the last forty, at a resolution that can see a stop's yard.
            OBS_OBJECTIVE_NEAR,
            OBS_COUNT
        };

        static constexpr float OBJECTIVE_SCALE = 500.0f;
        static constexpr float NEAR_SCALE = 40.0f;

        /// 1: the columns the move block carried to its revision 4, in their own block. 2 (2026-10-08, vision-only
        /// movement): the detour column left -- it was a constant 0 since the travel encounter went, and the walking
        /// way is no input of the policy's; five columns. A revision 1 checkpoint seeds by name (bootstrap's by-name
        /// remap): the named columns carry, the detour's weights are dropped.
        [[nodiscard]] uint32 Revision() const override { return 2; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        [[nodiscard]] static char const* ColumnName(uint32 column);
    };
}

#endif
