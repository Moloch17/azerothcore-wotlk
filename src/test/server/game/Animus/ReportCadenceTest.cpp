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

#include "ReportCadence.h"
#include "gtest/gtest.h"

namespace Mv = Animus::Movement;
namespace Cd = Animus::Movement::Cadence;
using Ops = std::vector<uint16_t>;

// Each held control's change is its own packet the moment it happens, in the client's order (fn 0x6ef860).
TEST(ReportCadenceTest, AChangeSendsItsOpcodeAtOnce)
{
    EXPECT_EQ(Cd::Changes(0, Mv::Flag::FORWARD, false, false), (Ops{ Cd::Op::START_FORWARD }));
    EXPECT_EQ(Cd::Changes(Mv::Flag::FORWARD, Mv::Flag::BACKWARD, false, false), (Ops{ Cd::Op::START_BACKWARD }));
    EXPECT_EQ(Cd::Changes(Mv::Flag::BACKWARD, 0, false, false), (Ops{ Cd::Op::STOP }));
    EXPECT_EQ(Cd::Changes(0, Mv::Flag::STRAFE_LEFT | Mv::Flag::LEFT, false, false),
        (Ops{ Cd::Op::START_STRAFE_LEFT, Cd::Op::START_TURN_LEFT }));
    EXPECT_EQ(Cd::Changes(Mv::Flag::RIGHT, 0, false, false), (Ops{ Cd::Op::STOP_TURN }));
    EXPECT_EQ(Cd::Changes(Mv::Flag::DESCENDING, 0, false, false), (Ops{ Cd::Op::STOP_ASCEND }));
    EXPECT_EQ(Cd::Changes(0, Mv::Flag::SWIMMING | Mv::Flag::PITCH_DOWN, false, false),
        (Ops{ Cd::Op::START_SWIM, Cd::Op::START_PITCH_DOWN }));
    EXPECT_EQ(Cd::Changes(Mv::Flag::FORWARD, Mv::Flag::FORWARD | Mv::Flag::FALLING, true, false), (Ops{ Cd::Op::JUMP }));
    EXPECT_EQ(Cd::Changes(Mv::Flag::FALLING, 0, false, true), (Ops{ Cd::Op::FALL_LAND }));
    EXPECT_EQ(Cd::Changes(0, Mv::Flag::WALKING, false, false), (Ops{ Cd::Op::SET_WALK_MODE }));
    EXPECT_TRUE(Cd::Changes(Mv::Flag::FORWARD, Mv::Flag::FORWARD, false, false).empty());
}

// A heartbeat 500 ms after the last packet of any kind while moving, falling, ascending or descending; turning on the
// spot alone sends none (the client's 0xc0100f).
TEST(ReportCadenceTest, AHeartbeatFollowsTheLastPacketByHalfASecondWhileMoving)
{
    EXPECT_FALSE(Cd::HeartbeatDue(Mv::Flag::FORWARD, 1499, 1000));
    EXPECT_TRUE(Cd::HeartbeatDue(Mv::Flag::FORWARD, 1500, 1000));
    EXPECT_TRUE(Cd::HeartbeatDue(Mv::Flag::FALLING, 2000, 1000));
    EXPECT_FALSE(Cd::HeartbeatDue(Mv::Flag::LEFT, 9000, 1000));
    EXPECT_FALSE(Cd::HeartbeatDue(0, 9000, 1000));
    EXPECT_EQ(Cd::HEARTBEAT_MS, 500u);
}
