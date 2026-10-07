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

#include "AreaDefines.h"
#include "EntityActions.h"
#include "EntranceRespawn.h"
#include "InstanceBosses.h"
#include "MoveControls.h"
#include "Opcodes.h"
#include "RewardLedger.h"
#include "SightBlock.h"
#include "Spell.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "WingRun.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
namespace Ea = Animus::Curriculum::EntityActions;
namespace Wr = Animus::Curriculum::WingRun;
namespace MC = Animus::Curriculum::MoveControls;

/*
 * The dungeon curriculum's party stages (G2 group2_corridor, D1 dungeon1_pulls, D2 dungeon2_ragefire, D3
 * dungeon3_deadmines): their layouts and arenas, Wailing Caverns held out of every training draw, the corridor's and
 * the chain pull's bookkeeping, the tier a run's outcome scales with, the stand-in's share, and D3's doors, levers and
 * cannon through the client's handlers.
 */

namespace
{
    constexpr char const* PARTY_STAGES[] = { "group2_corridor", "dungeon1_pulls", "dungeon2_ragefire",
        "dungeon3_deadmines" };

    /// The WING ladder's rows (InstanceBosses.cpp): Ragefire Chasm, the Deadmines, Wailing Caverns (held out).
    constexpr int8 ROW_RAGEFIRE = 0;
    constexpr int8 ROW_DEADMINES = 1;
    constexpr int8 ROW_WAILING_CAVERNS = 2;

    Cu::StageDefinition const& Stage(char const* name)
    {
        Cu::StageDefinition const* stage = Cu::FindStage(name);
        if (!stage)
        {
            ADD_FAILURE() << name << " was left out by the validation";
            static Cu::StageDefinition const none;
            return none;
        }
        return *stage;
    }

    std::vector<uint32> Weights(Cu::StageDefinition const& stage)
    {
        std::vector<uint32> weights;
        for (Cu::ArenaDefinition const& arena : stage.Arenas)
            weights.push_back(arena.Weight);
        return weights;
    }

    std::vector<uint32> Finals(Cu::StageDefinition const& stage)
    {
        std::vector<uint32> finals;
        for (Cu::ArenaDefinition const& arena : stage.Arenas)
            finals.push_back(arena.WeightFinal >= 0 ? uint32(arena.WeightFinal) : arena.Weight);
        return finals;
    }
}

