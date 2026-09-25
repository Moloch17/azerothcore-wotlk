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

#include "EnvPool.h"
#include "ResetTiming.h"
#include "RandomSeed.h"
#include "Common.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Random.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "IncomingSpell.h"
#include "SpellInfo.h"
#include "StageSettings.h"
#include "StringFormat.h"
#include <chrono>
#include <cmath>
#include "Unit.h"
#include <algorithm>

Animus::EnvPool::EnvPool(Scenario& scenario, StageSettings const& settings)
    : _scenario(scenario), _spec(scenario.Spec()), _episodeLengthMs(settings.EpisodeSeconds * IN_MILLISECONDS),
    _reportEpisodes(std::max<uint32>(1, settings.ReportEpisodes))
{
    uint32 const envs = settings.Envs;
    uint32 const agents = envs * _spec.AgentsPerEnv;

    if (_spec.Layouts.empty())
        _spec.Layouts.push_back(LayoutSpec{ scenario.Name(), _spec.ObsDim, _spec.NumActions });

    _envs.resize(envs);
    for (uint32 i = 0; i < envs; ++i)
    {
        _envs[i].Index = i;
        _envs[i].Id = settings.FirstEnvId + i;
        _envs[i].EpisodeLengthMs = _episodeLengthMs;
        _envs[i].StepStats.resize(_spec.AgentsPerEnv);
        _envs[i].EpisodeStats.resize(_spec.AgentsPerEnv);
    }

    Obs.assign(agents * _spec.ObsDim, 0.0f);
    State.assign(envs * _spec.StateDim, 0.0f);
    Mask.assign(agents * _spec.NumActions, 0);
    Rewards.assign(agents, 0.0f);
    Done.assign(envs, 0);
    Terminated.assign(envs, 0);
    FinalObs.assign(agents * _spec.ObsDim, 0.0f);
    FinalState.assign(envs * _spec.StateDim, 0.0f);
    EpisodeInfo.assign(agents * _spec.EpisodeInfoDim, 0.0f);
    Layout.assign(agents, 0);
    Present.assign(agents, 1);
    EpisodeSeed.assign(envs, NO_EPISODE_SEED);
    _envSeed.assign(envs, NO_EPISODE_SEED);
    Actions.assign(agents, 0);
    Goals.assign(agents, -1);     // Curriculum::NO_GOAL: no goal until a learner with a goal head sends one
    _reportInfoSum.assign(_spec.EpisodeInfoDim, 0.0);

    // NOT_FILED, not MapKey(0, 0): map 0 instance 0 is Eastern Kingdoms, a key an env can really have.
    _envMapKey.assign(envs, NOT_FILED);
    _envCollect.assign(envs, CollectTiming());
    _observed.assign(envs, 0);
}

bool Animus::EnvPool::Setup()
{
    for (Env& env : _envs)
    {
        if (!_scenario.Setup(env) || env.Bots.size() != _spec.AgentsPerEnv)
        {
            LOG_ERROR("module.animus", "Scenario {} failed to set up env {}", _scenario.Name(), env.Index);
            return false;
        }

        IndexEnv(env);
    }

    LOG_DEBUG("module.animus", "Scenario {}: {} envs x {} agents, obs {}, state {}, actions {}", _scenario.Name(),
        _envs.size(), _spec.AgentsPerEnv, _spec.ObsDim, _spec.StateDim, _spec.NumActions);

    return true;
}

void Animus::EnvPool::Teardown()
{
    _agents.clear();
    _allies.clear();
    _envByInstance.clear();
    _mapEnvs.clear();
    std::fill(_envMapKey.begin(), _envMapKey.end(), NOT_FILED);
    _decisionOpen = false;

    for (Env& env : _envs)
        _scenario.Teardown(env);
}

uint64 Animus::EnvPool::CompletedEpisodes() const
{
    uint64 episodes = 0;
    for (Env const& env : _envs)
        episodes += env.EpisodesCompleted;

    return episodes;
}

