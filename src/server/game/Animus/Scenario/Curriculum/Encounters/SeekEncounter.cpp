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

#include "SeekEncounter.h"
#include "BotFactory.h"
#include "Camera.h"
#include "EncoderSupport.h"
#include "GoalBlock.h"
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapVisionWorld.h"
#include "Standing.h"
#include "ModelIgnoreFlags.h"
#include "ObjectPool.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SeekDraw.h"
#include "SightDraw.h"
#include "StageScenario.h"
#include "StageState.h"
#include "UnitBody.h"
#include "UnitDefines.h"
#include <algorithm>
#include <cmath>

namespace
{
    namespace Draw = Animus::Curriculum::SeekDraw;

    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// How far above the room's floor a spot's floor is looked for from, and how far down.
    constexpr float PROBE_ABOVE = 1.0f;
    constexpr float PROBE_DOWN = 4.0f;
    /// Knee height, where a spot's clearance is measured.
    constexpr float KNEE = 0.5f;
    /// A hallway placement looks for the object on the seat's own storey, within this of its height.
    constexpr float STOREY_RISE = 4.0f;
    /// The salts of an evaluation's seeded placement draws.
    enum Salt : uint32 { SALT_TRIANGLE = 1, SALT_A, SALT_B, SALT_FACING, SALT_DOOR_U, SALT_DOOR_V, SALT_STRIDE = 8 };

    /// A cell of floor as one key: x and y in `cell`-yard squares, z in storeys of three yards.
    uint64 CellKey(float x, float y, float z, float cell)
    {
        uint64 const cx = uint64(uint32(int32(std::floor(x / cell)))) & 0x1FFFFF;
        uint64 const cy = uint64(uint32(int32(std::floor(y / cell)))) & 0x1FFFFF;
        uint64 const cz = uint64(uint32(int32(std::floor(z / 3.0f)))) & 0x3FFFFF;
        return cx << 43 | cy << 22 | cz;
    }

    /// Explore's cell: the side of a floor cell, yards.
    constexpr float EXPLORE_CELL = 2.0f;
    /// The frontier (FrontierPull): searched within this of the seat every FRONTIER_PULL_MS, a cluster being a cell of
    /// FRONTIER_CLUSTER yards (SeenPlaces::SAME_PLACE) of the grid.
    constexpr float FRONTIER_PULL_RADIUS = 40.0f;
    constexpr float FRONTIER_PULL_STEP = 2.0f;
    constexpr uint32 FRONTIER_PULL_MS = 2000;
    /// The trap drill: how far from the door's middle the jamb is looked for, and the least left between the seat and
    /// the jamb it stands at; the seat's pose may face the wall within this much of straight on, radians.
    constexpr float TRAP_REACH = 4.0f;
    constexpr float TRAP_NEAR = 0.5f;
    constexpr float TRAP_FAR = 1.5f;
    constexpr float TRAP_FACING_SLACK = 0.5f;
    constexpr float TRAP_FLAT = 0.6f;
    constexpr uint32 TRAP_ATTEMPTS = 8;

    /// A back room or an end room by its name in the room table (`east_south_2_back`, `west_end_1`, `east_end_3_back`):
    /// the rooms the front doors do not show. The hubs and `hall_end` (a front cell) are neither.
    bool BackRoomName(std::string const& name)
    {
        return (name.size() > 5 && name.compare(name.size() - 5, 5, "_back") == 0)
            || name.find("_end_") != std::string::npos;
    }

    /// What the seat's own mental map knows of the ground at (x, y), for the frontier search.
    Animus::Curriculum::SeenPlaces::Ground MapGround(Animus::Vision::MentalMap const& map, float x, float y)
    {
        namespace Places = Animus::Curriculum::SeenPlaces;
        Animus::Vision::MapCell const* cell = map.Find(x, y);
        if (!cell || !Animus::Vision::Known(*cell))
            return Places::Ground::Unknown;
        if (cell->Flags & (Animus::Vision::MAP_WALL_LOW | Animus::Vision::MAP_WALL_HIGH | Animus::Vision::MAP_HAZARD))
            return Places::Ground::Shut;
        return cell->Floor[0] != Animus::Vision::NO_FLOOR
            || (cell->Flags & (Animus::Vision::MAP_FREE | Animus::Vision::MAP_VISITED))
            ? Places::Ground::Open : Places::Ground::Unknown;
    }

    float WrapAngle(float angle)
    {
        while (angle > float(M_PI))
            angle -= 2.0f * float(M_PI);
        while (angle < -float(M_PI))
            angle += 2.0f * float(M_PI);
        return angle;
    }

    std::array<uint32, Draw::RUNGS> RungSeconds(Animus::Curriculum::CurriculumTuning::SeekTuning const& tuning)
    {
        return { tuning.RungSeconds0, tuning.RungSeconds1, tuning.RungSeconds2, tuning.RungSeconds3 };
    }
}

Animus::Curriculum::SeekEncounter::SeekEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::SeekEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::StepCost, RewardTerm::Death, RewardTerm::Stuck, RewardTerm::Wall,
        RewardTerm::Sighting, RewardTerm::NewGround, RewardTerm::RoomSeen, RewardTerm::RoomGoal, RewardTerm::RoomSwitch,
        RewardTerm::Return, RewardTerm::CellGoal, RewardTerm::CellProgress, RewardTerm::CellSwitch,
        RewardTerm::CellLost, RewardTerm::CellStale, RewardTerm::Explore, RewardTerm::FrontierPull,
        RewardTerm::Circling, RewardTerm::Escape };
}

std::vector<std::string> Animus::Curriculum::SeekEncounter::RoomNames(ArenaDefinition const& arena)
{
    std::vector<std::string> names;
    for (SeekRoom const& room : arena.Rooms)
        names.push_back(room.Name);
    return names;
}

std::vector<std::string> Animus::Curriculum::SeekEncounter::ObjectNames(ArenaDefinition const& arena)
{
    std::vector<std::string> names;
    for (SeekObject const& object : arena.Objects)
        names.push_back(object.Kind);
    return names;
}

