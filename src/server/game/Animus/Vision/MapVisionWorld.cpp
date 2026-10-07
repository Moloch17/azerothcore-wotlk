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
#include "Creature.h"
#include "DynamicObject.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GridTerrainData.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
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
    GameObjectModel const* model = nullptr;
    if (_map->GetMapCollisionData().GetDynamicTree().GetSurfaceHit(_phaseMask, from.X, from.Y, from.Z, to.X, to.Y,
        to.Z, distance, normalZ, &model))
    {
        hit.Distance = distance;
        hit.NormalZ = normalZ;
        hit.Object = model;
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

bool Animus::Vision::ShowsQuestMark(uint32 status)
{
    // The yellow and blue "!" and "?", and the grey "?" of a quest under way; not the grey "!" of one not yet
    // available, nor the low-level marks the client hides unless asked to track them.
    switch (status)
    {
        case DIALOG_STATUS_INCOMPLETE:
        case DIALOG_STATUS_REWARD_REP:
        case DIALOG_STATUS_AVAILABLE_REP:
        case DIALOG_STATUS_AVAILABLE:
        case DIALOG_STATUS_REWARD2:
        case DIALOG_STATUS_REWARD:
            return true;
        default:
            return false;
    }
}

std::vector<uint32> Animus::Vision::KillTargets(Player* seat)
{
    std::vector<uint32> entries;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = seat->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || seat->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        for (uint8 objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
        {
            int32 const entry = quest->RequiredNpcOrGo[objective];
            if (entry > 0 && seat->GetReqKillOrCastCurrentCount(questId, entry)
                < quest->RequiredNpcOrGoCount[objective])
                entries.push_back(uint32(entry));
        }
    }
    return entries;
}

Animus::Vision::EntityFacts Animus::Vision::FactsOf(Player* seat, Unit* unit, std::vector<uint32> const& killTargets)
{
    EntityFacts facts;
    facts.Player = unit->IsPlayer();
    facts.Dead = !unit->IsAlive();
    facts.Reaction = seat->IsHostileTo(unit) ? -1 : (seat->IsFriendlyTo(unit) ? 1 : 0);
    Creature* creature = unit->ToCreature();
    if (!creature)
        return facts;
    // The sparkle the core sends this seat alone (Unit::BuildValuesUpdate: UNIT_DYNFLAG_LOOTABLE only for a player
    // allowed to loot it).
    facts.LootableByMe = facts.Dead && creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)
        && seat->isAllowedToLoot(creature);
    facts.Vendor = creature->IsVendor();
    facts.Trainer = creature->IsTrainer();
    facts.Interactive = creature->GetNpcFlags() != UNIT_NPC_FLAG_NONE;
    if (!facts.Dead && creature->IsQuestGiver())
        facts.QuestMark = ShowsQuestMark(uint32(seat->GetQuestDialogStatus(creature)));
    facts.QuestTarget = std::find(killTargets.begin(), killTargets.end(), creature->GetEntry()) != killTargets.end();
    return facts;
}

Animus::Vision::EntityFacts Animus::Vision::FactsOf(Player* seat, GameObject* object)
{
    EntityFacts facts;
    facts.GameObject = true;
    GameObjectTemplate const* info = object->GetGOInfo();
    facts.ObjectType = uint32(object->GetGoType());
    if (LockEntry const* lock = sLockStore.LookupEntry(info->GetLockId()))
        for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_SKILL && (lock->Index[i] == LOCKTYPE_HERBALISM
                || lock->Index[i] == LOCKTYPE_MINING))
            {
                facts.LockSkill = lock->Index[i];
                break;
            }
    facts.QuestRelevant = object->ActivateToQuest(seat);
    if (facts.ObjectType == GAMEOBJECT_TYPE_QUESTGIVER)
        facts.QuestMark = ShowsQuestMark(uint32(seat->GetQuestDialogStatus(object)));

    // The cog cursor: the kinds a click does something with, selectable, and not withheld by a condition the seat
    // does not meet (GO_FLAG_INTERACT_COND lifts for the quest that needs it).
    bool usableKind = false;
    switch (object->GetGoType())
    {
        case GAMEOBJECT_TYPE_DOOR:
        case GAMEOBJECT_TYPE_BUTTON:
        case GAMEOBJECT_TYPE_QUESTGIVER:
        case GAMEOBJECT_TYPE_CHEST:
        case GAMEOBJECT_TYPE_BINDER:
        case GAMEOBJECT_TYPE_CHAIR:
        case GAMEOBJECT_TYPE_TEXT:
        case GAMEOBJECT_TYPE_GOOBER:
        case GAMEOBJECT_TYPE_CAMERA:
        case GAMEOBJECT_TYPE_FISHINGNODE:
        case GAMEOBJECT_TYPE_SUMMONING_RITUAL:
        case GAMEOBJECT_TYPE_MAILBOX:
        case GAMEOBJECT_TYPE_GUARDPOST:
        case GAMEOBJECT_TYPE_SPELLCASTER:
        case GAMEOBJECT_TYPE_MEETINGSTONE:
        case GAMEOBJECT_TYPE_FLAGSTAND:
        case GAMEOBJECT_TYPE_FISHINGHOLE:
        case GAMEOBJECT_TYPE_FLAGDROP:
        case GAMEOBJECT_TYPE_BARBER_CHAIR:
        case GAMEOBJECT_TYPE_GUILD_BANK:
        case GAMEOBJECT_TYPE_TRAPDOOR:
            usableKind = true;
            break;
        default:
            break;
    }
    facts.Usable = usableKind && !object->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE)
        && (!object->HasGameObjectFlag(GO_FLAG_INTERACT_COND) || facts.QuestRelevant);
    facts.Lootable = facts.ObjectType == GAMEOBJECT_TYPE_CHEST && object->getLootState() == GO_READY && facts.Usable;
    return facts;
}

