/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_LIB_CURRICULUM_ROUTE_PLANNER_H
#define ANIMUS_LIB_CURRICULUM_ROUTE_PLANNER_H

#include "Define.h"
#include "Position.h"
#include <string>

class Map;

namespace Animus::Curriculum
{
    /// A way to somewhere: the corners a seat would actually turn at, and how far is left from each.
    ///
    /// Corners rather than a sampled line. PathGenerator's default smoothing walks the surface every four
    /// yards and then runs UpdateAllowedPositionZ -- a map height plus a vmap ray -- on every one of those
    /// points, which is why it is capped at 74 of them and why a route over about 292 yards silently becomes
    /// a straight line. A route only has to say which way to go next and how much is left, and the corner list
    /// findStraightPath already returns answers both, exactly, for far less.
    ///
    /// Remaining[i] is the length from corner i to the end, so the distance left from anywhere is
    /// dist(seat, corner[Next]) + Remaining[Next] -- arithmetic on cached numbers, no query between replans.
    /// That is what makes shaping on the real distance affordable rather than a per-decision pathfind.
    struct Route
    {
        /// 256 corners is not 256 yards. A corner is a turn, so open country spends very few and a cave system
        /// spends many: the cap is on how convoluted a route may be, not how long -- which is the distinction
        /// PathGenerator's point limit fails to make.
        static constexpr uint32 MAX_CORNERS = 256;

        float X[MAX_CORNERS] = {};
        float Y[MAX_CORNERS] = {};
        float Z[MAX_CORNERS] = {};
        float Remaining[MAX_CORNERS] = {};      // path length from this corner to the objective

        uint32 Count = 0;
        uint32 Next = 0;                        // the corner being walked towards
        float Length = 0.0f;                    // the whole route, corner to corner
        bool Complete = false;                  // it reaches the objective; false means it stops short
        Position To;                            // where it was planned to
        Position From;                          // and from where
        uint32 Ms = 0;                          // when
        bool Valid = false;

        void Clear() { *this = Route(); }

        /// Yards left to the objective from (x, y, z), walking towards corner Next.
        [[nodiscard]] float RemainingFrom(float x, float y, float z) const;

        /// Step Next past every corner already reached. No query: distances against what is cached.
        void Advance(float x, float y, float z, float reachedWithin);
    };

    /// Plans routes, and owns the query it plans them with.
    ///
    /// The core's navmesh query is deliberately not used. MapCollisionData says why in as many words --
    /// "navMeshQuery is not thread safe and needs its own instance per map" -- and it is shared with every
    /// creature pathing on that map during Map::Update. It is also built with a 1024-node pool, which is a
    /// ceiling on how far one search may look before it answers partially. A private query with a large pool
    /// removes the ceiling and the sharing together.
    ///
    /// **Thread safety is inherited, not enforced.** Callers must be on the world thread and outside
    /// MapMgr::Update -- which is where EnvPool observes, rewards and resets. Nothing here checks it.
    ///
    /// **What actually limits a long route is loaded mmap tiles**, measured rather than assumed. Detour can
    /// only walk mesh that is in memory, and tiles come in with their grids. Barrens to Durotar -- 2161 yd
    /// straight -- planned 1152 yd and stopped dead on a tile boundary; routing to that boundary first, which
    /// loads the grids, then re-planning the same pair reached 1459 yd and stopped on the next one. A 1095 yd
    /// route between two loaded points completes in 14 corners at 1.06x the straight line, so neither the node
    /// pool nor the corner cap is the constraint; the edge of the loaded world is.
    ///
    /// The consequence is a design one, not a bug: a journey is walked and re-planned, because the tiles ahead
    /// load as the traveller approaches them. A partial route is therefore the normal answer at range and not
    /// a failure -- which is why Plan returns true for one.
    class RoutePlanner
    {
    public:
        static RoutePlanner& Instance();

        /// A way from `from` to `to`, or false if there is none. `out` is cleared either way.
        ///
        /// A route that stops short still returns true with Complete false: that the way runs out after ninety
        /// yards is worth more than knowing nothing, and it is what lets a seat set off towards somewhere it
        /// cannot yet see a whole way to.
        bool Plan(Map* map, Position const& from, Position const& to, Route& out);

        /// What Plan would say, as a table, for a console to print. The bench for all of the above.
        std::string Report(Map* map, Position const& from, Position const& to);

    private:
        RoutePlanner() = default;
        ~RoutePlanner();
        RoutePlanner(RoutePlanner const&) = delete;
        RoutePlanner& operator=(RoutePlanner const&) = delete;
    };
}

#endif
