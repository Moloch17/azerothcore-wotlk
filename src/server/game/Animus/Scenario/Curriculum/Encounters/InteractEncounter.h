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

#ifndef ANIMUS_LIB_CURRICULUM_INTERACT_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_INTERACT_ENCOUNTER_H

#include "Encounter.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <string>
#include <vector>

class GameObject;
class Unit;

namespace Animus::Curriculum
{
    struct ArenaDefinition;

    /// **M3 interact** (Opposition::Interact; dungeon-curriculum M3): in an empty Deadmines, telling objects apart and
    /// using the dungeon's own levers, doors and locks, through the sight block's entity presses (I1) and nothing else.
    /// No looting: no press loots, and the objects the seat is asked about are reached or used, never opened.
    ///
    /// **The goal names what, never where** (SeatView::NamedTask, the sight block's named row): the object's kind --
    /// its semantic class and template entry, as a quest's log names its objective -- and the task (reach it, or use
    /// the key item on it). No objective flag (perception-goals amendment 8: from M3 the objective is a real object
    /// the label system describes) and no compass: the seat finds it by what it sees and remembers.
    ///
    /// **The ladder** (InteractDraw::Rung, the shaping fade's rungs), each episode at one of the arena's sites (a real
    /// door with what opens it, and the floor either side; StageDefinition InteractSite):
    /// - distinguish: the named object and DecoysMin to DecoysMax decoys of other kinds of the pool on the site's near
    ///   side, every one in sight of the seat's eye; reach the right one (stopped within SeekRadius);
    /// - switch: the named object (and decoys) behind the site's shut door; the lever on the seat's side opens it --
    ///   pressed as the client presses it (CMSG_GAMEOBJ_USE through the handler), the map's own script linking it to
    ///   the door, and FollowLever doing the same should the script not; the door itself refuses a hand (Locked);
    /// - key: the lock the goal names -- the Deadmines' cannon -- which only the item its lock takes opens: the Defias
    ///   Gunpowder, carried from the start; used on it (CMSG_USE_ITEM at the cannon), its script blows the Iron Clad
    ///   Door.
    /// Each training episode plays the ladder's rung, or Interact.CarryShare of the time the one below. The seat
    /// carries every site's key item on every rung, so its bags never say which rung it is on.
    ///
    /// **The world, each reset**: the map's own objects but its doors and levers and the sites' locks are sent away
    /// (chests, veins, pots: the episode's objects are the only others); every site's door shut and its opener
    /// ready again; anything a lock's script summoned near it sent away; last episode's objects removed.
    ///
    /// An evaluation plays the training rung, its site, spawn, spots and kinds fixed by its seed
    /// (InteractDraw::EvaluationPick); the held-out "sweep" arena plays every rung in turn (InteractDraw::SweepRung).
    ///
    /// Paid: Arrive once, the right object reached or its lock given the key (Outcome); DoorOpened once, the switch
    /// rung's door opened by the seat's own lever press (Outcome); WrongObject once a decoy, stopped beside or
    /// pressed (Cost); StepCost, Death, Stuck and Wall (Cost; Stuck and Wall at their own fixed price); Sighting once,
    /// the first frame listing the named object (Shaping, faded). A press the world refuses is priced by the sight
    /// block (Actions.Aimless.ActRefused). Measured (episode info): right_object (overall and by rung), the rung
    /// (interact_rung), door_opened and door_by_lever, key_used, lever_pressed, wrong_objects, decoys, sighting and
    /// timing, by site and by object (stage.json episode_categories).
    class InteractEncounter final : public Encounter
    {
    public:
        InteractEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

        /// The arena's sites' and objects' names, in the order the episode info columns interact_site and
        /// interact_object index them (stage.json episode_categories). The object column's last name is the lock's
        /// ("lock"): the key rung names the site's opener, no object of the pool.
        [[nodiscard]] static std::vector<std::string> SiteNames(ArenaDefinition const& arena);
        [[nodiscard]] static std::vector<std::string> ObjectNames(ArenaDefinition const& arena);

        /// **A lever's link to its door** (the switch rung): when `lever` has just been used (its loot state
        /// activated) and `door` is still shut and ready, the door is opened as the map's script opens it
        /// (UseDoorOrButton); true when it opened it. A door the script opened already is left alone, so the two
        /// never fight.
        static bool FollowLever(GameObject* lever, GameObject* door, Unit* user);
        /// A door, button or lock as the map spawned it: shut or ready, not in use, so the next episode finds it as
        /// a fresh instance would.
        static void ResetObject(GameObject* object);

    private:
        struct EnvInteract
        {
            bool Placed = false;
            uint32 Rung = 0;                // the episode's (InteractDraw::Rung)
            uint32 LadderRung = 0;
            bool Carried = false;
            bool Sweep = false;
            uint32 Site = 0;
            bool Fallback = false;          // fewer objects than drawn could be placed
            // The named object: a spawned one of the pool (ObjectIndex), or the site's lock (the key rung).
            ObjectGuid Target;
            bool TargetSpawned = false;
            uint32 ObjectIndex = 0;         // into the pool; the pool's size for the lock
            Position Spot;
            float Radius = 0.5f;
            float Height = 1.0f;
            uint8 NamedClass = 0;
            uint32 NamedEntry = 0;
            std::vector<ObjectGuid> Decoys;
            std::vector<Position> DecoySpots;
            std::vector<bool> DecoyTaken;   // charged WrongObject already
            ObjectGuid Door;
            ObjectGuid Opener;
            // What happened.
            bool Found = false;
            uint32 FoundMs = 0;
            bool SightPaid = false;
            uint32 SightMs = 0;
            bool LeverPressed = false;
            bool DoorOpened = false;
            bool DoorByLever = false;
            bool DoorPaid = false;
            bool KeyUsed = false;
            uint32 WrongPresses = 0;
            uint32 WrongStops = 0;
            uint32 PendingWrong = 0;        // decoys pressed this decision, charged at its reward
            bool PendingRight = false;      // the named object used this decision
            uint32 Decisions = 0;
            uint32 VisibleDecisions = 0;
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            float Travelled = 0.0f;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
        };

        /// Spawn the pool's object `kind` at `spot`; its GUID, or empty.
        ObjectGuid Spawn(Map* map, ArenaDefinition const& arena, uint32 kind, Position const& spot,
            uint32 phase) const;
        /// Remove last episode's objects and put the map's own back as the next episode wants them.
        void ClearWorld(EnvInteract& state, Map* map, ArenaDefinition const& arena) const;
        /// Finish the episode: the right object reached or used.
        void Found(Env const& env, EnvInteract& state) const;

        std::vector<EnvInteract> _envs;
    };
}

#endif
