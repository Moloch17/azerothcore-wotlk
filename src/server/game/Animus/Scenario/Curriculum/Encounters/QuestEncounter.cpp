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
#include "Creature.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "WorldActions.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    constexpr uint32 SALT_QUEST = 17;
    /// Where the seat stands at the start: a few yards in front of the giver, facing it.
    constexpr float START_YARDS = 1.5f;     // close: a giver on a ledge or a stair has no ground four yards out
    /// A living objective creature this near replaces the objective's place as the waypoint.
    constexpr float OBJECTIVE_SIGHT = 120.0f;

    enum Waypoints : uint8 { WAY_GIVER = 1, WAY_OBJECTIVE = 2, WAY_ENDER = 3 };
}

Animus::Curriculum::QuestEncounter::QuestEncounter(StageScenario& scenario, uint32 envs)
    : LifeEncounter(scenario, envs, "quest"), _quests(envs)
{
    LifeWorld::QuestSet const& quests = LifeWorld::QuestSet::Instance();
    if (quests.Size() == 0)
        LOG_ERROR("module.animus", "{}: the world database has no quest the life stage can use", scenario.Name());
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::QuestEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::Progress, RewardTerm::Death, RewardTerm::Wasted,
        RewardTerm::QuestAccepted, RewardTerm::QuestCredit, RewardTerm::QuestTurnIn, RewardTerm::Timeout };
}

void Animus::Curriculum::QuestEncounter::AddMoreEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("quest_id", [this](Env const& env, uint32)
    {
        return _quests[env.Index].Quest ? float(_quests[env.Index].Quest->Id) : 0.0f;
    });
    table.Add("quest_collect", [this](Env const& env, uint32)
    {
        return _quests[env.Index].Quest && _quests[env.Index].Quest->Collect ? 1.0f : 0.0f;
    });
    table.Add("quest_accepted", [this](Env const& env, uint32) { return _quests[env.Index].Accepted ? 1.0f : 0.0f; });
    table.Add("quest_progress", [this](Env const& env, uint32) { return _quests[env.Index].Progress; });
    table.Add("quest_complete", [this](Env const& env, uint32) { return _quests[env.Index].Complete ? 1.0f : 0.0f; });
    table.Add("quest_turned_in", [this](Env const& env, uint32) { return _quests[env.Index].TurnedIn ? 1.0f : 0.0f; });
    table.Add("quest_kills", [this](Env const& env, uint32) { return float(_quests[env.Index].Kills); });
}

bool Animus::Curriculum::QuestEncounter::Place(Env& env, EnvLife& life)
{
    EnvQuest& quest = _quests[env.Index];
    quest = EnvQuest();

    LifeWorld::QuestSet const& set = LifeWorld::QuestSet::Instance();
    std::vector<LifeWorld::QuestCandidate const*> const* candidates = &set.For(life.Tier, life.Side);
    if (candidates->empty())
    {
        // The other side's, then: the seat's side follows the quest (EpisodeTeam).
        life.Side = life.Side == LifeWorld::Side::Horde ? LifeWorld::Side::Alliance : LifeWorld::Side::Horde;
        candidates = &set.For(life.Tier, life.Side);
        _scenario.Data(env).EpisodeTeam = uint8(LifeWorld::TeamOf(life.Side)) + 1;
    }
    if (candidates->empty())
        return false;

    quest.Quest = (*candidates)[LifeWorld::Draw(env, uint32(candidates->size()), SALT_QUEST + life.Draws++)];
    EnvState& data = _scenario.Data(env);
    data.EpisodeMapId = quest.Quest->Giver->Map;
    data.HasEpisodeMap = true;
    // The band admits a quest whose minimum level is inside it; the seat's level meets that minimum.
    if (quest.Quest->MinLevel > data.EpisodeLevel)
        data.EpisodeLevel = uint8(std::min<uint32>(quest.Quest->MinLevel, DEFAULT_MAX_LEVEL));
    Position const& giver = quest.Quest->Giver->Pos;
    float const facing = giver.GetOrientation();
    data.EpisodeSpawn.Relocate(giver.GetPositionX() + std::cos(facing) * START_YARDS,
        giver.GetPositionY() + std::sin(facing) * START_YARDS, giver.GetPositionZ(), facing + float(M_PI));
    data.HasEpisodeSpawn = true;
    return true;
}

bool Animus::Curriculum::QuestEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvLife& life = _envs[env.Index];
    EnvQuest& quest = _quests[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    if (!quest.Quest || !bot || !map)
        return false;

    // The bot is the filter the table could not be: a giver's conditions and the chain's implicit prerequisites
    // are only known to Player::CanTakeQuest. A refusal fails the build, and the next draw is another quest.
    Quest const* info = sObjectMgr->GetQuestTemplate(quest.Quest->Id);
    if (!info || !bot->CanTakeQuest(info, false) || !bot->CanAddQuest(info, false))
    {
        LOG_INFO("module.animus", "{}: env {} cannot take quest {}; another next time", _scenario.Name(), env.Index,
            quest.Quest->Id);
        return false;
    }

    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    Creature* giver = Summon(env, life, map, *quest.Quest->Giver);
    if (!giver)
        return false;
    quest.Giver = giver->GetGUID();
    if (quest.Quest->Ender != quest.Quest->Giver)
        if (Creature* ender = Summon(env, life, map, *quest.Quest->Ender))
            quest.Ender = ender->GetGUID();
    if (quest.Ender.IsEmpty())
        quest.Ender = quest.Giver;

    // The objectives' places, with the world's own creatures around them: what the quest is about, and whatever
    // else lives there.
    for (Position const& place : quest.Quest->Objectives)
        SummonAround(env, life, map, place, tuning.ObjectiveRadius, tuning.ObjectiveSpawns, false);

    SetWaypoint(life, WAY_GIVER, giver->GetPosition());
    return true;
}

