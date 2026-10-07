/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "BotAccounts.h"
#include "gtest/gtest.h"

namespace Ba = Animus::BotAccounts;

// The learned seats' bot accounts start where they always did (the owner, opponent and ambusher ranges that sat past
// them were deleted with the first curriculum, 2026-10-07): account ids may be remembered by a run's cluster state, so
// the seat range's start, its stride and the envs it holds do not move.
TEST(BotAccountsTest, TheSeatRangeStartsWhereItDid)
{
    EXPECT_EQ(Ba::BASE, 0x7F000000u);
    EXPECT_EQ(Ba::Seat(0, 0, 0), 0x7F000000u);
    EXPECT_EQ(Ba::Seat(0, 0, 1), 0x7F000001u);
    EXPECT_EQ(Ba::Seat(0, 1, 0), 0x7F000002u);
    EXPECT_EQ(Ba::Seat(1, 0, 0), 0x7F000000u + 80);
    EXPECT_EQ(Ba::Seat(3, 5, 1), 0x7F000000u + 3 * 80 + 5 * 2 + 1);
    EXPECT_EQ(Ba::SEATS_PER_ENV, 40u);
    EXPECT_EQ(Ba::SESSIONS_PER_BOT, 2u);
    EXPECT_EQ(Ba::MAX_ENVS, 1250u);
    EXPECT_EQ(Ba::Probe(0), 0x7F000000u - 1);
}
