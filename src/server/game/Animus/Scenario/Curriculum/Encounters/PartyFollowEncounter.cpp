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

#include "PartyFollowEncounter.h"
#include "CreatureData.h"
#include "EncoderSupport.h"
#include "Encounters.h"
#include "EpisodeInfoTable.h"
#include "FollowEncounter.h"
#include "InstanceBosses.h"
#include "Log.h"
#include "Map.h"
#include "MarkerEncounter.h"
#include "ObjectMgr.h"
#include "ObjectPool.h"
#include "PartyFramesBlock.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "Seek.h"
#include "SightDraw.h"
#include "StageScenario.h"
#include "StageState.h"
#include <algorithm>
#include <cmath>

namespace
{
    /// The leader has reached a stop, or a corner of its way, this close.
    constexpr float LEADER_ARRIVE = 3.0f;
    /// A route re-planned when the leader strays this far from the corner it walks to.
    constexpr float LEADER_STRAY = 15.0f;
    /// The party gathers at the door this long before the leader sets off (not a regroup: it starts together).
    constexpr uint32 START_PAUSE_MS = 3000;
    /// A fall of the leader's this deep counts as a drop it took.
    constexpr float DROP_YARDS = 4.0f;
    /// A scripted leader that died stands up where it fell after this long.
    constexpr uint32 LEADER_RISE_MS = 2000;
}

Animus::Curriculum::PartyFollowEncounter::PartyFollowEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

bool Animus::Curriculum::PartyFollowEncounter::InTheWay(float leaderX, float leaderY, float leaderYaw,
    bool leaderMoving, float x, float y, float yards, float halfAngle)
{
    if (!leaderMoving)
        return false;
    float const dx = x - leaderX;
    float const dy = y - leaderY;
    float const distance = std::sqrt(dx * dx + dy * dy);
    if (distance > yards)
        return false;
    // Right on top of it is in its way whichever way it faces.
    if (distance < 0.25f)
        return true;
    float const angle = std::atan2(dy, dx) - leaderYaw;
    float const off = std::fabs(std::atan2(std::sin(angle), std::cos(angle)));
    return off <= halfAngle * float(M_PI) / 180.0f;
}

float Animus::Curriculum::PartyFollowEncounter::RegroupShare(float seconds, float window)
{
    if (window <= 0.0f)
        return seconds <= 0.0f ? 1.0f : 0.0f;
    return std::clamp(1.0f - seconds / window, 0.0f, 1.0f);
}

