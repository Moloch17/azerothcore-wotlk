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

#include "GoalBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "Player.h"
#include "SeatView.h"
#include <boost/json/array.hpp>
#include <algorithm>
#include <cmath>
#include <string>

Animus::Curriculum::BlockSize Animus::Curriculum::GoalBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, 0 };
}

namespace
{
    /// A goal target's name for the column names: none, enemy_0.., friend_0.., place_0.., assignment.
    std::string TargetName(uint32 target)
    {
        using namespace Animus::Curriculum;
        if (target == GOAL_TARGET_NONE)
            return "none";
        if (target < GOAL_TARGET_FRIEND_FIRST)
            return "enemy_" + std::to_string(target - GOAL_TARGET_ENEMY_FIRST);
        if (target < GOAL_TARGET_PLACE_FIRST)
            return "friend_" + std::to_string(target - GOAL_TARGET_FRIEND_FIRST);
        if (target < GOAL_TARGET_ASSIGNMENT)
            return "place_" + std::to_string(target - GOAL_TARGET_PLACE_FIRST);
        return "assignment";
    }
}

void Animus::Curriculum::GoalBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    // In the order of Obs, so a name stays with its meaning across a revision (bootstrap seeds by name).
    for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
        names.emplace_back("goal_kind_" + std::string(GoalName(SeatGoal(kind))));
    for (uint32 target = 0; target < GOAL_TARGETS; ++target)
        names.emplace_back("goal_target_" + TargetName(target));
    for (char const* name : { "goal_ended", "goal_reached", "goal_secondary_ended", "goal_event" })
        names.emplace_back(name);
    for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
        names.emplace_back("goal_achieved_kind_" + std::string(GoalName(SeatGoal(kind))));
    for (uint32 target = 0; target < GOAL_TARGETS; ++target)
        names.emplace_back("goal_achieved_target_" + TargetName(target));
    // Revision 3, appended: the held goal's place, then each place slot's features.
    for (char const* name : { "goal_held_present", "goal_held_sin", "goal_held_cos", "goal_held_dist",
        "goal_held_near" })
        names.emplace_back(name);
    for (uint32 slot = 0; slot < GOAL_PLACE_SLOTS; ++slot)
        for (char const* feature : { "sin", "cos", "dist", "cover", "age" })
            names.emplace_back("goal_place_" + std::to_string(slot) + "_" + feature);
    // Revision 4, appended: the secondary hold's point, the plan's next step, the plan left, the choice frame.
    for (char const* group : { "goal_held2_", "goal_next_" })
        for (char const* feature : { "present", "sin", "cos", "dist", "near" })
            names.emplace_back(std::string(group) + feature);
    for (char const* name : { "goal_plan_left", "goal_from_present", "goal_from_row", "goal_from_col" })
        names.emplace_back(name);
}

