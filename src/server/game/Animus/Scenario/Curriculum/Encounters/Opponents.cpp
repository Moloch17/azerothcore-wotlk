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

#include "Opponents.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SummonLevel.h"
#include "TemporarySummon.h"
#include "WorldCreatures.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr uint32 SPAWN_ATTEMPTS = 24;
    constexpr float PACK_SPREAD = 5.0f;
    constexpr float MAX_HEIGHT_DIFFERENCE = 6.0f;
    constexpr float MAX_PATH_DETOUR = 1.5f;     // a walking path at most this many times the straight line
    constexpr float MAX_STAT_MOD = 2.0f;        // health and damage multipliers of a normal creature
    /// An elite's are higher by design: at 2 the world has six elites a gauntlet or pack could meet, the same few in
    /// every elite pull. These bounds keep open-world and outdoor-quest elites and leave out raid and boss tuning.
    constexpr float MAX_ELITE_HEALTH_MOD = 3.0f;
    constexpr float MAX_ELITE_DAMAGE_MOD = 2.5f;

    constexpr uint32 UNUSABLE_UNIT_FLAGS = UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_SELECTABLE
        | UNIT_FLAG_PACIFIED;
    constexpr uint32 UNUSABLE_EXTRA_FLAGS = CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER
        | CREATURE_FLAG_EXTRA_GUARD;

    bool IsFairOpponentType(uint32 type)
    {
        switch (type)
        {
            case CREATURE_TYPE_BEAST:
            case CREATURE_TYPE_DRAGONKIN:
            case CREATURE_TYPE_DEMON:
            case CREATURE_TYPE_ELEMENTAL:
            case CREATURE_TYPE_GIANT:
            case CREATURE_TYPE_UNDEAD:
            case CREATURE_TYPE_HUMANOID:
                return true;
            default:
                return false;
        }
    }

    /// Whether a creature spawns out of reach or out of sight whatever the seat does: hovering or flying in the air,
    /// unable to walk, rooted, or stealthed or invisible by its addon's auras. stage1_duel's Witchwing Ambusher
    /// (stealthed) was never once seen, let alone killed, in 20 evaluation episodes.
    bool SpawnsUnreachable(CreatureTemplate const& info)
    {
        CreatureMovementData const& movement = info.Movement;
        if (!movement.IsGroundAllowed() || movement.IsFlightAllowed() || movement.IsRooted())
            return true;

        CreatureAddon const* addon = sObjectMgr->GetCreatureTemplateAddon(info.Entry);
        if (!addon)
            return false;

        if ((addon->bytes1 >> 24) & 0xFF)       // UNIT_BYTES_1_OFFSET_ANIM_TIER: hovering, flying or submerged
            return true;

        return std::any_of(addon->auras.begin(), addon->auras.end(), [](uint32 spellId)
        {
            SpellInfo const* aura = sSpellMgr->GetSpellInfo(spellId);
            return aura && (aura->HasAura(SPELL_AURA_MOD_STEALTH) || aura->HasAura(SPELL_AURA_MOD_INVISIBILITY));
        });
    }

    /// Whether the bot can walk to (x, y, z) by a path not much longer than the straight line: the creature spawned
    /// there then has a path back. A creature with no path to its victim stands still and regenerates its health
    /// (Creature::IsNotReachableAndNeedRegen), which stage1_duel met in 125 of its 641 opponents.
    bool Walkable(Player* bot, float x, float y, float z)
    {
        PathGenerator path(bot);
        if (!path.CalculatePath(x, y, z))
            return false;

        PathType const type = path.GetPathType();
        if (type & PATHFIND_NOT_USING_PATH)
            return true;        // no navigation mesh to check against

        return (type & PATHFIND_NORMAL) && path.getPathLength() <= bot->GetExactDist(x, y, z) * MAX_PATH_DETOUR;
    }
}

Animus::Curriculum::Opponents::OpponentPool const& Animus::Curriculum::Opponents::OpponentPool::Instance()
{
    static OpponentPool const pool;
    return pool;
}

