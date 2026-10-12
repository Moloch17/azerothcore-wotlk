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
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "InstanceBosses.h"
#include "Log.h"
#include "Map.h"
#include "Standing.h"
#include "ObjectMgr.h"
#include "MapMgr.h"
#include "ObjectPool.h"
#include "PartyFramesBlock.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "Seek.h"
#include "SightDraw.h"
#include "StageScenario.h"
#include "StageState.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace
{
    /// The leader has reached a corner of its hop this close.
    constexpr float LEADER_ARRIVE = 3.0f;
    /// A leader that strays this far from the corner it walks to turns for the nearest corner of its hop.
    constexpr float LEADER_STRAY = 15.0f;
    /// Chained calls of the stock PathGenerator for one leg: it stops at MAX_POINT_PATH_LENGTH points (~296 yd).
    constexpr uint32 PATH_LEGS = 16;
    /// A leg whose path ends this far from its stop was cut short (a closed door, a gap in the navmesh).
    constexpr float LEG_SHORT_YARDS = 5.0f;
    /// The party gathers at its start this long before the leader sets off (not a regroup: it starts together).
    constexpr uint32 START_PAUSE_MS = 3000;
    /// A fall of the leader's this deep counts as a drop it took, and a fall this deep between two corners of the
    /// way makes that step one-way.
    constexpr float DROP_YARDS = 4.0f;
    /// A halt shorter than this has no looking round.
    constexpr uint32 LOOK_MIN_STOP_MS = 2500;
    /// With no hop of the drawn length either way, the leader stands this long and draws again.
    constexpr uint32 NOWHERE_STOP_MS = 5000;
    /// A waypoint is a start only with the ground found within this many yards of its height, and no floor more than
    /// this above it within START_CEILING_YARDS.
    constexpr float START_FLOOR_YARDS = 2.5f;
    constexpr float START_CEILING_YARDS = 20.0f;
    /// A stuck leader jumps from this long stuck, and side-steps from this long (PartyFollowEncounter::Steer).
    constexpr uint32 UNSTICK_JUMP_MS = 500;
    constexpr uint32 UNSTICK_SIDESTEP_MS = 2000;

    /// The corners of the stock PathGenerator's way from `from` to `to` (the smoothed path its creatures walk, a point
    /// every 4 yd), chained while it stops short of the end; the point `from` itself is left out. Empty when there is
    /// no path at all. Once per map, never per decision.
    std::vector<Position> PathLeg(Player* walker, Position from, Position const& to)
    {
        std::vector<Position> corners;
        for (uint32 leg = 0; leg < PATH_LEGS; ++leg)
        {
            PathGenerator path(walker);
            path.CalculatePath(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ(), to.GetPositionX(),
                to.GetPositionY(), to.GetPositionZ(), false);
            if ((path.GetPathType() & PATHFIND_NOPATH) || path.GetPath().size() < 2)
                break;
            for (std::size_t i = 1; i < path.GetPath().size(); ++i)
                corners.emplace_back(path.GetPath()[i].x, path.GetPath()[i].y, path.GetPath()[i].z);
            G3D::Vector3 const& end = path.GetPath().back();
            Position const reached(end.x, end.y, end.z);
            if (from.GetExactDist2d(&reached) < 1.0f)
                break;
            from = reached;
            if (!(path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_SHORT)))
                break;
        }
        return corners;
    }
    /// A scripted leader that died stands up where it fell after this long.
    constexpr uint32 LEADER_RISE_MS = 2000;
}

Animus::Curriculum::PartyFollowEncounter::PartyFollowEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _starts(envs, 0), _rngs(envs)
{
}

Animus::Curriculum::PartyFollowEncounter::Way Animus::Curriculum::PartyFollowEncounter::MakeWay(
    Position const& door, std::vector<Position> const& stops, std::vector<std::vector<Position>> const& legs,
    float waypointYards, float shortYards, float dropYards)
{
    Way way;
    way.Chain.push_back(door);
    way.Along.push_back(0.0f);
    std::vector<uint32> ends;
    // A step onto the chain's next corner: a cut where the path before it stopped short, else a drop or level.
    auto const add = [&way, dropYards](Position const& corner, bool cut)
    {
        Position const& last = way.Chain.back();
        Step step = cut ? Step::Cut : last.GetPositionZ() - corner.GetPositionZ() >= dropYards ? Step::Drop
            : Step::Both;
        way.Steps.push_back(step);
        way.Along.push_back(way.Along.back() + last.GetExactDist(&corner));
        way.Chain.push_back(corner);
    };
    bool cut = false;
    for (std::size_t leg = 0; leg < legs.size() && leg < stops.size(); ++leg)
    {
        for (Position const& corner : legs[leg])
        {
            add(corner, cut);
            cut = false;
        }
        // A leg that never got to its stop leaves the way cut between where it ended and the stop: the next leg
        // begins at the stop itself.
        bool const reached = !legs[leg].empty() && legs[leg].back().GetExactDist2d(&stops[leg]) <= shortYards;
        if (!reached)
        {
            add(stops[leg], true);
            cut = false;
        }
        ends.push_back(uint32(way.Chain.size() - 1));
    }

    // Waypoints: the door, a corner every waypointYards, and each leg's end (a boss's place).
    float next = 0.0f;
    for (uint32 index = 0; index < way.Chain.size(); ++index)
        if (way.Along[index] >= next || std::find(ends.begin(), ends.end(), index) != ends.end())
        {
            way.Waypoints.push_back(index);
            next = way.Along[index] + std::max(1.0f, waypointYards);
        }
    return way;
}