void Animus::Curriculum::SeekEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // `found` is the stage's measure: stopped beside the object before the clock ran out. The convergence and the
    // shaping fade (whose rungs are the placement ladder's) are gated on it (configs/move2_seek.yaml).
    table.Add("found", [this](Env const& env, uint32) { return _envs[env.Index].Found ? 1.0f : 0.0f; });
    // Per event (animus.episode_means.PER_EVENT): the clock at the arrival, over the episodes that found it.
    table.Add("find_seconds", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found ? float(seek.FoundMs) / 1000.0f : 0.0f;
    });
    // The first frame with any objective-flag pixel, over the episodes that saw it at all.
    table.Add("sighted", [this](Env const& env, uint32) { return _envs[env.Index].SightPaid ? 1.0f : 0.0f; });
    table.Add("sight_seconds", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.SightPaid ? float(seek.SightMs) / 1000.0f : 0.0f;
    });
    // From that first frame to the arrival, over the episodes that both saw it and found it.
    table.Add("found_sighted", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found && seek.SightPaid ? 1.0f : 0.0f;
    });
    table.Add("sight_to_arrival", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found && seek.SightPaid
            ? float(seek.FoundMs - std::min(seek.FoundMs, seek.SightMs)) / 1000.0f : 0.0f;
    });
    // The share of decisions whose frame showed the object.
    table.Add("objective_visible", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Decisions ? float(seek.VisibleDecisions) / float(seek.Decisions) : 0.0f;
    });
    // The rooms (SeekRoom floors) stepped into before the object was found or the clock ran out: distinct rooms,
    // returns to a room already entered (backtracking), the two together, and the share of entries that were returns
    // (the revisit rate, per entry). rooms_before_found is per episode that found it. rooms_looked: the rooms whose
    // floor a frame showed (RoomSeen's), entered or not.
    table.Add("rooms_entered", [this](Env const& env, uint32) { return float(_envs[env.Index].RoomsEntered); });
    table.Add("rooms_reentered", [this](Env const& env, uint32) { return float(_envs[env.Index].RoomsReentered); });
    table.Add("room_entries", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return float(seek.RoomsEntered + seek.RoomsReentered);
    });
    // The share of the visits that were returns (Seek.Return's definition: back into a visited room after
    // ReturnAwayMs outside it by ReturnAwayYards): the door's flicker is not one.
    table.Add("revisit_rate", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        uint32 visits = 0;
        for (RoomTrack const& room : seek.Track)
            visits += room.Visited ? 1 : 0;
        return visits + seek.Returns ? float(seek.Returns) / float(visits + seek.Returns) : 0.0f;
    });
    table.Add("rooms_before_found", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found ? float(seek.RoomsBeforeFound) : 0.0f;
    });
    table.Add("rooms_looked", [this](Env const& env, uint32) { return float(_envs[env.Index].RoomsLooked); });
    // What was hidden where: the room and the object (indexes into stage.json episode_categories; a hallway object's
    // room is the one whose opening is nearest it), the room's depth (its rank by walking distance, 0 the nearest), its
    // tier (thirds: `difficulty`, so the evaluation tables split by it) and whether it is the deepest third
    // (`deep_room`).
    table.Add("seek_room", [this](Env const& env, uint32) { return float(std::max(0, _envs[env.Index].Room)); });
    table.Add("seek_object", [this](Env const& env, uint32) { return float(_envs[env.Index].ObjectIndex); });
    table.Add("room_depth", [this](Env const& env, uint32) { return _envs[env.Index].Depth; });
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add("deep_room", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Tier + 1 >= Draw::TIERS ? 1.0f : 0.0f;
    });
    // **The ladder** (REDESIGN §2): the placement's rung (seek_rung, 0 the hallway to 3 deep) and the ladder's
    // (room_ladder, as a share of the top: 0, 1/3, 2/3, 1), whether this one was the carry-over from the rung below,
    // and at_top_rung (the ladder at deep: an evaluation's at the training rung, so it is only at the top when the
    // fade is), which convergence.top_rung waits for. Per rung, whether the episode placed there (rung_<name>) and
    // whether it found it (found_<name>, per event over rung_<name>: the status headline's found by rung).
    table.Add("seek_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    table.Add("room_ladder", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].LadderRung) / float(Draw::RUNGS - 1);
    });
    table.Add("rung_carried", [this](Env const& env, uint32) { return _envs[env.Index].Carried ? 1.0f : 0.0f; });
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].LadderRung + 1 >= Draw::RUNGS ? 1.0f : 0.0f;
    });
    for (uint32 rung = 0; rung < Draw::RUNGS; ++rung)
    {
        std::string const name = Draw::RUNG_NAMES[rung];
        table.Add("rung_" + name, [this, rung](Env const& env, uint32)
        {
            return _envs[env.Index].Rung == rung ? 1.0f : 0.0f;
        });
        table.Add("found_" + name, [this, rung](Env const& env, uint32)
        {
            EnvSeek const& seek = _envs[env.Index];
            return seek.Rung == rung && seek.Found ? 1.0f : 0.0f;
        });
    }
    table.Add("object_fallback", [this](Env const& env, uint32) { return _envs[env.Index].Fallback ? 1.0f : 0.0f; });
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });

    // **Room goals** (M2 goals plan): the share of decisions under a place goal, the room goals chosen, reached and
    // lost (a room slot; the way on is not one) and the share reached, the room goals given up for another place goal,
    // the rooms checked (and per minute of the episode), the returns, the most glimpsed rooms that waited for a slot,
    // and the seconds to the first place goal chosen (0 when none was).
    auto const seatOf = [this](Env const& env, uint32 seat) -> SeatState const&
    {
        return _scenario.Data(env).Seats[seat];
    };
    table.Add("plan_share", [this, seatOf](Env const& env, uint32 seat)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Decisions ? float(seatOf(env, seat).PlanDecisions) / float(seek.Decisions) : 0.0f;
    });
    table.Add("goals_room_chosen", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).RoomGoalsChosen);
    });
    table.Add("goals_room_reached", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).RoomGoalsReached);
    });
    table.Add("goals_room_lost", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).RoomGoalsLost);
    });
    // Reached over chosen, of whichever goals the episode offers (rooms or cells).
    table.Add("goal_follow_rate", [seatOf](Env const& env, uint32 seat)
    {
        SeatState const& state = seatOf(env, seat);
        uint32 const chosen = state.RoomGoalsChosen + state.CellGoalsChosen;
        return chosen ? float(state.RoomGoalsReached + state.CellGoalsReached) / float(chosen) : 0.0f;
    });
    table.Add("goal_switches_room", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).RoomGoalSwitches);
    });
    // **Cell goals** (free choice goals): the goals started (a hold made), reached and lost, the choices the sim could
    // not decode (a learner/sim desync: must stay 0) and the ones that were the goal held, chosen again (free), the
    // cell goals given up for another, the seconds a cell goal ran and the yards it was chosen at (means over the
    // goals), the share of blocks chosen that the seat had stood on, the plan's mean depth when a block was chosen,
    // the promotions that carried a reached goal on, and the cells of ground walked (the NewGround cells: exploration,
    // comparable across Seek.GoalSource).
    table.Add("goals_cell_chosen", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).CellGoalsChosen);
    });
    table.Add("goals_cell_reached", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).CellGoalsReached);
    });
    table.Add("goals_cell_lost", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).CellGoalsLost);
    });
    table.Add("goals_cell_invalid", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).CellGoalsInvalid);
    });
    table.Add("goals_cell_same", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).CellGoalsSame);
    });
    table.Add("goal_switches_cell", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).CellSwitches);
    });
    table.Add("cell_goal_seconds", [seatOf](Env const& env, uint32 seat)
    {
        // The goals that ended or were replaced, and the cell goals still running when the episode did.
        SeatState const& state = seatOf(env, seat);
        float seconds = state.CellSecondsSum;
        uint32 goals = state.CellGoalsRun;
        for (GoalHold const& hold : state.Holds)
            if (GoalBlock::IsCellGoal(hold.Goal) && !hold.Ended)
            {
                seconds += float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, hold.CellStartMs)) / 1000.0f;
                ++goals;
            }
        return goals ? seconds / float(goals) : 0.0f;
    });
    table.Add("cell_goal_yards", [seatOf](Env const& env, uint32 seat)
    {
        SeatState const& state = seatOf(env, seat);
        return state.CellGoalsChosen ? state.CellYardsSum / float(state.CellGoalsChosen) : 0.0f;
    });
    table.Add("cell_stale_share", [seatOf](Env const& env, uint32 seat)
    {
        SeatState const& state = seatOf(env, seat);
        return state.CellLatches ? float(state.CellLatchesStale) / float(state.CellLatches) : 0.0f;
    });
    table.Add("plan_depth", [seatOf](Env const& env, uint32 seat)
    {
        SeatState const& state = seatOf(env, seat);
        return state.PlanDepthSteps ? float(state.PlanDepthSum) / float(state.PlanDepthSteps) : 0.0f;
    });
    table.Add("plan_advances", [seatOf](Env const& env, uint32 seat)
    {
        return float(seatOf(env, seat).PlanAdvances);
    });
    table.Add("ground_cells", [this](Env const& env, uint32) { return float(_envs[env.Index].Cells.size()); });
    table.Add("rooms_checked", [this](Env const& env, uint32) { return float(_envs[env.Index].RoomsChecked); });
    table.Add("rooms_checked_per_min", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].RoomsChecked) * 60000.0f / float(std::max<uint32>(1, env.EpisodeElapsedMs));
    });
    table.Add("returns", [this](Env const& env, uint32) { return float(_envs[env.Index].Returns); });
    table.Add("slots_waiting", [this](Env const& env, uint32) { return float(_envs[env.Index].MaxWaiting); });
    table.Add("time_to_first_goal", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.FirstGoal ? float(seek.FirstGoalMs) / 1000.0f : 0.0f;
    });
    // The check rule's calibration, over the episodes that found the object: whether its room was checked before the
    // arrival (a miss of the rule), and how much of the room's floor the rays had hit at the arrival. Both 0 on an
    // episode that did not find it.
    table.Add("checked_miss", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found && seek.ObjectChecked && seek.FoundMs > seek.ObjectCheckedMs ? 1.0f : 0.0f;
    });
    table.Add("check_cover_at_find", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found ? seek.CoverAtFind : 0.0f;
    });
    // **Explore, don't circle, get unstuck** (decision 0023): the 2-yd cells of floor first landed on by a ray after
    // the first look, and the ones that paid the room bonus (a room not yet entered); the nominal Explore and
    // FrontierPull sums (before the scale and the floor: the reward_explore and reward_frontier_pull columns are what
    // was paid); the seconds charged as circling and the times it began; the trap drill's episode, whether the seat got
    // Seek.TrapEscapeYards away within Seek.TrapEscapeMs (per event over trap_episode) and when (per event over
    // trap_escaped); and the back and end rooms (by name) entered.
    table.Add("explore_cells", [this](Env const& env, uint32) { return float(_envs[env.Index].ExploreCells); });
    table.Add("explore_reward", [this](Env const& env, uint32) { return _envs[env.Index].ExploreNominal; });
    table.Add("explore_room_cells", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].ExploreRoomCells);
    });
    table.Add("frontier_pull_reward", [this](Env const& env, uint32) { return _envs[env.Index].FrontierNominal; });
    table.Add("circling_seconds", [this](Env const& env, uint32) { return _envs[env.Index].CirclingSeconds; });
    table.Add("circling_events", [this](Env const& env, uint32) { return float(_envs[env.Index].CirclingEvents); });
    table.Add("trap_episode", [this](Env const& env, uint32) { return _envs[env.Index].Trap ? 1.0f : 0.0f; });
    table.Add("trap_escaped", [this](Env const& env, uint32) { return _envs[env.Index].Escaped ? 1.0f : 0.0f; });
    table.Add("trap_escape_seconds", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Escaped ? float(seek.EscapeMs) / 1000.0f : 0.0f;
    });
    table.Add("back_room_visits", [this](Env const& env, uint32) { return float(_envs[env.Index].BackRoomVisits); });
    // Where the object stood and where the seat's last position was, absolute coordinates in the instance (yards), so
    // the routes can be analysed offline.
    table.Add("object_x", [this](Env const& env, uint32) { return _envs[env.Index].Spot.GetPositionX(); });
    table.Add("object_y", [this](Env const& env, uint32) { return _envs[env.Index].Spot.GetPositionY(); });
    table.Add("object_z", [this](Env const& env, uint32) { return _envs[env.Index].Spot.GetPositionZ(); });
    table.Add("end_x", [this](Env const& env, uint32) { return _envs[env.Index].LastX; });
    table.Add("end_y", [this](Env const& env, uint32) { return _envs[env.Index].LastY; });
    table.Add("end_z", [this](Env const& env, uint32) { return _envs[env.Index].LastZ; });
}

