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
#include "Creature.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "MotionMaster.h"
#include "Opponents.h"
#include "Player.h"
#include "Random.h"
#include "StageScenario.h"

/*
 * The rotation drill (Opposition::Dummy): the kit with nothing, or nearly nothing, fighting back. The first place
 * in the curriculum where what is pressed is the whole lesson -- there is no fight to survive and no clock to beat,
 * only output and what it cost, so a class learns its buttons with purpose (Component H's judgement) before it
 * learns to live through a fight.
 *
 * The dummies are the duel pool's own same-level creatures, their health scaled so they outlive the episode. What
 * is paid is output against the dummy's own unscaled health -- a kill's worth of damage is worth Dummy.Damage however
 * long the dummy lasts -- and, for a mana user, the mana left at the end.
 */

namespace
{
    /// How far the moving drill's dummies wander from where they stand (yards).
    constexpr float WANDER_YARDS = 12.0f;

    /// How often the bleeding drill's damage lands (ms).
    constexpr uint32 BLEED_TICK_MS = 1000;
}

Animus::Curriculum::DummyEncounter::DummyEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::DummyEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Kill, RewardTerm::Death, RewardTerm::HealthKept, RewardTerm::Readiness };
}

void Animus::Curriculum::DummyEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("dummy_drill", [this](Env const& env, uint32)
    {
        return float(uint32(_envs[env.Index].Drill));
    });
    // Output in kills' worth of the dummy's own health, the number the drill is about.
    table.Add("dummy_output", [this](Env const& env, uint32) { return _envs[env.Index].Output; });
    table.Add("dummy_adds", [this](Env const& env, uint32) { return float(_envs[env.Index].Adds); });
    table.Add("dummy_killed", [this](Env const& env, uint32) { return _envs[env.Index].KillPaid ? 1.0f : 0.0f; });
    table.Add("mana_kept", [this](Env const& env, uint32) { return _envs[env.Index].ManaKept; });
}

void Animus::Curriculum::DummyEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvDummies();
}

Creature* Animus::Curriculum::DummyEncounter::Spawn(Env& env, Map* map, Player* bot, bool main)
{
    EnvDummies& dummies = _envs[env.Index];
    uint32 const entry = Opponents::OpponentPool::Instance().Random(dummies.Level);
    if (!entry)
        return nullptr;

    Creature* dummy = Opponents::SummonOpponent(bot, map, entry, Opponents::FindSpawnPoint(bot, map), dummies.Level);
    if (!dummy)
        return nullptr;

    CurriculumTuning::DummyTuning const& tuning = _scenario.Tuning().Dummy;
    bool const hits = dummies.Drill == DummyDrill::Hitting && main;
    if (main)
        dummies.BaseHealth = std::max<uint32>(1, dummy->GetMaxHealth());

    // Kept as a stat modifier, not a bare SetMaxHealth: a stat update would recompute the health and undo it.
    float const scale = hits ? tuning.HittingHealthScale : tuning.HealthScale;
    dummy->ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, (scale - 1.0f) * 100.0f);
    dummy->UpdateMaxHealth();
    dummy->SetFullHealth();

    if (!hits)
    {
        // Does not fight back: the drill is the seat's output, not a fight.
        dummy->SetReactState(REACT_PASSIVE);
        if (dummies.Drill == DummyDrill::Moving)
            dummy->GetMotionMaster()->MoveRandom(WANDER_YARDS);
        else
            dummy->GetMotionMaster()->MoveIdle();
    }

    env.Targets.push_back(dummy->GetGUID());
    return dummy;
}

bool Animus::Curriculum::DummyEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;

    EnvDummies& dummies = _envs[env.Index];
    dummies = EnvDummies();
    dummies.Drill = _scenario.Arena(env).Drill;
    dummies.Level = data.Seats[0].Level;
    CurriculumTuning::DummyTuning const& tuning = _scenario.Tuning().Dummy;
    dummies.NextAddMs = urand(tuning.AddEveryMs / 2, tuning.AddEveryMs * 3 / 2);
    dummies.NextBleedMs = BLEED_TICK_MS;

    env.Targets.clear();
    if (!Spawn(env, map, bot, true))
        return false;

    _scenario.PrepareFighter(bot, data.Seats[0]);
    return true;
}

