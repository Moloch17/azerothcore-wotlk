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
#include "LootMgr.h"
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

    /// An objective drill starts this far from the objective's place: within reach, not on top of it.
    constexpr float DRILL_START_MIN = 25.0f;
    constexpr float DRILL_START_MAX = 60.0f;
    constexpr uint32 SALT_DRILL = 43;

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
        RewardTerm::Poach, RewardTerm::Stall };
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

    /// Refusals after which a quest is never drawn again, and draws tried before settling for a retired one.
    constexpr uint32 REFUSALS_TO_RETIRE = 2;
    constexpr uint32 DRAW_ATTEMPTS = 8;

    /// Which of Player::CanTakeQuest's checks refused `quest`, for the log, and whether that check would refuse
    /// every bot alike (`general`) or only this one -- its class, race, level, skill, reputation or its own conditions.
    char const* RefusalReason(Player* bot, Quest const* quest, bool& general)
    {
        general = false;
        if (!bot->SatisfyQuestStatus(quest, false)) return "status";
        if (!bot->SatisfyQuestClass(quest, false)) return "class";
        if (!bot->SatisfyQuestRace(quest, false)) return "race";
        if (!bot->SatisfyQuestLevel(quest, false)) return "level";
        if (!bot->SatisfyQuestSkill(quest, false)) return "skill";
        if (!bot->SatisfyQuestReputation(quest, false)) return "reputation";
        if (!bot->SatisfyQuestConditions(quest, false)) return "conditions";
        general = true;
        if (!bot->SatisfyQuestExclusiveGroup(quest, false)) return "exclusive group";
        if (!bot->SatisfyQuestPreviousQuest(quest, false)) return "previous quest";
        if (!bot->SatisfyQuestTimed(quest, false)) return "timed";
        if (!bot->SatisfyQuestNextChain(quest, false)) return "next chain";
        if (!bot->SatisfyQuestPrevChain(quest, false)) return "previous chain";
        if (!bot->SatisfyQuestBreadcrumb(quest, false)) return "breadcrumb";
        if (!bot->CanAddQuest(quest, false)) return "cannot add (log, start item)";
        return "disabled or periodic";
    }
}

bool Animus::Curriculum::QuestEncounter::Retired(uint32 questId) const
{
    std::lock_guard<std::mutex> guard(_refusedLock);
    auto const found = _refusals.find(questId);
    return found != _refusals.end() && found->second >= REFUSALS_TO_RETIRE;
}

void Animus::Curriculum::QuestEncounter::Refused(uint32 questId)
{
    std::lock_guard<std::mutex> guard(_refusedLock);
    if (++_refusals[questId] == REFUSALS_TO_RETIRE)
        LOG_INFO("module.animus", "{}: quest {} retired from the draw after {} refusals", _scenario.Name(), questId,
            REFUSALS_TO_RETIRE);
}

void Animus::Curriculum::QuestEncounter::Tally(EnvQuests const& quests)
{
    // A drill's quest was taken for it and started beside its objective: not a turn-in rate of the kind.
    if (quests.Drill)
        return;
    if (quests.Evaluating)
        return;

    std::lock_guard<std::mutex> guard(_refusedLock);
    for (EnvQuest const& group : quests.Groups)
        // Each quest of the chain the group reached counts once for every kind among its objectives.
        for (uint32 index = 0; index < group.Chain.size() && index <= group.TurnedInCount; ++index)
        {
            QuestPlan const* plan = group.Chain[index]->Plan;
            if (!plan)
                continue;
            std::array<bool, size_t(ObjectiveKind::Count)> kinds{};
            for (PlannedObjective const& objective : plan->Objectives)
                kinds[size_t(objective.Kind)] = true;
            for (size_t kind = 0; kind < kinds.size(); ++kind)
                if (kinds[kind])
                {
                    ++_kindAttempts[kind];
                    _kindTurnedIn[kind] += index < group.TurnedInCount ? 1 : 0;
                }
        }
}

uint32 Animus::Curriculum::QuestEncounter::QuestWeight(LifeWorld::QuestCandidate const& candidate) const
{
    // 1 / (the turn-in rate of its hardest kind), the rate smoothed (a kind never drawn reads as one in two) and
    // floored at 5%, so a weight runs from 1 to 20; floored in turn at a quarter of the most, so what the seats
    // already do well still comes up one draw in a few.
    constexpr float MOST = 20.0f;
    float rate = 1.0f;
    if (candidate.Plan)
        for (PlannedObjective const& objective : candidate.Plan->Objectives)
        {
            size_t const kind = size_t(objective.Kind);
            rate = std::min(rate, (float(_kindTurnedIn[kind]) + 1.0f) / (float(_kindAttempts[kind]) + 2.0f));
        }
    float const weight = std::clamp(1.0f / std::max(rate, 0.05f), MOST * 0.25f, MOST);
    return uint32(weight * 100.0f);
}

