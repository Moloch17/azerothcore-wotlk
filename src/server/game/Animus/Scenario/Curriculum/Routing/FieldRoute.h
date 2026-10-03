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

#ifndef ANIMUS_LIB_CURRICULUM_FIELD_ROUTE_H
#define ANIMUS_LIB_CURRICULUM_FIELD_ROUTE_H

#include "Define.h"
#include "Position.h"
#include <string>
#include <vector>

namespace Animus::Curriculum::FieldRoute
{
    /// The most a seat climbs from one yard of floor to the next (a slope, a stair); the move block's step.
    constexpr float MAX_CLIMB = 2.5f;
    /// The most a seat drops off a ledge onto the floor below. A player drops further, but past this the fall
    /// starts to hurt, and a route is the way a party walks, not the way it survives.
    constexpr float MAX_DROP = 8.0f;
    /// The room a seat needs above a floor to stand on it.
    constexpr float MIN_HEADROOM = 2.0f;

    /// **A way through the world as a seat walks it, from the layered field** (LayeredField::Store): yard by yard
    /// over the floors the field holds, a climb of up to MAX_CLIMB or a drop of up to MAX_DROP from one cell to the
    /// next, with room to stand, never on burning ground (magma, slime). It is the bots' own picture of the ground
    /// -- what their move block steers by -- so it goes where they can go, which the server's navmesh does not:
    /// Ragefire Chasm's mesh has no link down into its lower cavern that a player simply walks off a ledge into,
    /// and the parties stood at the top of the drop for the rest of the run (2026-10-02).
    ///
    /// `out` is the cells walked, one a yard, `from` first and the cell reaching `to` last. False when the field
    /// does not hold the ground (no field for a grid) or no way is found within `maxNodes` expanded cells; `out`
    /// is then empty. World thread.
    bool Plan(uint32 mapId, Position const& from, Position const& to, std::vector<Position>& out,
        uint32 maxNodes = 4000000);

    /// Whether the field holds the ground at (x, y) on `mapId`: a route can be planned there at all.
    [[nodiscard]] bool Covers(uint32 mapId, float x, float y);

    /// What Plan would say, for a console: the start's floors, whether one was taken, the cells expanded, and how
    /// near the goal the search came and where.
    [[nodiscard]] std::string Report(uint32 mapId, Position const& from, Position const& to);
}

#endif
