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
#include "EncoderSupport.h"
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "Log.h"
#include "Camera.h"
#include "Map.h"
#include "MarkerEncounter.h"
#include "ModelIgnoreFlags.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SeekDraw.h"
#include "StageScenario.h"
#include "StageState.h"
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
    /// The salts of an evaluation's seeded placement draws.
    enum Salt : uint32 { SALT_TRIANGLE = 1, SALT_A, SALT_B, SALT_FACING, SALT_STRIDE = 8 };

    /// A cell of floor as one key: x and y in `cell`-yard squares, z in storeys of three yards.
    uint64 CellKey(float x, float y, float z, float cell)
    {
        uint64 const cx = uint64(uint32(int32(std::floor(x / cell)))) & 0x1FFFFF;
        uint64 const cy = uint64(uint32(int32(std::floor(y / cell)))) & 0x1FFFFF;
        uint64 const cz = uint64(uint32(int32(std::floor(z / 3.0f)))) & 0x3FFFFF;
        return cx << 43 | cy << 22 | cz;
    }
}

Animus::Curriculum::SeekEncounter::SeekEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::SeekEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::StepCost, RewardTerm::Death, RewardTerm::Stuck, RewardTerm::Wall,
        RewardTerm::Sighting, RewardTerm::NewGround };
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
    // `found` is the stage's measure: stopped beside the object before the clock ran out. The convergence, the shaping
    // fade and the cost ladder are gated on it (configs/move2_seek.yaml).
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
    // (the revisit rate, per entry). rooms_before_found is per episode that found it.
    table.Add("rooms_entered", [this](Env const& env, uint32) { return float(_envs[env.Index].RoomsEntered); });
    table.Add("rooms_reentered", [this](Env const& env, uint32) { return float(_envs[env.Index].RoomsReentered); });
    table.Add("room_entries", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return float(seek.RoomsEntered + seek.RoomsReentered);
    });
    table.Add("revisit_rate", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        uint32 const entries = seek.RoomsEntered + seek.RoomsReentered;
        return entries ? float(seek.RoomsReentered) / float(entries) : 0.0f;
    });
    table.Add("rooms_before_found", [this](Env const& env, uint32)
    {
        EnvSeek const& seek = _envs[env.Index];
        return seek.Found ? float(seek.RoomsBeforeFound) : 0.0f;
    });
    // What was hidden where: the room and the object (indexes into stage.json episode_categories), the room's depth
    // (its rank by walking distance, 0 the nearest), its tier (thirds: `difficulty`, so the evaluation tables split by
    // it) and whether it is the deepest third (`deep_room`: the evaluation's found_deepest, the P1 gate's reading).
    // The ladder is the fade's, not the room's: `room_ladder` is 1 - the shaping scale the room was drawn at, and
    // `at_top_rung` whether the fade was done (an evaluation's always is), so a stage that converges at the top of its
    // ladder (convergence.top_rung) converges only once the fade is.
    table.Add("seek_room", [this](Env const& env, uint32) { return float(std::max(0, _envs[env.Index].Room)); });
    table.Add("seek_object", [this](Env const& env, uint32) { return float(_envs[env.Index].ObjectIndex); });
    table.Add("room_depth", [this](Env const& env, uint32) { return _envs[env.Index].Depth; });
    table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Tier); });
    table.Add("deep_room", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Tier + 1 >= Draw::TIERS ? 1.0f : 0.0f;
    });
    table.Add("room_ladder", [this](Env const& env, uint32) { return _envs[env.Index].Ladder; });
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Ladder >= 1.0f - 1e-4f ? 1.0f : 0.0f;
    });
    table.Add("object_fallback", [this](Env const& env, uint32) { return _envs[env.Index].Fallback ? 1.0f : 0.0f; });
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });
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

void Animus::Curriculum::SeekEncounter::Remove(EnvSeek& seek, Map* map) const
{
    if (seek.Placed && map)
        if (GameObject* object = map->GetGameObject(seek.Object))
            object->Delete();
    seek.Placed = false;
    seek.Object.Clear();
}

