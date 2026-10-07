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
#include "Baselines.h"
#include "CurriculumTuning.h"
#include "EntityActions.h"
#include "EntranceRespawn.h"
#include "HintBlock.h"
#include "MoveControls.h"
#include "SightBlock.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "StageState.h"
#include "WingTeacher.h"
#include "gtest/gtest.h"
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace Cu = Animus::Curriculum;
namespace Ea = Animus::Curriculum::EntityActions;
namespace Wt = Animus::Curriculum::WingTeacher;
namespace MC = Animus::Curriculum::MoveControls;

/*
 * The dungeon teacher (dungeon-curriculum I6): its brain on hand-made situations, its press on a hand-made layout,
 * the handler packets its presses send, the hint it writes, the rise at the entrance it walks back from, and the
 * stand-in's hands.
 */

namespace
{
    /// A layout's action space as a wing stage lays it out: the no-op, then the move block, the sight block, the duel
    /// block and the gauntlet block.
    Wt::PressSpace Space()
    {
        Wt::PressSpace space;
        space.Move = { 1, MC::ACTION_COUNT };
        space.Sight = { space.Move.First + space.Move.Count, Cu::SightBlock::ACTION_COUNT };
        space.Duel = { space.Sight.First + space.Sight.Count, 10 };
        space.Gauntlet = { space.Duel.First + space.Duel.Count, 3 };
        return space;
    }

    constexpr uint32 SPELL_ACTION = 1000;      // what the spell pick answers, past every block above

    std::vector<uint8> AllAllowed()
    {
        return std::vector<uint8>(SPELL_ACTION + 1, 1);
    }

    /// Every spell kind castable, each its own action past the blocks.
    int32 AnySpell(Wt::Spell spell)
    {
        return int32(SPELL_ACTION - 20 + uint32(spell));
    }

    Wt::Place At(float yards, float bearing, int32 slot = -1)
    {
        Wt::Place place;
        place.Present = true;
        place.Yards = yards;
        place.Bearing = bearing;
        place.Slot = slot;
        return place;
    }

    /// A party of five at rest around the tank, everyone up: the facts of seat `role`.
    Wt::Facts Rested(Wt::Role role)
    {
        Wt::Facts facts;
        facts.Is = role;
        facts.Mana = role == Wt::Role::Healer ? 0.9f : -1.0f;
        facts.FoodLeft = true;
        facts.DrinkLeft = true;
        facts.PartyCount = 4;
        for (uint32 i = 0; i < 4; ++i)
        {
            Wt::Member& member = facts.Party[i];
            member.At = At(5.0f, 0.5f * float(i), int32(10 + i));
            member.Health = 1.0f;
            member.Mana = i == 1 ? 0.95f : -1.0f;
            member.Tank = role != Wt::Role::Tank && i == 0;
        }
        if (role != Wt::Role::Tank)
            facts.Tank = At(5.0f, 0.0f, 10);
        facts.Step = At(12.0f, 0.1f);
        return facts;
    }

    /// One enemy in the fight.
    Wt::Enemy Fighting(float yards, float bearing, int32 slot)
    {
        Wt::Enemy enemy;
        enemy.At = At(yards, bearing, slot);
        enemy.InFight = true;
        return enemy;
    }

    void AddEnemy(Wt::Facts& facts, Wt::Enemy const& enemy)
    {
        facts.Enemies[facts.EnemyCount++] = enemy;
    }

    bool Has(Wt::Choice const& choice, Wt::Do what, int32 slot = -2)
    {
        for (uint32 i = 0; i < choice.Count; ++i)
            if (choice.Options[i].What == what && (slot == -2 || choice.Options[i].Slot == slot))
                return true;
        return false;
    }

    bool Casts(Wt::Choice const& choice, Wt::Spell spell)
    {
        for (uint32 i = 0; i < choice.Count; ++i)
            if (choice.Options[i].What == Wt::Do::Cast && choice.Options[i].Cast == spell)
                return true;
        return false;
    }

    /// The keys a teacher may press: walk forward, let go of forward or a strafe, a turn rate. Never back, a strafe,
    /// a jump, a pitch, flight or the walk toggle -- and never anything that is not a key.
    bool AKeyItMayPress(uint32 key)
    {
        return key == MC::ACTION_MOVE_FORWARD || key == MC::ACTION_MOVE_STOP || key == MC::ACTION_STRAFE_STOP
            || MC::IsTurn(key);
    }

