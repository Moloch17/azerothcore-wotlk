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

#include "RolesEncounter.h"
#include "PointNavEncounter.h"
#include "BotFactory.h"
#include "CombatEncounter.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Encounters.h"
#include "Env.h"
#include "EnvPool.h"
#include "EntityActions.h"
#include "EpisodeInfoTable.h"
#include "Log.h"
#include "Map.h"
#include "Opponents.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "StageScenario.h"
#include "StageState.h"
#include "Supplies.h"
#include <algorithm>
#include <limits>
#include <cmath>
#include <optional>
#include <utility>

namespace
{
    namespace Draw = Animus::Curriculum::RolesDraw;
    constexpr float TWO_PI = 2.0f * float(M_PI);
    /// How far a pack further on may swing off the line from seat 0 through the pack before it, either way (radians).
    constexpr float NEXT_SPREAD = 1.0f;
    /// A further pack stands between the spacing and this much beyond it.
    constexpr float SPACING_SLACK = 10.0f;
    /// A healer under this share of its mana in a fight is running dry (the measure low_mana_seconds).
    constexpr float LOW_MANA = 0.1f;

    /// An enemy held out of the fight by crowd control: confused (polymorph, hibernate), feared, transformed, or
    /// stunned for longer than a melee stun (a sap, a shackle, a freezing trap).
    constexpr int32 LONG_STUN_MS = 4000;
    bool HeldOut(Creature const* creature)
    {
        if (creature->HasAuraType(SPELL_AURA_MOD_CONFUSE) || creature->HasAuraType(SPELL_AURA_MOD_FEAR)
            || creature->HasAuraType(SPELL_AURA_TRANSFORM))
            return true;
        for (AuraEffect const* stun : creature->GetAuraEffectsByType(SPELL_AURA_MOD_STUN))
            if (stun->GetBase() && stun->GetBase()->GetDuration() >= LONG_STUN_MS)
                return true;
        return false;
    }

    static_assert(Draw::ROLE_TANK == Animus::Curriculum::DUNGEON_TANK
        && Draw::ROLE_HEALER == Animus::Curriculum::DUNGEON_HEALER
        && Draw::ROLE_DAMAGE == Animus::Curriculum::DUNGEON_DAMAGE, "the drills' roles are StageState's DungeonRole");
}

Animus::Curriculum::RolesEncounter::RolesEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs), _ladder(scenario, "roles rung")
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::RolesEncounter::RewardTerms() const
{
    return { RewardTerm::Kill, RewardTerm::FightWon, RewardTerm::PackClear, RewardTerm::KillProgress,
        RewardTerm::HoldPotential, RewardTerm::KeepPotential, RewardTerm::PullExtra, RewardTerm::Death,
        RewardTerm::Away, RewardTerm::InterruptLanded, RewardTerm::FireHurt, RewardTerm::KillOrder,
        RewardTerm::Controlled, RewardTerm::CCBroken, RewardTerm::PartyWipe };
}

float Animus::Curriculum::RolesEncounter::FightTier(Env const& env) const
{
    return Draw::TierWeight(_scenario.Tuning().Difficulty.TierScale, _envs[env.Index].Tier);
}

