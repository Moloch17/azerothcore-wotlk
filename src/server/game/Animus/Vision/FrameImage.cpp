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
#include <algorithm>
#include <cmath>
#include <vector>
#include <zlib.h>

namespace Animus::Vision
{
    uint8_t const KIND_COLOURS[KINDS][3] = {
        { 30, 30, 80 },     // sky
        { 60, 160, 60 },    // terrain
        { 160, 160, 160 },  // model
        { 170, 100, 40 },   // door or game object
        { 40, 90, 220 },    // water
        { 240, 80, 0 },     // deadly liquid
        { 220, 0, 0 },      // hostile unit
        { 230, 230, 0 },    // other unit
    };

    char const* const KIND_NAMES[KINDS] = { "sky", "terrain", "model", "door", "water", "deadly", "hostile",
        "other" };
}

namespace
{
    namespace Vi = Animus::Vision;

    constexpr uint8_t GAP_GREY = 64;
    constexpr uint32_t PANELS = 4;

    uint8_t Grey(float value)
    {
        return uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    void Put32(std::string& out, uint32_t value)
    {
        out += char((value >> 24) & 0xFF);
        out += char((value >> 16) & 0xFF);
        out += char((value >> 8) & 0xFF);
        out += char(value & 0xFF);
    }

    void Chunk(std::string& out, char const* tag, std::string const& body)
    {
        Put32(out, uint32_t(body.size()));
        std::string tagged(tag, 4);
        tagged += body;
        out += tagged;
        Put32(out, uint32_t(crc32(0L, reinterpret_cast<Bytef const*>(tagged.data()), uInt(tagged.size()))));
    }

    /// An RGB PNG of `width` x `height` from its scanlines, each already led by filter byte 0; empty on failure.
    std::string WritePng(uint32_t width, uint32_t height, std::string const& raw)
    {
        uLongf packedSize = compressBound(uLong(raw.size()));
        std::string packed(packedSize, '\0');
        if (compress2(reinterpret_cast<Bytef*>(packed.data()), &packedSize, reinterpret_cast<Bytef const*>(raw.data()),
            uLong(raw.size()), Z_BEST_SPEED) != Z_OK)
            return {};
        packed.resize(packedSize);

        std::string png("\x89PNG\r\n\x1a\n", 8);
        std::string header;
        Put32(header, width);
        Put32(header, height);
        header += char(8);  // bit depth
        header += char(2);  // RGB
        header += std::string(3, '\0');     // deflate, adaptive filtering, no interlace
        Chunk(png, "IHDR", header);
        Chunk(png, "IDAT", packed);
        Chunk(png, "IEND", {});
        return png;
    }
}

std::array<uint32_t, Vi::KINDS> Vi::KindCounts(Settings const& settings, uint8_t const* image)
{
    std::array<uint32_t, KINDS> counts{};
    uint32_t const pixels = settings.Width * settings.Height;
    for (uint32_t pixel = 0; pixel < pixels; ++pixel)
        ++counts[std::min<uint32_t>(image[std::size_t(pixel) * BYTES_PER_PIXEL + 3] & 15u, KINDS - 1)];
    return counts;
}

std::string Vi::FramePng(Settings const& settings, uint8_t const* image, uint32_t scale)
{
    scale = std::max<uint32_t>(scale, 1);
    uint32_t const width = settings.Width;
    uint32_t const height = settings.Height;
    uint32_t const panelWidth = width * scale;
    uint32_t const outWidth = panelWidth * PANELS + PANEL_GAP * (PANELS - 1);
    uint32_t const outHeight = height * scale;

    // Each source pixel's four panel colours, decoded as the learner decodes the bytes.
    std::vector<std::array<uint8_t, 3 * PANELS>> colours(std::size_t(width) * height);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel)
    {
        float decoded[CHANNELS];
        DecodePixel(&image[std::size_t(pixel) * BYTES_PER_PIXEL], decoded);
        std::array<uint8_t, 3 * PANELS>& out = colours[pixel];
        uint8_t const depth = Grey(decoded[CHANNEL_DISTANCE]);
        uint8_t const rise = Grey((decoded[CHANNEL_HEIGHT] + 1.0f) / 2.0f);
        uint8_t const slope = Grey(decoded[CHANNEL_NORMAL]);
        uint32_t const kind = std::min<uint32_t>(uint32_t(decoded[CHANNEL_KIND]), KINDS - 1);
        bool const objective = decoded[CHANNEL_OBJECTIVE] > 0.5f;
        for (uint32_t c = 0; c < 3; ++c)
        {
            out[c] = depth;
            out[3 + c] = objective ? 255 : KIND_COLOURS[kind][c];
            out[6 + c] = rise;
            out[9 + c] = slope;
        }
    }

