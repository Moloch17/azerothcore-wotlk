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
#include "CellImpl.h"
#include "CombatReward.h"
#include "Creature.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "WorldActions.h"
#include <algorithm>
#include <list>

namespace
{
    using namespace Animus::Curriculum;

    /// How far a hostile of the place is offered as a target before it has attacked: the pack block needs a slot
    /// to aim at to open a fight.
    constexpr float HOSTILE_REACH = 40.0f;
    /// Salts for the seeded draws, so the rung, the side and the place of one seed differ.
    constexpr uint32 SALT_SIDE = 11;
    constexpr uint32 SALT_LEVEL = 13;
}

Animus::Curriculum::LifeEncounter::LifeEncounter(StageScenario& scenario, uint32 envs, std::string what)
    : Encounter(scenario), _envs(envs), _ladder(scenario, what + " band"), _what(std::move(what))
{
    // The indexes are built at startup rather than in the first episode: reading the spawn tables is seconds.
    LifeWorld::SpawnIndex::Instance();
}

void Animus::Curriculum::LifeEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // The rung is `difficulty` -- the column the convergence rule's ladder signal reads -- unless another
    // encounter of the stage reports its own under that name (a creature duel, pulls, an instance: the crossroads).
    auto const reportsDifficulty = [](ArenaDefinition const& arena)
    {
        return arena.Against == Opposition::Creature || arena.Against == Opposition::Pulls
            || arena.Against == Opposition::Instance;
    };
    if (!_scenario.Stage().AnyArena(reportsDifficulty))
        table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add(_what + "_band", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add(_what + "_side", [this](Env const& env, uint32) { return float(uint8(_envs[env.Index].Side)); });
    table.Add(_what + "_won", [this](Env const& env, uint32) { return _envs[env.Index].Won ? 1.0f : 0.0f; });
    // Per seat: what it pressed and what it took.
    auto const seat = [this](Env const& env, uint32 index) -> SeatLife const&
    {
        return _envs[env.Index].Seats[std::min<uint32>(index, MAX_SEATS - 1)];
    };
    table.Add("interactions", [seat](Env const& env, uint32 i) { return float(seat(env, i).Interactions); });
    table.Add("wasted_presses", [seat](Env const& env, uint32 i) { return float(seat(env, i).Wasted); });
    table.Add("corpses_looted", [seat](Env const& env, uint32 i) { return float(seat(env, i).CorpsesLooted); });
    table.Add("items_looted", [seat](Env const& env, uint32 i) { return float(seat(env, i).ItemsLooted); });
    table.Add("copper_looted", [seat](Env const& env, uint32 i) { return float(seat(env, i).CopperLooted); });
    table.Add("spawned", [this](Env const& env, uint32) { return float(_envs[env.Index].Spawned.size()); });
    table.Add("progressed", [seat](Env const& env, uint32 i) { return seat(env, i).Progressed; });
    AddMoreEpisodeInfo(table);
}

void Animus::Curriculum::LifeEncounter::ResetEpisode(Env& env)
{
    // The spawns go with the rebuild (BeforeRebuild); the rest starts over.
    std::vector<ObjectGuid> spawned = std::move(_envs[env.Index].Spawned);
    std::array<Group*, LIFE_GROUPS> groups = _envs[env.Index].Groups;
    _envs[env.Index] = EnvLife();
    _envs[env.Index].Spawned = std::move(spawned);
    _envs[env.Index].Groups = groups;
}

void Animus::Curriculum::LifeEncounter::BeforeRebuild(Env& env)
{
    Disband(env);
    ForgetSpawns(env);
}

void Animus::Curriculum::LifeEncounter::Teardown(Env& env)
{
    Disband(env);
    ForgetSpawns(env);
}

void Animus::Curriculum::LifeEncounter::ForgetSpawns(Env& env)
{
    // Out of the enemy slots first: whoever despawns what is left in them must not despawn these a second time.
    std::vector<ObjectGuid>& spawned = _envs[env.Index].Spawned;
    std::erase_if(env.Targets, [&spawned](ObjectGuid const& guid)
    {
        return std::find(spawned.begin(), spawned.end(), guid) != spawned.end();
    });
    LifeWorld::Despawn(env.FindMap(), spawned);
}

void Animus::Curriculum::LifeEncounter::FoundPlaces::Seen(Position const& where, uint32 nowMs, float merge)
{
    for (Found& found : Places)
        if (found.Where.GetExactDist2d(&where) <= merge)
        {
            found.SeenMs = nowMs;
            return;
        }
    if (Places.size() >= WorldView::JOURNAL_PLACES)
        Places.erase(std::min_element(Places.begin(), Places.end(),
            [](Found const& a, Found const& b) { return a.SeenMs < b.SeenMs; }));
    Places.push_back({ where, nowMs });
}

