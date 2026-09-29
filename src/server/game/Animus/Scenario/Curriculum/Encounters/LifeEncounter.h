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

#include "Block.h"
#include "DifficultyLadder.h"
#include "DirectorLayout.h"
#include "Encounter.h"
#include "LifeWorld.h"
#include "WorldCoordinator.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "StageDefinition.h"
#include <array>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

class Group;

namespace Animus::Curriculum
{
    struct WorldView;

    /// Groups a life episode can hold: the two sides sharing a zone, and each seat questing alone beside them.
    constexpr uint32 LIFE_GROUPS = TEAM_COUNT + MAX_LONE_SEATS;

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
        /// What one seat did and earned (a life arena of a group has several).
        struct SeatLife
        {
            bool Died = false;
            bool DeathPaid = false;
            uint32 Interactions = 0;
            uint32 Wasted = 0;
            uint32 WastedPaid = 0;
            uint32 CorpsesLooted = 0;
            uint32 ItemsLooted = 0;
            uint32 CopperLooted = 0;
            uint32 GatherCasts = 0;
            float LastDistance = -1.0f;         // to its group's waypoint, for the potential
            float Progressed = 0.0f;            // yards of potential paid, for the episode info
        };

        /// Places the journal keeps of things seen (nodes in a field, traders in a town): the newest JOURNAL_PLACES,
        /// merged when close, with when each was last seen.
        struct FoundPlaces
        {
            struct Found { Position Where; uint32 SeenMs = 0; };
            std::vector<Found> Places;
            void Seen(Position const& where, uint32 nowMs, float merge = 25.0f);
            void Write(WorldView& world, uint32 nowMs) const;
            void Forget(Position const& where, float within = 5.0f);
        };

        /// Where the episode wants a group next, and the potential on the way there.
        struct Waypoint
        {
            bool Has = false;
            uint8 Kind = 0;                     // the subclass's own tags; a change resets the potential
            Position Where;
        };

        struct EnvLife
        {
            uint32 Tier = 0;                    // the band
            bool Counts = false;
            bool Recorded = false;
            uint16 Layout = 0;
            uint8 Spec = 0;
            LifeWorld::Side Side = LifeWorld::Side::Any;
            std::vector<ObjectGuid> Spawned;    // what the episode put into the phase
            bool OutcomePaid = false;
            bool Won = false;                   // the episode's goal was reached (the subclass says when)
            bool Done = false;                  // the episode is over for a reason of its own
            uint32 Draws = 0;
            std::array<SeatLife, MAX_SEATS> Seats{};
            std::array<Waypoint, LIFE_GROUPS> Ways{};
            /// A group of seats is a real group (a sim Group per side), so kills and loot credit its members.
            std::array<Group*, LIFE_GROUPS> Groups{};
        };

        /// The rung's band, drawn: fix the episode's map and spawn for it. False when the band has nothing.
        virtual bool Place(Env& env, EnvLife& life) = 0;
        /// The subclass's part of a seat's view: the quest fields, and the waypoint the travel block shows.
        virtual void Sensed(Env const& env, EnvLife const& life, uint32 seat, SeatView& view) const = 0;
        /// The subclass's terms for a seat's decision, after the shared ones.
        virtual void RewardMore(Env& env, EnvLife& life, uint32 seat, Player* bot, RewardLedger& ledger) = 0;
        /// The subclass's accounting of what a seat's press did (the shared counters are kept here).
        virtual void Account(Env& /*env*/, EnvLife& /*life*/, uint32 /*seat*/, SeatActionResult const& /*result*/) { }
        /// Whether the episode's goal is reached and the episode over.
        [[nodiscard]] virtual bool Finished(Env const& env, EnvLife const& life) const = 0;
        /// The subclass's columns.
        virtual void AddMoreEpisodeInfo(EpisodeInfoTable& /*table*/) { }

