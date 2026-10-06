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

#include "Camera.h"
#include "FreeLook.h"
#include "PlayerController.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <array>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

/*
 * Free look and mixed render resolutions (camera-vision.FREELOOK.md, section E): the camera's integration, the look
 * head's choices, the nearest-pixel upscaling, the render size draw and RenderSizes' parsing.
 */
namespace
{
    namespace Vi = Animus::Vision;
    namespace FL = Animus::Vision::FreeLook;

    constexpr float DEG = Vi::DEGREES;

    /// A look choice by its values: yaw rate degrees a second, pitch rate, zoom choice.
    std::array<int32_t, FL::HEADS> Choice(float yawDeg, float pitchDeg, uint32_t zoom)
    {
        std::array<int32_t, FL::HEADS> choice{ -1, -1, int32_t(zoom) };
        for (uint32_t i = 0; i < FL::YAW_RATE_COUNT; ++i)
            if (FL::YAW_RATES_DEG[i] == yawDeg)
                choice[FL::HEAD_YAW_RATE] = int32_t(i);
        for (uint32_t i = 0; i < FL::PITCH_RATE_COUNT; ++i)
            if (FL::PITCH_RATES_DEG[i] == pitchDeg)
                choice[FL::HEAD_PITCH_RATE] = int32_t(i);
        return choice;
    }

    FL::State Fresh(Vi::Settings const& settings = Vi::Settings())
    {
        FL::State state;
        FL::Reset(state, settings);
        return state;
    }

    /// A frame of distinct pixels: pixel (r, c) is { r, c, r + c, class (r + c) % 23 | objective when c is odd,
    /// slot (r * c) % 33 }.
    std::vector<uint8_t> Pattern(uint32_t w, uint32_t h)
    {
        std::vector<uint8_t> frame(std::size_t(w) * h * Vi::BYTES_PER_PIXEL);
        for (uint32_t r = 0; r < h; ++r)
            for (uint32_t c = 0; c < w; ++c)
            {
                uint8_t* p = &frame[(std::size_t(r) * w + c) * Vi::BYTES_PER_PIXEL];
                p[0] = uint8_t(r);
                p[1] = uint8_t(c);
                p[2] = uint8_t(r + c);
                p[3] = uint8_t(((r + c) % Vi::CLASSES) | (c % 2 ? Vi::OBJECTIVE_BIT : 0));
                p[4] = uint8_t((r * c) % (Vi::ENTITY_SLOTS + 1));
            }
        return frame;
    }
}

// An episode starts at yaw offset 0, the conf's pitch, the zoom level nearest the conf's zoom, the rates let go.
TEST(VisionFreeLookTest, ResetIsTheConfsCamera)
{
    Vi::Settings settings;
    settings.Pitch = -15.0f;
    settings.Zoom = 6.0f;
    FL::State state = Fresh(settings);
    EXPECT_FLOAT_EQ(state.YawOffset, 0.0f);
    EXPECT_FLOAT_EQ(state.Pitch, -15.0f * DEG);
    EXPECT_EQ(state.ZoomLevel, 2u);
    EXPECT_FLOAT_EQ(state.YawRate, 0.0f);
    EXPECT_FLOAT_EQ(state.PitchRate, 0.0f);
    EXPECT_FALSE(state.Observed);

    EXPECT_EQ(FL::NearestZoomLevel(0.0f), 0u);
    EXPECT_EQ(FL::NearestZoomLevel(4.0f), 1u);
    EXPECT_EQ(FL::NearestZoomLevel(10.0f), 3u);
    EXPECT_EQ(FL::NearestZoomLevel(50.0f), 3u);
    EXPECT_FLOAT_EQ(FL::CameraOf(state).Zoom, 6.0f);
}

