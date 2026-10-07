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

#include "EvalVideo.h"
#include "FrameImage.h"
#include "MentalMap.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <boost/json/parse.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>
#include <zlib.h>

namespace Vi = Animus::Vision;
namespace fs = std::filesystem;

namespace
{
    uint32_t Read32(std::string const& data, std::size_t at)
    {
        return (uint32_t(uint8_t(data[at])) << 24) | (uint32_t(uint8_t(data[at + 1])) << 16)
            | (uint32_t(uint8_t(data[at + 2])) << 8) | uint32_t(uint8_t(data[at + 3]));
    }

    uint16_t Read16(std::string const& data, std::size_t at)
    {
        return uint16_t((uint32_t(uint8_t(data[at])) << 8) | uint32_t(uint8_t(data[at + 1])));
    }

    struct PngChunk
    {
        std::string Tag;
        std::string Body;
    };

    /// The file's chunks, every CRC checked.
    std::vector<PngChunk> Chunks(std::string const& png)
    {
        std::vector<PngChunk> out;
        EXPECT_EQ(png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
        std::size_t at = 8;
        while (at + 12 <= png.size())
        {
            uint32_t const length = Read32(png, at);
            std::string const tagged = png.substr(at + 4, 4 + length);
            EXPECT_EQ(Read32(png, at + 8 + length),
                uint32_t(crc32(0L, reinterpret_cast<Bytef const*>(tagged.data()), uInt(tagged.size()))));
            out.push_back({ tagged.substr(0, 4), tagged.substr(4) });
            at += 12 + length;
        }
        EXPECT_EQ(at, png.size());
        return out;
    }

    /// A frame's packed rows back to RGB, undoing the filters ApngWriter uses (none, Sub, Up).
    std::vector<uint8_t> Unpack(std::string const& packed, uint32_t width, uint32_t height)
    {
        std::size_t const stride = std::size_t(width) * 3;
        std::vector<uint8_t> raw(std::size_t(height) * (1 + stride));
        uLongf size = uLongf(raw.size());
        EXPECT_EQ(uncompress(raw.data(), &size, reinterpret_cast<Bytef const*>(packed.data()), uLong(packed.size())),
            Z_OK);
        EXPECT_EQ(size, raw.size());
        std::vector<uint8_t> rgb(std::size_t(height) * stride);
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t const filter = raw[y * (1 + stride)];
            uint8_t const* in = &raw[y * (1 + stride) + 1];
            uint8_t* out = &rgb[y * stride];
            for (std::size_t i = 0; i < stride; ++i)
            {
                uint8_t const left = i >= 3 ? out[i - 3] : 0;
                uint8_t const up = y ? out[i - stride] : 0;
                EXPECT_LE(filter, 2);
                out[i] = uint8_t(in[i] + (filter == 1 ? left : filter == 2 ? up : 0));
            }
        }
        return rgb;
    }

    /// A camera frame of `settings`: sky above the horizon, terrain below whose distance changes with `phase` (so
    /// successive frames differ), and a hostile creature and the objective somewhere in it.
    std::vector<uint8_t> Scene(Vi::Settings const& settings, uint32_t phase)
    {
        std::vector<uint8_t> image(Vi::ImageBytes(settings));
        Vi::FillNoFrame(image.data(), uint32_t(image.size()));
        for (uint32_t row = settings.Height / 2; row < settings.Height; ++row)
            for (uint32_t col = 0; col < settings.Width; ++col)
            {
                Vi::Hit hit;
                hit.Distance = 3.0f + float(settings.Height - row) * 2.0f + float((col + phase) % 7);
                hit.What = Vi::Class::Terrain;
                hit.Z = -1.5f + 0.1f * float(col % 5);
                hit.NormalZ = 0.9f;
                bool const creature = col >= (phase % settings.Width) && col < (phase % settings.Width) + 4
                    && row < settings.Height / 2 + 4;
                if (creature)
                    hit.What = Vi::Class::HostileCreature;
                bool const objective = col == settings.Width / 2 && row == settings.Height - 3;
                Vi::EncodePixel(hit, 0.0f, objective, 0, &image[(std::size_t(row) * settings.Width + col)
                    * Vi::BYTES_PER_PIXEL]);
            }
        return image;
    }

