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

#include "StandIn.h"
#include "gtest/gtest.h"
#include <cmath>
#include <optional>
#include <set>
#include <vector>

namespace Mv = Animus::Movement;
namespace SI = Animus::Curriculum::StandIn;

namespace
{
    /// Open flat ground at z = 0.
    class Open final : public Mv::WorldQuery
    {
    public:
        [[nodiscard]] float FloorBelow(float, float, float z, float search) const override
        {
            return z >= -1e-3f && z <= search ? 0.0f : Mv::INVALID_FLOOR;
        }
        [[nodiscard]] float FloorNormalZ(float, float, float) const override { return 1.0f; }
        [[nodiscard]] Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float, float, float, float up) const override { return up; }
        [[nodiscard]] bool InTerrain(float, float, float z) const override { return z < -0.5f; }
    };

    /// Tuning with every quirk off; a test turns on the one it is about.
    SI::Tuning Quiet()
    {
        SI::Tuning tuning;
        tuning.PullEarlyChance = tuning.WanderChance = tuning.RestChance = tuning.LagChance = tuning.AfkChance = 0;
        return tuning;
    }

    /// A style with only `quirk` (or nothing), following or leading, at the fast pace.
    SI::Style Only(std::optional<SI::Quirk> quirk, bool leads = false, uint64_t seed = 7)
    {
        SI::Style style;
        style.Seed = seed;
        style.Leads = leads;
        if (quirk)
            style.Quirks[uint32_t(*quirk)] = true;
        return style;
    }

    /// Out of combat, a pull ahead 40 yards off and the leader 15 yards off.
    SI::Situation Calm(uint32_t nowMs)
    {
        SI::Situation seen;
        seen.NowMs = nowMs;
        seen.DecisionMs = 250;
        seen.HasTarget = true;
        seen.TargetYards = 40.0f;
        seen.HasLeader = true;
        seen.LeaderYards = 15.0f;
        return seen;
    }

    bool Same(SI::Style const& a, SI::Style const& b)
    {
        return a.Seed == b.Seed && a.Leads == b.Leads && a.Wanted == b.Wanted && a.Speed == b.Speed
            && a.Quirks == b.Quirks;
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
    std::set<SI::Pace> paces;
    std::vector<std::set<bool>> quirks(SI::QUIRKS);
    for (uint32_t index = 0; index < 512; ++index)
    {
        SI::Style const style = SI::Draw(SI::EvaluationSeed(index), tuning, true);
        leads.insert(style.Leads);
        roles.insert(style.Wanted);
        paces.insert(style.Speed);
        for (uint32_t quirk = 0; quirk < SI::QUIRKS; ++quirk)
            quirks[quirk].insert(style.Quirks[quirk]);
        // A party with an owner is the owner's to lead.
        EXPECT_FALSE(SI::Draw(SI::EvaluationSeed(index), tuning, false).Leads);
    }
    EXPECT_EQ(leads.size(), 2u);
    EXPECT_EQ(roles.size(), 3u);
    EXPECT_EQ(paces.size(), 2u);
    for (uint32_t quirk = 0; quirk < SI::QUIRKS; ++quirk)
        EXPECT_EQ(quirks[quirk].size(), 2u) << SI::QUIRK_NAMES[quirk];

    // Chances at their ends: 0 never, 100 always.
    SI::Tuning always = Quiet();
    always.AfkChance = 100;
    always.LeadChance = 0;
    always.SlowChance = 100;
    for (uint64_t seed = 0; seed < 32; ++seed)
    {
        SI::Style const style = SI::Draw(seed, always, true);
        EXPECT_TRUE(style.Has(SI::Quirk::Afk));
        EXPECT_FALSE(style.Has(SI::Quirk::Wander));
        EXPECT_FALSE(style.Leads);
        EXPECT_EQ(style.Speed, SI::Pace::Slow);
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

TEST(StandInTest, BehaviourIsDeterministicPerSeed)
{
    SI::Tuning const tuning;
    SI::Style const style = SI::Draw(99, tuning, false);
    SI::Behaviour first(style, SI::Role::Damage);
    SI::Behaviour second(style, SI::Role::Damage);
    for (uint32_t step = 0; step < 4800; ++step)
    {
        SI::Situation seen = Calm(step * 250);
        // A fight every so often, so every branch is walked.
        seen.InCombat = seen.PartyInCombat = (step / 200) % 3 == 1;
        seen.Health = (step % 500) < 50 ? 0.2f : 1.0f;
        SI::Intent const a = first.Decide(seen, tuning);
        SI::Intent const b = second.Decide(seen, tuning);
        ASSERT_EQ(a.Doing, b.Doing) << step;
        ASSERT_EQ(a.Go, b.Go) << step;
        ASSERT_EQ(a.Fight, b.Fight) << step;
    }
    for (uint32_t quirk = 0; quirk < SI::QUIRKS; ++quirk)
        EXPECT_EQ(first.Fired(SI::Quirk(quirk)), second.Fired(SI::Quirk(quirk)));
}

TEST(StandInTest, AQuirkOffNeverFires)
{
    SI::Tuning tuning = Quiet();
    // Rates high enough to fire on every decision, if the style had them.
    tuning.PullEarlyPerMinute = tuning.WanderPerMinute = tuning.RestPerMinute = tuning.LagPerMinute =
        tuning.AfkPerMinute = 1e6f;
    SI::Behaviour plays(Only(std::nullopt), SI::Role::Damage);
    for (uint32_t step = 0; step < 2400; ++step)
    {
        SI::Situation seen = Calm(step * 250);
        seen.Health = 0.1f;                         // low: still no rest without the quirk
        EXPECT_EQ(plays.Decide(seen, tuning).Doing, SI::Mode::Normal);
    }
    for (uint32_t quirk = 0; quirk < SI::QUIRKS; ++quirk)
        EXPECT_EQ(plays.Fired(SI::Quirk(quirk)), 0u) << SI::QUIRK_NAMES[quirk];
}

TEST(StandInTest, AfkLetsGoOfEverythingAndStaysAway)
{
    SI::Tuning tuning = Quiet();
    tuning.AfkPerMinute = 1e6f;
    tuning.AfkMinMs = tuning.AfkMaxMs = 10000;
    SI::Behaviour plays(Only(SI::Quirk::Afk), SI::Role::Damage);

    SI::Intent intent = plays.Decide(Calm(5000), tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::Afk);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Hold);
    EXPECT_FALSE(intent.Fight);
    EXPECT_EQ(plays.Fired(SI::Quirk::Afk), 1u);

    // A fight starting meanwhile does not bring it back: the party covers for it.
    SI::Situation fight = Calm(8000);
    fight.InCombat = fight.PartyInCombat = true;
    intent = plays.Decide(fight, tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::Afk);
    EXPECT_FALSE(intent.Fight);

    // Back once its while is over, and into the fight.
    fight.NowMs = 15250;
    intent = plays.Decide(fight, tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::Normal);
    EXPECT_TRUE(intent.Fight);

    // Holding nothing: the keys it holds while AFK are none at all.
    Mv::BodyState body;
    Mv::ControlState const keys = SI::Keys(body, false, 10.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, false);
    EXPECT_EQ(keys.Forward, 0);
    EXPECT_EQ(keys.Strafe, 0);
    EXPECT_EQ(keys.TurnRate, 0.0f);
    EXPECT_FALSE(keys.Jump);
}

TEST(StandInTest, PullEarlyGoesForThePullUntilItIsOn)
{
    SI::Tuning tuning = Quiet();
    tuning.PullEarlyPerMinute = 1e6f;
    SI::Behaviour plays(Only(SI::Quirk::PullEarly), SI::Role::Damage);

    SI::Situation seen = Calm(5000);
    SI::Intent intent = plays.Decide(seen, tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::PullEarly);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Target);
    EXPECT_FALSE(intent.Fight);                     // still 40 yards off

    seen.NowMs += 250;
    seen.TargetYards = 2.0f;
    intent = plays.Decide(seen, tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::PullEarly);
    EXPECT_TRUE(intent.Fight);                      // in reach: it pulls

    seen.NowMs += 250;
    seen.InCombat = seen.PartyInCombat = true;
    EXPECT_EQ(plays.Decide(seen, tuning).Doing, SI::Mode::Normal);
    EXPECT_EQ(plays.Fired(SI::Quirk::PullEarly), 1u);

    // Nothing to pull: no pulling early.
    SI::Behaviour idle(Only(SI::Quirk::PullEarly), SI::Role::Damage);
    SI::Situation empty = Calm(5000);
    empty.HasTarget = false;
    EXPECT_EQ(idle.Decide(empty, tuning).Doing, SI::Mode::Normal);
}

