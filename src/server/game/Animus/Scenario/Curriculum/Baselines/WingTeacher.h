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
 * **The dungeon teacher** (dungeon-curriculum I6): a party clearing a dungeon pack by pack, as players do, played on
 * the player controller and the sight block's presses -- the same hands and feet a learned seat has.
 *
 * The tank leads along the wing's route, holds the pull until every member is up (health and mana) and with it, pulls
 * one pack at a time and takes whatever hits somebody else (a taunt), with its threat abilities before plain damage;
 * the healer keeps the party up within its mana, its heals at the friend it focuses; damage assists the tank's
 * target, lets the tank take it first and stops casts it can; between pulls everyone eats and drinks. A dead seat
 * rises at the entrance (EntranceRespawn) and the teacher walks it back along the route. Doors, levers and the
 * Deadmines' cannon are used with a real press: interact (CMSG_GAMEOBJ_USE) or the key item on it (CMSG_USE_ITEM).
 *
 * **What it may know** is a script's: the route the planner laid (navmesh and field), where the party and the enemies
 * are, who is on whom. **What it may do** is a player's: held keys (MoveControls), a sight-list press on an entity the
 * seat sees or remembers (EntityActions: select, interact, use an item on, assist, focus), a spell (sent through the
 * client's handler, CastThroughClient), eat or drink, and start the auto attack. Never a spline, a teleport, a
 * server-side cast or an object opened for it. An entity the seat has not seen cannot be pressed: the teacher turns
 * to it first.
 *
 * This is the brain, with no world in it: a seat's Facts in, a Choice out, so a test can drive it. The adapter
 * (StageScenario::TeachSeat) gathers the facts and turns the choice into one layout action.
 */
namespace Animus::Curriculum::WingTeacher
{
    enum class Role : uint8
    {
        Tank,
        Healer,
        Damage
    };

    /// Something at a place, from the seat: how far, which way in the seat's own frame (radians, + left, as a held
    /// turn rate is), and the sight list's slot that names it (-1: the seat neither sees nor remembers it).
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
        bool PetClass = false;          // its class fights with a pet (a hunter's beast, a warlock's demon, ...)
        bool PetOut = false;            // ... and a living one is out
        bool PetOnTarget = false;       // ... attacking the seat's selection
        float FightSeconds = 0.0f;      // since the seat entered its current combat

        /// The keys held (the controller's ControlState).
        int8 Forward = 0;
        int8 Strafe = 0;
        float TurnRate = 0.0f;

        std::array<Member, PARTY_OTHERS> Party{};
        uint32 PartyCount = 0;
        std::array<Enemy, ENEMIES> Enemies{};
        uint32 EnemyCount = 0;

        /// The party's tank, for every seat but the tank: present while it lives.
        Place Tank;
        /// Where the route takes this seat next (the script's path along the planned route; never a bot input): the
        /// tank's way on, everyone else's way to the tank, a risen seat's way back from the entrance.
        Place Step;
        /// The next pack, the tank's to pull: its nearest member not yet in a fight.
        Place Pull;
        bool PullSelected = false;      // ... and it is the seat's selection
        /// What opens the way on (a door, a lever, the cannon), seen near: use it.
        Place Object;
        bool ObjectNeedsKey = false;    // used with the key item it takes (the cannon's gunpowder), not a hand
        float ObjectReach = 5.0f;
        /// A tank risen at the entrance, its party still deep in: back along the route before anything else.
        bool Behind = false;
        /// The party's time with no progress (the ready check's valve).
        float StillSeconds = 0.0f;
    };

    /// The spell a choice asks for: the adapter finds the seat's first allowed one of the kind (Baselines::SpellFor).
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
        PetAttack,      // the pet bar's Attack at the selection (CMSG_PET_ACTION in a sight stage)
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

    /// The teacher's choice: what it would press, best first -- the first the seat's layout allows is pressed -- and
    /// why. None at all is "nothing to press": the seat waits or walks on with what it holds, and no hint is given.
    struct Choice
    {
        std::array<Option, MAX_OPTIONS> Options{};
        uint32 Count = 0;
        bool Waiting = false;           // held for something specific (the party, the pull, a cast)
        std::string Reason;

        void Add(Option const& option)
        {
            if (Count < MAX_OPTIONS)
                Options[Count++] = option;
        }
    };

    /// The thresholds, as a player's party plays.
    constexpr float PULL_YARDS = 28.0f;         // the tank pulls the pack ahead from this close
    constexpr float READY_HEALTH = 0.7f;        // ... once every member has this much health
    constexpr float READY_MANA = 0.7f;          // ... and every member with mana this much mana
    constexpr float GATHER_YARDS = 20.0f;       // ... and is this near
    constexpr float WAIT_SECONDS = 60.0f;       // a minute with nothing gained: health and mana are waived
    constexpr float FOLLOW_YARDS = 7.0f;        // out of a fight the others keep this close to the tank
    constexpr float FOLLOW_STOP_YARDS = 4.0f;   // ... stopping this close
    constexpr float LEASH_YARDS = 30.0f;        // in a fight they come back past this
    constexpr float REST_HEALTH = 0.7f;         // between pulls a seat eats below this ...
    constexpr float REST_MANA = 0.6f;           // ... and drinks below this
    constexpr float MELEE_YARDS = 4.5f;         // in reach
    constexpr float MELEE_STOP_YARDS = 3.0f;    // walking in, stop this close
    constexpr float CAST_BEYOND_YARDS = 28.0f;  // a ranged seat closes in from beyond this ...
    constexpr float CAST_STOP_YARDS = 24.0f;    // ... and stops inside this
    constexpr float HEAL_YARDS = 35.0f;         // a heal reaches this far (40 with a margin)
    constexpr float HEAL_FIGHT = 0.6f;          // in a fight the healer heals below this ...
    constexpr float HEAL_REST = 0.8f;           // ... and between pulls below this
    constexpr float LET_TANK_SECONDS = 3.0f;    // damage lets the tank take a fresh pull for this long
    constexpr float PULL_COMES_SECONDS = 4.0f;  // the tank lets what it pulled from range come to it this long
    constexpr float TURN_WITHIN = 0.3926991f;   // steering: nearer than this to straight ahead, stop turning
    constexpr float FACE_WITHIN = 0.6981317f;   // fighting: facing within this of the target is facing it
    /// The decision the turn rate is chosen for: the rate whose swing over one decision best fits the error.
    constexpr float DECISION_SECONDS = 0.25f;

    /// Steer to a place `yards` away at `bearing` and stop within `stopYards`: turn toward it (a held turn rate, let
    /// go of near straight ahead), then hold forward; once there, let go of forward. The press it needs now, or none
    /// (already doing it).
    [[nodiscard]] bool Steer(Facts const& facts, float bearing, float yards, float stopYards, uint32& key);
    /// Turn to face `bearing` (within FACE_WITHIN), and stop turning once facing.
    [[nodiscard]] bool Face(Facts const& facts, float bearing, uint32& key);
    /// Let go of forward, then of a strafe, then of the turn: whichever is held first. None when nothing is.
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
        uint32 StartAttack = 0;     // DuelBlock::ACTION_START_ATTACK, ACTION_PET_ATTACK, ACTION_CALL_BEAST_FIRST
        uint32 PetAttack = 1;
        uint32 CallPet = 10;
        uint32 Eat = 0;             // GauntletBlock::ACTION_EAT, ACTION_DRINK
        uint32 Drink = 1;
    };
    /// A seat's first allowed spell of a kind (Baselines::SpellFor), as a layout action; -1 none.
    using SpellPick = std::function<int32(Spell)>;

    /// **The press** (the hint, and a played seat's action): the choice's first option the space holds and `mask`
    /// allows, as a layout action. Only a held key (the move block), a sight-list press (the sight block), a spell
    /// `spells` picks, the auto attack, or eat or drink -- there is nothing else it can name. -1: none (the no-op is
    /// never a press).
    [[nodiscard]] int32 Press(Choice const& choice, PressSpace const& space, uint8 const* mask,
        SpellPick const& spells);

    /// **The stand-in's hands** (dungeon-curriculum I7's seam): the same choice with every key taken out -- its feet are
    /// its style's -- so it selects, assists, focuses, casts and uses objects through the sight block as the teacher
    /// does, and never targets server-side.
    [[nodiscard]] Choice Hands(Facts const& facts);

    /// Whether a ready check passes: every member alive, rested to READY_HEALTH and READY_MANA, and within
    /// GATHER_YARDS; the tank itself too. `why` says what it waits for; `hard` whether any of it is a member dead,
    /// out of sight of the party or beyond GATHER_YARDS -- which no wait waives (the dead rejoin), where health and
    /// mana are waived after WAIT_SECONDS with nothing gained (no drinks left, a caster that never drank).
    [[nodiscard]] bool Ready(Facts const& facts, std::string& why, bool& hard);
}

#endif
