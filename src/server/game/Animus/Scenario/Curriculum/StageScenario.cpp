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

#include <atomic>
#include "StageScenario.h"
#include <set>
#include "ResetTiming.h"
#include "Baselines.h"
#include "CharmInfo.h"
#include "Battleground.h"
#include "BotAccounts.h"
#include "Config.h"
#include "Containers.h"
#include "ObjectAccessor.h"
#include "CoreBlock.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DuelBlock.h"
#include "MoveBlock.h"
#include "World.h"
#include "EncoderSupport.h"
#include "Encounters.h"
#include "SpellMgr.h"
#include "Env.h"
#include "EnvPool.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MapDefines.h"
#include "Opponents.h"
#include "PetBlock.h"
#include "Player.h"
#include "Random.h"
#include "SeatCharacter.h"
#include "SeatEncoder.h"
#include "CombatReward.h"
#include "SpawnArea.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "StageDefinition.h"
#include <cmath>
#include <numeric>
#include "StringFormat.h"
#include "Supplies.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace
{
    using namespace Animus::Curriculum;
    using namespace Animus::SpellChecks;
    using Encoding::RelativePosition;

    static_assert(MAX_SEATS <= Animus::BotAccounts::SEATS_PER_ENV, "every seat needs its own bot accounts");
    static_assert(MAX_SEATS <= Animus::MAX_AGENTS, "every seat needs a column in the per-agent stats");
    static_assert(MAX_SEATS == RAID_GROUPS * GROUP_SEATS, "the seats are the raid's groups");
    static_assert(PARTY_MEMBERS == GROUP_MEMBERS + SPOTLIGHT_SLOTS, "teammate slots are the group and the spotlights");

    constexpr float PARTY_SPACING = 3.0f;
    /// The shortest scatter worth asking for. Under a yard PathGenerator builds a spline with no length, which
    /// is the zero-length-jump fault again: Validate() checks a path's size and its velocity, never its length.
    constexpr float SCATTER_MIN = 1.0f;
    constexpr float REWARD_TUNING_MS = 50.0f;       // per-decision reward terms are tuned for this decision interval
    constexpr float MAX_COMBAT_TIME_MS = 60000.0f;
    constexpr float MAX_UNSEEN_TIME_MS = 20000.0f;
    constexpr float GOAL_RANGE_SLACK_YARDS = 5.0f;  // a ranged spec holds its range to within this (SeatGoal::Position)
    constexpr float LOW_HEALTH_PCT = 35.0f;         // a friend below this is low (low_health_seconds)
    /// How far a cast counts as one this seat could have answered (interruptible_casts_seen): an interrupt's own
    /// range, near enough, and beyond it the press was never available anyway.
    constexpr float INTERRUPTIBLE_CAST_RANGE = 30.0f;
    /// How often the nearest hazard is searched for, and how far. A ground effect does not move, so between searches
    /// the cached one is simply measured again: the search is a grid visit, the measurement is arithmetic.
    constexpr uint32 HAZARD_SEARCH_MS = 1000;

    /// Whether the class itself can make itself stealthed, asked of its trainers' spell list.
    ///
    /// Not of the action catalog, which is the union over every race the class may be: that union holds
    /// Shadowmeld (58984), the night elf racial, and answering from it returns eleven of the eighteen
    /// class/roles. Shadowmeld is a way to hide -- stage 17 is about exactly that and offers it to everyone --
    /// but it is not stealth: it breaks on movement, so it cannot be used to close on anything, which is the
    /// whole of what the stealth stage asks for. The kit is per class, so what it holds is true of every
    /// member of the class rather than of one race of it.
    bool CanStealth(Animus::Curriculum::ClassAssets const& assets)
    {
        if (!assets.Kit)
            return false;

        for (Animus::Curriculum::ClassKit::KitSpell const& kitSpell : assets.Kit->Spells())
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(kitSpell.SpellId);
                spell && spell->HasAura(SPELL_AURA_MOD_STEALTH))
                return true;

        return false;
    }

    /// The core's breath, in game milliseconds: WaterBreath.Timer, 180 s by default (World::LoadConfigSettings).
    uint32 BreathMs()
    {
        return std::max<uint32>(1000, sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER));
    }

    /// How long an accepted resurrection is given to land before the offer may be taken again. A delayed
    /// teleport reschedules the resurrect (Player::ProcessDelayedOperations), so it does not always finish on
    /// the decision it was accepted on.
    constexpr uint64 RESURRECT_RETRY_MS = 5000;
    constexpr float HAZARD_SEARCH_RANGE = 30.0f;
    constexpr int32 ABSORB_EXPIRY_SLACK_MS = 500;   // an absorb gone with more than a decision and this left soaked it

    /// Version of stage.json (2 adds the stage's arenas, 3 each arena's seat plan and the cast list).
    constexpr uint32 STAGE_FILE_FORMAT = 3;

    /// How a character's talent points are spent this episode (CurriculumTuning::CharacterTuning).
    SeatCharacter::TalentPlan RandomTalentPlan(CurriculumTuning::CharacterTuning const& tuning)
    {
        int32 const roll = irand(0, 99);
        if (roll < tuning.NoisyTalentChance)
            return SeatCharacter::TalentPlan::Noisy;
        if (roll < tuning.NoisyTalentChance + tuning.RandomTalentChance)
            return SeatCharacter::TalentPlan::Random;

        return SeatCharacter::TalentPlan::Standard;
    }

    /// How many level bands an evaluation spreads its seeds over (LEVEL_BANDS x 20 levels).
    constexpr uint32 LEVEL_BANDS = 4;

    /// A level every seat's class can be: `fixed` when set (raised to minLevel), else drawn from the tuning: the high
    /// levels, the low levels (when the classes can be that low), or any level.
    ///
    /// An evaluation episode takes its band from its seed instead (`seedIndex`), so every class/role meets every
    /// band in equal numbers however the training draw is weighted: training sees the levels the shipped companions
    /// play, and the evaluation still measures all of them. A band the seat's classes cannot reach (a death knight
    /// below 55) falls through to the next one up.
    uint8 RandomLevel(uint8 minLevel, uint32 fixed, CurriculumTuning::CharacterTuning const& tuning,
        uint32 seedIndex, uint32 layouts)
    {
        if (fixed)
            return uint8(std::clamp<uint32>(fixed, minLevel, DEFAULT_MAX_LEVEL));

        if (seedIndex != Animus::NO_EPISODE_SEED)
        {
            uint32 const width = std::max<uint32>(1, DEFAULT_MAX_LEVEL / LEVEL_BANDS);
            uint32 const band = (seedIndex / std::max<uint32>(1, layouts)) % LEVEL_BANDS;
            uint32 const last = std::min<uint32>(DEFAULT_MAX_LEVEL, (band + 1) * width);
            if (last >= minLevel)
                return uint8(urand(std::max<uint32>(minLevel, band * width + 1), last));

            return uint8(urand(minLevel, DEFAULT_MAX_LEVEL));
        }

        uint32 const highFirst = std::clamp<uint32>(tuning.HighLevelFirst, 1, DEFAULT_MAX_LEVEL);
        uint32 const lowLast = std::min<uint32>(tuning.LowLevelLast, DEFAULT_MAX_LEVEL);
        int32 const roll = irand(0, 99);
        bool const high = roll < tuning.HighLevelChance;
        bool const low = !high && roll < tuning.HighLevelChance + tuning.LowLevelChance;
        if (minLevel <= highFirst && high)
            return uint8(urand(highFirst, DEFAULT_MAX_LEVEL));
        if (minLevel <= lowLast && low)
            return uint8(urand(minLevel, lowLast));
        return uint8(urand(minLevel, DEFAULT_MAX_LEVEL));
    }

    /// The classic makeup for `seats` seats: somebody who can hold the pull and somebody who can keep the hurt one
    /// up at the head of every group, and no demand at all on the rest. A party is one group, so it reads as it
    /// always did; a raid gets one of each per group, which is what a raid brings.
    ///
    /// The last of those is the honest part. A group's third, fourth and fifth seats were "damage", which was a
    /// name for having nothing asked of them -- so now nothing is asked of them, and whoever turns up can play.
    std::array<AptitudeDemand, MAX_SEATS> ClassicDemands(uint32 seats)
    {
        std::array<AptitudeDemand, MAX_SEATS> demands;
        demands.fill(AptitudeDemand::Anything());
        for (uint32 seat = 0; seat < seats && seat < MAX_SEATS; ++seat)
        {
            uint32 const inGroup = seat % GROUP_SEATS;
            if (inGroup == 0)
                demands[seat] = AptitudeDemand::HoldsThePull();
            else if (inGroup == 1)
                demands[seat] = AptitudeDemand::KeepsThemUp();
        }

        return demands;
    }

    /// A demand drawn at random: somebody to hold the pull with `tankChance` percent, somebody to keep the hurt one
    /// up with `healerChance`, and nothing in particular otherwise. One roll from the world thread's random
    /// numbers, so seeded episodes draw the same makeup.
    AptitudeDemand RollDemand(int32 tankChance, int32 healerChance)
    {
        int32 const roll = irand(0, 99);
        if (roll < tankChance)
            return AptitudeDemand::HoldsThePull();
        if (roll < tankChance + healerChance)
            return AptitudeDemand::KeepsThemUp();

        return AptitudeDemand::Anything();
    }

    /// How many party seats get a character, drawn from the size weights.
    uint32 RandomPartySize(CurriculumTuning::PartyTuning const& tuning)
    {
        std::array<int32, MAX_SEATS> const weights =
            { tuning.SizeWeight1, tuning.SizeWeight2, tuning.SizeWeight3, tuning.SizeWeight4 };

        int32 total = 0;
        for (int32 weight : weights)
            total += weight;
        if (total <= 0)
            return MAX_SEATS;

        int32 roll = irand(0, total - 1);
        for (uint32 size = 1; size <= MAX_SEATS; ++size)
        {
            if (roll < weights[size - 1])
                return size;
            roll -= weights[size - 1];
        }

        return MAX_SEATS;
    }

    float OtherPower(Unit const* unit)
    {
        Powers const power = unit->getPowerType();
        if (power == POWER_MANA)
            return 0.0f;

        uint32 const maxPower = unit->GetMaxPower(power);
        return maxPower ? float(unit->GetPower(power)) / float(maxPower) : 0.0f;
    }

    /// Write `content` to `path` unless the file already holds exactly that: manifests are rebuilt at every start but
    /// rarely change.
    bool WriteIfChanged(std::filesystem::path const& path, std::string const& content)
    {
        std::error_code error;
        if (std::filesystem::file_size(path, error) == content.size() && !error)
        {
            std::ifstream existing(path, std::ios::binary);
            std::string const current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (current == content)
                return true;
        }

        std::filesystem::path const partial = path.string() + ".partial";
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            file << content;
            if (!file)
                return false;
        }

        std::filesystem::rename(partial, path, error);
        return !error;
    }
}

Animus::Curriculum::StageScenario::StageScenario(StageSettings const& settings, StageDefinition const& stage)
    : _stage(stage), _tuning(CurriculumTuning::Load(settings.TuningPrefix)),
    _spawnMapId(stage.MapId ? stage.MapId : settings.SpawnMapId),
    _spawnPoint(stage.MapId && !stage.SpawnPoints.empty() ? stage.SpawnPoints.front() : settings.SpawnPosition),
    _seatCount(stage.SeatCount()), _level(settings.Level),
    _decisionScale(float(settings.DecisionMs) / REWARD_TUNING_MS), _decisionMs(settings.DecisionMs)
{
    if (MapEntry const* mapEntry = sMapStore.LookupEntry(_spawnMapId))
        _continent = !mapEntry->Instanceable();

    // A reset stays on its env's own continent replica unless an arena sends the episode elsewhere (an instance, a
    // battleground, a quest giver's, a node's or an inn's map) or builds what every map shares and nothing locks: a
    // core group (an owner, a party), the director's orders over the others. Only then is it safe to run on the map
    // thread (EnvPool, ResetDefer); every other stage resets on the world thread as it always has.
    _resetsStayOnMap = _continent && !stage.AnyArena([](ArenaDefinition const& arena)
    {
        return arena.Owner || arena.PartyGroup || arena.Directed || arena.Against == Opposition::Instance
            || arena.Against == Opposition::Quest || arena.Against == Opposition::Gather
            || arena.Against == Opposition::Town || arena.Against == Opposition::Flag;
    });

    // A map-thread reset cannot load a grid's collision and navmesh: while map tasks run, those wait for the world
    // thread (MapMgr::MapTasksRunning). An encounter built from a spawn point on a grid no one has stood on would find
    // no mesh there and fail until the next decision loaded it. So every grid a seat can start in, and the grids an
    // objective or a march reaches from there, are loaded now, on the world thread, once: terrain and collision only.
    if (_resetsStayOnMap)
    {
        if (Map* base = sMapMgr->CreateBaseMap(_spawnMapId))
        {
            constexpr float REACH = 80.0f;
            std::set<std::pair<uint32, uint32>> grids;
            auto addAround = [&grids](std::vector<Position> const& points)
            {
                for (Position const& point : points)
                    for (int32 dx = -1; dx <= 1; ++dx)
                        for (int32 dy = -1; dy <= 1; ++dy)
                        {
                            GridCoord const grid = Acore::ComputeGridCoord(point.GetPositionX() + float(dx) * REACH,
                                point.GetPositionY() + float(dy) * REACH);
                            grids.emplace(grid.x_coord, grid.y_coord);
                        }
            };
            addAround(stage.SpawnPoints);
            addAround(stage.HeldOutSpawnPoints);
            for (ArenaDefinition const& arena : stage.Arenas)
            {
                addAround(arena.SpawnPoints);
                addAround(arena.HeldOutSpawnPoints);
            }
            for (auto const& [x, y] : grids)
                base->EnsureGridCreated(GridCoord(x, y));
            LOG_INFO("module.animus", "{}: resets on the map threads; {} spawn-area grids of map {} loaded", Name(),
                grids.size(), _spawnMapId);
        }
    }

    // How many envs share one continent map. A map has 31 phases to give away and an env needs one of its own,
    // so that is the ceiling however few replicas were asked for; asking for more replicas than that makes the
    // blocks smaller. At the default (0) a pool of 31 envs or fewer is one map, exactly as a continent stage
    // has always been, and a larger pool is no longer capped at 31.
    uint32 const replicas = std::max<uint32>(1, settings.ContinentReplicas);
    _envsPerReplica = std::clamp<uint32>((settings.Envs + replicas - 1) / replicas, 1, ENV_PHASE_BITS);

    // The classes this run plays: StageSettings::Classes, or all of them.
    for (ClassProfile const& profile : ClassProfiles())
    {
        if (!settings.Classes.empty() && std::find(settings.Classes.begin(), settings.Classes.end(),
            profile.Name) == settings.Classes.end())
            continue;

        ClassAssets const& assets = ClassAssets::For(profile);
        if (assets.Races.empty())
            continue;

        // A stage about closing on someone unseen is played only by the classes that can actually do it.
        if (_stage.NeedsStealth && !CanStealth(assets))
            continue;

        Layout layout = Layout::Build(profile, _stage);
        layout.Index = uint16(_layouts.size());
        _layouts.push_back(std::move(layout));
    }

    // A scripted owner or enemy player can be any class, whatever StageSettings::Classes says: build every
    // profile's assets now (seconds each) rather than on the world thread in the middle of an episode reset.
    if (_stage.AnyArena([](ArenaDefinition const& arena)
        {
            return arena.Owner || arena.Against == Opposition::ScriptedPlayer;
        }))
        for (ClassProfile const& profile : ClassProfiles())
            ClassAssets::For(profile);

    // A stage with any learned-directed arena carries the two director agents in every episode: the spec is
    // fixed for the run, so the undirected episodes mark them absent instead (AgentPresence).
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.Directed && arena.DirectorLearned; }))
    {
        Layout director = Layout::BuildDirector(_stage);
        director.Index = uint16(_layouts.size());
        _directorLayout = director.Index;
        _layouts.push_back(std::move(director));
    }

    // The owner's own row, after the seats and the directors, where an arena plays it from a frozen checkpoint.
    _castOwner = _stage.AnyArena([](ArenaDefinition const& arena) { return arena.Owner && arena.OwnerCast; });
    _spec.AgentsPerEnv = _seatCount + (HasDirectors() ? TEAM_COUNT : 0) + (_castOwner ? 1 : 0);
    for (Layout const& layout : _layouts)
    {
        _spec.ObsDim = std::max(_spec.ObsDim, layout.ObsDim);
        _spec.NumActions = std::max(_spec.NumActions, layout.NumActions);
        _spec.Layouts.push_back(LayoutSpec{ layout.Director ? DirectorLayout::Name() : layout.Profile->Name,
            layout.ObsDim, layout.NumActions });
    }

    _spec.StateDim = STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + PACK_SLOTS * STATE_ENEMY_FEATURES;
    _data.resize(settings.Envs);

    // The encounters any of the stage's arenas uses, in build order.
    uint32 const envs = settings.Envs;
    OpponentEncounter* opponent = nullptr;
    PullsEncounter* pulls = nullptr;
    CreatureEncounter* creature = nullptr;
    AmbushEncounter* ambush = nullptr;
    TravelEncounter* travel = nullptr;
    FlagEncounter* flag = nullptr;
    InstanceEncounter* instance = nullptr;

    auto const add = [this](auto encounter)
    {
        auto* raw = encounter.get();
        _encounters.push_back(std::move(encounter));
        return raw;
    };

    auto const fightsPlayer = [](ArenaDefinition const& arena)
    {
        return arena.Against == Opposition::ScriptedPlayer || arena.Against == Opposition::MirrorSeat
            || arena.Against == Opposition::Flag;
    };
    auto const hasPulls = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Pulls; };
    auto const hasCreature = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Creature; };
    auto const hasHazards = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Hazards; };
    auto const hasAmbush = [](ArenaDefinition const& arena) { return arena.Ambushers > 0; };
    auto const hasTravel = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Travel; };
    auto const hasFlag = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Flag; };
    auto const hasInstance = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Instance; };
    auto const hasQuest = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Quest; };
    auto const hasGather = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Gather; };
    auto const hasTown = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Town; };
    auto const directed = [](ArenaDefinition const& arena) { return arena.Directed; };

    // Build order matters: the owner comes before the party group (which it leads) and the pulls (which spawn around
    // it); both check it. Rewards do not depend on each other's order: what several read (a seat's damage taken, the
    // owner's totals) is computed before any encounter's Reward.
    if (_stage.AnyArena(fightsPlayer))
        opponent = add(std::make_unique<OpponentEncounter>(*this, envs));
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.Owner; }))
        _owner = add(std::make_unique<OwnerEncounter>(*this, envs));
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.PartyGroup; }))
        _party = add(std::make_unique<PartyEncounter>(*this, envs));
    // After the owner and the group: it moves both to the boss.
    if (_stage.AnyArena(hasInstance))
        instance = add(std::make_unique<InstanceEncounter>(*this, envs));
    // Life outside the fight: each fixes its own map and spawn (BeforeLevel), builds after the seat is placed.
    Encounter* quest = nullptr;
    Encounter* gather = nullptr;
    Encounter* town = nullptr;
    if (_stage.AnyArena(hasQuest))
        quest = add(std::make_unique<QuestEncounter>(*this, envs));
    if (_stage.AnyArena(hasGather))
        gather = add(std::make_unique<GatherEncounter>(*this, envs));
    if (_stage.AnyArena(hasTown))
        town = add(std::make_unique<TownEncounter>(*this, envs));
    if (_stage.AnyArena(hasPulls))
        pulls = add(std::make_unique<PullsEncounter>(*this, envs));
    if (_stage.AnyArena(hasCreature))
        creature = add(std::make_unique<CreatureEncounter>(*this, envs));
    // Nothing to fight and nothing to order: it only puts fire on the ground, so it can go anywhere in the order.
    Encounter* hazards = nullptr;
    if (_stage.AnyArena(hasHazards))
        hazards = add(std::make_unique<HazardEncounter>(*this, envs));
    // After the owner and the pulls: ambushers find the owner and take the slots the pull leaves.
    if (_stage.AnyArena(hasAmbush))
        ambush = add(std::make_unique<AmbushEncounter>(*this, envs));
    if (_stage.AnyArena(hasTravel))
        travel = add(std::make_unique<TravelEncounter>(*this, envs));
    // After the opponent, which makes the two seats enemies.
    if (_stage.AnyArena(hasFlag))
        flag = add(std::make_unique<FlagEncounter>(*this, envs));
    // Last: its orders are read from what every other encounter has already set up.
    Encounter* director = nullptr;
    if (_stage.AnyArena(directed))
    {
        auto owned = std::make_unique<DirectorEncounter>(*this, envs);
        _director = owned.get();
        director = add(std::move(owned));
    }

    // The order episode info columns and reward terms are listed in. An encounter left out of this list still
    // runs -- it is only the columns and the terms that are missed -- which is how hazard_patches went missing
    // while the drill around it worked.
    for (Encounter* encounter : std::initializer_list<Encounter*>{ creature, pulls, instance, quest, gather, town,
        hazards, _owner, _party, opponent, ambush, travel, flag, director })
        if (encounter)
            _rewardOrder.push_back(encounter);

    // Which of them each arena uses, its share of episodes and its episode length.
    uint32 longestMs = settings.EpisodeSeconds * IN_MILLISECONDS;
    for (ArenaDefinition const& arena : _stage.Arenas)
    {
        auto const uses = [&](Encounter const* encounter)
        {
            return (encounter == opponent && fightsPlayer(arena)) || (encounter == _owner && arena.Owner)
                || (encounter == _party && arena.PartyGroup) || (encounter == pulls && hasPulls(arena))
                || (encounter == creature && hasCreature(arena)) || (encounter == ambush && hasAmbush(arena))
                || (encounter == travel && hasTravel(arena)) || (encounter == flag && hasFlag(arena))
                || (encounter == hazards && hasHazards(arena))
                || (encounter == instance && hasInstance(arena))
                || (encounter == quest && hasQuest(arena)) || (encounter == gather && hasGather(arena))
                || (encounter == town && hasTown(arena))
                || (encounter == director && directed(arena));
        };

        std::vector<Encounter*>& build = _arenaEncounters.emplace_back();
        for (auto const& encounter : _encounters)
            if (uses(encounter.get()))
                build.push_back(encounter.get());

        std::vector<Encounter*>& reward = _arenaRewardOrder.emplace_back();
        for (Encounter* encounter : _rewardOrder)
            if (uses(encounter))
                reward.push_back(encounter);

        _arenaWeights.push_back(sConfigMgr->GetOption<uint32>(
            Acore::StringFormat("{}Arena.{}.{}.Weight", settings.TuningPrefix, _stage.Name, arena.Name), arena.Weight,
            false));

        _arenaMaxRung.push_back(sConfigMgr->GetOption<int32>(
            Acore::StringFormat("{}Arena.{}.{}.MaxRung", settings.TuningPrefix, _stage.Name, arena.Name),
            arena.MaxRung, false));

        uint32 const episodeMs = (arena.EpisodeSeconds ? arena.EpisodeSeconds : settings.EpisodeSeconds)
            * IN_MILLISECONDS;
        _arenaEpisodeMs.push_back(episodeMs);
        longestMs = std::max(longestMs, episodeMs);
    }

    if (std::all_of(_arenaWeights.begin(), _arenaWeights.end(), [](uint32 weight) { return weight == 0; }))
    {
        LOG_ERROR("module.animus", "{}: every arena weight is 0; the arenas are drawn evenly", Name());
        std::fill(_arenaWeights.begin(), _arenaWeights.end(), 1);
    }

    _spec.LongestEpisodeSeconds = longestMs / IN_MILLISECONDS;

    // Load it at startup rather than on the first episode. A hazard stage fights nothing but still draws its
    // ground from the pool (OpponentPool::RandomHazardSpell), and without this the first episode of every env
    // paid for the load.
    if (_stage.AnyArena(hasCreature) || _stage.AnyArena(hasPulls) || _stage.AnyArena(hasHazards))
        Opponents::OpponentPool::Instance();
    ConsumablePool::Instance();

    AddCoreEpisodeInfo();
    for (Encounter* encounter : _rewardOrder)
        encounter->AddEpisodeInfo(_info);

    // What the seats are paid for, term by term.
    for (Encounter* encounter : _rewardOrder)
    {
        for (RewardTerm term : encounter->RewardTerms())
        {
            std::string name = "reward_" + std::string(RewardTermName(term));
            if (_info.Contains(name))
                continue;

            _info.Add(std::move(name), [this, term](Env const& env, uint32 seat)
            {
                return Data(env).Seats[seat].Rewards.Episode(term);
            });
        }
    }

    // Repeats, self-healing, goals and ground effects are paid in every stage, by the scenario rather than an
    // encounter, so they are listed here: a term no encounter claims has no column, and a charge with no column is
    // invisible in exactly the run where it matters.
    for (RewardTerm term : { RewardTerm::Repeat, RewardTerm::SelfHealing, RewardTerm::GoalMatch,
        RewardTerm::Hazard, RewardTerm::HealingMana })
        _info.Add("reward_" + std::string(RewardTermName(term)), [this, term](Env const& env, uint32 seat)
        {
            return Data(env).Seats[seat].Rewards.Episode(term);
        });

    _spec.EpisodeInfoDim = _info.Size();
    _spec.GoalCount = GOAL_COUNT;

    if (!settings.LayoutsDir.empty())
        WriteStageFiles(settings);

    LOG_DEBUG("module.animus", "{}: {} seats per env, {} class/role layouts (obs up to {}, actions up to {}), state {}",
        Name(), _seatCount, _layouts.size(), _spec.ObsDim, _spec.NumActions, _spec.StateDim);
    if (_stage.Arenas.size() > 1)
        for (std::size_t arena = 0; arena < _stage.Arenas.size(); ++arena)
            LOG_DEBUG("module.animus", "{}: arena {} (weight {}, {} s episodes)", Name(), _stage.Arenas[arena].Name,
                _arenaWeights[arena], _arenaEpisodeMs[arena] / IN_MILLISECONDS);
}

