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

#include "MoveControls.h"
#include "gtest/gtest.h"
#include <set>
#include <string>

namespace MC = Animus::Curriculum::MoveControls;

namespace
{
    constexpr uint32_t DECAY = 2500;
}

// The layout the manifest describes: 25 actions with distinct names, at a revision past the old design's 0 and 1, so a
// checkpoint or a manifest of the bearing lattice is refused on resume and seeded fresh (criterion A).
TEST(MoveControlsTest, LayoutIsTheControllersAtRevisionTwo)
{
    EXPECT_EQ(uint32_t(MC::ACTION_COUNT), 25u);
    EXPECT_GE(MC::REVISION, 2u);
    std::set<std::string> names(MC::NAMES.begin(), MC::NAMES.end());
    EXPECT_EQ(names.size(), size_t(MC::ACTION_COUNT));
    EXPECT_STREQ(MC::NAMES[MC::ACTION_TURN_STOP], "turn_stop");
    EXPECT_STREQ(MC::NAMES[MC::ACTION_PITCH_STOP], "pitch_stop");
    EXPECT_STREQ(MC::NAMES[MC::ACTION_JUMP], "jump");
    EXPECT_STREQ(MC::NAMES[MC::ACTION_WALK_TOGGLE], "walk_toggle");
    EXPECT_EQ(MC::TURN_RATES_DEG[MC::TURN_STOP_INDEX], 0.0f);
    EXPECT_EQ(MC::PITCH_RATES_DEG[MC::PITCH_STOP_INDEX], 0.0f);
}

// Masks only what is physically impossible: on the ground the pitch rates, ascend and descend; falling or flying the
// jump; dead, everything. The stops stay legal everywhere.
TEST(MoveControlsTest, MasksOnlyTheImpossible)
{
    MC::MaskState ground{ true, true, false };
    MC::MaskState falling{ true, false, false };
    MC::MaskState swimming{ true, true, true };
    MC::MaskState flying{ true, false, true };
    MC::MaskState dead{ false, true, true };

    for (uint32_t action = 0; action < MC::ACTION_COUNT; ++action)
        EXPECT_FALSE(MC::Allowed(action, dead)) << MC::NAMES[action];

    EXPECT_TRUE(MC::Allowed(MC::ACTION_JUMP, ground));
    EXPECT_FALSE(MC::Allowed(MC::ACTION_ASCEND, ground));
    EXPECT_FALSE(MC::Allowed(MC::ACTION_DESCEND, ground));
    EXPECT_FALSE(MC::Allowed(MC::ACTION_PITCH_FIRST, ground));
    EXPECT_TRUE(MC::Allowed(MC::ACTION_PITCH_STOP, ground));
    EXPECT_TRUE(MC::Allowed(MC::ACTION_VERTICAL_STOP, ground));
    for (uint32_t turn = 0; turn < MC::TURN_COUNT; ++turn)
        EXPECT_TRUE(MC::Allowed(MC::ACTION_TURN_FIRST + turn, ground));

    EXPECT_FALSE(MC::Allowed(MC::ACTION_JUMP, falling));
    EXPECT_TRUE(MC::Allowed(MC::ACTION_MOVE_FORWARD, falling));     // held, does nothing in the air: priced, not masked

    EXPECT_TRUE(MC::Allowed(MC::ACTION_JUMP, swimming));            // the swim jump, at any depth (C0c)
    EXPECT_TRUE(MC::Allowed(MC::ACTION_ASCEND, swimming));
    EXPECT_TRUE(MC::Allowed(MC::ACTION_PITCH_FIRST + 4, swimming));

    EXPECT_FALSE(MC::Allowed(MC::ACTION_JUMP, flying));
    EXPECT_TRUE(MC::Allowed(MC::ACTION_DESCEND, flying));
    EXPECT_FALSE(MC::Allowed(MC::ACTION_COUNT, ground));
}