void Animus::Curriculum::LifeEncounter::FoundPlaces::Forget(Position const& where, float within)
{
    std::erase_if(Places, [&](Found const& found) { return found.Where.GetExactDist2d(&where) <= within; });
}

void Animus::Curriculum::LifeEncounter::FoundPlaces::Write(WorldView& world, uint32 nowMs) const
{
    for (uint32 i = 0; i < Places.size() && i < WorldView::JOURNAL_PLACES; ++i)
    {
        world.Places[i].Present = true;
        world.Places[i].Where = Places[i].Where;
        world.Places[i].AgeSeconds = float(nowMs - std::min(nowMs, Places[i].SeenMs)) / 1000.0f;
    }
}

uint32 Animus::Curriculum::LifeEncounter::GroupOf(Env const& env, uint32 seat) const
{
    if (_scenario.Arena(env).Seats != SeatPlan::Teams)
        return 0;
    // A seat questing alone is a group of its own, after the sides.
    if (_scenario.IsLoneSeat(env, seat))
        return std::min<uint32>(TEAM_COUNT + seat - std::min(_scenario.Arena(env).TeamSeats, TEAM_SEATS) * TEAM_COUNT,
            LIFE_GROUPS - 1);
    return _scenario.SideOf(env, seat);
}

void Animus::Curriculum::LifeEncounter::FormGroups(Env& env)
{
    EnvLife& life = _envs[env.Index];
    EnvState const& data = _scenario.Data(env);
    if (data.ActiveSeats < 2)
        return;
    Player* lead = _scenario.SeatBot(env, 0);
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && lead && _scenario.IsLoneSeat(env, seat))
            bot->SetFaction(lead->GetFaction());    // alone, but on the same side as everyone else in the world
    for (uint32 group = 0; group < TEAM_COUNT; ++group)
    {
        if (life.Groups[group])
            continue;
        Player* leader = nullptr;
        for (uint32 seat = 0; seat < data.ActiveSeats && !leader; ++seat)
            if (GroupOf(env, seat) == group)
                leader = _scenario.SeatBot(env, seat);
        if (!leader || leader->GetGroup())
            continue;       // already grouped (a party's PartyEncounter did it)
        Group* made = new Group();
        made->SetSimGroup(true);
        if (!made->Create(leader))
        {
            delete made;
            continue;
        }
        sGroupMgr->AddGroup(made);
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            if (Player* bot = _scenario.SeatBot(env, seat); bot && bot != leader && GroupOf(env, seat) == group
                && !bot->GetGroup())
            {
                // Everyone in the world is on the leader's side (friends, not the flag match's two teams).
                if (lead)
                    bot->SetFaction(lead->GetFaction());
                made->AddMember(bot);
            }
        life.Groups[group] = made;
    }
}

void Animus::Curriculum::LifeEncounter::Disband(Env& env)
{
    for (Group*& group : _envs[env.Index].Groups)
    {
        if (group)
            group->Disband(true);   // removes it from the group manager and deletes it
        group = nullptr;
    }
}

void Animus::Curriculum::LifeEncounter::BeforeLevel(Env& env)
{
    // The rung is the band: seat 0's class and build climb it as they win; an evaluation spreads its seeds over
    // the three. The band fixes the level (a draw within it), and the side is drawn too, since a quest and a town
    // belong to one; the race draw honours it (EnvState::EpisodeTeam).
    EnvState& data = _scenario.Data(env);
    EnvLife& life = _envs[env.Index];
    SeatState const& seat = data.Seats[0];
    life.Layout = seat.L ? seat.L->Index : 0;
    life.Spec = seat.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, life.Layout, life.Spec, LifeWorld::BAND_COUNT - 1);
    life.Tier = std::min<uint32>(pick.Tier, LifeWorld::BAND_COUNT - 1);
    life.Counts = pick.Counts;
    life.Side = LifeWorld::Draw(env, 2, SALT_SIDE) ? LifeWorld::Side::Horde : LifeWorld::Side::Alliance;

    LifeWorld::Band const& band = LifeWorld::BandAt(life.Tier);
    data.EpisodeLevel = uint8(band.Min + LifeWorld::Draw(env, band.Max - band.Min + 1, SALT_LEVEL));
    data.EpisodeTeam = uint8(life.Side == LifeWorld::Side::Horde ? TEAM_HORDE : TEAM_ALLIANCE) + 1;

    if (!Place(env, life))
    {
        LOG_ERROR("module.animus", "{}: env {} has nowhere to go for band {} ({})", _scenario.Name(), env.Index,
            life.Tier, LifeWorld::SideName(life.Side));
        data.EpisodeLevel = 0;
        data.EpisodeTeam = 0;
    }
}