    /// Every situation the sweep below drives the teacher through: roles, fights and none, the way on near and far
    /// and to every side, keys held and not.
    std::vector<Wt::Facts> Situations()
    {
        std::vector<Wt::Facts> out;
        for (Wt::Role role : { Wt::Role::Tank, Wt::Role::Healer, Wt::Role::Damage })
            for (bool fight : { false, true })
                for (float bearing : { -2.5f, -0.9f, 0.0f, 0.2f, 1.2f, 3.0f })
                    for (float yards : { 1.0f, 6.0f, 20.0f, 45.0f })
                        for (int8 forward : { int8(0), int8(1) })
                            for (float turn : { 0.0f, 1.5707964f })
                            {
                                Wt::Facts facts = Rested(role);
                                facts.Forward = forward;
                                facts.TurnRate = turn;
                                facts.Step = At(yards, bearing);
                                if (role != Wt::Role::Tank)
                                    facts.Tank = At(yards, bearing, 10);
                                facts.Pull = At(yards, -bearing, yards < 30.0f ? 20 : -1);
                                facts.Object = At(yards, bearing * 0.5f, 21);
                                facts.Object.Present = yards < 10.0f;
                                facts.Health = yards < 5.0f ? 0.5f : 1.0f;
                                if (fight)
                                {
                                    Wt::Enemy enemy = Fighting(yards, bearing, yards < 30.0f ? 30 : -1);
                                    enemy.OnOther = bearing > 0.0f;
                                    enemy.OnTank = !enemy.OnOther;
                                    enemy.Selected = yards > 5.0f;
                                    enemy.TankTarget = true;
                                    AddEnemy(facts, enemy);
                                    facts.FightSeconds = 10.0f;
                                }
                                out.push_back(facts);
                            }
        return out;
    }

    class WingTeacherHandlerTest : public IntegrationTestFixture
    {
    protected:
        void SetUp() override
        {
            IntegrationTestFixture::SetUp();
            _bot = CreateTestPlayer();
            _bot->SetFaction(TEST_FACTION_HOSTILE_TO_MONSTERS);
            _bot->Relocate(0.0f, 0.0f, 0.0f, 0.0f);
        }

        /// Finds only `units`, as the seat's client has them.
        static Ea::Resolver Knows(std::vector<Unit*> units)
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

    /// Keeps every packet a press sends, instead of handing it to the session.
    class Recorder final : public Ea::ClientPort
    {
    public:
        void Send(Player* /*bot*/, WorldPacket& packet) override { Sent.push_back(packet); }
        std::vector<WorldPacket> Sent;
    };

