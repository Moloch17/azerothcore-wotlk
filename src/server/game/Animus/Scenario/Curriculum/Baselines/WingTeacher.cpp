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

#include "WingTeacher.h"
#include "MoveControls.h"
#include "StringFormat.h"
#include <cmath>

namespace
{
    namespace Wt = Animus::Curriculum::WingTeacher;
    namespace MC = Animus::Curriculum::MoveControls;

    float Wrap(float angle)
    {
        return std::remainder(angle, 2.0f * float(M_PI));
    }

    /// The turn rate whose swing over one decision best fits `bearing` (same sign).
    uint32 TurnFor(float bearing)
    {
        uint32 best = MC::TURN_STOP_INDEX;
        for (uint32 turn = 0; turn < MC::TURN_COUNT; ++turn)
        {
            float const rate = MC::TURN_RATES_DEG[turn] * MC::DEG;
            if (rate == 0.0f || (rate > 0.0f) != (bearing > 0.0f))
                continue;
            if (best == MC::TURN_STOP_INDEX || std::fabs(rate * Wt::DECISION_SECONDS - bearing)
                < std::fabs(MC::TURN_RATES_DEG[best] * MC::DEG * Wt::DECISION_SECONDS - bearing))
                best = turn;
        }
        return best;
    }

    /// Turn toward `bearing` unless within `within` of it: the rate to hold, or a let-go once within. False when the
    /// right rate is already held, or none is needed and none is held.
    bool Turn(Wt::Facts const& facts, float bearing, float within, uint32& key)
    {
        bearing = Wrap(bearing);
        if (std::fabs(bearing) <= within)
        {
            if (facts.TurnRate == 0.0f)
                return false;
            key = MC::ACTION_TURN_STOP;
            return true;
        }
        uint32 const best = TurnFor(bearing);
        if (std::fabs(MC::TURN_RATES_DEG[best] * MC::DEG - facts.TurnRate) < 1e-3f)
            return false;
        key = MC::ACTION_TURN_FIRST + best;
        return true;
    }

    bool Moving(Wt::Facts const& facts)
    {
        return facts.Forward != 0 || facts.Strafe != 0 || facts.TurnRate != 0.0f;
    }

    void AddKey(Wt::Choice& choice, bool pressed, uint32 key)
    {
        if (pressed)
            choice.Add({ Wt::Do::Key, key, -1, Wt::Spell::None });
    }

    void AddCast(Wt::Choice& choice, Wt::Spell spell)
    {
        choice.Add({ Wt::Do::Cast, 0, -1, spell });
    }

    void AddPress(Wt::Choice& choice, Wt::Do press, int32 slot)
    {
        if (slot >= 0)
            choice.Add({ press, 0, slot, Wt::Spell::None });
    }

    bool AnyFight(Wt::Facts const& facts)
    {
        for (uint32 i = 0; i < facts.EnemyCount; ++i)
            if (facts.Enemies[i].InFight)
                return true;
        return false;
    }

    uint32 InFight(Wt::Facts const& facts)
    {
        uint32 count = 0;
        for (uint32 i = 0; i < facts.EnemyCount; ++i)
            count += facts.Enemies[i].InFight ? 1 : 0;
        return count;
    }

    /// The nearest enemy in the fight that `wanted` picks, or -1.
    template <typename Wanted>
    int32 Nearest(Wt::Facts const& facts, Wanted wanted)
    {
        int32 best = -1;
        for (uint32 i = 0; i < facts.EnemyCount; ++i)
        {
            Wt::Enemy const& enemy = facts.Enemies[i];
            if (!enemy.InFight || !wanted(enemy))
                continue;
            if (best < 0 || enemy.At.Yards < facts.Enemies[uint32(best)].At.Yards)
                best = int32(i);
        }
        return best;
    }

