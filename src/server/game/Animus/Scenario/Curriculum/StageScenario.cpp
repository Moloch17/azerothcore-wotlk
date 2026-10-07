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
#include "Camera.h"
#include <set>
#include <span>
#include "ResetTiming.h"
#include "ControllerCost.h"
#include "CharmInfo.h"
#include "BotAccounts.h"
#include "Config.h"
#include "Containers.h"
#include "Corpse.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "CoreBlock.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DeathBlock.h"
#include "DuelBlock.h"
#include "MoveBlock.h"
#include "MovePrice.h"
#include "Forge.h"
#include "World.h"
#include "EncoderSupport.h"
#include "GoalBlock.h"
#include "Encounters.h"
#include "SeekEncounter.h"
#include "SightEncounter.h"
#include "InteractEncounter.h"
#include "CombatEncounter.h"
#include "RolesEncounter.h"
#include "CombatBlock.h"
#include "PartyFollowEncounter.h"
#include "BuildRetry.h"
#include "SpellMgr.h"
#include "Env.h"
#include "EnvPool.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MapDefines.h"
#include "Opponents.h"
#include "PartyFramesBlock.h"
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
#include "MoveSpline.h"
#include "MapWorldQuery.h"
#include "PlayerLink.h"
#include "UnitBody.h"
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
    static_assert(PACK_SLOTS <= Animus::MAX_TARGETS, "every enemy slot a seat observes is attributed its damage");
    static_assert(NAMED_ENEMY_SLOTS <= PACK_SLOTS, "the named enemy slots are the first of the observed");
    static_assert(MAX_SEATS == RAID_GROUPS * GROUP_SEATS, "the seats are the raid's groups");
    static_assert(PARTY_MEMBERS == GROUP_MEMBERS + SPOTLIGHT_SLOTS, "teammate slots are the group and the spotlights");

    constexpr float PARTY_SPACING = 3.0f;
    constexpr float REWARD_TUNING_MS = 50.0f;       // per-decision reward terms are tuned for this decision interval
    constexpr float MAX_COMBAT_TIME_MS = 60000.0f;
    constexpr float MAX_UNSEEN_TIME_MS = 20000.0f;
    /// Yards round the spawn an instance used as empty ground is cleared over (SpawnArea::ClearMap): the whole of a
    /// small dungeon -- the Stockades spans about 150 by 290 yards.
    constexpr float INSTANCE_CLEAR_RADIUS = 300.0f;
    /// ... and for the interact stage's Deadmines (M3), whose sites lie up to about 350 yards from its far end.
    constexpr float INTERACT_CLEAR_RADIUS = 600.0f;
    /// ... and the party follow's dungeons, whose last bosses stand further from the door than that (the Deadmines'
    /// ship is several hundred yards from its entrance).
    constexpr float DUNGEON_CLEAR_RADIUS = 1000.0f;
    constexpr float GOAL_RANGE_SLACK_YARDS = 5.0f;  // a ranged spec holds its range to within this (SeatGoal::Position)
    constexpr float LOW_HEALTH_PCT = 35.0f;         // a friend below this is low (low_health_seconds)
    /// How far a cast counts as one this seat could have answered (interruptible_casts_seen): an interrupt's own
    /// range, near enough, and beyond it the press was never available anyway.
    constexpr float INTERRUPTIBLE_CAST_RANGE = 30.0f;

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
    /// A tank's tanking stance, form, aura or presence (its first rank): Defensive Stance, Righteous Fury, Bear Form,
    /// Frost Presence. 0 for a class that has none.
    uint32 TankModeSpell(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:         return 71;
            case CLASS_PALADIN:         return 25780;
            case CLASS_DRUID:           return 5487;
            case CLASS_DEATH_KNIGHT:    return 48263;
            default:                    return 0;
        }
    }

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
    _decisionScale(float(settings.DecisionMs) / REWARD_TUNING_MS), _decisionMs(settings.DecisionMs),
    // The ladder is not saved with the policy: a run resumed from a checkpoint names the rung it had reached.
    _wingLadder(uint32(WING_RUNGS.size()), _tuning.Instance.WingRungRuns, _tuning.Instance.WingRungTarget,
        _tuning.Instance.WingRungStart)
{

    if (MapEntry const* mapEntry = sMapStore.LookupEntry(_spawnMapId))
        _continent = !mapEntry->Instanceable();

    // A reset stays on its env's own continent replica unless an arena sends the episode elsewhere (an instance) or
    // builds what every map shares and nothing locks: a core group (an owner, a party). Only then is it safe to run
    // on the map thread (EnvPool, ResetDefer); every other stage resets on the world thread as it always has.
    _resetsStayOnMap = _continent && !stage.AnyArena([this](ArenaDefinition const& arena)
    {
        return arena.Owner || arena.PartyGroup || arena.Against == Opposition::Instance
            || (arena.MapId && arena.MapId != _spawnMapId);
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

    // An owner can be any class, whatever StageSettings::Classes says: build every profile's assets now (seconds
    // each) rather than on the world thread in the middle of an episode reset.
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.Owner; }))
        for (ClassProfile const& profile : ClassProfiles())
            ClassAssets::For(profile);

    // The owner's own row, after the seats, where an arena plays it from a frozen checkpoint.
    // ... or the party follow's leader (Opposition::PartyFollow): the same slot, moved by the controller, played by a
    // frozen checkpoint in the episodes that cast it and by the encounter's own keys in the rest. The leader is no
    // dead code: PartyFollowEncounter builds it in this slot (OwnerAgent) and CastOwnerActive plays it.
    _castOwner = _stage.AnyArena([](ArenaDefinition const& arena)
    {
        return (arena.Owner && arena.OwnerCast) || arena.Against == Opposition::PartyFollow;
    });
    _spec.AgentsPerEnv = _seatCount + (_castOwner ? 1 : 0);
    for (Layout const& layout : _layouts)
    {
        _spec.ObsDim = std::max(_spec.ObsDim, layout.ObsDim);
        _spec.NumActions = std::max(_spec.NumActions, layout.NumActions);
        _spec.Layouts.push_back(LayoutSpec{ layout.Profile->Name,
            layout.ObsDim, layout.NumActions });
    }

    _spec.StateDim = STATE_GLOBAL_COUNT + MAX_SEATS * STATE_SEAT_FEATURES + PACK_SLOTS * STATE_ENEMY_FEATURES;
    // The camera's image travels as bytes beside the rows (camera-vision.BYTES.md): every agent has a row of them
    // when the stage has a vision block, and none is sent when it has not.
    _spec.ImageBytes = _stage.Has(BlockId::Vision) ? Vision::ImageBytes(Vision::Current()) : 0;
    // ... and its look head's choices come back with the actions (free look, protocol 22).
    _spec.LookHeads = _stage.Has(BlockId::Vision) ? Vision::FreeLook::HEADS : 0;
    // The mental map's crop travels as bytes too, in a section of its own (perception-goals REDESIGN §3, protocol 24).
    _spec.MapBytes = _stage.Has(BlockId::Map) ? Vision::CROP_BYTES : 0;
    _data.resize(settings.Envs);

    // The encounters any of the stage's arenas uses, in build order.
    uint32 const envs = settings.Envs;
    PullsEncounter* pulls = nullptr;
    CreatureEncounter* creature = nullptr;
    InstanceEncounter* instance = nullptr;

    auto const add = [this](auto encounter)
    {
        auto* raw = encounter.get();
        _encounters.push_back(std::move(encounter));
        return raw;
    };

    auto const hasPulls = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Pulls; };
    auto const hasCreature = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Creature; };
    auto const hasHazards = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Hazards; };
    auto const hasInstance = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Instance; };
    auto const hasDummy = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Dummy; };
    auto const hasPartyFollow = [](ArenaDefinition const& arena)
    {
        return arena.Against == Opposition::PartyFollow;
    };
    auto const hasSeek = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Seek; };
    auto const hasSight = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Sight; };
    auto const hasInteract = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Interact; };
    auto const hasCombat = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Combat; };
    auto const hasRoles = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Roles; };

    // Build order matters: the owner comes before the party group (which it leads) and the pulls (which spawn around
    // it); both check it. Rewards do not depend on each other's order: what several read (a seat's damage taken, the
    // owner's totals) is computed before any encounter's Reward.
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.Owner; }))
        _owner = add(std::make_unique<OwnerEncounter>(*this, envs));
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.PartyGroup; }))
        _party = add(std::make_unique<PartyEncounter>(*this, envs));
    // After the owner and the group: it moves both to the boss.
    if (_stage.AnyArena(hasInstance))
        instance = add(std::make_unique<InstanceEncounter>(*this, envs));
    if (_stage.AnyArena(hasPulls))
        pulls = add(std::make_unique<PullsEncounter>(*this, envs));
    if (_stage.AnyArena(hasCreature))
        creature = add(std::make_unique<CreatureEncounter>(*this, envs));
    Encounter* dummy = nullptr;
    if (_stage.AnyArena(hasDummy))
        dummy = add(std::make_unique<DummyEncounter>(*this, envs));
    // Nothing to fight and nothing to order: it only puts fire on the ground, so it can go anywhere in the order.
    Encounter* hazards = nullptr;
    if (_stage.AnyArena(hasHazards))
        hazards = add(std::make_unique<HazardEncounter>(*this, envs));
    // The party follow's leader: built in the owner's slot, nothing else to order against.
    if (_stage.AnyArena(hasPartyFollow))
        _partyFollow = add(std::make_unique<PartyFollowEncounter>(*this, envs));
    // The seek stage's hidden object: nothing to fight, nothing else to order against.
    Encounter* seek = nullptr;
    if (_stage.AnyArena(hasSeek))
        seek = add(std::make_unique<SeekEncounter>(*this, envs));
    // M1's object in the hallways: nothing to fight, nothing else to order against.
    Encounter* sight = nullptr;
    if (_stage.AnyArena(hasSight))
        sight = add(std::make_unique<SightEncounter>(*this, envs));
    // M3's doors, levers and named objects: nothing to fight, nothing else to order against.
    Encounter* interact = nullptr;
    if (_stage.AnyArena(hasInteract))
        interact = add(std::make_unique<InteractEncounter>(*this, envs));
    // The combat stages' creatures on a cleared dungeon: nothing else to order against.
    Encounter* combat = nullptr;
    if (_stage.AnyArena(hasCombat))
        combat = add(std::make_unique<CombatEncounter>(*this, envs));
    // G1's party drills on the same ground: after the party group (PartyEncounter), whose members it places.
    Encounter* roles = nullptr;
    if (_stage.AnyArena(hasRoles))
        roles = add(std::make_unique<RolesEncounter>(*this, envs));

    // The order episode info columns and reward terms are listed in. An encounter left out of this list still
    // runs -- it is only the columns and the terms that are missed -- which is how hazard_patches went missing
    // while the drill around it worked.
    for (Encounter* encounter : std::initializer_list<Encounter*>{ creature, dummy, pulls, instance,
        hazards, _owner, _party, _partyFollow, seek, sight,
        interact, combat, roles })
        if (encounter)
            _rewardOrder.push_back(encounter);

    // Which of them each arena uses, its share of episodes and its episode length.
    uint32 longestMs = settings.EpisodeSeconds * IN_MILLISECONDS;
    for (ArenaDefinition const& arena : _stage.Arenas)
    {
        auto const uses = [&](Encounter const* encounter)
        {
            return (encounter == _owner && arena.Owner)
                || (encounter == _party && arena.PartyGroup) || (encounter == pulls && hasPulls(arena))
                || (encounter == creature && hasCreature(arena))
                || (encounter == hazards && hasHazards(arena))
                || (encounter == instance && hasInstance(arena))
                || (encounter == dummy && hasDummy(arena))
                || (encounter == _partyFollow && hasPartyFollow(arena))
                || (encounter == seek && hasSeek(arena)) || (encounter == sight && hasSight(arena))
                || (encounter == interact && hasInteract(arena)) || (encounter == combat && hasCombat(arena))
                || (encounter == roles && hasRoles(arena));
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
        _arenaWeightsFinal.push_back(uint32(std::max(0, sConfigMgr->GetOption<int32>(
            Acore::StringFormat("{}Arena.{}.{}.WeightFinal", settings.TuningPrefix, _stage.Name, arena.Name),
            arena.WeightFinal >= 0 ? arena.WeightFinal : int32(_arenaWeights.back()), false))));

        _arenaMaxRung.push_back(sConfigMgr->GetOption<int32>(
            Acore::StringFormat("{}Arena.{}.{}.MaxRung", settings.TuningPrefix, _stage.Name, arena.Name),
            arena.MaxRung, false));
        // The "human" stand-in's share of the arena's training episodes (I7): its own, else StandIn.Share's (-1).
        _arenaStandInShare.push_back(std::clamp(sConfigMgr->GetOption<int32>(
            Acore::StringFormat("{}Arena.{}.{}.StandInShare", settings.TuningPrefix, _stage.Name, arena.Name),
            arena.StandInShare, false), -1, 100));

        uint32 const episodeMs = (arena.EpisodeSeconds ? arena.EpisodeSeconds : settings.EpisodeSeconds)
            * IN_MILLISECONDS;
        _arenaEpisodeMs.push_back(episodeMs);
        longestMs = std::max(longestMs, episodeMs);
    }

    // Where a sight stage's goal places come from in a dungeon: the stage's, or the conf's (0 seen and layout, 1 seen
    // only).
    _goalPlaces = sConfigMgr->GetOption<int32>(Acore::StringFormat("{}Stage.{}.GoalPlaces", settings.TuningPrefix,
        _stage.Name), int32(_stage.GoalPlaces), false) == int32(SeenPlaces::Source::SeenOnly)
        ? SeenPlaces::Source::SeenOnly : SeenPlaces::Source::SeenAndLayout;

    if (std::all_of(_arenaWeightsFinal.begin(), _arenaWeightsFinal.end(), [](uint32 weight) { return weight == 0; }))
        _arenaWeightsFinal = _arenaWeights;
    if (std::all_of(_arenaWeights.begin(), _arenaWeights.end(), [](uint32 weight) { return weight == 0; }))
    {
        LOG_ERROR("module.animus", "{}: every arena weight is 0; the arenas are drawn evenly", Name());
        std::fill(_arenaWeights.begin(), _arenaWeights.end(), 1);
    }

    _spec.LongestEpisodeSeconds = longestMs / IN_MILLISECONDS;

    // Load it at startup rather than on the first episode. A hazard stage fights nothing but still draws its
    // ground from the pool (OpponentPool::RandomHazardSpell), and without this the first episode of every env
    // paid for the load.
    if (_stage.AnyArena(hasCreature) || _stage.AnyArena(hasPulls) || _stage.AnyArena(hasHazards)
        || _stage.AnyArena(hasDummy))
        Opponents::OpponentPool::Instance();
    ConsumablePool::Instance();

    AddCoreEpisodeInfo();
    AddStandInEpisodeInfo();
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
    for (RewardTerm term : { RewardTerm::Repeat, RewardTerm::Jitter, RewardTerm::Aimless, RewardTerm::Effort,
        RewardTerm::Fidget, RewardTerm::SelfHealing, RewardTerm::GoalReached,
        RewardTerm::GoalSwitch, RewardTerm::Hazard, RewardTerm::HealingMana, RewardTerm::CombatClock })
        _info.Add("reward_" + std::string(RewardTermName(term)), [this, term](Env const& env, uint32 seat)
        {
            return Data(env).Seats[seat].Rewards.Episode(term);
        });
    if (_stage.Has(BlockId::Death))
        _info.Add("reward_" + std::string(RewardTermName(RewardTerm::DeathRun)), [this](Env const& env, uint32 seat)
        {
            return Data(env).Seats[seat].Rewards.Episode(RewardTerm::DeathRun);
        });
    // The width this episode's camera frames were cast at (AnimusForge.Vision.RenderSizes), before they were scaled
    // up to the canonical image: the learner can split its measures by render tier.
    if (_stage.Has(BlockId::Vision))
        _info.Add("vision_render_width", [this](Env const& env, uint32 seat)
        {
            return float(Data(env).Seats[seat].Look.Render.Width);
        });

    // The episode's score (RewardLedger::Score): its Outcome and Cost terms as tuned, before the rung's tier and the
    // role's scale. Evaluation, best.pt and the league are judged on it rather than on the return, so shaping can be
    // turned down without the yardstick moving with it.
    _info.Add("score_outcome", [this](Env const& env, uint32 agent)
    {
        if (agent < _seatCount)
            return Data(env).Seats[agent].Rewards.Score();

        return 0.0f;
    });

    _spec.EpisodeInfoDim = _info.Size();
    _spec.GoalCount = GOAL_JOINT_COUNT;

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
    uint32 const arena = Data(env).Arena;
    uint32 const arenaMap = arena < _stage.Arenas.size() ? _stage.Arenas[arena].MapId : 0;
    if (!_stage.MapId && !arenaMap)
        return none;

    // An arena that needs its own ground stands where it says, not where the env does; and a scored episode
    // stands on the control ground, which training never touches, so what the gates measure is whether the seat
    // can read terrain at all rather than whether it has seen this terrain before. An arena or a stage with no
    // control of its own falls back to the ground it trains on, and says so by being unable to tell the two apart.
    if (arena < _stage.Arenas.size() && !_stage.Arenas[arena].SpawnPoints.empty())
    {
        ArenaDefinition const& definition = _stage.Arenas[arena];
        if (env.Evaluating && !definition.HeldOutSpawnPoints.empty())
            return definition.HeldOutSpawnPoints;

        return definition.SpawnPoints;
    }

    // An arena on a map of its own has no use for the stage's ground (the check refuses one without its own).
    if (!_stage.MapId || (arenaMap && arenaMap != _stage.MapId))
        return none;

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

uint32 Animus::Curriculum::StageScenario::DrawArena(bool evaluating) const
{
    // An evaluation pinned to one arena (the learner's eval.heldout) plays only it.
    if (uint32 const pinned = _evaluationArena.load(std::memory_order_relaxed); evaluating && pinned)
        return pinned - 1;
    if (_arenaWeights.size() == 1 && !_stage.Arenas.front().EvalOnly)
        return 0;

    // Linear from Weight to WeightFinal over the stage's budget; an evaluation draws by the final weights, so it
    // measures what the stage is heading for. A held-out arena never (ArenaDrawWeights), and a pull drill in an
    // evaluation only in a stage of drills.
    std::vector<uint32> const weights = ArenaDrawWeights(_stage.Arenas, _arenaWeights, _arenaWeightsFinal, evaluating,
        _stageProgress.load(std::memory_order_relaxed));
    uint32 total = 0;
    for (uint32 weight : weights)
        total += weight;
    // Nothing to draw (every weight set to 0 by hand): the first arena the stage trains on -- never a held-out one.
    if (!total)
    {
        for (uint32 arena = 0; arena < _stage.Arenas.size(); ++arena)
            if (!_stage.Arenas[arena].EvalOnly)
                return arena;
        return 0;
    }

    uint32 roll = urand(0, total - 1);
    for (uint32 arena = 0; arena < weights.size(); ++arena)
    {
        if (roll < weights[arena])
            return arena;
        roll -= weights[arena];
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
    // Throughput where it counts: damage per second in combat, and that over the level's damage scale, so a level 80
    // and a level 20 read on one scale (plan 1.9).
    _info.Add("combat_dps", [seat](Env const& env, uint32 index)
    {
        float const seconds = std::max(1.0f, float(seat(env, index).CombatMs) / 1000.0f);
        return float(env.EpisodeStats[index].Damage) / seconds;
    });
    _info.Add("dps_scaled", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        float const seconds = std::max(1.0f, float(state.CombatMs) / 1000.0f);
        return float(env.EpisodeStats[index].Damage) / seconds / std::max(1.0f, state.DamageScale);
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
    // Leaving the ground (the controller's jumps and falls, landed by the server): jumps taken, drops (a landing two
    // yards or more below where the fall began), whether the seat fell, what the landings cost in health (the
    // server's own HandleFall) and whether one killed it. A drill about ledges reads these; every stage gets them.
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
    _info.Add("drops", [seat](Env const& env, uint32 index) { return float(seat(env, index).Drops); });
    _info.Add("fell", [seat](Env const& env, uint32 index) { return seat(env, index).Falls ? 1.0f : 0.0f; });
    _info.Add("fall_damage", [seat](Env const& env, uint32 index) { return seat(env, index).FallDamage; });
    _info.Add("fall_deaths", [seat](Env const& env, uint32 index) { return float(seat(env, index).FallDeaths); });
    // Of the fall deaths, the core's kill under the map's floor; and ticks that ended inside the terrain from above it
    // (the controller's own rule forbids it: any is a bug to report).
    _info.Add("void_deaths", [seat](Env const& env, uint32 index) { return float(seat(env, index).VoidDeaths); });
    _info.Add("into_terrain", [seat](Env const& env, uint32 index) { return float(seat(env, index).IntoTerrain); });
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
    // A party seat left empty this episode reports 0: ignore its row. So does the "human" stand-in's seat
    // (StandIn.h): a frozen partner's, not the policy's, so no class's episode and nothing the learner scores.
    _info.Add("present", [this, seat](Env const& env, uint32 index)
    {
        return seat(env, index).L && Data(env).StandInPlay.Seat != int32(index) ? 1.0f : 0.0f;
    });
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
    // An episode that could not be built ends at once and is rebuilt: 1 on that episode's row. How often a stage's
    // resets fail -- a quest the bot refuses, a spot with no objective -- is what it trains on less than it seems.
    _info.Add("build_failed", [this](Env const& env, uint32) { return Data(env).BuildFailed ? 1.0f : 0.0f; });
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
    // The share of the opponent's health stealth openers took in their first seconds (Stealth.OpenerDamage).
    _info.Add("opener_damage", [tally](Env const& env, uint32 index)
    {
        return tally(env, index).OpenerDamage;
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

    // The corpse run, where death runs on.
    if (_stage.Has(BlockId::Death))
    {
        auto const run = [seat](Env const& env, uint32 index) -> SeatState::DeathRunTally const&
        {
            return seat(env, index).DeathRun;
        };
        _info.Add("open_world_deaths", [run](Env const& env, uint32 index) { return float(run(env, index).Deaths); });
        _info.Add("releases", [run](Env const& env, uint32 index) { return float(run(env, index).Releases); });
        _info.Add("corpse_runs_completed", [run](Env const& env, uint32 index)
        {
            return float(run(env, index).CorpseRises);
        });
        _info.Add("seconds_dead", [run](Env const& env, uint32 index)
        {
            return float(run(env, index).DeadMs) / 1000.0f;
        });
        _info.Add("safe_rises", [run](Env const& env, uint32 index) { return float(run(env, index).SafeRises); });
        _info.Add("died_again", [run](Env const& env, uint32 index) { return float(run(env, index).DiedAgain); });
        _info.Add("spirit_healer_uses", [run](Env const& env, uint32 index)
        {
            return float(run(env, index).SpiritHealer);
        });
        _info.Add("resurrections_accepted", [run](Env const& env, uint32 index)
        {
            return float(run(env, index).Accepted);
        });
    }

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
    // Intent (StageScenario::JudgePress): the presses judged against the goal and how many served it, the aimless
    // ones, every press charged effort, how often the feet start, the seconds spent shuffling at range, and the
    // rate of presses inside a fight (players: roughly 30-70 a minute).
    _info.Add("serving_share", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return state.JudgedPresses ? float(state.ServingPresses) / float(state.JudgedPresses) : 0.0f;
    });
    _info.Add("aimless_presses", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).AimlessPresses);
    });
    // The aimless presses by cause (AimlessCause), and the mode changes: what each price is charging.
    // A sight stage's refused presses only where there is a sight block: the other stages' tables are as they were.
    for (size_t cause = 0; cause < AIMLESS_CAUSES; ++cause)
        if (AimlessCause(cause) != AimlessCause::ActRefused || _stage.Has(BlockId::Sight))
            _info.Add(std::string("aimless_") + AimlessCauseName(AimlessCause(cause)), [seat, cause](Env const& env,
                uint32 index) { return float(seat(env, index).AimlessBy[cause]); });
    // A sight stage's refused presses by why (EntityActions::Refusal): what the act_refused price is charging.
    if (_stage.Has(BlockId::Sight))
        for (uint32 refusal = 1; refusal < EntityActions::REFUSALS; ++refusal)
            _info.Add(std::string("act_refused_") + EntityActions::RefusalName(EntityActions::Refusal(refusal)),
                [seat, refusal](Env const& env, uint32 index)
                {
                    return float(seat(env, index).ActRefusedBy[refusal]);
                });
    // Of the goals chosen of each kind, the share reached: which kinds the seat can actually finish.
    for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
        _info.Add(std::string("goal_success_") + std::string(GoalName(SeatGoal(kind))), [seat, kind](Env const& env,
            uint32 index)
        {
            SeatState const& state = seat(env, index);
            return state.GoalsChosenBy[kind] ? float(state.GoalsReachedBy[kind]) / float(state.GoalsChosenBy[kind])
                : 0.0f;
        });
    _info.Add("mode_switches", [seat](Env const& env, uint32 index) { return float(seat(env, index).ModeSwitches); });
    _info.Add("effort_presses", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).EffortPresses);
    });
    _info.Add("move_starts_per_minute", [seat](Env const& env, uint32 index)
    {
        float const minutes = std::max(0.001f, float(env.EpisodeElapsedMs) / 60000.0f);
        return float(seat(env, index).MoveStarts) / minutes;
    });
    // A stop followed by a new start within a second, per minute (movement-smooth): the body stopping and starting
    // again, the stutter a player never shows.
    _info.Add("move_stop_starts", [seat](Env const& env, uint32 index)
    {
        float const minutes = std::max(0.001f, float(env.EpisodeElapsedMs) / 60000.0f);
        return float(seat(env, index).MoveStopStarts) / minutes;
    });
    _info.Add("fidget_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).FidgetMs) / 1000.0f;
    });
    _info.Add("combat_actions_per_minute", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return state.CombatMs ? float(state.CombatPresses) / (float(state.CombatMs) / 60000.0f) : 0.0f;
    });
    // Presses of an action past the free ones in its window (Tuning().Actions.Repeat).
    _info.Add("repeated_presses", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).RepeatedPresses);
    });
    // Steering that did not commit (Actions.Jitter, MoveControls::Press): turn rates reversed within the window, and
    // the feet reversed within it (forward to back, left to right; bearing_flips).
    _info.Add("turn_reversals", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).TurnReversals);
    });
    _info.Add("bearing_flips", [seat](Env const& env, uint32 index)
    {
        return seat(env, index).BearingFlips;
    });
    _info.Add("pitch_reversals", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).PitchReversals);
    });
    // Weaves: turn or pitch rates, climbs or the feet reversed 1.5 to 4 s on, the slow wobble the decay prices and the
    // old window let through (movement-smooth C).
    _info.Add("weaves", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).Weaves);
    });
    // The player controller (player-controller C4): reports the server refused, time pressing into a wall and time
    // stuck with a key held (no motion for a second or more), the course turning more than 20 degrees within a tick
    // and control changes, per minute; and the movement packets a seat sent, per minute (the client's cadence).
    _info.Add("moves_refused", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        return float(state.Mover.Counts.Refused - state.MoverAtStart.Refused);
    });
    _info.Add("wall_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).WallMs) / 1000.0f;
    });
    _info.Add("stuck_seconds", [seat](Env const& env, uint32 index)
    {
        return float(seat(env, index).StuckMs) / 1000.0f;
    });
    _info.Add("course_kinks", [seat](Env const& env, uint32 index)
    {
        float const minutes = std::max(0.001f, float(env.EpisodeElapsedMs) / 60000.0f);
        return float(seat(env, index).CourseKinks) / minutes;
    });
    _info.Add("control_changes_per_minute", [seat](Env const& env, uint32 index)
    {
        float const minutes = std::max(0.001f, float(env.EpisodeElapsedMs) / 60000.0f);
        return float(seat(env, index).ControlChanges) / minutes;
    });
    _info.Add("move_reports_per_minute", [seat](Env const& env, uint32 index)
    {
        SeatState const& state = seat(env, index);
        float const minutes = std::max(0.001f, float(env.EpisodeElapsedMs) / 60000.0f);
        return float(state.Mover.Counts.Reports - state.MoverAtStart.Reports) / minutes;
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
    _info.Add("goals_reached", [seat](Env const& env, uint32 index) { return float(seat(env, index).GoalsReached); });
    _info.Add("goals_lost", [seat](Env const& env, uint32 index) { return float(seat(env, index).GoalsLost); });
    _info.Add("goal_targeted_share", [seat, goalDecisions](Env const& env, uint32 index)
    {
        float const total = goalDecisions(env, index);
        return total ? float(seat(env, index).GoalTargetedDecisions) / total : 0.0f;
    });
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
        // The seat plan, so the learner can tell a party's arena from a solo seat's.
        entry["plan"] = definition.Seats == SeatPlan::Solo ? "solo" : "party";
        entry["eval_only"] = definition.EvalOnly;
        // The party stages' arenas (G2, D1-D3): a pull drill, a corridor's length, and the stand-in's share of the
        // training runs.
        entry["pull_drill"] = definition.PullDrill;
        entry["corridor_packs"] = definition.CorridorPacks;
        entry["stand_in_share"] = StandInShare(uint32(arena));
        // The seat a drill is about (ArenaDefinition::DrillRole: seat 0), which the learner's co-op partners never
        // play (animus.partners); -1 for an arena that drills no one.
        entry["drill_seat"] = definition.DrillRole ? 0 : -1;
    }

    // Agents the sim declares for a frozen checkpoint to play: the owner, where an arena casts it.
    boost::json::array& cast = stageFile["cast"].emplace_array();
    if (_castOwner)
    {
        boost::json::object& entry = cast.emplace_back(boost::json::object()).get_object();
        entry["agent"] = OwnerAgent();
        // The learner's cast.agents names it: the party follow's leader, or the owner.
        entry["name"] = _partyFollow ? "leader" : "owner";
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

    auto const layoutName = [](Layout const& layout) { return layout.Profile->Name; };

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
        if (layout.Profile)
            for (SpecProfile const& spec : layout.Profile->Specs)
                specNames.push_back(boost::json::string(spec.Name));
        // And the role each is drawn into a party's seat for (FitsDungeonRole: what it is geared for), so the learner
        // can grade a build against the others of its role -- a healer on keeping its group up, not on its party's
        // score, which a holy paladin casting two heals a fight shared with the seats that carried it (2026-10-04).
        boost::json::array& specRoles = entry["spec_roles"].emplace_array();
        if (layout.Profile)
            for (SpecProfile const& spec : layout.Profile->Specs)
                specRoles.push_back(boost::json::string(spec.Stats == StatProfile::Tank ? "tank"
                    : spec.Stats == StatProfile::Healer ? "healer" : "damage"));

        // Its entities as sets, for the learner's set encoders and pointer heads (mappo.seat_sets; DescribeSeatSets).
        DescribeSeatSets(layout, entry["sets"].emplace_array());

        boost::json::array& spans = entry["blocks"].emplace_array();
        for (BlockId id : layout.Blocks)
        {
            BlockSlice const& slice = layout.Slice(id);
            boost::json::object& block = spans.emplace_back(boost::json::object()).get_object();
            block["name"] = BlockName(id);
            block["obs"] = Span(slice.ObsFirst, slice.ObsCount);
            block["actions"] = Span(slice.ActionFirst, slice.ActionCount);
            if (uint32 const revision = GetBlock(id).Revision())
                block["revision"] = revision;
            if (id == BlockId::Core)
                block["action_features"] = CoreBlock::ACTION_FEATURES;
            // The camera's image, as the layout's manifest describes it: the learner gives a layout whose vision block
            // has one its image encoder (camera-vision).
            if (id == BlockId::Vision)
            {
                boost::json::object manifest;
                GetBlock(id).DescribeManifest(layout, manifest);
                block["image"] = manifest["image"];
                block["camera"] = manifest["camera"];
                block["look"] = manifest["look"];
            }
            // The entity list, as a set the learner reads beside the camera (perception-goals 1b).
            if (id == BlockId::Entities)
            {
                boost::json::object manifest;
                GetBlock(id).DescribeManifest(layout, manifest);
                block["entities"] = manifest["entities"];
            }
            // The mental map's crop: its shape and channels, for the learner's map encoder (REDESIGN §3).
            if (id == BlockId::Map)
            {
                boost::json::object manifest;
                GetBlock(id).DescribeManifest(layout, manifest);
                block["map"] = manifest["map"];
            }
            // The seen and remembered list and its pointer presses (dungeon-curriculum I1, I2), for the learner's
            // sight encoder beside the camera's entity list.
            if (id == BlockId::Sight)
            {
                boost::json::object manifest;
                GetBlock(id).DescribeManifest(layout, manifest);
                block["sight"] = manifest["sight"];
            }
            // Its columns by name, where the block names them: a seed follows a column that moved (bootstrap).
            boost::json::array names;
            GetBlock(id).DescribeColumns(layout, names);
            if (!names.empty())
                block["obs_names"] = std::move(names);
            boost::json::array rescaled;
            GetBlock(id).DescribeRescaled(layout, rescaled);
            if (!rescaled.empty())
                block["rescaled"] = std::move(rescaled);
        }
    }

    boost::json::array& episodeInfo = stageFile["episode_info"].emplace_array();
    for (std::string const& name : _info.Names())
        episodeInfo.push_back(boost::json::string(name));

    // Episode info columns that index a list of names (the seek stage's room and object): the learner's evaluation
    // tables split by them (animus.evaluation, EvalResult.categories).
    boost::json::object& categories = stageFile["episode_categories"].emplace_object();
    // A pull drill's pack (drill_pack): the route's packs, pack_1 first, as many as a route's cells can name.
    if (_stage.AnyArena([](ArenaDefinition const& arena) { return arena.PullDrill; }))
    {
        boost::json::array packs;
        for (uint32 pack = 1; pack <= InstanceEncounter::EXPLORE_PACKS; ++pack)
            packs.emplace_back(Acore::StringFormat("pack_{}", pack));
        categories["drill_pack"] = std::move(packs);
    }
    for (ArenaDefinition const& arena : _stage.Arenas)
        if (arena.Against == Opposition::Seek)
        {
            boost::json::array rooms;
            for (std::string const& name : SeekEncounter::RoomNames(arena))
                rooms.emplace_back(name);
            boost::json::array objects;
            for (std::string const& name : SeekEncounter::ObjectNames(arena))
                objects.emplace_back(name);
            categories["seek_room"] = std::move(rooms);
            categories["seek_object"] = std::move(objects);
        }
        else if (arena.Against == Opposition::Interact)
        {
            // M3's sites and named objects (InteractEncounter): the evaluation's right object by each.
            boost::json::array sites;
            for (std::string const& name : InteractEncounter::SiteNames(arena))
                sites.emplace_back(name);
            boost::json::array objects;
            for (std::string const& name : InteractEncounter::ObjectNames(arena))
                objects.emplace_back(name);
            categories["interact_site"] = std::move(sites);
            categories["interact_object"] = std::move(objects);
        }
        else if (arena.Against == Opposition::Sight)
        {
            // M1's object (SightEncounter): the evaluation's arrival by object.
            boost::json::array objects;
            for (std::string const& name : SightEncounter::ObjectNames(arena))
                objects.emplace_back(name);
            categories["sight_object"] = std::move(objects);
            // ... and by where it stood (episode info objective_corner): every measure in sight and round a corner.
            categories["objective_corner"] = boost::json::array{ "in_sight", "corner" };
        }

    // Every term's category (RewardTermCategory), so the learner's reward audit reads what the sim pays rather than
    // a list of names kept by hand beside it.
    {
        boost::json::object categories;
        for (std::size_t term = 0; term < REWARD_TERM_COUNT; ++term)
        {
            RewardCategory const category = RewardTermCategory(RewardTerm(term));
            categories[RewardTermName(RewardTerm(term))] = category == RewardCategory::Outcome ? "outcome"
                : category == RewardCategory::Cost ? "cost" : "shaping";
        }
        stageFile["reward_terms"] = std::move(categories);
    }

    // The goal space (Component C): a goal is kind * targets + target. The learner masks its goal head with the
    // goal block's columns (the last block of every layout: kinds, then targets, then "ended") and the table of
    // which targets each kind accepts.
    {
        // Built apart and moved in: a reference into an object's value is invalidated by the next key inserted
        // into that object, so two arrays filled side by side cannot both be references into `goals`.
        boost::json::array kinds;
        boost::json::array accepts;
        for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
        {
            kinds.emplace_back(std::string(GoalName(SeatGoal(kind))));
            boost::json::array row;
            for (uint32 target = 0; target < GOAL_TARGETS; ++target)
                row.emplace_back(GoalAccepts(SeatGoal(kind), target) ? 1 : 0);
            accepts.emplace_back(std::move(row));
        }
        boost::json::object& goals = stageFile["goals"].emplace_object();
        goals["kinds"] = std::move(kinds);
        goals["accepts"] = std::move(accepts);
        goals["targets"] = GOAL_TARGETS;
        goals["block"] = "goal";
        // Where the next-run columns sit in the block, from its first column: the secondary ending, the event, the
        // commanded primary (a flag, then kind and target one-hots) and what was achieved (kind and target), and
        // how many goals ACT carries a seat.
        boost::json::object columns;
        columns["secondary_ended"] = uint32(GoalBlock::OBS_SECONDARY_ENDED);
        columns["event"] = uint32(GoalBlock::OBS_EVENT);
        columns["from_order"] = uint32(GoalBlock::OBS_FROM_ORDER);
        columns["order_kind"] = uint32(GoalBlock::OBS_ORDER_KIND_FIRST);
        columns["order_target"] = uint32(GoalBlock::OBS_ORDER_TARGET_FIRST);
        columns["achieved_kind"] = uint32(GoalBlock::OBS_ACHIEVED_KIND_FIRST);
        columns["achieved_target"] = uint32(GoalBlock::OBS_ACHIEVED_TARGET_FIRST);
        columns["width"] = uint32(GoalBlock::OBS_COUNT);
        goals["columns"] = std::move(columns);
        goals["slots_on_wire"] = GOAL_SLOTS_ON_WIRE;
    }
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
    return seat < MAX_SEATS ? _data[env.Index].Seats[seat].Bot.Active() : nullptr;
}