void Animus::Curriculum::SeekEncounter::ResetEpisode(Env& env)
{
    // The object stays in the world until the next Build removes it (it has the map); the episode's measures go.
    EnvSeek& seek = _envs[env.Index];
    ObjectGuid const object = seek.Object;
    bool const placed = seek.Placed;
    seek = EnvSeek();
    seek.Object = object;
    seek.Placed = placed;
}

bool Animus::Curriculum::SeekEncounter::Summon(EnvSeek& seek, Map* map, ArenaDefinition const& arena,
    Position const& spot, uint32 phase) const
{
    SeekObject const& kind = arena.Objects[seek.ObjectIndex];
    GameObject* object = ObjectPool::Summon(map, kind, spot, phase);
    if (!object)
    {
        LOG_ERROR("module.animus", "{}: the seek object {} ({}) could not be spawned at ({:.1f} {:.1f} {:.1f})",
            _scenario.Name(), kind.Entry, kind.Kind, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
        return false;
    }
    seek.Object = object->GetGUID();
    seek.Placed = true;
    seek.Spot = spot;
    seek.Centre = Position(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ() + kind.Height * 0.5f,
        spot.GetOrientation());
    return true;
}

bool Animus::Curriculum::SeekEncounter::Place(Env const& env, EnvSeek& seek, Map* map, ArenaDefinition const& arena,
    uint32 phase) const
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    SeekRoom const& room = arena.Rooms[uint32(seek.Room)];
    bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED;
    auto const uniform = [&](uint32 attempt, uint32 salt)
    {
        return seeded ? Draw::SeedUniform(env.EpisodeSeedIndex, attempt * SALT_STRIDE + salt) : frand(0.0f, 1.0f);
    };

    // A spot on the room's floor: the floor under it within FloorTolerance of the room's (not a step, a crate or a
    // pit), and nothing solid within Clearance along the axes at knee height (not inside a pillar or against a wall).
    auto const clear = [&](float x, float y, float z)
    {
        static float const AXES[4][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
        for (auto const& axis : AXES)
            if (!map->isInLineOfSight(x, y, z + KNEE, x + axis[0] * tuning.Clearance, y + axis[1] * tuning.Clearance,
                z + KNEE, phase, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;
        return true;
    };
    auto const floorAt = [&](float x, float y)
    {
        return map->GetHeight(x, y, room.FloorZ + PROBE_ABOVE, true, PROBE_DOWN);
    };

    bool found = false;
    float x = room.Centre.first;
    float y = room.Centre.second;
    float z = room.FloorZ;
    // The doorway rung: just inside the opening, toward the room's centre (SeekDraw::DoorwaySpot), visible from the
    // hallway when the seat looks in.
    if (seek.Rung == uint32(Draw::Rung::Doorway))
        for (uint32 attempt = 0; attempt < std::max<uint32>(1, tuning.Attempts) && !found; ++attempt)
        {
            auto const [px, py] = Draw::DoorwaySpot(room, tuning.DoorwayInside, tuning.DoorwayDeeper,
                tuning.DoorwaySpread, uniform(attempt, SALT_DOOR_U), uniform(attempt, SALT_DOOR_V));
            float const pz = floorAt(px, py);
            if (std::fabs(pz - room.FloorZ) > tuning.FloorTolerance || !clear(px, py, pz))
                continue;
            x = px;
            y = py;
            z = pz;
            found = true;
        }
    for (uint32 attempt = 0; attempt < std::max<uint32>(1, tuning.Attempts) && !found; ++attempt)
    {
        auto const [px, py] = Draw::PointIn(room.Floor, uniform(attempt, SALT_TRIANGLE), uniform(attempt, SALT_A),
            uniform(attempt, SALT_B));
        float const pz = floorAt(px, py);
        if (std::fabs(pz - room.FloorZ) > tuning.FloorTolerance || !clear(px, py, pz))
            continue;
        x = px;
        y = py;
        z = pz;
        found = true;
    }
    if (!found)
    {
        float const centre = floorAt(x, y);
        if (std::fabs(centre - room.FloorZ) <= tuning.FloorTolerance)
            z = centre;
    }
    seek.Fallback = !found;

    float const facing = uniform(0, SALT_FACING) * TWO_PI;
    return Summon(seek, map, arena, Position(x, y, z, facing), phase);
}

bool Animus::Curriculum::SeekEncounter::PlaceInHallway(EnvSeek& seek, Map* map, ArenaDefinition const& arena,
    Player* bot, Position const& start) const
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    std::vector<Position> const& points = arena.SpawnPoints;
    SeekObject const& kind = arena.Objects[seek.ObjectIndex];
    SightDraw::Viewing viewing;
    viewing.EyeRise = Vision::PIVOT_SHARE * Movement::ShapeOf(bot).Height;
    viewing.CentreRise = kind.Height * 0.5f;
    viewing.Radius = kind.Radius;
    viewing.Nearest = tuning.HallwayNearest;
    viewing.Furthest = tuning.HallwayFurthest;
    viewing.StoreyRise = STOREY_RISE;
    viewing.Attempts = std::max<uint32>(1, tuning.Attempts);
    // In sight of the seat's eye, as M1 places its object: an evaluation's frand is its seed's (the world thread's
    // random numbers are reseeded before each seeded episode), so it meets the same spots.
    Vision::MapVisionWorld const world(map, bot->GetPhaseMask());
    SightDraw::Placement const placement = SightDraw::Place(points, start, false, viewing, world,
        []() { return frand(0.0f, 1.0f); });
    if (placement.Point < 0)
        return false;
    Position const& at = points[uint32(placement.Point)];
    seek.Room = Draw::NearestOpening(arena.Rooms, at.GetPositionX(), at.GetPositionY());
    return Summon(seek, map, arena, Position(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(),
        frand(0.0f, TWO_PI)), bot->GetPhaseMask());
}

bool Animus::Curriculum::SeekEncounter::TrapPose(Map* map, ArenaDefinition const& arena, Player* bot,
    Position& pose) const
{
    // A door of the room table at random; its axis runs from the opening (the middle of the doorway, on the cells the
    // room shares with the region it is entered from) toward the room's centre. The seat stands TRAP_NEAR to TRAP_FAR
    // yards from the wall that bounds the doorway on one side (the jamb: the nearest solid across the axis, found by
    // the camera's static ray at knee height), 0.5-1.5 yd along the axis from the opening, facing it. Valid when the
    // floor there (vmaps) is the room's within FloorTolerance and level with the doorway's, and the line from the pose
    // back to the opening is clear: a way out exists, straight, but the seat is not facing it. The doorway's cells
    // are walkable ones of the offline navmesh scan and the pose is a point of them at least TRAP_NEAR from a wall,
    // so it is on navmesh the stock walker would also stand on; nothing here asks the navmesh (reset cost: ray casts).
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    if (arena.Rooms.empty())
        return false;
    uint32 const phase = bot->GetPhaseMask();
    Vision::MapVisionWorld const world(map, phase);
    for (uint32 attempt = 0; attempt < TRAP_ATTEMPTS; ++attempt)
    {
        SeekRoom const& room = arena.Rooms[urand(0, uint32(arena.Rooms.size()) - 1)];
        float dx = room.Centre.first - room.Opening.first;
        float dy = room.Centre.second - room.Opening.second;
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length < 1.0f)
            continue;
        dx /= length;
        dy /= length;
        float const along = frand(TRAP_NEAR, TRAP_FAR);
        float const qx = room.Opening.first + dx * along;
        float const qy = room.Opening.second + dy * along;
        float const qz = map->GetHeight(qx, qy, room.FloorZ + PROBE_ABOVE, true, PROBE_DOWN);
        if (std::fabs(qz - room.FloorZ) > tuning.FloorTolerance)
            continue;

        float const side = urand(0, 1) ? 1.0f : -1.0f;
        for (float sign : { side, -side })
        {
            float const px = -dy * sign;
            float const py = dx * sign;
            Vision::SurfaceHit const jamb = world.StaticHit(Vision::Vec3{ qx, qy, qz + KNEE },
                Vision::Vec3{ qx + px * TRAP_REACH, qy + py * TRAP_REACH, qz + KNEE });
            float const gap = frand(TRAP_NEAR, TRAP_FAR);
            if (jamb.Distance < 0.0f || jamb.Distance - gap < TRAP_NEAR * 0.5f)
                continue;
            float const x = qx + px * (jamb.Distance - gap);
            float const y = qy + py * (jamb.Distance - gap);
            float const z = map->GetHeight(x, y, qz + PROBE_ABOVE, true, PROBE_DOWN);
            if (std::fabs(z - qz) > TRAP_FLAT || std::fabs(z - room.FloorZ) > tuning.FloorTolerance)
                continue;
            // The way out: the opening in the clear from the pose, at knee height.
            if (world.StaticAnyHit(Vision::Vec3{ x, y, z + KNEE },
                Vision::Vec3{ room.Opening.first, room.Opening.second, z + KNEE }))
                continue;
            float const facing = std::atan2(py, px) + frand(-TRAP_FACING_SLACK, TRAP_FACING_SLACK);
            pose = Position(x, y, z, facing < 0.0f ? facing + TWO_PI : facing);
            return true;
        }
    }
    return false;
}

bool Animus::Curriculum::SeekEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;

    ArenaDefinition const& arena = _scenario.Arena(env);
    if (arena.Rooms.empty() || arena.Objects.empty())
        return false;

    EnvSeek& seek = _envs[env.Index];
    ObjectPool::Remove(map, seek.Object);
    seek = EnvSeek();
    // The dungeon's own game objects -- its chests, the Hallow's End pumpkins -- out of the way: the one object in it
    // is the one to find.
    ObjectPool::ClearOwn(map);

    // The ladder (REDESIGN §2): the fade's rung, the placement's (a tenth of training episodes the rung below), and
    // the rooms that rung draws from. An evaluation plays the training rung's rooms in turn; the held-out sweep every
    // (room, object) pair at the top rung.
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED;
    seek.Sweep = arena.EvalOnly;
    Draw::Rung const ladder = seek.Sweep ? Draw::Rung::Deep : Draw::Rung(SightDraw::Rung(_scenario.ShapingScale()));
    seek.LadderRung = uint32(ladder);
    Draw::Rung placed = ladder;
    if (seek.Sweep && seeded)
    {
        auto const [room, object] = Draw::EvaluationPick(env.EpisodeSeedIndex, uint32(arena.Rooms.size()),
            uint32(arena.Objects.size()));
        seek.Room = int32(room);
        seek.ObjectIndex = object;
    }
    else if (seeded)
    {
        auto const [room, object] = Draw::RungEvaluationPick(env.EpisodeSeedIndex,
            Draw::EvaluationRooms(arena.Rooms, ladder), uint32(arena.Objects.size()));
        seek.Room = int32(room);
        seek.ObjectIndex = object;
    }
    else
    {
        placed = Draw::PlacedRung(ladder, frand(0.0f, 1.0f), tuning.CarryShare);
        std::vector<uint32> const rooms = Draw::RungRooms(arena.Rooms, placed);
        seek.Room = rooms.empty() ? int32(urand(0, uint32(arena.Rooms.size()) - 1))
            : int32(rooms[urand(0, uint32(rooms.size()) - 1)]);
        seek.ObjectIndex = urand(0, uint32(arena.Objects.size()) - 1);
    }
    seek.Rung = uint32(placed);
    seek.Carried = placed != ladder;

    // Facing a random way where the scenario put the seat (a random hallway point, the entrance among them). The
    // scenario reads the seat's facing from the bot after the encounters are built.
    Position const& spawn = _scenario.SpawnPointFor(env);
    Position const start(spawn.GetPositionX(), spawn.GetPositionY(), spawn.GetPositionZ(), frand(0.0f, TWO_PI));
    // The trap drill (Seek.TrapShare): a training episode starts, now and then, with the seat against the jamb of a
    // door of the room table, facing it. Never an evaluation's (it would move `found`, the fade's gate), and the
    // hallway rung's object stays in sight of the spawn, not of the pose: the drill is for the way out.
    Position pose = start;
    if (!seeded && !seek.Sweep && tuning.TrapShare > 0.0f && frand(0.0f, 1.0f) < tuning.TrapShare
        && TrapPose(map, arena, bot, pose))
    {
        seek.Trap = true;
        seek.TrapStart = pose;
    }
    BotFactory::TeleportWithinMap(bot, pose);

    bool built = false;
    if (placed == Draw::Rung::Hallway)
        built = PlaceInHallway(seek, map, arena, bot, start);
    if (!built)
    {
        // A hallway with no point in sight falls back to a front cell's doorway.
        if (placed == Draw::Rung::Hallway)
        {
            std::vector<uint32> const front = Draw::RungRooms(arena.Rooms, Draw::Rung::Doorway);
            if (!front.empty())
                seek.Room = int32(front[urand(0, uint32(front.size()) - 1)]);
            seek.Rung = uint32(Draw::Rung::Doorway);
        }
        built = Place(env, seek, map, arena, bot->GetPhaseMask());
    }
    // The episode's clock is its placement rung's (as placed: a hallway with nothing in sight is a doorway's): short
    // early episodes make the reward come often.
    env.EpisodeLengthMs = (seek.Sweep ? std::max<uint32>(1, arena.EpisodeSeconds)
        : Draw::RungSeconds(Draw::Rung(seek.Rung), RungSeconds(tuning))) * IN_MILLISECONDS;
    if (seek.Room >= 0 && uint32(seek.Room) < arena.Rooms.size())
    {
        seek.Depth = Draw::Depths(arena.Rooms)[uint32(seek.Room)];
        seek.Tier = Draw::Tier(seek.Depth);
    }
    seek.Entered.assign(arena.Rooms.size(), false);
    seek.Looked.assign(arena.Rooms.size(), false);
    // The room goals' bookkeeping: a track per room, the cells of its floor, and whether this episode offers them.
    seek.Track.assign(arena.Rooms.size(), RoomTrack());
    for (std::size_t index = 0; index < arena.Rooms.size(); ++index)
        seek.Track[index].Floor = Draw::FloorCells(arena.Rooms[index].Floor);
    seek.RoomGoals = tuning.Goals && seek.Rung >= tuning.GoalsFromRung;
    return built;
}

