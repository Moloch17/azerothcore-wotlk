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
#include "CastWatch.h"
#include "CurriculumTuning.h"
#include "EntitiesBlock.h"
#include "EntityActions.h"
#include "EntityMemory.h"
#include "Layout.h"
#include "SeatView.h"
#include "SightBlock.h"
#include "Spell.h"
#include "StageDefinition.h"
#include "StageState.h"
#include "gtest/gtest.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
namespace Ea = Animus::Curriculum::EntityActions;
namespace Vi = Animus::Vision;

namespace
{
    using Sight = Cu::SightBlock;

    /// Keeps every packet a press sends, instead of handing it to the session.
    class Recorder final : public Ea::ClientPort
    {
    public:
        void Send(Player* /*bot*/, WorldPacket& packet) override { Sent.push_back(packet); }
        std::vector<WorldPacket> Sent;
    };

    /// The GUID a packet of one GUID carries.
    ObjectGuid GuidIn(WorldPacket packet)
    {
        packet.rpos(0);
        ObjectGuid guid;
        packet >> guid;
        return guid;
    }

    Vi::EntityInfo Entity(uint64 guid, Vi::Vec3 at, bool object = false)
    {
        Vi::EntityInfo info;
        info.Guid = guid;
        info.Id.What = object ? Vi::Class::Door : Vi::Class::HostileCreature;
        info.GameObject = object;
        info.Entry = 1234;
        info.Centre = at;
        info.Level = 12.0f;
        info.Reaction = object ? 0 : -1;
        return info;
    }

    Vi::SeenList Frame(std::initializer_list<Vi::EntityInfo> entities)
    {
        Vi::SeenList seen;
        seen.Camera = { 0.0f, 0.0f, 2.0f };
        seen.SeatLevel = 12.0f;
        for (Vi::EntityInfo const& info : entities)
            seen.Info[seen.Count++] = info;
        return seen;
    }

    float Column(std::vector<float> const& obs, uint32 slot, uint32 column)
    {
        return obs[slot * Sight::SIGHT_FEATURES + column];
    }

    class SightBlockTest : public IntegrationTestFixture
    {
    protected:
        void SetUp() override
        {
            IntegrationTestFixture::SetUp();
            _bot = CreateTestPlayer();
            _bot->SetFaction(TEST_FACTION_HOSTILE_TO_MONSTERS);
            _bot->Relocate(0.0f, 0.0f, 0.0f, 0.0f);
        }

        /// A friendly npc with a gossip flag, `distance` yards off.
        TestCreature* Npc(ObjectGuid::LowType low, float distance)
        {
            TestCreature* npc = CreateTestCreature(low, 90000 + low, TEST_FACTION_HOSTILE_TO_MONSTERS);
            npc->Relocate(distance, 0.0f, 0.0f, 0.0f);
            npc->ReplaceAllNpcFlags(UNIT_NPC_FLAG_GOSSIP);
            return npc;
        }

        /// Finds only `units`, as the seat's client has them.
        Ea::Resolver Knows(std::vector<Unit*> units)
        {
            return [units](Player* /*bot*/, ObjectGuid guid) -> WorldObject*
            {
                for (Unit* unit : units)
                    if (unit->GetGUID() == guid)
                        return unit;
                return nullptr;
            };
        }

        TestPlayer* _bot = nullptr;
    };
}

