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

#ifndef ANIMUS_LIB_CURRICULUM_SEEK_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_SEEK_ENCOUNTER_H

#include "Encounter.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <unordered_set>
#include <vector>

namespace Animus::Curriculum
{
    struct ArenaDefinition;

    /// **M2 seek** (Opposition::Seek; perception-goals plan §4, "M2 in detail"): one real object hidden in one of a
    /// dungeon's rooms, found by sight and stopped beside. The seat has no compass: the object is shown only by the
    /// camera's objective flag, on the pixels whose rays pass by it before they hit anything (line of sight), standing
    /// in for a quest object's glow. Its working memory is the GRU's.
    ///
    /// Each reset draws a room (SeekDraw::Weights: the shaping fade's rungs move the weights from the rooms seen from
    /// the hallway to the deepest; every room always possible) and an object (uniformly), and puts the object at a
    /// random spot of the room's floor with a random orientation: a real gameobject whose display has a collision
    /// model, so the camera's rays hit it. The objective point the flag marks is the object's centre. The object of
    /// the episode before is removed first, and so are the map's own game objects (the Stockades' chests and the
    /// Hallow's End decorations), so the one object in the dungeon is the one to find. An evaluation sweeps every
    /// (room, object) pair once a pass (SeekDraw::EvaluationPick: 195 episodes) and places the object by its seed
    /// (SeedUniform).
    ///
    /// Paid: Arrive once, stopped within the arena's SeekRadius of the object (Outcome); StepCost, Death, Stuck and
    /// Wall (Cost); Sighting and NewGround, the training-only aids (Shaping, faded). Measured: found, the time to the
    /// first frame with the flag, from there to the arrival, the rooms entered before finding it and the rooms
    /// re-entered, by room and by object (stage.json episode_categories).
    class SeekEncounter final : public Encounter
    {
    public:
        SeekEncounter(StageScenario& scenario, uint32 envs);

        [[nodiscard]] std::vector<RewardTerm> RewardTerms() const override;
        void AddEpisodeInfo(EpisodeInfoTable& table) override;
        void ResetEpisode(Env& env) override;
        bool Build(Env& env, Map* map, uint8 level) override;
        bool SelectTarget(Env const& env, uint32 seat, Unit*& target) override;
        void View(Env const& env, uint32 seat, SeatView& view) const override;
        void Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger) override;
        void WriteState(Env const& env, float* state) const override;
        [[nodiscard]] bool IsTerminal(Env const& env) const override;
        void Teardown(Env& env) override;

        /// The rooms' and objects' names of the stage's seek arena, in the order the episode info columns seek_room and
        /// seek_object index them (stage.json episode_categories).
        [[nodiscard]] static std::vector<std::string> RoomNames(ArenaDefinition const& arena);
        [[nodiscard]] static std::vector<std::string> ObjectNames(ArenaDefinition const& arena);

    private:
        struct EnvSeek
        {
            bool Placed = false;
            ObjectGuid Object;
            Position Spot;                  // the object's base, where it stands
            Position Centre;                // the objective point: its centre, which the flag marks
            int32 Room = -1;
            uint32 ObjectIndex = 0;
            float Depth = 0.0f;
            uint32 Tier = 0;
            float Ladder = 0.0f;            // 1 - the shaping scale the room was drawn at
            bool Fallback = false;          // placed at the room's centre: no drawn spot passed
            bool Found = false;
            uint32 FoundMs = 0;
            bool SightPaid = false;
            uint32 SightMs = 0;
            uint32 VisibleDecisions = 0;
            uint32 Decisions = 0;
            int32 LastRoom = -1;
            std::vector<bool> Entered;
            uint32 RoomsEntered = 0;
            uint32 RoomsReentered = 0;
            uint32 RoomsBeforeFound = 0;
            std::unordered_set<uint64> Cells;
            float LastX = 0.0f;
            float LastY = 0.0f;
            bool HasLastPos = false;
            float Travelled = 0.0f;
            uint32 LastStuckMs = 0;
            uint32 LastWallMs = 0;
            bool Recorded = false;
        };

        /// Remove the episode's object, if it is still in `map`.
        void Remove(EnvSeek& seek, Map* map) const;
        /// Put the episode's object in `room` of `arena`: a spot on its floor, clear of walls, else its centre.
        bool Place(Env const& env, EnvSeek& seek, Map* map, ArenaDefinition const& arena, uint32 phase) const;

        std::vector<EnvSeek> _envs;
    };
}

#endif
