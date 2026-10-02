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
#include "IncomingSpell.h"
#include "CombatReward.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "Log.h"
#include "Map.h"
#include "Opponents.h"
#include "PetBlock.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SupportBlock.h"
#include "SpellAuraEffects.h"
#include "Supplies.h"
#include "Containers.h"
#include <algorithm>
#include <array>

namespace
{
    constexpr uint32 HIGHEST_OPPONENT_LEVEL = 83;
    constexpr float PULL_TIME_SCALE_MS = 60000.0f;      // observation and fast-pull scale
    constexpr float QUIET_TIME_SCALE_MS = 20000.0f;
    constexpr float NEXT_PULL_SCALE_MS = 20000.0f;
    constexpr float ARRIVAL_SCALE_MS = 30000.0f;
    constexpr float READY_LOW_HEALTH = 0.5f;            // a pull engaged below this health or mana was started low
    constexpr float READY_LOW_MANA = 0.3f;
    constexpr uint8 HIGHER_LEVEL_STEP = 10;             // a higher-level pull is up to one level more per this many
    constexpr int32 MEAL_LEFT_SLACK_MS = 500;           // a meal ending with more than a decision and this left was cut
    constexpr float MEAL_FULL = 0.95f;                  // ... unless what it restores was already this full

    /// A single pack's rungs, climbed per class/role (DifficultyLadder): more creatures, then something on the
    /// ground, then an elite, then a level more. Every rung has a spellcaster (OpponentPool::RandomCaster), so there
    /// is always a cast to interrupt, and the upper rungs have a hazard caster (RandomHazardCaster), so there is
    /// something to step out of -- without one, nothing in the curriculum ever puts anything on the ground and the
    /// hazard features and charge read zero everywhere. The others are any pack creature, casters included.
    struct PackRung
    {
        uint8 Casters;
        uint8 Others;
        uint8 Elites;
        uint8 Levels;       // above the seat's
        uint8 Hazards = 0;  // casters that put something on the ground to walk out of (RandomHazardCaster)
    };

    constexpr std::array<PackRung, 6> PACK_RUNGS =
    {{
        { 1, 1, 0, 0, 0 },  // 2 creatures
        { 1, 2, 0, 0, 0 },  // 3
        { 1, 3, 0, 0, 0 },  // 4
        { 1, 2, 0, 0, 1 },  // 4, one of them putting something on the ground
        { 1, 1, 1, 0, 1 },  // 4 with an elite and a hazard
        { 1, 1, 1, 1, 1 },  // 4 with an elite and a hazard, a level above
    }};

    /// A raid's rungs (SeatPlan::Raid). A raid is not a bigger party: it is many seats around one large enemy, so
    /// difficulty comes from what the enemy is -- elite, levels above, something on the ground -- rather than from
    /// the count, which PACK_SLOTS caps at what a seat can observe anyway. The seats outnumber the enemies by design;
    /// what is being trained is a raid's coordination against a fight that punishes standing in the wrong place, not
    /// a brawl.
    constexpr std::array<PackRung, 6> RAID_RUNGS =
    {{
        { 0, 0, 1, 1, 0 },  // one elite, a level above: the stand-in boss
        { 0, 1, 1, 1, 1 },  // ... with an add and something on the ground
        { 1, 1, 1, 2, 1 },  // ... a caster too, two levels above
        { 0, 1, 2, 2, 1 },  // two elites
        { 1, 0, 2, 3, 1 },  // ... three levels above
        { 0, 1, 2, 3, 2 },  // two elites and two hazards to stand clear of
    }};

    static_assert(RAID_RUNGS.size() == PACK_RUNGS.size(), "one ladder indexes both rung tables");

    /// A party's drill rungs (a proper party against a single pack, ArenaDefinition::ProperParty): what a group of
    /// five at the dungeons' levels meets, from a small pack to elites a level above, so holding a pull, keeping a
    /// group up and killing the tank's target are learned before a dungeon asks for all of them at once.
    constexpr std::array<PackRung, 6> PARTY_RUNGS =
    {{
        { 1, 1, 0, 0, 0 },  // 2
        { 1, 2, 0, 0, 0 },  // 3
        { 1, 2, 1, 0, 0 },  // 4 with an elite
        { 1, 0, 2, 0, 1 },  // 4: two elites and something on the ground
        { 1, 0, 2, 1, 1 },  // ... a level above
        { 0, 0, 3, 1, 1 },  // four: three elites a level above, and a hazard caster
    }};

    static_assert(PARTY_RUNGS.size() == PACK_RUNGS.size(), "one ladder indexes the party's rungs too");

    /// A camp's rungs (PullSchedule::Camp, the pull drill), climbed on their own ladder: more packs, closer together,
    /// then elites among them, two packs that are one pull, a patrol walking through, and a level more. Each pack is
    /// a caster and the rest pack creatures; the elites take a pack each from the first. Far enough apart, one pack is
    /// pulled at a time by anyone; close, only by a tank that pulls back to where the party stands -- and not while
    /// the patrol is passing.
    struct CampRung
    {
        uint8 Packs;
        uint8 Size;         // creatures a pack
        uint8 Elites;       // in the whole camp
        uint8 Levels;       // above the seat's
        float Spacing;      // yards from one pack to the next, at least
        bool Linked = false;    // the second and third packs stand apart but are one pull: either brings the other
        bool Patrol = false;    // a pair more walks the camp from its first pack to its last and back
    };

    constexpr std::array<CampRung, 6> CAMP_RUNGS =
    {{
        { 2, 2, 0, 0, 30.0f },                  // two pairs, well apart
        { 3, 2, 0, 0, 25.0f },                  // three pairs
        { 3, 2, 1, 0, 20.0f },                  // ... closer, one with an elite
        { 3, 2, 1, 0, 20.0f, true },            // ... two of them one pull
        { 3, 2, 2, 0, 18.0f, false, true },     // two with an elite, and a patrol
        { 3, 2, 2, 1, 15.0f, true, true },      // linked and patrolled, closer still, a level above
    }};
    constexpr uint32 PATROL_SIZE = 2;
    constexpr uint32 PATROL_POINT_ID = 41;
    constexpr float PATROL_FOLLOW_YARDS = 2.0f;

    static_assert(CAMP_RUNGS.size() == PACK_RUNGS.size(), "the pack rungs' top (Pulls.MaxTier) caps the camp's too");
    constexpr float CAMP_SPACING_SPREAD = 5.0f;     // yards a pack may stand past its rung's spacing
    constexpr float CAMP_BEARING_SPREAD = 1.0f;     // radians a pack may turn off the line from the party to the first

    /// A planned run's pulls (PullSchedule::Sequence), in order: the same fights every episode, ending on a pack
    /// that cannot be walked into without something saved for it. A seat that spends everything on the first pull
    /// arrives at the last one with nothing, which is the whole point of the stage.
    constexpr std::array<PackRung, 8> SEQUENCE_PULLS =
    {{
        { 1, 1, 0, 0 },     // 2: an opener
        { 1, 2, 0, 0 },     // 3
        { 1, 2, 0, 0, 1 },  // 4, one of them putting something on the ground
        { 1, 1, 0, 0 },     // 2: a breather, if it is used as one
        { 1, 3, 0, 0 },     // 4
        { 1, 1, 1, 0, 0 },  // 3 with an elite
        { 1, 1, 1, 1, 1 },  // 4 with an elite and a hazard, a level above
        { 1, 1, 1, 2, 1 },  // the last stand: an elite pack two levels above, with something on the ground
    }};

    /// A party's planned run (PullSchedule::Sequence with a party of its own, the group stage's corridor): the same
    /// shape for five seats -- bigger pulls, elites from the third, and a last pull of three elites two levels up,
    /// the stand-in for a boss and its guards. Its length is SEQUENCE_PULLS', so one run ends either way.
    constexpr std::array<PackRung, 8> GROUP_PULLS =
    {{
        { 1, 2, 0, 0 },     // 3: an opener
        { 1, 3, 0, 0 },     // 4
        { 1, 1, 1, 0, 1 },  // 4 with an elite and something on the ground
        { 1, 2, 0, 0 },     // 3: a breather
        { 1, 1, 2, 0 },     // 4, two of them elites
        { 1, 0, 2, 1, 1 },  // 4: two elites and a hazard, a level above
        { 0, 1, 2, 1, 1 },  // ... again
        { 1, 0, 3, 2 },     // the last: three elites two levels above, and their caster
    }};

    /// A party's run climbs its own ladder (per class and build of seat 0, as the packs'): longer runs, fewer wipes to
    /// stand up from, then levels above. A run of fewer pulls than GROUP_PULLS takes them spread over its length, so it
    /// still opens on trash and ends on the three elites.
    struct GroupRung
    {
        uint8 Length;
        uint8 Wipes;        // wipes the party may stand up from
        uint8 Levels;       // above the seat's, on every pull
    };

    constexpr std::array<GroupRung, 6> GROUP_RUNGS =
    {{
        { 3, 3, 0 },
        { 5, 3, 0 },
        { 6, 2, 0 },
        { 8, 2, 0 },
        { 8, 1, 1 },
        { 8, 1, 2 },
    }};

    static_assert(GROUP_RUNGS.size() == PACK_RUNGS.size(), "the pack rungs' top (Pulls.MaxTier) caps the run's too");
    static_assert(GROUP_PULLS.size() == SEQUENCE_PULLS.size(), "the longest run is as long whoever runs it");

    /// The pull's creatures leave; enemy players in the slots (ambushers) stay.
    void Despawn(Animus::Env& env)
    {
        for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
            if (Creature* enemy = env.FindTarget(slot))
                enemy->DespawnOrUnsummon();

        std::erase_if(env.Targets, [](ObjectGuid const& guid) { return !guid.IsPlayer(); });
    }

    /// Whether a pull is up: creatures in the enemy slots (ambushers are not a pull).
    bool HasCreatures(Animus::Env const& env)
    {
        return std::any_of(env.Targets.begin(), env.Targets.end(), [](ObjectGuid const& guid)
        {
            return !guid.IsPlayer();
        });
    }

    float HealthFraction(Player const* bot)
    {
        return float(bot->GetHealth()) / float(std::max<uint32>(1, bot->GetMaxHealth()));
    }

    /// The mana fraction; 1 for a seat without mana.
    float ManaFraction(Player const* bot)
    {
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        return maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f;
    }

    /// The remaining time of the seat's aura of `type` (food: MOD_REGEN, drink: MOD_POWER_REGEN); -1 without one.
    int32 RegenLeftMs(Player const* bot, AuraType type)
    {
        Unit::AuraEffectList const& effects = bot->GetAuraEffectsByType(type);
        int32 left = -1;
        for (AuraEffect const* effect : effects)
            left = std::max(left, effect->GetBase()->GetDuration());
        return left;
    }

    /// The episode's time limit is reached.
    bool TimeIsUp(Animus::Env const& env)
    {
        return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    }
}