bool Animus::Curriculum::PartyFollowEncounter::Walkable(Way const& way, uint32 from, uint32 to)
{
    if (from == to || from >= way.Chain.size() || to >= way.Chain.size())
        return false;
    // A step the leader could not walk (Way::Blocked), that way, is as a cut.
    auto const blocked = [&way](uint32 step, uint8 bit)
    {
        return step < way.Blocked.size() && (way.Blocked[step].load(std::memory_order_relaxed) & bit);
    };
    if (from < to)
    {
        for (uint32 step = from; step < to; ++step)
            if (way.Steps[step] == Step::Cut || blocked(step, BLOCKED_FORWARD))
                return false;
        return true;
    }
    for (uint32 step = to; step < from; ++step)
        if (way.Steps[step] != Step::Both || blocked(step, BLOCKED_BACK))
            return false;
    return true;
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

bool Animus::Curriculum::PartyFollowEncounter::Chance(Env const& env, int32 percent) const
{
    if (percent <= 0)
        return false;
    if (percent >= 100)
        return true;
    return std::uniform_int_distribution<int32>(0, 99)(Rng(env)) < percent;
}

float Animus::Curriculum::PartyFollowEncounter::Uniform(Env const& env, float low, float high) const
{
    if (high <= low)
        return low;
    return std::uniform_real_distribution<float>(low, high)(Rng(env));
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PartyFollowEncounter::RewardTerms() const
{
    // Fidget is the scenario's column already (every stage's noise prices); this encounter pays it too.
    return { RewardTerm::FollowKept, RewardTerm::Settled, RewardTerm::Regroup, RewardTerm::Approach, RewardTerm::Drift,
        RewardTerm::Lost,
        RewardTerm::Blocking, RewardTerm::Death, RewardTerm::Stuck, RewardTerm::Wall };
}

void Animus::Curriculum::PartyFollowEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    auto const seatOf = [this](Env const& env, uint32 seat) -> SeatFollow const*
    {
        return seat < GROUP_SEATS ? &_envs[env.Index].Seats[seat] : nullptr;
    };

    // The stage's measure: the share of the episode each follower spent within the band of the leader.
    table.Add("follow_kept_share", [seatOf](Env const& env, uint32 seat)
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
    // Waiting with a leader who stands (decision 0033): of the time in the band beside a leader that had stood
    // SettleAfterMs, the share stood still, and the seconds moving about; and the seconds strayed past DriftYards.
    table.Add("settled_share", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow && follow->WaitMs ? float(follow->SettledMs) / float(follow->WaitMs) : 0.0f;
    });
    table.Add("wait_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->WaitMs) / 1000.0f : 0.0f;
    });
    table.Add("wait_fidget_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->FidgetMs) / 1000.0f : 0.0f;
    });
    // The gap past the band closed, less opened (decision 0034): a frozen follower reads about minus the leader's
    // lead; one that keeps up reads near 0 or above.
    table.Add("approach_yards", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? follow->ApproachYards : 0.0f;
    });
    // Moved at all: a follower that stood the episode through (the freeze seen at 40M) reads 0.
    table.Add("follower_yards", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? follow->MovedYards : 0.0f;
    });
    table.Add("drift_seconds", [seatOf](Env const& env, uint32 seat)
    {
        SeatFollow const* follow = seatOf(env, seat);
        return follow ? float(follow->DriftMs) / 1000.0f : 0.0f;
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
    // Death in the instance (I4): deaths, rises at the start point, rejoins, the time from rising to rejoining (per
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
    // How the leader went: hops drawn and walked to their end (and as a share of those ended either way), turned
    // back, passed through without a stop, stops made, hops given up, sudden stops, halts it looked round at, drops
    // taken, stood up after dying, cast or scripted; where it began (yards along the way from the door).
    table.Add("leader_hops", [this](Env const& env, uint32) { return float(_envs[env.Index].Hops); });
    table.Add("leader_route_share", [this](Env const& env, uint32)
    {
        EnvParty const& party = _envs[env.Index];
        uint32 const ended = party.HopsDone + party.Skips;
        return ended ? float(party.HopsDone) / float(ended) : 0.0f;
    });
    table.Add("leader_reverse_hops", [this](Env const& env, uint32) { return float(_envs[env.Index].ReverseHops); });
    table.Add("leader_pass_hops", [this](Env const& env, uint32) { return float(_envs[env.Index].PassHops); });
    table.Add("leader_stops_reached", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].StopsReached);
    });
    table.Add("leader_skips", [this](Env const& env, uint32) { return float(_envs[env.Index].Skips); });
    table.Add("leader_sudden_stops", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].SuddenStops);
    });
    table.Add("leader_look_stops", [this](Env const& env, uint32) { return float(_envs[env.Index].LookStops); });
    table.Add("leader_drops", [this](Env const& env, uint32) { return float(_envs[env.Index].Drops); });
    table.Add("leader_rises", [this](Env const& env, uint32) { return float(_envs[env.Index].LeaderRises); });
    table.Add("leader_cast", [this](Env const& env, uint32) { return _envs[env.Index].Cast ? 1.0f : 0.0f; });
    table.Add("start_along", [this](Env const& env, uint32) { return _envs[env.Index].StartAlong; });
    // Where it was (the route viewer's map and origin; follow_map as seek_map is the seek stage's).
    table.Add("follow_map", [this](Env const& env, uint32) { return float(_envs[env.Index].MapId); });
    table.Add("start_x", [this](Env const& env, uint32) { return _envs[env.Index].Start.GetPositionX(); });
    table.Add("start_y", [this](Env const& env, uint32) { return _envs[env.Index].Start.GetPositionY(); });
    table.Add("start_z", [this](Env const& env, uint32) { return _envs[env.Index].Start.GetPositionZ(); });
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
    // The episode's draws: from the evaluation seed in an evaluation (the same routes every time), fresh otherwise.
    uint32 const mapId = _scenario.Arena(env).MapId;
    if (env.Evaluating && env.EpisodeSeedIndex != Animus::NO_EPISODE_SEED)
        _rngs[env.Index].seed(uint32(env.EpisodeSeedIndex) * 2654435761u ^ (mapId * 40503u + 0x5f3759dfu));
    else
        _rngs[env.Index].seed(urand(0, std::numeric_limits<uint32>::max() - 1));

    // The dungeon's level band (the dungeon finder's, as a whole dungeon's run was), and its front door: where a party
    // that walked in stands.
    EnvState& data = _scenario.Data(env);
    for (BossRow const& row : FollowBosses())
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
    _starts[env.Index] = 0;
    if (AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId))
    {
        data.EpisodeSpawn.Relocate(entrance->target_X, entrance->target_Y, entrance->target_Z,
            entrance->target_Orientation);
        data.HasEpisodeSpawn = true;
    }

    // Anywhere along the way (decision 0033), once the map's way is known (its first episode starts at the door and
    // works it out): a waypoint drawn, the party facing on along the way.
    Way const* way = WayOf(mapId);
    if (!way || way->Starts.size() < 2 || !Chance(env, _scenario.Tuning().PartyFollow.StartAnywhere))
        return;
    std::size_t const pick = std::uniform_int_distribution<std::size_t>(0, way->Starts.size() - 1)(Rng(env));
    uint32 const at = way->Starts[pick];
    Position const& here = way->Chain[at];
    Position const& on = way->Chain[at + 1 < way->Chain.size() ? at + 1 : at - 1];
    float const yaw = std::atan2(on.GetPositionY() - here.GetPositionY(), on.GetPositionX() - here.GetPositionX());
    data.EpisodeSpawn.Relocate(here.GetPositionX(), here.GetPositionY(), here.GetPositionZ(), yaw);
    data.HasEpisodeSpawn = true;
    _starts[env.Index] = at;
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
    party.Cast = !env.Evaluating && tuning.CastShare > 0 && Chance(env, tuning.CastShare);
    party.MapId = map->GetId();
    party.Start = _scenario.SpawnPointFor(env);

    Map* leaderMap = map;
    Player* leader = _scenario.BuildOwnerSeat(env, leaderMap, level, party.Start, AptitudeDemand::Anything());
    if (!leader)
        return false;
    party.Built = true;
    leader->SetFaction(first->GetFaction());
    env.Allies = { leader->GetGUID() };

    // The way, as the stock PathGenerator walks it from the door (worked out once per map: its first episode pays,
    // and starts at the door).
    Way const* way = WayOf(party.MapId);
    if (!way)
    {
        AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(party.MapId);
        Position const door = entrance ? Position(entrance->target_X, entrance->target_Y, entrance->target_Z,
            entrance->target_Orientation) : party.Start;
        way = WorkOutWay(map, first, door);
    }
    if (!way || way->Chain.size() < 2)
    {
        LOG_ERROR("module.animus", "{}: map {} has no way for the party follow (no boss places, or no path)",
            _scenario.Name(), party.MapId);
        return false;
    }
    party.At = std::min<uint32>(_starts[env.Index], uint32(way->Chain.size() - 1));
    party.StartAlong = way->Along[party.At];
    party.Mode = Phase::Stopped;
    party.StopStartMs = env.EpisodeElapsedMs;
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
    for (BossRow const& row : FollowBosses())
        if (row.MapId == mapId)
            if (CreatureData const* spawn = InstanceEncounter::FindSpawn(row))
                stops.emplace_back(spawn->posX, spawn->posY, spawn->posZ, spawn->orientation);
    return stops;
}

