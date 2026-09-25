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

#include "CombatRewardScenario.h"
#include "CombatReward.h"        // Style and TimeLeftSince, which OneOnOne calls unqualified
#include "RewardLedger.h"
#include "EncoderSupport.h"
#include "Creature.h"
#include "PetBlock.h"
#include "Env.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "StageScenario.h"
#include <algorithm>

namespace
{
    /// How soon after a feign death ends an opponent's evade still counts as caused by it.
    constexpr uint32 FEIGN_RESET_WINDOW_MS = 3000;
}

void Animus::Curriculum::CombatReward::OneOnOne(StageScenario& scenario, Env const& env,
    uint32 seatIndex, Player* bot, Unit* opponent, RewardLedger& ledger, float tierScale)
{
    CurriculumTuning::DuelTuning const& tuning = scenario.Tuning().Duel;
    SeatState& seat = scenario.Data(env).Seats[seatIndex];
    CombatTally& tally = seat.Combat;

    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * scenario.DecisionScale());
    if (!bot || !opponent)
        return;

    AgentStats const& step = env.StepStats[seatIndex];
    float const opponentHealth = float(std::max<uint32>(1, opponent->GetMaxHealth()));
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

    // Damage is a fraction of the opponent's health, so a kill is worth DamageDealt in damage at any level.
    ledger.Add(RewardTerm::DamageDealt, tuning.DamageDealt * float(step.Damage) / opponentHealth);

    tally.DamageTaken += step.DamageTaken;
    ledger.Add(RewardTerm::DamageTaken, -tuning.DamageTaken * seat.LastStepDamageTaken);

    Casting(bot, step, tally, scenario.Tuning().Casting, ledger);
    Approach(bot, opponent, DesiredRange(seat, tuning), tuning.Approach, tally, ledger);

    Stealth(tally, tuning.StealthOpener, tuning.StealthUtility, ledger);

    if (bot->GetPetGUID() || Encoding::FirstPet(bot))
        tally.PetSummoned = true;

    if (!tally.Engaged && (bot->IsInCombat() || opponent->IsInCombat()))
    {
        tally.Engaged = true;
        tally.EngageMs = env.EpisodeElapsedMs;

        // Ready to fight: a class that keeps a pet should start the fight with it out. Paid once, when the fight
        // starts, so summoning over and over earns nothing -- the same shape as the gauntlet's readiness and buff
        // coverage. Measured 2026-09-17: the warlock summoned in 7% of the episodes it did not start with one,
        // where the hunter summoned in 85% of its own.
        if (seat.L && PetBlock::HasPet(seat.L->Profile->Class) && PetBlock::FindPet(bot))
            ledger.Add(RewardTerm::Readiness, scenario.Tuning().Support.PetReady);
    }

    if (tally.Engaged && bot->IsAlive() && opponent->IsAlive())
        Style(bot, opponent, scenario.DecisionMs(), tally);

    // Feign death: a feign that leaves the opponent nothing to fight sends it home to evade at full health, within
    // a few seconds of the feign ending.
    bool const feigning = bot->IsAlive() && bot->HasAuraType(SPELL_AURA_FEIGN_DEATH);
    if (feigning && !tally.WasFeigning)
    {
        ++tally.FeignDeaths;
        tally.FeignResetCounted = false;
    }
    if (feigning)
        tally.FeignEndMs = env.EpisodeElapsedMs;
    tally.WasFeigning = feigning;

    if (Creature const* creature = opponent->ToCreature(); creature && tally.FeignDeaths && !tally.FeignResetCounted
        && creature->IsInEvadeMode() && env.EpisodeElapsedMs <= tally.FeignEndMs + FEIGN_RESET_WINDOW_MS)
    {
        ++tally.FeignDeathResets;
        tally.FeignResetCounted = true;
    }

    if (!tally.Killed && !opponent->IsAlive())
    {
        tally.Killed = true;
        tally.KillTimeMs = env.EpisodeElapsedMs;

        float const healthKept = 1.0f - std::min(1.0f, float(tally.DamageTaken) / botHealth);
        float const timeLeft = TimeLeftSince(env, tally.Engaged ? tally.EngageMs : env.EpisodeElapsedMs);
        ledger.Add(RewardTerm::Kill, (tuning.Kill + tuning.FastKill * timeLeft) * tierScale);
        ledger.Add(RewardTerm::HealthKept, tuning.HealthKept * healthKept * tierScale);
    }

    // Every death costs, including one after resurrecting itself.
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death / tierScale);
    }
}