// The block: the visible half and the remembered half of one list, the entity list's columns first, five pointer
// groups over it, and its manifest names them for the learner.
TEST(SightBlockLayoutTest, TheListAndItsPointers)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Sight);
    Cu::Layout layout;
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::Sight), "sight");
    EXPECT_EQ(Cu::SIGHT_SLOTS, 64u);
    EXPECT_EQ(block.Size(layout).Obs, Sight::OBS_COUNT);
    EXPECT_EQ(block.Size(layout).Actions, 5 * Cu::SIGHT_SLOTS);
    EXPECT_EQ(uint32(Sight::SIGHT_VISIBLE), uint32(Cu::EntitiesBlock::ENTITY_FEATURES));

    boost::json::object entry;
    block.DescribeManifest(layout, entry);
    boost::json::object const& sight = entry.at("sight").as_object();
    EXPECT_EQ(sight.at("slots").to_number<uint32>(), Cu::SIGHT_SLOTS);
    EXPECT_EQ(sight.at("visible_slots").to_number<uint32>(), Vi::ENTITY_SLOTS);
    EXPECT_EQ(sight.at("width").to_number<uint32>(), uint32(Sight::SIGHT_FEATURES));
    EXPECT_EQ(sight.at("features").as_array().size(), std::size_t(Sight::SIGHT_FEATURES));
    EXPECT_EQ(sight.at("memory_column").to_number<uint32>(), uint32(Cu::EntitiesBlock::ENTITY_MEMORY));
    boost::json::array const& pointers = sight.at("pointers").as_array();
    ASSERT_EQ(pointers.size(), 5u);
    std::vector<std::string> presses;
    for (auto const& pointer : pointers)
    {
        presses.emplace_back(pointer.as_object().at("press").as_string());
        EXPECT_EQ(pointer.as_object().at("count").to_number<uint32>(), Cu::SIGHT_SLOTS);
    }
    EXPECT_EQ(presses, (std::vector<std::string>{ "select", "interact", "use_item", "assist", "focus" }));

    EXPECT_EQ(block.ActionName(layout, Sight::ACTION_SELECT_FIRST + 3), "select_3");
    EXPECT_EQ(block.ActionName(layout, Sight::ACTION_INTERACT_FIRST + 40), "interact_40");
    EXPECT_EQ(block.ActionName(layout, Sight::ACTION_FOCUS_FIRST + 63), "focus_63");
}

// No loot path exists: no press of the block is a loot, and every press that would open a loot window -- a chest,
// a herb or ore node, a fishing node or hole, a corpse -- is refused before anything is sent.
TEST(SightBlockLayoutTest, NoPressLoots)
{
    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Sight);
    Cu::Layout layout;
    for (uint32 local = 0; local < Sight::ACTION_COUNT; ++local)
        EXPECT_EQ(block.ActionName(layout, local).find("loot"), std::string::npos) << local;

    for (uint32 type : { GAMEOBJECT_TYPE_CHEST, GAMEOBJECT_TYPE_FISHINGNODE, GAMEOBJECT_TYPE_FISHINGHOLE })
    {
        EXPECT_TRUE(Ea::OpensLoot(type));
        Ea::ObjectFacts facts;
        facts.Type = type;
        facts.Reach = 5.0f;
        EXPECT_EQ(Ea::JudgeObjectUse(facts), Ea::Refusal::Loot);
    }
    for (uint32 type : { GAMEOBJECT_TYPE_DOOR, GAMEOBJECT_TYPE_BUTTON, GAMEOBJECT_TYPE_GOOBER })
        EXPECT_FALSE(Ea::OpensLoot(type));
    Ea::UnitFacts corpse;
    corpse.Dead = true;
    corpse.NpcFlags = true;
    corpse.Reach = 5.5f;
    EXPECT_EQ(Ea::JudgeUnitInteract(corpse), Ea::Refusal::Loot);
}

// A game object's use is judged as its handler judges it: within its interaction distance, selectable.
TEST(SightBlockLayoutTest, AnObjectsUseIsJudgedAsTheHandlerDoes)
{
    Ea::ObjectFacts lever;
    lever.Type = GAMEOBJECT_TYPE_BUTTON;
    lever.Reach = 5.0f;
    lever.Distance = 3.0f;
    EXPECT_EQ(Ea::JudgeObjectUse(lever), Ea::Refusal::None);
    lever.Distance = 30.0f;
    EXPECT_EQ(Ea::JudgeObjectUse(lever), Ea::Refusal::Reach);
    lever.Distance = 3.0f;
    lever.Selectable = false;
    EXPECT_EQ(Ea::JudgeObjectUse(lever), Ea::Refusal::Kind);

    // The packet is the client's: CMSG_GAMEOBJ_USE and the object's GUID.
    ObjectGuid const door = ObjectGuid::Create<HighGuid::GameObject>(1731, 77);
    WorldPacket const packet = Ea::GameObjectUse(door);
    EXPECT_EQ(packet.GetOpcode(), CMSG_GAMEOBJ_USE);
    EXPECT_EQ(GuidIn(packet), door);
}