bool Animus::Curriculum::QuestEncounter::Place(Env& env, EnvLife& life)
{
    EnvQuests& quests = _quests[env.Index];
    Tally(quests);
    quests = EnvQuests();
    quests.Evaluating = env.Evaluating;

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
    // Training weighs each quest by how rarely its hardest objective kind is turned in; evaluation draws them alike,
    // so its numbers stay comparable from one evaluation to the next.
    std::vector<uint32> questWeights;
    uint32 total = 0;
    if (!env.Evaluating)
    {
        std::lock_guard<std::mutex> guard(_refusedLock);
        questWeights.reserve(candidates->size());
        for (LifeWorld::QuestCandidate const* candidate : *candidates)
            total += questWeights.emplace_back(QuestWeight(*candidate));
    }
    auto const drawOne = [&](uint32 salt) -> LifeWorld::QuestCandidate const*
    {
        if (!total)
            return (*candidates)[LifeWorld::Draw(env, uint32(candidates->size()), salt)];
        uint32 roll = LifeWorld::Draw(env, total, salt);
        for (size_t index = 0; index < questWeights.size(); ++index)
        {
            if (roll < questWeights[index])
                return (*candidates)[index];
            roll -= questWeights[index];
        }
        return candidates->back();
    };
    LifeWorld::QuestCandidate const* first = nullptr;
    int8 const drill = _scenario.Arena(env).QuestDrill;
    Position drillStart;
    if (drill >= 0)
    {
        // A drill: a quest with an objective of the drill's kind, from a random point in the candidates, and a
        // place to start within reach of it.
        quests.Drill = true;
        uint32 const from = LifeWorld::Draw(env, uint32(candidates->size()), SALT_DRILL + life.Draws++);
        for (uint32 i = 0; i < candidates->size() && !first; ++i)
        {
            LifeWorld::QuestCandidate const* candidate = (*candidates)[(from + i) % candidates->size()];
            if (!Retired(candidate->Id)
                && DrillStart(*candidate, ObjectiveKind(drill), SALT_DRILL + life.Draws, env, drillStart))
                first = candidate;
        }
        // None of the kind in this band and side (explore quests are few): an ordinary quest instead, never a
        // failed episode -- one env that cannot set up ends the whole plan.
        quests.Drill = first != nullptr;
        if (first)
            quests.Groups[0].Chain = { first };
    }
    if (!quests.Drill)
    {
        for (uint32 attempt = 0; attempt < DRAW_ATTEMPTS && (!first || Retired(first->Id)); ++attempt)
            first = drawOne(SALT_QUEST + life.Draws++);
        quests.Groups[0].Chain = ChainFrom(first, length(SALT_CHAIN + life.Draws), env.Evaluating, life.Side);
    }

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
                if (candidate != first && candidate->Giver->Map == first->Giver->Map && !Retired(candidate->Id)
                    && candidate->Giver->Pos.GetExactDist2d(&first->Giver->Pos) <= SHARED_ZONE_YARDS)
                    near.push_back(candidate);
            if (!near.empty())
                other = near[LifeWorld::Draw(env, uint32(near.size()), SALT_QUEST + 31 * g + life.Draws)];
        }
        quests.Groups[g].Chain = quests.Drill ? std::vector<LifeWorld::QuestCandidate const*>{ first }
            : ChainFrom(other, length(SALT_CHAIN + 13 * g + life.Draws), env.Evaluating, life.Side);
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
    if (quests.Drill)
        data.EpisodeSpawn = drillStart;
    data.HasEpisodeSpawn = true;
    return true;
}

