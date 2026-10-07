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

/// Dungeon-curriculum G1, group1_roles (RolesEncounter, RolesDraw): the stage's definition, its drills' packs and
/// camps by rung, each drill's outcome (the drilled seat's lesson as Outcome, its misses as Cost, tier-scaled), what
/// wins an episode, and a death in a party coming back at the entrance and rejoining the others.

#include "Block.h"
#include "CombatDraw.h"
#include "CurriculumTuning.h"
#include "EntranceRespawn.h"
#include "RewardLedger.h"
#include "RolesDraw.h"
#include "StageDefinition.h"
#include "StageState.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
namespace Draw = Animus::Curriculum::RolesDraw;
using Cu::RolesDrill;

namespace
{
    Cu::CurriculumTuning::RolesTuning const Tuning;
}

// ---------------------------------------------------------------- the stage

// G1: a party of five drilling one role an episode on the combat stages' cleared Ragefire Chasm at its band, seeded
// from C3 with M4's party frames merged in, perceiving through the party frames (revision 2) and the combat block
// (revision 1) beside the sight list; every arena a proper party in a core group whose drilled role is the drill's, its dead
// back at the entrance; the curriculum accepts it.
TEST(RolesStageTest, TheStageIsDefined)
{
    using Id = Cu::BlockId;
    Cu::StageDefinition const* stage = Cu::FindStage("group1_roles");
    ASSERT_NE(stage, nullptr) << "group1_roles was left out";
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;
    EXPECT_EQ(stage->Extends, "combat3_survive");
    EXPECT_EQ(stage->Merges, std::vector<std::string>{ "move4_follow" });
    EXPECT_EQ(stage->Blocks, (std::vector<Id>{ Id::Core, Id::Move, Id::Duel, Id::Pet, Id::Gauntlet, Id::Vision,
        Id::Entities, Id::Map, Id::Sight, Id::PartyFrames, Id::Combat, Id::Goal }));
    // One source of party-member state: never the server lists of the party or support blocks.
    EXPECT_FALSE(stage->Has(Id::Party));
    EXPECT_FALSE(stage->Has(Id::Support));
    EXPECT_FALSE(stage->Has(Id::Pack));
    EXPECT_EQ(Cu::GetBlock(Id::PartyFrames).Revision(), 2u);
    EXPECT_EQ(Cu::GetBlock(Id::Combat).Revision(), 1u);
    EXPECT_EQ(stage->MapId, 389u);
    EXPECT_EQ(stage->FocusLevelFirst, 13);
    EXPECT_EQ(stage->FocusLevelLast, 18);
    EXPECT_EQ(stage->FocusChance, 100);
    EXPECT_EQ(stage->SeatCount(), Cu::GROUP_SEATS);

    struct Want
    {
        char const* Name;
        RolesDrill Drill;
        uint8 Role;
    };
    std::vector<Want> const wants = { { "tank_hold", RolesDrill::Hold, Cu::DUNGEON_TANK },
        { "heal_keep", RolesDrill::Keep, Cu::DUNGEON_HEALER },
        { "damage_discipline", RolesDrill::Focus, Cu::DUNGEON_DAMAGE },
        { "pull", RolesDrill::Pull, Cu::DUNGEON_TANK } };
    ASSERT_EQ(stage->Arenas.size(), wants.size());
    for (std::size_t i = 0; i < wants.size(); ++i)
    {
        Cu::ArenaDefinition const& arena = stage->Arenas[i];
        EXPECT_EQ(arena.Name, wants[i].Name);
        EXPECT_EQ(arena.Against, Cu::Opposition::Roles) << arena.Name;
        EXPECT_EQ(arena.Roles, wants[i].Drill) << arena.Name;
        EXPECT_EQ(arena.DrillRole, wants[i].Role) << arena.Name;
        EXPECT_EQ(Draw::DrilledRole(arena.Roles), arena.DrillRole) << arena.Name;
        EXPECT_EQ(arena.Seats, Cu::SeatPlan::Party) << arena.Name;
        EXPECT_EQ(arena.SeatCount(), Cu::GROUP_SEATS) << arena.Name;
        EXPECT_TRUE(arena.ProperParty) << arena.Name;
        EXPECT_TRUE(arena.PartyGroup) << arena.Name;
        EXPECT_FALSE(arena.Owner) << arena.Name;
        EXPECT_TRUE(arena.RespawnAtEntrance) << arena.Name << ": no stage ends at the first death";
        EXPECT_FALSE(arena.DeathRuns) << arena.Name << ": no graveyard, ghost or corpse run";
        EXPECT_EQ(arena.Schedule, Cu::PullSchedule::None) << arena.Name;
        EXPECT_EQ(arena.Combat, Cu::CombatDrill::None) << arena.Name;
        EXPECT_GE(arena.EpisodeSeconds, 240u) << arena.Name;
        EXPECT_FALSE(arena.EvalOnly) << arena.Name;
    }

    // Its parents come before it, and the combat stages stay one seat with no party frames.
    std::vector<std::string> order;
    for (Cu::StageDefinition const& each : Cu::CurriculumStages())
        order.push_back(each.Name);
    auto const at = [&order](char const* name) { return std::find(order.begin(), order.end(), name) - order.begin(); };
    EXPECT_LT(at("combat3_survive"), at("group1_roles"));
    EXPECT_LT(at("move4_follow"), at("group1_roles"));
    for (char const* name : { "combat1_fight", "combat2_packs", "combat3_survive" })
    {
        Cu::StageDefinition const* combat = Cu::FindStage(name);
        ASSERT_NE(combat, nullptr) << name;
        EXPECT_FALSE(combat->Has(Id::PartyFrames)) << name;
        EXPECT_EQ(combat->SeatCount(), 1u) << name;
    }
}

