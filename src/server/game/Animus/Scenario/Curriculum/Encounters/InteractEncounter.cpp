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

#include "InteractEncounter.h"
#include "BotFactory.h"
#include "Camera.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "EnvPool.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Identity.h"
#include "InteractDraw.h"
#include "Log.h"
#include "Map.h"
#include "MapVisionWorld.h"
#include "Standing.h"
#include "ObjectPool.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SightDraw.h"
#include "StageScenario.h"
#include "StageState.h"
#include "UnitBody.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace
{
    namespace Draw = Animus::Curriculum::InteractDraw;
    using Animus::Curriculum::InteractSite;

    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// The salts of a seeded episode's spot order (each spot drawn after the seed's own uniforms).
    constexpr uint32 SALT_ORDER = 200;

    std::array<uint32, Draw::RUNGS> RungSeconds(Animus::Curriculum::CurriculumTuning::InteractTuning const& tuning)
    {
        return { tuning.RungSeconds0, tuning.RungSeconds1, tuning.RungSeconds2 };
    }

    /// The first of the map's own spawns of `entry`.
    GameObject* OwnOf(Map* map, uint32 entry)
    {
        if (!map || !entry)
            return nullptr;
        for (auto const& [spawnId, object] : map->GetGameObjectBySpawnIdStore())
            if (object && object->GetEntry() == entry && object->IsInWorld())
                return object;
        return nullptr;
    }

    /// The order a list of `count` spots is tried in: a uniform shuffle (Fisher-Yates), from `uniform`.
    template <typename Uniform>
    std::vector<uint32> Order(uint32 count, Uniform&& uniform)
    {
        std::vector<uint32> order(count);
        for (uint32 i = 0; i < count; ++i)
            order[i] = i;
        for (uint32 i = count; i > 1; --i)
            std::swap(order[i - 1], order[std::min<uint32>(i - 1, uint32(uniform() * float(i)))]);
        return order;
    }
}

Animus::Curriculum::InteractEncounter::InteractEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::InteractEncounter::RewardTerms() const
{
    return { RewardTerm::Arrive, RewardTerm::DoorOpened, RewardTerm::WrongObject, RewardTerm::StepCost,
        RewardTerm::Death, RewardTerm::Stuck, RewardTerm::Wall, RewardTerm::Sighting };
}

std::vector<std::string> Animus::Curriculum::InteractEncounter::SiteNames(ArenaDefinition const& arena)
{
    std::vector<std::string> names;
    for (InteractSite const& site : arena.Sites)
        names.push_back(site.Name);
    return names;
}

std::vector<std::string> Animus::Curriculum::InteractEncounter::ObjectNames(ArenaDefinition const& arena)
{
    std::vector<std::string> names;
    for (SeekObject const& object : arena.Objects)
        names.push_back(object.Kind);
    names.emplace_back("lock");
    return names;
}

bool Animus::Curriculum::InteractEncounter::FollowLever(GameObject* lever, GameObject* door, Unit* user)
{
    if (!lever || !door || lever->getLootState() != GO_ACTIVATED)
        return false;
    if (door->getLootState() != GO_READY || door->GetGoState() != GO_STATE_READY)
        return false;
    door->UseDoorOrButton(0, false, user);
    return true;
}

void Animus::Curriculum::InteractEncounter::ResetObject(GameObject* object)
{
    if (!object)
        return;
    object->RemoveGameObjectFlag(GO_FLAG_IN_USE);
    if (object->GetGoState() != GO_STATE_READY)
        object->SetGoState(GO_STATE_READY);
    if (object->getLootState() != GO_READY)
        object->SetLootState(GO_READY);
}