    /// An entity the seat cannot press yet (not in its sight list): it turns to it, so the camera finds it, and walks
    /// toward it while already facing it (too far, or round a corner, to be seen from here).
    void TurnToSee(Wt::Facts const& facts, Wt::Choice& choice, Wt::Place const& place)
    {
        uint32 key = 0;
        if (Wt::Face(facts, place.Bearing, key)
            || Wt::Steer(facts, place.Bearing, place.Yards, Wt::MELEE_STOP_YARDS, key))
            AddKey(choice, true, key);
    }

    /// Select `enemy` through the sight list, or turn to see it.
    void SelectOrSee(Wt::Facts const& facts, Wt::Choice& choice, Wt::Enemy const& enemy)
    {
        if (enemy.At.Slot >= 0)
            AddPress(choice, Wt::Do::Select, enemy.At.Slot);
        else
            TurnToSee(facts, choice, enemy.At);
    }

    /// The pet sent at the seat's target, while it is not on it already: first of the fight's presses.
    void SendPet(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (facts.PetClass && facts.PetOut && !facts.PetOnTarget)
            choice.Add({ Wt::Do::PetAttack, 0, -1, Wt::Spell::None });
    }

    /// Out of a fight with no pet out: revive or summon it, standing (a demon's summon is a cast), else the hunter's
    /// call (Call Pet) -- every one of them a cast through the client.
    void Summon(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (!facts.PetClass || facts.PetOut || facts.Forward != 0)
            return;
        AddCast(choice, Wt::Spell::Summon);
        choice.Add({ Wt::Do::CallPet, 0, -1, Wt::Spell::None });
    }

    /// Eating and drinking between pulls: sit until full, stop first, then eat and drink. True when it has a say.
    bool Rest(Wt::Facts const& facts, Wt::Choice& choice)
    {
        bool const usesMana = facts.Mana >= 0.0f;
        uint32 key = 0;
        if ((facts.Eating && facts.Health < 0.99f) || (facts.Drinking && usesMana && facts.Mana < 0.99f))
        {
            choice.Waiting = true;
            choice.Reason = "eating or drinking";
            AddKey(choice, Wt::Halt(facts, key), key);
            return true;
        }
        bool const food = facts.Health < Wt::REST_HEALTH && facts.FoodLeft && !facts.Eating;
        bool const drink = usesMana && facts.Mana < Wt::REST_MANA && facts.DrinkLeft && !facts.Drinking;
        if (!food && !drink)
            return false;
        // Eating and drinking are cast standing: stop first.
        if (Wt::Halt(facts, key))
        {
            choice.Reason = "stops to eat or drink";
            AddKey(choice, true, key);
            return true;
        }
        choice.Reason = food ? "eats" : "drinks";
        if (food)
            choice.Add({ Wt::Do::Eat, 0, -1, Wt::Spell::None });
        if (drink)
            choice.Add({ Wt::Do::Drink, 0, -1, Wt::Spell::None });
        return true;
    }

    /// The others between pulls: with the tank, along the route to it (the step), stopping close; the route alone
    /// with no tank to follow (a risen seat's way back, a party whose tank is down).
    void Follow(Wt::Facts const& facts, Wt::Choice& choice)
    {
        uint32 key = 0;
        if (facts.Tank.Present)
        {
            // Started at FOLLOW_YARDS, carried on to FOLLOW_STOP_YARDS: no stop-and-go at the edge.
            bool const going = facts.Forward > 0;
            float const start = going ? Wt::FOLLOW_STOP_YARDS : Wt::FOLLOW_YARDS;
            if (facts.Tank.Yards > start)
            {
                choice.Reason = Acore::StringFormat("follows the tank ({:.0f} yd)", facts.Tank.Yards);
                Wt::Place const& to = facts.Step.Present ? facts.Step : facts.Tank;
                AddKey(choice, Wt::Steer(facts, to.Bearing, std::max(to.Yards, 1.0f), 0.0f, key), key);
                return;
            }
            choice.Reason = "with the tank";
            AddKey(choice, Wt::Halt(facts, key), key);
            return;
        }
        if (facts.Step.Present)
        {
            choice.Reason = "walks the route";
            AddKey(choice, Wt::Steer(facts, facts.Step.Bearing, facts.Step.Yards, 0.5f, key), key);
        }
    }