void Animus::EnvPool::AdvanceClock(uint32 diff)
{
    for (Env& env : _envs)
        env.EpisodeElapsedMs += diff;
}

void Animus::EnvPool::ResetAll()
{
    for (Env& env : _envs)
    {
        ResetEnv(env);

        uint32 const e = env.Index;
        _scenario.Observe(env, &Obs[e * _spec.AgentsPerEnv * _spec.ObsDim], &State[e * _spec.StateDim],
            &Mask[e * _spec.AgentsPerEnv * _spec.NumActions]);
        DescribeAgents(env);
    }

    std::fill(Rewards.begin(), Rewards.end(), 0.0f);
    std::fill(Done.begin(), Done.end(), 0);
    std::fill(Terminated.begin(), Terminated.end(), 0);

    // Every env starts afresh, so whatever a decision had scored before this is discarded with it.
    _decisionOpen = false;
}

namespace
{
    /// Nanoseconds since `from`, and `from` moved to now: the next section starts where this one ended.
    uint64 Since(std::chrono::steady_clock::time_point& from)
    {
        auto const now = std::chrono::steady_clock::now();
        uint64 const elapsed = uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - from).count());
        from = now;
        return elapsed;
    }
}

void Animus::EnvPool::BeginDecision()
{
    // Where this decision's time goes, for the host's report. The clock is read a handful of times per env, not
    // per agent or per action, so the measurement does not pay for itself.
    _collect = CollectTiming();
    _envCollect.assign(_envs.size(), CollectTiming());
    _observed.assign(_envs.size(), 0);
    _decisionOpen = true;
}

void Animus::EnvPool::ObserveEnv(Env& env)
{
    uint32 const agentsPerEnv = _spec.AgentsPerEnv;
    uint32 const e = env.Index;
    CollectTiming& timing = _envCollect[e];
    auto mark = std::chrono::steady_clock::now();

    _scenario.Reward(env, &Rewards[e * agentsPerEnv]);
    timing.RewardNs += Since(mark);

    for (uint32 agent = 0; agent < agentsPerEnv; ++agent)
    {
        env.EpisodeStats[agent].Add(env.StepStats[agent]);
        env.StepStats[agent] = AgentStats();
    }
    env.StepInterruptedTargets.clear();

    bool const terminal = _scenario.IsTerminal(env);
    bool const done = terminal || env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    Done[e] = done ? 1 : 0;
    Terminated[e] = terminal ? 1 : 0;

    _observed[e] = 1;

    if (done)
    {
        // No mask: nothing acts on the final observation. The next episode does not exist yet -- FinishEnv
        // builds it on the world thread and observes it there.
        _scenario.Observe(env, &FinalObs[e * agentsPerEnv * _spec.ObsDim], &FinalState[e * _spec.StateDim],
            nullptr);
        timing.FinalObserveNs += Since(mark);
        return;
    }

    _scenario.Observe(env, &Obs[e * agentsPerEnv * _spec.ObsDim], &State[e * _spec.StateDim],
        &Mask[e * agentsPerEnv * _spec.NumActions]);
    DescribeAgents(env);
    ++timing.Observes;
    timing.ObserveNs += Since(mark);
}

void Animus::EnvPool::FinishEnv(Env& env)
{
    uint32 const agentsPerEnv = _spec.AgentsPerEnv;
    uint32 const e = env.Index;
    auto mark = std::chrono::steady_clock::now();

    _scenario.EpisodeInfo(env, &EpisodeInfo[e * agentsPerEnv * _spec.EpisodeInfoDim]);
    EpisodeSeed[e] = _envSeed[e];

    ++env.EpisodesCompleted;
    ReportEpisode(e);
    _collect.FinalObserveNs += Since(mark);

    CurrentReset = {};
    ResetEnv(env);
    ++_collect.Resets;
    _collect.ResetNs += Since(mark);
    _collect.ResetCreateNs += CurrentReset.CreateNs;
    _collect.ResetPlaceNs += CurrentReset.PlaceNs;
    _collect.ResetConfigureNs += CurrentReset.ConfigureNs;
    _collect.ResetDestroyNs += CurrentReset.DestroyNs;

    _scenario.Observe(env, &Obs[e * agentsPerEnv * _spec.ObsDim], &State[e * _spec.StateDim],
        &Mask[e * agentsPerEnv * _spec.NumActions]);
    DescribeAgents(env);
    ++_collect.Observes;
    _collect.ObserveNs += Since(mark);
}