TEST(StandInTest, LagDriftsBehindAFollowerOnly)
{
    SI::Tuning tuning = Quiet();
    tuning.LagPerMinute = 1e6f;
    SI::Behaviour follower(Only(SI::Quirk::Lag), SI::Role::Damage);
    SI::Intent const intent = follower.Decide(Calm(5000), tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::Lag);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Leader);
    EXPECT_EQ(intent.StopYards, tuning.LagYards);

    SI::Behaviour leader(Only(SI::Quirk::Lag, true), SI::Role::Tank);
    for (uint32_t step = 0; step < 400; ++step)
        EXPECT_NE(leader.Decide(Calm(step * 250), tuning).Doing, SI::Mode::Lag);
    EXPECT_EQ(leader.Fired(SI::Quirk::Lag), 0u);
}

TEST(StandInTest, WanderAndRestEndWhenAFightStarts)
{
    SI::Tuning tuning = Quiet();
    tuning.WanderPerMinute = 1e6f;
    SI::Behaviour wanders(Only(SI::Quirk::Wander), SI::Role::Damage);
    SI::Intent intent = wanders.Decide(Calm(5000), tuning);
    EXPECT_EQ(intent.Doing, SI::Mode::Wander);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Spot);
    EXPECT_TRUE(wanders.Started());
    EXPECT_GE(wanders.SpotYards(), tuning.WanderMinYards);
    EXPECT_LE(wanders.SpotYards(), tuning.WanderMaxYards);
    SI::Situation fight = Calm(6000);
    fight.PartyInCombat = true;
    EXPECT_EQ(wanders.Decide(fight, tuning).Doing, SI::Mode::Normal);

    // A resting style stops when it runs low, whatever its rate.
    SI::Tuning resting = Quiet();
    resting.RestPerMinute = 0.0f;
    SI::Behaviour rests(Only(SI::Quirk::Rest), SI::Role::Healer);
    SI::Situation low = Calm(5000);
    low.Mana = 0.1f;
    intent = rests.Decide(low, resting);
    EXPECT_EQ(intent.Doing, SI::Mode::Rest);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Hold);
    low.NowMs = 5250;
    low.InCombat = true;
    EXPECT_EQ(rests.Decide(low, resting).Doing, SI::Mode::Normal);
}