    ObjectGuid GuidIn(WorldPacket packet)
    {
        packet.rpos(0);
        ObjectGuid guid;
        packet >> guid;
        return guid;
    }
}

// **It moves only by held keys** (the user: scripted movement through the controller, so the bots learn smooth
// movement). Over every situation of the sweep, every key it would press is a held key a player presses -- forward,
// let go, a turn rate -- and every press lands in the move block: there is no spline, no point order, no teleport to
// name. Nothing it chooses is outside the move, sight, duel (the swing), gauntlet (eat, drink) blocks and spells.
TEST(WingTeacherTest, ItMovesOnlyByHeldKeys)
{
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();
    uint32 keys = 0;
    for (Wt::Facts const& facts : Situations())
    {
        Wt::Choice const choice = Wt::Decide(facts);
        for (uint32 i = 0; i < choice.Count; ++i)
        {
            Wt::Option const& option = choice.Options[i];
            if (option.What != Wt::Do::Key)
                continue;
            ++keys;
            EXPECT_TRUE(AKeyItMayPress(option.Key)) << MC::NAMES[option.Key] << " (" << choice.Reason << ")";
        }
        int32 const press = Wt::Press(choice, space, mask.data(), AnySpell);
        EXPECT_NE(press, 0) << "the no-op is never a press (" << choice.Reason << ")";
        if (press <= 0)
            continue;
        uint32 const action = uint32(press);
        bool const move = action >= space.Move.First && action < space.Move.First + space.Move.Count;
        bool const sight = action >= space.Sight.First && action < space.Sight.First + space.Sight.Count;
        bool const swing = action == space.Duel.First + space.StartAttack;
        bool const rest = action == space.Gauntlet.First + space.Eat || action == space.Gauntlet.First + space.Drink;
        bool const spell = action >= SPELL_ACTION - 20 && action <= SPELL_ACTION;
        EXPECT_TRUE(move || sight || swing || rest || spell) << action << " (" << choice.Reason << ")";
        if (move)
            EXPECT_TRUE(AKeyItMayPress(action - space.Move.First));
    }
    EXPECT_GT(keys, 100u) << "the sweep never steered";

    // Steering is turning toward the way on and holding forward: a turn while off by more than the tolerance, forward
    // once nearly facing it, let go of within the stop distance.
    Wt::Facts facts;
    uint32 key = 0;
    ASSERT_TRUE(Wt::Steer(facts, 1.2f, 20.0f, 1.0f, key));
    EXPECT_TRUE(MC::IsTurn(key));
    EXPECT_GT(MC::TURN_RATES_DEG[key - MC::ACTION_TURN_FIRST], 0.0f) << "a bearing to the left turns left";
    ASSERT_TRUE(Wt::Steer(facts, 0.1f, 20.0f, 1.0f, key));
    EXPECT_EQ(key, uint32(MC::ACTION_MOVE_FORWARD));
    facts.Forward = 1;
    EXPECT_FALSE(Wt::Steer(facts, 0.1f, 20.0f, 1.0f, key)) << "already walking that way: nothing to press";
    ASSERT_TRUE(Wt::Steer(facts, 0.1f, 0.5f, 1.0f, key));
    EXPECT_EQ(key, uint32(MC::ACTION_MOVE_STOP));
}

// **It acts only through the handler paths**: a teacher's press on an entity is a sight-list press, and a sight-list
// press goes out as the client's packet through the session's handler -- a select, an assist, the swing. Its spells
// are only ever the spell pick's (Baselines::SpellFor, cast through CMSG_CAST_SPELL in a sight stage).
TEST_F(WingTeacherHandlerTest, ItActsOnlyThroughTheHandlerPaths)
{
    TestCreature* tank = CreateTestCreature(301, 90301, TEST_FACTION_HOSTILE_TO_MONSTERS);
    TestCreature* mob = CreateTestCreature(302, 90302, TEST_FACTION_HOSTILE_TO_ALL);
    tank->SetGuidValue(UNIT_FIELD_TARGET, mob->GetGUID());
    Ea::Resolver const knows = Knows({ tank, mob });
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();

    // Damage in a fight, the tank's target not selected yet: it assists the tank (its slot 2).
    Wt::Facts facts = Rested(Wt::Role::Damage);
    facts.Tank = At(4.0f, 0.0f, 2);
    Wt::Enemy enemy = Fighting(8.0f, 0.2f, 5);
    enemy.TankTarget = true;
    enemy.OnTank = true;
    AddEnemy(facts, enemy);
    Wt::Choice choice = Wt::Decide(facts);
    int32 press = Wt::Press(choice, space, mask.data(), AnySpell);
    ASSERT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_ASSIST_FIRST + 2)) << choice.Reason;
    uint32 const local = uint32(press) - space.Sight.First;
    EXPECT_EQ(Cu::SightBlock::PressOf(local), Ea::Press::Assist);
    EXPECT_EQ(Cu::SightBlock::SlotOf(local), 2u);
    Recorder port;
    ObjectGuid focus;
    Cu::SeatActionResult result;
    EXPECT_EQ(Ea::Apply(Ea::Press::Assist, _bot, tank->GetGUID().GetRawValue(), focus, result, port, knows),
        Ea::Refusal::None);
    ASSERT_EQ(port.Sent.size(), 1u);
    EXPECT_EQ(port.Sent[0].GetOpcode(), CMSG_SET_SELECTION);
    EXPECT_EQ(GuidIn(port.Sent[0]), mob->GetGUID());

    // The tank, something loose on a member: it selects it through the sight list.
    Wt::Facts tankFacts = Rested(Wt::Role::Tank);
    Wt::Enemy loose = Fighting(6.0f, 0.3f, 7);
    loose.OnOther = true;
    AddEnemy(tankFacts, loose);
    choice = Wt::Decide(tankFacts);
    press = Wt::Press(choice, space, mask.data(), AnySpell);
    ASSERT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_SELECT_FIRST + 7)) << choice.Reason;
    EXPECT_EQ(Ea::Apply(Ea::Press::Select, _bot, mob->GetGUID().GetRawValue(), focus, result, port, knows),
        Ea::Refusal::None);
    ASSERT_EQ(port.Sent.size(), 2u);
    EXPECT_EQ(port.Sent[1].GetOpcode(), CMSG_SET_SELECTION);
    EXPECT_EQ(GuidIn(port.Sent[1]), mob->GetGUID());

    // ... and once it holds it: the taunt, a spell -- the pick's, nothing else names a spell.
    tankFacts.Enemies[0].Selected = true;
    choice = Wt::Decide(tankFacts);
    EXPECT_TRUE(Casts(choice, Wt::Spell::Taunt)) << choice.Reason;
    press = Wt::Press(choice, space, mask.data(), AnySpell);
    EXPECT_EQ(press, AnySpell(Wt::Spell::Taunt));
    press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    EXPECT_NE(press, 0);
    EXPECT_TRUE(press < 0 || uint32(press) < space.Gauntlet.First + space.Gauntlet.Count)
        << "with no spell to cast it swings, or presses nothing: " << press;

    // The swing is the client's: CMSG_ATTACKSWING at the selection, through the handler.
    EXPECT_TRUE(Ea::StartAttackThroughClient(_bot, mob->GetGUID(), port));
    ASSERT_EQ(port.Sent.size(), 3u);
    EXPECT_EQ(port.Sent[2].GetOpcode(), CMSG_ATTACKSWING);
    EXPECT_EQ(GuidIn(port.Sent[2]), mob->GetGUID());
}