void Animus::Curriculum::GoalBlock::Available(SeatView const& view, std::array<bool, GOAL_COUNT>& kinds,
    std::array<bool, GOAL_TARGETS>& targets)
{
    kinds.fill(false);
    targets.fill(false);
    targets[GOAL_TARGET_NONE] = true;
    Player* bot = view.Bot;
    if (!bot || !bot->IsAlive())
    {
        kinds[uint32(SeatGoal::Fight)] = true;
        return;
    }

    // Enemies: the pack's named slots (the ones that matter most), or the one target of a stage without them.
    uint32 enemies = 0;
    if (view.EnemyCount)
    {
        for (uint32 slot = 0; slot < view.EnemyCount && slot < NAMED_ENEMY_SLOTS; ++slot)
            if (view.Enemies[slot] && view.Enemies[slot]->IsAlive())
            {
                targets[GOAL_TARGET_ENEMY_FIRST + slot] = true;
                ++enemies;
            }
    }
    else if (view.Target && view.Target->IsAlive())
    {
        targets[GOAL_TARGET_ENEMY_FIRST] = true;
        enemies = 1;
    }

    // Friends other than the seat itself: its own health is Recover's.
    bool friends = false;
    for (uint32 slot = FRIEND_OWNER; slot < FRIEND_SLOTS; ++slot)
        if (Unit* friendUnit = Encoding::FriendUnit(view, slot); friendUnit && friendUnit != bot && friendUnit->IsAlive())
        {
            targets[GOAL_TARGET_FRIEND_FIRST + slot] = true;
            friends = true;
        }

    // A dungeon's seen places (WorldView::HasSeenPlaces): its places and the assignment. A stage without them (the
    // movement stages) has a trip's objective, which takes the assignment's slot, so TravelTo has a target there too.
    WorldView const& world = view.World;
    bool places = false;
    if (world.CellGoals)
    {
        // Free choice goals: one place target, about a block of the seat's own crop, offered while the crop has one a
        // goal can name (MapBlock::Observe counted them this decision, or the last, before the goal loop).
        places = targets[GOAL_CELL_TARGET] = view.Crop && view.Crop->Choosable > 0;
    }
    else if (world.RoomGoals)
    {
        // The seek stage's room goals: a room not yet checked and the way on, six and one of the places. A room just
        // checked is not offered (it is the goal just reached, held for one observation). No assignment.
        for (uint32 i = 0; i <= WorldView::WAY_ON_SLOT; ++i)
            if (world.Places[i].Present && !world.Places[i].Done)
                places = targets[GOAL_TARGET_PLACE_FIRST + i] = true;
    }
    else
    {
        for (uint32 i = 0; i < WorldView::JOURNAL_PLACES && world.HasSeenPlaces; ++i)
            if (world.Places[i].Present)
                places = targets[GOAL_TARGET_PLACE_FIRST + i] = true;
        if (world.HasSeenPlaces && world.HasAssignment)
            places = targets[GOAL_TARGET_ASSIGNMENT] = true;
    }
    if (!world.HasSeenPlaces && view.HasObjective)
        places = targets[GOAL_TARGET_ASSIGNMENT] = true;

    bool const combat = bot->IsInCombat();
    bool const hurt = bot->GetHealthPct() < 95.0f
        || (bot->GetMaxPower(POWER_MANA) && bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA) * 95 / 100);
    kinds[uint32(SeatGoal::Fight)] = true;                  // always: a seat can always mean to fight
    kinds[uint32(SeatGoal::Control)] = enemies >= 2;
    kinds[uint32(SeatGoal::Recover)] = hurt;
    kinds[uint32(SeatGoal::Protect)] = friends;
    kinds[uint32(SeatGoal::Position)] = enemies > 0;
    kinds[uint32(SeatGoal::Prepare)] = !combat;
    kinds[uint32(SeatGoal::TravelTo)] = places;
    kinds[uint32(SeatGoal::Rest)] = !combat && hurt;
    // Room goals: a place to go to is the one kind that is offered. Fight about no one stays only as the placeholder
    // for "no plan yet", while there is no place.
    if (world.RoomGoals)
    {
        kinds.fill(false);
        kinds[uint32(SeatGoal::TravelTo)] = places;
    }

    // A kind with no target it accepts is not on offer after all.
    for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
    {
        if (!kinds[kind])
            continue;
        bool any = false;
        for (uint32 target = 0; target < GOAL_TARGETS && !any; ++target)
            any = targets[target] && GoalAccepts(SeatGoal(kind), target);
        kinds[kind] = any;
    }
    kinds[uint32(SeatGoal::Fight)] = !(world.RoomGoals && places);  // about no one in particular: always accepted
}

bool Animus::Curriculum::GoalBlock::PlaceOf(SeatView const& view, uint32 t, Position& where, uint32 slot)
{
    WorldView const& world = view.World;
    if (world.CellGoals && t == GOAL_CELL_TARGET)
        return slot < view.HeldCell.size() && view.HeldCell[slot].Valid && (where = view.HeldCell[slot].Where, true);
    if (!world.HasSeenPlaces)
    {
        // A trip's objective, where there is no dungeon's route (Available) -- and only where the seat is told where it is
        // (SeatView::ObjectivePlaceKnown): with the compass withheld, or no compass at all, TravelTo's reached bit
        // would otherwise say "within PLACE_REACH" through walls. Arrival is the encounter's to decide and pay.
        if (t == GOAL_TARGET_ASSIGNMENT && view.HasObjective && view.ObjectivePlaceKnown)
            return where = view.Objective, true;
        return false;
    }
    // A dungeon's seen places and the assignment (WorldView::HasSeenPlaces).
    if (t < GOAL_TARGET_PLACE_FIRST)
        return false;
    if (t >= GOAL_TARGET_PLACE_FIRST && t < GOAL_TARGET_ASSIGNMENT)
    {
        WorldView::JournalPlace const& place = world.Places[t - GOAL_TARGET_PLACE_FIRST];
        return where = place.Where, place.Present;
    }
    if (t == GOAL_TARGET_ASSIGNMENT)
        return where = world.Assignment, world.HasAssignment;
    return false;
}