std::vector<Position> const& Animus::Curriculum::StageScenario::SpawnGroundFor(Env const& env) const
{
    static std::vector<Position> const none;
    if (!_stage.MapId)
        return none;

    // An arena that needs its own ground stands where it says, not where the env does; and a scored episode
    // stands on the control ground, which training never touches, so what the gates measure is whether the seat
    // can read terrain at all rather than whether it has seen this terrain before. An arena or a stage with no
    // control of its own falls back to the ground it trains on, and says so by being unable to tell the two apart.
    uint32 const arena = Data(env).Arena;
    if (arena < _stage.Arenas.size() && !_stage.Arenas[arena].SpawnPoints.empty())
    {
        ArenaDefinition const& definition = _stage.Arenas[arena];
        if (env.Evaluating && !definition.HeldOutSpawnPoints.empty())
            return definition.HeldOutSpawnPoints;

        return definition.SpawnPoints;
    }

    if (env.Evaluating && !_stage.HeldOutSpawnPoints.empty())
        return _stage.HeldOutSpawnPoints;

    return _stage.SpawnPoints;
}

uint32 Animus::Curriculum::StageScenario::EpisodeMapId(Env const& env) const
{
    EnvState const& data = Data(env);
    return data.HasEpisodeMap ? data.EpisodeMapId : _spawnMapId;
}

Position const& Animus::Curriculum::StageScenario::SpawnPointFor(Env const& env) const
{
    if (Data(env).HasEpisodeSpawn)
        return Data(env).EpisodeSpawn;

    std::vector<Position> const& ground = SpawnGroundFor(env);
    if (ground.empty())
        return _spawnPoint;

    return ground[std::min<std::size_t>(Data(env).Spawn, ground.size() - 1)];
}

void Animus::Curriculum::StageScenario::ScatterSeats(Env const& env, Map* map) const
{
    ArenaDefinition const& arena = Arena(env);
    if (arena.SpawnScatter < SCATTER_MIN || !map)
        return;

    EnvState const& data = Data(env);
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
    {
        Player* bot = SeatBot(env, seat);
        if (!bot)
            continue;

        // The facing costs nothing and needs no ground to be true, so it is taken whether the offset is found or
        // not: a seat that cannot be moved in a tight room can still open the episode looking somewhere else.
        Position where(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            frand(0.0f, 2.0f * float(M_PI)));

        // FindPlace is the same validation the objective gets -- on the mesh, reachable, and inside the building
        // when the arena is -- which is the reason to spend a pathfind here rather than offset blindly into a
        // wall. A room that has no room for one keeps the spawn point; Relocate leaves the facing alone.
        Position place;
        if (TravelEncounter::FindPlace(bot, map, SCATTER_MIN, arena.SpawnScatter, false, place, 0.0f, nullptr,
            false, nullptr, arena.Indoors))
            where.Relocate(place.GetPositionX(), place.GetPositionY(), place.GetPositionZ());

        BotFactory::TeleportWithinMap(bot, where);
    }
}

uint32 Animus::Curriculum::StageScenario::ReplicaOf(Env const& env) const
{
    return env.Index / _envsPerReplica;
}

uint32 Animus::Curriculum::StageScenario::EnvPhase(Env const& env)
{
    // Phase 1 is the world's own; each env takes one of the other 31 bits. Envs are dealt to replicas in
    // consecutive blocks of at most this many, so the envs sharing a map always have distinct remainders and
    // therefore distinct bits.
    return uint32(1) << (1 + env.Index % ENV_PHASE_BITS);
}

Animus::Curriculum::ArenaDefinition const& Animus::Curriculum::StageScenario::Arena(Env const& env) const
{
    uint32 const arena = Data(env).Arena;
    return _stage.Arenas[arena < _stage.Arenas.size() ? arena : 0];
}

int32 Animus::Curriculum::StageScenario::ArenaMaxRung(Env const& env) const
{
    uint32 const arena = Data(env).Arena;
    return arena < _arenaMaxRung.size() ? _arenaMaxRung[arena] : -1;
}

bool Animus::Curriculum::StageScenario::Uses(Env const& env, Encounter const& encounter) const
{
    std::vector<Encounter*> const& active = ActiveEncounters(env);
    return std::find(active.begin(), active.end(), &encounter) != active.end();
}

std::vector<Animus::Curriculum::Encounter*> const& Animus::Curriculum::StageScenario::ActiveEncounters(
    Env const& env) const
{
    static std::vector<Encounter*> const none;
    uint32 const arena = Data(env).Arena;
    return arena < _arenaEncounters.size() ? _arenaEncounters[arena] : none;
}

std::vector<Animus::Curriculum::Encounter*> const& Animus::Curriculum::StageScenario::ActiveRewardOrder(
    Env const& env) const
{
    static std::vector<Encounter*> const none;
    uint32 const arena = Data(env).Arena;
    return arena < _arenaRewardOrder.size() ? _arenaRewardOrder[arena] : none;
}

uint32 Animus::Curriculum::StageScenario::DrawArena() const
{
    if (_arenaWeights.size() == 1)
        return 0;

    uint32 total = 0;
    for (uint32 weight : _arenaWeights)
        total += weight;

    uint32 roll = urand(0, total - 1);
    for (uint32 arena = 0; arena < _arenaWeights.size(); ++arena)
    {
        if (roll < _arenaWeights[arena])
            return arena;
        roll -= _arenaWeights[arena];
    }

    return 0;
}

Animus::Curriculum::StageScenario::~StageScenario() = default;

char const* Animus::Curriculum::StageScenario::Name() const
{
    return _stage.Name.c_str();
}