Player* Animus::Curriculum::StageScenario::SeatBotInWorld(Env const& env, uint32 seat) const
{
    Player* bot = SeatBot(env, seat);
    return bot && bot->IsInWorld() ? bot : nullptr;
}

Player* Animus::Curriculum::StageScenario::Owner(Env const& env) const
{
    return _owner && Arena(env).Owner ? _owner->Find(env) : nullptr;
}

bool Animus::Curriculum::StageScenario::CastOwnerActive(Env const& env) const
{
    if (!_castOwner)
        return false;
    // An owner is always its own row, an evaluation's too; the party follow's leader keeps its script in one.
    ArenaDefinition const& arena = Arena(env);
    if (arena.OwnerCast && _owner && _owner->IsCast(env))
        return true;
    return !env.Evaluating && arena.Against == Opposition::PartyFollow && _partyFollow && _partyFollow->IsCast(env);
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
            for (uint8 spec : ClassAssets::For(*layout.Profile).SpecsMeeting(demand))
                castings.push_back({ &layout, spec });
        }

    // Nothing in the run can do it, or nothing was asked for: every class with every build it has. The pairs, not
    // the classes, because a class with a build that holds a pull and one that heals is two things to be.
    if (castings.empty())
        for (Layout const& layout : _layouts)
        {
            for (uint8 spec = 0; spec < uint8(layout.Profile->Specs.size()); ++spec)
                castings.push_back({ &layout, spec });
        }

    return castings;
}