void Animus::Curriculum::InteractEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // `right_object` is the stage's measure: the named object reached, or its lock given the key, before the clock
    // ran out. The convergence and the fade (whose rungs are the ladder's) are gated on it
    // (configs/move3_interact.yaml).
    table.Add("right_object", [this](Env const& env, uint32) { return _envs[env.Index].Found ? 1.0f : 0.0f; });
    table.Add("right_seconds", [this](Env const& env, uint32)
    {
        EnvInteract const& state = _envs[env.Index];
        return state.Found ? float(state.FoundMs) / 1000.0f : 0.0f;
    });
    // The first frame listing the named object, over the episodes that listed it at all, and from there to the end.
    table.Add("sighted", [this](Env const& env, uint32) { return _envs[env.Index].SightPaid ? 1.0f : 0.0f; });
    table.Add("sight_seconds", [this](Env const& env, uint32)
    {
        EnvInteract const& state = _envs[env.Index];
        return state.SightPaid ? float(state.SightMs) / 1000.0f : 0.0f;
    });
    table.Add("found_sighted", [this](Env const& env, uint32)
    {
        EnvInteract const& state = _envs[env.Index];
        return state.Found && state.SightPaid ? 1.0f : 0.0f;
    });
    table.Add("sight_to_arrival", [this](Env const& env, uint32)
    {
        EnvInteract const& state = _envs[env.Index];
        return state.Found && state.SightPaid
            ? float(state.FoundMs - std::min(state.FoundMs, state.SightMs)) / 1000.0f : 0.0f;
    });
    table.Add("objective_visible", [this](Env const& env, uint32)
    {
        EnvInteract const& state = _envs[env.Index];
        return state.Decisions ? float(state.VisibleDecisions) / float(state.Decisions) : 0.0f;
    });
    // The wrong objects: decoys taken for the named one (each once, stopped beside or pressed), and how.
    table.Add("wrong_objects", [this](Env const& env, uint32)
    {
        EnvInteract const& state = _envs[env.Index];
        return float(std::count(state.DecoyTaken.begin(), state.DecoyTaken.end(), true));
    });
    table.Add("wrong_presses", [this](Env const& env, uint32) { return float(_envs[env.Index].WrongPresses); });
    table.Add("wrong_stops", [this](Env const& env, uint32) { return float(_envs[env.Index].WrongStops); });
    table.Add("decoys", [this](Env const& env, uint32) { return float(_envs[env.Index].Decoys.size()); });
    // The door and the key: the lever pressed (sent), the site's door open by the end (door_opened) and opened after
    // the seat's own lever press (door_by_lever, the switch rung's "door opened by the right action"), the key item
    // used on the lock (key_used).
    table.Add("lever_pressed", [this](Env const& env, uint32) { return _envs[env.Index].LeverPressed ? 1.0f : 0.0f; });
    table.Add("door_opened", [this](Env const& env, uint32) { return _envs[env.Index].Watch.Opened ? 1.0f : 0.0f; });
    table.Add("door_by_lever", [this](Env const& env, uint32) { return _envs[env.Index].Watch.ByLever ? 1.0f : 0.0f; });
    table.Add("key_used", [this](Env const& env, uint32) { return _envs[env.Index].KeyUsed ? 1.0f : 0.0f; });
    // Where and what (indexes into stage.json episode_categories): the site, and the named object's kind (the last
    // name, "lock", for the key rung's opener).
    table.Add("interact_site", [this](Env const& env, uint32) { return float(_envs[env.Index].Site); });
    table.Add("interact_object", [this](Env const& env, uint32) { return float(_envs[env.Index].ObjectIndex); });
    // **The ladder**: the episode's rung (interact_rung, 0 distinguish to 2 key: the one *_rung column, which the
    // evaluation videos spread their sample over), the ladder's (interact_ladder, a share of the top), the carry-over,
    // and at_top_rung (convergence.top_rung). Per rung, whether the episode played it (rung_<name>) and whether it got
    // the right object (right_<name>, per event over rung_<name>).
    table.Add("interact_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });
    table.Add("interact_ladder", [this](Env const& env, uint32)
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
        table.Add("right_" + name, [this, rung](Env const& env, uint32)
        {
            EnvInteract const& state = _envs[env.Index];
            return state.Rung == rung && state.Found ? 1.0f : 0.0f;
        });
    }
    table.Add("object_fallback", [this](Env const& env, uint32) { return _envs[env.Index].Fallback ? 1.0f : 0.0f; });
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });
}

void Animus::Curriculum::InteractEncounter::ResetEpisode(Env& env)
{
    // The objects stay in the world until the next Build removes them (it has the map); the episode's measures go.
    EnvInteract& state = _envs[env.Index];
    EnvInteract kept;
    kept.Target = state.Target;
    kept.TargetSpawned = state.TargetSpawned;
    kept.Decoys = state.Decoys;
    kept.Placed = state.Placed;
    state = kept;
}