uint64 Animus::EnvPool::MapKey(Map const& map)
{
    return MapKey(map.GetId(), map.GetInstanceId());
}

void Animus::EnvPool::ApplyActionsForMap(Map const& map)
{
    auto const envs = _mapEnvs.find(MapKey(map));
    if (envs == _mapEnvs.end())
        return;

    auto mark = std::chrono::steady_clock::now();

    for (uint32 index : envs->second)
    {
        Env& env = _envs[index];
        if (!Goals.empty())
            _scenario.ApplyGoals(env, &Goals[index * _spec.AgentsPerEnv]);

        _scenario.ApplyActions(env, &Actions[index * _spec.AgentsPerEnv]);
    }

    // Every map's share of the apply, which they run at once: the sum, not the wall clock.
    _applyNs.fetch_add(Since(mark), std::memory_order_relaxed);
}

void Animus::EnvPool::ObserveMap(Map const& map)
{
    auto const envs = _mapEnvs.find(MapKey(map));
    if (envs == _mapEnvs.end())
        return;

    for (uint32 index : envs->second)
        ObserveEnv(_envs[index]);
}

void Animus::EnvPool::FinishCollect()
{
    if (!_decisionOpen)
        return;

    _decisionOpen = false;

    // An env whose map did not tick was never scored. That is a scheduler bug, not a reason to hand the
    // learner a stale transition, so it is scored here and said once.
    for (Env& env : _envs)
    {
        if (_observed[env.Index])
            continue;

        if (!_strayLogged)
        {
            _strayLogged = true;
            LOG_ERROR("module.animus", "Env {} is on map {} instance {}, which no map task ticked: scoring it on "
                "the world thread instead. Either the scheduler skipped the map, or the env holds no player to "
                "keep it awake (MapMgr skips a continent and MapInstanced an instance that has none).",
                env.Index, env.MapId, env.InstanceId);
        }

        ObserveEnv(env);
    }

    for (CollectTiming const& timing : _envCollect)
    {
        _collect.RewardNs += timing.RewardNs;
        _collect.ObserveNs += timing.ObserveNs;
        _collect.FinalObserveNs += timing.FinalObserveNs;
        _collect.Observes += timing.Observes;
    }

    // The maps' share of applying the last decision's actions, whenever in the ticks since they ran it.
    _collect.ApplyNs = _applyNs.exchange(0, std::memory_order_relaxed);

    for (Env& env : _envs)
        if (Done[env.Index])
            FinishEnv(env);
}

void Animus::EnvPool::DescribeAgents(Env const& env)
{
    uint32 const first = env.Index * _spec.AgentsPerEnv;
    _scenario.AgentLayouts(env, &Layout[first]);
    _scenario.AgentPresence(env, &Present[first]);
}