bool Animus::Curriculum::StageScenario::FitsDungeonRole(Casting const& casting, uint8 role)
{
    if (role == DUNGEON_ANY || !casting.L || !casting.L->Profile || casting.Spec >= casting.L->Profile->Specs.size())
        return true;
    // What the spec is geared for: a level-80 aptitude read a restoration shaman with a shield as somebody who holds
    // a pull, and drew parties of two "tanks" and no healer at 17-20 (2026-10-01).
    StatProfile const stats = casting.L->Profile->Specs[casting.Spec].Stats;
    bool const holds = stats == StatProfile::Tank;
    bool const heals = stats == StatProfile::Healer;
    switch (role)
    {
        case DUNGEON_TANK:
            return holds;
        case DUNGEON_HEALER:
            return heals && !holds;
        case DUNGEON_DAMAGE:
            return !holds && !heals;
        default:
            return true;
    }
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
    //
    if (env.EpisodeSeedIndex != NO_EPISODE_SEED)
    {
        std::size_t const count = castings.size();
        std::size_t const index = env.EpisodeSeedIndex;
        return castings[(index + seat) % count];
    }

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

void Animus::Curriculum::StageScenario::NoteWingRun(uint32 rung, bool probe, float progress)
{
    // The ladder moves only on what the policy does alone (2026-10-01: "taper off only based on the progress made by
    // the learner"): the probes, against a fixed target (Instance.WingRungTarget) -- the rung's other runs, as the
    // reference, crept up from 0.71 to 0.82 on rung 0 and took the target with them. It never steps back on a score
    // (WingLadder); its collapse alarm is the host's, here.
    std::lock_guard<std::mutex> guard(_wingLadderLock);
    uint32 const now = _wingLadder.Rung();
    progress = std::clamp(progress, 0.0f, 1.0f);
    // A worker reports its runs to the host, whose ladder is the cluster's.
    if (_wingFollower)
    {
        if (rung != now)
            return;
        if (_wingTallyRung != rung)
        {
            _wingTallyProbes.clear();
            _wingTallyOthers.clear();
            _wingTallyRung = rung;
        }
        std::string& tally = probe ? _wingTallyProbes : _wingTallyOthers;
        tally += Acore::StringFormat("{}{:.3f}", tally.empty() ? "" : ",", progress);
        return;
    }

    WingLadder::Result const result = _wingLadder.Note(rung, probe, progress);
    if (result.Moved)
        LOG_INFO("module.animus", "{}: the dungeon ladder steps down from rung {} to {}: the last {} probes made {:.2f} "
            "of the dungeon (target {:.2f}; the rung's other runs {:.2f})", Name(), result.Moved->From,
            result.Moved->To, std::max<uint32>(1, _tuning.Instance.WingRungRuns), result.Moved->Probes,
            _tuning.Instance.WingRungTarget, result.Moved->Others);
    if (result.Alarm)
        LOG_WARN("module.animus", "{}: {}", Name(), *result.Alarm);
}

std::string Animus::Curriculum::StageScenario::TakeClusterTally()
{
    std::lock_guard<std::mutex> guard(_wingLadderLock);
    if (!_wingFollower || (_wingTallyProbes.empty() && _wingTallyOthers.empty()))
        return {};
    std::string tally = Acore::StringFormat("{}/{}/{}", _wingTallyRung,
        _wingTallyProbes.empty() ? "-" : _wingTallyProbes, _wingTallyOthers.empty() ? "-" : _wingTallyOthers);
    _wingTallyProbes.clear();
    _wingTallyOthers.clear();
    return tally;
}

void Animus::Curriculum::StageScenario::AddClusterTally(std::string const& tally)
{
    // "rung/probes/others": each a comma-separated list of runs' progress, or "-".
    std::size_t const first = tally.find('/');
    std::size_t const second = first == std::string::npos ? std::string::npos : tally.find('/', first + 1);
    if (second == std::string::npos)
        return;
    uint32 const rung = uint32(std::strtoul(tally.substr(0, first).c_str(), nullptr, 10));
    auto const each = [&](std::string const& list, bool probe)
    {
        if (list == "-")
            return;
        std::size_t at = 0;
        while (at < list.size())
        {
            std::size_t const end = std::min(list.find(',', at), list.size());
            NoteWingRun(rung, probe, std::strtof(list.substr(at, end - at).c_str(), nullptr));
            at = end + 1;
        }
    };
    each(tally.substr(second + 1), false);
    each(tally.substr(first + 1, second - first - 1), true);
}

void Animus::Curriculum::StageScenario::FollowClusterRung(uint32 rung)
{
    std::lock_guard<std::mutex> guard(_wingLadderLock);
    _wingFollower = true;
    uint32 const next = std::min<uint32>(rung, uint32(WING_RUNGS.size()) - 1);
    if (next != _wingLadder.Rung())
        LOG_INFO("module.animus", "{}: the host puts the dungeon ladder on rung {}", Name(), next);
    _wingLadder.Follow(next);
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

bool Animus::Curriculum::StageScenario::Setup(Env& env)
{
    if (_layouts.empty())
    {
        LOG_ERROR("module.animus", "{}: no class/role to play (check the host's class/role list{})", Name(),
            _stage.NeedsStealth ? ", and this stage is played only by class/roles whose kit has stealth" : "");
        return false;
    }

    // As in training (Reset): a failed build draws again -- another arena, another spawn point -- and only a stage
    // that keeps failing is given up on.
    bool const built = RetryBuild(SETUP_BUILD_ATTEMPTS, [&] { return Rebuild(env); }, [&](uint32 attempt)
    {
        LOG_ERROR("module.animus", "{}: env {} could not build its first episode (try {} of {}); drawing again",
            Name(), env.Index, attempt + 1, SETUP_BUILD_ATTEMPTS);
    });
    if (!built)
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
    data.Arena = DrawArena(env.Evaluating);
    // No stand-in until the seats are built and DrawStandIn says so (a build that fails leaves none).
    data.StandInPlay = EnvState::StandInSeat();
    // An arena on a map of its own (ArenaDefinition::MapId) sends the episode there; an encounter that fixes its
    // own map (an instance rung, a quest giver's) still decides later, in BeforeLevel.
    data.EpisodeMapId = _stage.Arenas[data.Arena].MapId;
    data.HasEpisodeMap = data.EpisodeMapId != 0;
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
    // Each seat's camera draws the size it casts this episode's frames at (AnimusForge.Vision.RenderSizes), from the
    // world thread's random numbers as the rest of the reset does, by the sizes' weights; one size draws nothing, and
    // a stage without a camera draws nothing at all, so its random numbers are what they were.
    if (_stage.Has(BlockId::Vision))
        for (SeatState& seat : data.Seats)
            seat.Look.Render = Vision::DrawRenderSize(Vision::Current(), [](float total)
            {
                return std::min(frand(0.0f, total), std::nextafter(total, 0.0f));
            });
    // Each seat's mental map: kept across this reset KeepShare of the time in training, older by a random offset
    // (amendment 1); never in an evaluation, which starts every seat's map empty so its scores compare. Applied at the
    // episode's first observation, once the seat's instance is known (ObserveSeat). Its entity memory (a sight block,
    // dungeon-curriculum I2) lives by the same roll: a shipped bot keeps both together.
    if (_stage.Has(BlockId::Map) || _stage.Has(BlockId::Sight))
    {
        Vision::MapRunSettings const& maps = Vision::MapCurrent();
        for (SeatState& seat : data.Seats)
        {
            seat.MapPending = true;
            seat.MapKept = false;
            seat.MapKeep = !env.Evaluating && maps.KeepShare > 0.0f && frand(0.0f, 1.0f) < maps.KeepShare;
            seat.MapAgeOffset = seat.MapKeep ? frand(0.0f, std::max(0.0f, maps.AgeOffsetSeconds)) : 0.0f;
        }
    }
    // No resurrection offer is in flight into a new episode, and the clock it was taken on has restarted.
    data.ResurrectBy.fill(NO_SEAT);
    data.ResurrectMs.fill(0);
    for (auto const& encounter : _encounters)
        encounter->ResetEpisode(env);

    // What the last episode's arena had and this one does not (an owner, a group, an enemy player) goes first.
    for (Encounter* encounter : previousEncounters)
        if (!Uses(env, *encounter))
            encounter->Deactivate(env);

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->BeforeRebuild(env);

    // The creatures still in the enemy slots, despawned once the new seats are in: read after the encounters have
    // cleared their own: a creature despawned by an encounter and again here is a summon queued for removal twice,
    // and map threads tripping over it later (Map.cpp:682's PendingAdd assert, a freed TempSummon in Creature::Update).
    std::vector<Creature*> oldTargets;
    for (uint32 target = 0; target < env.Targets.size(); ++target)
        if (Creature* creature = env.FindTarget(target))
            oldTargets.push_back(creature);

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
        // An instance is run by a full group with somebody to hold the pull and somebody to keep them up: a heroic
        // attempted by two or three was lost before it started, and the dungeon stage fielded a full five 40% of the
        // time. Companions in the open world keep the random size (1-4, Party.SizeWeight*).
        bool const instance = arena.Against == Opposition::Instance && arena.Seats == SeatPlan::Party;
        bool const proper = (instance && arena.Instance == InstanceLadder::Wing) || arena.ProperParty;
        // A party of a fixed size (ArenaDefinition::PartySize: the party follow's followers) keeps it.
        if (arena.Seats == SeatPlan::Party && !instance && !arena.ProperParty && !arena.PartySize)
            data.ActiveSeats = RandomPartySize(_tuning.Party);

        // Some parties are the classic makeup (somebody to hold the pull, somebody to keep the hurt one up, and no
        // demand on the rest, in a random order); the others draw every seat's demand on its own. The makeup is
        // built for the seats actually in play: a four-entry array left the other MAX_SEATS - 4 zero-filled, which
        // a raid would have shuffled into the group that got them.
        std::array<AptitudeDemand, MAX_SEATS> demands = ClassicDemands(data.ActiveSeats);
        // A drill puts the drilled role in seat 0 (its class and build climbs the pack ladder); the classic makeup
        // has the tank there and the healer in seat 1.
        if (arena.DrillRole == DUNGEON_HEALER && data.ActiveSeats > 1)
            std::swap(demands[0], demands[1]);
        else if (arena.DrillRole == DUNGEON_DAMAGE && data.ActiveSeats > 2)
            std::swap(demands[0], demands[2]);
        // A drill keeps the classic makeup as it was placed: one tank, one healer, the drilled role in seat 0. It fell
        // through to the drawn roles below, so a drill party had no tank one time in four and two or more one time in
        // three, and its seat 0 was a healer or a mage as often as the role it drilled (2026-10-03, stage6).
        bool const classic = arena.DrillRole || instance || proper || roll_chance_i(_tuning.Party.ClassicChance);
        if (classic && !arena.DrillRole)
            std::shuffle(demands.begin(), demands.begin() + data.ActiveSeats, RandomEngine::Instance());
        else if (!classic)
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
            data.Seats[seat].DungeonRole = DUNGEON_ANY;
        }

        // A whole dungeon: a tank, a healer and three damage dealers, whichever seats they are (the demands were
        // shuffled), each drawn among the castings that fit its place; the level redraw below keeps to them too.
        // Every casting of the stage, taken once for the whole party rather than once per seat.
        std::vector<Casting> const everyCasting = proper ? Castings(AptitudeDemand::Anything())
            : std::vector<Casting>();
        if (proper)
            for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            {
                SeatState& s = data.Seats[seat];
                s.DungeonRole = s.Want.Feature == AptitudeDemand::HoldsThePull().Feature ? DUNGEON_TANK
                    : s.Want.Feature == AptitudeDemand::KeepsThemUp().Feature ? DUNGEON_HEALER : DUNGEON_DAMAGE;
                // A stage trained wholly in a band (FocusChance 100) draws only castings that can be its level: a death
                // knight lifted every seat to 55.
                uint8 const capLevel = _stage.FocusChance >= 100 ? _stage.FocusLevelLast : 0;
                std::vector<Casting> fits;
                // By spec alone: a demand's measured aptitude never let a bear druid hold the pull, so no party drew
                // one (2026-10-02, stage6: 1,920 seats, no feral bear).
                for (Casting const& casting : everyCasting)
                    if (FitsDungeonRole(casting, s.DungeonRole)
                        && (!capLevel || (casting.L && casting.L->Assets->Kit->MinLevel() <= capLevel)))
                        fits.push_back(casting);
                if (fits.empty())
                    continue;
                // Training draws by the learner's weights (the pairs furthest below their baseline get more), as
                // DrawCasting does: drawn evenly, a drill's weakest build -- the holy paladin, healing two heals a
                // fight -- got a fifth of the healer drill like the builds that had learned it (2026-10-03).
                std::size_t pick = 0;
                if (env.EpisodeSeedIndex != NO_EPISODE_SEED)
                    pick = (std::size_t(env.EpisodeSeedIndex) + seat) % fits.size();
                else
                {
                    float total = 0.0f;
                    for (Casting const& casting : fits)
                        total += Weight(*casting.L, casting.Spec);
                    pick = urand(0, uint32(fits.size()) - 1);
                    if (total > 0.0f)
                    {
                        float roll = frand(0.0f, total);
                        for (std::size_t index = 0; index < fits.size(); ++index)
                        {
                            roll -= Weight(*fits[index].L, fits[index].Spec);
                            pick = index;
                            if (roll <= 0.0f)
                                break;
                        }
                    }
                }
                s.L = fits[pick].L;
                s.Spec = fits[pick].Spec;
            }
    }
    else
    {
        // Any class with any of its builds: drawn (evenly, or by the learner's weights), or spread over the seeds
        // in an evaluation. In training a seat whose character can still be kept keeps its class and build
        // (Characters.KeepCasting), so the reuse below keeps the character.
        bool const keep = _tuning.Characters.KeepCasting && _tuning.Characters.ReuseEpisodes > 0 && !firstBuild
            && !env.Evaluating && env.EpisodeSeedIndex == NO_EPISODE_SEED;
        for (uint32 seat = 0; seat < _seatCount; ++seat)
        {
            SeatState const& kept = data.Seats[seat];
            if (keep && seat < data.ActiveSeats && seat < previousActiveSeats && kept.L && kept.Bot.Active()
                && kept.EpisodesPlayed < _tuning.Characters.ReuseEpisodes)
            {
                data.Seats[seat].Want = AptitudeDemand::Anything();
                continue;
            }

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

    // A level fixed that low (an instance rung below 55) is not every class's: the seats draw again among the
    // classes that can be it. A death knight among them lifted every seat to 55, so the Ragefire Chasm, Deadmines
    // and Scarlet Monastery rungs were fought twenty levels over. Seat 0 once kept its class (the rung was drawn for
    // it), and a death knight there put 54% of the Deadmines runs at 55 (2026-09-30): a dungeon's seats are all
    // characters of its level range now, seat 0 included.
    // The party follow (M4) runs a dungeon at its level band too, and a death knight there would lift it to 55.
    bool const dungeonLevel = (arena.Against == Opposition::Instance && arena.Instance == InstanceLadder::Wing)
        || arena.Against == Opposition::PartyFollow;
    if (data.EpisodeLevel)
        for (uint32 seat = dungeonLevel ? 0 : 1; seat < data.ActiveSeats; ++seat)
        {
            // Among the castings that can be the level only: drawn again, an evaluation's seeded spread handed back
            // the same death knight every time, and half the Deadmines evaluation was fought at 55 (2026-09-30).
            SeatState& s = data.Seats[seat];
            if (!s.L || s.L->Assets->Kit->MinLevel() <= data.EpisodeLevel)
                continue;
            std::vector<Casting> fits;
            AptitudeDemand const demand = s.DungeonRole != DUNGEON_ANY ? AptitudeDemand::Anything() : s.Want;
            for (Casting const& casting : Castings(demand))
                if (casting.L && casting.L->Assets->Kit->MinLevel() <= data.EpisodeLevel
                    && FitsDungeonRole(casting, s.DungeonRole))
                    fits.push_back(casting);
            if (fits.empty())
                continue;
            std::size_t const pick = env.EpisodeSeedIndex != NO_EPISODE_SEED
                ? (std::size_t(env.EpisodeSeedIndex) + seat) % fits.size() : urand(0, uint32(fits.size()) - 1);
            s.L = fits[pick].L;
            s.Spec = fits[pick].Spec;
        }

    // One level every seat's class/role can be. A drawn tank's includes the level its tanking stance, form or aura
    // is learned: a bear-spec druid tank below 10 had no Bear Form, a warrior no Defensive Stance or Taunt -- a
    // third of stage6's druid tanks "out of bear form" were evaluated at levels 1-9 (2026-10-03).
    uint8 minLevel = 1;
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (SeatState const& s = data.Seats[seat]; s.L)
        {
            minLevel = std::max(minLevel, s.L->Assets->Kit->MinLevel());
            if (s.DungeonRole == DUNGEON_TANK && s.L->Profile)
                minLevel = std::max(minLevel, s.L->Assets->Kit->LevelOf(TankModeSpell(s.L->Profile->Class)));
        }

    minLevel = std::max({ minLevel, _stage.MinLevel, arena.MinLevel });

    // Characters.ReuseEpisodes: a seat whose draw gave it the class and build it already has keeps its character
    // for a few episodes rather than building a new one (the build was a quarter of a decision's cost). The env then
    // keeps its level too, since every seat shares one; the level draw is random anyway, so holding it a few
    // episodes biases nothing. Never in an evaluation (its seeded spread of characters is the yardstick), never on
    // never a seat that was empty or lost.
    std::array<bool, MAX_SEATS> reuse{};
    uint8 keptLevel = 0;
    // (An env whose last episode was on another map, an instance rung, rebuilds on the map this one wants.)
    // A dungeon run starts from a fresh instance every episode: every creature alive, every boss's script at its
    // start. Reused, the trash a group killed stayed dead for the next one, and only the boss was reset.
    bool const freshInstance = Arena(env).Instance == InstanceLadder::Wing;
    bool const changesMap = (env.FindMap() && env.FindMap()->GetId() != EpisodeMapId(env)) || freshInstance;
    if (!firstBuild && !env.Evaluating && !changesMap && _tuning.Characters.ReuseEpisodes > 0)
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

    // The stage's focus band (StageDefinition::FocusLevelFirst/Last) for most training characters.
    bool const focus = !_level && _stage.FocusChance && env.EpisodeSeedIndex == NO_EPISODE_SEED
        && _stage.FocusLevelLast >= std::max<uint8>(minLevel, _stage.FocusLevelFirst)
        && irand(0, 99) < int32(_stage.FocusChance);
    // An evaluation of a stage trained wholly in its band (FocusChance 100) is in the band too: spread over 1-80 it
    // scored stage6 mostly on levels the stage never trains (2026-10-03).
    bool const focusEval = !_level && _stage.FocusChance >= 100 && env.EpisodeSeedIndex != NO_EPISODE_SEED
        && _stage.FocusLevelLast >= std::max<uint8>(minLevel, _stage.FocusLevelFirst);
    // A stage fixed at one level (StageDefinition::Level) holds every character there, raised to its class's minimum.
    uint8 const level = data.EpisodeLevel ? std::clamp<uint8>(data.EpisodeLevel, minLevel, DEFAULT_MAX_LEVEL)
        : _stage.Level ? std::clamp<uint8>(_stage.Level, minLevel, DEFAULT_MAX_LEVEL)
        : keptLevel ? keptLevel
        : focus || focusEval ? uint8(urand(std::max<uint8>(minLevel, _stage.FocusLevelFirst), _stage.FocusLevelLast))
        : RandomLevel(minLevel, _level, _tuning.Characters, env.EpisodeSeedIndex, uint32(_layouts.size()));

    // The first build opens a new instance (or a phase of the continent); every later one reuses it. An env whose
    // seats were all lost keeps its instance while the map still exists, and opens a new one when it is gone. An
    // episode fixed to another map (an instance rung) opens a new instance of that map; the old one unloads once
    // its last bot has left.
    Map* map = (!firstBuild || env.InstanceId) && !freshInstance ? env.FindMap() : nullptr;
    if (map && map->GetId() != EpisodeMapId(env))
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
        if (arena.Seats == SeatPlan::Party || arena.Seats == SeatPlan::Raid)
        {
            // Within a group as a party has always spread; groups themselves step back in rows, so forty seats do
            // not spawn in one line forty spacings long.
            uint32 const inGroup = seat % GROUP_SEATS;
            uint32 const group = seat / GROUP_SEATS;
            start.m_positionX += (inGroup % 2 ? -PARTY_SPACING : PARTY_SPACING) * float(1 + inGroup / 2);
            start.m_positionY += (inGroup % 2 ? PARTY_SPACING : -PARTY_SPACING) - PARTY_SPACING * 2.0f * float(group);
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
    // An instance used as empty ground (M1 and M2's Stockades) is cleared whole, its far grids loaded first,
    // so no mob further along the hallway is there to kill a level 1 seat; any other instance, around the spawn.
    // The party follow (M4) moves between dungeons from episode to episode, so each new instance it opens is emptied
    // when it opens, not only the env's first.
    bool const partyFollow = Arena(env).Against == Opposition::PartyFollow;
    bool const newInstance = env.MapId != map->GetId() || env.InstanceId != map->GetInstanceId();
    if ((firstBuild || (partyFollow && newInstance)) && map->Instanceable())
    {
        if (Arena(env).Against == Opposition::Seek || Arena(env).Against == Opposition::Sight || Arena(env).Against == Opposition::Combat
            || Arena(env).Against == Opposition::Roles || partyFollow)
            SpawnArea::ClearMap(lead, partyFollow ? DUNGEON_CLEAR_RADIUS : INSTANCE_CLEAR_RADIUS);
        // M3's Deadmines is wider than the Stockades: from any of its sites to the ship's far end.
        else if (Arena(env).Against == Opposition::Interact)
            SpawnArea::ClearMap(lead, INTERACT_CLEAR_RADIUS);
        else
            SpawnArea::Clear(lead);
    }

    env.MapId = map->GetId();
    env.InstanceId = map->GetInstanceId();
    // One agent slot per agent, seats first; an empty seat's slot holds no bot. EnvPool wants one slot per agent
    // either way.
    env.Bots.clear();
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        env.Bots.push_back(seat < data.ActiveSeats ? SeatBot(env, seat)->GetGUID() : ObjectGuid::Empty);
    // The owner's slot: OwnerEncounter::Build fills it where the episode plays the owner through its row.
    if (_castOwner)
        env.Bots.push_back(ObjectGuid::Empty);
    env.Targets.clear();

    // A spawn point no objective can be found from used to take the whole run down with it: the plan stops when
    // its first scenario fails to start, so one bad patch in a list of twenty-six was a dead run. The ground is
    // drawn per episode now, so the answer is to draw again -- move the seats to another point and build there.
    // Only a stage whose every spawn point is bad fails now, which is a stage that deserves to.
    auto partMark = std::chrono::steady_clock::now();
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

    // Where each seat is looking, and its body, start as where the world put it. ResetEpisode cleared them, which
    // would aim every seat due east; this is the first point at which the bots have stopped being teleported about.
    // The owner's slot too, when it holds someone this episode (a cast owner, the party follow's leader): its client
    // is kept across episodes like a seat's, and would otherwise set out from where the last episode left its body.
    if (_castOwner)
        if (Player* bot = SeatBot(env, OwnerAgent()); bot && Data(env).Seats[OwnerAgent()].L)
        {
            data.Seats[OwnerAgent()].Facing = bot->GetOrientation();
            StartMover(data.Seats[OwnerAgent()], bot, env.EpisodeElapsedMs);
        }
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (Player* bot = SeatBot(env, seat))
        {
            data.Seats[seat].Facing = bot->GetOrientation();
            StartMover(data.Seats[seat], bot, env.EpisodeElapsedMs);
        }

    partMark = std::chrono::steady_clock::now();
    StockSeats(env);
    GivePets(env);
    CurrentReset.StockNs += ResetSinceNs(partMark);
    DrawStandIn(env);
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

    // A party follow's whole party is one faction (EnvState::EpisodeTeam); any other episode draws from the whole
    // list.
    std::vector<uint8> const& races = layout.Assets->Races;
    std::vector<uint8> pool;
    if (Data(env).EpisodeTeam)
    {
        TeamId const want = TeamId(Data(env).EpisodeTeam - 1);
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
    // map, not the stage's.
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
    Configure(bot, seat);
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

void Animus::Curriculum::StageScenario::Configure(Player* bot, SeatState& seat) const
{
    // Most characters get the spec's standard build; the rest have to be played as they are.
    seat.TalentPlan = RandomTalentPlan(_tuning.Characters);
    uint32 const noise = std::max<uint32>(1, _tuning.Characters.TalentNoisePoints);
    SeatCharacter::Built const built = SeatCharacter::Configure(bot, *seat.L, seat.Spec, seat.TalentPlan,
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

    // A party's drawn tank starts in its tanking stance, form or aura, as a tank walks into a dungeon: only the warrior
    // was given one, and by its build, so a bear (no shield) or a paladin without Righteous Fury fought a pull it was
    // drawn to hold in the wrong mode (stage6, 2026-10-03). Changing it is the seat's own press from here on.
    if (seat.DungeonRole != DUNGEON_TANK || !seat.L->Profile)
        return;
    constexpr uint32 SPELL_DIRE_BEAR_FORM = 9634;
    uint32 mode = TankModeSpell(seat.L->Profile->Class);
    if (seat.L->Profile->Class == CLASS_DRUID && bot->HasSpell(SPELL_DIRE_BEAR_FORM))
        mode = SPELL_DIRE_BEAR_FORM;
    if (mode && bot->HasSpell(mode) && !bot->HasAura(mode))
        bot->CastSpell(bot, mode, true);
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

        // Where death runs on, a seat with the death block takes a resurrection when it chooses to (DeathBlock's
        // accept), not the moment one is offered: waiting for one is its choice against the corpse run.
        if (slot < data.ActiveSeats && Arena(env).DeathRuns && data.Seats[slot].L
            && data.Seats[slot].L->Has(BlockId::Death))
        {
            if (!data.Seats[slot].DeathRun.AcceptResurrection)
                continue;
            data.Seats[slot].DeathRun.AcceptResurrection = false;
            ++data.Seats[slot].DeathRun.Accepted;
        }

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
    // Where death runs on, nobody is dead for good: the seat releases and runs back (DeathBlock); nor where it comes
    // back alive at the entrance (dungeon-curriculum I4).
    if (Arena(env).DeathRuns || Arena(env).RespawnAtEntrance)
        return false;
    CombatTally const& tally = Data(env).Seats[seatIndex].Combat;
    Player* bot = env.FindBot(seatIndex);
    if (!tally.Died || (bot && bot->IsAlive()))
        return false;

    bool const canResurrect = bot && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
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

void Animus::Curriculum::StageScenario::ApplyGoals(Env& env, int32 const* goals)
{
    EnvState& data = Data(env);
    auto const valid = [](int32 goal) { return goal >= 0 && goal < int32(GOAL_JOINT_COUNT) ? goal : NO_GOAL; };
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        SeatState& state = data.Seats[seat];
        // Primary then secondary (GOAL_SLOTS_ON_WIRE a seat). The secondary is the seat's own, and none when it
        // would repeat the primary.
        // A commanded arena's goal is given the same way (ArenaDefinition::CommandedGoals).
        int32 ordered = NO_GOAL;
        if (Arena(env).CommandedGoals)
            ordered = state.Commanded;
        int32 const primary = ordered != NO_GOAL ? ordered : valid(goals[seat * GOAL_SLOTS_ON_WIRE]);
        int32 secondary = valid(goals[seat * GOAL_SLOTS_ON_WIRE + 1]);
        if (secondary == primary)
            secondary = NO_GOAL;
        state.Holds[0].FromOrder = ordered != NO_GOAL;

        std::array<int32, GOAL_SLOTS> const next = { primary, secondary };
        for (uint32 slot = 0; slot < GOAL_SLOTS; ++slot)
        {
            GoalHold& hold = state.Holds[slot];
            int32 const goal = next[slot];
            // Any change of a goal still in progress -- its kind or its target -- is a plan abandoned, charged
            // (Goals.Switch); a goal that ended (reached, or no longer possible: the next enemy after this one died)
            // is replaced free, and so is one a commanded goal set or replaced, which is not the seat's doing.
            if (goal != hold.Goal && hold.Goal != NO_GOAL && goal != NO_GOAL)
            {
                ++state.GoalChanges;
                bool const ordered = slot == 0 && (hold.FromOrder || state.Holds[0].FromOrder);
                if (!hold.Ended && !ordered)
                    ++state.StepGoalSwitches;   // charged at the next reward (Goals.Switch)
            }

            // A new goal is a new thing to reach, and is paid for again when it is. Its progress is measured from
            // its first observation, which knows where its place is (PotentialReady).
            if (goal != hold.Goal)
            {
                bool const fromOrder = hold.FromOrder;
                hold = GoalHold();
                hold.Goal = goal;
                hold.Fresh = true;
                hold.FromOrder = fromOrder;
                if (goal != NO_GOAL)
                    ++state.GoalsChosenBy[GoalKindOf(goal)];
            }
        }
    }
}

void Animus::Curriculum::StageScenario::ApplyLook(Env& env, int32 const* look)
{
    if (!_stage.Has(BlockId::Vision))
        return;
    EnvState& data = Data(env);
    Vision::Settings const& settings = Vision::Current();
    // The seats with a character this episode, and the cast owner while it is played through its row: the rows with
    // a camera. An empty seat's and an unplayed owner's rows are placeholders, never applied (R5).
    auto const take = [&](uint32 agent)
    {
        SeatState& seat = data.Seats[agent];
        if (!seat.L)
            return;
        // Turn to camera: the body's turn goes to the player controller as a one-shot control, which snaps the facing
        // at the next tick's start and reports it with one SET_FACING, as a client does. Still free (R1): it is not a
        // press, so nothing prices, repeats or tallies it.
        float const turn = Vision::FreeLook::Apply(seat.Look, look + std::size_t(agent) * Vision::FreeLook::HEADS,
            settings);
        if (turn != 0.0f)
            seat.Controls.Held.FaceTurn = turn;
    };
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        take(seat);
    if (CastOwnerActive(env))
        take(OwnerAgent());
}

std::pair<uint32, uint32> Animus::Curriculum::StageScenario::CameraRenderSize(Env const& env, uint32 agent) const
{
    if (!_stage.Has(BlockId::Vision))
        return { 0, 0 };
    EnvState const& data = Data(env);
    bool const seat = agent < _seatCount || (_castOwner && agent == OwnerAgent());
    if (!seat || agent >= data.Seats.size() || !data.Seats[agent].L)
        return { 0, 0 };
    Vision::Resolution const render = data.Seats[agent].Look.Render;
    return { render.Width, render.Height };
}

bool Animus::Curriculum::StageScenario::GoalHeld(Env const& env, uint32 seatIndex, Player* bot,
    Unit const* target) const
{
    SeatState const& seat = Data(env).Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    if (!bot || !bot->IsAlive() || seat.Holds[0].Goal == NO_GOAL)
        return false;

    switch (SeatGoal(GoalKindOf(seat.Holds[0].Goal)))
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
        case SeatGoal::Rest:
            return step.SelfHealing > 0 || bot->HasAuraType(SPELL_AURA_MOD_REGEN)
                || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
        case SeatGoal::TravelTo:
        case SeatGoal::Gather:
        case SeatGoal::Interact:
            // On the way, or there and doing it (a cast, a loot window).
            return (seat.Holds[0].HasPlace && bot->GetExactDist2d(&seat.Holds[0].Place) <= GoalBlock::PLACE_REACH)
                || !bot->movespline->Finalized() || bot->IsNonMeleeSpellCast(false) || !bot->GetLootGUID().IsEmpty();
        case SeatGoal::Loot:
            return !bot->GetLootGUID().IsEmpty() || !bot->movespline->Finalized();
        case SeatGoal::Resurrect:
            return seat.StepRevivedAlly || bot->IsNonMeleeSpellCast(false);
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
    // The upkeep can have changed what each seat fights (a wing's slots are rebuilt): asked again below.
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        Data(env).Seats[seat].DecisionTargetKnown = false;
    if (_castOwner)
        Data(env).Seats[OwnerAgent()].DecisionTargetKnown = false;

    AcceptResurrections(env);

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        ApplySeatAction(env, seat, actions[seat]);

    if (CastOwnerActive(env))
        ApplySeatAction(env, OwnerAgent(), actions[OwnerAgent()]);
}

void Animus::Curriculum::StageScenario::StartMover(SeatState& seat, Player* bot, uint32 nowMs)
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap())
        return;
    Movement::PlayerLink link(bot, seat.Link);
    Movement::MapWorldQuery const world(bot->GetMap(), bot->GetPhaseMask());
    Movement::Body const shape = Movement::ShapeOf(bot);
    seat.Mover.Start(link, shape, world, nowMs);
    seat.Facing = seat.Mover.Body.Yaw;
}

void Animus::Curriculum::StageScenario::SubTick(Env& env, uint32 diffMs, bool /*decided*/)
{
    // Every world tick, after the decision's presses when there is one: the player controller moves each seat's
    // body under the keys it holds and reports it to the server as a client would (Movement::Client, §5A), after
    // answering whatever the server ordered it since the last tick (a root, a knockback, flying, ...).
    uint32 const nowMs = env.EpisodeElapsedMs;
    auto const started = std::chrono::steady_clock::now();
    uint64 seatTicks = 0;
    auto const tick = [&](uint32 index)
    {
        SeatState& seat = Data(env).Seats[index];
        Player* bot = env.FindBot(index);
        if (!bot || !seat.L || !bot->IsInWorld() || !bot->GetMap())
            return;
        ++seatTicks;
        Movement::PlayerLink link(bot, seat.Link);
        Movement::MapWorldQuery const world(bot->GetMap(), bot->GetPhaseMask());
        Movement::Body const shape = Movement::ShapeOf(bot);
        if (!seat.Mover.Started())
            seat.Mover.Start(link, shape, world, nowMs);
        if (Animus::Client::Inbox* inbox = bot->GetSession() ? bot->GetSession()->MovementOrders() : nullptr)
        {
            // Map threads tick envs in parallel: each drains into its own buffer.
            thread_local std::vector<Animus::Client::Order> orders;
            inbox->Drain(orders);
            for (Animus::Client::Order const& order : orders)
                seat.Mover.Order(order, link, shape, world, nowMs);
        }
        // Standing (or swimming, flying) above the terrain's surface before the tick, and inside it after: the
        // controller walked or fell through a hillside, which it must not (PlayerController's IntoTerrain).
        Movement::BodyState const& body = seat.Mover.Body;
        bool const above = world.TerrainHeight(body.X, body.Y) > Movement::INVALID_FLOOR + 1.0f
            && !world.InTerrain(body.X, body.Y, body.Z + 0.1f);
        seat.Mover.Tick(seat.Controls.Held, Movement::SpeedsOf(bot), shape, world, diffMs, nowMs, link);
        bool const intoTerrain = above && world.InTerrain(body.X, body.Y, body.Z + 0.1f);
        seat.Facing = seat.Mover.Body.Yaw;
        TrackController(seat, diffMs);
        WatchFall(seat, bot, nowMs, Arena(env).Name, env.Evaluating, intoTerrain);
    };
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        tick(seat);
    // The owner's slot when it is played through its row, or when it holds the party follow's leader, whose
    // scripted keys the controller moves as it moves a seat's.
    if (CastOwnerActive(env)
        || (_partyFollow && Arena(env).Against == Opposition::PartyFollow && _partyFollow->HasLeader(env)))
        tick(OwnerAgent());
    if (seatTicks)
        Movement::ControllerCost::Add(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count()), seatTicks);
}

namespace
{
    constexpr uint32 LOG_DEATH = 0;
    constexpr uint32 LOG_VOID_FALL = 1;
    constexpr uint32 LOG_BURIED = 2;
    constexpr uint32 LOG_INTO_TERRAIN = 3;
    constexpr uint32 LOG_OVER_VOID = 4;
}

bool Animus::Curriculum::StageScenario::MayLog(SeatState const& seat, uint32 kind, uint32 cap) const
{
    uint32 const layout = seat.L ? std::min<uint32>(seat.L->Index, LOG_LAYOUTS - 1) : LOG_LAYOUTS - 1;
    return _logged[std::min(kind, LOG_KINDS - 1)][layout].fetch_add(1, std::memory_order_relaxed) < cap;
}

void Animus::Curriculum::StageScenario::WatchFall(SeatState& seat, Player* bot, uint32 nowMs, std::string const& arena,
    bool evaluating, bool intoTerrain) const
{
    Movement::BodyState const& body = seat.Mover.Body;
    uint8 const kind = uint8(body.Kind);
    if (kind == uint8(Movement::Mode::Falling) && seat.LastKind != kind)
    {
        seat.FallStartMs = nowMs;
        seat.FallStartZ = body.Z;
        seat.FallFrom = seat.LastKind;
    }
    seat.LastKind = kind;

    // Taken from the server deeper than a step inside the ground with nothing under it, and stood on the terrain
    // (Resync's last rung): a spawn point, or the server, put the unit in the ground. Once an episode a seat.
    if (seat.Mover.Counts.Unburied > seat.MoverAtStart.Unburied && !seat.UnburiedLogged)
    {
        seat.UnburiedLogged = true;
        if (MayLog(seat, LOG_BURIED, 8))
            LOG_WARN("module.animus", "Player controller: {} ({} arena, {}) was put inside the ground at ({:.1f}, "
                "{:.1f}, {:.1f}) on map {} with nothing under it, and was stood on the terrain at z {:.1f}: the spawn "
                "point or the server's position is buried", bot->GetName(), arena,
                evaluating ? "evaluating" : "training", body.X, body.Y, seat.Mover.UnburiedFromZ, bot->GetMapId(),
                body.Z);
    }

    // A step refused over nothing at all (no floor, no terrain, no water below): the world query has a hole there --
    // the vmaps lack what the client has (M1's Stockades hallway). Once a seat an episode.
    if (seat.Mover.Counts.OverVoid > seat.MoverAtStart.OverVoid && !seat.OverVoidLogged)
    {
        seat.OverVoidLogged = true;
        if (MayLog(seat, LOG_OVER_VOID, 8))
            LOG_WARN("module.animus", "Player controller: {} ({} arena, {}) was kept from stepping over nothing at all "
                "at ({:.1f}, {:.1f}, {:.1f}) on map {}: no floor, terrain or water below -- a hole in the world query "
                "there", bot->GetName(), arena, evaluating ? "evaluating" : "training", seat.Mover.VoidX,
                seat.Mover.VoidY, seat.Mover.VoidZ, bot->GetMapId());
    }

    // Through the terrain's surface from above in one tick: never, by the controller's own rule (IntoTerrain). Counted
    // (the into_terrain column), and logged once a seat an episode if it ever happens.
    if (intoTerrain)
    {
        ++seat.IntoTerrain;
        if (!seat.IntoTerrainLogged && MayLog(seat, LOG_INTO_TERRAIN, 8))
        {
            seat.IntoTerrainLogged = true;
            Map const* map = bot->GetMap();
            LOG_WARN("module.animus", "Player controller: {} ({} arena, {}) ended a tick with its feet inside the "
                "terrain, from above it: body ({:.1f}, {:.1f}, {:.1f}) mode {} map {}, the terrain {:.1f} there",
                bot->GetName(), arena, evaluating ? "evaluating" : "training", body.X, body.Y, body.Z,
                uint32(body.Kind), map->GetId(), map->GetGridHeight(body.X, body.Y));
        }
    }

    // Three seconds down with no floor anywhere under it, a seat falls until the map kills it at its floor (z -500):
    // the M1 dry check lost shaman seats so (2026-10-05). The first few are logged with what explains them.
    constexpr uint32 VOID_FALL_MS = 3000;
    if (seat.VoidFallLogged || kind != uint8(Movement::Mode::Falling) || nowMs - seat.FallStartMs < VOID_FALL_MS)
        return;
    Map* map = bot->GetMap();
    if (map->GetHeight(bot->GetPhaseMask(), body.X, body.Y, body.Z, true, 2000.0f) > INVALID_HEIGHT)
        return;
    seat.VoidFallLogged = true;
    if (!MayLog(seat, LOG_VOID_FALL, 4))
        return;

    std::string auras;
    uint32 shown = 0;
    for (auto const& [spell, application] : bot->GetAppliedAuras())
        if (shown++ < 16)
            auras += Acore::StringFormat("{}{}", auras.empty() ? "" : " ", spell);
    Movement::Speeds const speeds = Movement::SpeedsOf(bot);
    Movement::Client::Counters const& counts = seat.Mover.Counts;
    LOG_WARN("module.animus", "Player controller: {} ({}, class {} race {} level {}) has fallen {:.1f} s with no floor "
        "under it ({} arena, {}): body ({:.1f}, {:.1f}, {:.1f}) map {} instance {} phase {}, the server's z {:.1f}; it "
        "began to fall at {} ms from z {:.1f} out of mode {} (terrain {:.1f} there, a floor from just above that "
        "height {:.1f}); client: last order {} at {} ms, granted 0x{:X}, rooted {}, {} resyncs, {} acks, {} yield "
        "ticks, {} refused; fly {} slow fall {} water walk {}, radius {:.2f} height {:.2f}, form {}; auras {}",
        bot->GetName(), seat.L ? seat.L->ModelName() : "?", uint32(bot->getClass()), uint32(bot->getRace()),
        uint32(bot->GetLevel()), float(nowMs - seat.FallStartMs) / 1000.0f, arena,
        evaluating ? "evaluating" : "training", body.X, body.Y, body.Z, map->GetId(), map->GetInstanceId(),
        bot->GetPhaseMask(), bot->GetPositionZ(), seat.FallStartMs, seat.FallStartZ,
        uint32(seat.FallFrom), map->GetGridHeight(body.X, body.Y),
        map->GetHeight(bot->GetPhaseMask(), body.X, body.Y, seat.FallStartZ + 2.0f, true, 10.0f),
        uint32(seat.Mover.LastOrder), seat.Mover.LastOrderMs, seat.Mover.Granted(), seat.Mover.Rooted(),
        counts.Resyncs, counts.Acks, counts.YieldTicks, counts.Refused, speeds.CanFly, speeds.SlowFall,
        speeds.WaterWalk, Movement::ShapeOf(bot).Radius, Movement::ShapeOf(bot).Height,
        uint32(bot->GetShapeshiftForm()), auras);
}

void Animus::Curriculum::StageScenario::TrackController(SeatState& seat, uint32 diffMs)
{
    Movement::BodyState const& body = seat.Mover.Body;
    Movement::ControlState const& held = seat.Controls.Held;
    bool const keys = held.Forward || held.Strafe || held.Vertical;
    // Pressing into a wall, and held keys getting nowhere for a second or more (a wall, a slope, a ledge's lip).
    if (keys && seat.Mover.TickWall)
        seat.WallMs += diffMs;
    bool const stuck = keys && seat.Mover.TickCommanded > 0.01f
        && seat.Mover.TickMoved < 0.1f * seat.Mover.TickCommanded;
    // Counted from the second it began, once it has lasted a second.
    uint32 const before = seat.StuckRunMs;
    seat.StuckRunMs = stuck ? seat.StuckRunMs + diffMs : 0;
    if (seat.StuckRunMs >= 1000)
        seat.StuckMs += before < 1000 ? seat.StuckRunMs : diffMs;
    // Jumps taken, and the falls the server landed: a drop is a landing from two yards or more; what the landings
    // cost is the server's own (Player::HandleFall, measured by PlayerLink).
    seat.Jumps += seat.Mover.TickJumps;
    if (seat.Mover.TickLandings && seat.Mover.TickFallHeight >= 2.0f)
    {
        ++seat.Drops;
        ++seat.Falls;
    }
    seat.FallDamage += seat.Link.FallDamage;
    seat.FallDeaths += seat.Link.FallDeaths;
    seat.VoidDeaths += seat.Link.VoidDeaths;
    seat.Link.FallDamage = 0.0f;
    seat.Link.FallDeaths = 0;
    seat.Link.VoidDeaths = 0;

    // The course turning more than 20 degrees within a tick while moving: a kink a watcher sees. Read from the
    // body's way between ticks, not its velocity, which the ground step zeroes every step (only a fall carries one):
    // read off the velocity, course_kinks was 0 for every seat on the ground (dry check, 2026-10-05).
    if (CourseKink(seat, body.X, body.Y, diffMs))
        ++seat.CourseKinks;
}

bool Animus::Curriculum::StageScenario::CourseKink(SeatState& seat, float x, float y, uint32 diffMs)
{
    bool kink = false;
    float const dx = x - seat.CourseX;
    float const dy = y - seat.CourseY;
    float const seconds = float(diffMs) / 1000.0f;
    // Moving: at least half a yard a second over the tick.
    if (seat.HasCoursePos && seconds > 0.0f && dx * dx + dy * dy >= (0.5f * seconds) * (0.5f * seconds))
    {
        float const course = std::atan2(dy, dx);
        kink = seat.HasCourse && std::fabs(std::remainder(course - seat.LastCourse, 2.0f * float(M_PI))) > 0.3490659f;
        seat.LastCourse = course;
        seat.HasCourse = true;
    }
    else if (seat.HasCoursePos)
        seat.HasCourse = false;
    seat.CourseX = x;
    seat.CourseY = y;
    seat.HasCoursePos = true;
    return kink;
}

Unit* Animus::Curriculum::StageScenario::SeatTarget(Env const& env, uint32 seat) const
{
    EnvState const& data = Data(env);
    if (seat < data.Seats.size())
        if (ObjectGuid const guid = data.Seats[seat].CurrentTargetGuid)
            if (Player* bot = env.FindBot(seat))
                return Encoding::UnitThrough(*bot, guid);

    return env.FindTargetUnit(0);
}

Unit* Animus::Curriculum::StageScenario::CurrentTarget(Env const& env, uint32 seat)
{
    Unit* target = nullptr;
    bool chosen = false;
    // In a stage with a sight block the seat chooses its own target from what it sees (dungeon-curriculum I1): its
    // target is its client's selection, never one an encounter picked for it.
    if (_stage.Has(BlockId::Sight))
    {
        Player* bot = env.FindBot(seat);
        target = bot && bot->IsInWorld() ? Encoding::UnitThrough(*bot, bot->GetTarget()) : nullptr;
        chosen = true;
    }
    for (Encounter* encounter : ActiveEncounters(env))
        if (!chosen && encounter->SelectTarget(env, seat, target))
        {
            chosen = true;
            break;
        }
    if (!chosen)
        target = env.FindTargetUnit(0);

    // Never a unit on its way out of the world or on another map than the seat's: the encoder's cast checks build a
    // Spell against it (SpellChecks::CheckCast), and a stage18_life map thread faulted in
    // Encoding::IsSpellActionAllowed on a target being removed while the seat was observed.
    if (target && (!target->IsInWorld() || target->IsDuringRemoveFromWorld()))
        return nullptr;
    Player* bot = target ? env.FindBot(seat) : nullptr;
    if (bot && bot->IsInWorld() && target->GetMap() != bot->GetMap())
        return nullptr;
    return target;
}

Unit* Animus::Curriculum::StageScenario::DecisionTarget(Env const& env, uint32 seatIndex)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.DecisionTargetKnown)
    {
        Unit* target = CurrentTarget(env, seatIndex);
        seat.DecisionTarget = target ? target->GetGUID() : ObjectGuid::Empty;
        seat.DecisionTargetKnown = true;
        return target;
    }
    if (seat.DecisionTarget.IsEmpty())
        return nullptr;
    Player* bot = env.FindBot(seatIndex);
    Unit* target = bot ? Encoding::UnitThrough(*bot, seat.DecisionTarget) : nullptr;
    if (!target || !target->IsInWorld() || target->IsDuringRemoveFromWorld()
        || (bot->IsInWorld() && target->GetMap() != bot->GetMap()))
        return nullptr;
    return target;
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
    view.Goal = seat.Holds[0].Goal;
    view.Goal2 = seat.Holds[1].Goal;
    view.GoalEnded = seat.Holds[0].Ended;
    // The keys it holds and the body they move, carried over from the last decision: without them a held key is
    // forgotten before it can do anything.
    view.Controls = &seat.Controls;
    // Its camera, in a stage with one: the vision block advances it by the decision and renders from it.
    view.Look = _stage.Has(BlockId::Vision) ? &seat.Look : nullptr;
    view.Seen = _stage.Has(BlockId::Vision) ? &seat.Seen : nullptr;
    // Its entity memory and what the sight block's slots name, in a stage with one (dungeon-curriculum I1, I2).
    if (_stage.Has(BlockId::Sight))
    {
        view.Recall = &seat.Recall;
        view.RecallKept = seat.RecallKept;
        view.SightGuids = &seat.SightGuids;
        view.Focus = &seat.Focus;
    }
    // Until this episode's client has taken its body from the server, the seat reads the server's (Client::Stop).
    view.Body = seat.Mover.Started() ? &seat.Mover.Body : nullptr;
    view.Facing = seat.Facing;
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
    view.NowMs = env.EpisodeElapsedMs;
    view.DecisionMs = _decisionMs;
    view.LastStepDamage = seat.LastStepDamage;
    view.LastStepPowerDelta = seat.LastStepPowerDelta;
    view.LastStepDamageTaken = seat.LastStepDamageTaken;
    view.EpisodeTime = std::min(1.0f, float(env.EpisodeElapsedMs) / EPISODE_TIME_SCALE_MS);
    view.CombatTime = seat.InCombat
        ? std::min(1.0f, float(env.EpisodeElapsedMs - seat.CombatStartMs) / MAX_COMBAT_TIME_MS) : 0.0f;
    view.Supplies = seat.Supplies;
    view.SelfResurrectAllowed = true;
    view.DeathRuns = Arena(env).DeathRuns;
    view.DeadSeconds = seat.DeathRun.DeadSinceMs && env.EpisodeElapsedMs > seat.DeathRun.DeadSinceMs
        ? float(env.EpisodeElapsedMs - seat.DeathRun.DeadSinceMs) / 1000.0f : 0.0f;

    view.StableCount = uint32(std::min<std::size_t>(seat.Stable.size(), STABLE_SLOTS));
    std::copy_n(seat.Stable.begin(), view.StableCount, view.Stable.begin());

    if (_stage.Has(BlockId::Sight))
    {
        // A sight stage's enemies are what the seat saw (dungeon-curriculum I3): the last frame's visible living
        // hostiles in its slot order, never the encounter's spawn list -- nothing behind a wall, nothing round a
        // corner. Its selection's place is the frame's too: out of it, where the seat last saw it.
        // Alive only: a dead seat sees nothing, and a risen one nothing until its camera casts a frame again
        // (RiseAtEntrance clears the list), so no pre-death frame leaks into its first decision at the entrance.
        if (bot && bot->IsInWorld() && bot->IsAlive())
        {
            Player* const seer = bot;
            view.EnemyCount = CombatBlock::VisibleEnemies(seat.Seen, [seer](uint64 guid) -> Unit*
            {
                Unit* unit = Encoding::UnitThrough(*seer, ObjectGuid(guid));
                return unit && unit->IsInWorld() && unit->GetMap() == seer->GetMap() ? unit : nullptr;
            }, view.Enemies.data(), PACK_SLOTS);
        }
        if (target)
        {
            view.TargetInView = target == bot || CombatBlock::InView(seat.Seen, target->GetGUID().GetRawValue());
            if (!view.TargetInView)
                if (Vision::Remembered const* entry = seat.Recall.Find(target->GetGUID().GetRawValue()))
                {
                    view.TargetSeen = true;
                    view.LastSeen.Relocate(entry->Position.X, entry->Position.Y, entry->Position.Z);
                    view.TargetUnseenTime = std::min(1.0f, std::max(0.0f, seat.Recall.AgeOf(*entry))
                        * 1000.0f / MAX_UNSEEN_TIME_MS);
                }
        }
    }
    else
    {
        view.EnemyCount = uint32(std::min<std::size_t>(env.Targets.size(), PACK_SLOTS));
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            view.Enemies[slot] = env.FindTargetUnit(slot);
    }
    view.TargetSlot = seat.TargetSlot;
    view.FriendSlot = seat.FriendSlot;
    view.RankTier = seat.RankTier;

    // The party frames (the party frames block, revision 2: the one source of party-member state): from the seat's
    // core group, where it has one; an encounter whose party has none (the party follow) fills them in its View.
    if (_stage.Has(BlockId::PartyFrames))
    {
        view.MinimapYards = _tuning.PartyFollow.MinimapYards;
        PartyFramesBlock::FillFromGroup(view);
    }

    for (Encounter* encounter : ActiveEncounters(env))
        encounter->View(env, seatIndex, view);
    // Where a trip's objective is, the seat knows only through a compass it is shown: without one -- a stage with no
    // compass block, or an episode that withholds it -- the goal block has no place for it, so its TravelTo cannot
    // read "within 20 yd" through walls (GoalBlock::PlaceOf).
    view.ObjectivePlaceKnown = GoalBlock::ObjectivePlaceKnown(_stage.Has(BlockId::Compass), view.CompassWithheld);

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
    seat.Pressed = action;

    // Asked again after the encounters' upkeep (ApplyActions), which can have changed the targets since the
    // observation: once for the action.
    Unit* target = DecisionTarget(env, seatIndex);
    if (!target && !SeatEncoder::ActsWithoutTarget(*seat.L))
        return;

    TrackTarget(env, seat, bot, target);

    // A paced action is masked, so only a policy that ignores the mask gets here with one: it does nothing.
    if (action > 0 && Paced(env, seat, uint32(action)))
        action = 0;

    SeatView view = ViewSeat(env, seatIndex, bot, target);
    view.NearestHazard = seat.NearestHazard;
    // A sight stage's ground fire is what the camera shows (I3): the visible hazards, never the server's areas.
    if (_stage.Has(BlockId::Sight))
    {
        CombatBlock::SeenHazards const seen = bot && bot->IsAlive()
            ? CombatBlock::ReadHazards(seat.Seen, bot->GetPositionX(), bot->GetPositionY(), bot->GetOrientation())
            : CombatBlock::SeenHazards();
        view.HazardsSeen = true;
        view.StandingSeen = seen.Standing;
        view.DeepestSeen = seen.Deepest;
        view.NearestHazard = seen.Nearest;
    }
    view.Option = &seat.Option;
    SeatActionResult result;
    SeatOptionSet const started = seat.Option;
    SeatEncoder::Apply(view, action, result);
    // The frame the next decision observes from (the move block keeps the controls and the body in place).
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
    // A held control pressed again is the key kept down (MoveBlock): no repeat, effort or verdict.
    if (action > 0 && !result.KeyStillHeld)
    {
        Press(env, seat, bot, uint32(action), result.DidSomething());
        JudgePress(env, seat, bot, target, uint32(action), result);
    }

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
    seat.ControlChanges += result.ControlChanged ? 1 : 0;
    seat.TurnReversals += result.TurnReversals;
    seat.BearingFlips += result.BearingFlip;
    seat.PitchReversals += result.PitchReversals;
    seat.Weaves += result.Weaves;
    seat.StepJitter += result.JitterWeight;
    seat.TrinketUses += result.TrinketUses;
    seat.ItemUses += result.ItemUses;
    seat.ConsumablesUsed += result.ConsumablesUsed;
    seat.SelfResurrections += result.SelfResurrected ? 1 : 0;
    seat.DeathRun.Releases += result.Released ? 1 : 0;
    seat.DeathRun.CorpseRises += result.RoseAtCorpse ? 1 : 0;
    seat.DeathRun.SpiritHealer += result.SpiritHealer ? 1 : 0;
    seat.DeathRun.StepSpiritHealer |= result.SpiritHealer;
    seat.DeathRun.AcceptResurrection |= result.AcceptResurrection;
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

void Animus::Curriculum::StageScenario::Observe(Env& env, float* obs, float* state, uint8* mask, uint8* image,
    uint8* map)
{
    // An agent's image row, or none for a stage without a camera; its map row, or none without a map.
    auto const imageRow = [this, image](uint32 agent)
    {
        return image ? image + std::size_t(agent) * _spec.ImageBytes : nullptr;
    };
    auto const mapRow = [this, map](uint32 agent)
    {
        return map ? map + std::size_t(agent) * _spec.MapBytes : nullptr;
    };
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        ObserveSeat(env, seat, obs + seat * _spec.ObsDim, mask ? mask + seat * _spec.NumActions : nullptr,
            imageRow(seat), mapRow(seat));
    }
    // The seats have paid the goals they reached into this decision's reward; the row is the pool's again.
    Data(env).StepReward = nullptr;

    // The owner's row: a seat's observation when it is played through it, else an empty row that allows only
    // the no-op (the learner marks it absent, AgentPresence).
    if (_castOwner)
    {
        uint32 const agent = OwnerAgent();
        float* row = obs + agent * _spec.ObsDim;
        uint8* maskRow = mask ? mask + agent * _spec.NumActions : nullptr;
        if (CastOwnerActive(env))
            ObserveSeat(env, agent, row, maskRow, imageRow(agent), mapRow(agent));
        else
        {
            std::fill(row, row + _spec.ObsDim, 0.0f);
            if (uint8* pixels = imageRow(agent))
                Vision::FillNoFrame(pixels, _spec.ImageBytes);
            if (uint8* cells = mapRow(agent))
                std::fill(cells, cells + _spec.MapBytes, uint8(0));
            if (maskRow)
            {
                std::fill(maskRow, maskRow + _spec.NumActions, uint8(0));
                maskRow[0] = 1;
            }
        }
    }

    WriteState(env, state);
}

void Animus::Curriculum::StageScenario::AgentLayouts(Env const& env, uint16* layout) const
{
    EnvState const& data = Data(env);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        layout[seat] = data.Seats[seat].L ? data.Seats[seat].L->Index : 0;

    if (_castOwner)
        layout[OwnerAgent()] = data.Seats[OwnerAgent()].L ? data.Seats[OwnerAgent()].L->Index : 0;
}

void Animus::Curriculum::StageScenario::AgentPresence(Env const& env, uint8* present) const
{
    EnvState const& data = Data(env);
    // The "human" stand-in's seat (StandIn.h) is 2: a real row, which the learner plays with a frozen partner and
    // never trains on (protocol 25).
    for (uint32 seat = 0; seat < _seatCount; ++seat)
        present[seat] = StandIn::Presence(data.Seats[seat].L != nullptr, data.StandInPlay.Seat == int32(seat));

    // The owner is an agent only in the episodes that play it through its row (a follow stage's evaluation keeps its
    // scripted leader: the learner neither runs the cast actor nor trains on the row).
    if (_castOwner)
        present[OwnerAgent()] = CastOwnerActive(env) ? 1 : 0;
}

void Animus::Curriculum::StageScenario::AgentKinematics(Env const& env, float* kinematics) const
{
    namespace K = Animus::Kinematics;
    EnvState const& data = Data(env);
    uint32 const agents = Spec().AgentsPerEnv;
    float const seconds = float(env.EpisodeElapsedMs) / float(IN_MILLISECONDS);
    for (uint32 agent = 0; agent < agents; ++agent)
    {
        float* out = kinematics + std::size_t(agent) * K::SAMPLE_DIM;
        // A seat's body, or the owner's when an arena plays it through its row.
        bool const seat = agent < _seatCount || (_castOwner && agent == OwnerAgent());
        Player const* bot = seat && data.Seats[agent].L ? SeatBotInWorld(env, agent) : nullptr;
        if (!bot)
        {
            K::Clear(out);
            continue;
        }

        // Airborne is a jump or fall spline on its way (Unit::IsFalling reads MOVEMENTFLAG_FALLING, which no client
        // ever clears on a seat); water is the unit's own test, as MoveBlock reads it. No seat flies: the first
        // curriculum's mounts and flight were deleted with it.
        bool const spline = bot->movespline->Initialized() && !bot->movespline->Finalized();
        bool const jumping = spline && (bot->movespline->isFalling() || bot->movespline->isParabolic());
        bool const inWater = bot->Unit::IsInWater();

        K::Body body;
        body.X = bot->GetPositionX();
        body.Y = bot->GetPositionY();
        body.Z = bot->GetPositionZ();
        body.Yaw = bot->GetOrientation();
        body.Pitch = data.Seats[agent].Mover.Body.Pitch;
        body.Motion = K::ModeOf(jumping, inWater, false);
        body.Mounted = bot->IsMounted();
        // The speed the body actually moves under, which is what its steps are measured in: a seat flagged flying
        // launches every spline at flight speed, on the ground too (TravelBlock::Apply), and one in water swims.
        UnitMoveType const moveType = bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING) ? MOVE_FLIGHT
            : inWater ? MOVE_SWIM : MOVE_RUN;
        body.Speed = bot->GetSpeed(moveType);
        body.InCombat = bot->IsInCombat();
        K::Write(seconds, body, out);
    }
}