float Animus::Curriculum::PartyFollowEncounter::StopSeconds(uint32 rung, float first, float last)
{
    float const t = float(std::min(rung, RUNGS - 1)) / float(RUNGS - 1);
    return first + (last - first) * t;
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PartyFollowEncounter::RewardTerms() const
{
    return { RewardTerm::FollowKept, RewardTerm::Regroup, RewardTerm::Lost, RewardTerm::Blocking, RewardTerm::Death,
        RewardTerm::Stuck, RewardTerm::Wall };
}

void Animus::Curriculum::PartyFollowEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    auto const seatOf = [this](Env const& env, uint32 seat) -> SeatFollow const*
    {
        return seat < GROUP_SEATS ? &_envs[env.Index].Seats[seat] : nullptr;
    };

    // The stage's measure: the share of the episode each follower spent within the band of the leader.
    table.Add("follow_kept_share", [this, seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow && env.EpisodeElapsedMs ? float(follow->InBandMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    table.Add("follow_distance_mean", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow && follow->Samples ? float(follow->DistanceSum / follow->Samples) : 0.0f;
    });
    table.Add("lost_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->LostMs) / 1000.0f : 0.0f;
    });
    table.Add("blocking_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->BlockingMs) / 1000.0f : 0.0f;
    });
    // The regroups: the leader's stops the follower was counted for, those it came back into the band within the
    // window of, their share (per event over regroup_stops) and how long they took (per event over regroups).
    table.Add("regroup_stops", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->RegroupStops) : 0.0f;
    });
    table.Add("regroups", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->Regroups) : 0.0f;
    });
    table.Add("regroup_share", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow && follow->RegroupStops ? float(follow->Regroups) / float(follow->RegroupStops) : 0.0f;
    });
    table.Add("regroup_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow && follow->Regroups ? float(follow->RegroupMsTotal) / float(follow->Regroups) / 1000.0f : 0.0f;
    });
    // Death in the instance (I4): deaths, rises at the entrance, rejoins, the time from rising to rejoining (per
    // event over rejoins), the share of rises that rejoined (per event over rises) and the time spent dead.
    table.Add("deaths", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->Clock.Deaths) : 0.0f;
    });
    table.Add("rises", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->Clock.Rises) : 0.0f;
    });
    table.Add("rejoins", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->Clock.Rejoins) : 0.0f;
    });
    table.Add("rejoin_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? follow->Clock.RejoinSeconds() : 0.0f;
    });
    table.Add("rejoined", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow && follow->Clock.Rises ? follow->Clock.RejoinedShare() : 0.0f;
    });
    table.Add("dead_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->Clock.OutMsTotal) / 1000.0f : 0.0f;
    });
    // How the leader went: stops reached of the route's (and as a share), legs given up, sudden stops, drops taken,
    // stood up after dying, cast or scripted.
    table.Add("leader_stops_reached", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].StopsReached);
    });
    table.Add("leader_route_share", [this](Env const& env, uint32)
    {
        EnvParty const& party = _envs[env.Index];
        return party.Stops.empty() ? 0.0f : float(party.StopsReached) / float(party.Stops.size());
    });
    table.Add("leader_skips", [this](Env const& env, uint32) { return float(_envs[env.Index].Skips); });
    table.Add("leader_sudden_stops", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].SuddenStops);
    });
    table.Add("leader_drops", [this](Env const& env, uint32) { return float(_envs[env.Index].Drops); });
    table.Add("leader_rises", [this](Env const& env, uint32) { return float(_envs[env.Index].LeaderRises); });
    table.Add("leader_cast", [this](Env const& env, uint32) { return _envs[env.Index].Cast ? 1.0f : 0.0f; });
    // The ladder: the rung (`difficulty`, which the evaluation tables split by) and whether it is the top.
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Rung + 1 >= RUNGS ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::PartyFollowEncounter::ResetEpisode(Env& env)
{
    // The leader's character stays until Build replaces it (Deactivate and Teardown send it away).
    EnvParty& party = _envs[env.Index];
    bool const built = party.Built;
    party = EnvParty();
    party.Built = built;
}

void Animus::Curriculum::PartyFollowEncounter::BeforeLevel(Env& env)
{
    // The dungeon's level band (the dungeon finder's, as a whole dungeon's run was), and its front door: where a party
    // that walked in stands, and where a seat that died stands up again.
    EnvState& data = _scenario.Data(env);
    uint32 const mapId = _scenario.Arena(env).MapId;
    for (BossRow const& row : InstanceLadderRows(InstanceLadder::Dungeon))
        if (row.MapId == mapId)
        {
            auto const [low, high] = InstanceEncounter::DungeonLevels(row);
            data.EpisodeLevel = uint8(std::min<uint32>(urand(low, std::max(low, high)), DEFAULT_MAX_LEVEL));
            break;
        }
    data.DungeonDifficulty = 0;
    data.RaidDifficulty = 0;
    // One faction's races for the whole party, the leader too (BuildSeat honours EpisodeTeam): a party of both would
    // read each other as hostile players in the camera and the entities block.
    data.EpisodeTeam = uint8(urand(TEAM_ALLIANCE, TEAM_HORDE)) + 1;
    if (AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId))
    {
        data.EpisodeSpawn.Relocate(entrance->target_X, entrance->target_Y, entrance->target_Z,
            entrance->target_Orientation);
        data.HasEpisodeSpawn = true;
    }
}

