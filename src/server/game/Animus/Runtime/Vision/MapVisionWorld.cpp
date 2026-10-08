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
#include "SceneRegistry.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DynamicObject.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "UnitBody.h"
#include <algorithm>
#include <mutex>
#include <set>

Animus::Vision::MapVisionWorld::MapVisionWorld(Map* map, uint32 phaseMask) : _map(map), _phaseMask(phaseMask),
    _query(map, phaseMask, false), _scene(SceneRegistry::Instance().Get(map->GetId()))
{
    if (_scene)
        return;
    // PrepareScenes bakes or loads every map a stage runs on and refuses to start without one: reaching here is an
    // ad hoc camera on a map no stage uses, which sees an empty world (and is told so once).
    static std::mutex warnLock;
    static std::set<uint32> warned;
    std::lock_guard lock(warnLock);
    if (warned.insert(map->GetId()).second)
        LOG_ERROR("module.animus", "No baked camera scene for map {}: its static world is not drawn", map->GetId());
}

Animus::Vision::SurfaceHit Animus::Vision::MapVisionWorld::StaticHit(Vec3 from, Vec3 to) const
{
    return _scene ? _scene->StaticHit(from, to) : SurfaceHit();
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
    return _scene ? _scene->ModelLiquid(from, to) : LiquidHit();
}

bool Animus::Vision::MapVisionWorld::StaticAnyHit(Vec3 from, Vec3 to) const
{
    return _scene && _scene->StaticAnyHit(from, to);
}

bool Animus::Vision::MapVisionWorld::DynamicAnyHit(Vec3 from, Vec3 to) const
{
    return _map->GetMapCollisionData().GetDynamicTree().AnyHit(_phaseMask, from.X, from.Y, from.Z, to.X, to.Y, to.Z);
}

Animus::Vision::TerrainTile Animus::Vision::MapVisionWorld::Tile(int32_t tileX, int32_t tileY) const
{
    return _scene ? _scene->Tile(tileX, tileY) : TerrainTile();
}

Animus::Vision::TerrainCell Animus::Vision::MapVisionWorld::Cell(int32_t tileX, int32_t tileY, int32_t cellX,
    int32_t cellY, bool liquid) const
{
    return _scene ? _scene->Cell(tileX, tileY, cellX, cellY, liquid) : TerrainCell();
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

    /// A game object's display bounds as a box candidate: scaled, turned by its rotation, about its position. The
    /// box's middle and half-extents are in the world's units, its axes (the rotation's columns) in Rot.
    bool BoxOf(GameObject const* object, Vi::SensorCandidate& box)
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
        G3D::Matrix3 const rotation = object->GetFinalWorldRotation().toRotationMatrix();
        float mid[3];
        for (int32 i = 0; i < 3; ++i)
        {
            mid[i] = 0.5f * (low[i] + high[i]);
            box.Half[i] = 0.5f * (high[i] - low[i]);
        }
        // The box's middle back in the world: the object's position plus its local middle through the rotation.
        for (int32 r = 0; r < 3; ++r)
            for (int32 c = 0; c < 3; ++c)
                box.Rot[r * 3 + c] = rotation[r][c];
        box.Centre = { object->GetPositionX() + box.Rot[0] * mid[0] + box.Rot[1] * mid[1] + box.Rot[2] * mid[2],
            object->GetPositionY() + box.Rot[3] * mid[0] + box.Rot[4] * mid[1] + box.Rot[5] * mid[2],
            object->GetPositionZ() + box.Rot[6] * mid[0] + box.Rot[7] * mid[1] + box.Rot[8] * mid[2] };
        return true;
    }
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