void Animus::Curriculum::StageScenario::ObserveSeat(Env& env, uint32 seatIndex, float* obs, uint8* mask,
    uint8* image, uint8* map)
{
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    // The image row starts as no frame, as the observation row starts zeroed: a seat that renders nothing this
    // decision (no character, no map) sends "nothing seen"; its map row, every cell unknown.
    if (image)
        Vision::FillNoFrame(image, _spec.ImageBytes);
    if (map)
        std::fill(map, map + _spec.MapBytes, uint8(0));
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
    Unit* target = DecisionTarget(env, seatIndex);     // may be null between gauntlet pulls

    // Note when the bot entered or left combat (SeatView::CombatTime).
    bool const inCombat = bot && bot->IsAlive() && bot->IsInCombat();
    if (inCombat && !seat.InCombat)
        seat.CombatStartMs = env.EpisodeElapsedMs;
    seat.InCombat = inCombat;

    // Swimming is the client's to report (the controller's START_SWIM / STOP_SWIM, applied by the server's own
    // handling, which keeps m_isInWater and MOVEMENTFLAG_SWIMMING): the sim no longer sets them on a seat's behalf.

    TrackTarget(env, seat, bot, target);
    TrackMotion(env, seat, bot, target);
    if (seat.Memory.Actions() != seat.L->NumActions)
        seat.Memory.Reset(seat.L->NumActions);
    seat.Memory.Observe(bot, target, env.EpisodeElapsedMs);
    // The mental map at the episode's first look (amendment 1): kept, older by the reset's offset, when the reset
    // rolled it and the seat is on the instance it is of; else started afresh.
    if ((_stage.Has(BlockId::Map) || _stage.Has(BlockId::Sight)) && bot && seat.MapPending)
    {
        seat.MapPending = false;
        bool const same = seat.MapMapId == bot->GetMapId() && seat.MapInstanceId == bot->GetInstanceId();
        seat.MapKept = seat.MapKeep && same && seat.Map.Tiles() > 0;
        if (seat.MapKept)
            seat.Map.Advance(seat.MapAgeOffset);
        else
            seat.Map.Clear();
        seat.Map.Configure(Vision::MapCurrent().Caps);
        // Entity memory by the same roll and offset (I2): every sighting in it ages as the map's looks do.
        seat.Recall.Configure(Vision::MemoryCurrent());
        seat.RecallKept = seat.MapKeep && same && seat.Recall.Count() > 0;
        if (seat.RecallKept)
            seat.Recall.Advance(seat.MapAgeOffset);
        else
            seat.Recall.Clear();
        seat.MapMapId = bot->GetMapId();
        seat.MapInstanceId = bot->GetInstanceId();
    }
    SeatView view = ViewSeat(env, seatIndex, bot, target);
    view.NearestHazard = seat.NearestHazard;
    // A sight stage's ground fire is what the camera shows (I3): the visible hazards, never the server's areas.
    if (_stage.Has(BlockId::Sight))
    {
        CombatBlock::SeenHazards const seen = bot && bot->IsAlive()
            ? CombatBlock::ReadHazards(seat.Seen, bot->GetPositionX(), bot->GetPositionY(), bot->GetOrientation())
            : CombatBlock::SeenHazards();
        view.HazardsSeen = true;
        view.StandingSeen = seen.Standing;
        view.DeepestSeen = seen.Deepest;
        view.NearestHazard = seen.Nearest;
    }
    if (_stage.Has(BlockId::Map))
    {
        view.Hits = &seat.Hits;
        view.Map = &seat.Map;
        view.MapRow = map;
        view.MapKept = seat.MapKept;
    }
    view.Option = &seat.Option;

    // Each goal held, read off the world as it now is: reached (paid now, into this decision's reward) or no longer
    // possible -- either way it has ended. A primary that ended makes the learner promote its queue or choose again
    // at this decision; a secondary that ended is dropped by both sides until the next choice.
    view.Goal2Ended = false;
    for (uint32 slot = 0; slot < GOAL_SLOTS; ++slot)
    {
        GoalHold& hold = seat.Holds[slot];
        if (hold.Goal == NO_GOAL || hold.Ended)
            continue;
        bool reached = false;
        bool possible = false;
        GoalBlock::Status(view, hold.Goal, reached, possible);
        // True already when chosen: nothing was done to reach it, so it is held unpaid (ending on its clock) until
        // it stops being true. Without this a goal that is true on choice -- Fight about no one where there is
        // nothing to fight -- was paid at every choice: the first fast pass of stage1_move earned ~14 an episode.
        reached = GoalBlock::Earned(reached, hold.Fresh, hold.SatisfiedAtChoice);
        // Protect is also reached by keeping its friend above half health while something attacks it, for
        // Goals.ProtectHoldMs of the goal: a healer that keeps the tank up never lets it fall to be healed back.
        if (!reached && possible && SeatGoal(GoalKindOf(hold.Goal)) == SeatGoal::Protect && bot
            && !hold.Friend.IsEmpty())
            if (Unit const* friendUnit = Encoding::UnitThrough(*bot, hold.Friend); friendUnit && friendUnit->IsAlive()
                && friendUnit->GetHealthPct() >= 50.0f && !friendUnit->getAttackers().empty())
            {
                hold.ProtectSafeMs += DecisionMs();
                reached = hold.ProtectSafeMs >= _tuning.Goals.ProtectHoldMs;
            }
        if (reached && !hold.Rewarded)
        {
            // Paid by what it achieved (GoalValue; a secondary at Goals.SecondaryShare), into the reward of the
            // decision that reached it: that decision belongs to the goal's own span, where paying it at the next
            // one put it in the span of the goal chosen after -- crediting the plan that followed for the one that
            // worked.
            hold.Rewarded = true;
            ++seat.GoalsReached;
            ++seat.GoalsReachedBy[GoalKindOf(hold.Goal)];
            float const value = GroupHealer(env, seat) && SeatGoal(GoalKindOf(hold.Goal)) == SeatGoal::Fight ? 0.0f
                : GoalValue(hold, bot) * (slot ? _tuning.Goals.SecondaryShare : 1.0f);
            float const paid = seat.Rewards.AddTaken(RewardTerm::GoalReached, value);
            if (float* reward = Data(env).StepReward)
                reward[seatIndex] += paid;
        }
        else if (!possible)
            ++seat.GoalsLost;
        hold.Ended = reached || !possible;
        hold.WasReached = reached;
        if (slot && hold.Ended)
            view.Goal2Ended = true;
    }
    view.GoalEnded = seat.Holds[0].Ended;
    view.GoalReached = seat.Holds[0].Ended && seat.Holds[0].WasReached;
    for (GoalHold& hold : seat.Holds)
    {
        hold.HasPlace = hold.Goal != NO_GOAL && GoalBlock::PlaceOf(view, GoalTargetOf(hold.Goal), hold.Place);
        hold.Friend.Clear();
        if (uint32 const goalTarget = GoalTargetOf(hold.Goal); hold.Goal != NO_GOAL
            && goalTarget >= GOAL_TARGET_FRIEND_FIRST && goalTarget < GOAL_TARGET_OBJECTIVE_FIRST)
            if (Unit* friendUnit = Encoding::FriendUnit(view, goalTarget - GOAL_TARGET_FRIEND_FIRST))
                hold.Friend = friendUnit->GetGUID();
        // A new goal's progress starts here, with its place and friend known.
        if (hold.Goal != NO_GOAL && !hold.PotentialReady && bot)
        {
            hold.Potential = GoalPotential(env, seat, hold, bot, target);
            hold.PotentialReady = true;
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            hold.ChoiceResource = std::min(bot->GetHealthPct() / 100.0f,
                maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f);
            // Chosen while dead: a rise gives half of everything back, which is the rise's, not the recovery's --
            // measured from zero, dying would farm Recover.
            if (!bot->IsAlive())
                hold.ChoiceResource = 0.5f;
        }
    }
    // A secondary that ended is gone: the learner drops it on seeing OBS_SECONDARY_ENDED, and so does the sim.
    if (seat.Holds[1].Ended)
        seat.Holds[1] = GoalHold();

    // The event (choose again now) and what the seat achieved this decision (the hindsight columns).
    ObserveGoalSignals(env, seat, bot);
    view.GoalEvent = seat.Event;
    view.Achieved = seat.Achieved;
    view.Goal2 = seat.Holds[1].Goal;
    // A commanded arena gives the seat a new goal on its clock or when the one given ended, shown as an order is.
    if (Arena(env).CommandedGoals && bot && bot->IsAlive())
    {
        constexpr uint32 COMMAND_EVERY_MS = 4000;
        if (seat.Commanded == NO_GOAL || seat.Holds[0].Ended
            || env.EpisodeElapsedMs >= seat.CommandedAtMs + COMMAND_EVERY_MS)
        {
            std::array<bool, GOAL_COUNT> kinds;
            std::array<bool, GOAL_TARGETS> targets;
            GoalBlock::Available(view, kinds, targets);
            std::vector<int32> offered;
            for (uint32 kind = 0; kind < GOAL_COUNT; ++kind)
                for (uint32 target = 0; kinds[kind] && target < GOAL_TARGETS; ++target)
                    if (targets[target] && GoalAccepts(SeatGoal(kind), target))
                        offered.push_back(MakeGoal(SeatGoal(kind), target));
            seat.Commanded = offered.empty() ? MakeGoal(SeatGoal::Fight, GOAL_TARGET_NONE)
                : offered[urand(0, uint32(offered.size()) - 1)];
            seat.CommandedAtMs = env.EpisodeElapsedMs;
        }
        view.OrderGoal = seat.Commanded;
    }
    else
        view.OrderGoal = seat.Holds[0].FromOrder ? seat.Holds[0].Goal : NO_GOAL;
    SeatEncoder::AddObserve(SeatEncoder::OBSERVE_VIEW, uint64(std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - viewMark).count()));
    view.Image = image;
    SeatEncoder::Observe(view, obs, mask);
    // What the frame showed of the objective: its flagged pixels, and the first frame with any (seek's measures).
    if (image && view.HasObjective)
    {
        seat.ObjectivePixels = Vision::CountObjectivePixels(image, _spec.ImageBytes);
        if (seat.ObjectivePixels && !seat.ObjectiveSighted)
        {
            seat.ObjectiveSighted = true;
            seat.ObjectiveSightMs = env.EpisodeElapsedMs;
        }
    }
    else
        seat.ObjectivePixels = 0;

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

    // The same action again within the window, past the free ones. Whether it is charged is the intent verdict's
    // call (JudgePress, SettleIntent): a press that served the seat's goal is never a repeat -- a caster's rotation
    // is one nuke over and over, and charging that charged the correct play (stage1_duel at 10M: warlock_dps at 39.7
    // repeated presses an episode and mage_dps at 16.6 were the two lowest scoring) -- and one that did not is,
    // movement included. A press nothing judges (an order to a pet already obeying, a target selected again, a
    // stance pressed twice) is charged when it did nothing, as before.
    seat.PendingRepeat = false;
    if (seat.PressTimes.size() != layout.NumActions)
        seat.PressTimes.assign(layout.NumActions, {});

    std::vector<uint32>& presses = seat.PressTimes[action];
    std::erase_if(presses, [now, &pacing](uint32 pressed) { return pressed + pacing.RepeatWindowMs <= now; });
    presses.push_back(now);
    if (presses.size() <= pacing.RepeatFree)
        return;

    bool const judged = didSomething || GetBlock(*block).IsMovement(action - layout.Slice(*block).ActionFirst);
    if (judged)
        seat.PendingRepeat = true;          // settled by the verdict
    else
    {
        ++seat.StepRepeats;
        ++seat.RepeatedPresses;
    }
}