// ---------------------------------------------------------------- the draws

// The packs by rung: two creatures at rung 0 growing by one every two rungs to four, half a level a rung, a caster
// from rung 1, linked from 2, an elite from 4; heal_keep's at twice their health; the other drills' at their own.
TEST(RolesStageTest, ThePacksByRung)
{
    Draw::Pack const first = Draw::PlanPack(RolesDrill::Hold, 0, Tuning);
    EXPECT_EQ(first.Size, 2u);
    EXPECT_EQ(first.LevelOffset, Tuning.LevelBase);
    EXPECT_FALSE(first.Caster);
    EXPECT_FALSE(first.Linked);
    EXPECT_FALSE(first.Elite);
    EXPECT_EQ(first.HealthPct, 100u);

    EXPECT_EQ(Draw::PlanPack(RolesDrill::Hold, 2, Tuning).Size, 3u);
    EXPECT_TRUE(Draw::PlanPack(RolesDrill::Hold, 1, Tuning).Caster);
    EXPECT_TRUE(Draw::PlanPack(RolesDrill::Hold, 2, Tuning).Linked);
    Draw::Pack const top = Draw::PlanPack(RolesDrill::Focus, Tuning.MaxTier, Tuning);
    EXPECT_EQ(top.Size, Tuning.PackSizeMax);
    EXPECT_TRUE(top.Elite);
    EXPECT_EQ(top.LevelOffset, Tuning.LevelBase + int32(Tuning.MaxTier * Tuning.LevelsPerTier / 2));
    EXPECT_EQ(Draw::PlanPack(RolesDrill::Keep, 0, Tuning).HealthPct, 200u);
    EXPECT_EQ(Draw::PlanPack(RolesDrill::Pull, 3, Tuning).HealthPct, 100u);
}

// The pull drill's camp: two packs at rung 0, one more every two rungs, four at most, standing 45 yd apart and closing
// to 25 at the top; every other drill keeps the pack in front and the next one on, at the combat stages' spacing.
TEST(RolesStageTest, ThePullDrillsCamp)
{
    EXPECT_EQ(Draw::StandingPacks(RolesDrill::Pull, 0, Tuning), 2u);
    EXPECT_EQ(Draw::StandingPacks(RolesDrill::Pull, 2, Tuning), 3u);
    EXPECT_EQ(Draw::StandingPacks(RolesDrill::Pull, Tuning.MaxTier, Tuning), 4u);
    for (RolesDrill drill : { RolesDrill::Hold, RolesDrill::Keep, RolesDrill::Focus })
    {
        EXPECT_EQ(Draw::StandingPacks(drill, Tuning.MaxTier, Tuning), 2u);
        EXPECT_FLOAT_EQ(Draw::PackSpacing(drill, 3, Tuning.MaxTier, Tuning, 30.0f), 30.0f);
    }
    EXPECT_FLOAT_EQ(Draw::PackSpacing(RolesDrill::Pull, 0, Tuning.MaxTier, Tuning, 30.0f), 45.0f);
    EXPECT_FLOAT_EQ(Draw::PackSpacing(RolesDrill::Pull, Tuning.MaxTier, Tuning.MaxTier, Tuning, 30.0f), 25.0f);
    float const middle = Draw::PackSpacing(RolesDrill::Pull, 2, Tuning.MaxTier, Tuning, 30.0f);
    EXPECT_LT(middle, 45.0f);
    EXPECT_GT(middle, 25.0f);
}