ObjectGuid Animus::Curriculum::InteractEncounter::Spawn(Map* map, ArenaDefinition const& arena, uint32 kind,
    Position const& spot, uint32 phase) const
{
    SeekObject const& object = arena.Objects[kind];
    GameObject* spawned = ObjectPool::Summon(map, object, spot, phase);
    if (!spawned)
    {
        LOG_ERROR("module.animus", "{}: the interact object {} ({}) could not be spawned at ({:.1f} {:.1f} {:.1f})",
            _scenario.Name(), object.Entry, object.Kind, spot.GetPositionX(), spot.GetPositionY(),
            spot.GetPositionZ());
        return ObjectGuid::Empty;
    }
    return spawned->GetGUID();
}

void Animus::Curriculum::InteractEncounter::ClearWorld(EnvInteract& state, Map* map,
    ArenaDefinition const& arena) const
{
    if (state.TargetSpawned)
        ObjectPool::Remove(map, state.Target);
    for (ObjectGuid& decoy : state.Decoys)
        ObjectPool::Remove(map, decoy);
    state.Decoys.clear();
    if (!map)
        return;

    // The map's own objects: its doors and levers, and the sites' locks, as a fresh instance has them; everything
    // else (chests, veins, the gunpowder barrel, the foundry's pots) away for a week, so the episode's objects are the
    // only others. Cheap enough every reset (a few dozen).
    std::set<uint32> openers;
    for (InteractSite const& site : arena.Sites)
        openers.insert(site.Opener);
    std::vector<GameObject*> drop;
    for (auto const& [spawnId, object] : map->GetGameObjectBySpawnIdStore())
    {
        if (!object || !object->IsInWorld())
            continue;
        bool const door = object->GetGoType() == GAMEOBJECT_TYPE_DOOR || object->GetGoType() == GAMEOBJECT_TYPE_BUTTON;
        if (door || openers.contains(object->GetEntry()))
            ResetObject(object);
        else if (object->isSpawned())
            drop.push_back(object);
    }
    for (GameObject* object : drop)
        object->DespawnOrUnsummon(0ms, Seconds(WEEK));

    // What a lock's script summoned when it was used (the cannon's two pirates): sent away.
    float const sweep = _scenario.Tuning().Interact.SummonSweep;
    for (InteractSite const& site : arena.Sites)
    {
        GameObject* opener = OwnOf(map, site.Opener);
        if (!opener)
            continue;
        std::vector<Creature*> summoned;
        auto const worker = [&summoned](WorldObject* object)
        {
            if (Creature* creature = object->ToCreature(); creature && creature->IsInWorld() && creature->IsSummon())
                summoned.push_back(creature);
        };
        Acore::WorldObjectWorker<decltype(worker)> searcher(opener, worker, GRID_MAP_TYPE_MASK_CREATURE);
        Cell::VisitObjects(opener, searcher, sweep);
        for (Creature* creature : summoned)
            creature->DespawnOrUnsummon();
    }
}

