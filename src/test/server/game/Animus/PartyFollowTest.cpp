/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
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

#include "Block.h"
#include "CurriculumTuning.h"
#include "EntranceRespawn.h"
#include "Encounters.h"
#include "FollowEncounter.h"
#include "Layout.h"
#include "PartyFollowEncounter.h"
#include "PartyFramesBlock.h"
#include "RewardLedger.h"
#include "SeatView.h"
#include "StageDefinition.h"
#include "gtest/gtest.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
using Cu::PartyFollowEncounter;
using Cu::PartyFramesBlock;
using Cu::RespawnClock;

namespace
{
    constexpr uint32 DECISION_MS = 250;
    constexpr float PI = float(M_PI);
}

// ---------------------------------------------------------------- I4: death, the rise at the entrance, the rejoin

// A fake death in a seeded instance, decision by decision: the seat goes down 20 s in, is out for the delay -- no rise
// a decision early -- then rises (at the entrance: RiseAtEntrance's place is the clock's caller's), walks back from
// 150 yd at 7 yd/s, and has rejoined on the decision it comes within the rejoin yards. Nothing in it ends the episode.
TEST(EntranceRespawnTest, AFakeDeathRisesAfterTheDelayAndWalksBack)
{
    Cu::CurriculumTuning::RespawnTuning const tuning;
    RespawnClock clock;
    uint32 const diedAt = 20000;
    uint32 rose = 0;
    uint32 rejoined = 0;
    float yards = 5.0f;
    for (uint32 now = 0; now <= 120000; now += DECISION_MS)
    {
        bool const dead = now >= diedAt && !rose;
        if (rose && !rejoined)
            yards = std::max(0.0f, 150.0f - 7.0f * float(now - rose) / 1000.0f);
        RespawnClock::Step const step = clock.Note(now, !dead, yards, tuning.DelayMs, tuning.RejoinYards);
        if (now == diedAt)
            EXPECT_EQ(step, RespawnClock::Step::Died);
        else if (dead && now < diedAt + tuning.DelayMs)
            EXPECT_EQ(step, RespawnClock::Step::None) << "rose early at " << now;
        if (step == RespawnClock::Step::Rise)
        {
            EXPECT_EQ(now, diedAt + tuning.DelayMs);
            clock.Risen(now);
            rose = now;
            yards = 150.0f;       // at the entrance, far from the party
        }
        if (step == RespawnClock::Step::Rejoined)
            rejoined = now;
    }
    ASSERT_GT(rose, 0u);
    ASSERT_GT(rejoined, rose);
    EXPECT_EQ(clock.Deaths, 1u);
    EXPECT_EQ(clock.Rises, 1u);
    EXPECT_EQ(clock.Rejoins, 1u);
    EXPECT_FALSE(clock.Out);
    EXPECT_FALSE(clock.Rejoining);
    // 135 yd closed at 7 yd/s: 19.3 s, on the next decision.
    EXPECT_NEAR(clock.RejoinSeconds(), 135.0f / 7.0f, 0.3f);
    EXPECT_FLOAT_EQ(clock.RejoinedShare(), 1.0f);
    EXPECT_EQ(clock.OutMsTotal, tuning.DelayMs);
}

// The rejoin is measured against the party: no party to be with (negative yards) never rejoins, a far one does not,
// and a second death while walking back is a second death, not a rejoin.
TEST(EntranceRespawnTest, TheRejoinMeasure)
{
    RespawnClock clock;
    EXPECT_EQ(clock.Note(1000, false, 10.0f, 10000, 15.0f), RespawnClock::Step::Died);
    EXPECT_EQ(clock.Note(11000, false, 10.0f, 10000, 15.0f), RespawnClock::Step::Rise);
    clock.Risen(11000);
    EXPECT_EQ(clock.Note(12000, true, -1.0f, 10000, 15.0f), RespawnClock::Step::None);
    EXPECT_EQ(clock.Note(13000, true, 60.0f, 10000, 15.0f), RespawnClock::Step::None);
    EXPECT_TRUE(clock.Rejoining);
    // Down again on the way back.
    EXPECT_EQ(clock.Note(14000, false, 50.0f, 10000, 15.0f), RespawnClock::Step::Died);
    EXPECT_FALSE(clock.Rejoining);
    EXPECT_EQ(clock.Deaths, 2u);
    EXPECT_FLOAT_EQ(clock.RejoinedShare(), 0.0f);
    // Stood up by a friend where it lay: a rise, and it rejoins from there.
    EXPECT_EQ(clock.Note(16000, true, 12.0f, 10000, 15.0f), RespawnClock::Step::Rejoined);
    EXPECT_EQ(clock.Rises, 2u);
    EXPECT_EQ(clock.Rejoins, 1u);
    EXPECT_FLOAT_EQ(clock.RejoinSeconds(), 0.0f);
    EXPECT_FLOAT_EQ(clock.RejoinedShare(), 0.5f);
}

