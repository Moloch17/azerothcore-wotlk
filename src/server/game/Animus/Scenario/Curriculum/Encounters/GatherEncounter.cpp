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

#include "LifeEncounter.h"
#include "BotFactory.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "WorldActions.h"
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;

    constexpr uint32 SALT_GROUND = 19;
    enum Waypoints : uint8 { WAY_NODE = 1 };

    /// The gathering skill a player of the band has kept up: the cap of the level's rank.
    uint16 SkillForLevel(uint8 level)
    {
        return uint16(std::min<uint32>(300, std::max<uint32>(75, uint32(level) * 5)));
    }

    uint32 GatherSkillOf(Player const* bot)
    {
        return uint32(bot->GetSkillValue(SKILL_HERBALISM)) + uint32(bot->GetSkillValue(SKILL_MINING))
            + uint32(bot->GetSkillValue(SKILL_SKINNING));
    }
}

Animus::Curriculum::GatherEncounter::GatherEncounter(StageScenario& scenario, uint32 envs)
    : LifeEncounter(scenario, envs, "gather"), _gathers(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::GatherEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::Progress, RewardTerm::Death, RewardTerm::Wasted,
        RewardTerm::GatherNode, RewardTerm::GatherSkillUp };
}

void Animus::Curriculum::GatherEncounter::AddMoreEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("nodes_spawned", [this](Env const& env, uint32) { return float(_gathers[env.Index].NodesSpawned); });
    table.Add("nodes_gathered", [this](Env const& env, uint32) { return float(_gathers[env.Index].NodesGathered); });
    table.Add("skill_ups", [this](Env const& env, uint32) { return float(_gathers[env.Index].SkillUps); });
    table.Add("skinned", [this](Env const& env, uint32) { return float(_gathers[env.Index].Skinned); });
}

bool Animus::Curriculum::GatherEncounter::Place(Env& env, EnvLife& life)
{
    EnvGather& gather = _gathers[env.Index];
    gather = EnvGather();

    std::vector<LifeWorld::Ground> const& grounds = LifeWorld::GatherGrounds(life.Tier);
    if (grounds.empty())
        return false;

    gather.Ground = &grounds[LifeWorld::Draw(env, uint32(grounds.size()), SALT_GROUND)];
    EnvState& data = _scenario.Data(env);
    data.EpisodeMapId = gather.Ground->Map;
    data.HasEpisodeMap = true;
    data.EpisodeSpawn.Relocate(gather.Ground->X, gather.Ground->Y, gather.Ground->Z, 0.0f);
    data.HasEpisodeSpawn = true;
    return true;
}

bool Animus::Curriculum::GatherEncounter::Build(Env& env, Map* map, uint8 level)
{
    EnvLife& life = _envs[env.Index];
    EnvGather& gather = _gathers[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    if (!gather.Ground || !bot || !map)
        return false;

    // The ground's height is the nodes' average: put the seat on the actual ground under it.
    Position start(gather.Ground->X, gather.Ground->Y, gather.Ground->Z, bot->GetOrientation());
    start.m_positionZ = LifeWorld::GroundZ(map, bot->GetPhaseMask(), start.GetPositionX(), start.GetPositionY(),
        start.GetPositionZ());
    BotFactory::TeleportWithinMap(bot, start);

    // The professions at the band's skill, and the tools.
    uint16 const skill = SkillForLevel(level);
    WorldActions::LearnProfession(bot, SKILL_HERBALISM, skill);
    WorldActions::LearnProfession(bot, SKILL_MINING, skill);
    WorldActions::LearnProfession(bot, SKILL_SKINNING, skill);
    gather.SkillAtStart = GatherSkillOf(bot);

    // The field: the nodes of the table around the ground, and the creatures that live among them.
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    std::vector<LifeWorld::Spawn const*> spawns;
    LifeWorld::SpawnIndex::Instance().ObjectsNear(map->GetId(), start.GetPositionX(), start.GetPositionY(),
        tuning.NodeRadius, spawns);
    for (LifeWorld::Spawn const* spawn : spawns)
    {
        if (gather.NodesSpawned >= tuning.NodeSpawns)
            break;
        if (WorldActions::NodeOfEntry(spawn->Entry) == WorldActions::NodeKind::None)
            continue;
        if (SummonObject(env, life, map, *spawn))
            ++gather.NodesSpawned;
    }
    SummonAround(env, life, map, start, tuning.NodeRadius, tuning.ObjectiveSpawns, false);

    if (!gather.NodesSpawned)
        LOG_INFO("module.animus", "{}: env {} found no nodes at ({:.0f} {:.0f}) on map {}", _scenario.Name(),
            env.Index, start.GetPositionX(), start.GetPositionY(), map->GetId());
    return gather.NodesSpawned > 0;
}

void Animus::Curriculum::GatherEncounter::Update(Env& env)
{
    EnvLife& life = _envs[env.Index];
    EnvGather& gather = _gathers[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !bot->IsAlive())
        return;

    uint32 const skill = GatherSkillOf(bot);
    if (skill > gather.SkillAtStart + gather.SkillUps)
        gather.SkillUps = skill - gather.SkillAtStart;

    // The nearest node the seat can open is where it should be going.
    GameObject* nearest = nullptr;
    for (ObjectGuid const& guid : life.Spawned)
    {
        if (!guid.IsGameObject())
            continue;
        GameObject* node = bot->GetMap()->GetGameObject(guid);
        if (!node || !node->isSpawned() || !WorldActions::CanGather(bot, node))
            continue;
        if (!nearest || bot->GetExactDist2d(node) < bot->GetExactDist2d(nearest))
            nearest = node;
    }
    if (nearest)
        SetWaypoint(life, WAY_NODE, nearest->GetPosition());
    else
        ClearWaypoint(life);
}

void Animus::Curriculum::GatherEncounter::Sensed(Env const& /*env*/, EnvLife const& /*life*/, SeatView& /*view*/) const
{
}

void Animus::Curriculum::GatherEncounter::Account(Env& env, EnvLife& /*life*/, SeatActionResult const& result)
{
    EnvGather& gather = _gathers[env.Index];
    gather.NodesGathered += result.NodesLooted;
}

void Animus::Curriculum::GatherEncounter::RewardMore(Env& env, EnvLife& /*life*/, Player* /*bot*/,
    RewardLedger& ledger)
{
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    EnvGather& gather = _gathers[env.Index];
    float const tierScale = TierScale(env);

    if (gather.NodesGathered > gather.NodesPaid)
    {
        ledger.Add(RewardTerm::GatherNode, tuning.GatherNode * float(gather.NodesGathered - gather.NodesPaid)
            * tierScale);
        gather.NodesPaid = gather.NodesGathered;
    }
    if (gather.SkillUps > gather.SkillUpsPaid)
    {
        ledger.Add(RewardTerm::GatherSkillUp, tuning.GatherSkillUp * float(gather.SkillUps - gather.SkillUpsPaid));
        gather.SkillUpsPaid = gather.SkillUps;
    }
}

bool Animus::Curriculum::GatherEncounter::Finished(Env const& env, EnvLife const& /*life*/) const
{
    // There is no winning a field: the clock ends it. Gathering the whole field early does, and counts as a win.
    EnvGather const& gather = _gathers[env.Index];
    return gather.NodesSpawned > 0 && gather.NodesGathered >= gather.NodesSpawned;
}