    /// Close to fighting distance of `enemy` and fight it: steer in, then stop, face it, swing and cast.
    void Engage(Wt::Facts const& facts, Wt::Choice& choice, Wt::Enemy const& enemy)
    {
        uint32 key = 0;
        SendPet(facts, choice);
        float const beyond = facts.Ranged ? Wt::CAST_BEYOND_YARDS : Wt::MELEE_YARDS;
        float const stop = facts.Ranged ? Wt::CAST_STOP_YARDS : Wt::MELEE_STOP_YARDS;
        if (enemy.At.Yards > beyond)
        {
            AddKey(choice, Wt::Steer(facts, enemy.At.Bearing, enemy.At.Yards, stop, key), key);
            return;
        }
        if (facts.Forward != 0)
            AddKey(choice, Wt::Halt(facts, key), key);
        AddKey(choice, Wt::Face(facts, enemy.At.Bearing, key), key);
        if (!facts.Ranged && !facts.Swinging)
            choice.Add({ Wt::Do::StartAttack, 0, -1, Wt::Spell::None });
        AddCast(choice, Wt::Spell::Damage);
    }

    void TankFight(Wt::Facts const& facts, Wt::Choice& choice)
    {
        // Whatever is on somebody else first (the nearest), then what it already holds, then the nearest in the fight.
        int32 pick = Nearest(facts, [](Wt::Enemy const& enemy) { return enemy.OnOther; });
        if (pick < 0)
            pick = Nearest(facts, [](Wt::Enemy const& enemy) { return enemy.Selected; });
        if (pick < 0)
            pick = Nearest(facts, [](Wt::Enemy const&) { return true; });
        if (pick < 0)
            return;
        Wt::Enemy const& target = facts.Enemies[uint32(pick)];
        if (!target.Selected)
        {
            choice.Reason = target.OnOther ? "the tank takes the loose one" : "the tank targets the pull";
            SelectOrSee(facts, choice, target);
            return;
        }

        uint32 key = 0;
        // The loose one: taunt it, from where it stands.
        if (target.OnOther)
            AddCast(choice, Wt::Spell::Taunt);
        SendPet(facts, choice);
        if (target.At.Yards > Wt::MELEE_YARDS)
        {
            // Pulled from range: let it come to the party rather than run into the next pack.
            if (target.OnSelf && facts.FightSeconds < Wt::PULL_COMES_SECONDS)
            {
                choice.Waiting = true;
                choice.Reason = "the tank lets the pull come";
                AddKey(choice, Wt::Halt(facts, key), key);
                return;
            }
            choice.Reason = "the tank closes in";
            AddKey(choice, Wt::Steer(facts, target.At.Bearing, target.At.Yards, Wt::MELEE_STOP_YARDS, key), key);
            return;
        }
        choice.Reason = target.OnOther ? "the tank taunts" : "the tank holds";
        if (facts.Forward != 0)
            AddKey(choice, Wt::Halt(facts, key), key);
        AddKey(choice, Wt::Face(facts, target.At.Bearing, key), key);
        if (InFight(facts) >= 3)
            AddCast(choice, Wt::Spell::AreaThreat);
        if (!facts.Swinging)
            choice.Add({ Wt::Do::StartAttack, 0, -1, Wt::Spell::None });
        AddCast(choice, Wt::Spell::HighThreat);
        AddCast(choice, Wt::Spell::Damage);
    }

