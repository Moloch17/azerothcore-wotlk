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

/// Dungeon-curriculum I3 and C1-C3: perception-true combat inputs (the combat block, the sight list's combat columns,
/// the frame's enemy list), the combat stages' draws and definitions, the respawn at the entrance (I4's seam), and the
/// movement stages left as they were.

#include "IntegrationTestFixture.h"
#include "MapVisionWorld.h"
#include "Block.h"
#include "CombatBlock.h"
#include "CombatDraw.h"
#include "CurriculumTuning.h"
#include "EntranceRespawn.h"
#include "EntitiesBlock.h"
#include "EntityMemory.h"
#include "Layout.h"
#include "PartyFramesBlock.h"
#include "ObjectMgr.h"
#include "RewardLedger.h"
#include "SeatView.h"
#include "SightBlock.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "StageState.h"
#include "TestCreature.h"
#include "TestMap.h"
#include "ThreatManager.h"
#include "VisionCaster.h"
#include "WorldMock.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value_to.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
namespace Vi = Animus::Vision;
namespace Mv = Animus::Movement;
namespace Draw = Animus::Curriculum::CombatDraw;

using Combat = Cu::CombatBlock;
using Sight = Cu::SightBlock;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::_;

namespace
{
    /// Flat ground at z 0 over the grids round (32, 32), and solid boxes in the static tree: the walls.
    class WalledVision final : public Vi::VisionWorld
    {
    public:
        struct Wall
        {
            float X0, X1, Y0, Y1, Z0, Z1;
        };
        std::vector<Wall> Walls;

        Vi::SurfaceHit StaticHit(Vi::Vec3 from, Vi::Vec3 to) const override
        {
            Vi::SurfaceHit best;
            for (Wall const& wall : Walls)
            {
                float const hit = Slab(from, to, wall);
                if (hit >= 0.0f && (best.Distance < 0.0f || hit < best.Distance))
                {
                    best.Distance = hit;
                    best.NormalZ = 0.0f;
                }
            }
            return best;
        }
        Vi::SurfaceHit DynamicHit(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::LiquidHit ModelLiquid(Vi::Vec3, Vi::Vec3) const override { return {}; }
        Vi::TerrainTile Tile(int32_t tileX, int32_t tileY) const override
        {
            Vi::TerrainTile tile;
            tile.Loaded = tileX >= 31 && tileX <= 33 && tileY >= 31 && tileY <= 33;
            tile.Heights = tile.Loaded;
            tile.MaxHeight = 1.0e4f;
            return tile;
        }
        Vi::TerrainCell Cell(int32_t, int32_t, int32_t, int32_t, bool) const override
        {
            Vi::TerrainCell cell;
            cell.Solid = true;
            return cell;
        }
        Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        float FloorBelow(float, float, float z, float search) const override
        {
            return z >= 0.0f && z - search <= 0.0f ? 0.0f : Mv::INVALID_FLOOR;
        }

    private:
        static float Slab(Vi::Vec3 from, Vi::Vec3 to, Wall const& wall)
        {
            Vi::Vec3 const d = to - from;
            float lo = 0.0f;
            float hi = 1.0f;
            float const o[3] = { from.X, from.Y, from.Z };
            float const v[3] = { d.X, d.Y, d.Z };
            float const mins[3] = { wall.X0, wall.Y0, wall.Z0 };
            float const maxs[3] = { wall.X1, wall.Y1, wall.Z1 };
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::fabs(v[axis]) < 1e-9f)
                {
                    if (o[axis] < mins[axis] || o[axis] > maxs[axis])
                        return -1.0f;
                    continue;
                }
                float t0 = (mins[axis] - o[axis]) / v[axis];
                float t1 = (maxs[axis] - o[axis]) / v[axis];
                if (t0 > t1)
                    std::swap(t0, t1);
                lo = std::max(lo, t0);
                hi = std::min(hi, t1);
                if (lo > hi)
                    return -1.0f;
            }
            return lo * Vi::Length(d);
        }
    };

    /// A point well inside grid (32, 32).
    constexpr float X0 = -200.0f;
    constexpr float Y0 = -200.0f;

    Vi::EntityInfo Listed(uint64 guid, int8 reaction, bool dead = false, bool object = false)
    {
        Vi::EntityInfo info;
        info.Guid = guid;
        info.Reaction = reaction;
        info.Dead = dead;
        info.GameObject = object;
        info.Id.What = object ? Vi::Class::Door : reaction < 0 ? Vi::Class::HostileCreature
            : Vi::Class::FriendlyCreature;
        info.Level = 15.0f;
        return info;
    }

    class CombatPerceptionTest : public IntegrationTestFixture
    {
    protected:
        void SetUp() override
        {
            IntegrationTestFixture::SetUp();
            _bot = CreateTestPlayer();
            _bot->SetFaction(TEST_FACTION_HOSTILE_TO_MONSTERS);
            _bot->Relocate(X0, Y0, 0.0f, 0.0f);
        }

        /// Resolves the GUIDs of `units` and nothing else.
        static Combat::UnitResolver Knowing(std::vector<Unit*> units)
        {
            return [units](uint64 guid) -> Unit*
            {
                for (Unit* unit : units)
                    if (unit->GetGUID().GetRawValue() == guid)
                        return unit;
                return nullptr;
            };
        }

        TestPlayer* _bot = nullptr;
    };
}

