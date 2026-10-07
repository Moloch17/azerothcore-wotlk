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

#include "Protocol.h"
#include "StandIn.h"
#include "gtest/gtest.h"
#include <set>

namespace SI = Animus::Curriculum::StandIn;

namespace
{
    bool Same(SI::Style const& a, SI::Style const& b)
    {
        return a.Seed == b.Seed && a.Leads == b.Leads && a.Wanted == b.Wanted;
    }
}

TEST(StandInTest, StyleDrawsAreDeterministicPerSeed)
{
    SI::Tuning const tuning;
    for (uint64_t seed = 0; seed < 64; ++seed)
        EXPECT_TRUE(Same(SI::Draw(seed, tuning, true), SI::Draw(seed, tuning, true))) << seed;

    // An evaluation's seed index meets the same person every time, and different indexes meet different people.
    EXPECT_EQ(SI::EvaluationSeed(17), SI::EvaluationSeed(17));
    std::set<uint64_t> seeds;
    for (uint32_t index = 0; index < 256; ++index)
        seeds.insert(SI::EvaluationSeed(index));
    EXPECT_EQ(seeds.size(), 256u);
}

TEST(StandInTest, StylesCoverEveryChoice)
{
    SI::Tuning const tuning;
    std::set<bool> leads;
    std::set<SI::Role> roles;
    for (uint32_t index = 0; index < 512; ++index)
    {
        SI::Style const style = SI::Draw(SI::EvaluationSeed(index), tuning, true);
        leads.insert(style.Leads);
        roles.insert(style.Wanted);
        // A party with an owner (or a drilled seat 0) is not the stand-in's to lead.
        EXPECT_FALSE(SI::Draw(SI::EvaluationSeed(index), tuning, false).Leads);
    }
    EXPECT_EQ(leads.size(), 2u);
    EXPECT_EQ(roles.size(), 3u);

    // Chances at their ends: 0 never, 100 always.
    SI::Tuning always;
    always.LeadChance = 0;
    always.TankChance = 100;
    always.HealerChance = 0;
    for (uint64_t seed = 0; seed < 32; ++seed)
    {
        SI::Style const style = SI::Draw(seed, always, true);
        EXPECT_FALSE(style.Leads);
        EXPECT_EQ(style.Wanted, SI::Role::Tank);
    }
}

TEST(StandInTest, RoleIsWithinTheBuild)
{
    EXPECT_EQ(SI::RoleFor(SI::Role::Tank, true, false), SI::Role::Tank);
    EXPECT_EQ(SI::RoleFor(SI::Role::Tank, false, true), SI::Role::Damage);
    EXPECT_EQ(SI::RoleFor(SI::Role::Healer, false, true), SI::Role::Healer);
    EXPECT_EQ(SI::RoleFor(SI::Role::Healer, true, false), SI::Role::Damage);
    EXPECT_EQ(SI::RoleFor(SI::Role::Damage, true, true), SI::Role::Damage);
}

TEST(StandInTest, TheStandInsRowIsPresentTwo)
{
    // The wire's present (protocol 25): the stand-in's seat is a real row (not 0, which the learner skips) that the
    // learner tells from its own (1) and plays with a frozen partner; an empty seat is 0 whatever else.
    EXPECT_EQ(SI::Presence(true, false), SI::PRESENT_LEARNER);
    EXPECT_EQ(SI::Presence(true, true), SI::PRESENT_STAND_IN);
    EXPECT_EQ(SI::Presence(false, true), SI::PRESENT_NONE);
    EXPECT_EQ(SI::Presence(false, false), SI::PRESENT_NONE);
    EXPECT_EQ(SI::PRESENT_LEARNER, 1);
    EXPECT_EQ(SI::PRESENT_STAND_IN, 2);
}

TEST(StandInTest, NoEpisodeHasAStandInWithoutTheModeFlag)
{
    // A training MODE without MODE_FLAG_STAND_IN (a learner with no partner to field) leaves every party without one,
    // whatever the arena's share, and draws no random number for it.
    uint32_t rolls = 0;
    auto const roll = [&rolls](int32_t) { ++rolls; return true; };
    for (bool evaluating : { false, true })
        for (int32_t share : { 0, 1, 50, 100 })
            EXPECT_FALSE(SI::Fields(false, true, 5, evaluating, share, roll)) << evaluating << " " << share;
    EXPECT_EQ(rolls, 0u);

    // The flag itself: bit 2 (bit 1 is unused), and a MODE with no flags set does not carry it.
    EXPECT_EQ(AnimusForge::MODE_FLAG_STAND_IN, 2u);
    EXPECT_FALSE((0u & AnimusForge::MODE_FLAG_STAND_IN) != 0);
}

TEST(StandInTest, WithTheFlagAnEvaluationHasOneAndTrainingRollsItsShare)
{
    uint32_t rolls = 0;
    bool answer = false;
    auto const roll = [&](int32_t) { ++rolls; return answer; };

    // Evaluation: every party episode, no roll.
    EXPECT_TRUE(SI::Fields(true, true, 5, true, 0, roll));
    EXPECT_EQ(rolls, 0u);
    // Training: no share, no roll, no stand-in; a share rolls, and the roll decides.
    EXPECT_FALSE(SI::Fields(true, true, 5, false, 0, roll));
    EXPECT_EQ(rolls, 0u);
    EXPECT_FALSE(SI::Fields(true, true, 5, false, 30, roll));
    answer = true;
    EXPECT_TRUE(SI::Fields(true, true, 5, false, 30, roll));
    EXPECT_EQ(rolls, 2u);
    // Never alone, never outside a party or a raid.
    EXPECT_FALSE(SI::Fields(true, true, 1, true, 100, roll));
    EXPECT_FALSE(SI::Fields(true, false, 5, true, 100, roll));
}
