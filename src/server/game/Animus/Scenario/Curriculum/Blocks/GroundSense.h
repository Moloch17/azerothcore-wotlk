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

#ifndef ANIMUS_CURRICULUM_GROUND_SENSE_H
#define ANIMUS_CURRICULUM_GROUND_SENSE_H

#include "Define.h"
#include "DetourNavMesh.h"

class Map;
class dtNavMeshQuery;

/// How much room a seat has on the navmesh, as a function of a place rather than of a seat: what is left of the move
/// block's ground probe once its rays went (move block revision 4). Its one caller is the travel encounter's
/// clearance charge (TravelEncounter's SeatClearance), only while that charge is on; nothing in a seat's
/// observation queries the navmesh.
namespace Animus::Curriculum::GroundSense
{
    /// Where a measurement is taken from.
    struct Origin
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        uint32 Phase = 1;
        float Collision = 2.0f;         // the height of whoever stands there (liquid depth is judged against it)
    };

    /// How much room there is, and which way is out, in the world's frame.
    struct Room
    {
        float Clearance = 1.0f;         // yards to the nearest edge of walkable space / CLEARANCE_RANGE
        bool Directed = false;          // whether there is a way out to point at
        float Away = 0.0f;              // and its world angle when there is
    };

    /// The seat's own polygon; 0 when the mesh does not cover it.
    dtPolyRef StartPoly(dtNavMeshQuery const* query, Origin const& at);

    Room MeasureRoom(dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at);
}

#endif