bool Animus::Curriculum::InteractEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    Player* bot = _scenario.SeatBot(env, 0);
    if (!bot || !map)
        return false;
    ArenaDefinition const& arena = _scenario.Arena(env);
    if (arena.Sites.empty() || arena.Objects.empty())
        return false;

    EnvInteract& state = _envs[env.Index];
    ClearWorld(state, map, arena);
    state = EnvInteract();

    // The rung: the fade's (a training episode CarryShare of the time the one below); an evaluation plays the
    // training rung, the held-out sweep every rung in turn.
    CurriculumTuning::InteractTuning const& tuning = _scenario.Tuning().Interact;
    bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED;
    state.Sweep = arena.EvalOnly;
    Draw::Rung const ladder = state.Sweep && seeded ? Draw::SweepRung(env.EpisodeSeedIndex)
        : Draw::RungOf(_scenario.ShapingScale());
    state.LadderRung = uint32(ladder);
    Draw::Rung const placed = seeded ? ladder : Draw::PlacedRung(ladder, frand(0.0f, 1.0f), tuning.CarryShare);
    state.Rung = uint32(placed);
    state.Carried = placed != ladder;
    std::vector<uint32> const sites = Draw::RungSites(arena.Sites, placed);
    if (sites.empty())
        return false;

    // The episode's draws: its seed's (InteractDraw::EvaluationPick), or fresh ones.
    Draw::EvaluationEpisode pick;
    if (seeded)
        pick = Draw::EvaluationPick(env.EpisodeSeedIndex, sites);
    else
    {
        pick.Site = sites[urand(0, uint32(sites.size()) - 1)];
        pick.SpawnU = frand(0.0f, 1.0f);
        pick.TargetU = frand(0.0f, 1.0f);
        pick.DecoysU = frand(0.0f, 1.0f);
        pick.KindsU = frand(0.0f, 1.0f);
        pick.CountU = frand(0.0f, 1.0f);
        pick.FacingU = frand(0.0f, 1.0f);
    }
    uint32 salt = SALT_ORDER;
    auto const uniform = [&]()
    {
        return seeded ? SeekDraw::SeedUniform(env.EpisodeSeedIndex, salt++) : frand(0.0f, 1.0f);
    };
    state.Site = pick.Site;
    InteractSite const& site = arena.Sites[state.Site];
    if (site.Near.empty())
        return false;
    GameObject* door = OwnOf(map, site.Door);
    GameObject* opener = OwnOf(map, site.Opener);
    if (!door || !opener)
    {
        LOG_ERROR("module.animus", "{}: site {} has no door {} or opener {} on map {}", _scenario.Name(), site.Name,
            site.Door, site.Opener, map->GetId());
        return false;
    }
    state.Door = door->GetGUID();
    state.Opener = opener->GetGUID();

    // Every site's key item, one of each, on every rung: the bags never say which rung it is.
    for (InteractSite const& any : arena.Sites)
        if (any.Key && !bot->HasItemCount(any.Key, 1))
            bot->AddItem(any.Key, 1);

    // The seat on the site's near side, facing a random way.
    uint32 const spawn = Draw::Index(pick.SpawnU, uint32(site.Near.size()));
    Position const& at = site.Near[spawn];
    Position const start(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), pick.FacingU * TWO_PI);
    BotFactory::TeleportWithinMap(bot, start);

    // What it is to look for: the pool's kinds (the named one and its decoys, each kind once), or the site's lock.
    uint32 const objects = uint32(arena.Objects.size());
    uint32 const decoyCount = Draw::DecoyCount(pick.CountU, tuning.DecoysMin, tuning.DecoysMax, objects);
    std::vector<uint32> kinds;
    if (placed == Draw::Rung::Key)
    {
        state.ObjectIndex = objects;
        kinds = Draw::DecoyKinds(objects, decoyCount, objects, pick.KindsU);
    }
    else
    {
        state.ObjectIndex = Draw::Index(pick.TargetU, objects);
        kinds.push_back(state.ObjectIndex);
        std::vector<uint32> const decoys = Draw::DecoyKinds(state.ObjectIndex, decoyCount, objects, pick.KindsU);
        kinds.insert(kinds.end(), decoys.begin(), decoys.end());
    }

    // Where: the distinguish and key rungs' objects on the near side, in sight of the seat's eye; the switch rung's
    // behind the door. Spacing yards apart, and off the seat's spot.
    std::vector<Position> const& pool = placed == Draw::Rung::Switch ? site.Far : site.Near;
    Vision::MapVisionWorld const world(map, bot->GetPhaseMask());
    float const eye = Vision::PIVOT_SHARE * Movement::ShapeOf(bot).Height;
    std::vector<Position> taken{ start };
    std::vector<uint32> const order = Order(uint32(pool.size()), uniform);
    std::vector<std::pair<uint32, Position>> spots;
    uint32 tested = 0;
    for (uint32 kind : kinds)
    {
        SeekObject const& object = arena.Objects[kind];
        SightDraw::Viewing viewing;
        viewing.EyeRise = eye;
        viewing.CentreRise = object.Height * 0.5f;
        viewing.Radius = object.Radius;
        for (uint32 candidate : order)
        {
            if (tested >= std::max<uint32>(1, tuning.Attempts))
                break;
            Position const& spot = pool[candidate];
            bool const apart = std::all_of(taken.begin(), taken.end(), [&](Position const& other)
            {
                return other.GetExactDist2d(&spot) >= tuning.Spacing;
            });
            if (!apart)
                continue;
            ++tested;
            if (placed != Draw::Rung::Switch)
            {
                float const away = start.GetExactDist2d(&spot);
                if (away < tuning.SightNearest || away > tuning.SightFurthest
                    || !SightDraw::Seen(pool, start, candidate, viewing, world))
                    continue;
            }
            taken.push_back(spot);
            spots.emplace_back(kind, Position(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(),
                uniform() * TWO_PI));
            break;
        }
    }

    // The named object first, then the decoys; one that found no spot is left out (object_fallback).
    uint32 const phase = bot->GetPhaseMask();
    state.Fallback = spots.size() < kinds.size();
    if (placed == Draw::Rung::Key)
    {
        state.Target = state.Opener;
        state.Spot = opener->GetPosition();
        state.NamedEntry = opener->GetEntry();
        state.NamedClass = uint8(Vision::Classify(Vision::FactsOf(bot, opener)).What);
    }
    for (auto const& [kind, spot] : spots)
    {
        bool const named = placed != Draw::Rung::Key && kind == state.ObjectIndex;
        ObjectGuid const guid = Spawn(map, arena, kind, spot, phase);
        if (guid.IsEmpty())
            continue;
        if (named)
        {
            state.Target = guid;
            state.TargetSpawned = true;
            state.Spot = spot;
            state.Radius = arena.Objects[kind].Radius;
            state.Height = arena.Objects[kind].Height;
            state.NamedEntry = arena.Objects[kind].Entry;
            if (GameObject* spawned = map->GetGameObject(guid))
                state.NamedClass = uint8(Vision::Classify(Vision::FactsOf(bot, spawned)).What);
            continue;
        }
        state.Decoys.push_back(guid);
        state.DecoySpots.push_back(spot);
    }
    state.DecoyTaken.assign(state.Decoys.size(), false);
    state.Placed = !state.Target.IsEmpty();
    if (!state.Placed)
        LOG_ERROR("module.animus", "{}: env {} rung {} at {}: the named object could not be placed", _scenario.Name(),
            env.Index, Draw::RUNG_NAMES[state.Rung], site.Name);

    // The episode's clock is its rung's (the sweep's too).
    env.EpisodeLengthMs = RungSeconds(tuning)[std::min<uint32>(state.Rung, Draw::RUNGS - 1)] * IN_MILLISECONDS;
    return state.Placed;
}

