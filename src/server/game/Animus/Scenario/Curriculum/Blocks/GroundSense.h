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

/// What the move block's ground probe measures, as functions of a place and a heading rather than of a seat.
///
/// The probe used to take the Player it was describing; nothing it asked of the player but where it stood, its
/// phase and its height. Taking those as numbers is what lets the same measurement run anywhere the geometry is
/// loaded: live for a seat, and offline for every cell of a map (the probe bake), so a baked table and a live
/// fallback are one definition, not two that are hoped to agree.
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

    /// The height march along one heading: how far it got, what stopped it.
    struct March
    {
        float Reach = 1.0f;             // yards walked before something stopped it / MARCH_MAX
        float Step = 0.0f;              // the height change that stopped it (or the steepest walked), / allowance
        float Water = 0.0f;             // 1 when it crossed swimmable water
        float Burns = 0.0f;             // 1 when it stopped at magma or slime
    };

    /// The three navmesh rays along one heading, in yards; negative is no answer (off the mesh, a failed query).
    struct Rays
    {
        float Wet = -1.0f;              // ground and water
        float Dry = -1.0f;              // ground alone
        float All = -1.0f;              // anything liquid, magma and slime included
    };

    /// One bearing as the observation carries it: GroundProbe's Reach, Step, Shore and Burns.
    struct Bearing
    {
        float Reach = 1.0f;
        float Step = 0.0f;
        float Shore = 1.0f;
        float Burns = 0.0f;
    };

    /// How much room there is, and which way is out, in the world's frame.
    struct Room
    {
        float Clearance = 1.0f;         // yards to the nearest edge of walkable space / CLEARANCE_RANGE
        bool Directed = false;          // whether there is a way out to point at
        float Away = 0.0f;              // and its world angle when there is
    };

    /// The seat's own polygon, for every ray from `at`; 0 when the mesh does not cover it.
    dtPolyRef StartPoly(dtNavMeshQuery const* query, Origin const& at);

    /// Yards along `heading` before the mesh refuses `includeFlags`, up to `range`; negative is no answer.
    float NavRay(dtNavMeshQuery const* query, dtPolyRef startRef, float x, float y, float z, float heading,
        float range, uint16 includeFlags);

    /// The height march. `pitch` 0 is the five cells of MoveBlock::MARCH_RANGES; above 0 it samples every `pitch`
    /// yards out to MARCH_MAX, judging each sample against the one before for a step and against the ground
    /// MARCH_WINDOW behind it for a slope, so a gap narrower than a cell cannot be stepped over.
    March MarchBearing(Map* map, Origin const& at, float heading, float pitch);

    Rays CastRays(dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at, float heading);

    /// The march and the rays made one bearing: the nearer of the two senses wins.
    Bearing Combine(March const& march, Rays const& rays);

    /// One bearing, whole: the march and the rays along `heading`.
    Bearing Sense(Map* map, dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at, float heading,
        float pitch);

    /// A wedge `halfWidth` either side of `heading`, sampled by `rays` headings across it: the worst of them, so
    /// a pillar between two rays is not invisible. Reach and Shore are the least, Burns the most, and Step
    /// comes from the ray that set the Reach. `rays` 1 is Sense at `heading`.
    Bearing SenseWedge(Map* map, dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at, float heading,
        float halfWidth, uint32 rays, float pitch);

    /// The worse of two readings of one bearing, by SenseWedge's rules.
    Bearing Worst(Bearing const& a, Bearing const& b);

    Room MeasureRoom(dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at);

    /// How far behind a dense march's sample the slope is judged from: the first legacy cell's gap, so a dense
    /// march admits the same climb over the same distance as the five cells did.
    constexpr float MARCH_WINDOW = 6.0f;
}

#endif