// **Doors, levers and the cannon are used with a real press** (no auto doors): out of reach it walks there on its keys;
// in reach it stops and interacts (CMSG_GAMEOBJ_USE) -- or uses the key item the lock takes (the cannon's gunpowder,
// CMSG_USE_ITEM); not seen yet, it turns to see it. Never a loot: the sight list's interact refuses a chest.
TEST(WingTeacherTest, ADoorOrLeverIsUsedWithARealInteract)
{
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();
    Wt::Facts facts = Rested(Wt::Role::Tank);
    facts.Object = At(3.0f, 0.2f, 7);
    facts.ObjectReach = 5.0f;

    Wt::Choice choice = Wt::Decide(facts);
    ASSERT_TRUE(Has(choice, Wt::Do::Interact, 7)) << choice.Reason;
    int32 press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    ASSERT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_INTERACT_FIRST + 7)) << choice.Reason;
    EXPECT_EQ(Cu::SightBlock::PressOf(uint32(press) - space.Sight.First), Ea::Press::Interact);
    // The press's packet: the object's use, as the client sends it.
    ObjectGuid const lever = ObjectGuid::Create<HighGuid::GameObject>(101834, 7);
    EXPECT_EQ(Ea::GameObjectUse(lever).GetOpcode(), CMSG_GAMEOBJ_USE);
    Ea::ObjectFacts judged;
    judged.Type = GAMEOBJECT_TYPE_BUTTON;
    judged.Reach = 5.0f;
    judged.Distance = 3.0f;
    EXPECT_EQ(Ea::JudgeObjectUse(judged), Ea::Refusal::None);
    judged.Type = GAMEOBJECT_TYPE_CHEST;
    EXPECT_EQ(Ea::JudgeObjectUse(judged), Ea::Refusal::Loot) << "a chest is never opened: no looting";

    // Walking in, it stops before it uses it.
    facts.Forward = 1;
    choice = Wt::Decide(facts);
    press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    EXPECT_EQ(press, int32(space.Move.First + MC::ACTION_MOVE_STOP));
    facts.Forward = 0;

    // The cannon: its key item, used on it.
    facts.ObjectNeedsKey = true;
    choice = Wt::Decide(facts);
    press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    EXPECT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_USE_ITEM_FIRST + 7)) << choice.Reason;
    facts.ObjectNeedsKey = false;

    // Out of reach: walked to, on its keys.
    facts.Object = At(15.0f, 0.1f, 7);
    choice = Wt::Decide(facts);
    EXPECT_FALSE(Has(choice, Wt::Do::Interact)) << choice.Reason;
    press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    EXPECT_EQ(press, int32(space.Move.First + MC::ACTION_MOVE_FORWARD));

    // Not in its sight list: it turns to it, rather than pressing what it cannot name.
    facts.Object = At(4.0f, 2.0f, -1);
    choice = Wt::Decide(facts);
    EXPECT_FALSE(Has(choice, Wt::Do::Interact)) << choice.Reason;
    press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    ASSERT_GT(press, 0);
    EXPECT_TRUE(MC::IsTurn(uint32(press) - space.Move.First));
}

