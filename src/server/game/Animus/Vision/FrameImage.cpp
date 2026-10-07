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
#include "MentalMap.h"
#include "VisionCaster.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <zlib.h>

namespace Animus::Vision
{
    uint8_t const CLASS_COLOURS[CLASSES][3] = {
        { 30, 30, 80 },     // sky
        { 60, 160, 60 },    // terrain
        { 160, 160, 160 },  // model
        { 170, 100, 40 },   // door
        { 40, 90, 220 },    // water
        { 240, 80, 0 },     // deadly liquid
        { 220, 0, 0 },      // hostile creature
        { 230, 230, 0 },    // neutral creature
        { 0, 230, 160 },    // friendly creature
        { 255, 0, 170 },    // hostile player
        { 0, 160, 255 },    // friendly player
        { 255, 170, 0 },    // quest giver
        { 170, 90, 230 },   // vendor
        { 110, 60, 180 },   // trainer
        { 255, 120, 120 },  // lootable corpse
        { 110, 40, 40 },    // corpse
        { 200, 150, 60 },   // chest
        { 120, 255, 80 },   // herb
        { 120, 180, 200 },  // ore
        { 40, 40, 200 },    // mailbox
        { 255, 255, 140 },  // quest object
        { 0, 255, 255 },    // usable object
        { 120, 90, 60 },    // other object
        { 255, 40, 220 },   // ground hazard
    };
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

    void Put16(std::string& out, uint16_t value)
    {
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

    /// The scanlines deflated for an APNG frame: fast, run-length matching (the filtered rows are mostly zero runs);
    /// empty on failure.
    std::string Deflate(std::string const& raw)
    {
        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 15, 8, Z_RLE) != Z_OK)
            return {};
        std::string packed(deflateBound(&stream, uLong(raw.size())), '\0');
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
        stream.avail_in = uInt(raw.size());
        stream.next_out = reinterpret_cast<Bytef*>(packed.data());
        stream.avail_out = uInt(packed.size());
        int const result = deflate(&stream, Z_FINISH);
        packed.resize(stream.total_out);
        deflateEnd(&stream);
        return result == Z_STREAM_END ? packed : std::string();
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

std::array<uint8_t, 3> Vi::MapColour(uint8_t const* cell)
{
    static uint8_t const CODES[MAP_CODES][3] = { { 18, 18, 24 }, { 60, 150, 60 }, { 210, 210, 210 }, { 220, 130, 40 },
        { 40, 90, 220 } };
    uint8_t const code = std::min<uint8_t>(cell[CROP_CODE], uint8_t(MAP_CODES - 1));
    std::array<uint8_t, 3> out = { CODES[code][0], CODES[code][1], CODES[code][2] };
    if (code == uint8_t(MapCode::Floor))
    {
        // Newer looks brighter: full at once, half by an hour.
        float const fresh = 1.0f - 0.5f * std::min(1.0f, float(cell[CROP_AGE]) / 236.0f);
        for (uint8_t& c : out)
            c = uint8_t(float(c) * fresh);
        if (cell[CROP_VISITED])
            out = { 40, 200, 200 };
    }
    if (cell[CROP_FRONTIER])
        out = { 240, 220, 40 };
    if (uint8_t const what = cell[CROP_CLASS] & CLASS_MASK; what && what < CLASSES)
        out = { CLASS_COLOURS[what][0], CLASS_COLOURS[what][1], CLASS_COLOURS[what][2] };
    return out;
}

std::vector<std::array<uint8_t, 3>> Vi::MapPanel(uint8_t const* map, uint32_t side)
{
    std::vector<std::array<uint8_t, 3>> out(std::size_t(side) * side);
    for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x < side; ++x)
        {
            uint32_t const row = y * CROP / side;
            uint32_t const col = x * CROP / side;
            // The body: the four cells round the centre's corner.
            bool const body = (row == CROP / 2 - 1 || row == CROP / 2) && (col == CROP / 2 - 1 || col == CROP / 2);
            out[std::size_t(y) * side + x] = body ? std::array<uint8_t, 3>{ 230, 30, 30 }
                : MapColour(map + (std::size_t(row) * CROP + col) * CROP_CHANNELS);
        }
    return out;
}