// **The stages as defined**: the seed chain from G1, every one a party of five on a real dungeon's own ground
// (InstanceEncounter, InstanceLadder::Wing, the party's group), G1's blocks (the party frames revision 2, the sight
// list
// with the combat block's columns) with the pack block -- and no crowd block (its pack ahead and nearest
// object
// are radius reads through walls), no party or support block (the party frames are the members' one source).
TEST(DungeonStagesTest, TheStagesLayoutsAndEncounters)
{
    EXPECT_TRUE(Cu::CurriculumProblems().empty());
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;

    // The seed chain: C3 -> G1 -> G2 -> D1 -> D2 -> D3.
    EXPECT_EQ(Stage("group2_corridor").Extends, "group1_roles");
    EXPECT_EQ(Stage("dungeon1_pulls").Extends, "group2_corridor");
    EXPECT_EQ(Stage("dungeon2_ragefire").Extends, "dungeon1_pulls");
    EXPECT_EQ(Stage("dungeon3_deadmines").Extends, "dungeon2_ragefire");

    using Id = Cu::BlockId;
    std::vector<Id> const blocks = Stage("group2_corridor").Blocks;
    for (char const* name : PARTY_STAGES)
    {
        Cu::StageDefinition const& stage = Stage(name);
        EXPECT_TRUE(stage.InDefaultQueue) << name;
        EXPECT_EQ(stage.Blocks, blocks) << name << ": the party stages share one layout, so each seeds the next whole";
        for (Id block : { Id::Core, Id::Move, Id::Duel, Id::Pet, Id::Pack, Id::Gauntlet, Id::Vision, Id::Entities,
            Id::Map, Id::Sight, Id::PartyFrames, Id::Combat, Id::Goal })
            EXPECT_TRUE(stage.Has(block)) << name << " " << Cu::BlockName(block);
        // The party frames (revision 2) are the members' one source: no party or support block beside them.
        for (Id block : { Id::Crowd, Id::Party, Id::Support, Id::Compass, Id::Travel, Id::Order, Id::Companion })
            EXPECT_FALSE(stage.Has(block)) << name << " " << Cu::BlockName(block);
        EXPECT_EQ(stage.SeatCount(), Cu::GROUP_SEATS) << name;
        for (Cu::ArenaDefinition const& arena : stage.Arenas)
        {
            EXPECT_EQ(arena.Against, Cu::Opposition::Instance) << name << " " << arena.Name;
            EXPECT_EQ(arena.Instance, Cu::InstanceLadder::Wing) << name << " " << arena.Name;
            EXPECT_EQ(arena.Seats, Cu::SeatPlan::Party) << name << " " << arena.Name;
            EXPECT_TRUE(arena.PartyGroup) << name << " " << arena.Name;
            EXPECT_FALSE(arena.Owner || arena.Directed || arena.DeathRuns) << name << " "
                << arena.Name << ": no owner, no director, no corpse run";
            EXPECT_GE(arena.InstanceRow, 0) << name << " " << arena.Name;
        }
    }

    // G2: a corridor of four packs in route order, in Ragefire and the Deadmines.
    Cu::StageDefinition const& g2 = Stage("group2_corridor");
    ASSERT_EQ(g2.Arenas.size(), 2u);
    EXPECT_EQ(g2.Arenas[0].InstanceRow, ROW_RAGEFIRE);
    EXPECT_EQ(g2.Arenas[1].InstanceRow, ROW_DEADMINES);
    for (Cu::ArenaDefinition const& arena : g2.Arenas)
    {
        EXPECT_EQ(arena.CorridorPacks, 4u) << arena.Name;
        EXPECT_FALSE(arena.PullDrill || arena.EvalOnly) << arena.Name;
    }
    EXPECT_EQ(g2.Arenas[1].LevelFirst, 17);
    EXPECT_EQ(g2.Arenas[1].LevelLast, 20);

    // D1: Ragefire's pull drill alone, evaluated as a drill.
    Cu::StageDefinition const& d1 = Stage("dungeon1_pulls");
    ASSERT_EQ(d1.Arenas.size(), 1u);
    EXPECT_TRUE(d1.Arenas[0].PullDrill);
    EXPECT_EQ(d1.Arenas[0].InstanceRow, ROW_RAGEFIRE);
    EXPECT_TRUE(Cu::EvaluatesDrills(d1.Arenas));

    // D2 and D3: the whole dungeon, and Wailing Caverns held out.
    for (auto const& [name, row] : { std::pair<char const*, int8>{ "dungeon2_ragefire", ROW_RAGEFIRE },
        std::pair<char const*, int8>{ "dungeon3_deadmines", ROW_DEADMINES } })
    {
        Cu::StageDefinition const& stage = Stage(name);
        ASSERT_EQ(stage.Arenas.size(), 2u) << name;
        Cu::ArenaDefinition const& dungeon = stage.Arenas[0];
        EXPECT_EQ(dungeon.Name, "dungeon");
        EXPECT_EQ(dungeon.InstanceRow, row) << name;
        EXPECT_FALSE(dungeon.PullDrill || dungeon.EvalOnly || dungeon.CorridorPacks) << name;
        Cu::ArenaDefinition const& heldout = stage.Arenas[1];
        EXPECT_EQ(heldout.Name, "heldout");
        EXPECT_EQ(heldout.InstanceRow, ROW_WAILING_CAVERNS) << name;
        EXPECT_TRUE(heldout.EvalOnly) << name;
        EXPECT_FALSE(Cu::EvaluatesDrills(stage.Arenas)) << name;
    }
    // The Deadmines' bar is at 17-20.
    EXPECT_EQ(Stage("dungeon3_deadmines").Arenas[0].LevelFirst, 17);
    EXPECT_EQ(Stage("dungeon3_deadmines").Arenas[0].LevelLast, 20);
    EXPECT_GE(Stage("dungeon3_deadmines").Arenas[0].EpisodeSeconds, 7200u);

    // The rows are the dungeons they say.
    std::vector<Cu::BossRow> const& rows = Cu::InstanceLadderRows(Cu::InstanceLadder::Wing);
    ASSERT_GE(rows.size(), 3u);
    EXPECT_EQ(rows[ROW_RAGEFIRE].MapId, uint32(MAP_RAGEFIRE_CHASM));
    EXPECT_EQ(rows[ROW_DEADMINES].MapId, uint32(MAP_DEADMINES));
    EXPECT_EQ(rows[ROW_WAILING_CAVERNS].MapId, 43u);
}