float Animus::Curriculum::StageScenario::GoalGap(SeatState const& seat, Player* bot, Unit const* target) const
{
    // Where either goal wants the seat: a step toward the nearer serves, and standing where one wants it is not
    // fidgeting.
    float nearest = -1.0f;
    for (GoalHold const& hold : seat.Holds)
        if (float const gap = GoalGap(seat, hold, bot, target); gap >= 0.0f && (nearest < 0.0f || gap < nearest))
            nearest = gap;
    return nearest;
}

float Animus::Curriculum::StageScenario::GoalGap(SeatState const& seat, GoalHold const& hold, Player* bot,
    Unit const* target) const
{
    if (!bot || !bot->IsAlive() || !seat.L || hold.Goal == NO_GOAL)
        return -1.0f;

    SeatGoal const goal = SeatGoal(GoalKindOf(hold.Goal));
    // A goal about a place: the yards still to go to it.
    if ((goal == SeatGoal::TravelTo || goal == SeatGoal::Gather || goal == SeatGoal::Interact) && hold.HasPlace)
        return std::max(0.0f, bot->GetExactDist2d(&hold.Place) - GoalBlock::PLACE_REACH);
    if ((goal != SeatGoal::Fight && goal != SeatGoal::Position) || !target || !target->IsAlive())
        return -1.0f;

    // Melee wants to be in reach; ranged wants to be out of melee and inside its range, with the slack Position
    // allows (GoalHeld). Zero inside the band, the yards to its nearer edge outside it.
    float const wanted = Animus::Curriculum::CombatReward::DesiredRange(seat, _tuning.Duel);
    if (wanted <= _tuning.Duel.MeleeRange)
        return bot->IsWithinMeleeRange(target) ? 0.0f : std::max(0.0f, bot->GetExactDist(target) - _tuning.Duel.MeleeRange);

    float const distance = bot->GetExactDist(target);
    if (distance < _tuning.Duel.MeleeRange)
        return _tuning.Duel.MeleeRange - distance;
    return std::max(0.0f, distance - (wanted + GOAL_RANGE_SLACK_YARDS));
}