// I3: the enemies a seat fights are the ones its camera shows. A creature in front is in the frame and in the list; one
// standing behind a wall -- there in the world, hostile, alive, within range -- is in neither, however near it is.
// Remembered, it is in the sight list's recalled half (its last sighting), never in the live enemy list.
TEST_F(CombatPerceptionTest, ACreatureBehindAWallIsNotListed)
{
    TestCreature* front = CreateTestCreature(101, 90101, TEST_FACTION_HOSTILE_TO_ALL);
    TestCreature* hidden = CreateTestCreature(102, 90102, TEST_FACTION_HOSTILE_TO_ALL);
    front->Relocate(X0 + 6.0f, Y0, 0.0f, 0.0f);
    hidden->Relocate(X0 + 15.0f, Y0, 0.0f, 0.0f);

    // The frame: the seat at (X0, Y0) facing +x, a wall across the corridor between the two creatures.
    WalledVision world;
    world.Walls.push_back({ X0 + 10.0f, X0 + 11.0f, Y0 - 8.0f, Y0 + 8.0f, -1.0f, 12.0f });
    std::vector<Vi::UnitShape> units = {
        { front->GetPositionX(), front->GetPositionY(), 0.0f, 0.5f, 2.0f, Vi::Class::HostileCreature, false, 1 },
        { hidden->GetPositionX(), hidden->GetPositionY(), 0.0f, 0.5f, 2.0f, Vi::Class::HostileCreature, false, 2 },
    };
    Vi::Sight const sight(units);
    Vi::Settings const settings;
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    Vi::CameraState camera;
    camera.Zoom = 0.0f;
    camera.Pitch = 0.0f;
    std::vector<uint8_t> image(Vi::ImageBytes(settings));
    std::array<float, Vi::SCALARS> scalars{};
    Vi::FrameSlots slots;
    Vi::Render(settings, pose, camera, world, sight, nullptr, image.data(), scalars.data(), nullptr,
        Vi::OBJECTIVE_RADIUS, &slots);
    ASSERT_EQ(slots.Count, 1u) << "the wall hides the second creature from every ray";
    ASSERT_EQ(slots.Slots[0].Entity, 1u);

    // The frame's list, as the vision block writes it.
    Vi::SeenList seen;
    seen.Count = slots.Count;
    seen.Camera = slots.Camera;
    seen.SeatLevel = 15.0f;
    seen.Info[0] = Listed(front->GetGUID().GetRawValue(), -1);
    seen.Info[0].Centre = { front->GetPositionX(), front->GetPositionY(), 1.0f };

    std::array<Unit*, Cu::PACK_SLOTS> enemies{};
    uint32 const count = Combat::VisibleEnemies(seen, Knowing({ front, hidden }), enemies.data(), Cu::PACK_SLOTS);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(enemies[0], front);
    EXPECT_TRUE(Combat::InView(seen, front->GetGUID().GetRawValue()));
    EXPECT_FALSE(Combat::InView(seen, hidden->GetGUID().GetRawValue()));

    // Seen a moment ago, then hidden: remembered in the sight list's recalled half, not in the live list.
    Vi::EntityMemory memory;
    Vi::SeenList before = seen;
    before.Count = 2;
    before.Info[1] = Listed(hidden->GetGUID().GetRawValue(), -1);
    before.Info[1].Centre = { hidden->GetPositionX(), hidden->GetPositionY(), 1.0f };
    memory.Write(before);
    memory.Advance(1.0f);
    memory.Write(seen);
    std::array<uint64, Cu::SIGHT_SLOTS> guids{};
    std::vector<float> obs(Cu::SIGHT_SLOTS * Sight::SIGHT_FEATURES);
    Sight::Write(seen, memory, 0, 0, obs.data(), guids);
    EXPECT_EQ(guids[0], front->GetGUID().GetRawValue());
    EXPECT_EQ(guids[Cu::SIGHT_VISIBLE_SLOTS], hidden->GetGUID().GetRawValue());
    EXPECT_FLOAT_EQ(obs[Cu::SIGHT_VISIBLE_SLOTS * Sight::SIGHT_FEATURES + Sight::SIGHT_VISIBLE], 0.0f);
    EXPECT_EQ(Combat::VisibleEnemies(seen, Knowing({ front, hidden }), enemies.data(), Cu::PACK_SLOTS), 1u);
}

// The live list is the frame's living hostile units in slot order: not a friend, a corpse, a game object, nor a unit
// the client no longer has; and never more than asked for.
TEST_F(CombatPerceptionTest, TheEnemiesAreTheFramesLivingHostiles)
{
    TestCreature* first = CreateTestCreature(111, 90111, TEST_FACTION_HOSTILE_TO_ALL);
    TestCreature* corpse = CreateTestCreature(112, 90112, TEST_FACTION_HOSTILE_TO_ALL);
    TestCreature* second = CreateTestCreature(113, 90113, TEST_FACTION_HOSTILE_TO_ALL);
    TestCreature* gone = CreateTestCreature(114, 90114, TEST_FACTION_HOSTILE_TO_ALL);
    corpse->SetAlive(false);

    Vi::SeenList seen;
    seen.Info[seen.Count++] = Listed(77, 1);                                    // a friend
    seen.Info[seen.Count++] = Listed(first->GetGUID().GetRawValue(), -1);
    seen.Info[seen.Count++] = Listed(corpse->GetGUID().GetRawValue(), -1);      // alive when the frame was cast
    seen.Info[seen.Count++] = Listed(78, 0, false, true);                       // a door
    seen.Info[seen.Count++] = Listed(second->GetGUID().GetRawValue(), -1);
    seen.Info[seen.Count++] = Listed(gone->GetGUID().GetRawValue(), -1);        // the client has it no more
    seen.Info[seen.Count++] = Listed(79, -1, true);                             // dead as the frame showed it

    std::array<Unit*, Cu::PACK_SLOTS> enemies{};
    uint32 const count = Combat::VisibleEnemies(seen, Knowing({ first, corpse, second }), enemies.data(),
        Cu::PACK_SLOTS);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(enemies[0], first);
    EXPECT_EQ(enemies[1], second);
    EXPECT_EQ(Combat::VisibleEnemies(seen, Knowing({ first, corpse, second }), enemies.data(), 1), 1u);
    EXPECT_EQ(Combat::VisibleEnemies(Vi::SeenList(), Knowing({ first }), enemies.data(), Cu::PACK_SLOTS), 0u);
}