// **The ready check holds the pull while a member is low** -- every member: health, mana (anyone with mana), alive
// (a dead one rises at the entrance and walks back), and near. The tank waits, pressing at most the key it lets go of;
// a minute with nothing gained and it pulls as it is.
TEST(WingTeacherTest, TheReadyCheckHoldsThePullWhileAMemberIsLow)
{
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();
    Wt::Facts ready = Rested(Wt::Role::Tank);
    ready.Pull = At(20.0f, 0.0f, 30);

    Wt::Choice choice = Wt::Decide(ready);
    ASSERT_TRUE(Has(choice, Wt::Do::Select, 30)) << "ready, the tank pulls: " << choice.Reason;

    auto const holds = [&](Wt::Facts const& facts, char const* why)
    {
        Wt::Choice const held = Wt::Decide(facts);
        EXPECT_TRUE(held.Waiting) << why;
        EXPECT_FALSE(Has(held, Wt::Do::Select)) << why << ": " << held.Reason;
        EXPECT_FALSE(Has(held, Wt::Do::Cast)) << why << ": " << held.Reason;
        EXPECT_NE(held.Reason.find("waits"), std::string::npos) << held.Reason;
        return held;
    };

    Wt::Facts low = ready;
    low.Party[2].Health = 0.5f;
    Wt::Choice const held = holds(low, "a member's health");
    EXPECT_NE(held.Reason.find("member 2 health"), std::string::npos) << held.Reason;
    EXPECT_EQ(Wt::Press(held, space, mask.data(), AnySpell), -1) << "standing still: no hint at all";

    Wt::Facts dry = ready;
    dry.Party[1].Mana = 0.3f;
    holds(dry, "a member's mana (any member with mana, not only the healer)");

    Wt::Facts dead = ready;
    dead.Party[3].Alive = false;
    holds(dead, "a dead member, rising at the entrance and walking back");

    Wt::Facts far = ready;
    far.Party[0].At.Yards = 60.0f;
    holds(far, "a member far behind");

    Wt::Facts self = ready;
    self.Health = 0.4f;
    self.FoodLeft = false;
    holds(self, "the tank itself");

    // Walking when the check fails: it lets go of forward -- the one press it hints.
    Wt::Facts walking = low;
    walking.Forward = 1;
    Wt::Choice const stop = Wt::Decide(walking);
    EXPECT_EQ(Wt::Press(stop, space, mask.data(), AnySpell), int32(space.Move.First + MC::ACTION_MOVE_STOP));

    // A minute of nothing: the valve, so a party with no drinks left does not stand for the hour.
    Wt::Facts waited = low;
    waited.StillSeconds = Wt::WAIT_SECONDS + 1.0f;
    choice = Wt::Decide(waited);
    EXPECT_TRUE(Has(choice, Wt::Do::Select, 30)) << choice.Reason;
    EXPECT_NE(choice.Reason.find("waited out"), std::string::npos) << choice.Reason;

    // ... but nothing waives a member who is not back: a dead one rising at the entrance, one walking back.
    for (Wt::Facts behind : { dead, far })
    {
        behind.StillSeconds = Wt::WAIT_SECONDS * 5.0f;
        Wt::Choice const waits = Wt::Decide(behind);
        EXPECT_TRUE(waits.Waiting) << waits.Reason;
        EXPECT_FALSE(Has(waits, Wt::Do::Select)) << "pulled without a member: " << waits.Reason;
    }

    std::string why;
    bool hard = false;
    EXPECT_TRUE(Wt::Ready(ready, why, hard)) << why;
    EXPECT_FALSE(Wt::Ready(low, why, hard));
    EXPECT_FALSE(hard) << "health is waivable";
    EXPECT_FALSE(Wt::Ready(dead, why, hard));
    EXPECT_TRUE(hard) << "a dead member is not";
}

// **An unseen pack is never walked at**: a pack ahead the tank's sight list does not name (a ledge, the lava, round a
// corner) is turned to at most, then the route goes on -- it passes every pack, and brings it into view from where it
// can be reached. The same for a door or lever not yet seen.
TEST(WingTeacherTest, AnUnseenPackIsNeverWalkedAt)
{
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();
    Wt::Facts facts = Rested(Wt::Role::Tank);
    facts.Step = At(10.0f, 0.0f);
    facts.Pull = At(15.0f, 2.0f, -1);
    Wt::Choice choice = Wt::Decide(facts);
    int32 press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    ASSERT_GT(press, 0);
    EXPECT_TRUE(MC::IsTurn(uint32(press) - space.Move.First)) << "turned to look: " << choice.Reason;
    // Facing it and still not seeing it: on along the route, not at the pack.
    facts.Pull.Bearing = 0.3f;
    facts.Step = At(10.0f, -1.0f);
    choice = Wt::Decide(facts);
    EXPECT_NE(choice.Reason.find("advances"), std::string::npos) << choice.Reason;
    press = Wt::Press(choice, space, mask.data(), [](Wt::Spell) { return -1; });
    ASSERT_GT(press, 0);
    uint32 const key = uint32(press) - space.Move.First;
    ASSERT_TRUE(MC::IsTurn(key));
    EXPECT_LT(MC::TURN_RATES_DEG[key - MC::ACTION_TURN_FIRST], 0.0f) << "toward the route step (right), not the pack";

    facts.Pull = Wt::Place();
    facts.Object = At(6.0f, 0.1f, -1);
    choice = Wt::Decide(facts);
    EXPECT_FALSE(Has(choice, Wt::Do::Interact)) << choice.Reason;
    EXPECT_NE(choice.Reason.find("advances"), std::string::npos) << choice.Reason;
}

