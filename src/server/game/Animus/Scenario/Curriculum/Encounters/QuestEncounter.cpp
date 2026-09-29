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
#include <unordered_map>
#include <unordered_set>

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
        RewardTerm::QuestAccepted, RewardTerm::QuestCredit, RewardTerm::QuestTurnIn, RewardTerm::Timeout,
        RewardTerm::Poach };
}

uint32 Animus::Curriculum::QuestEncounter::GroupCount(Env const& env) const
{
    ArenaDefinition const& arena = _scenario.Arena(env);
    return arena.Seats == SeatPlan::Teams ? TEAM_COUNT + std::min(arena.LoneSeats, MAX_LONE_SEATS) : 1;
}

void Animus::Curriculum::QuestEncounter::AddMoreEpisodeInfo(EpisodeInfoTable& table)
{
    auto const group = [this](Env const& env, uint32 seat) -> EnvQuest const&
    {
        return _quests[env.Index].Groups[GroupOf(env, seat)];
    };
    table.Add("quest_id", [group](Env const& env, uint32 seat)
    {
        EnvQuest const& quest = group(env, seat);
        return !quest.Chain.empty() ? float(quest.Chain.front()->Id) : 0.0f;
    });
    table.Add("quest_collect", [group](Env const& env, uint32 seat)
    {
        LifeWorld::QuestCandidate const* quest = group(env, seat).Quest();
        return quest && quest->Collect ? 1.0f : 0.0f;
    });
    table.Add("quest_held_out", [group](Env const& env, uint32 seat)
    {
        EnvQuest const& quest = group(env, seat);
        return std::any_of(quest.Chain.begin(), quest.Chain.end(),
            [](LifeWorld::QuestCandidate const* q) { return q->HeldOut; }) ? 1.0f : 0.0f;
    });
    table.Add("quest_chain_length", [group](Env const& env, uint32 seat) { return float(group(env, seat).Chain.size()); });
    table.Add("quests_turned_in", [group](Env const& env, uint32 seat) { return float(group(env, seat).TurnedInCount); });
    table.Add("quest_accepted", [group](Env const& env, uint32 seat) { return group(env, seat).Accepted ? 1.0f : 0.0f; });
    table.Add("quest_progress", [group](Env const& env, uint32 seat) { return group(env, seat).Progress; });
    table.Add("quest_complete", [group](Env const& env, uint32 seat) { return group(env, seat).Complete ? 1.0f : 0.0f; });
    table.Add("quest_turned_in", [group](Env const& env, uint32 seat)
    {
        EnvQuest const& quest = group(env, seat);
        return !quest.Chain.empty() && quest.TurnedInCount >= quest.Chain.size() ? 1.0f : 0.0f;
    });
    table.Add("quest_kills", [group](Env const& env, uint32 seat) { return float(group(env, seat).Kills); });
    table.Add("quest_places_found", [group](Env const& env, uint32 seat) { return float(group(env, seat).Places.size()); });
    // Poaching, and how contested the zone was (world_shared): credit taken in another group's place, and the
    // claims standing at the episode's end.
    table.Add("quest_poached", [this](Env const& env, uint32 seat)
    {
        return seat < MAX_SEATS ? float(_quests[env.Index].Pay[seat].Poached) : 0.0f;
    });
    table.Add("world_claims", [this](Env const& env, uint32)
    {
        return float(_quests[env.Index].Coordinator.Claims().size());
    });
    // Which objective kinds the group's quests hold, so turn-in rates can be read per kind.
    for (uint32 kind = 0; kind < OBJECTIVE_KIND_COUNT; ++kind)
        table.Add("quest_has_" + std::string(ObjectiveKindName(ObjectiveKind(kind))), [group, kind](Env const& env,
            uint32 seat)
        {
            for (LifeWorld::QuestCandidate const* quest : group(env, seat).Chain)
                for (PlannedObjective const& objective : quest->Plan->Objectives)
                    if (uint32(objective.Kind) == kind)
                        return 1.0f;
            return 0.0f;
        });
}

namespace
{
    /// A chain from `first`: its next quests while they are candidates for this seat (not a held-out one in
    /// training), up to `length`.
    std::vector<Animus::Curriculum::LifeWorld::QuestCandidate const*> ChainFrom(
        Animus::Curriculum::LifeWorld::QuestCandidate const* first, uint32 length, bool evaluating,
        Animus::Curriculum::LifeWorld::Side side)
    {
        std::vector<Animus::Curriculum::LifeWorld::QuestCandidate const*> chain;
        for (auto const* next = first; next && chain.size() < length; next = next->Next)
        {
            if (!chain.empty() && ((next->HeldOut && !evaluating)
                || (next->For != Animus::Curriculum::LifeWorld::Side::Any && next->For != side)))
                break;
            chain.push_back(next);
        }
        return chain;
    }