// The player frame and the pet frame are always known: with no camera frame at all, the seat's own frame reads its
// health and power, and its select and focus presses are offered; an empty frame (no pet) is the only mask. The rest of
// the party's frames are not the combat block's (revision 1: they are the party frames block's).
TEST_F(CombatPerceptionTest, TheOwnFramesAreAlwaysKnown)
{
    _bot->SetMaxHealth(1000);
    _bot->SetHealth(250);
    Cu::SeatView view;
    view.Bot = _bot;
    view.Seen = nullptr;
    std::vector<float> obs(Combat::OBS_COUNT, -1.0f);
    std::vector<uint8> mask(Combat::ACTION_COUNT, 9);
    Cu::GetBlock(Cu::BlockId::Combat).Observe(view, obs.data(), mask.data());

    float const* self = obs.data() + Combat::OBS_FRAMES_FIRST + Combat::FRAME_SELF * Combat::FRAME_FEATURES;
    EXPECT_FLOAT_EQ(self[Combat::FRAME_PRESENT], 1.0f);
    EXPECT_FLOAT_EQ(self[Combat::FRAME_ALIVE], 1.0f);
    EXPECT_NEAR(self[Combat::FRAME_HEALTH], 0.25f, 1e-4f);
    EXPECT_FLOAT_EQ(self[Combat::FRAME_IN_RANGE], 1.0f);
    EXPECT_EQ(mask[Combat::ACTION_SELECT_FRAME_FIRST + Combat::FRAME_SELF], 1);
    EXPECT_EQ(mask[Combat::ACTION_FOCUS_FRAME_FIRST + Combat::FRAME_SELF], 1);
    for (uint32 frame = Combat::FRAME_PET; frame < Combat::OWN_FRAMES; ++frame)
    {
        EXPECT_FLOAT_EQ(obs[Combat::OBS_FRAMES_FIRST + frame * Combat::FRAME_FEATURES + Combat::FRAME_PRESENT], 0.0f);
        EXPECT_EQ(mask[Combat::ACTION_SELECT_FRAME_FIRST + frame], 0);
        EXPECT_EQ(mask[Combat::ACTION_FOCUS_FRAME_FIRST + frame], 0);
    }
    // No selection: the target frame is empty.
    for (uint32 column = 0; column < Combat::TARGET_FEATURES; ++column)
        EXPECT_FLOAT_EQ(obs[Combat::OBS_TARGET_FIRST + column], 0.0f);

    // The same with a frame that shows nothing: the frames do not need the camera.
    Vi::SeenList empty;
    view.Seen = &empty;
    Cu::GetBlock(Cu::BlockId::Combat).Observe(view, obs.data(), mask.data());
    EXPECT_NEAR(self[Combat::FRAME_HEALTH], 0.25f, 1e-4f);
    EXPECT_EQ(Combat::FrameUnits(_bot)[Combat::FRAME_SELF], _bot);
}

// **A party member's frame** (PartyFrames revision 2, FillFrame: the one place a member's frame is read): its health
// and power as the client shows them, whether it leads, in the frame's range, and its minimap dot within the radius --
// right and forward of the seat's facing; with no target, the target's columns are empty. Nothing about it needs the
// camera.
TEST_F(CombatPerceptionTest, AMembersFrameIsReadAsItsClientShowsIt)
{
    TestPlayer* member = CreateTestPlayer(2, "Member");
    member->SetFaction(TEST_FACTION_HOSTILE_TO_MONSTERS);
    member->Relocate(X0 + 20.0f, Y0 - 10.0f, 0.0f, 0.0f);
    member->SetMaxHealth(1000);
    member->SetHealth(600);

    Cu::SeatView::PartyFrame frame;
    frame.HasTarget = true;     // cleared by the fill
    Cu::PartyFramesBlock::FillFrame(frame, member, true, _bot, X0, Y0, 0.0f, 60.0f, ObjectGuid::Empty, nullptr);
    EXPECT_TRUE(frame.Present);
    EXPECT_EQ(frame.Guid, member->GetGUID());
    EXPECT_TRUE(frame.Leader);
    EXPECT_TRUE(frame.Alive);
    EXPECT_NEAR(frame.Health, 0.6f, 1e-4f);
    EXPECT_TRUE(frame.InRange);
    EXPECT_FALSE(frame.Selected);
    EXPECT_FALSE(frame.Focused);
    EXPECT_FALSE(frame.Aggro);
    EXPECT_TRUE(frame.DotShown);
    EXPECT_NEAR(frame.DotForward, 20.0f, 1e-3f);
    EXPECT_NEAR(frame.DotRight, 10.0f, 1e-3f);
    EXPECT_FALSE(frame.HasTarget);

    // Selected and focused by the seat; 50 yd off: past the frame's range, still on the minimap.
    _bot->SetSelection(member->GetGUID());
    member->Relocate(X0 + 50.0f, Y0, 0.0f, 0.0f);
    Cu::PartyFramesBlock::FillFrame(frame, member, false, _bot, X0, Y0, 0.0f, 60.0f, member->GetGUID(), nullptr);
    EXPECT_TRUE(frame.Selected);
    EXPECT_TRUE(frame.Focused);
    EXPECT_FALSE(frame.Leader);
    EXPECT_FALSE(frame.InRange);
    EXPECT_TRUE(frame.DotShown);
    // Past the minimap's radius: the frame, no dot.
    member->Relocate(X0 + 70.0f, Y0, 0.0f, 0.0f);
    Cu::PartyFramesBlock::FillFrame(frame, member, false, _bot, X0, Y0, 0.0f, 60.0f, ObjectGuid::Empty, nullptr);
    EXPECT_TRUE(frame.Present);
    EXPECT_FALSE(frame.DotShown);
    _bot->SetSelection(ObjectGuid::Empty);

    // No member: an empty frame.
    Cu::PartyFramesBlock::FillFrame(frame, nullptr, true, _bot, X0, Y0, 0.0f, 60.0f, ObjectGuid::Empty, nullptr);
    EXPECT_FALSE(frame.Present);
    EXPECT_TRUE(frame.Guid.IsEmpty());

    // Without a group, the frames from the group are empty (a solo combat stage has none to show).
    Cu::SeatView view;
    view.Bot = _bot;
    view.Frames[2].Present = true;
    Cu::PartyFramesBlock::FillFromGroup(view);
    for (Cu::SeatView::PartyFrame const& empty : view.Frames)
        EXPECT_FALSE(empty.Present);
}

