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

#ifndef ANIMUS_LIB_CURRICULUM_DIRECTOR_LAYOUT_H
#define ANIMUS_LIB_CURRICULUM_DIRECTOR_LAYOUT_H

#include "Aptitude.h"
#include "Block.h"
#include <array>
#include <boost/json/object.hpp>
#include <string>
#include <vector>

/*
 * **The director** (restored 2026-10-26; the user: "we do want a director for long term planning and group
 * coordination"): what the agent commanding a party sees and says. One layout, the same in every stage that has a
 * party, so nothing here names a class, a spell or a piece of content.
 *
 * The director says one thing per call. The standing order is state the director edits, and an action names the one
 * field it is changing -- "hold here", "you: loot that", "you: use that", "you: fight that one" -- so a call is an
 * order rather than a fresh opinion every decision. It speaks on its turns (its clock and the fight's events,
 * DirectorOrders), up to a few calls each, and holds otherwise: off its turn only the hold action is allowed, so
 * those decisions carry no choice.
 *
 * The party, the enemies and the objects near the party are sets: the learner reads each slot with one shared
 * encoder and pools them (DirectorSets), and scores the actions that name a slot -- address this member, fight that
 * one, loot that -- from the slot's own encoding (pointer logits), so an order follows the thing, not the slot it
 * landed in. Party-sized for now (five seats, eight enemies, four objects); the set encoding widens.
 */
namespace Animus::Curriculum::DirectorLayout
{
    constexpr uint32 DIRECTOR_SEATS = GROUP_SEATS;
    constexpr uint32 DIRECTOR_ENEMIES = 8;
    constexpr uint32 DIRECTOR_OBJECTS = 4;

    /// What an object near the party is, to the director: a door (opened by a hand or by its lever), a lever or
    /// button, a goober that takes something (the cannon), a chest that holds a key item (the gunpowder's barrel).
    enum ObjectKind : uint32
    {
        OBJECT_DOOR = 0,
        OBJECT_LEVER,
        OBJECT_GOOBER,
        OBJECT_KEY_CHEST,
        OBJECT_KINDS
    };

    /// Features a seat of the party contributes.
    enum SeatFeature : uint32
    {
        SEAT_PRESENT        = 0,
        SEAT_ALIVE,
        SEAT_HEALTH,
        SEAT_POWER,
        /// What its build can do, as the six-number brief of its Aptitude: who to give an order to is "the one whose
        /// build can hold this", not a label.
        SEAT_APTITUDE_FIRST,
        SEAT_IN_COMBAT      = SEAT_APTITUDE_FIRST + Aptitude::BRIEF_COUNT,
        SEAT_CASTING,
        SEAT_MOVING,
        SEAT_SPREAD,                // its distance from the party's centre / DISTANCE_SCALE
        SEAT_BEARING_SIN,           // and which way, about the party's axis (the leader's facing)
        SEAT_BEARING_COS,
        SEAT_LEADER,                // it leads the party (sets the pace)
        SEAT_ROLE_FIRST,            // one-hot: the party's tank, healer, a damage dealer (DungeonRole; none: all 0)
        SEAT_CARRIES_KEY    = SEAT_ROLE_FIRST + 3,  // it carries a key item a lock on the way takes
        SEAT_ADDRESSED,             // the next order goes to it
        SEAT_ATTACKED,              // enemies attacking it / ENEMY_COUNT_SCALE
        SEAT_ORDER_FIRST,           // one-hot over OrderKind: the order it holds
        SEAT_ORDER_AGE      = SEAT_ORDER_FIRST + ORDER_KIND_COUNT,     // seconds since / ORDER_AGE_SCALE
        SEAT_ORDER_NEAR,            // it is within reach of its order's target
        SEAT_ORDER_DONE,            // its order was carried out this decision
        SEAT_GOAL_FIRST,            // one-hot over SeatGoal: the primary goal it holds (none: all 0)
        SEAT_FEATURES       = SEAT_GOAL_FIRST + GOAL_COUNT
    };