TEST(StandInTest, PaceSetsTheBreatherAndTheReaction)
{
    SI::Tuning const tuning = Quiet();
    for (SI::Pace pace : { SI::Pace::Fast, SI::Pace::Slow })
    {
        SI::Style style = Only(std::nullopt);
        style.Speed = pace;
        SI::Behaviour plays(style, SI::Role::Damage);
        uint32_t const wait = style.WaitMs(tuning);
        uint32_t const react = style.ReactMs(tuning);

        // A fight starts at 1 s: it holds until it has reacted, then fights.
        SI::Situation seen = Calm(1000);
        seen.InCombat = seen.PartyInCombat = true;
        EXPECT_FALSE(plays.Decide(seen, tuning).Fight);
        seen.NowMs = 1000 + react;
        EXPECT_TRUE(plays.Decide(seen, tuning).Fight);

        // The fight ends at 10 s: a breather, then on after the leader.
        SI::Situation calm = Calm(10000);
        EXPECT_EQ(plays.Decide(calm, tuning).Go, SI::Intent::Goal::Hold);
        calm.NowMs = 10000 + wait - 250;
        EXPECT_EQ(plays.Decide(calm, tuning).Go, SI::Intent::Goal::Hold);
        calm.NowMs = 10000 + wait;
        EXPECT_EQ(plays.Decide(calm, tuning).Go, SI::Intent::Goal::Leader);
    }
    EXPECT_GT(tuning.SlowWaitMs, tuning.FastWaitMs);
    EXPECT_GT(tuning.SlowReactMs, tuning.FastReactMs);
}