    // The scanlines, each led by filter byte 0 (none).
    std::string raw;
    raw.reserve(std::size_t(outHeight) * (1 + std::size_t(outWidth) * 3));
    for (uint32_t y = 0; y < outHeight; ++y)
    {
        raw += char(0);
        uint32_t const row = y / scale;
        for (uint32_t panel = 0; panel < PANELS; ++panel)
        {
            if (panel)
                raw.append(std::size_t(PANEL_GAP) * 3, char(GAP_GREY));
            for (uint32_t x = 0; x < panelWidth; ++x)
            {
                std::array<uint8_t, 3 * PANELS> const& source = colours[std::size_t(row) * width + x / scale];
                for (uint32_t c = 0; c < 3; ++c)
                    raw += char(source[panel * 3 + c]);
            }
        }
    }

    return WritePng(outWidth, outHeight, raw);
}

std::string Vi::CompositePng(Settings const& settings, uint8_t const* image, uint32_t scale)
{
    scale = std::max<uint32_t>(scale, 1);
    uint32_t const width = settings.Width;
    uint32_t const height = settings.Height;

    // Every pixel decoded once, as the learner decodes the bytes.
    std::vector<std::array<float, CHANNELS>> decoded(std::size_t(width) * height);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel)
        DecodePixel(&image[std::size_t(pixel) * BYTES_PER_PIXEL], decoded[pixel].data());
    auto const rise = [&](uint32_t pixel) { return decoded[pixel][CHANNEL_HEIGHT] * HEIGHT_SCALE; };
    auto const level = [](float yards) { return int32_t(std::floor((yards + 100.0f) / COMPOSITE_CONTOUR)); };

    std::vector<std::array<uint8_t, 3>> colours(std::size_t(width) * height);
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col)
        {
            uint32_t const pixel = row * width + col;
            std::array<float, CHANNELS> const& in = decoded[pixel];
            std::array<uint8_t, 3>& out = colours[pixel];
            if (in[CHANNEL_OBJECTIVE] > 0.5f)
            {
                out = { 255, 255, 255 };
                continue;
            }
            if (in[CHANNEL_DISTANCE] >= 0.999f)     // sky, or nothing loaded that way
            {
                out = { COMPOSITE_SKY[0], COMPOSITE_SKY[1], COMPOSITE_SKY[2] };
                continue;
            }

            // Back from the log encoding to yards; floors lit, walls dark; far fades towards the haze.
            float const yards = NEAR * std::pow(DISTANCE_REFERENCE / NEAR, in[CHANNEL_DISTANCE]);
            float const shade = 0.45f + 0.55f * std::clamp(in[CHANNEL_NORMAL], 0.0f, 1.0f);
            float const fog = std::pow(std::min(1.0f, yards / COMPOSITE_FOG_YARDS), 0.7f);
            uint32_t const kind = std::min<uint32_t>(uint32_t(in[CHANNEL_KIND]), KINDS - 1);
            float colour[3];
            for (uint32_t c = 0; c < 3; ++c)
                colour[c] = float(KIND_COLOURS[kind][c]) * shade * (1.0f - fog) + float(COMPOSITE_HAZE[c]) * fog * 0.6f;

            // A contour where this pixel and its right or lower neighbour lie across a level, inside the height
            // channel's range (its clamped ends are no level at all).
            float const here = rise(pixel);
            bool const across = std::fabs(here) < HEIGHT_SCALE - 0.5f
                && ((col + 1 < width && level(here) != level(rise(pixel + 1)))
                    || (row + 1 < height && level(here) != level(rise(pixel + width))));
            for (uint32_t c = 0; c < 3; ++c)
                out[c] = uint8_t(std::clamp(colour[c] * (across ? 0.35f : 1.0f), 0.0f, 255.0f));
        }

    uint32_t const outWidth = width * scale;
    uint32_t const outHeight = height * scale;
    std::string raw;
    raw.reserve(std::size_t(outHeight) * (1 + std::size_t(outWidth) * 3));
    for (uint32_t y = 0; y < outHeight; ++y)
    {
        raw += char(0);
        for (uint32_t x = 0; x < outWidth; ++x)
            for (uint8_t c : colours[std::size_t(y / scale) * width + x / scale])
                raw += char(c);
    }
    return WritePng(outWidth, outHeight, raw);
}