bool Animus::EnvPool::ChooseLocalActions(std::string const& policy, bool opponentsOnly)
{
    uint32 const agents = NumEnvs() * _spec.AgentsPerEnv;
    uint32 const numActions = _spec.NumActions;
    bool const random = policy == "random";

    for (uint32 i = 0; i < agents; ++i)
    {
        if (opponentsOnly && !_scenario.IsOpponentSeat(_envs[i / _spec.AgentsPerEnv], i % _spec.AgentsPerEnv))
            continue;

        float const* obs = &Obs[i * _spec.ObsDim];
        uint8 const* mask = &Mask[i * numActions];

        if (random)
        {
            uint32 allowed = 0;
            for (uint32 a = 0; a < numActions; ++a)
                allowed += mask[a];

            // Uniform over unmasked actions; action 0 if the scenario masked everything.
            int32 chosen = 0;
            if (allowed)
            {
                uint32 pick = urand(0, allowed - 1);
                for (uint32 a = 0; a < numActions; ++a)
                {
                    if (!mask[a])
                        continue;

                    if (pick-- == 0)
                    {
                        chosen = static_cast<int32>(a);
                        break;
                    }
                }
            }

            Actions[i] = chosen;
        }
        else if (!_scenario.ScriptedAction(policy, obs, mask, Layout[i], Actions[i]))
            return false;
    }

    return true;
}

void Animus::EnvPool::SetEvaluation(bool enabled, uint32 seedBase, uint32 episodes, std::string const& baseline,
    bool opponentsOnly)
{
    _evaluating = enabled;
    _evalSeedBase = seedBase;
    _evalEpisodes = enabled ? episodes : 0;
    _evalNextSeed = 0;
    _evalBaseline = enabled ? baseline : std::string();
    _evalOpponentsOnly = enabled && !_evalBaseline.empty() && opponentsOnly;
}

void Animus::EnvPool::SetReplay(uint32 seedBase, float fraction, std::vector<uint32> seeds)
{
    _replaySeedBase = seedBase;
    _replayFraction = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 0.0f;
    _replaySeeds = std::move(seeds);
}

void Animus::EnvPool::RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type,
    SpellInfo const* spell)
{
    if (!attacker || !victim || !damage)
        return;

    // What the agent did to itself, or the world did to it: Player::EnvironmentalDamage deals drowning, fatigue,
    // lava and falls as SELF_DAMAGE from the player to itself, and nothing below would see it. Kept apart from
    // DamageTaken, which is what enemies did.
    if (type == SELF_DAMAGE)
    {
        if (attacker == victim)
            if (auto const self = _agents.find(victim->GetGUID()); self != _agents.end())
                _envs[self->second.Env].StepStats[self->second.Agent].SelfDamage += damage;
        return;
    }
    if (type != DIRECT_DAMAGE && type != SPELL_DIRECT_DAMAGE && type != DOT)
        return;

    // Damage an agent takes. The victim is the agent itself (pets absorb their own damage).
    auto const hit = _agents.find(victim->GetGUID());
    if (hit != _agents.end())
    {
        Env& env = _envs[hit->second.Env];
        AgentStats& stats = env.StepStats[hit->second.Agent];
        stats.DamageTaken += damage;

        // Damage from a thing occupying ground rather than aimed at the agent: what stepping out of it would have
        // avoided. A persistent area aura is a ground effect; an area aura from its caster (a consecration, a boss's
        // damage aura) is the same problem from the seat's point of view -- stand somewhere else.
        if (spell && (spell->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA) || spell->HasAreaAuraEffect()))
            stats.HazardDamage += damage;

        // ... and which enemy slot dealt it, so crowd control can be paid what holding that enemy saves. A pet, totem
        // or guardian counts for its owner's slot; an attacker in no slot lands in DamageTaken alone.
        ObjectGuid const source = attacker->GetCharmerOrOwnerOrOwnGUID();
        for (std::size_t slot = 0; slot < env.Targets.size() && slot < MAX_TARGETS; ++slot)
            if (env.Targets[slot] == source)
            {
                stats.DamageTakenBy[slot] += damage;
                break;
            }
    }

    // Damage an ally takes counts against every agent of its env.
    auto const ally = _allies.find(victim->GetGUID());
    if (ally != _allies.end())
    {
        for (AgentStats& stats : _envs[ally->second.Env].StepStats)
        {
            stats.AllyDamageTaken += damage;
            stats.AllyDamageTakenBy[ally->second.Agent] += damage;
        }
    }

    if (hit != _agents.end() || ally != _allies.end())
        RecordPrevented(hit != _agents.end() ? hit->second : ally->second, hit != _agents.end(), victim, damage);

    // Pets, guardians and totems deal damage for their owner.
    auto const itr = _agents.find(attacker->GetCharmerOrOwnerOrOwnGUID());
    if (itr == _agents.end())
        return;

    // Damage counts on the env's targets, and on its other agents (self-play).
    Env& env = _envs[itr->second.Env];
    auto const victimAgent = _agents.find(victim->GetGUID());
    bool const onOtherAgent = victimAgent != _agents.end() && victimAgent->second.Env == itr->second.Env
        && victimAgent->second.Agent != itr->second.Agent;
    if (!onOtherAgent && std::find(env.Targets.begin(), env.Targets.end(), victim->GetGUID()) == env.Targets.end())
        return;

    AgentStats& stats = env.StepStats[itr->second.Agent];
    stats.Damage += damage;
    if (attacker->GetGUID() != itr->first)
        stats.PetDamage += damage;
    // The agent's own, by damage class. A player's ranged auto-attack is a spell (Auto Shot, Shoot), so a white hit
    // is a melee swing. Spell damage the hook could not match to its spell counts as a spell.
    else if (type == DIRECT_DAMAGE || (spell && spell->DmgClass == SPELL_DAMAGE_CLASS_MELEE))
        stats.MeleeDamage += damage;
    else if (spell && spell->DmgClass == SPELL_DAMAGE_CLASS_RANGED)
        stats.ShotDamage += damage;
    else
        stats.SpellDamage += damage;

    if (type == DIRECT_DAMAGE)
    {
        stats.WhiteDamage += damage;
        ++stats.WhiteHits;
    }
    else
    {
        stats.SpecialDamage += damage;
        ++stats.SpecialHits;
    }
}