bool Animus::Curriculum::SeekEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seat acts without a target (SeatEncoder::ActsWithoutTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::SeekEncounter::View(Env const& env, uint32 seatIndex, SeatView& view) const
{
    // The objective is the camera's to show (its flag, in line of sight) and nothing else's: the stage has no compass,
    // and no other block of it reads the point (the goal block gives it no place: ObjectivePlaceKnown).
    EnvSeek const& seek = _envs[env.Index];
    view.HasObjective = seek.Placed && !seek.Found;
    view.Objective = seek.Centre;
    // The flag on the object itself: its own radius and a quarter yard, not a yard round its centre.
    ArenaDefinition const& arena = _scenario.Arena(env);
    if (seek.ObjectIndex < arena.Objects.size())
        view.ObjectiveRadius = Vision::ObjectiveRadiusFor(arena.Objects[seek.ObjectIndex].Radius);

    // Room goals (Seek.Goals): the places are what the seat's own frames showed and its map's frontier -- the stage
    // has no assignment, so the dud goal about the objective (GoalBlock::Available) goes with the seen places.
    WorldView& world = view.World;
    world.Places = {};
    world.HasAssignment = false;
    world.HasSeenPlaces = _scenario.Tuning().Seek.Goals != 0;
    world.RoomGoals = seek.Placed && seek.RoomGoals && view.Bot && view.Bot->IsAlive();
    world.SearchGoals = world.RoomGoals && _scenario.Tuning().Seek.SearchGoals != 0;
    // Cell goals (Seek.GoalSource 1): the goal is a block of the seat's own crop, so there are no room slots and no
    // way on to fill; RoomGoals stays true beside it (the gates that exempt a place goal from the straight-line gap).
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    world.CellGoals = world.RoomGoals && tuning.GoalSource == 1;
    world.CellReach = tuning.CellReach;
    world.CellRise = tuning.CellRise;
    if (!world.RoomGoals || world.CellGoals || seek.Track.size() != arena.Rooms.size())
        return;

    // The rooms: the mean of the floor the seat's rays hit, never the room table's centre or opening.
    for (uint32 slot = 0; slot < GOAL_ROOM_SLOTS; ++slot)
    {
        int32 const index = seek.SlotRoom[slot];
        if (index < 0)
            continue;
        RoomTrack const& room = seek.Track[uint32(index)];
        if (!room.Hits)
            continue;
        WorldView::JournalPlace& place = world.Places[slot];
        place.Present = true;
        place.Where.Relocate(float(room.SumX / room.Hits), float(room.SumY / room.Hits), float(room.SumZ / room.Hits));
        place.Done = room.Checked;
        place.Coverage = std::min(1.0f, float(room.Hit.size()) / float(std::max<uint32>(1, room.Floor)));
        place.Age = std::min(1.0f, float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, room.GlimpseMs))
            / (1000.0f * GoalBlock::AGE_SCALE_S));
    }

    // The way on: the nearest frontier of its own mental map (known open ground beside ground it has not seen), as a
    // dungeon stage's (InstanceEncounter::SeenWorld), refreshed every FRONTIER_MS.
    if (!view.Bot || seatIndex >= MAX_SEATS)
        return;
    constexpr uint32 FRONTIER_MS = 2000;
    constexpr float FRONTIER_RADIUS = 40.0f;
    constexpr float FRONTIER_STEP = 2.0f;
    Player const* bot = view.Bot;
    SeenPlaces::Point const at{ bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    if (!seek.FrontierReady || env.EpisodeElapsedMs < seek.FrontierMs
        || env.EpisodeElapsedMs >= seek.FrontierMs + FRONTIER_MS)
    {
        Vision::MentalMap const& map = _scenario.Data(env).Seats[seatIndex].Map;
        seek.Frontier = SeenPlaces::Frontier(at, FRONTIER_RADIUS, FRONTIER_STEP, 1, [&map](float x, float y)
        {
            Vision::MapCell const* cell = map.Find(x, y);
            if (!cell || !Vision::Known(*cell))
                return SeenPlaces::Ground::Unknown;
            if (cell->Flags & (Vision::MAP_WALL_LOW | Vision::MAP_WALL_HIGH | Vision::MAP_HAZARD))
                return SeenPlaces::Ground::Shut;
            return cell->Floor[0] != Vision::NO_FLOOR || (cell->Flags & (Vision::MAP_FREE | Vision::MAP_VISITED))
                ? SeenPlaces::Ground::Open : SeenPlaces::Ground::Unknown;
        });
        seek.FrontierMs = env.EpisodeElapsedMs;
        seek.FrontierReady = true;
    }
    if (!seek.Frontier.empty())
    {
        WorldView::JournalPlace& wayOn = world.Places[WorldView::WAY_ON_SLOT];
        wayOn.Present = true;
        wayOn.Where.Relocate(seek.Frontier.front().X, seek.Frontier.front().Y, seek.Frontier.front().Z);
        wayOn.Coverage = 1.0f;
    }
}

