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
#include "BotAccounts.h"
#include "CombatReward.h"
#include "CombatRewardScenario.h"
#include "Env.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <algorithm>

static_assert(Animus::Curriculum::MAX_AMBUSHERS <= Animus::BotAccounts::AMBUSHERS_PER_ENV,
    "every ambusher needs its own bot accounts");

Animus::Curriculum::AmbushEncounter::AmbushEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::AmbushEncounter::RewardTerms() const
{
    // Alone it is a one-on-one; beside pulls only the kills are its own.
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::StealthUtility, RewardTerm::Kill,
        RewardTerm::HealthKept, RewardTerm::Death, RewardTerm::PlayerKill };
}

void Animus::Curriculum::AmbushEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("ambushers", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Arrived ? float(_envs[env.Index].Count) : 0.0f;
    });
    table.Add("ambushers_killed", [this](Env const& env, uint32) { return float(_envs[env.Index].Killed); });
}

void Animus::Curriculum::AmbushEncounter::ResetEpisode(Env& env)
{
    EnvAmbush& ambush = _envs[env.Index];
    ambush.Count = 0;
    ambush.ArriveMs = 0;
    ambush.Arrived = false;
    ambush.Killed = 0;
    ambush.StepKills = 0;
    for (Ambusher& ambusher : ambush.Ambushers)
        ambusher.KillCounted = false;
}

void Animus::Curriculum::AmbushEncounter::BeforeRebuild(Env& env)
{
    // Last episode's ambushers leave before the new characters come.
    RemoveBots(env);
}

bool Animus::Curriculum::AmbushEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvAmbush& ambush = _envs[env.Index];
    ArenaDefinition const& arena = _scenario.Arena(env);
    ambush.Count = urand(1, std::clamp<uint32>(arena.Ambushers, 1, MAX_AMBUSHERS));

    if (!Alone(env))
    {
        // Beside pulls: at a random time, early enough to matter.
        CurriculumTuning::AmbushTuning const& tuning = _scenario.Tuning().Ambush;
        uint32 const latest = std::max<uint32>(tuning.MinMs, std::min(tuning.MaxMs, env.EpisodeLengthMs * 3 / 4));
        ambush.ArriveMs = urand(std::min(tuning.MinMs, latest), latest);
        return true;
    }

    // Alone: the fight starts now, and nothing else prepares the seats for it.
    EnvState& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        _scenario.PrepareFighter(_scenario.SeatBot(env, seat), data.Seats[seat]);

    return Arrive(env, map);
}

bool Animus::Curriculum::AmbushEncounter::Arrive(Env& env, Map* map)
{
    EnvAmbush& ambush = _envs[env.Index];
    EnvState const& data = _scenario.Data(env);
    Player* lead = _scenario.SeatBot(env, 0);
    Player* owner = _scenario.Owner(env);
    Player* anchor = owner && owner->IsAlive() ? owner : lead;
    if (!lead || !anchor || !map)
        return false;

    CurriculumTuning::OpponentTuning const& opponents = _scenario.Tuning().Opponent;
    uint32 const envIndex = env.Id;
    uint32 created = 0;

    // The ambushers made take the first slots, so Find(0..Count-1) reaches every one of them even when one failed.
    for (uint32 attempt = 0; attempt < ambush.Count; ++attempt)
    {
        uint32 const index = created;
        Ambusher& ambusher = ambush.Ambushers[index];
        uint8 const level = uint8(std::clamp<int32>(int32(data.Seats[0].Level)
            + irand(-opponents.LevelSpread, opponents.LevelSpread), 1, DEFAULT_MAX_LEVEL));

        EnemyPlayers::Naming const naming{
            [envIndex, index](uint8 session) { return Acore::StringFormat("Amb{}{}{}", envIndex, index,
                session ? "b" : "a"); },
            [envIndex, index](uint8 session) { return BotAccounts::Ambusher(envIndex, index, session); },
        };

        EnemyPlayers::Spawned const spawned = EnemyPlayers::Create(ambusher.Bot, naming, level, opponents, anchor,
            map, _scenario.SpawnMapId(), ambusher.Script);
        if (!spawned.Bot)
            continue;

        ambusher.Class = spawned.Class;
        ambusher.Apt = spawned.Apt;
        ambusher.KillCounted = false;
        ambusher.Script.EngageMs = env.EpisodeElapsedMs + urand(0, _scenario.Tuning().Ambush.EngageMaxMs);

        EnemyPlayers::MakeEnemies(lead, spawned.Bot);
        EnemyPlayers::Flag(owner);
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            EnemyPlayers::Flag(_scenario.SeatBot(env, seat));

        // Ambushers keep the first slots the pulls leave free (the pulls stay within PACK_SLOTS - Ambushers).
        env.Targets.push_back(spawned.Bot->GetGUID());
        ++created;
    }

    ambush.Count = created;
    ambush.Arrived = true;
    return created > 0;
}