void Animus::Curriculum::StageScenario::AddCoreEpisodeInfo()
{
    auto const seat = [this](Env const& env, uint32 index) -> SeatState const& { return Data(env).Seats[index]; };

    _info.Add("damage", [](Env const& env, uint32 index) { return float(env.EpisodeStats[index].Damage); });
    _info.Add("dps", [](Env const& env, uint32 index)
    {
        float const seconds = std::max(0.001f, float(env.EpisodeElapsedMs) / 1000.0f);
        return float(env.EpisodeStats[index].Damage) / seconds;
    });
    _info.Add("white_damage", [](Env const& env, uint32 index) { return float(env.EpisodeStats[index].WhiteDamage); });
    _info.Add("special_damage", [](Env const& env, uint32 index)
    {
        return float(env.EpisodeStats[index].SpecialDamage);
    });
    _info.Add("level", [seat](Env const& env, uint32 index) { return float(seat(env, index).Level); });
    _info.Add("race", [seat](Env const& env, uint32 index) { return float(seat(env, index).Race); });
    _info.Add("spec", [seat](Env const& env, uint32 index) { return float(seat(env, index).Spec); });
    // Which way this character's talents were spent (SeatCharacter::TalentPlan): 0 standard, 1 noisy, 2 random.
    _info.Add("talent_plan", [seat](Env const& env, uint32 index)
    {
        return float(uint32(seat(env, index).TalentPlan));
    });
    _info.Add("unspent_talent_points", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).UnspentTalentPoints);
    });
    _info.Add("equipped_items", [seat](Env const& env, uint32 index) { return float(seat(env, index).EquippedItems); });
    _info.Add("spell_casts", [seat](Env const& env, uint32 index) { return float(seat(env, index).SpellCasts); });
    // Leaving the ground (MoveBlock's jump, Encoding::FallToGround): jumps launched, jumps pressed with nowhere to
    // land, drops (a landing more than a step below the seat) and the deepest of them, drops made under Slow Fall
    // or Levitate, and the falls that followed -- whether there was one, what they cost in health, and whether one
    // killed the seat. A drill about ledges reads these; every other stage gets them for free.
    // Water, for every stage: time swimming, time with the head under, surfacings, the most of a breath spent
    // (past 1 while drowning), damage taken under water past the breath and whether it killed the seat, and time
    // walking on water under an aura for it.
    _info.Add("swim_seconds", [seat](Env const& env, uint32 index) { return float(seat(env, index).WaterMs) / 1000.0f; });
    _info.Add("dive_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).SubmergedMs) / 1000.0f;
    });
    _info.Add("breaths", [seat](Env const& env, uint32 index) { return float(seat(env, index).Breaths); });
    _info.Add("breath_spent", [seat](Env const& env, uint32 index) { return seat(env, index).BreathSpentMax; });
    _info.Add("drowning_damage", [seat](Env const& env, uint32 index) { return seat(env, index).DrowningDamage; });
    // A death under water with drowning damage behind it is a drowning whether or not a decision came after it to
    // set the flag: a drowned seat ends the episode, and the next ApplySeatAction never runs.
    _info.Add("drowned", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        Player const* bot = env.FindBot(index);
        return state.Drowned || (bot && !bot->IsAlive() && state.DrowningDamage > 0.0f) ? 1.0f : 0.0f;
    });
    _info.Add("water_walk_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).WaterWalkMs) / 1000.0f;
    });
    // What a build did about its air: time in a druid's Aquatic Form, and water-breathing spells started (Unending
    // Breath, Water Breathing, Aquatic Form again). Read by the chain drill; every stage gets them for free.
    _info.Add("aquatic_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).AquaticMs) / 1000.0f;
    });
    _info.Add("breathing_casts", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).BreathingCasts);
    });
    _info.Add("jumps", [seat](Env const& env, uint32 index) { return float(seat(env, index).Jumps); });
    _info.Add("jumps_refused", [seat](Env const& env, uint32 index) { return float(seat(env, index).JumpsRefused); });
    _info.Add("drops", [seat](Env const& env, uint32 index) { return float(seat(env, index).Drops); });
    _info.Add("drop_yards", [seat](Env const& env, uint32 index) { return seat(env, index).DropYards; });
    _info.Add("feather_falls", [seat](Env const& env, uint32 index) { return float(seat(env, index).FeatherFalls); });
    _info.Add("fell", [seat](Env const& env, uint32 index) { return seat(env, index).Falls ? 1.0f : 0.0f; });
    _info.Add("fall_damage", [seat](Env const& env, uint32 index) { return seat(env, index).FallDamage; });
    _info.Add("fall_deaths", [seat](Env const& env, uint32 index) { return float(seat(env, index).FallDeaths); });
    // Durative actions: how many the seat started and how long they ran.
    _info.Add("options_started", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).OptionPresses);
    });
    _info.Add("option_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).OptionMs) / 1000.0f;
    });
    _info.Add("trinket_uses", [seat](Env const& env, uint32 index) { return float(seat(env, index).TrinketUses); });
    _info.Add("item_uses", [seat](Env const& env, uint32 index) { return float(seat(env, index).ItemUses); });
    _info.Add("class", [seat](Env const& env, uint32 index)
    {
        Layout const* layout = seat(env, index).L;
        return layout ? float(layout->Profile->Class) : 0.0f;
    });
    // What the character could actually do, so a run can read how its builds fared without anybody having named
    // them. The spec column above carries the build's name, which is what the gates key on (target.spec_metrics);
    // these two are the measurement, and they are what tells a strange build apart from the template it came from.
    _info.Add("aptitude_mitigation", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return state.L ? state.Apt[Aptitude::MITIGATION] : 0.0f;
    });
    _info.Add("aptitude_healing", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return state.L ? std::max(state.Apt[Aptitude::DIRECT_HEAL], state.Apt[Aptitude::HOT_HEAL]) : 0.0f;
    });
    // A party seat left empty this episode reports 0: ignore its row.
    _info.Add("present", [seat](Env const& env, uint32 index) { return seat(env, index).L ? 1.0f : 0.0f; });
    // The episode's arena: its index in stage.json's arenas.
    _info.Add("arena", [this](Env const& env, uint32)
    {
        uint32 const arena = Data(env).Arena;
        return arena == NO_ARENA ? 0.0f : float(arena);
    });
    // Which spawn point the episode was built from, and which one it drew first. An index into the stage's (or
    // the arena's) SpawnPoints, or HeldOutSpawnPoints while evaluating -- the two lists are never mixed, so the
    // column means whichever list the episode drew from.
    //
    // Read them together. Equal, the first choice worked. Different, that point could not build an episode and
    // the reset moved on, and a point that is drawn often and never built from is one no episode can start at:
    // a control room that scores nothing while still being counted as control ground. That is not hypothetical
    // -- it is how stage2_indoor came to be scored on two of its three rooms without anything saying so.
    _info.Add("spawn_point", [this](Env const& env, uint32) { return float(Data(env).Spawn); });
    _info.Add("spawn_drawn", [this](Env const& env, uint32) { return float(Data(env).SpawnDrawn); });
    // The other side of a self-play episode: an evaluation against a scripted opponent leaves its row out.
    _info.Add("opponent_seat", [this](Env const& env, uint32 index)
    {
        return IsOpponentSeat(env, index) ? 1.0f : 0.0f;
    });

    // Fights against something that fights back.
    auto const tally = [this](Env const& env, uint32 index) -> CombatTally const&
    {
        return Data(env).Seats[index].Combat;
    };

    _info.Add("killed", [tally](Env const& env, uint32 index) { return tally(env, index).Killed ? 1.0f : 0.0f; });
    _info.Add("died", [tally](Env const& env, uint32 index) { return tally(env, index).Died ? 1.0f : 0.0f; });
    _info.Add("time_to_kill", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return float(combat.Killed ? combat.KillTimeMs : env.EpisodeElapsedMs) / 1000.0f;
    });
    _info.Add("damage_taken", [tally](Env const& env, uint32 index) { return float(tally(env, index).DamageTaken); });
    _info.Add("health_left", [](Env const& env, uint32 index)
    {
        Player* bot = env.FindBot(index);
        return bot ? bot->GetHealthPct() / 100.0f : 0.0f;
    });
    _info.Add("stealth_openers", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).StealthOpeners);
    });
    _info.Add("stealth_utility_casts", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).StealthUtilityCasts);
    });
    // Out-of-combat buffs, forms, stealth and summons, in the time they took (the stall grace's refund, uncapped).
    _info.Add("preparation_seconds", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).PreparationMs) / 1000.0f;
    });
    // Pets: how much of the damage they dealt, whether one died, and how the seat commanded them.
    _info.Add("pet_damage_share", [](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        return stats.Damage ? float(stats.PetDamage) / float(stats.Damage) : 0.0f;
    });
    // The seat's own damage by the game's damage class, as shares of all its damage (with pet_damage_share they add up
    // to 1): melee swings and melee abilities, ranged weapon attacks, and spells.
    _info.Add("melee_damage_share", [](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        return stats.Damage ? float(stats.MeleeDamage) / float(stats.Damage) : 0.0f;
    });
    _info.Add("shot_damage_share", [](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        return stats.Damage ? float(stats.ShotDamage) / float(stats.Damage) : 0.0f;
    });
    _info.Add("spell_damage_share", [](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        return stats.Damage ? float(stats.SpellDamage) / float(stats.Damage) : 0.0f;
    });
    _info.Add("pet_died", [seat](Env const& env, uint32 index) { return seat(env, index).PetDied ? 1.0f : 0.0f; });
    _info.Add("pet_abilities", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).PetAbilities);
    });
    _info.Add("pet_orders", [seat](Env const& env, uint32 index) { return float(seat(env, index).PetOrders); });
    // Each kind of pet order, and what the pet did while out: seconds out, and the share of that time it was
    // attacking something, set passive, or told to stay.
    for (auto const& [name, order] : std::initializer_list<std::pair<char const*, PetOrder>>{
        { "pet_attack_orders", PetOrder::Attack }, { "pet_passive_orders", PetOrder::Passive },
        { "pet_defensive_orders", PetOrder::Defensive }, { "pet_aggressive_orders", PetOrder::Aggressive },
        { "pet_follow_orders", PetOrder::Follow }, { "pet_stay_orders", PetOrder::Stay } })
        _info.Add(name, [seat, order](Env const& env, uint32 index)
        {
            return float(seat(env, index).PetOrderCounts[std::size_t(order)]);
        });
    _info.Add("pet_out_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).PetOutMs) / 1000.0f;
    });
    auto const petShare = [seat](Env const& env, uint32 index, uint32 SeatState::*ms)
    {
        SeatState const& s = seat(env, index);
        return s.PetOutMs ? float(s.*ms) / float(s.PetOutMs) : 0.0f;
    };
    _info.Add("pet_attacking_share", [petShare](Env const& env, uint32 index)
    {
        return petShare(env, index, &SeatState::PetAttackingMs);
    });
    _info.Add("pet_passive_share", [petShare](Env const& env, uint32 index)
    {
        return petShare(env, index, &SeatState::PetPassiveMs);
    });
    _info.Add("pet_staying_share", [petShare](Env const& env, uint32 index)
    {
        return petShare(env, index, &SeatState::PetStayingMs);
    });
    _info.Add("pet_at_start", [seat](Env const& env, uint32 index)
    {
        return seat(env, index).PetAtStart ? 1.0f : 0.0f;
    });
    _info.Add("pet_summoned", [tally](Env const& env, uint32 index)
    {
        return tally(env, index).PetSummoned ? 1.0f : 0.0f;
    });
    _info.Add("opponent", [this](Env const& env, uint32) { return float(Data(env).OpponentEntry); });
    _info.Add("casts_completed", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsCompleted);
    });
    _info.Add("casts_cancelled", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsCancelled);
    });
    _info.Add("cast_seconds_wasted", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastMsWasted) / 1000.0f;
    });
    _info.Add("cancelled_stopped", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsStopped);
    });
    _info.Add("cancelled_moved", [tally](Env const& env, uint32 index) { return float(tally(env, index).CastsMoved); });
    _info.Add("cancelled_target", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).CastsTargetLost);
    });
    _info.Add("cancelled_other", [tally](Env const& env, uint32 index) { return float(tally(env, index).CastsOther); });
    _info.Add("consumables_used", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).ConsumablesUsed);
    });
    _info.Add("self_resurrections", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).SelfResurrections);
    });

    // Why a fight was not won, read off how it ended: which of the two ways it was lost, whether it ever started,
    // how far the opponent was from dead and the bot from it, the form and power it ended in, and time the
    // opponent was out of reach (evading) or out of sight.
    _info.Add("timed_out", [tally](Env const& env, uint32 index) { return tally(env, index).TimedOut ? 1.0f : 0.0f; });
    _info.Add("engaged", [tally](Env const& env, uint32 index) { return tally(env, index).Engaged ? 1.0f : 0.0f; });
    _info.Add("engage_time", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return float(combat.Engaged ? combat.EngageMs : env.EpisodeElapsedMs) / 1000.0f;
    });
    _info.Add("target_health_left", [this](Env const& env, uint32 index)
    {
        Unit* target = SeatTarget(env, index);
        return target && target->IsAlive() ? target->GetHealthPct() / 100.0f : 0.0f;
    });
    _info.Add("distance_at_end", [this](Env const& env, uint32 index)
    {
        Player* bot = env.FindBot(index);
        Unit* target = SeatTarget(env, index);
        return bot && target && bot->IsInMap(target) ? bot->GetDistance(target) : 0.0f;
    });
    // ShapeshiftForm: 0 none, 1 cat, 2 tree, 5 bear, 8 dire bear, 17-19 warrior stances, 28 shadowform, 30 stealth,
    // 31 moonkin.
    _info.Add("form_at_end", [](Env const& env, uint32 index)
    {
        Player* bot = env.FindBot(index);
        return bot ? float(bot->GetShapeshiftForm()) : 0.0f;
    });
    _info.Add("power_left", [](Env const& env, uint32 index)
    {
        Player* bot = env.FindBot(index);
        if (!bot)
            return 0.0f;
        Powers const power = bot->getPowerType();
        uint32 const maxPower = bot->GetMaxPower(power);
        return maxPower ? float(bot->GetPower(power)) / float(maxPower) : 0.0f;
    });
    _info.Add("target_evade_seconds", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).TargetEvadeMs) / 1000.0f;
    });
    _info.Add("out_of_sight_seconds", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).OutOfSightMs) / 1000.0f;
    });
    // Time the opponent had no path to its victim, and how often it was put back beside it for that.
    _info.Add("target_unreachable_seconds", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).UnreachableMs) / 1000.0f;
    });
    _info.Add("target_teleports", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).OpponentTeleports);
    });
    // Style over the fight (one-on-one arenas, the bot alive): the share of it spent within melee reach of the
    // opponent, and the share the opponent spent attacking the seat's pet or guardian instead of the seat.
    _info.Add("in_melee_share", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return combat.FightMs ? float(combat.InMeleeMs) / float(combat.FightMs) : 0.0f;
    });
    _info.Add("target_on_pet_share", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return combat.FightMs ? float(combat.OnPetMs) / float(combat.FightMs) : 0.0f;
    });
    // Roots and snares from the bot, its pet or its totems: the share of the fight the opponent spent under them, and
    // how often one went on where there was none.
    _info.Add("target_rooted_share", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return combat.FightMs ? float(combat.RootedMs) / float(combat.FightMs) : 0.0f;
    });
    _info.Add("target_snared_share", [tally](Env const& env, uint32 index)
    {
        CombatTally const& combat = tally(env, index);
        return combat.FightMs ? float(combat.SnaredMs) / float(combat.FightMs) : 0.0f;
    });
    _info.Add("roots_applied", [tally](Env const& env, uint32 index) { return float(tally(env, index).RootsApplied); });
    // Feign deaths, and those after which the opponent went home to evade at full health.
    _info.Add("feign_deaths", [tally](Env const& env, uint32 index) { return float(tally(env, index).FeignDeaths); });
    _info.Add("feign_death_resets", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).FeignDeathResets);
    });
    _info.Add("snares_applied", [tally](Env const& env, uint32 index)
    {
        return float(tally(env, index).SnaresApplied);
    });
    _info.Add("actions_per_minute", [seat](Env const& env, uint32 index)
    {
        float const minutes = std::max(0.001f, float(env.EpisodeElapsedMs) / 60000.0f);
        return float(seat(env, index).ActionsPressed) / minutes;
    });
    // Presses of an action past the free ones in its window (Tuning().Actions.Repeat).
    _info.Add("repeated_presses", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).RepeatedPresses);
    });

    // Support: healing and protection done (on itself, the owner and teammates) as fractions of the bot's health, the
    // share of healing cast that overhealed, casts that could only be wasted, defensives, how often heals were cast
    // below their highest rank, and time any friend (the bot included) spent below LOW_HEALTH_PCT.
    auto const health = [this](Env const& env, uint32 index)
    {
        Player* bot = SeatBot(env, index);
        return float(std::max<uint32>(1, bot ? bot->GetMaxHealth() : 1));
    };
    // What healing is actually worth: what it restored for what it cost. Neither half says it alone -- healing_done
    // rewards a seat for spending its whole pool, and overheal counted at the cast misreads every heal over time,
    // whose ticks are only waste if they land on a full bar.
    _info.Add("healing_per_mana", [seat](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        uint64 healed = stats.SelfHealing + stats.AllyHealing;
        for (uint64 agent : stats.AgentHealingBy)
            healed += agent;

        uint64 const spent = seat(env, index).HealingPowerSpent;
        return spent ? float(healed) / float(spent) : 0.0f;
    });
    // How much of the healing arrived as ticks of a heal over time. A mana charge lands the moment a heal over time
    // is cast while its healing arrives over the next ten to twenty seconds, discounted and mostly past the GAE
    // trace, so the two are not paid symmetrically: if that tilts a policy off them, this is where it shows.
    _info.Add("hot_healing_share", [](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        uint64 healed = stats.SelfHealing + stats.AllyHealing;
        for (uint64 agent : stats.AgentHealingBy)
            healed += agent;
        return healed ? float(stats.PeriodicHealing) / float(healed) : 0.0f;
    });

    _info.Add("healing_mana_spent", [seat](Env const& env, uint32 index)
    {
        Player const* bot = env.FindBot(index);
        uint32 const pool = bot ? std::max<uint32>(1, bot->GetMaxPower(POWER_MANA)) : 1;
        return float(seat(env, index).HealingPowerSpent) / float(pool);
    });

    _info.Add("healing_done", [health](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        uint64 healed = stats.SelfHealing + stats.AllyHealing;
        for (uint64 agent : stats.AgentHealingBy)
            healed += agent;
        return float(healed) / health(env, index);
    });
    _info.Add("protection_done", [health](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        uint64 kept = stats.SelfProtection;
        for (uint64 ally : stats.AllyProtectionBy)
            kept += ally;
        for (uint64 agent : stats.AgentProtectionBy)
            kept += agent;
        return float(kept) / health(env, index);
    });
    _info.Add("overheal_share", [](Env const& env, uint32 index)
    {
        AgentStats const& stats = env.EpisodeStats[index];
        uint64 healed = stats.SelfHealing + stats.AllyHealing;
        for (uint64 agent : stats.AgentHealingBy)
            healed += agent;
        return stats.HealingRaw
            ? std::clamp(1.0f - float(healed) / float(stats.HealingRaw), 0.0f, 1.0f) : 0.0f;
    });
    _info.Add("heals_on_full", [seat](Env const& env, uint32 index) { return float(seat(env, index).HealsOnFull); });
    _info.Add("defensive_casts", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).DefensiveCasts);
    });
    _info.Add("healing_casts", [seat](Env const& env, uint32 index) { return float(seat(env, index).HealingCasts); });
    _info.Add("downranked_share", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return state.HealingCasts ? float(state.DownrankedCasts) / float(state.HealingCasts) : 0.0f;
    });
    _info.Add("low_health_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).LowHealthMs) / 1000.0f;
    });

    // Ground effects: how long the seat stood in one, what that cost it as a share of its health, and how many casts
    // it could have interrupted were there to interrupt. The last is the denominator for the press-to-interrupt
    // ratio -- presses alone cannot say whether a policy is pressing too often or whether there was nothing to stop.
    _info.Add("hazard_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).HazardMs) / 1000.0f;
    });
    _info.Add("hazard_damage", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        Player const* bot = env.FindBot(index);
        return bot ? float(state.HazardDamage) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;
    });
    _info.Add("interruptible_casts_seen", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).InterruptibleCastsSeen);
    });

    // Goals (SeatGoal): the share of decisions spent on each, how often the decisions matched the goal, and how often
    // the goal changed. All zero for a policy without a goal head.
    auto const goalDecisions = [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return float(std::accumulate(state.GoalDecisions.begin(), state.GoalDecisions.end(), uint32(0)));
    };
    for (uint32 goal = 0; goal < GOAL_COUNT; ++goal)
        _info.Add("goal_" + std::string(GoalName(SeatGoal(goal))) + "_share",
            [seat, goalDecisions, goal](Env const& env, uint32 index)
            {
                float const total = goalDecisions(env, index);
                return total ? float(seat(env, index).GoalDecisions[goal]) / total : 0.0f;
            });
    _info.Add("goal_match_share", [seat, goalDecisions](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        float const total = goalDecisions(env, index);
        return total ? float(std::accumulate(state.GoalMatches.begin(), state.GoalMatches.end(), uint32(0))) / total
            : 0.0f;
    });
    _info.Add("goal_changes", [seat](Env const& env, uint32 index) { return float(seat(env, index).GoalChanges); });
}