// With no death there is nothing to rejoin, and nothing is counted.
TEST(EntranceRespawnTest, NoDeathNoRejoin)
{
    RespawnClock clock;
    for (uint32 now = 0; now < 10000; now += DECISION_MS)
        EXPECT_EQ(clock.Note(now, true, 3.0f, 10000, 15.0f), RespawnClock::Step::None);
    EXPECT_EQ(clock.Deaths + clock.Rises + clock.Rejoins, 0u);
    EXPECT_FLOAT_EQ(clock.RejoinedShare(), 1.0f);
}

// ---------------------------------------------------------------- I5: spacing, blocking, regrouping, the ladder

// The band: 3-10 yd is kept, closer is crowding, past 40 yd is lost (PartyFollow's defaults).
TEST(PartyFollowTest, FollowSpacing)
{
    Cu::CurriculumTuning::PartyFollowTuning const tuning;
    auto const band = [&](float yards)
    {
        return Cu::FollowEncounter::Band(yards, tuning.BandMin, tuning.BandMax, tuning.LostYards);
    };
    EXPECT_EQ(band(1.0f), 0u);
    EXPECT_EQ(band(2.9f), 0u);
    EXPECT_EQ(band(3.0f), 1u);
    EXPECT_EQ(band(10.0f), 1u);
    EXPECT_EQ(band(10.5f), 2u);
    EXPECT_EQ(band(40.0f), 2u);
    EXPECT_EQ(band(41.0f), 3u);
}

// In a moving leader's way: close and in front of it. Behind it, beside it, far ahead, or before a leader standing
// still, nobody blocks; standing on top of it does, whichever way it faces.
TEST(PartyFollowTest, BlockingDetection)
{
    float const yards = 2.5f;
    float const half = 45.0f;
    // Leader at the origin facing +x.
    EXPECT_TRUE(PartyFollowEncounter::InTheWay(0, 0, 0, true, 2.0f, 0.0f, yards, half));
    EXPECT_TRUE(PartyFollowEncounter::InTheWay(0, 0, 0, true, 1.5f, 1.0f, yards, half));     // 34 degrees off
    EXPECT_TRUE(PartyFollowEncounter::InTheWay(0, 0, 0, true, 0.1f, 0.1f, yards, half));     // on top of it
    EXPECT_FALSE(PartyFollowEncounter::InTheWay(0, 0, 0, false, 2.0f, 0.0f, yards, half));   // standing still
    EXPECT_FALSE(PartyFollowEncounter::InTheWay(0, 0, 0, true, -2.0f, 0.0f, yards, half));   // behind
    EXPECT_FALSE(PartyFollowEncounter::InTheWay(0, 0, 0, true, 0.5f, 2.0f, yards, half));    // beside
    EXPECT_FALSE(PartyFollowEncounter::InTheWay(0, 0, 0, true, 5.0f, 0.0f, yards, half));    // ahead, clear
    // Facing +y (yaw pi/2): in front is +y now.
    EXPECT_TRUE(PartyFollowEncounter::InTheWay(0, 0, PI / 2.0f, true, 0.0f, 2.0f, yards, half));
    EXPECT_FALSE(PartyFollowEncounter::InTheWay(0, 0, PI / 2.0f, true, 2.0f, 0.0f, yards, half));
    // Across the yaw's wrap: facing just under 2 pi, a follower just under the x axis is in front.
    EXPECT_TRUE(PartyFollowEncounter::InTheWay(0, 0, 2.0f * PI - 0.1f, true, 2.0f, -0.1f, yards, half));
}

// A regroup pays all of Regroup at once, less the later it comes, nothing past the window.
TEST(PartyFollowTest, RegroupPaysSoonerMore)
{
    EXPECT_FLOAT_EQ(PartyFollowEncounter::RegroupShare(0.0f, 20.0f), 1.0f);
    EXPECT_FLOAT_EQ(PartyFollowEncounter::RegroupShare(5.0f, 20.0f), 0.75f);
    EXPECT_FLOAT_EQ(PartyFollowEncounter::RegroupShare(20.0f, 20.0f), 0.0f);
    EXPECT_FLOAT_EQ(PartyFollowEncounter::RegroupShare(30.0f, 20.0f), 0.0f);
    EXPECT_FLOAT_EQ(PartyFollowEncounter::RegroupShare(0.0f, 0.0f), 1.0f);
}

