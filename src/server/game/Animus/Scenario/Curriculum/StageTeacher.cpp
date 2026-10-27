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

#include "StageScenario.h"
#include "DuelBlock.h"
#include "Encounters.h"
#include "Env.h"
#include "GameObject.h"
#include "GauntletBlock.h"
#include "HintBlock.h"
#include "MoveBlock.h"
#include "ObjectAccessor.h"
#include "PetBlock.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SeatView.h"
#include "SightBlock.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TeacherSpells.h"
#include "WingTeacher.h"
#include <cmath>

/*
 * **The teacher's adapter** (dm_clear, the user 2026-10-27: "a teacher again that slowly withdraws"; WingTeacher is the
 * brain): what a seat's teacher knows, read off the world on the map's thread as a script may read it, and its choice
 * turned into one of the seat's own layout actions and a heading -- which is the hint the learner imitates and, for a
 * decision the teacher plays, what the seat does (StageScenario::ApplyLook, ApplyContinuous, ApplyActions take it in
 * place of the policy's). Nothing here moves a seat, opens a door or casts: the press goes through ApplySeatAction like
 * a learned seat's.
 */

namespace
{
    using namespace Animus::Curriculum;
    namespace Wt = WingTeacher;

    float Wrap(float angle)
    {
        return std::remainder(angle, 2.0f * float(M_PI));
    }

    /// The sight list's slot naming `guid`, or -1 (neither seen nor remembered).
    int32 SightSlot(SeatState const& seat, ObjectGuid guid)
    {
        if (guid.IsEmpty())
            return -1;
        uint64 const raw = guid.GetRawValue();
        for (uint32 slot = 0; slot < SIGHT_SLOTS; ++slot)
            if (seat.SightGuids[slot] == raw)
                return int32(slot);
        return -1;
    }

    Wt::Place PlaceAt(Player const* bot, float x, float y, float z)
    {
        Wt::Place place;
        place.Present = true;
        place.Yards = bot->GetExactDist(x, y, z);
        place.Bearing = Wrap(std::atan2(y - bot->GetPositionY(), x - bot->GetPositionX()) - bot->GetOrientation());
        return place;
    }

