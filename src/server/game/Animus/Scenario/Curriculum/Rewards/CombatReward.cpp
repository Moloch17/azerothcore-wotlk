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

#include "CombatReward.h"
#include "RewardLedger.h"
#include "StageState.h"
#include "Layout.h"

std::string_view Animus::Curriculum::RewardTermName(RewardTerm term)
{
    switch (term)
    {
        case RewardTerm::DamageDealt:           return "damage_dealt";
        case RewardTerm::StepCost:              return "step_cost";
        case RewardTerm::Approach:              return "approach";
        case RewardTerm::Hazard:                return "hazard";
        case RewardTerm::HealingMana:           return "healing_mana";
        case RewardTerm::Kill:                  return "kill";
        case RewardTerm::Clear:                 return "clear";
        case RewardTerm::Death:                 return "death";
        case RewardTerm::Threat:                return "threat";
        case RewardTerm::TeammateDamageTaken:   return "teammate_damage_taken";
        case RewardTerm::TeammateHealing:       return "teammate_healing";
        case RewardTerm::TeammateThreat:        return "teammate_threat";
        case RewardTerm::TeammateDeath:         return "teammate_death";
        case RewardTerm::Revive:                return "revive";
        case RewardTerm::Progress:              return "progress";
        case RewardTerm::Arrive:                return "arrive";
        case RewardTerm::Timeout:               return "timeout";
        case RewardTerm::Stall:                 return "stall";
        case RewardTerm::SelfHealing:           return "self_healing";
        case RewardTerm::GoalReached:           return "goal_reached";
        case RewardTerm::GoalProgress:          return "goal_progress";
        case RewardTerm::GoalSwitch:            return "goal_switch";
        case RewardTerm::Repeat:                return "repeat";
        case RewardTerm::Jitter:                return "jitter";
        case RewardTerm::Aimless:               return "aimless";
        case RewardTerm::Effort:                return "effort";
        case RewardTerm::Fidget:                return "fidget";
        case RewardTerm::CombatClock:           return "combat_clock";
        case RewardTerm::PullClean:             return "pull_clean";
        case RewardTerm::EarlyPull:             return "early_pull";
        case RewardTerm::DrillHold:             return "drill_hold";
        case RewardTerm::DrillFocus:            return "drill_focus";
        case RewardTerm::DrillKeep:             return "drill_keep";
        case RewardTerm::PullExtra:             return "pull_extra";
        case RewardTerm::Facing:                return "facing";
        case RewardTerm::Stuck:                 return "stuck";
        case RewardTerm::Wall:                  return "wall";
        case RewardTerm::FollowKept:            return "follow_kept";
        case RewardTerm::Lost:                  return "lost";
        case RewardTerm::Sighting:              return "sighting";
        case RewardTerm::NewGround:             return "new_ground";
        case RewardTerm::RoomSeen:              return "room_seen";
        case RewardTerm::DoorOpened:            return "door_opened";
        case RewardTerm::WrongObject:           return "wrong_object";
        case RewardTerm::Regroup:               return "regroup";
        case RewardTerm::Blocking:              return "blocking";
        case RewardTerm::Survived:              return "survived";
        case RewardTerm::InterruptLanded:       return "interrupt_landed";
        case RewardTerm::Away:                  return "away";
        case RewardTerm::Hurt:                  return "hurt";
        case RewardTerm::FireHurt:              return "fire_hurt";
        case RewardTerm::ReadyPull:             return "ready_pull";
        case RewardTerm::Idle:                  return "idle";
        case RewardTerm::RoomGoal:              return "room_goal";
        case RewardTerm::RoomSwitch:            return "room_switch";
        case RewardTerm::Return:                return "return";
        case RewardTerm::CellGoal:              return "cell_goal";
        case RewardTerm::CellProgress:          return "cell_progress";
        case RewardTerm::CellSwitch:            return "cell_switch";
        case RewardTerm::CellLost:              return "cell_lost";
        case RewardTerm::CellStale:             return "cell_stale";
        case RewardTerm::Explore:               return "explore";
        case RewardTerm::FrontierPull:          return "frontier_pull";
        case RewardTerm::Circling:              return "circling";
        case RewardTerm::Escape:                return "escape";
        case RewardTerm::Count:                 break;
    }

    return "unknown";
}

float Animus::Curriculum::CombatReward::DesiredRange(SeatState const& seat,
    CurriculumTuning::DuelTuning const& duel)
{
    return seat.L->Profile->Specs[seat.Spec].Range == RangeBand::Melee ? duel.MeleeRange : duel.RangedRange;
}