void Animus::Curriculum::RolesEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    auto const of = [this](auto read)
    {
        return [this, read](Env const& env, uint32) { return float(read(_envs[env.Index])); };
    };
    auto const drilled = [this](RolesDrill drill)
    {
        return [this, drill](Env const& env, uint32) { return _envs[env.Index].Drill == drill ? 1.0f : 0.0f; };
    };
    // The stage's measure: the drill won (RolesDraw::Won), the evaluation videos' success; and by drill, over the
    // episodes of each (PER_EVENT on drill_*).
    table.Add("won", [this](Env const& env, uint32)
    {
        return Draw::Won(_envs[env.Index].Tally, _scenario.Tuning().Roles) ? 1.0f : 0.0f;
    });
    // ... and by the leader's role (StageDefinition::LeaderAnyRole: over leader_<role>, PER_EVENT).
    if (_scenario.Stage().LeaderAnyRole)
        for (auto const& [name, role] : { std::pair{ "won_leader_tank", DUNGEON_TANK },
            std::pair{ "won_leader_healer", DUNGEON_HEALER }, std::pair{ "won_leader_damage", DUNGEON_DAMAGE } })
        {
            uint32 const want = uint32(role);
            table.Add(name, [this, want](Env const& env, uint32)
            {
                PartyEncounter const* party = _scenario.Party();
                return party && party->LeaderRole(env) == want
                    && Draw::Won(_envs[env.Index].Tally, _scenario.Tuning().Roles) ? 1.0f : 0.0f;
            });
        }
    table.Add("drill_hold", drilled(RolesDrill::Hold));
    table.Add("drill_keep", drilled(RolesDrill::Keep));
    table.Add("drill_focus", drilled(RolesDrill::Focus));
    table.Add("drill_pull", drilled(RolesDrill::Pull));
    // The party combat curriculum (2026-10-27): its drills, and the party's readings over every episode.
    table.Add("drill_party", drilled(RolesDrill::Party));
    table.Add("drill_packs", drilled(RolesDrill::Packs));
    table.Add("drill_camp", drilled(RolesDrill::Camp));
    table.Add("interrupts", of([](EnvRoles const& r) { return r.Tally.Interrupts; }));
    table.Add("kill_orders", of([](EnvRoles const& r) { return r.Tally.KillOrders; }));
    table.Add("cc_broken", of([](EnvRoles const& r) { return r.Tally.CCBroken; }));
    table.Add("controlled_seconds", of([](EnvRoles const& r) { return r.Tally.ControlledSeconds; }));
    table.Add("fire_taken", of([](EnvRoles const& r) { return r.Tally.FireTaken; }));
    for (auto const& [name, drill] : { std::pair{ "won_hold", RolesDrill::Hold }, std::pair{ "won_keep",
        RolesDrill::Keep }, std::pair{ "won_focus", RolesDrill::Focus }, std::pair{ "won_pull", RolesDrill::Pull } })
        table.Add(name, [this, drill](Env const& env, uint32)
        {
            EnvRoles const& roles = _envs[env.Index];
            return roles.Drill == drill && Draw::Won(roles.Tally, _scenario.Tuning().Roles) ? 1.0f : 0.0f;
        });

    // Each drill's own reading, over the episodes of its drill (PER_EVENT): the share of the enemy-decisions the tank
    // held; the share of member-decisions in a fight the healer had above half health, and its seconds running dry;
    // the share of the damage dealer's damage on the tank's target, and its seconds with an enemy taken off the tank;
    // the share of the packs cleared that were pulled alone (over packs_cleared).
    table.Add("hold_share", of([](EnvRoles const& r) { return Draw::HoldShare(r.Tally); }));
    table.Add("kept_share", of([](EnvRoles const& r)
    {
        return r.KeptMembers > 0.0f ? r.KeptUp / r.KeptMembers : 0.0f;
    }));
    table.Add("low_mana_seconds", of([](EnvRoles const& r) { return r.LowManaSeconds; }));
    table.Add("focus_share", of([](EnvRoles const& r) { return Draw::FocusShare(r.Tally); }));
    table.Add("pulled_seconds", of([](EnvRoles const& r) { return r.Tally.PulledOffSeconds; }));
    table.Add("clean_share", of([](EnvRoles const& r)
    {
        return r.Tally.Clears ? float(r.CleanClears) / float(r.Tally.Clears) : 0.0f;
    }));
    table.Add("packs_cleared", of([](EnvRoles const& r) { return r.Tally.Clears; }));
    table.Add("clean_pulls", of([](EnvRoles const& r) { return r.CleanClears; }));
    table.Add("extra_pulls", of([](EnvRoles const& r) { return r.Tally.ExtraPulls; }));
    table.Add("pulls", of([](EnvRoles const& r) { return r.Pulls; }));
    table.Add("party_deaths", of([](EnvRoles const& r) { return r.Tally.PartyDeaths; }));
    table.Add("wipes", of([](EnvRoles const& r) { return r.Tally.Wipes; }));

    // I4's measures over the party (RespawnClock, every seat's summed): rises at the entrance, rejoins, the mean
    // seconds from a rise to back with the party (PER_EVENT on rejoins), the share of rises that came back (PER_EVENT
    // on rises), the seconds dead and the seconds charged Away.
    auto const party = [this](auto read)
    {
        return [this, read](Env const& env, uint32)
        {
            float total = 0.0f;
            for (SeatRoles const& seat : _envs[env.Index].Seats)
                total += float(read(seat));
            return total;
        };
    };
    table.Add("rises", party([](SeatRoles const& s) { return s.Clock.Rises; }));
    table.Add("rejoins", party([](SeatRoles const& s) { return s.Clock.Rejoins; }));
    table.Add("rejoin_seconds", [this](Env const& env, uint32)
    {
        uint32 rejoins = 0;
        uint32 ms = 0;
        for (SeatRoles const& seat : _envs[env.Index].Seats)
        {
            rejoins += seat.Clock.Rejoins;
            ms += seat.Clock.RejoinMsTotal;
        }
        return rejoins ? float(ms) / float(rejoins) / 1000.0f : 0.0f;
    });
    table.Add("rejoined", [this](Env const& env, uint32)
    {
        uint32 rises = 0;
        uint32 rejoins = 0;
        for (SeatRoles const& seat : _envs[env.Index].Seats)
        {
            rises += seat.Clock.Rises;
            rejoins += seat.Clock.Rejoins;
        }
        return rises ? float(rejoins) / float(rises) : 0.0f;
    });
    table.Add("dead_seconds", party([](SeatRoles const& s) { return float(s.Clock.OutMsTotal) / 1000.0f; }));
    table.Add("away_seconds", party([](SeatRoles const& s) { return s.AwaySeconds; }));

    // The rung: `roles_rung` for the evaluation videos (Vision::EvalVideoRungColumn) and the status, `difficulty` for
    // the learner's per-class ladder tracking, `at_top_rung` its top.
    table.Add("roles_rung", of([](EnvRoles const& r) { return r.Tier; }));
    table.Add("difficulty", of([](EnvRoles const& r) { return r.Tier; }));
    table.Add("at_top_rung", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Tier >= _scenario.Tuning().Roles.MaxTier ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::RolesEncounter::ResetEpisode(Env& env)
{
    // The creatures stay until the next Build clears them (it has the map); the episode's measures go.
    EnvRoles& roles = _envs[env.Index];
    std::vector<Pack> packs = std::move(roles.Packs);
    roles = EnvRoles();
    roles.Packs = std::move(packs);
}

std::vector<Animus::Curriculum::CombatDraw::Point> const& Animus::Curriculum::RolesEncounter::Corridors(Player* bot,
    Map* map) const
{
    std::lock_guard<std::mutex> guard(_corridorLock);
    auto const known = _corridors.find(map->GetId());
    if (known != _corridors.end())
        return known->second;
    return _corridors.emplace(map->GetId(), CombatEncounter::FindCorridors(bot, map, _scenario.Tuning().Combat,
        _scenario.Name())).first->second;
}

bool Animus::Curriculum::RolesEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    Player* lead = _scenario.SeatBot(env, Draw::DRILLED_SEAT);
    if (!lead || !map)
        return false;
    Despawn(env);

    EnvState& data = _scenario.Data(env);
    SeatState& drilled = data.Seats[Draw::DRILLED_SEAT];
    EnvRoles& roles = _envs[env.Index];
    roles = EnvRoles();
    roles.Drill = _scenario.Arena(env).Roles;
    roles.Tally.Drill = roles.Drill;
    CurriculumTuning::RolesTuning const& tuning = _scenario.Tuning().Roles;

    // The rung: the drilled seat's class and build's own (an evaluation's spread over the seeds).
    roles.Layout = drilled.L ? drilled.L->Index : 0;
    roles.Spec = drilled.Spec;
    DifficultyLadder::Pick const pick = _ladder.Draw(env, roles.Layout, roles.Spec, tuning.MaxTier);
    roles.Tier = pick.Tier;
    roles.Counts = pick.Counts;
    roles.Level = drilled.Level;

    // Where the party starts: seat 0 at one of the dungeon's corridor points, from the entrance it stands at now; the
    // others round it, in its sight.
    // On the dungeon's own creatures (dm4): at a site of the clear way, its packs at the sites on from it.
    PointNavEncounter* nav = _scenario.PointNav();
    PointNavEncounter::ClearWay const* way = _scenario.Arena(env).DungeonCreatures && nav
        ? nav->WayOf(env, map, lead) : nullptr;
    std::vector<CombatDraw::Point> const& corridors = Corridors(lead, map);
    if (way)
    {
        bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED;
        uint32 const draw = seeded ? env.EpisodeSeedIndex : urand(0, std::numeric_limits<int32>::max());
        int32 const site = PointNavEncounter::NextSite(*way, int32(draw % uint32(way->Spawns.size())) - 1);
        Position at;
        if (site >= 0 && PointNavEncounter::SiteSpawn(*way, uint32(site), at))
        {
            roles.Site = site;
            roles.Corridor = site;
            BotFactory::TeleportWithinMap(lead, Position(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(),
                frand(0.0f, TWO_PI)));
        }
    }
    else if (!corridors.empty())
    {
        bool const seeded = env.EpisodeSeedIndex != NO_EPISODE_SEED;
        uint32 const at = seeded ? env.EpisodeSeedIndex % uint32(corridors.size())
            : urand(0, uint32(corridors.size()) - 1);
        CombatDraw::Point const& point = corridors[at];
        roles.Corridor = int32(at);
        BotFactory::TeleportWithinMap(lead, Position(point.X, point.Y, point.Z, frand(0.0f, TWO_PI)));
    }
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < GROUP_SEATS; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot)
            continue;
        if (seat != Draw::DRILLED_SEAT)
        {
            Position at = Opponents::FindSpawnPoint(lead, map, tuning.PartyNearest, tuning.PartyFurthest);
            at.SetOrientation(frand(0.0f, TWO_PI));
            BotFactory::TeleportWithinMap(bot, at);
        }
        _scenario.PrepareFighter(bot, data.Seats[seat]);
        // Food and drink for the rest between packs: the gauntlet block eats and drinks them.
        ConsumablePool const& consumables = ConsumablePool::Instance();
        StockConsumables(bot, consumables.Food(data.Seats[seat].Level),
            bot->GetMaxPower(POWER_MANA) ? consumables.Drink(data.Seats[seat].Level) : 0);
    }

    // The packs: the first in seat 0's sight, the others on from it.
    if (!SpawnPack(env, map, lead, nullptr))
        return false;
    uint32 const standing = Draw::StandingPacks(roles.Drill, roles.Tier, tuning);
    while (roles.Packs.size() < standing && SpawnPack(env, map, lead, &roles.Packs.back().Spot))
        ;
    ListTargets(env);
    return true;
}