// The ladder: long stops on the slow, steady rung, short ones at the top.
TEST(PartyFollowTest, StopsShortenUpTheLadder)
{
    Cu::CurriculumTuning::PartyFollowTuning const tuning;
    EXPECT_FLOAT_EQ(PartyFollowEncounter::StopSeconds(0, tuning.StopSecondsFirst, tuning.StopSecondsLast), 8.0f);
    EXPECT_FLOAT_EQ(PartyFollowEncounter::StopSeconds(3, tuning.StopSecondsFirst, tuning.StopSecondsLast), 3.0f);
    EXPECT_FLOAT_EQ(PartyFollowEncounter::StopSeconds(9, tuning.StopSecondsFirst, tuning.StopSecondsLast), 3.0f);
    float const middle = PartyFollowEncounter::StopSeconds(1, tuning.StopSecondsFirst, tuning.StopSecondsLast);
    EXPECT_GT(middle, 3.0f);
    EXPECT_LT(middle, 8.0f);
    EXPECT_LT(tuning.WalkRungs, tuning.SuddenFromRung);
    EXPECT_LE(tuning.SuddenFromRung, tuning.BackStepFromRung);
    EXPECT_LT(tuning.BackStepFromRung, PartyFollowEncounter::RUNGS);
}

// The follow's purpose is Outcome (kept, regrouped), its prices Costs (lost, blocking, death); nothing is shaped.
TEST(PartyFollowTest, TheTermsAreOutcomeAndCost)
{
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::FollowKept), Cu::RewardCategory::Outcome);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Regroup), Cu::RewardCategory::Outcome);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Lost), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Blocking), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Death), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::Regroup), "regroup");
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::Blocking), "blocking");
}

// ---------------------------------------------------------------- the minimap's dots and the party frames

// The minimap: a dot within its radius, in the seat's facing frame (forward along the facing, right clockwise of it);
// nothing beyond it.
TEST(PartyFramesTest, MinimapDotIsRelativeAndOnlyWithinTheRadius)
{
    // Facing +x: a member 10 yd ahead, then one 10 yd to the right (-y: the core's yaw turns left).
    PartyFramesBlock::Dot ahead = PartyFramesBlock::DotOf(0, 0, 0, 10, 0, 60);
    EXPECT_TRUE(ahead.Shown);
    EXPECT_NEAR(ahead.Forward, 10.0f, 1e-4f);
    EXPECT_NEAR(ahead.Right, 0.0f, 1e-4f);
    PartyFramesBlock::Dot right = PartyFramesBlock::DotOf(0, 0, 0, 0, -10, 60);
    EXPECT_NEAR(right.Forward, 0.0f, 1e-4f);
    EXPECT_NEAR(right.Right, 10.0f, 1e-4f);
    // Facing +y: the member at +x is now on the right.
    PartyFramesBlock::Dot turned = PartyFramesBlock::DotOf(0, 0, PI / 2.0f, 10, 0, 60);
    EXPECT_NEAR(turned.Forward, 0.0f, 1e-4f);
    EXPECT_NEAR(turned.Right, 10.0f, 1e-4f);
    // Behind, at the edge, and past it.
    PartyFramesBlock::Dot behind = PartyFramesBlock::DotOf(100, 100, 0, 70, 100, 60);
    EXPECT_TRUE(behind.Shown);
    EXPECT_NEAR(behind.Forward, -30.0f, 1e-4f);
    EXPECT_TRUE(PartyFramesBlock::DotOf(0, 0, 0, 60, 0, 60).Shown);
    PartyFramesBlock::Dot far = PartyFramesBlock::DotOf(0, 0, 0, 61, 0, 60);
    EXPECT_FALSE(far.Shown);
    EXPECT_FLOAT_EQ(far.Forward, 0.0f);
    EXPECT_FLOAT_EQ(far.Right, 0.0f);
    EXPECT_NEAR(far.Distance, 61.0f, 1e-4f);
}

