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

class Unit;

namespace Animus::Curriculum
{
    /// **What a director has said, and when it may speak** -- the rules the forge's DirectorEncounter and the module's
    /// companion party share, so a director trained in one behaves the same in the other. The members and enemies
    /// are given in slot order each time (the director's own observation's slots); a member's own order is kept by
    /// its slot.
    struct DirectorOrders
    {
        TeamPosture Posture = TeamPosture::Attack;
        TeamRally Rally = TeamRally::None;
        PlaceAnchor Anchor = PlaceAnchor::TeamCentre;
        /// Toward, not At: at the side's own centre a place is just where the side already is.
        PlaceOffset Offset = PlaceOffset::Toward;
        PlaceRing Ring = PlaceRing::Near;
        ObjectGuid Focus;

        /// Who the next order goes to: the side, a group (AddressGroup) or a member (AddressMember, a slot).
        OrderSource Address = OrderSource::Side;
        uint32 AddressGroup = 0;
        uint32 AddressMember = 0;

        /// The orders to members alone, by member slot.
        struct MemberOrder
        {
            OrderKind Kind = OrderKind::None;
            ObjectGuid Target;
            uint32 Objective = 0;
            OrderSource Source = OrderSource::Side;
            uint32 IssuedStep = 0;
        };
        std::array<MemberOrder, DirectorLayout::DIRECTOR_SEATS> Members{};

        /// The director's turn: calls left, whether an event opened it, when the clock gives the next, and what the
        /// events are measured against.
        uint32 CallsLeft = 0;
        bool ByEvent = false;
        uint32 NextClock = 0;
        std::array<uint8, DirectorLayout::DIRECTOR_SEATS> WasAlive{};
        std::array<uint8, DirectorLayout::DIRECTOR_SEATS> WasLow{};
        uint32 EnemiesAlive = 0;
        bool FocusWasAlive = false;

        uint32 Changes = 0;
        uint32 CalledStep = 0;
        uint32 Turns = 0;
        uint32 Calls = 0;
        uint32 MemberOrders = 0;
        /// Member orders that replaced a live one, and those of them that replaced one of the same source younger
        /// than HoldSteps: what the forge charges the director for (Director.OrderChange, Director.OrderChurn).
        uint32 HoldSteps = 8;
        uint32 Replaced = 0;
        uint32 Churned = 0;

        void Changed(uint32 step)
        {
            ++Changes;
            CalledStep = step;
        }
    };

    namespace DirectorRules
    {
        /// Whether the director gets a turn now: on its clock (every `clockDecisions`) or on an event -- a member
        /// down or newly below `lowHealth` of its health, a new living enemy, the focus dead -- with four calls for
        /// a group and eight for a raid, kept until spent or held. `step` is the decision count.
        void PrepareTurn(DirectorOrders& orders, uint32 step, Unit* const* members, uint32 memberCount,
            Unit* const* enemies, uint32 enemyCount, uint32 clockDecisions, float lowHealth);

        /// One call. `callable` holds, per enemy slot, the enemy a call may name (empty where the side does not
        /// believe one alive there); `members` the member slots in order. Off the director's turn nothing changes
        /// when `turns` (a learned director), and holding ends the turn.
        void Apply(DirectorOrders& orders, uint32 step, int32 action, Unit* const* members, uint32 memberCount,
            ObjectGuid const* callable, uint32 enemyCount, bool turns);

        /// Give an order to whoever is addressed; a member's own order outranks one to its group or the side.
        void Order(DirectorOrders& orders, uint32 step, uint32 memberCount, OrderKind kind, ObjectGuid target,
            uint32 objective);

        /// Whether member slot `slot` is the one addressed, or in the group addressed.
        [[nodiscard]] bool Addressed(DirectorOrders const& orders, uint32 slot);
    }
}

#endif
