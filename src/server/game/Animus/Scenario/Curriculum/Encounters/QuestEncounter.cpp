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
    constexpr uint32 SALT_CHAIN = 29;
    /// Where the seat stands at the start: a few yards in front of the giver, facing it.
    constexpr float START_YARDS = 1.5f;     // close: a giver on a ledge or a stair has no ground four yards out
    /// A living objective creature this near replaces the objective's place as the waypoint.
    constexpr float OBJECTIVE_SIGHT = 120.0f;
    /// Found places: what the current quest wants, seen this near, is written into the journal (merged within
    /// FOUND_MERGE yards), looked for every FOUND_SCAN_MS.
    constexpr float FOUND_SIGHT = 60.0f;
    constexpr float FOUND_MERGE = 25.0f;
    constexpr uint32 FOUND_SCAN_MS = 1000;
    /// How long a chain an episode draws: one quest half the time, two or three the rest (when the chain has them).
    constexpr std::array<uint32, 3> CHAIN_WEIGHTS = { 5, 3, 2 };

    enum Waypoints : uint8 { WAY_GIVER = 1, WAY_OBJECTIVE = 2, WAY_ENDER = 3 };

    bool IsCreatureKind(ObjectiveKind kind)
    {
        return kind == ObjectiveKind::Kill || kind == ObjectiveKind::CollectFromCreature
            || kind == ObjectiveKind::UseItemOn;
    }
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
        return !_quests[env.Index].Chain.empty() ? float(_quests[env.Index].Chain.front()->Id) : 0.0f;
    });
    table.Add("quest_collect", [this](Env const& env, uint32)
    {
        LifeWorld::QuestCandidate const* quest = _quests[env.Index].Quest();
        return quest && quest->Collect ? 1.0f : 0.0f;
    });
    table.Add("quest_held_out", [this](Env const& env, uint32)
    {
        EnvQuest const& quest = _quests[env.Index];
        return std::any_of(quest.Chain.begin(), quest.Chain.end(),
            [](LifeWorld::QuestCandidate const* q) { return q->HeldOut; }) ? 1.0f : 0.0f;
    });
    table.Add("quest_chain_length", [this](Env const& env, uint32) { return float(_quests[env.Index].Chain.size()); });
    table.Add("quests_turned_in", [this](Env const& env, uint32) { return float(_quests[env.Index].TurnedInCount); });
    table.Add("quest_accepted", [this](Env const& env, uint32) { return _quests[env.Index].Accepted ? 1.0f : 0.0f; });
    table.Add("quest_progress", [this](Env const& env, uint32) { return _quests[env.Index].Progress; });
    table.Add("quest_complete", [this](Env const& env, uint32) { return _quests[env.Index].Complete ? 1.0f : 0.0f; });
    table.Add("quest_turned_in", [this](Env const& env, uint32)
    {
        EnvQuest const& quest = _quests[env.Index];
        return !quest.Chain.empty() && quest.TurnedInCount >= quest.Chain.size() ? 1.0f : 0.0f;
    });
    table.Add("quest_kills", [this](Env const& env, uint32) { return float(_quests[env.Index].Kills); });
    table.Add("quest_places_found", [this](Env const& env, uint32) { return float(_quests[env.Index].Places.size()); });
    // Which objective kinds the episode's quests hold, so turn-in rates can be read per kind.
    for (uint32 kind = 0; kind < OBJECTIVE_KIND_COUNT; ++kind)
        table.Add("quest_has_" + std::string(ObjectiveKindName(ObjectiveKind(kind))), [this, kind](Env const& env, uint32)
        {
            for (LifeWorld::QuestCandidate const* quest : _quests[env.Index].Chain)
                for (PlannedObjective const& objective : quest->Plan->Objectives)
                    if (uint32(objective.Kind) == kind)
                        return 1.0f;
            return 0.0f;
        });
}

