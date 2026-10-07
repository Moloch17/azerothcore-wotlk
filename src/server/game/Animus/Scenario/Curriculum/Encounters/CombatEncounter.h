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

#ifndef ANIMUS_LIB_CURRICULUM_COMBAT_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_COMBAT_ENCOUNTER_H

#include "CombatDraw.h"
#include "DifficultyLadder.h"
#include "Encounter.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <mutex>
#include <unordered_map>
#include <vector>

class Creature;

namespace Animus::Curriculum
{
    /// **The combat stages** (Opposition::Combat; dungeon-curriculum C1-C3): creatures on a cleared dungeon's own
    /// ground -- Ragefire Chasm, its creatures removed (SpawnArea::ClearMap) -- fought by a seat that finds them by
    /// sight and acts on what it sees (the sight and combat blocks, I1-I3): its target is its own selection, its
    /// spells go as the client sends them, its enemy list is the camera's.
    ///
    /// **Where**: each episode the seat starts at one of the dungeon's own creature spawn points within
    /// Combat.CorridorWalk yards' walk of the entrance (CombatDraw::CorridorPoints, once per map), facing a random way
    /// (an evaluation's seed picks the point); the pull stands Combat.FightNearest to FightFurthest yards off, in its
    /// line of sight (Opponents::FindSpawnPoint), and in C2 and C3 the next pack NextNearest to NextFurthest yards
    /// further on.
    ///
    /// **What** (ArenaDefinition::Combat, CombatDraw::PlanPull at the class and build's rung, DifficultyLadder):
    /// - **Fight (C1)**: one creature at a time from the open-world pool, the next Combat.NextFightMs after each kill;
    ///   casters from Combat.CasterTier, an elite from EliteTier. The `guard` arena stands a friendly fighter by the
    ///   seat (passive: it never strikes back) whom each creature goes for first: taunting it off and healing it are
    ///   the drill. Paid: Kill.
    /// - **Packs (C2)**: packs of 2-4 (a caster, linked, fire underfoot) with the next one standing further on: pulling
    ///   it before this one is down is an extra pull (ExtraPull, a Cost). Paid: Clear, InterruptLanded.
    /// - **Survive (C3)**: packs that can kill, the next waiting until the seat goes for it: food and drink are
    ///   stocked (the gauntlet block's rest), so resting between pulls is the seat's own choice. Paid: Clear,
    ///   InterruptLanded, Survived, Rejoin.
    ///
    /// **Death** (dungeon-curriculum I4, ArenaDefinition::RespawnAtEntrance): never the end of the episode. The seat is
    /// out Combat.RespawnDelayMs, then alive at the dungeon's entrance (StageScenario::RespawnAtEntrance, the seam I4
    /// replaces) and walks back on the controller, by its own map and memory; the pull it died to goes home. It is
    /// back once within Combat.RejoinYards of where it fell (Rejoin). The episode ends on its clock alone.
    ///
    /// **Outcome** (times the tier's w): Kill, Clear, Survived; InterruptLanded and Rejoin. **Cost**, at full price
    /// from the first step: Death and AllyDeath (over w), Hurt, FireHurt, ExtraPull, Clock (StepCost: per second a
    /// pull's creature lives engaged). **Shaping**: DamageDealt (Combat.Damage), faded.
    class CombatEncounter final : public Encounter
    {
    public:
        CombatEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

        /// The places a seat may start on `map` (CombatDraw::CorridorPoints over its creature spawns), worked out once
        /// per map from `bot` standing at its entrance; empty when it has none.
        [[nodiscard]] std::vector<CombatDraw::Point> const& Corridors(Player* bot, Map* map) const;

    private:
        struct Pack
        {
            std::vector<ObjectGuid> Members;
            std::vector<bool> Counted;          // its death already counted
            Position Spot;
            bool Engaged = false;               // a member entered combat
            uint32 EngageMs = 0;
            bool Linked = false;
            bool Hazard = false;
            bool Caster = false;
            bool Elite = false;
            bool ExtraCounted = false;          // as the next pack, drawn in early: charged once
        };

        struct EnvCombat
        {
            uint32 Tier = 0;
            bool Counts = false;
            uint16 Layout = 0;
            uint8 Spec = 0;
            bool Recorded = false;
            uint8 Level = 1;
            int32 Corridor = -1;
            float StartWalk = 0.0f;

            std::vector<Pack> Packs;            // the pull in front, then (C2, C3) the next one
            uint32 NextFightMs = 0;             // C1: the next creature comes at this episode time; 0 none waiting
            ObjectGuid Ally;                    // the guard arena's friend
            bool AllyDead = false;
            bool NewAllyDeath = false;          // ... fell this decision

            // This decision.
            uint32 NewKills = 0;
            uint32 NewClears = 0;
            uint32 NewExtra = 0;
            ObjectGuid PendingInterrupt;

            // The episode.
            uint32 Kills = 0;
            uint32 Clears = 0;
            uint32 Pulls = 0;
            uint32 ExtraPulls = 0;
            uint32 Interrupts = 0;
            uint32 Deaths = 0;
            uint32 Respawns = 0;
            uint32 Rejoins = 0;
            float RejoinSeconds = 0.0f;
            uint32 AllyDeaths = 0;
            uint32 HazardPulls = 0;
            uint32 LinkedPulls = 0;
            uint32 CasterPulls = 0;
            float KillSeconds = 0.0f;
            float HurtShare = 0.0f;
            float FireShare = 0.0f;
            uint32 Decisions = 0;
            uint32 InViewDecisions = 0;
            uint32 SelectedDecisions = 0;
            uint32 RestMs = 0;

            // Death and the way back.
            bool DeathPaid = false;
            uint32 DeadSinceMs = 0;
            bool RejoinPending = false;
            uint32 RespawnedAtMs = 0;
            CombatDraw::Point FellAt;
        };

        [[nodiscard]] CombatDrill Drill(Env const& env) const;
        /// Spawn a pull of the episode's rung, centred `from` (or round the seat when null), as the back of the queue.
        bool SpawnPull(Env& env, Map* map, Player* bot, Position const* from);
        /// The friend of the guard arena, by the seat; its creatures go for it first.
        void SpawnAlly(Env& env, Map* map, Player* bot);
        /// env.Targets as the packs stand (the critic's and the step statistics' list, never the seat's).
        void ListTargets(Env& env) const;
        [[nodiscard]] Creature* Member(Env const& env, ObjectGuid guid) const;
        void Despawn(Env& env);

        std::vector<EnvCombat> _envs;
        DifficultyLadder _ladder;
        mutable std::mutex _corridorLock;
        mutable std::unordered_map<uint32, std::vector<CombatDraw::Point>> _corridors;
    };
}

#endif