bool Animus::Curriculum::RolesEncounter::SpawnPack(Env& env, Map* map, Player* lead, Position const* from)
{
    EnvRoles& roles = _envs[env.Index];
    CurriculumTuning const& all = _scenario.Tuning();
    CurriculumTuning::RolesTuning const& tuning = all.Roles;
    Draw::Pack const plan = Draw::PlanPack(roles.Drill, roles.Tier, tuning);
    CombatDraw::Pull pull;
    pull.LevelOffset = plan.LevelOffset;
    uint8 const level = CombatDraw::CreatureLevel(roles.Level, pull);

    // Who is in it: an elite and a caster first where the rung asks, the rest from the pack pool; on the dungeon's own
    // creatures (dm4), its own entries -- elites where the rung asks, a caster first where it asks one.
    Opponents::OpponentPool const& pool = Opponents::OpponentPool::Instance();
    std::vector<uint32> entries;
    PointNavEncounter* nav = _scenario.PointNav();
    PointNavEncounter::ClearWay const* way = _scenario.Arena(env).DungeonCreatures && nav
        ? nav->WayOf(env, map, lead) : nullptr;
    if (way)
    {
        Opponents::DungeonPool const& own = Opponents::DungeonPool::Of(map->GetId());
        for (uint32 member = 0; member < plan.Size; ++member)
            if (uint32 const entry = own.Random(plan.Elite, plan.Caster && member == 0))
                entries.push_back(entry);
    }
    else if (plan.Elite)
        if (uint32 const elite = pool.RandomElite(level))
            entries.push_back(elite);
    // party_packs' fire-caster: ground fire to step out of.
    if (plan.Hazard)
    {
        if (uint32 const burner = pool.RandomHazardCaster(level))
        {
            if (entries.size() >= plan.Size && !entries.empty())
                entries.back() = burner;
            else
                entries.push_back(burner);
        }
    }
    if (plan.Caster && entries.size() < plan.Size)
        if (uint32 const casting = pool.RandomCaster(level))
            entries.push_back(casting);
    while (!way && entries.size() < plan.Size)
    {
        uint32 const entry = pool.RandomPackMember(level);
        if (!entry)
            break;
        entries.push_back(entry);
    }
    if (entries.empty())
        return false;

    // Where: in seat 0's sight, or the spacing on from the pack before, away from seat 0.
    std::optional<Position> onward;
    if (way)
    {
        // At the next site of the clear way from the last pack's, where the dungeon spawns its own.
        int32 const site = PointNavEncounter::NextSite(*way, roles.Site);
        Position at;
        if (site < 0 || !PointNavEncounter::SiteSpawn(*way, uint32(site), at))
            return false;
        roles.Site = site;
        onward = at;
    }
    else if (from)
    {
        float const spacing = Draw::PackSpacing(roles.Drill, roles.Tier, tuning.MaxTier, tuning,
            all.Combat.NextNearest);
        float const bearing = std::atan2(from->GetPositionY() - lead->GetPositionY(),
            from->GetPositionX() - lead->GetPositionX());
        onward = Opponents::FindSpawnPointFrom(lead, map, *from, bearing, NEXT_SPREAD, spacing,
            spacing + SPACING_SLACK);
        if (!onward)
            return false;
    }
    Position const spot = onward ? *onward
        : Opponents::FindSpawnPoint(lead, map, all.Combat.FightNearest, all.Combat.FightFurthest);
    std::vector<Creature*> members = Opponents::SpawnPack(lead, map, entries, level, spot);
    if (members.empty())
        return false;
    // heal_keep's packs take longer to kill (Roles.KeepHealthPct): the fight outlasts a mana bar.
    if (plan.HealthPct != 100)
        for (Creature* member : members)
        {
            member->ApplyStatPctModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(plan.HealthPct) - 100.0f);
            member->UpdateMaxHealth();
            member->SetFullHealth();
        }

    Pack pack;
    pack.Spot = spot;
    pack.Linked = plan.Linked;
    for (Creature* member : members)
    {
        pack.Members.push_back(member->GetGUID());
        pack.Counted.push_back(false);
        pack.WasControlled.push_back(0);
        pack.LastHealth.push_back(member->GetHealth());
        // Its caster: the first of a casting class (a mage's, or a paladin's that heals), the one to kill first.
        if (pack.Caster.IsEmpty() && (member->getClass() == CLASS_MAGE || member->getClass() == CLASS_PALADIN))
            pack.Caster = member->GetGUID();
    }
    roles.Packs.push_back(std::move(pack));
    return true;
}

