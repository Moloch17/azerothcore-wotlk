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

#ifndef ANIMUS_LIB_ENV_H
#define ANIMUS_LIB_ENV_H

#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <vector>

class Map;
class Player;
class Creature;
class Unit;

namespace Animus
{
    /// Most scripted allies an env can have (Env::Allies): a party's other four members.
    constexpr std::size_t MAX_ALLIES = 4;

    /// Most learned agents an env can have: a raid's forty seats. Indexes AgentHealingBy and AgentProtectionBy by
    /// agent, so it must cover Curriculum::MAX_SEATS -- StageScenario asserts that it does.
    constexpr std::size_t MAX_AGENTS = 40;

    /// Most enemy slots an env attributes damage to (Env::Targets): a pack's four, and the enemy players of the
    /// self-play and party arenas. Targets past this take damage as usual, they are just not attributed per slot.
    constexpr std::size_t MAX_TARGETS = 8;

    /// Combat totals for one agent. Written only by the map thread that updates the agent's
    /// instance (damage hooks), read by the world thread after MapMgr::Update has joined.
    struct AgentStats
    {
        uint64 Damage = 0;
        uint64 WhiteDamage = 0;
        uint64 SpecialDamage = 0;
        uint64 PetDamage = 0;           // of Damage, what the agent's pets and guardians dealt
        // The rest of Damage, the agent's own, by the game's damage class: melee swings and melee abilities (Raptor
        // Strike, Sinister Strike), ranged weapon attacks (Auto Shot, Steady Shot, a wand), and spells (DoTs too).
        uint64 MeleeDamage = 0;
        uint64 ShotDamage = 0;
        uint64 SpellDamage = 0;
        uint32 WhiteHits = 0;
        uint32 SpecialHits = 0;
        uint64 DamageTaken = 0;         // by the agent, from anything
        /// Damage the agent dealt itself with no attacker behind it (SELF_DAMAGE): the environment's drowning,
        /// fatigue, lava and falls, and a warlock's Life Tap. Not part of DamageTaken, which is what enemies did.
        uint64 SelfDamage = 0;
        // Of DamageTaken, what each enemy slot (Env::Targets) dealt, a pet's or totem's counted for its owner's slot.
        // What crowd control prevents is read from here: an enemy's own damage rate is what holding it out of the
        // fight saves. Damage from anything not in a target slot is in DamageTaken only.
        std::array<uint64, MAX_TARGETS> DamageTakenBy{};
        /// Of DamageTaken, what came from something standing on the ground rather than aimed at the agent: a fire
        /// pool, a poison cloud, a consecration (a persistent area aura, or an area aura from its caster). This is
        /// the damage a seat could have walked out of, and until it was counted it was indistinguishable from a
        /// melee swing.
        uint64 HazardDamage = 0;
        uint64 AllyDamageTaken = 0;     // by the env's allies (Env::Allies), from anything
        uint64 AllyHealing = 0;         // effective healing the agent (or its pets) did on the env's allies
        std::array<uint64, MAX_ALLIES> AllyDamageTakenBy{};    // the same, per Env::Allies index
        std::array<uint64, MAX_ALLIES> AllyHealingBy{};
        std::array<uint64, MAX_AGENTS> AgentHealingBy{};    // effective healing on the env's other agents, by agent
        uint64 SelfHealing = 0;         // effective healing the agent (or its pets) did on itself
        uint64 HealingRaw = 0;          // healing the agent cast on itself, its allies and agents, overhealing included
        /// Of the effective healing above, what arrived as ticks of a heal over time. A heal over time is judged by
        /// where its ticks land, not by the cast: the same Rejuvenation is the right call on a target about to be
        /// hit and waste on a full one, and only the ticks can tell the two apart.
        uint64 PeriodicHealing = 0;
        // Protection: damage the agent's own absorbs soaked (Scenario-polled) and its own damage-taken reductions
        // prevented (Pain Suppression on a friend, Barkskin on itself), on itself, per ally and per other agent.
        uint64 SelfProtection = 0;
        std::array<uint64, MAX_ALLIES> AllyProtectionBy{};
        std::array<uint64, MAX_AGENTS> AgentProtectionBy{};
        uint32 CastsCompleted = 0;      // the agent's own cast-time spells that finished casting
        uint32 CastsCancelled = 0;      // ... that were cut short (moved, stopped, interrupted, died)
        uint64 CastMsCompleted = 0;     // cast time of the completed casts
        uint64 CastMsWasted = 0;        // cast time already spent on the cancelled casts
        // Why cancelled casts were cancelled (they add up to CastsCancelled):
        uint32 CastsStopped = 0;        // by the caster itself (the stop-casting action)
        uint32 CastsMoved = 0;          // the caster was moving
        uint32 CastsTargetLost = 0;     // the cast's unit target died or is gone
        uint32 CastsOther = 0;          // anything else: interrupts, silences, stuns, form changes, death

