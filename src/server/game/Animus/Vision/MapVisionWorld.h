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
#include "MapWorldQuery.h"
#include "VisionCaster.h"
#include <vector>

class Map;
class Player;

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

    /// The units a seat's camera can see: every creature and player within `range` of the camera that the seat can
    /// see or detect, the dead included (a corpse is in the world, and in the way), the seat itself marked Self.
    /// Visits the grid around the seat: on the seat's own map thread only (not under AnimusForge.ObserveAfterJoin).
    void GatherUnits(Player* seat, Vec3 camera, float range, std::vector<UnitShape>& out);
}

#endif