bool Animus::Curriculum::PartyFollowEncounter::Build(Env& env, Map* map, uint8 level)
{
    EnvParty& party = _envs[env.Index];
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    Player* first = _scenario.SeatBot(env, 0);
    if (!first || !map)
        return false;

    // Last episode's leader goes; this one's is built fresh in the owner's slot.
    if (party.Built)
        _scenario.ReleaseOwnerSeat(env);
    party = EnvParty();

    // The dungeon's own game objects -- its doors, levers, chests -- out of the way: the follow is movement alone, and
    // a closed door (M3's lesson) would stop the leader's script at it.
    ObjectPool::ClearOwn(map);

    // The ladder's rung, the fade's (an evaluation plays the training rung, as the seek stage's does).
    party.Rung = std::min<uint32>(SightDraw::Rung(_scenario.ShapingScale()), RUNGS - 1);
    party.Cast = !env.Evaluating && tuning.CastShare > 0 && roll_chance_i(tuning.CastShare);
    party.Entrance = _scenario.SpawnPointFor(env);

    // The route: the door, then each boss's place in the dungeon's order. The bosses themselves were cleared with the
    // rest of the instance; their places are the dungeon's way through.
    uint32 const mapId = map->GetId();
    party.Stops = RouteStops(mapId);
    if (party.Stops.empty())
    {
        LOG_ERROR("module.animus", "{}: map {} has no boss places for the party follow's route", _scenario.Name(),
            mapId);
        return false;
    }

    Map* leaderMap = map;
    Player* leader = _scenario.BuildOwnerSeat(env, leaderMap, level, party.Entrance, AptitudeDemand::Anything());
    if (!leader)
        return false;
    party.Built = true;
    leader->SetFaction(first->GetFaction());
    env.Allies = { leader->GetGUID() };

    party.Mode = Phase::Stopped;
    party.StopUntilMs = env.EpisodeElapsedMs + START_PAUSE_MS;
    party.LegStuckMs = _scenario.Data(env).Seats[_scenario.OwnerAgent()].StuckMs;
    ScheduleSudden(env, party);
    return true;
}

std::vector<Position> Animus::Curriculum::PartyFollowEncounter::RouteStops(uint32 mapId)
{
    // The spawn table is fixed after startup, and finding a boss's spawn walks all of it: once per map.
    std::lock_guard<std::mutex> lock(_routesLock);
    auto const known = _routes.find(mapId);
    if (known != _routes.end())
        return known->second;
    std::vector<Position>& stops = _routes[mapId];
    for (BossRow const& row : InstanceLadderRows(InstanceLadder::Dungeon))
        if (row.MapId == mapId)
            if (CreatureData const* spawn = InstanceEncounter::FindSpawn(row))
                stops.emplace_back(spawn->posX, spawn->posY, spawn->posZ, spawn->orientation);
    return stops;
}

Player* Animus::Curriculum::PartyFollowEncounter::Leader(Env const& env) const
{
    return _envs[env.Index].Built ? _scenario.SeatBot(env, _scenario.OwnerAgent()) : nullptr;
}

void Animus::Curriculum::PartyFollowEncounter::ScheduleSudden(Env const& env, EnvParty& party) const
{
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    if (party.Rung < tuning.SuddenFromRung)
    {
        party.NextSuddenMs = 0;
        return;
    }
    // The top rung stops twice as often.
    float const scale = party.Rung >= tuning.BackStepFromRung ? 0.5f : 1.0f;
    float const gap = frand(tuning.SuddenGapMin, std::max(tuning.SuddenGapMin, tuning.SuddenGapMax)) * scale;
    party.NextSuddenMs = env.EpisodeElapsedMs + std::max<uint32>(1000, uint32(gap * 1000.0f));
}

void Animus::Curriculum::PartyFollowEncounter::Stop(Env const& env, EnvParty& party, uint32 ms, bool counts) const
{
    party.Mode = Phase::Stopped;
    party.StopStartMs = env.EpisodeElapsedMs;
    party.StopUntilMs = env.EpisodeElapsedMs + ms;
    party.Way.Clear();
    if (!counts || ms < _scenario.Tuning().PartyFollow.RegroupMinStopMs)
        return;
    ++party.RegroupStops;
    EnvState const& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < GROUP_SEATS; ++seat)
    {
        SeatFollow& follow = party.Seats[seat];
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsAlive() || follow.Clock.Out)
            continue;
        follow.RegroupPending = true;
        ++follow.RegroupStops;
    }
}