        void Add(AgentStats const& other)
        {
            Damage += other.Damage;
            HazardDamage += other.HazardDamage;
            WhiteDamage += other.WhiteDamage;
            SpecialDamage += other.SpecialDamage;
            PetDamage += other.PetDamage;
            MeleeDamage += other.MeleeDamage;
            ShotDamage += other.ShotDamage;
            SpellDamage += other.SpellDamage;
            WhiteHits += other.WhiteHits;
            SpecialHits += other.SpecialHits;
            DamageTaken += other.DamageTaken;
            for (std::size_t target = 0; target < MAX_TARGETS; ++target)
                DamageTakenBy[target] += other.DamageTakenBy[target];
            AllyDamageTaken += other.AllyDamageTaken;
            AllyHealing += other.AllyHealing;
            for (std::size_t ally = 0; ally < MAX_ALLIES; ++ally)
            {
                AllyDamageTakenBy[ally] += other.AllyDamageTakenBy[ally];
                AllyHealingBy[ally] += other.AllyHealingBy[ally];
                AllyProtectionBy[ally] += other.AllyProtectionBy[ally];
            }
            for (std::size_t agent = 0; agent < MAX_AGENTS; ++agent)
            {
                AgentHealingBy[agent] += other.AgentHealingBy[agent];
                AgentProtectionBy[agent] += other.AgentProtectionBy[agent];
            }
            SelfHealing += other.SelfHealing;
            HealingRaw += other.HealingRaw;
            PeriodicHealing += other.PeriodicHealing;
            SelfProtection += other.SelfProtection;
            CastsCompleted += other.CastsCompleted;
            CastsCancelled += other.CastsCancelled;
            CastMsCompleted += other.CastMsCompleted;
            CastMsWasted += other.CastMsWasted;
            CastsStopped += other.CastsStopped;
            CastsMoved += other.CastsMoved;
            CastsTargetLost += other.CastsTargetLost;
            CastsOther += other.CastsOther;
        }
    };

    /// One environment: an instance map holding the scenario's bots and target(s).
    ///
    /// Objects are held by GUID and resolved per use, never as raw pointers across ticks.
    struct Env
    {
        uint32 Index = 0;                   // position in its pool
        uint32 Id = 0;                      // unique among the server's envs: bot accounts and names (StageSettings)

        /// The env's instance. A host that sets both before EnvPool::Setup has the env built in that instance (the
        /// stage viewer puts it in its player's); left 0, the first seat's bot opens a new one.
        uint32 MapId = 0;
        uint32 InstanceId = 0;

        std::vector<ObjectGuid> Bots;       // one per agent, agent order
        std::vector<ObjectGuid> Targets;
        std::vector<ObjectGuid> Allies;     // scripted friendly players the agents fight for (not agents)

        /// The seed index of the episode being built or played when it is an evaluation episode, so a scenario can
        /// spread the seeds evenly over what it would otherwise draw at random (its class/roles). EnvPool sets it
        /// before Scenario::Reset; NO_EPISODE_SEED (EnvPool.h) for a training episode.
        uint32 EpisodeSeedIndex = 0xFFFFFFFF;
        /// This episode is being scored, not trained on. A seed alone does not say so -- a replayed evaluation is
        /// a training episode with a seed -- and what stands on the control ground has to be the scored episodes
        /// only, or the ground is not held out at all.
        bool Evaluating = false;

        uint32 EpisodeElapsedMs = 0;
        uint32 EpisodeLengthMs = 0;
        uint32 EpisodesCompleted = 0;

        std::vector<AgentStats> StepStats;      // since the last decision
        std::vector<AgentStats> EpisodeStats;   // since the last reset

        /// Targets whose cast or channel was cut short by something other than themselves since the last decision
        /// (an interrupt, stun, silence, ...). Written by the map thread updating the env's instance.
        /// Enemies whose cast was stopped this decision, with what the cast was worth stopping (IncomingSpell's
        /// Prevented, kept as a plain value so Env stays free of the curriculum's headers). An interrupt is paid by
        /// what it prevented, so the kind has to survive the moment the cast dies -- afterwards there is nothing
        /// left to read it from.
        struct InterruptedCast
        {
            ObjectGuid Caster;
            uint8 Prevented = 0;
        };

        std::vector<InterruptedCast> StepInterruptedTargets;

        [[nodiscard]] Map* FindMap() const;
        [[nodiscard]] Player* FindBot(uint32 agent) const;
        [[nodiscard]] Creature* FindTarget(uint32 target) const;
        [[nodiscard]] Unit* FindTargetUnit(uint32 target) const;  // a creature or a player target
    };
}

#endif