void Animus::Curriculum::QuestEncounter::Update(Env& env)
{
    EnvLife& life = _envs[env.Index];
    EnvQuest& quest = _quests[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    if (!quest.Quest || !bot)
        return;

    QuestStatus const status = bot->GetQuestStatus(quest.Quest->Id);
    quest.Accepted = quest.Accepted || status != QUEST_STATUS_NONE;
    quest.Complete = status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED;
    quest.TurnedIn = quest.TurnedIn || status == QUEST_STATUS_REWARDED || bot->GetQuestRewardStatus(quest.Quest->Id);
    quest.Progress = WorldActions::QuestProgress(bot, quest.Quest->Id);
    if (Quest const* info = sObjectMgr->GetQuestTemplate(quest.Quest->Id))
    {
        uint32 kills = 0;
        for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (info->RequiredNpcOrGo[i] > 0)
                kills += bot->GetReqKillOrCastCurrentCount(quest.Quest->Id, info->RequiredNpcOrGo[i]);
        quest.Kills = std::max(quest.Kills, kills);
    }

    // Where the quest wants the seat next: the giver, then the objectives (the nearest living objective creature
    // in sight, else the nearest objective place), then the turn-in.
    if (!quest.Accepted)
    {
        if (Creature* giver = ObjectAccessor::GetCreature(*bot, quest.Giver))
            SetWaypoint(life, WAY_GIVER, giver->GetPosition());
    }
    else if (!quest.Complete)
    {
        Position const* best = nullptr;
        for (Position const& place : quest.Quest->Objectives)
            if (!best || bot->GetExactDist2d(&place) < bot->GetExactDist2d(best))
                best = &place;
        Unit* target = env.FindTargetUnit(0);
        if (target && target->IsAlive() && bot->GetExactDist2d(target) <= OBJECTIVE_SIGHT)
            SetWaypoint(life, WAY_OBJECTIVE, target->GetPosition());
        else if (best)
            SetWaypoint(life, WAY_OBJECTIVE, *best);
    }
    else if (!quest.TurnedIn)
    {
        if (Creature* ender = ObjectAccessor::GetCreature(*bot, quest.Ender))
            SetWaypoint(life, WAY_ENDER, ender->GetPosition());
    }
    else
        ClearWaypoint(life);
}

void Animus::Curriculum::QuestEncounter::Sensed(Env const& env, EnvLife const& /*life*/, SeatView& view) const
{
    EnvQuest const& quest = _quests[env.Index];
    WorldView& world = view.World;
    world.QuestState = !quest.Accepted ? WorldView::QUEST_NONE : quest.Complete ? WorldView::QUEST_COMPLETE
        : WorldView::QUEST_ACTIVE;
    world.QuestProgress = quest.Progress;
}

void Animus::Curriculum::QuestEncounter::Account(Env& env, EnvLife& /*life*/, SeatActionResult const& result)
{
    EnvQuest& quest = _quests[env.Index];
    if (result.QuestAccepted)
        quest.Accepted = true;
    if (result.QuestTurnedIn)
        quest.TurnedIn = true;
}

void Animus::Curriculum::QuestEncounter::RewardMore(Env& env, EnvLife& life, Player* /*bot*/, RewardLedger& ledger)
{
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    EnvQuest& quest = _quests[env.Index];
    float const tierScale = TierScale(env);

    if (quest.Accepted && !quest.AcceptPaid)
    {
        quest.AcceptPaid = true;
        ledger.Add(RewardTerm::QuestAccepted, tuning.QuestAccepted);
    }
    // Each objective count as it lands: a kill, an item looted.
    if (quest.Progress > quest.ProgressPaid)
    {
        ledger.Add(RewardTerm::QuestCredit, tuning.QuestCredit * (quest.Progress - quest.ProgressPaid) * tierScale);
        quest.ProgressPaid = quest.Progress;
    }

    // The outcome, once: the turn-in wins; the clock without it costs what a lost fight costs, less what was done.
    if (life.OutcomePaid)
        return;
    if (quest.TurnedIn)
        ledger.Add(RewardTerm::QuestTurnIn, tuning.QuestTurnIn * tierScale);
    else if (TimeIsUp(env))
        ledger.Add(RewardTerm::Timeout, -tuning.QuestTimeout * (1.0f - quest.Progress) / tierScale);
}

bool Animus::Curriculum::QuestEncounter::Finished(Env const& env, EnvLife const& /*life*/) const
{
    return _quests[env.Index].TurnedIn;
}