void Animus::Curriculum::PartyFollowEncounter::Steer(Env& env, EnvParty& party, Player* leader)
{
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    SeatState& seat = _scenario.Data(env).Seats[_scenario.OwnerAgent()];
    uint32 const now = env.EpisodeElapsedMs;

    // Where it stands in its plan: a stop over, a sudden stop due, a stop reached, a leg given up.
    if (party.Mode == Phase::Stopped && now >= party.StopUntilMs)
    {
        party.Mode = party.NextStop < party.Stops.size() ? Phase::Walking : Phase::Done;
        party.LegStuckMs = seat.StuckMs;
    }
    if (party.Mode == Phase::Walking && party.NextSuddenMs && now >= party.NextSuddenMs)
    {
        ++party.SuddenStops;
        uint32 const ms = urand(tuning.SuddenStopMinMs, std::max(tuning.SuddenStopMinMs, tuning.SuddenStopMaxMs));
        if (party.Rung >= tuning.BackStepFromRung && roll_chance_i(tuning.BackStepChance))
        {
            // A few yards back the way it came, then the stop.
            float const yaw = leader->GetOrientation() + float(M_PI);
            party.BackTo.Relocate(leader->GetPositionX() + std::cos(yaw) * tuning.BackStepYards,
                leader->GetPositionY() + std::sin(yaw) * tuning.BackStepYards, leader->GetPositionZ());
            party.Mode = Phase::BackStep;
            party.BackStopMs = ms;
            party.LegStuckMs = seat.StuckMs;
        }
        else
            Stop(env, party, ms, true);
        ScheduleSudden(env, party);
    }
    if (party.Mode == Phase::BackStep && (leader->GetExactDist2d(&party.BackTo) <= 1.0f
        || seat.StuckMs - std::min(seat.StuckMs, party.LegStuckMs) >= tuning.GiveUpMs))
        Stop(env, party, party.BackStopMs, true);
    if (party.Mode == Phase::Walking)
    {
        Position const& stop = party.Stops[party.NextStop];
        bool const arrived = leader->GetExactDist2d(&stop) <= LEADER_ARRIVE;
        bool const stuck = seat.StuckMs - std::min(seat.StuckMs, party.LegStuckMs) >= tuning.GiveUpMs;
        if (arrived || stuck)
        {
            ++party.NextStop;
            party.LegStuckMs = seat.StuckMs;
            party.Way.Clear();
            if (arrived)
            {
                ++party.StopsReached;
                Stop(env, party, uint32(StopSeconds(party.Rung, tuning.StopSecondsFirst, tuning.StopSecondsLast)
                    * 1000.0f), true);
            }
            else
                ++party.Skips;
            if (party.NextStop >= party.Stops.size() && party.Mode == Phase::Walking)
                party.Mode = Phase::Done;
        }
    }

    // The keys: none while standing, the seek helper's toward the next corner of the way otherwise. Not before the
    // controller has started this episode (a bearing from a default body would be from nowhere).
    Movement::ControlState held;
    if ((party.Mode == Phase::Walking || party.Mode == Phase::BackStep) && seat.Mover.Started())
    {
        Position const target = party.Mode == Phase::BackStep ? party.BackTo : party.Stops[party.NextStop];
        float x = target.GetPositionX();
        float y = target.GetPositionY();
        if (party.Mode == Phase::Walking)
        {
            if (!party.Way.Valid || party.Way.To.GetExactDist2d(&target) > 1.0f)
            {
                Position const from(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), 0.0f);
                RoutePlanner::Instance().Plan(leader->GetMap(), from, target, party.Way);
            }
            if (party.Way.Valid)
            {
                party.Way.Advance(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                    LEADER_ARRIVE);
                if (party.Way.Next < party.Way.Count)
                {
                    x = party.Way.X[party.Way.Next];
                    y = party.Way.Y[party.Way.Next];
                    if (leader->GetExactDist2d(x, y) > LEADER_STRAY)
                        party.Way.Valid = false;    // strayed: planned again next decision
                }
            }
        }
        held = Movement::Seek(seat.Mover.Body, x, y);
        held.Walk = party.Rung < tuning.WalkRungs || party.Mode == Phase::BackStep;
    }
    held.FaceTurnApplied = seat.Controls.Held.FaceTurnApplied;
    seat.Controls.Held = held;
}