void Animus::Curriculum::StageScenario::WriteStageFiles(StageSettings const& settings) const
{
    // Each layout's manifest, for export to publish beside its model, and the stage's description: its blocks, what
    // it extends (the learner's seed chain), its models and the effective tuning (copied into every run).
    std::filesystem::path const directory = std::filesystem::path(settings.LayoutsDir) / _stage.Name;
    std::error_code error;
    std::filesystem::create_directories(directory, error);

    for (Layout const& layout : _layouts)
        if (!WriteIfChanged(directory / (layout.ModelName() + ".json"), layout.Manifest()))
            LOG_WARN("module.animus", "{}: could not write the {} layout manifest to {}", Name(), layout.ModelName(),
                directory.string());

    boost::json::object stageFile;
    stageFile["format"] = STAGE_FILE_FORMAT;
    stageFile["stage"] = _stage.Name;
    stageFile["suffix"] = _stage.Suffix;
    stageFile["extends"] = _stage.Extends;
    stageFile["summary"] = _stage.Summary;
    stageFile["seats"] = _seatCount;

    boost::json::array& blocks = stageFile["blocks"].emplace_array();
    for (BlockId id : _stage.Blocks)
        blocks.push_back(boost::json::string(BlockName(id)));

    // What its episodes are: the episode info column "arena" is an index into this list.
    boost::json::array& arenas = stageFile["arenas"].emplace_array();
    for (std::size_t arena = 0; arena < _stage.Arenas.size(); ++arena)
    {
        ArenaDefinition const& definition = _stage.Arenas[arena];
        boost::json::object& entry = arenas.emplace_back(boost::json::object()).get_object();
        entry["name"] = definition.Name;
        entry["weight"] = _arenaWeights[arena];
        entry["seats"] = definition.SeatCount();
        entry["episode_seconds"] = _arenaEpisodeMs[arena] / IN_MILLISECONDS;
        entry["pvp"] = definition.Pvp;
        entry["ambushers"] = definition.Ambushers;
        entry["checkpoints"] = definition.Checkpoints;
        // The seat plan and a team's width, so the learner can tell an arena's opponent seats (IsOpponentSeat) and
        // play them from a frozen checkpoint (its cast league) without a word on the wire.
        entry["plan"] = definition.Seats == SeatPlan::Solo ? "solo" : definition.Seats == SeatPlan::Party ? "party"
            : definition.Seats == SeatPlan::Mirror ? "mirror" : definition.Seats == SeatPlan::Raid ? "raid" : "teams";
        entry["team_seats"] = definition.Seats == SeatPlan::Teams ? definition.TeamSeats
            : definition.Seats == SeatPlan::Mirror ? 1u : 0u;
        entry["directed"] = definition.Directed;
    }

    // Agents beyond the seats: a directed arena's two directors (one a side, after the seats). The learner never
    // casts a director's row.
    boost::json::array& directorAgents = stageFile["director_agents"].emplace_array();
    if (HasDirectors())
        for (uint32 side = 0; side < TEAM_COUNT; ++side)
            directorAgents.push_back(_seatCount + side);
    // Agents the sim declares for a frozen checkpoint to play: the owner, where an arena casts it.
    boost::json::array& cast = stageFile["cast"].emplace_array();
    if (_castOwner)
    {
        boost::json::object& entry = cast.emplace_back(boost::json::object()).get_object();
        entry["agent"] = OwnerAgent();
        entry["name"] = "owner";
    }

    // The stages a run seeds from, closest first: the learner takes the first one that has been trained.
    boost::json::array& seedChain = stageFile["seed_chain"].emplace_array();
    for (StageDefinition const* base = FindStage(_stage.Extends); base; base = FindStage(base->Extends))
        seedChain.push_back(boost::json::string(base->Name));

    // A merge's further parents: each seeds the blocks only it has, and can teach its arenas.
    boost::json::array& merges = stageFile["merges"].emplace_array();
    for (std::string const& merge : _stage.Merges)
        merges.push_back(boost::json::string(merge));

    // Where the critic state holds the episode's arena, so the learner knows each decision's arena.
    boost::json::object& state = stageFile["state"].emplace_object();
    state["arena_first"] = uint32(STATE_ARENA_FIRST);
    state["arena_count"] = MAX_ARENAS;

    // Keyed by layout name, which for the director is "director": it has no class/role to be named after.
    auto const layoutName = [](Layout const& layout)
    {
        return layout.Director ? std::string(DirectorLayout::Name()) : layout.Profile->Name;
    };

    boost::json::object& models = stageFile["models"].emplace_object();
    for (Layout const& layout : _layouts)
        models[layoutName(layout)] = layout.ModelName();

    // Where each block sits in each layout: a later stage seeds its networks block by block from these.
    boost::json::object& layouts = stageFile["layouts"].emplace_object();
    for (Layout const& layout : _layouts)
    {
        boost::json::object& entry = layouts[layoutName(layout)].emplace_object();
        entry["obs_dim"] = layout.ObsDim;
        entry["num_actions"] = layout.NumActions;

        boost::json::array& actionNames = entry["action_names"].emplace_array();
        for (std::string const& name : layout.ActionNames())
            actionNames.push_back(boost::json::string(name));

        // The class's builds, in the order the episode info column "spec" indexes them, so the learner can group
        // and gate by build (target.spec_metrics) without having to know the classes.
        boost::json::array& specNames = entry["spec_names"].emplace_array();
        if (!layout.Director && layout.Profile)
            for (SpecProfile const& spec : layout.Profile->Specs)
                specNames.push_back(boost::json::string(spec.Name));

        boost::json::array& spans = entry["blocks"].emplace_array();
        for (BlockId id : layout.Blocks)
        {
            BlockSlice const& slice = layout.Slice(id);
            boost::json::object& block = spans.emplace_back(boost::json::object()).get_object();
            block["name"] = BlockName(id);
            block["obs"] = Span(slice.ObsFirst, slice.ObsCount);
            block["actions"] = Span(slice.ActionFirst, slice.ActionCount);
        }
    }

    boost::json::array& episodeInfo = stageFile["episode_info"].emplace_array();
    for (std::string const& name : _info.Names())
        episodeInfo.push_back(boost::json::string(name));

    stageFile["tuning"] = _tuning.Json();

    if (!WriteIfChanged(directory / "stage.json", boost::json::serialize(stageFile)))
        LOG_WARN("module.animus", "{}: could not write stage.json to {}", Name(), directory.string());
}

Animus::Curriculum::EnvState& Animus::Curriculum::StageScenario::Data(Env const& env)
{
    return _data[env.Index];
}

Animus::Curriculum::EnvState const& Animus::Curriculum::StageScenario::Data(Env const& env) const
{
    return _data[env.Index];
}

Player* Animus::Curriculum::StageScenario::SeatBot(Env const& env, uint32 seat) const
{
    return _data[env.Index].Seats[seat].Bot.Active();
}

Player* Animus::Curriculum::StageScenario::Owner(Env const& env) const
{
    return _owner && Arena(env).Owner ? _owner->Find(env) : nullptr;
}

bool Animus::Curriculum::StageScenario::CastOwnerActive(Env const& env) const
{
    return _castOwner && Arena(env).OwnerCast && !env.Evaluating && _owner && _owner->IsCast(env);
}

Player* Animus::Curriculum::StageScenario::BuildOwnerSeat(Env& env, Map*& map, uint8 level, Position const& start,
    AptitudeDemand demand)
{
    uint32 const agent = OwnerAgent();
    SeatState& seat = Data(env).Seats[agent];
    // Drawn evenly, not by the training weights: a class the learner has held back from the draw because it has
    // converged is as good an owner as any, and the checkpoint in the seat learns nothing either way.
    Casting const casting = DrawCasting(env, agent, demand, false);
    if (!casting.L)
        return nullptr;

    seat.L = casting.L;
    seat.Spec = casting.Spec;
    seat.Want = demand;
    seat.Bot.Begin();
    Player* bot = BuildSeat(env, agent, map, level, start);
    if (!bot)
    {
        seat.Bot.Abort();
        seat.L = nullptr;
        return nullptr;
    }

    PrepareFighter(bot, seat);
    seat.Bot.Promote();
    if (agent < env.Bots.size())
        env.Bots[agent] = bot->GetGUID();

    // What the seats get from StockSeats and GivePets, which stop at ActiveSeats: the checkpoint in this seat was
    // trained with potions, food and a pet to hand, and without them plays with those actions masked.
    seat.Supplies = ConsumablePool::Instance().Supplies(seat.Level, bot->GetMaxPower(POWER_MANA) > 0,
        seat.L->Profile->Class == CLASS_WARLOCK, false);
    StockBattleSupplies(bot, seat.Supplies, seat.L->Profile->Specs[seat.Spec].Stats);
    seat.PetAtStart = PetBlock::HasPet(seat.L->Profile->Class) && roll_chance_i(_tuning.Characters.PetOutChance)
        && SeatCharacter::GivePet(bot, seat.Stable);
    return bot;
}

void Animus::Curriculum::StageScenario::ReleaseOwnerSeat(Env& env)
{
    SeatState& seat = Data(env).Seats[OwnerAgent()];
    seat.Bot.Destroy();
    seat.L = nullptr;
    if (OwnerAgent() < env.Bots.size())
        env.Bots[OwnerAgent()] = ObjectGuid::Empty;
}

Player* Animus::Curriculum::StageScenario::PartyTank(Env const& env) const
{
    return _party && Arena(env).PartyGroup ? _party->Tank(env) : nullptr;
}

std::vector<Animus::Curriculum::StageScenario::Casting> Animus::Curriculum::StageScenario::Castings(
    AptitudeDemand demand) const
{
    std::vector<Casting> castings;
    if (demand.Any())
        for (Layout const& layout : _layouts)
        {
            if (layout.Director)
                continue;

            for (uint8 spec : ClassAssets::For(*layout.Profile).SpecsMeeting(demand))
                castings.push_back({ &layout, spec });
        }

    // Nothing in the run can do it, or nothing was asked for: every class with every build it has. The pairs, not
    // the classes, because a class with a build that holds a pull and one that heals is two things to be.
    if (castings.empty())
        for (Layout const& layout : _layouts)
        {
            if (layout.Director)
                continue;

            for (uint8 spec = 0; spec < uint8(layout.Profile->Specs.size()); ++spec)
                castings.push_back({ &layout, spec });
        }

    return castings;
}

std::string Animus::Curriculum::StageScenario::SpecName(uint16 layout, uint8 spec) const
{
    if (layout >= _layouts.size() || !_layouts[layout].Profile)
        return "?";

    ClassProfile const& profile = *_layouts[layout].Profile;
    return spec < profile.Specs.size() ? profile.Specs[spec].Name : "?";
}

Animus::Curriculum::StageScenario::Casting Animus::Curriculum::StageScenario::DrawCasting(Env const& env,
    uint32 seat, AptitudeDemand demand, bool weighted) const
{
    std::vector<Casting> const castings = Castings(demand);

    // An evaluation spreads its seeds over the (class, build) pairs instead of drawing them: seed i plays pair
    // (i + seat) % count. Each pair is then scored on an equal share of the seeds, whatever the env count, so a
    // paladin's healing build is as well measured as its tanking one and two checkpoints meet the same characters.
    if (env.EpisodeSeedIndex != NO_EPISODE_SEED)
        return castings[(env.EpisodeSeedIndex + seat) % castings.size()];

    // Training: the learner's weights (the forge's WEIGHTS message), so the pairs furthest below their baseline
    // get more of the data. Without them, or when none of the pairs carries one, draw evenly.
    float total = 0.0f;
    if (weighted)
        for (Casting const& casting : castings)
            total += Weight(*casting.L, casting.Spec);

    if (total <= 0.0f)
        return castings[urand(0, uint32(castings.size()) - 1)];

    float roll = frand(0.0f, total);
    for (Casting const& casting : castings)
    {
        roll -= Weight(*casting.L, casting.Spec);
        if (roll <= 0.0f)
            return casting;
    }

    return castings.back();
}

float Animus::Curriculum::StageScenario::Weight(Layout const& layout, uint8 spec) const
{
    std::size_t const row = std::size_t(layout.Index) * MAX_SPECS + std::size_t(std::min<uint32>(spec, MAX_SPECS - 1));
    return row < _layoutWeights.size() ? _layoutWeights[row] : 1.0f;
}

void Animus::Curriculum::StageScenario::SetLayoutWeights(std::vector<float> const& weights)
{
    if (weights.empty())
    {
        _layoutWeights.clear();
        return;
    }

    // One weight per (class, spec), layout-major and MAX_SPECS wide, so the shape is fixed whichever classes a run
    // plays: a class with a build that holds the line and one that heals is weighted as two things, because it is
    // two things to be bad at -- and so are two damage builds of one class, which a role could not tell apart.
    if (weights.size() != _layouts.size() * MAX_SPECS)
    {
        LOG_ERROR("module.animus", "{}: {} layout weights for {} layouts by {} specs; keeping the ones in use",
            Name(), weights.size(), _layouts.size(), MAX_SPECS);
        return;
    }

    float total = 0.0f;
    for (float weight : weights)
    {
        if (!std::isfinite(weight) || weight < 0.0f)
        {
            LOG_ERROR("module.animus", "{}: layout weights must be finite and not negative; keeping the ones in use",
                Name());
            return;
        }
        total += weight;
    }

    if (total <= 0.0f)
    {
        LOG_ERROR("module.animus", "{}: layout weights are all zero; keeping the ones in use", Name());
        return;
    }

    _layoutWeights = weights;
}

bool Animus::Curriculum::StageScenario::IsTerminal(Env const& env) const
{
    if (Data(env).BuildFailed)
        return true;

    std::vector<Encounter*> const& active = ActiveEncounters(env);
    return std::any_of(active.begin(), active.end(),
        [&env](Encounter const* encounter) { return encounter->IsTerminal(env); });
}

uint32 Animus::Curriculum::StageScenario::SideOf(Env const& env, uint32 seat) const
{
    uint32 const perSide = Arena(env).Seats == SeatPlan::Teams ? Arena(env).TeamSeats : 1;
    return std::min<uint32>(seat / perSide, TEAM_COUNT - 1);
}

bool Animus::Curriculum::StageScenario::IsOpponentSeat(Env const& env, uint32 agent) const
{
    // The director of the far side is that side, as much as its seats are: a scripted-opponent evaluation has to
    // replace both or the learner is still commanding the team it is being scored against.
    if (HasDirectors() && agent >= _seatCount)
        return agent == _seatCount + 1;

    // Self-play: the far side of the match is the opponent. One seat a side in a Mirror, TEAM_SEATS of them in
    // a Teams arena.
    switch (Arena(env).Seats)
    {
        case SeatPlan::Mirror: return agent == 1;
        case SeatPlan::Teams:  return agent >= Arena(env).TeamSeats;
        default:               return false;
    }
}

bool Animus::Curriculum::StageScenario::Setup(Env& env)
{
    if (_layouts.empty())
    {
        LOG_ERROR("module.animus", "{}: no class/role to play (check the host's class/role list{})", Name(),
            _stage.NeedsStealth ? ", and this stage is played only by class/roles whose kit has stealth" : "");
        return false;
    }

    if (!Rebuild(env))
        return false;

    Data(env).Fresh = true;
    return true;
}

void Animus::Curriculum::StageScenario::Reset(Env& env)
{
    // Setup already built the first episode's characters.
    EnvState& data = Data(env);
    if (data.Fresh)
    {
        data.Fresh = false;
        return;
    }

    // A failed build ends the episode at the next decision, and the reset that follows tries again.
    data.BuildFailed = !Rebuild(env);
    if (data.BuildFailed)
        LOG_ERROR("module.animus", "{}: env {} could not build its episode; it ends at once and is rebuilt", Name(),
            env.Index);
}

