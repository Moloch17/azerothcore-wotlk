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
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Creature.h"
#include "DynamicObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opponents.h"
#include "TemporarySummon.h"
#include "Player.h"
#include "StageScenario.h"

/*
 * A hazard drill with nothing to fight: fire lands under the seat every few seconds and stays, so the only thing
 * that hurts is standing still.
 *
 * Why an invisible caster rather than a trap gameobject. The first build of this used GAMEOBJECT_TYPE_TRAP, which
 * is the one ground hazard that needs no unit behind it, and it burned the seats and taught them nothing: the two
 * halves of the hazard machinery read different things. Encoding::FindNearestHazard, which the observation uses,
 * accepts a trap. Encoding::StandingInHazards, which the charge uses, counts DYNOBJ_AURA_TYPE auras and nothing
 * else. A trap leaves no such aura, so hazard_seconds, hazard_damage and reward_hazard all stayed at zero while
 * health_left fell to 0.68 -- damage with no signal, which is worse than no drill.
 *
 * So the fire is a real persistent area aura, cast by a World Invisible Trigger that is hostile, immune,
 * unselectable and unattackable. There is nothing to fight and nothing to target, but the DynamicObject it leaves
 * is the same one the rest of the curriculum's hazards leave, so every sensing and reward path works unchanged.
 * The spells are the ones the world's own hazard casters use (OpponentPool::RandomHazardSpell).
 *
 * Why under the seat. A hazard drill whose fire sits in fixed places teaches nothing: every reward in an episode
 * with no enemy is a penalty (RewardTerm::Hazard is charged and never paid), so a policy would learn to stand in
 * a clear corner and do nothing, which is the behaviour the hazard cap exists to prevent in the stages that do
 * have a fight. Fire that lands where the seat is standing removes that option -- the only way to spend less is to
 * move -- so the penalty alone is enough and the drill needs no objective of its own.
 *
 * Why the emitter is not the seat's target. It was, once, and for a reason: every movement action used to be
 * target-relative, so a stage with nothing to target had nothing it could do -- the first run of this drill spent
 * four million steps with allowed_actions at 1.00, entropy at 0 and the seats standing perfectly still while the
 * fire burned them. Handing over an immune, unkillable creature purely so the movement actions would unmask was
 * the cheapest way out at the time.
 *
 * MoveBlock is what that was standing in for, and it needs no target at all: eight bearings, a halt, a held yaw
 * and the ground read along each one. So the emitter goes back to being only what it is, a thing that lays fire,
 * and the drill is finally what it was written to be -- a stage about the ground with nothing in it to fight.
 * A fake enemy in the observation is a fake enemy the policy learns to read.
 *
 * What it is not: a fight. Nothing can be killed and the episode always runs its full length. The numbers to read
 * are hazard_seconds and hazard_damage, both of which should fall.
 */

namespace
{
    /// The stock invisible trigger: unselectable, unattackable and immune by its template's flags, so it can hold
    /// a hostile faction without ever becoming something to fight.
    constexpr uint32 EMITTER_ENTRY = 12999;

    /// Hostile to players, so Encoding::FindNearestHazard reads what it casts as a hazard rather than as somewhere
    /// friendly to stand (it takes a DynamicObject only from a caster the seat is not friendly to).
    constexpr uint32 EMITTER_FACTION = 14;

    /// A new patch under each living seat this often. At the 250 ms decision that is a patch every ten decisions:
    /// long enough to have moved out deliberately rather than by accident, short enough that a stationary seat is
    /// always standing in something.
    constexpr uint32 PLACE_EVERY_MS = 2500;

    /// How long the emitter lives; the patches it lays expire on their own spells' duration.
    constexpr uint32 EMITTER_SECONDS = 600;
}

Animus::Curriculum::HazardEncounter::HazardEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

void Animus::Curriculum::HazardEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("hazard_patches", [this](Env const& env, uint32) { return float(_envs[env.Index].Placed); });
}

void Animus::Curriculum::HazardEncounter::ResetEpisode(Env& env)
{
    Clear(env);
    EnvHazards& state = _envs[env.Index];
    state.Placed = 0;
    state.NextMs = 0;
}

void Animus::Curriculum::HazardEncounter::BeforeRebuild(Env& env)
{
    Clear(env);
}