Animus::Curriculum::PartyFollowEncounter::Way const* Animus::Curriculum::PartyFollowEncounter::WayOf(
    uint32 mapId) const
{
    std::lock_guard<std::mutex> lock(_routesLock);
    auto const known = _ways.find(mapId);
    return known != _ways.end() ? &known->second : nullptr;
}

Animus::Curriculum::PartyFollowEncounter::Way const* Animus::Curriculum::PartyFollowEncounter::WorkOutWay(Map* map,
    Player* walker, Position const& door)
{
    std::vector<Position> const stops = RouteStops(map->GetId());
    if (stops.empty())
        return nullptr;
    // The stops and the ground are fixed after startup: worked out once per map, here on the thread that resets.
    std::lock_guard<std::mutex> lock(_routesLock);
    auto const known = _ways.find(map->GetId());
    if (known != _ways.end())
        return &known->second;
    std::vector<std::vector<Position>> legs;
    Position cursor(door);
    for (Position const& stop : stops)
    {
        legs.push_back(PathLeg(walker, cursor, stop));
        cursor = stop;
    }
    Way& way = _ways[map->GetId()];
    way = MakeWay(door, stops, legs, _scenario.Tuning().PartyFollow.WaypointYards, LEG_SHORT_YARDS, DROP_YARDS);
    way.Blocked = std::vector<std::atomic<uint8>>(way.Steps.size());
    // The way's terrain and collision, loaded now on the base map (every instance of the map reads it), on the world
    // thread: a party started or led far along the way stood on grids nobody had loaded, the loads were deferred past
    // the decision, and the controller found no floor and fell through the world (2026-10-12). The constructor loads
    // the stage's spawn points' grids the same way; the way reaches far past them.
    if (Map* base = sMapMgr->CreateBaseMap(map->GetId()))
    {
        constexpr float REACH = 40.0f;
        std::set<std::pair<uint32, uint32>> grids;
        for (Position const& corner : way.Chain)
            for (int32 dx = -1; dx <= 1; ++dx)
                for (int32 dy = -1; dy <= 1; ++dy)
                {
                    GridCoord const grid = Acore::ComputeGridCoord(corner.GetPositionX() + float(dx) * REACH,
                        corner.GetPositionY() + float(dy) * REACH);
                    grids.emplace(grid.x_coord, grid.y_coord);
                }
        for (auto const& [x, y] : grids)
            base->EnsureGridCreated(GridCoord(x, y));
        LOG_INFO("module.animus", "{}: map {} follow way: {} grids along it loaded", _scenario.Name(), map->GetId(),
            grids.size());
    }
    // The starts: the waypoints with the ground under them at their own height and no floor just above (the server's
    // heights; the map's grids load on demand here, on the world thread). The door is always one.
    for (uint32 index : way.Waypoints)
    {
        Position& corner = way.Chain[index];
        float const below = map->GetHeight(corner.GetPositionX(), corner.GetPositionY(), corner.GetPositionZ() + 1.0f);
        float const above = map->GetHeight(corner.GetPositionX(), corner.GetPositionY(),
            corner.GetPositionZ() + START_CEILING_YARDS, true, START_CEILING_YARDS + 5.0f);
        bool const floor = below > INVALID_HEIGHT + 1.0f
            && std::fabs(below - corner.GetPositionZ()) <= START_FLOOR_YARDS;
        bool const ceiling = above > INVALID_HEIGHT + 1.0f && above > corner.GetPositionZ() + START_FLOOR_YARDS;
        if (index == 0 || (floor && !ceiling))
            way.Starts.push_back(index);
    }
    uint32 const cuts = uint32(std::count(way.Steps.begin(), way.Steps.end(), Step::Cut));
    uint32 const drops = uint32(std::count(way.Steps.begin(), way.Steps.end(), Step::Drop));
    LOG_INFO("module.animus", "{}: map {} follow way: {} legs, {} corners, {:.0f} yd, {} waypoints ({} starts: the "
        "others' ground is not at their height), {} drops, {} cut (the leader never crosses a cut, nor climbs a drop)",
        _scenario.Name(), map->GetId(), legs.size(), way.Chain.size(), way.Along.empty() ? 0.0f : way.Along.back(),
        way.Waypoints.size(), way.Starts.size(), drops, cuts);
    return &way;
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
    float const gap = Uniform(env, tuning.SuddenGapMin, std::max(tuning.SuddenGapMin, tuning.SuddenGapMax)) * scale;
    party.NextSuddenMs = env.EpisodeElapsedMs + std::max<uint32>(1000, uint32(gap * 1000.0f));
}