// Each press sets one held control; pressing the value already held changes nothing and is free.
TEST(MoveControlsTest, PressesSetHeldControls)
{
    MC::SeatControls seat;
    EXPECT_TRUE(MC::Press(seat, MC::ACTION_MOVE_FORWARD, 0, DECAY).Changed);
    EXPECT_EQ(seat.Held.Forward, 1);
    EXPECT_FALSE(MC::Press(seat, MC::ACTION_MOVE_FORWARD, 250, DECAY).Changed);
    MC::Press(seat, MC::ACTION_STRAFE_LEFT, 500, DECAY);
    EXPECT_EQ(seat.Held.Strafe, -1);
    MC::Press(seat, MC::ACTION_TURN_FIRST + 6, 750, DECAY);        // left 90 deg/s
    EXPECT_NEAR(seat.Held.TurnRate, 90.0f * MC::DEG, 1e-6f);
    MC::Press(seat, MC::ACTION_TURN_STOP, 1000, DECAY);
    EXPECT_EQ(seat.Held.TurnRate, 0.0f);
    MC::Press(seat, MC::ACTION_PITCH_FIRST + 3, 1000, DECAY);      // up 30
    EXPECT_NEAR(seat.Held.PitchRate, 30.0f * MC::DEG, 1e-6f);
    MC::Press(seat, MC::ACTION_ASCEND, 1000, DECAY);
    EXPECT_EQ(seat.Held.Vertical, 1);
    MC::Press(seat, MC::ACTION_WALK_TOGGLE, 1000, DECAY);
    EXPECT_TRUE(seat.Held.Walk);
    EXPECT_TRUE(MC::Press(seat, MC::ACTION_JUMP, 1000, DECAY).Changed);
    EXPECT_TRUE(seat.Held.Jump);
    MC::Press(seat, MC::ACTION_MOVE_STOP, 1250, DECAY);
    MC::Press(seat, MC::ACTION_STRAFE_STOP, 1250, DECAY);
    MC::Press(seat, MC::ACTION_VERTICAL_STOP, 1250, DECAY);
    EXPECT_EQ(seat.Held.Forward, 0);
    EXPECT_EQ(seat.Held.Strafe, 0);
    EXPECT_EQ(seat.Held.Vertical, 0);
}

// A reversal of a recent choice is priced by what it takes back, weighed by recency; a choice that agrees costs
// nothing. Forward straight to back is half a turn of the feet.
TEST(MoveControlsTest, ReversalsArePricedByRecency)
{
    MC::SeatControls seat;
    MC::Press(seat, MC::ACTION_MOVE_FORWARD, 0, DECAY);
    MC::PressOutcome const back = MC::Press(seat, MC::ACTION_MOVE_BACK, 250, DECAY);
    EXPECT_NEAR(back.JitterWeight, 2.0f, 1e-6f);                  // held until now: full weight
    EXPECT_EQ(back.FeetFlip, 1.0f);

    // Stopped, then reversed three seconds later: a weave, weighed by how long ago forward was let go of.
    MC::SeatControls late;
    MC::Press(late, MC::ACTION_MOVE_FORWARD, 0, DECAY);
    MC::Press(late, MC::ACTION_MOVE_STOP, 1000, DECAY);
    MC::PressOutcome const weave = MC::Press(late, MC::ACTION_MOVE_BACK, 4000, DECAY);
    EXPECT_NEAR(weave.JitterWeight, 2.0f * Animus::Curriculum::MovePrice::Recency(3000, DECAY), 1e-5f);
    EXPECT_EQ(weave.FeetFlip, 0.0f);
    EXPECT_EQ(weave.Weaves, 1u);

    // A turn rate against the last: the smaller rate in quarter turns a second. Left 90 then right 180.
    MC::SeatControls turn;
    MC::Press(turn, MC::ACTION_TURN_FIRST + 6, 0, DECAY);
    MC::PressOutcome const swing = MC::Press(turn, MC::ACTION_TURN_FIRST + 1, 250, DECAY);
    EXPECT_NEAR(swing.JitterWeight, 1.0f, 1e-5f);
    EXPECT_EQ(swing.TurnReversals, 1u);
    EXPECT_NEAR(swing.Effort, 1.0f, 1e-6f);                         // 180 deg/s is a full press
    // The same way again is a correction, not a reversal; a small rate is a small press.
    MC::PressOutcome const agree = MC::Press(turn, MC::ACTION_TURN_FIRST + 3, 500, DECAY);
    EXPECT_EQ(agree.JitterWeight, 0.0f);
    EXPECT_NEAR(agree.Effort, 30.0f / 180.0f, 1e-5f);

    // A climb against a dive is a pitch reversal.
    MC::SeatControls climb;
    MC::Press(climb, MC::ACTION_DESCEND, 0, DECAY);
    EXPECT_EQ(MC::Press(climb, MC::ACTION_ASCEND, 500, DECAY).PitchReversals, 1u);
}
