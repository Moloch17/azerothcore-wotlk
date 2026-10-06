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

#include "FrameImage.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <vector>
#include <zlib.h>

namespace Vi = Animus::Vision;

namespace
{
    uint32_t Read32(std::string const& data, std::size_t at)
    {
        return (uint32_t(uint8_t(data[at])) << 24) | (uint32_t(uint8_t(data[at + 1])) << 16)
            | (uint32_t(uint8_t(data[at + 2])) << 8) | uint32_t(uint8_t(data[at + 3]));
    }

    /// The PNG's pixels, RGB a pixel, row by row (FramePng writes one IDAT and filter 0 only).
    std::vector<uint8_t> Pixels(std::string const& png, uint32_t& width, uint32_t& height)
    {
        width = Read32(png, 16);
        height = Read32(png, 20);
        std::size_t at = 8;
        std::string packed;
        while (at + 8 <= png.size())
        {
            uint32_t const length = Read32(png, at);
            std::string const tag = png.substr(at + 4, 4);
            if (tag == "IDAT")
                packed += png.substr(at + 8, length);
            at += 12 + length;
        }
        std::vector<uint8_t> raw(std::size_t(height) * (1 + std::size_t(width) * 3));
        uLongf size = uLongf(raw.size());
        EXPECT_EQ(uncompress(raw.data(), &size, reinterpret_cast<Bytef const*>(packed.data()), uLong(packed.size())),
            Z_OK);
        std::vector<uint8_t> rgb;
        for (uint32_t y = 0; y < height; ++y)
        {
            EXPECT_EQ(raw[std::size_t(y) * (1 + width * 3)], 0);
            uint8_t const* row = &raw[std::size_t(y) * (1 + width * 3) + 1];
            rgb.insert(rgb.end(), row, row + width * 3);
        }
        return rgb;
    }
}

TEST(VisionFrameImageTest, FourPanelsAsTheLearnerDecodesThem)
{
    Vi::Settings settings;
    settings.Width = 8;
    settings.Height = 4;
    std::vector<uint8_t> image(Vi::ImageBytes(settings));
    Vi::FillNoFrame(image.data(), uint32_t(image.size()));

    // Pixel (row 1, col 2): level terrain 10 yd off, 2 yd under the feet. Pixel (row 2, col 5): the objective, on
    // a wall.
    Vi::Hit ground;
    ground.Distance = 10.0f;
    ground.What = Vi::Kind::Terrain;
    ground.Z = -2.0f;
    ground.NormalZ = 1.0f;
    Vi::EncodePixel(ground, 0.0f, false, &image[(1 * 8 + 2) * Vi::BYTES_PER_PIXEL]);
    Vi::Hit wall;
    wall.Distance = 5.0f;
    wall.What = Vi::Kind::Model;
    wall.Z = 1.0f;
    wall.NormalZ = 0.0f;
    Vi::EncodePixel(wall, 0.0f, true, &image[(2 * 8 + 5) * Vi::BYTES_PER_PIXEL]);

    std::array<uint32_t, Vi::KINDS> const kinds = Vi::KindCounts(settings, image.data());
    EXPECT_EQ(kinds[uint32_t(Vi::Kind::Sky)], 30u);
    EXPECT_EQ(kinds[uint32_t(Vi::Kind::Terrain)], 1u);
    EXPECT_EQ(kinds[uint32_t(Vi::Kind::Model)], 1u);

    uint32_t const scale = 3;
    std::string const png = Vi::FramePng(settings, image.data(), scale);
    ASSERT_GT(png.size(), 33u);
    EXPECT_EQ(png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> const rgb = Pixels(png, width, height);
    uint32_t const panel = 8 * scale;
    ASSERT_EQ(width, panel * 4 + Vi::PANEL_GAP * 3);
    ASSERT_EQ(height, 4 * scale);

    // Output pixel (x, y) of panel p, at the centre of source pixel (row, col).
    auto const at = [&](uint32_t p, uint32_t row, uint32_t col)
    {
        uint32_t const x = p * (panel + Vi::PANEL_GAP) + col * scale + scale / 2;
        uint32_t const y = row * scale + scale / 2;
        return &rgb[(std::size_t(y) * width + x) * 3];
    };

    // Sky: white depth, the sky colour, mid-grey height, black slope.
    EXPECT_EQ(at(0, 0, 0)[0], 255);
    EXPECT_EQ(at(1, 0, 0)[0], Vi::KIND_COLOURS[0][0]);
    EXPECT_EQ(at(1, 0, 0)[2], Vi::KIND_COLOURS[0][2]);
    EXPECT_NEAR(at(2, 0, 0)[0], 128, 1);
    EXPECT_EQ(at(3, 0, 0)[0], 0);

    // The ground: nearer than sky, terrain green, darker than the feet, level.
    EXPECT_LT(at(0, 1, 2)[0], 255);
    EXPECT_EQ(at(1, 1, 2)[1], Vi::KIND_COLOURS[uint32_t(Vi::Kind::Terrain)][1]);
    EXPECT_LT(at(2, 1, 2)[0], 128);
    EXPECT_EQ(at(3, 1, 2)[0], 255);

    // The objective is white in the kind panel; the wall it is on reads as one in the slope panel.
    EXPECT_EQ(at(1, 2, 5)[0], 255);
    EXPECT_EQ(at(1, 2, 5)[1], 255);
    EXPECT_EQ(at(1, 2, 5)[2], 255);
    EXPECT_EQ(at(3, 2, 5)[0], 0);
    EXPECT_GT(at(2, 2, 5)[0], 128);

    // The gap between panels is grey.
    EXPECT_EQ(rgb[(std::size_t(0) * width + panel) * 3], 64);
}
