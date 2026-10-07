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

#ifndef ANIMUS_LIB_CURRICULUM_ROLES_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_ROLES_ENCOUNTER_H

#include "CombatDraw.h"
#include "DifficultyLadder.h"
#include "Encounter.h"
#include "EntranceRespawn.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "RolesDraw.h"
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

class Creature;

namespace Animus::Curriculum
{
    /// **The roles stage** (Opposition::Roles; dungeon-curriculum G1): a party of five on the combat stages' cleared
    /// Ragefire Chasm, drilling one role an episode -- the archived curriculum's stage6 drills (tank_hold, heal_keep,
    /// damage_discipline, pull), now on real dungeon ground and perceived as a player perceives (the camera, the sight
    /// list, the party frames, the target frame's threat).
    ///
    /// **The party** (StageScenario's DrillRole makeup and PartyEncounter's core group): the drilled role in seat 0,
    /// its class and build drawn among those whose spec plays it (StageScenario::FitsDungeonRole; every class plays
    /// every role its builds can), a proper party round it -- a tank, a healer and damage dealers. Partners from the
    /// I7 pool play some of the other seats (the learner's cast.partners, never seat 0: stage.json's drill_seat), and the
    /// "human" stand-in one of them in Roles.StandInShare percent of the training episodes.
    ///
    /// **Where**: the party starts round one of the dungeon's corridor points (CombatEncounter::FindCorridors), its
    /// members Roles.PartyNearest-PartyFurthest yards from seat 0 in its sight; the first pack stands Combat.
    /// FightNearest-FightFurthest yards from seat 0 in its sight, the others further on (RolesDraw::PackSpacing). A
    /// cleared pack's place is taken by a new one beyond the last, so pack after pack comes for the episode's clock.
    ///
    /// **The drills** (ArenaDefinition::Roles; RolesDraw's pays, each the drilled seat's own Outcome):
    /// - **Hold** (tank_hold): DrillHold per enemy on the tank, less per enemy on anyone else;
    /// - **Keep** (heal_keep, packs at Roles.KeepHealthPct of their health): DrillKeep per member above half health,
    ///   less per member under 35% or dead and for the healing wasted -- keeping them all up within its mana;
    /// - **Focus** (damage_discipline): DrillFocus for its damage on the tank's target, less per enemy it took off the
    ///   tank;
    /// - **Pull** (pull): a camp of packs closer each rung; PullClean for a pack cleared with no other pack in the
    ///   fight, PullExtra for each pack drawn in beside another (the puller in full, the others Roles.PullOthers).
    /// Every seat: Clear per pack the party clears and Survived with no death of its own (Outcome, times the rung's w);
    /// Death, Away and the Clock (Cost); DamageDealt (Shaping, faded). PartyEncounter's role shaping stays for every
    /// seat but the drilled one, whose lesson is this encounter's.
    ///
    /// **Death** (I4, EntranceRespawn): never the end. A seat that dies is out Respawn.DelayMs, then alive at the
    /// entrance and walks back to the party; it has rejoined once within Respawn.RejoinYards of the living party. A
    /// wipe (the whole party down at once) is counted; the packs go home and wait. The episode ends on its clock.
    class RolesEncounter final : public Encounter
    {
    public:
        RolesEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void UpdateEnemies(Env& env) override;
        void Update(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void BeforeRewards(Env& env) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

    private:
        struct Pack
        {
            std::vector<ObjectGuid> Members;
            std::vector<bool> Counted;          // its death already counted
            Position Spot;
            bool Linked = false;
            bool Engaged = false;               // a member has been in a fight
            uint32 EngageMs = 0;
            RolesDraw::PackFight Fight;         // in a fight now; clean; counted as an extra pull
        };

        struct SeatRoles
        {
            RespawnClock Clock;                 // I4: out, risen at the entrance, back with the party
            bool DeathPaid = false;
            uint32 Deaths = 0;
            float AwaySeconds = 0.0f;
        };

        struct EnvRoles
        {
            RolesDrill Drill = RolesDrill::None;
            uint32 Tier = 0;
            bool Counts = false;
            uint16 Layout = 0;
            uint8 Spec = 0;
            uint8 Level = 1;
            bool Recorded = false;
            int32 Corridor = -1;
            std::vector<Pack> Packs;            // the pack in front first, then the ones standing further on
            std::array<SeatRoles, GROUP_SEATS> Seats{};
            bool AllDown = false;               // the whole party dead at the last decision

            // This decision.
            uint32 NewClears = 0;
            uint32 NewClean = 0;
            uint32 NewExtra = 0;

            // The episode.
            RolesDraw::Tally Tally;
            uint32 Pulls = 0;                   // packs that came into a fight
            uint32 CleanClears = 0;
            float KeptUp = 0.0f;                // the drilled healer's member-decisions above half ...
            float KeptMembers = 0.0f;           // ... of all its member-decisions in a fight
            float LowManaSeconds = 0.0f;        // ... and its seconds in a fight under a tenth of its mana
        };

        /// The pack in front's members alive and fighting, or the pack's spot; where a seat's fight is.
        [[nodiscard]] Position FightPoint(Env const& env, Player const* bot) const;
        /// The living party's middle, `seat` aside (its rejoin is measured to it); false with nobody else up.
        [[nodiscard]] bool PartyMiddle(Env const& env, uint32 seat, Position& middle) const;
        [[nodiscard]] bool FrontFighting(Env const& env) const;
        [[nodiscard]] Creature* Member(Env const& env, ObjectGuid guid) const;
        /// A pack of the drill at the episode's rung, standing `from` (null: in seat 0's sight) further on.
        bool SpawnPack(Env& env, Map* map, Player* lead, Position const* from);
        void ListTargets(Env& env) const;
        void Despawn(Env& env);
        [[nodiscard]] std::vector<CombatDraw::Point> const& Corridors(Player* bot, Map* map) const;

        std::vector<EnvRoles> _envs;
        DifficultyLadder _ladder;
        mutable std::mutex _corridorLock;
        mutable std::unordered_map<uint32, std::vector<CombatDraw::Point>> _corridors;
    };
}

#endif
