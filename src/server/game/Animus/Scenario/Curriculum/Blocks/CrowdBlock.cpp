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

#include "CrowdBlock.h"
#include "MoveKeep.h"
#include "MoveSpline.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Item.h"
#include "Spell.h"
#include "Player.h"
#include "WorldActions.h"
#include "SeatView.h"
#include "EncoderSupport.h"
#include "MotionMaster.h"
#include <boost/json/object.hpp>
#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint32 ADVANCE_POINT_ID = 31;
    constexpr uint32 APPROACH_OBJECT_POINT_ID = 32;
    constexpr float ARRIVED_YARDS = 3.0f;

}

Animus::Curriculum::BlockSize Animus::Curriculum::CrowdBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::CrowdBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["slots"] = CROWD_SLOTS;
    block["slot_features"] = uint32(SLOT_FEATURES);
}

void Animus::Curriculum::CrowdBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    CrowdView const& crowd = view.Crowd;
    Player* bot = view.Bot;
    if (!crowd.Present || !bot)
        return;

    if (Unit const* tank = crowd.Tank; tank && tank != bot && tank->IsAlive() && tank->IsInMap(bot))
    {
        obs[OBS_TANK_PRESENT] = 1.0f;
        obs[OBS_TANK_DISTANCE] = std::min(1.0f, bot->GetExactDist(tank) / 100.0f);
        float const angle = bot->GetRelativeAngle(tank);
        obs[OBS_TANK_SIN] = std::sin(angle);
        obs[OBS_TANK_COS] = std::cos(angle);
        for (uint32 slot = 0; slot < view.EnemyCount && slot < NAMED_ENEMY_SLOTS; ++slot)
            if (view.Enemies[slot] && view.Enemies[slot] == tank->GetVictim())
                obs[OBS_TANK_TARGET_FIRST + slot] = 1.0f;
    }

    if (GameObject const* object = crowd.Object; object && object->IsInMap(bot))
    {
        obs[OBS_OBJECT_PRESENT] = 1.0f;
        obs[OBS_OBJECT_DISTANCE] = std::min(1.0f, bot->GetExactDist(object) / 40.0f);
        float const angle = bot->GetRelativeAngle(object);
        obs[OBS_OBJECT_SIN] = std::sin(angle);
        obs[OBS_OBJECT_COS] = std::cos(angle);
        obs[OBS_OBJECT_DOOR] = object->GetGoType() == GAMEOBJECT_TYPE_DOOR ? 1.0f : 0.0f;
        if (mask && bot->IsAlive() && !bot->IsInCombat() && CanUse(bot, object))
        {
            bool const inReach = bot->IsWithinDistInMap(object, WorldActions::INTERACT_YARDS);
            mask[ACTION_USE_OBJECT] = inReach ? 1 : 0;
            mask[ACTION_APPROACH_OBJECT] = inReach ? 0 : 1;
        }
    }
    if (mask && bot->IsAlive() && view.HasObjective && bot->GetExactDist(&view.Objective) > ARRIVED_YARDS
        && !bot->HasUnitState(Encoding::IMMOBILE_STATES))
        mask[ACTION_ADVANCE] = 1;

    obs[OBS_PRESENT] = 1.0f;
    obs[OBS_ON_PARTY] = std::min(2.0f, float(crowd.OnParty) / 8.0f);
    obs[OBS_ON_TANK] = std::min(2.0f, float(crowd.OnTank) / 8.0f);
    obs[OBS_LOOSE] = std::min(2.0f, float(crowd.OnParty - std::min(crowd.OnParty, crowd.OnTank)) / 8.0f);
    obs[OBS_ELITES] = std::min(2.0f, float(crowd.Elites) / 4.0f);
    obs[OBS_UNSEEN] = std::min(2.0f, float(crowd.OnParty - std::min(crowd.OnParty, PACK_SLOTS)) / 4.0f);
    obs[OBS_IS_TANK] = crowd.Tank == bot ? 1.0f : 0.0f;
    obs[OBS_BEHIND] = crowd.Behind ? 1.0f : 0.0f;
    obs[OBS_AHEAD_DISTANCE] = 1.0f;
    if (crowd.HasAhead)
    {
        obs[OBS_AHEAD_DISTANCE] = std::min(1.0f, bot->GetExactDist(&crowd.Ahead) / 60.0f);
        obs[OBS_AHEAD_SIZE] = std::min(2.0f, float(crowd.AheadSize) / 6.0f);
        float const angle = bot->GetRelativeAngle(&crowd.Ahead);
        obs[OBS_AHEAD_SIN] = std::sin(angle);
        obs[OBS_AHEAD_COS] = std::cos(angle);
    }

    obs[OBS_STILL] = std::clamp(crowd.Still, 0.0f, 1.0f);
    obs[OBS_SECOND_GAP] = 1.0f;
    obs[OBS_SECOND_DISTANCE] = 1.0f;
    if (crowd.HasSecond)
    {
        obs[OBS_SECOND_PRESENT] = 1.0f;
        if (crowd.HasAhead)
            obs[OBS_SECOND_GAP] = std::min(1.0f, crowd.Ahead.GetExactDist(&crowd.Second) / 40.0f);
        obs[OBS_SECOND_DISTANCE] = std::min(1.0f, bot->GetExactDist(&crowd.Second) / 60.0f);
        float const angle = bot->GetRelativeAngle(&crowd.Second);
        obs[OBS_SECOND_SIN] = std::sin(angle);
        obs[OBS_SECOND_COS] = std::cos(angle);
    }
    // How near the seat is to being noticed: the pack ahead (within a pack's reach of it), and anything else idle in
    // the pack block's slots or past them.
    {
        constexpr float PACK_REACH = 12.0f;
        constexpr float MARGIN_SCALE = 20.0f;
        float ahead = MARGIN_SCALE;
        float other = MARGIN_SCALE;
        auto const consider = [&](Unit const* unit)
        {
            Creature const* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsInCombat() || !creature->IsInMap(bot)
                || !creature->IsHostileTo(bot))
                return;
            float const margin = bot->GetExactDist(creature) - creature->GetAggroRange(bot);
            bool const ofAhead = crowd.HasAhead && creature->GetExactDist(&crowd.Ahead) <= PACK_REACH;
            float& nearest = ofAhead ? ahead : other;
            nearest = std::min(nearest, margin);
        };
        for (uint32 slot = 0; slot < view.EnemyCount && slot < PACK_SLOTS; ++slot)
            consider(view.Enemies[slot]);
        for (uint32 slot = 0; slot < crowd.Count && slot < CROWD_SLOTS; ++slot)
            consider(crowd.Units[slot]);
        obs[OBS_AHEAD_MARGIN] = std::clamp(ahead / MARGIN_SCALE, -1.0f, 1.0f);
        obs[OBS_SECOND_MARGIN] = std::clamp(other / MARGIN_SCALE, -1.0f, 1.0f);
    }

    for (uint32 slot = 0; slot < crowd.Count && slot < CROWD_SLOTS; ++slot)
    {
        Unit* unit = crowd.Units[slot];
        if (!unit || !unit->IsAlive() || !unit->IsInMap(bot))
            continue;
        float* features = obs + OBS_SLOT_FIRST + slot * SLOT_FEATURES;
        features[SLOT_PRESENT] = 1.0f;
        features[SLOT_HEALTH] = unit->GetHealthPct() / 100.0f;
        features[SLOT_DISTANCE] = std::min(1.0f, bot->GetExactDist(unit) / 40.0f);
        float const angle = bot->GetRelativeAngle(unit);
        features[SLOT_SIN] = std::sin(angle);
        features[SLOT_COS] = std::cos(angle);
        features[SLOT_IN_COMBAT] = unit->IsInCombat() ? 1.0f : 0.0f;
        Unit const* victim = unit->GetVictim();
        features[SLOT_ON_ME] = victim == bot ? 1.0f : 0.0f;
        features[SLOT_ON_TANK] = victim && victim == crowd.Tank ? 1.0f : 0.0f;
        Creature const* creature = unit->ToCreature();
        features[SLOT_ELITE] = creature && creature->isElite() ? 1.0f : 0.0f;
    }
}