// **Wailing Caverns is never trained**: no arena of any stage that a training draw can reach is on Wailing Caverns
// (the WING ladder's row 2, map 43); every one that names it is held out, and the arena draw gives it no weight in
// training at any point of the budget -- nor in an ordinary evaluation, which plays it only when pinned to it.
TEST(DungeonStagesTest, WailingCavernsIsNeverDrawnInTraining)
{
    std::vector<Cu::BossRow> const& rows = Cu::InstanceLadderRows(Cu::InstanceLadder::Wing);
    uint32 heldOut = 0;
    for (Cu::StageDefinition const& stage : Cu::CurriculumStages())
    {
        std::vector<uint32> const weights = Weights(stage);
        std::vector<uint32> const finals = Finals(stage);
        for (float progress : { 0.0f, 0.25f, 0.5f, 1.0f })
            for (bool evaluating : { false, true })
            {
                std::vector<uint32> const draw = Cu::ArenaDrawWeights(stage.Arenas, weights, finals, evaluating,
                    progress);
                ASSERT_EQ(draw.size(), stage.Arenas.size());
                for (std::size_t arena = 0; arena < stage.Arenas.size(); ++arena)
                {
                    Cu::ArenaDefinition const& definition = stage.Arenas[arena];
                    bool const wailing = definition.Instance == Cu::InstanceLadder::Wing
                        && definition.InstanceRow == ROW_WAILING_CAVERNS;
                    if (definition.EvalOnly || wailing)
                        EXPECT_EQ(draw[arena], 0u) << stage.Name << " " << definition.Name << " at " << progress
                            << (evaluating ? " in an evaluation" : " in training");
                }
            }
        for (Cu::ArenaDefinition const& arena : stage.Arenas)
        {
            if (arena.Instance != Cu::InstanceLadder::Wing || arena.InstanceRow < 0)
                continue;
            ASSERT_LT(std::size_t(arena.InstanceRow), rows.size());
            if (rows[std::size_t(arena.InstanceRow)].MapId != 43)
                continue;
            EXPECT_TRUE(arena.EvalOnly) << stage.Name << " " << arena.Name << " trains on Wailing Caverns";
            EXPECT_FALSE(arena.CorridorPacks || arena.PullDrill) << stage.Name << " " << arena.Name;
            ++heldOut;
        }
    }
    // Held out from D2 on: D2 and D3 measure it.
    EXPECT_GE(heldOut, 2u);
    for (char const* name : { "dungeon2_ragefire", "dungeon3_deadmines" })
        EXPECT_TRUE(std::any_of(Stage(name).Arenas.begin(), Stage(name).Arenas.end(), [](auto const& arena)
        {
            return arena.EvalOnly && arena.InstanceRow == ROW_WAILING_CAVERNS;
        })) << name;

    // A drill is evaluated only in a stage of drills; in a mixed stage it is training's alone.
    std::vector<Cu::ArenaDefinition> mixed(2);
    mixed[0].Name = "dungeon";
    mixed[1].Name = "pull";
    mixed[1].PullDrill = true;
    std::vector<uint32> const draw = Cu::ArenaDrawWeights(mixed, { 1, 1 }, { 1, 1 }, true, 0.0f);
    EXPECT_GT(draw[0], 0u);
    EXPECT_EQ(draw[1], 0u);
    EXPECT_GT(Cu::ArenaDrawWeights(mixed, { 1, 1 }, { 1, 1 }, false, 0.0f)[1], 0u);
    EXPECT_FALSE(Cu::EvaluatesDrills(mixed));
}

// **A corridor's packs in route order** (G2): a corridor starts at a pack leaving its length after it, the same one
// for the same evaluation seed; its packs are noted as found cleared, in route order when every earlier one already
// was; it is done when all are.
TEST(DungeonStagesTest, ACorridorsPacksAreClearedInRouteOrder)
{
    // Its first pack, over a route of 10 packs and a corridor of 4: 0 to 6.
    std::set<uint32> firsts;
    for (uint32 roll = 0; roll < 100; ++roll)
    {
        uint32 const first = Wr::CorridorFirst(10, 4, false, 0, roll);
        EXPECT_LE(first, 6u);
        firsts.insert(first);
    }
    EXPECT_EQ(firsts.size(), 7u) << "every start is drawn";
    EXPECT_EQ(Wr::CorridorFirst(3, 4, false, 0, 77), 0u) << "a route shorter than the corridor starts at the door";
    std::set<uint32> seeded;
    for (uint32 seed = 0; seed < 64; ++seed)
    {
        EXPECT_EQ(Wr::CorridorFirst(10, 4, true, seed, 0), Wr::CorridorFirst(10, 4, true, seed, 999))
            << "an evaluation's start is its seed's, whatever the roll";
        seeded.insert(Wr::CorridorFirst(10, 4, true, seed, 0));
    }
    EXPECT_GE(seeded.size(), 5u) << "the seeds spread over the starts";

    Wr::Corridor corridor;
    corridor.Begin(2, 6);
    EXPECT_TRUE(corridor.Active());
    EXPECT_EQ(corridor.Length(), 4u);
    std::vector<bool> cleared(10, false);
    cleared[0] = cleared[1] = true;                 // before the corridor: not its
    EXPECT_EQ(corridor.Note(cleared), 0u);
    cleared[2] = true;
    EXPECT_EQ(corridor.Note(cleared), 1u);
    EXPECT_EQ(corridor.Note(cleared), 0u) << "each pack once";
    cleared[4] = true;                              // pulled past pack 3
    EXPECT_EQ(corridor.Note(cleared), 0u);
    EXPECT_EQ(corridor.OutOfOrder, 1u);
    cleared[3] = true;
    EXPECT_EQ(corridor.Note(cleared), 1u);
    EXPECT_FALSE(corridor.Done());
    cleared[5] = true;
    cleared[6] = true;                              // after the corridor: not its
    EXPECT_EQ(corridor.Note(cleared), 1u);
    EXPECT_TRUE(corridor.Done());
    EXPECT_EQ(corridor.InOrder, 3u);
    EXPECT_EQ(corridor.Cleared(), 4u);
    EXPECT_FLOAT_EQ(corridor.Share(), 1.0f);
}