void Animus::EnvPool::RecordPrevented(AgentSlot const& victimSlot, bool victimIsAgent, Unit const* victim,
    uint32 damage)
{
    // What each agent's own damage-taken reductions on the victim prevented: the hit would have been damage / the
    // product of that agent's reductions. Other casters' reductions (the owner's, a creature's) are nobody's here.
    Unit::AuraEffectList const& reductions = victim->GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN);
    if (reductions.empty())
        return;

    Env& env = _envs[victimSlot.Env];
    std::array<float, MAX_AGENTS> multiplier;
    multiplier.fill(1.0f);
    bool any = false;
    for (AuraEffect const* effect : reductions)
    {
        if (effect->GetAmount() >= 0)
            continue;

        auto const caster = _agents.find(effect->GetCasterGUID());
        if (caster == _agents.end() || caster->second.Env != victimSlot.Env || caster->second.Agent >= MAX_AGENTS)
            continue;

        multiplier[caster->second.Agent] *= std::max(0.01f, 1.0f + float(effect->GetAmount()) / 100.0f);
        any = true;
    }

    if (!any)
        return;

    for (uint32 agent = 0; agent < MAX_AGENTS && agent < env.StepStats.size(); ++agent)
    {
        if (multiplier[agent] >= 1.0f)
            continue;

        uint64 const prevented = uint64(float(damage) * (1.0f / multiplier[agent] - 1.0f));
        AgentStats& stats = env.StepStats[agent];
        if (!victimIsAgent)
            stats.AllyProtectionBy[victimSlot.Agent] += prevented;
        else if (victimSlot.Agent == agent)
            stats.SelfProtection += prevented;
        else if (victimSlot.Agent < MAX_AGENTS)
            stats.AgentProtectionBy[victimSlot.Agent] += prevented;
    }
}

