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
#include "RewardLedger.h"      // RewardTerm and SeatState used to arrive through StageScenario.h, which this
#include "StageState.h"        // half of the library no longer includes
#include "Layout.h"
#include "EncoderSupport.h"
#include "Creature.h"
#include "PetBlock.h"
#include "Env.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include <algorithm>

namespace
{
    /// Whether `target` carries an aura of `type` from `bot` or from something `bot` owns (its pet, a totem).
    bool HasAuraFrom(Unit const* target, AuraType type, Player const* bot)
    {
        for (AuraEffect const* effect : target->GetAuraEffectsByType(type))
        {
            ObjectGuid const caster = effect->GetCasterGUID();
            if (caster == bot->GetGUID())
                return true;

            if (Unit const* unit = ObjectAccessor::GetUnit(*bot, caster); unit
                && unit->GetCharmerOrOwnerGUID() == bot->GetGUID())
                return true;
        }

        return false;
    }
}

std::string_view Animus::Curriculum::RewardTermName(RewardTerm term)
{
    switch (term)
    {
        case RewardTerm::DamageDealt:           return "damage_dealt";
        case RewardTerm::DamageTaken:           return "damage_taken";
        case RewardTerm::StepCost:              return "step_cost";
        case RewardTerm::Casting:               return "casting";
        case RewardTerm::Approach:              return "approach";
        case RewardTerm::StealthOpener:         return "stealth_opener";
        case RewardTerm::StealthUtility:        return "stealth_utility";
        case RewardTerm::Interrupt:             return "interrupt";
        case RewardTerm::Hazard:                return "hazard";
        case RewardTerm::HealingMana:           return "healing_mana";
        case RewardTerm::Kill:                  return "kill";
        case RewardTerm::Clear:                 return "clear";
        case RewardTerm::HealthKept:            return "health_kept";
        case RewardTerm::Death:                 return "death";
        case RewardTerm::OwnerDamageTaken:      return "owner_damage_taken";
        case RewardTerm::OwnerHealing:          return "owner_healing";
        case RewardTerm::TankDamageRefund:      return "tank_damage_refund";
        case RewardTerm::Threat:                return "threat";
        case RewardTerm::SoloFight:             return "solo_fight";
        case RewardTerm::Follow:                return "follow";
        case RewardTerm::OwnerDeath:            return "owner_death";
        case RewardTerm::TeammateDamageTaken:   return "teammate_damage_taken";
        case RewardTerm::TeammateHealing:       return "teammate_healing";
        case RewardTerm::TeammateThreat:        return "teammate_threat";
        case RewardTerm::TeammateDeath:         return "teammate_death";
        case RewardTerm::Revive:                return "revive";
        case RewardTerm::PlayerKill:            return "player_kill";
        case RewardTerm::Progress:              return "progress";
        case RewardTerm::Arrive:                return "arrive";
        case RewardTerm::FlagCapture:           return "flag_capture";
        case RewardTerm::FlagPickup:            return "flag_pickup";
        case RewardTerm::FlagReturn:            return "flag_return";
        case RewardTerm::CarrierKill:           return "carrier_kill";
        case RewardTerm::FlagLost:              return "flag_lost";
        case RewardTerm::Timeout:               return "timeout";
        case RewardTerm::Stall:                 return "stall";
        case RewardTerm::Spacing:               return "spacing";
        case RewardTerm::Readiness:             return "readiness";
        case RewardTerm::Control:               return "control";
        case RewardTerm::SelfHealing:           return "self_healing";
        case RewardTerm::GoalMatch:             return "goal_match";
        case RewardTerm::OrderMatch:            return "order_match";
        case RewardTerm::PlaceMatch:            return "place_match";
        case RewardTerm::BrokeContact:          return "broke_contact";
        case RewardTerm::Stalk:                 return "stalk";
        case RewardTerm::Clearance:             return "clearance";
        case RewardTerm::Repeat:                return "repeat";
        case RewardTerm::BossProgress:          return "boss_progress";
        case RewardTerm::Wasted:                return "wasted";
        case RewardTerm::QuestAccepted:         return "quest_accepted";
        case RewardTerm::QuestCredit:           return "quest_credit";
        case RewardTerm::QuestTurnIn:           return "quest_turn_in";
        case RewardTerm::GatherNode:            return "gather_node";
        case RewardTerm::GatherSkillUp:         return "gather_skill_up";
        case RewardTerm::TownSold:              return "town_sold";
        case RewardTerm::TownRepaired:          return "town_repaired";
        case RewardTerm::TownStocked:           return "town_stocked";
        case RewardTerm::TownEquipped:          return "town_equipped";
        case RewardTerm::TownDone:              return "town_done";
        case RewardTerm::Count:                 break;
    }

    return "unknown";
}

float Animus::Curriculum::CombatReward::DesiredRange(SeatState const& seat,
    CurriculumTuning::DuelTuning const& duel)
{
    return seat.L->Profile->Specs[seat.Spec].Range == RangeBand::Melee ? duel.MeleeRange : duel.RangedRange;
}