// **The chain pull** (G2's cost, and every dungeon stage's): a pack drawn into a fight another pack started; the
// fight's end forgets them.
TEST(DungeonStagesTest, AChainPullIsAPackJoiningAnotherPacksFight)
{
    Wr::FightPacks drawn;
    EXPECT_EQ(drawn.Note({ 3, 3, 3 }), 0u) << "one pack's fight";
    EXPECT_EQ(drawn.Note({ 3 }), 0u);
    EXPECT_EQ(drawn.Note({ 3, 4 }), 1u) << "the next pack ran in";
    EXPECT_EQ(drawn.Note({ 4, 3 }), 0u);
    EXPECT_EQ(drawn.Note({ 5, 6 }), 2u);
    drawn.End();
    EXPECT_EQ(drawn.Note({ 6 }), 0u) << "a new fight";
}

// **Tier-scaled outcomes** (animus-tier-scaled-outcomes): a wing's tier is its difficulty ladder's rung -- 0 at the
// easiest, rising to the evaluation's own conditions -- never the stage's pinned row, which is the same every run.
TEST(DungeonStagesTest, AWingsTierIsItsLaddersRung)
{
    constexpr uint32 LAST = uint32(Cu::StageScenario::WING_RUNGS.size()) - 1;
    EXPECT_EQ(Wr::TierOfRung(0), 0u);
    EXPECT_EQ(Wr::TierOfRung(LAST), 4u);
    for (uint32 rung = 1; rung <= LAST; ++rung)
        EXPECT_GE(Wr::TierOfRung(rung), Wr::TierOfRung(rung - 1));
    // The ladder's last rung is the evaluation's conditions: the dungeon's own levels, no wipes spared. Every rung is
    // a step of difficulty -- the levels or the spare wipes come down, never back up, and no two rungs are the same.
    Cu::StageScenario::WingRung const& last = Cu::StageScenario::WING_RUNGS.back();
    EXPECT_EQ(last.Lift, 0u);
    EXPECT_EQ(last.ExtraWipes, 0u);
    for (uint32 rung = 1; rung <= LAST; ++rung)
    {
        Cu::StageScenario::WingRung const& before = Cu::StageScenario::WING_RUNGS[rung - 1];
        Cu::StageScenario::WingRung const& now = Cu::StageScenario::WING_RUNGS[rung];
        EXPECT_LE(now.Lift, before.Lift) << rung;
        EXPECT_LE(now.ExtraWipes, before.ExtraWipes) << rung;
        EXPECT_TRUE(now.Lift < before.Lift || now.ExtraWipes < before.ExtraWipes) << rung << " repeats " << rung - 1;
    }

    // SeededPick: the same seed, the same pick; spread over the picks.
    std::set<uint32> picks;
    for (uint32 seed = 0; seed < 100; ++seed)
    {
        EXPECT_EQ(Wr::SeededPick(7, seed), Wr::SeededPick(7, seed));
        EXPECT_LT(Wr::SeededPick(7, seed), 7u);
        picks.insert(Wr::SeededPick(7, seed));
    }
    EXPECT_EQ(picks.size(), 7u);
    EXPECT_EQ(Wr::SeededPick(1, 12345), 0u);
}

// **Each stage's purpose is paid as Outcome, its prices as Cost** (the fade takes Shaping away): the corridor's packs
// and the full clear (Clear), the pulls started ready (ReadyPull), the bosses (Kill), the drill's clean pull
// (PullClean); the chain pull (PullExtra), standing about (Idle), straying from the leader (Lost), deaths and wipes
// (Death), the clock (StepCost, Timeout).
TEST(DungeonStagesTest, ThePurposesAreOutcomesAndThePricesCosts)
{
    using Cu::RewardTerm;
    for (RewardTerm term : { RewardTerm::Clear, RewardTerm::ReadyPull, RewardTerm::Kill, RewardTerm::PullClean })
        EXPECT_EQ(Cu::RewardTermCategory(term), Cu::RewardCategory::Outcome) << Cu::RewardTermName(term);
    for (RewardTerm term : { RewardTerm::PullExtra, RewardTerm::Idle, RewardTerm::Lost, RewardTerm::Death,
        RewardTerm::StepCost, RewardTerm::Timeout })
        EXPECT_EQ(Cu::RewardTermCategory(term), Cu::RewardCategory::Cost) << Cu::RewardTermName(term);
    EXPECT_EQ(Cu::RewardTermName(RewardTerm::ReadyPull), "ready_pull");
    EXPECT_EQ(Cu::RewardTermName(RewardTerm::Idle), "idle");
    EXPECT_TRUE(Cu::EveryRewardTermCategorised());
}

