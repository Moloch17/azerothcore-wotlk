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
#include <algorithm>
#include <cmath>

namespace
{
    namespace Wt = Animus::Curriculum::WingTeacher;
    namespace MC = Animus::Curriculum::MoveControls;

    float Wrap(float angle)
    {
        return std::remainder(angle, 2.0f * float(M_PI));
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

    void Stop(Wt::Facts const& facts, Wt::Choice& choice)
    {
        uint32 key = 0;
        AddKey(choice, Wt::Halt(facts, key), key);
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
    /// toward it (too far, or round a corner, to be seen from here).
    void TurnToSee(Wt::Facts const& facts, Wt::Choice& choice, Wt::Place const& place)
    {
        Wt::Steer(facts, choice, place.Bearing, place.Yards, Wt::MELEE_STOP_YARDS);
    }

    /// Select `enemy` through the sight list, or turn to see it.
    void SelectOrSee(Wt::Facts const& facts, Wt::Choice& choice, Wt::Enemy const& enemy)
    {
        if (enemy.At.Slot >= 0)
            AddPress(choice, Wt::Do::Select, enemy.At.Slot);
        else
            TurnToSee(facts, choice, enemy.At);
    }

    /// The pet sent at the seat's target, while it is not on it already.
    void SendPet(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (facts.PetClass && facts.PetOut && !facts.PetOnTarget)
            choice.Add({ Wt::Do::PetAttack, 0, -1, Wt::Spell::None });
    }

    /// Out of a fight with no pet out: revive or summon it, standing, else the hunter's call.
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
        if ((facts.Eating && facts.Health < 0.99f) || (facts.Drinking && usesMana && facts.Mana < 0.99f))
        {
            choice.Waiting = true;
            choice.Reason = "eating or drinking";
            Stop(facts, choice);
            return true;
        }
        bool const food = facts.Health < Wt::REST_HEALTH && facts.FoodLeft && !facts.Eating;
        bool const drink = usesMana && facts.Mana < Wt::REST_MANA && facts.DrinkLeft && !facts.Drinking;
        if (!food && !drink)
            return false;
        // Eating and drinking are cast standing: stop first.
        uint32 key = 0;
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

    /// **The way's next object, opened as a player opens it** (the user, 2026-10-27: "teach the bots how to open the
    /// doors"), by the seat the adapter named its opener: walk to it, turn to see it, and press -- a click on a Heavy
    /// Door, a click that loots the barrel, the gunpowder used on the cannon. True when it has a say.
    bool Open(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (!facts.Object.Present)
            return false;
        char const* what = facts.ObjectOpens == Wt::Opens::UseKey ? "uses the gunpowder on the cannon"
            : facts.ObjectOpens == Wt::Opens::Loot ? "loots the gunpowder" : "opens the door";
        // Out of reach, or not yet seen: walk to it (the walk brings it into view).
        if (facts.Object.Slot < 0 || facts.Object.Yards > facts.ObjectReach - 1.0f)
        {
            choice.Reason = Acore::StringFormat("{} ({:.0f} yd{})", what, facts.Object.Yards,
                facts.Object.Slot < 0 ? ", not seen yet" : "");
            Wt::Steer(facts, choice, facts.Object.Bearing, facts.Object.Yards,
                std::max(1.0f, facts.ObjectReach - 2.0f));
            return true;
        }
        choice.Reason = what;
        Stop(facts, choice);
        Wt::Face(choice, facts.Object.Bearing);
        AddPress(choice, facts.ObjectOpens == Wt::Opens::UseKey ? Wt::Do::UseItem : Wt::Do::Interact,
            facts.Object.Slot);
        return true;
    }

    /// The others between pulls: with the tank, along the recorded line to it (the step), stopping close; the line
    /// alone with no tank to follow.
    void Follow(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (facts.Tank.Present)
        {
            // Started at FOLLOW_YARDS, carried on to FOLLOW_STOP_YARDS: no stop-and-go at the edge.
            bool const going = facts.Forward > 0;
            float const start = going ? Wt::FOLLOW_STOP_YARDS : Wt::FOLLOW_YARDS;
            if (facts.Tank.Yards > start)
            {
                choice.Reason = Acore::StringFormat("follows the tank ({:.0f} yd)", facts.Tank.Yards);
                Wt::Place const& to = facts.Step.Present ? facts.Step : facts.Tank;
                Wt::Steer(facts, choice, to.Bearing, std::max(to.Yards, 1.0f), 0.0f);
                return;
            }
            choice.Reason = "with the tank";
            Stop(facts, choice);
            return;
        }
        if (facts.Step.Present)
        {
            choice.Reason = "walks the line";
            Wt::Steer(facts, choice, facts.Step.Bearing, facts.Step.Yards, 0.5f);
        }
    }

    /// Close to fighting distance of `enemy` and fight it: steer in, then stop, face it, swing and cast.
    void Engage(Wt::Facts const& facts, Wt::Choice& choice, Wt::Enemy const& enemy)
    {
        SendPet(facts, choice);
        float const beyond = facts.Ranged ? Wt::CAST_BEYOND_YARDS : Wt::MELEE_YARDS;
        float const stop = facts.Ranged ? Wt::CAST_STOP_YARDS : Wt::MELEE_STOP_YARDS;
        if (enemy.At.Yards > beyond)
        {
            Wt::Steer(facts, choice, enemy.At.Bearing, enemy.At.Yards, stop);
            return;
        }
        Stop(facts, choice);
        Wt::Face(choice, enemy.At.Bearing);
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
                Stop(facts, choice);
                Wt::Face(choice, target.At.Bearing);
                return;
            }
            choice.Reason = "the tank closes in";
            Wt::Steer(facts, choice, target.At.Bearing, target.At.Yards, Wt::MELEE_STOP_YARDS);
            return;
        }
        choice.Reason = target.OnOther ? "the tank taunts" : "the tank holds";
        Stop(facts, choice);
        Wt::Face(choice, target.At.Bearing);
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
        if (Rest(facts, choice))
            return;

        // The ready check: every member, the dead included. A minute with nothing gained waives health and mana, two
        // a member not back -- the teacher never stands still for good (2026-10-01: a script that stood still taught
        // bots to stand still).
        std::string why;
        bool hard = false;
        if (!Wt::Ready(facts, why, hard))
        {
            float const waive = hard ? Wt::HARD_WAIT_SECONDS : Wt::WAIT_SECONDS;
            if (facts.StillSeconds < waive)
            {
                choice.Waiting = true;
                choice.Reason = "the tank waits:" + why;
                Stop(facts, choice);
                return;
            }
            choice.Reason = "waited out:" + why + " ";
        }

        // The way's object, when the tank is its opener (a Heavy Door).
        if (Open(facts, choice))
            return;

        Summon(facts, choice);
        AddCast(choice, Wt::Spell::Buff);
        // The pack ahead if it is close and seen; else on along the line. A pack the seat does not see is turned to
        // at most, never walked at: the line passes every pack, and brings it into view from where it can be reached.
        if (facts.Pull.Present && facts.Pull.Yards <= Wt::PULL_YARDS && !facts.PullSelected && facts.Pull.Slot < 0)
        {
            choice.Reason += "the tank looks at the pack ahead";
            if (Wt::Face(choice, facts.Pull.Bearing))
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
                Stop(facts, choice);
                Wt::Face(choice, facts.Pull.Bearing);
                AddCast(choice, Wt::Spell::RangedPull);
                return;
            }
            if (facts.Pull.Yards > Wt::MELEE_YARDS)
            {
                Wt::Steer(facts, choice, facts.Pull.Bearing, facts.Pull.Yards, Wt::MELEE_STOP_YARDS);
                return;
            }
            Stop(facts, choice);
            Wt::Face(choice, facts.Pull.Bearing);
            choice.Add({ Wt::Do::StartAttack, 0, -1, Wt::Spell::None });
            AddCast(choice, Wt::Spell::Damage);
            return;
        }
        if (facts.Step.Present)
        {
            choice.Reason += "the tank advances";
            Wt::Steer(facts, choice, facts.Step.Bearing, facts.Step.Yards, 0.5f);
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
            Stop(facts, choice);
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
        if (member.At.Yards > Wt::HEAL_YARDS)
        {
            Wt::Steer(facts, choice, member.At.Bearing, member.At.Yards, Wt::HEAL_YARDS - 5.0f);
            return true;
        }
        Stop(facts, choice);
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
        if (fight)
        {
            // Near the tank, in reach of it, and standing.
            if (facts.Tank.Present && facts.Tank.Yards > Wt::HEAL_YARDS - 5.0f)
            {
                choice.Reason = "the healer keeps in reach of the tank";
                Wt::Place const& to = facts.Step.Present ? facts.Step : facts.Tank;
                Wt::Steer(facts, choice, to.Bearing, std::max(to.Yards, 1.0f), 0.0f);
                return;
            }
            choice.Waiting = true;
            choice.Reason = "the healer watches the party";
            Stop(facts, choice);
            return;
        }
        if (Rest(facts, choice))
            return;
        if (Open(facts, choice))
            return;
        Summon(facts, choice);
        AddCast(choice, Wt::Spell::Buff);
        Follow(facts, choice);
    }