bool Animus::Curriculum::InteractEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seat acts without a target (its selection is its own, StageScenario::CurrentTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::InteractEncounter::OnSeatAction(Env& env, uint32 /*seat*/, SeatActionResult const& result)
{
    EnvInteract& state = _envs[env.Index];
    if (!state.Placed || state.Found || result.ActedOn.IsEmpty())
        return;
    std::vector<uint64> decoys;
    for (ObjectGuid const& decoy : state.Decoys)
        decoys.push_back(decoy.GetRawValue());
    Draw::PressFacts facts;
    facts.Press = result.ActPress;
    facts.Sent = result.ActRefused == 0;
    facts.Reached = Draw::Reached(result.ActRefused);
    facts.On = result.ActedOn.GetRawValue();
    facts.Target = state.Target.GetRawValue();
    facts.Opener = state.Opener.GetRawValue();
    facts.Asked = Draw::TaskOf(Draw::Rung(state.Rung));
    facts.Decoys = &decoys;
    switch (Draw::Judge(facts))
    {
        case Draw::Verdict::Right:
            state.PendingRight = true;
            if (facts.Asked == Draw::Task::UseItem)
                state.KeyUsed = true;
            break;
        case Draw::Verdict::Wrong:
        {
            ++state.WrongPresses;
            auto const at = std::find(decoys.begin(), decoys.end(), facts.On);
            std::size_t const index = std::size_t(at - decoys.begin());
            if (index < state.DecoyTaken.size() && !state.DecoyTaken[index])
            {
                state.DecoyTaken[index] = true;
                ++state.PendingWrong;
            }
            break;
        }
        case Draw::Verdict::Opener:
        {
            state.LeverPressed = true;
            state.Watch.Press();
            // The map's script opened the door with the lever's use; should it not have, the lever's link does.
            Map* map = env.FindMap();
            Player* bot = _scenario.SeatBot(env, 0);
            if (map)
                FollowLever(map->GetGameObject(state.Opener), map->GetGameObject(state.Door), bot);
            break;
        }
        case Draw::Verdict::None:
            break;
    }
}

void Animus::Curriculum::InteractEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    // What the goal names, never where: no objective flag on the object (it is a real object the label system
    // describes: amendment 8), no compass, and the goal block gives it no place.
    EnvInteract const& state = _envs[env.Index];
    view.HasObjective = false;
    view.ObjectivePlaceKnown = false;
    if (state.Placed)
    {
        view.NamedTask = uint8(Draw::TaskOf(Draw::Rung(state.Rung)));
        view.NamedClass = state.NamedClass;
        view.NamedEntry = state.NamedEntry;
        view.NamedObject = true;
    }
    view.ArriveWithin = _scenario.Arena(env).SeekRadius;
}