Animus::Curriculum::Opponents::OpponentPool::OpponentPool()
{
    std::unordered_set<uint32> const& spawned = WorldCreatures::SpawnedIds();
    std::unordered_set<uint32> const& walkers = WorldCreatures::WaypointWalkerIds();

    // SmartAI creatures whose scripts only cast spells or talk, on combat events: casters and ability users
    // without scripts that flee, summon, despawn or change phases. Pack and gauntlet stages only.
    std::unordered_set<uint32> castOnlySmart;
    if (QueryResult result = WorldDatabase.Query("SELECT ss.entryorguid FROM smart_scripts ss "
        "JOIN creature_template ct ON ct.entry = ss.entryorguid AND ct.AIName = 'SmartAI' AND ct.ScriptName = '' "
        "WHERE ss.source_type = 0 AND ss.entryorguid > 0 GROUP BY ss.entryorguid "
        "HAVING SUM(ss.action_type NOT IN (1, 11)) = 0 "
        "AND SUM(ss.event_type NOT IN (0, 2, 3, 4, 5, 6, 8, 9, 12, 13, 14)) = 0 AND SUM(ss.action_type = 11) > 0"))
    {
        do
        {
            castOnlySmart.insert(uint32(result->Fetch()[0].Get<int32>()));
        } while (result->NextRow());
    }

    // Of those, the ones that cast something with a cast time: an interrupt can stop it, and the ones that put
    // something on the ground: a persistent area aura to walk out of.
    std::unordered_set<uint32> castTimeSmart;
    std::unordered_set<uint32> hazardSmart;
    std::unordered_map<uint32, uint32> hazardSpellOf;       // creature entry -> the ground it lays
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_param1 FROM smart_scripts "
        "WHERE source_type = 0 AND entryorguid > 0 AND action_type = 11"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const entry = uint32(fields[0].Get<int32>());
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(fields[1].Get<uint32>());
            if (!castOnlySmart.contains(entry) || !spell)
                continue;

            if (spell->CastTimeEntry && spell->CastTimeEntry->CastTime > 0)
                castTimeSmart.insert(entry);
            if (spell->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA) || spell->HasAreaAuraEffect())
            {
                hazardSmart.insert(entry);
                hazardSpellOf.emplace(entry, spell->Id);
            }
        } while (result->NextRow());
    }

    uint32 opponents = 0;
    uint32 packMembers = 0;
    uint32 elites = 0;
    uint32 casters = 0;
    uint32 hazards = 0;
    for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!spawned.contains(entry) || walkers.contains(entry))
            continue;

        // Plain combat creatures with sane stat multipliers.
        bool const elite = info.rank == CREATURE_ELITE_ELITE;
        float const maxHealthMod = elite ? MAX_ELITE_HEALTH_MOD : MAX_STAT_MOD;
        float const maxDamageMod = elite ? MAX_ELITE_DAMAGE_MOD : MAX_STAT_MOD;
        if (!IsFairOpponentType(info.type) || info.npcflag || info.VehicleId || (info.unit_flags & UNUSABLE_UNIT_FLAGS)
            || (info.flags_extra & UNUSABLE_EXTRA_FLAGS) || info.ModHealth < 0.5f || info.ModHealth > maxHealthMod
            || info.DamageModifier < 0.5f || info.DamageModifier > maxDamageMod || !info.minlevel
            || SpawnsUnreachable(info))
            continue;

        // Default AI: no SmartAI or C++ script that could summon, flee or despawn.
        bool const defaultAI = !info.ScriptID && info.AIName.empty();
        bool const castingAI = castOnlySmart.contains(entry);
        if (!defaultAI && !castingAI)
            continue;

        uint32 const maxLevel = std::min<uint32>(info.maxlevel, DEFAULT_MAX_LEVEL);
        if (info.rank == CREATURE_ELITE_NORMAL)
        {
            bool const caster = castTimeSmart.contains(entry);
            bool const hazard = hazardSmart.contains(entry);
            for (uint32 level = info.minlevel; level <= maxLevel; ++level)
            {
                if (defaultAI)
                    _byLevel[level].push_back(entry);
                _packByLevel[level].push_back(entry);
                if (caster)
                    _castersByLevel[level].push_back(entry);
                if (hazard)
                {
                    _hazardCastersByLevel[level].push_back(entry);
                    if (auto const spell = hazardSpellOf.find(entry); spell != hazardSpellOf.end())
                        _hazardSpellsByLevel[level].push_back(spell->second);
                }
            }

            opponents += defaultAI ? 1 : 0;
            casters += caster ? 1 : 0;
            hazards += hazard ? 1 : 0;
            ++packMembers;
        }
        else if (elite)
        {
            for (uint32 level = info.minlevel; level <= maxLevel; ++level)
                _elitesByLevel[level].push_back(entry);

            ++elites;
        }
    }

    LOG_INFO("module.animus", "Opponent pool: {} opponent creatures, {} pack creatures ({} casting, {} with cast-time "
        "spells), {} elites", opponents, packMembers, castOnlySmart.size(), casters, elites);
}