bool Animus::Curriculum::QuestEncounter::DrillStart(LifeWorld::QuestCandidate const& candidate, ObjectiveKind kind,
    uint32 salt, Env const& env, Position& start)
{
    for (uint32 i = 0; i < candidate.Plan->Objectives.size() && i < candidate.Places.size(); ++i)
    {
        if (candidate.Plan->Objectives[i].Kind != kind)
            continue;
        // Where a creature of the world stands a little way off: its spawn is on the ground, which the objective's
        // place (a quest_poi centroid) need not be.
        Position const& where = candidate.Places[i].Where;
        std::vector<LifeWorld::Spawn const*> spawns;
        LifeWorld::SpawnIndex::Instance().CreaturesNear(candidate.Giver->Map, where.GetPositionX(),
            where.GetPositionY(), DRILL_START_MAX, spawns);
        std::erase_if(spawns, [&where](LifeWorld::Spawn const* spawn)
        {
            return spawn->Object || spawn->Pos.GetExactDist2d(&where) < DRILL_START_MIN;
        });
        if (spawns.empty())
        {
            // Nothing to stand beside: on the place itself, except where reaching the place is the objective.
            if (kind == ObjectiveKind::Explore)
                continue;
            start = where;
            return true;
        }
        Position const& at = spawns[LifeWorld::Draw(env, uint32(spawns.size()), salt)]->Pos;
        start.Relocate(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(),
            std::atan2(where.GetPositionY() - at.GetPositionY(), where.GetPositionX() - at.GetPositionX()));
        return true;
    }
    return false;
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
            // Only a refusal every bot would meet retires the quest: one of this bot's class or race says nothing
            // about the next, and retiring it would drain class and race quests out of training.
            bool general = false;
            char const* const reason = leader && info ? RefusalReason(leader, info, general) : "no seat or quest";
            LOG_INFO("module.animus", "{}: env {} cannot take quest {} ({}); another next time", _scenario.Name(),
                env.Index, quest.Chain.front()->Id, reason);
            if (general)
                Refused(quest.Chain.front()->Id);
            return false;
        }

        // A drill starts with the quest taken.
        if (quests.Drill)
            for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
                if (Player* bot = _scenario.SeatBot(env, seat); bot && GroupOf(env, seat) == g
                    && bot->CanTakeQuest(info, false) && bot->CanAddQuest(info, false))
                    bot->AddQuestAndCheckCompletion(info, nullptr);

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
                // Objects to use or open: at least twice what the objective counts, so every count can be made
                // (a summoned object is gone once used). Copies stand a few yards round the ones placed.
                if (q->Plan->Objectives[i].SourcesAreObjects && placed)
                {
                    uint32 const wanted = std::min(tuning.ObjectiveSpawns, 2 * q->Plan->Objectives[i].Count);
                    std::vector<LifeWorld::Spawn const*> objects;
                    for (LifeWorld::Spawn const* spawn : place.Spawns)
                        if (spawn->Object)
                            objects.push_back(spawn);
                    for (uint32 copy = 0; placed < wanted && !objects.empty() && copy < wanted; ++copy)
                    {
                        LifeWorld::Spawn moved = *objects[copy % objects.size()];
                        float const angle = float(copy) * 2.39996f;     // the golden angle: spread, never stacked
                        float const yards = 3.0f + float(copy / objects.size()) * 2.0f;
                        float const x = moved.Pos.GetPositionX() + std::cos(angle) * yards;
                        float const y = moved.Pos.GetPositionY() + std::sin(angle) * yards;
                        float const ground = map->GetHeight(x, y, moved.Pos.GetPositionZ() + 2.0f);
                        moved.Pos.Relocate(x, y, ground > INVALID_HEIGHT ? ground : moved.Pos.GetPositionZ());
                        if (SummonObject(env, life, map, moved))
                            ++placed;
                    }
                }
            }
        }

        if (Creature* giver = map->GetCreature(quest.Givers.front()))
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
    // An episode that failed to build ends at the next decision: its quests were drawn but their givers and enders
    // may never have been placed, and the second fast pass's life stage crashed reading one that was not there.
    if (data.BuildFailed)
        return;

    for (uint32 g = 0; g < GroupCount(env); ++g)
    {
        EnvQuest& quest = quests.Groups[g];
        LifeWorld::QuestCandidate const* current = quest.Quest();
        if (!current || quest.Current >= quest.Givers.size() || quest.Current >= quest.Enders.size())
            continue;

        // Only seats standing on the env's map: a seat between maps (a resurrection, a teleport) is not in the world,
        // and reading anything through it crashed the first fast pass's life stage on a map thread.
        Map* map = env.FindMap();
        std::vector<Player*> seats;
        for (uint32 seat = 0; seat < data.ActiveSeats && map; ++seat)
            if (Player* bot = _scenario.SeatBot(env, seat); bot && bot->IsInWorld() && bot->GetMap() == map
                && GroupOf(env, seat) == g)
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
        if (accepted && !complete && !env.Evaluating && tuning.DropRerolls)
            GuaranteeDrops(env, life, quest, seats, map);
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
                if (!current || quest.Current >= quest.Givers.size() || quest.Current >= quest.Enders.size())
                    continue;
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
            for (ObjectGuid const& guid : life.Spawned)
            {
                WorldObject* thing = guid.IsGameObject() ? static_cast<WorldObject*>(map->GetGameObject(guid))
                    : static_cast<WorldObject*>(map->GetCreature(guid));
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
            if (Creature* giver = map->GetCreature(quest.Givers[quest.Current]))
                SetWaypoint(life, WAY_GIVER, giver->GetPosition(), g);
        }
        else if (!quest.Complete)
        {
            std::vector<Position> undone;
            bool creatureWanted = false;
            for (uint32 i = 0; i < current->Places.size() && i < current->Plan->Objectives.size(); ++i)
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
        else if (Creature* ender = map->GetCreature(quest.Enders[quest.Current]))
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
        // Per objective (Life.QuestCredit): the progress is the objectives' mean, so times their number.
        LifeWorld::QuestCandidate const* current = quest.Quest();
        float const objectives = current ? float(std::max<std::size_t>(1, current->Plan->Objectives.size())) : 1.0f;
        float const gained = quest.Progress - pay.ProgressPaid;
        ledger.Add(RewardTerm::QuestCredit, tuning.QuestCredit * objectives * gained * tierScale);
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
    // A finished quest carried about instead of handed in.
    if (quest.Accepted && quest.Complete && !quest.TurnedIn)
        ledger.Add(RewardTerm::Stall, -tuning.CompleteHeld * _scenario.DecisionScale());

    // The clock without the whole chain costs what a lost fight costs, less what was done -- once per seat.
    if (pay.TimeoutPaid || life.OutcomePaid || Finished(env, life) || !TimeIsUp(env) || quest.Chain.empty())
        return;
    pay.TimeoutPaid = true;
    float const done = (float(quest.TurnedInCount) + quest.Progress) / float(quest.Chain.size());
    ledger.Add(RewardTerm::Timeout, -tuning.QuestTimeout * (1.0f - done) / tierScale);
}

void Animus::Curriculum::QuestEncounter::GuaranteeDrops(Env const& env, EnvLife const& life, EnvQuest& quest,
    std::vector<Player*> const& seats, Map* map) const
{
    LifeWorld::QuestCandidate const* current = quest.Quest();
    uint32 const rerolls = _scenario.Tuning().Life.DropRerolls;
    if (!current || !map)
        return;

    for (ObjectGuid const& guid : life.Spawned)
    {
        if (!guid.IsCreatureOrVehicle())
            continue;
        Creature* creature = map->GetCreature(guid);
        if (!creature || creature->IsAlive()
            || std::find(quest.DropsChecked.begin(), quest.DropsChecked.end(), guid) != quest.DropsChecked.end())
            continue;
        // Checked once, at the first decision it lies dead: nobody has looted it yet (loot is taken in the seats'
        // actions, after this), so rolling it again takes nothing from anyone.
        quest.DropsChecked.push_back(guid);
        uint32 const lootId = creature->GetCreatureTemplate()->lootid;
        if (!lootId || creature->loot.isLooted())
            continue;

        for (PlannedObjective const& objective : current->Plan->Objectives)
        {
            if (objective.Kind != ObjectiveKind::CollectFromCreature
                || std::find(objective.Sources.begin(), objective.Sources.end(), creature->GetEntry())
                    == objective.Sources.end())
                continue;
            // A member who still needs it, to roll the loot for (its group shares the quest items).
            auto const needs = std::find_if(seats.begin(), seats.end(), [&objective](Player* bot)
            {
                return bot->IsAlive() && bot->HasQuestForItem(objective.Entry);
            });
            if (needs == seats.end())
                continue;
            auto const dropped = [creature, &objective]()
            {
                return std::any_of(creature->loot.quest_items.begin(), creature->loot.quest_items.end(),
                    [&objective](LootItem const& item) { return item.itemid == objective.Entry; });
            };
            for (uint32 roll = 0; roll < rerolls && !dropped(); ++roll)
            {
                // As the kill filled it (Unit::Kill): cleared, filled for the looter and its group, then the money.
                creature->loot.clear();
                creature->loot.FillLoot(lootId, LootTemplates_Creature, *needs, false, false, creature->GetLootMode(),
                    creature);
                if (creature->GetLootMode())
                    creature->loot.generateMoneyLoot(creature->GetCreatureTemplate()->mingold,
                        creature->GetCreatureTemplate()->maxgold);
            }
            if (!creature->loot.isLooted())
                creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            break;
        }
    }
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