int32 Animus::Curriculum::SeekEncounter::AchievedGoal(Env const& env, uint32 /*seat*/) const
{
    // The room that was checked at this decision, whatever the seat pursued: the goal it would have been (hindsight),
    // of the kind the stage offers its places as (search, or travel_to at Seek.SearchGoals 0).
    // Not in cell mode: a room slot's id would be a lie there, and the planner's hindsight reads the goal block's
    // choice-frame columns instead.
    EnvSeek const& seek = _envs[env.Index];
    bool const rooms = seek.RoomGoals && _scenario.Tuning().Seek.GoalSource == 0;
    SeatGoal const kind = _scenario.Tuning().Seek.SearchGoals != 0 ? SeatGoal::Search : SeatGoal::TravelTo;
    return rooms && seek.AchievedSlot >= 0 ? MakeGoal(kind, GOAL_TARGET_PLACE_FIRST + uint32(seek.AchievedSlot))
        : NO_GOAL;
}

void Animus::Curriculum::SeekEncounter::Explore(Env const& env, EnvSeek& seek, ArenaDefinition const& arena,
    SeatState const& seat, Player* bot, bool paid, RewardLedger& ledger)
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;

    // The floor the frame landed on, a cell (2 yd, by storey) at a time: the episode's own record of what its rays
    // reached, beside the mental map, which outlives the episode (a map kept from before would show everything seen
    // already: amendment 6). A cell new to the record pays ExploreSeen, times ExploreRoomBonus when it lies in a room
    // of the table the seat has not entered.
    float nominal = 0.0f;
    uint64 last = ~uint64(0);
    Draw::FloorRays(seat.Hits, [&](float x, float y, float z)
    {
        uint64 const key = CellKey(x, y, z, EXPLORE_CELL);
        if (key == last || !seek.Seen.insert(key).second)
            return;
        last = key;
        if (!paid)
            return;
        ++seek.ExploreCells;
        float weight = 1.0f;
        int32 const room = Draw::RoomAt(arena.Rooms, x, y, z);
        if (room >= 0 && uint32(room) < seek.Entered.size() && !seek.Entered[uint32(room)])
        {
            weight = tuning.ExploreRoomBonus;
            ++seek.ExploreRoomCells;
        }
        nominal += tuning.ExploreSeen * weight;
    });
    if (nominal > 0.0f)
    {
        float const pay = std::min(nominal, std::max(0.0f, tuning.ExploreCap - seek.ExploreNominal));
        if (pay > 0.0f)
        {
            seek.ExploreNominal += pay;
            ledger.Add(RewardTerm::Explore, pay);
        }
    }

    // The way on: the nearest frontier of the seat's own map (known open ground beside ground it has not seen),
    // looked for every FRONTIER_PULL_MS. A yard closed on it pays FrontierPull once per frontier cluster (the 10-yd
    // cell of its point): the best distance reached to each is kept, so a new cluster starts at the distance it is
    // first met at and walking back and forth between two pays only what was never closed before.
    if (tuning.FrontierPull <= 0.0f || !bot)
        return;
    SeenPlaces::Point const at{ bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    if (!seek.FrontierChecked || env.EpisodeElapsedMs < seek.FrontierCheckedMs
        || env.EpisodeElapsedMs >= seek.FrontierCheckedMs + FRONTIER_PULL_MS)
    {
        Vision::MentalMap const& map = seat.Map;
        std::vector<SeenPlaces::Point> const frontier = SeenPlaces::Frontier(at, FRONTIER_PULL_RADIUS,
            FRONTIER_PULL_STEP, 1, [&map](float x, float y) { return MapGround(map, x, y); });
        seek.FrontierChecked = true;
        seek.FrontierCheckedMs = env.EpisodeElapsedMs;
        seek.HasFrontier = !frontier.empty();
        if (seek.HasFrontier)
        {
            seek.FrontierAt = frontier.front();
            seek.FrontierKey = CellKey(seek.FrontierAt.X, seek.FrontierAt.Y, seek.FrontierAt.Z,
                SeenPlaces::SAME_PLACE);
        }
    }
    if (!seek.HasFrontier)
        return;
    float const distance = std::hypot(seek.FrontierAt.X - at.X, seek.FrontierAt.Y - at.Y);
    auto const [best, fresh] = seek.FrontierBest.emplace(seek.FrontierKey, distance);
    if (fresh || distance >= best->second)
        return;
    float const closed = best->second - distance;
    best->second = distance;
    if (!paid)
        return;
    float const pay = std::min(closed * tuning.FrontierPull,
        std::max(0.0f, tuning.FrontierCap - seek.FrontierNominal));
    if (pay > 0.0f)
    {
        seek.FrontierNominal += pay;
        ledger.Add(RewardTerm::FrontierPull, pay);
    }
}