void Animus::Curriculum::PartyFollowEncounter::RespawnFollowers(Env& env, EnvParty& party)
{
    CurriculumTuning::RespawnTuning const& tuning = _scenario.Tuning().Respawn;
    EnvState& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < GROUP_SEATS; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsInWorld())
            continue;
        SeatFollow& follow = party.Seats[seat];
        RespawnClock::Step const step = follow.Clock.Note(env.EpisodeElapsedMs, bot->IsAlive(),
            PartyYards(env, seat, bot), tuning.DelayMs, tuning.RejoinYards);
        if (step == RespawnClock::Step::Died)
            follow.RegroupPending = false;
        else if (step == RespawnClock::Step::Rise)
        {
            RiseAtEntrance(bot, data.Seats[seat], party.Entrance, env.EpisodeElapsedMs);
            follow.Clock.Risen(env.EpisodeElapsedMs);
            follow.HasLastPos = false;
        }
    }
}

void Animus::Curriculum::PartyFollowEncounter::Update(Env& env)
{
    EnvParty& party = _envs[env.Index];
    RespawnFollowers(env, party);

    Player* leader = Leader(env);
    if (!leader || !leader->IsInWorld())
        return;
    uint32 const agent = _scenario.OwnerAgent();
    SeatState& seat = _scenario.Data(env).Seats[agent];

    // The leader is the script's, not a seat: a fall that killed it (Ragefire's cavern) stands it up where it fell,
    // and it is never left hurt.
    if (!leader->IsAlive())
    {
        if (!seat.Combat.DeathMs)
            seat.Combat.DeathMs = std::max<uint32>(1, env.EpisodeElapsedMs);
        if (env.EpisodeElapsedMs >= seat.Combat.DeathMs + LEADER_RISE_MS)
        {
            Position const here(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                leader->GetOrientation());
            RiseAtEntrance(leader, seat, here, env.EpisodeElapsedMs);
            seat.Combat.DeathMs = 0;
            ++party.LeaderRises;
        }
        return;
    }
    if (leader->GetHealth() < leader->GetMaxHealth())
        leader->SetFullHealth();

    // Drops the leader took: a fall of DROP_YARDS or more, counted once when it lands.
    float const z = leader->GetPositionZ();
    if (party.HasLeaderZ && z < party.LastLeaderZ - 0.5f)
    {
        if (!party.Falling)
            party.FallFromZ = party.LastLeaderZ;
        party.Falling = true;
    }
    else if (party.Falling)
    {
        if (party.FallFromZ - z >= DROP_YARDS)
            ++party.Drops;
        party.Falling = false;
    }
    party.LastLeaderZ = z;
    party.HasLeaderZ = true;

    // A cast leader plays its own row (its objective is the next stop, View); the script's leader holds its keys.
    if (party.Cast)
    {
        if (party.NextStop < party.Stops.size()
            && leader->GetExactDist2d(&party.Stops[party.NextStop]) <= LEADER_ARRIVE)
        {
            ++party.NextStop;
            ++party.StopsReached;
        }
        return;
    }
    Steer(env, party, leader);
}

bool Animus::Curriculum::PartyFollowEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight.
    target = nullptr;
    return true;
}

float Animus::Curriculum::PartyFollowEncounter::PartyYards(Env const& env, uint32 seat, Player const* bot) const
{
    if (!bot)
        return -1.0f;
    Player const* leader = Leader(env);
    if (leader && leader->IsInWorld() && leader->IsAlive() && leader->GetMapId() == bot->GetMapId())
        return bot->GetExactDist(leader);

    // The leader down: the living others' middle.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 count = 0;
    EnvState const& data = _scenario.Data(env);
    for (uint32 other = 0; other < data.ActiveSeats; ++other)
    {
        Player const* member = other == seat ? nullptr : _scenario.SeatBot(env, other);
        if (!member || !member->IsInWorld() || !member->IsAlive())
            continue;
        x += member->GetPositionX();
        y += member->GetPositionY();
        z += member->GetPositionZ();
        ++count;
    }
    if (!count)
        return -1.0f;
    Position const middle(x / float(count), y / float(count), z / float(count));
    return bot->GetExactDist(&middle);
}