std::array<uint32_t, Vi::CLASSES> Vi::ClassCounts(Settings const& settings, uint8_t const* image)
{
    std::array<uint32_t, CLASSES> counts{};
    uint32_t const pixels = settings.Width * settings.Height;
    for (uint32_t pixel = 0; pixel < pixels; ++pixel)
        ++counts[std::min<uint32_t>(image[std::size_t(pixel) * BYTES_PER_PIXEL + CLASS_BYTE] & CLASS_MASK,
            CLASSES - 1)];
    return counts;
}

std::string Vi::FramePng(Settings const& settings, uint8_t const* image, uint32_t scale, uint8_t const* map)
{
    scale = std::max<uint32_t>(scale, 1);
    uint32_t const width = settings.Width;
    uint32_t const height = settings.Height;
    uint32_t const panelWidth = width * scale;
    uint32_t const outHeight = height * scale;
    // The map panel, square and as tall as the others.
    uint32_t const mapSide = map ? outHeight : 0;
    std::vector<std::array<uint8_t, 3>> const mapPanel = map ? MapPanel(map, mapSide) : std::vector<std::array<uint8_t,
        3>>();
    uint32_t const outWidth = panelWidth * PANELS + PANEL_GAP * (PANELS - 1) + (map ? PANEL_GAP + mapSide : 0);

    // Each source pixel's four panel colours, decoded as the learner decodes the bytes.
    std::vector<std::array<uint8_t, 3 * PANELS>> colours(std::size_t(width) * height);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel)
    {
        float decoded[DECODED_VALUES];
        DecodePixel(&image[std::size_t(pixel) * BYTES_PER_PIXEL], decoded);
        std::array<uint8_t, 3 * PANELS>& out = colours[pixel];
        uint8_t const depth = Grey(decoded[CHANNEL_DISTANCE]);
        uint8_t const rise = Grey((decoded[CHANNEL_HEIGHT] + 1.0f) / 2.0f);
        uint8_t const slope = Grey(decoded[CHANNEL_NORMAL]);
        uint32_t const what = std::min<uint32_t>(uint32_t(decoded[CHANNEL_CLASS]), CLASSES - 1);
        bool const objective = decoded[CHANNEL_OBJECTIVE] > 0.5f;
        for (uint32_t c = 0; c < 3; ++c)
        {
            out[c] = depth;
            out[3 + c] = objective ? 255 : CLASS_COLOURS[what][c];
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
        if (map)
        {
            raw.append(std::size_t(PANEL_GAP) * 3, char(GAP_GREY));
            for (uint32_t x = 0; x < mapSide; ++x)
                for (uint8_t c : mapPanel[std::size_t(y) * mapSide + x])
                    raw += char(c);
        }
    }

    return WritePng(outWidth, outHeight, raw);
}

std::string Vi::CompositePng(Settings const& settings, uint8_t const* image, uint32_t scale, uint8_t const* map)
{
    return RgbPng(CompositeRgb(settings, image, scale, map));
}

Vi::RgbImage Vi::CompositeRgb(Settings const& settings, uint8_t const* image, uint32_t scale, uint8_t const* map)
{
    scale = std::max<uint32_t>(scale, 1);
    uint32_t const width = settings.Width;
    uint32_t const height = settings.Height;

    // Every pixel decoded once, as the learner decodes the bytes.
    std::vector<std::array<float, DECODED_VALUES>> decoded(std::size_t(width) * height);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel)
        DecodePixel(&image[std::size_t(pixel) * BYTES_PER_PIXEL], decoded[pixel].data());
    auto const rise = [&](uint32_t pixel) { return decoded[pixel][CHANNEL_HEIGHT] * HEIGHT_SCALE; };
    auto const level = [](float yards) { return int32_t(std::floor((yards + 100.0f) / COMPOSITE_CONTOUR)); };

    std::vector<std::array<uint8_t, 3>> colours(std::size_t(width) * height);
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col)
        {
            uint32_t const pixel = row * width + col;
            std::array<float, DECODED_VALUES> const& in = decoded[pixel];
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
            uint32_t const what = std::min<uint32_t>(uint32_t(in[CHANNEL_CLASS]), CLASSES - 1);
            float colour[3];
            for (uint32_t c = 0; c < 3; ++c)
                colour[c] = float(CLASS_COLOURS[what][c]) * shade * (1.0f - fog)
                    + float(COMPOSITE_HAZE[c]) * fog * 0.6f;

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
    // The mini-map inset: half the picture's height, in its top right corner, a grey frame round it.
    uint32_t const side = map ? std::max<uint32_t>(8, outHeight / 2) : 0;
    uint32_t const left = outWidth > side + 2 ? outWidth - side - 2 : 0;
    std::vector<std::array<uint8_t, 3>> const inset = map ? MapPanel(map, side) : std::vector<std::array<uint8_t,
        3>>();
    RgbImage out;
    out.Width = outWidth;
    out.Height = outHeight;
    out.Pixels.resize(std::size_t(outHeight) * outWidth * 3);
    uint8_t* write = out.Pixels.data();
    for (uint32_t y = 0; y < outHeight; ++y)
        for (uint32_t x = 0; x < outWidth; ++x)
        {
            std::array<uint8_t, 3> pixel = colours[std::size_t(y / scale) * width + x / scale];
            if (map && x + 1 >= left && x <= left + side && y <= side + 1)
            {
                bool const frame = x + 1 == left || x == left + side || y == side + 1;
                pixel = frame ? std::array<uint8_t, 3>{ 128, 128, 128 }
                    : x >= left && y >= 1 && y - 1 < side ? inset[std::size_t(y - 1) * side + (x - left)] : pixel;
            }
            for (uint8_t c : pixel)
                *write++ = c;
        }
    return out;
}