// **The stand-in's share** (I7, from G2 on): every arena of the party stages has the "human" stand-in in a fifth of
// its training runs; the stages before them keep StandIn.Share's (0 by default: none), so M4's follow is unchanged.
TEST(DungeonStagesTest, TheStandInPlaysAShareOfEveryPartyStage)
{
    for (char const* name : PARTY_STAGES)
        for (Cu::ArenaDefinition const& arena : Stage(name).Arenas)
        {
            if (arena.EvalOnly)
            {
                EXPECT_LT(arena.StandInShare, 1) << name << " " << arena.Name << ": held out, never trained";
                continue;
            }
            EXPECT_GT(arena.StandInShare, 0) << name << " " << arena.Name;
            EXPECT_LE(arena.StandInShare, 100) << name << " " << arena.Name;
            EXPECT_TRUE(arena.Seats == Cu::SeatPlan::Party) << name << " " << arena.Name;
        }
    for (char const* name : { "move4_follow", "combat3_survive", "group1_roles" })
        for (Cu::ArenaDefinition const& arena : Stage(name).Arenas)
            EXPECT_EQ(arena.StandInShare, -1) << name << " " << arena.Name;
}

// **The per-boss measures**: every boss of Ragefire Chasm and the Deadmines (side bosses among them) has its column;
// Wailing Caverns' too, for the held-out runs. Each row's last boss is among its dungeon's.
TEST(DungeonStagesTest, EveryBossOfTheDungeonsIsMeasured)
{
    std::vector<Cu::WingBoss> const& bosses = Cu::WingBosses();
    auto const of = [&bosses](uint32 map)
    {
        std::set<uint32> out;
        for (Cu::WingBoss const& boss : bosses)
            if (boss.MapId == map)
                out.insert(boss.Entry);
        return out;
    };
    EXPECT_EQ(of(MAP_RAGEFIRE_CHASM), (std::set<uint32>{ 11517, 11518, 11519, 11520 }));
    EXPECT_EQ(of(MAP_DEADMINES), (std::set<uint32>{ 639, 642, 643, 644, 645, 646, 647, 1763 }));
    EXPECT_GE(of(43).size(), 4u);
    std::set<std::string> names;
    for (Cu::WingBoss const& boss : bosses)
    {
        EXPECT_TRUE(names.insert(boss.Name).second) << "two columns named boss_" << boss.Name;
        for (char c : std::string(boss.Name))
            EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_') << boss.Name;
    }
    for (Cu::BossRow const& row : Cu::InstanceLadderRows(Cu::InstanceLadder::Wing))
        EXPECT_TRUE(of(row.MapId).count(row.Entry)) << row.Name;
}

// **D3's doors, levers and the cannon through the client's handlers** (the user: "use the proper actions to activate
// doors and cannons"; no auto doors): D3's layout acts on what its sight list names -- an interact (CMSG_GAMEOBJ_USE)
// on each of the Deadmines' levers, the key item (the Defias Gunpowder, CMSG_USE_ITEM) on the cannon -- and the doors
// the levers and the cannon open are shut to a hand.
TEST(DungeonStagesTest, TheDeadminesDoorsLeversAndCannonAreUsedThroughTheHandlers)
{
    Cu::StageDefinition const& d3 = Stage("dungeon3_deadmines");
    ASSERT_TRUE(d3.Has(Cu::BlockId::Sight));
    EXPECT_FALSE(d3.Has(Cu::BlockId::Crowd)) << "no use-object press off a server list";
    EXPECT_EQ(Cu::SightBlock::PressOf(Cu::SightBlock::ACTION_INTERACT_FIRST), Ea::Press::Interact);
    EXPECT_EQ(Cu::SightBlock::PressOf(Cu::SightBlock::ACTION_USE_ITEM_FIRST), Ea::Press::UseItem);

    // The Deadmines' levers (the Factory, Foundry and Mast Room doors'): an interact through CMSG_GAMEOBJ_USE.
    constexpr uint32 LEVERS[] = { 101831, 101834, 101832 };
    for (uint32 lever : LEVERS)
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::GameObject>(lever, 9);
        WorldPacket packet = Ea::GameObjectUse(guid);
        EXPECT_EQ(packet.GetOpcode(), CMSG_GAMEOBJ_USE);
        ObjectGuid sent;
        packet >> sent;
        EXPECT_EQ(sent, guid);
        Ea::ObjectFacts judged;
        judged.Type = GAMEOBJECT_TYPE_BUTTON;
        judged.Reach = 5.0f;
        judged.Distance = 3.0f;
        EXPECT_EQ(Ea::JudgeObjectUse(judged), Ea::Refusal::None) << lever;
    }

    // The Iron Clad Door's cannon: its key item (the gunpowder) used on it through CMSG_USE_ITEM, never a hand.
    SpellCastTargets targets;
    targets.SetGOTarget(nullptr);
    EXPECT_EQ(Ea::UseItem(255, 23, ObjectGuid::Empty, 6250, 1, targets).GetOpcode(), CMSG_USE_ITEM);

    // The doors themselves are shut to a hand: a locked door, button or goober opens only by its lever or key.
    EXPECT_TRUE(Ea::LockedToHand(GAMEOBJECT_TYPE_DOOR, 85));
    EXPECT_TRUE(Ea::LockedToHand(GAMEOBJECT_TYPE_GOOBER, 83));
    Ea::ObjectFacts door;
    door.Type = GAMEOBJECT_TYPE_DOOR;
    door.Reach = 5.0f;
    door.Distance = 3.0f;
    door.Locked = true;
    EXPECT_NE(Ea::JudgeObjectUse(door), Ea::Refusal::None) << "a locked door refuses a hand";
    // Nothing is looted.
    door.Type = GAMEOBJECT_TYPE_CHEST;
    door.Locked = false;
    EXPECT_EQ(Ea::JudgeObjectUse(door), Ea::Refusal::Loot);
}