// A cast is sent as the client writes it, and a refusal sent back to the watched session is read back; another
// session's is not.
TEST(SightBlockLayoutTest, TheCastPacketAndItsRefusal)
{
    SpellCastTargets targets;
    WorldPacket cast = Ea::CastSpell(133, 7, targets);
    EXPECT_EQ(cast.GetOpcode(), CMSG_CAST_SPELL);
    cast.rpos(0);
    uint8 count = 0;
    uint32 spell = 0;
    uint8 flags = 1;
    uint32 mask = 1;
    cast >> count >> spell >> flags >> mask;
    EXPECT_EQ(count, 7u);
    EXPECT_EQ(spell, 133u);
    EXPECT_EQ(flags, 0u);
    EXPECT_EQ(mask, 0u);

    WorldSession const* session = reinterpret_cast<WorldSession const*>(uintptr_t(0x1000));
    WorldSession const* other = reinterpret_cast<WorldSession const*>(uintptr_t(0x2000));
    WorldPacket failed(SMSG_CAST_FAILED, 6);
    failed << uint8(7) << uint32(133) << uint8(SPELL_FAILED_OUT_OF_RANGE);
    {
        Animus::Movement::ScopedCastWatch watch(session);
        Animus::Movement::NoteCastFailed(other, failed);
        EXPECT_EQ(watch.Watch().Failures, 0u);
        Animus::Movement::NoteCastFailed(session, failed);
        EXPECT_EQ(watch.Watch().Failures, 1u);
        EXPECT_EQ(watch.Watch().Spell, 133u);
        EXPECT_EQ(watch.Watch().Result, uint8(SPELL_FAILED_OUT_OF_RANGE));
    }
    EXPECT_EQ(Animus::Movement::WatchedCast(), nullptr);
}

// The block's columns: the frame's entities first, in its order, then the remembered ones it does not show, each with
// its memory id (the entities block's reserved column too), visible now or how long ago, and the GUID a press acts on.
TEST(SightBlockLayoutTest, TheVisibleThenTheRemembered)
{
    Vi::EntityMemory memory;
    memory.Write(Frame({ Entity(11, { 10.0f, 0.0f, 2.0f }), Entity(12, { 0.0f, 10.0f, 2.0f }, true) }));
    memory.Advance(20.0f);
    Vi::SeenList const now = Frame({ Entity(13, { 5.0f, 5.0f, 2.0f }), Entity(11, { 12.0f, 0.0f, 2.0f }) });
    memory.Write(now);

    std::vector<float> obs(Cu::SIGHT_SLOTS * Sight::SIGHT_FEATURES, -1.0f);
    std::array<uint64, Cu::SIGHT_SLOTS> guids{};
    Sight::Write(now, memory, 11, 12, obs.data(), guids);

    // Visible: the frame's two, in its order.
    EXPECT_EQ(guids[0], 13u);
    EXPECT_EQ(guids[1], 11u);
    EXPECT_FLOAT_EQ(Column(obs, 0, Sight::SIGHT_VISIBLE), 1.0f);
    EXPECT_FLOAT_EQ(Column(obs, 1, Sight::SIGHT_AGE), 0.0f);
    EXPECT_FLOAT_EQ(Column(obs, 1, Cu::EntitiesBlock::ENTITY_MEMORY), float(memory.IdOf(11)));
    EXPECT_FLOAT_EQ(Column(obs, 1, Sight::SIGHT_SELECTED), 1.0f);
    EXPECT_FLOAT_EQ(Column(obs, 2, Cu::EntitiesBlock::ENTITY_PRESENT), 0.0f);

    // Remembered: the door it no longer sees, 20 s old, with no pixels.
    uint32 const recalled = Cu::SIGHT_VISIBLE_SLOTS;
    EXPECT_EQ(guids[recalled], 12u);
    EXPECT_EQ(guids[recalled + 1], 0u);
    EXPECT_FLOAT_EQ(Column(obs, recalled, Cu::EntitiesBlock::ENTITY_PRESENT), 1.0f);
    EXPECT_FLOAT_EQ(Column(obs, recalled, Cu::EntitiesBlock::ENTITY_OBJECT), 1.0f);
    EXPECT_FLOAT_EQ(Column(obs, recalled, Sight::SIGHT_VISIBLE), 0.0f);
    EXPECT_GT(Column(obs, recalled, Sight::SIGHT_AGE), 0.0f);
    EXPECT_FLOAT_EQ(Column(obs, recalled, Cu::EntitiesBlock::ENTITY_SHARE), 0.0f);
    EXPECT_FLOAT_EQ(Column(obs, recalled, Cu::EntitiesBlock::ENTITY_MEMORY), float(memory.IdOf(12)));
    EXPECT_FLOAT_EQ(Column(obs, recalled, Sight::SIGHT_FOCUSED), 1.0f);
    // Straight ahead along +y from the camera, the view's azimuth 0: 90 degrees to the left.
    EXPECT_NEAR(Column(obs, recalled, Cu::EntitiesBlock::ENTITY_YAW_SIN), 1.0f, 1e-4f);

    // The entities block reads the ids back into its reserved column; without a memory it stays 0.
    std::vector<float> list(Vi::ENTITY_SLOTS * Cu::EntitiesBlock::ENTITY_FEATURES, 0.0f);
    Cu::EntitiesBlock::Write(now, list.data(), &memory);
    EXPECT_FLOAT_EQ(list[Cu::EntitiesBlock::ENTITY_MEMORY], float(memory.IdOf(13)));
    EXPECT_NE(memory.IdOf(13), 0u);
    Cu::EntitiesBlock::Write(now, list.data());
    EXPECT_FLOAT_EQ(list[Cu::EntitiesBlock::ENTITY_MEMORY], 0.0f);
}