std::string Vi::RgbPng(RgbImage const& picture)
{
    std::size_t const stride = std::size_t(picture.Width) * 3;
    if (picture.Pixels.size() != stride * picture.Height)
        return {};
    std::string raw;
    raw.reserve(std::size_t(picture.Height) * (1 + stride));
    for (uint32_t y = 0; y < picture.Height; ++y)
    {
        raw += char(0);
        raw.append(reinterpret_cast<char const*>(&picture.Pixels[y * stride]), stride);
    }
    return WritePng(picture.Width, picture.Height, raw);
}

Vi::ApngWriter::ApngWriter(uint32_t width, uint32_t height, uint16_t delayNumerator, uint16_t delayDenominator)
    : _width(width), _height(height), _delayNumerator(delayNumerator), _delayDenominator(delayDenominator)
{
}

bool Vi::ApngWriter::Add(RgbImage const& frame)
{
    std::size_t const stride = std::size_t(_width) * 3;
    if (frame.Width != _width || frame.Height != _height || frame.Pixels.size() != stride * _height || !stride)
        return false;

    // Filter 2 (Up) for a row the same as the one above -- all zeros, which deflate all but drops -- and 1 (Sub)
    // otherwise, which zeroes the runs a nearest-pixel scale-up repeats.
    std::string raw(std::size_t(_height) * (1 + stride), '\0');
    for (uint32_t y = 0; y < _height; ++y)
    {
        uint8_t const* row = &frame.Pixels[y * stride];
        char* out = &raw[y * (1 + stride)];
        if (y && std::equal(row, row + stride, row - stride))
        {
            out[0] = char(2);
            continue;
        }
        out[0] = char(1);
        for (std::size_t i = 0; i < stride; ++i)
            out[1 + i] = char(uint8_t(row[i] - (i >= 3 ? row[i - 3] : 0)));
    }

    std::string packed = Deflate(raw);
    if (packed.empty())
        return false;
    _packed.push_back(std::move(packed));
    return true;
}

std::string Vi::ApngWriter::Finish() const
{
    if (_packed.empty())
        return {};

    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    Put32(header, _width);
    Put32(header, _height);
    header += char(8);  // bit depth
    header += char(2);  // RGB
    header += std::string(3, '\0');     // deflate, adaptive filtering, no interlace
    Chunk(png, "IHDR", header);

    std::string control;
    Put32(control, uint32_t(_packed.size()));
    Put32(control, 0);  // loop forever
    Chunk(png, "acTL", control);

    // fcTL and fdAT share one sequence; the first frame's data is the IDAT, which takes no number.
    uint32_t sequence = 0;
    for (std::size_t i = 0; i < _packed.size(); ++i)
    {
        std::string frame;
        Put32(frame, sequence++);
        Put32(frame, _width);
        Put32(frame, _height);
        Put32(frame, 0);    // x offset
        Put32(frame, 0);    // y offset
        Put16(frame, _delayNumerator);
        Put16(frame, _delayDenominator);
        frame += char(0);   // dispose: none
        frame += char(0);   // blend: source
        Chunk(png, "fcTL", frame);
        if (!i)
            Chunk(png, "IDAT", _packed[i]);
        else
        {
            std::string data;
            Put32(data, sequence++);
            data += _packed[i];
            Chunk(png, "fdAT", data);
        }
    }
    Chunk(png, "IEND", {});
    return png;
}