void Animus::Curriculum::RolesEncounter::ListTargets(Env& env) const
{
    env.Targets.clear();
    for (Pack const& pack : _envs[env.Index].Packs)
        for (ObjectGuid const& guid : pack.Members)
            if (env.Targets.size() < MAX_TARGETS)
                env.Targets.push_back(guid);
}

Creature* Animus::Curriculum::RolesEncounter::Member(Env const& env, ObjectGuid guid) const
{
    Map* map = env.FindMap();
    return map && !guid.IsEmpty() ? map->GetCreature(guid) : nullptr;
}

void Animus::Curriculum::RolesEncounter::Despawn(Env& env)
{
    EnvRoles& roles = _envs[env.Index];
    for (Pack const& pack : roles.Packs)
        for (ObjectGuid const& guid : pack.Members)
            if (Creature* creature = Member(env, guid))
                creature->DespawnOrUnsummon();
    roles.Packs.clear();
    env.Targets.clear();
}

void Animus::Curriculum::RolesEncounter::UpdateEnemies(Env& env)
{
    EnvRoles& roles = _envs[env.Index];
    for (Pack& pack : roles.Packs)
    {
        Unit* victim = nullptr;
        std::vector<Creature*> idle;
        bool fighting = false;
        for (ObjectGuid const& guid : pack.Members)
            if (Creature* member = Member(env, guid); member && member->IsAlive())
            {
                if (member->IsInCombat())
                {
                    fighting = true;
                    if (member->GetVictim() && !victim)
                        victim = member->GetVictim();
                }
                else
                    idle.push_back(member);
            }
        pack.Fight.Fighting = fighting;
        if (!pack.Engaged && fighting)
        {
            pack.Engaged = true;
            pack.EngageMs = env.EpisodeElapsedMs;
            ++roles.Pulls;
        }
        // A linked pack: one member in a fight brings the rest, on whoever it is fighting.
        if (pack.Linked && victim && victim->IsAlive())
            for (Creature* member : idle)
                if (member->IsAIEnabled && member->CanCreatureAttack(victim))
                    member->AI()->AttackStart(victim);
    }
}