// ---------------------------------------------------------------- each drill's outcome

// Every drill's lesson is an Outcome term and its misses a Cost: what the stage is for is never Shaping, which the fade
// would take away (the archived stage6's lesson, 2026-10-05).
TEST(RolesStageTest, TheDrillTermsAreOutcomesAndCosts)
{
    for (Cu::RewardTerm term : { Cu::RewardTerm::DrillHold, Cu::RewardTerm::DrillKeep, Cu::RewardTerm::DrillFocus,
        Cu::RewardTerm::PullClean, Cu::RewardTerm::Clear, Cu::RewardTerm::Survived })
        EXPECT_EQ(Cu::RewardTermCategory(term), Cu::RewardCategory::Outcome) << Cu::RewardTermName(term);
    for (Cu::RewardTerm term : { Cu::RewardTerm::PullExtra, Cu::RewardTerm::Death, Cu::RewardTerm::Away,
        Cu::RewardTerm::StepCost })
        EXPECT_EQ(Cu::RewardTermCategory(term), Cu::RewardCategory::Cost) << Cu::RewardTermName(term);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::DamageDealt), Cu::RewardCategory::Shaping);
}

// tank_hold: paid for every enemy on the tank a decision, charged for every one on someone else; the pay times the
// rung's w, the charge over it (the archived ladders' tier scale).
TEST(RolesStageTest, TheHoldDrillsOutcome)
{
    float const scale = 1.0f;
    Draw::Pay const held = Draw::HoldPay(3, 0, Tuning, scale, 1.0f);
    EXPECT_FLOAT_EQ(held.Earned, 3.0f * Tuning.Hold);
    EXPECT_FLOAT_EQ(held.Lost, 0.0f);
    Draw::Pay const loose = Draw::HoldPay(1, 2, Tuning, scale, 1.0f);
    EXPECT_FLOAT_EQ(loose.Earned, Tuning.Hold);
    EXPECT_FLOAT_EQ(loose.Lost, -2.0f * Tuning.Loose);
    float const w = Draw::TierWeight(0.25f, 4);
    EXPECT_FLOAT_EQ(w, 2.0f);
    Draw::Pay const hard = Draw::HoldPay(1, 2, Tuning, scale, w);
    EXPECT_FLOAT_EQ(hard.Earned, 2.0f * Tuning.Hold);
    EXPECT_FLOAT_EQ(hard.Lost, -Tuning.Loose);
    // Half the decision, half the pay.
    EXPECT_FLOAT_EQ(Draw::HoldPay(2, 0, Tuning, 0.5f, 1.0f).Earned, Tuning.Hold);

    // The window: 80% of the enemy-decisions on the tank wins, 70% does not; nothing cleared or a wipe never wins.
    Draw::Tally tally;
    tally.Drill = RolesDrill::Hold;
    tally.Clears = 2;
    tally.Held = 80;
    tally.OnParty = 100;
    EXPECT_TRUE(Draw::Won(tally, Tuning));
    tally.Held = 70;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
    tally.Held = 100;
    tally.Wipes = 1;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
    tally.Wipes = 0;
    tally.Clears = 0;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
}

