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
#include "Layout.h"
#include "gtest/gtest.h"
#include <boost/json/object.hpp>

namespace Cu = Animus::Curriculum;
namespace Vi = Animus::Vision;

// The vision block of revision 3 (camera-vision.BYTES.md): its float columns are the seven scalars, and both the
// layout manifest and stage.json (which copies the manifest's "image") say the image travels as 4 bytes a pixel.
TEST(VisionBlockTest, ScalarsAloneAndTheImageAsBytes)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Vision);
    Cu::Layout layout;
    EXPECT_EQ(block.Size(layout).Obs, 7u);
    EXPECT_EQ(block.Size(layout).Actions, 0u);
    EXPECT_EQ(block.Revision(), 3u);
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::Vision), "vision");

    boost::json::object entry;
    block.DescribeManifest(layout, entry);
    ASSERT_TRUE(entry.contains("image"));
    boost::json::object const& image = entry.at("image").as_object();
    Vi::Settings const& settings = Vi::Current();
    EXPECT_EQ(image.at("height").to_number<uint32>(), settings.Height);
    EXPECT_EQ(image.at("width").to_number<uint32>(), settings.Width);
    EXPECT_EQ(image.at("channels").to_number<uint32>(), 5u);
    EXPECT_EQ(image.at("kinds").to_number<uint32>(), 8u);
    EXPECT_EQ(image.at("kind_channel").to_number<uint32>(), 3u);
    EXPECT_EQ(image.at("scalars").to_number<uint32>(), 7u);
    EXPECT_EQ(image.at("transport").as_string(), "bytes");
    EXPECT_EQ(image.at("bytes_per_pixel").to_number<uint32>(), 4u);
}