    void Tank(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (AnyFight(facts))
        {
            TankFight(facts, choice);
            return;
        }

        uint32 key = 0;
        // Risen at the entrance with the party deep in: back to it first.
        if (facts.Behind && facts.Step.Present)
        {
            choice.Reason = "the tank walks back from the entrance";
            AddKey(choice, Wt::Steer(facts, facts.Step.Bearing, facts.Step.Yards, 0.5f, key), key);
            return;
        }
        if (Rest(facts, choice))
            return;

        // The ready check: every member, the dead included (they rise at the entrance and walk back). A minute with
        // nothing gained waives health and mana; nothing waives a member who is not back.
        std::string why;
        bool hard = false;
        if (!Wt::Ready(facts, why, hard))
        {
            if (hard || facts.StillSeconds < Wt::WAIT_SECONDS)
            {
                choice.Waiting = true;
                choice.Reason = "the tank waits:" + why;
                AddKey(choice, Wt::Halt(facts, key), key);
                return;
            }
            choice.Reason = "waited out:" + why + " ";
        }

        // What opens the way on, seen near: walk to it and use it with a real press. One the seat does not see yet is
        // turned to, at most: the route goes on, and brings it into view.
        if (facts.Object.Present && facts.Object.Yards <= Wt::PULL_YARDS && facts.Object.Slot < 0
            && Wt::Face(facts, facts.Object.Bearing, key))
        {
            choice.Reason += "the tank looks for the way on";
            AddKey(choice, true, key);
            return;
        }
        if (facts.Object.Present && facts.Object.Yards <= Wt::PULL_YARDS && facts.Object.Slot >= 0)
        {
            choice.Reason += facts.ObjectNeedsKey ? "the tank uses its key on the way on" : "the tank opens the way on";
            // Out of reach: walk to it (nothing to press while already walking there).
            if (facts.Object.Yards > facts.ObjectReach - 1.0f)
            {
                AddKey(choice, Wt::Steer(facts, facts.Object.Bearing, facts.Object.Yards,
                    std::max(1.0f, facts.ObjectReach - 2.0f), key), key);
                return;
            }
            AddKey(choice, Wt::Halt(facts, key), key);
            AddPress(choice, facts.ObjectNeedsKey ? Wt::Do::UseItem : Wt::Do::Interact, facts.Object.Slot);
            return;
        }

        Summon(facts, choice);
        // Ready: its buffs up, then the pack ahead if it is close and seen; else on along the route. A pack the seat
        // does not see (on a ledge, across the lava, round a corner) is turned to at most, never walked at: the route
        // passes every pack, and brings it into view from where it can be reached.
        AddCast(choice, Wt::Spell::Buff);
        if (facts.Pull.Present && facts.Pull.Yards <= Wt::PULL_YARDS && !facts.PullSelected && facts.Pull.Slot < 0
            && Wt::Face(facts, facts.Pull.Bearing, key))
        {
            choice.Reason += "the tank looks at the pack ahead";
            AddKey(choice, true, key);
            return;
        }
        if (facts.Pull.Present && facts.Pull.Yards <= Wt::PULL_YARDS && (facts.PullSelected || facts.Pull.Slot >= 0))
        {
            choice.Reason += "the tank pulls";
            if (!facts.PullSelected)
            {
                AddPress(choice, Wt::Do::Select, facts.Pull.Slot);
                return;
            }
            // From range when it can: one pack brought back to the party, rather than the tank walking into it.
            if (facts.HasRangedPull)
            {
                AddKey(choice, Wt::Halt(facts, key), key);
                AddKey(choice, Wt::Face(facts, facts.Pull.Bearing, key), key);
                AddCast(choice, Wt::Spell::RangedPull);
                return;
            }
            if (facts.Pull.Yards > Wt::MELEE_YARDS)
            {
                AddKey(choice, Wt::Steer(facts, facts.Pull.Bearing, facts.Pull.Yards, Wt::MELEE_STOP_YARDS, key), key);
                return;
            }
            AddKey(choice, Wt::Halt(facts, key), key);
            AddKey(choice, Wt::Face(facts, facts.Pull.Bearing, key), key);
            {
                choice.Add({ Wt::Do::StartAttack, 0, -1, Wt::Spell::None });
                AddCast(choice, Wt::Spell::Damage);
            }
            return;
        }
        if (facts.Step.Present)
        {
            choice.Reason += "the tank advances";
            AddKey(choice, Wt::Steer(facts, facts.Step.Bearing, facts.Step.Yards, 0.5f, key), key);
        }
    }

