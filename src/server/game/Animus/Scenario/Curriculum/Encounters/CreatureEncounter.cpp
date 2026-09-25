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

#include "Encounters.h"
#include "CombatReward.h"
#include "CombatRewardScenario.h"
#include "Creature.h"
#include "Env.h"
#include "Opponents.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "StageScenario.h"

namespace
{
    /// How long a creature duel's opponent may have no path to its victim before it is put beside it (the core evades
    /// it after 10 s, and it regenerates from 5 s before that).
    constexpr uint32 UNREACHABLE_TELEPORT_MS = 3000;
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::CreatureEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::StealthUtility, RewardTerm::Kill,
        RewardTerm::HealthKept, RewardTerm::Death, RewardTerm::Timeout, RewardTerm::Stall, RewardTerm::Spacing,
        RewardTerm::Readiness, RewardTerm::Interrupt };
}

Animus::Curriculum::CreatureEncounter::CreatureEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "difficulty tier")
{
}

void Animus::Curriculum::CreatureEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // The fight's difficulty tier, and whether its opponent was an elite.
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add("opponent_elite", [this](Env const& env, uint32) { return _envs[env.Index].Elite ? 1.0f : 0.0f; });

    // The duel has paid RewardTerm::Interrupt since the term was added, with no column to show for it -- so the one
    // stage where a class first meets an interruptible cast reported nothing about whether it ever kicked one.
    table.Add("interrupts", [this](Env const& env, uint32) { return float(_envs[env.Index].Interrupts); });
    table.Add("control_seconds", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].ControlMs) / 1000.0f;
    });
}

bool Animus::Curriculum::CreatureEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);
    CurriculumTuning::DifficultyTuning const& difficulty = _scenario.Tuning().Difficulty;
    SeatState const& seat = data.Seats[0];

    // The tier (DifficultyLadder): the one the stage viewer chose, spread over the seeds in an evaluation, the
    // class and build's own in training (now and then a lower one).
    EnvFight& fight = _envs[env.Index];
    fight = EnvFight();
    fight.Layout = seat.L ? seat.L->Index : 0;
    fight.Spec = seat.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, fight.Layout, fight.Spec, difficulty.MaxTier);
    fight.Tier = uint8(pick.Tier);
    fight.Counts = pick.Counts;

    fight.Elite = fight.Tier >= difficulty.EliteTier;
    uint32 const steps = fight.Elite ? fight.Tier - difficulty.EliteTier : fight.Tier;
    uint8 const level = uint8(std::min<uint32>(seat.Level + steps * difficulty.LevelsPerTier, DEFAULT_MAX_LEVEL + 3));

    // A share of duels are against something that casts, and some of those against something that puts a hazard on
    // the ground. The duel's own pool is default-AI creatures, which never cast at all: stage 1 had no cast to
    // interrupt, no debuff worth dispelling and nothing to step out of, so everything a seat learns about answering
    // a caster had to wait for stage 2's packs. These come from the same cast-or-talk-only scripts the packs use.
    Opponents::OpponentPool const& pool = Opponents::OpponentPool::Instance();
    data.OpponentEntry = fight.Elite ? pool.RandomElite(level) : 0;
    if (!data.OpponentEntry && roll_chance_i(difficulty.CasterChance))
        data.OpponentEntry = roll_chance_i(difficulty.HazardChance) ? pool.RandomHazardCaster(level)
                                                                    : pool.RandomCaster(level);
    if (!data.OpponentEntry)
    {
        fight.Elite = false;
        data.OpponentEntry = pool.Random(level);
    }

    // A lake arena (ArenaDefinition::Water on a creature arena) puts the opponent in the water, so the fight is a
    // swimming one for whoever goes in after the other; a spawn point with no water in reach fights on land.
    Position const where = _scenario.Arena(env).Water ? Opponents::FindSpawnPointInWater(bot, map)
        : Opponents::FindSpawnPoint(bot, map);
    Creature* opponent = data.OpponentEntry
        ? Opponents::SummonOpponent(bot, map, data.OpponentEntry, where, level) : nullptr;
    if (!opponent)
        return false;

    env.Targets = { opponent->GetGUID() };

    // No pet and no attack: summoning one, stealthing and approaching are all the policy's to learn.
    _scenario.PrepareFighter(bot, data.Seats[0]);
    return true;
}

void Animus::Curriculum::CreatureEncounter::OnSeatAction(Env& env, uint32 /*seat*/, SeatActionResult const& result)
{
    if (!result.PendingInterrupt.IsEmpty())
        _envs[env.Index].PendingInterrupt = result.PendingInterrupt;
}

