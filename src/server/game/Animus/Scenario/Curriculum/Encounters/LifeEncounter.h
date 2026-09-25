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

#ifndef ANIMUS_LIB_CURRICULUM_LIFE_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_LIFE_ENCOUNTER_H

#include "DifficultyLadder.h"
#include "Encounter.h"
#include "LifeWorld.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    struct WorldView;

    /// What the three life encounters (quest, gather, town) share: the rung is a level band drawn on the difficulty
    /// ladder, the episode is a place on a continent with the world's own spawns copied into the env's phase, the
    /// seat reads the world through the WorldBlock (SeatView::World), and the shaping is a potential on the distance
    /// to whatever the episode wants the seat at next (the waypoint).
    ///
    /// Nothing here fights on purpose: the objective creatures fight back with their own AI, and the pack block's
    /// slots are filled with whatever is in combat with the seat, then the nearest hostile of the objective, so the
    /// fighting the combat stages taught carries over unchanged.
    class LifeEncounter : public Encounter
    {
    public:
        LifeEncounter(StageScenario& scenario, uint32 envs, std::string what);

        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        void BeforeRebuild(Env& env) override;
        void BeforeLevel(Env& env) override;
        void UpdateEnemies(Env& env) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

    protected:
        struct EnvLife
        {
            uint32 Tier = 0;                    // the band
            bool Counts = false;
            bool Recorded = false;
            uint16 Layout = 0;
            uint8 Spec = 0;
            LifeWorld::Side Side = LifeWorld::Side::Any;
            std::vector<ObjectGuid> Spawned;    // what the episode put into the phase
            bool Died = false;
            bool DeathPaid = false;
            bool OutcomePaid = false;
            bool Won = false;                   // the episode's goal was reached (the subclass says when)
            bool Done = false;                  // the episode is over for a reason of its own
            uint32 Draws = 0;
            uint32 Interactions = 0;
            uint32 Wasted = 0;
            uint32 WastedPaid = 0;
            uint32 CorpsesLooted = 0;
            uint32 ItemsLooted = 0;
            uint32 CopperLooted = 0;
            uint32 GatherCasts = 0;
            // The waypoint: where the episode wants the seat next, and the potential on the way there.
            bool HasWaypoint = false;
            uint8 WaypointKind = 0;             // the subclass's own tags; a change resets the potential
            Position Waypoint;
            float LastDistance = -1.0f;
            float Progressed = 0.0f;            // yards of potential paid, for the episode info
        };

        /// The rung's band, drawn: fix the episode's map and spawn for it. False when the band has nothing.
        virtual bool Place(Env& env, EnvLife& life) = 0;
        /// The subclass's part of the seat's view: the quest fields, and the waypoint the travel block shows.
        virtual void Sensed(Env const& env, EnvLife const& life, SeatView& view) const = 0;
        /// The subclass's terms for a decision, after the shared ones.
        virtual void RewardMore(Env& env, EnvLife& life, Player* bot, RewardLedger& ledger) = 0;
        /// The subclass's accounting of what a press did (the shared counters are kept here).
        virtual void Account(Env& /*env*/, EnvLife& /*life*/, SeatActionResult const& /*result*/) { }
        /// Whether the episode's goal is reached and the episode over.
        [[nodiscard]] virtual bool Finished(Env const& env, EnvLife const& life) const = 0;
        /// The subclass's columns.
        virtual void AddMoreEpisodeInfo(EpisodeInfoTable& /*table*/) { }

        /// Point the waypoint somewhere (the potential restarts when its kind changes).
        static void SetWaypoint(EnvLife& life, uint8 kind, Position const& where);
        static void ClearWaypoint(EnvLife& life);
        /// Summon `spawn` into the env's phase and remember it. Null when the summon failed.
        Creature* Summon(Env& env, EnvLife& life, Map* map, LifeWorld::Spawn const& spawn);
        GameObject* SummonObject(Env& env, EnvLife& life, Map* map, LifeWorld::Spawn const& spawn);
        /// The world's creatures within `radius` of `where`, up to `cap`, summoned.
        uint32 SummonAround(Env& env, EnvLife& life, Map* map, Position const& where, float radius, uint32 cap,
            bool npcs);
        [[nodiscard]] float TierScale(Env const& env) const;
        [[nodiscard]] bool TimeIsUp(Env const& env) const;
        [[nodiscard]] LifeWorld::Side SideOfSeat(Env const& env) const;

        std::vector<EnvLife> _envs;
        DifficultyLadder _ladder;
        std::string _what;
    };

    /// A quest of the seat's level band: the giver, the objectives (kills, or items the objective creatures drop),
    /// the turn-in, all the world's own, copied into the env's phase around the giver. Won on the turn-in.
    class QuestEncounter final : public LifeEncounter
    {
    public:
        QuestEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;

    protected:
        bool Place(Env& env, EnvLife& life) override;
        void Sensed(Env const& env, EnvLife const& life, SeatView& view) const override;
        void RewardMore(Env& env, EnvLife& life, Player* bot, RewardLedger& ledger) override;
        void Account(Env& env, EnvLife& life, SeatActionResult const& result) override;
        [[nodiscard]] bool Finished(Env const& env, EnvLife const& life) const override;
        void AddMoreEpisodeInfo(EpisodeInfoTable& table) override;

    private:
        struct EnvQuest
        {
            LifeWorld::QuestCandidate const* Quest = nullptr;
            ObjectGuid Giver;
            ObjectGuid Ender;
            bool Accepted = false;
            bool Complete = false;
            bool TurnedIn = false;
            bool AcceptPaid = false;
            float Progress = 0.0f;              // objectives done, 0 to 1
            float ProgressPaid = 0.0f;
            uint32 Kills = 0;
        };

        std::vector<EnvQuest> _quests;
    };

    /// A field of the band's herb and ore nodes, with the zone's own creatures around them; the seat has the
    /// professions at the band's skill. Scored by the nodes gathered before the clock; there is no winning it.
    class GatherEncounter final : public LifeEncounter
    {
    public:
        GatherEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;

    protected:
        bool Place(Env& env, EnvLife& life) override;
        void Sensed(Env const& env, EnvLife const& life, SeatView& view) const override;
        void RewardMore(Env& env, EnvLife& life, Player* bot, RewardLedger& ledger) override;
        void Account(Env& env, EnvLife& life, SeatActionResult const& result) override;
        [[nodiscard]] bool Finished(Env const& env, EnvLife const& life) const override;
        void AddMoreEpisodeInfo(EpisodeInfoTable& table) override;

    private:
        struct EnvGather
        {
            LifeWorld::Ground const* Ground = nullptr;
            uint32 NodesSpawned = 0;
            uint32 NodesGathered = 0;
            uint32 NodesPaid = 0;
            uint32 SkillAtStart = 0;
            uint32 SkillUps = 0;
            uint32 SkillUpsPaid = 0;
            uint32 Skinned = 0;
        };

        std::vector<EnvGather> _gathers;
    };

    /// A town of the seat's side: its vendors, repairer and innkeeper copied into the phase around the inn. The
    /// seat arrives with junk in its bags, damaged gear, a bag of food nearly empty and two upgrades it has not put
    /// on. Won when the junk is sold, the gear repaired, the food restocked and the upgrades worn.
    class TownEncounter final : public LifeEncounter
    {
    public:
        TownEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        bool Build(Env& env, Map* map, uint8 level) override;
        void Update(Env& env) override;

    protected:
        bool Place(Env& env, EnvLife& life) override;
        void Sensed(Env const& env, EnvLife const& life, SeatView& view) const override;
        void RewardMore(Env& env, EnvLife& life, Player* bot, RewardLedger& ledger) override;
        void Account(Env& env, EnvLife& life, SeatActionResult const& result) override;
        [[nodiscard]] bool Finished(Env const& env, EnvLife const& life) const override;
        void AddMoreEpisodeInfo(EpisodeInfoTable& table) override;

    private:
        struct EnvTown
        {
            uint32 Inn = 0;
            uint32 Npcs = 0;
            uint32 JunkAtStart = 0;             // vendor value, copper
            uint32 CopperSold = 0;
            uint32 SoldPaidCopper = 0;
            uint32 Repairs = 0;
            uint32 CopperRepaired = 0;
            bool RepairPaid = false;
            bool Stocked = false;
            bool StockPaid = false;
            uint32 SuppliesBought = 0;
            uint32 Upgrades = 0;                // put in the bags at the start
            uint32 Equipped = 0;
            uint32 EquippedPaid = 0;
            bool SoldOut = false;               // no junk left
            bool Repaired = false;              // durability back to 1
        };

        std::vector<EnvTown> _towns;
    };
}

#endif