// **M1 and M2 are unchanged** by the party stages (their layouts are pinned in SightBlockTest as well): their blocks,
// one arena each of their own oppositions, no stand-in.
TEST(DungeonStagesTest, TheMovementStagesAreUnchanged)
{
    using Id = Cu::BlockId;
    EXPECT_EQ(Stage("move1_controls").Blocks,
        (std::vector<Id>{ Id::Core, Id::Move, Id::Compass, Id::Vision, Id::Entities, Id::Goal }));
    EXPECT_EQ(Stage("move2_seek").Blocks, (std::vector<Id>{ Id::Core, Id::Move, Id::Vision, Id::Entities, Id::Map,
        Id::Goal }));
    for (char const* name : { "move1_controls", "move2_seek" })
        for (Cu::ArenaDefinition const& arena : Stage(name).Arenas)
        {
            EXPECT_EQ(arena.StandInShare, -1) << name;
            EXPECT_FALSE(arena.CorridorPacks || arena.LevelFirst) << name;
            EXPECT_EQ(arena.Seats, Cu::SeatPlan::Solo) << name;
        }
    EXPECT_EQ(Stage("move1_controls").Arenas.front().Against, Cu::Opposition::Sight);
    EXPECT_EQ(Stage("move2_seek").Arenas.front().Against, Cu::Opposition::Seek);
}

// **Lost is priced from the party's actual leader**: the "human" stand-in when it leads (it sits in seat 0), else the
// tank -- never the tank while a leading stand-in has the party; the leader itself never strays.
TEST(DungeonStagesTest, LostIsPricedFromTheActualLeader)
{
    constexpr int32 TANK = 2;
    EXPECT_EQ(Wr::LeaderSeat(-1, false, TANK), TANK) << "an all-bot party follows its tank";
    EXPECT_EQ(Wr::LeaderSeat(3, false, TANK), TANK) << "a following stand-in leads nobody";
    EXPECT_EQ(Wr::LeaderSeat(0, true, TANK), 0) << "a leading stand-in is the leader";
    EXPECT_EQ(Wr::LeaderSeat(-1, true, -1), -1);

    float const stray = 25.0f;
    // Seat 1, 30 yd from the leading stand-in: strays. The tank 30 yd off is not its measure when the stand-in leads.
    EXPECT_TRUE(Wr::Strays(true, false, false, true, 30.0f, stray));
    EXPECT_FALSE(Wr::Strays(true, false, false, true, 10.0f, stray));
    EXPECT_FALSE(Wr::Strays(true, true, false, true, 90.0f, stray)) << "the leader does not stray from itself";
    EXPECT_FALSE(Wr::Strays(true, false, false, false, 90.0f, stray)) << "a dead leader is no one to keep with";
    EXPECT_FALSE(Wr::Strays(false, false, false, true, 90.0f, stray)) << "the dead are Away's";
}