bool Animus::Curriculum::QuestEncounter::Place(Env& env, EnvLife& life)
{
    EnvQuest& quest = _quests[env.Index];
    quest = EnvQuest();

    LifeWorld::QuestSet const& set = LifeWorld::QuestSet::Instance();
    std::vector<LifeWorld::QuestCandidate const*> const* candidates = &set.For(life.Tier, life.Side, env.Evaluating);
    if (candidates->empty())
    {
        // The other side's, then: the seat's side follows the quest (EpisodeTeam).
        life.Side = life.Side == LifeWorld::Side::Horde ? LifeWorld::Side::Alliance : LifeWorld::Side::Horde;
        candidates = &set.For(life.Tier, life.Side, env.Evaluating);
        _scenario.Data(env).EpisodeTeam = uint8(LifeWorld::TeamOf(life.Side)) + 1;
    }
    if (candidates->empty())
        return false;

    LifeWorld::QuestCandidate const* first =
        (*candidates)[LifeWorld::Draw(env, uint32(candidates->size()), SALT_QUEST + life.Draws++)];

    // The chain: follow the next quests while they are candidates for this seat (not a held-out one in training).
    uint32 const weights = CHAIN_WEIGHTS[0] + CHAIN_WEIGHTS[1] + CHAIN_WEIGHTS[2];
    uint32 const roll = LifeWorld::Draw(env, weights, SALT_CHAIN + life.Draws);
    uint32 const length = roll < CHAIN_WEIGHTS[0] ? 1 : roll < CHAIN_WEIGHTS[0] + CHAIN_WEIGHTS[1] ? 2 : 3;
    for (LifeWorld::QuestCandidate const* next = first; next && quest.Chain.size() < length; next = next->Next)
    {
        if (quest.Chain.size() && ((next->HeldOut && !env.Evaluating)
            || (next->For != LifeWorld::Side::Any && next->For != life.Side)))
            break;
        quest.Chain.push_back(next);
    }

    EnvState& data = _scenario.Data(env);
    data.EpisodeMapId = first->Giver->Map;
    data.HasEpisodeMap = true;
    // The band admits a quest whose minimum level is inside it; the seat's level meets every quest's minimum.
    uint32 level = data.EpisodeLevel;
    for (LifeWorld::QuestCandidate const* q : quest.Chain)
        level = std::max(level, q->MinLevel);
    data.EpisodeLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
    Position const& giver = first->Giver->Pos;
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
    if (quest.Chain.empty() || !bot || !map)
        return false;

    // Prerequisites granted (the plan's decision): whatever each quest needs rewarded, except the chain's own
    // quests, which the seat does itself.
    for (LifeWorld::QuestCandidate const* q : quest.Chain)
    {
        QuestPlan trimmed = *q->Plan;
        std::erase_if(trimmed.Prerequisites, [&quest](uint32 id)
        {
            return std::any_of(quest.Chain.begin(), quest.Chain.end(),
                [id](LifeWorld::QuestCandidate const* c) { return c->Id == id; });
        });
        QuestPlanner::GrantPrerequisites(bot, trimmed);
    }

    // The bot is the filter the table could not be: a giver's conditions and the chain's implicit prerequisites
    // are only known to Player::CanTakeQuest. A refusal fails the build, and the next draw is another quest.
    Quest const* info = sObjectMgr->GetQuestTemplate(quest.Chain.front()->Id);
    if (!info || !bot->CanTakeQuest(info, false) || !bot->CanAddQuest(info, false))
    {
        LOG_INFO("module.animus", "{}: env {} cannot take quest {}; another next time", _scenario.Name(), env.Index,
            quest.Chain.front()->Id);
        return false;
    }

    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    std::unordered_map<uint32, ObjectGuid> summonedNpcs;
    auto const npc = [&](LifeWorld::Spawn const& spawn) -> ObjectGuid
    {
        if (auto const found = summonedNpcs.find(spawn.SpawnId); found != summonedNpcs.end())
            return found->second;
        Creature* creature = Summon(env, life, map, spawn);
        ObjectGuid const guid = creature ? creature->GetGUID() : ObjectGuid::Empty;
        summonedNpcs[spawn.SpawnId] = guid;
        return guid;
    };

    for (LifeWorld::QuestCandidate const* q : quest.Chain)
    {
        ObjectGuid const giver = npc(*q->Giver);
        if (giver.IsEmpty())
            return false;
        quest.Givers.push_back(giver);
        ObjectGuid const ender = npc(*q->Ender);
        quest.Enders.push_back(ender.IsEmpty() ? giver : ender);

        // Each objective's place with what it needs: its sources' spawns first (objects, a vendor, the creatures
        // to kill or loot), then the world's own creatures around it, whatever else lives there.
        for (uint32 i = 0; i < q->Places.size(); ++i)
        {
            LifeWorld::ObjectivePlace const& place = q->Places[i];
            ObjectiveKind const kind = q->Plan->Objectives[i].Kind;
            uint32 placed = 0;
            for (LifeWorld::Spawn const* spawn : place.Spawns)
            {
                if (placed >= tuning.ObjectiveSpawns)
                    break;
                if (spawn->Object ? SummonObject(env, life, map, *spawn) != nullptr : !npc(*spawn).IsEmpty())
                    ++placed;
            }
            if (IsCreatureKind(kind))
                SummonAround(env, life, map, place.Where, tuning.ObjectiveRadius,
                    tuning.ObjectiveSpawns > placed ? tuning.ObjectiveSpawns - placed : 0, false);
        }
    }

    if (Creature* giver = ObjectAccessor::GetCreature(*bot, quest.Givers.front()))
        SetWaypoint(life, WAY_GIVER, giver->GetPosition());
    return true;
}

