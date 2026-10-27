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

#ifndef ANIMUS_LIB_CURRICULUM_WING_TEACHER_H
#define ANIMUS_LIB_CURRICULUM_WING_TEACHER_H

#include "Define.h"
#include <array>
#include <functional>
#include <string>

/*
 * **The dungeon teacher** (revived for dm_clear, the user 2026-10-27: "a teacher again that slowly withdraws"; the
 * first one, dungeon-curriculum I6, was removed 2026-10-07): a party clearing the Deadmines pack by pack along the
 * recorded way, played on the player controller and the sight block's presses -- the same hands and feet a learned
 * seat has.
 *
 * The tank leads along the recorded line, holds the pull until every member is up and with it, pulls one pack at a
 * time and takes whatever hits somebody else; the healer keeps the party up within its mana; damage assists the
 * tank's target and lets the tank take it first; between pulls everyone eats and drinks. **The way's doors are opened
 * as a player opens them** (the user: "one of the key things it needs to do is teach the bots how to open the
 * doors"): a Heavy Door clicked, the gunpowder barrel looted, the gunpowder used on the cannon by the seat that holds
 * it -- each by the seat the adapter names the opener -- and a boss's door by killing its boss.
 *
 * **What it may know** is a script's: the recorded line, the packs, who is on whom. **What it may do** is a player's:
 * held keys and a heading (the controller's), a sight-list press on an entity the seat sees or remembers, a spell, eat
 * or drink, the auto attack. An entity the seat has not seen cannot be pressed: the teacher turns to it first.
 *
 * This is the brain, with no world in it: a seat's Facts in, a Choice out. The adapter (StageScenario's teacher,
 * StageTeacher.cpp) gathers the facts and turns the choice into one layout action and a heading.
 */
namespace Animus::Curriculum::WingTeacher
{
    enum class Role : uint8
    {
        Tank,
        Healer,
        Damage
    };

    /// Something at a place, from the seat: how far, which way in the seat's own frame (radians, + left), and the
    /// sight list's slot that names it (-1: the seat neither sees nor remembers it).
    struct Place
    {
        bool Present = false;
        float Yards = 0.0f;
        float Bearing = 0.0f;
        int32 Slot = -1;
    };

    struct Member
    {
        Place At;
        bool Alive = true;
        float Health = 1.0f;
        float Mana = -1.0f;         // < 0: no mana
        bool Tank = false;
        bool Healer = false;
        bool Focused = false;       // the seat's client focus (its beneficial spells go there)
    };

    struct Enemy
    {
        Place At;
        bool InFight = false;       // in combat with the party
        bool OnTank = false;        // attacking the party's tank
        bool OnSelf = false;        // attacking this seat
        bool OnOther = false;       // attacking a member other than the tank (this seat included)
        bool Selected = false;      // the seat's selection
        bool TankTarget = false;    // the tank's selection
        bool Interruptible = false; // casting something an interrupt stops
    };

    /// How the way's next object is opened: a click (a Heavy Door), a loot (the gunpowder barrel), the key item used
    /// on it (the cannon, with the gunpowder).
    enum class Opens : uint8
    {
        Click,
        Loot,
        UseKey
    };

    constexpr uint32 PARTY_OTHERS = 4;
    constexpr uint32 ENEMIES = 16;

    struct Facts
    {
        Role Is = Role::Damage;
        bool Alive = true;
        float Health = 1.0f;
        float Mana = -1.0f;             // < 0: no mana
        bool Casting = false;           // a cast or channel in progress
        bool Swinging = false;          // auto attack on
        bool Eating = false;
        bool Drinking = false;
        bool FoodLeft = false;
        bool DrinkLeft = false;
        bool Ranged = false;            // the spec fights from range
        bool HasRangedPull = false;     // a pull from range is castable now (the adapter asks the layout)
        bool FocusOnFriend = false;     // the focus is a living friend: a beneficial spell goes there, not to the seat
        bool PetClass = false;          // its class fights with a pet
        bool PetOut = false;            // ... and a living one is out
        bool PetOnTarget = false;       // ... attacking the seat's selection
        float FightSeconds = 0.0f;      // since the seat entered its current combat