void Animus::Curriculum::RolesEncounter::Update(Env& env)
{
    EnvRoles& roles = _envs[env.Index];
    EnvState& data = _scenario.Data(env);
    Map* map = env.FindMap();
    if (!map)
        return;

    // A death (I4, EntranceRespawn): out for the delay, then alive at the entrance to walk back to the party.
    CurriculumTuning::RespawnTuning const& respawn = _scenario.Tuning().Respawn;
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < GROUP_SEATS; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot)
            continue;
        Position middle;
        float const partyYards = bot->IsAlive() && PartyMiddle(env, seat, middle) ? bot->GetExactDist(&middle) : -1.0f;
        SeatRoles& state = roles.Seats[seat];
        RespawnClock::Step const step = state.Clock.Note(env.EpisodeElapsedMs, bot->IsAlive(), partyYards,
            respawn.DelayMs, respawn.RejoinYards);
        if (step == RespawnClock::Step::Rise && _scenario.Arena(env).RespawnAtEntrance)
        {
            RiseAtEntrance(bot, data.Seats[seat], _scenario.SpawnPointFor(env), env.EpisodeElapsedMs);
            state.Clock.Risen(env.EpisodeElapsedMs);
            state.DeathPaid = false;
        }
    }

    // The standing packs kept up: a cleared one's place taken by a new one beyond the last, whatever happened while
    // the party was away (a spawn that found no ground, a pack cleared while they lay dead).
    Player* lead = _scenario.SeatBot(env, Draw::DRILLED_SEAT);
    if (!lead || !lead->IsAlive())
        return;
    uint32 const standing = Draw::StandingPacks(roles.Drill, roles.Tier, _scenario.Tuning().Roles);
    bool spawned = false;
    if (roles.Packs.empty())
        spawned = SpawnPack(env, map, lead, nullptr);
    while (!roles.Packs.empty() && roles.Packs.size() < standing
        && SpawnPack(env, map, lead, &roles.Packs.back().Spot))
        spawned = true;
    if (spawned)
        ListTargets(env);
}

bool Animus::Curriculum::RolesEncounter::PartyMiddle(Env const& env, uint32 seat, Position& middle) const
{
    EnvState const& data = _scenario.Data(env);
    Player const* self = _scenario.SeatBot(env, seat);
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 count = 0;
    for (uint32 other = 0; other < data.ActiveSeats && other < GROUP_SEATS; ++other)
    {
        Player const* member = other == seat ? nullptr : _scenario.SeatBot(env, other);
        if (!member || !member->IsInWorld() || !member->IsAlive() || !self || member->GetMap() != self->GetMap())
            continue;
        x += member->GetPositionX();
        y += member->GetPositionY();
        z += member->GetPositionZ();
        ++count;
    }
    if (!count)
        return false;
    middle.Relocate(x / float(count), y / float(count), z / float(count));
    return true;
}