    /// How far another group's quest giver may be from the first's in a shared zone: close enough that their work
    /// overlaps.
    constexpr float SHARED_ZONE_YARDS = 400.0f;
}

bool Animus::Curriculum::QuestEncounter::Place(Env& env, EnvLife& life)
{
    EnvQuests& quests = _quests[env.Index];
    quests = EnvQuests();

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

    uint32 const weights = CHAIN_WEIGHTS[0] + CHAIN_WEIGHTS[1] + CHAIN_WEIGHTS[2];
    auto const length = [&](uint32 salt)
    {
        uint32 const roll = LifeWorld::Draw(env, weights, salt);
        return roll < CHAIN_WEIGHTS[0] ? 1u : roll < CHAIN_WEIGHTS[0] + CHAIN_WEIGHTS[1] ? 2u : 3u;
    };
    LifeWorld::QuestCandidate const* first =
        (*candidates)[LifeWorld::Draw(env, uint32(candidates->size()), SALT_QUEST + life.Draws++)];
    quests.Groups[0].Chain = ChainFrom(first, length(SALT_CHAIN + life.Draws), env.Evaluating, life.Side);

    // The other groups in the same zone (world_shared) -- the second side, and each seat questing alone beside
    // them: half the time the same quest -- the same creatures, the hardest sharing there is -- else another whose
    // giver is near the first's.
    for (uint32 g = 1; g < GroupCount(env); ++g)
    {
        LifeWorld::QuestCandidate const* other = first;
        if (LifeWorld::Draw(env, 2, SALT_CHAIN + 7 * g + life.Draws))
        {
            std::vector<LifeWorld::QuestCandidate const*> near;
            for (LifeWorld::QuestCandidate const* candidate : *candidates)
                if (candidate != first && candidate->Giver->Map == first->Giver->Map
                    && candidate->Giver->Pos.GetExactDist2d(&first->Giver->Pos) <= SHARED_ZONE_YARDS)
                    near.push_back(candidate);
            if (!near.empty())
                other = near[LifeWorld::Draw(env, uint32(near.size()), SALT_QUEST + 31 * g + life.Draws)];
        }
        quests.Groups[g].Chain = ChainFrom(other, length(SALT_CHAIN + 13 * g + life.Draws), env.Evaluating,
            life.Side);
    }

    EnvState& data = _scenario.Data(env);
    data.EpisodeMapId = first->Giver->Map;
    data.HasEpisodeMap = true;
    // The band admits a quest whose minimum level is inside it; the seats' level meets every quest's minimum.
    uint32 level = data.EpisodeLevel;
    for (EnvQuest const& group : quests.Groups)
        for (LifeWorld::QuestCandidate const* q : group.Chain)
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
    EnvQuests& quests = _quests[env.Index];
    EnvState const& data = _scenario.Data(env);
    if (quests.Groups[0].Chain.empty() || !map)
        return false;

    // A group of seats is a real group: kill and loot credit go to all of it.
    FormGroups(env);

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
    std::unordered_set<uint32> placedObjects;

    for (uint32 g = 0; g < GroupCount(env); ++g)
    {
        EnvQuest& quest = quests.Groups[g];
        if (quest.Chain.empty())
            continue;

        // Prerequisites granted (the plan's decision) to each seat of the group: whatever each quest needs
        // rewarded, except the chain's own quests, which the group does itself.
        Player* leader = nullptr;
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        {
            Player* bot = _scenario.SeatBot(env, seat);
            if (!bot || GroupOf(env, seat) != g)
                continue;
            leader = leader ? leader : bot;
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
        }

        // The bot is the filter the table could not be: a giver's conditions and the chain's implicit
        // prerequisites are only known to Player::CanTakeQuest. A refusal fails the build.
        Quest const* info = sObjectMgr->GetQuestTemplate(quest.Chain.front()->Id);
        if (!leader || !info || !leader->CanTakeQuest(info, false) || !leader->CanAddQuest(info, false))
        {
            LOG_INFO("module.animus", "{}: env {} cannot take quest {}; another next time", _scenario.Name(),
                env.Index, quest.Chain.front()->Id);
            return false;
        }

        for (LifeWorld::QuestCandidate const* q : quest.Chain)
        {
            ObjectGuid const giver = npc(*q->Giver);
            if (giver.IsEmpty())
                return false;
            quest.Givers.push_back(giver);
            ObjectGuid const ender = npc(*q->Ender);
            quest.Enders.push_back(ender.IsEmpty() ? giver : ender);

            // Each objective's place with what it needs: its sources' spawns first, then the world's own creatures
            // around it -- once, however many groups want it.
            for (uint32 i = 0; i < q->Places.size(); ++i)
            {
                LifeWorld::ObjectivePlace const& place = q->Places[i];
                ObjectiveKind const kind = q->Plan->Objectives[i].Kind;
                uint32 placed = 0;
                for (LifeWorld::Spawn const* spawn : place.Spawns)
                {
                    if (placed >= tuning.ObjectiveSpawns)
                        break;
                    if (spawn->Object ? placedObjects.insert(spawn->SpawnId).second
                            && SummonObject(env, life, map, *spawn) != nullptr : !npc(*spawn).IsEmpty())
                        ++placed;
                }
                if (IsCreatureKind(kind) && placed < tuning.ObjectiveSpawns)
                    SummonAround(env, life, map, place.Where, tuning.ObjectiveRadius, tuning.ObjectiveSpawns - placed,
                        false);
            }
        }

        if (Creature* giver = ObjectAccessor::GetCreature(*leader, quest.Givers.front()))
            SetWaypoint(life, WAY_GIVER, giver->GetPosition(), g);
    }
    return true;
}

