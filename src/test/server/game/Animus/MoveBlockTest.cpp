/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#include "Block.h"
#include "Layout.h"
#include "MoveBlock.h"
#include "MoveControls.h"
#include "gtest/gtest.h"
#include <boost/json/object.hpp>

namespace Cu = Animus::Curriculum;
using Animus::Curriculum::MoveBlock;

// The move block of revision 4: no ground rays, no flight rays, no clearance. Its 63 columns are the body, the held
// controls, the target, hazard and objective bearings, the water, the detour and motion rates and the trail -- the
// 64 ray columns (16 rays x reach, step, shore, burns) that came after the objective's distance are gone, so the
// water's columns follow it directly. 130 at revision 2, 127 at 3 (never shipped), 63 now.
TEST(MoveBlockTest, NoRayColumns)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Move);
    Cu::Layout layout;
    EXPECT_EQ(block.Revision(), 4u);
    EXPECT_EQ(Cu::MoveControls::REVISION, 4u);
    EXPECT_EQ(block.Size(layout).Obs, 63u);
    EXPECT_EQ(uint32(MoveBlock::OBS_COUNT), 63u);
    EXPECT_EQ(block.Size(layout).Actions, uint32(Cu::MoveControls::ACTION_COUNT));

    // Nothing between the objective's distance and the water.
    EXPECT_EQ(uint32(MoveBlock::OBS_IN_WATER), uint32(MoveBlock::OBS_OBJECTIVE_DISTANCE) + 1);
    EXPECT_EQ(uint32(MoveBlock::OBS_TRAIL_FIRST), uint32(MoveBlock::OBS_OBJECTIVE_NEAR) + 1);
    EXPECT_EQ(uint32(MoveBlock::OBS_TRAIL_DWELL) + 1, uint32(MoveBlock::OBS_COUNT));

    // The manifest says so, and carries none of the probe's settings.
    boost::json::object manifest;
    block.DescribeManifest(layout, manifest);
    EXPECT_EQ(manifest.at("ground_probe").as_string(), "none");
    EXPECT_FALSE(manifest.contains("rays"));
    EXPECT_FALSE(manifest.contains("march_ranges"));
    EXPECT_FALSE(manifest.contains("march_max"));
    EXPECT_FALSE(manifest.contains("probe_yards"));
    EXPECT_FALSE(manifest.contains("clearance_range"));
}