void Animus::Curriculum::SeekEncounter::Circle(Env const& env, EnvSeek& seek, Player* bot, float moved, bool stuck,
    RewardLedger& ledger)
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    if (tuning.Circling <= 0.0f || !tuning.CircleWindowMs)
        return;

    // The window: the decisions of the last CircleWindowMs (the oldest kept one is where the net displacement is
    // measured from; the steps and turns are the ones taken since it).
    CircleSample sample;
    sample.Ms = env.EpisodeElapsedMs;
    sample.X = bot->GetPositionX();
    sample.Y = bot->GetPositionY();
    sample.Yaw = bot->GetOrientation();
    sample.Step = moved;
    if (!seek.Circle.empty())
        sample.Turn = std::fabs(WrapAngle(sample.Yaw - seek.Circle.back().Yaw));
    seek.Circle.push_back(sample);
    while (seek.CircleHead + 1 < seek.Circle.size()
        && sample.Ms - std::min(sample.Ms, seek.Circle[seek.CircleHead].Ms) > tuning.CircleWindowMs)
        ++seek.CircleHead;
    if (seek.CircleHead >= 128)
    {
        seek.Circle.erase(seek.Circle.begin(), seek.Circle.begin() + std::ptrdiff_t(seek.CircleHead));
        seek.CircleHead = 0;
    }

    CircleSample const& first = seek.Circle[seek.CircleHead];
    float path = 0.0f;
    float turn = 0.0f;
    for (std::size_t index = seek.CircleHead + 1; index < seek.Circle.size(); ++index)
    {
        path += seek.Circle[index].Step;
        turn += seek.Circle[index].Turn;
    }
    float const net = std::hypot(sample.X - first.X, sample.Y - first.Y);
    bool const holds = net < tuning.CircleNetYards
        && (path >= tuning.CircleYards || turn * 180.0f / float(M_PI) >= tuning.CircleTurnDeg);
    if (holds && !seek.CircleOn)
        ++seek.CirclingEvents;
    seek.CircleOn = holds;
    if (!holds || stuck)
        return;
    float const seconds = float(_scenario.StepMs(env)) / 1000.0f;
    seek.CirclingSeconds += seconds;
    ledger.AddFixed(RewardTerm::Circling, -tuning.Circling * seconds);
}