bool Animus::Curriculum::SeekEncounter::Place(Env const& env, EnvSeek& seek, Map* map, ArenaDefinition const& arena,
    uint32 phase) const
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    SeekRoom const& room = arena.Rooms[uint32(seek.Room)];
    SeekObject const& kind = arena.Objects[seek.ObjectIndex];
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
    Position const spot(x, y, z, facing);
    map->LoadGrid(x, y);
    // No respawn: it is removed at the next reset (Remove). The rotation is the facing's, about the vertical.
    GameObject* object = map->SummonGameObject(kind.Entry, spot, 0.0f, 0.0f, std::sin(facing * 0.5f),
        std::cos(facing * 0.5f), 0);
    if (!object)
    {
        LOG_ERROR("module.animus", "{}: the seek object {} ({}) could not be spawned in room {}", _scenario.Name(),
            kind.Entry, kind.Kind, room.Name);
        return false;
    }
    object->SetPhaseMask(phase, true);
    seek.Object = object->GetGUID();
    seek.Placed = true;
    seek.Spot = spot;
    seek.Centre = Position(x, y, z + kind.Height * 0.5f, facing);
    return true;
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
    Remove(seek, map);
    seek = EnvSeek();

    // The dungeon's own game objects -- its chests, the Hallow's End pumpkins -- out of the way for a week: the one
    // object in it is the one to find. Cheap enough to do every reset (a few dozen), and a respawned one goes again.
    std::vector<GameObject*> own;
    for (auto const& [spawnId, object] : map->GetGameObjectBySpawnIdStore())
        if (object && object->IsInWorld() && object->isSpawned())
            own.push_back(object);
    for (GameObject* object : own)
        object->DespawnOrUnsummon(0ms, Seconds(WEEK));

    // The room: the ladder's weights in training, every room in turn in an evaluation; the object uniformly.
    std::vector<float> const depths = Draw::Depths(arena.Rooms);
    if (env.EpisodeSeedIndex != NO_EPISODE_SEED)
    {
        auto const [room, object] = Draw::EvaluationPick(env.EpisodeSeedIndex, uint32(arena.Rooms.size()),
            uint32(arena.Objects.size()));
        seek.Room = int32(room);
        seek.ObjectIndex = object;
        seek.Ladder = 1.0f;
    }
    else
    {
        seek.Ladder = 1.0f - _scenario.ShapingScale();
        seek.Room = int32(Draw::Pick(Draw::Weights(arena.Rooms, seek.Ladder), frand(0.0f, 1.0f)));
        seek.ObjectIndex = urand(0, uint32(arena.Objects.size()) - 1);
    }
    seek.Depth = depths[uint32(seek.Room)];
    seek.Tier = Draw::Tier(seek.Depth);
    seek.Entered.assign(arena.Rooms.size(), false);

    return Place(env, seek, map, arena, bot->GetPhaseMask());
}

bool Animus::Curriculum::SeekEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seat acts without a target (SeatEncoder::ActsWithoutTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::SeekEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    // The objective is the camera's to show (its flag, in line of sight) and nothing else's: the stage has no compass,
    // and no other block of it reads the point.
    EnvSeek const& seek = _envs[env.Index];
    view.HasObjective = seek.Placed && !seek.Found;
    view.Objective = seek.Centre;
    // The flag on the object itself: its own radius and a quarter yard, not a yard round its centre.
    ArenaDefinition const& arena = _scenario.Arena(env);
    if (seek.ObjectIndex < arena.Objects.size())
        view.ObjectiveRadius = Vision::ObjectiveRadiusFor(arena.Objects[seek.ObjectIndex].Radius);
    view.MountsAllowed = false;
    view.GroundMountAllowed = false;
    view.ArriveWithin = _scenario.Arena(env).SeekRadius;
}

void Animus::Curriculum::SeekEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::SeekTuning const& tuning = _scenario.Tuning().Seek;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvSeek& seek = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
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

    // The costs, read off the controller's counts as the ground course reads them.
    uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, seek.LastStuckMs);
    uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, seek.LastWallMs);
    seek.LastStuckMs = seat.StuckMs;
    seek.LastWallMs = seat.WallMs;
    if (stuckMs)
        ledger.Add(RewardTerm::Stuck, -tuning.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
        float const charge = MarkerEncounter::WallCharge(float(wallMs) / 1000.0f, moved, asked, tuning.Wall,
            tuning.WallSlide);
        if (charge > 0.0f)
            ledger.Add(RewardTerm::Wall, -charge);
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

    // New ground, as shaping: a cell of floor walked onto for the first time.
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();
    if (seek.Cells.insert(CellKey(x, y, z, std::max(1.0f, tuning.NewGroundCell))).second && !firstLook)
        ledger.Add(RewardTerm::NewGround, tuning.NewGround);

    // The rooms: stepping into one, and coming back to one already entered.
    ArenaDefinition const& arena = _scenario.Arena(env);
    int32 const room = Draw::RoomAt(arena.Rooms, x, y, z);
    if (room >= 0 && room != seek.LastRoom && uint32(room) < seek.Entered.size())
    {
        if (seek.Entered[uint32(room)])
            ++seek.RoomsReentered;
        else
        {
            seek.Entered[uint32(room)] = true;
            ++seek.RoomsEntered;
        }
    }
    seek.LastRoom = room;

    // Found: stopped (MarkerEncounter::Stopped, the server's applied state) beside the object, on its floor.
    float const distance = bot->GetExactDist2d(&seek.Spot);
    bool const beside = distance <= arena.SeekRadius && std::fabs(z - seek.Spot.GetPositionZ()) <= tuning.ArriveRise;
    bool const stopped = !firstLook && MarkerEncounter::Stopped(bot->GetUnitMovementFlags(), moved,
        _scenario.Tuning().Markers.StopMoved);
    if (!beside || !stopped)
        return;

    seek.Found = true;
    seek.FoundMs = env.EpisodeElapsedMs;
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
    Remove(seek, env.FindMap());
}