// heal_keep: paid for every member above half health a decision, charged for every one under 35% or dead and for the
// healing that landed on nothing; won only with nobody dead.
TEST(RolesStageTest, TheKeepDrillsOutcome)
{
    Draw::Kept kept;
    Draw::Count(kept, true, 1.0f);
    Draw::Count(kept, true, 0.8f);
    Draw::Count(kept, true, 0.45f);     // neither up nor low
    Draw::Count(kept, true, 0.2f);      // low
    Draw::Count(kept, false, 0.0f);     // dead: low
    EXPECT_EQ(kept.Members, 5u);
    EXPECT_EQ(kept.Up, 2u);
    EXPECT_EQ(kept.Low, 2u);
    Draw::Pay const pay = Draw::KeepPay(kept, 0.0f, 0.5f, Tuning, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(pay.Earned, 2.0f * Tuning.Keep);
    EXPECT_FLOAT_EQ(pay.Lost, -2.0f * Tuning.KeepLow);
    // A whole health's worth of healing wasted, at Overheal x Party.TeammateHealing.
    Draw::Pay const wasted = Draw::KeepPay(Draw::Kept(), 1.0f, 0.5f, Tuning, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(wasted.Earned, 0.0f);
    EXPECT_FLOAT_EQ(wasted.Lost, -0.5f * Tuning.Overheal);
    EXPECT_FLOAT_EQ(Draw::KeepPay(kept, 0.0f, 0.5f, Tuning, 1.0f, 2.0f).Earned, 4.0f * Tuning.Keep);

    Draw::Tally tally;
    tally.Drill = RolesDrill::Keep;
    tally.Clears = 1;
    EXPECT_TRUE(Draw::Won(tally, Tuning));
    tally.PartyDeaths = 1;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
}

// damage_discipline: paid for its damage on the tank's target (none for damage elsewhere), charged for every enemy it
// takes off the tank; won with WinFocus of its damage on the tank's target and at most WinPulledSeconds pulled.
TEST(RolesStageTest, TheFocusDrillsOutcome)
{
    Draw::Pay const onTarget = Draw::FocusPay(0.4f, 0, Tuning, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(onTarget.Earned, 0.4f * Tuning.Focus);
    EXPECT_FLOAT_EQ(onTarget.Lost, 0.0f);
    Draw::Pay const pulled = Draw::FocusPay(0.0f, 2, Tuning, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(pulled.Earned, 0.0f);
    EXPECT_FLOAT_EQ(pulled.Lost, -2.0f * Tuning.PulledOff);

    Draw::Tally tally;
    tally.Drill = RolesDrill::Focus;
    tally.Clears = 1;
    tally.Damage = 10.0f;
    tally.FocusDamage = 7.0f;
    tally.PulledOffSeconds = 2.0f;
    EXPECT_NEAR(Draw::FocusShare(tally), 0.7f, 1e-6f);
    EXPECT_TRUE(Draw::Won(tally, Tuning));
    tally.PulledOffSeconds = 8.0f;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
    tally.PulledOffSeconds = 0.0f;
    tally.FocusDamage = 4.0f;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
}

// pull: two packs in a fight at once -- the second one drawn in is an extra pull, counted once, and neither of them
// clean any more; a pack fought alone stays clean. Won with no extra pull.
TEST(RolesStageTest, ThePullDrillsOutcome)
{
    struct Pack
    {
        Draw::PackFight Fight;
    };
    std::vector<Pack> packs(3);
    packs[0].Fight.Fighting = true;
    EXPECT_EQ(Draw::NoteFights(packs), 0u);
    EXPECT_TRUE(packs[0].Fight.Clean);

    packs[2].Fight.Fighting = true;     // the far pack comes too
    EXPECT_EQ(Draw::NoteFights(packs), 1u);
    EXPECT_FALSE(packs[0].Fight.Clean);
    EXPECT_FALSE(packs[2].Fight.Clean);
    EXPECT_TRUE(packs[2].Fight.ExtraCounted);
    EXPECT_FALSE(packs[0].Fight.ExtraCounted);
    EXPECT_TRUE(packs[1].Fight.Clean) << "the pack that stayed out of it";
    EXPECT_EQ(Draw::NoteFights(packs), 0u) << "counted once";
    packs[1].Fight.Fighting = true;
    EXPECT_EQ(Draw::NoteFights(packs), 1u);

    Draw::Tally tally;
    tally.Drill = RolesDrill::Pull;
    tally.Clears = 3;
    EXPECT_TRUE(Draw::Won(tally, Tuning));
    tally.ExtraPulls = 1;
    EXPECT_FALSE(Draw::Won(tally, Tuning));
    EXPECT_FALSE(Draw::Won(Draw::Tally(), Tuning)) << "no drill, no win";
}

// ---------------------------------------------------------------- respawn and rejoin in a party

// A death in a party of five (I4): seat 2 goes down while the others fight on 120 yd from the entrance; it is out the
// delay, rises at the entrance, and walks back at 7 yd/s, rejoining when it comes within the rejoin yards of the living
// others' middle. While it is dead or walking back beyond AwayYards of the fight it is charged Away; once back, it is
// not. Nothing ends the episode, and the others never left.
TEST(RolesStageTest, ADeathInAPartyRisesAtTheEntranceAndRejoins)
{
    Cu::CurriculumTuning::RespawnTuning const respawn;
    constexpr uint32 DECISION_MS = 250;
    constexpr float FIGHT_X = 120.0f;
    std::array<Cu::RespawnClock, Cu::GROUP_SEATS> clocks{};
    std::array<float, Cu::GROUP_SEATS> x = { FIGHT_X, FIGHT_X - 2.0f, FIGHT_X, FIGHT_X + 3.0f, FIGHT_X - 4.0f };
    std::array<bool, Cu::GROUP_SEATS> alive{};
    alive.fill(true);
    uint32 const diedAt = 30000;
    uint32 rose = 0;
    uint32 rejoined = 0;
    float awaySeconds = 0.0f;
    for (uint32 now = 0; now <= 180000; now += DECISION_MS)
    {
        if (now == diedAt)
            alive[2] = false;
        if (rose && !rejoined)
            x[2] = std::min(FIGHT_X, x[2] + 7.0f * float(DECISION_MS) / 1000.0f);
        for (uint32 seat = 0; seat < Cu::GROUP_SEATS; ++seat)
        {
            // The living others' middle (RolesEncounter::PartyMiddle), on a line.
            float sum = 0.0f;
            uint32 count = 0;
            for (uint32 other = 0; other < Cu::GROUP_SEATS; ++other)
                if (other != seat && alive[other])
                {
                    sum += x[other];
                    ++count;
                }
            float const partyYards = alive[seat] && count ? std::fabs(x[seat] - sum / float(count)) : -1.0f;
            Cu::RespawnClock::Step const step = clocks[seat].Note(now, alive[seat], partyYards, respawn.DelayMs,
                respawn.RejoinYards);
            if (seat != 2)
            {
                EXPECT_EQ(step, Cu::RespawnClock::Step::None) << seat;
                continue;
            }
            if (step == Cu::RespawnClock::Step::Rise)
            {
                EXPECT_EQ(now, diedAt + respawn.DelayMs);
                clocks[seat].Risen(now);
                alive[seat] = true;
                x[seat] = 0.0f;         // the entrance
                rose = now;
            }
            if (step == Cu::RespawnClock::Step::Rejoined)
                rejoined = now;
            float const fightYards = std::fabs(x[seat] - FIGHT_X);
            if (Cu::CombatDraw::AwayCharged(alive[seat], clocks[seat].Rejoining, true, fightYards, Tuning.AwayYards))
                awaySeconds += float(DECISION_MS) / 1000.0f;
        }
    }
    ASSERT_GT(rose, 0u);
    ASSERT_GT(rejoined, rose);
    EXPECT_EQ(clocks[2].Deaths, 1u);
    EXPECT_EQ(clocks[2].Rises, 1u);
    EXPECT_EQ(clocks[2].Rejoins, 1u);
    EXPECT_FLOAT_EQ(clocks[2].RejoinedShare(), 1.0f);
    // From the entrance to within the rejoin yards of the others' middle (about 120 yd) at 7 yd/s.
    EXPECT_NEAR(clocks[2].RejoinSeconds(), (FIGHT_X - 0.25f - respawn.RejoinYards) / 7.0f, 1.0f);
    // Away: the delay dead, then the walk back until within AwayYards of the fight.
    float const walkAway = (FIGHT_X - Tuning.AwayYards) / 7.0f;
    EXPECT_NEAR(awaySeconds, float(respawn.DelayMs) / 1000.0f + walkAway, 1.0f);
    for (uint32 seat : { 0u, 1u, 3u, 4u })
        EXPECT_EQ(clocks[seat].Deaths, 0u);
}

// A wipe: the whole party down at once. Each is out the delay and rises at the entrance with the others, where it is
// with the party again at once -- the packs went home; the walk back to them is the party's together.
TEST(RolesStageTest, AWipeRisesTogether)
{
    Cu::CurriculumTuning::RespawnTuning const respawn;
    std::array<Cu::RespawnClock, Cu::GROUP_SEATS> clocks{};
    for (Cu::RespawnClock& clock : clocks)
        EXPECT_EQ(clock.Note(5000, false, -1.0f, respawn.DelayMs, respawn.RejoinYards),
            Cu::RespawnClock::Step::Died);
    for (Cu::RespawnClock& clock : clocks)
    {
        EXPECT_EQ(clock.Note(5000 + respawn.DelayMs, false, -1.0f, respawn.DelayMs, respawn.RejoinYards),
            Cu::RespawnClock::Step::Rise);
        clock.Risen(5000 + respawn.DelayMs);
    }
    // Risen at the entrance within a few yards of each other.
    for (Cu::RespawnClock& clock : clocks)
        EXPECT_EQ(clock.Note(5250 + respawn.DelayMs, true, 2.0f, respawn.DelayMs, respawn.RejoinYards),
            Cu::RespawnClock::Step::Rejoined);
    for (Cu::RespawnClock const& clock : clocks)
    {
        EXPECT_EQ(clock.Deaths, 1u);
        EXPECT_EQ(clock.Rises, 1u);
        EXPECT_EQ(clock.Rejoins, 1u);
    }
}