    Wt::Place PlaceOf(Player const* bot, SeatState const& seat, WorldObject const* object)
    {
        if (!object || !object->IsInWorld() || !object->IsInMap(bot))
            return {};
        Wt::Place place = PlaceAt(bot, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
        place.Slot = SightSlot(seat, object->GetGUID());
        return place;
    }

    float ManaShare(Player const* bot)
    {
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        if (!maxMana || bot->getPowerType() != POWER_MANA)
            return -1.0f;
        return float(bot->GetPower(POWER_MANA)) / float(maxMana);
    }

    bool Interruptible(Unit const* unit)
    {
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* spell = unit->GetCurrentSpell(type))
                if (spell->GetSpellInfo() && (spell->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
                    return true;
        return false;
    }

    /// The next point, a few yards on, of the navmesh's way from the seat to `to`: the teacher's way to what is off
    /// the recorded line (a pack's straggler in another room, a tank gone after one). Its knowledge only -- the seat
    /// still walks it with its own keys. False without a way.
    bool PathStep(Player* bot, Position const& to, Position& step)
    {
        constexpr float PATH_STEP_YARDS = 3.0f;
        PathGenerator path(bot);
        if (!path.CalculatePath(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ())
            || (path.GetPathType() & PATHFIND_NOPATH))
            return false;
        Movement::PointsArray const& points = path.GetPath();
        for (G3D::Vector3 const& point : points)
            if (bot->GetExactDist2d(point.x, point.y) > PATH_STEP_YARDS)
            {
                step.Relocate(point.x, point.y, point.z);
                return true;
            }
        if (points.empty())
            return false;
        step.Relocate(points.back().x, points.back().y, points.back().z);
        return true;
    }

    SpecProfile const* SpecOf(SeatState const& seat)
    {
        return seat.L && seat.L->Profile && seat.Spec < seat.L->Profile->Specs.size()
            ? &seat.L->Profile->Specs[seat.Spec] : nullptr;
    }

    Wt::PressSpace SpaceOf(Layout const& layout)
    {
        static_assert(SightBlock::ACTION_INTERACT_FIRST == SIGHT_SLOTS
            && SightBlock::ACTION_USE_ITEM_FIRST == 2 * SIGHT_SLOTS
            && SightBlock::ACTION_ASSIST_FIRST == 3 * SIGHT_SLOTS
            && SightBlock::ACTION_FOCUS_FIRST == 4 * SIGHT_SLOTS
            && SightBlock::ACTION_CLEAR_FOCUS == 5 * SIGHT_SLOTS && SightBlock::ACTION_COUNT == 5 * SIGHT_SLOTS + 1,
            "WingTeacher::Press reads the sight block's five groups and its clear-focus press");
        auto const range = [&layout](BlockId block)
        {
            Wt::PressSpace::Range out;
            if (layout.Has(block))
            {
                out.First = layout.Slice(block).ActionFirst;
                out.Count = layout.Slice(block).ActionCount;
            }
            return out;
        };
        Wt::PressSpace space;
        space.Move = range(BlockId::Move);
        space.MoveBack = MoveBlock::Backpedals(layout);
        space.Sight = range(BlockId::Sight);
        space.Duel = range(BlockId::Duel);
        space.Gauntlet = range(BlockId::Gauntlet);
        space.StartAttack = DuelBlock::ACTION_START_ATTACK;
        space.PetAttack = DuelBlock::ACTION_PET_ATTACK;
        space.CallPet = DuelBlock::ACTION_CALL_BEAST_FIRST;
        space.Eat = GauntletBlock::ACTION_EAT;
        space.Drink = GauntletBlock::ACTION_DRINK;
        return space;
    }
}

bool Animus::Curriculum::StageScenario::TeacherOn() const
{
    return _tuning.Teacher.Enable && _stage.Teacher && _stage.Has(BlockId::Hint) && _instance;
}

void Animus::Curriculum::StageScenario::TeachSeat(Env& env, uint32 seatIndex, SeatView const& view, Player* bot,
    float* obs, uint8 const* mask)
{
    EnvState const& data = Data(env);
    SeatState& seat = Data(env).Seats[seatIndex];
    seat.TeacherAction = -1;
    seat.TeacherHasHeading = false;
    seat.TeacherHeading = 0.0f;
    seat.TeacherPlays = false;
    if (!TeacherOn() || !seat.L || !seat.L->Has(BlockId::Hint) || !bot || !bot->IsInWorld() || !mask)
        return;
    float const tau = _instance->TeacherTau(env);
    if (tau <= 0.0f)
    {
        seat.TeacherSegmentLeft = 0;
        return;
    }
    Map* map = env.FindMap();
    InstanceEncounter::TeacherWay const& way = _instance->Teacher(env, map);
    if (!way.Valid)
        return;

    // The facts, as a script reads the world.
    Wt::Facts facts;
    facts.Alive = bot->IsAlive();
    facts.Health = bot->GetMaxHealth() ? float(bot->GetHealth()) / float(bot->GetMaxHealth()) : 1.0f;
    facts.Mana = ManaShare(bot);
    facts.Casting = bot->IsNonMeleeSpellCast(false, false, true);
    facts.Swinging = bot->GetVictim() != nullptr;
    facts.Eating = bot->HasAuraType(SPELL_AURA_MOD_REGEN);
    facts.Drinking = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    auto const pressable = [&seat, mask](uint32 local)
    {
        return seat.L->Has(BlockId::Gauntlet) && local < seat.L->Slice(BlockId::Gauntlet).ActionCount
            && mask[seat.L->Slice(BlockId::Gauntlet).ActionFirst + local];
    };
    facts.FoodLeft = view.FoodItem && bot->HasItemCount(view.FoodItem, 1) && pressable(GauntletBlock::ACTION_EAT);
    facts.DrinkLeft = view.DrinkItem && bot->HasItemCount(view.DrinkItem, 1) && pressable(GauntletBlock::ACTION_DRINK);
    SpecProfile const* spec = SpecOf(seat);
    facts.Ranged = spec && spec->Range != RangeBand::Melee;
    facts.FightSeconds = seat.InCombat
        ? float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, seat.CombatStartMs)) / 1000.0f : 0.0f;
    facts.PetClass = seat.L->Profile && PetBlock::HasPet(seat.L->Profile->Class);
    if (Unit* pet = bot->GetFirstControlled(); pet && pet->IsAlive() && pet->IsInWorld())
    {
        facts.PetOut = true;
        facts.PetOnTarget = pet->GetVictim() && pet->GetVictim()->GetGUID() == bot->GetTarget();
    }
    facts.Forward = seat.Controls.Held.Forward;
    facts.Strafe = seat.Controls.Held.Strafe;