// The block: the frames are always there (alive, health, power, the leader); the dot's columns only for a member the
// minimap shows, scaled by its radius; empty slots read 0.
TEST(PartyFramesTest, TheBlockReadsTheFramesAlwaysAndTheDotsWithinTheRadius)
{
    PartyFramesBlock const& block = static_cast<PartyFramesBlock const&>(Cu::GetBlock(Cu::BlockId::PartyFrames));
    Cu::Layout layout;
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::PartyFrames), "party_frames");
    EXPECT_EQ(block.Size(layout).Obs, Cu::GROUP_MEMBERS * uint32(PartyFramesBlock::FRAME_FEATURES));
    EXPECT_EQ(block.Size(layout).Actions, 0u);
    boost::json::array names;
    block.DescribeColumns(layout, names);
    ASSERT_EQ(names.size(), Cu::GROUP_MEMBERS * uint32(PartyFramesBlock::FRAME_FEATURES));
    EXPECT_EQ(std::string(names[0].as_string()), "member0_present");
    EXPECT_EQ(std::string(names[PartyFramesBlock::FRAME_DOT_FORWARD].as_string()), "member0_dot_forward");

    Cu::SeatView view;
    view.MinimapYards = 60.0f;
    // The leader, within the minimap's radius, 30 yd ahead and 15 to the left.
    Cu::SeatView::PartyFrame& leader = view.Frames[0];
    leader.Present = true;
    leader.Alive = true;
    leader.Leader = true;
    leader.Health = 1.0f;
    leader.Power = 0.5f;
    leader.DotShown = true;
    leader.DotForward = 30.0f;
    leader.DotRight = -15.0f;
    // A member beyond it, hurt and in combat: the frame, no dot.
    Cu::SeatView::PartyFrame& far = view.Frames[1];
    far.Present = true;
    far.Alive = true;
    far.InCombat = true;
    far.Health = 0.25f;
    far.DotForward = 99.0f;     // not shown: never read
    // A dead one.
    view.Frames[2].Present = true;

    std::vector<float> obs(block.Size(layout).Obs, -1.0f);
    block.Observe(view, obs.data(), nullptr);
    auto const at = [&](uint32 member, uint32 feature)
    {
        return obs[member * PartyFramesBlock::FRAME_FEATURES + feature];
    };
    EXPECT_FLOAT_EQ(at(0, PartyFramesBlock::FRAME_PRESENT), 1.0f);
    EXPECT_FLOAT_EQ(at(0, PartyFramesBlock::FRAME_LEADER), 1.0f);
    EXPECT_FLOAT_EQ(at(0, PartyFramesBlock::FRAME_POWER), 0.5f);
    EXPECT_FLOAT_EQ(at(0, PartyFramesBlock::FRAME_DOT), 1.0f);
    EXPECT_FLOAT_EQ(at(0, PartyFramesBlock::FRAME_DOT_FORWARD), 0.5f);
    EXPECT_FLOAT_EQ(at(0, PartyFramesBlock::FRAME_DOT_RIGHT), -0.25f);
    EXPECT_NEAR(at(0, PartyFramesBlock::FRAME_DOT_DISTANCE), std::sqrt(30.0f * 30.0f + 15.0f * 15.0f) / 60.0f, 1e-5f);
    EXPECT_FLOAT_EQ(at(1, PartyFramesBlock::FRAME_PRESENT), 1.0f);
    EXPECT_FLOAT_EQ(at(1, PartyFramesBlock::FRAME_IN_COMBAT), 1.0f);
    EXPECT_FLOAT_EQ(at(1, PartyFramesBlock::FRAME_HEALTH), 0.25f);
    EXPECT_FLOAT_EQ(at(1, PartyFramesBlock::FRAME_LEADER), 0.0f);
    EXPECT_FLOAT_EQ(at(1, PartyFramesBlock::FRAME_DOT), 0.0f);
    EXPECT_FLOAT_EQ(at(1, PartyFramesBlock::FRAME_DOT_FORWARD), 0.0f);
    EXPECT_FLOAT_EQ(at(2, PartyFramesBlock::FRAME_PRESENT), 1.0f);
    EXPECT_FLOAT_EQ(at(2, PartyFramesBlock::FRAME_ALIVE), 0.0f);
    for (uint32 feature = 0; feature < PartyFramesBlock::FRAME_FEATURES; ++feature)
        EXPECT_FLOAT_EQ(at(3, feature), 0.0f);
}

// ---------------------------------------------------------------- the stage, and the stages before it