Animus::Curriculum::PullsEncounter::PullsEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "pack rung"), _campLadder(scenario, "camp rung"),
    _groupLadder(scenario, "run rung")
{
    // Load it at startup rather than on the first episode.
    Opponents::OpponentPool::Instance();
    if (AnyGauntlet())
        ConsumablePool::Instance();
}

bool Animus::Curriculum::PullsEncounter::AnyGauntlet() const
{
    return _scenario.Stage().AnyArena([](ArenaDefinition const& arena)
    {
        return arena.Schedule == PullSchedule::Gauntlet || arena.Schedule == PullSchedule::Sequence;
    });
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PullsEncounter::RewardTerms() const
{
    std::vector<RewardTerm> terms = { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken,
        RewardTerm::Casting, RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::StealthUtility,
        RewardTerm::Interrupt, RewardTerm::Kill, RewardTerm::Clear, RewardTerm::HealthKept, RewardTerm::Death,
        RewardTerm::Timeout, RewardTerm::Stall, RewardTerm::Spacing };
    if (AnyGauntlet())
        terms.push_back(RewardTerm::Readiness);

    // Control is paid for a single pack too (Pulls.SinglePackControl), so every pulls stage carries the term.
    terms.push_back(RewardTerm::Control);
    terms.push_back(RewardTerm::Ranged);
    terms.push_back(RewardTerm::PetTank);
    // A camp charges a second pack in the fight as the dungeon charges a crowd (Pulls.CampExtraPack).
    if (_scenario.Stage().AnyArena([](ArenaDefinition const& arena) { return arena.Schedule == PullSchedule::Camp; }))
        terms.push_back(RewardTerm::Threat);
    return terms;
}

bool Animus::Curriculum::PullsEncounter::SinglePack(Env const& env) const
{
    ArenaDefinition const& arena = _scenario.Arena(env);
    return (arena.Schedule == PullSchedule::SinglePack || arena.Schedule == PullSchedule::Camp) && !arena.Owner;
}

void Animus::Curriculum::PullsEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("kills", [this](Env const& env, uint32) { return float(_envs[env.Index].Kills); });
    table.Add("interrupts", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].Interrupts);
    });
    table.Add("pack_size", [this](Env const& env, uint32) { return float(_envs[env.Index].PackSize); });
    table.Add("linked", [this](Env const& env, uint32) { return _envs[env.Index].Linked ? 1.0f : 0.0f; });

    // Crowd control: enemy-time held out of the fight, and what that time saved in the seat's own maximum healths.
    // Both are reported whether or not control is paid for, so a run says what control would have been worth.
    table.Add("control_seconds", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].ControlMs) / 1000.0f;
    });
    table.Add("control_prevented", [this](Env const& env, uint32 seat)
    {
        return _envs[env.Index].Seats[seat].ControlPrevented;
    });

    // The single pack's rung, as the creature duel's tier (a stage with both reports the duel's, and a stage with
    // a real instance reports the boss ladder's: the raid stages' single pack is their control arena).
    auto const singlePack = [](ArenaDefinition const& arena)
    {
        return arena.Against == Opposition::Pulls && arena.Schedule == PullSchedule::SinglePack && !arena.Owner;
    };
    auto const creature = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Creature; };
    auto const instance = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Instance; };
    if (_scenario.Stage().AnyArena(singlePack) && !_scenario.Stage().AnyArena(creature)
        && !_scenario.Stage().AnyArena(instance))
        table.Add("difficulty", [this](Env const& env, uint32) { return float(_envs[env.Index].Rung); });

    if (AnyGauntlet())
    {
        table.Add("pulls_cleared", [this](Env const& env, uint32) { return float(_envs[env.Index].PullsCleared); });
        table.Add("food_used", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].FoodUsed);
        });
        table.Add("drink_used", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].DrinkUsed);
        });
        table.Add("sustain_casts", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].SustainCasts);
        });
        table.Add("deaths", [this](Env const& env, uint32 seat)
        {
            return float(_scenario.Data(env).Seats[seat].Combat.Deaths);
        });

        // Recovery: how ready the seat was for each pull it engaged, and how it rested between them.
        table.Add("engage_health", [this](Env const& env, uint32 seat)
        {
            SeatPull const& pull = _envs[env.Index].Seats[seat];
            return pull.PullsEngaged ? pull.EngageHealthSum / float(pull.PullsEngaged) : 1.0f;
        });
        table.Add("engage_mana", [this](Env const& env, uint32 seat)
        {
            SeatPull const& pull = _envs[env.Index].Seats[seat];
            return pull.PullsEngaged ? pull.EngageManaSum / float(pull.PullsEngaged) : 1.0f;
        });
        table.Add("pulls_started_low", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].PullsStartedLow);
        });
        table.Add("pulls_arrived", [this](Env const& env, uint32) { return float(_envs[env.Index].PullsArrived); });
        table.Add("rest_seconds", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].RestMs) / 1000.0f;
        });
        table.Add("eat_failed", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].FoodFailed);
        });
        table.Add("drink_failed", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].DrinkFailed);
        });
        table.Add("meals_cut_short", [this](Env const& env, uint32 seat)
        {
            return float(_envs[env.Index].Seats[seat].MealsCutShort);
        });
        table.Add("buff_coverage", [this](Env const& env, uint32 seat)
        {
            SeatPull const& pull = _envs[env.Index].Seats[seat];
            return pull.PullsEngaged ? pull.BuffCoverageSum / float(pull.PullsEngaged) : 0.0f;
        });
    }

    // The pull drill: how many packs were in the fight at once at most, and how many were killed on their own.
    if (_scenario.Stage().AnyArena([](ArenaDefinition const& arena) { return arena.Schedule == PullSchedule::Camp; }))
    {
        table.Add("camp_packs", [this](Env const& env, uint32) { return float(_envs[env.Index].CampPacks); });
        table.Add("camp_peak_packs", [this](Env const& env, uint32) { return float(_envs[env.Index].CampPeak); });
        table.Add("camp_clean_packs", [this](Env const& env, uint32) { return float(_envs[env.Index].CampClean); });
    }

    // A party's run: its rung on the run ladder (length, wipes to spare, levels).
    if (_scenario.Stage().AnyArena([](ArenaDefinition const& arena)
        {
            return arena.Schedule == PullSchedule::Sequence && arena.PartyGroup && arena.Seats == SeatPlan::Party;
        }))
        table.Add("run_rung", [this](Env const& env, uint32) { return float(_envs[env.Index].RunRung); });

    if (_scenario.Stage().AnyArena([](ArenaDefinition const& arena) { return arena.Owner || arena.PartyGroup; }))
        table.Add("wipes", [this](Env const& env, uint32) { return float(_envs[env.Index].Wipes); });
}

void Animus::Curriculum::PullsEncounter::ResetEpisode(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];
    std::array<SeatPull, MAX_SEATS> seats = pulls.Seats;
    pulls = EnvPulls();

    // The seats' supplies belong to their characters, which the rebuild replaces anyway.
    for (uint32 seat = 0; seat < MAX_SEATS; ++seat)
    {
        pulls.Seats[seat].FoodItem = seats[seat].FoodItem;
        pulls.Seats[seat].DrinkItem = seats[seat].DrinkItem;
    }
}

bool Animus::Curriculum::PullsEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];

    // Pulls spawn around the owner: it has to be built first (see the build order in StageScenario).
    if (_scenario.Arena(env).Owner && !_scenario.Owner(env))
    {
        LOG_ERROR("module.animus", "{}: env {} builds its pulls before its owner", _scenario.Name(), env.Index);
        return false;
    }

    for (uint32 seatIndex = 0; seatIndex < data.ActiveSeats; ++seatIndex)
    {
        SeatState& seat = data.Seats[seatIndex];
        Player* bot = _scenario.SeatBot(env, seatIndex);
        _scenario.PrepareFighter(bot, seat);

        SeatPull& supplies = pulls.Seats[seatIndex];
        supplies = SeatPull();
        if (Gauntlet(env))
        {
            ConsumablePool const& consumables = ConsumablePool::Instance();
            supplies.FoodItem = consumables.Food(seat.Level);
            supplies.DrinkItem = bot->GetMaxPower(POWER_MANA) ? consumables.Drink(seat.Level) : 0;
            StockConsumables(bot, supplies.FoodItem, supplies.DrinkItem, Supplies(env));
        }
    }

    // A party's run: its rung decides its length, its wipes to spare and its levels.
    if (GroupRun(env))
    {
        uint16 const layout = data.Seats[0].L ? data.Seats[0].L->Index : 0;
        uint8 const spec = data.Seats[0].Spec;
        DifficultyLadder::Pick const pick = _groupLadder.Draw(env, layout, spec, MaxRung(env));
        GroupRung const& rung = GROUP_RUNGS[pick.Tier];
        pulls.RunRung = pick.Tier;
        pulls.RunLength = rung.Length;
        pulls.RunWipes = rung.Wipes;
        pulls.RunLevels = rung.Levels;
        pulls.RungLayout = layout;
        pulls.RungSpec = spec;
        pulls.RunCounts = pick.Counts;
    }

    return SpawnPull(env, map);
}

uint32 Animus::Curriculum::PullsEncounter::RunLength(Env const& env) const
{
    uint32 const length = _envs[env.Index].RunLength;
    return length ? length : uint32(SEQUENCE_PULLS.size());
}

void Animus::Curriculum::PullsEncounter::RecordRun(Env& env, bool won)
{
    EnvPulls& pulls = _envs[env.Index];
    if (!pulls.RunCounts || pulls.RunRecorded)
        return;
    pulls.RunRecorded = true;
    _groupLadder.Record(pulls.RungLayout, pulls.RungSpec, pulls.RunRung, won, MaxRung(env));
}