void Animus::Curriculum::SeekEncounter::TrackRooms(Env const& env, EnvSeek& seek, ArenaDefinition const& arena,
    Player* bot, int32 room, std::vector<uint32> const& counts, RewardLedger& ledger)
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    uint32 const stepMs = _scenario.StepMs(env);
    uint32 const now = env.EpisodeElapsedMs;
    if (seek.Track.size() != arena.Rooms.size() || counts.size() != arena.Rooms.size())
        return;

    // A room checked at an earlier decision has been shown as done: its slot is free now.
    for (int32& index : seek.SlotRoom)
        if (index >= 0 && seek.Track[uint32(index)].Checked)
        {
            seek.Track[uint32(index)].Slot = -1;
            index = -1;
        }

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    for (uint32 index = 0; index < seek.Track.size(); ++index)
    {
        RoomTrack& track = seek.Track[index];
        // Glimpsed: a frame with GlimpseRays floor rays on the room.
        if (!track.Glimpsed && !track.Checked && counts[index] >= std::max<uint32>(1, tuning.GlimpseRays))
        {
            track.Glimpsed = true;
            track.GlimpseMs = now;
            track.Order = seek.Glimpses++;
        }

        // The visits: standing in the room for EnterDwellMs makes it visited.
        bool const inside = int32(index) == room;
        track.DwellMs = inside ? track.DwellMs + stepMs : 0;
        if (track.DwellMs >= tuning.EnterDwellMs)
            track.Visited = true;

        // Checked: visited, or CheckedShare of its floor cells hit by this episode's rays.
        if (!track.Checked && (track.Visited || (counts[index] && tuning.CheckedShare <= 1.0f
            && float(track.Hit.size()) >= tuning.CheckedShare * float(track.Floor))))
        {
            track.Checked = true;
            track.CheckedMs = now;
            ++seek.RoomsChecked;
            if (int32(index) == seek.Room && !seek.ObjectChecked)
            {
                seek.ObjectChecked = true;
                seek.ObjectCheckedMs = now;
            }
            if (track.Slot >= 0 && seek.AchievedSlot < 0)
                seek.AchievedSlot = track.Slot;
        }

        // The returns: back into a visited room after ReturnAwayMs outside its polygon by ReturnAwayYards or more.
        if (!track.Visited)
            continue;
        if (inside)
        {
            if (track.Armed)
            {
                track.Armed = false;
                ++seek.Returns;
                ledger.AddFixed(RewardTerm::Return, -tuning.Return);
            }
            track.AwayMs = 0;
            continue;
        }
        SeekRoom const& table = arena.Rooms[index];
        bool const far = std::fabs(z - table.FloorZ) > 4.0f
            || Draw::OutsideBy(table.Floor, x, y) >= tuning.ReturnAwayYards;
        if (far)
        {
            track.AwayMs += stepMs;
            track.Armed = track.Armed || track.AwayMs >= tuning.ReturnAwayMs;
        }
        else if (!track.Armed)
            track.AwayMs = 0;
    }

    // The slots: the rooms glimpsed and not checked, in the order of the glimpses, into the free slots; the rest wait.
    std::vector<uint32> waiting;
    for (uint32 index = 0; index < seek.Track.size(); ++index)
        if (seek.Track[index].Glimpsed && !seek.Track[index].Checked && seek.Track[index].Slot < 0)
            waiting.push_back(index);
    std::sort(waiting.begin(), waiting.end(), [&seek](uint32 a, uint32 b)
    {
        return seek.Track[a].Order < seek.Track[b].Order;
    });
    std::size_t next = 0;
    for (uint32 slot = 0; slot < GOAL_ROOM_SLOTS && next < waiting.size(); ++slot)
        if (seek.SlotRoom[slot] < 0)
        {
            seek.SlotRoom[slot] = int32(waiting[next]);
            seek.Track[waiting[next++]].Slot = int32(slot);
        }
    seek.MaxWaiting = std::max<uint32>(seek.MaxWaiting, uint32(waiting.size() - next));
}

