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
#include "GameObjectAIFactory.h"
#include "CurriculumTuning.h"
#include "EntitiesBlock.h"
#include "EntityActions.h"
#include "EntityMemory.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "InteractDraw.h"
#include "InteractEncounter.h"
#include "Layout.h"
#include "ObjectMgr.h"
#include "RewardLedger.h"
#include "SeatView.h"
#include "SightBlock.h"
#include "Spell.h"
#include "StageDefinition.h"
#include "StageState.h"
#include "VisionCaster.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <cmath>
#include <set>
#include <vector>

/// **M3 interact** (move3_interact, InteractEncounter): the stage's layout, its ladder and draws, what its goal names,
/// how a press is judged, a lever opening its door through the session's own handler, the key item being what opens
/// the lock, and the open door the camera still draws. The sites against the map's own data are
/// DeadminesSitesDataTest's.
namespace Cu = Animus::Curriculum;
namespace Ea = Animus::Curriculum::EntityActions;
namespace Draw = Animus::Curriculum::InteractDraw;
namespace Vi = Animus::Vision;

namespace
{
    using Sight = Cu::SightBlock;

    Cu::StageDefinition const& Interact()
    {
        Cu::StageDefinition const* stage = Cu::FindStage("move3_interact");
        EXPECT_NE(stage, nullptr) << "move3_interact was left out";
        static Cu::StageDefinition const none;
        return stage ? *stage : none;
    }

    Cu::ArenaDefinition const& Sites()
    {
        return Interact().Arenas.at(0);
    }

    /// Keeps every packet a press sends, instead of handing it to the session.
    class Recorder final : public Ea::ClientPort
    {
    public:
        void Send(Player* /*bot*/, WorldPacket& packet) override { Sent.push_back(packet); }
        std::vector<WorldPacket> Sent;
    };

    /// A game object in the test map, as the map's own store finds it (Map::GetGameObject), with a template the
    /// object manager knows (its AI is the plain GameObjectAI) -- enough for CMSG_GAMEOBJ_USE's handler and
    /// GameObject::Use of a door, a button or a goober.
    class TestGameObject final : public GameObject
    {
    public:
        void Place(ObjectGuid::LowType low, GameObjectTemplate const& info, Map* map, float x, float y)
        {
            Object::_Create(low, info.entry, HighGuid::GameObject);
            SetEntry(info.entry);
            m_goInfo = &info;
            SetObjectScale(1.0f);
            SetByteValue(GAMEOBJECT_BYTES_1, 1, uint8(info.type));
            SetByteValue(GAMEOBJECT_BYTES_1, 0, uint8(GO_STATE_READY));
            m_lootState = GO_READY;
            WorldObject::SetMap(map);
            Relocate(x, y, 0.0f, 0.0f);
            SetWorldRotation(G3D::Quat(0.0f, 0.0f, 0.0f, 1.0f));
            SetPhaseMask(1, false);
            AIM_Initialize();
            Object::AddToWorld();
            map->GetObjectsStore().Insert<GameObject>(GetGUID(), this);
        }

        void Lift(Map* map)
        {
            map->GetObjectsStore().Remove<GameObject>(GetGUID());
            map->RemoveUpdateObject(this);
            Object::RemoveFromWorld();
        }
    };

    /// A template the object manager returns for `entry` (GameObject::GetAIName reads it from there).
    GameObjectTemplate const& Template(uint32 entry, uint32 type, uint32 lockId, uint32 autoClose = 0)
    {
        static bool registered = false;
        if (!registered)
        {
            registered = true;
            (new GameObjectAIFactory<GameObjectAI>("GameObjectAI"))->RegisterSelf();
        }
        auto* store = const_cast<GameObjectTemplateContainer*>(sObjectMgr->GetGameObjectTemplates());
        GameObjectTemplate& info = (*store)[entry];
        info = GameObjectTemplate();
        info.entry = entry;
        info.type = type;
        info.size = 1.0f;
        info.AIName = "GameObjectAI";
        info.ScriptId = 0;
        // A door's, a button's and a goober's lock are their own fields of the data (door.lockId, button.lockId,
        // goober.lockId); GetLockId reads the one of the type.
        if (type == GAMEOBJECT_TYPE_GOOBER)
            info.goober.lockId = lockId;
        else
        {
            info.door.lockId = lockId;
            info.door.autoCloseTime = autoClose;
        }
        return info;
    }