bool Animus::Curriculum::PullsEncounter::SpawnPull(Env& env, Map* map)
{
    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    ArenaDefinition const& arena = _scenario.Arena(env);
    Opponents::OpponentPool const& pool = Opponents::OpponentPool::Instance();

    Player* lead = _scenario.SeatBot(env, 0);
    if (!lead)
        return false;

    uint8 const botLevel = data.Seats[0].Level;
    uint8 level = botLevel;
    std::vector<uint32> entries;
    pulls.EliteOrHigher = false;

    // A party's gauntlet faces dungeon-like packs: 2-4 creatures, each sometimes elite, up to 2 levels higher. A
    // drill's single pack climbs the party's rungs instead, a party's run has its own pulls and a raid its own rungs
    // (below): this branch took every grouped arena before them, so the raid rungs were never drawn.
    if (arena.PartyGroup && arena.Seats == SeatPlan::Party && arena.Schedule == PullSchedule::Gauntlet)
    {
        level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + urand(0, 2)));
        uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
        for (uint32 i = urand(2, PACK_SLOTS); i > 0; --i)
        {
            uint32 const elite = roll_chance_i(tuning.PartyEliteChance) ? pool.RandomElite(poolLevel) : 0;
            if (elite)
                pulls.EliteOrHigher = true;
            if (uint32 const entry = elite ? elite : pool.RandomPackMember(poolLevel))
                entries.push_back(entry);
        }
    }

    // A planned run: the same pull for the same position, every episode.
    if (entries.empty() && Sequence(env))
    {
        std::size_t index = std::min<std::size_t>(pulls.PullsCleared, SEQUENCE_PULLS.size() - 1);
        uint32 const length = RunLength(env);
        if (GroupRun(env) && length > 1)
        {
            std::size_t const at = std::min<std::size_t>(pulls.PullsCleared, length - 1);
            index = (at * (GROUP_PULLS.size() - 1) + (length - 1) / 2) / (length - 1);
        }
        PackRung const& planned = GroupRun(env) ? GROUP_PULLS[index] : SEQUENCE_PULLS[index];
        level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + planned.Levels + pulls.RunLevels));
        uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
        pulls.EliteOrHigher = planned.Elites || planned.Levels;
        for (uint32 i = 0; i < planned.Casters; ++i)
            if (uint32 const entry = pool.RandomCaster(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < planned.Hazards; ++i)
            if (uint32 const entry = pool.RandomHazardCaster(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < planned.Elites; ++i)
            if (uint32 const entry = pool.RandomElite(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < planned.Others; ++i)
            if (uint32 const entry = pool.RandomPackMember(poolLevel))
                entries.push_back(entry);
    }

    // A single pack is its class and role's rung on the ladder; a raid's is the raid ladder, since forty seats against
    // a pack of four is not a fight.
    if (entries.empty() && SinglePack(env))
    {
        bool const raid = arena.Seats == SeatPlan::Raid;
        uint16 const layout = data.Seats[0].L ? data.Seats[0].L->Index : 0;
        uint8 const spec = data.Seats[0].Spec;
        DifficultyLadder::Pick const pick = (Camp(env) ? _campLadder : _ladder).Draw(env, layout, spec, MaxRung(env));
        PackRung const& rung = raid ? RAID_RUNGS[pick.Tier]
            : arena.PartyGroup ? PARTY_RUNGS[pick.Tier] : PACK_RUNGS[pick.Tier];
        pulls.Rung = pick.Tier;
        pulls.RungLayout = layout;
        pulls.RungSpec = spec;
        pulls.RungCounts = pick.Counts;

        if (Camp(env))
        {
            CampRung const& camp = CAMP_RUNGS[pick.Tier];
            return SpawnCamp(env, map, camp.Packs, camp.Size, camp.Elites, camp.Levels, camp.Spacing, camp.Linked,
                camp.Patrol);
        }

        level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + rung.Levels));
        uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
        pulls.EliteOrHigher = rung.Elites || rung.Levels;
        for (uint32 i = 0; i < rung.Casters; ++i)
            if (uint32 const entry = pool.RandomCaster(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < rung.Hazards; ++i)
            if (uint32 const entry = pool.RandomHazardCaster(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < rung.Elites; ++i)
            if (uint32 const entry = pool.RandomElite(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < rung.Others; ++i)
            if (uint32 const entry = pool.RandomPackMember(poolLevel))
                entries.push_back(entry);

        // Which slot the caster takes is no tell.
        Acore::Containers::RandomShuffle(entries);
    }

    if (entries.empty() && Gauntlet(env) && arena.Seats == SeatPlan::Raid)
    {
        // Each pull of a raid's run is a raid fight, harder as the run goes on: what the seats have left when the
        // last one comes is the stage's whole question, as it is for the solo gauntlet.
        PackRung const& rung = RAID_RUNGS[std::min<std::size_t>(pulls.PullsCleared, RAID_RUNGS.size() - 1)];
        level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + rung.Levels));
        uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
        pulls.EliteOrHigher = rung.Elites || rung.Levels;
        for (uint32 i = 0; i < rung.Casters; ++i)
            if (uint32 const entry = pool.RandomCaster(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < rung.Hazards; ++i)
            if (uint32 const entry = pool.RandomHazardCaster(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < rung.Elites; ++i)
            if (uint32 const entry = pool.RandomElite(poolLevel))
                entries.push_back(entry);
        for (uint32 i = 0; i < rung.Others; ++i)
            if (uint32 const entry = pool.RandomPackMember(poolLevel))
                entries.push_back(entry);

        Acore::Containers::RandomShuffle(entries);
    }

    if (entries.empty() && Gauntlet(env) && roll_chance_i(tuning.EliteChance))
    {
        if (uint32 const elite = pool.RandomElite(botLevel))
        {
            entries.push_back(elite);
            pulls.EliteOrHigher = true;
        }
    }

    if (entries.empty())
    {
        if (Gauntlet(env) && roll_chance_i(tuning.HigherLevelChance))
        {
            // +3 is a different fight at level 5 than at 70: one level more per HIGHER_LEVEL_STEP, up to three.
            uint32 const most = std::clamp<uint32>(botLevel / HIGHER_LEVEL_STEP, 1, 3);
            level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, botLevel + urand(1, most)));
            pulls.EliteOrHigher = true;
        }

        uint32 const count = Gauntlet(env) ? urand(1, PACK_SLOTS) : urand(2, PACK_SLOTS);
        for (uint32 i = 0; i < count; ++i)
            if (uint32 const entry = pool.RandomPackMember(uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL))))
                entries.push_back(entry);
    }

    // Every pull has a rung, which its outcome terms are scaled by (CombatReward::TierScale): a single pack's is
    // its class and build's ladder rung (drawn above), a planned run's and a raid run's is the pull's index in
    // the run, and a gauntlet pull's says only whether it was an elite or a level above. The `difficulty` column
    // reports it.
    if (Sequence(env))
        pulls.Rung = uint32(std::min<std::size_t>(pulls.PullsCleared, SEQUENCE_PULLS.size() - 1));
    else if (Gauntlet(env) && arena.Seats == SeatPlan::Raid)
        pulls.Rung = uint32(std::min<std::size_t>(pulls.PullsCleared, RAID_RUNGS.size() - 1));
    else if (Gauntlet(env))
        pulls.Rung = pulls.EliteOrHigher ? 3 : 0;

    // A hazard arena puts one in every pull, whatever the rung drew. The pack ladder only reaches hazards at
    // rung 3, so a class/role that stalls below it never meets one, and elsewhere they are thin enough to be hard
    // to learn from (stage4_gauntlet averages about a second of hazard an episode). Replacing the last entry rather
    // than adding one keeps the pull the size its rung says.
    if (arena.Hazards && !entries.empty())
        if (uint32 const entry = pool.RandomHazardCaster(uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL))))
            entries.back() = entry;

    // Ambushers keep their enemy slots: the pull takes what is left.
    uint32 const room = PACK_SLOTS - std::min(PACK_SLOTS, arena.Ambushers);
    if (entries.size() > room)
        entries.resize(room);
    if (entries.empty())
        return false;

    // With an owner, pulls spawn around the owner; whoever takes part hears of the pull first (the owner decides when
    // it walks over).
    Player* anchor = _scenario.Owner(env);
    _scenario.NotifyPullStarting(env);

    std::vector<Creature*> pack = Opponents::SpawnPack(anchor ? anchor : lead, map, entries, level);
    if (pack.empty())
        return false;

    // A raid's pull is sized to the raid: health by the seats (a party's pull for every five), melee damage by the
    // groups, so there is something for forty to do and the healers have to heal. Stat modifiers, not a bare
    // SetMaxHealth, which a stat update would undo.
    if (arena.Seats == SeatPlan::Raid)
    {
        CurriculumTuning::RaidTuning const& raid = _scenario.Tuning().Raid;
        float const seats = float(std::max<uint32>(1, data.ActiveSeats));
        float const groups = std::ceil(seats / float(GROUP_SEATS));
        float const health = std::max(1.0f, seats / float(GROUP_SEATS) * raid.HealthPerGroup);
        float const damage = 1.0f + raid.DamagePerGroup * (groups - 1.0f);
        for (Creature* member : pack)
        {
            member->ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, (health - 1.0f) * 100.0f);
            member->UpdateMaxHealth();
            member->SetFullHealth();
            member->ApplyStatPctModifier(UNIT_MOD_DAMAGE_MAINHAND, TOTAL_PCT, (damage - 1.0f) * 100.0f);
            member->UpdateDamagePhysical(BASE_ATTACK);
        }
    }

    // A drill's pack can take longer to kill (ArenaDefinition::PackHealthPct): the healing drill's fights outlast a
    // mana bar.
    if (arena.PackHealthPct != 100 && arena.PackHealthPct > 0)
        for (Creature* member : pack)
        {
            member->ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(arena.PackHealthPct) - 100.0f);
            member->UpdateMaxHealth();
            member->SetFullHealth();
        }

    // The new pull replaces the old one's creatures; enemy players (ambushers) keep their slots, first.
    std::erase_if(env.Targets, [](ObjectGuid const& guid) { return !guid.IsPlayer(); });
    for (Creature* member : pack)
        env.Targets.push_back(member->GetGUID());

    if (!pulls.PullsCleared && !pulls.PackSize)
    {
        pulls.PackSize = uint32(pack.size());
        data.OpponentEntry = entries.front();
    }

    pulls.Linked = roll_chance_i(tuning.LinkedChance);
    StartPull(env);
    return true;
}

void Animus::Curriculum::PullsEncounter::StartPull(Env& env)
{
    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    pulls.PullKills = 0;
    pulls.PullStartMs = env.EpisodeElapsedMs;
    pulls.PullEngaged = false;
    pulls.Arrived = false;
    if (PullsArrive(env))
    {
        uint32 const wait = urand(tuning.ArriveMinMs, tuning.ArriveMaxMs);
        uint32 const shrink = std::min(wait, tuning.ArriveShrinkMs * pulls.PullsCleared);
        pulls.ArriveMs = env.EpisodeElapsedMs + std::max(tuning.ArriveFloorMs, wait - shrink);
    }

    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
    {
        data.Seats[seat].TargetSlot = 0;
        data.Seats[seat].Combat.LastDistance = -1.0f;
        pulls.Seats[seat].PullDamageTaken = 0;
        pulls.Seats[seat].PullControlPaid = 0.0f;
        pulls.Seats[seat].PreparationBaseMs = data.Seats[seat].Combat.PreparationMs;
        // Each pull brings its own creatures into the same slots: what the last one's did says nothing about these.
        pulls.Seats[seat].SlotDamage.fill(0);
        pulls.Seats[seat].SlotFreeMs.fill(0);
        pulls.Seats[seat].ControlledMs = 0;
        if (Player* bot = env.FindBot(seat))
        {
            pulls.Seats[seat].ReadyHealth = HealthFraction(bot);
            pulls.Seats[seat].ReadyMana = ManaFraction(bot);
        }
    }
}