uint32 Animus::Curriculum::Opponents::OpponentPool::PickNear(std::array<std::vector<uint32>, 81> const& byLevel,
    uint8 level)
{
    // The level itself, then the nearest levels either side.
    for (int32 offset = 0; offset <= DEFAULT_MAX_LEVEL; ++offset)
    {
        for (int32 candidate : { int32(level) - offset, int32(level) + offset })
        {
            if (candidate < 1 || candidate > DEFAULT_MAX_LEVEL || byLevel[candidate].empty())
                continue;

            std::vector<uint32> const& entries = byLevel[candidate];
            return entries[urand(0, uint32(entries.size()) - 1)];
        }
    }

    return 0;
}

uint32 Animus::Curriculum::Opponents::OpponentPool::Random(uint8 level) const
{
    return PickNear(_byLevel, level);
}

uint32 Animus::Curriculum::Opponents::OpponentPool::RandomPackMember(uint8 level) const
{
    return PickNear(_packByLevel, level);
}

uint32 Animus::Curriculum::Opponents::OpponentPool::RandomElite(uint8 level) const
{
    return PickNear(_elitesByLevel, level);
}

uint32 Animus::Curriculum::Opponents::OpponentPool::RandomCaster(uint8 level) const
{
    return PickNear(_castersByLevel, level);
}

uint32 Animus::Curriculum::Opponents::OpponentPool::RandomHazardCaster(uint8 level) const
{
    return PickNear(_hazardCastersByLevel, level);
}

uint32 Animus::Curriculum::Opponents::OpponentPool::RandomHazardSpell(uint8 level) const
{
    return PickNear(_hazardSpellsByLevel, level);
}

Position Animus::Curriculum::Opponents::FindSpawnPoint(Player* bot, Map* map)
{
    // A random bearing and distance; retry bearings for a spot in line of sight on roughly level ground that the bot
    // can walk to, so the opponent is reachable. Without one, a walkable spot out of sight; the last try otherwise.
    Position pos;
    std::optional<Position> walkable;
    bool found = false;
    for (uint32 attempt = 0; attempt < SPAWN_ATTEMPTS && !found; ++attempt)
    {
        float const bearing = frand(0.0f, 2.0f * float(M_PI));
        float const distance = frand(SPAWN_DISTANCE_MIN, SPAWN_DISTANCE_MAX);

        pos.m_positionX = bot->GetPositionX() + distance * std::cos(bearing);
        pos.m_positionY = bot->GetPositionY() + distance * std::sin(bearing);
        pos.m_positionZ = bot->GetPositionZ();

        float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 5.0f);
        if (ground <= INVALID_HEIGHT)
            continue;

        pos.m_positionZ = ground;
        if (std::fabs(ground - bot->GetPositionZ()) >= MAX_HEIGHT_DIFFERENCE
            || !Walkable(bot, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()))
            continue;

        found = bot->IsWithinLOS(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 2.0f);
        if (!found && !walkable)
            walkable = pos;
    }

    if (!found && walkable)
        pos = *walkable;

    // A random facing, so the bot has to learn to get behind it.
    pos.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
    return pos;
}