    class InteractActionsTest : public IntegrationTestFixture
    {
    protected:
        void SetUp() override
        {
            IntegrationTestFixture::SetUp();
            _bot = CreateTestPlayer();
            _bot->SetFaction(TEST_FACTION_HOSTILE_TO_MONSTERS);
            _bot->Relocate(0.0f, 0.0f, 0.0f, 0.0f);
        }

        void TearDown() override
        {
            for (TestGameObject* object : _objects)
            {
                object->Lift(GetTestMap());
                delete object;
            }
            _objects.clear();
            IntegrationTestFixture::TearDown();
        }

        TestGameObject* Spawn(ObjectGuid::LowType low, GameObjectTemplate const& info, float x, float y)
        {
            auto* object = new TestGameObject();
            object->Place(low, info, GetTestMap(), x, y);
            _objects.push_back(object);
            return object;
        }

        /// Finds only `objects`, as the seat's client has them.
        Ea::Resolver Knows(std::vector<WorldObject*> objects)
        {
            return [objects](Player* /*bot*/, ObjectGuid guid) -> WorldObject*
            {
                for (WorldObject* object : objects)
                    if (object->GetGUID() == guid)
                        return object;
                return nullptr;
            };
        }

        TestPlayer* _bot = nullptr;
        std::vector<TestGameObject*> _objects;
    };
}

// The stage: after M2, its blocks the plan's (Core, Move, Vision with its entity list, Map, Sight, Goal; no compass),
// the Deadmines band, a training arena and the held-out sweep over the same four sites; the curriculum accepts it.
TEST(InteractStageTest, TheStagesLayout)
{
    using Id = Cu::BlockId;
    Cu::StageDefinition const& stage = Interact();
    ASSERT_EQ(stage.Name, "move3_interact");
    EXPECT_EQ(stage.Extends, "move2_seek");
    EXPECT_EQ(stage.Blocks, (std::vector<Id>{ Id::Core, Id::Move, Id::Vision, Id::Entities, Id::Map, Id::Sight,
        Id::Goal }));
    EXPECT_FALSE(stage.Has(Id::Compass));
    EXPECT_EQ(stage.MapId, 36u);
    EXPECT_EQ(stage.FocusLevelFirst, 17);
    EXPECT_EQ(stage.FocusLevelLast, 20);
    EXPECT_EQ(stage.FocusChance, 100);
    EXPECT_EQ(stage.Level, 0);
    ASSERT_EQ(stage.Arenas.size(), 2u);
    EXPECT_EQ(stage.Arenas[0].Name, "sites");
    EXPECT_FALSE(stage.Arenas[0].EvalOnly);
    EXPECT_EQ(stage.Arenas[1].Name, "sweep");
    EXPECT_TRUE(stage.Arenas[1].EvalOnly);
    for (Cu::ArenaDefinition const& arena : stage.Arenas)
    {
        EXPECT_EQ(arena.Against, Cu::Opposition::Interact);
        EXPECT_EQ(arena.Sites.size(), 4u);
        EXPECT_EQ(arena.Objects.size(), 5u);
        EXPECT_FLOAT_EQ(arena.SeekRadius, 3.0f);
    }
    EXPECT_TRUE(Cu::CurriculumProblems().empty());

    // The sight block with its named row (revision 2): the slots, then the row.
    Cu::Layout layout;
    Cu::Block const& sight = Cu::GetBlock(Id::Sight);
    EXPECT_EQ(sight.Revision(), 2u);
    EXPECT_EQ(sight.Size(layout).Obs, Cu::SIGHT_SLOTS * Sight::SIGHT_FEATURES + Sight::NAMED_FEATURES);
    EXPECT_EQ(uint32(Sight::NAMED_FEATURES), uint32(Cu::EntitiesBlock::ENTITY_FEATURES) + 3);
    boost::json::object entry;
    sight.DescribeManifest(layout, entry);
    boost::json::object const& named = entry.at("sight").as_object().at("named").as_object();
    EXPECT_EQ(named.at("offset").to_number<uint32>(), Sight::NAMED_FIRST);
    EXPECT_EQ(named.at("width").to_number<uint32>(), uint32(Sight::NAMED_FEATURES));
    EXPECT_EQ(named.at("tasks").as_array().size(), 3u);

    // M1 and M2 carry no sight block, so neither the named row nor anything else of this stage changes them
    // (SightBlockLayoutTest.TheMovementStagesAreUnchanged pins their every block).
    for (char const* name : { "move1_controls", "move2_seek" })
    {
        Cu::StageDefinition const* movement = Cu::FindStage(name);
        ASSERT_NE(movement, nullptr) << name;
        EXPECT_FALSE(movement->Has(Id::Sight)) << name;
        for (Cu::ArenaDefinition const& arena : movement->Arenas)
            EXPECT_NE(arena.Against, Cu::Opposition::Interact) << name;
    }
}