    /// A map crop: floor everywhere, a wall ring, frontier cells, as MapColour reads them.
    std::vector<uint8_t> Crop(uint32_t phase)
    {
        std::vector<uint8_t> crop(Vi::CROP_BYTES, 0);
        for (uint32_t row = 0; row < Vi::CROP; ++row)
            for (uint32_t col = 0; col < Vi::CROP; ++col)
            {
                uint8_t* cell = &crop[(std::size_t(row) * Vi::CROP + col) * Vi::CROP_CHANNELS];
                bool const wall = row == 4 || row == Vi::CROP - 5 || col == 4 || col == Vi::CROP - 5;
                cell[Vi::CROP_CODE] = uint8_t(wall ? Vi::MapCode::Wall : Vi::MapCode::Floor);
                cell[Vi::CROP_FRONTIER] = (row + col + phase) % 23 == 0 ? 1 : 0;
            }
        return crop;
    }

    std::string Slurp(fs::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    fs::path FreshDir(std::string const& name)
    {
        fs::path const dir = fs::temp_directory_path() / ("animus-eval-video-" + name + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(dir);
        return dir;
    }
}

TEST(VisionEvalVideoTest, SelectionIsTheSameForTheSameSeeds)
{
    std::vector<uint32_t> const once = Vi::EvalVideoSeeds(0, 1024, 8, 18);
    EXPECT_EQ(once, Vi::EvalVideoSeeds(0, 1024, 8, 18));
    ASSERT_EQ(once.size(), 8u);
    EXPECT_TRUE(std::is_sorted(once.begin(), once.end()));
    EXPECT_EQ(std::set<uint32_t>(once.begin(), once.end()).size(), once.size());
    for (uint32_t seed : once)
        EXPECT_LT(seed, 1024u);
    // Nothing to do with what the episodes did: no outcome goes in, so successive checkpoints film the same seeds.
    EXPECT_EQ(Vi::EvalVideoSeeds(0, 1024, 8, 18), (std::vector<uint32_t>{ 0, 128, 256, 384, 513, 641, 769, 897 }));
}

TEST(VisionEvalVideoTest, SelectionSpreadsOverClassesAndRungs)
{
    // 18 (class, build) pairs, 4 rungs: seed i plays pair i % 18 at rung (i / 18) % 4.
    uint32_t const pairs = 18;
    std::vector<uint32_t> const seeds = Vi::EvalVideoSeeds(0, 1024, 8, pairs);
    std::set<uint32_t> classes;
    std::set<uint32_t> rungs;
    for (uint32_t seed : seeds)
    {
        classes.insert(seed % pairs);
        rungs.insert((seed / pairs) % 4);
    }
    EXPECT_EQ(classes.size(), 8u);
    EXPECT_EQ(rungs.size(), 4u);

    // Fewer pairs than videos: every pair filmed, and still spread over the rungs.
    std::vector<uint32_t> const few = Vi::EvalVideoSeeds(0, 256, 8, 3);
    ASSERT_EQ(few.size(), 8u);
    std::set<uint32_t> fewClasses;
    std::set<uint32_t> fewCycles;
    for (uint32_t seed : few)
    {
        fewClasses.insert(seed % 3);
        fewCycles.insert(seed / 3);
    }
    EXPECT_EQ(fewClasses.size(), 3u);
    EXPECT_EQ(fewCycles.size(), 8u);

    // One pair (a scenario without castings): evenly over the range.
    EXPECT_EQ(Vi::EvalVideoSeeds(0, 64, 8, 1), (std::vector<uint32_t>{ 0, 8, 16, 24, 32, 40, 48, 56 }));
}

TEST(VisionEvalVideoTest, SelectionKeepsToItsRange)
{
    // A cluster sim's share: its own seeds only.
    for (uint32_t seed : Vi::EvalVideoSeeds(500, 1000, 8, 18))
    {
        EXPECT_GE(seed, 500u);
        EXPECT_LT(seed, 1000u);
    }
    // A range shorter than the count: every seed once.
    EXPECT_EQ(Vi::EvalVideoSeeds(100, 104, 8, 18), (std::vector<uint32_t>{ 100, 101, 102, 103 }));
    // A range shorter than the pairs: distinct, in range.
    std::vector<uint32_t> const short_ = Vi::EvalVideoSeeds(0, 10, 8, 18);
    EXPECT_EQ(short_.size(), 8u);
    EXPECT_EQ(std::set<uint32_t>(short_.begin(), short_.end()).size(), 8u);
    EXPECT_LT(short_.back(), 10u);
    EXPECT_TRUE(Vi::EvalVideoSeeds(5, 5, 8, 18).empty());
    EXPECT_TRUE(Vi::EvalVideoSeeds(0, 100, 0, 18).empty());
}

TEST(VisionEvalVideoTest, OutcomeAndRungFromTheInfoRow)
{
    std::vector<std::string> const names = { "damage", "seek_rung", "at_top_rung", "found", "arrived" };
    float const found[] = { 1.0f, 2.0f, 0.0f, 1.0f, 0.0f };
    float const lost[] = { 1.0f, 2.0f, 0.0f, 0.0f, 1.0f };
    EXPECT_EQ(Vi::EvalVideoOutcome(names, found), "success");
    EXPECT_EQ(Vi::EvalVideoOutcome(names, lost), "failure");     // "found" comes before "arrived"
    EXPECT_EQ(Vi::EvalVideoOutcome(names, nullptr), "unfinished");
    EXPECT_EQ(Vi::EvalVideoOutcome({ "damage" }, found), "ended");
    EXPECT_EQ(Vi::EvalVideoRungColumn(names), 1);
    EXPECT_EQ(Vi::EvalVideoRungColumn({ "wing_rung_share", "tier" }), 1);
    EXPECT_EQ(Vi::EvalVideoRungColumn({ "damage" }), -1);
}

TEST(VisionEvalVideoTest, CompositeFrameIsTheCompositePicture)
{
    Vi::Settings settings;
    settings.Width = 16;
    settings.Height = 8;
    std::vector<uint8_t> const image = Scene(settings, 3);
    std::vector<uint8_t> const crop = Crop(3);
    for (uint8_t const* map : { static_cast<uint8_t const*>(nullptr), crop.data() })
    {
        Vi::RgbImage const frame = Vi::CompositeRgb(settings, image.data(), 4, map);
        EXPECT_EQ(frame.Width, 64u);
        EXPECT_EQ(frame.Height, 32u);
        ASSERT_EQ(frame.Pixels.size(), std::size_t(64) * 32 * 3);

        // The same picture CompositePng packs (one IDAT, filter 0).
        std::vector<PngChunk> const chunks = Chunks(Vi::CompositePng(settings, image.data(), 4, map));
        std::string packed;
        for (PngChunk const& chunk : chunks)
            if (chunk.Tag == "IDAT")
                packed += chunk.Body;
        EXPECT_EQ(Unpack(packed, 64, 32), frame.Pixels);
    }
    // The inset's frame is grey in the top right corner; without a map the corner is the picture.
    Vi::RgbImage const inset = Vi::CompositeRgb(settings, image.data(), 4, crop.data());
    Vi::RgbImage const plain = Vi::CompositeRgb(settings, image.data(), 4);
    EXPECT_NE(inset.Pixels, plain.Pixels);
}

TEST(VisionEvalVideoTest, AnimatedPngHoldsEveryFrame)
{
    Vi::Settings settings;
    settings.Width = 16;
    settings.Height = 8;
    std::vector<Vi::RgbImage> frames;
    Vi::ApngWriter writer(48, 24, 250, 1000);
    for (uint32_t i = 0; i < 5; ++i)
    {
        std::vector<uint8_t> const image = Scene(settings, i);
        frames.push_back(Vi::CompositeRgb(settings, image.data(), 3));
        EXPECT_TRUE(writer.Add(frames.back()));
    }
    Vi::RgbImage wrong;
    wrong.Width = 4;
    wrong.Height = 4;
    wrong.Pixels.resize(48);
    EXPECT_FALSE(writer.Add(wrong));
    EXPECT_EQ(writer.Frames(), 5u);

    std::vector<PngChunk> const chunks = Chunks(writer.Finish());
    ASSERT_GE(chunks.size(), 4u);
    EXPECT_EQ(chunks.front().Tag, "IHDR");
    EXPECT_EQ(Read32(chunks[0].Body, 0), 48u);
    EXPECT_EQ(Read32(chunks[0].Body, 4), 24u);
    EXPECT_EQ(chunks[1].Tag, "acTL");
    EXPECT_EQ(Read32(chunks[1].Body, 0), 5u);    // frames
    EXPECT_EQ(Read32(chunks[1].Body, 4), 0u);    // loops forever
    EXPECT_EQ(chunks.back().Tag, "IEND");

    // fcTL, then the frame's data (IDAT for the first, fdAT after), one sequence across both.
    uint32_t sequence = 0;
    uint32_t controls = 0;
    std::vector<std::vector<uint8_t>> decoded;
    bool idatSeen = false;
    for (std::size_t i = 2; i + 1 < chunks.size(); ++i)
    {
        PngChunk const& chunk = chunks[i];
        if (chunk.Tag == "fcTL")
        {
            EXPECT_EQ(Read32(chunk.Body, 0), sequence++);
            EXPECT_EQ(Read32(chunk.Body, 4), 48u);
            EXPECT_EQ(Read32(chunk.Body, 8), 24u);
            EXPECT_EQ(Read16(chunk.Body, 20), 250);
            EXPECT_EQ(Read16(chunk.Body, 22), 1000);
            ++controls;
        }
        else if (chunk.Tag == "IDAT")
        {
            EXPECT_EQ(controls, 1u);    // after the first fcTL
            idatSeen = true;
            decoded.push_back(Unpack(chunk.Body, 48, 24));
        }
        else if (chunk.Tag == "fdAT")
        {
            EXPECT_EQ(Read32(chunk.Body, 0), sequence++);
            decoded.push_back(Unpack(chunk.Body.substr(4), 48, 24));
        }
        else
            ADD_FAILURE() << "unexpected chunk " << chunk.Tag;
    }
    EXPECT_TRUE(idatSeen);
    EXPECT_EQ(controls, 5u);
    ASSERT_EQ(decoded.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i)
        EXPECT_EQ(decoded[i], frames[i].Pixels) << "frame " << i;

    EXPECT_TRUE(Vi::ApngWriter(8, 8, 1, 4).Finish().empty());
}

/// The encoder path end to end, without a worldserver: a 128 x 64 camera with a map, an evaluation of three filmed
/// seeds (one found, one not, one cut short by the evaluation's end), written by the worker thread.
TEST(VisionEvalVideoTest, RecorderWritesVideosSidecarsAndIndex)
{
    fs::path const dir = FreshDir("recorder");
    Vi::Settings settings;     // 128 x 64
    std::vector<std::string> const names = { "found", "seek_rung", "distance_travelled", "unused" };

    Vi::EvalVideoOptions options;
    options.Dir = dir / "videos" / "123456";
    options.Scenario = "move2_seek";
    options.Label = "123456";
    options.Camera = settings;
    options.Scale = 4;
    options.DecisionMs = 250;
    options.ImageBytes = Vi::ImageBytes(settings);
    options.MapBytes = Vi::CROP_BYTES;
    options.MaxFrames = 200;
    options.InfoNames = names;

    uint32_t constexpr FRAMES = 240;
    std::vector<std::vector<uint8_t>> scenes;
    std::vector<std::vector<uint8_t>> crops;
    for (uint32_t i = 0; i < 16; ++i)
    {
        scenes.push_back(Scene(settings, i));
        crops.push_back(Crop(i));
    }

    double captureNs = 0.0;
    uint32_t captures = 0;
    auto const encodeStarted = std::chrono::steady_clock::now();
    {
        Vi::EvalVideoRecorder recorder;
        EXPECT_FALSE(recorder.Active());
        recorder.Begin(options, { 7, 40, 99 });
        ASSERT_TRUE(recorder.Active());
        EXPECT_TRUE(recorder.Wanted(40));
        EXPECT_FALSE(recorder.Wanted(41));

        auto const film = [&](uint32_t env, uint32_t seed, std::string const& layout)
        {
            Vi::EvalVideoEpisode episode;
            episode.Seed = seed;
            episode.Env = env;
            episode.Layout = layout;
            episode.Class = 4;
            episode.Race = 1;
            episode.Level = 1;
            episode.RenderWidth = 48;
            episode.RenderHeight = 24;
            recorder.Start(episode);
        };
        film(0, 7, "Rogue");
        film(1, 40, "Mage");
        film(2, 99, "Priest");
        EXPECT_FALSE(recorder.Wanted(40));      // started: never twice in one evaluation
        EXPECT_TRUE(recorder.Recording(1));
        EXPECT_EQ(recorder.Seed(1), 40u);

        for (uint32_t frame = 0; frame < FRAMES; ++frame)
            for (uint32_t env = 0; env < 3; ++env)
            {
                if (env == 1 && frame >= 120)
                    continue;           // the mage's episode ended at 120
                auto const mark = std::chrono::steady_clock::now();
                recorder.Frame(env, scenes[(frame + env) % scenes.size()].data(),
                    crops[(frame / 8) % crops.size()].data());
                captureNs += double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - mark).count());
                ++captures;
                if (env == 1 && frame == 119)
                {
                    float const lost[] = { 0.0f, 2.0f, 31.5f, 0.0f };
                    recorder.Finish(1, lost);
                }
            }
        float const found[] = { 1.0f, 1.0f, 12.0f, 0.0f };
        recorder.Finish(0, found);
        // The priest's episode is still going when the evaluation ends.
        recorder.End();
        EXPECT_FALSE(recorder.Active());
        recorder.Drain();
        EXPECT_EQ(recorder.Recorded(), 3u);
        EXPECT_EQ(recorder.LastDir(), options.Dir.string());
    }
    double const encodeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - encodeStarted)
        .count();

    fs::path const rogue = options.Dir / "7-rogue-success.png";
    fs::path const mage = options.Dir / "40-mage-failure.png";
    fs::path const priest = options.Dir / "99-priest-unfinished.png";
    ASSERT_TRUE(fs::exists(rogue));
    ASSERT_TRUE(fs::exists(mage));
    ASSERT_TRUE(fs::exists(priest));
    ASSERT_TRUE(fs::exists(options.Dir / "index.json"));
    ASSERT_TRUE(fs::exists(options.Dir / "index.html"));

    // The rogue's: capped at MaxFrames, 512 x 256.
    std::vector<PngChunk> const chunks = Chunks(Slurp(rogue));
    EXPECT_EQ(Read32(chunks[0].Body, 0), 512u);
    EXPECT_EQ(Read32(chunks[0].Body, 4), 256u);
    EXPECT_EQ(Read32(chunks[1].Body, 0), 200u);

    boost::json::object const sidecar = boost::json::parse(Slurp(options.Dir / "7-rogue-success.json")).as_object();
    EXPECT_EQ(sidecar.at("seed").to_number<uint64_t>(), 7u);
    EXPECT_EQ(sidecar.at("layout").as_string(), "Rogue");
    EXPECT_EQ(sidecar.at("class").to_number<uint64_t>(), 4u);
    EXPECT_EQ(sidecar.at("outcome").as_string(), "success");
    EXPECT_DOUBLE_EQ(sidecar.at("rung").to_number<double>(), 1.0);
    EXPECT_EQ(sidecar.at("frames").to_number<uint64_t>(), 200u);
    EXPECT_TRUE(sidecar.at("truncated").as_bool());
    EXPECT_EQ(sidecar.at("width").to_number<uint64_t>(), 512u);
    EXPECT_EQ(sidecar.at("height").to_number<uint64_t>(), 256u);
    EXPECT_EQ(sidecar.at("render").as_string(), "48x24");
    EXPECT_TRUE(sidecar.at("map_inset").as_bool());
    EXPECT_DOUBLE_EQ(sidecar.at("fps").to_number<double>(), 4.0);
    boost::json::object const& measures = sidecar.at("measures").as_object();
    EXPECT_DOUBLE_EQ(measures.at("distance_travelled").to_number<double>(), 12.0);
    EXPECT_FALSE(measures.contains("unused"));

    boost::json::object const mageSidecar = boost::json::parse(Slurp(options.Dir / "40-mage-failure.json"))
        .as_object();
    EXPECT_EQ(mageSidecar.at("frames").to_number<uint64_t>(), 120u);
    EXPECT_FALSE(mageSidecar.at("truncated").as_bool());
    boost::json::object const priestSidecar = boost::json::parse(Slurp(options.Dir / "99-priest-unfinished.json"))
        .as_object();
    EXPECT_TRUE(priestSidecar.at("rung").is_null());

    boost::json::object const index = boost::json::parse(Slurp(options.Dir / "index.json")).as_object();
    EXPECT_EQ(index.at("evaluation").as_string(), "123456");
    EXPECT_EQ(index.at("selected_seeds").as_array().size(), 3u);
    EXPECT_EQ(index.at("videos").as_array().size(), 3u);

    // The cost, for the record: the world thread's copy per frame, the worker's composite and deflate per frame, and
    // a 120-frame (30 s) episode's file.
    std::cout << "[ eval video ] capture " << captureNs / 1e3 / double(captures) << " us/frame (world thread), encode "
        << sidecar.at("encode_ms_per_frame").to_number<double>() << " ms/frame (worker), " << encodeSeconds
        << " s for " << captures << " frames; files: rogue " << fs::file_size(rogue) << " B / 200 frames, mage "
        << fs::file_size(mage) << " B / 120 frames\n";

    fs::remove_all(dir);
}

TEST(VisionEvalVideoTest, NothingWithoutAnEvaluation)
{
    Vi::EvalVideoRecorder recorder;
    Vi::EvalVideoEpisode episode;
    episode.Seed = 1;
    recorder.Start(episode);
    EXPECT_FALSE(recorder.Recording(0));
    recorder.Begin(Vi::EvalVideoOptions(), {});
    EXPECT_FALSE(recorder.Active());
    recorder.End();
    recorder.Drain();
    EXPECT_EQ(recorder.Recorded(), 0u);
}