void Animus::Curriculum::PartyFollowEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvParty const& party = _envs[env.Index];
    view.MountsAllowed = false;
    view.GroundMountAllowed = false;
    if (seat == _scenario.OwnerAgent())
    {
        // The cast leader: the next stop is its objective, as a marker was in the stage it was trained on.
        view.HasObjective = party.NextStop < party.Stops.size();
        if (view.HasObjective)
            view.Objective = party.Stops[party.NextStop];
        view.ArriveWithin = LEADER_ARRIVE;
        return;
    }

    // A follower has no objective point: the leader is found by sight, the minimap's dot and memory.
    view.HasObjective = false;
    Player* bot = view.Bot;
    if (!bot)
        return;
    view.MinimapYards = _scenario.Tuning().PartyFollow.MinimapYards;
    Movement::BodyState const* body = view.Body;
    float const selfX = body ? body->X : bot->GetPositionX();
    float const selfY = body ? body->Y : bot->GetPositionY();

    auto const frame = [&](uint32 slot, Player const* member, bool leads)
    {
        SeatView::PartyFrame& out = view.Frames[slot];
        out.Present = true;
        out.Leader = leads;
        out.Alive = member->IsAlive();
        out.InCombat = member->IsInCombat();
        out.Health = member->GetMaxHealth() ? float(member->GetHealth()) / float(member->GetMaxHealth()) : 0.0f;
        Powers const power = member->getPowerType();
        out.Power = member->GetMaxPower(power) ? float(member->GetPower(power)) / float(member->GetMaxPower(power))
            : 0.0f;
        // The minimap: a dot only within its radius, and only on the same map.
        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            return;
        PartyFramesBlock::Dot const dot = PartyFramesBlock::DotOf(selfX, selfY, view.Facing,
            member->GetPositionX(), member->GetPositionY(), view.MinimapYards);
        out.DotShown = dot.Shown;
        out.DotRight = dot.Right;
        out.DotForward = dot.Forward;
    };

    uint32 slot = 0;
    if (Player const* leader = Leader(env); leader && leader->IsInWorld())
        frame(slot++, leader, true);
    EnvState const& data = _scenario.Data(env);
    for (uint32 other = 0; other < data.ActiveSeats && slot < GROUP_MEMBERS; ++other)
    {
        Player const* member = other == seat ? nullptr : _scenario.SeatBot(env, other);
        if (member && member->IsInWorld())
            frame(slot++, member, false);
    }
}

