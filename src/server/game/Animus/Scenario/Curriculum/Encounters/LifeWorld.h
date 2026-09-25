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

#ifndef ANIMUS_LIB_CURRICULUM_LIFE_WORLD_H
#define ANIMUS_LIB_CURRICULUM_LIFE_WORLD_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <map>
#include <unordered_map>
#include <vector>

class Creature;
class GameObject;
class Map;
class Player;

namespace Animus
{
    struct Env;
}

/*
 * The world the life stages (quest, gather, town) put into an env's phase, and what they read back out of it.
 *
 * Nothing here is a hand-placed position. The world database says where every creature and gameobject of the
 * continents stands; a life episode takes the spawns around one place -- a quest giver, a field of nodes, an inn
 * -- and summons copies of them into the env's own phase, where the seat is the only player. The world's own
 * copies stay in phase 1, untouched. What the seat then does to them (kills, loots, gathers, sells) lives on the
 * bot and the summons, in memory, and goes with the episode.
 */
namespace Animus::Curriculum::LifeWorld
{
    /// The level bands a life stage climbs: the rung is the band, the level a draw within it.
    struct Band
    {
        uint8 Min;
        uint8 Max;
    };

    constexpr uint32 BAND_COUNT = 3;
    [[nodiscard]] Band const& BandAt(uint32 index);
    [[nodiscard]] uint32 BandOf(uint8 level);

    /// Which side a thing is for: quests are often one side's, and a town always is.
    enum class Side : uint8 { Any, Alliance, Horde };
    [[nodiscard]] Side SideOf(TeamId team);
    [[nodiscard]] TeamId TeamOf(Side side);

    /// One row of the creature or gameobject table on a continent.
    struct Spawn
    {
        uint32 SpawnId = 0;
        uint32 Entry = 0;
        uint32 Map = 0;
        Position Pos;
        bool Object = false;
        float Rotation[4] = { 0.0f, 0.0f, 0.0f, 0.0f };     // gameobjects: the quaternion the table gives
    };

    /// Every continent spawn, in 100-yard cells for range queries. Built once (world thread, first use).
    class SpawnIndex
    {
    public:
        static SpawnIndex const& Instance();

        /// Spawns within `radius` of (x, y) on `map`, nearest first.
        void CreaturesNear(uint32 map, float x, float y, float radius, std::vector<Spawn const*>& out) const;
        void ObjectsNear(uint32 map, float x, float y, float radius, std::vector<Spawn const*>& out) const;
        /// The continent spawns of creature `entry`.
        [[nodiscard]] std::vector<Spawn const*> const& CreaturesOfEntry(uint32 entry) const;

    private:
        SpawnIndex();

        using Cells = std::unordered_map<uint64, std::vector<Spawn const*>>;
        void Near(Cells const& cells, uint32 map, float x, float y, float radius,
            std::vector<Spawn const*>& out) const;

        std::vector<Spawn> _creatures;
        std::vector<Spawn> _objects;
        Cells _creatureCells;
        Cells _objectCells;
        std::unordered_map<uint32, std::vector<Spawn const*>> _byEntry;
    };

    /// A quest an episode can be: kill or collect, its giver and its turn-in both spawned on a continent within
    /// reach of each other, its objectives' places (the quest POI table) within reach of the giver.
    struct QuestCandidate
    {
        uint32 Id = 0;
        uint32 Band = 0;
        uint32 MinLevel = 0;                    // the quest's; the episode's level is at least this
        Side For = Side::Any;
        Spawn const* Giver = nullptr;
        Spawn const* Ender = nullptr;
        bool Collect = false;                   // needs items looted (else kills alone)
        std::vector<Position> Objectives;       // one place per objective, on the giver's map
    };

    /// The quests of the world database that pass the life stage's filter, by band and side. Built once.
    class QuestSet
    {
    public:
        static QuestSet const& Instance();

        /// Candidates a seat of `side` in `band` could be given (its own side's and the ones for both).
        [[nodiscard]] std::vector<QuestCandidate const*> const& For(uint32 band, Side side) const;
        [[nodiscard]] std::size_t Size() const { return _quests.size(); }

    private:
        QuestSet();

        std::vector<QuestCandidate> _quests;
        std::map<std::pair<uint32, Side>, std::vector<QuestCandidate const*>> _byBandAndSide;
    };

    /// Where the gather stage stands a seat: the middle of a field of nodes of the band's zones (the densest
    /// 400-yard cells of herb and ore spawns, measured from the world database on 2026-09-24). The z is the nodes'
    /// average; the seat is put on the ground under it.
    struct Ground
    {
        uint32 Map;
        float X;
        float Y;
        float Z;
    };

    [[nodiscard]] std::vector<Ground> const& GatherGrounds(uint32 band);

    /// The innkeepers whose inns the town stage uses, by band and side (a neutral inn is in both sides' lists).
    [[nodiscard]] std::vector<uint32> const& TownInns(uint32 band, Side side);

    /// A copy of `spawn` in `phase` of `map`, as the table has it. Null when the summon failed.
    Creature* Summon(Map* map, uint32 phase, Spawn const& spawn);
    GameObject* SummonObject(Map* map, uint32 phase, Spawn const& spawn);
    /// Remove what an episode summoned.
    void Despawn(Map* map, std::vector<ObjectGuid>& spawned);

    /// Creatures worth summoning around a place: not the world's vendors, trainers and other flagged NPCs unless
    /// `npcs` asks for them, and never a spawn the table phases out of the world.
    [[nodiscard]] bool IsWorldCreature(Spawn const& spawn, bool npcs);

    /// The ground under (x, y) on `map` near `z`, the grid loaded if it has to be; `z` when none is found.
    [[nodiscard]] float GroundZ(Map* map, uint32 phase, float x, float y, float z);

    /// An episode draw: seeded from the evaluation's seed when there is one, random otherwise.
    [[nodiscard]] uint32 Draw(Env const& env, uint32 count, uint32 salt);

    /// The name of a side, for logs and episode info.
    [[nodiscard]] char const* SideName(Side side);
}

#endif