// The client's threat status (UnitThreatSituation), as the target frame and the nameplates show it.
TEST(CombatThreatTest, TheThreatStatusIsTheClients)
{
    EXPECT_EQ(Combat::ThreatStatus(false, false, 0.0f, 100.0f, 100.0f), 0u);    // not on its list
    EXPECT_EQ(Combat::ThreatStatus(true, false, 50.0f, 100.0f, 100.0f), 0u);    // below whoever it is on
    EXPECT_EQ(Combat::ThreatStatus(true, false, 105.0f, 100.0f, 100.0f), 1u);   // above it: about to pull it
    EXPECT_EQ(Combat::ThreatStatus(true, true, 100.0f, 0.0f, 120.0f), 2u);      // tanking, someone above
    EXPECT_EQ(Combat::ThreatStatus(true, true, 100.0f, 0.0f, 100.0f), 2u);      // ... or level with it
    EXPECT_EQ(Combat::ThreatStatus(true, true, 100.0f, 0.0f, 50.0f), 3u);       // tanking, alone at the top
}

namespace
{
    class CombatThreatListTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _previousWorld = std::move(sWorld);
            _worldMock = new NiceMock<WorldMock>();
            ON_CALL(*_worldMock, getIntConfig(_)).WillByDefault(Return(0));
            ON_CALL(*_worldMock, getFloatConfig(_)).WillByDefault(Return(1.0f));
            ON_CALL(*_worldMock, getBoolConfig(_)).WillByDefault(Return(false));
            static std::string emptyString;
            ON_CALL(*_worldMock, GetDataPath()).WillByDefault(ReturnRef(emptyString));
            sWorld.reset(_worldMock);

            for (uint32 id : { 90201u, 90202u })
            {
                auto* faction = new FactionTemplateEntry{};
                faction->ID = id;
                faction->faction = id;
                faction->ourMask = id == 90201u ? 1 : 2;
                faction->hostileMask = id == 90201u ? 2 : 1;
                for (auto& e : faction->enemyFaction)
                    e = 0;
                for (auto& f : faction->friendFaction)
                    f = 0;
                sFactionTemplateStore.SetEntry(id, faction);
            }
            TestMap::EnsureDBC();
            _map = new TestMap();
            for (uint32 i = 0; i < 4; ++i)
            {
                _units[i] = new TestCreature();
                _units[i]->SetupForCombatTest(_map, 301 + i, 92301 + i);
                _units[i]->SetFaction(i == 0 ? 90201 : 90202);
            }
        }

        void TearDown() override
        {
            for (TestCreature* unit : _units)
                unit->CleanupCombatState();
            for (TestCreature* unit : _units)
                delete unit;
            delete _map;
            sWorld = std::move(_previousWorld);
        }

        std::unique_ptr<IWorld> _previousWorld;
        NiceMock<WorldMock>* _worldMock = nullptr;
        TestMap* _map = nullptr;
        std::array<TestCreature*, 4> _units{};
    };
}

// The target frame's threat indicator reads the creature's own list: the one it is on securely tanks (3); a seat
// above that threat but not yet its victim is about to pull it (1), which makes the victim's hold insecure (2); a seat
// not on the list is 0.
TEST_F(CombatThreatListTest, TheTargetFramesThreatReadsTheCreaturesList)
{
    TestCreature* enemy = _units[0];
    TestCreature* tank = _units[1];
    TestCreature* other = _units[2];
    TestCreature* stranger = _units[3];
    ThreatManager& threat = enemy->TestGetThreatMgr();
    threat.AddThreat(tank, 100.0f);
    threat.AddThreat(other, 50.0f);
    threat.UpdateVictimForTesting();
    ASSERT_EQ(threat.GetLastVictim(), tank);
    EXPECT_EQ(Combat::ThreatStatusOf(enemy, tank), 3u);
    EXPECT_EQ(Combat::ThreatStatusOf(enemy, other), 0u);
    EXPECT_EQ(Combat::ThreatStatusOf(enemy, stranger), 0u);

    // Over the tank's threat, under the 110% that takes it: still the tank's, now insecurely.
    threat.AddThreat(other, 55.0f);
    threat.UpdateVictimForTesting();
    ASSERT_EQ(threat.GetLastVictim(), tank);
    EXPECT_EQ(Combat::ThreatStatusOf(enemy, other), 1u);
    EXPECT_EQ(Combat::ThreatStatusOf(enemy, tank), 2u);
}