    /// The healer's heal, at whoever needs it most: focused, in range and standing. True when it has one to give.
    bool Heal(Wt::Facts const& facts, Wt::Choice& choice, bool fight)
    {
        float const below = fight ? Wt::HEAL_FIGHT : Wt::HEAL_REST;
        int32 lowest = -1;
        float lowestHealth = facts.Health < below ? facts.Health : below;
        for (uint32 i = 0; i < facts.PartyCount; ++i)
        {
            Wt::Member const& member = facts.Party[i];
            if (member.Alive && member.At.Present && member.Health < lowestHealth)
            {
                lowest = int32(i);
                lowestHealth = member.Health;
            }
        }
        uint32 key = 0;
        if (lowest < 0)
        {
            if (facts.Health >= below)
                return false;
            // Itself: a beneficial spell goes to the focus while it is a living friend, so the focus is let go of
            // first (/clearfocus), then the heal goes to the seat.
            if (facts.FocusOnFriend)
            {
                choice.Reason = "the healer clears its focus to heal itself";
                choice.Add({ Wt::Do::ClearFocus, 0, -1, Wt::Spell::None });
                return true;
            }
            choice.Reason = "the healer heals itself";
            AddKey(choice, Wt::Halt(facts, key), key);
            AddCast(choice, Wt::Spell::Heal);
            return true;
        }
        Wt::Member const& member = facts.Party[uint32(lowest)];
        choice.Reason = Acore::StringFormat("the healer heals a member at {:.0f}%", member.Health * 100.0f);
        if (!member.Focused)
        {
            if (member.At.Slot >= 0)
                AddPress(choice, Wt::Do::Focus, member.At.Slot);
            else
                TurnToSee(facts, choice, member.At);
            return true;
        }
        if (member.At.Yards > Wt::HEAL_YARDS && Wt::Steer(facts, member.At.Bearing, member.At.Yards,
            Wt::HEAL_YARDS - 5.0f, key))
        {
            AddKey(choice, true, key);
            return true;
        }
        AddKey(choice, Wt::Halt(facts, key), key);
        AddCast(choice, Wt::Spell::Heal);
        return true;
    }

    void Healer(Wt::Facts const& facts, Wt::Choice& choice)
    {
        bool const fight = AnyFight(facts);
        if (fight && facts.Health < 0.3f)
            AddCast(choice, Wt::Spell::Defensive);
        if (Heal(facts, choice, fight))
            return;
        uint32 key = 0;
        if (fight)
        {
            // Near the tank, in reach of it, and standing.
            if (facts.Tank.Present && facts.Tank.Yards > Wt::HEAL_YARDS - 5.0f)
            {
                choice.Reason = "the healer keeps in reach of the tank";
                Wt::Place const& to = facts.Step.Present ? facts.Step : facts.Tank;
                AddKey(choice, Wt::Steer(facts, to.Bearing, std::max(to.Yards, 1.0f), 0.0f, key), key);
                return;
            }
            choice.Waiting = true;
            choice.Reason = "the healer watches the party";
            AddKey(choice, Wt::Halt(facts, key), key);
            return;
        }
        if (Rest(facts, choice))
            return;
        Summon(facts, choice);
        AddCast(choice, Wt::Spell::Buff);
        Follow(facts, choice);
    }