bool Animus::Curriculum::PullsEncounter::SpawnCamp(Env& env, Map* map, uint32 packs, uint32 size, uint32 elites,
    uint32 levels, float spacing, bool linked, bool patrol)
{
    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    Opponents::OpponentPool const& pool = Opponents::OpponentPool::Instance();
    Player* lead = _scenario.SeatBot(env, 0);
    if (!lead)
        return false;

    packs = std::min<uint32>(packs, CAMP_PACKS - (patrol ? 1 : 0));
    uint32 const room = uint32(MAX_TARGETS) - (patrol ? PATROL_SIZE : 0);
    size = std::clamp<uint32>(size, 1, room / std::max<uint32>(1, packs));
    uint8 const level = uint8(std::min<uint32>(HIGHEST_OPPONENT_LEVEL, data.Seats[0].Level + levels));
    uint8 const poolLevel = uint8(std::min<uint32>(level, DEFAULT_MAX_LEVEL));
    pulls.EliteOrHigher = elites || levels;

    // The first pack where a single pack would stand, the rest each on from the one before, roughly away from the
    // party: a corridor of camps, the next one always further in.
    Position center = Opponents::FindSpawnPoint(lead, map);
    float const bearing = lead->GetAbsoluteAngle(&center);
    std::vector<Creature*> camp;
    pulls.CampMembers.clear();
    pulls.PatrolA = center;
    pulls.PatrolB = center;
    _scenario.NotifyPullStarting(env);
    for (uint32 pack = 0; pack < packs; ++pack)
    {
        // Nowhere to stand the next pack on from the last: the camp ends there, rather than one pack on another.
        if (pack)
        {
            std::optional<Position> const next = Opponents::FindSpawnPointFrom(lead, map, center, bearing,
                CAMP_BEARING_SPREAD, spacing, spacing + CAMP_SPACING_SPREAD);
            if (!next)
                break;
            center = *next;
        }

        std::vector<uint32> entries;
        if (uint32 const caster = pool.RandomCaster(poolLevel))
            entries.push_back(caster);
        if (pack < elites)
            if (uint32 const elite = pool.RandomElite(poolLevel))
                entries.push_back(elite);
        while (entries.size() < size)
        {
            uint32 const entry = pool.RandomPackMember(poolLevel);
            if (!entry)
                break;
            entries.push_back(entry);
        }
        entries.resize(std::min<std::size_t>(entries.size(), size));

        // Packs are numbered as they stand, so a pack that failed to spawn leaves no gap to count as cleared. Linked,
        // the third pack takes the second's number: one pull standing in two places, which is what joins them
        // (UpdateEnemies), counts them as one in a fight and reports them as one ahead.
        bool const joins = linked && pack == 2 && pulls.CampPacks == 2;
        uint8 const index = joins ? uint8(1) : pulls.CampPacks;
        for (Creature* member : Opponents::SpawnPack(lead, map, entries, level, center))
        {
            camp.push_back(member);
            pulls.CampMembers.emplace_back(member->GetGUID(), index);
        }
        if (!joins && !pulls.CampMembers.empty() && pulls.CampMembers.back().second == index)
            ++pulls.CampPacks;
        pulls.PatrolB = center;
    }
    if (camp.empty())
        return false;

    // The patrol: a pair walking from the camp's first pack to its last and back (UpdateEnemies), a pull of its own
    // that joins whatever fight it walks past.
    if (patrol && pulls.CampPacks < CAMP_PACKS && pulls.PatrolA.GetExactDist2d(&pulls.PatrolB) > spacing)
    {
        std::vector<uint32> entries;
        for (uint32 i = 0; i < PATROL_SIZE; ++i)
            if (uint32 const entry = pool.RandomPackMember(poolLevel))
                entries.push_back(entry);
        uint8 const index = pulls.CampPacks;
        for (Creature* member : Opponents::SpawnPack(lead, map, entries, level, pulls.PatrolB))
        {
            camp.push_back(member);
            pulls.CampMembers.emplace_back(member->GetGUID(), index);
        }
        if (pulls.CampMembers.back().second == index)
        {
            pulls.PatrolPack = int8(index);
            pulls.PatrolToB = false;
            ++pulls.CampPacks;
        }
    }

    ArenaDefinition const& arena = _scenario.Arena(env);
    if (arena.PackHealthPct != 100 && arena.PackHealthPct > 0)
        for (Creature* member : camp)
        {
            member->ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(arena.PackHealthPct) - 100.0f);
            member->UpdateMaxHealth();
            member->SetFullHealth();
        }

    std::erase_if(env.Targets, [](ObjectGuid const& guid) { return !guid.IsPlayer(); });
    for (Creature* member : camp)
        if (env.Targets.size() < MAX_TARGETS)
            env.Targets.push_back(member->GetGUID());
    pulls.PackSize = uint32(camp.size());
    data.OpponentEntry = camp.front()->GetEntry();
    // A pack is a pack: its members always join one another (UpdateCamp), and no pack is linked to the next.
    pulls.Linked = false;
    StartPull(env);
    return true;
}

void Animus::Curriculum::PullsEncounter::UpdateEnemies(Env& env)
{
    // A camp's packs: each pack's members join whichever of them is fighting, and only those.
    if (Camp(env))
    {
        EnvPulls const& pulls = _envs[env.Index];
        Player* seat = _scenario.SeatBot(env, 0);
        if (!seat || !seat->IsInWorld())
            return;
        std::array<Unit*, CAMP_PACKS> victims{};
        for (auto const& [guid, pack] : pulls.CampMembers)
            if (Creature* member = Encoding::CreatureThrough(*seat, guid); member && member->IsAlive()
                && member->IsInCombat() && !victims[pack])
                victims[pack] = member->GetVictim();
        for (auto const& [guid, pack] : pulls.CampMembers)
            if (Creature* member = Encoding::CreatureThrough(*seat, guid); member && member->IsAlive()
                && !member->IsInCombat() && victims[pack] && victims[pack]->IsAlive() && member->IsAIEnabled
                && member->CanCreatureAttack(victims[pack]))
                member->AI()->AttackStart(victims[pack]);

        // The patrol walks while it is out of a fight: its first member from one end of the camp to the other, the
        // rest at its heels. A patrol that fought and won goes back to walking from where it stands.
        if (pulls.PatrolPack >= 0 && !victims[pulls.PatrolPack])
        {
            Creature* leader = nullptr;
            for (auto const& [guid, pack] : pulls.CampMembers)
            {
                Creature* member = pack == pulls.PatrolPack ? Encoding::CreatureThrough(*seat, guid) : nullptr;
                if (!member || !member->IsAlive() || member->IsInCombat() || member->IsInEvadeMode())
                    continue;
                if (!leader)
                {
                    leader = member;
                    continue;
                }
                if (member->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                    member->GetMotionMaster()->MoveFollow(leader, PATROL_FOLLOW_YARDS, float(M_PI));
            }
            if (leader && leader->movespline->Finalized())
            {
                EnvPulls& walking = _envs[env.Index];
                Position const& end = walking.PatrolToB ? walking.PatrolB : walking.PatrolA;
                if (leader->GetExactDist2d(&end) < PATROL_FOLLOW_YARDS * 2.0f)
                    walking.PatrolToB = !walking.PatrolToB;
                Position const& next = walking.PatrolToB ? walking.PatrolB : walking.PatrolA;
                leader->SetWalk(true);
                leader->GetMotionMaster()->MovePoint(PATROL_POINT_ID, next.GetPositionX(), next.GetPositionY(),
                    next.GetPositionZ());
            }
        }
        return;
    }

    // Linked pulls: once one member is in combat, the rest of the pack joins in, on whoever the engaged member is
    // fighting.
    if (!_envs[env.Index].Linked)
        return;

    std::vector<Creature*> members;
    Unit* engagedVictim = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        if (Creature* member = env.FindTarget(slot); member && member->IsAlive())
        {
            members.push_back(member);
            if (member->IsInCombat() && !engagedVictim)
                engagedVictim = member->GetVictim() ? member->GetVictim() : env.FindBot(0);
        }
    }

    if (engagedVictim && engagedVictim->IsAlive())
        for (Creature* member : members)
            if (!member->IsInCombat() && member->IsAIEnabled && member->CanCreatureAttack(engagedVictim))
                member->AI()->AttackStart(engagedVictim);
}

void Animus::Curriculum::PullsEncounter::Update(Env& env)
{
    bool const hasOwner = _scenario.Arena(env).Owner;
    // Seen before Recover stands it up again: an owner death loses the episode's win.
    if (Player* owner = hasOwner ? _scenario.Owner(env) : nullptr; owner && !owner->IsAlive())
        _envs[env.Index].OwnerDied = true;
    if (Grouped(env))
        Recover(env);
    if (GroupRun(env) && TimeIsUp(env))
        RecordRun(env, false);

    EnvPulls const& pulls = _envs[env.Index];
    if (PullsArrive(env) && HasCreatures(env) && !pulls.PullEngaged && env.EpisodeElapsedMs >= pulls.ArriveMs)
        SendPull(env);

    if (!Gauntlet(env) || HasCreatures(env) || env.EpisodeElapsedMs < pulls.NextPullMs)
        return;

    // A planned run ends when its last pull has been cleared: nothing more spawns.
    if (Sequence(env) && pulls.PullsCleared >= RunLength(env))
        return;

    // The next pull once the break is over, if anyone is left to fight it.
    bool anyoneAlive = false;
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
        if (Player* bot = env.FindBot(seat); bot && bot->IsAlive())
            anyoneAlive = true;

    Player* owner = _scenario.Owner(env);
    if (anyoneAlive && (!hasOwner || (owner && owner->IsAlive())) && !_envs[env.Index].AwaitingRevive)
        if (Map* map = env.FindMap())
            SpawnPull(env, map);
}

void Animus::Curriculum::PullsEncounter::SendPull(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];
    // The seat it walks to: the only one alone, the nearest one standing in a group (a raid's seat 0 may be dead or
    // at the back).
    Player* bot = nullptr;
    Creature const* lead = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size() && !lead; ++slot)
        if (Creature* member = env.FindTarget(slot); member && member->IsAlive())
            lead = member;
    float nearest = 0.0f;
    for (uint32 seat = 0; seat < (Grouped(env) ? _scenario.Data(env).ActiveSeats : 1); ++seat)
    {
        Player* candidate = _scenario.SeatBot(env, seat);
        if (!candidate || !candidate->IsAlive() || !candidate->IsInWorld())
            continue;
        float const distance = lead ? lead->GetExactDist(candidate) : 0.0f;
        if (!bot || distance < nearest)
        {
            bot = candidate;
            nearest = distance;
        }
    }
    if (!bot)
        return;

    if (!pulls.Arrived)
    {
        pulls.Arrived = true;
        ++pulls.PullsArrived;
    }

    // Every decision until it is engaged: a creature that can attack the seat does; one that cannot yet (the seat is
    // stealthed, or out of its sight) walks to where the seat is.
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Creature* member = env.FindTarget(slot);
        if (!member || !member->IsAlive() || member->IsInCombat() || !member->IsAIEnabled)
            continue;

        if (member->CanCreatureAttack(bot) && member->CanSeeOrDetect(bot))
            member->AI()->AttackStart(bot);
        else if (member->movespline->Finalized())
            member->GetMotionMaster()->MovePoint(0, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
}

