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
#include "Identity.h"
#include "MapWorldQuery.h"
#include "VisionCaster.h"
#include <vector>

class GameObject;
class Map;
class Player;
class Unit;

/// The camera's VisionWorld over a live map: the static and dynamic collision trees cast apart, the static tree's WMO
/// liquids, the loaded grids' terrain cells and liquids as GridTerrainData holds them (never creating a grid: a grid
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

    /// **What a seat's camera can see round it** (perception-goals 1a and 1b): the shapes the caster reads and what
    /// each numbered entity is. Entities[n] is entity number n's (1 to MAX_SEEN; [0] is unused).
    struct SightStore
    {
        std::vector<UnitShape> Units;
        std::vector<BoxShape> Boxes;
        std::vector<DoorShape> Doors;
        std::vector<EntityInfo> Entities;

        [[nodiscard]] Sight View() const
        {
            Sight sight;
            sight.Units = Units;
            sight.Boxes = Boxes;
            sight.Doors = Doors;
            return sight;
        }
    };

    /// The entities a seat's camera can see, within `range` of `pivot` (its head), as this seat's client knows them:
    /// - every creature and player it can see or detect, the dead included (a corpse is in the world, and in the
    ///   way), the seat itself marked Self, each a cylinder of its class (Classify over FactsOf);
    /// - every spawned game object it can see: one with an enabled collision model (in the dynamic tree) by its
    ///   model (a DoorShape); one with none, or a disabled one (an opened chest, an open door: still drawn by the
    ///   client), by its display's bounding box (a BoxShape) -- an open door by the band at the top of its frame
    ///   (OpenDoorBox: the doorway clear under it), listed with Open set;
    /// - numbered nearest the head first (NumberNearest), MAX_SEEN of them at most, each number's EntityInfo kept.
    /// Visits the grid around the seat: on the seat's own map thread only (not under AnimusForge.ObserveAfterJoin).
    void GatherSight(Player* seat, Vec3 pivot, float range, SightStore& out);

    /// The facts this seat's client shows of a unit or a game object (the UI rule, perception-goals amendment 7).
    /// `killTargets`: the creature entries the seat's incomplete quests still need killed (KillTargets).
    [[nodiscard]] EntityFacts FactsOf(Player* seat, Unit* unit, std::vector<uint32> const& killTargets);
    [[nodiscard]] EntityFacts FactsOf(Player* seat, GameObject* object);
    [[nodiscard]] std::vector<uint32> KillTargets(Player* seat);
    /// Whether a quest giver status is a mark the client draws over the giver's head by default.
    [[nodiscard]] bool ShowsQuestMark(uint32 status);
}

#endif