// **A risen seat walking back pays Away, never Lost**: the same seconds are not charged twice. Driven through the
// RespawnClock as the wing's RiseDead drives it: dead (Away), risen and walking back from the entrance (Away, not Lost
// however far), rejoined (neither, until it strays again: Lost).
TEST(DungeonStagesTest, ARisenSeatWalkingBackIsAwayNotLost)
{
    Cu::RespawnClock clock;
    float const stray = 25.0f;
    EXPECT_EQ(clock.Note(1000, false, -1.0f, 10000, 15.0f), Cu::RespawnClock::Step::Died);
    EXPECT_TRUE(Wr::Away(false, clock.Rejoining));
    EXPECT_EQ(clock.Note(11000, false, -1.0f, 10000, 15.0f), Cu::RespawnClock::Step::Rise);
    clock.Risen(11000);
    ASSERT_TRUE(clock.Rejoining);
    // At the entrance, 200 yd from the leader, walking back.
    EXPECT_EQ(clock.Note(12000, true, 200.0f, 10000, 15.0f), Cu::RespawnClock::Step::None);
    EXPECT_TRUE(Wr::Away(true, clock.Rejoining));
    EXPECT_FALSE(Wr::Strays(true, false, clock.Rejoining, true, 200.0f, stray));
    // Back with the party.
    EXPECT_EQ(clock.Note(60000, true, 10.0f, 10000, 15.0f), Cu::RespawnClock::Step::Rejoined);
    EXPECT_FALSE(Wr::Away(true, clock.Rejoining));
    EXPECT_FALSE(Wr::Strays(true, false, clock.Rejoining, true, 10.0f, stray));
    // Wandering off again afterwards is a stray.
    EXPECT_TRUE(Wr::Strays(true, false, clock.Rejoining, true, 40.0f, stray));
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Away), Cu::RewardCategory::Cost);
}

// **No learned seat reads the crowd block** (bots perceive only what a player perceives): its pack ahead, overflow and
// nearest object are radius reads off the server's lists. Every stage trains a policy, so none carries it: the startup
// check refuses it anywhere.
TEST(DungeonStagesTest, NoStageDeclaresTheCrowdBlock)
{
    EXPECT_TRUE(Cu::CurriculumProblems().empty());
    for (Cu::StageDefinition const& stage : Cu::CurriculumStages())
        EXPECT_FALSE(stage.Has(Cu::BlockId::Crowd)) << stage.Name << " trains a policy on the crowd block";
    // Nor is any stage left without a learned seat: the teacher's check stages are gone.
    for (char const* name : { "teacher_ragefire", "teacher_deadmines" })
        EXPECT_EQ(Cu::FindStage(name), nullptr) << name;
}

namespace
{
    namespace Sp = Animus::Curriculum::SeenPlaces;

    bool Near(Sp::Point const& a, Sp::Point const& b)
    {
        return Sp::Distance(a, b) < 0.01f;
    }
}

// **An unseen pack or boss never reaches a seat's goal places** (the coordinator's ruling, 2026-10-07): the places are
// what the seat's entity memory saw (alive, where it last saw it), its map's frontier, the layout's unexplored ground
// and the leader -- nothing else. A pack it never saw is no input at all; a seen one that died is no place; nor is a
// game object. Every place chosen is one of the inputs.
TEST(DungeonStagesTest, AnUnseenPackOrBossNeverReachesTheGoalPlaces)
{
    Sp::Point const unseenPack{ 40.0f, 0.0f, 0.0f };
    Sp::Point const unseenBoss{ 120.0f, 30.0f, -5.0f };
    std::vector<Sp::Point> const layout = { { 0.0f, 60.0f, 0.0f }, { 30.0f, 60.0f, 0.0f }, { 90.0f, 60.0f, 0.0f } };

    Sp::Input in;
    in.Seat = { 0.0f, 0.0f, 0.0f };
    in.Memory = { { { 15.0f, 5.0f, 0.0f }, true, false, false },        // a pack it saw: a place
        { { 25.0f, -5.0f, 0.0f }, true, true, false },                  // one it saw die: none
        { { 8.0f, 8.0f, 0.0f }, false, false, false },                  // a friend: none
        { { 9.0f, -9.0f, 0.0f }, true, false, true } };                 // a game object: none
    in.Frontier = { { 0.0f, 20.0f, 0.0f } };
    in.Layout = &layout;
    in.LayoutExplored = { true, false, false };
    in.HasLeader = true;
    in.Leader = { -5.0f, 0.0f, 0.0f };

    Sp::Choice const choice = Sp::Choose(in);
    std::vector<Sp::Point> const allowed = { { 15.0f, 5.0f, 0.0f }, { 0.0f, 20.0f, 0.0f }, layout[1], layout[2],
        in.Leader };
    uint32 present = 0;
    for (uint32 place = 0; place < Sp::PLACES; ++place)
    {
        if (!choice.Present[place])
            continue;
        ++present;
        Sp::Point const& at = choice.Where[place];
        EXPECT_TRUE(std::any_of(allowed.begin(), allowed.end(), [&at](Sp::Point const& ok) { return Near(ok, at); }))
            << "place " << place << " at " << at.X << " " << at.Y << " is nothing the seat saw, mapped or follows";
        EXPECT_FALSE(Near(at, unseenPack) || Near(at, unseenBoss));
        EXPECT_FALSE(Near(at, { 25.0f, -5.0f, 0.0f })) << "a pack seen dead";
        EXPECT_FALSE(Near(at, layout[0])) << "explored ground is no way on";
    }
    EXPECT_GE(present, 4u);
    EXPECT_TRUE(Near(choice.Where[0], { 15.0f, 5.0f, 0.0f })) << "the nearest thing it saw first";
    EXPECT_TRUE(Near(choice.Where[Sp::WAY_ON], { 0.0f, 20.0f, 0.0f })) << "its own frontier before the layout";
    EXPECT_TRUE(Near(choice.Where[Sp::LEADER], in.Leader));
    ASSERT_TRUE(choice.HasAssignment);
    EXPECT_TRUE(Near(choice.Assignment, { 15.0f, 5.0f, 0.0f }));

    // Seen only: no layout node, ever.
    Sp::Input seenOnly = in;
    seenOnly.Layout = nullptr;
    seenOnly.LayoutExplored.clear();
    Sp::Choice const only = Sp::Choose(seenOnly);
    for (uint32 place = 0; place < Sp::PLACES; ++place)
        if (only.Present[place])
            for (Sp::Point const& node : layout)
                EXPECT_FALSE(Near(only.Where[place], node)) << "seen only, yet a layout node";

    // Nothing seen, nothing mapped, no leader: no place and no assignment -- never a fallback to the route.
    Sp::Input blind;
    Sp::Choice const none = Sp::Choose(blind);
    EXPECT_TRUE(std::none_of(none.Present.begin(), none.Present.end(), [](bool p) { return p; }));
    EXPECT_FALSE(none.HasAssignment);
}