Position Animus::Curriculum::Opponents::FindSpawnPointInWater(Player* bot, Map* map)
{
    constexpr float BODY_HEIGHT = 2.0f;
    for (uint32 attempt = 0; attempt < SPAWN_ATTEMPTS * 2; ++attempt)
    {
        float const bearing = frand(0.0f, 2.0f * float(M_PI));
        float const distance = frand(SPAWN_DISTANCE_MIN, SPAWN_DISTANCE_MAX);
        float const x = bot->GetPositionX() + distance * std::cos(bearing);
        float const y = bot->GetPositionY() + distance * std::sin(bearing);
        map->LoadGrid(x, y);

        // The bed under the spot, and the water over it: Map::GetHeight is blind to liquid.
        float const bed = map->GetHeight(bot->GetPhaseMask(), x, y, bot->GetPositionZ() + 60.0f, true, 120.0f);
        if (bed <= INVALID_HEIGHT)
            continue;
        LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), x, y, bed, bot->GetCollisionHeight(), {});
        if (liquid.Status == LIQUID_MAP_NO_WATER || liquid.Level <= INVALID_HEIGHT
            || (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) == 0
            || liquid.Level - bed < BODY_HEIGHT)
            continue;

        Position pos(x, y, liquid.Level - bot->GetCollisionHeight() * 0.5f, frand(0.0f, 2.0f * float(M_PI)));
        if (bot->IsWithinLOS(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 2.0f))
            return pos;
    }

    return FindSpawnPoint(bot, map);
}

Creature* Animus::Curriculum::Opponents::SummonOpponent(Player* bot, Map* map, uint32 entry, Position const& pos,
    uint8 level)
{
    PendingSummonLevel = level;
    TempSummon* opponent = map->SummonCreature(entry, pos);
    PendingSummonLevel = 0;

    if (!opponent)
    {
        LOG_ERROR("module.animus", "Could not summon opponent {} for bot {}", entry, bot->GetName());
        return nullptr;
    }

    // Into the env's own phase. Every env's bots stand in a phase bit of their own (StageScenario::EnvPhase) so
    // that envs sharing a continent do not see each other, and a summon with no summoner is created in the
    // world's phase (Map::SummonCreature): the two never overlapped, so the opponent could not be attacked, could
    // not aggro, and was not even a valid target -- start_attack was masked in every decision of every creature
    // stage, and the scripted baseline dealt no damage at all (stage8_duel, first run on format 7: dps 0.0 for
    // all ten classes, in_melee_share 0.0).
    opponent->SetPhaseMask(bot->GetPhaseMask(), true);
    opponent->SetFaction(FACTION_MONSTER);
    opponent->SetReactState(REACT_AGGRESSIVE);
    opponent->SetHomePosition(pos);
    opponent->SetFullHealth();
    // A creature that loses its path to the seat mid-fight (kited onto a ledge, round a rock) stops and regenerates
    // to full: a fight nobody can win however well it is played. It still heals when it evades home.
    opponent->SetRegeneratingHealth(false);

    return opponent;
}

std::vector<Creature*> Animus::Curriculum::Opponents::SpawnPack(Player* bot, Map* map,
    std::vector<uint32> const& entries, uint8 level)
{
    Position const center = FindSpawnPoint(bot, map);

    std::vector<Creature*> pack;
    for (uint32 i = 0; i < entries.size(); ++i)
    {
        // Loosely clustered around the center, each facing its own way.
        Position pos = center;
        if (i)
        {
            float const angle = frand(0.0f, 2.0f * float(M_PI));
            float const offset = frand(2.0f, PACK_SPREAD);
            pos.m_positionX += offset * std::cos(angle);
            pos.m_positionY += offset * std::sin(angle);

            float const ground = map->GetHeight(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 5.0f);
            if (ground > INVALID_HEIGHT)
                pos.m_positionZ = ground;

            pos.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
        }

        if (Creature* member = SummonOpponent(bot, map, entries[i], pos, level))
            pack.push_back(member);
    }

    return pack;
}