namespace
{
    /// Walk the advance's run (SeatView::CrowdView::Path) and remember it, so BeforeApply can carry it on.
    void WalkAdvance(Animus::Curriculum::SeatView& view)
    {
        Player* bot = view.Bot;
        std::vector<G3D::Vector3> points;
        for (uint32 i = 0; i < view.Crowd.PathPoints; ++i)
            points.emplace_back(view.Crowd.Path[i].GetPositionX(), view.Crowd.Path[i].GetPositionY(),
                view.Crowd.Path[i].GetPositionZ());
        Animus::Curriculum::Encoding::WalkPath(bot, points);
        if (view.Steering)
            view.Steering->AdvanceRunId = bot->movespline->GetId();
    }
}

void Animus::Curriculum::CrowdBlock::BeforeApply(SeatView& view, SeatActionResult& /*result*/) const
{
    // An advance under way is carried on before it arrives (movement-smooth A8): with a couple of decisions of its
    // run left, out of a fight, the next run along the route is launched from where it is, so the party walks the
    // route without a stop at each run's end -- and neither the policy nor the dungeon script has to press it again.
    // Any other move (a different run), a fight, or the objective reached ends it.
    Player* bot = view.Bot;
    if (!bot || !view.Steering || !view.Steering->AdvanceRunId || !bot->IsAlive())
        return;
    if (bot->movespline->Finalized() || bot->movespline->GetId() != view.Steering->AdvanceRunId || bot->IsInCombat()
        || !view.Crowd.HasStep || !view.Crowd.PathPoints || Encoding::CastHoldsFeet(bot))
    {
        if (bot->movespline->Finalized() || bot->movespline->GetId() != view.Steering->AdvanceRunId)
            view.Steering->AdvanceRunId = 0;
        return;
    }
    G3D::Vector3 const end = bot->movespline->FinalDestination();
    float const remaining = bot->GetExactDist(end.x, end.y, end.z);
    if (MoveKeep::CoastsTooFar(remaining, bot->movespline->Velocity(), view.DecisionMs))
        return;
    WalkAdvance(view);
}