// In a layout with the combat block every sight slot carries the combat columns after its own, and the manifest names
// them for the learner; a layout without one (M3's) keeps the narrower list. The block's own size and presses.
TEST(CombatBlockLayoutTest, TheSightListCarriesTheCombatColumns)
{
    Cu::Layout plain;
    plain.Blocks = { Cu::BlockId::Core, Cu::BlockId::Move, Cu::BlockId::Vision, Cu::BlockId::Entities,
        Cu::BlockId::Sight };
    Cu::Layout combat = plain;
    combat.Blocks.push_back(Cu::BlockId::Combat);
    Cu::Block const& sight = Cu::GetBlock(Cu::BlockId::Sight);
    EXPECT_EQ(Sight::Width(plain), uint32(Sight::SIGHT_FEATURES));
    EXPECT_EQ(sight.Size(plain).Obs, uint32(Sight::OBS_COUNT));
    EXPECT_EQ(Sight::NamedFirst(plain), uint32(Sight::NAMED_FIRST));
    EXPECT_EQ(Sight::Width(combat), uint32(Sight::SIGHT_FEATURES) + uint32(Combat::COMBAT_SLOT_FEATURES));
    // The slots, widened, then the named row (revision 2) after them, present and 0 where nothing is named.
    EXPECT_EQ(Sight::NamedFirst(combat), Cu::SIGHT_SLOTS * Sight::Width(combat));
    EXPECT_EQ(sight.Size(combat).Obs, Sight::NamedFirst(combat) + uint32(Sight::NAMED_FEATURES));
    EXPECT_EQ(sight.Revision(), 2u);
    EXPECT_EQ(sight.Size(combat).Actions, sight.Size(plain).Actions);

    boost::json::object entry;
    sight.DescribeManifest(combat, entry);
    boost::json::object const& described = entry.at("sight").as_object();
    EXPECT_EQ(boost::json::value_to<uint64>(described.at("width")), uint64(Sight::Width(combat)));
    boost::json::array const& names = described.at("features").as_array();
    ASSERT_EQ(names.size(), std::size_t(Sight::Width(combat)));
    EXPECT_EQ(std::string(names[Sight::SIGHT_FEATURES].as_string()), "combat_casting");
    boost::json::object const& named = described.at("named").as_object();
    EXPECT_EQ(boost::json::value_to<uint64>(named.at("offset")), uint64(Sight::NamedFirst(combat)));
    EXPECT_EQ(std::string(names.back().as_string()), "combat_debuffs");

    Cu::Block const& block = Cu::GetBlock(Cu::BlockId::Combat);
    EXPECT_EQ(Cu::BlockName(Cu::BlockId::Combat), "combat");
    EXPECT_EQ(block.Size(combat).Obs, uint32(Combat::OBS_COUNT));
    EXPECT_EQ(block.Size(combat).Actions, 2 * Combat::OWN_FRAMES);
    EXPECT_EQ(block.ActionName(combat, Combat::ACTION_SELECT_FRAME_FIRST), "select_frame_self");
    EXPECT_EQ(block.ActionName(combat, Combat::ACTION_FOCUS_FRAME_FIRST + Combat::FRAME_PET), "focus_frame_pet");
    boost::json::array columns;
    block.DescribeColumns(combat, columns);
    EXPECT_EQ(columns.size(), std::size_t(Combat::OBS_COUNT));
}

// A visible unit's combat columns are what its nameplate shows; a dead one, or none, reads 0.
TEST_F(CombatPerceptionTest, ASlotsCombatColumnsAreItsNameplates)
{
    TestCreature* mob = CreateTestCreature(121, 90121, TEST_FACTION_HOSTILE_TO_ALL);
    std::array<float, Combat::COMBAT_SLOT_FEATURES> out{};
    out.fill(7.0f);
    Combat::WriteSlot(mob, _bot, nullptr, ObjectGuid::Empty, out.data());
    EXPECT_FLOAT_EQ(out[Combat::SLOT_CASTING], 0.0f);
    EXPECT_FLOAT_EQ(out[Combat::SLOT_ATTACKS_ME], 0.0f);
    EXPECT_FLOAT_EQ(out[Combat::SLOT_THREAT], 0.0f);
    mob->SetAlive(false);
    out.fill(7.0f);
    Combat::WriteSlot(mob, _bot, nullptr, ObjectGuid::Empty, out.data());
    for (float value : out)
        EXPECT_FLOAT_EQ(value, 0.0f);
    out.fill(7.0f);
    Combat::WriteSlot(nullptr, _bot, nullptr, ObjectGuid::Empty, out.data());
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FALSE(Combat::IsPartyOf(_bot, mob));
    EXPECT_FALSE(Combat::IsPartyOf(_bot, _bot));
}

// The rungs: C1 one creature climbing from two levels under; C2 a pack growing to four, a caster from rung 1, linked
// from 2; C3 packs that can kill, linked; an elite from rung 4 everywhere; levels never out of the game's range.
TEST(CombatDrawTest, TheRungsPulls)
{
    Cu::CurriculumTuning::CombatTuning const tuning;
    Draw::Pull const easy = Draw::PlanPull(Cu::CombatDrill::Fight, 0, tuning, false, true);
    EXPECT_EQ(easy.Size, 1u);
    EXPECT_EQ(easy.LevelOffset, -2);
    EXPECT_FALSE(easy.Caster);
    EXPECT_FALSE(easy.Elite);
    EXPECT_TRUE(Draw::PlanPull(Cu::CombatDrill::Fight, 1, tuning, false, true).Caster);
    EXPECT_FALSE(Draw::PlanPull(Cu::CombatDrill::Fight, 1, tuning, true, false).Caster);
    EXPECT_FALSE(Draw::PlanPull(Cu::CombatDrill::Fight, 3, tuning, true, true).Hazard);
    EXPECT_TRUE(Draw::PlanPull(Cu::CombatDrill::Fight, 4, tuning, false, false).Elite);

    Draw::Pull const pack0 = Draw::PlanPull(Cu::CombatDrill::Packs, 0, tuning, false, false);
    EXPECT_EQ(pack0.Size, 2u);
    EXPECT_FALSE(pack0.Linked);
    EXPECT_FALSE(pack0.Caster);
    EXPECT_TRUE(Draw::PlanPull(Cu::CombatDrill::Packs, 0, tuning, true, false).Hazard);
    EXPECT_TRUE(Draw::PlanPull(Cu::CombatDrill::Packs, 1, tuning, false, false).Caster);
    EXPECT_TRUE(Draw::PlanPull(Cu::CombatDrill::Packs, 2, tuning, false, false).Linked);
    for (uint32 tier = 0; tier <= tuning.MaxTier; ++tier)
    {
        uint32 const size = Draw::PlanPull(Cu::CombatDrill::Packs, tier, tuning, false, false).Size;
        EXPECT_GE(size, 2u);
        EXPECT_LE(size, 4u);
    }
    EXPECT_EQ(Draw::PlanPull(Cu::CombatDrill::Packs, tuning.MaxTier, tuning, false, false).Size, 4u);

    Draw::Pull const survive = Draw::PlanPull(Cu::CombatDrill::Survive, 0, tuning, false, false);
    EXPECT_TRUE(survive.Linked);
    EXPECT_GE(survive.Size, 3u);
    EXPECT_GT(survive.LevelOffset, Draw::PlanPull(Cu::CombatDrill::Fight, 0, tuning, false, false).LevelOffset);

    EXPECT_EQ(Draw::CreatureLevel(15, easy), 13u);
    EXPECT_EQ(Draw::CreatureLevel(1, easy), 1u);
    Draw::Pull high;
    high.LevelOffset = 10;
    EXPECT_EQ(Draw::CreatureLevel(80, high), 83u);
}