void Animus::Vision::GatherCandidates(Player* seat, Vec3 pivot, float range, GatheredSight& out)
{
    out.Candidates.clear();
    out.Objects.clear();
    if (!seat || !seat->IsInWorld())
        return;

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
            if (unit == seat || !within(unit) || !seat->CanSeeOrDetect(unit))
                return;
            Movement::Body const shape = Movement::ShapeOf(unit);
            SensorCandidate candidate;
            candidate.Shape = SensedShape::Unit;
            candidate.Guid = unit->GetGUID().GetRawValue();
            candidate.Feet = { unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ() };
            candidate.Radius = shape.Radius;
            candidate.Height = shape.Height;
            candidate.Centre = { candidate.Feet.X, candidate.Feet.Y, candidate.Feet.Z + 0.5f * shape.Height };
            out.Candidates.push_back(candidate);
            out.Objects.push_back(unit);
            return;
        }

        // A hostile ground effect (an area spell's persistent area): a disc at its radius.
        if (DynamicObject* area = object->ToDynObject())
        {
            if (!HostileGround(seat, area) || !within(area))
                return;
            SensorCandidate candidate;
            candidate.Shape = SensedShape::Hazard;
            candidate.Guid = area->GetGUID().GetRawValue();
            candidate.Feet = { area->GetPositionX(), area->GetPositionY(), area->GetPositionZ() };
            candidate.Radius = area->GetRadius();
            candidate.Height = HAZARD_THICKNESS;
            candidate.Centre = { candidate.Feet.X, candidate.Feet.Y,
                candidate.Feet.Z - HAZARD_SINK + 0.5f * HAZARD_THICKNESS };
            out.Candidates.push_back(candidate);
            out.Objects.push_back(area);
            return;
        }

        GameObject* go = object->ToGameObject();
        if (!go || !go->isSpawned() || !within(go) || !seat->CanSeeOrDetect(go))
            return;
        SensorCandidate candidate;
        candidate.Guid = go->GetGUID().GetRawValue();
        GameObjectModel const* model = go->m_model;
        if (model && model->isEnabled())
        {
            // In the dynamic tree: by its model's bounds; its own shadow rays do not stop at its surface.
            G3D::AABox const bounds = model->GetBounds();
            G3D::Vector3 const centre = bounds.center();
            G3D::Vector3 const extent = bounds.extent();
            candidate.Shape = SensedShape::Model;
            candidate.Model = model;
            candidate.Centre = { centre.x, centre.y, centre.z };
            for (int32 i = 0; i < 3; ++i)
                candidate.Half[i] = 0.5f * extent[i];
        }
        else
        {
            // Anything else (a herb, a chest, an open door) by its display's box.
            candidate.Shape = SensedShape::Box;
            if (!BoxOf(go, candidate))
                return;
        }
        candidate.Feet = { go->GetPositionX(), go->GetPositionY(), go->GetPositionZ() };
        candidate.Radius = std::max(candidate.Half[0], candidate.Half[1]);
        candidate.Height = 2.0f * candidate.Half[2];
        out.Candidates.push_back(candidate);
        out.Objects.push_back(go);
    };

    // Around the seat, out to the range plus the boom: the camera is never further than the zoom from the pivot.
    float const reach = range + Length(pivot - Vec3{ seat->GetPositionX(), seat->GetPositionY(),
        seat->GetPositionZ() });
    Acore::WorldObjectWorker<decltype(worker)> searcher(seat, worker,
        GRID_MAP_TYPE_MASK_CREATURE | GRID_MAP_TYPE_MASK_PLAYER | GRID_MAP_TYPE_MASK_GAMEOBJECT
        | GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
    Cell::VisitObjects(seat, searcher, reach);
}

void Animus::Vision::ClassifySeen(Player* seat, GatheredSight const& gathered, SensorOutput const& sensed,
    SeenList& seen)
{
    seen.Count = 0;
    // The seat's quest log is read once, for the first unit that needs it (a quest's kill targets).
    std::vector<uint32> killTargets;
    bool haveTargets = false;
    for (SensedEntity const& found : sensed.Seen)
    {
        if (seen.Count >= ENTITY_SLOTS || found.Index >= gathered.Candidates.size())
            break;
        SensorCandidate const& candidate = gathered.Candidates[found.Index];
        WorldObject* object = gathered.Objects[found.Index];
        EntityInfo info;
        info.Guid = candidate.Guid;
        info.Centre = candidate.Centre;
        info.Radius = candidate.Radius;
        info.Height = candidate.Height;
        info.Los = found.Los;
        switch (candidate.Shape)
        {
            case SensedShape::Unit:
            {
                Unit* unit = object->ToUnit();
                if (!unit)
                    continue;
                if (!haveTargets)
                {
                    killTargets = KillTargets(seat);
                    haveTargets = true;
                }
                info.Id = Classify(FactsOf(seat, unit, killTargets));
                info.Entry = unit->IsPlayer() ? 0 : unit->GetEntry();
                info.Level = float(unit->GetLevel());
                info.Health = unit->GetMaxHealth() ? float(unit->GetHealth()) / float(unit->GetMaxHealth()) : 0.0f;
                info.Reaction = seat->IsHostileTo(unit) ? -1 : (seat->IsFriendlyTo(unit) ? 1 : 0);
                info.Orientation = unit->GetOrientation();
                info.Dead = !unit->IsAlive();
                break;
            }
            case SensedShape::Hazard:
                info.Id.What = Class::GroundHazard;
                // Not a unit and not to be selected: listed as an object (the sight list masks select on one).
                info.GameObject = true;
                info.Reaction = -1;
                break;
            case SensedShape::Model:
            case SensedShape::Box:
            {
                GameObject* go = object->ToGameObject();
                if (!go)
                    continue;
                info.Id = Classify(FactsOf(seat, go));
                info.Entry = go->GetEntry();
                info.GameObject = true;
                info.Orientation = go->GetOrientation();
                bool const door = go->GetGoType() == GAMEOBJECT_TYPE_DOOR || go->GetGoType() == GAMEOBJECT_TYPE_BUTTON;
                // A door or button stands open in either of its active states (the cannon blows the Iron Clad Door
                // into the alternative one).
                info.Open = door ? go->GetGoState() != GO_STATE_READY : go->GetGoState() == GO_STATE_ACTIVE;
                info.Used = info.Open || go->getLootState() != GO_READY;
                break;
            }
        }
        seen.Info[seen.Count] = info;
        ++seen.Count;
    }
}