bool Animus::Curriculum::StageScenario::Rebuild(Env& env)
{
    EnvState& data = Data(env);
    auto prepareMark = std::chrono::steady_clock::now();

    // The episode's arena, drawn first: an evaluation episode's random numbers decide it like everything else.
    std::vector<Encounter*> const previousEncounters = ActiveEncounters(env);
    data.Arena = DrawArena();
    data.EpisodeMapId = 0;
    data.HasEpisodeMap = false;
    data.EpisodeLevel = 0;
    data.EpisodeTeam = 0;
    data.DungeonDifficulty = 0;
    data.RaidDifficulty = 0;
    data.HasEpisodeSpawn = false;

    // A spawn point per episode, not per env. Keyed on env.Index, an env stood on the same patch of ground for
    // its whole life: 128 envs saw 8 places between them, every episode, and a policy can fit that rather than
    // learn to read what is in front of it. Drawn here, so an evaluation episode's draw comes from its seed like
    // everything else the reset rolls.
    {
        std::vector<Position> const& ground = SpawnGroundFor(env);
        data.Spawn = ground.empty() ? 0 : urand(0, uint32(ground.size()) - 1);
        data.SpawnDrawn = data.Spawn;
    }
    ArenaDefinition const& arena = Arena(env);
    env.EpisodeLengthMs = _arenaEpisodeMs[data.Arena];

    // A new episode starts from clean totals, in every encounter: those this arena does not use report 0.
    for (SeatState& seat : data.Seats)
        seat.ResetEpisode();
    // No resurrection offer is in flight into a new episode, and the clock it was taken on has restarted.
    data.ResurrectBy.fill(NO_SEAT);
    data.ResurrectMs.fill(0);
    for (auto const& encounter : _encounters)
        encounter->ResetEpisode(env);

    std::vector<Creature*> oldTargets;
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            oldTargets.push_back(creature);

    // What the last episode's arena had and this one does not (an owner, a group, an enemy player) goes first.
    for (Encounter* encounter : previousEncounters)
        if (!Uses(env, *encounter))
            encounter->Deactivate(env);

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->BeforeRebuild(env);

    bool const firstBuild = !SeatBot(env, 0);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        data.Seats[seat].Bot.Begin();

    // What the seats' current characters are, to put back if a new one cannot be built: the old bots stay.
    struct Character
    {
        Layout const* L;
        uint8 Race;
        uint8 Level;
        uint8 Spec;
        SeatCharacter::TalentPlan TalentPlan;
        float DamageScale;
        TalentBuilder::Build Build;
        uint32 UnspentTalentPoints;
        uint32 EquippedItems;
        std::vector<SpellInfo const*> KnownRanks;
    };

    uint32 const previousActiveSeats = data.ActiveSeats;
    std::array<Character, MAX_SEATS> previous{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        SeatState const& s = data.Seats[seat];
        previous[seat] = { s.L, s.Race, s.Level, s.Spec, s.TalentPlan, s.DamageScale, s.Build, s.UnspentTalentPoints,
            s.EquippedItems, s.KnownRanks };
    }

    // How many seats play this episode, and their class/roles: the arena's seats, except in a party, which has 1-4
    // like a player's companions; the rest stay empty: no character, no layout, only the no-op allowed.
    data.ActiveSeats = arena.SeatCount();
    if (arena.Seats == SeatPlan::Party || arena.Seats == SeatPlan::Raid)
    {
        if (arena.Seats == SeatPlan::Party)
            data.ActiveSeats = RandomPartySize(_tuning.Party);

        // Some parties are the classic makeup (somebody to hold the pull, somebody to keep the hurt one up, and no
        // demand on the rest, in a random order); the others draw every seat's demand on its own. The makeup is
        // built for the seats actually in play: a four-entry array left the other MAX_SEATS - 4 zero-filled, which
        // a raid would have shuffled into the group that got them.
        std::array<AptitudeDemand, MAX_SEATS> demands = ClassicDemands(data.ActiveSeats);
        if (roll_chance_i(_tuning.Party.ClassicChance))
            std::shuffle(demands.begin(), demands.begin() + data.ActiveSeats, RandomEngine::Instance());
        else
            for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
                demands[seat] = RollDemand(_tuning.Party.RoleTankChance, _tuning.Party.RoleHealerChance);

        // A drill arena fixes the seats it is about, after the makeup is drawn, so the rest of the group is still
        // whatever the party would have been.
        for (uint32 seat = 0; seat < arena.SeatAptitudes.size() && seat < data.ActiveSeats; ++seat)
            demands[seat] = arena.SeatAptitudes[seat];

        for (uint32 seat = 0; seat < _seatCount; ++seat)
        {
            Casting const casting = seat < data.ActiveSeats ? DrawCasting(env, seat, demands[seat]) : Casting();
            data.Seats[seat].L = casting.L;
            data.Seats[seat].Want = seat < data.ActiveSeats ? demands[seat] : AptitudeDemand::Anything();
            data.Seats[seat].Spec = casting.Spec;
        }
    }
    else
    {
        // Any class with any of its builds: drawn (evenly, or by the learner's weights), or spread over the seeds
        // in an evaluation.
        for (uint32 seat = 0; seat < _seatCount; ++seat)
        {
            // No composition to honour, so the class and the build are drawn together, over every pair the run can
            // field: what keeps a class with two very different builds training both.
            Casting const casting = seat < data.ActiveSeats
                ? DrawCasting(env, seat, AptitudeDemand::Anything()) : Casting();
            data.Seats[seat].L = casting.L;
            data.Seats[seat].Want = AptitudeDemand::Anything();
            data.Seats[seat].Spec = casting.Spec;
        }
    }

    // An encounter that fixes the level, the map or the spawn for this episode (an instance's boss rung) says so
    // now, with the seats' classes drawn and before their level is.
    for (Encounter* encounter : ActiveEncounters(env))
        encounter->BeforeLevel(env);

    // One level every seat's class/role can be.
    uint8 minLevel = 1;
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        if (data.Seats[seat].L)
            minLevel = std::max(minLevel, data.Seats[seat].L->Assets->Kit->MinLevel());

    minLevel = std::max(minLevel, _stage.MinLevel);

    // Characters.ReuseEpisodes: a seat whose draw gave it the class and build it already has keeps its character
    // for a few episodes rather than building a new one (the build was a quarter of a decision's cost). The env then
    // keeps its level too, since every seat shares one; the level draw is random anyway, so holding it a few
    // episodes biases nothing. Never in an evaluation (its seeded spread of characters is the yardstick), never on
    // a battleground (the match is rebuilt with the episode), never a seat that was empty or lost.
    std::array<bool, MAX_SEATS> reuse{};
    uint8 keptLevel = 0;
    // The encounters ended the last match in ResetEpisode above and make the next one in BeforeSeats below, so
    // MatchFor is empty here: the map says whether the seats stand in a battleground. Reusing a seat there moved a
    // bot within a map whose match had just been taken down; the move failed, and with it the whole build --
    // most stage25_warsong resets, each followed by old bots on a map with no match asking to be sent home.
    Map const* envMap = env.FindMap();
    bool const onMatch = (envMap && envMap->IsBattlegroundOrArena())
        || std::any_of(ActiveEncounters(env).begin(), ActiveEncounters(env).end(),
            [&env](Encounter const* encounter) { return encounter->MatchFor(env) != nullptr; });
    // (An env whose last episode was on another map, an instance rung, rebuilds on the map this one wants.)
    bool const changesMap = env.FindMap() && env.FindMap()->GetId() != EpisodeMapId(env);
    if (!firstBuild && !env.Evaluating && !onMatch && !changesMap && _tuning.Characters.ReuseEpisodes > 0)
        for (uint32 seat = 0; seat < data.ActiveSeats && seat < previousActiveSeats; ++seat)
        {
            SeatState const& s = data.Seats[seat];
            Character const& c = previous[seat];
            Player const* bot = s.Bot.Active();
            if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !c.L || s.L != c.L || s.Spec != c.Spec
                || s.EpisodesPlayed >= _tuning.Characters.ReuseEpisodes || c.Level < minLevel
                || (keptLevel && c.Level != keptLevel) || (data.EpisodeLevel && c.Level != data.EpisodeLevel)
                || (data.EpisodeTeam && Player::TeamIdForRace(c.Race) != TeamId(data.EpisodeTeam - 1)))
                continue;

            reuse[seat] = true;
            keptLevel = c.Level;
        }

    uint8 const level = data.EpisodeLevel ? std::clamp<uint8>(data.EpisodeLevel, minLevel, DEFAULT_MAX_LEVEL)
        : keptLevel ? keptLevel : RandomLevel(minLevel, _level, _tuning.Characters,
        env.EpisodeSeedIndex, uint32(_layouts.size()));

    // The first build opens a new instance (or a phase of the continent); every later one reuses it. An env whose
    // seats were all lost keeps its instance while the map still exists, and opens a new one when it is gone. An
    // episode fixed to another map (an instance rung) opens a new instance of that map; the old one unloads once
    // its last bot has left.
    Map* map = !firstBuild || env.InstanceId ? env.FindMap() : nullptr;
    // A battleground's map is its match's: a new match (FlagEncounter::BeforeSeats) gets a map of its own, which the
    // first seat's bot opens through the invitation it carries. Reused, the new bots stood on the last match's map --
    // "map 596 cannot unload: Forge31s19a (bg id 660)" -- and it was unloaded under them.
    if (map && (map->GetId() != EpisodeMapId(env) || map->IsBattlegroundOrArena()))
        map = nullptr;

    // A continent is not one map for the whole pool any more: the envs are dealt out over replicas of it, each
    // a Map object of its own and so a map task of its own. Replica 0 is the base map, which is where every env
    // of a pool that fits in one map's phases goes, as before.
    if (!map)
    {
        uint32 const episodeMapId = EpisodeMapId(env);
        MapEntry const* episodeEntry = sMapStore.LookupEntry(episodeMapId);
        if (episodeEntry && !episodeEntry->Instanceable())
            map = sMapMgr->CreateContinentReplica(episodeMapId, ReplicaOf(env));
    }

    // The new bots go on idle sessions and into the map before the old ones leave, so the instance always has a
    // bound player.
    Player* firstNew = nullptr;
    for (Encounter* encounter : ActiveEncounters(env))
        encounter->BeforeSeats(env, level);
    CurrentReset.PrepareNs += ResetSinceNs(prepareMark);

    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
    {
        Position start = SpawnPointFor(env);
        if (arena.Seats == SeatPlan::Party || arena.Seats == SeatPlan::Raid || arena.Seats == SeatPlan::Teams)
        {
            // Within a group as a party has always spread; groups themselves step back in rows, so forty seats do
            // not spawn in one line forty spacings long.
            uint32 const inGroup = seat % GROUP_SEATS;
            uint32 const group = seat / GROUP_SEATS;
            start.m_positionX += (inGroup % 2 ? -PARTY_SPACING : PARTY_SPACING) * float(1 + inGroup / 2);
            start.m_positionY += (inGroup % 2 ? PARTY_SPACING : -PARTY_SPACING) - PARTY_SPACING * 2.0f * float(group);
        }
        else if (arena.Seats == SeatPlan::Mirror && seat == 1 && firstNew)
        {
            // Out of range of the first seat's new bot, at a random bearing, facing a random way.
            start = Opponents::FindSpawnPoint(firstNew, map);
            start.SetOrientation(frand(0.0f, 2.0f * float(M_PI)));
        }

        Player* bot = reuse[seat] ? ReuseSeat(env, seat, start) : BuildSeat(env, seat, map, level, start);
        // A character that cannot be made ready again is replaced, not a reason to lose the episode.
        if (!bot && reuse[seat])
            bot = BuildSeat(env, seat, map, level, start);
        if (!bot)
        {
            // Nothing changes: the bots already made for this episode go, the old characters stay with their seats.
            for (uint32 other = 0; other < _seatCount; ++other)
            {
                data.Seats[other].Bot.Abort();

                SeatState& s = data.Seats[other];
                Character const& c = previous[other];
                s.L = c.L;
                s.Race = c.Race;
                s.Level = c.Level;
                s.Spec = c.Spec;
                s.TalentPlan = c.TalentPlan;
                s.DamageScale = c.DamageScale;
                s.Build = c.Build;
                s.UnspentTalentPoints = c.UnspentTalentPoints;
                s.EquippedItems = c.EquippedItems;
                // A character that stays keeps the ranks resolved for it, not the aborted build's.
                s.KnownRanks = c.KnownRanks;
            }

            data.ActiveSeats = previousActiveSeats;
            return false;
        }

        if (seat == 0)
            firstNew = bot;
    }

    CurrentReset.SeatsNs += ResetSinceNs(prepareMark);
    for (Creature* creature : oldTargets)
        creature->DespawnOrUnsummon();
    CurrentReset.DespawnNs += ResetSinceNs(prepareMark);

    auto destroyMark = std::chrono::steady_clock::now();
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        data.Seats[seat].Bot.Promote();
    CurrentReset.DestroyNs += ResetSinceNs(destroyMark);

    Player* lead = SeatBot(env, 0);
    // A continent's own creatures are in another phase than the env's, and belong to every env.
    if (firstBuild && !_continent)
        SpawnArea::Clear(lead);

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    // One agent slot per agent, seats first; an empty seat's slot holds no bot, and neither does a director's --
    // it commands a side rather than playing a character. EnvPool wants one slot per agent either way.
    env.Bots.clear();
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        env.Bots.push_back(seat < data.ActiveSeats ? SeatBot(env, seat)->GetGUID() : ObjectGuid::Empty);
    for (uint32 side = 0; side < TEAM_COUNT && HasDirectors(); ++side)
        env.Bots.push_back(ObjectGuid::Empty);
    // The owner's slot: OwnerEncounter::Build fills it where the episode plays the owner through its row.
    if (_castOwner)
        env.Bots.push_back(ObjectGuid::Empty);
    env.Targets.clear();

    auto partMark = std::chrono::steady_clock::now();
    ScatterSeats(env, map);
    CurrentReset.ScatterNs += ResetSinceNs(partMark);

    // A spawn point no objective can be found from used to take the whole run down with it: the plan stops when
    // its first scenario fails to start, so one bad patch in a list of twenty-six was a dead run. The ground is
    // drawn per episode now, so the answer is to draw again -- move the seats to another point and build there.
    // Only a stage whose every spawn point is bad fails now, which is a stage that deserves to.
    constexpr uint32 SPAWN_ATTEMPTS = 4;
    std::vector<Position> const& ground = SpawnGroundFor(env);
    bool built = false;
    for (uint32 attempt = 0; attempt < SPAWN_ATTEMPTS && !built; ++attempt)
    {
        built = true;
        for (Encounter* encounter : ActiveEncounters(env))
            if (!encounter->Build(env, map, level))
            {
                built = false;
                break;
            }

        if (built || ground.size() < 2 || attempt + 1 >= SPAWN_ATTEMPTS)
            break;

        // Somewhere else in the list, never the point that just failed.
        data.Spawn = (data.Spawn + 1 + urand(0, uint32(ground.size()) - 2)) % uint32(ground.size());
        Position const& retry = SpawnPointFor(env);
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            if (Player* bot = SeatBot(env, seat))
                BotFactory::TeleportWithinMap(bot, retry);

        ScatterSeats(env, map);
    }

    if (!built)
    {
        Position const& where = SpawnPointFor(env);
        LOG_ERROR("module.animus", "{}: env {} could not build an encounter, last tried from ({:.0f} {:.0f} "
            "{:.0f}) on map {}", Name(), env.Index, where.GetPositionX(), where.GetPositionY(),
            where.GetPositionZ(), map->GetId());
        return false;
    }

    CurrentReset.EncounterNs += ResetSinceNs(partMark);

    // Where each seat is looking starts as where the world put it. ResetEpisode cleared it to 0, which would aim
    // every seat due east; this is the first point at which the bots have stopped being teleported about.
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (Player const* bot = SeatBot(env, seat))
            data.Seats[seat].Facing = bot->GetOrientation();

    partMark = std::chrono::steady_clock::now();
    StockSeats(env);
    GivePets(env);
    CurrentReset.StockNs += ResetSinceNs(partMark);
    return true;
}

void Animus::Curriculum::StageScenario::GivePets(Env& env)
{
    // After the encounters prepared the seats (a hunter's stable offer) and stocked them (soul shards, corpse dust).
    EnvState& data = Data(env);
    for (uint32 seatIndex = 0; seatIndex < data.ActiveSeats; ++seatIndex)
    {
        SeatState& seat = data.Seats[seatIndex];
        seat.PetAtStart = false;
        Player* bot = SeatBot(env, seatIndex);
        if (!bot || !seat.L || !PetBlock::HasPet(seat.L->Profile->Class)
            || !roll_chance_i(_tuning.Characters.PetOutChance))
            continue;

        seat.PetAtStart = SeatCharacter::GivePet(bot, seat.Stable);
    }
}

Player* Animus::Curriculum::StageScenario::BuildSeat(Env& env, uint32 seatIndex, Map*& map, uint8 level,
    Position const& start)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    Layout const& layout = *seat.L;

    // A team match needs real factions, not labels: the battleground counts a side by GetBgTeamId, but whether
    // two seats can fight each other comes from their races. Side 0 draws Alliance, side 1 Horde; every class has
    // both in Wrath, so no layout is lost. Any other arena draws from the whole list as it always did.
    std::vector<uint8> const& races = layout.Assets->Races;
    std::vector<uint8> pool;
    if (Arena(env).Seats == SeatPlan::Teams || Data(env).EpisodeTeam)
    {
        // A life episode's quest or town belongs to a side too (EnvState::EpisodeTeam).
        TeamId const want = Data(env).EpisodeTeam ? TeamId(Data(env).EpisodeTeam - 1)
            : seatIndex < Arena(env).TeamSeats ? TEAM_ALLIANCE : TEAM_HORDE;
        for (uint8 race : races)
            if (Player::TeamIdForRace(race) == want)
                pool.push_back(race);
    }

    std::vector<uint8> const& from = pool.empty() ? races : pool;
    seat.Race = from[urand(0, uint32(from.size()) - 1)];
    seat.Level = level;
    // The build was settled with the class, when the seats were laid out (SeatState::Spec): a casting is a class
    // and one of its builds, so there is nothing left to draw here.
    if (seat.Spec >= layout.Profile->Specs.size())
        seat.Spec = 0;
    seat.DamageScale = DamageScale(level);

    uint8 const session = seat.Bot.NextSession();

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Forge{}s{}{}", env.Id, seatIndex, session ? "b" : "a");
    spec.Race = seat.Race;
    spec.Class = layout.Profile->Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = level;
    spec.AccountId = BotAccounts::Seat(env.Id, seatIndex, session);

    // A battleground stage sends its seats to the match rather than to an instance of their own; the side is the
    // one their race already belongs to.
    for (Encounter const* encounter : ActiveEncounters(env))
        if (Battleground* match = encounter->MatchFor(env))
        {
            spec.BattlegroundId = match->GetInstanceID();
            spec.BattlegroundType = uint32(match->GetBgTypeID());
            spec.BattlegroundTeam = uint8(seatIndex < TEAM_SEATS ? TEAM_ALLIANCE : TEAM_HORDE);
            break;
        }

    // An instance rung opens its map at the difficulty it names (MapInstanced asks the first player in).
    spec.DungeonDifficulty = Data(env).DungeonDifficulty;
    spec.RaidDifficulty = Data(env).RaidDifficulty;

    auto resetMark = std::chrono::steady_clock::now();
    Player* bot = seat.Bot.CreateNext(spec, map, EpisodeMapId(env), start);
    CurrentReset.CreateNs += ResetSinceNs(resetMark);
    if (!bot)
        return nullptr;
    seat.EpisodesPlayed = 0;

    // On a shared continent every env lives in its own phase: its seats see only what it spawns. The episode's
    // map, not the stage's: a life episode of an instance stage (the crossroads) is on a continent.
    MapEntry const* episodeMap = sMapStore.LookupEntry(EpisodeMapId(env));
    if (_continent || (episodeMap && !episodeMap->Instanceable()))
        bot->SetPhaseMask(EnvPhase(env), true);

    // A bot placed by the sim never runs the map update that works out where it is standing, so until this it
    // counts as indoors wherever it is: IsOutdoors() is false, and every outdoor-only spell is refused. Stage 10's
    // seats sat in the open in Nagrand and could not summon a gryphon (SPELL_FAILED_ONLY_OUTDOORS) while ground
    // mounts, which carry no such attribute, worked and hid it. Read after the phase: terrain status is per phase.
    bot->UpdatePositionData();

    // Talent points depend on the map for death knights (Ebon Hold, where Create put the bot, only counts
    // quest-rewarded points); recompute them on the spawn map.
    bot->InitTalentForLevel();
    CurrentReset.PlaceNs += ResetSinceNs(resetMark);
    Configure(bot, seat, Arena(env).Pvp);
    CurrentReset.ConfigureNs += ResetSinceNs(resetMark);
    return bot;
}

Player* Animus::Curriculum::StageScenario::ReuseSeat(Env& env, uint32 seatIndex, Position const& start)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    Player* bot = seat.Bot.Active();
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return nullptr;

    // As a player brings a character to a new fight: out of combat, alive and full, no buffs left over (the arena
    // rule keeps passives and talents), cooldowns clear, the pet away (GivePets decides whether it is out again).
    // Supplies top up to their counts in StockSeats, so the bags need no clearing.
    bot->CombatStopWithPets(true);
    bot->ClearInCombat();
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);
    bot->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);
    bot->RemoveArenaAuras();
    bot->RemoveAllSpellCooldown();
    bot->ResetAllPowers();
    if (bot->IsMounted())
        bot->Dismount();

    if (!BotFactory::TeleportWithinMap(bot, start))
        return nullptr;

    // Begin was called for every seat; nothing was created for this one, so the character stays the slot's bot.
    seat.Bot.Abort();
    ++seat.EpisodesPlayed;
    ++_reused;
    return bot;
}

void Animus::Curriculum::StageScenario::Configure(Player* bot, SeatState& seat, bool pvp) const
{
    // Most characters get the spec's standard build; the rest have to be played as they are.
    seat.TalentPlan = RandomTalentPlan(_tuning.Characters);
    uint32 const noise = std::max<uint32>(1, _tuning.Characters.TalentNoisePoints);
    SeatCharacter::Built const built = SeatCharacter::Configure(bot, *seat.L, seat.Spec, pvp, seat.TalentPlan,
        urand(1, noise));
    seat.Build = built.Build;
    seat.UnspentTalentPoints = built.UnspentTalentPoints;
    seat.EquippedItems = built.EquippedItems;

    // The character's spellbook is final now, so resolve every catalog action's highest known rank once. The
    // encoders ask for it three times per action per decision (observation, mask, and applying the action),
    // and each ask walked the rank chain; nothing an episode does changes what the bot knows.
    std::vector<ActionCatalog::Action> const& actions = seat.L->Catalog().Actions();
    seat.KnownRanks.assign(actions.size(), nullptr);
    for (ActionCatalog::Action const& action : actions)
        if (action.Type == ActionCatalog::Kind::Spell)
            seat.KnownRanks[action.Index] = ActionCatalog::KnownRank(bot, action.FirstRank);

    // And read what this character can actually do, now that it is the character it is going to be: the talents
    // are spent, the gear is on and the spellbook is final. Everything that used to ask for a role asks this.
    seat.Apt = Aptitude::Of(ClassAssets::For(*seat.L->Profile), seat.Build, bot);
}

void Animus::Curriculum::StageScenario::PrepareFighter(Player* bot, SeatState& seat) const
{
    seat.Stable = SeatCharacter::PrepareFighter(bot, *seat.L, seat.Apt);
}