bool Animus::Curriculum::HazardEncounter::Build(Env& env, Map* map, uint8 level)
{
    EnvHazards& state = _envs[env.Index];
    // The first patch lands one interval in, so a seat is never already standing in one on its first decision.
    state.NextMs = PLACE_EVERY_MS;
    state.Spell = Opponents::OpponentPool::Instance().RandomHazardSpell(level);
    if (!state.Spell)
    {
        LOG_ERROR("module.animus", "{}: the world has no hazard spell to cast; env {} has nothing to avoid",
            _scenario.Name(), env.Index);
        return false;
    }

    Player* first = _scenario.SeatBot(env, 0);
    if (!map || !first)
        return false;

    // One emitter for the env, standing where the seats start. It never moves and never needs to: a persistent
    // area aura is cast at a point, not at a target, so range is the only thing that matters and the arena is
    // small. Hostile so its ground reads as a hazard, and immune and unselectable so it is not a fight.
    TempSummon* emitter = map->SummonCreature(EMITTER_ENTRY, *first, nullptr, EMITTER_SECONDS * IN_MILLISECONDS);
    if (!emitter)
        return false;

    // Into the env's own phase, as an opponent is (Opponents::SummonOpponent): a summon with no summoner is
    // created in the world's phase, which no env's bots stand in.
    emitter->SetPhaseMask(first->GetPhaseMask(), true);
    emitter->SetFaction(EMITTER_FACTION);
    // Unattackable and immune, so it can never be killed and the episode can never be won; not NOT_SELECTABLE,
    // because the seat is handed it as a target and the encoders read it. Staying alive is what keeps the
    // movement actions unmasked.
    emitter->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC));
    emitter->SetImmuneToAll(true);
    emitter->SetReactState(REACT_PASSIVE);
    state.Emitter = emitter->GetGUID();
    return true;
}

bool Animus::Curriculum::HazardEncounter::SelectTarget(Env const& env, uint32 /*seat*/, Unit*& target)
{
    if (_scenario.Arena(env).Against != Opposition::Hazards)
        return false;

    // Nothing to fight, and nothing pretending to be. The seat steers with MoveBlock, which asks for no target.
    target = nullptr;
    return true;
}

void Animus::Curriculum::HazardEncounter::Update(Env& env)
{
    if (_scenario.Arena(env).Against != Opposition::Hazards)
        return;

    EnvHazards& state = _envs[env.Index];
    if (env.EpisodeElapsedMs < state.NextMs)
        return;

    Map* map = env.FindMap();
    Creature* emitter = map ? map->GetCreature(state.Emitter) : nullptr;
    if (!emitter)
        return;

    state.NextMs = env.EpisodeElapsedMs + PLACE_EVERY_MS;
    for (uint32 seat = 0; seat < uint32(env.Bots.size()); ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsAlive())
            continue;

        // Under the seat, where it is standing now. Triggered, so nothing about the emitter's own state -- line
        // of sight, facing, power, a global cooldown -- can stop the fire arriving.
        emitter->CastSpell(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), state.Spell, true);
        ++state.Placed;
    }
}

bool Animus::Curriculum::HazardEncounter::IsTerminal(Env const& env) const
{
    if (_scenario.Arena(env).Against != Opposition::Hazards)
        return false;

    // Nothing to clear, so the episode runs its length. It ends early only when every seat that exists is dead.
    //
    // "Exists" is the load-bearing word. Asking only whether anything is alive says yes-it-is-over in the moment
    // before the seats are built, which ended all 128 episodes of the first run at once, none of them having taken
    // a single decision. A seat with no bot behind it has not died; it has not started.
    bool anySeat = false;
    for (uint32 seat = 0; seat < uint32(env.Bots.size()); ++seat)
    {
        Player const* bot = _scenario.SeatBot(env, seat);
        if (!bot)
            continue;

        anySeat = true;
        if (bot->IsAlive())
            return false;
    }
    return anySeat;
}

void Animus::Curriculum::HazardEncounter::Clear(Env& env)
{
    EnvHazards& state = _envs[env.Index];
    if (Map* map = env.FindMap())
        if (Creature* emitter = map->GetCreature(state.Emitter))
        {
            // Its ground goes with it: a DynamicObject belongs to its caster, so nothing burns into the next
            // episode and no seat starts one standing in the last one's fire.
            emitter->RemoveAllDynObjects();
            emitter->DespawnOrUnsummon();
        }

    state.Emitter.Clear();
}
