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

#include "FreeLook.h"
#include "MoveControls.h"
#include "Protocol.h"
#include "StageState.h"
#include "gtest/gtest.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

/*
 * Protocol 22 (camera-vision.FREELOOK.md C): SPEC's LookHeads, ACT's look section and its refusal of values out of
 * range; and R1, looking is free.
 */
namespace
{
    namespace Vi = Animus::Vision;
    namespace FL = Animus::Vision::FreeLook;
    namespace Cu = Animus::Curriculum;
    namespace MC = Animus::Curriculum::MoveControls;
    using namespace AnimusForge;

    /// An ACT body (after its header) as the learner packs it: actions, goals when given, look when given.
    std::vector<char> Body(std::vector<int32> const& actions, std::vector<int32> const& goals,
        std::vector<int32> const& look)
    {
        std::vector<char> body;
        for (std::vector<int32> const* part : { &actions, &goals, &look })
        {
            std::size_t const at = body.size();
            body.resize(at + part->size() * sizeof(int32));
            if (!part->empty())
                std::memcpy(body.data() + at, part->data(), part->size() * sizeof(int32));
        }
        return body;
    }

    std::vector<int32> LookOf(std::vector<char> const& body, ActCut const& cut, std::size_t rows)
    {
        std::vector<int32> look(rows * FL::HEADS);
        std::memcpy(look.data(), body.data() + cut.LookOffset, look.size() * sizeof(int32));
        return look;
    }
}

// SPEC is "<12I32s4I", 96 bytes, ending with ImageBytes, LookHeads, then MapBytes (protocol 24, the mental map).
TEST(VisionProtocolTest, SpecCarriesLookHeads)
{
    EXPECT_EQ(PROTOCOL_VERSION, 24u);
    EXPECT_EQ(sizeof(SpecMsg), 96u);
    EXPECT_EQ(offsetof(SpecMsg, ImageBytes), 84u);
    EXPECT_EQ(offsetof(SpecMsg, LookHeads), 88u);
    EXPECT_EQ(offsetof(SpecMsg, MapBytes), 92u);
    EXPECT_EQ(FL::HEADS, 3u);
    EXPECT_EQ(FL::HEAD_SIZES, (std::array<uint32_t, 3>{ 7, 5, 5 }));
}

// ACT with look values: the look section follows the actions, or the actions and goals, agent-major; the values come
// back as they were sent.
TEST(VisionProtocolTest, ActCarriesTheLook)
{
    std::size_t const rows = 4;     // two envs of two agents
    std::vector<int32> const actions = { 1, 2, 3, 4 };
    std::vector<int32> const goals = { 0, -1, 1, -1, 2, -1, 3, -1 };
    std::vector<int32> const look = { 3, 2, 0, 6, 4, 3, 0, 0, 1, 5, 1, 2 };

    std::vector<char> body = Body(actions, {}, look);
    ActCut cut = CutAct(body.size(), rows, FL::HEADS);
    ASSERT_TRUE(cut.Valid);
    EXPECT_FALSE(cut.Goals);
    EXPECT_EQ(LookOf(body, cut, rows), look);
    EXPECT_EQ(BadLookRow(LookOf(body, cut, rows).data(), rows, FL::HEADS, FL::HEAD_SIZES.data()), -1);

    body = Body(actions, goals, look);
    cut = CutAct(body.size(), rows, FL::HEADS);
    ASSERT_TRUE(cut.Valid);
    EXPECT_TRUE(cut.Goals);
    EXPECT_EQ(LookOf(body, cut, rows), look);

    // A vision stage needs the look: an ACT without it is refused, with or without goals.
    EXPECT_FALSE(CutAct(Body(actions, {}, {}).size(), rows, FL::HEADS).Valid);
    EXPECT_FALSE(CutAct(Body(actions, goals, {}).size(), rows, FL::HEADS).Valid);
}