    // The party's tank (the dungeon role's, else the first living seat) and healer.
    Player* tank = nullptr;
    Player* healer = nullptr;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
        if (Player* member = SeatBotInWorld(env, index); member && member->IsAlive())
        {
            if (!tank && data.Seats[index].DungeonRole == DUNGEON_TANK)
                tank = member;
            if (!healer && data.Seats[index].DungeonRole == DUNGEON_HEALER)
                healer = member;
        }
    for (uint32 index = 0; index < data.ActiveSeats && !tank; ++index)
        if (Player* member = SeatBotInWorld(env, index); member && member->IsAlive())
            tank = member;
    facts.Is = bot == tank ? Wt::Role::Tank : bot == healer ? Wt::Role::Healer : Wt::Role::Damage;

    Unit const* focus = seat.Focus.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, seat.Focus);
    std::array<Player const*, MAX_SEATS> party{};
    uint32 partyCount = 0;
    for (uint32 index = 0; index < data.ActiveSeats; ++index)
    {
        Player* member = SeatBotInWorld(env, index);
        if (!member)
            continue;
        party[partyCount++] = member;
        if (index == seatIndex || facts.PartyCount >= Wt::PARTY_OTHERS)
            continue;
        Wt::Member& row = facts.Party[facts.PartyCount++];
        row.At = PlaceOf(bot, seat, member);
        row.Alive = member->IsAlive();
        row.Health = member->GetMaxHealth() ? float(member->GetHealth()) / float(member->GetMaxHealth()) : 1.0f;
        row.Mana = ManaShare(member);
        row.Tank = member == tank;
        row.Healer = member == healer;
        row.Focused = focus == member;
    }
    facts.FocusOnFriend = focus && focus->IsAlive() && std::find(party.begin(), party.begin() + partyCount, focus)
        != party.begin() + partyCount;
    if (tank && tank != bot)
        facts.Tank = PlaceOf(bot, seat, tank);
    auto const inParty = [&party, partyCount](Unit const* unit)
    {
        if (!unit)
            return false;
        ObjectGuid const owner = unit->GetOwnerGUID();
        for (uint32 i = 0; i < partyCount; ++i)
            if (party[i] == unit || (!owner.IsEmpty() && party[i]->GetGUID() == owner))
                return true;
        return false;
    };

    // The enemies the encounter keeps in the fight's slots, in the fight first, then nearest first.
    constexpr float ENEMY_YARDS = 60.0f;
    std::vector<Unit*> units;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* unit = env.FindTargetUnit(slot))
            units.push_back(unit);
    ObjectGuid const selection = bot->GetTarget();
    ObjectGuid const tankTarget = tank ? tank->GetTarget() : ObjectGuid::Empty;
    std::sort(units.begin(), units.end(), [bot, &inParty](Unit const* a, Unit const* b)
    {
        bool const fightA = a->IsInCombat() && inParty(a->GetVictim());
        bool const fightB = b->IsInCombat() && inParty(b->GetVictim());
        if (fightA != fightB)
            return fightA;
        return bot->GetExactDist(a) < bot->GetExactDist(b);
    });
    for (Unit* unit : units)
    {
        if (facts.EnemyCount >= Wt::ENEMIES)
            break;
        if (!unit->IsAlive() || !unit->IsInWorld() || !unit->IsInMap(bot) || unit->IsPlayer()
            || bot->GetExactDist(unit) > ENEMY_YARDS || !bot->IsValidAttackTarget(unit))
            continue;
        Unit const* victim = unit->GetVictim();
        bool const fighting = unit->IsInCombat() && inParty(victim);
        Wt::Enemy& enemy = facts.Enemies[facts.EnemyCount++];
        enemy.At = PlaceOf(bot, seat, unit);
        enemy.InFight = fighting;
        enemy.OnTank = fighting && victim == tank;
        enemy.OnSelf = fighting && victim == bot;
        enemy.OnOther = fighting && victim != tank;
        enemy.Selected = unit->GetGUID() == selection;
        enemy.TankTarget = !tankTarget.IsEmpty() && unit->GetGUID() == tankTarget;
        enemy.Interruptible = fighting && Interruptible(unit);
    }

    // The way: the next step along the recorded line, the pack ahead (the tank's), the object to open (its opener's).
    Position step;
    if (_instance->TeacherStep(env, bot, tank, bot == tank, step))
        facts.Step = PlaceAt(bot, step.GetPositionX(), step.GetPositionY(), step.GetPositionZ());
    // A member far from its tank goes the navmesh's way to it: the tank may be off the line after a straggler.
    constexpr float FOLLOW_PATH_YARDS = 15.0f;
    if (bot != tank && tank && bot->GetExactDist(tank) > FOLLOW_PATH_YARDS && PathStep(bot, tank->GetPosition(), step))
        facts.Step = PlaceAt(bot, step.GetPositionX(), step.GetPositionY(), step.GetPositionZ());
    if (bot == tank && !way.Pull.IsEmpty())
        if (Creature* pull = map ? map->GetCreature(way.Pull) : nullptr)
        {
            facts.Pull = PlaceOf(bot, seat, pull);
            facts.PullSelected = pull->GetGUID() == selection;
            if (facts.PullSelected)
                facts.HasRangedPull = TeacherSpellFor(Wt::Spell::RangedPull, *seat.L, spec, obs, mask).has_value();
            // At the line's end with the pack further on or out of sight (a straggler, a patrol, another room): the
            // navmesh's way to it (2026-10-27: legs 2, 26 and 30 ended with the pack's last members 37-72 yd off).
            if ((!facts.Step.Present || facts.Step.Yards < 2.0f)
                && (facts.Pull.Yards > Wt::PULL_YARDS || facts.Pull.Slot < 0))
            {
                if (PathStep(bot, pull->GetPosition(), step))
                    facts.Step = PlaceAt(bot, step.GetPositionX(), step.GetPositionY(), step.GetPositionZ());
                else
                    facts.Step = facts.Pull;
            }
        }
    if (!way.Object.IsEmpty() && way.Opener == bot->GetGUID())
        if (GameObject* object = map ? map->GetGameObject(way.Object) : nullptr)
        {
            facts.Object = PlaceOf(bot, seat, object);
            facts.ObjectOpens = way.ObjectOpens;
            facts.ObjectReach = object->GetInteractionDistance();
            Position along;
            if (facts.Object.Yards > Wt::OBJECT_DIRECT_YARDS
                && _instance->TeacherStepTo(env, bot, way.ObjectGate, along))
                facts.ObjectStep = PlaceAt(bot, along.GetPositionX(), along.GetPositionY(), along.GetPositionZ());
        }
    facts.StillSeconds = _instance->NoProgressSeconds(env);

    // The choice, as one of the seat's own actions and a heading.
    Wt::Choice const choice = Wt::Decide(facts);
    int32 const press = Wt::Press(choice, SpaceOf(*seat.L), mask, [&](Wt::Spell spell)
    {
        return TeacherSpellFor(spell, *seat.L, spec, obs, mask).value_or(-1);
    });
    seat.TeacherAction = press;
    seat.TeacherHasHeading = choice.HasHeading && facts.Alive;
    seat.TeacherHeading = choice.HasHeading ? Wrap(choice.Heading) : 0.0f;
    seat.TeacherReason = choice.Reason;
    // Who plays the decision: a segment of SegmentDecisions decisions at a time, the teacher's with probability tau.
    // A seat the teacher played that is still casting stays the teacher's until the cast ends: handed back mid-cast,
    // the policy's keys broke it (2026-10-27: the gunpowder's Fire Cannon, a cast, never once reached the cannon).
    if (!seat.TeacherSegmentLeft && !(seat.TeacherSegmentPlays && facts.Casting))
    {
        seat.TeacherSegmentLeft = std::max<uint32>(1, _tuning.Teacher.SegmentDecisions);
        seat.TeacherSegmentPlays = frand(0.0f, 1.0f) < tau;
    }
    if (!seat.TeacherSegmentLeft)
        seat.TeacherSegmentLeft = 1;
    --seat.TeacherSegmentLeft;
    seat.TeacherPlays = seat.TeacherSegmentPlays && facts.Alive;
    ++seat.TeacherDecisions;
    seat.TeacherPlayed += seat.TeacherPlays ? 1 : 0;
    seat.TeacherHinted += press > 0 ? 1 : 0;

    // The hint block: the press and its weight only for a press (nothing is never a hint: a policy taught a script's
    // no-ops stood still on its own, 2026-10-01), the heading where the teacher heads somewhere, and whether it played.
    float* hint = obs + seat.L->Slice(BlockId::Hint).ObsFirst;
    hint[HintBlock::OBS_ACTION] = press > 0 ? float(press + 1) : 0.0f;
    hint[HintBlock::OBS_WEIGHT] = press > 0 ? tau : 0.0f;
    hint[HintBlock::OBS_SCRIPTED] = seat.TeacherPlays ? 1.0f : 0.0f;
    hint[HintBlock::OBS_HEADING] = seat.TeacherHasHeading ? seat.TeacherHeading : 0.0f;
    hint[HintBlock::OBS_HEADING_WEIGHT] = seat.TeacherHasHeading ? tau : 0.0f;
}