// A held rate turns the camera by rate x dt, on both axes, and stays held until the head changes it.
TEST(VisionFreeLookTest, RateHeldOverTheDecision)
{
    Vi::Settings settings;
    FL::State state = Fresh(settings);
    auto choice = Choice(90.0f, 20.0f, FL::ZOOM_HOLD);
    FL::Apply(state, choice.data(), settings);
    EXPECT_FLOAT_EQ(state.YawRate, 90.0f * DEG);
    EXPECT_FLOAT_EQ(state.PitchRate, 20.0f * DEG);

    FL::Integrate(state, 0.25f);
    EXPECT_NEAR(state.YawOffset, 22.5f * DEG, 1e-5f);
    EXPECT_NEAR(state.Pitch, (-15.0f + 5.0f) * DEG, 1e-5f);
    // Held: a second decision with no new choice turns as far again.
    FL::Integrate(state, 0.25f);
    EXPECT_NEAR(state.YawOffset, 45.0f * DEG, 1e-5f);
    EXPECT_NEAR(state.Pitch, (-15.0f + 10.0f) * DEG, 1e-5f);

    // + is left, - is right.
    choice = Choice(-30.0f, 0.0f, FL::ZOOM_HOLD);
    FL::Apply(state, choice.data(), settings);
    FL::Integrate(state, 0.5f);
    EXPECT_NEAR(state.YawOffset, 30.0f * DEG, 1e-5f);
    EXPECT_NEAR(state.Pitch, -5.0f * DEG, 1e-5f);

    // The camera's state is what the renderer is given, with the held rates for the scalars.
    Vi::CameraState const camera = FL::CameraOf(state);
    EXPECT_FLOAT_EQ(camera.YawOffset, state.YawOffset);
    EXPECT_FLOAT_EQ(camera.Pitch, state.Pitch);
    EXPECT_FLOAT_EQ(camera.YawRate, -30.0f * DEG);
    EXPECT_FLOAT_EQ(camera.PitchRate, 0.0f);
}

// The yaw offset wraps to (-pi, pi]: past +pi it comes round from -pi, and -pi itself is +pi.
TEST(VisionFreeLookTest, YawWrapsAtPi)
{
    EXPECT_FLOAT_EQ(FL::Wrap(Vi::PI), Vi::PI);
    EXPECT_FLOAT_EQ(FL::Wrap(-Vi::PI), Vi::PI);
    EXPECT_NEAR(FL::Wrap(3.0f * Vi::PI / 2.0f), -Vi::PI / 2.0f, 1e-5f);
    EXPECT_NEAR(FL::Wrap(-3.0f * Vi::PI / 2.0f), Vi::PI / 2.0f, 1e-5f);
    EXPECT_NEAR(std::fabs(FL::Wrap(5.0f * Vi::PI)), Vi::PI, 1e-4f);

    Vi::Settings settings;
    FL::State state = Fresh(settings);
    auto const left = Choice(180.0f, 0.0f, FL::ZOOM_HOLD);
    FL::Apply(state, left.data(), settings);
    // 170 degrees, then 20 more: -170.
    FL::Integrate(state, 170.0f / 180.0f);
    EXPECT_NEAR(state.YawOffset, 170.0f * DEG, 1e-4f);
    FL::Integrate(state, 20.0f / 180.0f);
    EXPECT_NEAR(state.YawOffset, -170.0f * DEG, 1e-4f);
    for (int i = 0; i < 50; ++i)
    {
        FL::Integrate(state, 0.25f);
        EXPECT_GT(state.YawOffset, -Vi::PI);
        EXPECT_LE(state.YawOffset, Vi::PI);
    }
}

// The pitch stops at +-80 degrees however long a rate is held.
TEST(VisionFreeLookTest, PitchClamps)
{
    Vi::Settings settings;
    FL::State state = Fresh(settings);
    auto const up = Choice(0.0f, 60.0f, FL::ZOOM_HOLD);
    FL::Apply(state, up.data(), settings);
    FL::Integrate(state, 10.0f);
    EXPECT_FLOAT_EQ(state.Pitch, 80.0f * DEG);
    auto const down = Choice(0.0f, -60.0f, FL::ZOOM_HOLD);
    FL::Apply(state, down.data(), settings);
    FL::Integrate(state, 10.0f);
    EXPECT_FLOAT_EQ(state.Pitch, -80.0f * DEG);
}

// In and out step one level, clamped at the ends; hold keeps it.
TEST(VisionFreeLookTest, ZoomStepsAndClamps)
{
    Vi::Settings settings;
    FL::State state = Fresh(settings);
    ASSERT_EQ(state.ZoomLevel, 2u);
    auto step = [&](uint32_t zoom)
    {
        auto const choice = Choice(0.0f, 0.0f, zoom);
        FL::Apply(state, choice.data(), settings);
        return state.ZoomLevel;
    };
    EXPECT_EQ(step(FL::ZOOM_OUT), 3u);
    EXPECT_FLOAT_EQ(FL::CameraOf(state).Zoom, 12.0f);
    EXPECT_EQ(step(FL::ZOOM_OUT), 3u);
    EXPECT_EQ(step(FL::ZOOM_HOLD), 3u);
    EXPECT_EQ(step(FL::ZOOM_IN), 2u);
    EXPECT_EQ(step(FL::ZOOM_IN), 1u);
    EXPECT_EQ(step(FL::ZOOM_IN), 0u);
    EXPECT_FLOAT_EQ(FL::CameraOf(state).Zoom, 0.0f);
    EXPECT_EQ(step(FL::ZOOM_IN), 0u);
}

