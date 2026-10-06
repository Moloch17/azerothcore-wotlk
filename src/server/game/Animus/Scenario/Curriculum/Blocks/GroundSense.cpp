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

#include "GroundSense.h"
#include "DetourExtended.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include "MoveBlock.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace Animus::Curriculum::GroundSense
{
    dtPolyRef StartPoly(dtNavMeshQuery const* query, Origin const& at)
    {
        if (!query)
            return 0;

        // Extents match the core's own lookup in cs_mmaps; a place the mesh does not cover has no room measured.
        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);
        float const point[3] = { at.Y, at.Z, at.X };
        float const extents[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef startRef = 0;
        if (dtStatusFailed(query->findNearestPoly(point, extents, &filter, &startRef, nullptr)))
            return 0;
        return startRef;
    }

    Room MeasureRoom(dtNavMeshQuery const* query, dtPolyRef startRef, Origin const& at)
    {
        // How much room there is, and which way is out. The filter is the walkable set a seat actually uses, so a
        // lava edge and a shoreline both count as an edge to keep off -- which is the honest answer for something
        // on legs.
        Room room;
        if (!query || !startRef)
            return room;

        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(0);
        float const point[3] = { at.Y, at.Z, at.X };
        float distance = 0.0f;
        float hit[3] = { 0.0f, 0.0f, 0.0f };
        float normal[3] = { 0.0f, 0.0f, 0.0f };
        if (!dtStatusSucceed(query->findDistanceToWall(startRef, point, MoveBlock::CLEARANCE_RANGE, &filter,
            &distance, hit, normal)))
            return room;

        if (std::isfinite(distance))
            room.Clearance = std::clamp(distance / MoveBlock::CLEARANCE_RANGE, 0.0f, 1.0f);

        // hitNormal is normalize(centre - hit): it already points from the wall back at the seat, which is the
        // way out. Detour's axes are {y, z, x}, so the world components are [2] and [0].
        //
        // And it is not always a direction. Detour builds that vector by subtracting the hit from the centre and
        // normalising in place, dividing by the vector's own length -- so a seat standing exactly on an edge,
        // where the hit *is* the centre, normalises a zero vector and gets three NaNs. atan2 carries them, sin
        // and cos carry them, and two NaN observation planes reach the networks, which return a NaN logit, which
        // torch.multinomial reports as a probability tensor containing inf or nan, naming nothing. A seat on an
        // edge is not rare: it is a doorway.
        //
        // There is no direction out of a point that is already on the edge, so the honest reading is the one the
        // probe starts with -- no direction at all -- and the clearance distance beside it still says the seat
        // is against something.
        //
        // And it is only a direction when a wall was found at all. findDistanceToWall writes hitPos only inside
        // its loop, on a hit, but subtracts and normalises unconditionally at the end -- so with nothing in range
        // it normalises (centre - {0, 0, 0}), which is the bearing from the world origin to the seat. That is
        // finite, non-zero and completely wrong: it would hand the policy an absolute compass reading of where in
        // the world it is standing, in open country, where the held-out arenas exist precisely to prove it is
        // reading terrain and not remembering places. Nothing further out than the search radius has a way out
        // to point at.
        float const outX = normal[2];
        float const outY = normal[0];
        if (distance < MoveBlock::CLEARANCE_RANGE
            && std::isfinite(outX) && std::isfinite(outY) && outX * outX + outY * outY > 1e-6f)
        {
            room.Directed = true;
            room.Away = std::atan2(outY, outX);
        }
        return room;
    }
}