void Animus::Curriculum::PullsEncounter::TrackRest(uint32 decisionMs, SeatPull& pull, Player const* bot)
{
    int32 const foodLeft = RegenLeftMs(bot, SPELL_AURA_MOD_REGEN);
    int32 const drinkLeft = RegenLeftMs(bot, SPELL_AURA_MOD_POWER_REGEN);
    if (foodLeft >= 0 || drinkLeft >= 0)
        pull.RestMs += decisionMs;

    // A meal that ended with time left, while what it restores still wasn't full: stood up, moved, or was pulled.
    int32 const slack = int32(decisionMs) + MEAL_LEFT_SLACK_MS;
    if (foodLeft < 0 && pull.FoodLeftMs > slack && pull.ReadyHealth < MEAL_FULL)
        ++pull.MealsCutShort;
    if (drinkLeft < 0 && pull.DrinkLeftMs > slack && pull.ReadyMana < MEAL_FULL)
        ++pull.MealsCutShort;

    pull.FoodLeftMs = foodLeft;
    pull.DrinkLeftMs = drinkLeft;
}

uint32 Animus::Curriculum::PullsEncounter::Supplies(Env const& env) const
{
    return Gauntlet(env) ? _scenario.Tuning().Pulls.GauntletSupplies : CONSUMABLE_COUNT;
}

void Animus::Curriculum::PullsEncounter::EndPull(Env& env, EnvPulls& pulls)
{
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    pulls.PullKills = 0;
    pulls.PullCleared = false;
    pulls.QuietSinceMs = env.EpisodeElapsedMs;

    // Alone, each pull cleared shortens the break before the next (the pull being cleared is counted by now).
    uint32 breakMs = urand(tuning.NextPullMinMs, tuning.NextPullMaxMs);
    if (SoloGauntlet(env))
        breakMs = std::max(std::min(breakMs, tuning.NextPullFloorMs),
            breakMs - std::min(breakMs, tuning.NextPullShrinkMs * pulls.PullsCleared));
    pulls.NextPullMs = env.EpisodeElapsedMs + breakMs;

    EnvState& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
    {
        data.Seats[seat].TargetSlot = 0;
        data.Seats[seat].Combat.LastDistance = -1.0f;
    }
}

void Animus::Curriculum::PullsEncounter::Recover(Env& env)
{
    EnvState const& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    Player* owner = _scenario.Owner(env);

    bool anyoneAlive = owner && owner->IsAlive();
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && bot->IsAlive())
            anyoneAlive = true;

    // A wipe: nobody is left to finish the pull, so it is cleared away and the next one comes after the usual break.
    if (!anyoneAlive && HasCreatures(env))
    {
        Despawn(env);
        ++pulls.Wipes;
        if (GroupRun(env) && pulls.Wipes > pulls.RunWipes)
            RecordRun(env, false);
        EndPull(env, pulls);
    }

    pulls.AwaitingRevive = false;
    if (HasCreatures(env))
        return;

    // Between pulls the dead wait a while for a resurrection they can get -- their own Soulstone or Reincarnation, or
    // a living seat's resurrection spell -- and then stand up with part of their health and mana, and their deaths can
    // be paid for again. Resurrecting them is the party's to learn; standing up only keeps the episode going.
    bool resurrector = false;
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        resurrector |= _scenario.SeatCanResurrect(env, seat);

    bool const graceOver = env.EpisodeElapsedMs >= pulls.QuietSinceMs + _scenario.Tuning().Resurrection.GraceMs;
    float const fraction = _scenario.Tuning().Pulls.RecoverFraction;
    auto const recover = [&](Player* player, bool selfResurrect)
    {
        if (!graceOver && (resurrector || selfResurrect))
        {
            pulls.AwaitingRevive = true;
            return false;
        }

        player->ResurrectPlayer(fraction);
        player->SetPower(POWER_MANA, uint32(float(player->GetMaxPower(POWER_MANA)) * fraction));
        return true;
    };

    if (owner && !owner->IsAlive() && recover(owner, false))
        _scenario.NotifyRecovered(env, RECOVERED_OWNER);

    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || bot->IsAlive() || !recover(bot, bot->GetUInt32Value(PLAYER_SELF_RES_SPELL) != 0))
            continue;

        _scenario.NotifyRecovered(env, int32(seat));
    }
}

bool Animus::Curriculum::PullsEncounter::SelectTarget(Env const& env, uint32 seatIndex, Unit*& target)
{
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (Unit* selected = env.FindTargetUnit(seat.TargetSlot); selected && selected->IsAlive())
    {
        target = selected;
        return true;
    }

    // The selection died or despawned: the nearest living enemy, like a player tabbing to the next one.
    Player* bot = env.FindBot(seatIndex);
    target = nullptr;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive())
            continue;

        if (!target || (bot && bot->GetDistance(enemy) < bot->GetDistance(target)))
        {
            target = enemy;
            seat.TargetSlot = slot;
        }
    }

    return true;
}

void Animus::Curriculum::PullsEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    SeatPull& pull = _envs[env.Index].Seats[seat];
    pull.SustainCasts += result.SustainCasts;
    pull.FoodUsed += result.FoodUsed;
    pull.DrinkUsed += result.DrinkUsed;
    pull.FoodFailed += result.FoodFailed;
    pull.DrinkFailed += result.DrinkFailed;

    if (!result.PendingInterrupt.IsEmpty())
        pull.PendingInterrupt = result.PendingInterrupt;
}

void Animus::Curriculum::PullsEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvPulls const& pulls = _envs[env.Index];

    view.PullsCleared = pulls.PullsCleared;
    view.QuietTime = std::min(1.0f, float(env.EpisodeElapsedMs - pulls.QuietSinceMs) / QUIET_TIME_SCALE_MS);
    view.PullTime = std::min(1.0f, float(env.EpisodeElapsedMs - pulls.PullStartMs) / PULL_TIME_SCALE_MS);
    view.ElitePull = pulls.EliteOrHigher;
    view.FoodItem = pulls.Seats[seat].FoodItem;
    view.DrinkItem = pulls.Seats[seat].DrinkItem;
    view.GauntletSupplies = Supplies(env);

    bool const pullActive = HasCreatures(env);
    view.PullArrival = PullsArrive(env) && pullActive && !pulls.PullEngaged && !pulls.Arrived
        ? std::min(1.0f, float(pulls.ArriveMs - std::min(pulls.ArriveMs, env.EpisodeElapsedMs)) / ARRIVAL_SCALE_MS)
        : 0.0f;
    uint32 const nextPullMs = pulls.NextPullMs - std::min(pulls.NextPullMs, env.EpisodeElapsedMs);
    view.NextPull = Gauntlet(env) && !pullActive ? std::min(1.0f, float(nextPullMs) / NEXT_PULL_SCALE_MS) : 0.0f;

    // A party's drill reports its crowd as a dungeon does (CrowdBlock): what is on the party and on the tank, what is
    // past the pack's slots, and in a camp the next pack standing -- the tank's way on, the others' the tank.
    ArenaDefinition const& arena = _scenario.Arena(env);
    if (!arena.PartyGroup || !SinglePack(env) || !view.Bot)
        return;
    CrowdView& crowd = view.Crowd;
    crowd.Present = true;
    crowd.Tank = PartyTank(env);
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* unit = env.FindTargetUnit(slot);
        if (!unit || !unit->IsAlive() || !unit->IsInMap(view.Bot))
            continue;
        Unit const* victim = unit->GetVictim();
        if (victim && victim->IsPlayer())
        {
            ++crowd.OnParty;
            crowd.OnTank += victim == crowd.Tank ? 1 : 0;
            Creature const* creature = unit->ToCreature();
            crowd.Elites += creature && creature->isElite() ? 1 : 0;
        }
        if (slot >= PACK_SLOTS && crowd.Count < CROWD_SLOTS)
            crowd.Units[crowd.Count++] = unit;
        // The slots are fight first, then nearest the tank (OrderCamp): the first standing is the next pack.
        if (Camp(env) && !crowd.HasAhead && !unit->IsInCombat())
        {
            crowd.HasAhead = true;
            crowd.Ahead.Relocate(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
            auto const member = std::find_if(pulls.CampMembers.begin(), pulls.CampMembers.end(),
                [unit](auto const& entry) { return entry.first == unit->GetGUID(); });
            if (member != pulls.CampMembers.end())
                for (auto const& [guid, pack] : pulls.CampMembers)
                    if (Creature* other = pack == member->second ? Encoding::CreatureThrough(*view.Bot, guid) : nullptr;
                        other && other->IsAlive())
                        ++crowd.AheadSize;
        }
    }

    if (!Camp(env))
        return;
    if (crowd.Tank == view.Bot && crowd.HasAhead && !pulls.CampFighting)
    {
        view.HasObjective = true;
        view.Objective = crowd.Ahead;
    }
    else if (crowd.Tank && crowd.Tank != view.Bot && crowd.Tank->IsInMap(view.Bot))
    {
        view.HasObjective = true;
        view.Objective.Relocate(crowd.Tank->GetPositionX(), crowd.Tank->GetPositionY(), crowd.Tank->GetPositionZ());
    }
}

void Animus::Curriculum::PullsEncounter::BeforeRewards(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];

    // A pull's creatures only leave the map by dying: a corpse that decayed during a long pull (Corpse.Decay.* game
    // seconds) is still a kill, or the kills after it would go uncounted.
    uint32 alive = 0;
    uint32 dead = 0;
    if (env.FindMap())
    {
        for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        {
            if (env.Targets[slot].IsPlayer())
                continue;

            Creature* enemy = env.FindTarget(slot);
            ++(enemy && enemy->IsAlive() ? alive : dead);
        }
    }

    pulls.NewKills = dead > pulls.PullKills ? dead - pulls.PullKills : 0;
    pulls.Kills += pulls.NewKills;
    pulls.PullKills = std::max(pulls.PullKills, dead);
    pulls.PullCleared = HasCreatures(env) && !alive && dead;
    if (Camp(env))
        UpdateCamp(env);
}