// Observed for a seat: a slot with nothing cannot be pressed, and a game object cannot be selected, assisted or
// focused; every other press is allowed -- interacting with a remembered entity far away included.
TEST_F(SightBlockTest, OnlyEmptySlotsAndUntargetableObjectsAreMasked)
{
    Vi::EntityMemory memory;
    Vi::SeenList seen = Frame({ Entity(21, { 50.0f, 0.0f, 2.0f }), Entity(22, { 0.0f, 80.0f, 2.0f }, true) });
    memory.Write(seen);
    std::array<uint64, Cu::SIGHT_SLOTS> guids{};
    ObjectGuid focus;
    Cu::SeatView view;
    view.Bot = _bot;
    view.Seen = &seen;
    view.Recall = &memory;
    view.SightGuids = &guids;
    view.Focus = &focus;
    std::vector<float> obs(Sight::OBS_COUNT);
    std::vector<uint8> mask(Sight::ACTION_COUNT, 9);
    Cu::GetBlock(Cu::BlockId::Sight).Observe(view, obs.data(), mask.data());

    EXPECT_EQ(mask[Sight::ACTION_SELECT_FIRST + 0], 1u);
    EXPECT_EQ(mask[Sight::ACTION_INTERACT_FIRST + 0], 1u);
    EXPECT_EQ(mask[Sight::ACTION_ASSIST_FIRST + 0], 1u);
    EXPECT_EQ(mask[Sight::ACTION_SELECT_FIRST + 1], 0u);
    EXPECT_EQ(mask[Sight::ACTION_FOCUS_FIRST + 1], 0u);
    EXPECT_EQ(mask[Sight::ACTION_INTERACT_FIRST + 1], 1u);
    EXPECT_EQ(mask[Sight::ACTION_USE_ITEM_FIRST + 1], 1u);
    for (uint32 slot = 2; slot < Cu::SIGHT_SLOTS; ++slot)
        for (uint32 press = 0; press < 5; ++press)
            EXPECT_EQ(mask[press * Cu::SIGHT_SLOTS + slot], 0u) << press << " " << slot;
}

// Select goes through the session's own CMSG_SET_SELECTION handler: the selection is what it set.
TEST_F(SightBlockTest, SelectGoesThroughTheHandler)
{
    TestCreature* npc = Npc(201, 30.0f);
    ObjectGuid focus;
    Cu::SeatActionResult result;
    Ea::Refusal const refusal = Ea::Apply(Ea::Press::Select, _bot, npc->GetGUID().GetRawValue(), focus, result,
        Ea::SessionPort(), Knows({ npc }));
    EXPECT_EQ(refusal, Ea::Refusal::None);
    EXPECT_EQ(_bot->GetTarget(), npc->GetGUID());
    EXPECT_EQ(result.Selections, 1u);
    EXPECT_EQ(result.ActRefused, 0u);
}