void Animus::Curriculum::QuestEncounter::Update(Env& env)
{
    EnvLife& life = _envs[env.Index];
    EnvQuest& quest = _quests[env.Index];
    Player* bot = _scenario.SeatBot(env, 0);
    LifeWorld::QuestCandidate const* current = quest.Quest();
    if (!current || !bot)
        return;

    QuestStatus const status = bot->GetQuestStatus(current->Id);
    quest.Accepted = quest.Accepted || status != QUEST_STATUS_NONE;
    quest.Complete = status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_REWARDED;
    quest.TurnedIn = quest.TurnedIn || status == QUEST_STATUS_REWARDED || bot->GetQuestRewardStatus(current->Id);

    float done = 0.0f;
    for (uint32 i = 0; i < current->Plan->Objectives.size(); ++i)
        done += QuestPlanner::Progress(bot, *current->Plan, i);
    quest.Progress = quest.TurnedIn ? 1.0f : done / float(std::max<std::size_t>(1, current->Plan->Objectives.size()));
    if (Quest const* info = sObjectMgr->GetQuestTemplate(current->Id))
    {
        uint32 kills = 0;
        for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (info->RequiredNpcOrGo[i] > 0)
                kills += bot->GetReqKillOrCastCurrentCount(current->Id, info->RequiredNpcOrGo[i]);
        quest.Kills = std::max(quest.Kills, kills);
    }

    // On to the chain's next quest once this one is handed in.
    if (quest.TurnedIn)
    {
        if (quest.TurnedInCount <= quest.Current)
            quest.TurnedInCount = quest.Current + 1;
        if (quest.Current + 1 < quest.Chain.size())
        {
            ++quest.Current;
            quest.Accepted = quest.Complete = quest.TurnedIn = quest.AcceptPaid = false;
            quest.Progress = quest.ProgressPaid = 0.0f;
            quest.Places.clear();
            current = quest.Quest();
        }
        else
        {
            ClearWaypoint(life);
            return;
        }
    }

    // Places found: the current quest's undone objectives' sources, seen near the seat.
    if (env.EpisodeElapsedMs >= quest.ScannedMs + FOUND_SCAN_MS)
    {
        quest.ScannedMs = env.EpisodeElapsedMs;
        for (ObjectGuid const& guid : life.Spawned)
        {
            WorldObject* thing = guid.IsGameObject() ? static_cast<WorldObject*>(bot->GetMap()->GetGameObject(guid))
                : static_cast<WorldObject*>(ObjectAccessor::GetCreature(*bot, guid));
            if (!thing || bot->GetExactDist2d(thing) > FOUND_SIGHT)
                continue;
            if (Creature* creature = thing->ToCreature(); creature && !creature->IsAlive())
                continue;
            for (uint32 i = 0; i < current->Plan->Objectives.size(); ++i)
            {
                PlannedObjective const& objective = current->Plan->Objectives[i];
                if (objective.SourcesAreObjects != guid.IsGameObject()
                    || std::find(objective.Sources.begin(), objective.Sources.end(), thing->GetEntry())
                        == objective.Sources.end()
                    || QuestPlanner::Progress(bot, *current->Plan, i) >= 1.0f)
                    continue;
                auto const near = std::find_if(quest.Places.begin(), quest.Places.end(),
                    [thing](EnvQuest::Found const& f) { return f.Where.GetExactDist2d(thing) <= FOUND_MERGE; });
                if (near != quest.Places.end())
                    near->SeenMs = env.EpisodeElapsedMs;
                else
                {
                    if (quest.Places.size() >= WorldView::JOURNAL_PLACES)
                        quest.Places.erase(std::min_element(quest.Places.begin(), quest.Places.end(),
                            [](EnvQuest::Found const& a, EnvQuest::Found const& b) { return a.SeenMs < b.SeenMs; }));
                    quest.Places.push_back({ thing->GetPosition(), env.EpisodeElapsedMs, uint8(i) });
                }
            }
        }
    }

    // Where the quest wants the seat next: the giver, then the nearest undone objective (a living source in sight
    // for a creature objective, else its place), then the turn-in.
    if (!quest.Accepted)
    {
        if (Creature* giver = ObjectAccessor::GetCreature(*bot, quest.Givers[quest.Current]))
            SetWaypoint(life, WAY_GIVER, giver->GetPosition());
    }
    else if (!quest.Complete)
    {
        Position const* best = nullptr;
        bool creatureWanted = false;
        for (uint32 i = 0; i < current->Places.size(); ++i)
        {
            if (QuestPlanner::Progress(bot, *current->Plan, i) >= 1.0f)
                continue;
            creatureWanted = creatureWanted || IsCreatureKind(current->Plan->Objectives[i].Kind);
            Position const& place = current->Places[i].Where;
            if (!best || bot->GetExactDist2d(&place) < bot->GetExactDist2d(best))
                best = &place;
        }
        Unit* target = env.FindTargetUnit(0);
        if (creatureWanted && target && target->IsAlive() && bot->GetExactDist2d(target) <= OBJECTIVE_SIGHT)
            SetWaypoint(life, WAY_OBJECTIVE, target->GetPosition());
        else if (best)
            SetWaypoint(life, WAY_OBJECTIVE, *best);
    }
    else if (Creature* ender = ObjectAccessor::GetCreature(*bot, quest.Enders[quest.Current]))
        SetWaypoint(life, WAY_ENDER, ender->GetPosition());
}