void Animus::Curriculum::PullsEncounter::UpdateCamp(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];
    Map* map = env.FindMap();
    if (!map)
        return;

    std::array<bool, CAMP_PACKS> fighting{};
    std::array<bool, CAMP_PACKS> standing{};
    for (auto const& [guid, pack] : pulls.CampMembers)
    {
        Creature* member = map->GetCreature(guid);
        if (!member || !member->IsAlive())
            continue;
        standing[pack] = true;
        fighting[pack] |= member->IsInCombat();
    }

    pulls.CampFighting = 0;
    for (uint32 pack = 0; pack < pulls.CampPacks; ++pack)
        pulls.CampFighting += fighting[pack] ? 1 : 0;
    pulls.CampPeak = std::max(pulls.CampPeak, pulls.CampFighting);
    if (pulls.CampFighting)
        pulls.CampQuietMs = env.EpisodeElapsedMs;

    for (uint32 pack = 0; pack < pulls.CampPacks; ++pack)
    {
        if (fighting[pack] && pulls.CampFighting > 1)
            pulls.CampMixed[pack] = true;
        if (!standing[pack] && !pulls.CampCleared[pack])
        {
            pulls.CampCleared[pack] = true;
            // A wipe clears the camp away (Recover): nothing it took was killed.
            if (!pulls.CampMixed[pack] && !pulls.Wipes)
            {
                ++pulls.CampNewClean;
                ++pulls.CampClean;
            }
        }
    }
}

void Animus::Curriculum::PullsEncounter::OrderCamp(Env& env)
{
    Player* tank = PartyTank(env);
    Player* from = tank ? tank : _scenario.SeatBot(env, 0);
    if (!from || !from->IsInWorld())
        return;

    // Each slot's rank: fighting, then standing, then dead; nearer first within each.
    struct Ranked
    {
        uint32 Slot;
        uint32 Group;
        float Distance;
    };
    std::vector<Ranked> ranked;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* unit = env.FindTargetUnit(slot);
        bool const alive = unit && unit->IsAlive() && unit->IsInMap(from);
        ranked.push_back({ slot, !alive ? 2u : unit->IsInCombat() ? 0u : 1u,
            alive ? from->GetExactDist(unit) : 0.0f });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](Ranked const& a, Ranked const& b)
    {
        return a.Group != b.Group ? a.Group < b.Group : a.Distance < b.Distance;
    });

    bool moved = false;
    for (uint32 slot = 0; slot < ranked.size(); ++slot)
        moved |= ranked[slot].Slot != slot;
    if (!moved)
        return;

    // Old slot to new, for everything kept per slot.
    std::array<uint32, MAX_TARGETS> to{};
    std::vector<ObjectGuid> targets(env.Targets.size());
    for (uint32 slot = 0; slot < ranked.size(); ++slot)
    {
        targets[slot] = env.Targets[ranked[slot].Slot];
        if (ranked[slot].Slot < MAX_TARGETS)
            to[ranked[slot].Slot] = slot;
    }
    env.Targets = std::move(targets);

    EnvState& data = _scenario.Data(env);
    EnvPulls& pulls = _envs[env.Index];
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
    {
        if (data.Seats[seat].TargetSlot < ranked.size() && data.Seats[seat].TargetSlot < MAX_TARGETS)
            data.Seats[seat].TargetSlot = to[data.Seats[seat].TargetSlot];
        SeatPull& pull = pulls.Seats[seat];
        std::array<uint64, MAX_TARGETS> damage{};
        std::array<uint32, MAX_TARGETS> freeMs{};
        for (uint32 slot = 0; slot < ranked.size() && slot < MAX_TARGETS; ++slot)
            if (to[slot] < MAX_TARGETS)
            {
                damage[to[slot]] = pull.SlotDamage[slot];
                freeMs[to[slot]] = pull.SlotFreeMs[slot];
            }
        pull.SlotDamage = damage;
        pull.SlotFreeMs = freeMs;
    }
}

Player* Animus::Curriculum::PullsEncounter::PartyTank(Env const& env) const
{
    EnvState const& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        if (data.Seats[seat].DungeonRole == DUNGEON_TANK)
        {
            Player* bot = _scenario.SeatBot(env, seat);
            return bot && bot->IsAlive() && bot->IsInWorld() ? bot : nullptr;
        }
    return nullptr;
}