    void Damage(Wt::Facts const& facts, Wt::Choice& choice)
    {
        if (!AnyFight(facts))
        {
            if (Rest(facts, choice))
                return;
            if (Open(facts, choice))
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
            Wt::Steer(facts, choice, to.Bearing, std::max(to.Yards, 1.0f), 0.0f);
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
            Stop(facts, choice);
            Wt::Face(choice, target.At.Bearing);
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
    else
        return false;
    return true;
}

bool Animus::Curriculum::WingTeacher::Face(Choice& choice, float bearing)
{
    bearing = Wrap(bearing);
    if (std::fabs(bearing) <= FACE_WITHIN)
        return false;
    choice.Head(bearing);
    return true;
}

void Animus::Curriculum::WingTeacher::Steer(Facts const& facts, Choice& choice, float bearing, float yards,
    float stopYards)
{
    uint32 key = 0;
    if (yards <= stopYards)
    {
        AddKey(choice, Halt(facts, key), key);
        return;
    }
    bearing = Wrap(bearing);
    // Head for it every decision (the controller eases the body round); the feet walk on while it is ahead enough,
    // so a bend is taken in stride, and stop to turn on the spot when it is behind.
    choice.Head(bearing);
    if (facts.Strafe != 0)
    {
        AddKey(choice, true, MC::ACTION_STRAFE_STOP);
        return;
    }
    if (facts.Forward > 0 && std::fabs(bearing) > STOP_TO_TURN)
    {
        AddKey(choice, true, MC::ACTION_MOVE_STOP);
        return;
    }
    if (facts.Forward <= 0 && std::fabs(bearing) < WALK_WITHIN)
        AddKey(choice, true, MC::ACTION_MOVE_FORWARD);
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