void Animus::Curriculum::InteractEncounter::Found(Env const& env, EnvInteract& state) const
{
    state.Found = true;
    state.FoundMs = env.EpisodeElapsedMs;
}

void Animus::Curriculum::InteractEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::InteractTuning const& tuning = _scenario.Tuning().Interact;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvInteract& state = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (!bot)
        return;

    float moved = 0.0f;
    if (state.HasLastPos)
    {
        float const dx = bot->GetPositionX() - state.LastX;
        float const dy = bot->GetPositionY() - state.LastY;
        moved = std::sqrt(dx * dx + dy * dy);
        state.Travelled += moved;
    }
    bool const firstLook = !state.HasLastPos;
    state.LastX = bot->GetPositionX();
    state.LastY = bot->GetPositionY();
    state.HasLastPos = true;

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
    if (state.Placed && !state.Found && timeIsUp)
        tally.TimedOut = true;
    if (!state.Placed || state.Found || !bot->IsAlive())
        return;

    // The noise prices, on from the first step at their own fixed price, as the seek stage's.
    uint32 const stuckMs = seat.StuckMs - std::min(seat.StuckMs, state.LastStuckMs);
    uint32 const wallMs = seat.WallMs - std::min(seat.WallMs, state.LastWallMs);
    state.LastStuckMs = seat.StuckMs;
    state.LastWallMs = seat.WallMs;
    if (stuckMs)
        ledger.AddFixed(RewardTerm::Stuck, -tuning.Stuck * float(stuckMs) / 1000.0f);
    if (wallMs)
    {
        Movement::ControlState const& held = seat.Controls.Held;
        UnitMoveType const kind = held.Walk ? MOVE_WALK : held.Forward < 0 && !held.Strafe ? MOVE_RUN_BACK : MOVE_RUN;
        float const asked = bot->GetSpeed(kind) * float(_scenario.DecisionMs()) / 1000.0f;
        float const charge = Standing::WallCharge(float(wallMs) / 1000.0f, moved, asked, tuning.Wall,
            tuning.WallSlide);
        if (charge > 0.0f)
            ledger.AddFixed(RewardTerm::Wall, -charge);
    }

    // The frame the seat decided on: whether its list shows the named object. The first is paid once, as shaping.
    ++state.Decisions;
    uint64 const target = state.Target.GetRawValue();
    bool visible = false;
    for (uint32 slot = 0; slot < seat.Seen.Count && !visible; ++slot)
        visible = seat.Seen.Info[slot].Guid == target;
    state.VisibleDecisions += visible ? 1 : 0;
    if (visible && !state.SightPaid)
    {
        state.SightPaid = true;
        state.SightMs = env.EpisodeElapsedMs;
        ledger.Add(RewardTerm::Sighting, tuning.Sighting);
    }

    // The door: open by now, and opened after the seat's own press on its lever (the switch rung's half).
    // Paid once an episode, on its first opening after the seat's own lever press (DoorWatch), on the switch rung.
    if (Map* map = bot->GetMap())
        if (GameObject* door = map->GetGameObject(state.Door))
            if (state.Watch.Look(door->GetGoState() != GO_STATE_READY) && state.Rung == uint32(Draw::Rung::Switch))
                ledger.Add(RewardTerm::DoorOpened, tuning.DoorOpened);

    // Decoys pressed this decision (OnSeatAction): each once.
    if (state.PendingWrong)
    {
        ledger.Add(RewardTerm::WrongObject, -tuning.WrongObject * float(state.PendingWrong));
        state.PendingWrong = 0;
    }

    // The named object used (a reach task's object pressed, or the lock given its key): the episode's outcome.
    if (state.PendingRight)
    {
        state.PendingRight = false;
        Found(env, state);
        ledger.Add(RewardTerm::Arrive, tuning.Arrive);
        return;
    }

    // Stopped beside an object (Standing::Stopped), on its floor: the nearest within reach is the one reached.
    bool const stopped = !firstLook && Standing::Stopped(bot->GetUnitMovementFlags(), moved,
        _scenario.Tuning().Markers.StopMoved);
    if (!stopped)
        return;
    float const reach = _scenario.Arena(env).SeekRadius;
    float const z = bot->GetPositionZ();
    auto const beside = [&](Position const& spot)
    {
        float const distance = bot->GetExactDist2d(&spot);
        return distance <= reach && std::fabs(z - spot.GetPositionZ()) <= tuning.ArriveRise ? distance : -1.0f;
    };
    float best = beside(state.Spot);
    int32 nearest = -1;     // -1 the named object, else a decoy
    bool const reachTask = Draw::TaskOf(Draw::Rung(state.Rung)) == Draw::Task::Reach;
    if (!reachTask)
        best = -1.0f;
    for (std::size_t index = 0; index < state.DecoySpots.size(); ++index)
    {
        float const distance = beside(state.DecoySpots[index]);
        if (distance >= 0.0f && (best < 0.0f || distance < best))
        {
            best = distance;
            nearest = int32(index);
        }
    }
    if (best < 0.0f)
        return;
    if (nearest < 0)
    {
        Found(env, state);
        ledger.Add(RewardTerm::Arrive, tuning.Arrive);
        return;
    }
    if (!state.DecoyTaken[std::size_t(nearest)])
    {
        state.DecoyTaken[std::size_t(nearest)] = true;
        ++state.WrongStops;
        ledger.Add(RewardTerm::WrongObject, -tuning.WrongObject);
    }
}