// The sites: three a lever opens and one its key item opens, each a door and its opener of the Deadmines' own; the
// lever doors have floor behind them, every site floor on the opener's side, and no point is on top of the door or
// its opener.
TEST(InteractStageTest, TheSites)
{
    Cu::ArenaDefinition const& arena = Sites();
    std::set<std::string> names;
    uint32 levers = 0;
    uint32 keys = 0;
    for (Cu::InteractSite const& site : arena.Sites)
    {
        names.insert(site.Name);
        EXPECT_NE(site.Door, 0u) << site.Name;
        EXPECT_NE(site.Opener, 0u) << site.Name;
        EXPECT_GE(site.Near.size(), 10u) << site.Name;
        if (site.Key)
        {
            ++keys;
            EXPECT_EQ(site.Key, 5397u) << site.Name;        // the Defias Gunpowder
            EXPECT_EQ(site.Opener, 16398u) << site.Name;    // the Defias Cannon
            EXPECT_EQ(site.Door, 16397u) << site.Name;      // the Iron Clad Door
        }
        else
        {
            ++levers;
            EXPECT_GE(site.Far.size(), 10u) << site.Name;
        }
    }
    EXPECT_EQ(names, (std::set<std::string>{ "factory", "foundry", "mast_room", "iron_clad" }));
    EXPECT_EQ(levers, 3u);
    EXPECT_EQ(keys, 1u);
    // Near and far never share a point.
    for (Cu::InteractSite const& site : arena.Sites)
        for (Position const& near : site.Near)
            for (Position const& far : site.Far)
                EXPECT_GT(near.GetExactDist2d(&far), 0.5f) << site.Name;
}

// The ladder steps on the fade's rungs (1, 0.5, 0): distinguish, switch, key; a training episode keeps a share of the
// rung below; each rung plays the sites it can, and asks the task it is for.
TEST(InteractStageTest, TheLadder)
{
    EXPECT_EQ(Draw::RungOf(1.0f), Draw::Rung::Distinguish);
    EXPECT_EQ(Draw::RungOf(0.5f), Draw::Rung::Switch);
    EXPECT_EQ(Draw::RungOf(0.0f), Draw::Rung::Key);
    EXPECT_EQ(Draw::RungOf(0.3f), Draw::Rung::Switch);
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Key, 0.05f, 0.1f), Draw::Rung::Switch);
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Key, 0.5f, 0.1f), Draw::Rung::Key);
    EXPECT_EQ(Draw::PlacedRung(Draw::Rung::Distinguish, 0.0f, 0.1f), Draw::Rung::Distinguish);

    std::vector<Cu::InteractSite> const& sites = Sites().Sites;
    EXPECT_EQ(Draw::RungSites(sites, Draw::Rung::Distinguish).size(), 4u);
    for (uint32 site : Draw::RungSites(sites, Draw::Rung::Switch))
        EXPECT_EQ(sites[site].Key, 0u);
    EXPECT_EQ(Draw::RungSites(sites, Draw::Rung::Switch).size(), 3u);
    ASSERT_EQ(Draw::RungSites(sites, Draw::Rung::Key).size(), 1u);
    EXPECT_EQ(sites[Draw::RungSites(sites, Draw::Rung::Key)[0]].Name, "iron_clad");

    EXPECT_EQ(Draw::TaskOf(Draw::Rung::Distinguish), Draw::Task::Reach);
    EXPECT_EQ(Draw::TaskOf(Draw::Rung::Switch), Draw::Task::Reach);
    EXPECT_EQ(Draw::TaskOf(Draw::Rung::Key), Draw::Task::UseItem);
    // The fade's rungs are the ladder's, and the rung column is the one *_rung column (the evaluation videos).
    EXPECT_EQ(Draw::FADE_SCALES, (std::array<float, Draw::RUNGS>{ 1.0f, 0.5f, 0.0f }));
}

