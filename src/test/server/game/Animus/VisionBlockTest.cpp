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
#include "Camera.h"
#include "FreeLook.h"
#include "Layout.h"
#include "gtest/gtest.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

namespace Cu = Animus::Curriculum;
namespace Vi = Animus::Vision;

// The vision block of revision 5 (camera-vision.BYTES.md, FREELOOK.md, perception-goals 1a): its float columns are
// the eleven scalars, and both the layout manifest and stage.json (which copies the manifest's "image" and "look")
// say the image travels as 5 bytes a pixel with the class table, the learner's patch, the render sizes and the look
// head's three categoricals.
TEST(VisionBlockTest, ScalarsAloneAndTheImageAsBytes)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Vision);
    Cu::Layout layout;
    EXPECT_EQ(block.Size(layout).Obs, 11u);
    EXPECT_EQ(block.Size(layout).Actions, 0u);
    EXPECT_EQ(block.Revision(), 5u);
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::Vision), "vision");

    boost::json::object entry;
    block.DescribeManifest(layout, entry);
    ASSERT_TRUE(entry.contains("image"));
    boost::json::object const& image = entry.at("image").as_object();
    Vi::Settings const& settings = Vi::Current();
    EXPECT_EQ(image.at("height").to_number<uint32>(), settings.Height);
    EXPECT_EQ(image.at("width").to_number<uint32>(), settings.Width);
    EXPECT_EQ(image.at("channels").to_number<uint32>(), 5u);
    EXPECT_EQ(image.at("classes").to_number<uint32>(), Vi::CLASSES);
    EXPECT_EQ(image.at("class_limit").to_number<uint32>(), 32u);
    EXPECT_EQ(image.at("class_channel").to_number<uint32>(), 3u);
    EXPECT_EQ(image.at("class_byte").to_number<uint32>(), 3u);
    EXPECT_EQ(image.at("slot_byte").to_number<uint32>(), 4u);
    EXPECT_EQ(image.at("entity_slots").to_number<uint32>(), 32u);
    EXPECT_EQ(image.at("scalars").to_number<uint32>(), 11u);
    EXPECT_EQ(image.at("transport").as_string(), "bytes");
    EXPECT_EQ(image.at("bytes_per_pixel").to_number<uint32>(), 5u);
    boost::json::array const& classNames = image.at("class_names").as_array();
    boost::json::array const& kinds = image.at("class_kinds").as_array();
    ASSERT_EQ(classNames.size(), Vi::CLASSES);
    ASSERT_EQ(kinds.size(), Vi::CLASSES);
    EXPECT_EQ(classNames[0].as_string(), "sky");
    EXPECT_EQ(classNames[uint32(Vi::Class::QuestGiver)].as_string(), "quest_giver");
    EXPECT_EQ(classNames[uint32(Vi::Class::OtherObject)].as_string(), "other_object");
    EXPECT_EQ(kinds[uint32(Vi::Class::HostilePlayer)].to_number<uint32>(), uint32(Vi::Kind::Hostile));
    EXPECT_EQ(image.at("patch").to_number<uint32>(), Vi::Patch(settings));

    boost::json::array const& sizes = image.at("render_sizes").as_array();
    ASSERT_EQ(sizes.size(), settings.RenderSizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        boost::json::array const& size = sizes[i].as_array();
        ASSERT_EQ(size.size(), 2u);
        EXPECT_EQ(size[0].to_number<uint32>(), settings.RenderSizes[i].Width);
        EXPECT_EQ(size[1].to_number<uint32>(), settings.RenderSizes[i].Height);
    }
    boost::json::array const& weights = image.at("render_weights").as_array();
    ASSERT_EQ(weights.size(), settings.RenderSizes.size());
    for (std::size_t i = 0; i < weights.size(); ++i)
        EXPECT_FLOAT_EQ(float(weights[i].to_number<double>()), settings.RenderWeights[i]);

    ASSERT_TRUE(entry.contains("look"));
    boost::json::object const& look = entry.at("look").as_object();
    boost::json::array const& heads = look.at("heads").as_array();
    boost::json::array const& names = look.at("names").as_array();
    ASSERT_EQ(heads.size(), 3u);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(heads[0].to_number<uint32>(), 7u);
    EXPECT_EQ(heads[1].to_number<uint32>(), 5u);
    EXPECT_EQ(heads[2].to_number<uint32>(), 5u);
    EXPECT_EQ(names[0].as_string(), "yaw_rate");
    EXPECT_EQ(names[1].as_string(), "pitch_rate");
    EXPECT_EQ(names[2].as_string(), "zoom");
    EXPECT_EQ(entry.at("camera").as_object().at("mode").as_string(), "free, never adjust");
}

// The contract's defaults (FREELOOK A): a 128 x 64 canonical image, 40 KB an agent at five bytes a pixel, patch 8
// (a 16 x 8 grid), and four weighted render sizes within it, the native 128 x 64 at 0.4; 64 x 32 keeps the old patch of 4.
TEST(VisionBlockTest, CanonicalDefaults)
{
    Vi::Settings settings;
    EXPECT_EQ(settings.Width, 128u);
    EXPECT_EQ(settings.Height, 64u);
    EXPECT_EQ(Vi::ImageBytes(settings), 40960u);
    EXPECT_EQ(Vi::Patch(settings), 8u);
    EXPECT_EQ(settings.Width / Vi::Patch(settings), 16u);
    EXPECT_EQ(settings.Height / Vi::Patch(settings), 8u);
    ASSERT_EQ(settings.RenderSizes.size(), 4u);
    EXPECT_EQ(settings.RenderSizes[0], (Vi::Resolution{ 32, 16 }));
    EXPECT_EQ(settings.RenderSizes[1], (Vi::Resolution{ 48, 24 }));
    EXPECT_EQ(settings.RenderSizes[2], (Vi::Resolution{ 64, 32 }));
    EXPECT_EQ(settings.RenderSizes[3], (Vi::Resolution{ 128, 64 }));
    EXPECT_EQ(settings.RenderWeights, (std::vector<float>{ 1.0f, 1.0f, 1.0f, 0.4f }));
    EXPECT_EQ(Vi::SCALARS, 11u);

    settings.Width = 64;
    settings.Height = 32;
    EXPECT_EQ(Vi::Patch(settings), 4u);
}