// Recentre: yaw offset 0, the conf's pitch, both rates let go -- whatever rates the same choice asked for -- and the
// zoom kept.
TEST(VisionFreeLookTest, RecentreReturnsBehindTheFacing)
{
    Vi::Settings settings;
    settings.Pitch = -20.0f;
    FL::State state = Fresh(settings);
    auto const turn = Choice(90.0f, 60.0f, FL::ZOOM_OUT);
    FL::Apply(state, turn.data(), settings);
    FL::Integrate(state, 1.0f);
    ASSERT_NE(state.YawOffset, 0.0f);
    ASSERT_EQ(state.ZoomLevel, 3u);

    auto const recentre = Choice(180.0f, -60.0f, FL::ZOOM_RECENTRE);
    FL::Apply(state, recentre.data(), settings);
    EXPECT_FLOAT_EQ(state.YawOffset, 0.0f);
    EXPECT_FLOAT_EQ(state.Pitch, -20.0f * DEG);
    EXPECT_FLOAT_EQ(state.YawRate, 0.0f);
    EXPECT_FLOAT_EQ(state.PitchRate, 0.0f);
    EXPECT_EQ(state.ZoomLevel, 3u);
    FL::Integrate(state, 1.0f);
    EXPECT_FLOAT_EQ(state.YawOffset, 0.0f);
}

// Never adjust camera: a sideways look held while running stays exactly where the look head put it. The forward key is
// held and the body runs for five seconds; nothing pulls the offset back behind the facing. Only the held rates move
// it, and recentre brings it back.
TEST(VisionFreeLookTest, ASidewaysLookHoldsWhileRunning)
{
    Vi::Settings settings;
    FL::State state = Fresh(settings);
    state.YawOffset = 60.0f * DEG;
    Animus::Movement::ControlState held;
    held.Forward = 1;
    Animus::Movement::BodyState body;
    body.Vx = 7.0f;
    FL::Advance(state, 0.25f);
    for (int decision = 0; decision < 20; ++decision)
    {
        body.X += body.Vx * 0.25f;
        FL::Advance(state, 0.25f);
        ASSERT_FLOAT_EQ(state.YawOffset, 60.0f * DEG) << "decision " << decision;
    }
    EXPECT_EQ(held.Forward, 1);
    EXPECT_FLOAT_EQ(body.X, 35.0f);

    // A held yaw rate turns it, by the rate alone.
    auto const turn = Choice(30.0f, 0.0f, FL::ZOOM_HOLD);
    FL::Apply(state, turn.data(), settings);
    FL::Advance(state, 0.5f);
    EXPECT_NEAR(state.YawOffset, 75.0f * DEG, 1e-5f);

    // Recentre is how it comes back.
    auto const recentre = Choice(0.0f, 0.0f, FL::ZOOM_RECENTRE);
    FL::Apply(state, recentre.data(), settings);
    FL::Advance(state, 0.25f);
    EXPECT_FLOAT_EQ(state.YawOffset, 0.0f);
}

// The episode's first observation renders where the reset put the camera: dt = 0, whatever the rates. Every later
// one advances by the decision.
TEST(VisionFreeLookTest, FirstObservationDoesNotAdvance)
{
    Vi::Settings settings;
    FL::State state = Fresh(settings);
    state.YawRate = 90.0f * DEG;
    state.PitchRate = 20.0f * DEG;
    state.YawOffset = 30.0f * DEG;
    FL::Advance(state, 0.25f);
    EXPECT_FLOAT_EQ(state.YawOffset, 30.0f * DEG);
    EXPECT_FLOAT_EQ(state.Pitch, -15.0f * DEG);
    EXPECT_TRUE(state.Observed);
    FL::Advance(state, 0.25f);
    EXPECT_NEAR(state.YawOffset, 52.5f * DEG, 1e-5f);

    // A new episode: the first observation again.
    FL::Reset(state, settings);
    state.YawRate = 90.0f * DEG;
    FL::Advance(state, 0.25f);
    EXPECT_FLOAT_EQ(state.YawOffset, 0.0f);
}