void Animus::Curriculum::PullsEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());
    if (!bot)
        return;

    EnvPulls& pulls = _envs[env.Index];
    SeatPull& pull = pulls.Seats[seatIndex];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    CombatTally& tally = seat.Combat;
    AgentStats const& step = env.StepStats[seatIndex];
    float const botHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));

    // Damage is a fraction of the pull's total health, taken damage a fraction of the bot's.
    float pullHealth = 0.0f;
    float pullLeft = 0.0f;
    Unit* nearest = nullptr;
    bool fighting = false;
    // A camp's approach is to what is fighting while anything is: closing on the nearest standing pack was paying the
    // seats to pull the next one into the fight.
    bool const camp = Camp(env);
    bool const campFight = camp && pulls.CampFighting > 0;
    // ... and between fights only the tank closes on the next pack: the others' way on is the tank (View).
    Player const* tank = camp ? PartyTank(env) : nullptr;
    bool const campFollow = camp && !campFight && tank && tank != bot;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy)
            continue;

        fighting |= !enemy->IsPlayer() && (!enemy->IsAlive() || enemy->IsInCombat());
        if (((campFight && !enemy->IsInCombat()) || campFollow) && enemy->IsAlive())
        {
            pullHealth += float(enemy->GetMaxHealth());
            pullLeft += float(enemy->GetHealth());
            continue;
        }

        // The pull is its creatures; an ambusher is paid for by the ambush, but still a place to close in on.
        if (!enemy->IsPlayer())
        {
            pullHealth += float(enemy->GetMaxHealth());
            if (enemy->IsAlive())
                pullLeft += float(enemy->GetHealth());
        }
        if (enemy->IsAlive() && (!nearest || bot->GetDistance(enemy) < bot->GetDistance(nearest)))
            nearest = enemy;
    }

    // The pull's clock starts when it is engaged, not when it spawns: sizing it up, stealthing in and resting first are
    // the seat's call. Once per env (every seat sees the same pull).
    if (!pulls.PullEngaged && fighting)
    {
        pulls.PullEngaged = true;
        pulls.PullEngageMs = env.EpisodeElapsedMs;
    }

    // Recovery between pulls. Health and mana are kept from the decision before (a pull's first blow may land before
    // the seat's next decision): a pull engaged this decision pays readiness from them, and a meal that ended is
    // judged by them.
    if (Gauntlet(env) && bot->IsAlive())
    {
        TrackRest(_scenario.DecisionMs(), pull, bot);

        if (pulls.PullEngaged && pulls.PullEngageMs == env.EpisodeElapsedMs && pullHealth > 0.0f)
        {
            bool const usesMana = bot->GetMaxPower(POWER_MANA) > 0;
            float const ready = usesMana ? std::min(pull.ReadyHealth, pull.ReadyMana) : pull.ReadyHealth;
            ++pull.PullsEngaged;
            pull.EngageHealthSum += pull.ReadyHealth;
            pull.EngageManaSum += pull.ReadyMana;
            if (pull.ReadyHealth < READY_LOW_HEALTH || (usesMana && pull.ReadyMana < READY_LOW_MANA))
                ++pull.PullsStartedLow;
            ledger.Add(RewardTerm::Readiness,
                (SoloGauntlet(env) ? tuning.SoloGauntletReadiness : tuning.OwnerReadiness) * ready);

            // Buffs up when the pull starts: the layout's buff groups on the seat, and on the owner with one.
            float coverage = SupportBlock::BuffCoverage(*seat.L, bot);
            if (Player* owner = _scenario.Owner(env); owner && owner->IsAlive())
                coverage = 0.5f * (coverage + SupportBlock::BuffCoverage(*seat.L, owner));
            pull.BuffCoverageSum += coverage;
            ledger.Add(RewardTerm::Readiness, _scenario.Tuning().Support.BuffCoverage * coverage);

            // And its pet out, as the duel pays for at its engagement (CombatReward::OneOnOne).
            if (seat.L && PetBlock::HasPet(seat.L->Profile->Class) && PetBlock::FindPet(bot))
                ledger.Add(RewardTerm::Readiness, _scenario.Tuning().Support.PetReady);
        }

        if (!pulls.PullEngaged || pulls.PullEngageMs != env.EpisodeElapsedMs)
        {
            pull.ReadyHealth = HealthFraction(bot);
            pull.ReadyMana = ManaFraction(bot);
        }
    }

    // A gauntlet is about lasting through many fights, so what is paid every decision counts for less there: the
    // clear, surviving, readiness and control are what a plan earns (Pulls.GauntletDenseScale).
    float const dense = Gauntlet(env) ? tuning.GauntletDenseScale : 1.0f;

    if (pullHealth > 0.0f)
        ledger.Add(RewardTerm::DamageDealt, dense * tuning.DamageDealt * float(step.Damage) / pullHealth);

    tally.DamageTaken += step.DamageTaken;
    pull.PullDamageTaken += step.DamageTaken;
    ledger.Add(RewardTerm::DamageTaken,
        -dense * (Gauntlet(env) ? tuning.GauntletDamageTaken : tuning.DamageTaken) * seat.LastStepDamageTaken);

    CombatReward::Casting(bot, step, tally, _scenario.Tuning().Casting, ledger);

    // Shooting from range, and a pet holding enemies off its owner (Duel.ShotAtRange, ShotPaused, PetTank), as in the
    // duel.
    if (pullHealth > 0.0f)
    {
        bool meleed = false;
        for (uint32 slot = 0; slot < env.Targets.size() && !meleed; ++slot)
            if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
                meleed = enemy->GetVictim() == bot && enemy->IsWithinMeleeRange(bot);
        ArenaDefinition const& arena = _scenario.Arena(env);
        CombatReward::RangedAndPet(bot, seat, step, env.FindTargetUnit(seat.TargetSlot), pullHealth, meleed,
            !arena.PartyGroup && !arena.Owner, _scenario.Tuning().Duel, _scenario.DecisionMs(), ledger);
    }
    CombatReward::Approach(bot, nearest && bot->IsAlive() ? nearest : nullptr,
        CombatReward::DesiredRange(seat, _scenario.Tuning().Duel), dense * tuning.Approach, tally, ledger);

    CombatReward::Stealth(tally, tuning.StealthOpener, tuning.StealthUtility, ledger);

    // How the seat is fighting its current target, measured as a duel measures it. CombatReward::OneOnOne is not
    // called here and it held the whole style tally, so without this a pack's roots and snares read zero and every
    // share divided by FightMs -- in_melee_share, target_on_pet_share, the two control shares -- reads 0 out of 0.
    // A target that just died still counts the time: the fight goes on, there is simply nothing to hold.
    if (pulls.PullEngaged && bot->IsAlive())
    {
        Unit* target = env.FindTargetUnit(seat.TargetSlot);
        CombatReward::Style(bot, target && target->IsAlive() ? target : nullptr, _scenario.DecisionMs(), tally);
    }

    // An interrupt counts when the enemy it was cast at had its cast cut short since: a cast that finished on its own,
    // or an enemy that died, is not one.
    if (!pull.PendingInterrupt.IsEmpty())
    {
        auto const stopped = std::find_if(env.StepInterruptedTargets.begin(), env.StepInterruptedTargets.end(),
            [&pull](Env::InterruptedCast const& cast) { return cast.Caster == pull.PendingInterrupt; });
        if (stopped != env.StepInterruptedTargets.end())
        {
            // Paid by what it prevented, not per press: stopping a heal undoes damage already dealt, an area spell
            // would have hit the whole party, a long cast was a large part of the caster's output. An ordinary cast
            // still pays the full Interrupt -- that term is how a class finds the behaviour at all, and a policy
            // that only ever saw a heal pay might never find it.
            ledger.Add(RewardTerm::Interrupt, tuning.Interrupt
                * PreventedScale(tuning.InterruptHeal, tuning.InterruptArea, tuning.InterruptLong,
                    stopped->Prevented));
            ++pull.Interrupts;
        }

        pull.PendingInterrupt.Clear();
    }

    if (bot->GetPetGUID() || !bot->m_Controlled.empty())
        tally.PetSummoned = true;

    // Kills and clears are the party's: every seat shares them. With an owner they count more: the pilot's party
    // learned to fight less to avoid the penalties. And every outcome term carries the pull's rung: a win is
    // multiplied by the tier scale and a loss divided by it, so a hard pull is worth attempting and the score
    // stays comparable as the ladder climbs (Difficulty.TierScale).
    float const clearScale = _scenario.Arena(env).Owner ? tuning.OwnerClearScale : 1.0f;
    float const tierScale = CombatReward::TierScale(_scenario.Tuning().Difficulty.TierScale, pulls.Rung);
    if (pulls.NewKills)
        ledger.Add(RewardTerm::Kill, dense * tuning.Kill * clearScale * tierScale * float(pulls.NewKills));

    if (pulls.PullCleared)
    {
        float const healthKept = 1.0f - std::min(1.0f, float(pull.PullDamageTaken) / botHealth);
        uint32 const engageMs = pulls.PullEngaged ? pulls.PullEngageMs : env.EpisodeElapsedMs;

        if (Gauntlet(env))
        {
            float const pullTime = float(env.EpisodeElapsedMs - engageMs);
            float const fast = 1.0f - std::min(1.0f, pullTime / PULL_TIME_SCALE_MS);
            ledger.Add(RewardTerm::Clear, (SoloGauntlet(env)
                ? tuning.SoloGauntletClear + tuning.SoloGauntletFastPull * fast
                : (tuning.Clear + tuning.FastPull * fast) * clearScale) * tierScale);
            pull.PullDamageTaken = 0;
        }
        else
        {
            if (!tally.Killed)
            {
                tally.Killed = true;
                tally.KillTimeMs = env.EpisodeElapsedMs;
            }

            ledger.Add(RewardTerm::Clear,
                (tuning.PackClear + tuning.FastClear * CombatReward::TimeLeftSince(env, engageMs)) * tierScale);
        }

        float const kept = SinglePack(env) ? tuning.PackHealthKept
            : SoloGauntlet(env) ? tuning.SoloGauntletHealthKept : tuning.HealthKept;
        ledger.Add(RewardTerm::HealthKept, kept * healthKept * tierScale);
    }

    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -(SoloGauntlet(env) ? tuning.SoloGauntletDeath
            : Gauntlet(env) ? tuning.GauntletDeath : tuning.PackDeath) / tierScale);

        // A single pack lost in overtime: the rest of the overtime too, which timing out would have cost.
        if (SinglePack(env) && !tally.Killed && pulls.PullEngaged
            && env.EpisodeElapsedMs > pulls.PullEngageMs + tuning.OvertimeGraceMs * std::max<uint32>(1, pulls.CampPacks)
            && env.EpisodeLengthMs > env.EpisodeElapsedMs)
            ledger.Add(RewardTerm::Timeout,
                -tuning.Overtime * float(env.EpisodeLengthMs - env.EpisodeElapsedMs) / 1000.0f / tierScale);
    }

    if (SoloGauntlet(env))
    {
        GauntletAloneTerms(env, seat, pull, bot, ledger);
        return;
    }

    if (Gauntlet(env))
    {
        GauntletOwnerTerms(env, seat, pull, bot, ledger);
        return;
    }

    if (!SinglePack(env))
        return;

    // A single pack is won or lost, as the duel is. Standing off is charged as it happens once the grace is gone, so is
    // a fight dragged past its own grace (under the timeout term: it is the timeout arriving), a ranged spec is charged
    // for being hit in melee reach, and running out the clock is a lost fight.
    if (bot->IsAlive() && !tally.Killed)
    {
        float const seconds = float(_scenario.DecisionMs()) / 1000.0f;
        uint32 const graceMs = tuning.StallGraceMs + PreparationRefundMs(tuning, tally, pull);
        if (!pulls.PullEngaged && env.EpisodeElapsedMs > graceMs)
            ledger.Add(RewardTerm::Stall, -tuning.Stall * seconds);

        // A camp: standing about between packs past the rest allowed, more than one pack in the fight, and each pack
        // killed on its own.
        if (camp)
        {
            bool const resting = bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
            if (pulls.PullEngaged && !pulls.CampFighting && !resting
                && env.EpisodeElapsedMs > pulls.CampQuietMs + tuning.CampRestMs)
                ledger.Add(RewardTerm::Stall, -tuning.Stall * seconds);
            if (pulls.CampFighting > 1)
                ledger.Add(RewardTerm::Threat, -tuning.CampExtraPack * float(pulls.CampFighting - 1) * seconds);
            if (pulls.CampNewClean)
                ledger.Add(RewardTerm::Clear, tuning.CampCleanPack * tierScale * float(pulls.CampNewClean));
        }

        if (pulls.PullEngaged)
            ControlPreventedTerm(env, seat, pull, bot, step, ledger);

        // Time spent holding an add extends the grace, up to Pulls.ControlGraceMaxMs: control lengthens a fight on
        // purpose, and charging it as dragging one out is what left crowd control paying less than it cost.
        uint32 const overtimeGraceMs = tuning.OvertimeGraceMs * (camp ? std::max<uint32>(1, pulls.CampPacks) : 1)
            + std::min(pull.ControlledMs, tuning.ControlGraceMaxMs);
        if (pulls.PullEngaged && env.EpisodeElapsedMs > pulls.PullEngageMs + overtimeGraceMs)
            ledger.Add(RewardTerm::Timeout, -tuning.Overtime * seconds / tierScale);

        if (seat.L && seat.L->Profile->Specs[seat.Spec].Range != RangeBand::Melee)
        {
            bool meleed = false;
            for (uint32 slot = 0; slot < env.Targets.size() && !meleed; ++slot)
                if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
                    meleed = enemy->GetVictim() == bot && enemy->IsWithinMeleeRange(bot);

            if (meleed)
                ledger.Add(RewardTerm::Spacing, -tuning.Spacing * seconds);
        }
    }

    // What the clock costs is what is left of the pull, as the duel charges what is left of its opponent: a pack
    // nearly down is a near miss, one untouched is a refusal to fight.
    if (!tally.Killed && !tally.Died && !tally.TimedOut && TimeIsUp(env))
    {
        tally.TimedOut = true;
        ledger.Add(RewardTerm::Timeout, -tuning.Timeout * CombatReward::TimeoutScale(tuning.TimeoutFloor,
            pullHealth > 0.0f ? pullLeft / pullHealth : 1.0f) / tierScale);
    }

    // The outcome moves that class and role on the ladder, once: a clear without a death is a win.
    if (seatIndex == 0 && pulls.RungCounts && !pulls.RungRecorded && (tally.Killed || tally.Died || tally.TimedOut))
    {
        pulls.RungRecorded = true;
        (Camp(env) ? _campLadder : _ladder).Record(pulls.RungLayout, pulls.RungSpec, pulls.Rung,
            tally.Killed && !tally.Died, MaxRung(env));
    }
}

void Animus::Curriculum::PullsEncounter::GauntletAloneTerms(Env& env, SeatState& seat, SeatPull& pull, Player* bot,
    RewardLedger& ledger)
{
    EnvPulls const& pulls = _envs[env.Index];
    CombatTally& tally = seat.Combat;
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    if (!bot->IsAlive())
        return;

    // Lasting to the end with Pulls.SoloGauntletWinPulls cleared is the gauntlet's win: counted as the kill
    // (clean_kill is then a gauntlet endured). Lasting on fewer is the clock running out.
    if (!tally.Killed && !tally.Died && !tally.TimedOut && TimeIsUp(env))
    {
        // A planned run is won by finishing it, not by lasting: its clock running out is a loss however far it got.
        if (!Sequence(env) && pulls.PullsCleared >= tuning.SoloGauntletWinPulls)
        {
            tally.Killed = true;
            tally.KillTimeMs = env.EpisodeElapsedMs;
        }
        else
            tally.TimedOut = true;
    }

    if (!HasCreatures(env))
        return;

    // A pull left standing is charged once its grace from the spawn is gone (not while eating or drinking, which is
    // recovering for it), and a ranged spec hit in melee reach.
    float const seconds = float(_scenario.DecisionMs()) / 1000.0f;
    uint32 const graceMs = tuning.StallGraceMs + PreparationRefundMs(tuning, tally, pull);
    bool const resting = bot->HasAuraType(SPELL_AURA_MOD_REGEN) || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
    if (!pulls.PullEngaged && !resting && env.EpisodeElapsedMs > pulls.PullStartMs + graceMs)
        ledger.Add(RewardTerm::Stall, -tuning.Stall * seconds);

    if (seat.L && seat.L->Profile->Specs[seat.Spec].Range != RangeBand::Melee)
    {
        bool meleed = false;
        for (uint32 slot = 0; slot < env.Targets.size() && !meleed; ++slot)
            if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
                meleed = enemy->GetVictim() == bot && enemy->IsWithinMeleeRange(bot);

        if (meleed)
            ledger.Add(RewardTerm::Spacing, -tuning.Spacing * seconds);
    }

    if (!pulls.PullEngaged)
        return;

    ControlTerm(env, seat, pull, tuning.SoloGauntletControl, tuning.SoloGauntletControlMax, ledger);
}

void Animus::Curriculum::PullsEncounter::GauntletOwnerTerms(Env& env, SeatState& seat, SeatPull& pull, Player* bot,
    RewardLedger& ledger)
{
    EnvPulls const& pulls = _envs[env.Index];
    CombatTally& tally = seat.Combat;
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;

    // Beside an owner the gauntlet is won by lasting to the end with the owner never dead, no wipe and
    // Pulls.OwnerWinPulls cleared: counted as the kill, so clean_kill is the gauntlet won with the seat alive. Anything
    // short of it is the clock running out. Deaths don't end the episode here, so it is judged at the end whatever
    // happened to the seat.
    if (!tally.Killed && !tally.TimedOut && TimeIsUp(env))
    {
        if (!pulls.OwnerDied && !pulls.Wipes && pulls.PullsCleared >= tuning.OwnerWinPulls)
        {
            tally.Killed = true;
            tally.KillTimeMs = env.EpisodeElapsedMs;
        }
        else
            tally.TimedOut = true;
    }

    // Control keeps adds off the owner as much as off the seat.
    if (bot->IsAlive() && HasCreatures(env) && pulls.PullEngaged)
        ControlTerm(env, seat, pull, tuning.OwnerControl, tuning.OwnerControlMax, ledger);
}