// **A hint is never the no-op while waiting**: waiting for the party, for a cast, for food, for the tank to take the
// pull, the teacher hints the specific press it waits with (a key let go of) or nothing at all -- and the hint block
// carries nothing for nothing, while still saying the teacher played the seat.
TEST(WingTeacherTest, AHintIsNeverANoOpWhileWaiting)
{
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();

    std::vector<Wt::Facts> waits;
    Wt::Facts party = Rested(Wt::Role::Tank);
    party.Party[0].Health = 0.3f;
    waits.push_back(party);
    Wt::Facts casting = Rested(Wt::Role::Healer);
    casting.Casting = true;
    waits.push_back(casting);
    Wt::Facts eating = Rested(Wt::Role::Damage);
    eating.Eating = true;
    eating.Health = 0.5f;
    waits.push_back(eating);
    Wt::Facts letting = Rested(Wt::Role::Damage);
    Wt::Enemy coming = Fighting(20.0f, 0.0f, 4);
    coming.Selected = true;
    coming.TankTarget = true;
    AddEnemy(letting, coming);
    letting.FightSeconds = 1.0f;
    waits.push_back(letting);
    Wt::Facts watching = Rested(Wt::Role::Healer);
    AddEnemy(watching, Fighting(10.0f, 0.0f, 4));
    waits.push_back(watching);

    for (Wt::Facts facts : waits)
        for (int8 forward : { int8(0), int8(1) })
        {
            facts.Forward = forward;
            Wt::Choice const choice = Wt::Decide(facts);
            EXPECT_TRUE(choice.Waiting) << choice.Reason;
            int32 const press = Wt::Press(choice, space, mask.data(), AnySpell);
            EXPECT_NE(press, 0) << choice.Reason;
            if (press > 0)
                EXPECT_TRUE(uint32(press) >= space.Move.First && uint32(press) < space.Move.First + space.Move.Count)
                    << "a wait hints only the key it lets go of: " << choice.Reason;
        }

    std::array<float, Cu::HintBlock::OBS_COUNT> columns{};
    for (int32 nothing : { -1, 0 })
    {
        columns.fill(9.0f);
        Cu::Baselines::WriteHint(columns.data(), nothing, 1.0f, true);
        EXPECT_EQ(columns[Cu::HintBlock::OBS_ACTION], 0.0f) << "no hint for " << nothing;
        EXPECT_EQ(columns[Cu::HintBlock::OBS_WEIGHT], 0.0f);
        EXPECT_EQ(columns[Cu::HintBlock::OBS_SCRIPTED], 1.0f) << "the teacher still played the seat";
    }
    Cu::Baselines::WriteHint(columns.data(), 12, 0.75f, false);
    EXPECT_EQ(columns[Cu::HintBlock::OBS_ACTION], 13.0f);
    EXPECT_EQ(columns[Cu::HintBlock::OBS_WEIGHT], 0.75f);
    EXPECT_EQ(columns[Cu::HintBlock::OBS_SCRIPTED], 0.0f);
    // Imitation off (the cutoff): no hint even for a press.
    Cu::Baselines::WriteHint(columns.data(), 12, 0.0f, true);
    EXPECT_EQ(columns[Cu::HintBlock::OBS_ACTION], 0.0f);
    EXPECT_EQ(columns[Cu::HintBlock::OBS_WEIGHT], 0.0f);
}

// **The roles, by aptitude**: the healer focuses the most hurt member and heals it, standing; damage lets a fresh pull
// reach the tank, then fights the tank's target, stopping its casts; everyone eats and drinks between pulls.
TEST(WingTeacherTest, TheRolesPlayTheirParts)
{
    Wt::Facts healer = Rested(Wt::Role::Healer);
    healer.Party[2].Health = 0.4f;
    Wt::Choice choice = Wt::Decide(healer);
    EXPECT_TRUE(Has(choice, Wt::Do::Focus, 12)) << choice.Reason;
    healer.Party[2].Focused = true;
    choice = Wt::Decide(healer);
    EXPECT_TRUE(Casts(choice, Wt::Spell::Heal)) << choice.Reason;
    healer.Party[2].At.Yards = 50.0f;
    choice = Wt::Decide(healer);
    EXPECT_FALSE(Casts(choice, Wt::Spell::Heal)) << "out of reach, it walks in first: " << choice.Reason;

    Wt::Facts damage = Rested(Wt::Role::Damage);
    Wt::Enemy target = Fighting(3.0f, 0.0f, 6);
    target.Selected = true;
    target.TankTarget = true;
    target.OnTank = true;
    target.Interruptible = true;
    AddEnemy(damage, target);
    damage.FightSeconds = 5.0f;
    choice = Wt::Decide(damage);
    ASSERT_GT(choice.Count, 0u);
    EXPECT_TRUE(Casts(choice, Wt::Spell::Interrupt)) << choice.Reason;
    EXPECT_TRUE(Has(choice, Wt::Do::StartAttack)) << choice.Reason;
    EXPECT_TRUE(Casts(choice, Wt::Spell::Damage)) << choice.Reason;

    Wt::Facts hungry = Rested(Wt::Role::Damage);
    hungry.Health = 0.5f;
    choice = Wt::Decide(hungry);
    EXPECT_TRUE(Has(choice, Wt::Do::Eat)) << choice.Reason;
    Wt::Facts thirsty = Rested(Wt::Role::Healer);
    thirsty.Mana = 0.3f;
    choice = Wt::Decide(thirsty);
    EXPECT_TRUE(Has(choice, Wt::Do::Drink)) << choice.Reason;

    // The tank, several on it: threat on all of them at once.
    Wt::Facts tank = Rested(Wt::Role::Tank);
    for (int32 i = 0; i < 3; ++i)
    {
        Wt::Enemy enemy = Fighting(2.0f, 0.0f, 40 + i);
        enemy.OnTank = true;
        enemy.Selected = i == 0;
        AddEnemy(tank, enemy);
    }
    tank.Swinging = true;
    choice = Wt::Decide(tank);
    EXPECT_TRUE(Casts(choice, Wt::Spell::AreaThreat)) << choice.Reason;
    EXPECT_TRUE(Casts(choice, Wt::Spell::HighThreat)) << choice.Reason;
}

