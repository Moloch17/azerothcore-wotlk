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

#include "RewardLedger.h"
#include "gtest/gtest.h"

#include <set>
#include <string>

using Animus::Curriculum::RewardCategory;
using Animus::Curriculum::RewardLedger;
using Animus::Curriculum::RewardTerm;

// The score (peak-play W0) is what evaluation, best.pt and the league are judged on: the Outcome and Cost terms as
// tuned. A rung's tier, a role's scale and every Shaping term change the reward and must never reach it.
TEST(RewardLedgerTest, ScoreIsOutcomeAndCostBeforeTierAndRole)
{
    RewardLedger ledger;
    ledger.Scale(RewardTerm::Kill, 0.5f);          // a role's share of the kill
    ledger.Add(RewardTerm::Kill, 4.0f, 2.0f);      // paid 4 x 0.5 x 2 = 4, scored 4
    ledger.Add(RewardTerm::Death, -3.0f, 0.5f);    // paid -1.5, scored -3
    ledger.Add(RewardTerm::Approach, 7.0f, 2.0f);  // shaping: paid 14, never scored
    EXPECT_FLOAT_EQ(ledger.TakeStep(), 4.0f - 1.5f + 14.0f);
    EXPECT_FLOAT_EQ(ledger.Episode(RewardTerm::Kill), 4.0f);
    EXPECT_FLOAT_EQ(ledger.Episode(RewardTerm::Death), -1.5f);
    EXPECT_FLOAT_EQ(ledger.Episode(RewardTerm::Approach), 14.0f);
    EXPECT_FLOAT_EQ(ledger.Score(), 1.0f);

    // Paid into a row already taken: the episode's sums and the score, never the next step.
    ledger.AddTaken(RewardTerm::Clear, 2.0f);
    ledger.AddTaken(RewardTerm::GoalReached, 9.0f);
    EXPECT_FLOAT_EQ(ledger.Score(), 3.0f);
    EXPECT_FLOAT_EQ(ledger.TakeStep(), 0.0f);

    // The same kill at an easy and a hard rung: paid differently, scored the same.
    RewardLedger easy;
    RewardLedger hard;
    easy.Add(RewardTerm::Kill, 4.0f, 0.5f);
    hard.Add(RewardTerm::Kill, 4.0f, 2.0f);
    EXPECT_NE(easy.TakeStep(), hard.TakeStep());
    EXPECT_FLOAT_EQ(easy.Score(), hard.Score());

    ledger.ResetEpisode();
    EXPECT_FLOAT_EQ(ledger.Score(), 0.0f);
    EXPECT_FLOAT_EQ(ledger.Episode(RewardTerm::Kill), 0.0f);
}

// The shaping fade (peak-play W1): at scale 0 the reward is the outcome and cost terms alone, at any scale the score
// is untouched, and what Add returns is what was paid -- what the director's mirror of a seat's shaping must use.
TEST(RewardLedgerTest, ShapingScaleTouchesOnlyShaping)
{
    RewardLedger full;
    RewardLedger faded;
    faded.SetShaping(0.0f);
    for (RewardLedger* ledger : { &full, &faded })
    {
        ledger->Add(RewardTerm::Kill, 4.0f, 2.0f);
        ledger->Add(RewardTerm::Timeout, -1.0f);
        ledger->Add(RewardTerm::Threat, 3.0f, 2.0f);
        ledger->Add(RewardTerm::GoalProgress, 0.5f);
    }
    EXPECT_FLOAT_EQ(full.TakeStep(), 8.0f - 1.0f + 6.0f + 0.5f);
    EXPECT_FLOAT_EQ(faded.TakeStep(), 8.0f - 1.0f);
    EXPECT_FLOAT_EQ(full.Score(), faded.Score());
    EXPECT_FLOAT_EQ(faded.Episode(RewardTerm::Threat), 0.0f);

    RewardLedger half;
    half.SetShaping(0.5f);
    EXPECT_FLOAT_EQ(half.Add(RewardTerm::OrderMatch, 2.0f), 1.0f);
    EXPECT_FLOAT_EQ(half.Add(RewardTerm::Kill, 2.0f), 2.0f);
    EXPECT_FLOAT_EQ(half.AddTaken(RewardTerm::GoalReached, 4.0f), 2.0f);
    EXPECT_FLOAT_EQ(half.Score(), 2.0f);
}

// Every term has a column (reward_<name>) and a category; two terms under one name would share a column, which is how
// a drill's clean pull was read as the dungeon's kills.
TEST(RewardLedgerTest, EveryTermHasItsOwnNameAndACategory)
{
    std::set<std::string> names;
    for (std::size_t index = 0; index < Animus::Curriculum::REWARD_TERM_COUNT; ++index)
    {
        RewardTerm const term = RewardTerm(index);
        std::string const name(Animus::Curriculum::RewardTermName(term));
        EXPECT_FALSE(name.empty()) << index;
        EXPECT_NE(name, "unknown") << index;
        EXPECT_TRUE(names.insert(name).second) << name;

        RewardCategory const category = Animus::Curriculum::RewardTermCategory(term);
        EXPECT_NE(category, RewardCategory::None) << name;
        EXPECT_EQ(Animus::Curriculum::ScoresOutcome(term),
            category == RewardCategory::Outcome || category == RewardCategory::Cost) << name;
    }
}