bool Animus::Curriculum::RolesEncounter::RouteGoal(Env const& env, uint32 seat, Position& goal) const
{
    Player* bot = _scenario.SeatBotInWorld(env, seat);
    if (!bot || _envs[env.Index].Packs.empty())
        return false;
    goal = FightPoint(env, bot);
    return true;
}

Position Animus::Curriculum::RolesEncounter::FightPoint(Env const& env, Player const* bot) const
{
    EnvRoles const& roles = _envs[env.Index];
    Creature const* nearest = nullptr;
    for (Pack const& pack : roles.Packs)
    {
        if (!pack.Fight.Fighting)
            continue;
        for (ObjectGuid const& guid : pack.Members)
            if (Creature* member = Member(env, guid); member && member->IsAlive() && member->IsInCombat()
                && (!nearest || bot->GetExactDist(member) < bot->GetExactDist(nearest)))
                nearest = member;
    }
    if (nearest)
        return Position(*nearest);
    return roles.Packs.empty() ? Position(*bot) : roles.Packs.front().Spot;
}

bool Animus::Curriculum::RolesEncounter::FrontFighting(Env const& env) const
{
    for (Pack const& pack : _envs[env.Index].Packs)
        if (pack.Fight.Fighting)
            return true;
    return false;
}

void Animus::Curriculum::RolesEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    // The gauntlet block's rest: its food and drink, the packs cleared, how long the fight has gone on. Nothing tells
    // a player when a pack will come, and none here comes on its own -- the party goes to it.
    EnvRoles const& roles = _envs[env.Index];
    view.PullsCleared = roles.Tally.Clears;
    if (view.Bot)
    {
        ConsumablePool const& consumables = ConsumablePool::Instance();
        view.FoodItem = consumables.Food(roles.Level);
        view.DrinkItem = view.Bot->GetMaxPower(POWER_MANA) ? consumables.Drink(roles.Level) : 0;
        view.GauntletSupplies = CONSUMABLE_COUNT;
    }
    for (Pack const& pack : roles.Packs)
        if (pack.Fight.Fighting)
        {
            view.PullTime = std::min(1.0f, float(env.EpisodeElapsedMs - pack.EngageMs) / 60000.0f);
            break;
        }
}

void Animus::Curriculum::RolesEncounter::BeforeRewards(Env& env)
{
    EnvRoles& roles = _envs[env.Index];
    EnvState const& data = _scenario.Data(env);
    roles.NewClears = 0;
    roles.NewClean = 0;
    roles.NewExtra = 0;
    roles.NewKillOrder = 0;
    roles.NewControlled = 0;
    roles.NewBroken = 0;
    roles.NewWipe = false;

    // Packs drawn into a fight beside another: extra pulls, and none of them clean.
    roles.NewExtra = Draw::NoteFights(roles.Packs);
    roles.Tally.ExtraPulls += roles.NewExtra;

    // A wipe: the whole party down at once, counted on the decision it happens.
    bool allDown = data.ActiveSeats > 0;
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < GROUP_SEATS; ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && bot->IsAlive())
            allDown = false;
    if (allDown && !roles.AllDown)
    {
        ++roles.Tally.Wipes;
        roles.NewWipe = true;
    }
    roles.AllDown = allDown;

    // Kill order and crowd control (the party curriculum's): a pack's first death its caster; an enemy held out of
    // its fighting pack by crowd control; crowd control that ended with the enemy freshly hurt, the party's damage
    // having broken it.
    float const decision = float(_scenario.StepMs(env)) / 1000.0f;
    for (Pack& pack : roles.Packs)
        for (std::size_t i = 0; i < pack.Members.size(); ++i)
        {
            Creature* member = Member(env, pack.Members[i]);
            bool const alive = member && member->IsAlive();
            if (!alive && !pack.FirstDeathSeen)
            {
                pack.FirstDeathSeen = true;
                if (!pack.Caster.IsEmpty() && pack.Members[i] == pack.Caster)
                {
                    ++roles.NewKillOrder;
                    ++roles.Tally.KillOrders;
                }
            }
            if (!alive)
                continue;
            bool const held = HeldOut(member);
            if (held && pack.Fight.Fighting)
            {
                ++roles.NewControlled;
                roles.Tally.ControlledSeconds += decision;
            }
            if (pack.WasControlled[i] && !held && member->GetHealth() < pack.LastHealth[i])
            {
                ++roles.NewBroken;
                ++roles.Tally.CCBroken;
            }
            pack.WasControlled[i] = held ? 1 : 0;
            pack.LastHealth[i] = member->GetHealth();
        }

    // Packs cleared: each death counted once, a pack cleared when none of it is left; its place goes to the next.
    bool removed = false;
    for (auto it = roles.Packs.begin(); it != roles.Packs.end();)
    {
        bool alive = false;
        for (std::size_t i = 0; i < it->Members.size(); ++i)
        {
            Creature* member = Member(env, it->Members[i]);
            if (member && member->IsAlive())
                alive = true;
            else
                it->Counted[i] = true;
        }
        if (alive)
        {
            ++it;
            continue;
        }
        ++roles.NewClears;
        ++roles.Tally.Clears;
        // PackClear, the fight tracker's (once a pack: its clear's index this episode).
        _scenario.Data(env).Fights.PackCleared(roles.Tally.Clears);
        if (it->Fight.Clean)
        {
            ++roles.NewClean;
            ++roles.CleanClears;
        }
        it = roles.Packs.erase(it);
        removed = true;
    }
    if (removed)
        ListTargets(env);
}