void Animus::Curriculum::QuestEncounter::Update(Env& env)
{
    EnvLife& life = _envs[env.Index];
    EnvQuests& quests = _quests[env.Index];
    EnvState const& data = _scenario.Data(env);
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    quests.Coordinator.Expire(env.EpisodeElapsedMs);

    for (uint32 g = 0; g < GroupCount(env); ++g)
    {
        EnvQuest& quest = quests.Groups[g];
        LifeWorld::QuestCandidate const* current = quest.Quest();
        if (!current)
            continue;

        std::vector<Player*> seats;
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            if (Player* bot = _scenario.SeatBot(env, seat); bot && GroupOf(env, seat) == g)
                seats.push_back(bot);
        if (seats.empty())
            continue;

        // A quest one member takes is shared with the rest, as a player shares it with the group.
        Quest const* info = sObjectMgr->GetQuestTemplate(current->Id);
        bool accepted = false;
        for (Player* bot : seats)
            accepted = accepted || bot->GetQuestStatus(current->Id) != QUEST_STATUS_NONE
                || bot->GetQuestRewardStatus(current->Id);
        if (accepted && info)
            for (Player* bot : seats)
                if (bot->IsAlive() && bot->GetQuestStatus(current->Id) == QUEST_STATUS_NONE
                    && !bot->GetQuestRewardStatus(current->Id) && bot->CanTakeQuest(info, false)
                    && bot->CanAddQuest(info, false))
                    bot->AddQuestAndCheckCompletion(info, nullptr);

        // The group's state: taken by any, complete and handed in by every living member, and its mean progress.
        quest.Accepted = accepted;
        bool complete = true, turnedIn = true;
        float done = 0.0f;
        uint32 living = 0;
        for (Player* bot : seats)
        {
            if (!bot->IsAlive())
                continue;
            ++living;
            QuestStatus const status = bot->GetQuestStatus(current->Id);
            bool const rewarded = status == QUEST_STATUS_REWARDED || bot->GetQuestRewardStatus(current->Id);
            complete = complete && (status == QUEST_STATUS_COMPLETE || rewarded);
            turnedIn = turnedIn && rewarded;
            float mine = 0.0f;
            for (uint32 i = 0; i < current->Plan->Objectives.size(); ++i)
                mine += QuestPlanner::Progress(bot, *current->Plan, i);
            done += rewarded ? 1.0f : mine / float(std::max<std::size_t>(1, current->Plan->Objectives.size()));
        }
        if (!living)
            continue;
        quest.Complete = complete;
        quest.TurnedIn = quest.TurnedIn || turnedIn;
        quest.Progress = done / float(living);
        if (info)
        {
            uint32 kills = 0;
            for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (info->RequiredNpcOrGo[i] > 0)
                    kills += seats.front()->GetReqKillOrCastCurrentCount(current->Id, info->RequiredNpcOrGo[i]);
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
                quest.Accepted = quest.Complete = quest.TurnedIn = false;
                quest.Progress = 0.0f;
                quest.Places.clear();
                current = quest.Quest();
            }
            else
            {
                ClearWaypoint(life, g);
                continue;
            }
        }

        // Places found: the current quest's undone objectives' sources, seen near any member.
        if (env.EpisodeElapsedMs >= quest.ScannedMs + FOUND_SCAN_MS)
        {
            quest.ScannedMs = env.EpisodeElapsedMs;
            Player* anchor = seats.front();
            for (ObjectGuid const& guid : life.Spawned)
            {
                WorldObject* thing = guid.IsGameObject()
                    ? static_cast<WorldObject*>(anchor->GetMap()->GetGameObject(guid))
                    : static_cast<WorldObject*>(ObjectAccessor::GetCreature(*anchor, guid));
                if (!thing)
                    continue;
                if (Creature* creature = thing->ToCreature(); creature && !creature->IsAlive())
                    continue;
                bool const seen = std::any_of(seats.begin(), seats.end(),
                    [thing](Player* bot) { return bot->IsAlive() && bot->GetExactDist2d(thing) <= FOUND_SIGHT; });
                if (!seen)
                    continue;
                for (uint32 i = 0; i < current->Plan->Objectives.size(); ++i)
                {
                    PlannedObjective const& objective = current->Plan->Objectives[i];
                    if (objective.SourcesAreObjects != guid.IsGameObject()
                        || std::find(objective.Sources.begin(), objective.Sources.end(), thing->GetEntry())
                            == objective.Sources.end()
                        || QuestPlanner::Progress(seats.front(), *current->Plan, i) >= 1.0f)
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

        // The coordinator: a group working a place of its quest holds it for a while.
        for (Player* bot : seats)
        {
            if (!bot->IsAlive())
                continue;
            for (EnvQuest::Found const& found : quest.Places)
                if (bot->GetExactDist2d(&found.Where) <= tuning.ClaimRadius)
                    quests.Coordinator.Stake(g, found.Where, tuning.ClaimRadius, env.EpisodeElapsedMs, tuning.ClaimHoldMs);
            for (LifeWorld::ObjectivePlace const& place : current->Places)
                if (bot->GetExactDist2d(&place.Where) <= tuning.ClaimRadius)
                    quests.Coordinator.Stake(g, place.Where, tuning.ClaimRadius, env.EpisodeElapsedMs, tuning.ClaimHoldMs);
        }

        // Where the quest wants the group next: the giver, then the undone objective the coordinator assigns (a
        // place no other group holds, when there is one) -- a living source in sight for a creature objective --
        // then the turn-in.
        Player* lead = seats.front();
        if (!quest.Accepted)
        {
            if (Creature* giver = ObjectAccessor::GetCreature(*lead, quest.Givers[quest.Current]))
                SetWaypoint(life, WAY_GIVER, giver->GetPosition(), g);
        }
        else if (!quest.Complete)
        {
            std::vector<Position> undone;
            bool creatureWanted = false;
            for (uint32 i = 0; i < current->Places.size(); ++i)
                if (QuestPlanner::Progress(lead, *current->Plan, i) < 1.0f)
                {
                    creatureWanted = creatureWanted || IsCreatureKind(current->Plan->Objectives[i].Kind);
                    undone.push_back(current->Places[i].Where);
                }
            std::sort(undone.begin(), undone.end(), [lead](Position const& a, Position const& b)
            {
                return lead->GetExactDist2d(&a) < lead->GetExactDist2d(&b);
            });
            Unit* target = env.FindTargetUnit(0);
            if (creatureWanted && target && target->IsAlive() && lead->GetExactDist2d(target) <= OBJECTIVE_SIGHT
                && !quests.Coordinator.ClaimedByOther(g, *target, env.EpisodeElapsedMs))
                SetWaypoint(life, WAY_OBJECTIVE, target->GetPosition(), g);
            else if (Position const* assigned = quests.Coordinator.Assign(g, undone, env.EpisodeElapsedMs))
                SetWaypoint(life, WAY_OBJECTIVE, *assigned, g);
        }
        else if (Creature* ender = ObjectAccessor::GetCreature(*lead, quest.Enders[quest.Current]))
            SetWaypoint(life, WAY_ENDER, ender->GetPosition(), g);
    }
}

void Animus::Curriculum::QuestEncounter::Sensed(Env const& env, EnvLife const& life, uint32 seat, SeatView& view) const
{
    EnvQuests const& quests = _quests[env.Index];
    uint32 const g = GroupOf(env, seat);
    EnvQuest const& quest = quests.Groups[g];
    WorldView& world = view.World;
    world.QuestState = !quest.Accepted ? WorldView::QUEST_NONE : quest.Complete ? WorldView::QUEST_COMPLETE
        : WorldView::QUEST_ACTIVE;
    world.QuestProgress = quest.Progress;

    // The journal, shared by the group: the current quest's objectives, where to take it and hand it in, what was
    // found (and whether another group holds it), the chain, and the coordinator's assignment.
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
    world.HasGiver = !quest.Accepted;          // the giver leaves the journal once the quest is taken
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
        place.Claimed = quests.Coordinator.ClaimedByOther(g, place.Where, env.EpisodeElapsedMs);
    }
    if (Waypoint const& way = life.Ways[g]; way.Has && way.Kind == WAY_OBJECTIVE)
    {
        world.HasAssignment = true;
        world.Assignment = way.Where;
    }
}

void Animus::Curriculum::QuestEncounter::ViewDirector(Env const& env, uint32 side,
    DirectorLayout::DirectorView& view) const
{
    EnvQuest const& quest = _quests[env.Index].Groups[std::min<uint32>(side, TEAM_COUNT - 1)];
    LifeWorld::QuestCandidate const* current = quest.Quest();
    Player* bot = nullptr;
    for (uint32 seat = 0; seat < _scenario.Data(env).ActiveSeats && !bot; ++seat)
        if (GroupOf(env, seat) == side)
            bot = _scenario.SeatBot(env, seat);
    if (!current || !bot || !quest.Accepted)
        return;
    view.HasObjective = true;
    for (uint32 i = 0; i < current->Plan->Objectives.size() && i < view.Objectives.size(); ++i)
        view.Objectives[i] = QuestPlanner::Progress(bot, *current->Plan, i) < 1.0f;
}

void Animus::Curriculum::QuestEncounter::RewardMore(Env& env, EnvLife& life, uint32 seat, Player* bot,
    RewardLedger& ledger)
{
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    EnvQuests& quests = _quests[env.Index];
    if (seat >= MAX_SEATS)
        return;
    uint32 const g = GroupOf(env, seat);
    EnvQuest const& quest = quests.Groups[g];
    SeatPay& pay = quests.Pay[seat];
    float const tierScale = TierScale(env);

    // Each member is paid its group's quest: taking it, each objective count as it lands, each hand-in.
    uint32 const accepted = quest.TurnedInCount + (quest.Accepted && !quest.TurnedIn ? 1 : 0);
    for (; pay.AcceptPaid < accepted; ++pay.AcceptPaid)
        ledger.Add(RewardTerm::QuestAccepted, tuning.QuestAccepted);
    if (pay.ProgressQuest != quest.Current)
    {
        pay.ProgressQuest = quest.Current;
        pay.ProgressPaid = 0.0f;
    }
    if (quest.Progress > pay.ProgressPaid)
    {
        float const gained = quest.Progress - pay.ProgressPaid;
        ledger.Add(RewardTerm::QuestCredit, tuning.QuestCredit * gained * tierScale);
        pay.ProgressPaid = quest.Progress;
        // Taken in a place another group holds: poached.
        if (bot && bot->IsAlive() && quests.Coordinator.ClaimedByOther(g, *bot, env.EpisodeElapsedMs))
        {
            ledger.Add(RewardTerm::Poach, -tuning.Poach * gained);
            ++pay.Poached;
        }
    }
    for (; pay.TurnInsPaid < quest.TurnedInCount; ++pay.TurnInsPaid)
        ledger.Add(RewardTerm::QuestTurnIn, tuning.QuestTurnIn * tierScale);

    // The clock without the whole chain costs what a lost fight costs, less what was done -- once per seat.
    if (pay.TimeoutPaid || life.OutcomePaid || Finished(env, life) || !TimeIsUp(env) || quest.Chain.empty())
        return;
    pay.TimeoutPaid = true;
    float const done = (float(quest.TurnedInCount) + quest.Progress) / float(quest.Chain.size());
    ledger.Add(RewardTerm::Timeout, -tuning.QuestTimeout * (1.0f - done) / tierScale);
}

bool Animus::Curriculum::QuestEncounter::Finished(Env const& env, EnvLife const& /*life*/) const
{
    EnvQuests const& quests = _quests[env.Index];
    bool any = false;
    for (uint32 g = 0; g < GroupCount(env); ++g)
    {
        EnvQuest const& quest = quests.Groups[g];
        if (quest.Chain.empty())
            continue;
        any = true;
        if (quest.TurnedInCount < quest.Chain.size())
            return false;
    }
    return any;
}