void Animus::EnvPool::ResetEnv(Env& env)
{
    env.EpisodeElapsedMs = 0;

    std::vector<ObjectGuid> const previousBots = env.Bots;
    std::vector<ObjectGuid> const previousAllies = env.Allies;

    // An evaluation episode is built from its seed: the world thread's random numbers restart from it for
    // the reset (race, level, spec, talents, gear, opponents, spawn points) and go back to entropy afterwards.
    // Resets run on the world thread, and everything a scenario rolls there comes from those numbers.
    auto const seedFor = [](uint32 base, uint32 index)
    {
        uint32 const seed = (base + 1) * 2654435761u ^ (index + 1) * 2246822519u;
        return seed ? seed : 1;
    };

    _envSeed[env.Index] = NO_EPISODE_SEED;
    uint32 buildSeed = NO_EPISODE_SEED;
    if (_evaluating && _evalNextSeed < _evalEpisodes)
    {
        uint32 const index = _evalNextSeed++;
        rand_seed(seedFor(_evalSeedBase, index));
        _envSeed[env.Index] = index;
        buildSeed = index;
    }
    else if (!_evaluating && !_replaySeeds.empty() && _replayFraction > 0.0f && frand(0.0f, 1.0f) < _replayFraction)
    {
        // A lost evaluation episode, rebuilt as it was built then. It is still a training episode for the learner.
        uint32 const index = _replaySeeds[urand(0, uint32(_replaySeeds.size()) - 1)];
        rand_seed(seedFor(_replaySeedBase, index));
        buildSeed = index;
        ++_replayed;
    }

    // The scenario builds the episode knowing which seed it is: an evaluation spreads its seeds over the class/roles
    // instead of drawing them, so each is scored on its own equal share (and a replay gets the class/role it had).
    env.EpisodeSeedIndex = buildSeed;
    env.Evaluating = _evaluating;

    _scenario.Reset(env);
    _collect.Reused = _scenario.CharactersReused();

    if (buildSeed != NO_EPISODE_SEED)
        rand_seed(0);

    // After the reset: tearing down the old character (a cast cut short, its pet's last hit) still reports
    // to the hooks, and none of that belongs to the new episode.
    for (uint32 agent = 0; agent < _spec.AgentsPerEnv; ++agent)
    {
        env.StepStats[agent] = AgentStats();
        env.EpisodeStats[agent] = AgentStats();
    }
    env.StepInterruptedTargets.clear();

    // A scenario may rebuild its bots and allies on reset. Safe to update here: resets run on the world
    // thread while no map is updating, so no damage or heal hook is reading the maps.
    if (env.Bots != previousBots || env.Allies != previousAllies)
    {
        for (ObjectGuid const& guid : previousBots)
            _agents.erase(guid);
        for (ObjectGuid const& guid : previousAllies)
            _allies.erase(guid);
    }

    IndexEnv(env);
}

void Animus::EnvPool::IndexEnv(Env const& env)
{
    // An empty seat's slot holds no bot: its empty GUID is shared by every env and must never be a key.
    for (uint32 agent = 0; agent < env.Bots.size(); ++agent)
        if (!env.Bots[agent].IsEmpty())
            _agents[env.Bots[agent]] = AgentSlot{ env.Index, agent };

    for (uint32 ally = 0; ally < env.Allies.size() && ally < MAX_ALLIES; ++ally)
        if (!env.Allies[ally].IsEmpty())
            _allies[env.Allies[ally]] = AgentSlot{ env.Index, ally };

    if (env.InstanceId)
        _envByInstance[env.InstanceId] = env.Index;

    // Where the env lives now. A reset can move it (a new instance, another continent replica), so it leaves
    // the map it was filed under first.
    uint64 const key = MapKey(env.MapId, env.InstanceId);
    uint64& filed = _envMapKey[env.Index];
    if (filed == key)
        return;

    if (auto const previous = _mapEnvs.find(filed); previous != _mapEnvs.end())
    {
        std::vector<uint32>& envs = previous->second;
        envs.erase(std::remove(envs.begin(), envs.end(), env.Index), envs.end());
        if (envs.empty())
            _mapEnvs.erase(previous);
    }

    _mapEnvs[key].push_back(env.Index);
    filed = key;
}