namespace
{
    namespace Vi = Animus::Vision;

    /// A game object's display bounds as its box (BoxShape): scaled, turned by its rotation, about its position.
    bool BoxOf(GameObject const* object, Vi::BoxShape& box)
    {
        GameObjectDisplayInfoEntry const* display = sGameObjectDisplayInfoStore.LookupEntry(object->GetDisplayId());
        if (!display)
            return false;
        float const scale = object->GetObjectScale();
        float const low[3] = { display->minX * scale, display->minY * scale, display->minZ * scale };
        float const high[3] = { display->maxX * scale, display->maxY * scale, display->maxZ * scale };
        for (int32 i = 0; i < 3; ++i)
            if (!(high[i] > low[i]))
                return false;
        box.X = object->GetPositionX();
        box.Y = object->GetPositionY();
        box.Z = object->GetPositionZ();
        // The rotation's inverse is its transpose: row r of InvRot is column r of the rotation.
        G3D::Matrix3 const rotation = object->GetFinalWorldRotation().toRotationMatrix();
        for (int32 r = 0; r < 3; ++r)
            for (int32 c = 0; c < 3; ++c)
                box.InvRot[r * 3 + c] = rotation[c][r];
        for (int32 i = 0; i < 3; ++i)
        {
            box.Low[i] = low[i];
            box.High[i] = high[i];
        }
        return true;
    }

    /// One entity before it is numbered: which shape it is, and its distance from the head.
    struct Candidate
    {
        enum class Shape : uint8 { Unit, Box, Door } Of;
        std::size_t Index;
        Vi::EntityInfo Info;
    };
}

bool Animus::Vision::HostileGround(Player* seat, DynamicObject const* area)
{
    // An area spell's persistent area that would hurt this seat: harmful, its caster not on the seat's side. The
    // seat's own and its friends' consecrations are somewhere to stand, not to leave.
    if (!seat || !area || !area->IsInWorld() || area->GetRadius() <= 0.0f
        || area->GetByteValue(DYNAMICOBJECT_BYTES, 0) != DYNAMIC_OBJECT_AREA_SPELL)
        return false;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(area->GetSpellId());
    Unit* caster = area->GetCaster();
    return info && !info->IsPositive() && (!caster || !seat->IsFriendlyTo(caster));
}

