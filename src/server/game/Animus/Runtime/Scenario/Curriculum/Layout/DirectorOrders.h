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

#ifndef ANIMUS_LIB_CURRICULUM_DIRECTOR_ORDERS_H
#define ANIMUS_LIB_CURRICULUM_DIRECTOR_ORDERS_H

#include "DirectorLayout.h"
#include "ObjectGuid.h"
#include <array>

namespace Animus::Curriculum
{
    /// **What a director has said, and when it may speak** (DirectorEncounter): the standing order -- the party's
    /// posture, the member addressed, each member's own order -- and the director's turn. A member's order is kept by
    /// its seat.
    struct DirectorOrders
    {
        TeamPosture Posture = TeamPosture::Advance;
        /// Who the next order goes to (a seat index). The first seat at the start, so an early call is an order.
        uint32 Address = 0;

        struct MemberOrder
        {
            OrderKind Kind = OrderKind::None;
            ObjectGuid Target;
            uint32 IssuedMs = 0;
            uint32 IssuedStep = 0;
            /// How far it has got: the seat damaged the named enemy (Focus), the named one was in combat with the
            /// party (Pull), the member was below the low line since (Heal), the time within reach (Follow), and
            /// whether it was carried out this decision.
            bool Damaged = false;
            bool Engaged = false;
            bool WasLow = false;
            uint32 NearMs = 0;
            bool DoneNow = false;
            bool FailedNow = false;
        };
        std::array<MemberOrder, DirectorLayout::DIRECTOR_SEATS> Members{};

        /// The director's turn: calls left, whether an event opened it, when the clock gives the next, and what the
        /// events are measured against.
        uint32 CallsLeft = 0;
        bool ByEvent = false;
        uint32 NextClock = 0;
        std::array<uint8, DirectorLayout::DIRECTOR_SEATS> WasAlive{};
        std::array<uint8, DirectorLayout::DIRECTOR_SEATS> WasLow{};
        uint32 EnemiesOnParty = 0;
        /// An event the encounter raises itself (an order carried out or lapsed, a key looted): opens a turn.
        bool Event = false;
        uint32 LastCallMs = 0;

        // The episode's tally.
        uint32 Turns = 0;
        uint32 Calls = 0;
        uint32 Orders = 0;
        uint32 Replaced = 0;
        uint32 Churned = 0;
        uint32 Impossible = 0;
        uint32 Done = 0;
        uint32 Failed = 0;
        std::array<uint32, ORDER_KIND_COUNT> Given{};
        std::array<uint32, ORDER_KIND_COUNT> DoneOf{};
        uint32 LootUnordered = 0;
        uint32 GatesOpened = 0;
        uint32 PostureChanges = 0;
    };

    namespace DirectorRules
    {
        /// One call, decoded from the layout's action.
        struct Call
        {
            enum What : uint8 { None, Hold, Posture, Address, Clear, Order };
            What Kind = None;
            TeamPosture Stance = TeamPosture::Advance;  // Posture: the posture named
            uint32 Member = 0;              // Address: the member
            OrderKind Verb = OrderKind::None;           // Order: its kind ...
            uint32 Slot = 0;                // ... and the enemy, member or object slot it names
        };
        [[nodiscard]] Call Decode(int32 action);

        /// Whether the director gets a turn now: on its clock (every `clockDecisions`) or on an event -- a member
        /// down or newly below the low line, more enemies on the party, or one the encounter raised -- with `calls`
        /// calls, kept until spent or held. `step` is the decision count.
        void PrepareTurn(DirectorOrders& orders, uint32 step, bool const* alive, bool const* low, uint32 count,
            uint32 enemiesOnParty, uint32 clockDecisions, uint32 calls);

        /// A call spent (false: no turn, nothing spent). Hold ends the turn.
        bool Spend(DirectorOrders& orders, bool hold);

        /// Give `member` an order; what was there is replaced (counted, and as churn when younger than holdSteps).
        /// Returns whether a live order was replaced.
        bool Order(DirectorOrders& orders, uint32 step, uint32 ms, uint32 member, OrderKind kind, ObjectGuid target,
            uint32 holdSteps);
    }
}

#endif