    /// Features an enemy slot contributes: the env's target slots, in the order the party's pack block has them.
    enum EnemyFeature : uint32
    {
        ENEMY_PRESENT       = 0,
        ENEMY_ALIVE,
        ENEMY_HEALTH,
        ENEMY_IN_COMBAT,
        ENEMY_CASTING,
        ENEMY_ELITE,
        ENEMY_BOSS,
        ENEMY_LEVEL,                // its level less the party's / 10
        ENEMY_DISTANCE,             // from the party's centre / DISTANCE_SCALE
        ENEMY_BEARING_SIN,
        ENEMY_BEARING_COS,
        ENEMY_ON_PARTY,             // it is attacking a seat
        ENEMY_ORDERED,              // seats holding an order about it / DIRECTOR_SEATS
        ENEMY_FEATURES
    };

    /// Features an object slot contributes: what the party can use near it (InstanceEncounter's objects).
    enum ObjectFeature : uint32
    {
        OBJECT_PRESENT      = 0,
        OBJECT_KIND_FIRST,          // one-hot over ObjectKind
        OBJECT_OPEN         = OBJECT_KIND_FIRST + OBJECT_KINDS,     // open, used or looted already
        OBJECT_NEEDS_KEY,           // its lock takes a key item (the cannon the gunpowder)
        OBJECT_KEY_HELD,            // ... and a seat carries it
        OBJECT_DISTANCE,
        OBJECT_BEARING_SIN,
        OBJECT_BEARING_COS,
        OBJECT_IS_GATE,             // the way's next gate
        OBJECT_ORDERED,             // seats holding an order about it / DIRECTOR_SEATS
        OBJECT_FEATURES
    };

    enum Observation : uint32
    {
        OBS_ACTIVE              = 0,
        /// **The director may speak now** (MAY_CALL_COLUMN): it is given a turn on its clock and on events -- a member
        /// down or badly hurt, a new enemy on the party, an order carried out or lapsed, a key looted -- and keeps it
        /// for up to its budget of calls, or until it holds. Off its turn only the hold action is allowed.
        OBS_MAY_CALL,
        OBS_CALLS_LEFT,             // / Director.Calls
        OBS_BY_EVENT,               // this turn was opened by an event, not the clock
        OBS_EPISODE_TIME,
        OBS_PARTY_ALIVE,            // living seats / present seats
        OBS_PARTY_HEALTH,           // mean health of the living
        OBS_PARTY_POWER,            // mean power of the living mana users
        OBS_IN_COMBAT,              // any seat in combat
        OBS_ENEMIES_ALIVE,          // / DIRECTOR_ENEMIES
        OBS_ENEMIES_ON_PARTY,       // / DIRECTOR_ENEMIES
        OBS_SINCE_CALL,             // seconds since the last call / ORDER_AGE_SCALE
        OBS_ORDERS_STANDING,        // / DIRECTOR_SEATS
        OBS_KEY_HELD,               // a seat carries a key item
        OBS_GATE_AHEAD,             // the way's next gate is among the objects
        OBS_POSTURE_FIRST,          // one-hot over TeamPosture: the standing posture
        OBS_ADDRESS_FIRST       = OBS_POSTURE_FIRST + TEAM_POSTURE_COUNT,   // one-hot: the member addressed
        OBS_GLOBAL_COUNT        = OBS_ADDRESS_FIRST + DIRECTOR_SEATS,
        OBS_SEAT_FIRST          = OBS_GLOBAL_COUNT,
        OBS_ENEMY_FIRST         = OBS_SEAT_FIRST + DIRECTOR_SEATS * SEAT_FEATURES,
        OBS_OBJECT_FIRST        = OBS_ENEMY_FIRST + DIRECTOR_ENEMIES * ENEMY_FEATURES,
        OBS_COUNT               = OBS_OBJECT_FIRST + DIRECTOR_OBJECTS * OBJECT_FEATURES
    };

    constexpr uint32 MAY_CALL_COLUMN = OBS_MAY_CALL;

    /// One call. The posture is the party's; an address says who the orders after it go to; each order kind names
    /// its target by slot.
    enum Action : uint32
    {
        ACTION_HOLD             = 0,    // nothing more this turn
        ACTION_POSTURE_FIRST    = 1,
        ACTION_ADDRESS_FIRST    = ACTION_POSTURE_FIRST + TEAM_POSTURE_COUNT,    // + member slot
        ACTION_CLEAR            = ACTION_ADDRESS_FIRST + DIRECTOR_SEATS,         // the addressed one's order cleared
        ACTION_REST,                                                            // ... told to rest
        ACTION_FOCUS_FIRST,                                                     // + enemy slot
        ACTION_PULL_FIRST       = ACTION_FOCUS_FIRST + DIRECTOR_ENEMIES,
        ACTION_HEAL_FIRST       = ACTION_PULL_FIRST + DIRECTOR_ENEMIES,          // + member slot
        ACTION_FOLLOW_FIRST     = ACTION_HEAL_FIRST + DIRECTOR_SEATS,
        ACTION_LOOT_FIRST       = ACTION_FOLLOW_FIRST + DIRECTOR_SEATS,          // + object slot
        ACTION_USE_FIRST        = ACTION_LOOT_FIRST + DIRECTOR_OBJECTS,
        ACTION_COUNT            = ACTION_USE_FIRST + DIRECTOR_OBJECTS
    };