float Animus::Curriculum::StageScenario::GoalPotential(Env const& env, SeatState const& seat, GoalHold const& hold,
    Player* bot, Unit const* target) const
{
    if (hold.Goal == NO_GOAL || !bot)
        return 0.0f;

    auto const far = [](float yards) { return -std::min(yards, 60.0f) / 60.0f; };
    SeatGoal const kind = SeatGoal(GoalKindOf(hold.Goal));
    if (!bot->IsAlive())
    {
        // Dead: standing up again is what closes anything. A ghost closes half of it on the way back to its corpse;
        // what rising gives back of Recover's pool is Recover's.
        if (kind == SeatGoal::Resurrect && GoalTargetOf(hold.Goal) == GOAL_TARGET_NONE)
        {
            Corpse const* corpse = bot->GetCorpse();
            return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) && corpse && corpse->IsInMap(bot)
                ? -0.5f + 0.5f * far(bot->GetExactDist2d(corpse)) : -1.0f;
        }
        return kind == SeatGoal::Recover || kind == SeatGoal::Rest ? -1.0f : 0.0f;
    }

    auto const health = [](Unit const* unit)
    {
        return unit && unit->IsAlive() ? float(unit->GetHealth()) / float(std::max<uint32>(1, unit->GetMaxHealth()))
            : 0.0f;
    };
    uint32 const goalTarget = GoalTargetOf(hold.Goal);
    bool const namesEnemy = goalTarget >= GOAL_TARGET_ENEMY_FIRST && goalTarget < GOAL_TARGET_FRIEND_FIRST;
    switch (SeatGoal(GoalKindOf(hold.Goal)))
    {
        case SeatGoal::Fight:
        {
            if (namesEnemy)
                return -health(env.FindTargetUnit(goalTarget - GOAL_TARGET_ENEMY_FIRST));
            // About no one: what is left of every enemy in the fight.
            float left = 0.0f;
            uint32 enemies = 0;
            for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
                if (Unit const* enemy = env.FindTargetUnit(slot))
                {
                    left += health(enemy);
                    ++enemies;
                }
            return enemies ? -left / float(enemies) : 0.0f;
        }
        case SeatGoal::Control:
        {
            Unit const* enemy = namesEnemy ? env.FindTargetUnit(goalTarget - GOAL_TARGET_ENEMY_FIRST) : nullptr;
            return enemy && enemy->IsAlive() && !Encoding::IsCrowdControlled(enemy) ? -1.0f : 0.0f;
        }
        case SeatGoal::Recover:
        case SeatGoal::Rest:
        {
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            return std::min(bot->GetHealthPct() / 100.0f,
                maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f) - 1.0f;
        }
        case SeatGoal::Protect:
        {
            Unit const* friendUnit = hold.Friend.IsEmpty() ? nullptr : Encoding::UnitThrough(*bot, hold.Friend);
            return friendUnit && friendUnit->IsAlive() ? health(friendUnit) - 1.0f : 0.0f;
        }
        case SeatGoal::Position:
        {
            float const gap = GoalGap(seat, hold, bot, target);
            return gap > 0.0f ? far(gap) : 0.0f;
        }
        case SeatGoal::TravelTo:
            return hold.HasPlace ? far(bot->GetExactDist(&hold.Place)) : 0.0f;
        case SeatGoal::Resurrect:
        {
            // A dead friend raised; standing itself up is over once alive.
            Unit const* friendUnit = hold.Friend.IsEmpty() ? nullptr : Encoding::UnitThrough(*bot, hold.Friend);
            return friendUnit && !friendUnit->IsAlive() ? -1.0f : 0.0f;
        }
        default:
            return 0.0f;
    }
}

float Animus::Curriculum::StageScenario::GoalValue(GoalHold const& hold, Player* bot) const
{
    CurriculumTuning::GoalTuning const& tuning = _tuning.Goals;
    switch (SeatGoal(GoalKindOf(hold.Goal)))
    {
        case SeatGoal::Fight:    return tuning.FightValue;
        case SeatGoal::Control:  return tuning.ControlValue;
        case SeatGoal::Protect:  return tuning.ProtectValue;
        case SeatGoal::TravelTo: return tuning.TravelValue;
        case SeatGoal::Loot:
        case SeatGoal::Gather:
        case SeatGoal::Interact: return tuning.WorldValue;
        case SeatGoal::Recover:
        case SeatGoal::Rest:
        {
            // By what it restored since it was chosen: recovering from half costs half a pool's worth of pay.
            if (!bot)
                return 0.0f;
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            float const now = std::min(bot->GetHealthPct() / 100.0f,
                maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f);
            return tuning.RecoverValue * std::max(0.0f, now - hold.ChoiceResource);
        }
        default:
            return tuning.Reached;
    }
}

void Animus::Curriculum::StageScenario::ObserveGoalSignals(Env const& env, SeatState& seat, Player* bot) const
{
    seat.Event = false;
    seat.Achieved = NO_GOAL;
    if (!bot || !bot->IsAlive())
        return;

    // Achieved, whatever the seat pursued: an enemy in one of its named slots (the goal space's) died since the last
    // decision, else its own health and mana came back past Recover's line. The learner trains the actions it took
    // as if it had meant it.
    std::array<uint8, NAMED_ENEMY_SLOTS> alive{};
    uint32 enemies = 0;
    for (uint32 slot = 0; slot < PACK_SLOTS && slot < env.Targets.size(); ++slot)
        if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
        {
            if (slot < NAMED_ENEMY_SLOTS)
                alive[slot] = 1;
            if (enemy->IsInCombat())
                ++enemies;
        }
    for (uint32 slot = 0; slot < NAMED_ENEMY_SLOTS && seat.Achieved == NO_GOAL; ++slot)
        if (seat.EnemySeenAlive[slot] && !alive[slot])
            seat.Achieved = MakeGoal(SeatGoal::Fight, GOAL_TARGET_ENEMY_FIRST + slot);
    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    float const resource = std::min(bot->GetHealthPct() / 100.0f,
        maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f);
    bool const below = resource < 0.8f;
    if (seat.Achieved == NO_GOAL && seat.BelowRecover && !below && !bot->IsInCombat())
        seat.Achieved = MakeGoal(SeatGoal::Recover, GOAL_TARGET_NONE);
    seat.EnemySeenAlive = alive;
    seat.BelowRecover = below;

    // The event: something a plan should answer changed -- the seat newly below the escape line, more enemies in
    // the fight than before, or the owner newly under attack.
    constexpr float EVENT_HEALTH_PCT = 35.0f;
    bool const low = bot->GetHealthPct() < EVENT_HEALTH_PCT;
    Player const* owner = Owner(env);
    bool const ownerAttacked = owner && owner->IsAlive() && !owner->getAttackers().empty();
    seat.Event = (low && !seat.EventLow) || enemies > seat.EventEnemies || (ownerAttacked && !seat.EventOwnerAttacked);
    seat.EventLow = low;
    seat.EventEnemies = enemies;
    seat.EventOwnerAttacked = ownerAttacked;
}

char const* Animus::Curriculum::AimlessCauseName(AimlessCause cause)
{
    switch (cause)
    {
        case AimlessCause::OffFocus:         return "off_focus";
        case AimlessCause::AoeMissed:        return "aoe_missed";
        case AimlessCause::InRangeCast:      return "in_range_cast";
        case AimlessCause::UnprovokedHarm:   return "unprovoked_harm";
        case AimlessCause::HelpOffGoal:      return "help_off_goal";
        case AimlessCause::StepAway:         return "step_away";
        case AimlessCause::TargetSwitch:     return "target_switch";
        case AimlessCause::PetOffGoal:       return "pet_off_goal";
        case AimlessCause::ConsumeNotNeeded: return "consume_not_needed";
        case AimlessCause::TrapNoEnemy:      return "trap_no_enemy";
        case AimlessCause::ModeFlip:         return "mode_flip";
        case AimlessCause::ModeReverse:      return "mode_reverse";
        case AimlessCause::NeedlessMove:     return "needless_move";
        case AimlessCause::TauntOffRole:     return "taunt_off_role";
        case AimlessCause::TankModeOffRole:  return "tank_mode_off_role";
        case AimlessCause::CastFacing:       return "cast_facing";
        case AimlessCause::CastRange:        return "cast_range";
        case AimlessCause::CastSight:        return "cast_sight";
        case AimlessCause::CastMoving:       return "cast_moving";
        case AimlessCause::CastPower:        return "cast_power";
        case AimlessCause::ActRefused:       return "act_refused";
        case AimlessCause::Count:            break;
    }
    return "unknown";
}

namespace
{
    /// Actions.Aimless.<cause>.
    float AimlessPrice(Animus::Curriculum::CurriculumTuning::ActionTuning const& tuning,
        Animus::Curriculum::AimlessCause cause)
    {
        using Animus::Curriculum::AimlessCause;
        switch (cause)
        {
            case AimlessCause::OffFocus:         return tuning.AimlessOffFocus;
            case AimlessCause::AoeMissed:        return tuning.AimlessAoeMissed;
            case AimlessCause::InRangeCast:      return tuning.AimlessInRangeCast;
            case AimlessCause::UnprovokedHarm:   return tuning.AimlessUnprovokedHarm;
            case AimlessCause::HelpOffGoal:      return tuning.AimlessHelpOffGoal;
            case AimlessCause::StepAway:         return tuning.AimlessStepAway;
            case AimlessCause::TargetSwitch:     return tuning.AimlessTargetSwitch;
            case AimlessCause::PetOffGoal:       return tuning.AimlessPetOffGoal;
            case AimlessCause::ConsumeNotNeeded: return tuning.AimlessConsumeNotNeeded;
            case AimlessCause::TrapNoEnemy:      return tuning.AimlessTrapNoEnemy;
            case AimlessCause::ModeFlip:         return tuning.AimlessModeFlip;
            case AimlessCause::ModeReverse:      return tuning.AimlessModeReverse;
            case AimlessCause::NeedlessMove:     return tuning.AimlessNeedlessMove;
            case AimlessCause::TauntOffRole:     return tuning.AimlessTauntOffRole;
            case AimlessCause::TankModeOffRole:  return tuning.AimlessTankModeOffRole;
            case AimlessCause::CastFacing:
            case AimlessCause::CastRange:
            case AimlessCause::CastSight:
            case AimlessCause::CastMoving:
            case AimlessCause::CastPower:        return tuning.AimlessCastFailed;
            case AimlessCause::ActRefused:       return tuning.AimlessActRefused;
            case AimlessCause::Count:            break;
        }
        return tuning.Aimless;
    }
}

bool Animus::Curriculum::StageScenario::IsPartyTank(SeatState const& seat)
{
    return seat.DungeonRole == DUNGEON_TANK
        || (seat.DungeonRole == DUNGEON_ANY && AptitudeDemand::HoldsThePull().MetBy(seat.Apt));
}

bool Animus::Curriculum::StageScenario::PartyHasLivingTank(Env const& env, Player const* bot) const
{
    EnvState const& data = Data(env);
    if (data.ActiveSeats < 2)
        return false;
    for (uint32 other = 0; other < data.ActiveSeats; ++other)
        if (Player* mate = env.FindBot(other); mate && mate != bot && mate->IsAlive() && data.Seats[other].L
            && IsPartyTank(data.Seats[other]))
            return true;
    return false;
}