// Tier-scaled outcomes (the archived ladders' rule): an outcome times w, a death over it; won is something down and
// no death.
TEST(CombatDrawTest, OutcomesScaleWithTheRung)
{
    EXPECT_FLOAT_EQ(Draw::TierWeight(0.25f, 0), 1.0f);
    EXPECT_FLOAT_EQ(Draw::TierWeight(0.25f, 4), 2.0f);
    EXPECT_TRUE(Draw::Won(1, 0));
    EXPECT_FALSE(Draw::Won(0, 0));
    EXPECT_FALSE(Draw::Won(3, 1));
}

// Where a seat may start: the dungeon's spawn points a path from the entrance reaches within the walk, one per spacing.
TEST(CombatDrawTest, TheCorridorPointsAreReachableAndApart)
{
    std::vector<Draw::Point> const candidates = { { 0, 0, 0 }, { 3, 0, 0 }, { 50, 0, 0 }, { 400, 0, 0 },
        { 60, 0, 0 } };
    std::vector<Draw::Point> const kept = Draw::CorridorPoints(candidates, [](Draw::Point const& point)
    {
        return point.X == 60.0f ? -1.0f : point.X;     // no path to the last
    }, 220.0f, 8.0f);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_FLOAT_EQ(kept[0].X, 0.0f);
    EXPECT_FLOAT_EQ(kept[1].X, 50.0f);
}

// Away from the fight (Combat.Away, a Cost): every second dead; alive, the walk back from the entrance and a fighting
// pull left behind beyond the yards; never standing back before a pull is engaged. No term pays for coming back.
TEST(CombatRespawnTest, AwayIsDeadOrOffFromTheFight)
{
    EXPECT_TRUE(Draw::AwayCharged(false, false, false, 0.0f, 30.0f));
    EXPECT_TRUE(Draw::AwayCharged(true, true, false, 31.0f, 30.0f));
    EXPECT_FALSE(Draw::AwayCharged(true, true, false, 29.0f, 30.0f));
    EXPECT_TRUE(Draw::AwayCharged(true, false, true, 45.0f, 30.0f));
    EXPECT_FALSE(Draw::AwayCharged(true, false, false, 45.0f, 30.0f));
    EXPECT_EQ(Cu::RewardTermCategory(Cu::RewardTerm::Away), Cu::RewardCategory::Cost);
    EXPECT_EQ(Cu::RewardTermName(Cu::RewardTerm::Away), "away");
}

// I4's one implementation drives the combat stages' deaths: the clock says when to rise, after the delay, and when the
// risen seat is back within the yards of the fight.
TEST(CombatRespawnTest, TheClockRisesAfterTheDelayAndRejoinsAtTheFight)
{
    Cu::RespawnClock clock;
    EXPECT_EQ(clock.Note(1000, false, -1.0f, 10000, 15.0f), Cu::RespawnClock::Step::Died);
    EXPECT_EQ(clock.Note(10999, false, -1.0f, 10000, 15.0f), Cu::RespawnClock::Step::None);
    EXPECT_EQ(clock.Note(11000, false, -1.0f, 10000, 15.0f), Cu::RespawnClock::Step::Rise);
    clock.Risen(11000);
    EXPECT_EQ(clock.Note(20000, true, 80.0f, 10000, 15.0f), Cu::RespawnClock::Step::None);
    EXPECT_EQ(clock.Note(41000, true, 12.0f, 10000, 15.0f), Cu::RespawnClock::Step::Rejoined);
    EXPECT_FLOAT_EQ(clock.RejoinSeconds(), 30.0f);
}

// No server-side leak after a rise: the seat's last frame is forgotten, so the enemy list -- the frame's visible
// hostiles -- is empty until its camera casts a frame at the entrance, however many it saw where it died.
TEST_F(CombatPerceptionTest, ARisenSeatSeesNoEnemyUntilAFrameIsCast)
{
    TestCreature* mob = CreateTestCreature(131, 90131, TEST_FACTION_HOSTILE_TO_ALL);
    Cu::SeatState seat;
    seat.Seen.Count = 1;
    seat.Seen.Info[0] = Listed(mob->GetGUID().GetRawValue(), -1);
    seat.SightGuids[0] = mob->GetGUID().GetRawValue();
    std::array<Unit*, Cu::PACK_SLOTS> enemies{};
    ASSERT_EQ(Combat::VisibleEnemies(seat.Seen, Knowing({ mob }), enemies.data(), Cu::PACK_SLOTS), 1u);

    // Before anything else, whether or not the seat could be stood up (here no bot is: the fixture's map cannot move
    // one), the frame is forgotten.
    EXPECT_FALSE(Cu::RiseAtEntrance(nullptr, seat, Position(3.81f, -14.82f, -17.84f, 4.39f), 5000));
    EXPECT_EQ(seat.Seen.Count, 0u);
    EXPECT_EQ(seat.SightGuids[0], 0u);
    EXPECT_EQ(Combat::VisibleEnemies(seat.Seen, Knowing({ mob }), enemies.data(), Cu::PACK_SLOTS), 0u);
    EXPECT_FALSE(Combat::InView(seat.Seen, mob->GetGUID().GetRawValue()));
}

