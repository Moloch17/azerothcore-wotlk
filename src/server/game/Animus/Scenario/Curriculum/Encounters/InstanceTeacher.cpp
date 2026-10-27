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

#include "Encounters.h"
#include "Creature.h"
#include "EntityActions.h"
#include "GameObject.h"
#include "Map.h"
#include "Player.h"
#include <algorithm>
#include <cmath>
#include <limits>

/*
 * The teacher's view of the way (TeacherTuning; the user, 2026-10-27: "a teacher again that slowly withdraws", "one of
 * the key things it needs to do is teach the bots how to open the doors"): where along the recorded line the party
 * goes next, what of the way is shut and who opens it, and the next pack. What a script may know -- never a seat's
 * input: the seats see only what their blocks show, and the teacher's presses reach them as hints or played decisions.
 */

namespace
{
    using namespace Animus::Curriculum;

    constexpr float AHEAD_YARDS = 6.0f;         // a step is this far along the line
    constexpr float TANK_DIRECT_YARDS = 15.0f;  // a member this near the tank (and on its floor) walks straight to it
    constexpr float SAME_FLOOR_YARDS = 4.0f;
    constexpr float CANNON_YARDS = 25.0f;       // the opener of a door its key works (the cannon) stands this near it

    bool Open(GameObject const* object)
    {
        return InstanceEncounter::GateOpen(object);
    }

    GameObject* GateObject(PointNavEncounter::ClearWay const& way, uint32 k, Map* map)
    {
        if (k >= way.GateSpawns.size() || !way.GateSpawns[k])
            return nullptr;
        auto const range = map->GetGameObjectBySpawnIdStore().equal_range(way.GateSpawns[k]);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second && it->second->IsInWorld())
                return it->second;
        return nullptr;
    }

    /// The point of `points` in [begin, end) nearest `at` (a floor counts twice the yards: the dungeon's ways pass
    /// over each other).
    uint32 Nearest(std::vector<Position> const& points, uint32 begin, uint32 end, Position const& at)
    {
        uint32 best = begin;
        float bestScore = std::numeric_limits<float>::max();
        for (uint32 i = begin; i < end; ++i)
        {
            float const dx = points[i].GetPositionX() - at.GetPositionX();
            float const dy = points[i].GetPositionY() - at.GetPositionY();
            float const dz = 2.0f * (points[i].GetPositionZ() - at.GetPositionZ());
            float const score = dx * dx + dy * dy + dz * dz;
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }
}

Animus::Curriculum::InstanceEncounter::TeacherLine const& Animus::Curriculum::InstanceEncounter::LineOf(
    PointNavEncounter::ClearWay const& way)
{
    std::lock_guard lock(_teacherLineLock);
    auto found = _teacherLines.find(&way);
    if (found != _teacherLines.end())
        return found->second;
    TeacherLine& line = _teacherLines[&way];
    uint32 const legs = uint32(way.Points.size());
    line.LegFirst.assign(legs, 0);
    line.LegEnd.assign(legs, 0);
    auto const add = [&line](Position const& at)
    {
        float along = 0.0f;
        if (!line.Points.empty())
            along = line.Along.back() + line.Points.back().GetExactDist(&at);
        line.Points.push_back(at);
        line.Along.push_back(along);
    };
    for (uint32 k = 0; k < legs; ++k)
    {
        line.LegFirst[k] = uint32(line.Points.size());
        // The recorded line where there is one (the player's own steps), else the checkpoint alone.
        if (k < way.LegPath.size() && !way.LegPath[k].empty())
            for (Position const& at : way.LegPath[k])
                add(at);
        add(way.Points[k]);
        line.LegEnd[k] = uint32(line.Points.size());
    }
    return line;
}