Animus::Curriculum::LifeWorld::Side Animus::Curriculum::LifeEncounter::SideOfSeat(Env const& env) const
{
    Player const* bot = _scenario.SeatBot(env, 0);
    return bot ? LifeWorld::SideOf(bot->GetTeamId()) : _envs[env.Index].Side;
}

void Animus::Curriculum::LifeEncounter::SetWaypoint(EnvLife& life, uint8 kind, Position const& where, uint32 group)
{
    Waypoint& way = life.Ways[std::min<uint32>(group, LIFE_GROUPS - 1)];
    if (!way.Has || way.Kind != kind)
        for (SeatLife& seat : life.Seats)
            seat.LastDistance = -1.0f;
    way.Has = true;
    way.Kind = kind;
    way.Where = where;
}

void Animus::Curriculum::LifeEncounter::ClearWaypoint(EnvLife& life, uint32 group)
{
    life.Ways[std::min<uint32>(group, LIFE_GROUPS - 1)].Has = false;
    for (SeatLife& seat : life.Seats)
        seat.LastDistance = -1.0f;
}

Creature* Animus::Curriculum::LifeEncounter::Summon(Env& env, EnvLife& life, Map* map,
    LifeWorld::Spawn const& spawn)
{
    Creature* creature = LifeWorld::Summon(map, StageScenario::EnvPhase(env), spawn);
    if (creature)
        life.Spawned.push_back(creature->GetGUID());
    return creature;
}

GameObject* Animus::Curriculum::LifeEncounter::SummonObject(Env& env, EnvLife& life, Map* map,
    LifeWorld::Spawn const& spawn)
{
    GameObject* object = LifeWorld::SummonObject(map, StageScenario::EnvPhase(env), spawn);
    if (object)
        life.Spawned.push_back(object->GetGUID());
    return object;
}

uint32 Animus::Curriculum::LifeEncounter::SummonAround(Env& env, EnvLife& life, Map* map, Position const& where,
    float radius, uint32 cap, bool npcs)
{
    std::vector<LifeWorld::Spawn const*> spawns;
    LifeWorld::SpawnIndex::Instance().CreaturesNear(map->GetId(), where.GetPositionX(), where.GetPositionY(),
        radius, spawns);
    uint32 count = 0;
    for (LifeWorld::Spawn const* spawn : spawns)
    {
        if (count >= cap)
            break;
        if (!LifeWorld::IsWorldCreature(*spawn, npcs))
            continue;
        if (Summon(env, life, map, *spawn))
            ++count;
    }
    return count;
}

float Animus::Curriculum::LifeEncounter::TierScale(Env const& env) const
{
    return CombatReward::TierScale(_scenario.Tuning().Difficulty.TierScale, _envs[env.Index].Tier);
}

bool Animus::Curriculum::LifeEncounter::TimeIsUp(Env const& env) const
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

void Animus::Curriculum::LifeEncounter::UpdateEnemies(Env& env)
{
    // Whatever is fighting the seats, nearest first, then the nearest hostile within reach that is not yet: the
    // pack block's slots, so a fight can be opened and finished with the combat blocks as they were trained. With
    // several seats, around any of them, and nearest to the nearest of them.
    env.Targets.clear();
    std::vector<Player*> seats;
    for (uint32 index = 0; index < _scenario.Data(env).ActiveSeats; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsAlive() && bot->IsInWorld())
            seats.push_back(bot);
    if (seats.empty())
        return;

    std::list<Unit*> units;
    for (Player* seat : seats)
    {
        std::list<Unit*> near;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(seat, seat, HOSTILE_REACH);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(seat, near, check);
        Cell::VisitObjects(seat, searcher, HOSTILE_REACH);
        for (Unit* unit : near)
            // A player among them is a hostile one ganking the seats (a quest's ambushers): the seats are on one
            // side, so none of them is a valid target of another.
            if (unit->IsAlive() && seat->IsValidAttackTarget(unit)
                && std::find(units.begin(), units.end(), unit) == units.end())
                units.push_back(unit);
    }
    auto const nearest = [&seats](Unit const* unit)
    {
        float best = 1e9f;
        for (Player const* seat : seats)
            best = std::min(best, seat->GetDistance(unit));
        return best;
    };
    units.sort([&nearest](Unit* a, Unit* b)
    {
        bool const fightingA = a->IsInCombat();
        bool const fightingB = b->IsInCombat();
        if (fightingA != fightingB)
            return fightingA;
        return nearest(a) < nearest(b);
    });
    for (Unit* unit : units)
    {
        if (env.Targets.size() >= PACK_SLOTS)
            break;
        env.Targets.push_back(unit->GetGUID());
    }
}