// Decoys are other kinds than the named one, each kind once; two to four of them, never more than the pool allows.
// An evaluation's layout is its seed's, every time; the held-out sweep goes round every rung.
TEST(InteractStageTest, DecoysAndEvaluationDraws)
{
    uint32 const objects = uint32(Sites().Objects.size());
    for (uint32 target = 0; target < objects; ++target)
        for (float u : { 0.0f, 0.3f, 0.7f, 0.999f })
            for (uint32 count = 0; count <= 4; ++count)
            {
                std::vector<uint32> const kinds = Draw::DecoyKinds(target, count, objects, u);
                EXPECT_EQ(kinds.size(), std::min<uint32>(count, objects - 1));
                EXPECT_EQ(std::count(kinds.begin(), kinds.end(), target), 0);
                EXPECT_EQ(std::set<uint32>(kinds.begin(), kinds.end()).size(), kinds.size());
            }
    for (float u : { 0.0f, 0.5f, 0.999f })
    {
        uint32 const count = Draw::DecoyCount(u, 2, 4, objects);
        EXPECT_GE(count, 2u);
        EXPECT_LE(count, 4u);
    }
    EXPECT_EQ(Draw::DecoyCount(0.999f, 2, 4, 3), 2u);

    std::vector<uint32> const sites{ 0, 1, 2, 3 };
    for (uint32 seed = 0; seed < 16; ++seed)
    {
        Draw::EvaluationEpisode const a = Draw::EvaluationPick(seed, sites);
        Draw::EvaluationEpisode const b = Draw::EvaluationPick(seed, sites);
        EXPECT_EQ(a.Site, b.Site);
        EXPECT_FLOAT_EQ(a.SpawnU, b.SpawnU);
        EXPECT_FLOAT_EQ(a.TargetU, b.TargetU);
        EXPECT_EQ(a.Site, seed % 4);
    }
    std::set<Draw::Rung> rungs;
    for (uint32 seed = 0; seed < 3; ++seed)
        rungs.insert(Draw::SweepRung(seed));
    EXPECT_EQ(rungs.size(), 3u);
}

// What a press came to: the named object used is the outcome; a decoy pressed is priced (WrongObject, a Cost, each
// decoy once in the encounter); the site's lever is the switch rung's way through; a refused press is nothing here (the
// sight block prices it); and a lock asked for its key is opened by the key alone.
TEST(InteractStageTest, AWrongObjectPressIsPriced)
{
    std::vector<uint64> const decoys{ 31, 32 };
    Draw::PressFacts facts;
    facts.Target = 30;
    facts.Opener = 40;
    facts.Decoys = &decoys;
    facts.Sent = true;
    facts.Press = uint8(Ea::Press::Interact);

    facts.On = 31;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Wrong);
    facts.Press = uint8(Ea::Press::UseItem);
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Wrong);
    facts.Sent = false;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::None);
    facts.Sent = true;
    facts.Press = uint8(Ea::Press::Select);
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::None);

    facts.Press = uint8(Ea::Press::Interact);
    facts.On = 30;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Right);
    facts.On = 40;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Opener);
    facts.On = 99;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::None);

    // The key rung: the named thing is the lock; a hand on it is nothing, the key on it is the outcome.
    facts.Asked = Draw::Task::UseItem;
    facts.Target = 40;
    facts.On = 40;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::None);
    facts.Press = uint8(Ea::Press::UseItem);
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Right);

    // The prices: the decoy a Cost (never faded), the door opened and the right object Outcomes.
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::WrongObject), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::DoorOpened), Cu::RewardCategory::Outcome);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Arrive), Cu::RewardCategory::Outcome);
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Sighting), Cu::RewardCategory::Shaping);
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::WrongObject), "wrong_object");
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::DoorOpened), "door_opened");
    Cu::CurriculumTuning::InteractTuning const tuning;
    EXPECT_GT(tuning.WrongObject, 0.0f);
    EXPECT_LT(tuning.WrongObject, tuning.Arrive);
    EXPECT_GT(Cu::CurriculumTuning::ActionTuning().AimlessActRefused, 0.0f);
}