void Animus::Curriculum::StageScenario::JudgePress(Env const& env, SeatState& seat, Player* bot, Unit* target,
    uint32 action, SeatActionResult const& result) const
{
    Layout const& layout = *seat.L;
    std::optional<BlockId> const block = layout.BlockOfAction(action);
    if (!block || !bot)
        return;

    // Every press but the no-op costs a little effort: the no-op is what a player does most of the time. A steering
    // press by its angle (SeatActionResult::EffortWeight).
    seat.StepEffort += result.EffortWeight;
    ++seat.EffortPresses;
    if (bot->IsInCombat())
        ++seat.CombatPresses;

    // A press that failed for something the seat controls -- facing away, out of range or sight, a cast time on the
    // move, short of power -- whatever the goal: offered rather than masked (Encoding::SituationalFailure), so this
    // charge is how the seat learns to put it right before it presses (2026-10-04).
    // The seat's own press only: a held interrupt that fails in the same decision is not the press's doing.
    if (*block == BlockId::Core && !result.SpellCasts && result.RefusedCast)
    {
        AimlessCause failed = AimlessCause::Count;
        switch (Encoding::SituationalFailure(result.RefusedCast))
        {
            case Encoding::Situational::Facing: failed = AimlessCause::CastFacing; break;
            case Encoding::Situational::Range:  failed = AimlessCause::CastRange; break;
            case Encoding::Situational::Sight:  failed = AimlessCause::CastSight; break;
            case Encoding::Situational::Moving: failed = AimlessCause::CastMoving; break;
            case Encoding::Situational::Power:  failed = AimlessCause::CastPower; break;
            case Encoding::Situational::None:   break;
        }
        if (failed != AimlessCause::Count)
        {
            ++seat.JudgedPresses;
            ++seat.StepAimless;
            ++seat.AimlessPresses;
            ++seat.StepAimlessBy[size_t(failed)];
            ++seat.AimlessBy[size_t(failed)];
            return;
        }
    }

    // A sight block press the world refused (dungeon-curriculum I1): a remembered entity gone or out of reach, the
    // wrong kind of thing, no key item, a cast refused. Offered rather than masked, so this charge is how the seat
    // learns to go to a thing before it uses it, whatever the goal.
    if (*block == BlockId::Sight && result.ActRefused && result.ActRefused < EntityActions::REFUSALS)
    {
        ++seat.ActRefusedBy[result.ActRefused];
        ++seat.JudgedPresses;
        ++seat.StepAimless;
        ++seat.AimlessPresses;
        ++seat.StepAimlessBy[size_t(AimlessCause::ActRefused)];
        ++seat.AimlessBy[size_t(AimlessCause::ActRefused)];
        return;
    }

    auto const chargeRepeat = [&seat]
    {
        ++seat.StepRepeats;
        ++seat.RepeatedPresses;
    };

    if (seat.Holds[0].Goal == NO_GOAL)
    {
        // Nothing to judge by: a repeat is charged unless it did something, as it was before goals. A step with no
        // goal is movement, which was free.
        if (seat.PendingRepeat && !result.DidSomething() && *block != BlockId::Move)
            chargeRepeat();
        return;
    }

    // Below this share of its health the seat may act on any enemy (CoreBlock's goal escape uses the same).
    constexpr float ESCAPE_HEALTH_PCT = 35.0f;
    // An enemy hurting the seat or one of its friends: a reason to act on it whatever the goal names.
    auto const hurtingFriend = [bot](Unit const* enemy)
    {
        Unit const* victim = enemy && enemy->IsAlive() ? enemy->GetVictim() : nullptr;
        return victim && victim->IsAlive() && (victim == bot || bot->IsFriendlyTo(victim));
    };
    uint32 const local = action - layout.Slice(*block).ActionFirst;
    enum class Verdict : uint8 { Neutral, Serves, Aimless };
    struct Judgement
    {
        bool Judged = false;
        Verdict Is = Verdict::Neutral;
        AimlessCause Cause = AimlessCause::Count;
    };

    if (*block == BlockId::Move)
    {
        // A step (forward, back, a strafe, a climb or dive, a jump) is judged at the reward, by the gap it closed or
        // opened (SettleIntent). Rates, stops and walk are neutral: the jitter charge prices a head that cannot
        // settle.
        bool const step = MoveControls::IsStep(local);
        if (step && !Encoding::StandingInHazards(bot, nullptr))
            seat.MoveGap = GoalGap(seat, bot, target);
        // A repeated step waits for its verdict; a repeated stop served nothing. Turn and pitch rates are steering
        // corrections, which come in runs: the jitter charge prices the ones that undo each other.
        bool const steer = MoveControls::IsTurn(local) || MoveControls::IsPitch(local);
        if (seat.PendingRepeat && seat.MoveGap < 0.0f && !steer)
            chargeRepeat();
        seat.MoveRepeat = seat.PendingRepeat && seat.MoveGap >= 0.0f;
        return;
    }

    // The press judged against one goal held; the seat's verdict is the best over its goals (a press that serves
    // the secondary is not aimless for missing the primary: Fight A and hold B).
    auto const judgeFor = [&](GoalHold const& hold) -> Judgement
    {
        SeatGoal const goal = SeatGoal(GoalKindOf(hold.Goal));
        uint32 const goalTarget = GoalTargetOf(hold.Goal);
        Verdict verdict = Verdict::Neutral;
        AimlessCause cause = AimlessCause::Count;           // why, when aimless (derived from the goal when not set)
        bool judged = false;
        // The enemy slot the goal names, if it names one.
        int32 const namedSlot = goalTarget >= GOAL_TARGET_ENEMY_FIRST
            && goalTarget < GOAL_TARGET_ENEMY_FIRST + NAMED_ENEMY_SLOTS
            ? int32(goalTarget - GOAL_TARGET_ENEMY_FIRST) : -1;

        if (result.SpellCasts && !result.Revives)
        {
            judged = true;
            bool const hurt = bot->GetHealthPct() < 50.0f;
            bool const onSelf = result.CastAt == bot->GetGUID();
            // The enemy the goal names, else the focus; the friend it names, if any.
            Unit* named = target;
            if (goalTarget >= GOAL_TARGET_ENEMY_FIRST && goalTarget < GOAL_TARGET_FRIEND_FIRST)
                if (Unit* slotted = env.FindTargetUnit(goalTarget - GOAL_TARGET_ENEMY_FIRST))
                    named = slotted;
            bool const onFocus = named && result.CastAt == named->GetGUID();
            ObjectGuid const namedFriend = hold.Friend;     // resolved at the observation
            bool const untargeted = result.CastAt.IsEmpty();
            // An area spell has no unit to read: it serves a fight when the focus was inside its radius.
            bool const focusNear = result.CastReachesFocus;

            if (result.CastTrap)
            {
                // A trap wants something to walk into it: an enemy near that is in the fight and hurting someone, or
                // the one the goal names (Control or Fight), close. Laid anywhere else it waits for nothing.
                bool near = false;
                for (uint32 slot = 0; slot < env.Targets.size() && !near; ++slot)
                {
                    Unit* enemy = env.FindTargetUnit(slot);
                    if (!enemy || !enemy->IsAlive() || bot->GetDistance(enemy) > 30.0f)
                        continue;
                    near = hurtingFriend(enemy) || (int32(slot) == namedSlot && bot->GetDistance(enemy) <= 15.0f
                        && (goal == SeatGoal::Control || goal == SeatGoal::Fight));
                }
                if (!near && bot->getAttackers().empty())
                {
                    verdict = Verdict::Aimless;
                    cause = AimlessCause::TrapNoEnemy;
                }
                else
                    verdict = goal == SeatGoal::Control ? Verdict::Serves : Verdict::Neutral;
            }
            else if (!result.PendingInterrupt.IsEmpty() || result.BreathingCasts || (result.DefensiveCasts && hurt)
                || result.StealthOpener || !result.StealthUtilityTarget.IsEmpty())
                verdict = Verdict::Neutral;         // always a reason: a cast stopped, a breath, a hurt seat, an opener
            else if ((result.CastTaunt || result.CastTankMode) && !IsPartyTank(seat) && PartyHasLivingTank(env, bot))
            {
                // Taunting, or taking up a tank's stance, form or aura, beside a living tank: the enemies are the
                // tank's to take, and a healer or damage dealer that pulls one onto itself has made the tank's job its
                // own (2026-10-03, stage6: holy paladins pressed Hand of Reckoning four times a fight from the healer's
                // seat, and Righteous Fury -- more threat from every heal -- three and a half).
                verdict = Verdict::Aimless;
                cause = result.CastTaunt ? AimlessCause::TauntOffRole : AimlessCause::TankModeOffRole;
            }
            else if (result.CastHarmful)
            {
                switch (goal)
                {
                    case SeatGoal::Fight:
                        verdict = onFocus || (untargeted && focusNear) ? Verdict::Serves : Verdict::Aimless;
                        break;
                    case SeatGoal::Control:
                        // Control aimed at an enemy other than the focus, or an area one that lands on the fight.
                        verdict = result.CastTactical && ((!onFocus && !untargeted) || (untargeted && focusNear))
                            ? Verdict::Serves
                            : onFocus || (untargeted && focusNear) ? Verdict::Neutral : Verdict::Aimless;
                        break;
                    case SeatGoal::Position:
                        // Casting at the focus while getting to range is fine; once in range the seat is fighting
                        // and should say so. Neutral for good, Position was a goal under which no cast could ever
                        // be aimless: the 10M intent trial's rotation drill chose it 99% of the time, and its
                        // serving share fell to 0.05.
                        verdict = GoalGap(seat, hold, bot, named) > 0.0f && (onFocus || (untargeted && focusNear))
                            ? Verdict::Neutral : Verdict::Aimless;
                        break;
                    case SeatGoal::Prepare:
                        verdict = bot->IsInCombat() ? Verdict::Neutral : Verdict::Aimless;  // pulling while preparing
                        break;
                    case SeatGoal::Protect:
                    {
                        // Protecting someone in a fight is also taking down what is hitting them: the friend the goal
                        // names, or any friend but the seat when it names none. Without it a damage dealer guarding its
                        // owner served nothing all fight (the trial's companion stage read a serving share of 0.06).
                        Unit const* hit = untargeted ? nullptr : Encoding::UnitThrough(*bot, result.CastAt);
                        Unit const* victim = hit ? hit->GetVictim() : nullptr;
                        bool const onAttacker = victim && victim != bot && victim->IsAlive()
                            && (namedFriend.IsEmpty() ? bot->IsFriendlyTo(victim) : victim->GetGUID() == namedFriend);
                        verdict = onAttacker ? Verdict::Serves
                            : bot->getAttackers().empty() ? Verdict::Aimless : Verdict::Neutral;
                        break;
                    }
                    case SeatGoal::Recover:
                    case SeatGoal::Rest:
                    case SeatGoal::TravelTo:
                    case SeatGoal::Loot:
                    case SeatGoal::Gather:
                    case SeatGoal::Interact:
                    case SeatGoal::Resurrect:
                        // Starting a fight while resting, travelling or looting: unless something started it first
                        // (the mask's escape already let it through), it served nothing the seat said it wanted.
                        verdict = bot->getAttackers().empty() ? Verdict::Aimless : Verdict::Neutral;
                        break;
                    case SeatGoal::Count:
                        break;
                }
            }
            else
            {
                switch (goal)
                {
                    case SeatGoal::Protect:
                        // The friend the goal names, or any friend when it names none.
                        verdict = !onSelf && !untargeted && (namedFriend.IsEmpty() || result.CastAt == namedFriend)
                            ? Verdict::Serves : Verdict::Neutral;
                        break;
                    case SeatGoal::Recover:
                    case SeatGoal::Rest:
                        verdict = onSelf || untargeted ? Verdict::Serves : Verdict::Neutral;
                        break;
                    case SeatGoal::Prepare:
                        verdict = result.PreparationMs ? Verdict::Serves : Verdict::Neutral;
                        break;
                    case SeatGoal::Fight:
                    case SeatGoal::Control:
                        // Help on someone else while the seat said it was fighting: it should have said Protect. Not a
                        // heal: one lands only on missing health, and a healer holds Fight most of a fight -- every
                        // heal on a teammate was charged as aimless (2026-10-03, stage6 healers: Fight 83-88%).
                        verdict = !onSelf && !untargeted && !hurt && !result.HealingCasts ? Verdict::Aimless
                            : Verdict::Neutral;
                        break;
                    case SeatGoal::Resurrect:
                        // Raising the friend it named (any dead friend when it names none).
                        verdict = result.Revives && (namedFriend.IsEmpty() || result.CastAt == namedFriend)
                            ? Verdict::Serves : Verdict::Neutral;
                        break;
                    case SeatGoal::Position:
                    case SeatGoal::TravelTo:
                    case SeatGoal::Loot:
                    case SeatGoal::Gather:
                    case SeatGoal::Interact:
                    case SeatGoal::Count:
                        break;
                }
            }

            if (verdict == Verdict::Aimless && cause == AimlessCause::Count)
            {
                if (!result.CastHarmful)
                    cause = AimlessCause::HelpOffGoal;
                else if (goal == SeatGoal::Fight || goal == SeatGoal::Control)
                    cause = untargeted ? AimlessCause::AoeMissed : AimlessCause::OffFocus;
                else if (goal == SeatGoal::Position)
                    cause = AimlessCause::InRangeCast;
                else
                    cause = AimlessCause::UnprovokedHarm;
            }

        }
        else if (result.FoodUsed || result.DrinkUsed)
        {
            // Eating restores health and drinking mana: with that already nearly full it is a supply thrown away,
            // whatever the goal says. Below it, it serves recovering.
            judged = true;
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            float const full = _tuning.Actions.ConsumeFullPct;
            bool const needed = (result.FoodUsed && bot->GetHealthPct() < full)
                || (result.DrinkUsed && maxMana && 100.0f * float(bot->GetPower(POWER_MANA)) / float(maxMana) < full);
            if (!needed)
            {
                verdict = Verdict::Aimless;
                cause = AimlessCause::ConsumeNotNeeded;
            }
            else
                verdict = goal == SeatGoal::Recover || goal == SeatGoal::Prepare || goal == SeatGoal::Rest
                    ? Verdict::Serves : Verdict::Neutral;
        }
        else if (*block == BlockId::Pack && local < PACK_SLOTS)
        {
            // Selecting an enemy: the one the goal names serves; another, only if it is hurting someone (peeling it) or
            // the seat is in trouble. With no enemy named any choice is the seat's own.
            judged = true;
            Unit const* chosen = env.FindTargetUnit(local);
            if (namedSlot < 0)
                verdict = Verdict::Neutral;
            else if (int32(local) == namedSlot)
                verdict = Verdict::Serves;
            else if (hurtingFriend(chosen) || bot->GetHealthPct() < ESCAPE_HEALTH_PCT)
                verdict = Verdict::Neutral;
            else
            {
                verdict = Verdict::Aimless;
                cause = AimlessCause::TargetSwitch;
            }
        }
        else if (result.PetOrderGiven == PetOrder::Attack)
        {
            // The pet sent at the seat's target: judged like a selection.
            judged = true;
            Unit const* named = namedSlot >= 0 ? env.FindTargetUnit(uint32(namedSlot)) : nullptr;
            if (!named || !target)
                verdict = Verdict::Neutral;
            else if (target == named)
                verdict = Verdict::Serves;
            else if (hurtingFriend(target) || bot->GetHealthPct() < ESCAPE_HEALTH_PCT)
                verdict = Verdict::Neutral;
            else
            {
                verdict = Verdict::Aimless;
                cause = AimlessCause::PetOffGoal;
            }
        }
        return { judged, verdict, cause };
    };

    Judgement judgement = judgeFor(seat.Holds[0]);
    if (seat.Holds[1].Goal != NO_GOAL && judgement.Is != Verdict::Serves)
    {
        Judgement const second = judgeFor(seat.Holds[1]);
        if (second.Judged && (second.Is == Verdict::Serves || (second.Is == Verdict::Neutral
            && judgement.Is == Verdict::Aimless)))
            judgement = second;
    }
    bool const judged = judgement.Judged;
    Verdict verdict = judgement.Is;
    AimlessCause cause = judgement.Cause;
    if (result.FoodUsed || result.DrinkUsed)
        seat.StepSuppliesSpent += result.FoodUsed + result.DrinkUsed;

    if (result.SpellCasts && !result.Revives)
    {
        // An aspect, stance, form or presence changed: a standing choice, made again only when something about the
        // seat's situation is different -- in or out of a fight, mana past a threshold (Viper in and out), mounted.
        // The same situation as at the last change is a flip, and a flip back within ten seconds a reversal.
        if (action < layout.ModeGroups.size() && layout.ModeGroups[action])
        {
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            float const manaPct = maxMana ? 100.0f * float(bot->GetPower(POWER_MANA)) / float(maxMana) : 100.0f;
            uint8 const manaBand = manaPct < 30.0f ? 0 : manaPct < 80.0f ? 1 : 2;
            uint8 const situation = uint8((bot->IsInCombat() ? 1 : 0) | (manaBand << 1) | (bot->IsMounted() ? 8 : 0));
            if (seat.ModeSituation == situation)
            {
                verdict = Verdict::Aimless;
                cause = env.EpisodeElapsedMs < seat.ModeChangedMs + 10000 ? AimlessCause::ModeReverse
                    : AimlessCause::ModeFlip;
            }
            seat.ModeSituation = situation;
            seat.ModeChangedMs = env.EpisodeElapsedMs;
            ++seat.StepModeSwitches;
            ++seat.ModeSwitches;
        }
    }

    if (seat.PendingRepeat && verdict != Verdict::Serves && (judged || !result.DidSomething()))
        chargeRepeat();

    if (!judged)
        return;

    ++seat.JudgedPresses;
    if (verdict != Verdict::Aimless)
        seat.PurposefulMs = env.EpisodeElapsedMs;
    if (verdict == Verdict::Serves)
        ++seat.ServingPresses;
    else if (verdict == Verdict::Aimless)
    {
        ++seat.StepAimless;
        ++seat.AimlessPresses;
        if (cause != AimlessCause::Count)
        {
            ++seat.StepAimlessBy[size_t(cause)];
            ++seat.AimlessBy[size_t(cause)];
        }
    }
}

/// The corpse run, where death runs on (ArenaDefinition::DeathRuns): the time dead costs, a death soon after rising
/// costs more, a rise at the corpse with nothing waiting to kill it again pays, and the spirit healer's sickness
/// costs. Dying itself is the arena's own death term.
/// Every death, once, with the state that explains it -- written for the chain drill, whose first evaluation lost a
/// quarter of its seats to something no tally could see. From the reward step, which every seat takes every decision:
/// read where the seat acted it saw only deaths an episode outlived -- a shaman's, whose Reincarnation keeps it from
/// being dead for good (DeadForGood), while every other class's death ended the episode first (the M1 dry check's
/// "all shamans"). Capped per layout and stage (MayLog), so neither one class nor one stage can hide another.
void Animus::Curriculum::StageScenario::LogDeath(Env const& env, SeatState& seat, Player* bot) const
{
    if (bot && !bot->IsAlive() && !seat.DeathLogged)
    {
        seat.DeathLogged = true;
        if (MayLog(seat, LOG_DEATH, 8))
        {
            LiquidData const liquid = bot->GetMap()->GetLiquidData(bot->GetPhaseMask(), bot->GetPositionX(),
                bot->GetPositionY(), bot->GetPositionZ(), bot->GetCollisionHeight(), {});
            LOG_INFO("module.animus", "Seat died: {} level {} at {:.0f} s (env {}, {} arena, {}, map {} at ({:.1f}, "
                "{:.1f}); void deaths {} fall deaths {} this episode), under {} swimming {} liquid status {} "
                "z {:.1f} level {:.1f} form {}, breath mirror {:.2f} submerged since {} ms, self damage this step "
                "{:.2f} taken {:.2f}, breaths {} submerged {} s, action {}",
                seat.L ? seat.L->ModelName() : "?", bot->GetLevel(), float(env.EpisodeElapsedMs) / 1000.0f, env.Index,
                Arena(env).Name, env.Evaluating ? "evaluating" : "training", bot->GetMapId(), bot->GetPositionX(),
                bot->GetPositionY(), seat.VoidDeaths + seat.Link.VoidDeaths, seat.FallDeaths + seat.Link.FallDeaths,
                bot->IsUnderWater(), bot->Unit::IsInWater(), uint32(liquid.Status), bot->GetPositionZ(),
                liquid.Level, uint32(bot->GetShapeshiftForm()), float(seat.BreathSpentMs) / float(BreathMs()),
                seat.SubmergedSinceMs, seat.LastStepSelfDamage, seat.LastStepDamageTaken, seat.Breaths,
                seat.SubmergedMs / 1000, seat.Pressed);
        }
    }
}

void Animus::Curriculum::StageScenario::SettleDeath(Env& env, SeatState& seat, Player* bot)
{
    CurriculumTuning::DeathTuning const& tuning = _tuning.Death;
    SeatState::DeathRunTally& run = seat.DeathRun;
    uint64 const now = std::max<uint64>(1, env.EpisodeElapsedMs);
    if (bot && !bot->IsAlive())
    {
        if (!run.DeadSinceMs)
        {
            run.DeadSinceMs = now;
            ++run.Deaths;
            if (run.RoseAtMs && now < run.RoseAtMs + tuning.DiedAgainMs)
            {
                ++run.DiedAgain;
                seat.Rewards.Add(RewardTerm::DeathRun, -tuning.DiedAgain);
            }
        }
        run.DeadMs += _decisionMs;
        seat.Rewards.Add(RewardTerm::DeathRun, -tuning.TimeDead * DecisionScale());
    }
    else if (bot && run.DeadSinceMs)
    {
        // Risen, however it happened. Only a rise at the corpse is the seat's own choice of where.
        bool const atCorpse = run.CorpseRises > run.SafeRisesChecked;
        run.SafeRisesChecked = run.CorpseRises;
        if (atCorpse && !DeathBlock::HostilesNear(bot, bot, DeathBlock::SAFE_RISE_SEARCH, true))
        {
            ++run.SafeRises;
            seat.Rewards.Add(RewardTerm::DeathRun, tuning.SafeRise);
        }
        run.DeadSinceMs = 0;
        run.RoseAtMs = now;
    }
    if (run.StepSpiritHealer)
    {
        run.StepSpiritHealer = false;
        seat.Rewards.Add(RewardTerm::DeathRun, -tuning.SpiritHealer);
    }
    // An accept left over from a death already over is not one for the next.
    if (bot && bot->IsAlive())
        run.AcceptResurrection = false;
}