void Animus::Curriculum::CreatureEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    // An interrupt counts when the opponent it was cast at had its cast cut short since, and is paid by what it
    // stopped (IncomingSpell::Prevented) -- a heal is worth far more than a filler, and the duel is the cheapest
    // place to learn the difference.
    EnvFight& pending = _envs[env.Index];
    if (!pending.PendingInterrupt.IsEmpty())
    {
        CurriculumTuning::DuelTuning const& duel = _scenario.Tuning().Duel;
        auto const stopped = std::find_if(env.StepInterruptedTargets.begin(), env.StepInterruptedTargets.end(),
            [&pending](Env::InterruptedCast const& cast) { return cast.Caster == pending.PendingInterrupt; });
        if (stopped != env.StepInterruptedTargets.end())
        {
            ledger.Add(RewardTerm::Interrupt, duel.Interrupt
                * PreventedScale(duel.InterruptHeal, duel.InterruptArea, duel.InterruptLong, stopped->Prevented));
            ++pending.Interrupts;
        }

        pending.PendingInterrupt.Clear();
    }

    Unit* opponent = env.FindTargetUnit(0);
    CombatReward::OneOnOne(_scenario, env, seat, bot, opponent, ledger,
        CombatReward::TierScale(_scenario.Tuning().Difficulty.TierScale, _envs[env.Index].Tier));

    // Measured, not paid: the duel's reward stays as trained, but a class that wins by holding the opponent stunned
    // is now visible as such rather than only as a shorter fight.
    if (opponent && PullsEncounter::Controlled(opponent))
        pending.ControlMs += _scenario.DecisionMs();

    SeatState& seatState = _scenario.Data(env).Seats[seat];
    CombatTally& tally = seatState.Combat;
    CurriculumTuning::DuelTuning const& tuning = _scenario.Tuning().Duel;
    if (bot && opponent && opponent->IsAlive())
    {
        uint32 const decisionMs = _scenario.DecisionMs();
        float const seconds = float(decisionMs) / 1000.0f;
        Creature* creature = opponent->ToCreature();
        if (creature && creature->IsInEvadeMode())
            tally.TargetEvadeMs += decisionMs;
        if (creature && creature->CanNotReachTarget())
        {
            tally.UnreachableMs += decisionMs;
            tally.UnreachableStreakMs += decisionMs;

            // A creature with no path to its victim regenerates, and evades home at full health after 10 s: a fight
            // no play can win. Put it beside its victim first, as instance trash does with
            // Creature.Instance.TeleportToUnreachableTarget.
            Unit* victim = creature->GetVictim();
            if (tally.UnreachableStreakMs >= UNREACHABLE_TELEPORT_MS && victim && victim->IsAlive()
                && victim->IsInMap(creature))
            {
                creature->NearTeleportTo(victim->GetPositionX(), victim->GetPositionY(), victim->GetPositionZ(),
                    victim->GetOrientation());
                creature->SetCannotReachTarget();
                tally.UnreachableStreakMs = 0;
                ++tally.OpponentTeleports;
            }
        }
        else
            tally.UnreachableStreakMs = 0;
        if (tally.Engaged && bot->IsAlive() && !bot->IsWithinLOSInMap(opponent))
            tally.OutOfSightMs += decisionMs;

        // The fight not started once the grace is gone: standing where it spawned is paid for as it happens, not
        // only when the clock runs out.
        // Time spent preparing (buffs, forms, stealth, a pet) is added to the grace, up to PreparationRefundMaxMs.
        uint32 const graceMs = tuning.StallGraceMs + std::min(tally.PreparationMs, tuning.PreparationRefundMaxMs);
        if (!tally.Engaged && bot->IsAlive() && env.EpisodeElapsedMs > graceMs)
            ledger.Add(RewardTerm::Stall, -tuning.Stall * seconds);

        // A ranged spec with the opponent hitting it in melee.
        if (bot->IsAlive() && seatState.L && seatState.L->Profile->Specs[seatState.Spec].Range != RangeBand::Melee
            && opponent->GetVictim() == bot && opponent->IsWithinMeleeRange(bot))
            ledger.Add(RewardTerm::Spacing, -tuning.Spacing * seconds);
    }

    // The outcome, once: a kill without a death is a win, a death or the clock a loss.
    EnvFight& fight = _envs[env.Index];
    if (seat == 0 && !fight.Recorded && (tally.Killed || tally.Died || TimeIsUp(env)))
    {
        fight.Recorded = true;
        if (fight.Counts)
            _ladder.Record(fight.Layout, fight.Spec, fight.Tier, tally.Killed && !tally.Died,
                _scenario.Tuning().Difficulty.MaxTier);
    }

    // Out of time with neither side dead: the fight is lost (IsTerminal ends it as a loss, not a cut-off).
    //
    // What it costs is what is left of the opponent. A clock that runs out with the creature nearly dead is a near
    // miss; one that runs out with it untouched is a refusal to fight, and until 2026-09-17 the two cost the same
    // as dying did -- so a seat unsure of the kill could take the certain loss by keeping its distance, which is
    // what the low-level casters learned to do (timeouts 0.15 against the scripted baseline's 0.03).
    if (!tally.Killed && !tally.Died && !tally.TimedOut && TimeIsUp(env))
    {
        tally.TimedOut = true;
        ledger.Add(RewardTerm::Timeout, -tuning.Timeout
            * CombatReward::TimeoutScale(tuning.TimeoutFloor, CombatReward::HealthLeft(opponent))
            / CombatReward::TierScale(_scenario.Tuning().Difficulty.TierScale, fight.Tier));
    }
}

void Animus::Curriculum::CreatureEncounter::WriteState(Env const& env, float* state) const
{
    // The fight's tier, so the critic can predict a tier-scaled return.
    uint32 const top = std::max<uint32>(1, _scenario.Tuning().Difficulty.MaxTier);
    state[StageScenario::STATE_TIER] = float(_envs[env.Index].Tier) / float(top);
}

bool Animus::Curriculum::CreatureEncounter::TimeIsUp(Env const& env)
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

bool Animus::Curriculum::CreatureEncounter::IsTerminal(Env const& env) const
{
    // A death ends it once no resurrection of its own is left to wait for. Running out of time ends it too, and as
    // an outcome: the duel is there to be won, so the learner must not bootstrap past the clock as if the fight went
    // on.
    return _scenario.Data(env).Seats[0].Combat.Killed || _scenario.DeadForGood(env, 0) || TimeIsUp(env);
}
