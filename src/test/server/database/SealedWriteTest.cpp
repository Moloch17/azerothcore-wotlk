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

#include "DatabaseWorkerPool.h"
#include "gtest/gtest.h"

// A sealed pool's dropped write is logged once a kind: the second drop of the same statement is not.
TEST(SealedWriteTest, TheSameStatementLogsOnce)
{
    EXPECT_TRUE(NoteSealedWrite("sealed_test_world", "#7", "prepared statement 7"));
    EXPECT_FALSE(NoteSealedWrite("sealed_test_world", "#7", "prepared statement 7"));
    EXPECT_FALSE(NoteSealedWrite("sealed_test_world", "#7", "prepared statement 7"));
}

// The kind is per database and per statement: another statement, or the same one on another database, logs too.
TEST(SealedWriteTest, AnotherStatementOrDatabaseLogsAgain)
{
    EXPECT_TRUE(NoteSealedWrite("sealed_test_chars", "#7", "prepared statement 7"));
    EXPECT_TRUE(NoteSealedWrite("sealed_test_chars", "#8", "prepared statement 8"));
    EXPECT_TRUE(NoteSealedWrite("sealed_test_auth", "#7", "prepared statement 7"));
    EXPECT_TRUE(NoteSealedWrite("sealed_test_chars", "UPDATE gameobject SET state", "UPDATE gameobject SET state = 1"));
    EXPECT_FALSE(NoteSealedWrite("sealed_test_chars", "UPDATE gameobject SET state",
        "UPDATE gameobject SET state = 0"));
}