void Animus::Curriculum::PullsEncounter::ControlTerm(Env& env, SeatState const& seat, SeatPull& pull, float perSecond,
    float perPull, RewardLedger& ledger)
{
    // Control: pack members other than the target kept out of the fight while another member is alive.
    float const seconds = float(_scenario.DecisionMs()) / 1000.0f;
    Unit const* target = env.FindTargetUnit(seat.TargetSlot);
    uint32 alive = 0;
    uint32 controlled = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
    {
        Unit* enemy = env.FindTargetUnit(slot);
        if (!enemy || !enemy->IsAlive())
            continue;

        ++alive;
        if (enemy->IsPlayer() || enemy == target)
            continue;

        if (Controlled(enemy))
            ++controlled;
    }

    if (!controlled || alive < 2)
        return;

    pull.ControlMs += controlled * _scenario.DecisionMs();
    float const pay = std::min(perSecond * seconds * float(controlled), std::max(0.0f, perPull - pull.PullControlPaid));
    if (pay > 0.0f)
    {
        pull.PullControlPaid += pay;
        ledger.Add(RewardTerm::Control, pay);
    }
}

/// The stall grace a seat has earned by preparing for the pull it is facing: buffs, forms, stealth and a pet
/// started since this pull spawned, capped at PullTuning::PreparationRefundMaxMs. Per pull rather than per episode:
/// CombatTally::PreparationMs runs over the whole gauntlet, so preparing once bought the full refund on every pull
/// after it, and a seat that kept re-buffing between kites earned grace for it (stage2_pack 2026-09-18: the warlock's
/// preparation went from 5.6 s a fight to 14.2 s, 19.4 s in the fights it lost).
uint32 Animus::Curriculum::PullsEncounter::PreparationRefundMs(CurriculumTuning::PullTuning const& tuning,
    CombatTally const& tally, SeatPull const& pull)
{
    uint32 const prepared = tally.PreparationMs > pull.PreparationBaseMs
        ? tally.PreparationMs - pull.PreparationBaseMs : 0;
    return std::min(prepared, tuning.PreparationRefundMaxMs);
}

bool Animus::Curriculum::PullsEncounter::Controlled(Unit const* enemy)
{
    Unit const* victim = enemy->GetVictim();
    bool const rootedAway = enemy->HasUnitState(UNIT_STATE_ROOT) && !enemy->IsNonMeleeSpellCast(false)
        && (!victim || !enemy->IsWithinMeleeRange(victim));
    return enemy->HasUnitState(UNIT_STATE_CONTROLLED) || enemy->HasAuraType(SPELL_AURA_TRANSFORM) || rootedAway;
}

void Animus::Curriculum::PullsEncounter::ControlPreventedTerm(Env& env, SeatState const& seat, SeatPull& pull,
    Player const* bot, AgentStats const& step, RewardLedger& ledger)
{
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    uint32 const decisionMs = _scenario.DecisionMs();
    float const maxHealth = float(std::max<uint32>(1, bot->GetMaxHealth()));
    Unit const* target = env.FindTargetUnit(seat.TargetSlot);

    // What each enemy dealt while it was free to act, so its own rate can be read back when it is held. An enemy
    // that is controlled is neither dealing damage nor earning free time: only what it does when loose counts.
    uint32 alive = 0;
    uint32 held = 0;
    float measured = 0.0f;              // damage per ms over the slots with enough free time to trust
    uint32 measuredSlots = 0;
    for (std::size_t slot = 0; slot < env.Targets.size() && slot < MAX_TARGETS; ++slot)
    {
        Unit* enemy = env.FindTargetUnit(uint32(slot));
        if (!enemy || !enemy->IsAlive() || enemy->IsPlayer())
            continue;

        ++alive;
        pull.SlotDamage[slot] += step.DamageTakenBy[slot];
        if (!Controlled(enemy))
        {
            pull.SlotFreeMs[slot] += decisionMs;
            if (pull.SlotFreeMs[slot] >= tuning.ControlRateMinMs)
            {
                measured += float(pull.SlotDamage[slot]) / float(pull.SlotFreeMs[slot]);
                ++measuredSlots;
            }
        }
        else if (enemy != target)
            ++held;
    }

    if (!held || alive < 2)
        return;

    // A held enemy is credited its own rate once it has been loose long enough to have one, else the pull's mean,
    // else the configured fallback: a pack sapped before it ever swings still prevented something.
    float const fallback = measuredSlots ? measured / float(measuredSlots)
        : tuning.ControlFallbackDps * maxHealth / 1000.0f;
    float prevented = 0.0f;
    for (std::size_t slot = 0; slot < env.Targets.size() && slot < MAX_TARGETS; ++slot)
    {
        Unit* enemy = env.FindTargetUnit(uint32(slot));
        if (!enemy || !enemy->IsAlive() || enemy->IsPlayer() || enemy == target || !Controlled(enemy))
            continue;

        float const rate = pull.SlotFreeMs[slot] >= tuning.ControlRateMinMs
            ? float(pull.SlotDamage[slot]) / float(pull.SlotFreeMs[slot]) : fallback;
        prevented += rate * float(decisionMs);
    }

    // Over health now, not maximum health: the same hit prevented is worth more the less there is left to lose, which
    // is what makes control a survival tool. Floored so it cannot run away as the seat nears death.
    float const floor = std::max(1.0f, tuning.ControlHealthFloor * maxHealth);
    float const health = std::max(floor, float(bot->GetHealth()));
    float const value = prevented / health;

    // ControlMs is enemy-time, as the gauntlet's ControlTerm counts it, so control_seconds means the same thing in
    // both; ControlledMs is the wall time behind it, which is what the overtime grace is allowed to grow by.
    pull.ControlMs += held * decisionMs;
    pull.ControlledMs += decisionMs;
    pull.ControlPrevented += prevented / maxHealth;

    float const pay = std::min(tuning.SinglePackControl * value,
        std::max(0.0f, tuning.SinglePackControlMax - pull.PullControlPaid));
    if (pay > 0.0f)
    {
        pull.PullControlPaid += pay;
        ledger.Add(RewardTerm::Control, pay);
    }
}

uint32 Animus::Curriculum::PullsEncounter::MaxRung(Env const& env) const
{
    uint32 const top = std::min<uint32>(_scenario.Tuning().Pulls.MaxTier, uint32(PACK_RUNGS.size()) - 1);
    // A drill can pin its arena's ladder (ArenaDefinition::MaxRung) so the only thing that varies is the thing
    // being drilled. A pin of 0 holds every class/role on the bottom rung: Draw's review and stretch branches
    // have nowhere to go, and Record cannot promote past the cap it is given.
    int32 const pinned = _scenario.ArenaMaxRung(env);
    return pinned < 0 ? top : std::min<uint32>(uint32(pinned), top);
}

void Animus::Curriculum::PullsEncounter::AfterRewards(Env& env)
{
    EnvPulls& pulls = _envs[env.Index];
    if (Camp(env))
    {
        pulls.CampNewClean = 0;
        OrderCamp(env);
    }
    if (!pulls.PullCleared || !Gauntlet(env))
        return;

    // Clear the field and schedule the next pull.
    Despawn(env);
    ++pulls.PullsCleared;
    EndPull(env, pulls);

    // A planned run's last pull: the win is recorded here, not at the next decision's reward. IsTerminal ends the
    // episode as soon as the run is finished, so a seat waiting for its next reward would never be counted a winner.
    if (Sequence(env) && pulls.PullsCleared >= RunLength(env))
    {
        RecordRun(env, true);
        EnvState& data = _scenario.Data(env);
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        {
            Player* bot = _scenario.SeatBot(env, seat);
            CombatTally& tally = data.Seats[seat].Combat;
            if (bot && bot->IsAlive() && !tally.Killed && !tally.Died)
            {
                tally.Killed = true;
                tally.KillTimeMs = env.EpisodeElapsedMs;
            }
        }
    }
}

void Animus::Curriculum::PullsEncounter::WriteState(Env const& env, float* state) const
{
    EnvPulls const& pulls = _envs[env.Index];
    bool const pullActive = HasCreatures(env);

    state[StageScenario::STATE_PULL_ACTIVE] = pullActive ? 1.0f : 0.0f;
    state[StageScenario::STATE_PULLS_CLEARED] = std::min(1.0f, float(pulls.PullsCleared) / 10.0f);
    state[StageScenario::STATE_NEXT_PULL] = pullActive ? 0.0f
        : std::clamp((float(pulls.NextPullMs) - float(env.EpisodeElapsedMs)) / NEXT_PULL_SCALE_MS, 0.0f, 1.0f);
    state[StageScenario::STATE_ELITE_PULL] = pullActive && pulls.EliteOrHigher ? 1.0f : 0.0f;
    state[StageScenario::STATE_LINKED_PULL] = pullActive && pulls.Linked ? 1.0f : 0.0f;
    state[StageScenario::STATE_TIER] = float(pulls.Rung) / float(std::max<std::size_t>(1, PACK_RUNGS.size() - 1));
}

bool Animus::Curriculum::PullsEncounter::IsTerminal(Env const& env) const
{
    // With an owner nobody's death ends the episode (they stand up after the pull), so letting the owner die is
    // never a way out of the penalties. Alone, a death ends it once no resurrection of its own is left to wait for.
    // A single pack also ends on its clock, as a lost fight rather than a cut-off the critic bootstraps across.
    if (_scenario.Arena(env).Owner)
        return false;

    // A party or raid of its own: a wipe ends it (the pull that did it is cleared away and counted in Recover),
    // never one seat's death -- seat 0 is one raider of forty.
    if (_scenario.Arena(env).PartyGroup)
    {
        EnvPulls const& pulls = _envs[env.Index];
        if (pulls.Wipes > (GroupRun(env) ? pulls.RunWipes : 0))
            return true;
        if (Sequence(env))
            return pulls.PullsCleared >= RunLength(env);
        if (Gauntlet(env))
            return false;
        // A party's single pack or camp ends on its clear: nothing despawns it, so the clear was paid again every
        // decision until the clock ran out.
        return pulls.PullCleared || pulls.PullsCleared > 0 || TimeIsUp(env);
    }

    bool const dead = _scenario.DeadForGood(env, 0);
    if (Sequence(env))
        return dead || _envs[env.Index].PullsCleared >= RunLength(env);
    if (Gauntlet(env))
        return dead;

    return _scenario.Data(env).Seats[0].Combat.Killed || dead || TimeIsUp(env);
}