TEST(StandInTest, ALeaderGoesForThePullAndAFollowerAfterTheLeader)
{
    SI::Tuning const tuning = Quiet();
    SI::Behaviour leader(Only(std::nullopt, true), SI::Role::Tank);
    SI::Situation seen = Calm(60000);
    SI::Intent intent = leader.Decide(seen, tuning);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Target);
    EXPECT_EQ(intent.StopYards, tuning.MeleeYards);
    EXPECT_FALSE(intent.Fight);
    seen.NowMs += 250;
    seen.TargetYards = 2.5f;
    EXPECT_TRUE(leader.Decide(seen, tuning).Fight);  // the tank pulls

    SI::Behaviour follower(Only(std::nullopt), SI::Role::Healer);
    intent = follower.Decide(Calm(60000), tuning);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Leader);
    EXPECT_EQ(intent.StopYards, tuning.FollowYards);

    // In a fight a healer keeps its distance; a ranged build fights from range.
    SI::Situation fight = Calm(70000);
    fight.InCombat = fight.PartyInCombat = true;
    follower.Decide(fight, tuning);
    fight.NowMs += 2000;
    EXPECT_EQ(follower.Decide(fight, tuning).StopYards, tuning.HealerYards);
    SI::Behaviour archer(Only(std::nullopt), SI::Role::Damage);
    fight.NowMs = 70000;
    fight.Ranged = true;
    archer.Decide(fight, tuning);
    fight.NowMs += 2000;
    EXPECT_EQ(archer.Decide(fight, tuning).StopYards, tuning.RangedYards);

    // The dead do nothing.
    SI::Situation dead = Calm(80000);
    dead.Alive = false;
    intent = follower.Decide(dead, tuning);
    EXPECT_EQ(intent.Go, SI::Intent::Goal::Hold);
    EXPECT_FALSE(intent.Fight);
}

TEST(StandInTest, ItMovesOnlyByHeldKeys)
{
    // A follower 40 yards behind a leader: each decision its intent becomes held keys, and only the player
    // controller's physics move the body under them -- it arrives within its follow distance and stops there.
    SI::Tuning const tuning = Quiet();
    SI::Behaviour plays(Only(std::nullopt), SI::Role::Damage);
    Open world;
    Mv::Speeds speeds;
    Mv::Body shape;
    Mv::BodyState body;
    body.Yaw = 3.0f;                                // facing away
    float const leaderX = 40.0f;
    float const leaderY = 10.0f;
    uint32_t pressedForward = 0;
    for (uint32_t step = 0; step < 120; ++step)
    {
        SI::Situation seen = Calm(60000 + step * 250);
        seen.LeaderYards = std::hypot(leaderX - body.X, leaderY - body.Y);
        SI::Intent const intent = plays.Decide(seen, tuning);
        ASSERT_EQ(intent.Go, SI::Intent::Goal::Leader);
        Mv::ControlState keys = SI::Keys(body, true, leaderX, leaderY, intent.StopYards, false, 0.0f, 0.0f,
            intent.Walk);
        pressedForward += keys.Forward == 1 ? 1 : 0;
        EXPECT_EQ(keys.Strafe, 0);
        EXPECT_FALSE(keys.Jump);
        for (uint32_t tick = 0; tick < 5; ++tick)
            Mv::Step(body, keys, speeds, shape, world, 0.05f);
    }
    float const gap = std::hypot(leaderX - body.X, leaderY - body.Y);
    EXPECT_LE(gap, tuning.FollowYards + 0.5f);
    EXPECT_GT(gap, tuning.FollowYards - 2.0f);      // it stopped near the edge of its distance, not on top
    EXPECT_GT(pressedForward, 0u);

    // Standing at the edge of a fight it only turns to face what it fights.
    Mv::BodyState still;
    Mv::ControlState const face = SI::Keys(still, true, 1.0f, 0.0f, 3.0f, true, 0.0f, 5.0f, false);
    EXPECT_EQ(face.Forward, 0);
    EXPECT_GT(face.TurnRate, 0.0f);                 // the target is to its left
}