// Each press sends the client's packet for it: gossip for an npc, the member's target for assist, nothing for focus.
TEST_F(SightBlockTest, EachPressSendsItsPacket)
{
    TestCreature* npc = Npc(202, 3.0f);
    TestCreature* mob = CreateTestCreature(203, 90203, TEST_FACTION_HOSTILE_TO_ALL);
    npc->SetGuidValue(UNIT_FIELD_TARGET, mob->GetGUID());
    Ea::Resolver const knows = Knows({ npc, mob });
    ObjectGuid focus;
    Recorder port;

    Cu::SeatActionResult talked;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, npc->GetGUID().GetRawValue(), focus, talked, port, knows),
        Ea::Refusal::None);
    ASSERT_EQ(port.Sent.size(), 1u);
    EXPECT_EQ(port.Sent[0].GetOpcode(), CMSG_GOSSIP_HELLO);
    EXPECT_EQ(GuidIn(port.Sent[0]), npc->GetGUID());
    EXPECT_EQ(talked.Interactions, 1u);

    Cu::SeatActionResult assisted;
    EXPECT_EQ(Ea::Apply(Ea::Press::Assist, _bot, npc->GetGUID().GetRawValue(), focus, assisted, port, knows),
        Ea::Refusal::None);
    ASSERT_EQ(port.Sent.size(), 2u);
    EXPECT_EQ(port.Sent[1].GetOpcode(), CMSG_SET_SELECTION);
    EXPECT_EQ(GuidIn(port.Sent[1]), mob->GetGUID());

    Cu::SeatActionResult focused;
    EXPECT_EQ(Ea::Apply(Ea::Press::Focus, _bot, npc->GetGUID().GetRawValue(), focus, focused, port, knows),
        Ea::Refusal::None);
    EXPECT_EQ(focus, npc->GetGUID());
    EXPECT_EQ(port.Sent.size(), 2u);
}

// A press on a remembered entity is the world's to judge: out of reach, gone from the client, the wrong kind, no key
// item, a corpse -- each refused before any packet, recorded for its price, never masked.
TEST_F(SightBlockTest, OutOfReachAndGonePressesAreRefusedAndPriced)
{
    TestCreature* far = Npc(204, 30.0f);
    TestCreature* mob = CreateTestCreature(205, 90205, TEST_FACTION_HOSTILE_TO_ALL);
    mob->Relocate(2.0f, 0.0f, 0.0f, 0.0f);
    Ea::Resolver const knows = Knows({ far, mob });
    ObjectGuid focus;
    Recorder port;

    Cu::SeatActionResult reach;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, far->GetGUID().GetRawValue(), focus, reach, port, knows),
        Ea::Refusal::Reach);
    EXPECT_EQ(reach.ActRefused, uint8(Ea::Refusal::Reach));
    EXPECT_EQ(reach.Interactions, 0u);

    Cu::SeatActionResult gone;
    EXPECT_EQ(Ea::Apply(Ea::Press::Select, _bot, ObjectGuid::Create<HighGuid::Unit>(99999, 999).GetRawValue(), focus,
        gone, port, knows), Ea::Refusal::Gone);
    EXPECT_EQ(gone.ActRefused, uint8(Ea::Refusal::Gone));

    Cu::SeatActionResult hostile;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, mob->GetGUID().GetRawValue(), focus, hostile, port, knows),
        Ea::Refusal::Kind);

    Cu::SeatActionResult noItem;
    EXPECT_EQ(Ea::Apply(Ea::Press::UseItem, _bot, mob->GetGUID().GetRawValue(), focus, noItem, port, knows),
        Ea::Refusal::NoItem);

    Cu::SeatActionResult noTarget;
    EXPECT_EQ(Ea::Apply(Ea::Press::Assist, _bot, mob->GetGUID().GetRawValue(), focus, noTarget, port, knows),
        Ea::Refusal::NoTarget);

    mob->SetAlive(false);
    Cu::SeatActionResult loot;
    EXPECT_EQ(Ea::Apply(Ea::Press::Interact, _bot, mob->GetGUID().GetRawValue(), focus, loot, port, knows),
        Ea::Refusal::Loot);
    Cu::SeatActionResult lootItem;
    EXPECT_EQ(Ea::Apply(Ea::Press::UseItem, _bot, mob->GetGUID().GetRawValue(), focus, lootItem, port, knows),
        Ea::Refusal::Loot);

    // Nothing was sent, and no loot window was opened.
    EXPECT_TRUE(port.Sent.empty());
    EXPECT_TRUE(_bot->GetLootGUID().IsEmpty());

    // The refusal's price: its own aimless cause, at a price above nothing.
    EXPECT_STREQ(Cu::AimlessCauseName(Cu::AimlessCause::ActRefused), "act_refused");
    EXPECT_GT(Cu::CurriculumTuning::ActionTuning().AimlessActRefused, 0.0f);
}