        /// Point a group's waypoint somewhere (the potential restarts when its kind changes).
        static void SetWaypoint(EnvLife& life, uint8 kind, Position const& where, uint32 group = 0);
        static void ClearWaypoint(EnvLife& life, uint32 group = 0);
        /// The group a seat plays in: its side (one for a party, two for two groups sharing a zone).
        [[nodiscard]] uint32 GroupOf(Env const& env, uint32 seat) const;
        /// Every seat of an arena of more than one seat into a real group per side (kill and loot credit is shared);
        /// and back out.
        void FormGroups(Env& env);
        void Disband(Env& env);
        /// Summon `spawn` into the env's phase and remember it. Null when the summon failed.
        Creature* Summon(Env& env, EnvLife& life, Map* map, LifeWorld::Spawn const& spawn);
        GameObject* SummonObject(Env& env, EnvLife& life, Map* map, LifeWorld::Spawn const& spawn);
        /// The world's creatures within `radius` of `where`, up to `cap`, summoned.
        uint32 SummonAround(Env& env, EnvLife& life, Map* map, Position const& where, float radius, uint32 cap,
            bool npcs);
        [[nodiscard]] float TierScale(Env const& env) const;
        [[nodiscard]] bool TimeIsUp(Env const& env) const;
        /// Every seat of the episode has died (with one seat, that it has).
        [[nodiscard]] bool AllDead(Env const& env) const;
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
        void Sensed(Env const& env, EnvLife const& life, uint32 seat, SeatView& view) const override;
        void RewardMore(Env& env, EnvLife& life, uint32 seat, Player* bot, RewardLedger& ledger) override;
        [[nodiscard]] bool Finished(Env const& env, EnvLife const& life) const override;
        void AddMoreEpisodeInfo(EpisodeInfoTable& table) override;
        /// The quest's undone objectives, as places a director can send members to.
        void ViewDirector(Env const& env, uint32 side, DirectorLayout::DirectorView& view) const override;

    private:
        struct EnvQuest
        {
            /// The group's quests: one, or a chain of up to three (QuestCandidate::Next), done in order.
            std::vector<LifeWorld::QuestCandidate const*> Chain;
            uint32 Current = 0;                 // the one being done
            std::vector<ObjectGuid> Givers;     // per chain quest
            std::vector<ObjectGuid> Enders;
            bool Accepted = false;              // the current quest, by any seat of the group
            bool Complete = false;              // ... by every living seat
            bool TurnedIn = false;              // ... handed in by every living seat
            float Progress = 0.0f;              // the current quest's objectives done, 0 to 1 (the group's mean)
            uint32 TurnedInCount = 0;
            uint32 Kills = 0;
            /// Places where something the current quest wants was seen (the journal's), and when.
            struct Found { Position Where; uint32 SeenMs = 0; uint8 Objective = 0; };
            std::vector<Found> Places;
            uint32 ScannedMs = 0;

            [[nodiscard]] LifeWorld::QuestCandidate const* Quest() const
            {
                return Current < Chain.size() ? Chain[Current] : nullptr;
            }
        };

        /// What each seat has been paid of its group's quest, so every member is paid its group's progress once.
        struct SeatPay
        {
            uint32 AcceptPaid = 0;              // quests of the chain whose acceptance it was paid for
            float ProgressPaid = 0.0f;          // of the current quest
            uint32 ProgressQuest = 0;           // ... which one that was
            uint32 TurnInsPaid = 0;
            bool TimeoutPaid = false;
            uint32 Poached = 0;                 // credit it took in a place another group holds
        };

        /// A group per side (one for a party, two sharing a zone, and one for each seat questing alone beside
        /// them), what each seat was paid, and the zone's coordinator (claims and assignments between the groups).
        struct EnvQuests
        {
            std::array<EnvQuest, LIFE_GROUPS> Groups;
            std::array<SeatPay, MAX_SEATS> Pay{};
            WorldCoordinator Coordinator;
        };

        [[nodiscard]] uint32 GroupCount(Env const& env) const;
        /// Quests refused at build for a reason every bot would meet (a chain or breadcrumb the table does not show,
        /// an exclusive group; not a class, race, level, skill or reputation) are drawn again only until refused
        /// REFUSALS_TO_RETIRE times; then never. Shared by every env, which build on the map threads.
        [[nodiscard]] bool Retired(uint32 questId) const;
        void Refused(uint32 questId);

        std::vector<EnvQuests> _quests;
        mutable std::mutex _refusedLock;
        std::unordered_map<uint32, uint32> _refusals;
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
        void Sensed(Env const& env, EnvLife const& life, uint32 seat, SeatView& view) const override;
        void RewardMore(Env& env, EnvLife& life, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void Account(Env& env, EnvLife& life, uint32 seat, SeatActionResult const& result) override;
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
            FoundPlaces Found;                  // the nodes seen, for the journal
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
        void Sensed(Env const& env, EnvLife const& life, uint32 seat, SeatView& view) const override;
        void RewardMore(Env& env, EnvLife& life, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void Account(Env& env, EnvLife& life, uint32 seat, SeatActionResult const& result) override;
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
            FoundPlaces Found;                  // the traders seen, for the journal
            bool SoldOut = false;               // no junk left
            bool Repaired = false;              // durability back to 1
        };

        std::vector<EnvTown> _towns;
    };
}

#endif