// Every value of every head is a legal choice; anything else is refused and changes nothing.
TEST(VisionFreeLookTest, ChoicesInRange)
{
    Vi::Settings settings;
    for (int32_t yaw = 0; yaw < int32_t(FL::YAW_RATE_COUNT); ++yaw)
        for (int32_t pitch = 0; pitch < int32_t(FL::PITCH_RATE_COUNT); ++pitch)
            for (int32_t zoom = 0; zoom < int32_t(FL::ZOOM_CHOICES); ++zoom)
            {
                int32_t const choice[FL::HEADS] = { yaw, pitch, zoom };
                EXPECT_TRUE(FL::Valid(choice));
            }
    for (auto const& bad : { std::array<int32_t, 3>{ 7, 0, 0 }, std::array<int32_t, 3>{ -1, 0, 0 },
             std::array<int32_t, 3>{ 0, 5, 0 }, std::array<int32_t, 3>{ 0, 0, 5 }, std::array<int32_t, 3>{ 0, 0, -1 } })
    {
        EXPECT_FALSE(FL::Valid(bad.data()));
        FL::State state = Fresh(settings);
        FL::State const before = state;
        FL::Apply(state, bad.data(), settings);
        EXPECT_EQ(state.YawRate, before.YawRate);
        EXPECT_EQ(state.ZoomLevel, before.ZoomLevel);
    }
    EXPECT_TRUE(FL::Valid(FL::NEUTRAL.data()));
    FL::State state = Fresh(settings);
    FL::Apply(state, FL::NEUTRAL.data(), settings);
    EXPECT_FLOAT_EQ(state.YawRate, 0.0f);
    EXPECT_FLOAT_EQ(state.PitchRate, 0.0f);
    EXPECT_EQ(state.ZoomLevel, 2u);
}

// Nearest-pixel upscaling of every default render size into 128 x 64: canonical (r, c) is cast (r h / H, c w / W),
// every byte -- the objective bit with them -- copied as it is.
TEST(VisionFreeLookTest, UpscaleNearestPixel)
{
    uint32_t const W = 128;
    uint32_t const H = 64;
    for (Vi::Resolution const size : { Vi::Resolution{ 32, 16 }, Vi::Resolution{ 48, 24 },
             Vi::Resolution{ 64, 32 }, Vi::Resolution{ 128, 64 } })
    {
        std::vector<uint8_t> const cast = Pattern(size.Width, size.Height);
        std::vector<uint8_t> image(std::size_t(W) * H * Vi::BYTES_PER_PIXEL, 0xAB);
        Vi::Upscale(cast.data(), size.Width, size.Height, image.data(), W, H);
        uint32_t objectives = 0;
        for (uint32_t r = 0; r < H; ++r)
            for (uint32_t c = 0; c < W; ++c)
            {
                uint32_t const sr = r * size.Height / H;
                uint32_t const sc = c * size.Width / W;
                uint8_t const* out = &image[(std::size_t(r) * W + c) * Vi::BYTES_PER_PIXEL];
                uint8_t const* in = &cast[(std::size_t(sr) * size.Width + sc) * Vi::BYTES_PER_PIXEL];
                for (uint32_t b = 0; b < Vi::BYTES_PER_PIXEL; ++b)
                    ASSERT_EQ(out[b], in[b]) << size.Width << "x" << size.Height << " at " << r << "," << c;
                objectives += (out[3] & Vi::OBJECTIVE_BIT) ? 1 : 0;
            }
        // Half the cast columns carry the objective, so half the canonical ones do.
        EXPECT_EQ(objectives, W * H / 2) << size.Width << "x" << size.Height;
    }

    // 48 -> 128 is not a whole factor: columns 0..2 take cast 0, 3..5 cast 1, 6..7 cast 2.
    std::vector<uint8_t> const cast = Pattern(48, 24);
    std::vector<uint8_t> image(std::size_t(W) * H * Vi::BYTES_PER_PIXEL);
    Vi::Upscale(cast.data(), 48, 24, image.data(), W, H);
    std::vector<uint32_t> columns;
    for (uint32_t c = 0; c < 8; ++c)
        columns.push_back(image[std::size_t(c) * Vi::BYTES_PER_PIXEL + 1]);
    EXPECT_EQ(columns, (std::vector<uint32_t>{ 0, 0, 0, 1, 1, 1, 2, 2 }));
}