void Animus::Curriculum::GoalBlock::Status(SeatView const& view, int32 goal, bool& reached, bool& possible,
    CellPoint const* cell)
{
    reached = false;
    possible = false;
    Player* bot = view.Bot;
    if (goal < 0 || !bot)
        return;
    if (!bot->IsAlive())
    {
        // Dead, nothing is still to do: nothing offers a dead seat a goal but to fight on.
        return;
    }

    std::array<bool, GOAL_COUNT> kinds;
    std::array<bool, GOAL_TARGETS> targets;
    Available(view, kinds, targets);
    SeatGoal const kind = SeatGoal(GoalKindOf(goal));
    uint32 const target = GoalTargetOf(goal);
    possible = uint32(kind) < GOAL_COUNT && kinds[uint32(kind)] && targets[target] && GoalAccepts(kind, target);

    auto const enemyAt = [&view](uint32 slot) -> Unit*
    {
        if (view.EnemyCount)
            return slot < view.EnemyCount ? view.Enemies[slot] : nullptr;
        return slot == 0 ? view.Target : nullptr;
    };
    auto const placeOf = [&view](uint32 t, Position& where) { return PlaceOf(view, t, where); };
    bool const hasMana = bot->GetMaxPower(POWER_MANA) > 0;
    float const mana = hasMana ? float(bot->GetPower(POWER_MANA)) / float(bot->GetMaxPower(POWER_MANA)) : 1.0f;

    switch (kind)
    {
        case SeatGoal::Fight:
            if (target == GOAL_TARGET_NONE)
            {
                bool any = view.Target && view.Target->IsAlive();
                for (uint32 slot = 0; slot < view.EnemyCount && !any; ++slot)
                    any = view.Enemies[slot] && view.Enemies[slot]->IsAlive();
                reached = !any && bot->IsInCombat() == false;
            }
            else if (Unit* enemy = enemyAt(target - GOAL_TARGET_ENEMY_FIRST))
                reached = !enemy->IsAlive();
            break;
        case SeatGoal::Control:
            // By the seat's own crowd control: in a party somebody's stun or fear held the named enemy most of the
            // time, and Control became the goal heads' favourite free completion (half of all goals, 2026-10-02,
            // stage7).
            if (Unit* enemy = enemyAt(target - GOAL_TARGET_ENEMY_FIRST))
                reached = enemy->IsAlive() && Encoding::CrowdControlledBy(enemy, bot);
            break;
        case SeatGoal::Recover:
            reached = bot->GetHealthPct() >= 90.0f && mana >= 0.8f;
            break;
        case SeatGoal::Rest:
            reached = bot->GetHealthPct() >= 95.0f && mana >= 0.95f;
            break;
        case SeatGoal::Protect:
            // Back above PROTECT_REACHED_PCT: a friend healed out of danger. At 90 a healer that healed to 70 --
            // what the overheal charge asks of it -- reached nothing, and parties' healers chose Protect 5% of the
            // time with 2% of those reached (2026-10-02, stage6). Kept safe while attacked also reaches it
            // (Goals.ProtectHoldMs, in StageScenario, which keeps the time).
            if (Unit* friendUnit = Encoding::FriendUnit(view, target - GOAL_TARGET_FRIEND_FIRST))
                reached = friendUnit->IsAlive() && friendUnit->GetHealthPct() >= PROTECT_REACHED_PCT;
            break;
        case SeatGoal::TravelTo:
        {
            if (view.World.CellGoals && target == GOAL_CELL_TARGET)
            {
                // A cell goal: possible while the point the choice latched is valid (the scenario ends it when it is
                // not, or when the seat makes no headway), reached within CellReach of it on its own storey.
                possible = cell && cell->Valid;
                if (possible)
                    reached = bot->GetExactDist2d(&cell->Where) <= view.World.CellReach
                        && std::fabs(bot->GetPositionZ() - cell->Where.GetPositionZ()) <= view.World.CellRise;
                break;
            }
            Position where;
            bool const there = placeOf(target, where);
            if (view.World.RoomGoals && IsRoomTarget(target))
            {
                // A room goal is reached when its room is checked (the slot's Done), which the seat may do from the
                // door or by walking in: not by a distance. Done is held one observation, so it is still possible.
                reached = there && view.World.Places[target - GOAL_TARGET_PLACE_FIRST].Done;
                possible = possible || reached;
            }
            else
                reached = there && bot->GetExactDist2d(&where) <= PLACE_REACH;
            break;
        }
        case SeatGoal::Resurrect:
            if (target == GOAL_TARGET_NONE)
                reached = possible = true;      // alive: it stood up (held from before, or true on choice)
            else if (Unit* friendUnit = Encoding::FriendUnit(view, target - GOAL_TARGET_FRIEND_FIRST))
            {
                reached = friendUnit->IsAlive();
                possible = true;
            }
            break;
        case SeatGoal::Position:
        case SeatGoal::Prepare:
        case SeatGoal::Count:
            break;      // held, not reached: they end on the goal clock
    }
}