void Animus::Curriculum::PartyFollowEncounter::Stop(Env const& env, EnvParty& party, uint32 ms, bool counts) const
{
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    party.Mode = Phase::Stopped;
    party.StopStartMs = env.EpisodeElapsedMs;
    party.StopUntilMs = env.EpisodeElapsedMs + ms;
    party.LookFromMs = 0;
    party.LookUntilMs = 0;
    party.LookAgain = false;
    // Looking round at a halt long enough for it, as a player does: a turn a moment after stopping, maybe one back.
    if (ms >= LOOK_MIN_STOP_MS && Chance(env, tuning.LookChance))
    {
        ++party.LookStops;
        party.LookFromMs = env.EpisodeElapsedMs + uint32(Uniform(env, 300.0f, 1200.0f));
        party.LookUntilMs = party.LookFromMs + uint32(Uniform(env, 800.0f, 2200.0f));
        party.LookRate = tuning.LookRate * Uniform(env, 0.7f, 1.3f) * (Chance(env, 50) ? 1.0f : -1.0f);
        party.LookAgain = Chance(env, 50);
    }
    if (!counts || ms < tuning.RegroupMinStopMs)
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

bool Animus::Curriculum::PartyFollowEncounter::PickHop(Env const& env, EnvParty& party) const
{
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    Way const* way = WayOf(party.MapId);
    if (!way || way->Waypoints.size() < 2)
        return false;
    uint32 const at = party.At;
    float const here = way->Along[at];
    float const low = std::max(1.0f, tuning.HopMinYards);
    float const high = std::max(low, tuning.HopMaxYards);
    // The waypoints a hop the drawn way could end at: within the hop's length, and walkable from here.
    auto const candidates = [&](bool back)
    {
        std::vector<uint32> found;
        for (uint32 index : way->Waypoints)
        {
            if (back ? index >= at : index <= at)
                continue;
            float const yards = std::fabs(way->Along[index] - here);
            if (yards >= low && yards <= high && Walkable(*way, at, index))
                found.push_back(index);
        }
        return found;
    };
    bool back = party.Rung >= tuning.ReverseFromRung && Chance(env, tuning.ReverseChance);
    std::vector<uint32> found = candidates(back);
    if (found.empty())
    {
        // At an end of the way, or before a drop it cannot climb: the other way.
        back = !back;
        found = candidates(back);
    }
    if (found.empty())
    {
        // Nothing of the hop's length either way (a short stretch between cuts): the nearest walkable waypoint.
        float best = std::numeric_limits<float>::max();
        for (uint32 index : way->Waypoints)
            if (Walkable(*way, at, index) && std::fabs(way->Along[index] - here) < best)
            {
                best = std::fabs(way->Along[index] - here);
                found = { index };
            }
        if (found.empty())
            return false;
        back = found.front() < at;
    }
    uint32 const target = found[std::uniform_int_distribution<std::size_t>(0, found.size() - 1)(Rng(env))];
    party.Target = target;
    party.Hop.clear();
    party.HopIndex.clear();
    if (target > at)
        for (uint32 index = at + 1; index <= target; ++index)
        {
            party.Hop.push_back(way->Chain[index]);
            party.HopIndex.push_back(index);
        }
    else
        for (uint32 index = at; index-- > target;)
        {
            party.Hop.push_back(way->Chain[index]);
            party.HopIndex.push_back(index);
        }
    party.Corner = 0;
    party.HopStops = !Chance(env, tuning.PassChance);
    party.HopWalk = party.Rung < tuning.WalkRungs || Chance(env, tuning.WalkChance);
    ++party.Hops;
    if (back)
        ++party.ReverseHops;
    if (!party.HopStops)
        ++party.PassHops;
    return true;
}

void Animus::Curriculum::PartyFollowEncounter::Steer(Env& env, EnvParty& party, Player* leader)
{
    CurriculumTuning::PartyFollowTuning const& tuning = _scenario.Tuning().PartyFollow;
    SeatState& seat = _scenario.Data(env).Seats[_scenario.OwnerAgent()];
    uint32 const now = env.EpisodeElapsedMs;
    // Where it stands in its plan: a stop over (the hop it was on goes on after a sudden stop or a step back; else
    // the next hop), a sudden stop due, a hop's end reached, a hop given up.
    if (party.Mode == Phase::Stopped && now >= party.StopUntilMs)
    {
        if (party.Corner < party.Hop.size() || PickHop(env, party))
        {
            party.Mode = Phase::Walking;
            party.LegStuckMs = seat.StuckMs;
        }
        else
            Stop(env, party, NOWHERE_STOP_MS, false);
    }
    if (party.Mode == Phase::Walking && party.NextSuddenMs && now >= party.NextSuddenMs)
    {
        ++party.SuddenStops;
        uint32 const ms = uint32(Uniform(env, float(tuning.SuddenStopMinMs),
            float(std::max(tuning.SuddenStopMinMs, tuning.SuddenStopMaxMs))));
        if (party.Rung >= tuning.BackStepFromRung && Chance(env, tuning.BackStepChance))
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
        // The corners of the hop, no query: on past each one reached (every one a step carried it past, not only
        // the next), and the nearest when it has strayed (a fall, a step back).
        while (party.Corner < party.Hop.size() && leader->GetExactDist2d(&party.Hop[party.Corner]) <= LEADER_ARRIVE)
            ++party.Corner;
        if (party.Corner < party.Hop.size() && leader->GetExactDist2d(&party.Hop[party.Corner]) > LEADER_STRAY)
        {
            float nearest = std::numeric_limits<float>::max();
            for (std::size_t corner = 0; corner < party.Hop.size(); ++corner)
                if (float const away = leader->GetExactDist(&party.Hop[corner]); away < nearest)
                {
                    nearest = away;
                    party.Corner = uint32(corner);
                }
        }
        bool const arrived = party.Corner >= party.Hop.size();
        bool const stuck = seat.StuckMs - std::min(seat.StuckMs, party.LegStuckMs) >= tuning.GiveUpMs;
        if (arrived)
        {
            party.At = party.Target;
            party.Hop.clear();
            party.Corner = 0;
            ++party.HopsDone;
            if (party.HopStops)
            {
                ++party.StopsReached;
                Stop(env, party, uint32(StopSeconds(party.Rung, tuning.StopSecondsFirst, tuning.StopSecondsLast)
                    * Uniform(env, 0.6f, 1.4f) * 1000.0f), true);
            }
            else if (PickHop(env, party))
                party.LegStuckMs = seat.StuckMs;
            else
                Stop(env, party, NOWHERE_STOP_MS, false);
        }
        else if (stuck)
        {
            // Given up: the step into the corner it could not reach is blocked that way for good, and it stands where
            // it is on the way (the nearest corner of the chain it may walk on from) and draws again.
            ++party.Skips;
            BlockStep(party);
            // The first give-ups of each map, told: where the leader stood, the corner it walked to and what lay
            // between (the leader wandered on the Deadmines and Ragefire ways without finishing a hop, 2026-10-12).
            bool tell = false;
            {
                std::lock_guard<std::mutex> lock(_routesLock);
                tell = _skipsTold[party.MapId]++ < SKIPS_TOLD;
            }
            if (tell && party.Corner < party.Hop.size())
            {
                Position const& corner = party.Hop[party.Corner];
                Map* map = leader->GetMap();
                LOG_INFO("module.animus", "{}: map {} leader gave up hop {} -> {} at corner {}/{}: leader ({:.1f}, {:.1f}, "
                    "{:.1f}) ground {:.1f}; corner ({:.1f}, {:.1f}, {:.1f}) ground {:.1f}; {:.1f} yd away, {:.1f} yd "
                    "up; walking {}, wall {}, keys fwd {} strafe {} turn {:.2f}", _scenario.Name(), party.MapId, party.At,
                    party.Target, party.Corner, party.Hop.size(), leader->GetPositionX(), leader->GetPositionY(),
                    leader->GetPositionZ(), map->GetHeight(leader->GetPositionX(), leader->GetPositionY(),
                    leader->GetPositionZ() + 2.0f), corner.GetPositionX(), corner.GetPositionY(),
                    corner.GetPositionZ(), map->GetHeight(corner.GetPositionX(), corner.GetPositionY(),
                    corner.GetPositionZ() + 2.0f), leader->GetExactDist2d(&corner),
                    corner.GetPositionZ() - leader->GetPositionZ(), party.HopWalk, seat.Mover.TickWall,
                    int32(seat.Controls.Held.Forward), int32(seat.Controls.Held.Strafe), seat.Controls.Held.TurnRate);
            }
            if (Way const* way = WayOf(party.MapId))
            {
                float nearest = std::numeric_limits<float>::max();
                for (uint32 index = 0; index < way->Chain.size(); ++index)
                    if (float const away = leader->GetExactDist(&way->Chain[index]); away < nearest)
                    {
                        nearest = away;
                        party.At = index;
                    }
            }
            party.Hop.clear();
            party.Corner = 0;
            Stop(env, party, NOWHERE_STOP_MS / 5, false);
        }
    }
    // The keys: the seek helper's toward the hop's next corner while it walks (or the point a step back); while it
    // stands, nothing but a look round now and then. Not before the controller has started this episode (a bearing
    // from a default body would be from nowhere).
    Movement::ControlState held;
    if (seat.Mover.Started())
    {
        if (party.Mode == Phase::BackStep)
        {
            held = Movement::Seek(seat.Mover.Body, party.BackTo.GetPositionX(), party.BackTo.GetPositionY());
            held.Walk = true;
        }
        else if (party.Mode == Phase::Walking && party.Corner < party.Hop.size())
        {
            Position const& corner = party.Hop[party.Corner];
            held = Movement::Seek(seat.Mover.Body, corner.GetPositionX(), corner.GetPositionY());
            held.Walk = party.HopWalk;
            // Stuck, as a player gets out of it: a jump at a lip from half a second on, and from two seconds a step
            // to one side and then the other, a second each (the stock path climbs rock lips a player's controller
            // walks into, 2026-10-12). GiveUpMs later the hop is given up and the step marked blocked.
            if (seat.StuckRunMs >= UNSTICK_JUMP_MS)
                held.Jump = true;
            if (seat.StuckRunMs >= UNSTICK_SIDESTEP_MS)
                held.Strafe = (seat.StuckRunMs / 1000) % 2 ? 1 : -1;
        }
        else if (party.Mode == Phase::Stopped && party.LookUntilMs)
        {
            if (now >= party.LookFromMs && now < party.LookUntilMs)
                held.TurnRate = party.LookRate;
            else if (now >= party.LookUntilMs && party.LookAgain && now + 1500 < party.StopUntilMs)
            {
                // The look back the other way.
                party.LookAgain = false;
                party.LookFromMs = now + 300;
                party.LookUntilMs = party.LookFromMs + uint32(Uniform(env, 800.0f, 2000.0f));
                party.LookRate = -party.LookRate;
            }
        }
    }
    held.FaceTurnApplied = seat.Controls.Held.FaceTurnApplied;
    seat.Controls.Held = held;
}

void Animus::Curriculum::PartyFollowEncounter::BlockStep(EnvParty const& party)
{
    if (party.Corner >= party.HopIndex.size())
        return;
    Way const* way = WayOf(party.MapId);
    if (!way)
        return;
    // The step from the corner before (the hop's start for its first corner) into the corner it could not reach.
    uint32 const to = party.HopIndex[party.Corner];
    uint32 const from = party.Corner ? party.HopIndex[party.Corner - 1] : party.At;
    if (from == to || std::max(from, to) - std::min(from, to) != 1)
        return;
    uint32 const step = std::min(from, to);
    uint8 const bit = to > from ? BLOCKED_FORWARD : BLOCKED_BACK;
    if (step >= way->Blocked.size())
        return;
    uint8 const before = const_cast<Way*>(way)->Blocked[step].fetch_or(bit, std::memory_order_relaxed);
    if (!(before & bit))
        LOG_INFO("module.animus", "{}: map {} follow way: step {} ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}) "
            "blocked {} (the leader could not walk it)", _scenario.Name(), party.MapId, step,
            way->Chain[step].GetPositionX(), way->Chain[step].GetPositionY(), way->Chain[step].GetPositionZ(),
            way->Chain[step + 1].GetPositionX(), way->Chain[step + 1].GetPositionY(),
            way->Chain[step + 1].GetPositionZ(), to > from ? "forward" : "back");
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
            // At the episode's start point: the door, or the waypoint it began at (decision 0033).
            RiseAtEntrance(bot, data.Seats[seat], party.Start, env.EpisodeElapsedMs);
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
    // A cast leader plays its own row (its objective is its hop's end, View); the script's leader holds its keys.
    if (party.Cast)
    {
        Way const* way = WayOf(party.MapId);
        bool const reached = way && party.Target < way->Chain.size()
            && leader->GetExactDist2d(&way->Chain[party.Target]) <= LEADER_ARRIVE;
        if (party.Hop.empty() || reached)
        {
            if (reached)
            {
                party.At = party.Target;
                ++party.HopsDone;
                ++party.StopsReached;
            }
            PickHop(env, party);
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
    if (seat == _scenario.OwnerAgent())
    {
        // The cast leader: its hop's end is its objective, as a marker was in the stage it was trained on.
        Way const* way = WayOf(party.MapId);
        view.HasObjective = way && !party.Hop.empty() && party.Target < way->Chain.size();
        if (view.HasObjective)
            view.Objective = way->Chain[party.Target];
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
    // The party has no core group (the leader is the owner's slot), so its frames are the encounter's to name: the
    // leader, then the other followers in seat order. Each is read as any party frame is (PartyFramesBlock::FillFrame:
    // the minimap's dot only within its radius, and only on the same map; the seat's memory of where it was seen).
    view.Frames.fill(SeatView::PartyFrame());
    ObjectGuid const focus = view.Focus ? *view.Focus : ObjectGuid::Empty;
    auto const frame = [&](uint32 slot, Player const* member, bool leads)
    {
        PartyFramesBlock::FillFrame(view.Frames[slot], member, leads, bot, selfX, selfY, view.Facing,
            view.MinimapYards, focus, view.Seen, view.Party, view.NowMs);
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
    // on: the seat rises at the start point (Update) and walks back.
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
        follow.MovedYards += moved;
    }
    follow.LastX = bot->GetPositionX();
    follow.LastY = bot->GetPositionY();
    follow.HasLastPos = true;
    if (!bot->IsAlive() || follow.Clock.Out)
    {
        // A rise at the start point is not a gap opened by the seat: the gap is measured afresh after it.
        follow.LastGap = -1.0f;
        return;
    }
    CurriculumTuning::SeekTuning const& costs = _scenario.Tuning().Seek;
    if (stuckMs)
        ledger.AddFixed(RewardTerm::Stuck, -costs.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.StepMs(env)) / 1000.0f;
        float const charge = Standing::WallCharge(float(wallMs) / 1000.0f, moved, asked, costs.Wall,
            costs.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
    }
    Player* leader = Leader(env);
    if (!leader || !leader->IsInWorld() || !leader->IsAlive() || leader->GetMapId() != bot->GetMapId())
    {
        follow.LastGap = -1.0f;
        return;
    }
    float const distance = bot->GetExactDist(leader);
    follow.DistanceSum += distance;
    ++follow.Samples;
    SeatState const& leaderSeat = _scenario.Data(env).Seats[_scenario.OwnerAgent()];
    bool const leaderMoving = leaderSeat.Controls.Held.Forward != 0 || leaderSeat.Controls.Held.Strafe != 0;
    uint32 const band = Standing::Band(distance, tuning.BandMin, tuning.BandMax, tuning.LostYards);
    if (band == 1)
    {
        follow.InBandMs += stepMs;
        ledger.Add(RewardTerm::FollowKept, tuning.Kept * seconds);
    }
    // Drifting off: past DriftYards, or past DriftMovingYards (the band's edge) while the leader walks away -- watching
    // it go is not waiting (decision 0034).
    else if (band == 2 && (distance > tuning.DriftYards || (leaderMoving && distance > tuning.DriftMovingYards)))
    {
        follow.DriftMs += stepMs;
        ledger.Add(RewardTerm::Drift, -tuning.Drift * seconds);
    }
    else if (band == 3)
    {
        follow.LostMs += stepMs;
        ledger.Add(RewardTerm::Lost, -tuning.Lost * seconds);
    }
    // Closing the gap (decision 0034, Shaping: the fade takes it away up the ladder): the yards of the gap past the
    // band closed since the last decision, paid, and opened, charged -- a difference of one capped gap, so it sums to
    // the gap at the start less the gap at the end and cannot be farmed by going back and forth.
    float const gap = std::clamp(distance - tuning.BandMax, 0.0f, std::max(0.0f, tuning.ApproachCapYards));
    if (follow.LastGap >= 0.0f && gap != follow.LastGap)
    {
        follow.ApproachYards += follow.LastGap - gap;
        ledger.Add(RewardTerm::Approach, tuning.Approach * (follow.LastGap - gap));
    }
    follow.LastGap = gap;
    // Waiting with a leader who has stood a moment (decision 0033): standing still in the band is paid, moving about
    // in it is charged. Translation alone counts: turning and looking round are free.
    bool const waiting = party.Mode == Phase::Stopped && !leaderMoving
        && env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, party.StopStartMs) >= tuning.SettleAfterMs;
    if (waiting && band == 1)
    {
        float speed = seconds > 0.0f ? moved / seconds : 0.0f;
        if (seat.Mover.Started())
            speed = std::sqrt(seat.Mover.Body.Vx * seat.Mover.Body.Vx + seat.Mover.Body.Vy * seat.Mover.Body.Vy);
        follow.WaitMs += stepMs;
        if (speed < tuning.SettleSpeed)
        {
            follow.SettledMs += stepMs;
            ledger.Add(RewardTerm::Settled, tuning.Settled * seconds);
        }
        else
        {
            follow.FidgetMs += stepMs;
            ledger.Add(RewardTerm::Fidget, -tuning.Fidget * seconds);
        }
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
    // episode's start as every position in the state is, and the rung.
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
    // The episode runs to its clock: no death ends it (I4), and the leader wanders on until it does.
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
