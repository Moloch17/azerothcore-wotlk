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

#ifndef ANIMUS_LIB_CURRICULUM_OPPONENTS_H
#define ANIMUS_LIB_CURRICULUM_OPPONENTS_H

#include "Define.h"
#include "Position.h"
#include <array>
#include <utility>
#include <vector>

class Creature;
class Map;
class Player;
class Unit;

/*
 * Hostile creatures for the curriculum stages: the pools they are drawn from, where they spawn, and summoning them.
 */
namespace Animus::Curriculum::Opponents
{
    /// Distance band the opponent spawns at: beyond the aggro radius of a same-level creature (about
    /// 20 yd), so the bot always has to close in.
    constexpr float SPAWN_DISTANCE_MIN = 40.0f;
    constexpr float SPAWN_DISTANCE_MAX = 50.0f;

    /// Real creatures fit to be a fair same-level opponent: normal rank, attackable, no script, no
    /// NPC services, not civilian/guard/trigger/vehicle, walking on the ground in plain sight, and spawned
    /// somewhere in the world. Loaded once and bucketed by the levels each creature naturally has.
    class OpponentPool
    {
    public:
        static OpponentPool const& Instance();

        /// A random default-AI creature entry whose natural level range covers `level` (the nearest range
        /// when none does). 0 if the pool is empty. The duel stage's opponents.
        [[nodiscard]] uint32 Random(uint8 level) const;

        /// Like Random, but also creatures whose SmartAI only casts spells or talks (casters and ability
        /// users). The pack and gauntlet stages' creatures.
        [[nodiscard]] uint32 RandomPackMember(uint8 level) const;

        /// An elite creature (default AI or casting SmartAI) for the level, or 0.
        [[nodiscard]] uint32 RandomElite(uint8 level) const;

        /// A pack creature that is a spellcaster: its SmartAI casts at least one spell with a cast time, one an
        /// interrupt can stop. The pack ladder puts one in every pack.
        [[nodiscard]] uint32 RandomCaster(uint8 level) const;

        /// A pack creature that puts something on the ground: its SmartAI casts at least one spell with a persistent
        /// area aura (a fire pool, a poison cloud) or an area aura of its own. Nothing else in the curriculum
        /// creates a hazard, so without these the ground features and the hazard charge read zero everywhere and
        /// "step out of it" stays unlearnable until a dungeon.
        [[nodiscard]] uint32 RandomHazardCaster(uint8 level) const;

        /// A spell that puts something on the ground, from the same set the hazard casters use: a persistent area
        /// aura, so it leaves a DynamicObject behind. That is what the sensing and the charge both read
        /// (Encoding::StandingInHazards counts DYNOBJ_AURA_TYPE auras and nothing else), which is why a stage that
        /// wants hazards without a fight casts one of these rather than dropping a trap gameobject: a trap burns
        /// but is invisible to both. 0 when the world has none.
        [[nodiscard]] uint32 RandomHazardSpell(uint8 level) const;

    private:
        OpponentPool();

        [[nodiscard]] static uint32 PickNear(std::array<std::vector<uint32>, 81> const& byLevel, uint8 level);

        std::array<std::vector<uint32>, 81> _byLevel;         // index = level
        std::array<std::vector<uint32>, 81> _packByLevel;
        std::array<std::vector<uint32>, 81> _elitesByLevel;
        std::array<std::vector<uint32>, 81> _castersByLevel;
        std::array<std::vector<uint32>, 81> _hazardCastersByLevel;
        std::array<std::vector<uint32>, 81> _hazardSpellsByLevel;   // what those casters put on the ground
    };

    /// A random spot 40-50 yd from the bot, in line of sight on roughly level ground the bot can walk to (so a
    /// creature there has a path to it), with a random facing.
    [[nodiscard]] Position FindSpawnPoint(Player* bot, Map* map);
    /// The same, in the water: a spot at the surface of a lake at least BODY_HEIGHT deep, in line of sight, so the
    /// fight is a swimming one for whoever goes in after the other (a creature arena with ArenaDefinition::Water).
    /// The dry spot FindSpawnPoint would give when no water is in reach.
    [[nodiscard]] Position FindSpawnPointInWater(Player* bot, Map* map);

    /// Summon `entry` at `pos` and `level`, hostile to players and aggressive, and not regenerating health in a fight
    /// it cannot reach. Returns nullptr on failure.
    Creature* SummonOpponent(Player* bot, Map* map, uint32 entry, Position const& pos, uint8 level);

    /// Summon a pack of `entries` at `level`, clustered around one spawn point, each facing its own way.
    std::vector<Creature*> SpawnPack(Player* bot, Map* map, std::vector<uint32> const& entries, uint8 level);
}

#endif