Animus::Curriculum::InstanceEncounter::TeacherWay const& Animus::Curriculum::InstanceEncounter::Teacher(
    Env const& env, Map* map)
{
    EnvInstance& fight = _envs[env.Index];
    if (fight.TeacherStamp == env.EpisodeElapsedMs)
        return fight.Teacher;
    fight.TeacherStamp = env.EpisodeElapsedMs;
    TeacherWay& view = fight.Teacher;
    view = TeacherWay();
    if (!fight.Way || !map || fight.Way->Points.empty())
        return view;
    PointNavEncounter::ClearWay const& way = *fight.Way;
    uint32 const count = uint32(way.Points.size());
    // Where the party stood at the start (a leg's, a drawn start's: the checkpoint before), and its goal.
    uint32 const lower = fight.StartSite > 0 ? uint32(fight.StartSite) - 1 : 0;
    uint32 const goal = std::min<uint32>(fight.LegTrip && fight.Leg >= 0 ? uint32(fight.Leg)
        : uint32(std::max(fight.GoalSite, 0)), count - 1);
    view.Valid = true;
    view.Lower = lower;
    view.Target = goal;
    EnvState const& data = _scenario.Data(env);
    // Whether a living seat holds the way's key (the gunpowder): a key chest is done once it does -- the barrel resets
    // after it is looted, and read shut again it sent seat after seat back to loot it while the gunpowder's holder
    // never went to the cannon (2026-10-27, every cannon leg).
    bool keyHeld = false;
    for (uint32 index = 0; index < data.ActiveSeats && !keyHeld; ++index)
        if (Player* bot = _scenario.SeatBot(env, index); bot && bot->IsInWorld() && bot->IsAlive())
            keyHeld = EntityActions::CarriesKey(bot, map->GetId());
    // The first gate from the start to the goal still shut: the tank walks to it, and it is opened before anything.
    int32 shut = -1;
    for (uint32 k = lower; k <= goal && k < count; ++k)
    {
        if (!way.IsGate(k))
            continue;
        GameObject* gate = GateObject(way, k, map);
        bool const done = Open(gate) || (keyHeld && gate && gate->GetGoType() == GAMEOBJECT_TYPE_CHEST
            && EntityActions::KeyChest(gate));
        if (!done)
        {
            shut = int32(k);
            break;
        }
    }

    auto const living = [&](uint32 index) -> Player*
    {
        Player* bot = _scenario.SeatBot(env, index);
        return bot && bot->IsInWorld() && bot->IsAlive() ? bot : nullptr;
    };
    Player* tank = nullptr;
    for (uint32 index = 0; index < data.ActiveSeats && !tank; ++index)
        if (data.Seats[index].DungeonRole == DUNGEON_TANK)
            tank = living(index);
    for (uint32 index = 0; index < data.ActiveSeats && !tank; ++index)
        tank = living(index);

    if (shut >= 0)
    {
        view.Target = uint32(shut);
        view.AtGate = true;
        GameObject* gate = GateObject(way, uint32(shut), map);
        uint32 const mapId = map->GetId();
        auto const carrier = [&]() -> Player*
        {
            for (uint32 index = 0; index < data.ActiveSeats; ++index)
                if (Player* bot = living(index); bot && EntityActions::CarriesKey(bot, mapId))
                    return bot;
            return nullptr;
        };
        // The seat nearest `object` (kept while it lives: the seat named to loot the barrel stays named).
        auto const nearest = [&](GameObject const* object, ObjectGuid& kept) -> Player*
        {
            if (!kept.IsEmpty())
                for (uint32 index = 0; index < data.ActiveSeats; ++index)
                    if (Player* bot = living(index); bot && bot->GetGUID() == kept)
                        return bot;
            // Not a druid, nor anyone in a form: a shapeshifted seat cannot use an item, and the gunpowder it loots
            // never fires the cannon (2026-10-27: SPELL_FAILED_NOT_SHAPESHIFT on most uses, the druid tank looting
            // as the seat nearest the barrel). Anyone, if every seat is one.
            auto const handy = [](Player const* bot)
            {
                return bot->getClass() != CLASS_DRUID && !bot->GetShapeshiftForm();
            };
            Player* best = nullptr;
            for (bool const strict : { true, false })
            {
                for (uint32 index = 0; index < data.ActiveSeats; ++index)
                    if (Player* bot = living(index); bot && (!strict || handy(bot))
                        && (!best || bot->GetExactDist(object) < best->GetExactDist(object)))
                        best = bot;
                if (best)
                    break;
            }
            kept = best ? best->GetGUID() : ObjectGuid::Empty;
            return best;
        };
        auto const loot = [&](GameObject* chest, uint32 k)
        {
            view.Object = chest->GetGUID();
            view.ObjectGate = k;
            view.ObjectOpens = WingTeacher::Opens::Loot;
            if (Player* opener = nearest(chest, fight.LootOpener))
                view.Opener = opener->GetGUID();
        };
        if (gate && gate->GetGoType() == GAMEOBJECT_TYPE_CHEST && EntityActions::KeyChest(gate))
            loot(gate, uint32(shut));
        else if (gate && gate->GetGoType() == GAMEOBJECT_TYPE_DOOR && Usable(gate))
        {
            view.Object = gate->GetGUID();
            view.ObjectGate = uint32(shut);
            view.ObjectOpens = WingTeacher::Opens::Click;
            if (tank)
                view.Opener = tank->GetGUID();
        }
        else if (gate && gate->GetGoType() == GAMEOBJECT_TYPE_DOOR)
        {
            // A door no hand opens: its key's object near it (the cannon and the gunpowder), else its boss's (nothing
            // to press: the boss's death opens it).
            GameObject* cannon = nullptr;
            for (auto const& [spawnId, object] : map->GetGameObjectBySpawnIdStore())
                if (object && object->IsInWorld() && object->GetGoType() == GAMEOBJECT_TYPE_GOOBER
                    && KeyOfLock(object) && object->GetExactDist(gate) <= CANNON_YARDS
                    && (!cannon || object->GetExactDist(gate) < cannon->GetExactDist(gate)))
                    cannon = object;
            if (cannon)
            {
                if (Player* holder = carrier())
                {
                    view.Object = cannon->GetGUID();
                    view.ObjectGate = uint32(shut);
                    view.ObjectOpens = WingTeacher::Opens::UseKey;
                    view.Opener = holder->GetGUID();
                }
                else
                    // Nobody holds the gunpowder: back to the barrel before it, still unlooted.
                    for (uint32 k = uint32(shut); k-- > 0;)
                        if (GameObject* chest = GateObject(way, k, map); chest
                            && chest->GetGoType() == GAMEOBJECT_TYPE_CHEST && EntityActions::KeyChest(chest)
                            && !Open(chest))
                        {
                            loot(chest, k);
                            break;
                        }
            }
        }
    }
    else if (tank && view.Target < way.Spawns.size())
    {
        // The goal's pack: its nearest member not yet in a fight.
        Creature* pull = nullptr;
        for (uint32 spawnId : way.Spawns[view.Target])
        {
            auto const range = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            for (auto it = range.first; it != range.second; ++it)
                if (Creature* creature = it->second; creature && creature->IsInWorld() && creature->IsAlive()
                    && !creature->IsInCombat() && (!pull || tank->GetExactDist(creature) < tank->GetExactDist(pull)))
                    pull = creature;
        }
        if (pull)
            view.Pull = pull->GetGUID();
    }
    return view;
}