// M4: a party of four learned followers (the leader is the owner's slot's), in Ragefire Chasm and the Deadmines, with
// the camera, the map and the party frames, and no compass.
TEST(PartyFollowTest, TheStageIsDefined)
{
    Cu::StageDefinition const* stage = Cu::FindStage("move4_follow");
    ASSERT_NE(stage, nullptr) << "move4_follow was left out";
    EXPECT_EQ(stage->Extends, "move2_seek");
    std::vector<Cu::BlockId> const blocks = { Cu::BlockId::Core, Cu::BlockId::Move, Cu::BlockId::Vision,
        Cu::BlockId::Entities, Cu::BlockId::Map, Cu::BlockId::PartyFrames, Cu::BlockId::Goal };
    EXPECT_EQ(stage->Blocks, blocks);
    EXPECT_FALSE(stage->Has(Cu::BlockId::Compass));
    EXPECT_EQ(stage->SeatCount(), Cu::GROUP_MEMBERS);
    ASSERT_EQ(stage->Arenas.size(), 2u);
    EXPECT_EQ(stage->Arenas[0].MapId, 389u);
    EXPECT_EQ(stage->Arenas[1].MapId, 36u);
    for (Cu::ArenaDefinition const& arena : stage->Arenas)
    {
        EXPECT_EQ(arena.Against, Cu::Opposition::PartyFollow);
        EXPECT_EQ(arena.Seats, Cu::SeatPlan::Party);
        EXPECT_EQ(arena.PartySize, Cu::GROUP_MEMBERS);
        EXPECT_FALSE(arena.Owner);
        EXPECT_FALSE(arena.DeathRuns);
    }
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;
}

// **M1 and M2 are untouched** (the user's binding rule, 2026-10-06: training resumes exactly where it left off after
// the rebuild): their block lists, each block's columns and actions, and their seats are those of forge 7a90f9b2c.
// The party frames, the minimap's dots and the rise at the entrance are M4's and later stages', never inserted into a
// stage that does not declare them. The numbers are the running M1's and M2's stage.json (runs/move1_controls and
// runs/move2_seek, written by the 7a90f9b2c build on 2026-10-06). The core block's size is the class's, not the
// stage's, and needs the class data: unchanged by this work.
TEST(PartyFollowTest, M1AndM2LayoutsAreUnchanged)
{
    struct Expected
    {
        Cu::BlockId Id;
        uint32 Obs;
        uint32 Actions;
        uint32 Revision;
        std::size_t Columns;
    };
    auto const check = [](char const* name, std::vector<Expected> const& expected)
    {
        Cu::StageDefinition const* stage = Cu::FindStage(name);
        ASSERT_NE(stage, nullptr) << name;
        ASSERT_EQ(stage->Blocks.size(), expected.size()) << name;
        Cu::Layout layout;
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            EXPECT_EQ(stage->Blocks[i], expected[i].Id) << name << " block " << i;
            if (expected[i].Id == Cu::BlockId::Core)
                continue;
            Cu::Block const& block = Cu::GetBlock(expected[i].Id);
            boost::json::array columns;
            block.DescribeColumns(layout, columns);
            EXPECT_EQ(block.Size(layout).Obs, expected[i].Obs) << name << " " << Cu::BlockName(expected[i].Id);
            EXPECT_EQ(block.Size(layout).Actions, expected[i].Actions) << name << " "
                << Cu::BlockName(expected[i].Id);
            EXPECT_EQ(block.Revision(), expected[i].Revision) << name << " " << Cu::BlockName(expected[i].Id);
            EXPECT_EQ(columns.size(), expected[i].Columns) << name << " " << Cu::BlockName(expected[i].Id);
        }
        EXPECT_EQ(stage->SeatCount(), 1u) << name;
        for (Cu::ArenaDefinition const& arena : stage->Arenas)
        {
            EXPECT_EQ(arena.Seats, Cu::SeatPlan::Solo) << name;
            EXPECT_EQ(arena.PartySize, 0u) << name;
        }
        EXPECT_FALSE(stage->Has(Cu::BlockId::PartyFrames)) << name;
    };

    using enum Cu::BlockId;
    check("move1_controls", { { Core, 0, 0, 0, 0 }, { Move, 57, 25, 5, 57 },
        { Compass, 6, 0, 1, 6 }, { Vision, 11, 0, 5, 0 },
        { Entities, 640, 0, 1, 0 },
        { Goal, 128, 0, 0, 0 } });
    check("move2_seek", { { Core, 0, 0, 0, 0 }, { Move, 57, 25, 5, 57 },
        { Vision, 11, 0, 5, 0 },
        { Entities, 640, 0, 1, 0 },
        { Map, 4, 0, 1, 0 }, { Goal, 128, 0, 0, 0 } });
}