// The combat stages' own terms: their purposes Outcome (never faded), their prices Cost.
TEST(CombatStagesTest, TheirTermsAreOutcomeAndCost)
{
    using Cu::RewardTerm;
    for (RewardTerm term : { RewardTerm::Survived, RewardTerm::InterruptLanded, RewardTerm::Kill, RewardTerm::Clear })
        EXPECT_EQ(Cu::RewardTermCategory(term), Cu::RewardCategory::Outcome) << Cu::RewardTermName(term);
    for (RewardTerm term : { RewardTerm::Away, RewardTerm::Hurt, RewardTerm::FireHurt, RewardTerm::PullExtra,
        RewardTerm::Death, RewardTerm::StepCost, RewardTerm::TeammateDeath })
        EXPECT_EQ(Cu::RewardTermCategory(term), Cu::RewardCategory::Cost) << Cu::RewardTermName(term);
    EXPECT_EQ(Cu::RewardTermName(RewardTerm::InterruptLanded), "interrupt_landed");
    EXPECT_EQ(Cu::RewardTermName(RewardTerm::FireHurt), "fire_hurt");
    EXPECT_FALSE(Cu::PricesNoise(RewardTerm::Hurt));
}

// The three stages as defined: the chain from M2, their blocks (the sight list with the combat block after it; C3
// adds the gauntlet's rest), Ragefire Chasm at its level band, every arena a combat drill that brings a dead seat back
// at the entrance. The movement stages carry neither new block.
TEST(CombatStagesTest, TheStagesLayouts)
{
    using Id = Cu::BlockId;
    std::vector<Id> const fight = { Id::Core, Id::Move, Id::Duel, Id::Pet, Id::Vision, Id::Entities, Id::Map,
        Id::Sight, Id::Combat, Id::Goal };
    std::vector<Id> survive = fight;
    survive.insert(survive.begin() + 4, Id::Gauntlet);

    Cu::StageDefinition const* c1 = Cu::FindStage("combat1_fight");
    Cu::StageDefinition const* c2 = Cu::FindStage("combat2_packs");
    Cu::StageDefinition const* c3 = Cu::FindStage("combat3_survive");
    ASSERT_TRUE(c1 && c2 && c3) << "a combat stage was left out by the validation";
    EXPECT_TRUE(Cu::CurriculumProblems().empty());
    EXPECT_EQ(c1->Extends, "move3_interact");
    EXPECT_EQ(c2->Extends, "combat1_fight");
    EXPECT_EQ(c3->Extends, "combat2_packs");
    EXPECT_EQ(c1->Blocks, fight);
    EXPECT_EQ(c2->Blocks, fight);
    EXPECT_EQ(c3->Blocks, survive);

    std::vector<std::pair<Cu::StageDefinition const*, Cu::CombatDrill>> const drills = {
        { c1, Cu::CombatDrill::Fight }, { c2, Cu::CombatDrill::Packs }, { c3, Cu::CombatDrill::Survive } };
    for (auto const& [stage, drill] : drills)
    {
        EXPECT_EQ(stage->MapId, uint32(MAP_RAGEFIRE_CHASM)) << stage->Name;
        EXPECT_EQ(stage->FocusLevelFirst, 13) << stage->Name;
        EXPECT_EQ(stage->FocusLevelLast, 18) << stage->Name;
        EXPECT_EQ(stage->FocusChance, 100) << stage->Name;
        EXPECT_EQ(stage->Level, 0) << stage->Name;
        ASSERT_EQ(stage->SpawnPoints.size(), 1u) << stage->Name;
        EXPECT_NEAR(stage->SpawnPoints[0].GetPositionX(), 3.81f, 1e-3f);
        EXPECT_EQ(stage->SeatCount(), 1u) << stage->Name;
        for (Cu::ArenaDefinition const& arena : stage->Arenas)
        {
            EXPECT_EQ(arena.Against, Cu::Opposition::Combat) << arena.Name;
            EXPECT_EQ(arena.Combat, drill) << arena.Name;
            EXPECT_TRUE(arena.RespawnAtEntrance) << arena.Name << ": no stage ends at the first death";
            EXPECT_FALSE(arena.DeathRuns) << arena.Name << ": no graveyard, ghost or corpse run";
            EXPECT_GE(arena.EpisodeSeconds, 150u) << arena.Name;
            EXPECT_EQ(arena.Schedule, Cu::PullSchedule::None) << arena.Name;
        }
    }
    EXPECT_TRUE(std::any_of(c1->Arenas.begin(), c1->Arenas.end(), [](auto const& arena) { return arena.Ally; }));
    EXPECT_TRUE(std::any_of(c2->Arenas.begin(), c2->Arenas.end(), [](auto const& arena) { return arena.Hazards; }));

    // Each layout's sight list carries the combat columns.
    Cu::Layout layout;
    layout.Blocks = c1->Blocks;
    EXPECT_EQ(Sight::Width(layout), uint32(Sight::SIGHT_FEATURES) + uint32(Combat::COMBAT_SLOT_FEATURES));

    // A party member's state has one source (PartyFrames revision 2): no stage shows it beside the party or support
    // block's slots, and a combat stage with a party has the party frames.
    for (Cu::StageDefinition const& stage : Cu::CurriculumStages())
    {
        EXPECT_FALSE(stage.Has(Id::PartyFrames) && (stage.Has(Id::Party) || stage.Has(Id::Support))) << stage.Name;
        if (stage.Has(Id::Combat) && stage.SeatCount() > 1)
            EXPECT_TRUE(stage.Has(Id::PartyFrames)) << stage.Name;
    }

    // M1 and M2: neither the combat block nor the sight list (their layouts are pinned in SightBlockTest).
    for (char const* name : { "move1_controls", "move2_seek" })
    {
        Cu::StageDefinition const* stage = Cu::FindStage(name);
        ASSERT_TRUE(stage) << name;
        EXPECT_FALSE(stage->Has(Id::Combat)) << name;
        EXPECT_FALSE(stage->Has(Id::Sight)) << name;
        for (Cu::ArenaDefinition const& arena : stage->Arenas)
        {
            EXPECT_FALSE(arena.RespawnAtEntrance) << name;
            EXPECT_EQ(arena.Combat, Cu::CombatDrill::None) << name;
        }
    }
}

