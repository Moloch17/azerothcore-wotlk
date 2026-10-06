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
#include "EntitiesBlock.h"
#include "Identity.h"
#include "Layout.h"
#include "StageDefinition.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <cmath>
#include <vector>

namespace Cu = Animus::Curriculum;
namespace Vi = Animus::Vision;

namespace
{
    using Block = Cu::EntitiesBlock;
}

// The entity list (perception-goals 1b): 32 slots of the plan's fields, no actions; its manifest describes it as a
// set the learner reads beside the camera, with the class and type columns it embeds.
TEST(VisionEntitiesTest, TheBlockIsASetOfTheVisible)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Entities);
    Cu::Layout layout;
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::Entities), "entities");
    EXPECT_EQ(block.Size(layout).Obs, Vi::ENTITY_SLOTS * Block::ENTITY_FEATURES);
    EXPECT_EQ(block.Size(layout).Actions, 0u);
    EXPECT_EQ(Block::ENTITY_FEATURES, 20u);

    boost::json::object entry;
    block.DescribeManifest(layout, entry);
    boost::json::object const& set = entry.at("entities").as_object();
    EXPECT_EQ(set.at("name").as_string(), "visible");
    EXPECT_EQ(set.at("slots").to_number<uint32>(), 32u);
    EXPECT_EQ(set.at("width").to_number<uint32>(), Block::ENTITY_FEATURES);
    EXPECT_EQ(set.at("present").to_number<uint32>(), uint32(Block::ENTITY_PRESENT));
    EXPECT_EQ(set.at("class_column").to_number<uint32>(), uint32(Block::ENTITY_CLASS));
    EXPECT_EQ(set.at("type_column").to_number<uint32>(), uint32(Block::ENTITY_TYPE));
    EXPECT_EQ(set.at("classes").to_number<uint32>(), Vi::CLASS_LIMIT);
    EXPECT_EQ(set.at("features").as_array().size(), std::size_t(Block::ENTITY_FEATURES));
}

// Every stage with a camera has the entity list right after it; no other stage has one.
TEST(VisionEntitiesTest, ACameraBringsItsEntityList)
{
    bool any = false;
    for (Cu::StageDefinition const& stage : Cu::CurriculumStages())
    {
        auto const vision = std::find(stage.Blocks.begin(), stage.Blocks.end(), Cu::BlockId::Vision);
        auto const entities = std::find(stage.Blocks.begin(), stage.Blocks.end(), Cu::BlockId::Entities);
        if (vision == stage.Blocks.end())
        {
            EXPECT_EQ(entities, stage.Blocks.end()) << stage.Name;
            continue;
        }
        any = true;
        ASSERT_NE(entities, stage.Blocks.end()) << stage.Name;
        EXPECT_EQ(entities, vision + 1) << stage.Name;
    }
    EXPECT_TRUE(any) << "no stage has a camera";
}