void Animus::Curriculum::SeekEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.StepScale(env));
    // Explore and FrontierPull are paid at max(the shaping scale, this): the fade leaves them a floor (decision 0023).
    ledger.SetShapingFloor(tuning.ExploreFloor);

    EnvSeek& seek = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    seek.AchievedSlot = -1;
    if (!bot)
        return;

    float moved = 0.0f;
    if (seek.HasLastPos)
    {
        float const dx = bot->GetPositionX() - seek.LastX;
        float const dy = bot->GetPositionY() - seek.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
        seek.Travelled += moved;
    }
    bool const firstLook = !seek.HasLastPos;
    seek.LastX = bot->GetPositionX();
    seek.LastY = bot->GetPositionY();
    seek.LastZ = bot->GetPositionZ();
    seek.HasLastPos = true;

    CombatTally& tally = seat.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (seek.Placed && !seek.Found && timeIsUp)
        tally.TimedOut = true;
    if (!seek.Placed || seek.Found || !bot->IsAlive())
        return;

    // The costs, read off the controller's counts as the ground course reads them: on from the first step at their
    // own fixed price (REDESIGN §2), off the cost ladder.
    uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, seek.LastStuckMs);
    uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, seek.LastWallMs);
    seek.LastStuckMs = seat.StuckMs;
    seek.LastWallMs = seat.WallMs;
    if (stuckMs)
        ledger.AddFixed(RewardTerm::Stuck, -tuning.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.StepMs(env)) / 1000.0f;
        float const charge = Standing::WallCharge(float(wallMs) / 1000.0f, moved, asked, tuning.Wall,
            tuning.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
    }

    // Going round in circles (not on a decision that paid Stuck: that second is priced already), and the trap
    // drill's way out: Escape once, TrapEscapeYards from the pose within TrapEscapeMs of the start.
    Circle(env, seek, bot, moved, stuckMs > 0, ledger);
    if (seek.Trap && !seek.Escaped && env.EpisodeElapsedMs <= tuning.TrapEscapeMs
        && bot->GetExactDist2d(&seek.TrapStart) >= tuning.TrapEscapeYards)
    {
        seek.Escaped = true;
        seek.EscapeMs = env.EpisodeElapsedMs;
        ledger.Add(RewardTerm::Escape, tuning.Escape);
    }

    // What the camera showed, the frame the seat decided on (StageScenario::ObserveSeat counts the flag's pixels):
    // the first sighting is paid once, as shaping.
    ++seek.Decisions;
    seek.VisibleDecisions += seat.ObjectivePixels ? 1 : 0;
    if (seat.ObjectiveSighted && !seek.SightPaid)
    {
        seek.SightPaid = true;
        seek.SightMs = seat.ObjectiveSightMs;
        ledger.Add(RewardTerm::Sighting, tuning.Sighting);
    }

    // Looked into a room (REDESIGN §2): the first frame this episode whose cast rays show a room's floor, from the
    // frame alone and the episode's own list (amendment 6): a mental map kept from an earlier episode, which already
    // knows the room, takes nothing away. The same rays give the room goals their places: the floor cells and the
    // points each room's rays hit.
    ArenaDefinition const& arena = _scenario.Arena(env);
    std::vector<uint32> counts(arena.Rooms.size(), 0);
    if (!seat.Hits.Rays.empty() && seek.Track.size() == arena.Rooms.size())
    {
        Draw::FloorHits(arena.Rooms, seat.Hits, 4.0f, [&](uint32 index, float hitX, float hitY, float hitZ)
        {
            ++counts[index];
            RoomTrack& track = seek.Track[index];
            track.Hit.insert(Draw::CoverCell(hitX, hitY));
            track.SumX += hitX;
            track.SumY += hitY;
            track.SumZ += hitZ;
            ++track.Hits;
        });
        for (uint32 room : Draw::NewlyLooked(counts, seek.Looked, tuning.RoomSeenRays))
        {
            (void)room;
            ++seek.RoomsLooked;
            ledger.Add(RewardTerm::RoomSeen, tuning.RoomSeen);
        }
    }

    // Explore (before the rooms below are marked entered): the floor the frame landed on for the first time, and the
    // frontier closed on. The first look is the spawn's view: recorded, not paid.
    Explore(env, seek, arena, seat, bot, !firstLook, ledger);

    // New ground, as shaping: a cell of floor walked onto for the first time.
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    if (seek.Cells.insert(CellKey(x, y, z, std::max(1.0f, tuning.NewGroundCell))).second && !firstLook)
        ledger.Add(RewardTerm::NewGround, tuning.NewGround);

    // The rooms: stepping into one, and coming back to one already entered.
    int32 const room = Draw::RoomAt(arena.Rooms, x, y, z);
    if (room >= 0 && room != seek.LastRoom && uint32(room) < seek.Entered.size())
    {
        if (seek.Entered[uint32(room)])
            ++seek.RoomsReentered;
        else
        {
            seek.Entered[uint32(room)] = true;
            ++seek.RoomsEntered;
            if (BackRoomName(arena.Rooms[uint32(room)].Name))
                ++seek.BackRoomVisits;
        }
    }
    seek.LastRoom = room;
    TrackRooms(env, seek, arena, bot, room, counts, ledger);
    if (!seek.FirstGoal)
        for (GoalHold const& hold : seat.Holds)
            if (GoalBlock::IsPlanGoal(hold.Goal))
            {
                seek.FirstGoal = true;
                seek.FirstGoalMs = env.EpisodeElapsedMs;
                break;
            }

    // Found: stopped (Standing::Stopped, the server's applied state) beside the object, on its floor.
    float const distance = bot->GetExactDist2d(&seek.Spot);
    bool const beside = distance <= arena.SeekRadius && std::fabs(z - seek.Spot.GetPositionZ()) <= tuning.ArriveRise;
    bool const stopped = !firstLook && Standing::Stopped(bot->GetUnitMovementFlags(), moved,
        _scenario.Tuning().Markers.StopMoved);
    if (!beside || !stopped)
        return;

    seek.Found = true;
    seek.FoundMs = env.EpisodeElapsedMs;
    if (int32 const found = seek.Room; found >= 0 && uint32(found) < seek.Track.size())
        seek.CoverAtFind = std::min(1.0f, float(seek.Track[uint32(found)].Hit.size())
            / float(std::max<uint32>(1, seek.Track[uint32(found)].Floor)));
    seek.RoomsBeforeFound = seek.RoomsEntered;
    ledger.Add(RewardTerm::Arrive, tuning.Arrive);
}

void Animus::Curriculum::SeekEncounter::WriteState(Env const& env, float* state) const
{
    // A training-only aid for the critic (the actor never sees it): where the object is, in the first enemy slot (the
    // stage has no enemies), relative to the spawn as every position in the state is; and the room's depth.
    EnvSeek const& seek = _envs[env.Index];
    if (!seek.Placed)
        return;
    Position const& origin = _scenario.SpawnPointFor(env);
    float* slot = state + StageScenario::STATE_GLOBAL_COUNT + MAX_SEATS * StageScenario::STATE_SEAT_FEATURES;
    slot[StageScenario::STATE_ENEMY_PRESENT] = 1.0f;
    slot[StageScenario::STATE_ENEMY_ALIVE] = seek.Found ? 0.0f : 1.0f;
    slot[StageScenario::STATE_ENEMY_X] = Encoding::RelativePosition(seek.Spot.GetPositionX(), origin.GetPositionX());
    slot[StageScenario::STATE_ENEMY_Y] = Encoding::RelativePosition(seek.Spot.GetPositionY(), origin.GetPositionY());
    state[StageScenario::STATE_TIER] = seek.Depth;
}

bool Animus::Curriculum::SeekEncounter::IsTerminal(Env const& env) const
{
    return _envs[env.Index].Found || _scenario.DeadForGood(env, 0);
}

void Animus::Curriculum::SeekEncounter::Teardown(Env& env)
{
    EnvSeek& seek = _envs[env.Index];
    ObjectPool::Remove(env.FindMap(), seek.Object);
    seek.Placed = false;
}