void Animus::Curriculum::DummyEncounter::Update(Env& env)
{
    EnvDummies& dummies = _envs[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    Creature* main = env.FindTarget(0);
    if (!bot || !bot->IsAlive())
        return;

    CurriculumTuning::DummyTuning const& tuning = _scenario.Tuning().Dummy;

    // The moving drill: now and then another one to switch to, as a player's target changes in a real fight.
    if (dummies.Drill == DummyDrill::Moving && dummies.Adds < tuning.MaxAdds && env.Targets.size() < PACK_SLOTS
        && env.EpisodeElapsedMs >= dummies.NextAddMs)
    {
        if (Spawn(env, env.FindMap(), bot, false))
            ++dummies.Adds;
        dummies.NextAddMs = env.EpisodeElapsedMs + urand(tuning.AddEveryMs / 2, tuning.AddEveryMs * 3 / 2);
    }

    // The bleeding drill: damage lands on the seat whatever it does, in uneven ticks, so keeping its health up is
    // the drill -- a heal, a defensive, a bandage, whatever the kit has. Not a death sentence: at Dummy.Bleed a
    // second an untouched seat lasts longer than the episode, and what it costs is the time spent hurt.
    if (dummies.Drill == DummyDrill::Bleeding && env.EpisodeElapsedMs >= dummies.NextBleedMs)
    {
        dummies.NextBleedMs = env.EpisodeElapsedMs + BLEED_TICK_MS;
        float const share = tuning.Bleed * float(BLEED_TICK_MS) / 1000.0f * frand(0.0f, 2.0f);
        uint32 const damage = uint32(share * float(bot->GetMaxHealth()));
        if (damage)
            Unit::DealDamage(main && main->IsAlive() ? static_cast<Unit*>(main) : nullptr, bot, damage, nullptr,
                DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
    }
}

void Animus::Curriculum::DummyEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    if (seat != 0)
        return;

    EnvDummies& dummies = _envs[env.Index];
    CurriculumTuning::DummyTuning const& tuning = _scenario.Tuning().Dummy;
    CurriculumTuning::DuelTuning const& duel = _scenario.Tuning().Duel;
    SeatState& seatState = _scenario.Data(env).Seats[seat];
    CombatTally& tally = seatState.Combat;
    float const seconds = float(_scenario.DecisionMs()) / 1000.0f;

    ledger.Add(RewardTerm::StepCost, -duel.StepCost * _scenario.DecisionScale());
    if (!bot)
        return;

    // Output, per the dummy's own health: every dummy of the episode is the same level from the same pool.
    AgentStats const& step = env.StepStats[seat];
    float const output = float(step.Damage) / float(dummies.BaseHealth);
    dummies.Output += output;
    ledger.Add(RewardTerm::DamageDealt, tuning.Damage * output);
    CombatReward::Casting(bot, step, tally, _scenario.Tuning().Casting, ledger);

    if (bot->IsAlive())
    {
        // Hurt: the hitting dummy's blows as a duel charges them, and time spent below full in the bleeding drill.
        if (dummies.Drill == DummyDrill::Hitting)
            ledger.Add(RewardTerm::DamageTaken, -duel.DamageTaken * seatState.LastStepDamageTaken);
        else if (dummies.Drill == DummyDrill::Bleeding)
            ledger.Add(RewardTerm::HealthKept, -tuning.Hurt * (1.0f - bot->GetHealthPct() / 100.0f) * seconds);
    }
    else if (!dummies.DeathPaid)
    {
        dummies.DeathPaid = true;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }

    Creature const* main = env.FindTarget(0);
    if (dummies.Drill == DummyDrill::Hitting && !dummies.KillPaid && main && !main->IsAlive())
    {
        dummies.KillPaid = true;
        tally.Killed = true;
        ledger.Add(RewardTerm::Kill, tuning.Kill);
    }

    // At the end, what is left to spend: output bought with the whole mana bar is worth less than the same output
    // with some of it kept. Mana only -- rage and energy come back on their own and keeping them means nothing.
    if (!dummies.EndPaid && bot->IsAlive() && (TimeIsUp(env) || dummies.KillPaid))
    {
        dummies.EndPaid = true;
        if (bot->getPowerType() == POWER_MANA && bot->GetMaxPower(POWER_MANA))
        {
            dummies.ManaKept = float(bot->GetPower(POWER_MANA)) / float(bot->GetMaxPower(POWER_MANA));
            ledger.Add(RewardTerm::Readiness, tuning.Resource * dummies.ManaKept);
        }
    }
}

bool Animus::Curriculum::DummyEncounter::TimeIsUp(Env const& env)
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

bool Animus::Curriculum::DummyEncounter::IsTerminal(Env const& env) const
{
    return TimeIsUp(env) || _scenario.DeadForGood(env, 0) || _envs[env.Index].KillPaid;
}