// The draw: one entry means every seat renders at it and nothing is rolled; several are drawn by weight; none is the
// canonical size.
TEST(VisionFreeLookTest, RenderSizeDraw)
{
    Vi::Settings settings;
    settings.RenderSizes = { { 48, 24 } };
    settings.RenderWeights = { 0.3f };
    uint32_t rolls = 0;
    auto const roll = [&rolls](float total) { ++rolls; return total / 2.0f; };
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(Vi::DrawRenderSize(settings, roll), (Vi::Resolution{ 48, 24 }));
    EXPECT_EQ(rolls, 0u);

    // The roll walks the weights in order: [0, 1) the first, [1, 2) the second, [2, 3) the third, [3, 3.4) the last.
    settings.RenderSizes = { { 32, 16 }, { 48, 24 }, { 64, 32 }, { 128, 64 } };
    settings.RenderWeights = { 1.0f, 1.0f, 1.0f, 0.4f };
    auto const at = [&settings](float value) { return Vi::DrawRenderSize(settings, [value](float) { return value; }); };
    EXPECT_EQ(at(0.0f).Width, 32u);
    EXPECT_EQ(at(0.99f).Width, 32u);
    EXPECT_EQ(at(1.0f).Width, 48u);
    EXPECT_EQ(at(2.5f).Width, 64u);
    EXPECT_EQ(at(3.0f).Width, 128u);
    EXPECT_EQ(at(3.39f).Width, 128u);
    EXPECT_EQ(at(5.0f).Width, 128u);        // past the top (rounding): the last
    float total = 0.0f;
    EXPECT_EQ(Vi::DrawRenderSize(settings, [&total](float t) { total = t; return 0.0f; }).Width, 32u);
    EXPECT_FLOAT_EQ(total, 3.4f);

    settings.RenderSizes.clear();
    EXPECT_EQ(Vi::DrawRenderSize(settings, roll), (Vi::Resolution{ settings.Width, settings.Height }));
}

// The default mix, drawn 100,000 times from a fixed seed: each low size about 1 / 3.4 of the episodes and the native
// 128 x 64 about 0.4 / 3.4 (11.8%).
TEST(VisionFreeLookTest, RenderSizeDrawIsWeighted)
{
    Vi::Settings const settings;
    ASSERT_EQ(settings.RenderSizes.size(), 4u);
    std::mt19937 random(20261006);
    std::map<uint32_t, uint32_t> drawn;
    constexpr uint32_t DRAWS = 100000;
    for (uint32_t i = 0; i < DRAWS; ++i)
    {
        Vi::Resolution const size = Vi::DrawRenderSize(settings, [&random](float total)
        {
            return std::uniform_real_distribution<float>(0.0f, total)(random);
        });
        ++drawn[size.Width];
    }
    EXPECT_EQ(drawn.size(), 4u);
    EXPECT_NEAR(double(drawn[32]) / DRAWS, 1.0 / 3.4, 0.01);
    EXPECT_NEAR(double(drawn[48]) / DRAWS, 1.0 / 3.4, 0.01);
    EXPECT_NEAR(double(drawn[64]) / DRAWS, 1.0 / 3.4, 0.01);
    EXPECT_NEAR(double(drawn[128]) / DRAWS, 0.4 / 3.4, 0.01);
}