void Animus::Curriculum::StageScenario::StockSeats(Env& env)
{
    EnvState& data = Data(env);

    // Warlocks hand out healthstones to the party they are in.
    Player* owner = Owner(env);
    bool warlockInParty = owner && owner->getClass() == CLASS_WARLOCK;
    if (Arena(env).PartyGroup)
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            if (data.Seats[seat].L && data.Seats[seat].L->Profile->Class == CLASS_WARLOCK)
                warlockInParty = true;

    ConsumablePool const& pool = ConsumablePool::Instance();
    for (uint32 seatIndex = 0; seatIndex < data.ActiveSeats; ++seatIndex)
    {
        SeatState& seat = data.Seats[seatIndex];
        Player* bot = SeatBot(env, seatIndex);
        if (!bot || !seat.L)
            continue;

        seat.Supplies = pool.Supplies(seat.Level, bot->GetMaxPower(POWER_MANA) > 0,
            seat.L->Profile->Class == CLASS_WARLOCK, warlockInParty);
        StockBattleSupplies(bot, seat.Supplies, seat.L->Profile->Specs[seat.Spec].Stats);
    }
}

/// A client answers a resurrection offer once (CMSG_RESURRECT_RESPONSE) and is done with it, so nothing in the
/// core clears the request afterwards -- Player::ResurectUsingRequestData does not, and neither does
/// ResurrectPlayer. Polling it every decision, as this must, therefore has to remember that it has already
/// accepted one: without that, a single landed Rebirth stands its target up again free of charge every decision
/// it dies for the rest of the episode. Measured before this guard: 38.4 revives an episode in stage 4 against
/// 0.97 owner deaths, 88% of druid_dps's entire return, and the same shape in druid_heal.
///
/// The credit is paid when the ally is actually alive, not when the offer is taken: the resurrect can be held
/// up by a delayed teleport, and counting the attempt paid for one that never landed.
void Animus::Curriculum::StageScenario::AcceptResurrections(Env& env)
{
    EnvState& data = Data(env);

    // The seats, then the owner in the slot past them.
    std::array<Player*, MAX_SEATS + 1> players{};
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        players[seat] = SeatBot(env, seat);
    players[MAX_SEATS] = Owner(env);

    for (uint32 slot = 0; slot < players.size(); ++slot)
    {
        Player* player = players[slot];
        if (!player)
            continue;

        if (player->IsAlive())
        {
            if (!data.ResurrectMs[slot])
                continue;

            // It landed. Pay the seat that offered it, once, and clear the offer so the next death does not
            // accept a request nobody made again.
            if (uint32 const by = data.ResurrectBy[slot]; by < data.ActiveSeats)
            {
                data.Seats[by].StepRevivedAlly = true;
                ++data.Seats[by].Revives;
            }

            data.ResurrectMs[slot] = 0;
            player->clearResurrectRequestData();
            continue;
        }

        // An offer already accepted and still on its way: wait for it rather than take it again. The retry
        // window gives up on one that never lands, so a stuck teleport does not bar the seat for the episode.
        if (data.ResurrectMs[slot] && env.EpisodeElapsedMs < data.ResurrectMs[slot] + RESURRECT_RETRY_MS)
            continue;

        if (!player->isResurrectRequested())
            continue;

        uint32 by = NO_SEAT;
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            if (Player* reviver = SeatBot(env, seat); reviver && player->isResurrectRequestedBy(reviver->GetGUID()))
                by = seat;

        data.ResurrectBy[slot] = by;
        // 0 means no offer is in flight, so an offer taken on the episode's first millisecond still counts.
        data.ResurrectMs[slot] = std::max<uint64>(1, env.EpisodeElapsedMs);
        player->ResurectUsingRequestData();
    }
}

bool Animus::Curriculum::StageScenario::DeadForGood(Env const& env, uint32 seatIndex) const
{
    CombatTally const& tally = Data(env).Seats[seatIndex].Combat;
    Player* bot = env.FindBot(seatIndex);
    if (!tally.Died || (bot && bot->IsAlive()))
        return false;

    bool const canResurrect = bot && !Arena(env).Pvp && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
    return !canResurrect || env.EpisodeElapsedMs >= tally.DeathMs + _tuning.Resurrection.GraceMs;
}

bool Animus::Curriculum::StageScenario::SeatCanResurrect(Env const& env, uint32 seatIndex) const
{
    SeatState const& seat = Data(env).Seats[seatIndex];
    Player* bot = SeatBot(env, seatIndex);
    if (!bot || !bot->IsAlive() || !seat.L)
        return false;

    std::vector<ActionCatalog::Action> const& revives = seat.L->AllyRevives;
    return std::any_of(revives.begin(), revives.end(), [bot](ActionCatalog::Action const& revive)
    {
        return revive.Type == ActionCatalog::Kind::Spell && ActionCatalog::KnownRank(bot, revive.FirstRank);
    });
}

void Animus::Curriculum::StageScenario::NotifyRecovered(Env& env, int32 who)
{
    if (who >= 0)
        Data(env).Seats[who].Combat.DeathCounted = false;

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->OnRecovered(env, who);
}

void Animus::Curriculum::StageScenario::NotifyPullStarting(Env& env)
{
    for (Encounter* encounter : ActiveEncounters(env))
        encounter->OnPullStarting(env);
}

void Animus::Curriculum::StageScenario::ApplyGoals(Env& env, int32 const* goals)
{
    EnvState& data = Data(env);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        int32 const goal = goals[seat] >= 0 && goals[seat] < int32(GOAL_COUNT) ? goals[seat] : NO_GOAL;
        SeatState& state = data.Seats[seat];
        if (goal != state.Goal && state.Goal != NO_GOAL && goal != NO_GOAL)
            ++state.GoalChanges;

        // A new goal is a new thing to reach, and is paid for again when it is.
        if (goal != state.Goal)
            state.GoalRewarded = false;

        state.Goal = goal;
    }
}

bool Animus::Curriculum::StageScenario::GoalHeld(Env const& env, uint32 seatIndex, Player* bot,
    Unit const* target) const
{
    SeatState const& seat = Data(env).Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    if (!bot || !bot->IsAlive() || seat.Goal == NO_GOAL)
        return false;

    switch (SeatGoal(seat.Goal))
    {
        case SeatGoal::Fight:
            return step.Damage > 0;
        case SeatGoal::Control:
        {
            for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
            {
                Unit const* enemy = env.FindTargetUnit(slot);
                if (enemy && enemy != target && enemy->IsAlive() && Encoding::IsCrowdControlled(enemy))
                    return true;
            }
            return false;
        }
        case SeatGoal::Recover:
            return step.SelfHealing > 0 || bot->HasAuraType(SPELL_AURA_MOD_REGEN)
                || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
        case SeatGoal::Protect:
        {
            // Someone else kept alive: healing, absorbs and damage reductions on the owner or a teammate. What the
            // seat did for itself is Recover's, not Protect's.
            uint64 given = step.AllyHealing;
            for (uint64 healed : step.AgentHealingBy)
                given += healed;
            for (uint64 kept : step.AllyProtectionBy)
                given += kept;
            for (uint64 kept : step.AgentProtectionBy)
                given += kept;
            return given > 0;
        }
        case SeatGoal::Position:
        {
            if (!target || !target->IsAlive() || !seat.L)
                return false;

            float const wanted = Animus::Curriculum::CombatReward::DesiredRange(seat, _tuning.Duel);
            float const distance = bot->GetDistance(target);
            return wanted <= _tuning.Duel.MeleeRange ? bot->IsWithinMeleeRange(target)
                : distance >= _tuning.Duel.MeleeRange && distance <= wanted + GOAL_RANGE_SLACK_YARDS;
        }
        case SeatGoal::Prepare:
            return !bot->IsInCombat() && (seat.StepPreparationMs > 0 || bot->HasStealthAura());
        case SeatGoal::Count:
            break;
    }

    return false;
}

void Animus::Curriculum::StageScenario::ApplyActions(Env& env, int32 const* actions)
{
    // Env upkeep first (linked pulls, the owner, the next pull, the scripted opponent), so the targets below are
    // current.
    for (Encounter* encounter : ActiveEncounters(env))
        encounter->UpdateEnemies(env);
    for (Encounter* encounter : ActiveEncounters(env))
        encounter->Update(env);

    AcceptResurrections(env);

    // The directors speak first: a call made this decision is one the seats can already read when they act on it.
    if (_director && DirectorsActive(env))
        for (uint32 side = 0; side < TEAM_COUNT; ++side)
            _director->Call(env, side, actions[_seatCount + side]);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ApplySeatAction(env, seat, actions[seat]);

    if (CastOwnerActive(env))
        ApplySeatAction(env, OwnerAgent(), actions[OwnerAgent()]);
}

Unit* Animus::Curriculum::StageScenario::SeatTarget(Env const& env, uint32 seat) const
{
    EnvState const& data = Data(env);
    if (seat < data.Seats.size())
        if (ObjectGuid const guid = data.Seats[seat].CurrentTargetGuid)
            if (Player* bot = env.FindBot(seat))
                return ObjectAccessor::GetUnit(*bot, guid);

    return env.FindTargetUnit(0);
}

Unit* Animus::Curriculum::StageScenario::CurrentTarget(Env const& env, uint32 seat)
{
    Unit* target = nullptr;
    for (Encounter* encounter : ActiveEncounters(env))
        if (encounter->SelectTarget(env, seat, target))
            return target;

    return env.FindTargetUnit(0);
}

Animus::Curriculum::SeatView Animus::Curriculum::StageScenario::ViewSeat(Env const& env,
    uint32 seatIndex, Player* bot, Unit* target) const
{
    SeatState const& seat = Data(env).Seats[seatIndex];

    SeatView view;
    view.L = seat.L;
    view.Bot = bot;
    view.Target = target;
    view.Level = seat.Level;
    view.Race = seat.Race;
    view.Spec = seat.Spec;
    view.Apt = seat.Apt;
    // How it is steering, carried over from the last decision: without this a held bearing is forgotten before it
    // can be walked a second time, and the facing actions have nothing to act on.
    view.HeldBearing = seat.HeldBearing;
    view.FacingMode = seat.FacingMode;
    view.Turning = seat.Turning;
    view.PitchTurning = seat.PitchTurning;
    view.Pitch = seat.Pitch;
    view.Facing = seat.Facing;
    view.Probe = &seat.Probe;
    view.Trail = &seat.Trail;
    // Whether its legs are getting anywhere, measured for every seat (TrackMotion). The travel encounter's View
    // replaces the closing rate with the one toward the objective where there is one.
    view.MoveRate = seat.MoveRate;
    view.CloseRate = seat.CloseRate;
    view.SubmergedTime = seat.SubmergedSinceMs && env.EpisodeElapsedMs > seat.SubmergedSinceMs
        ? float(env.EpisodeElapsedMs - seat.SubmergedSinceMs) / 1000.0f : 0.0f;
    view.BreathSpent = float(seat.BreathSpentMs) / float(std::max<uint32>(1, BreathMs()));
    view.Build = &seat.Build;
    view.KnownRanks = &seat.KnownRanks;
    view.Memory = &seat.Memory;
    // Observing only reads the durative action; applying an action starts, runs and stops it (ApplySeatAction hands
    // the same seat's own).
    view.Options = _tuning.Options;
    view.JumpDropSearch = _tuning.Actions.JumpDropSearch;
    view.NowMs = env.EpisodeElapsedMs;
    view.LastStepDamage = seat.LastStepDamage;
    view.LastStepPowerDelta = seat.LastStepPowerDelta;
    view.LastStepDamageTaken = seat.LastStepDamageTaken;
    view.EpisodeTime = std::min(1.0f, float(env.EpisodeElapsedMs) / EPISODE_TIME_SCALE_MS);
    view.CombatTime = seat.InCombat
        ? std::min(1.0f, float(env.EpisodeElapsedMs - seat.CombatStartMs) / MAX_COMBAT_TIME_MS) : 0.0f;
    view.Supplies = seat.Supplies;
    view.SelfResurrectAllowed = !Arena(env).Pvp;

    view.StableCount = uint32(std::min<std::size_t>(seat.Stable.size(), STABLE_SLOTS));
    std::copy_n(seat.Stable.begin(), view.StableCount, view.Stable.begin());

    view.EnemyCount = uint32(std::min<std::size_t>(env.Targets.size(), PACK_SLOTS));
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
        view.Enemies[slot] = env.FindTargetUnit(slot);
    view.TargetSlot = seat.TargetSlot;
    view.FriendSlot = seat.FriendSlot;
    view.RankTier = seat.RankTier;

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->View(env, seatIndex, view);

    // What a player could not know. The critic's state keeps everything.
    if (bot && bot->IsAlive())
    {
        auto const hidden = [bot](Unit const* unit) { return unit && unit != bot && !bot->CanSeeOrDetect(unit); };

        if (hidden(target))
        {
            view.HiddenTarget = target;
            view.Target = nullptr;
            view.TargetSeen = seat.LastSeenGuid == target->GetGUID();
            if (view.TargetSeen)
            {
                view.LastSeen = seat.LastSeen;
                view.TargetUnseenTime = std::min(1.0f,
                    float(env.EpisodeElapsedMs - seat.LastSeenMs) / MAX_UNSEEN_TIME_MS);
            }
        }

        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (hidden(view.Enemies[slot]))
                view.Enemies[slot] = nullptr;

        view.OpponentHidden = hidden(view.Opponent);

        // The order's focus was the one thing here the filter never covered, so a seat observed a called
        // target's distance, bearing and health through a wall. It now says only that it was told, which is
        // what a player in that position knows.
        if (hidden(view.Order.Focus))
        {
            view.Order.FocusUnseen = true;
            view.Order.Focus = nullptr;
        }
    }

    return view;
}

void Animus::Curriculum::StageScenario::TrackTarget(Env const& env, SeatState& seat, Player* bot, Unit* target)
{
    if (!bot || !target || !bot->IsAlive() || !bot->CanSeeOrDetect(target))
        return;

    seat.LastSeenGuid = target->GetGUID();
    seat.LastSeen.Relocate(target);
    seat.LastSeenMs = env.EpisodeElapsedMs;
}

void Animus::Curriculum::StageScenario::ApplySeatAction(Env& env, uint32 seatIndex, int32 action)
{
    Player* bot = env.FindBot(seatIndex);
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!bot || !seat.L)
        return;

    Unit* target = CurrentTarget(env, seatIndex);
    if (!target && !SeatEncoder::ActsWithoutTarget(*seat.L))
        return;

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->BeforeSeatAction(env, seatIndex, target);

    TrackTarget(env, seat, bot, target);

    // A paced action is masked, so only a policy that ignores the mask gets here with one: it does nothing.
    if (action > 0 && Paced(env, seat, uint32(action)))
        action = 0;

    SeatView view = ViewSeat(env, seatIndex, bot, target);
    view.NearestHazard = seat.NearestHazard;
    view.Option = &seat.Option;
    SeatActionResult result;
    SeatOptionSet const started = seat.Option;
    SeatEncoder::Apply(view, action, result);
    // Steering is state, not a one-off order: what the feet and the head were told is what the next decision
    // continues from.
    seat.HeldBearing = view.HeldBearing;
    seat.FacingMode = view.FacingMode;
    seat.Turning = view.Turning;
    seat.PitchTurning = view.PitchTurning;
    seat.Pitch = view.Pitch;
    seat.Facing = view.Facing;
    // Water, the way the core keeps it. A breath is spent under water and comes back ten times as fast above it
    // (Player::HandleDrowning), so bobbing up for a decision buys a fraction of one rather than a whole one; under
    // a water-breathing aura the core runs no timer, and neither does this. Damage taken under water past the
    // breath is drowning -- nothing else hurts a seat down there -- and a death in that state is a drowning.
    if (bot && bot->IsAlive())
    {
        uint32 const breath = std::max<uint32>(1, BreathMs());
        bool const under = bot->IsUnderWater();
        bool const swimming = bot->Unit::IsInWater();
        if (swimming)
            seat.WaterMs += _decisionMs;
        if (bot->GetShapeshiftForm() == FORM_AQUA)
            seat.AquaticMs += _decisionMs;
        if (bot->HasWaterWalkAura() && !swimming
            && bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), bot->GetCollisionHeight(), {}).Status == LIQUID_MAP_WATER_WALK)
            seat.WaterWalkMs += _decisionMs;
        if (under)
        {
            seat.SubmergedMs += _decisionMs;
            if (!seat.SubmergedSinceMs)
                seat.SubmergedSinceMs = std::max<uint32>(1, env.EpisodeElapsedMs);
            if (bot->HasWaterBreathingAura())
                seat.BreathSpentMs = 0;
            else
            {
                // Drowning is the core's own self-damage (Player::EnvironmentalDamage), which the damage hook
                // keeps apart from what enemies do; under water nothing else deals it.
                seat.DrowningDamage += seat.LastStepSelfDamage;
                seat.BreathSpentMs += _decisionMs;
            }
        }
        else
        {
            if (seat.SubmergedSinceMs)
                ++seat.Breaths;
            seat.SubmergedSinceMs = 0;
            seat.BreathSpentMs -= std::min(seat.BreathSpentMs, 10 * _decisionMs);
        }
        seat.BreathSpentMax = std::max(seat.BreathSpentMax, float(seat.BreathSpentMs) / float(breath));
    }
    else if (bot && !bot->IsAlive() && seat.SubmergedSinceMs && !seat.Drowned
        && (seat.LastStepSelfDamage > 0.0f || seat.BreathSpentMs >= BreathMs()))
    {
        seat.Drowned = true;
        seat.SubmergedSinceMs = 0;
    }
    // Every death, once, with the state that explains it -- written for the chain drill, whose first evaluation
    // lost a quarter of its seats to something no tally could see. Capped per process so a bad stage cannot
    // flood the log.
    if (bot && !bot->IsAlive() && !seat.DeathLogged)
    {
        seat.DeathLogged = true;
        static std::atomic<uint32> logged{ 0 };
        if (logged.fetch_add(1) < 40)
        {
            LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(),
                bot->GetPositionY(), bot->GetPositionZ(), bot->GetCollisionHeight(), {});
            LOG_INFO("module.animus", "Seat died: {} level {} at {:.0f} s, under {} swimming {} liquid status {} "
                "z {:.1f} level {:.1f} form {}, breath mirror {:.2f} submerged since {} ms, self damage this step "
                "{:.2f} taken {:.2f}, breaths {} submerged {} s, action {}",
                seat.L ? seat.L->ModelName() : "?", bot->GetLevel(), float(env.EpisodeElapsedMs) / 1000.0f,
                bot->IsUnderWater(), bot->Unit::IsInWater(), uint32(liquid.Status), bot->GetPositionZ(),
                liquid.Level, uint32(bot->GetShapeshiftForm()), float(seat.BreathSpentMs) / float(BreathMs()),
                seat.SubmergedSinceMs, seat.LastStepSelfDamage, seat.LastStepDamageTaken, seat.Breaths,
                seat.SubmergedMs / 1000, action);
        }
    }
    if (action > 0)
        Press(env, seat, bot, uint32(action), result.DidSomething());

    // Time under a durative action (wall time, whichever of the slots are running), and each one started (the
    // options are set by the action this decision applied).
    bool running = false;
    for (std::size_t slot = 0; slot < seat.Option.Slots.size(); ++slot)
    {
        SeatOptionKind const kind = seat.Option.Slots[slot].Kind;
        if (kind == SeatOptionKind::None)
            continue;

        running = true;
        if (started.Slots[slot].Kind != kind)
            ++seat.OptionPresses;
    }

    if (running)
        seat.OptionMs += _decisionMs;

    seat.TargetSlot = view.TargetSlot;
    seat.StepPreparationMs += result.PreparationMs;
    seat.FriendSlot = view.FriendSlot;
    seat.RankTier = view.RankTier;
    seat.HealsOnFull += result.HealsOnFull;
    seat.DefensiveCasts += result.DefensiveCasts;
    seat.HealingCasts += result.HealingCasts;
    seat.HealingPowerSpent += result.HealingPowerSpent;
    seat.StepHealingPowerSpent += result.HealingPowerSpent;
    seat.DownrankedCasts += result.DownrankedCasts;
    seat.SpellCasts += result.SpellCasts;
    seat.BreathingCasts += result.BreathingCasts;
    seat.Jumps += result.Jumps;
    seat.JumpsRefused += result.JumpsRefused;
    if (result.Jumps && result.JumpDrop > MoveBlock::MAX_STEP)
    {
        ++seat.Drops;
        seat.DropYards = std::max(seat.DropYards, result.JumpDrop);
        if (result.JumpFeatherFall)
            ++seat.FeatherFalls;
    }
    seat.Falls += result.Falls;
    seat.FallDamage += result.FallDamage;
    if (result.Falls && bot && !bot->IsAlive())
        ++seat.FallDeaths;
    seat.TrinketUses += result.TrinketUses;
    seat.ItemUses += result.ItemUses;
    seat.ConsumablesUsed += result.ConsumablesUsed;
    seat.SelfResurrections += result.SelfResurrected ? 1 : 0;
    seat.PetAbilities += result.PetAbilities;
    seat.PetOrders += result.PetOrders;
    if (result.PetOrderGiven != PetOrder::None)
        ++seat.PetOrderCounts[std::size_t(result.PetOrderGiven)];

    CombatTally& tally = seat.Combat;
    tally.PreparationMs += result.PreparationMs;
    if (result.StealthOpener)
    {
        tally.StepStealthOpener = true;
        ++tally.StealthOpeners;
    }

    if (!result.StealthUtilityTarget.IsEmpty()
        && std::find(tally.StealthUtilityTargets.begin(), tally.StealthUtilityTargets.end(),
            result.StealthUtilityTarget) == tally.StealthUtilityTargets.end())
    {
        tally.StealthUtilityTargets.push_back(result.StealthUtilityTarget);
        ++tally.StepStealthUtility;
        ++tally.StealthUtilityCasts;
    }

    // A new stealth pays for its targets again.
    if (!bot->HasStealthAura())
        tally.StealthUtilityTargets.clear();

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->OnSeatAction(env, seatIndex, result);

    if (result.CallBeast && CallHunterBeast(bot, result.CallBeast))
        Encoding::StartCallBeastCooldown(bot);
}