void Animus::Curriculum::StageScenario::SettleIntent(Env& env, SeatState& seat, Player* bot, Unit* target)
{
    CurriculumTuning::ActionTuning const& tuning = _tuning.Actions;

    // A step pressed this decision: did it close on where the goal wants the seat, or open the gap?
    if (seat.MoveGap >= 0.0f)
    {
        float const gap = GoalGap(seat, bot, target);
        if (gap >= 0.0f)
        {
            ++seat.JudgedPresses;
            bool const serves = gap < seat.MoveGap - tuning.IntentSlackYards;
            if (serves)
                ++seat.ServingPresses;
            else if (gap > seat.MoveGap + tuning.IntentSlackYards)
            {
                // Away from where the goal wants the seat. A step inside the band is not judged here: getting
                // behind the focus, kiting and keeping up with a moving one all look like that (Fidget prices the
                // rest).
                ++seat.StepAimless;
                ++seat.AimlessPresses;
                ++seat.StepAimlessBy[size_t(AimlessCause::StepAway)];
                ++seat.AimlessBy[size_t(AimlessCause::StepAway)];
            }
            if (seat.MoveRepeat && !serves)
            {
                ++seat.StepRepeats;
                ++seat.RepeatedPresses;
            }
        }
        seat.MoveGap = -1.0f;
    }
    seat.MoveRepeat = false;

    if (bot && bot->IsAlive())
    {
        // Moving: the controlled body is under way (the core's own motion -- a fear, a knockback's spline -- too).
        Movement::BodyState const& body = seat.Mover.Body;
        bool const moving = (seat.Mover.Started() && body.Vx * body.Vx + body.Vy * body.Vy + body.Vz * body.Vz > 0.25f)
            || !bot->movespline->Finalized();
        bool const combat = bot->IsInCombat();
        if (combat)
            seat.CombatMs += _decisionMs;

        // Starting to move again moments after stopping is the stutter a player never shows: priced as jitter,
        // weighed by how recent the stop was (MovePrice::Recency, Options.JitterDecayMs) rather than in full up to a
        // second and not at all past it. Every start is counted, and a restart within a second for the column.
        if (moving && !seat.WasMoving)
        {
            ++seat.MoveStarts;
            if (seat.StoppedAtMs)
            {
                uint32 const since = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, seat.StoppedAtMs);
                seat.StepJitter += MovePrice::Recency(since, _tuning.Options.JitterDecayMs);
                if (since < 1000)
                    ++seat.MoveStopStarts;
            }
        }
        if (!moving && seat.WasMoving)
            seat.StoppedAtMs = std::max<uint32>(1, env.EpisodeElapsedMs);
        seat.WasMoving = moving;

        // Moving in a fight while already where the goal wants the seat, with nothing underfoot -- unless the focus
        // is moving (keeping up, kiting) or a melee seat is still working its way behind it (Backstab, Shred).
        bool const focusMoving = target && (target->isMoving() || !target->movespline->Finalized());
        if (seat.FromBehind < 0)
        {
            // Whether the kit has a spell that must be cast from behind (Backstab, Ambush, Garrote, Shred, Ravage):
            // only those seats have a reason to walk round a target they are already in reach of. A tank faces its
            // target, and every other melee spec's shuffling is the fidget this charges.
            seat.FromBehind = 0;
            for (ActionCatalog::Action const& def : seat.L->Catalog().Actions())
                if (SpellInfo const* info = def.FirstRank ? sSpellMgr->GetSpellInfo(def.FirstRank) : nullptr;
                    info && info->HasAttribute(SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET))
                    seat.FromBehind = 1;
        }
        bool const gettingBehind = seat.FromBehind > 0 && target && bot->IsWithinMeleeRange(target)
            && !target->isInBack(bot);
        // Charged once it has held Actions.SettleGraceMs (MovePrice::Settled): running into the band and stopping is
        // not a fidget, and a gap flickering at the band's edge is not charged each flicker (movement-smooth C).
        if (moving && combat && !focusMoving && !gettingBehind && GoalGap(seat, bot, target) == 0.0f
            && !Encoding::StandingInHazards(bot, nullptr))
        {
            seat.FidgetHeldMs += _decisionMs;
            if (MovePrice::Settled(seat.FidgetHeldMs, tuning.SettleGraceMs))
            {
                seat.StepFidgetMs += _decisionMs;
                seat.FidgetMs += _decisionMs;
            }
        }
        else
            seat.FidgetHeldMs = 0;

        // A ranged seat moving in a fight it could stand and shoot in: its target in reach and in sight, nothing
        // underfoot, nothing in melee with it, its owner (if it has one) close. Moving stops a hunter's Auto Shot
        // and a caster's cast, which is what a player stands still to avoid. Kiting, stepping out of melee or out
        // of fire, getting back into range or sight and keeping up with the owner are untouched.
        bool const ranged = seat.L && seat.L->Profile->Specs[seat.Spec].Range != RangeBand::Melee;
        bool needless = false;
        if (ranged && moving && combat && target && target->IsAlive() && !focusMoving)
        {
            float const distance = bot->GetDistance(target);
            float const minRange = bot->getClass() == CLASS_HUNTER ? 8.0f : 0.0f;
            bool meleed = false;
            for (Unit* attacker : bot->getAttackers())
                if (attacker->IsAlive() && attacker->IsWithinMeleeRange(bot))
                    meleed = true;
            Player* owner = Owner(env);
            bool const ownerNear = !owner || !owner->IsAlive() || bot->GetDistance(owner) <= 15.0f;
            needless = distance >= minRange && distance <= 30.0f && bot->IsWithinLOSInMap(target) && !meleed
                && ownerNear && !Encoding::StandingInHazards(bot, nullptr);
            // Held Actions.SettleGraceMs first, as the fidget is.
            seat.NeedlessHeldMs = needless ? seat.NeedlessHeldMs + _decisionMs : 0;
            if (needless && MovePrice::Settled(seat.NeedlessHeldMs, tuning.SettleGraceMs))
            {
                ++seat.StepAimless;
                ++seat.AimlessPresses;
                ++seat.StepAimlessBy[size_t(AimlessCause::NeedlessMove)];
                ++seat.AimlessBy[size_t(AimlessCause::NeedlessMove)];
            }
        }
        if (!needless)
            seat.NeedlessHeldMs = 0;
    }

    // Each cause at its own price; one left without a cause (none should be) at the plain one.
    float aimless = 0.0f;
    uint32 caused = 0;
    for (size_t c = 0; c < AIMLESS_CAUSES; ++c)
    {
        aimless += AimlessPrice(tuning, AimlessCause(c)) * float(seat.StepAimlessBy[c]);
        caused += seat.StepAimlessBy[c];
    }
    aimless += tuning.Aimless * float(seat.StepAimless - std::min(seat.StepAimless, caused));
    aimless += tuning.ModeSwitch * float(seat.StepModeSwitches);
    seat.Rewards.Add(RewardTerm::Aimless, -aimless);
    seat.Rewards.Add(RewardTerm::Effort, -tuning.Effort * float(seat.StepEffort)
        - tuning.SupplySpent * float(seat.StepSuppliesSpent));
    seat.StepAimlessBy.fill(0);
    seat.StepModeSwitches = 0;
    seat.StepSuppliesSpent = 0;
    seat.Rewards.Add(RewardTerm::Fidget, -tuning.Fidget * float(seat.StepFidgetMs) / 1000.0f);
    seat.StepAimless = 0;
    seat.StepEffort = 0.0f;
    seat.StepFidgetMs = 0;
}

void Animus::Curriculum::StageScenario::SetExploreStarts(float share, std::vector<ExploreStart> starts)
{
    share = std::isfinite(share) ? std::clamp(share, 0.0f, 1.0f) : 0.0f;
    std::erase_if(starts,
        [](ExploreStart const& start) { return !(start.Weight > 0.0f) || !std::isfinite(start.Weight); });
    std::lock_guard<std::mutex> guard(_exploreLock);
    bool const first = _exploreStarts.empty() && !starts.empty();
    _exploreShare = share;
    _exploreStarts = std::move(starts);
    if (first)
        LOG_INFO("module.animus", "Animus forge: stage {} starts {:.0f}% of its wing runs from the {} cells the "
            "learner sent (Go-Explore)", _stage.Name, 100.0f * share, _exploreStarts.size());
}

std::optional<Animus::ExploreStart> Animus::Curriculum::StageScenario::DrawExploreStart(uint32 arena, uint32 rows) const
{
    std::lock_guard<std::mutex> guard(_exploreLock);
    if (_exploreStarts.empty() || frand(0.0f, 1.0f) >= _exploreShare)
        return std::nullopt;
    float total = 0.0f;
    for (ExploreStart const& start : _exploreStarts)
        if (start.Arena == arena && start.Tier < rows)
            total += start.Weight;
    if (total <= 0.0f)
        return std::nullopt;
    float pick = frand(0.0f, total);
    for (ExploreStart const& start : _exploreStarts)
        if (start.Arena == arena && start.Tier < rows && (pick -= start.Weight) <= 0.0f)
            return start;
    return std::nullopt;
}

bool Animus::Curriculum::StageScenario::PinEvaluationArena(uint32 pin)
{
    if (pin && (pin > _stage.Arenas.size() || !_stage.Arenas[pin - 1].EvalOnly))
    {
        LOG_ERROR("module.animus", "Animus forge: stage {} has no held-out arena {} to evaluate on", _stage.Name,
            pin - 1);
        return false;
    }
    uint32 const before = _evaluationArena.exchange(pin, std::memory_order_relaxed);
    if (before != pin)
        LOG_INFO("module.animus", "Animus forge: stage {} evaluates {}", _stage.Name,
            pin ? "held-out arena " + _stage.Arenas[pin - 1].Name : std::string("its own arenas"));
    return true;
}

void Animus::Curriculum::StageScenario::SetShapingScale(float scale)
{
    // PROGRESS comes after every update; the scale changes only when the learner's fade ladder steps.
    float const before = _shapingScale.exchange(scale, std::memory_order_relaxed);
    if (before != scale)
        LOG_INFO("module.animus", "Animus forge: stage {} pays shaping x{} (was x{})", _stage.Name, scale, before);
}

void Animus::Curriculum::StageScenario::SetCostScale(float scale)
{
    // Like the shaping scale: it changes only when the learner's cost ladder steps.
    float const before = _costScale.exchange(scale, std::memory_order_relaxed);
    if (before != scale)
        LOG_INFO("module.animus", "Animus forge: stage {} charges noise x{} (was x{})", _stage.Name, scale, before);
}

void Animus::Curriculum::StageScenario::Reward(Env& env, float* reward)
{
    // The episode's last decision: each seat's controller reports where its body is, so the stretch since its last
    // report is credited rather than lost (§5A.1 point 4). Episodes ended by an outcome end on the server's reading.
    if (env.EpisodeElapsedMs >= env.EpisodeLengthMs)
        for (uint32 index = 0; index < Data(env).Seats.size(); ++index)
            if (Player* bot = env.FindBot(index); bot && bot->IsInWorld())
            {
                Movement::PlayerLink link(bot, Data(env).Seats[index].Link);
                Data(env).Seats[index].Mover.Finish(link, env.EpisodeElapsedMs);
            }

    // Held until this decision's observation, which pays a goal it sees reached into this row.
    Data(env).StepReward = reward;
    for (Encounter* encounter : ActiveRewardOrder(env))
        encounter->BeforeRewards(env);

    Data(env).StepEngaged = false;
    for (uint32 slot = 0; slot < env.Targets.size() && !Data(env).StepEngaged; ++slot)
        if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
            Data(env).StepEngaged = true;

    // The learner's shaping and cost scales, on every seat's ledger before anything is paid this decision; the goal
    // reached at the next observation is paid at the same scales.
    float const shaping = _shapingScale.load(std::memory_order_relaxed);
    float const costs = _costScale.load(std::memory_order_relaxed);
    for (uint32 seat = 0; seat < _seatCount; ++seat)
    {
        Data(env).Seats[seat].Rewards.SetShaping(shaping);
        Data(env).Seats[seat].Rewards.SetCosts(costs);
        // The world has ticked since the last decision: its targets are asked again (DecisionTarget).
        Data(env).Seats[seat].DecisionTargetKnown = false;
    }
    if (_castOwner)
        Data(env).Seats[OwnerAgent()].DecisionTargetKnown = false;

    for (uint32 seat = 0; seat < _seatCount; ++seat)
        reward[seat] = SeatReward(env, seat);

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

Unit* Animus::Curriculum::StageScenario::TrackSeatStep(Env& env, uint32 seatIndex, Player* bot)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return nullptr;

    seat.LastStepDamage = float(env.StepStats[seatIndex].Damage) / seat.DamageScale;
    Unit* target = DecisionTarget(env, seatIndex);
    seat.CurrentTargetGuid = target ? target->GetGUID() : ObjectGuid::Empty;
    seat.LastStepDamageTaken = bot
        ? float(env.StepStats[seatIndex].DamageTaken) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;
    seat.LastStepSelfDamage = bot
        ? float(env.StepStats[seatIndex].SelfDamage) / float(std::max<uint32>(1, bot->GetMaxHealth())) : 0.0f;
    TrackSupport(env, seatIndex, bot);
    return target;
}

/// The nearest ground effect the seat is not in yet, so it can be walked around rather than only walked out of.
/// The grid search runs every Encoding::HAZARD_SEARCH_MS; between searches the cached hazard is measured against the seat's
/// own position again, which is exact because a ground effect stays where it was cast.
void Animus::Curriculum::StageScenario::TrackHazards(Env const& env, SeatState& seat, Player* bot)
{
    // The shared tracker (the realm's companion keeps it the same way), measured from the seat's body (§5A.1).
    Movement::BodyState const& body = seat.Mover.Body;
    Position const self = seat.Mover.Started() ? Position(body.X, body.Y, body.Z) : bot->GetPosition();
    Encoding::TrackNearestHazard(bot, self, seat.Facing, env.EpisodeElapsedMs, seat.NearestHazard,
        seat.HazardSearchMs);
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

    // The seat's own motion, from its body (§5A.1 point 1): a client knows how fast it is going.
    Movement::BodyState const& body = seat.Mover.Body;
    bool const own = seat.Mover.Started();
    Position const self = own ? Position(body.X, body.Y, body.Z) : bot->GetPosition();
    float const x = self.GetPositionX();
    float const y = self.GetPositionY();
    if (seat.MotionHasLast)
    {
        float const dx = x - seat.MotionLastX;
        float const dy = y - seat.MotionLastY;
        seat.MotionTravelled += std::sqrt(dx * dx + dy * dy);
    }
    seat.MotionLastX = x;
    seat.MotionLastY = y;
    seat.MotionHasLast = true;

    float const range = target ? self.GetExactDist2d(target) : -1.0f;
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

    // At most every seat and the owner: a fixed array, not a vector per seat per decision.
    std::array<Friend, MAX_SEATS + 1> friendList;
    uint32 friendCount = 0;
    friendList[friendCount++] = { bot, -1, int32(seatIndex) };
    if (Player* owner = Owner(env))
        friendList[friendCount++] = { owner, 0, -1 };
    for (uint32 other = 0; other < _seatCount && friendCount < friendList.size(); ++other)
        if (Player* teammate = other != seatIndex && Data(env).Seats[other].L ? SeatBotInWorld(env, other) : nullptr)
            friendList[friendCount++] = { teammate, -1, int32(other) };
    auto const friends = std::span<Friend const>(friendList.data(), friendCount);

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
    std::vector<SeatState::AbsorbTrack>& now = seat.AbsorbScratch;
    now.clear();
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

    seat.Absorbs.swap(now);
    if (low && bot->IsAlive())
        seat.LowHealthMs += _decisionMs;
}

bool Animus::Curriculum::StageScenario::GroupHealer(Env const& env, SeatState const& seat) const
{
    return Arena(env).PartyGroup && seat.L && seat.Spec < seat.L->Profile->Specs.size()
        && seat.L->Profile->Specs[seat.Spec].Stats == StatProfile::Healer;
}

bool Animus::Curriculum::StageScenario::GroupTank(Env const& env, SeatState const& seat) const
{
    return Arena(env).PartyGroup && seat.L && seat.Spec < seat.L->Profile->Specs.size()
        && seat.L->Profile->Specs[seat.Spec].Stats == StatProfile::Tank;
}

float Animus::Curriculum::StageScenario::SeatReward(Env& env, uint32 seatIndex)
{
    SeatState& seat = Data(env).Seats[seatIndex];
    if (!seat.L)
        return 0.0f;        // an empty party seat

    Player* bot = env.FindBot(seatIndex);

    // A group's healer heals and only heals: none of the damage it deals is paid, whichever encounter pays damage
    // (2026-10-02: "The healer role should ideally not be rewarded for any damage whatever. Their job is to heal and
    // heal only"). A group's tank is paid for holding the enemies far more than for hitting them: its damage at
    // Party.TankDamageShare. The group's kills, clears and wipes are still theirs, as every seat's are.
    seat.Rewards.Scale(RewardTerm::DamageDealt, GroupHealer(env, seat) ? 0.0f
        : GroupTank(env, seat) ? _tuning.Party.TankDamageShare : 1.0f);

    // Before any encounter's reward, which read them: the step's damage dealt and taken (the pulls' and duel's damage
    // taken, the owner's tank refund), who this seat is actually fighting (asked of the encounters once a decision
    // and remembered for the const readers; the other seat in self-play, which no target slot holds), and what its
    // absorbs soaked on the owner and teammates.
    Unit* target = TrackSeatStep(env, seatIndex, bot);

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

    LogDeath(env, seat, bot);
    if (Arena(env).DeathRuns)
        SettleDeath(env, seat, bot);

    // The combat clock (Output.Clock): every second an engaged enemy lives costs every seat, dead or alive.
    if (Arena(env).Seats != SeatPlan::Raid && _tuning.Output.Clock > 0.0f)
    {
        if (Data(env).StepEngaged)
            seat.Rewards.Add(RewardTerm::CombatClock, -_tuning.Output.Clock * float(_decisionMs) / 1000.0f);
    }

    for (Encounter* encounter : ActiveRewardOrder(env))
        encounter->Reward(env, seatIndex, bot, seat.Rewards);

    seat.Rewards.Add(RewardTerm::Repeat, -_tuning.Actions.Repeat * float(seat.StepRepeats));
    seat.StepRepeats = 0;
    // Before the jitter charge: a stop-then-start settled here is charged with it.
    SettleIntent(env, seat, bot, target);
    seat.Rewards.Add(RewardTerm::Jitter, -_tuning.Actions.Jitter * seat.StepJitter);
    seat.StepJitter = 0.0f;

    // The goal the learner is pursuing, and whether this decision went with it.
    if (seat.Holds[0].Goal != NO_GOAL)
    {
        std::size_t const kind = std::size_t(GoalKindOf(seat.Holds[0].Goal));
        ++seat.GoalDecisions[kind];
        if (GoalTargetOf(seat.Holds[0].Goal) != GOAL_TARGET_NONE)
            ++seat.GoalTargetedDecisions;
        if (GoalHeld(env, seatIndex, bot, target))
            ++seat.GoalMatches[kind];
    }

    // Reaching the goal is paid once, at the observation that sees it (ObserveSeat), not for sitting in it: a
    // ranged seat held SeatGoal::Position by standing at its range, and paying that every decision made keeping
    // away from the fight the second largest earner in the stage (2026-09-17: +0.93 an episode). Closing on it is
    // paid here, potential-based: what moving toward it earns, moving away gives back.
    // The secondary at Goals.SecondaryShare, and charged Goals.Secondary for being held at all.
    for (uint32 slot = 0; slot < GOAL_SLOTS; ++slot)
    {
        GoalHold& hold = seat.Holds[slot];
        if (hold.Goal == NO_GOAL || !hold.PotentialReady || hold.Ended)
            continue;
        float const potential = GoalPotential(env, seat, hold, bot, target);
        // A healer's Fight goal is damage by another name: none of it is paid.
        bool const unpaid = GroupHealer(env, seat) && SeatGoal(GoalKindOf(hold.Goal)) == SeatGoal::Fight;
        float const paid = unpaid ? 0.0f : _tuning.Goals.Progress * (slot ? _tuning.Goals.SecondaryShare : 1.0f)
            * (_tuning.Goals.ProgressGamma * potential - hold.Potential);
        seat.Rewards.Add(RewardTerm::GoalProgress, paid);
        hold.Potential = potential;
    }
    if (seat.Holds[1].Goal != NO_GOAL)
    {
        ++seat.SecondaryDecisions;
        seat.Rewards.Add(RewardTerm::GoalSwitch, -_tuning.Goals.Secondary);
    }

    seat.Rewards.Add(RewardTerm::GoalSwitch, -_tuning.Goals.Switch * float(seat.StepGoalSwitches));
    seat.StepGoalSwitches = 0;

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
        Player* bot = SeatBotInWorld(env, seat);
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

    // The enemies: the env's targets.
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

    // The owner's row likewise: what happened to the owner is the seats' columns (owner_deaths and the rest),
    // and a zero `present` keeps the row out of every per-seat metric.
    if (_castOwner)
    {
        float* row = info + OwnerAgent() * _spec.EpisodeInfoDim;
        std::fill(row, row + _spec.EpisodeInfoDim, 0.0f);
    }
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