        /// The keys held (the controller's ControlState).
        int8 Forward = 0;
        int8 Strafe = 0;

        std::array<Member, PARTY_OTHERS> Party{};
        uint32 PartyCount = 0;
        std::array<Enemy, ENEMIES> Enemies{};
        uint32 EnemyCount = 0;

        /// The party's tank, for every seat but the tank: present while it lives.
        Place Tank;
        /// Where the recorded line takes this seat next: the tank's way on, everyone else's way to the tank.
        Place Step;
        /// The next pack, the tank's to pull: its nearest member not yet in a fight.
        Place Pull;
        bool PullSelected = false;      // ... and it is the seat's selection
        /// The way's next object, for the seat the adapter names its opener only: a Heavy Door, the barrel, the cannon.
        Place Object;
        Opens ObjectOpens = Opens::Click;
        float ObjectReach = 5.0f;
        /// The party's time with no progress (the ready check's valve).
        float StillSeconds = 0.0f;
    };

    /// The spell a choice asks for: the adapter finds the seat's first allowed one of the kind (TeacherSpells).
    enum class Spell : uint8
    {
        None,
        Damage,
        Taunt,
        AreaThreat,
        HighThreat,
        RangedPull,
        Interrupt,
        Heal,
        Defensive,
        Buff,
        Summon,         // the pet's summon (a warlock's demon, a hunter's Call Pet, a ghoul, a water elemental)
    };

    enum class Do : uint8
    {
        Key,            // a MoveControls action (Option::Key)
        Select,         // the sight list's presses, on Option::Slot
        Interact,
        UseItem,
        Assist,
        Focus,
        ClearFocus,     // the sight block's clear-focus press (the client's /clearfocus)
        Cast,           // a spell of Option::Cast
        StartAttack,
        PetAttack,      // the pet bar's Attack at the selection
        CallPet,        // a hunter's stable slot 0 (the duel block's call)
        Eat,
        Drink,
    };

    struct Option
    {
        Do What = Do::Key;
        uint32 Key = 0;
        int32 Slot = -1;
        Spell Cast = Spell::None;
    };

    constexpr uint32 MAX_OPTIONS = 8;

    /// The teacher's choice: what it would press, best first -- the first the seat's layout allows is pressed -- where
    /// it heads (the controller's heading, radians from the facing; HasHeading false holds the facing), and why. No
    /// option at all is "nothing to press": the seat walks on with what it holds, and no press is hinted.
    struct Choice
    {
        std::array<Option, MAX_OPTIONS> Options{};
        uint32 Count = 0;
        bool HasHeading = false;
        float Heading = 0.0f;
        bool Waiting = false;           // held for something specific (the party, the pull, a cast)
        std::string Reason;

        void Add(Option const& option)
        {
            if (Count < MAX_OPTIONS)
                Options[Count++] = option;
        }
        void Head(float bearing)
        {
            HasHeading = true;
            Heading = bearing;
        }
    };