void Animus::EnvPool::RecordHeal(Unit const* healer, Unit const* receiver, uint32 gain, bool periodic)
{
    if (!healer || !receiver || !gain)
        return;

    // Healing on an agent of the same env: itself, or a teammate.
    if (auto const patient = _agents.find(receiver->GetGUID()); patient != _agents.end())
    {
        auto const agent = _agents.find(healer->GetCharmerOrOwnerOrOwnGUID());
        if (agent == _agents.end() || agent->second.Env != patient->second.Env)
            return;

        AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
        if (agent->second.Agent == patient->second.Agent)
            stats.SelfHealing += gain;
        else if (patient->second.Agent < MAX_AGENTS)
            stats.AgentHealingBy[patient->second.Agent] += gain;
        if (periodic)
            stats.PeriodicHealing += gain;
        return;
    }

    auto const ally = _allies.find(receiver->GetGUID());
    if (ally == _allies.end())
        return;

    // Pets and totems heal for their owner.
    auto const agent = _agents.find(healer->GetCharmerOrOwnerOrOwnGUID());
    if (agent == _agents.end() || agent->second.Env != ally->second.Env)
        return;

    AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
    stats.AllyHealing += gain;
    stats.AllyHealingBy[ally->second.Agent] += gain;
    if (periodic)
        stats.PeriodicHealing += gain;
}

void Animus::EnvPool::RecordHealCast(Unit const* healer, Unit const* receiver, uint32 heal)
{
    if (!healer || !receiver || !heal)
        return;

    auto const agent = _agents.find(healer->GetCharmerOrOwnerOrOwnGUID());
    if (agent == _agents.end())
        return;

    auto const patient = _agents.find(receiver->GetGUID());
    auto const ally = _allies.find(receiver->GetGUID());
    bool const friendly = (patient != _agents.end() && patient->second.Env == agent->second.Env)
        || (ally != _allies.end() && ally->second.Env == agent->second.Env);
    if (friendly)
        _envs[agent->second.Env].StepStats[agent->second.Agent].HealingRaw += heal;
}

void Animus::EnvPool::RecordCastCompleted(Unit const* caster, Spell* spell)
{
    if (!caster || !spell || spell->IsTriggered() || spell->GetCastTime() <= 0 || spell->m_spellInfo->IsChanneled())
        return;

    auto const agent = _agents.find(caster->GetGUID());
    if (agent == _agents.end())
        return;

    AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
    ++stats.CastsCompleted;
    stats.CastMsCompleted += uint32(spell->GetCastTime());
}

void Animus::EnvPool::RecordCastCancelled(Unit const* caster, Spell* spell, bool bySelf)
{
    if (!caster || !spell || spell->IsTriggered())
        return;

    auto const agent = _agents.find(caster->GetGUID());
    if (agent == _agents.end())
    {
        RecordTargetInterrupted(caster, spell, bySelf);
        return;
    }

    // A seat's own cast stopped is also an interrupt landed by whoever stopped it. Against a creature or the scripted
    // enemy player that is the branch above, because neither is an agent; in self-play the enemy is another seat, so
    // without this an interrupt in a mirror match was filed only as the victim's cancelled cast and never recorded,
    // paid or counted.
    RecordTargetInterrupted(caster, spell, bySelf);

    // Only a cast still in its cast time: a cancelled channel has already paid out its ticks.
    if (spell->getState() != SPELL_STATE_PREPARING || spell->GetCastTime() <= 0)
        return;

    // Pushback adds to the time left, so clamp what was spent to [0, cast time].
    int32 const spent = std::clamp(spell->GetCastTime() - spell->GetCastTimeRemaining(), 0, spell->GetCastTime());

    AgentStats& stats = _envs[agent->second.Env].StepStats[agent->second.Agent];
    ++stats.CastsCancelled;
    stats.CastMsWasted += uint32(spent);

    Unit const* target = spell->m_targets.GetUnitTarget();
    // Moving is checked before bySelf: a cast the bot walked out of is cancelled by the caster too, so testing
    // bySelf first sent every move-cancel to CastsStopped and left CastsMoved dead (0 over a whole stage1_duel
    // run). Movement is the more specific cause, and telling the two apart is what says whether the policy is
    // stopping casts on purpose or running out of them.
    if (caster->IsAlive() && !caster->movespline->Finalized())
        ++stats.CastsMoved;
    else if (bySelf)
        ++stats.CastsStopped;
    else if (spell->m_targets.GetObjectTargetGUID() && (!target || !target->IsAlive() || !target->IsInWorld()))
        ++stats.CastsTargetLost;
    else
        ++stats.CastsOther;
}