void Animus::Curriculum::PartyFollowEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    if (!bot || seatIndex >= GROUP_SEATS || seatIndex >= _scenario.Data(env).ActiveSeats)
        return;

    EnvParty& party = _envs[env.Index];
    SeatFollow& follow = party.Seats[seatIndex];
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    uint32 const stepMs = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, follow.LastRewardMs);
    follow.LastRewardMs = env.EpisodeElapsedMs;
    float const seconds = float(stepMs) / 1000.0f;

    // A death costs, each time (SeatReward stands DeathCounted down again once the seat is alive). The episode goes
    // on: the seat rises at the entrance (Update) and walks back.
    CombatTally& tally = seat.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }

    // The ground's noise prices, at their own fixed price from the first step (off the cost ladder), as M2's.
    uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, follow.LastStuckMs);
    uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, follow.LastWallMs);
    follow.LastStuckMs = seat.StuckMs;
    follow.LastWallMs = seat.WallMs;
    float moved = 0.0f;
    if (follow.HasLastPos)
    {
        float const dx = bot->GetPositionX() - follow.LastX;
        float const dy = bot->GetPositionY() - follow.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
    }
    follow.LastX = bot->GetPositionX();
    follow.LastY = bot->GetPositionY();
    follow.HasLastPos = true;
    if (!bot->IsAlive() || follow.Clock.Out)
        return;
    CurriculumTuning::SeekTuning const& costs = _scenario.Tuning().Seek;
    if (stuckMs)
        ledger.AddFixed(RewardTerm::Stuck, -costs.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
        float const charge = MarkerEncounter::WallCharge(float(wallMs) / 1000.0f, moved, asked, costs.Wall,
            costs.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
    }

    Player* leader = Leader(env);
    if (!leader || !leader->IsInWorld() || !leader->IsAlive() || leader->GetMapId() != bot->GetMapId())
        return;

    float const distance = bot->GetExactDist(leader);
    follow.DistanceSum += distance;
    ++follow.Samples;
    uint32 const band = FollowEncounter::Band(distance, tuning.BandMin, tuning.BandMax, tuning.LostYards);
    if (band == 1)
    {
        follow.InBandMs += stepMs;
        ledger.Add(RewardTerm::FollowKept, tuning.Kept * seconds);
    }
    else if (band == 3)
    {
        follow.LostMs += stepMs;
        ledger.Add(RewardTerm::Lost, -tuning.Lost * seconds);
    }

    // Back with the leader after it stopped: paid once a stop, sooner paying more, within the window.
    if (follow.RegroupPending)
    {
        float const since = float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, party.StopStartMs)) / 1000.0f;
        if (band == 1)
        {
            follow.RegroupPending = false;
            ++follow.Regroups;
            follow.RegroupMsTotal += uint32(since * 1000.0f);
            ledger.Add(RewardTerm::Regroup, tuning.Regroup * RegroupShare(since, tuning.RegroupWindow));
        }
        else if (since >= tuning.RegroupWindow)
            follow.RegroupPending = false;
    }

    // In a moving leader's way: in front of it and close.
    SeatState const& leaderSeat = _scenario.Data(env).Seats[_scenario.OwnerAgent()];
    bool const leaderMoving = leaderSeat.Controls.Held.Forward != 0 || leaderSeat.Controls.Held.Strafe != 0;
    if (InTheWay(leader->GetPositionX(), leader->GetPositionY(), leader->GetOrientation(), leaderMoving,
        bot->GetPositionX(), bot->GetPositionY(), tuning.BlockYards, tuning.BlockHalfAngle))
    {
        follow.BlockingMs += stepMs;
        ledger.Add(RewardTerm::Blocking, -tuning.Blocking * seconds);
    }
}

void Animus::Curriculum::PartyFollowEncounter::WriteState(Env const& env, float* state) const
{
    // A training-only aid for the critic (the actors never see it): the leader in the owner's columns, relative to the
    // door as every position in the state is, and the rung.
    EnvParty const& party = _envs[env.Index];
    state[StageScenario::STATE_TIER] = float(party.Rung) / float(RUNGS - 1);
    Player const* leader = Leader(env);
    if (!leader || !leader->IsInWorld())
        return;
    Position const& origin = _scenario.SpawnPointFor(env);
    state[StageScenario::STATE_OWNER_PRESENT] = 1.0f;
    state[StageScenario::STATE_OWNER_ALIVE] = leader->IsAlive() ? 1.0f : 0.0f;
    state[StageScenario::STATE_OWNER_HEALTH] = leader->GetMaxHealth()
        ? float(leader->GetHealth()) / float(leader->GetMaxHealth()) : 0.0f;
    state[StageScenario::STATE_OWNER_X] = Encoding::RelativePosition(leader->GetPositionX(), origin.GetPositionX());
    state[StageScenario::STATE_OWNER_Y] = Encoding::RelativePosition(leader->GetPositionY(), origin.GetPositionY());
}

bool Animus::Curriculum::PartyFollowEncounter::IsTerminal(Env const& /*env*/) const
{
    // The episode runs to its clock: no death ends it (I4), and the route's end leaves the leader standing there.
    return false;
}

void Animus::Curriculum::PartyFollowEncounter::Deactivate(Env& env)
{
    Teardown(env);
}

void Animus::Curriculum::PartyFollowEncounter::Teardown(Env& env)
{
    EnvParty& party = _envs[env.Index];
    if (party.Built)
        _scenario.ReleaseOwnerSeat(env);
    party.Built = false;
    env.Allies.clear();
}
