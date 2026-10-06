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

#include "FloorScan.h"
#include "gtest/gtest.h"

namespace Scan = Animus::Curriculum::FloorScan;

// Each cell's verdict: the navmesh walks it and the controller has no floor (HOLE), both but apart (MISMATCH), the
// controller's floor too steep (STEEP), only the controller (NONAV), neither (UNWALKABLE), else OK.
TEST(FloorScanTest, CellsAreClassifiedAsTheScanDefinesThem)
{
    EXPECT_EQ(Scan::Classify(true, -25.61f, true, -25.61f, 1.0f), Scan::Cell::Ok);
    EXPECT_EQ(Scan::Classify(true, -25.61f, false, 0.0f, 0.0f), Scan::Cell::Hole);
    EXPECT_EQ(Scan::Classify(true, -25.61f, true, -26.52f, 1.0f), Scan::Cell::Mismatch);
    EXPECT_EQ(Scan::Classify(true, -25.61f, true, -25.20f, 1.0f), Scan::Cell::Ok);          // within 0.75 yd
    EXPECT_EQ(Scan::Classify(true, -24.22f, true, -24.85f, 0.94f), Scan::Cell::Ok);         // the navmesh at a foot
    EXPECT_EQ(Scan::Classify(true, -25.61f, true, -25.61f, 0.60f), Scan::Cell::Steep);
    EXPECT_EQ(Scan::Classify(true, -25.61f, true, -25.61f, 0.71f), Scan::Cell::Ok);         // 45 degrees: walkable
    EXPECT_EQ(Scan::Classify(false, 0.0f, true, -25.61f, 1.0f), Scan::Cell::NoNav);
    EXPECT_EQ(Scan::Classify(false, 0.0f, false, 0.0f, 0.0f), Scan::Cell::Unwalkable);
    // The ASCII map shows the worst verdict of a yard's cells.
    EXPECT_GT(Scan::Severity(Scan::Cell::Hole), Scan::Severity(Scan::Cell::Mismatch));
    EXPECT_GT(Scan::Severity(Scan::Cell::Mismatch), Scan::Severity(Scan::Cell::Steep));
    EXPECT_GT(Scan::Severity(Scan::Cell::Ok), Scan::Severity(Scan::Cell::Unwalkable));
    EXPECT_EQ(Scan::Glyph(Scan::Cell::Hole), 'H');
    EXPECT_EQ(Scan::Glyph(Scan::Cell::Unwalkable), ' ');
}

// The grids a box covers (the route planner's and the scan's: every grid between the ends gets its tiles, with a
// grid's margin): the Stockades' hallway is in one grid; a span across a grid line touches both.
TEST(FloorScanTest, GridSpansCoverTheBox)
{
    auto const hallway = Scan::GridSpan(50.0f, 180.0f);
    EXPECT_EQ(hallway.first, 0);
    EXPECT_EQ(hallway.second, 0);
    auto const across = Scan::GridSpan(-10.0f, 10.0f);
    EXPECT_EQ(across.first, -1);
    EXPECT_EQ(across.second, 0);
    auto const reversed = Scan::GridSpan(600.0f, 100.0f);
    EXPECT_EQ(reversed.first, 0);
    EXPECT_EQ(reversed.second, 1);
    auto const widened = Scan::GridSpan(50.0f, 180.0f, 100.0f);
    EXPECT_EQ(widened.first, -1);
    EXPECT_EQ(widened.second, 0);
}
