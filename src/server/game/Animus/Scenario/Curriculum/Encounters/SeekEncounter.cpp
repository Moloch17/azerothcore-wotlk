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
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapVisionWorld.h"
#include "MarkerEncounter.h"
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
        RewardTerm::Sighting, RewardTerm::NewGround, RewardTerm::RoomSeen };
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
    // The episode's clock is its placement rung's: short early episodes make the reward come often.
    env.EpisodeLengthMs = (seek.Sweep ? std::max<uint32>(1, arena.EpisodeSeconds)
        : Draw::RungSeconds(placed, RungSeconds(tuning))) * IN_MILLISECONDS;

    // Facing a random way where the scenario put the seat (a random hallway point, the entrance among them). The
    // scenario reads the seat's facing from the bot after the encounters are built.
    Position const& spawn = _scenario.SpawnPointFor(env);
    Position const start(spawn.GetPositionX(), spawn.GetPositionY(), spawn.GetPositionZ(), frand(0.0f, TWO_PI));
    BotFactory::TeleportWithinMap(bot, start);

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
            seek.Fallback = true;
        }
        built = Place(env, seek, map, arena, bot->GetPhaseMask());
    }
    if (seek.Room >= 0 && uint32(seek.Room) < arena.Rooms.size())
    {
        seek.Depth = Draw::Depths(arena.Rooms)[uint32(seek.Room)];
        seek.Tier = Draw::Tier(seek.Depth);
    }
    seek.Entered.assign(arena.Rooms.size(), false);
    seek.Looked.assign(arena.Rooms.size(), false);
    return built;
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
    // and no other block of it reads the point (the goal block gives it no place: ObjectivePlaceKnown).
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
        float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
        float const charge = MarkerEncounter::WallCharge(float(wallMs) / 1000.0f, moved, asked, tuning.Wall,
            tuning.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
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
    // knows the room, takes nothing away.
    ArenaDefinition const& arena = _scenario.Arena(env);
    if (!seat.Hits.Rays.empty())
        for (uint32 room : Draw::NewlyLooked(arena.Rooms, seat.Hits, seek.Looked, tuning.RoomSeenRays))
        {
            (void)room;
            ++seek.RoomsLooked;
            ledger.Add(RewardTerm::RoomSeen, tuning.RoomSeen);
        }

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
    ObjectPool::Remove(env.FindMap(), seek.Object);
    seek.Placed = false;
}