// DoorOpened is paid once an episode, on the door's first opening after the seat's own lever press: the lever pressed
// twice, or the door opening, shutting and opening again, pays once; an opening before any press pays nothing (it is
// measured), and a later press's opening still pays.
TEST(InteractStageTest, TheDoorIsPaidOnceAnEpisode)
{
    Draw::DoorWatch watch;
    EXPECT_FALSE(watch.Look(false));
    watch.Press();
    watch.Press();
    EXPECT_TRUE(watch.Look(true));
    EXPECT_FALSE(watch.Look(true));
    watch.Press();
    EXPECT_FALSE(watch.Look(true));
    EXPECT_FALSE(watch.Look(false));
    EXPECT_FALSE(watch.Look(true));
    watch.Press();
    EXPECT_FALSE(watch.Look(false));
    EXPECT_FALSE(watch.Look(true));
    EXPECT_TRUE(watch.Opened && watch.ByLever && watch.Paid);

    Draw::DoorWatch early;
    EXPECT_FALSE(early.Look(true));
    EXPECT_TRUE(early.Opened);
    EXPECT_FALSE(early.ByLever);
    EXPECT_FALSE(early.Look(false));
    early.Press();
    EXPECT_TRUE(early.Look(true));
    EXPECT_FALSE(early.Look(false));
    EXPECT_FALSE(early.Look(true));
}

// A decoy is taken for the named object only by pressing it within reach (sent, or refused for what it is: a chest's
// loot, its lock) -- never by a press refused for distance or a remembered decoy gone, and never by passing near it
// (the stop beside it is the encounter's other way, InteractEncounter::Reward).
TEST(InteractStageTest, AWrongObjectIsTakenByPressingOrStoppingNeverPassing)
{
    EXPECT_TRUE(Draw::Reached(uint8(Ea::Refusal::None)));
    EXPECT_TRUE(Draw::Reached(uint8(Ea::Refusal::Loot)));
    EXPECT_TRUE(Draw::Reached(uint8(Ea::Refusal::Locked)));
    EXPECT_FALSE(Draw::Reached(uint8(Ea::Refusal::Reach)));
    EXPECT_FALSE(Draw::Reached(uint8(Ea::Refusal::Gone)));
    EXPECT_FALSE(Draw::Reached(uint8(Ea::Refusal::Sight)));

    std::vector<uint64> const decoys{ 31 };
    Draw::PressFacts facts;
    facts.Target = 30;
    facts.Opener = 40;
    facts.Decoys = &decoys;
    facts.On = 31;
    facts.Press = uint8(Ea::Press::Interact);
    facts.Sent = false;
    facts.Reached = Draw::Reached(uint8(Ea::Refusal::Loot));
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Wrong);
    facts.Reached = Draw::Reached(uint8(Ea::Refusal::Reach));
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::None);
    // A refused press on the named object is no outcome: only one the world took.
    facts.On = 30;
    facts.Reached = true;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::None);
    // Price: a Cost of half the arrival, the same once for each decoy (the encounter's DecoyTaken).
    Cu::CurriculumTuning::InteractTuning const tuning;
    EXPECT_FLOAT_EQ(tuning.WrongObject, 0.5f);
}

// The named row is always there in a stage with the sight block (its size never depends on the stage), its presence
// column 0 where nothing is named.
TEST(InteractStageTest, TheNamedRowIsAlwaysInTheSightBlock)
{
    Cu::Layout layout;
    Cu::Block const& sight = Cu::GetBlock(Cu::BlockId::Sight);
    EXPECT_EQ(sight.Size(layout).Obs, Sight::OBS_COUNT);
    Cu::SeatView view;
    std::vector<float> row(Sight::NAMED_FEATURES, 9.0f);
    Sight::WriteNamed(view, row.data());
    for (float value : row)
        EXPECT_FLOAT_EQ(value, 0.0f);
}