// **The rise at the entrance replaces the teleport rejoin** (I4 wired into the wing runs): a death in a fight is out
// for Respawn.DelayMs and rises then -- not held until the fight is over, as the old rejoin was -- and a wipe counts
// once, every seat rising by its own clock rather than the party stood up at the door together. The old keys are gone:
// no WingRiseMs, no auto doors.
TEST(WingTeacherTest, TheRiseAtTheEntranceReplacesTheTeleportRejoin)
{
    Cu::CurriculumTuning const tuning;
    std::set<std::string> keys;
    Cu::CurriculumTuning::Visit(tuning, [&keys](char const* key, auto const&) { keys.insert(key); });
    EXPECT_TRUE(keys.contains("Respawn.DelayMs"));
    EXPECT_TRUE(keys.contains("Respawn.RejoinYards"));
    EXPECT_FALSE(keys.contains("Instance.WingRiseMs")) << "the old rejoin's clock is gone";
    EXPECT_FALSE(keys.contains("Instance.WingAutoDoors")) << "no door opens by itself";
    EXPECT_TRUE(keys.contains("Instance.WingHintOffRung"));

    constexpr uint32 DECISION = 250;
    uint32 const delay = tuning.Respawn.DelayMs;
    std::array<Cu::RespawnClock, 5> clocks{};
    Cu::WipeLatch wipe;
    uint32 wipes = 0;
    std::array<uint32, 5> rose{};
    // Seat 4 dies in a fight at 10 s; the rest of the party dies at 30 s: a wipe.
    for (uint32 now = 0; now <= 60000; now += DECISION)
    {
        std::array<bool, 5> alive{};
        bool anyone = false;
        for (uint32 seat = 0; seat < 5; ++seat)
        {
            uint32 const diedAt = seat == 4 ? 10000 : 30000;
            alive[seat] = now < diedAt || (rose[seat] && now >= rose[seat]);
            anyone = anyone || alive[seat];
        }
        if (wipe.Note(anyone, false))
            ++wipes;
        for (uint32 seat = 0; seat < 5; ++seat)
        {
            Cu::RespawnClock::Step const step = clocks[seat].Note(now, alive[seat], 50.0f, delay,
                tuning.Respawn.RejoinYards);
            if (step == Cu::RespawnClock::Step::Rise)
            {
                clocks[seat].Risen(now);
                rose[seat] = now;
            }
        }
    }
    EXPECT_EQ(rose[4], 10000 + delay) << "a death in a fight rises after the delay, the fight going on";
    for (uint32 seat = 0; seat < 4; ++seat)
        EXPECT_EQ(rose[seat], 30000 + delay) << "seat " << seat;
    EXPECT_EQ(wipes, 0u) << "seat 4 had risen and stood when the others fell: no wipe";

    // A whole party down at once: one wipe, until somebody stands.
    Cu::WipeLatch latch;
    EXPECT_TRUE(latch.Note(false, false));
    EXPECT_FALSE(latch.Note(false, false));
    EXPECT_FALSE(latch.Note(true, false));
    EXPECT_TRUE(latch.Note(false, false));
    EXPECT_FALSE(latch.Note(false, true)) << "a cleared dungeon is no wipe";
}