// Without a camera ACT is protocol 21's, byte for byte: actions, or actions and goals; a look section is refused.
TEST(VisionProtocolTest, ActWithoutVisionIsProtocol21)
{
    std::size_t const rows = 3;
    std::vector<int32> const actions = { 5, 6, 7 };
    std::vector<int32> const goals = { 0, 1, 2, 3, 4, 5 };
    ActCut cut = CutAct(Body(actions, {}, {}).size(), rows, 0);
    EXPECT_TRUE(cut.Valid);
    EXPECT_FALSE(cut.Goals);
    cut = CutAct(Body(actions, goals, {}).size(), rows, 0);
    EXPECT_TRUE(cut.Valid);
    EXPECT_TRUE(cut.Goals);
    EXPECT_FALSE(CutAct(Body(actions, {}, std::vector<int32>(rows * FL::HEADS, 0)).size(), rows, 0).Valid);
}

// A look value out of its head's range is a protocol error: BadLookRow names the first such row. A row of 0s (a
// director's placeholder) is in range.
TEST(VisionProtocolTest, ActRefusesLookOutOfRange)
{
    std::size_t const rows = 3;
    std::vector<int32> look = { 0, 0, 0, 6, 4, 3, 3, 2, 0 };
    EXPECT_EQ(BadLookRow(look.data(), rows, FL::HEADS, FL::HEAD_SIZES.data()), -1);
    for (auto const& [row, head, value] : { std::array<int32, 3>{ 1, 0, 7 }, std::array<int32, 3>{ 2, 1, 5 },
             std::array<int32, 3>{ 0, 2, 5 }, std::array<int32, 3>{ 1, 2, -1 }, std::array<int32, 3>{ 2, 0, -3 } })
    {
        std::vector<int32> bad = look;
        bad[std::size_t(row) * FL::HEADS + std::size_t(head)] = value;
        EXPECT_EQ(BadLookRow(bad.data(), rows, FL::HEADS, FL::HEAD_SIZES.data()), row);
    }
}

// R1, looking is free: a look choice reaches the seat's camera and nothing else. Two seats given the same movement
// press and different look choices hold the same controls, are charged the same for the press, and have the same
// reward ledger; only their cameras differ. (The scenario's ApplyLook feeds FreeLook::Apply alone, never
// ApplySeatAction, where every action price, repeat, tally and stuck check lives.)
TEST(VisionProtocolTest, LookingIsFree)
{
    Vi::Settings const settings;
    Cu::SeatState a;
    Cu::SeatState b;
    FL::Reset(a.Look, settings);
    FL::Reset(b.Look, settings);

    std::array<int32, FL::HEADS> const still = FL::NEUTRAL;
    std::array<int32, FL::HEADS> const spin = { 0, 4, int32(FL::ZOOM_OUT) };
    for (uint64 decision = 0; decision < 8; ++decision)
    {
        FL::Apply(a.Look, still.data(), settings);
        FL::Apply(b.Look, (decision % 2 ? still : spin).data(), settings);
        uint32 const press = decision % 2 ? MC::ACTION_TURN_FIRST + 1 : MC::ACTION_MOVE_FORWARD;
        MC::PressOutcome const pa = MC::Press(a.Controls, press, decision * 250, 1000);
        MC::PressOutcome const pb = MC::Press(b.Controls, press, decision * 250, 1000);
        EXPECT_EQ(pa.Changed, pb.Changed);
        EXPECT_FLOAT_EQ(pa.JitterWeight, pb.JitterWeight);
        EXPECT_FLOAT_EQ(pa.Effort, pb.Effort);
        EXPECT_EQ(pa.TurnReversals, pb.TurnReversals);
        FL::Advance(a.Look, 0.25f);
        FL::Advance(b.Look, 0.25f);
    }

    EXPECT_EQ(a.Controls.Held.Forward, b.Controls.Held.Forward);
    EXPECT_EQ(a.Controls.Held.Strafe, b.Controls.Held.Strafe);
    EXPECT_FLOAT_EQ(a.Controls.Held.TurnRate, b.Controls.Held.TurnRate);
    EXPECT_FLOAT_EQ(a.Controls.Held.PitchRate, b.Controls.Held.PitchRate);
    EXPECT_EQ(a.ControlChanges, b.ControlChanges);
    EXPECT_EQ(a.OptionPresses, b.OptionPresses);
    for (std::size_t term = 0; term < Cu::REWARD_TERM_COUNT; ++term)
        EXPECT_FLOAT_EQ(a.Rewards.Episode(Cu::RewardTerm(term)), b.Rewards.Episode(Cu::RewardTerm(term)));
    // ... and the cameras did differ.
    EXPECT_NE(a.Look.ZoomLevel, b.Look.ZoomLevel);
}