void Animus::Curriculum::QuestEncounter::Sensed(Env const& env, EnvLife const& /*life*/, SeatView& view) const
{
    EnvQuest const& quest = _quests[env.Index];
    WorldView& world = view.World;
    world.QuestState = !quest.Accepted ? WorldView::QUEST_NONE : quest.Complete ? WorldView::QUEST_COMPLETE
        : WorldView::QUEST_ACTIVE;
    world.QuestProgress = quest.Progress;

    // The journal: the current quest's objectives, where to take it and hand it in, what was found, the chain.
    LifeWorld::QuestCandidate const* current = quest.Quest();
    world.ChainLength = uint8(quest.Chain.size());
    world.ChainIndex = uint8(quest.TurnedInCount);
    if (!current || !view.Bot)
        return;

    for (uint32 i = 0; i < current->Plan->Objectives.size() && i < WorldView::JOURNAL_OBJECTIVES; ++i)
    {
        WorldView::JournalObjective& objective = world.Objectives[i];
        objective.Present = true;
        objective.Kind = uint8(current->Plan->Objectives[i].Kind);
        objective.Left = quest.Accepted ? 1.0f - QuestPlanner::Progress(view.Bot, *current->Plan, i) : 1.0f;
        objective.HasPlace = i < current->Places.size();
        if (objective.HasPlace)
            objective.Place = current->Places[i].Where;
    }
    world.HasGiver = true;
    world.GiverAt = current->Giver->Pos;
    world.HasEnder = true;
    world.EnderAt = current->Ender->Pos;
    for (uint32 i = 0; i < quest.Places.size() && i < WorldView::JOURNAL_PLACES; ++i)
    {
        WorldView::JournalPlace& place = world.Places[i];
        place.Present = true;
        place.Where = quest.Places[i].Where;
        place.AgeSeconds = float(env.EpisodeElapsedMs - quest.Places[i].SeenMs) / 1000.0f;
        place.Objective = quest.Places[i].Objective;
    }
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
    // Each objective count as it lands: a kill, an item looted, an object used, a place reached.
    if (quest.Progress > quest.ProgressPaid)
    {
        ledger.Add(RewardTerm::QuestCredit, tuning.QuestCredit * (quest.Progress - quest.ProgressPaid) * tierScale);
        quest.ProgressPaid = quest.Progress;
    }

    // Each turn-in, once; the clock without the whole chain costs what a lost fight costs, less what was done.
    for (; quest.TurnInsPaid < quest.TurnedInCount; ++quest.TurnInsPaid)
        ledger.Add(RewardTerm::QuestTurnIn, tuning.QuestTurnIn * tierScale);

    // (The base class marks the outcome paid after this, so the clock is charged once.)
    if (life.OutcomePaid || Finished(env, life))
        return;
    if (TimeIsUp(env))
    {
        float const done = (float(quest.TurnedInCount) + quest.Progress) / float(std::max<std::size_t>(1, quest.Chain.size()));
        ledger.Add(RewardTerm::Timeout, -tuning.QuestTimeout * (1.0f - done) / tierScale);
    }
}

bool Animus::Curriculum::QuestEncounter::Finished(Env const& env, EnvLife const& /*life*/) const
{
    EnvQuest const& quest = _quests[env.Index];
    return !quest.Chain.empty() && quest.TurnedInCount >= quest.Chain.size();
}
