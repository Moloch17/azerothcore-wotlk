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
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <set>
#include <string>

namespace Cu = Animus::Curriculum;
using Animus::Curriculum::MoveBlock;

// The move block of revision 5: no ground rays, no flight rays, no clearance (revision 4), and no objective (the
// compass block's since revision 5). Its 57 columns are the body, the held controls, the target and hazard bearings,
// the water, the motion rates and the trail. 130 at revision 2, 127 at 3 (never shipped), 63 at 4, 57 now.
TEST(MoveBlockTest, NoRayColumns)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Move);
    Cu::Layout layout;
    EXPECT_EQ(block.Revision(), 5u);
    EXPECT_EQ(Cu::MoveControls::REVISION, 5u);
    EXPECT_EQ(block.Size(layout).Obs, 57u);
    EXPECT_EQ(uint32(MoveBlock::OBS_COUNT), 57u);
    EXPECT_EQ(block.Size(layout).Actions, uint32(Cu::MoveControls::ACTION_COUNT));

    // Nothing between the hazard's radius and the water, nor between the motion rates and the trail.
    EXPECT_EQ(uint32(MoveBlock::OBS_IN_WATER), uint32(MoveBlock::OBS_HAZARD_RADIUS) + 1);
    EXPECT_EQ(uint32(MoveBlock::OBS_MOVE_RATE), uint32(MoveBlock::OBS_AIRBORNE) + 1);
    EXPECT_EQ(uint32(MoveBlock::OBS_TRAIL_FIRST), uint32(MoveBlock::OBS_CLOSE_RATE) + 1);
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

// Every column is named, once (stage.json obs_names: bootstrap maps a revision 4 checkpoint's columns by these names),
// and none of them is an objective's: those are the compass block's.
TEST(MoveBlockTest, ColumnsAreNamedAndNoneIsTheObjective)
{
    Cu::Layout layout;
    boost::json::array names;
    Cu::GetBlock(Cu::BlockId::Move).DescribeColumns(layout, names);
    ASSERT_EQ(names.size(), std::size_t(MoveBlock::OBS_COUNT));
    std::set<std::string> seen;
    for (auto const& name : names)
    {
        std::string const text(name.as_string());
        EXPECT_FALSE(text.empty());
        EXPECT_TRUE(seen.insert(text).second) << text;
        EXPECT_EQ(text.find("objective"), std::string::npos) << text;
        EXPECT_NE(text, "detour");
    }
    EXPECT_EQ(MoveBlock::ColumnName(MoveBlock::OBS_MOVING), "moving");
    EXPECT_EQ(MoveBlock::ColumnName(MoveBlock::OBS_MODE_FIRST + 2), "mode_swimming");
    EXPECT_EQ(MoveBlock::ColumnName(MoveBlock::OBS_TRAIL_FIRST + 3), "trail_1_left");
    EXPECT_EQ(MoveBlock::ColumnName(MoveBlock::OBS_TRAIL_DWELL), "trail_dwell");
}