// The goal names a kind, never a place: the named row is the kind's class, template and game-object column and the
// task, every other column 0 -- no distance, direction, centroid or pixels -- and the same wherever the seat stands
// and whatever it sees; with nothing named it is all 0.
TEST_F(InteractActionsTest, TheDistinguishGoalNamesATypeNotAPlace)
{
    Vi::EntityMemory memory;
    Vi::SeenList seen;
    seen.Camera = { 0.0f, 0.0f, 2.0f };
    std::array<uint64, Cu::SIGHT_SLOTS> guids{};
    ObjectGuid focus;
    Cu::SeatView view;
    view.Bot = _bot;
    view.Seen = &seen;
    view.Recall = &memory;
    view.SightGuids = &guids;
    view.Focus = &focus;
    view.NamedTask = uint8(Draw::Task::Reach);
    view.NamedClass = uint8(Vi::Class::OtherObject);
    view.NamedEntry = 179972;
    view.NamedObject = true;

    std::vector<float> obs(Sight::OBS_COUNT, -1.0f);
    Cu::GetBlock(Cu::BlockId::Sight).Observe(view, obs.data(), nullptr);
    float const* named = obs.data() + Sight::NAMED_FIRST;
    for (uint32 column = 0; column < Sight::NAMED_FEATURES; ++column)
    {
        float expected = 0.0f;
        if (column == Cu::EntitiesBlock::ENTITY_PRESENT || column == Cu::EntitiesBlock::ENTITY_OBJECT
            || column == Sight::NAMED_REACH)
            expected = 1.0f;
        else if (column == Cu::EntitiesBlock::ENTITY_CLASS)
            expected = float(uint32(Vi::Class::OtherObject));
        else if (column == Cu::EntitiesBlock::ENTITY_TYPE)
            expected = 179972.0f;
        EXPECT_FLOAT_EQ(named[column], expected) << column;
    }

    // Elsewhere, turned, with something in sight: the same row.
    std::vector<float> const first(named, named + Sight::NAMED_FEATURES);
    _bot->Relocate(40.0f, -25.0f, 3.0f, 2.0f);
    seen.Camera = { 40.0f, -25.0f, 5.0f };
    seen.Azimuth = 1.3f;
    Vi::EntityInfo info;
    info.Guid = 77;
    info.GameObject = true;
    info.Entry = 179972;
    info.Centre = { 45.0f, -25.0f, 3.5f };
    seen.Info[seen.Count++] = info;
    memory.Write(seen);
    Cu::GetBlock(Cu::BlockId::Sight).Observe(view, obs.data(), nullptr);
    EXPECT_EQ(std::vector<float>(obs.data() + Sight::NAMED_FIRST, obs.data() + Sight::OBS_COUNT), first);

    // The key rung's task, and nothing named at all.
    view.NamedTask = uint8(Draw::Task::UseItem);
    Cu::GetBlock(Cu::BlockId::Sight).Observe(view, obs.data(), nullptr);
    EXPECT_FLOAT_EQ(obs[Sight::NAMED_FIRST + Sight::NAMED_USE_ITEM], 1.0f);
    EXPECT_FLOAT_EQ(obs[Sight::NAMED_FIRST + Sight::NAMED_REACH], 0.0f);
    view.NamedTask = 0;
    Cu::GetBlock(Cu::BlockId::Sight).Observe(view, obs.data(), nullptr);
    for (uint32 column = Sight::NAMED_FIRST; column < Sight::OBS_COUNT; ++column)
        EXPECT_FLOAT_EQ(obs[column], 0.0f) << column;

    // No place goes with it: the encounter shows no objective flag and tells the goal block no place.
    Cu::SeatView fresh;
    EXPECT_FALSE(fresh.HasObjective);
    EXPECT_EQ(fresh.NamedTask, 0u);
}