bool Animus::Curriculum::GoalBlock::Earned(bool reached, bool& fresh, bool& satisfiedAtChoice)
{
    if (fresh)
    {
        fresh = false;
        satisfiedAtChoice = reached;
    }
    if (satisfiedAtChoice)
    {
        if (!reached)
            satisfiedAtChoice = false;
        return false;
    }
    return reached;
}

void Animus::Curriculum::GoalBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    std::array<bool, GOAL_COUNT> kinds;
    std::array<bool, GOAL_TARGETS> targets;
    Available(view, kinds, targets);
    for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
        obs[OBS_KIND_FIRST + kind] = kinds[kind] ? 1.0f : 0.0f;
    for (uint32 target = 0; target < GOAL_TARGETS; ++target)
        obs[OBS_TARGET_FIRST + target] = targets[target] ? 1.0f : 0.0f;
    obs[OBS_ENDED] = view.GoalEnded ? 1.0f : 0.0f;
    obs[OBS_REACHED] = view.GoalReached ? 1.0f : 0.0f;
    obs[OBS_SECONDARY_ENDED] = view.Goal2Ended ? 1.0f : 0.0f;
    obs[OBS_EVENT] = view.GoalEvent ? 1.0f : 0.0f;
    // The zero-filled buffer is the caller's: one-hots only where there is something to say.
    if (view.Achieved >= 0 && view.Achieved < int32(GOAL_JOINT_COUNT))
    {
        obs[OBS_ACHIEVED_KIND_FIRST + GoalKindOf(view.Achieved)] = 1.0f;
        obs[OBS_ACHIEVED_TARGET_FIRST + GoalTargetOf(view.Achieved)] = 1.0f;
    }
    ObservePlaces(view, obs);
}

namespace
{
    /// A place's bearing off the seat's facing and its straight-line distance, as CompassBlock reads its mark.
    void Bearing(Animus::Curriculum::SeatView const& view, Position const& place, float& sin, float& cos, float& range)
    {
        Player* bot = view.Bot;
        Animus::Movement::BodyState const* body = view.Body;
        Position const self = body ? Position(body->X, body->Y, body->Z, body->Yaw) : bot->GetPosition();
        float const angle = self.GetAngle(place.GetPositionX(), place.GetPositionY()) - view.Facing;
        float const relative = std::atan2(std::sin(angle), std::cos(angle));
        sin = std::sin(relative);
        cos = std::cos(relative);
        range = self.GetExactDist2d(&place);
    }
}

namespace
{
    /// A point as the goal block's held-place columns say it (present, bearing sin and cos, distance over
    /// OBJECTIVE_SCALE, distance over NEAR_SCALE), into `row`.
    void EncodePoint(Animus::Curriculum::SeatView const& view, Position const& point, float* row)
    {
        float sin;
        float cos;
        float range;
        Bearing(view, point, sin, cos, range);
        using Animus::Curriculum::GoalBlock;
        row[0] = 1.0f;
        row[1] = sin;
        row[2] = cos;
        row[3] = std::min(1.0f, range / GoalBlock::OBJECTIVE_SCALE);
        row[4] = std::min(1.0f, range / GoalBlock::NEAR_SCALE);
    }

