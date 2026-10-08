/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef ANIMUS_VISION_MAP_VISION_WORLD_H
#define ANIMUS_VISION_MAP_VISION_WORLD_H

#include "Define.h"
#include "EntitySensor.h"
#include "Identity.h"
#include "MapWorldQuery.h"
#include "VisionCaster.h"
#include <vector>

class DynamicObject;

class GameObject;
class Map;
class Player;
class Unit;
class WorldObject;

/// The camera's VisionWorld over a live map: the static and dynamic collision trees cast apart (and tested for any
/// hit, for the entity sensor's shadow rays), the static tree's WMO liquids, the loaded grids' terrain cells and liquids as GridTerrainData holds them (never creating a grid: a grid
/// not created is where a ray leaves the world it can see), and floors through an uncounted MapWorldQuery (the
/// controller's cost line keeps only the controller's rays). A tree hit's slope is its triangle's own. Read from the map's own update, as
/// the rest of a seat's observation is.
namespace Animus::Vision
{
    class MapVisionWorld final : public VisionWorld
    {
    public:
        MapVisionWorld(Map* map, uint32 phaseMask) : _map(map), _phaseMask(phaseMask),
            _query(map, phaseMask, false) { }

        [[nodiscard]] SurfaceHit StaticHit(Vec3 from, Vec3 to) const override;
        [[nodiscard]] SurfaceHit DynamicHit(Vec3 from, Vec3 to) const override;
        [[nodiscard]] LiquidHit ModelLiquid(Vec3 from, Vec3 to) const override;
        [[nodiscard]] bool StaticAnyHit(Vec3 from, Vec3 to) const override;
        [[nodiscard]] bool DynamicAnyHit(Vec3 from, Vec3 to) const override;
        [[nodiscard]] TerrainTile Tile(int32_t tileX, int32_t tileY) const override;
        [[nodiscard]] TerrainCell Cell(int32_t tileX, int32_t tileY, int32_t cellX, int32_t cellY,
            bool liquid) const override;
        [[nodiscard]] Movement::Liquid LiquidAt(float x, float y, float z) const override;
        [[nodiscard]] float FloorBelow(float x, float y, float z, float search) const override;

    private:
        Map* _map;
        uint32 _phaseMask;
        Movement::MapWorldQuery _query;
    };

    /// **Who a seat's camera might see round it** (entity-sensing): the sensor's candidates, and the core object
    /// behind each (the same index), so the survivors can be classified. Valid for the one call chain that gathered
    /// them, on the seat's own map thread: the pointers are not kept.
    struct GatheredSight
    {
        std::vector<SensorCandidate> Candidates;
        std::vector<WorldObject*> Objects;
    };

    /// Everything the seat can see or detect within `range` of `pivot` (its head), and nothing more of it than the
    /// sensor needs to place it -- no facts, no class, no quest status, which cost most of the visit:
    /// - every creature and player it can see or detect, the dead included (a corpse is in the world, and in the
    ///   way), as a cylinder from its feet (the seat itself is not a candidate);
    /// - every spawned game object it can see: one with an enabled collision model (in the dynamic tree) by its
    ///   model's bounds; one with none, or a disabled one (an opened chest, an open door), by its display's
    ///   bounding box, turned as it is;
    /// - every hostile ground effect, as a disc.
    /// Visits the grid around the seat: on the seat's own map thread only (not under AnimusForge.ObserveAfterJoin).
    void GatherCandidates(Player* seat, Vec3 pivot, float range, GatheredSight& out);

    /// The entity list from what the sensor found: for each of `sensed` (at most ENTITY_SLOTS, nearest first), as
    /// this seat's client knows it (Classify over FactsOf: the class, the level, the nameplate, the quest marks, a
    /// door's state), with its size and line-of-sight share. Fills `seen`'s Count and Info; the rest is the caller's.
    void ClassifySeen(Player* seat, GatheredSight const& gathered, SensorOutput const& sensed, SeenList& seen);

    /// A ground effect this seat's camera draws as a hazard (Class::GroundHazard): an area spell's persistent area,
    /// harmful, its caster (when there is one) not friendly to the seat.
    [[nodiscard]] bool HostileGround(Player* seat, DynamicObject const* area);

    /// The facts this seat's client shows of a unit or a game object (the UI rule, perception-goals amendment 7).
    /// `killTargets`: the creature entries the seat's incomplete quests still need killed (KillTargets).
    [[nodiscard]] EntityFacts FactsOf(Player* seat, Unit* unit, std::vector<uint32> const& killTargets);
    [[nodiscard]] EntityFacts FactsOf(Player* seat, GameObject* object);
    [[nodiscard]] std::vector<uint32> KillTargets(Player* seat);
    /// Whether a quest giver status is a mark the client draws over the giver's head by default.
    [[nodiscard]] bool ShowsQuestMark(uint32 status);
}

#endif