void Animus::Curriculum::AmbushEncounter::Update(Env& env)
{
    EnvAmbush& ambush = _envs[env.Index];
    if (!ambush.Arrived)
    {
        if (!Alone(env) && env.EpisodeElapsedMs >= ambush.ArriveMs)
            if (!Arrive(env, env.FindMap()))
                ambush.ArriveMs = ~uint32(0);   // nobody could come: no ambush this episode
        return;
    }

    EnvState const& data = _scenario.Data(env);
    Player* owner = _scenario.Owner(env);
    Player* lead = _scenario.SeatBot(env, 0);

    for (uint32 index = 0; index < ambush.Count; ++index)
    {
        Player* enemy = Find(env, index);
        if (!enemy || !enemy->IsAlive() || !lead)
            continue;

        // The owner while it lives, then the nearest living seat it can see (a hidden one only when none is seen).
        Player* victim = owner && owner->IsAlive() ? owner : nullptr;
        if (!victim)
        {
            bool victimSeen = false;
            for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            {
                Player* bot = _scenario.SeatBot(env, seat);
                if (!bot || !bot->IsAlive())
                    continue;

                bool const seen = enemy->CanSeeOrDetect(bot);
                if (!victim || (seen && !victimSeen)
                    || (seen == victimSeen && enemy->GetDistance(bot) < enemy->GetDistance(victim)))
                {
                    victim = bot;
                    victimSeen = seen;
                }
            }
        }
        if (!victim)
            continue;

        // Zone updates can drop the PvP flag; the fight needs it. The lead seat's team decides the sides: the owner
        // and teammates share its faction, not necessarily its race's team.
        if (!enemy->IsPvP() || !victim->IsPvP())
        {
            EnemyPlayers::MakeEnemies(lead, enemy);
            EnemyPlayers::Flag(victim);
        }

        ScriptedPlayer::UpdateOpponent(enemy, victim, env.EpisodeElapsedMs, ambush.Ambushers[index].Script,
            _scenario.Tuning().ScriptedPlayers);
    }
}

bool Animus::Curriculum::AmbushEncounter::SelectTarget(Env const& env, uint32 seatIndex, Unit*& target)
{
    // Beside pulls, the pulls pick targets among every enemy slot, ambushers included.
    if (!Alone(env))
        return false;

    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (Unit* selected = env.FindTargetUnit(seat.TargetSlot); selected && selected->IsAlive())
    {
        target = selected;
        return true;
    }

    Player* bot = env.FindBot(seatIndex);
    target = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!target || (bot && bot->GetDistance(enemy) < bot->GetDistance(target)))
        {
            target = enemy;
            seat.TargetSlot = slot;
        }
    }

    // Nobody alive: the dead ambusher stays the target, as the one-on-one's opponent does.
    if (!target)
        target = env.FindTargetUnit(0);
    return true;
}

void Animus::Curriculum::AmbushEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    uint32 index = 0;
    Player* enemy = FirstAlive(env, &index);
    if (!enemy)
        return;

    view.Opponent = enemy;
    view.OpponentClass = _envs[env.Index].Ambushers[index].Class;
    view.OpponentApt = _envs[env.Index].Ambushers[index].Apt;
    view.Mirror = false;
}

void Animus::Curriculum::AmbushEncounter::BeforeRewards(Env& env)
{
    EnvAmbush& ambush = _envs[env.Index];
    ambush.StepKills = 0;
    for (uint32 index = 0; index < ambush.Count; ++index)
    {
        Ambusher& ambusher = ambush.Ambushers[index];
        Player* enemy = Find(env, index);
        if (enemy && !enemy->IsAlive() && !ambusher.KillCounted)
        {
            ambusher.KillCounted = true;
            ++ambush.StepKills;
            ++ambush.Killed;
        }
    }
}

void Animus::Curriculum::AmbushEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    if (Alone(env))
    {
        // The fight is against the one ambusher, even once it is dead (its kill is paid then).
        if (Player* enemy = Find(env, 0); bot && enemy)
            CombatReward::OneOnOne(_scenario, env, seat, bot, enemy, ledger);
        return;
    }

    if (uint32 const kills = _envs[env.Index].StepKills)
        ledger.Add(RewardTerm::PlayerKill, _scenario.Tuning().Ambush.Kill * float(kills));
}

bool Animus::Curriculum::AmbushEncounter::IsTerminal(Env const& env) const
{
    if (!Alone(env))
        return false;

    return _scenario.Data(env).Seats[0].Combat.Killed || _scenario.DeadForGood(env, 0);
}

Player* Animus::Curriculum::AmbushEncounter::Find(Env const& env, uint32 ambusher) const
{
    if (ambusher >= _envs[env.Index].Count)
        return nullptr;

    Player* enemy = _envs[env.Index].Ambushers[ambusher].Bot.Active();
    return enemy && enemy->IsInWorld() ? enemy : nullptr;
}

Player* Animus::Curriculum::AmbushEncounter::FirstAlive(Env const& env, uint32* index) const
{
    if (!_envs[env.Index].Arrived)
        return nullptr;

    for (uint32 ambusher = 0; ambusher < _envs[env.Index].Count; ++ambusher)
    {
        if (Player* enemy = Find(env, ambusher); enemy && enemy->IsAlive())
        {
            if (index)
                *index = ambusher;
            return enemy;
        }
    }

    return nullptr;
}

void Animus::Curriculum::AmbushEncounter::RemoveBots(Env& env)
{
    EnvAmbush& ambush = _envs[env.Index];
    for (Ambusher& ambusher : ambush.Ambushers)
    {
        if (Player* enemy = ambusher.Bot.Active())
        {
            ObjectGuid const guid = enemy->GetGUID();
            std::erase(env.Targets, guid);
        }
        ambusher.Bot.Destroy();
    }

    ambush.Arrived = false;
}

void Animus::Curriculum::AmbushEncounter::Deactivate(Env& env)
{
    RemoveBots(env);
    for (Ambusher& ambusher : _envs[env.Index].Ambushers)
    {
        ambusher.Class = 0;
        ambusher.Apt = Aptitude();
    }
}

void Animus::Curriculum::AmbushEncounter::Teardown(Env& env)
{
    RemoveBots(env);
}