    constexpr float DISTANCE_SCALE = 100.0f;
    constexpr float ORDER_AGE_SCALE = 60.0f;    // seconds

    /// What the director is looking at. Built by DirectorEncounter; nothing here knows a class or a spell.
    struct DirectorView
    {
        bool Active = false;
        bool MayCall = false;
        uint32 CallsLeft = 0;
        uint32 CallsBudget = 1;
        bool ByEvent = false;
        float EpisodeTime = 0.0f;
        float PartyAlive = 0.0f;
        float PartyHealth = 0.0f;
        float PartyPower = 0.0f;
        bool InCombat = false;
        uint32 EnemiesAlive = 0;
        uint32 EnemiesOnParty = 0;
        float SinceCall = 0.0f;
        uint32 OrdersStanding = 0;
        bool KeyHeld = false;
        bool GateAhead = false;
        TeamPosture Posture = TeamPosture::Advance;
        uint32 Address = 0;

        struct SeatSlot
        {
            bool Present = false;
            bool Alive = false;
            float Health = 0.0f;
            float Power = 0.0f;
            Aptitude Apt;
            bool InCombat = false;
            bool Casting = false;
            bool Moving = false;
            float Spread = 0.0f;
            float BearingSin = 0.0f;
            float BearingCos = 0.0f;
            bool Leader = false;
            uint8 Role = 0;                     // DungeonRole: 0 any, 1 tank, 2 healer, 3 damage
            bool CarriesKey = false;
            bool Addressed = false;
            float Attacked = 0.0f;
            OrderKind Order = OrderKind::None;
            float OrderAge = 0.0f;
            bool OrderNear = false;
            bool OrderDone = false;
            int32 Goal = NO_GOAL;
        };

        struct EnemySlot
        {
            bool Present = false;
            bool Alive = false;
            float Health = 0.0f;
            bool InCombat = false;
            bool Casting = false;
            bool Elite = false;
            bool Boss = false;
            float Level = 0.0f;
            float Distance = 0.0f;
            float BearingSin = 0.0f;
            float BearingCos = 0.0f;
            bool OnParty = false;
            float Ordered = 0.0f;
        };

        struct ObjectSlot
        {
            bool Present = false;
            ObjectKind Kind = OBJECT_DOOR;
            bool Open = false;
            bool NeedsKey = false;
            bool KeyHeld = false;
            float Distance = 0.0f;
            float BearingSin = 0.0f;
            float BearingCos = 0.0f;
            bool IsGate = false;
            float Ordered = 0.0f;
        };

        std::array<SeatSlot, DIRECTOR_SEATS> Seats{};
        std::array<EnemySlot, DIRECTOR_ENEMIES> Enemies{};
        std::array<ObjectSlot, DIRECTOR_OBJECTS> Objects{};
    };

    /// The director's observation row and action mask. `obs` holds OBS_COUNT floats and `mask` ACTION_COUNT
    /// entries; both are cleared here. An inactive view, or one off its turn, leaves only the hold action.
    void Observe(DirectorView const& view, float* obs, uint8* mask);

    /// Every action's name, by index, for the manifest and the stage viewer.
    [[nodiscard]] std::vector<std::string> ActionNames();

    /// Every observation column's name, for the stage's obs_names (seeding by name).
    [[nodiscard]] std::vector<std::string> ColumnNames();

    /// Where the sets lie in the observation (first column, slots, per-slot width, the column that says a slot is
    /// present) and which actions are scored per slot of which set: stage.json's "director", for the learner's set
    /// encoder and pointer heads (DirectorSets).
    [[nodiscard]] boost::json::object SetDescriptor();

    /// The layout's name, the same in every stage that has a director.
    [[nodiscard]] char const* Name();
}

#endif
