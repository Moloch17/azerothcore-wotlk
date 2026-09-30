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

Animus::Curriculum::BlockSize Animus::Curriculum::GoalBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, 0 };
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
        // Standing up again at its own corpse, where death runs on (DeathBlock).
        kinds[uint32(SeatGoal::Resurrect)] = bot && view.DeathRuns;
        return;
    }

    // Enemies: the pack's slots, or the one target of a stage without them.
    uint32 enemies = 0;
    if (view.EnemyCount)
    {
        for (uint32 slot = 0; slot < view.EnemyCount && slot < PACK_SLOTS; ++slot)
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

    // Dead friends a seat with a resurrection can raise (Resurrect about a friend slot).
    bool deadFriends = false;
    if (view.L && !view.L->AllyRevives.empty())
        for (uint32 slot = FRIEND_OWNER; slot < FRIEND_SLOTS; ++slot)
            if (Unit* friendUnit = Encoding::FriendUnit(view, slot); friendUnit && friendUnit != bot
                && !friendUnit->IsAlive())
            {
                targets[GOAL_TARGET_FRIEND_FIRST + slot] = true;
                deadFriends = true;
            }

    // The journal.
    WorldView const& world = view.World;
    bool places = false;
    for (uint32 i = 0; i < WorldView::JOURNAL_OBJECTIVES && world.Active; ++i)
        if (world.Objectives[i].Present && world.Objectives[i].Left > 0.0f && world.Objectives[i].HasPlace)
            places = targets[GOAL_TARGET_OBJECTIVE_FIRST + i] = true;
    if (world.Active && world.HasGiver)
        places = targets[GOAL_TARGET_GIVER] = true;
    if (world.Active && world.HasEnder)
        places = targets[GOAL_TARGET_ENDER] = true;
    bool found = false;
    for (uint32 i = 0; i < WorldView::JOURNAL_PLACES && world.Active; ++i)
        if (world.Places[i].Present)
            found = places = targets[GOAL_TARGET_PLACE_FIRST + i] = true;
    if (world.Active && world.HasAssignment)
        places = targets[GOAL_TARGET_ASSIGNMENT] = true;
    // A trip's objective (the travel block's) is the place a travel stage is about. With no journal it takes the
    // assignment's slot, so TravelTo has a target and the movement phase trains the goal level too.
    if (!world.Active && view.HasObjective)
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
    kinds[uint32(SeatGoal::Loot)] = world.Active && world.Corpse;
    kinds[uint32(SeatGoal::Gather)] = world.Active && ((world.Node && world.NodeOpenable) || found);
    kinds[uint32(SeatGoal::Interact)] = world.Active && (world.HasGiver || world.HasEnder || world.QuestObject
        || world.ItemTarget || world.QuestVendor || targets[GOAL_TARGET_OBJECTIVE_FIRST]
        || targets[GOAL_TARGET_OBJECTIVE_FIRST + 1] || targets[GOAL_TARGET_OBJECTIVE_FIRST + 2]
        || targets[GOAL_TARGET_OBJECTIVE_FIRST + 3]);
    kinds[uint32(SeatGoal::Rest)] = !combat && hurt;
    kinds[uint32(SeatGoal::Resurrect)] = deadFriends;

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
    kinds[uint32(SeatGoal::Fight)] = true;                  // Fight about no one in particular is always accepted
}