    /// The thresholds, as a player's party plays.
    constexpr float PULL_YARDS = 28.0f;         // the tank pulls the pack ahead from this close
    constexpr float READY_HEALTH = 0.7f;        // ... once every member has this much health
    constexpr float READY_MANA = 0.7f;          // ... and every member with mana this much mana
    constexpr float GATHER_YARDS = 20.0f;       // ... and is this near
    constexpr float WAIT_SECONDS = 60.0f;       // a minute with nothing gained: health and mana are waived
    constexpr float HARD_WAIT_SECONDS = 120.0f; // two: a member not back is waived too (never stands still for good)
    constexpr float FOLLOW_YARDS = 7.0f;        // out of a fight the others keep this close to the tank
    constexpr float FOLLOW_STOP_YARDS = 4.0f;   // ... stopping this close
    constexpr float LEASH_YARDS = 30.0f;        // in a fight they come back past this
    constexpr float REST_HEALTH = 0.6f;         // between pulls a seat eats below this ...
    constexpr float REST_MANA = 0.6f;           // ... and drinks below this
    constexpr float MELEE_YARDS = 4.5f;         // in reach
    constexpr float MELEE_STOP_YARDS = 3.0f;    // walking in, stop this close
    constexpr float CAST_BEYOND_YARDS = 28.0f;  // a ranged seat closes in from beyond this ...
    constexpr float CAST_STOP_YARDS = 24.0f;    // ... and stops inside this
    constexpr float HEAL_YARDS = 35.0f;         // a heal reaches this far (40 with a margin)
    constexpr float HEAL_FIGHT = 0.7f;          // in a fight the healer heals below this ...
    constexpr float HEAL_REST = 0.8f;           // ... and between pulls below this
    constexpr float LET_TANK_SECONDS = 3.0f;    // damage lets the tank take a fresh pull for this long
    constexpr float PULL_COMES_SECONDS = 4.0f;  // the tank lets what it pulled from range come to it this long
    constexpr float FACE_WITHIN = 0.6981317f;   // fighting: facing within this of the target is facing it
    constexpr float WALK_WITHIN = 1.5707963f;   // walking on: forward held while the way is within this of the facing
    constexpr float STOP_TO_TURN = 2.3561945f;  // ... and let go of beyond this, to turn on the spot

    /// Steer to a place `yards` away at `bearing` and stop within `stopYards`: head for it, and hold forward while it
    /// is ahead enough; once there, let go. Adds the press it needs now, if any, and sets the heading.
    void Steer(Facts const& facts, Choice& choice, float bearing, float yards, float stopYards);
    /// Turn to face `bearing`: sets the heading while not within FACE_WITHIN of it. Whether it turns.
    bool Face(Choice& choice, float bearing);
    /// Let go of forward, then of a strafe: whichever is held first. False when neither is.
    [[nodiscard]] bool Halt(Facts const& facts, uint32& key);

    /// The teacher's choice for a seat.
    [[nodiscard]] Choice Decide(Facts const& facts);

    /// Where a seat's layout keeps the actions the teacher presses: each block's first action and its count (0: the
    /// layout has no such block), and the block-relative actions it uses there.
    struct PressSpace
    {
        struct Range
        {
            uint32 First = 0;
            uint32 Count = 0;
        };
        Range Move;                 // MoveControls' actions
        Range Sight;                // five pointer groups (select, interact, use item, assist, focus), a slot
                                    // each, then clear focus
        Range Duel;
        Range Gauntlet;
        uint32 StartAttack = 0;
        uint32 PetAttack = 1;
        uint32 CallPet = 10;
        uint32 Eat = 0;
        uint32 Drink = 1;
    };
    /// A seat's first allowed spell of a kind, as a layout action; -1 none.
    using SpellPick = std::function<int32(Spell)>;

    /// **The press** (the hint, and a played seat's action): the choice's first option the space holds and `mask`
    /// allows, as a layout action. -1: none (the no-op is never a press).
    [[nodiscard]] int32 Press(Choice const& choice, PressSpace const& space, uint8 const* mask,
        SpellPick const& spells);

    /// Whether a ready check passes: every member alive, rested to READY_HEALTH and READY_MANA, and within
    /// GATHER_YARDS; the tank itself too. `why` says what it waits for; `hard` whether any of it is a member dead or
    /// beyond GATHER_YARDS (waived after HARD_WAIT_SECONDS), where health and mana are waived after WAIT_SECONDS.
    [[nodiscard]] bool Ready(Facts const& facts, std::string& why, bool& hard);
}

#endif