void Animus::Curriculum::StageScenario::Observe(Env& env, float* obs, float* state, uint8* mask)
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ObserveSeat(env, seat, obs + seat * _spec.ObsDim, mask ? mask + seat * _spec.NumActions : nullptr);

    for (uint32 side = 0; side < TEAM_COUNT && HasDirectors(); ++side)
        ObserveDirector(env, side, obs + (_seatCount + side) * _spec.ObsDim,
            mask ? mask + (_seatCount + side) * _spec.NumActions : nullptr);

    // The owner's row: a seat's observation when it is played through it, else an empty row that allows only
    // the no-op (the learner marks it absent, AgentPresence).
    if (_castOwner)
    {
        uint32 const agent = OwnerAgent();
        float* row = obs + agent * _spec.ObsDim;
        uint8* maskRow = mask ? mask + agent * _spec.NumActions : nullptr;
        if (CastOwnerActive(env))
            ObserveSeat(env, agent, row, maskRow);
        else
        {
            std::fill(row, row + _spec.ObsDim, 0.0f);
            if (maskRow)
            {
                std::fill(maskRow, maskRow + _spec.NumActions, uint8(0));
                maskRow[0] = 1;
            }
        }
    }

    WriteState(env, state);
}

void Animus::Curriculum::StageScenario::ObserveDirector(Env& env, uint32 side, float* obs, uint8* mask)
{
    // The row is padded to the widest layout's, so clear all of it and let the director's own part fill the front.
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    if (mask)
    {
        std::fill(mask, mask + _spec.NumActions, uint8(0));
        mask[DirectorLayout::ACTION_HOLD] = 1;
    }

    DirectorLayout::DirectorView view;
    if (_director && DirectorsActive(env))
        _director->ViewSide(env, side, view);

    DirectorLayout::Observe(view, obs, mask);
}

void Animus::Curriculum::StageScenario::AgentLayouts(Env const& env, uint16* layout) const
{
    EnvState const& data = Data(env);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        layout[seat] = data.Seats[seat].L ? data.Seats[seat].L->Index : 0;

    for (uint32 side = 0; side < TEAM_COUNT && HasDirectors(); ++side)
        layout[_seatCount + side] = uint16(_directorLayout);

    if (_castOwner)
        layout[OwnerAgent()] = data.Seats[OwnerAgent()].L ? data.Seats[OwnerAgent()].L->Index : 0;
}

void Animus::Curriculum::StageScenario::AgentPresence(Env const& env, uint8* present) const
{
    EnvState const& data = Data(env);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        present[seat] = data.Seats[seat].L ? 1 : 0;

    // A director is an agent only in the episodes that have one; elsewhere it has nothing to say and earns
    // nothing, so the learner should not train on its row.
    bool const directing = DirectorsActive(env);
    for (uint32 side = 0; side < TEAM_COUNT && HasDirectors(); ++side)
        present[_seatCount + side] = directing ? 1 : 0;

    // The owner is an agent only in the episodes that play it through its row: an evaluation's owner and a
    // scripted-share owner are the script's, and the learner neither runs the cast actor nor trains on the row.
    if (_castOwner)
        present[OwnerAgent()] = CastOwnerActive(env) ? 1 : 0;
}

bool Animus::Curriculum::StageScenario::DirectorsActive(Env const& env) const
{
    ArenaDefinition const& arena = Arena(env);
    return HasDirectors() && arena.Directed && arena.DirectorLearned;
}

bool Animus::Curriculum::StageScenario::SideCanSee(Env const& env, uint32 side, Unit const* unit) const
{
    if (!unit)
        return false;

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        if (SideOf(env, seat) == side)
            if (Player const* bot = SeatBot(env, seat); bot && bot->IsAlive() && Encoding::CanSee(bot, unit))
                return true;

    return false;
}

uint32 Animus::Curriculum::StageScenario::SideSeats(Env const& env, uint32 side,
    std::array<uint32, TEAM_SEATS>& out) const
{
    out.fill(NO_SEAT);

    uint32 count = 0;
    for (uint32 seat = 0; seat < _seatCount && count < TEAM_SEATS; ++seat)
        if (SideOf(env, seat) == side)
            out[count++] = seat;

    return count;
}

void Animus::Curriculum::StageScenario::ObserveSeat(Env& env, uint32 seatIndex, float* obs, uint8* mask)
{
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    if (mask)
    {
        std::fill(mask, mask + _spec.NumActions, 0);
        mask[0] = 1;
    }

    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return;

    auto const viewMark = std::chrono::steady_clock::now();
    Player* bot = env.FindBot(seatIndex);
    Unit* target = CurrentTarget(env, seatIndex);      // may be null between gauntlet pulls

    // Note when the bot entered or left combat (SeatView::CombatTime).
    bool const inCombat = bot && bot->IsAlive() && bot->IsInCombat();
    if (inCombat && !seat.InCombat)
        seat.CombatStartMs = env.EpisodeElapsedMs;
    seat.InCombat = inCombat;

    // Whether the seat is in water is a thing the client normally tells the server: Player::SetInWater is called
    // from exactly one place in the core, the movement opcode handler, and a sessionless bot sends no opcodes.
    // So Player::IsInWater -- which returns the cached m_isInWater, unlike Unit::IsInWater which reads the map --
    // was false for the whole life of every bot this sim has ever run. Nothing above it could work: the seat
    // never counted as swimming, the three-dimensional steering never engaged, OBS_IN_WATER was always 0 and
    // swim_seconds was 0 in every episode of every run. The sim has to keep the state the client would.
    if (bot && bot->IsAlive())
    {
        LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(),
            bot->GetPositionY(), bot->GetPositionZ(), bot->GetCollisionHeight(), {});
        bool const swimming = (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) != 0;
        bot->SetInWater(swimming);
        // And the movement flag that goes with it, which is what the spline's speed is read from:
        // MoveSplineInit::Launch asks MovementInfo::GetSpeedType, which answers MOVE_SWIM only under
        // MOVEMENTFLAG_SWIMMING, and nothing ever set it for a bot -- the client does, and there is none. So every
        // swim was launched at run speed: OBS_SWIM_SPEED reported a speed that was never used, and the water
        // arena's crossings, placed so that swimming at 4.7 yd/s is a real choice against walking round at 7, were
        // all won by swimming at 7. The same shape as TravelBlock::AllowFlight, for the same reason.
        bot->SetSwim(swimming);
    }

    TrackTarget(env, seat, bot, target);
    TrackMotion(env, seat, bot, target);
    if (seat.Memory.Actions() != seat.L->NumActions)
        seat.Memory.Reset(seat.L->NumActions);
    seat.Memory.Observe(bot, target, env.EpisodeElapsedMs);
    SeatView view = ViewSeat(env, seatIndex, bot, target);
    view.NearestHazard = seat.NearestHazard;
    view.Option = &seat.Option;
    SeatEncoder::ObserveNs[SeatEncoder::OBSERVE_VIEW].fetch_add(uint64(std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - viewMark).count()), std::memory_order_relaxed);
    SeatEncoder::Observe(view, obs, mask);

    if (mask)
        for (uint32 action = 1; action < seat.L->NumActions; ++action)
            if (mask[action] && Paced(env, seat, action))
                mask[action] = 0;
}

bool Animus::Curriculum::StageScenario::Paced(Env const& env, SeatState const& seat, uint32 action) const
{
    return seat.Memory.Paced(*seat.L, action, env.EpisodeElapsedMs, _tuning.Actions);
}

void Animus::Curriculum::StageScenario::Press(Env const& env, SeatState& seat, Player* bot, uint32 action,
    bool didSomething) const
{
    Layout const& layout = *seat.L;
    std::optional<BlockId> const block = layout.BlockOfAction(action);
    if (!block)
        return;

    ++seat.ActionsPressed;
    uint32 const now = env.EpisodeElapsedMs;
    CurriculumTuning::ActionTuning const& pacing = _tuning.Actions;
    seat.Memory.Press(layout, action, now, pacing, bot, &seat.KnownRanks);

    // The same action again within the window, past the free presses: charged at the next reward. Movement orders
    // are always free, and so is a press that did something -- a spell that started casting, an item used, a pet
    // ability. Pressing the same button again is waste only when the button did nothing: a caster's rotation is one
    // nuke over and over, and charging that charges the correct play (stage1_duel at 10M: the two classes with the
    // most repeated presses, warlock_dps at 39.7 an episode and mage_dps at 16.6, were the two lowest scoring).
    // What the charge was built for is untouched: orders to a pet already obeying, a target selected again, a stance
    // pressed twice -- none of them do anything, and all of them still count.
    if (didSomething)
        return;

    if (GetBlock(*block).IsMovement(action - layout.Slice(*block).ActionFirst))
        return;

    if (seat.PressTimes.size() != layout.NumActions)
        seat.PressTimes.assign(layout.NumActions, {});

    std::vector<uint32>& presses = seat.PressTimes[action];
    std::erase_if(presses, [now, &pacing](uint32 pressed) { return pressed + pacing.RepeatWindowMs <= now; });
    presses.push_back(now);
    if (presses.size() > pacing.RepeatFree)
    {
        ++seat.StepRepeats;
        ++seat.RepeatedPresses;
    }
}

void Animus::Curriculum::StageScenario::Reward(Env& env, float* reward)
{
    for (Encounter* encounter : ActiveRewardOrder(env))
        encounter->BeforeRewards(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        reward[seat] = SeatReward(env, seat);

    // A director is paid exactly what its side is paid, averaged: it has no body to score, and a team-level
    // action is only worth what it did for the team. Any other reward would teach it to look busy.
    //
    // Less the compliance shaping its own orders earned those seats (RewardTerm::OrderMatch), which is the one
    // part of their reward it can move without the fight going any better: a director that kept it would learn
    // to call whoever its seats were already fighting. The seats are paid to follow; the director is paid only
    // for what following achieved.
    for (uint32 side = 0; side < TEAM_COUNT && HasDirectors(); ++side)
    {
        float total = 0.0f;
        uint32 seats = 0;
        for (uint32 seat = 0; seat < _seatCount && DirectorsActive(env); ++seat)
            if (SideOf(env, seat) == side && Data(env).Seats[seat].L)
            {
                total += reward[seat] - (_director ? _director->ShapingPaid(env, seat) : 0.0f);
                ++seats;
            }

        reward[_seatCount + side] = seats ? total / float(seats) : 0.0f;
    }

    // The owner's row is observed and acted on but never paid: it is a frozen checkpoint's, not a learner's.
    // Its own bookkeeping still runs, so what it observes of itself next decision is not stale.
    if (_castOwner)
    {
        reward[OwnerAgent()] = 0.0f;
        if (CastOwnerActive(env))
            TrackSeatStep(env, OwnerAgent(), env.FindBot(OwnerAgent()));
    }

    for (Encounter* encounter : ActiveRewardOrder(env))
        encounter->AfterRewards(env);
}

void Animus::Curriculum::StageScenario::TrackSeatStep(Env& env, uint32 seatIndex, Player* bot)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return;

    seat.LastStepDamage = float(env.StepStats[seatIndex].Damage) / seat.DamageScale;
    Unit* target = CurrentTarget(env, seatIndex);
    seat.CurrentTargetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
    seat.LastStepDamageTaken = bot
        ? float(env.StepStats[seatIndex].DamageTaken) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;
    seat.LastStepSelfDamage = bot
        ? float(env.StepStats[seatIndex].SelfDamage) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;
    TrackSupport(env, seatIndex, bot);
}

/// The nearest ground effect the seat is not in yet, so it can be walked around rather than only walked out of.
/// The grid search runs every HAZARD_SEARCH_MS; between searches the cached hazard is measured against the seat's
/// own position again, which is exact because a ground effect stays where it was cast.
void Animus::Curriculum::StageScenario::TrackHazards(Env const& env, SeatState& seat, Player* bot)
{
    Hazard& nearest = seat.NearestHazard;
    if (env.EpisodeElapsedMs >= seat.HazardSearchMs + HAZARD_SEARCH_MS || !seat.HazardSearchMs)
    {
        seat.HazardSearchMs = env.EpisodeElapsedMs;
        nearest = Hazard();
        nearest.Present = Encoding::FindNearestHazard(bot, HAZARD_SEARCH_RANGE, nearest);
        return;
    }

    if (!nearest.Present)
        return;

    // It may have run out, and the seat has moved: measure it again rather than search again.
    nearest.Distance = bot->GetExactDist2d(nearest.Centre.GetPositionX(), nearest.Centre.GetPositionY());
    // The seat's own frame, not the spline's: OBS_HAZARD_BEARING_* has to agree with every other bearing the
    // move block reports, and bot->GetOrientation() is the direction of travel while a spline is running.
    nearest.Bearing = bot->GetAngle(nearest.Centre.GetPositionX(), nearest.Centre.GetPositionY())
        - seat.Facing;
}

/// Whether the seat's legs are getting anywhere, for every arena: how far it moved over about the last second
/// against how far running would have carried it, and how much of the distance to its target that closed. The
/// travel encounter used to be the only source, so every other arena read 0 and a seat wedged against a rock in a
/// fight looked, from the inside, exactly like one walking freely. Where there is an objective the encounter's
/// View still replaces the closing rate with the one toward it.
void Animus::Curriculum::StageScenario::TrackMotion(Env const& env, SeatState& seat, Player const* bot,
    Unit const* target)
{
    constexpr uint32 MARK_MS = 1000;        // the rates are taken over about a second, as the encounter's were
    constexpr float RUN_SPEED = 7.0f;       // yards a second unmounted and unhasted (TravelBlock::BASE_RUN_SPEED)

    if (!bot || !bot->IsAlive())
    {
        seat.MotionHasLast = false;
        seat.MoveRate = 0.0f;
        seat.CloseRate = 0.0f;
        return;
    }

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    if (seat.MotionHasLast)
    {
        float const dx = x - seat.MotionLastX;
        float const dy = y - seat.MotionLastY;
        seat.MotionTravelled += std::sqrt(dx * dx + dy * dy);
    }
    seat.MotionLastX = x;
    seat.MotionLastY = y;
    seat.MotionHasLast = true;

    float const range = target ? bot->GetExactDist2d(target) : -1.0f;
    if (!seat.MotionMarkMs || env.EpisodeElapsedMs < seat.MotionMarkMs)
    {
        seat.MotionMarkMs = std::max<uint32>(1, env.EpisodeElapsedMs);
        seat.MotionMarkTravelled = seat.MotionTravelled;
        seat.MotionMarkRange = range;
        return;
    }

    if (env.EpisodeElapsedMs - seat.MotionMarkMs < MARK_MS)
        return;

    float const seconds = float(env.EpisodeElapsedMs - seat.MotionMarkMs) / 1000.0f;
    seat.MoveRate = (seat.MotionTravelled - seat.MotionMarkTravelled) / (seconds * RUN_SPEED);
    seat.CloseRate = seat.MotionMarkRange >= 0.0f && range >= 0.0f
        ? (seat.MotionMarkRange - range) / (seconds * RUN_SPEED) : 0.0f;
    seat.MotionMarkMs = env.EpisodeElapsedMs;
    seat.MotionMarkTravelled = seat.MotionTravelled;
    seat.MotionMarkRange = range;
}