// A lever pressed as the client presses it -- CMSG_GAMEOBJ_USE through the session's own handler -- is used (its
// loot state activated, its GO state active), and its link opens its door; the door itself, locked, refuses a hand
// before anything is sent; a reset puts both back as the map spawned them.
TEST_F(InteractActionsTest, ALeverPressThroughTheHandlerOpensItsDoor)
{
    TestGameObject* lever = Spawn(501, Template(990501, GAMEOBJECT_TYPE_BUTTON, 0, 3000), 2.0f, 0.0f);
    TestGameObject* door = Spawn(502, Template(990502, GAMEOBJECT_TYPE_DOOR, 85), 4.0f, 2.0f);
    Ea::Resolver const knows = Knows({ lever, door });
    ObjectGuid focus;

    // The door: locked, so no use is sent for it and it stays shut.
    Recorder port;
    Cu::SeatActionResult hand;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, door->GetGUID().GetRawValue(), focus, hand, port, knows),
        Ea::Refusal::Locked);
    EXPECT_EQ(hand.ActRefused, uint8(Ea::Refusal::Locked));
    EXPECT_EQ(hand.ActedOn, door->GetGUID());
    EXPECT_TRUE(port.Sent.empty());
    EXPECT_EQ(door->GetGoState(), GO_STATE_READY);

    // The lever, through the handler.
    Cu::SeatActionResult pulled;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, lever->GetGUID().GetRawValue(), focus, pulled, Ea::SessionPort(),
        knows), Ea::Refusal::None);
    EXPECT_EQ(pulled.Interactions, 1u);
    EXPECT_EQ(pulled.ActedOn, lever->GetGUID());
    EXPECT_EQ(pulled.ActPress, uint8(Ea::Press::Interact));
    EXPECT_EQ(lever->getLootState(), GO_ACTIVATED);
    EXPECT_EQ(lever->GetGoState(), GO_STATE_ACTIVE);
    EXPECT_EQ(door->GetGoState(), GO_STATE_READY);

    // Its link opens the door (the map's script does it in the Deadmines; this is the same step), once.
    EXPECT_TRUE(Cu::InteractEncounter::FollowLever(lever, door, _bot));
    EXPECT_EQ(door->GetGoState(), GO_STATE_ACTIVE);
    EXPECT_EQ(door->getLootState(), GO_ACTIVATED);
    EXPECT_FALSE(Cu::InteractEncounter::FollowLever(lever, door, _bot));

    // The press is judged the switch rung's way through.
    std::vector<uint64> const decoys;
    Draw::PressFacts facts;
    facts.Press = pulled.ActPress;
    facts.Sent = pulled.ActRefused == 0;
    facts.On = pulled.ActedOn.GetRawValue();
    facts.Target = 12345;
    facts.Opener = lever->GetGUID().GetRawValue();
    facts.Decoys = &decoys;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Opener);

    // A reset: both shut and ready, not in use.
    Cu::InteractEncounter::ResetObject(lever);
    Cu::InteractEncounter::ResetObject(door);
    for (GameObject* object : { static_cast<GameObject*>(lever), static_cast<GameObject*>(door) })
    {
        EXPECT_EQ(object->GetGoState(), GO_STATE_READY);
        EXPECT_EQ(object->getLootState(), GO_READY);
        EXPECT_FALSE(object->HasGameObjectFlag(GO_FLAG_IN_USE));
    }
    // An unused lever opens nothing.
    EXPECT_FALSE(Cu::InteractEncounter::FollowLever(lever, door, _bot));
}