void Animus::Vision::GatherSight(Player* seat, Vec3 pivot, float range, SightStore& out)
{
    out.Units.clear();
    out.Boxes.clear();
    out.Doors.clear();
    out.Entities.assign(1, EntityInfo());
    if (!seat || !seat->IsInWorld())
        return;

    std::vector<uint32> const killTargets = KillTargets(seat);
    thread_local std::vector<Candidate> candidates;
    candidates.clear();
    float const range2 = range * range;
    auto const within = [&](WorldObject const* object)
    {
        float const dx = object->GetPositionX() - pivot.X;
        float const dy = object->GetPositionY() - pivot.Y;
        float const dz = object->GetPositionZ() - pivot.Z;
        return dx * dx + dy * dy + dz * dz <= range2;
    };

    auto const worker = [&](WorldObject* object)
    {
        if (!object->IsInWorld())
            return;
        if (Unit* unit = object->ToUnit())
        {
            bool const self = unit == seat;
            if (!self && (!within(unit) || !seat->CanSeeOrDetect(unit)))
                return;
            Movement::Body const shape = Movement::ShapeOf(unit);
            UnitShape entry;
            entry.X = unit->GetPositionX();
            entry.Y = unit->GetPositionY();
            entry.Z = unit->GetPositionZ();
            entry.Radius = shape.Radius;
            entry.Height = shape.Height;
            entry.Self = self;
            if (!self)
            {
                Candidate candidate{ Candidate::Shape::Unit, out.Units.size(), EntityInfo() };
                EntityInfo& info = candidate.Info;
                info.Id = Classify(FactsOf(seat, unit, killTargets));
                info.Entry = unit->IsPlayer() ? 0 : unit->GetEntry();
                info.Level = float(unit->GetLevel());
                info.Health = unit->GetMaxHealth() ? float(unit->GetHealth()) / float(unit->GetMaxHealth()) : 0.0f;
                info.Reaction = seat->IsHostileTo(unit) ? -1 : (seat->IsFriendlyTo(unit) ? 1 : 0);
                info.Centre = { entry.X, entry.Y, entry.Z + 0.5f * entry.Height };
                info.Guid = unit->GetGUID().GetRawValue();
                info.Orientation = unit->GetOrientation();
                info.Dead = !unit->IsAlive();
                entry.What = info.Id.What;
                candidates.push_back(candidate);
            }
            out.Units.push_back(entry);
            return;
        }

        // A hostile ground effect (an area spell's persistent area): drawn as its visual, a disc at its radius.
        if (DynamicObject* area = object->ToDynObject())
        {
            if (!HostileGround(seat, area) || !within(area))
                return;
            UnitShape const disc = HazardDisc(area->GetPositionX(), area->GetPositionY(), area->GetPositionZ(),
                area->GetRadius());
            Candidate candidate{ Candidate::Shape::Unit, out.Units.size(), EntityInfo() };
            EntityInfo& info = candidate.Info;
            info.Id.What = Class::GroundHazard;
            // Not a unit and not to be selected: listed as an object (the sight list masks select on one).
            info.GameObject = true;
            info.Reaction = -1;
            info.Centre = { disc.X, disc.Y, disc.Z + 0.5f * disc.Height };
            info.Guid = area->GetGUID().GetRawValue();
            info.Radius = disc.Radius;
            candidates.push_back(candidate);
            out.Units.push_back(disc);
            return;
        }

        GameObject* go = object->ToGameObject();
        if (!go || !go->isSpawned() || !within(go) || !seat->CanSeeOrDetect(go))
            return;
        EntityInfo info;
        info.Id = Classify(FactsOf(seat, go));
        info.Entry = go->GetEntry();
        info.GameObject = true;
        info.Guid = go->GetGUID().GetRawValue();
        info.Orientation = go->GetOrientation();
        info.Open = go->GetGoState() == GO_STATE_ACTIVE;
        info.Used = info.Open || go->getLootState() != GO_READY;
        GameObjectModel const* model = go->m_model;
        bool const door = go->GetGoType() == GAMEOBJECT_TYPE_DOOR || go->GetGoType() == GAMEOBJECT_TYPE_BUTTON;
        if (model && model->isEnabled())
        {
            DoorShape shape;
            shape.Model = model;
            shape.X = go->GetPositionX();
            shape.Y = go->GetPositionY();
            shape.Z = go->GetPositionZ();
            shape.What = info.Id.What;
            G3D::Vector3 const centre = model->GetBounds().center();
            info.Centre = { centre.x, centre.y, centre.z };
            candidates.push_back({ Candidate::Shape::Door, out.Doors.size(), info });
            out.Doors.push_back(shape);
            return;
        }
        // An open door or button is out of the way; anything else is drawn by its box.
        if (model && door)
            return;
        BoxShape box;
        if (!BoxOf(go, box))
            return;
        box.What = info.Id.What;
        // The box's middle, back in the world (its space's middle through the rotation, the transpose of InvRot).
        float const mid[3] = { 0.5f * (box.Low[0] + box.High[0]), 0.5f * (box.Low[1] + box.High[1]),
            0.5f * (box.Low[2] + box.High[2]) };
        info.Centre = { box.X + box.InvRot[0] * mid[0] + box.InvRot[3] * mid[1] + box.InvRot[6] * mid[2],
            box.Y + box.InvRot[1] * mid[0] + box.InvRot[4] * mid[1] + box.InvRot[7] * mid[2],
            box.Z + box.InvRot[2] * mid[0] + box.InvRot[5] * mid[1] + box.InvRot[8] * mid[2] };
        candidates.push_back({ Candidate::Shape::Box, out.Boxes.size(), info });
        out.Boxes.push_back(box);
    };

    // Around the seat, out to the range plus the boom: the camera is never further than the zoom from the pivot.
    float const reach = range + Length(pivot - Vec3{ seat->GetPositionX(), seat->GetPositionY(),
        seat->GetPositionZ() });
    Acore::WorldObjectWorker<decltype(worker)> searcher(seat, worker,
        GRID_MAP_TYPE_MASK_CREATURE | GRID_MAP_TYPE_MASK_PLAYER | GRID_MAP_TYPE_MASK_GAMEOBJECT
        | GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
    Cell::VisitObjects(seat, searcher, reach);

    // Numbered nearest the head first: the frame's slots go in that order.
    thread_local std::vector<float> distances;
    thread_local std::vector<uint8> numbers;
    distances.resize(candidates.size());
    numbers.resize(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        Vec3 const offset = candidates[i].Info.Centre - pivot;
        distances[i] = Dot(offset, offset);
    }
    NumberNearest(distances, numbers);
    out.Entities.resize(std::min<std::size_t>(candidates.size(), MAX_SEEN) + 1);
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        uint8 const number = numbers[i];
        Candidate const& candidate = candidates[i];
        switch (candidate.Of)
        {
            case Candidate::Shape::Unit:
                out.Units[candidate.Index].Entity = number;
                break;
            case Candidate::Shape::Box:
                out.Boxes[candidate.Index].Entity = number;
                break;
            case Candidate::Shape::Door:
                out.Doors[candidate.Index].Entity = number;
                break;
        }
        if (number)
            out.Entities[number] = candidate.Info;
    }
}