/// Count an enemy cast the seat could have interrupted, once per cast. The press-to-interrupt ratio alone cannot
/// say whether a policy is pressing too often or whether there was simply nothing to interrupt; this is the
/// denominator. The seat's own target is the one it could act on, so that is the one counted.
void Animus::Curriculum::StageScenario::TrackInterruptibleCast(Env const& /*env*/, SeatState& seat, Player* bot,
    Unit* target)
{
    if (!target || !target->IsAlive() || !bot->IsWithinDistInMap(target, INTERRUPTIBLE_CAST_RANGE))
    {
        seat.LastInterruptibleCaster.Clear();
        seat.LastInterruptibleSpell = 0;
        return;
    }

    SpellInfo const* info = IncomingSpell::Interruptible(target)
        ? IncomingSpell::CastInProgress(target, nullptr, nullptr, nullptr) : nullptr;
    if (!info)
    {
        seat.LastInterruptibleCaster.Clear();
        seat.LastInterruptibleSpell = 0;
        return;
    }

    // The same cast seen again is not another cast.
    if (seat.LastInterruptibleCaster == target->GetGUID() && seat.LastInterruptibleSpell == info->Id)
        return;

    seat.LastInterruptibleCaster = target->GetGUID();
    seat.LastInterruptibleSpell = info->Id;
    ++seat.InterruptibleCastsSeen;
}

void Animus::Curriculum::StageScenario::TrackSupport(Env& env, uint32 seatIndex, Player* bot)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!bot)
    {
        seat.Absorbs.clear();
        return;
    }

    // Its friends: itself, the owner (ally 0) and the other seats.
    struct Friend
    {
        Unit* U;
        int32 Ally;
        int32 Agent;
    };

    std::vector<Friend> friends = { { bot, -1, int32(seatIndex) } };
    if (Player* owner = Owner(env))
        friends.push_back({ owner, 0, -1 });
    for (uint32 other = 0; other < _seatCount; ++other)
        if (Player* teammate = other != seatIndex && Data(env).Seats[other].L ? env.FindBot(other) : nullptr)
            friends.push_back({ teammate, -1, int32(other) });

    AgentStats& step = env.StepStats[seatIndex];
    auto const credit = [&step, seatIndex](Friend const& friendRef, uint64 amount)
    {
        if (friendRef.Ally >= 0)
            step.AllyProtectionBy[friendRef.Ally] += amount;
        else if (uint32(friendRef.Agent) == seatIndex)
            step.SelfProtection += amount;
        else if (uint32(friendRef.Agent) < MAX_AGENTS)
            step.AgentProtectionBy[friendRef.Agent] += amount;
    };

    bool low = false;
    std::vector<SeatState::AbsorbTrack> now;
    for (Friend const& friendRef : friends)
    {
        low |= friendRef.U->IsAlive() && friendRef.U->GetHealthPct() < LOW_HEALTH_PCT;
        for (AuraEffect const* effect : friendRef.U->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB))
            if (effect->GetCasterGUID() == bot->GetGUID())
                now.push_back({ friendRef.U->GetGUID(), effect->GetId(), effect->GetAmount(),
                    effect->GetBase()->GetDuration() });
    }

    // An absorb that lost amount soaked it; one gone early (not expired, its friend alive) soaked the rest. A re-cast
    // (more time left than before) starts over.
    int32 const expirySlack = int32(_decisionMs) + ABSORB_EXPIRY_SLACK_MS;
    for (SeatState::AbsorbTrack const& before : seat.Absorbs)
    {
        auto const friendRef = std::find_if(friends.begin(), friends.end(),
            [&before](Friend const& candidate) { return candidate.U->GetGUID() == before.Unit; });
        if (friendRef == friends.end())
            continue;

        auto const after = std::find_if(now.begin(), now.end(), [&before](SeatState::AbsorbTrack const& candidate)
        {
            return candidate.Unit == before.Unit && candidate.SpellId == before.SpellId;
        });

        if (after != now.end())
        {
            if (after->Amount < before.Amount && after->DurationLeftMs <= before.DurationLeftMs)
                credit(*friendRef, uint64(before.Amount - after->Amount));
        }
        else if (before.DurationLeftMs > expirySlack && friendRef->U->IsAlive() && before.Amount > 0)
            credit(*friendRef, uint64(before.Amount));
    }

    seat.Absorbs = std::move(now);
    if (low && bot->IsAlive())
        seat.LowHealthMs += _decisionMs;
}

float Animus::Curriculum::StageScenario::SeatReward(Env& env, uint32 seatIndex)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return 0.0f;        // an empty party seat

    Player* bot = env.FindBot(seatIndex);
    seat.LastStepDamage = float(env.StepStats[seatIndex].Damage) / seat.DamageScale;

    // Who this seat is actually fighting, asked of the encounters once a decision and remembered for the const
    // readers. It is the other seat in self-play, which no target slot holds.
    Unit* target = CurrentTarget(env, seatIndex);
    seat.CurrentTargetGuid = target ? target->GetGUID() : ObjectGuid::Empty;

    // Before any encounter's reward: several read it (the pulls' and duel's damage taken, the owner's tank refund).
    seat.LastStepDamageTaken = bot
        ? float(env.StepStats[seatIndex].DamageTaken) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;
    seat.LastStepSelfDamage = bot
        ? float(env.StepStats[seatIndex].SelfDamage) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;

    // Also before the encounters: the owner's and teammates' rewards read what the seat's absorbs soaked on them.
    TrackSupport(env, seatIndex, bot);

    // Standing in something, and what it cost. The damage is charged on top of DamageTaken: taking a hit that could
    // have been walked out of is worse than taking one that could not, and this is the only term that pays a seat
    // for moving its feet. Both read zero where nothing puts anything on the ground.
    if (bot && bot->IsAlive())
    {
        // Standing in one is charged by the second as well as by the damage it does. The damage alone is small,
        // late and noisy -- it arrives in ticks after the decision that put the seat there -- while the seconds are
        // immediate and describe the behaviour itself, which is what Spacing does for a ranged spec in melee. Capped
        // per episode so it can never be worth leaving a fight over: melee have to stand in melee.
        if (Encoding::StandingInHazards(bot, nullptr))
        {
            seat.HazardMs += _decisionMs;
            float const seconds = float(_decisionMs) / 1000.0f;
            float const room = std::max(0.0f, _tuning.Hazards.Max + seat.Rewards.Episode(RewardTerm::Hazard));
            seat.Rewards.Add(RewardTerm::Hazard, -std::min(_tuning.Hazards.Standing * seconds, room));
        }

        if (uint64 const hazardDamage = env.StepStats[seatIndex].HazardDamage)
        {
            seat.HazardDamage += hazardDamage;
            float const share = float(hazardDamage) / float(std::max<uint32>(1, bot->GetMaxHealth()));
            float const room = std::max(0.0f, _tuning.Hazards.Max + seat.Rewards.Episode(RewardTerm::Hazard));
            seat.Rewards.Add(RewardTerm::Hazard, -std::min(_tuning.Hazards.Damage * share, room));
        }

        // Enemy casts there was something to do about: counted once each, when one the seat could interrupt starts.
        TrackInterruptibleCast(env, seat, bot, target);
        TrackHazards(env, seat, bot);
    }

    // A pet that died: a corpse still the seat's (a hunter's beast), or one gone while nearly dead (a demon's body
    // leaves at once). Replacing a healthy pet with another is not a death.
    Creature* pet = bot ? PetBlock::FindPet(bot) : nullptr;
    if ((pet && !pet->IsAlive()) || (!pet && seat.LastPetHealth > 0.0f && seat.LastPetHealth < 0.1f))
        seat.PetDied = true;
    seat.LastPetHealth = pet && pet->IsAlive() ? std::max(0.001f, pet->GetHealthPct() / 100.0f) : 0.0f;
    PetBlock::DefaultStance(pet, seat.LastPetGuid);

    // What the pet does while it is out.
    if (pet && pet->IsAlive())
    {
        seat.PetOutMs += _decisionMs;
        if (pet->GetVictim())
            seat.PetAttackingMs += _decisionMs;
        if (pet->HasReactState(REACT_PASSIVE))
            seat.PetPassiveMs += _decisionMs;
        if (CharmInfo const* charmInfo = pet->GetCharmInfo(); charmInfo && charmInfo->HasCommandState(COMMAND_STAY))
            seat.PetStayingMs += _decisionMs;
    }

    // Standing again (resurrected, or recovered after a pull): the next death is paid for again.
    if (bot && bot->IsAlive())
        seat.Combat.DeathCounted = false;

    for (Encounter* encounter : ActiveRewardOrder(env))
        encounter->Reward(env, seatIndex, bot, seat.Rewards);

    seat.Rewards.Add(RewardTerm::Repeat, -_tuning.Actions.Repeat * float(seat.StepRepeats));
    seat.StepRepeats = 0;

    // The goal the learner is pursuing, and whether this decision went with it.
    if (seat.Goal != NO_GOAL)
    {
        ++seat.GoalDecisions[std::size_t(seat.Goal)];
        if (GoalHeld(env, seatIndex, bot, target))
        {
            ++seat.GoalMatches[std::size_t(seat.Goal)];

            // Paid for reaching the goal, once per goal held, not for sitting in it: a ranged seat holds
            // SeatGoal::Position by standing at its range, and paying that every decision made keeping away from
            // the fight the second largest earner in the stage (measured 2026-09-17: +0.93 an episode, more than
            // the approach, casting and health terms together). goal_match_share still reports every decision.
            if (!seat.GoalRewarded)
            {
                seat.GoalRewarded = true;
                seat.Rewards.Add(RewardTerm::GoalMatch, _tuning.Goals.Match);
            }
        }
    }

    seat.StepPreparationMs = 0;

    // Looking after itself, in every stage: effective healing, and what its own absorbs and reductions kept off,
    // less what the healing cost. Effective healing is already all the reward pays -- overhealing, a heal over time
    // ticking on a full bar included, earns nothing as it lands -- so what was missing was never a penalty for
    // waste but a price for mana. With one, the objective is healing per mana and a cheaper rank can win.
    if (bot)
    {
        AgentStats const& step = env.StepStats[seatIndex];
        seat.Rewards.Add(RewardTerm::SelfHealing, _tuning.Support.SelfHealing
            * float(step.SelfHealing + step.SelfProtection) / float(std::max<uint32>(1, bot->GetMaxHealth())));

        if (uint32 const spent = seat.StepHealingPowerSpent; spent && bot->getPowerType() == POWER_MANA)
        {
            // Pull after pull, the mana a heal costs is already paid for at the next engagement (readiness), so
            // charging it here as well prices the same mana twice. The charge is a stand-in for an opportunity cost
            // and belongs where there is no later fight to have one.
            PullSchedule const schedule = Arena(env).Schedule;
            bool const readiness = schedule == PullSchedule::Gauntlet || schedule == PullSchedule::Sequence;
            float const weight = readiness ? _tuning.Support.HealingManaWithReadiness : _tuning.Support.HealingMana;
            if (weight > 0.0f)
                seat.Rewards.Add(RewardTerm::HealingMana,
                    -weight * float(spent) / float(std::max<uint32>(1, bot->GetMaxPower(POWER_MANA))));
        }
        seat.StepHealingPowerSpent = 0;
    }

    if (bot)
    {
        Powers const power = bot->getPowerType();
        uint32 const current = bot->GetPower(power);
        float const maxPower = float(std::max<uint32>(1, bot->GetMaxPower(power)));
        seat.LastStepPowerDelta = (float(current) - float(seat.LastPower)) / maxPower;
        seat.LastPower = current;
    }

    return seat.Rewards.TakeStep();
}

void Animus::Curriculum::StageScenario::WriteState(Env const& env, float* state) const
{
    std::fill(state, state + _spec.StateDim, 0.0f);

    EnvState const& data = Data(env);
    float const originX = SpawnPointFor(env).GetPositionX();
    float const originY = SpawnPointFor(env).GetPositionY();

    state[STATE_EPISODE_TIME] = env.EpisodeLengthMs
        ? std::min(1.0f, float(env.EpisodeElapsedMs) / float(env.EpisodeLengthMs)) : 0.0f;
    if (Data(env).Arena < MAX_ARENAS)
        state[STATE_ARENA_FIRST + Data(env).Arena] = 1.0f;

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->WriteState(env, state);

    std::array<Player*, MAX_SEATS> bots{};
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Player* bot = env.FindBot(seat);
        SeatState const& slot = data.Seats[seat];
        bots[seat] = bot;
        if (!bot || !slot.L)
            continue;

        float* features = state + STATE_GLOBAL_COUNT + seat * STATE_SEAT_FEATURES;
        features[STATE_SEAT_PRESENT] = 1.0f;
        features[STATE_SEAT_ALIVE] = bot->IsAlive() ? 1.0f : 0.0f;
        features[STATE_SEAT_HEALTH] = bot->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
            features[STATE_SEAT_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
        features[STATE_SEAT_OTHER_POWER] = OtherPower(bot);
        features[STATE_SEAT_LEVEL] = float(slot.Level) / float(DEFAULT_MAX_LEVEL);
        slot.Apt.WriteBrief(features + STATE_SEAT_APTITUDE_FIRST);
        WriteOneHot(PLAYABLE_CLASSES, slot.L->Profile->Class, features + STATE_SEAT_CLASS_FIRST);
        features[STATE_SEAT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        features[STATE_SEAT_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
        features[STATE_SEAT_X] = RelativePosition(bot->GetPositionX(), originX);
        features[STATE_SEAT_Y] = RelativePosition(bot->GetPositionY(), originY);
    }

    // The enemies: the env's targets (creatures, or the scripted enemy player); in self-play each seat's opponent is
    // the other seat, already in the seat block.
    Player* owner = Owner(env);
    float const leadLevel = float(data.Seats[0].Level);
    for (uint32 slot = 0; slot < env.Targets.size() && slot < PACK_SLOTS; ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy)
            continue;

        float* features = state + STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + slot * STATE_ENEMY_FEATURES;
        Unit const* victim = enemy->GetVictim();

        features[STATE_ENEMY_PRESENT] = 1.0f;
        features[STATE_ENEMY_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
        features[STATE_ENEMY_HEALTH] = enemy->GetHealthPct() / 100.0f;
        features[STATE_ENEMY_X] = RelativePosition(enemy->GetPositionX(), originX);
        features[STATE_ENEMY_Y] = RelativePosition(enemy->GetPositionY(), originY);
        features[STATE_ENEMY_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        features[STATE_ENEMY_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
        features[STATE_ENEMY_LEVEL_DIFF] = (float(enemy->GetLevel()) - leadLevel) / 5.0f;
        features[STATE_ENEMY_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
        features[STATE_ENEMY_ON_OWNER] = victim && victim == owner ? 1.0f : 0.0f;
        for (uint32 seat = 0; seat < _seatCount; ++seat)
        {
            if (!victim || victim != bots[seat])
                continue;

            features[STATE_ENEMY_ON_SEAT] = 1.0f;
            features[STATE_ENEMY_SEAT_INDEX] = float(seat) / float(MAX_SEATS);
            uint32 const group = seat / GROUP_SEATS;
            if (group < RAID_GROUPS)
                features[STATE_ENEMY_SEAT_GROUP_FIRST + group] = 1.0f;
            if (data.Seats[seat].L)
                data.Seats[seat].Apt.WriteBrief(features + STATE_ENEMY_SEAT_APTITUDE_FIRST);
            break;
        }

        if (Player* lead = bots[0])
        {
            features[STATE_ENEMY_MAX_HEALTH] = std::min(1.0f,
                float(enemy->GetMaxHealth()) / float(std::max<uint32>(1, lead->GetMaxHealth())) / 4.0f);
            features[STATE_ENEMY_ARMOR] = Encoding::ArmorReduction(enemy, lead->GetLevel());
        }
        features[STATE_ENEMY_DAMAGE_MODIFIER] = Encoding::DamageModifier(enemy) / 2.0f;
        features[STATE_ENEMY_RUN_SPEED] = enemy->GetSpeedRate(MOVE_RUN) / 2.0f;
        Encoding::WriteOpponentType(enemy, features + STATE_ENEMY_TYPE_FIRST);
    }
}

void Animus::Curriculum::StageScenario::EpisodeInfo(Env const& env, float* info) const
{
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        _info.Write(env, seat, info + seat * _spec.EpisodeInfoDim);

    // A director has no character, so none of the per-seat columns mean anything for it. Its row is left zero,
    // and `present` being one of those zeros is what keeps it out of the episode metrics; what the director did
    // is reported by its side's seats (order_changes and the rest).
    for (uint32 side = 0; side < TEAM_COUNT && HasDirectors(); ++side)
    {
        float* row = info + (_seatCount + side) * _spec.EpisodeInfoDim;
        std::fill(row, row + _spec.EpisodeInfoDim, 0.0f);
    }

    // The owner's row likewise: what happened to the owner is the seats' columns (owner_deaths and the rest),
    // and a zero `present` keeps the row out of every per-seat metric.
    if (_castOwner)
    {
        float* row = info + OwnerAgent() * _spec.EpisodeInfoDim;
        std::fill(row, row + _spec.EpisodeInfoDim, 0.0f);
    }
}

bool Animus::Curriculum::StageScenario::ScriptedAction(std::string const& policy, float const* obs,
    uint8 const* mask, uint16 layoutIndex, int32& action) const
{
    if (_layouts.empty() || !Baselines::Supports(policy, _layouts.front()))
        return false;

    // A director has no scripted baseline to fall back on: its layout carries no catalog for one to reason
    // about, and what a baseline director would say is nothing at all, which is the hold action.
    if (layoutIndex < _layouts.size() && _layouts[layoutIndex].Director)
    {
        action = int32(DirectorLayout::ACTION_HOLD);
        return true;
    }

    action = layoutIndex < _layouts.size() ? Baselines::Choose(policy, _layouts[layoutIndex], obs, mask) : 0;
    return true;
}

void Animus::Curriculum::StageScenario::Teardown(Env& env)
{
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            creature->DespawnOrUnsummon();

    // In reverse reward order: the party disbands before its owner leaves.
    for (auto encounter = _rewardOrder.rbegin(); encounter != _rewardOrder.rend(); ++encounter)
        (*encounter)->Teardown(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        Data(env).Seats[seat].Bot.Destroy();
    if (_castOwner)
        ReleaseOwnerSeat(env);

    env.Bots.clear();
    env.Targets.clear();
}
