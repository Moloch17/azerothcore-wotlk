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
#include "CompassBlock.h"
#include "Layout.h"
#include "SeatView.h"
#include "StageDefinition.h"
#include "gtest/gtest.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
using Animus::Curriculum::CompassBlock;

// The compass (perception-goals P1): revision 4's objective columns, moved out of the move block in their order and
// under their names -- presence, bearing, both distances, the detour -- and no actions.
TEST(CompassBlockTest, ColumnsAreTheMoveBlocksObjectiveColumns)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Compass);
    Cu::Layout layout;
    EXPECT_EQ(block.Revision(), 1u);
    EXPECT_EQ(block.Size(layout).Obs, 6u);
    EXPECT_EQ(block.Size(layout).Actions, 0u);
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::Compass), "compass");

    boost::json::array names;
    block.DescribeColumns(layout, names);
    std::vector<std::string> const expected = { "objective", "objective_bearing_sin", "objective_bearing_cos",
        "objective_distance", "objective_near", "detour" };
    ASSERT_EQ(names.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ(std::string(names[i].as_string()), expected[i]);

    boost::json::object manifest;
    block.DescribeManifest(layout, manifest);
    EXPECT_DOUBLE_EQ(manifest.at("objective_scale").as_double(), 500.0);
    EXPECT_DOUBLE_EQ(manifest.at("near_scale").as_double(), 40.0);
}

// With no bot or no objective the compass reads nothing but the detour (the scenario's, 0 without one).
TEST(CompassBlockTest, NothingWithoutAnObjective)
{
    Cu::SeatView view;
    view.Detour = 2.0f;
    std::vector<float> obs(CompassBlock::OBS_COUNT, 0.0f);
    Cu::GetBlock(Cu::BlockId::Compass).Observe(view, obs.data(), nullptr);
    EXPECT_FLOAT_EQ(obs[CompassBlock::OBS_DETOUR], 0.5f);
    EXPECT_FLOAT_EQ(obs[CompassBlock::OBS_OBJECTIVE], 0.0f);
    EXPECT_FLOAT_EQ(obs[CompassBlock::OBS_OBJECTIVE_BEARING_COS], 0.0f);
}

// M1 carries the compass (its mark is a known point); M2 seek does not, so its layout has no objective column at all.
TEST(CompassBlockTest, M1CarriesItAndSeekDoesNot)
{
    Cu::StageDefinition const* controls = Cu::FindStage("move1_controls");
    ASSERT_NE(controls, nullptr);
    EXPECT_TRUE(controls->Has(Cu::BlockId::Compass));
    Cu::StageDefinition const* seek = Cu::FindStage("move2_seek");
    if (!seek)
        GTEST_SKIP() << "move2_seek is not defined yet";
    EXPECT_FALSE(seek->Has(Cu::BlockId::Compass));
    EXPECT_TRUE(seek->Has(Cu::BlockId::Move));
    EXPECT_TRUE(seek->Has(Cu::BlockId::Vision));
}