void Animus::Curriculum::RolesEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    if (!bot || seatIndex >= GROUP_SEATS)
        return;
    EnvRoles& roles = _envs[env.Index];
    EnvState& data = _scenario.Data(env);
    SeatState& state = data.Seats[seatIndex];
    SeatRoles& seat = roles.Seats[seatIndex];
    CurriculumTuning const& all = _scenario.Tuning();
    CurriculumTuning::RolesTuning const& tuning = all.Roles;
    RewardUnits const& units = all.Reward;
    // The rung's factor through Add's tier (a win times it, a loss over it), never into the value.
    float const w = FightTier(env);
    float const scale = _scenario.StepScale(env);
    float const decision = float(_scenario.StepMs(env)) / 1000.0f;
    AgentStats const& step = env.StepStats[seatIndex];
    // The drilled seat: seat 0, playing the drill's role (a makeup that could not fit it pays no drill).
    bool const drilledSeat = seatIndex == Draw::DRILLED_SEAT && state.DungeonRole == Draw::DrilledRole(roles.Drill);
    bool const fighting = FrontFighting(env);
    // The lesson this seat is measured on: the drill's, for the drilled seat; in the party curriculum every seat its
    // own role's (the tank's Hold, the healer's Keep, damage's Focus). Measured only (the Won rule, the columns): the
    // reward structure pays the roles by the fight tracker's hold and keep potentials and the focus weight.
    RolesDrill lesson = drilledSeat ? (roles.Drill == RolesDrill::Pull ? RolesDrill::Hold : roles.Drill)
        : RolesDrill::None;
    if (AllRoles(roles.Drill))
        lesson = state.DungeonRole == DUNGEON_TANK ? RolesDrill::Hold : state.DungeonRole == DUNGEON_HEALER
            ? RolesDrill::Keep : state.DungeonRole == DUNGEON_DAMAGE ? RolesDrill::Focus : RolesDrill::None;

    // Outcome: the packs cleared, the kills and the fights won are the fight tracker's (PackClear, Kill, FightWon).
    // A clean pull is a fight won with no second pack (PullClean, no longer paid; counted as clean_clears).

    // A second pack in a fight is every drill's cost: the pull drill's puller (the camp's tank) in full, the others
    // at Roles.PullOthers.
    bool const puller = (roles.Drill == RolesDrill::Pull && drilledSeat)
        || (roles.Drill == RolesDrill::Camp && state.DungeonRole == DUNGEON_TANK);
    float const pullShare = puller ? 1.0f : tuning.PullOthers;
    if (roles.NewExtra)
        ledger.Add(RewardTerm::PullExtra, -units.ExtraPull * pullShare * float(roles.NewExtra), 1.0f / w);

    // Who the living enemies in a fight are on (the critic's world, never the seat's observation).
    uint32 onSeat = 0;
    uint32 onOthers = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat())
        {
            Unit const* victim = enemy->GetVictim();
            onSeat += victim == bot ? 1 : 0;
            onOthers += victim && victim != bot && (victim->IsPlayer() || victim->IsPet()) ? 1 : 0;
        }

    // The seat's lesson, measured while it stands.
    if (lesson != RolesDrill::None && bot->IsAlive())
    {
        switch (lesson)
        {
            case RolesDrill::Hold:
                roles.Tally.Held += onSeat;
                roles.Tally.OnParty += onSeat + onOthers;
                break;
            case RolesDrill::Focus:
            {
                Player* tank = _scenario.PartyTank(env);
                Unit const* tankTarget = tank && tank != bot && tank->IsAlive() ? tank->GetVictim() : nullptr;
                bool const onTarget = tankTarget && bot->GetVictim() == tankTarget;
                roles.Tally.Damage += state.LastStepDamage;
                roles.Tally.FocusDamage += onTarget ? state.LastStepDamage : 0.0f;
                roles.Tally.PulledOffSeconds += onSeat ? decision : 0.0f;
                break;
            }
            case RolesDrill::Keep:
            {
                // The party as its frames show it, the healer among them, while a fight is on.
                if (!fighting)
                    break;
                Draw::Kept kept;
                for (uint32 member = 0; member < data.ActiveSeats && member < GROUP_SEATS; ++member)
                    if (Player* mate = _scenario.SeatBot(env, member); mate && data.Seats[member].L)
                        Draw::Count(kept, mate->IsAlive(), mate->GetMaxHealth()
                            ? float(mate->GetHealth()) / float(mate->GetMaxHealth()) : 0.0f);
                roles.KeptUp += float(kept.Up);
                roles.KeptMembers += float(kept.Members);
                if (uint32 const mana = bot->GetMaxPower(POWER_MANA);
                    mana && float(bot->GetPower(POWER_MANA)) < LOW_MANA * float(mana))
                    roles.LowManaSeconds += decision;
                break;
            }
            default:
                break;
        }
    }

    // The party curriculum's, every seat's: an interrupt that stopped its cast, ground fire taken, a pack's caster
    // killed first, enemies held out by crowd control, crowd control broken, a wipe.
    if (AllRoles(roles.Drill))
    {
        if (ObjectGuid& pending = roles.PendingInterrupt[seatIndex]; !pending.IsEmpty())
        {
            bool const stopped = std::any_of(env.StepInterruptedTargets.begin(), env.StepInterruptedTargets.end(),
                [&pending](Env::InterruptedCast const& cast) { return cast.Caster == pending; });
            if (stopped)
            {
                ++roles.Tally.Interrupts;
                ledger.AddOnce(RewardTerm::InterruptLanded, roles.Tally.Interrupts, units.Interrupt, w);
            }
            pending.Clear();
        }
        float const fire = float(step.HazardDamage) / float(std::max<uint32>(1, bot->GetMaxHealth()));
        if (fire > 0.0f)
        {
            roles.Tally.FireTaken += fire;
            ledger.Add(RewardTerm::FireHurt, -units.Fire * fire, 1.0f / w);
        }
        if (roles.NewKillOrder)
            ledger.Add(RewardTerm::KillOrder, tuning.KillOrder * float(roles.NewKillOrder), w);
        if (roles.NewControlled)
            ledger.Add(RewardTerm::Controlled, tuning.Controlled * scale * float(roles.NewControlled), w);
        if (roles.NewBroken)
            ledger.Add(RewardTerm::CCBroken, -tuning.CCBroken * float(roles.NewBroken), 1.0f / w);
        if (roles.NewWipe)
            ledger.Add(RewardTerm::PartyWipe, -units.WipeParty, 1.0f / w);
    }

    // The clock while a pack fights is the engaged clock (StageScenario), the damage dealt the kill-progress
    // potential (the fight tracker): no StepCost or DamageDealt here any more.

    // Death: charged once a death; the episode goes on (the rise is Update's).
    if (!bot->IsAlive() && !seat.DeathPaid)
    {
        seat.DeathPaid = true;
        ++seat.Deaths;
        ++roles.Tally.PartyDeaths;
        CombatTally& tally = state.Combat;
        tally.Died = true;
        tally.DeathCounted = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -(data.ActiveSeats > 1 ? units.DeathParty : units.DeathSolo), 1.0f / w);
    }

    // Dead, or away from a fight: charged by the second (never a reward for coming back, which would pay dying).
    float const fightYards = bot->IsAlive() ? bot->GetExactDist(FightPoint(env, bot)) : 0.0f;
    if (CombatDraw::AwayCharged(bot->IsAlive(), seat.Clock.Rejoining, fighting, fightYards, tuning.AwayYards))
    {
        seat.AwaySeconds += decision;
        ledger.Add(RewardTerm::Away, -units.Away * decision);
    }

    // The episode's end: the rung's window (the drilled seat's class and build, once). Survived is no longer paid
    // (the reward structure's FightWon needs the fight's kills; this one had no kill gate).
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (timeIsUp && !roles.Recorded && seatIndex == Draw::DRILLED_SEAT)
    {
        roles.Recorded = true;
        if (roles.Counts)
            _ladder.Record(roles.Layout, roles.Spec, roles.Tier, Draw::Won(roles.Tally, tuning), tuning.MaxTier);
    }
}

void Animus::Curriculum::RolesEncounter::WriteState(Env const& env, float* state) const
{
    // The rung, so the critic can predict a tier-scaled return.
    uint32 const top = std::max<uint32>(1, _scenario.Tuning().Roles.MaxTier);
    state[StageScenario::STATE_TIER] = float(_envs[env.Index].Tier) / float(top);
}

bool Animus::Curriculum::RolesEncounter::IsTerminal(Env const& env) const
{
    // Never a death or a wipe: the episode runs to its clock.
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

void Animus::Curriculum::RolesEncounter::Teardown(Env& env)
{
    Despawn(env);
}

void Animus::Curriculum::RolesEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    if (seat < GROUP_SEATS && !result.PendingInterrupt.IsEmpty())
        _envs[env.Index].PendingInterrupt[seat] = result.PendingInterrupt;
}