// **The layout input holds no creature data**: a node is a position and nothing else (SeenPlaces::Point), the nodes
// are ground samples at least the spacing apart, and they keep no order -- the same ground walked in any order gives
// the same nodes, so the route's pack order cannot survive in them.
TEST(DungeonStagesTest, TheLayoutHoldsNoCreatureDataAndNoOrder)
{
    static_assert(sizeof(Sp::Point) == 3 * sizeof(float));
    std::vector<Sp::Point> ground;
    for (int32 yard = 0; yard < 200; ++yard)
        ground.push_back({ float(yard), float(yard % 40), 0.0f });
    std::vector<Sp::Point> const nodes = Sp::Layout(ground, 25.0f);
    ASSERT_GE(nodes.size(), 4u);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        EXPECT_TRUE(std::any_of(ground.begin(), ground.end(), [&](Sp::Point const& g) { return Near(g, nodes[i]); }));
        for (std::size_t j = i + 1; j < nodes.size(); ++j)
            EXPECT_GE(Sp::Distance(nodes[i], nodes[j]), 25.0f);
    }
    std::vector<Sp::Point> reversed(ground.rbegin(), ground.rend());
    std::vector<Sp::Point> const again = Sp::Layout(reversed, 25.0f);
    ASSERT_EQ(again.size(), nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i)
        EXPECT_TRUE(Near(again[i], nodes[i])) << "the walk's order leaks into the layout";
}

// **The frontier is the seat's own map's**: open ground it has seen with unseen ground beside it, nearest first, the
// points apart; nothing where it has seen everything round.
TEST(DungeonStagesTest, TheFrontierIsOpenGroundBesideTheUnseen)
{
    // Seen: a corridor x in [-10, 30], |y| <= 4, walls at |y| = 6; beyond x = 30 nothing is known.
    auto const probe = [](float x, float y)
    {
        if (x > 30.0f || x < -10.0f || std::fabs(y) > 6.0f)
            return Sp::Ground::Unknown;
        return std::fabs(y) > 4.0f ? Sp::Ground::Shut : Sp::Ground::Open;
    };
    std::vector<Sp::Point> const frontier = Sp::Frontier({ 0.0f, 0.0f, 0.0f }, 40.0f, 2.0f, 6, probe);
    ASSERT_FALSE(frontier.empty());
    for (Sp::Point const& at : frontier)
        EXPECT_TRUE(at.X >= 29.0f || at.X <= -9.0f) << at.X << " " << at.Y << " is inside the seen corridor";
    EXPECT_TRUE(Sp::Frontier({ 0.0f, 0.0f, 0.0f }, 40.0f, 2.0f, 6,
        [](float, float) { return Sp::Ground::Open; }).empty());
    // The party stages default to what the seat discovered alone (the user, 2026-10-07); a conf key adds the layout.
    for (char const* name : PARTY_STAGES)
        EXPECT_EQ(Stage(name).GoalPlaces, Sp::Source::SeenOnly) << name;
}
