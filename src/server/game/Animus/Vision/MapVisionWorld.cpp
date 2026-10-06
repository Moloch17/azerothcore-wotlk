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

#include "MapVisionWorld.h"
#include "Cell.h"
#include "CellImpl.h"
#include "DBCStores.h"
#include "GridTerrainData.h"
#include "Map.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "UnitBody.h"

Animus::Vision::SurfaceHit Animus::Vision::MapVisionWorld::StaticHit(Vec3 from, Vec3 to) const
{
    SurfaceHit hit;
    float distance = 0.0f;
    float normalZ = 0.0f;
    if (_map->GetMapCollisionData().GetStaticTree().GetSurfaceHit(from.X, from.Y, from.Z, to.X, to.Y, to.Z, distance,
        normalZ))
    {
        hit.Distance = distance;
        hit.NormalZ = normalZ;
    }
    return hit;
}

Animus::Vision::SurfaceHit Animus::Vision::MapVisionWorld::DynamicHit(Vec3 from, Vec3 to) const
{
    SurfaceHit hit;
    float distance = 0.0f;
    float normalZ = 0.0f;
    if (_map->GetMapCollisionData().GetDynamicTree().GetSurfaceHit(_phaseMask, from.X, from.Y, from.Z, to.X, to.Y,
        to.Z, distance, normalZ))
    {
        hit.Distance = distance;
        hit.NormalZ = normalZ;
    }
    return hit;
}

Animus::Vision::LiquidHit Animus::Vision::MapVisionWorld::ModelLiquid(Vec3 from, Vec3 to) const
{
    LiquidHit hit;
    float distance = 0.0f;
    uint32 type = 0;
    if (!_map->GetMapCollisionData().GetStaticTree().GetLiquidHit(from.X, from.Y, from.Z, to.X, to.Y, to.Z, distance,
        type))
        return hit;
    hit.Distance = distance;
    // LiquidType.dbc's kind: 0 water, 1 ocean, 2 magma, 3 slime (the bits of MAP_LIQUID_TYPE_*).
    if (LiquidTypeEntry const* entry = sLiquidTypeStore.LookupEntry(type))
        hit.Deadly = entry->Type == 2 || entry->Type == 3;
    return hit;
}

Animus::Vision::TerrainTile Animus::Vision::MapVisionWorld::Tile(int32_t tileX, int32_t tileY) const
{
    TerrainTile tile;
    GridCoord const coord{ uint32(tileX), uint32(tileY) };
    tile.Loaded = _map->IsGridCreated(coord);
    if (!tile.Loaded)
        return tile;
    if (GridTerrainData const* terrain = _map->GetCreatedGridTerrainData(coord))
    {
        tile.Heights = terrain->HasHeights();
        tile.MaxHeight = terrain->GetMaxHeight();
        tile.Liquid = terrain->HasLiquid();
    }
    return tile;
}

Animus::Vision::TerrainCell Animus::Vision::MapVisionWorld::Cell(int32_t tileX, int32_t tileY, int32_t cellX,
    int32_t cellY, bool liquid) const
{
    TerrainCell cell;
    GridTerrainData const* terrain = _map->GetCreatedGridTerrainData(GridCoord(uint32(tileX), uint32(tileY)));
    if (!terrain)
        return cell;
    cell.Solid = terrain->GetCellHeights(cellX, cellY, cell.Corner, cell.Centre);
    if (liquid && terrain->HasLiquid())
    {
        // The cell's liquid, read at its centre (the level is the cell's, as getLiquidLevel reads it).
        float const x = WorldOfU(float(tileX * GRID_CELLS + cellX) + 0.5f);
        float const y = WorldOfU(float(tileY * GRID_CELLS + cellY) + 0.5f);
        uint32 flags = 0;
        cell.Liquid = terrain->GetLiquidSurface(x, y, cell.Level, flags);
        cell.Deadly = (flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
    }
    return cell;
}

Animus::Movement::Liquid Animus::Vision::MapVisionWorld::LiquidAt(float x, float y, float z) const
{
    return _query.LiquidAt(x, y, z);
}

float Animus::Vision::MapVisionWorld::FloorBelow(float x, float y, float z, float search) const
{
    return _query.FloorBelow(x, y, z, search);
}

void Animus::Vision::GatherUnits(Player* seat, Vec3 camera, float range, std::vector<UnitShape>& out)
{
    out.clear();
    if (!seat || !seat->IsInWorld())
        return;

    auto const worker = [&](WorldObject* object)
    {
        Unit* unit = object->ToUnit();
        if (!unit || !unit->IsInWorld())
            return;
        bool const self = unit == seat;
        float const dx = unit->GetPositionX() - camera.X;
        float const dy = unit->GetPositionY() - camera.Y;
        float const dz = unit->GetPositionZ() - camera.Z;
        if (!self && (dx * dx + dy * dy + dz * dz > range * range || !seat->CanSeeOrDetect(unit)))
            return;
        Movement::Body const shape = Movement::ShapeOf(unit);
        UnitShape entry;
        entry.X = unit->GetPositionX();
        entry.Y = unit->GetPositionY();
        entry.Z = unit->GetPositionZ();
        entry.Radius = shape.Radius;
        entry.Height = shape.Height;
        entry.What = !self && seat->IsHostileTo(unit) ? Class::HostileCreature : Class::NeutralCreature;
        entry.Self = self;
        out.push_back(entry);
    };

    // Around the seat, out to the range plus the boom: the camera is never further than the zoom from the pivot.
    float const reach = range + Length(camera - Vec3{ seat->GetPositionX(), seat->GetPositionY(),
        seat->GetPositionZ() });
    Acore::WorldObjectWorker<decltype(worker)> searcher(seat, worker,
        GRID_MAP_TYPE_MASK_CREATURE | GRID_MAP_TYPE_MASK_PLAYER);
    Cell::VisitObjects(seat, searcher, reach);
}