    /// Revision 4's columns, for an episode that offers cell goals.
    void ObserveCells(Animus::Curriculum::SeatView const& view, float* obs)
    {
        using namespace Animus::Curriculum;
        using Animus::Curriculum::GoalBlock;
        if (GoalBlock::IsCellGoal(view.Goal) && view.HeldCell[0].Valid)
            EncodePoint(view, view.HeldCell[0].Where, obs + GoalBlock::OBS_HELD_PRESENT);
        if (GoalBlock::IsCellGoal(view.Goal2) && view.HeldCell[1].Valid)
            EncodePoint(view, view.HeldCell[1].Where, obs + GoalBlock::OBS_HELD2_FIRST);
        if (view.NextCell.Valid)
            EncodePoint(view, view.NextCell.Where, obs + GoalBlock::OBS_NEXT_FIRST);
        obs[GoalBlock::OBS_PLAN_LEFT] = view.PlanLeft;
        // Where the seat stands in the frame of the crop its latest choice was drawn from (the planner's hindsight
        // label: the block it reached).
        if (view.ChoicePose.Valid)
        {
            Animus::Movement::BodyState const* body = view.Body;
            float const x = body ? body->X : view.Bot->GetPositionX();
            float const y = body ? body->Y : view.Bot->GetPositionY();
            uint32 row;
            uint32 col;
            if (CellGrid::Locate(view.ChoicePose, x, y, row, col))
            {
                obs[GoalBlock::OBS_FROM_PRESENT] = 1.0f;
                obs[GoalBlock::OBS_FROM_ROW] = (float(row) + 0.5f) / float(CellGrid::GRID);
                obs[GoalBlock::OBS_FROM_COL] = (float(col) + 0.5f) / float(CellGrid::GRID);
            }
        }
    }
}

void Animus::Curriculum::GoalBlock::ObservePlaces(SeatView const& view, float* obs)
{
    WorldView const& world = view.World;
    if (!world.RoomGoals || !view.Bot)
        return;
    if (world.CellGoals)
    {
        ObserveCells(view, obs);
        return;
    }

    // Each place slot the episode holds: where it is from the seat, how much of it was seen, how long ago.
    for (uint32 slot = 0; slot < PLACE_SLOTS; ++slot)
    {
        WorldView::JournalPlace const& place = world.Places[slot];
        if (!place.Present)
            continue;
        float sin;
        float cos;
        float range;
        Bearing(view, place.Where, sin, cos, range);
        float* row = obs + OBS_PLACE_FIRST + slot * PLACE_FEATURES;
        row[0] = sin;
        row[1] = cos;
        row[2] = std::min(1.0f, range / OBJECTIVE_SCALE);
        row[3] = std::clamp(place.Coverage, 0.0f, 1.0f);
        row[4] = std::clamp(place.Age, 0.0f, 1.0f);
    }

    // The primary goal's place, if it is about one of them and is still to be reached.
    uint32 const target = GoalTargetOf(view.Goal);
    if (view.Goal < 0 || SeatGoal(GoalKindOf(view.Goal)) != SeatGoal::TravelTo || target < GOAL_TARGET_PLACE_FIRST
        || target >= GOAL_TARGET_PLACE_FIRST + PLACE_SLOTS)
        return;
    WorldView::JournalPlace const& held = world.Places[target - GOAL_TARGET_PLACE_FIRST];
    if (!held.Present || held.Done)
        return;
    float sin;
    float cos;
    float range;
    Bearing(view, held.Where, sin, cos, range);
    obs[OBS_HELD_PRESENT] = 1.0f;
    obs[OBS_HELD_SIN] = sin;
    obs[OBS_HELD_COS] = cos;
    obs[OBS_HELD_DIST] = std::min(1.0f, range / OBJECTIVE_SCALE);
    obs[OBS_HELD_NEAR] = std::min(1.0f, range / NEAR_SCALE);
}