    void Damage(Wt::Facts const& facts, Wt::Choice& choice)
    {
        uint32 key = 0;
        if (!AnyFight(facts))
        {
            if (Rest(facts, choice))
                return;
            Summon(facts, choice);
            AddCast(choice, Wt::Spell::Buff);
            Follow(facts, choice);
            return;
        }

        if (facts.Health < 0.3f)
            AddCast(choice, Wt::Spell::Defensive);
        // Drawn away from the tank: back to it.
        if (facts.Tank.Present && facts.Tank.Yards > Wt::LEASH_YARDS)
        {
            choice.Reason = "back to the tank";
            Wt::Place const& to = facts.Step.Present ? facts.Step : facts.Tank;
            AddKey(choice, Wt::Steer(facts, to.Bearing, std::max(to.Yards, 1.0f), 0.0f, key), key);
            return;
        }
        // The tank's target; else the nearest in the fight on somebody.
        int32 pick = Nearest(facts, [](Wt::Enemy const& enemy) { return enemy.TankTarget; });
        if (pick < 0)
            pick = Nearest(facts, [](Wt::Enemy const& enemy) { return enemy.OnOther || enemy.OnTank; });
        if (pick < 0)
            pick = Nearest(facts, [](Wt::Enemy const&) { return true; });
        if (pick < 0)
            return;
        Wt::Enemy const& target = facts.Enemies[uint32(pick)];
        if (!target.Selected)
        {
            choice.Reason = "assists the tank";
            if (target.TankTarget && facts.Tank.Present && facts.Tank.Slot >= 0)
                AddPress(choice, Wt::Do::Assist, facts.Tank.Slot);
            SelectOrSee(facts, choice, target);
            return;
        }
        if (target.Interruptible)
            AddCast(choice, Wt::Spell::Interrupt);
        // A fresh pull not yet on the tank: the tank takes it first.
        if (target.TankTarget && !target.OnTank && !target.OnSelf && facts.FightSeconds < Wt::LET_TANK_SECONDS)
        {
            choice.Waiting = true;
            choice.Reason = "lets the tank take it";
            AddKey(choice, Wt::Halt(facts, key), key);
            return;
        }
        choice.Reason = "fights the tank's target";
        Engage(facts, choice, target);
    }
}

bool Animus::Curriculum::WingTeacher::Halt(Facts const& facts, uint32& key)
{
    if (facts.Forward != 0)
        key = MC::ACTION_MOVE_STOP;
    else if (facts.Strafe != 0)
        key = MC::ACTION_STRAFE_STOP;
    else if (facts.TurnRate != 0.0f)
        key = MC::ACTION_TURN_STOP;
    else
        return false;
    return true;
}

bool Animus::Curriculum::WingTeacher::Face(Facts const& facts, float bearing, uint32& key)
{
    return Turn(facts, bearing, FACE_WITHIN, key);
}

bool Animus::Curriculum::WingTeacher::Steer(Facts const& facts, float bearing, float yards, float stopYards,
    uint32& key)
{
    if (yards <= stopYards)
        return Halt(facts, key);
    if (facts.Strafe != 0)
    {
        key = MC::ACTION_STRAFE_STOP;
        return true;
    }
    bearing = Wrap(bearing);
    // Turn first; the feet walk on while it does, so a bend is taken in stride.
    if (Turn(facts, bearing, TURN_WITHIN, key))
        return true;
    if (facts.Forward <= 0 && std::fabs(bearing) < float(M_PI) / 2.0f)
    {
        key = MC::ACTION_MOVE_FORWARD;
        return true;
    }
    return false;
}