bool Animus::Curriculum::GoalBlock::PlaceOf(SeatView const& view, uint32 t, Position& where)
{
    WorldView const& world = view.World;
    if (!world.Active)
    {
        // A trip's objective, where there is no journal (Available).
        if (t == GOAL_TARGET_ASSIGNMENT && view.HasObjective)
            return where = view.Objective, true;
        return false;
    }
    if (t >= GOAL_TARGET_OBJECTIVE_FIRST && t < GOAL_TARGET_GIVER)
    {
        WorldView::JournalObjective const& objective = world.Objectives[t - GOAL_TARGET_OBJECTIVE_FIRST];
        where = objective.Place;
        return objective.Present && objective.HasPlace;
    }
    if (t == GOAL_TARGET_GIVER)
        return where = world.GiverAt, world.HasGiver;
    if (t == GOAL_TARGET_ENDER)
        return where = world.EnderAt, world.HasEnder;
    if (t >= GOAL_TARGET_PLACE_FIRST && t < GOAL_TARGET_ASSIGNMENT)
    {
        WorldView::JournalPlace const& place = world.Places[t - GOAL_TARGET_PLACE_FIRST];
        return where = place.Where, place.Present;
    }
    if (t == GOAL_TARGET_ASSIGNMENT)
        return where = world.Assignment, world.HasAssignment;
    return false;
}

void Animus::Curriculum::GoalBlock::Status(SeatView const& view, int32 goal, bool& reached, bool& possible)
{
    reached = false;
    possible = false;
    Player* bot = view.Bot;
    if (goal < 0 || !bot)
        return;
    if (!bot->IsAlive())
    {
        // Dead, only standing up again at its own corpse is still to do.
        possible = SeatGoal(GoalKindOf(goal)) == SeatGoal::Resurrect && GoalTargetOf(goal) == GOAL_TARGET_NONE
            && view.DeathRuns;
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
            if (Unit* enemy = enemyAt(target - GOAL_TARGET_ENEMY_FIRST))
                reached = enemy->IsAlive() && Encoding::IsCrowdControlled(enemy);
            break;
        case SeatGoal::Recover:
            reached = bot->GetHealthPct() >= 90.0f && mana >= 0.8f;
            break;
        case SeatGoal::Rest:
            reached = bot->GetHealthPct() >= 95.0f && mana >= 0.95f;
            break;
        case SeatGoal::Protect:
            if (Unit* friendUnit = Encoding::FriendUnit(view, target - GOAL_TARGET_FRIEND_FIRST))
                reached = friendUnit->IsAlive() && friendUnit->GetHealthPct() >= 90.0f;
            break;
        case SeatGoal::TravelTo:
        {
            Position where;
            reached = placeOf(target, where) && bot->GetExactDist2d(&where) <= PLACE_REACH;
            break;
        }
        case SeatGoal::Loot:
            reached = view.World.Active && !view.World.Corpse;
            break;
        case SeatGoal::Gather:
            if (target == GOAL_TARGET_NONE)
                reached = view.World.Active && !(view.World.Node && view.World.NodeOpenable);
            else
            {
                Position where;
                reached = placeOf(target, where) && bot->GetExactDist2d(&where) <= PLACE_REACH
                    && !(view.World.Node && view.World.NodeOpenable);
            }
            break;
        case SeatGoal::Interact:
            if (target >= GOAL_TARGET_OBJECTIVE_FIRST && target < GOAL_TARGET_GIVER)
            {
                WorldView::JournalObjective const& objective = view.World.Objectives[target - GOAL_TARGET_OBJECTIVE_FIRST];
                reached = objective.Present && objective.Left <= 0.0f;
            }
            else if (target == GOAL_TARGET_GIVER)
                reached = !view.World.HasGiver;         // the quest is taken: the giver leaves the journal
            break;
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
    if (view.OrderGoal >= 0 && view.OrderGoal < int32(GOAL_JOINT_COUNT))
    {
        obs[OBS_FROM_ORDER] = 1.0f;
        obs[OBS_ORDER_KIND_FIRST + GoalKindOf(view.OrderGoal)] = 1.0f;
        obs[OBS_ORDER_TARGET_FIRST + GoalTargetOf(view.OrderGoal)] = 1.0f;
    }
    if (view.Achieved >= 0 && view.Achieved < int32(GOAL_JOINT_COUNT))
    {
        obs[OBS_ACHIEVED_KIND_FIRST + GoalKindOf(view.Achieved)] = 1.0f;
        obs[OBS_ACHIEVED_TARGET_FIRST + GoalTargetOf(view.Achieved)] = 1.0f;
    }
}