void Animus::Curriculum::InteractEncounter::WriteState(Env const& env, float* state) const
{
    // A training-only aid for the critic (the actor never sees it): where the named object is, in the first enemy
    // slot, relative to the spawn as every position in the state is; and the rung.
    EnvInteract const& here = _envs[env.Index];
    if (!here.Placed)
        return;
    Position const& origin = _scenario.SpawnPointFor(env);
    float* slot = state + StageScenario::STATE_GLOBAL_COUNT + MAX_SEATS * StageScenario::STATE_SEAT_FEATURES;
    slot[StageScenario::STATE_ENEMY_PRESENT] = 1.0f;
    slot[StageScenario::STATE_ENEMY_ALIVE] = here.Found ? 0.0f : 1.0f;
    slot[StageScenario::STATE_ENEMY_X] = Encoding::RelativePosition(here.Spot.GetPositionX(), origin.GetPositionX());
    slot[StageScenario::STATE_ENEMY_Y] = Encoding::RelativePosition(here.Spot.GetPositionY(), origin.GetPositionY());
    state[StageScenario::STATE_TIER] = float(here.Rung) / float(Draw::RUNGS - 1);
}

bool Animus::Curriculum::InteractEncounter::IsTerminal(Env const& env) const
{
    return _envs[env.Index].Found || _scenario.DeadForGood(env, 0);
}

void Animus::Curriculum::InteractEncounter::Teardown(Env& env)
{
    EnvInteract& state = _envs[env.Index];
    Map* map = env.FindMap();
    if (state.TargetSpawned)
        ObjectPool::Remove(map, state.Target);
    for (ObjectGuid& decoy : state.Decoys)
        ObjectPool::Remove(map, decoy);
    state.Decoys.clear();
    state.Placed = false;
}