float Animus::Curriculum::CombatReward::TimeLeftSince(Env const& env, uint32 sinceMs)
{
    uint32 const spent = env.EpisodeElapsedMs > sinceMs ? env.EpisodeElapsedMs - sinceMs : 0;
    return env.EpisodeLengthMs ? 1.0f - std::min(1.0f, float(spent) / float(env.EpisodeLengthMs)) : 0.0f;
}

float Animus::Curriculum::CombatReward::HealthLeft(Unit const* unit)
{
    if (!unit || !unit->IsAlive() || !unit->GetMaxHealth())
        return 1.0f;    // nothing to read: the fight is charged as though none of it was done

    return std::clamp(float(unit->GetHealth()) / float(unit->GetMaxHealth()), 0.0f, 1.0f);
}

float Animus::Curriculum::CombatReward::TimeoutScale(float floor, float healthLeft)
{
    float const share = std::clamp(floor, 0.0f, 1.0f);
    return share + (1.0f - share) * std::clamp(healthLeft, 0.0f, 1.0f);
}

void Animus::Curriculum::CombatReward::Stealth(CombatTally& tally, float opener, float utility, RewardLedger& ledger)
{
    if (tally.StepStealthOpener)
    {
        ledger.Add(RewardTerm::StealthOpener, opener);
        tally.StepStealthOpener = false;
    }

    if (tally.StepStealthUtility)
    {
        ledger.Add(RewardTerm::StealthUtility, utility * float(tally.StepStealthUtility));
        tally.StepStealthUtility = 0;
    }
}

void Animus::Curriculum::CombatReward::Casting(Player* bot, AgentStats const& step, CombatTally& tally,
    CurriculumTuning::CastingTuning const& tuning, RewardLedger& ledger)
{
    tally.CastsCompleted += step.CastsCompleted;
    tally.CastsCancelled += step.CastsCancelled;
    tally.CastMsWasted += step.CastMsWasted;
    tally.CastsStopped += step.CastsStopped;
    tally.CastsMoved += step.CastsMoved;
    tally.CastsTargetLost += step.CastsTargetLost;
    tally.CastsOther += step.CastsOther;

    // Neither forces anything: cutting a cast short stays the policy's call when something else is worth more.
    // Cancels the bot brought on itself -- the stop-casting action, or moving out of its own cast -- also pay a
    // flat charge, so that stopping a cast the decision after starting it is not free of the seconds it never
    // spent. An enemy interrupt is not the bot's doing and is not charged.
    uint32 const selfCancelled = step.CastsStopped + step.CastsMoved;
    float reward = -tuning.TimeWasted * float(step.CastMsWasted) / 1000.0f
        - tuning.Cancel * float(selfCancelled);

    // Only in combat, so casting long spells at nothing is not a way to earn it.
    if (bot->IsInCombat())
        reward += tuning.TimeCompleted * float(step.CastMsCompleted) / 1000.0f;

    ledger.Add(RewardTerm::Casting, reward);
}

void Animus::Curriculum::CombatReward::Approach(Player* bot, Unit* target, float desiredRange, float weight,
    CombatTally& tally, RewardLedger& ledger)
{
    if (!target)
    {
        tally.LastDistance = -1.0f;
        return;
    }

    float const excess = std::max(0.0f, bot->GetDistance(target) - desiredRange);
    if (tally.LastDistance >= 0.0f)
        ledger.Add(RewardTerm::Approach, weight * (tally.LastDistance - excess) / 40.0f);
    tally.LastDistance = excess;
}

void Animus::Curriculum::CombatReward::Style(Player const* bot, Unit const* target, uint32 decisionMs,
    CombatTally& tally)
{
    // Style, measured only: where the bot fights from, and whether its pet holds what it is fighting. A hunter's
    // shots cannot be used inside melee reach, so its time there is time it played melee.
    //
    // FightMs is the divisor of every share below, so a caller that never reaches here leaves in_melee_share,
    // target_on_pet_share and the two control shares reading 0 out of 0.
    tally.FightMs += decisionMs;
    if (target)
    {
        if (bot->IsWithinMeleeRange(target))
            tally.InMeleeMs += decisionMs;
        if (Unit const* victim = target->GetVictim(); victim && victim != bot
            && victim->GetCharmerOrOwnerGUID() == bot->GetGUID())
            tally.OnPetMs += decisionMs;
    }

    // Whether the bot keeps what it is fighting in place or slowed: roots (Frost Nova, Entangling Roots) and snares
    // (Concussive Shot, Wing Clip, Frost Shock, Earthbind), its own or its pet's and totems'.
    bool const rooted = target && HasAuraFrom(target, SPELL_AURA_MOD_ROOT, bot);
    bool const snared = target && HasAuraFrom(target, SPELL_AURA_MOD_DECREASE_SPEED, bot);
    if (rooted)
        tally.RootedMs += decisionMs;
    if (snared)
        tally.SnaredMs += decisionMs;
    if (rooted && !tally.WasRooted)
        ++tally.RootsApplied;
    if (snared && !tally.WasSnared)
        ++tally.SnaresApplied;
    tally.WasRooted = rooted;
    tally.WasSnared = snared;
}
