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
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "UnitBody.h"

float Animus::Vision::MapVisionWorld::StaticHit(Vec3 from, Vec3 to) const
{
    return _query.StaticHit(from.X, from.Y, from.Z, to.X, to.Y, to.Z);
}

float Animus::Vision::MapVisionWorld::DynamicHit(Vec3 from, Vec3 to) const
{
    return _query.DynamicHit(from.X, from.Y, from.Z, to.X, to.Y, to.Z);
}

float Animus::Vision::MapVisionWorld::TerrainHeight(float x, float y) const
{
    return _query.TerrainHeight(x, y);
}

Animus::Movement::Liquid Animus::Vision::MapVisionWorld::LiquidAt(float x, float y, float z) const
{
    return _query.LiquidAt(x, y, z);
}

float Animus::Vision::MapVisionWorld::FloorBelow(float x, float y, float z, float search) const
{
    return _query.FloorBelow(x, y, z, search);
}

float Animus::Vision::MapVisionWorld::FloorNormalZ(float x, float y, float z) const
{
    return _query.FloorNormalZ(x, y, z);
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
        entry.Hostile = !self && seat->IsHostileTo(unit);
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