// **The stand-in acts through the sight list** (I7's seam): its hands are the teacher's for its style's role, on the
// enemies its sight list names only -- a select, an assist, a focus and heal -- and never a key (its feet are its
// style's) nor a server list's target.
TEST(WingTeacherTest, TheStandInActsThroughSight)
{
    Wt::PressSpace const space = Space();
    std::vector<uint8> const mask = AllAllowed();
    Cu::SeatState seat;
    Wt::Facts& seen = seat.StandInSeen;
    seen = Rested(Wt::Role::Tank);
    seen.Tank = At(5.0f, 0.0f, 10);
    Wt::Enemy unseen = Fighting(3.0f, 0.0f, -1);
    unseen.OnOther = true;
    AddEnemy(seen, unseen);
    Wt::Enemy visible = Fighting(9.0f, 0.4f, 4);
    visible.TankTarget = true;
    AddEnemy(seen, visible);

    // Damage: the one it does not see is not its to target; the tank's target, assisted through the tank's slot.
    Wt::Facts facts = Cu::StageScenario::StandInFacts(seat, Cu::StandIn::Role::Damage);
    ASSERT_EQ(facts.EnemyCount, 1u);
    EXPECT_EQ(facts.Is, Wt::Role::Damage);
    Wt::Choice choice = Wt::Hands(facts);
    for (uint32 i = 0; i < choice.Count; ++i)
        EXPECT_NE(choice.Options[i].What, Wt::Do::Key) << "a stand-in's hands press no keys";
    int32 press = Wt::Press(choice, space, mask.data(), AnySpell);
    EXPECT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_ASSIST_FIRST + 10)) << choice.Reason;

    // Without the tank in its list: it selects the enemy itself.
    seat.StandInSeen.Tank.Slot = -1;
    choice = Wt::Hands(Cu::StageScenario::StandInFacts(seat, Cu::StandIn::Role::Damage));
    press = Wt::Press(choice, space, mask.data(), AnySpell);
    EXPECT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_SELECT_FIRST + 4)) << choice.Reason;

    // A healer stand-in: focus the hurt member, through the sight list.
    seat.StandInSeen.Party[1].Health = 0.3f;
    choice = Wt::Hands(Cu::StageScenario::StandInFacts(seat, Cu::StandIn::Role::Healer));
    press = Wt::Press(choice, space, mask.data(), AnySpell);
    EXPECT_EQ(press, int32(space.Sight.First + Cu::SightBlock::ACTION_FOCUS_FIRST + 11)) << choice.Reason;
}

// **The enforced cutoff's rule**: hint imitation ends once a full window of probes -- the policy alone -- clears more of
// the dungeon than the script's runs; never on a short window, never on a tie.
TEST(WingTeacherTest, HintImitationEndsOnceTheProbesBeatTheScript)
{
    std::vector<float> const script(40, 0.7f);
    EXPECT_FALSE(Cu::StageScenario::ProbesBeatTheScript(std::vector<float>(40, 0.6f), script, 40));
    EXPECT_FALSE(Cu::StageScenario::ProbesBeatTheScript(std::vector<float>(40, 0.7f), script, 40)) << "a tie";
    EXPECT_TRUE(Cu::StageScenario::ProbesBeatTheScript(std::vector<float>(40, 0.75f), script, 40));
    EXPECT_FALSE(Cu::StageScenario::ProbesBeatTheScript(std::vector<float>(39, 0.9f), script, 40))
        << "a window not yet full";
    EXPECT_FALSE(Cu::StageScenario::ProbesBeatTheScript(std::vector<float>(40, 0.9f), std::vector<float>(9, 0.1f),
        40)) << "the script's share measured on too few runs";
    EXPECT_TRUE(Cu::StageScenario::ProbesBeatTheScript(std::vector<float>(40, 0.9f), std::vector<float>(10, 0.1f),
        40));
    // The rung's hint weight from the cutoff on is 0 (StageScenario::WingHintAt); the ladder's rungs before it keep
    // theirs.
    EXPECT_GT(Cu::StageScenario::WING_RUNGS.front().Hint, 0.0f);
}

// **The teacher's check stages**: Ragefire and the Deadmines run whole by the teacher, on the sight block's presses,
// in no training queue.
TEST(WingTeacherTest, TheTeacherStagesAreDefined)
{
    for (char const* name : { "teacher_ragefire", "teacher_deadmines" })
    {
        Cu::StageDefinition const* stage = Cu::FindStage(name);
        ASSERT_NE(stage, nullptr) << name << " was left out";
        EXPECT_FALSE(stage->InDefaultQueue) << name;
        for (Cu::BlockId block : { Cu::BlockId::Move, Cu::BlockId::Vision, Cu::BlockId::Entities, Cu::BlockId::Sight,
            Cu::BlockId::Duel, Cu::BlockId::Gauntlet, Cu::BlockId::Pack, Cu::BlockId::Party, Cu::BlockId::Hint })
            EXPECT_TRUE(stage->Has(block)) << name << " " << Cu::BlockName(block);
        ASSERT_EQ(stage->Arenas.size(), 1u);
        Cu::ArenaDefinition const& arena = stage->Arenas.front();
        EXPECT_TRUE(arena.Teacher) << name;
        EXPECT_EQ(arena.Instance, Cu::InstanceLadder::Wing);
        EXPECT_EQ(arena.Seats, Cu::SeatPlan::Party);
        EXPECT_FALSE(arena.PullDrill);
    }
    EXPECT_EQ(Cu::FindStage("teacher_ragefire")->Arenas.front().InstanceRow, 0);
    EXPECT_EQ(Cu::FindStage("teacher_deadmines")->Arenas.front().InstanceRow, 1);
    for (std::string const& problem : Cu::CurriculumProblems())
        ADD_FAILURE() << problem;
}
