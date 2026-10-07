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
#include "Baselines.h"
#include "CrowdBlock.h"
#include "DuelBlock.h"
#include "Env.h"
#include "GameObject.h"
#include "GauntletBlock.h"
#include "HintBlock.h"
#include "MoveControls.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "SightBlock.h"
#include "Spell.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include <cmath>

/*
 * The dungeon teacher's adapter (dungeon-curriculum I6; the brain is WingTeacher): what a seat's teacher knows, read
 * off the world on the map's thread as a script may read it, and its choice turned into one of the seat's own layout
 * actions -- a held key, a sight-list press, a spell, eat or drink, the auto attack -- which is both the hint the
 * learner imitates and, for a seat the teacher plays, the press the seat makes. Nothing here moves a seat, opens a
 * door or casts: the press goes through ApplySeatAction like a learned seat's.
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

    bool CanTank(SeatState const& seat)
    {
        return seat.DungeonRole == DUNGEON_TANK
            || (seat.DungeonRole == DUNGEON_ANY && AptitudeDemand::HoldsThePull().MetBy(seat.Apt));
    }

    bool CanHeal(SeatState const& seat)
    {
        return seat.DungeonRole == DUNGEON_HEALER
            || (seat.DungeonRole == DUNGEON_ANY && AptitudeDemand::KeepsThemUp().MetBy(seat.Apt));
    }

    /// Casting something an interrupt stops.
    bool Interruptible(Unit const* unit)
    {
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* spell = unit->GetCurrentSpell(type))
                if (spell->GetSpellInfo() && (spell->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
                    return true;
        return false;
    }
}

int32 Animus::Curriculum::Baselines::TeacherPress(WingTeacher::Choice const& choice, Layout const& layout,
    float const* obs, uint8 const* mask)
{
    static_assert(SightBlock::ACTION_INTERACT_FIRST == SIGHT_SLOTS && SightBlock::ACTION_USE_ITEM_FIRST == 2 * SIGHT_SLOTS
        && SightBlock::ACTION_ASSIST_FIRST == 3 * SIGHT_SLOTS && SightBlock::ACTION_FOCUS_FIRST == 4 * SIGHT_SLOTS
        && SightBlock::ACTION_COUNT == 5 * SIGHT_SLOTS, "WingTeacher::Press reads the sight block's five groups");
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
    space.Sight = range(BlockId::Sight);
    space.Duel = range(BlockId::Duel);
    space.Gauntlet = range(BlockId::Gauntlet);
    space.StartAttack = DuelBlock::ACTION_START_ATTACK;
    space.Eat = GauntletBlock::ACTION_EAT;
    space.Drink = GauntletBlock::ACTION_DRINK;
    return Wt::Press(choice, space, mask, [&layout, obs, mask](Wt::Spell spell)
    {
        return SpellFor(spell, layout, obs, mask).value_or(-1);
    });
}

void Animus::Curriculum::Baselines::WriteHint(float* columns, int32 action, float weight, bool scripted)
{
    // Only a press is a hint: "nothing" is never one -- a waiting teacher hints the specific press it waits with (a
    // key let go of) or none at all, and a policy taught the script's no-ops stood still on its own (2026-10-01).
    bool const hinted = action > 0 && weight > 0.0f;
    columns[HintBlock::OBS_ACTION] = hinted ? float(action + 1) : 0.0f;
    columns[HintBlock::OBS_WEIGHT] = hinted ? weight : 0.0f;
    // The script played the seat whatever it hinted: its row is not the policy's (out of the PPO terms).
    columns[HintBlock::OBS_SCRIPTED] = scripted ? 1.0f : 0.0f;
}

Animus::Curriculum::WingTeacher::Facts Animus::Curriculum::StageScenario::TeacherFacts(Env const& env,
    uint32 seatIndex, SeatView const& view, Player* bot, float const* obs, uint8 const* mask) const
{
    Wt::Facts facts;
    EnvState const& data = Data(env);
    SeatState const& seat = data.Seats[seatIndex];
    if (!bot || !bot->IsInWorld())
    {
        facts.Alive = false;
        return facts;
    }

    facts.Alive = bot->IsAlive();
    facts.Health = bot->GetMaxHealth() ? float(bot->GetHealth()) / float(bot->GetMaxHealth()) : 1.0f;
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    facts.Mana = maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : -1.0f;
    // An auto shot or a wand repeating is no cast to wait out.
    facts.Casting = bot->IsNonMeleeSpellCast(false, false, true);
    facts.Swinging = bot->GetVictim() != nullptr;
    facts.Eating = bot->HasAuraType(SPELL_AURA_MOD_REGEN);
    facts.Drinking = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    facts.FoodLeft = view.FoodItem && bot->HasItemCount(view.FoodItem, 1);
    facts.DrinkLeft = view.DrinkItem && bot->HasItemCount(view.DrinkItem, 1);
    facts.Ranged = seat.L && seat.L->Profile && seat.Spec < seat.L->Profile->Specs.size()
        && seat.L->Profile->Specs[seat.Spec].Range != RangeBand::Melee;
    facts.FightSeconds = seat.InCombat
        ? float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, seat.CombatStartMs)) / 1000.0f : 0.0f;
    facts.Forward = seat.Controls.Held.Forward;
    facts.Strafe = seat.Controls.Held.Strafe;
    facts.TurnRate = seat.Controls.Held.TurnRate;

    // The party's roles: its tank (the crowd's, in a whole dungeon; else the seat that holds a pull best), and its
    // healer (the one that heals best of the others).
    Player const* tank = view.Crowd.Tank ? view.Crowd.Tank->ToPlayer() : nullptr;
    if (!tank)
    {
        float most = -1.0f;
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* member = SeatBotInWorld(env, index); member && member->IsAlive()
                && CanTank(data.Seats[index]) && data.Seats[index].Apt[Aptitude::MITIGATION] > most)
            {
                most = data.Seats[index].Apt[Aptitude::MITIGATION];
                tank = member;
            }
    }
    Player const* healer = nullptr;
    {
        float best = -1.0f;
        for (uint32 index = 0; index < data.ActiveSeats; ++index)
            if (Player* member = SeatBotInWorld(env, index); member && member != tank && CanHeal(data.Seats[index])
                && data.Seats[index].Apt[Aptitude::DIRECT_HEAL] > best)
            {
                best = data.Seats[index].Apt[Aptitude::DIRECT_HEAL];
                healer = member;
            }
    }
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
        uint32 const memberMana = member->GetMaxPower(POWER_MANA);
        row.Mana = memberMana ? float(member->GetPower(POWER_MANA)) / float(memberMana) : -1.0f;
        row.Tank = member == tank;
        row.Healer = member == healer;
        row.Focused = focus == member;
    }
    facts.FocusOnFriend = focus && focus->IsAlive() && std::find(party.begin(), party.begin() + partyCount, focus)
        != party.begin() + partyCount;
    if (tank && tank != bot && tank->IsAlive())
        facts.Tank = PlaceOf(bot, seat, tank);
    auto const inParty = [&party, partyCount](Unit const* unit)
    {
        return unit && std::find(party.begin(), party.begin() + partyCount, unit) != party.begin() + partyCount;
    };

    // The enemies: what the encounter keeps in the fight's slots and past them, as the script knows them.
    constexpr float ENEMY_YARDS = 60.0f;
    std::array<Unit*, PACK_SLOTS + CROWD_SLOTS> units{};
    uint32 unitCount = 0;
    for (uint32 slot = 0; slot < env.Targets.size() && unitCount < units.size(); ++slot)
        if (Unit* unit = env.FindTargetUnit(slot))
            units[unitCount++] = unit;
    for (uint32 i = 0; i < view.Crowd.Count && unitCount < units.size(); ++i)
        if (Unit* unit = view.Crowd.Units[i]; unit && std::find(units.begin(), units.begin() + unitCount, unit)
            == units.begin() + unitCount)
            units[unitCount++] = unit;
    ObjectGuid const selection = bot->GetTarget();
    ObjectGuid const tankTarget = tank ? tank->GetTarget() : ObjectGuid::Empty;
    Unit const* pull = nullptr;
    // In the fight first, then the rest nearest first: the list holds ENEMIES.
    std::sort(units.begin(), units.begin() + unitCount, [bot, &inParty](Unit const* a, Unit const* b)
    {
        bool const fightA = a->IsInCombat() && inParty(a->GetVictim());
        bool const fightB = b->IsInCombat() && inParty(b->GetVictim());
        if (fightA != fightB)
            return fightA;
        return bot->GetExactDist(a) < bot->GetExactDist(b);
    });
    for (uint32 i = 0; i < unitCount; ++i)
    {
        Unit* unit = units[i];
        if (!unit->IsAlive() || !unit->IsInWorld() || !unit->IsInMap(bot) || unit->IsPlayer()
            || bot->GetExactDist(unit) > ENEMY_YARDS || !bot->IsValidAttackTarget(unit))
            continue;
        Unit const* victim = unit->GetVictim();
        bool const fighting = unit->IsInCombat() && inParty(victim);
        if (!fighting && !unit->IsInCombat() && (!pull || bot->GetExactDist(unit) < bot->GetExactDist(pull)))
            pull = unit;
        if (facts.EnemyCount >= Wt::ENEMIES)
            continue;
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
    if (facts.Is == Wt::Role::Tank && pull)
    {
        facts.Pull = PlaceOf(bot, seat, pull);
        facts.PullSelected = pull->GetGUID() == selection;
        // A pull from range, if the seat has one it can cast at it now (its selection is what the mask is for).
        if (facts.PullSelected && seat.L && obs && mask)
            facts.HasRangedPull = Baselines::SpellFor(Wt::Spell::RangedPull, *seat.L, obs, mask).has_value();
    }

    // The way on: the route's next step (the encounter's planned path; never a bot input), else the objective.
    if (view.Crowd.HasStep)
        facts.Step = PlaceAt(bot, view.Crowd.Step.GetPositionX(), view.Crowd.Step.GetPositionY(),
            view.Crowd.Step.GetPositionZ());
    else if (view.HasObjective)
        facts.Step = PlaceAt(bot, view.Objective.GetPositionX(), view.Objective.GetPositionY(),
            view.Objective.GetPositionZ());
    if (GameObject const* object = view.Crowd.Object)
    {
        facts.Object = PlaceOf(bot, seat, object);
        facts.ObjectNeedsKey = CrowdBlock::KeyOf(object) != 0;
        facts.ObjectReach = object->GetInteractionDistance();
    }
    facts.Behind = facts.Is == Wt::Role::Tank && view.Crowd.Behind;
    facts.StillSeconds = view.Crowd.Still * 120.0f;
    return facts;
}