// Training resumes where it left off after the rebuild: move1_controls' and move2_seek's layouts are as they were at
// forge 7a90f9b2c. Neither gets the sight block (only a stage that names it does, unlike the entity list after a
// camera), and every block they have keeps its columns, actions and revision; the entity list's memory-id column
// stays 0 without entity memory.
TEST(SightBlockLayoutTest, TheMovementStagesAreUnchanged)
{
    using Id = Cu::BlockId;
    struct Expected
    {
        Id Block;
        uint32 Obs;
        uint32 Actions;
        uint32 Revision;
    };
    std::vector<Expected> const m1 = { { Id::Move, 57, 25, 5 }, { Id::Compass, 6, 0, 1 }, { Id::Vision, 11, 0, 5 },
        { Id::Entities, 640, 0, 1 }, { Id::Goal, 128, 0, 0 } };
    std::vector<Expected> const m2 = { { Id::Move, 57, 25, 5 }, { Id::Vision, 11, 0, 5 },
        { Id::Entities, 640, 0, 1 }, { Id::Map, 4, 0, 1 }, { Id::Goal, 128, 0, 0 } };
    uint32 found = 0;
    for (Cu::StageDefinition const& stage : Cu::CurriculumStages())
    {
        std::vector<Expected> const* expected = stage.Name == "move1_controls" ? &m1
            : stage.Name == "move2_seek" ? &m2 : nullptr;
        if (!expected)
            continue;
        ++found;
        EXPECT_FALSE(stage.Has(Id::Sight)) << stage.Name;
        ASSERT_EQ(stage.Blocks.size(), expected->size() + 1) << stage.Name;
        EXPECT_EQ(stage.Blocks[0], Id::Core) << stage.Name;
        EXPECT_EQ(Cu::GetBlock(Id::Core).Revision(), 1u);
        Cu::Layout layout;
        for (std::size_t i = 0; i < expected->size(); ++i)
        {
            Expected const& want = (*expected)[i];
            ASSERT_EQ(stage.Blocks[i + 1], want.Block) << stage.Name << " block " << i + 1;
            Cu::Block const& block = Cu::GetBlock(want.Block);
            EXPECT_EQ(block.Size(layout).Obs, want.Obs) << stage.Name << " " << Cu::BlockName(want.Block);
            EXPECT_EQ(block.Size(layout).Actions, want.Actions) << stage.Name << " " << Cu::BlockName(want.Block);
            EXPECT_EQ(block.Revision(), want.Revision) << stage.Name << " " << Cu::BlockName(want.Block);
        }
    }
    EXPECT_EQ(found, 2u);

    // Without entity memory (no sight block, SeatView::Recall null) the list's memory-id column reads 0, as before.
    Vi::SeenList seen = Frame({ Entity(31, { 4.0f, 0.0f, 2.0f }) });
    Cu::SeatView view;
    view.Seen = &seen;
    std::vector<float> list(Vi::ENTITY_SLOTS * Cu::EntitiesBlock::ENTITY_FEATURES, 1.0f);
    Cu::GetBlock(Id::Entities).Observe(view, list.data(), nullptr);
    EXPECT_FLOAT_EQ(list[Cu::EntitiesBlock::ENTITY_PRESENT], 1.0f);
    EXPECT_FLOAT_EQ(list[Cu::EntitiesBlock::ENTITY_MEMORY], 0.0f);
}