// The key rung's lock (the cannon, a goober whose lock takes the gunpowder): a hand on it is refused (Locked) and
// nothing is sent; without the key item the item press is refused (NoItem); the press with it is CMSG_USE_ITEM aimed
// at the lock, and the encounter takes it as the key used -- the right object. (The item's spell 6250 opening lock 83
// and the cannon's script blowing the Iron Clad Door are the world's: the live preview's.)
TEST_F(InteractActionsTest, TheKeyItemUseOpensTheLock)
{
    TestGameObject* cannon = Spawn(503, Template(990503, GAMEOBJECT_TYPE_GOOBER, 83), 2.0f, 1.0f);
    Ea::Resolver const knows = Knows({ cannon });
    ObjectGuid focus;
    Recorder port;

    Cu::SeatActionResult hand;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, cannon->GetGUID().GetRawValue(), focus, hand, port, knows),
        Ea::Refusal::Locked);
    Cu::SeatActionResult empty;
    EXPECT_EQ(Ea::Apply(Ea::Press::UseItem, _bot, cannon->GetGUID().GetRawValue(), focus, empty, port, knows),
        Ea::Refusal::NoItem);
    EXPECT_EQ(empty.ActedOn, cannon->GetGUID());
    EXPECT_TRUE(port.Sent.empty());
    EXPECT_TRUE(Ea::LockedToHand(GAMEOBJECT_TYPE_GOOBER, 83));
    EXPECT_TRUE(Ea::LockedToHand(GAMEOBJECT_TYPE_DOOR, 85));
    EXPECT_FALSE(Ea::LockedToHand(GAMEOBJECT_TYPE_BUTTON, 0));
    EXPECT_FALSE(Ea::LockedToHand(GAMEOBJECT_TYPE_CHEST, 57));

    // The press with the gunpowder, as the client writes it: the item, its spell, the lock as the cast's target.
    SpellCastTargets targets;
    targets.SetGOTarget(cannon);
    ObjectGuid const item = ObjectGuid::Create<HighGuid::Item>(4242);
    WorldPacket packet = Ea::UseItem(INVENTORY_SLOT_BAG_0, 23, item, 6250, 3, targets);
    EXPECT_EQ(packet.GetOpcode(), CMSG_USE_ITEM);
    packet.rpos(0);
    uint8 bag = 0;
    uint8 slot = 0;
    uint8 castCount = 0;
    uint32 spellId = 0;
    ObjectGuid itemGuid;
    uint32 glyph = 0;
    uint8 flags = 0;
    packet >> bag >> slot >> castCount >> spellId >> itemGuid >> glyph >> flags;
    EXPECT_EQ(spellId, 6250u);
    EXPECT_EQ(itemGuid, item);
    SpellCastTargets read;
    read.Read(packet, _bot);
    EXPECT_EQ(read.GetGOTargetGUID(), cannon->GetGUID());

    // Sent, it is the key rung's outcome.
    std::vector<uint64> const decoys;
    Draw::PressFacts facts;
    facts.Press = uint8(Ea::Press::UseItem);
    facts.Sent = true;
    facts.On = cannon->GetGUID().GetRawValue();
    facts.Target = facts.On;
    facts.Opener = facts.On;
    facts.Asked = Draw::Task::UseItem;
    facts.Decoys = &decoys;
    EXPECT_EQ(Draw::Judge(facts), Draw::Verdict::Right);
}

// An open door is still drawn: the band its raised gate leaves at the top of its frame, of the door class -- a ray
// through the doorway at eye height passes under it, one at the lintel meets it.
TEST(InteractVisionTest, AnOpenDoorIsTheBandAtTheTopOfItsFrame)
{
    // The Deadmines' door (display 394): 2 yd thick along its facing, 9 yd wide, 7.44 yd tall, at the origin facing x.
    Vi::BoxShape closed;
    closed.Low[0] = -1.1f;
    closed.Low[1] = -4.41f;
    closed.Low[2] = 0.0f;
    closed.High[0] = 0.89f;
    closed.High[1] = 4.51f;
    closed.High[2] = 7.44f;
    closed.What = Vi::Class::Door;
    Vi::BoxShape const open = Vi::OpenDoorBox(closed);
    EXPECT_EQ(open.What, Vi::Class::Door);
    EXPECT_FLOAT_EQ(open.High[2], 7.44f);
    EXPECT_NEAR(open.Low[2], 7.44f * (1.0f - Vi::OPEN_DOOR_BAND), 1e-4f);

    float normalZ = 0.0f;
    Vi::Vec3 const eye{ -10.0f, 0.0f, 1.8f };
    Vi::Vec3 const ahead{ 1.0f, 0.0f, 0.0f };
    EXPECT_GE(Vi::RayBox(eye, ahead, 50.0f, closed, normalZ), 0.0f);
    EXPECT_LT(Vi::RayBox(eye, ahead, 50.0f, open, normalZ), 0.0f);
    Vi::Vec3 const lintel{ -10.0f, 0.0f, 7.2f };
    EXPECT_GE(Vi::RayBox(lintel, ahead, 50.0f, open, normalZ), 0.0f);

    // Looked up at from the eye, the seat sees the band (both casters cast boxes alike: the GPU diff gate holds them to
    // it, a quarter of its random boxes open doors).
    float const up = std::atan2(7.2f - 1.8f, 10.0f);
    Vi::Vec3 const toward{ std::cos(up), 0.0f, std::sin(up) };
    EXPECT_GE(Vi::RayBox(eye, toward, 50.0f, open, normalZ), 0.0f);
}