void Animus::Curriculum::LifeEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvLife const& life = _envs[env.Index];
    if (!view.Bot)
        return;

    WorldActions::Sense(view.Bot, _scenario.Tuning().Life.SenseRange, view.World);
    Sensed(env, life, seat, view);
    // The group's waypoint is the travel block's objective: the same features the trips were learned on.
    if (Waypoint const& way = life.Ways[GroupOf(env, seat)]; way.Has)
    {
        view.HasObjective = true;
        view.Objective = way.Where;
    }
}

void Animus::Curriculum::LifeEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    if (seat >= MAX_SEATS)
        return;
    EnvLife& life = _envs[env.Index];
    SeatLife& mine = life.Seats[seat];
    mine.Interactions += result.Interactions;
    mine.Wasted += result.Wasted;
    mine.CorpsesLooted += result.CorpsesLooted;
    mine.ItemsLooted += result.ItemsLooted;
    mine.CopperLooted += result.CopperLooted;
    mine.GatherCasts += result.GatherCasts;
    Account(env, life, seat, result);
}

void Animus::Curriculum::LifeEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::LifeTuning const& tuning = _scenario.Tuning().Life;
    EnvLife& life = _envs[env.Index];
    if (seatIndex >= MAX_SEATS)
        return;
    SeatLife& mine = life.Seats[seatIndex];

    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    if (bot && !bot->IsAlive() && !mine.Died)
        mine.Died = true;
    if (mine.Died && !mine.DeathPaid)
    {
        mine.DeathPaid = true;
        ledger.Add(RewardTerm::Death, -tuning.Death / TierScale(env));
    }

    // Potential-based shaping on the straight distance to the group's waypoint: what is closed pays, what is
    // given back costs. Spread over the trip's length (at least 100 yd) so a whole approach pays Life.Progress
    // once, and never paid across a change of waypoint (SetWaypoint resets the potential).
    if (Waypoint const& way = life.Ways[GroupOf(env, seatIndex)]; bot && bot->IsAlive() && way.Has)
    {
        float const distance = bot->GetExactDist2d(&way.Where);
        if (mine.LastDistance >= 0.0f)
        {
            float const trip = std::max(100.0f, mine.LastDistance);
            float const closed = mine.LastDistance - distance;
            ledger.Add(RewardTerm::Progress, tuning.Progress * closed / trip);
            mine.Progressed += closed;
        }
        mine.LastDistance = distance;
    }

    // Each wasted press (an interact with nothing to interact with, a buy that bought nothing), once.
    if (mine.Wasted > mine.WastedPaid)
    {
        ledger.Add(RewardTerm::Wasted, -tuning.Wasted * float(mine.Wasted - mine.WastedPaid));
        mine.WastedPaid = mine.Wasted;
    }

    RewardMore(env, life, seatIndex, bot, ledger);

    // The episode's outcome, once, on the last seat's reward of the decision it ends in.
    if (seatIndex + 1 < _scenario.Data(env).ActiveSeats)
        return;
    bool const finished = Finished(env, life);
    bool const over = finished || AllDead(env) || TimeIsUp(env);
    if (!over || life.OutcomePaid)
        return;
    life.OutcomePaid = true;
    life.Won = finished;
    if (!life.Recorded)
    {
        life.Recorded = true;
        if (life.Counts)
            _ladder.Record(life.Layout, life.Spec, life.Tier, finished, LifeWorld::BAND_COUNT - 1);
    }
}

void Animus::Curriculum::LifeEncounter::WriteState(Env const& env, float* state) const
{
    state[StageScenario::STATE_TIER] = float(_envs[env.Index].Tier) / float(LifeWorld::BAND_COUNT - 1);
}

bool Animus::Curriculum::LifeEncounter::IsTerminal(Env const& env) const
{
    EnvLife const& life = _envs[env.Index];
    return AllDead(env) || life.Done || Finished(env, life) || TimeIsUp(env);
}

bool Animus::Curriculum::LifeEncounter::AllDead(Env const& env) const
{
    EnvLife const& life = _envs[env.Index];
    uint32 const seats = std::max<uint32>(1, _scenario.Data(env).ActiveSeats);
    for (uint32 seat = 0; seat < seats && seat < MAX_SEATS; ++seat)
        if (!life.Seats[seat].Died)
            return false;
    return true;
}