bool Animus::Curriculum::WingTeacher::Ready(Facts const& facts, std::string& why, bool& hard)
{
    why.clear();
    hard = false;
    if (facts.Health < READY_HEALTH)
        why += " its own health;";
    if (facts.Mana >= 0.0f && facts.Mana < READY_MANA)
        why += " its own mana;";
    for (uint32 i = 0; i < facts.PartyCount; ++i)
    {
        Member const& member = facts.Party[i];
        if (!member.Alive || !member.At.Present || member.At.Yards > GATHER_YARDS)
        {
            hard = true;
            why += !member.Alive ? Acore::StringFormat(" member {} dead;", i)
                : Acore::StringFormat(" member {} {:.0f} yd;", i, member.At.Yards);
        }
        else if (member.Health < READY_HEALTH)
            why += Acore::StringFormat(" member {} health {:.0f}%;", i, member.Health * 100.0f);
        else if (member.Mana >= 0.0f && member.Mana < READY_MANA)
            why += Acore::StringFormat(" member {} mana {:.0f}%;", i, member.Mana * 100.0f);
    }
    return why.empty();
}

Animus::Curriculum::WingTeacher::Choice Animus::Curriculum::WingTeacher::Decide(Facts const& facts)
{
    Choice choice;
    if (!facts.Alive)
    {
        choice.Reason = "dead";
        return choice;
    }
    // A cast in progress finishes: a key now would break it.
    if (facts.Casting)
    {
        choice.Waiting = true;
        choice.Reason = "casting";
        return choice;
    }
    switch (facts.Is)
    {
        case Role::Tank:
            Tank(facts, choice);
            break;
        case Role::Healer:
            Healer(facts, choice);
            break;
        case Role::Damage:
            Damage(facts, choice);
            break;
    }
    return choice;
}

int32 Animus::Curriculum::WingTeacher::Press(Choice const& choice, PressSpace const& space, uint8 const* mask,
    SpellPick const& spells)
{
    auto const allowed = [mask](PressSpace::Range const& range, uint32 local) -> int32
    {
        if (local >= range.Count || !mask[range.First + local])
            return -1;
        return int32(range.First + local);
    };
    // The sight block's pointer groups, in its order: a slot of each.
    uint32 const slots = space.Sight.Count / 5;
    auto const press = [&](uint32 group, int32 slot) -> int32
    {
        if (slot < 0 || uint32(slot) >= slots)
            return -1;
        return allowed(space.Sight, group * slots + uint32(slot));
    };
    for (uint32 i = 0; i < choice.Count; ++i)
    {
        Option const& option = choice.Options[i];
        int32 action = -1;
        switch (option.What)
        {
            case Do::Key:           action = allowed(space.Move, option.Key); break;
            case Do::Select:        action = press(0, option.Slot); break;
            case Do::Interact:      action = press(1, option.Slot); break;
            case Do::UseItem:       action = press(2, option.Slot); break;
            case Do::Assist:        action = press(3, option.Slot); break;
            case Do::Focus:         action = press(4, option.Slot); break;
            case Do::ClearFocus:    action = slots ? allowed(space.Sight, 5 * slots) : -1; break;
            case Do::Cast:          action = spells ? spells(option.Cast) : -1; break;
            case Do::StartAttack:   action = allowed(space.Duel, space.StartAttack); break;
            case Do::PetAttack:     action = allowed(space.Duel, space.PetAttack); break;
            case Do::CallPet:       action = allowed(space.Duel, space.CallPet); break;
            case Do::Eat:           action = allowed(space.Gauntlet, space.Eat); break;
            case Do::Drink:         action = allowed(space.Gauntlet, space.Drink); break;
        }
        if (action > 0)
            return action;
    }
    return -1;
}

Animus::Curriculum::WingTeacher::Choice Animus::Curriculum::WingTeacher::Hands(Facts const& facts)
{
    Choice const full = Decide(facts);
    Choice hands;
    hands.Waiting = full.Waiting;
    hands.Reason = full.Reason;
    for (uint32 i = 0; i < full.Count; ++i)
        if (full.Options[i].What != Do::Key)
            hands.Add(full.Options[i]);
    return hands;
}
