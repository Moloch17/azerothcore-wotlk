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

#include "IntegrationTestFixture.h"
#include "Block.h"
#include "GoalBlock.h"
#include "SeatView.h"
#include "StageDefinition.h"

/// **The goal block tells no seat how near an objective it was not told of** (M1 redesigned; review 2026-10-06): a
/// trip's objective is a TravelTo target, reached within GoalBlock::PLACE_REACH (20 yd), and the reached bit is
/// observed (OBS_REACHED, StageScenario's view.GoalReached from GoalBlock::Status). With the compass withheld, or in
/// a stage with no compass, that bit would say "within 20 yd" through walls; there the objective has no place
/// (SeatView::ObjectivePlaceKnown), so TravelTo never reads reached -- arrival is the encounter's to decide and pay.
namespace
{
    namespace Cu = Animus::Curriculum;

    class GoalObjectiveLeakTest : public IntegrationTestFixture
    {
    protected:
        void SetUp() override
        {
            IntegrationTestFixture::SetUp();
            _bot = CreateTestPlayer();
            _bot->SetMaxHealth(100);
            _bot->SetHealth(100);
            _bot->Relocate(0.0f, 0.0f, 0.0f, 0.0f);
        }

        /// TravelTo the trip's objective, standing 5 yd from it: (reached, possible).
        std::pair<bool, bool> TravelFiveYardsOff(bool placeKnown)
        {
            Cu::SeatView view;
            view.Bot = _bot;
            view.HasObjective = true;
            view.Objective = Position(5.0f, 0.0f, 0.0f);
            view.ObjectivePlaceKnown = placeKnown;
            bool reached = false;
            bool possible = false;
            Cu::GoalBlock::Status(view, Cu::MakeGoal(Cu::SeatGoal::TravelTo, Cu::GOAL_TARGET_ASSIGNMENT), reached,
                possible);
            return { reached, possible };
        }

        TestPlayer* _bot = nullptr;
    };
}

TEST_F(GoalObjectiveLeakTest, AWithheldCompassLeavesTravelToUnreached)
{
    // The compass shown: reached at 5 yd, as before.
    auto const shown = TravelFiveYardsOff(Cu::GoalBlock::ObjectivePlaceKnown(true, false, false));
    EXPECT_TRUE(shown.first);
    EXPECT_TRUE(shown.second);
    // Withheld: the observed reached bit stays 0 -- and the goal is still possible, so it is held, not ended.
    auto const withheld = TravelFiveYardsOff(Cu::GoalBlock::ObjectivePlaceKnown(true, true, false));
    EXPECT_FALSE(withheld.first);
    EXPECT_TRUE(withheld.second);
    // And it has no place to read a distance from.
    Cu::SeatView view;
    view.HasObjective = true;
    view.ObjectivePlaceKnown = false;
    Position where;
    EXPECT_FALSE(Cu::GoalBlock::PlaceOf(view, Cu::GOAL_TARGET_ASSIGNMENT, where));
}

TEST_F(GoalObjectiveLeakTest, AStageWithoutACompassNeverKnowsThePlace)
{
    Cu::StageDefinition const* seek = Cu::FindStage("move2_seek");
    ASSERT_NE(seek, nullptr);
    bool const known = Cu::GoalBlock::ObjectivePlaceKnown(seek->Has(Cu::BlockId::Compass), false,
        seek->Has(Cu::BlockId::Travel));
    EXPECT_FALSE(known);
    EXPECT_FALSE(TravelFiveYardsOff(known).first);
    // M1 knows it while its compass is shown; a travel block's own bearing knows it too.
    Cu::StageDefinition const* controls = Cu::FindStage("move1_controls");
    ASSERT_NE(controls, nullptr);
    EXPECT_TRUE(Cu::GoalBlock::ObjectivePlaceKnown(controls->Has(Cu::BlockId::Compass), false, false));
    EXPECT_FALSE(Cu::GoalBlock::ObjectivePlaceKnown(controls->Has(Cu::BlockId::Compass), true, false));
    EXPECT_TRUE(Cu::GoalBlock::ObjectivePlaceKnown(false, false, true));
}