float Animus::Curriculum::InstanceEncounter::TeacherTau(Env const& env)
{
    EnvInstance const& fight = _envs[env.Index];
    CurriculumTuning::TeacherTuning const& teacher = _scenario.Tuning().Teacher;
    PointNavEncounter* nav = _scenario.PointNav();
    if (!teacher.Enable || !_scenario.Stage().Teacher || !_scenario.Stage().Has(BlockId::Hint) || env.Evaluating
        || fight.Evaluating
        || fight.TeacherFree || !fight.Way || !nav || fight.Way->Points.empty())
        return 0.0f;
    uint32 const count = uint32(fight.Way->Points.size());
    // The leg the party is on: a leg trip's own, a run's next goal.
    uint32 const leg = std::min<uint32>(fight.LegTrip && fight.Leg >= 0 ? uint32(fight.Leg)
        : uint32(std::max(fight.GoalSite, 0)), count - 1);
    return nav->LegTau(_scenario.Data(env).Arena, fight.Rung, leg);
}

bool Animus::Curriculum::InstanceEncounter::TeacherStep(Env const& env, Player const* bot, Player const* tank,
    bool isTank, Position& step)
{
    EnvInstance& fight = _envs[env.Index];
    if (!fight.Way || !bot || !fight.Teacher.Valid)
        return false;
    TeacherLine const& line = LineOf(*fight.Way);
    if (line.Points.empty())
        return false;
    TeacherWay const& view = fight.Teacher;
    uint32 const legs = uint32(line.LegFirst.size());
    uint32 const first = line.LegFirst[std::min(view.Lower, legs - 1)];
    uint32 const end = line.LegEnd[std::min(view.Target, legs - 1)];
    if (first >= end)
        return false;
    uint32 const at = Nearest(line.Points, first, end, *bot);
    // How far along to go: the tank toward the target's end, the others toward the tank.
    uint32 goal = end - 1;
    if (!isTank)
    {
        if (!tank)
            return false;
        if (bot->GetExactDist2d(tank) <= TANK_DIRECT_YARDS
            && std::fabs(bot->GetPositionZ() - tank->GetPositionZ()) <= SAME_FLOOR_YARDS)
        {
            step = tank->GetPosition();
            return true;
        }
        goal = Nearest(line.Points, first, end, *tank);
    }
    if (goal == at)
    {
        step = isTank ? line.Points[goal] : tank->GetPosition();
        return true;
    }
    // AHEAD_YARDS along the line, forward or back, not past the goal.
    float const from = line.Along[at];
    uint32 index = at;
    if (goal > at)
        while (index < goal && line.Along[index] - from < AHEAD_YARDS)
            ++index;
    else
        while (index > goal && from - line.Along[index] < AHEAD_YARDS)
            --index;
    step = line.Points[index];
    return true;
}

bool Animus::Curriculum::InstanceEncounter::TeacherStepTo(Env const& env, Player const* bot, uint32 k, Position& step)
{
    EnvInstance& fight = _envs[env.Index];
    if (!fight.Way || !bot || !fight.Teacher.Valid)
        return false;
    TeacherLine const& line = LineOf(*fight.Way);
    uint32 const legs = uint32(line.LegFirst.size());
    if (line.Points.empty() || !legs)
        return false;
    TeacherWay const& view = fight.Teacher;
    k = std::min(k, legs - 1);
    uint32 const first = line.LegFirst[std::min({ view.Lower, k, legs - 1 })];
    uint32 const end = line.LegEnd[std::min(std::max(view.Target, k), legs - 1)];
    if (first >= end)
        return false;
    uint32 const at = Nearest(line.Points, first, end, *bot);
    uint32 const goal = std::clamp(line.LegEnd[k] - 1, first, end - 1);
    float const from = line.Along[at];
    uint32 index = at;
    if (goal > at)
        while (index < goal && line.Along[index] - from < AHEAD_YARDS)
            ++index;
    else
        while (index > goal && from - line.Along[index] < AHEAD_YARDS)
            --index;
    step = line.Points[index];
    return true;
}