// AnimusForge.Vision.RenderSizes: "WxH" or "WxH:w" entries separated by commas, spaces ignored; an entry that does
// not parse, has no weight above 0 after its colon, or does not fit within the canonical size is left out and said;
// none left is the canonical size.
TEST(VisionFreeLookTest, ParseRenderSizes)
{
    Vi::Settings settings;      // 128 x 64
    std::vector<std::string> errors;
    std::vector<float> weights;
    std::vector<Vi::Resolution> sizes = Vi::ParseRenderSizes("32x16, 48x24, 64x32", settings, weights, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(sizes, (std::vector<Vi::Resolution>{ { 32, 16 }, { 48, 24 }, { 64, 32 } }));
    EXPECT_EQ(weights, (std::vector<float>{ 1.0f, 1.0f, 1.0f }));

    // The default, weights and all, is the Settings default.
    sizes = Vi::ParseRenderSizes(Vi::DEFAULT_RENDER_SIZES, settings, weights, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(sizes, settings.RenderSizes);
    EXPECT_EQ(weights, settings.RenderWeights);

    sizes = Vi::ParseRenderSizes(" 128 X 64 : 2.5 ,", settings, weights, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(sizes, (std::vector<Vi::Resolution>{ { 128, 64 } }));
    EXPECT_EQ(weights, (std::vector<float>{ 2.5f }));

    sizes = Vi::ParseRenderSizes("32x16, 256x16, 0x8, banana, 48x, 64x32:0, 64x32:-1, 64x32:x, 64x32:", settings,
        weights, errors);
    EXPECT_EQ(sizes, (std::vector<Vi::Resolution>{ { 32, 16 } }));
    EXPECT_EQ(weights, (std::vector<float>{ 1.0f }));
    EXPECT_EQ(errors.size(), 8u);

    errors.clear();
    sizes = Vi::ParseRenderSizes("", settings, weights, errors);
    EXPECT_EQ(sizes, (std::vector<Vi::Resolution>{ { 128, 64 } }));
    EXPECT_EQ(weights, (std::vector<float>{ 1.0f }));
    EXPECT_EQ(errors.size(), 1u);
}

// Face (turn to camera): the body is to turn by the yaw offset -- 90 degrees here. The offset is left alone until the
// body has turned: at the next observation the turn the controller applied comes off it, and it is 0; a turn that was
// dropped applies nothing and leaves it where it was. The held rates and the pitch are kept; with the offset already
// ~0 face turns nothing (no packet follows).
TEST(VisionFreeLookTest, FaceTurnsTheBodyToTheCamera)
{
    Vi::Settings settings;
    FL::State state = Fresh(settings);
    auto const turn = Choice(90.0f, 0.0f, FL::ZOOM_HOLD);
    FL::Apply(state, turn.data(), settings);
    FL::Advance(state, 0.0f);
    FL::Advance(state, 1.0f);
    ASSERT_NEAR(state.YawOffset, 90.0f * DEG, 1e-5f);
    float const pitch = state.Pitch;

    auto const face = Choice(0.0f, 0.0f, FL::ZOOM_FACE);
    float const body = FL::Apply(state, face.data(), settings);
    EXPECT_NEAR(body, 90.0f * DEG, 1e-5f);
    EXPECT_NEAR(state.YawOffset, 90.0f * DEG, 1e-5f);       // not yet: the body has not turned
    EXPECT_FLOAT_EQ(state.Pitch, pitch);

    // Dropped (the server held the body): nothing applied, nothing changes, and face is there to press again.
    FL::Advance(state, 0.25f, 0.0f);
    EXPECT_NEAR(state.YawOffset, 90.0f * DEG, 1e-5f);
    EXPECT_NEAR(FL::Apply(state, face.data(), settings), 90.0f * DEG, 1e-5f);

    // Applied: the body turned +90, and the camera's offset is 0 at the next observation -- it looks the same way in
    // the world, and the body faces it.
    FL::Advance(state, 0.25f, body);
    EXPECT_NEAR(state.YawOffset, 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(state.Pitch, pitch);

    // The rates a face choice asks for are held as with any other zoom choice.
    std::array<int32_t, FL::HEADS> const faceTurning = { int32_t(FL::YAW_STOP_INDEX) + 2,
        int32_t(FL::PITCH_STOP_INDEX) + 1, int32_t(FL::ZOOM_FACE) };
    FL::Apply(state, faceTurning.data(), settings);
    EXPECT_FLOAT_EQ(state.YawRate, 90.0f * DEG);
    EXPECT_FLOAT_EQ(state.PitchRate, 20.0f * DEG);

    // Already facing the camera: nothing.
    state.YawOffset = 0.0f;
    EXPECT_FLOAT_EQ(FL::Apply(state, face.data(), settings), 0.0f);
    state.YawOffset = 0.5f * FL::FACE_MIN;
    EXPECT_FLOAT_EQ(FL::Apply(state, face.data(), settings), 0.0f);
    // The applied turn wraps: an offset of -170 after a body turn of +20 is +170.
    state.YawOffset = 170.0f * DEG;
    state.YawRate = 0.0f;
    FL::Advance(state, 0.25f, -20.0f * DEG);
    EXPECT_NEAR(state.YawOffset, -170.0f * DEG, 1e-4f);
    // Every other choice turns no body.
    state.YawOffset = 1.0f;
    for (uint32_t zoom = 0; zoom < FL::ZOOM_FACE; ++zoom)
    {
        auto const other = Choice(0.0f, 0.0f, zoom);
        EXPECT_FLOAT_EQ(FL::Apply(state, other.data(), settings), 0.0f);
    }
    EXPECT_EQ(FL::ZOOM_CHOICES, 5u);
}