void Animus::Curriculum::CrowdBlock::Apply(SeatView& view, uint32 local, SeatActionResult& /*result*/) const
{
    Player* bot = view.Bot;
    if (!bot || !bot->IsAlive())
        return;
    if (local == ACTION_ADVANCE)
    {
        // Along the dungeon's field route a few yards at a time, straight: it is ground the seat can walk, where the
        // server's navmesh may not join it (a drop into a cavern). Off the route, the server's path.
        // Corner to corner along the route, as one run (movement-smooth A8), carried on in BeforeApply.
        if (view.Crowd.HasStep && view.Crowd.PathPoints)
            WalkAdvance(view);
        // The server's path to the objective -- not past a closed door, which a spline would walk through.
        else if (view.HasObjective && !view.Crowd.AtDoor)
            Encoding::MoveTo(bot, ADVANCE_POINT_ID, view.Objective.GetPositionX(), view.Objective.GetPositionY(),
                view.Objective.GetPositionZ());
        return;
    }
    GameObject* object = view.Crowd.Object;
    if (bot->IsInCombat() || !object || !object->IsInMap(bot))
        return;
    if (local == ACTION_APPROACH_OBJECT)
    {
        Encoding::MoveTo(bot, APPROACH_OBJECT_POINT_ID, object->GetPositionX(), object->GetPositionY(),
            object->GetPositionZ());
        return;
    }
    if (local != ACTION_USE_OBJECT || !bot->IsWithinDistInMap(object, WorldActions::INTERACT_YARDS)
        || !CanUse(bot, object))
        return;
    if (view.Crowd.Used)
        view.Crowd.Used->push_back(object->GetGUID());
    // As a player does it: a chest is looted (the gunpowder), a lock that takes a key gets the key's own use (the
    // gunpowder on the cannon, whose script answers that spell), and anything else is a right-click -- a lever or a
    // button runs what it is linked to, a door opens.
    if (object->GetGoType() == GAMEOBJECT_TYPE_CHEST)
    {
        uint32 items = 0;
        uint32 copper = 0;
        WorldActions::LootAll(bot, object, items, copper);
        return;
    }
    if (uint32 const key = KeyOf(object))
    {
        if (Item* carried = bot->GetItemByEntry(key))
        {
            SpellCastTargets targets;
            targets.SetGOTarget(object);
            bot->CastItemUseSpell(carried, targets, 1, 0);
        }
        return;
    }
    object->Use(bot);
}

std::string Animus::Curriculum::CrowdBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    switch (local)
    {
        case ACTION_USE_OBJECT:         return "use_object";
        case ACTION_ADVANCE:            return "advance";
        case ACTION_APPROACH_OBJECT:    return "approach_object";
        default:                        return {};
    }
}

/// The item a lock is opened with (LOCK_KEY_ITEM), or 0: the Deadmines' cannon takes the Defias Gunpowder.
uint32 Animus::Curriculum::CrowdBlock::KeyOf(GameObject const* object)
{
    LockEntry const* lock = sLockStore.LookupEntry(object->GetGOInfo()->GetLockId());
    if (!lock)
        return 0;
    for (uint32 i = 0; i < MAX_LOCK_CASE; ++i)
        if (lock->Type[i] == LOCK_KEY_ITEM && lock->Index[i])
            return lock->Index[i];
    return 0;
}

/// Whether `bot` can use `object` as it stands: a key it needs is carried.
bool Animus::Curriculum::CrowdBlock::CanUse(Player const* bot, GameObject const* object)
{
    uint32 const key = KeyOf(object);
    return !key || bot->HasItemCount(key, 1);
}