// Ground fire is seen (I3): a hazard is drawn as a flat disc of its class at its radius by the CPU caster (the device
// draws the same shape: VisionGpuTest's emulated frames), listed as an entity of the frame, and one behind a wall is
// not; the duel block's fire columns are read off the visible ones alone.
TEST_F(CombatPerceptionTest, GroundFireIsSeenAndReadFromTheFrame)
{
    WalledVision world;
    world.Walls.push_back({ X0 + 20.0f, X0 + 21.0f, Y0 - 10.0f, Y0 + 10.0f, -1.0f, 12.0f });
    std::vector<Vi::UnitShape> units = { Vi::HazardDisc(X0 + 8.0f, Y0, 0.0f, 3.0f, 1),
        Vi::HazardDisc(X0 + 30.0f, Y0, 0.0f, 3.0f, 2) };
    EXPECT_EQ(units[0].What, Vi::Class::GroundHazard);
    EXPECT_FLOAT_EQ(units[0].Height, Vi::HAZARD_THICKNESS);
    Vi::Sight const sight(units);
    Vi::Settings const settings;
    Vi::Pose pose;
    pose.X = X0;
    pose.Y = Y0;
    Vi::CameraState camera;
    camera.Zoom = 0.0f;
    camera.Pitch = -20.0f * Vi::DEGREES;
    std::vector<uint8_t> image(Vi::ImageBytes(settings));
    std::array<float, Vi::SCALARS> scalars{};
    Vi::FrameSlots slots;
    Vi::Render(settings, pose, camera, world, sight, nullptr, image.data(), scalars.data(), nullptr,
        Vi::OBJECTIVE_RADIUS, &slots);
    uint32 hazardPixels = 0;
    for (std::size_t pixel = 0; pixel < image.size() / Vi::BYTES_PER_PIXEL; ++pixel)
        hazardPixels += Vi::ClassOfByte(image[pixel * Vi::BYTES_PER_PIXEL + Vi::CLASS_BYTE]) == Vi::Class::GroundHazard;
    EXPECT_GT(hazardPixels, 0u);
    ASSERT_EQ(slots.Count, 1u) << "the disc behind the wall is not in the frame";
    EXPECT_EQ(slots.Slots[0].Entity, 1u);
    EXPECT_EQ(Vi::KindOf(Vi::Class::GroundHazard), Vi::Kind::Deadly);
    EXPECT_STREQ(Vi::CLASS_NAMES[uint32(Vi::Class::GroundHazard)], "ground_hazard");

    // The frame's list, as GatherSight fills it: a hazard is an object (never selected), hostile, with its radius.
    Vi::SeenList seen;
    seen.Count = 1;
    seen.Info[0].Id.What = Vi::Class::GroundHazard;
    seen.Info[0].GameObject = true;
    seen.Info[0].Reaction = -1;
    seen.Info[0].Guid = 4242;
    seen.Info[0].Radius = 3.0f;
    seen.Info[0].Centre = { X0 + 8.0f, Y0, 0.1f };
    std::array<Unit*, Cu::PACK_SLOTS> enemies{};
    EXPECT_EQ(Combat::VisibleEnemies(seen, Knowing({}), enemies.data(), Cu::PACK_SLOTS), 0u);

    // Standing outside it, facing it: the nearest, 5 yd to its edge, dead ahead; not standing in any.
    Combat::SeenHazards outside = Combat::ReadHazards(seen, X0, Y0, 0.0f);
    EXPECT_EQ(outside.Standing, 0u);
    ASSERT_TRUE(outside.Nearest.Present);
    EXPECT_NEAR(outside.Nearest.Distance - outside.Nearest.Radius, 5.0f, 1e-4f);
    EXPECT_NEAR(outside.Nearest.Bearing, 0.0f, 1e-4f);

    // Inside it, facing away: standing in one, the way out 2 yd, its centre behind.
    Combat::SeenHazards inside = Combat::ReadHazards(seen, X0 + 9.0f, Y0, 0.0f);
    EXPECT_EQ(inside.Standing, 1u);
    ASSERT_TRUE(inside.Deepest.Present);
    EXPECT_NEAR(inside.Deepest.Radius - inside.Deepest.Distance, 2.0f, 1e-4f);
    EXPECT_NEAR(std::fabs(inside.Deepest.Bearing), float(M_PI), 1e-4f);
    EXPECT_FALSE(inside.Nearest.Present);

    // Nothing seen, nothing known: the server's areas never reach it.
    Combat::SeenHazards const none = Combat::ReadHazards(Vi::SeenList(), X0 + 9.0f, Y0, 0.0f);
    EXPECT_EQ(none.Standing, 0u);
    EXPECT_FALSE(none.Nearest.Present);
    EXPECT_FALSE(Vi::HostileGround(nullptr, nullptr));
}

// Debuffs on an enemy are shown only as a player is shown them: the selection (target frame), the focus (focus frame)
// or one in the camera's frame (its nameplate); never every enemy's from the server.
TEST(CombatDebuffsTest, ShownOnlyForTheSelectionTheFocusOrTheVisible)
{
    Vi::SeenList seen;
    seen.Count = 1;
    seen.Info[0] = Listed(31, -1);
    EXPECT_TRUE(Combat::DebuffsShown(31, 0, 0, &seen));     // in view
    EXPECT_TRUE(Combat::DebuffsShown(32, 32, 0, &seen));    // the selection
    EXPECT_TRUE(Combat::DebuffsShown(33, 0, 33, &seen));    // the focus
    EXPECT_FALSE(Combat::DebuffsShown(34, 32, 33, &seen));  // a remembered or unseen enemy
    EXPECT_FALSE(Combat::DebuffsShown(34, 0, 0, nullptr));
    EXPECT_FALSE(Combat::DebuffsShown(0, 0, 0, &seen));
}