// A slot's features from a frame's list: what it is (the class and type raw, for the learner's embeddings), the UI
// facts, its distance and direction from the camera's view, its pixels' centroid and share; the slots past the list
// are all zeros, so nothing is listed without a pixel.
TEST(VisionEntitiesTest, SlotsCarryThePlansFields)
{
    Vi::SeenList seen;
    seen.Count = 2;
    seen.CastWidth = 64;
    seen.CastHeight = 32;
    seen.Camera = { 0.0f, 0.0f, 2.0f };
    seen.Azimuth = 0.0f;            // looking along +x
    seen.Elevation = 0.0f;
    seen.SeatLevel = 10.0f;

    // Slot 1: a hostile level-12 creature 10 yd ahead and 10 yd to the left, at the camera's height.
    Vi::EntityInfo& mob = seen.Info[0];
    mob.Id.What = Vi::Class::HostileCreature;
    mob.Id.Quest = true;
    mob.Entry = 1234;
    mob.Level = 12.0f;
    mob.Health = 0.5f;
    mob.Reaction = -1;
    mob.Centre = { 10.0f, 10.0f, 2.0f };
    seen.Stats[0] = { 7, 4, 4 * 10, 4 * 20 };     // number 7: 4 pixels at row 10, column 20
    // Slot 2: a lootable chest straight ahead and below.
    Vi::EntityInfo& chest = seen.Info[1];
    chest.Id.What = Vi::Class::Chest;
    chest.Id.Lootable = true;
    chest.Id.Usable = true;
    chest.Entry = 2843;
    chest.GameObject = true;
    chest.Centre = { 5.0f, 0.0f, -3.0f };
    seen.Stats[1] = { 9, 2, 2 * 31, 2 * 63 };

    std::vector<float> obs(Vi::ENTITY_SLOTS * Block::ENTITY_FEATURES, 7.0f);
    Block::Write(seen, obs.data());
    float const* a = obs.data();
    EXPECT_FLOAT_EQ(a[Block::ENTITY_PRESENT], 1.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_CLASS], float(Vi::Class::HostileCreature));
    EXPECT_FLOAT_EQ(a[Block::ENTITY_TYPE], 1234.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_OBJECT], 0.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_LEVEL], 12.0f / 80.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_LEVEL_DELTA], 0.2f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_HEALTH], 0.5f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_REACTION], -1.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_QUEST], 1.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_LOOTABLE], 0.0f);
    float const distance = std::sqrt(200.0f);
    EXPECT_NEAR(a[Block::ENTITY_DISTANCE], std::log(distance / Vi::NEAR) / std::log(1000.0f / Vi::NEAR), 1e-6f);
    EXPECT_NEAR(a[Block::ENTITY_YAW_SIN], std::sin(Vi::PI / 4.0f), 1e-6f);      // + left
    EXPECT_NEAR(a[Block::ENTITY_YAW_COS], std::cos(Vi::PI / 4.0f), 1e-6f);
    EXPECT_NEAR(a[Block::ENTITY_PITCH_SIN], 0.0f, 1e-6f);
    EXPECT_NEAR(a[Block::ENTITY_PITCH_COS], 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_CENTROID_X], 2.0f * 20.5f / 64.0f - 1.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_CENTROID_Y], 1.0f - 2.0f * 10.5f / 32.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_SHARE], 4.0f / 2048.0f);
    EXPECT_FLOAT_EQ(a[Block::ENTITY_MEMORY], 0.0f);

    float const* b = obs.data() + Block::ENTITY_FEATURES;
    EXPECT_FLOAT_EQ(b[Block::ENTITY_CLASS], float(Vi::Class::Chest));
    EXPECT_FLOAT_EQ(b[Block::ENTITY_OBJECT], 1.0f);
    EXPECT_FLOAT_EQ(b[Block::ENTITY_LEVEL], 0.0f);
    EXPECT_FLOAT_EQ(b[Block::ENTITY_LEVEL_DELTA], 0.0f);
    EXPECT_FLOAT_EQ(b[Block::ENTITY_HEALTH], 1.0f);
    EXPECT_FLOAT_EQ(b[Block::ENTITY_LOOTABLE], 1.0f);
    EXPECT_FLOAT_EQ(b[Block::ENTITY_USABLE], 1.0f);
    EXPECT_NEAR(b[Block::ENTITY_YAW_SIN], 0.0f, 1e-6f);
    EXPECT_LT(b[Block::ENTITY_PITCH_SIN], -0.5f);      // below the view
    EXPECT_GT(b[Block::ENTITY_CENTROID_X], 0.9f);
    EXPECT_LT(b[Block::ENTITY_CENTROID_Y], -0.9f);

    // Past the list: nothing.
    for (std::size_t i = 2 * Block::ENTITY_FEATURES; i < obs.size(); ++i)
        ASSERT_FLOAT_EQ(obs[i], 0.0f) << i;
    seen.Count = 0;
    Block::Write(seen, obs.data());
    EXPECT_TRUE(std::all_of(obs.begin(), obs.end(), [](float value) { return value == 0.0f; }));
}