void Animus::EnvPool::RecordTargetInterrupted(Unit const* caster, Spell* spell, bool bySelf)
{
    // Stopped by itself (it moved, changed its mind) or by dying is not an interrupt.
    if (bySelf || !caster->IsAlive())
        return;

    auto const env = _envByInstance.find(caster->GetInstanceId());
    if (env == _envByInstance.end())
        return;

    // A target slot holds a creature or the scripted enemy player; in self-play the enemy is another seat's bot and
    // is in no slot, so matching only the slots left every interrupt in stage15_arena and stage17_flag unrecorded --
    // and so unpaid and uncounted, however well the seat played it.
    Env& owner = _envs[env->second];
    ObjectGuid const casterGuid = caster->GetGUID();
    bool const known = std::find(owner.Targets.begin(), owner.Targets.end(), casterGuid) != owner.Targets.end()
        || std::find(owner.Bots.begin(), owner.Bots.end(), casterGuid) != owner.Bots.end();
    if (caster->GetMapId() == owner.MapId && known)
    {
        // What the interrupt was worth is decided here, while the cast still exists to be read.
        uint32 const castMs = spell && spell->GetCastTime() > 0 ? uint32(spell->GetCastTime()) : 0;
        Curriculum::IncomingSpell::Prevented const prevented =
            Curriculum::IncomingSpell::Classify(spell ? spell->m_spellInfo : nullptr, castMs);
        owner.StepInterruptedTargets.push_back({ casterGuid, uint8(prevented) });
    }
}

void Animus::EnvPool::ReportEpisode(uint32 envIndex)
{
    // Every present agent's episode counts as one; Present still describes the episode that just ended.
    for (uint32 agent = 0; agent < _spec.AgentsPerEnv; ++agent)
    {
        uint32 const row = envIndex * _spec.AgentsPerEnv + agent;
        if (!Present[row])
            continue;

        float const* info = &EpisodeInfo[row * _spec.EpisodeInfoDim];
        for (uint32 i = 0; i < _spec.EpisodeInfoDim; ++i)
            _reportInfoSum[i] += info[i];
        ++_reportedEpisodes;
    }

    if (_reportedEpisodes < _reportEpisodes)
        return;

    std::vector<std::string> const names = _scenario.EpisodeInfoNames();

    // Kept for the host's progress reports (forge status, .animus stage status) rather than logged on every batch.
    _lastEpisodeMeans.clear();
    std::string line;
    for (uint32 i = 0; i < _spec.EpisodeInfoDim; ++i)
    {
        std::string const name = i < names.size() ? names[i] : "info";
        double const mean = _reportInfoSum[i] / _reportedEpisodes;
        _lastEpisodeMeans.emplace_back(name, mean);
        line += Acore::StringFormat("{}{} {:.2f}", i ? ", " : "", name, mean);
    }

    _lastEpisodeMeansCount = _reportedEpisodes;
    LOG_DEBUG("module.animus", "Episodes {} (mean): {}", _reportedEpisodes, line);

    std::fill(_reportInfoSum.begin(), _reportInfoSum.end(), 0.0);
    _reportedEpisodes = 0;
}
