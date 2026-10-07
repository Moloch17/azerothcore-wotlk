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
#include <optional>
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

    private:
        OpponentPool();

        [[nodiscard]] static uint32 PickNear(std::array<std::vector<uint32>, 81> const& byLevel, uint8 level);

        std::array<std::vector<uint32>, 81> _byLevel;         // index = level
        std::array<std::vector<uint32>, 81> _packByLevel;
        std::array<std::vector<uint32>, 81> _elitesByLevel;
        std::array<std::vector<uint32>, 81> _castersByLevel;
        std::array<std::vector<uint32>, 81> _hazardCastersByLevel;
    };

    /// A random spot `minDistance` to `maxDistance` yards from the bot, in line of sight on roughly level ground the
    /// bot can walk to (so a creature there has a path to it), with a random facing.
    [[nodiscard]] Position FindSpawnPoint(Player* bot, Map* map, float minDistance, float maxDistance);
    /// A spot `minDistance` to `maxDistance` yards on from `from`, on a bearing within `spread` radians of
    /// `bearing`, on ground near `from`'s height that the bot can walk to: the next pack of a camp
    /// (PullSchedule::Camp). None when no try finds one.
    [[nodiscard]] std::optional<Position> FindSpawnPointFrom(Player* bot, Map* map, Position const& from,
        float bearing, float spread, float minDistance, float maxDistance);

    /// Summon `entry` at `pos` and `level`, hostile to players and aggressive, and not regenerating health in a fight
    /// it cannot reach. Returns nullptr on failure.
    Creature* SummonOpponent(Player* bot, Map* map, uint32 entry, Position const& pos, uint8 level);

    /// Summon a pack of `entries` at `level`, clustered around `center`, each facing its own way.
    std::vector<Creature*> SpawnPack(Player* bot, Map* map, std::vector<uint32> const& entries, uint8 level,
        Position const& center);
}

#endif
