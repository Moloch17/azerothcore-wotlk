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
#include "Env.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>
#include <limits>

Animus::Curriculum::PartyEncounter::PartyEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PartyEncounter::RewardTerms() const
{
    return { RewardTerm::TeammateDamageTaken, RewardTerm::TeammateHealing, RewardTerm::TeammateThreat,
        RewardTerm::TeammateDeath, RewardTerm::Threat, RewardTerm::Revive, RewardTerm::DamageDealt,
        RewardTerm::Stall, RewardTerm::EarlyPull };
}

void Animus::Curriculum::PartyEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("seat", [](Env const&, uint32 seat) { return float(seat); });
    table.Add("teammates_died", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].TeammatesDied);
    });
    table.Add("revives", [this](Env const& env, uint32 seat)
    {
        return float(_scenario.Data(env).Seats[seat].Revives);
    });
    table.Add("teammate_damage_taken", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].TeammateDamageTaken);
    });
    table.Add("teammate_healing", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].TeammateHealing);
    });
    // A healer's effectiveness, not its output: the share of its group's member-time in the seat's fights spent alive
    // above half health (a dead member counts against it), and the share of the damage its teammates took that its
    // healing and absorbs undid (1 when they took none). Read for every seat; the learner grades healers on them
    // (layout_sampling.role_metrics, 2026-10-04).
    table.Add("group_kept_share", [this](Env const& env, uint32 seat)
    {
        SeatParty const& party = _envs[env.Index].Seats[seat];
        return party.GroupMemberMs ? float(party.GroupKeptMs) / float(party.GroupMemberMs) : 1.0f;
    });
    table.Add("healing_coverage", [this](Env const& env, uint32 seat)
    {
        SeatParty const& party = _envs[env.Index].Seats[seat];
        return party.TeammateDamageTaken
            ? std::min(1.0f, float(party.TeammateHealing) / float(party.TeammateDamageTaken)) : 1.0f;
    });
    table.Add("threat_on_teammates", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].ThreatOnTeammates);
    });
    table.Add("idle_seconds_in_combat", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].IdleMs) / 1000.0f;
    });

    // The roles' readings (the Deadmines curriculum's phase B): the party tank's share of the party's enemies held, the
    // same on every seat of the party (only the tank's seat counts them; read off one seat, averaged over five, it read
    // a fifth of what the tank held), and a damage dealer's share of its damage on the tank's target and its seconds
    // with an enemy pulled off the tank, zero for the seats whose role it is not.
    table.Add("tank_hold_share", [this](Env const& env, uint32)
    {
        SeatParty const* tank = nullptr;
        for (SeatParty const& party : _envs[env.Index].Seats)
            if (party.EnemiesOnParty && (!tank || party.EnemiesOnParty > tank->EnemiesOnParty))
                tank = &party;
        return tank ? float(tank->EnemiesHeld) / float(tank->EnemiesOnParty) : 0.0f;
    });
    // The party tank's time in its tanking stance, form or aura while alive and fighting, the same on every seat. The
    // form at an episode's end read a dead druid, or one too low to have Bear Form, as one that had left it.
    table.Add("tank_form_share", [this](Env const& env, uint32)
    {
        SeatParty const* tank = nullptr;
        for (SeatParty const& party : _envs[env.Index].Seats)
            if (party.TankFightMs && (!tank || party.TankFightMs > tank->TankFightMs))
                tank = &party;
        return tank ? float(tank->TankModeMs) / float(tank->TankFightMs) : 0.0f;
    });
    table.Add("tank_target_share", [this](Env const& env, uint32 seat)
    {
        SeatParty const& party = _envs[env.Index].Seats[seat];
        return party.DamageDealt ? float(party.TankTargetDamage) / float(party.DamageDealt) : 0.0f;
    });
    table.Add("pulled_off_seconds", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].PulledOffMs) / 1000.0f;
    });
}

void Animus::Curriculum::PartyEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index].Seats.fill(SeatParty());
}

namespace
{
    using namespace Animus::Curriculum;

    /// Whether this build is one the group leans on to keep people standing -- it can hold the pull, or it can
    /// heal. The reward for a teammate's damage taken is smaller for one of these, because taking hits and
    /// spending health is part of what they are there to do.
    bool Protects(Aptitude const& aptitude)
    {
        return AptitudeDemand::HoldsThePull().MetBy(aptitude) || AptitudeDemand::KeepsThemUp().MetBy(aptitude);
    }

    bool Heals(Aptitude const& aptitude) { return AptitudeDemand::KeepsThemUp().MetBy(aptitude); }
    bool HoldsThePull(Aptitude const& aptitude) { return AptitudeDemand::HoldsThePull().MetBy(aptitude); }

    /// The party's tank: a dungeon's party names it (StageScenario::FitsDungeonRole); any other reads it off the build.
    /// The build alone mislabelled both ways -- a bear tank without a shield was charged as a damage dealer pulling
    /// aggro, and a shield-carrying healer counted as the tank (2026-10-03).
    bool IsTank(Animus::Curriculum::SeatState const& state)
    {
        return state.DungeonRole == Animus::Curriculum::DUNGEON_TANK
            || (state.DungeonRole == Animus::Curriculum::DUNGEON_ANY && HoldsThePull(state.Apt));
    }
}

Player* Animus::Curriculum::PartyEncounter::Tank(Env const& env) const
{
    // Whoever is best placed to hold it, rather than whoever was labelled: the living seat with the most
    // mitigation, provided its build can really do the job at all.
    // With no such build in the party, the one with the most mitigation all the same: somebody leads (a party of
    // level-17 builds none of which could hold a pull had nobody to follow and stood at the door, 2026-10-01).
    EnvState const& data = _scenario.Data(env);
    Player* best = nullptr;
    float most = -1.0f;
    bool holds = false;
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
    {
        SeatState const& state = data.Seats[seat];
        if (!state.L)
            continue;

        Player* tank = _scenario.SeatBot(env, seat);
        if (!tank || !tank->IsAlive())
            continue;

        bool const can = IsTank(state);
        if ((can && !holds) || (can == holds && state.Apt[Aptitude::MITIGATION] > most))
        {
            holds = holds || can;
            most = state.Apt[Aptitude::MITIGATION];
            best = tank;
        }
    }

    return best;
}

void Animus::Curriculum::PartyEncounter::BeforeRebuild(Env& env)
{
    // The old party goes before its members do.
    Disband(env);
}

bool Animus::Curriculum::PartyEncounter::Build(Env& env, Map* /*map*/, uint8 /*level*/)
{
    EnvState const& data = _scenario.Data(env);
    Player* lead = _scenario.SeatBot(env, 0);

    // Teammates are friends whatever their races.
    for (uint32 seat = 1; seat < data.ActiveSeats; ++seat)
        _scenario.SeatBot(env, seat)->SetFaction(lead->GetFaction());

    // The party is five learned seats: the first one leads.
    Player* leader = lead;

    EnvParty& party = _envs[env.Index];
    if (party.PartyGroup)
        return true;

    Group* group = new Group();
    group->SetSimGroup(true);
    if (!group->Create(leader))
    {
        LOG_ERROR("module.animus", "{}: env {} could not create its party", _scenario.Name(), env.Index);
        delete group;
        return true;
    }

    sGroupMgr->AddGroup(group);

    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && bot != leader && !group->AddMember(bot))
            LOG_ERROR("module.animus", "{}: env {} could not add seat {} to its party", _scenario.Name(), env.Index,
                seat);

    party.PartyGroup = group;
    return true;
}

void Animus::Curriculum::PartyEncounter::Disband(Env& env)
{
    EnvParty& party = _envs[env.Index];
    if (!party.PartyGroup)
        return;

    // Disband removes it from the group manager and deletes it.
    party.PartyGroup->Disband(true);
    party.PartyGroup = nullptr;
}

void Animus::Curriculum::PartyEncounter::View(Env const& env, uint32 seatIndex, SeatView& view) const
{
    EnvState const& data = _scenario.Data(env);
    uint32 const seats = _scenario.SeatCount();

    std::array<bool, MAX_SEATS> shown{};
    if (seatIndex < MAX_SEATS)
        shown[seatIndex] = true;

    auto const playing = [&](uint32 seat)
    {
        return seat < seats && seat < MAX_SEATS && !shown[seat] && data.Seats[seat].L
            && _scenario.SeatBotInWorld(env, seat);
    };
    auto const fill = [&](uint32 slot, uint32 seat)
    {
        SeatState const& other = data.Seats[seat];
        shown[seat] = true;
        view.Teammates[slot] = { _scenario.SeatBotInWorld(env, seat), other.Holds[0].Goal, other.Apt,
            other.L->Profile->Class };
    };

    // The seat's own group fills the first slots: a party is one group, everyone but the seat.
    uint32 slot = 0;
    uint32 const groupFirst = GroupFirstSeat(seatIndex);
    for (uint32 seat = groupFirst; seat < groupFirst + GROUP_SEATS && slot < GROUP_MEMBERS; ++seat)
        if (playing(seat))
            fill(slot++, seat);

    view.Tank = Tank(env);
}

void Animus::Curriculum::PartyEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    // A dead teammate the seat resurrected stood up: paid here.
    SeatState& reviver = _scenario.Data(env).Seats[seatIndex];
    if (reviver.StepRevivedAlly)
    {
        ledger.Add(RewardTerm::Revive, _scenario.Tuning().Resurrection.ReviveAlly);
        reviver.StepRevivedAlly = false;
    }

    if (!bot)
        return;

    CurriculumTuning::PartyTuning const& tuning = _scenario.Tuning().Party;
    EnvState const& data = _scenario.Data(env);
    SeatParty& seat = _envs[env.Index].Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    Aptitude const& apt = data.Seats[seatIndex].Apt;
    float const healShare = HealShare(data.Seats[seatIndex]);

    // Taking aggro while a teammate is there to hold it (Party.PulledThreat): the enemies on a damage dealer or a
    // healer are the tank's to take, and the seat that draws them is the one to charge.
    if (!IsTank(data.Seats[seatIndex]) && bot->IsAlive())
    {
        bool tankNearby = false;
        for (uint32 other = 0; other < _scenario.SeatCount() && !tankNearby; ++other)
            if (Player* mate = other == seatIndex ? nullptr : _scenario.SeatBotInWorld(env, other);
                mate && mate->IsAlive() && data.Seats[other].L && IsTank(data.Seats[other]))
                tankNearby = true;

        if (tankNearby)
        {
            uint32 onBot = 0;
            for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
                if (Unit* enemy = env.FindTargetUnit(enemySlot);
                    enemy && enemy->IsAlive() && enemy->IsInCombat() && enemy->GetVictim() == bot)
                    ++onBot;
            ledger.Add(RewardTerm::Threat, -tuning.PulledThreat * float(onBot) * _scenario.StepScale(env));
        }
    }

    uint32 const groupFirst = GroupFirstSeat(seatIndex);

    // The group's health while the seat fights (group_kept_share): every member of it, the seat too, alive above half
    // or not -- one that has died counts as not.
    if (bot->IsInCombat())
        for (uint32 member = groupFirst; member < groupFirst + GROUP_SEATS && member < _scenario.SeatCount(); ++member)
        {
            if (!data.Seats[member].L)
                continue;
            Player* mate = _scenario.SeatBotInWorld(env, member);
            seat.GroupMemberMs += _scenario.StepMs(env);
            seat.GroupKeptMs += mate && mate->IsAlive() && mate->GetHealthPct() > 50.0f ? _scenario.StepMs(env) : 0;
        }

    // Every other seat, not only the ones the observation has slots for: a heal lands on whoever needed it.
    for (uint32 teammateSeat = 0; teammateSeat < _scenario.SeatCount(); ++teammateSeat)
    {
        Player* teammate = teammateSeat == seatIndex ? nullptr : _scenario.SeatBotInWorld(env, teammateSeat);
        if (!teammate || !data.Seats[teammateSeat].L)
            continue;

        float const health = float(std::max<uint32>(1, teammate->GetMaxHealth()));
        uint64 const taken = env.StepStats[teammateSeat].DamageTaken;
        // Healing, and what the seat's absorbs soaked and its reductions prevented on the teammate, count alike.
        uint64 const healed = step.AgentHealingBy[teammateSeat] + step.AgentProtectionBy[teammateSeat];

        seat.TeammateDamageTaken += taken;
        seat.TeammateHealing += healed;

        // Somebody who can hold the pull is there to be hit; everyone else being hit is what the party wants to
        // avoid. And the charge is lighter on a build that protects, because keeping people up is its job.
        if (!IsTank(data.Seats[teammateSeat]))
            ledger.Add(RewardTerm::TeammateDamageTaken,
                -(Protects(apt) ? tuning.TeammateDamageTakenProtector : tuning.TeammateDamageTakenDps)
                * float(taken) / health);

        if (Heals(apt))
            ledger.Add(RewardTerm::TeammateHealing, healShare * tuning.TeammateHealing * float(healed)
                / health);

        if (!IsTank(data.Seats[teammateSeat]) && teammate->IsAlive())
        {
            uint32 onTeammate = 0;
            for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
                if (Unit* enemy = env.FindTargetUnit(enemySlot);
                    enemy && enemy->IsAlive() && enemy->IsInCombat() && enemy->GetVictim() == teammate)
                    ++onTeammate;

            seat.ThreatOnTeammates += onTeammate;
            if (IsTank(data.Seats[seatIndex]))
                ledger.Add(RewardTerm::TeammateThreat,
                    -tuning.TankLoseTeammate * float(onTeammate) * _scenario.StepScale(env));
        }

        if (teammate->IsAlive())
            seat.TeammateDeathSeen[teammateSeat] = false;
        else if (!seat.TeammateDeathSeen[teammateSeat])
        {
            seat.TeammateDeathSeen[teammateSeat] = true;
            ++seat.TeammatesDied;
            ledger.Add(RewardTerm::TeammateDeath, -tuning.TeammateDeath);
        }
    }

    RewardRole(env, seatIndex, bot, ledger);
}

float Animus::Curriculum::PartyEncounter::HealShare(SeatState const& seat) const
{
    int32 const primary = seat.Holds[0].Goal;
    int32 const secondary = seat.Holds[1].Goal;
    if (primary == NO_GOAL && secondary == NO_GOAL)
        return 1.0f;
    bool const protecting = (primary != NO_GOAL && SeatGoal(GoalKindOf(primary)) == SeatGoal::Protect)
        || (secondary != NO_GOAL && SeatGoal(GoalKindOf(secondary)) == SeatGoal::Protect);
    return protecting ? 1.0f : _scenario.Tuning().Party.HealOffGoal;
}

bool Animus::Curriculum::PartyEncounter::InTankingStance(Player const* bot)
{
    constexpr uint32 SPELL_RIGHTEOUS_FURY = 25780;
    constexpr uint32 SPELL_FROST_PRESENCE = 48263;
    switch (bot->GetShapeshiftForm())
    {
        case FORM_DEFENSIVESTANCE:
        case FORM_BEAR:
        case FORM_DIREBEAR:
            return true;
        default:
            break;
    }
    return bot->HasAura(SPELL_RIGHTEOUS_FURY) || bot->HasAura(SPELL_FROST_PRESENCE);
}

void Animus::Curriculum::PartyEncounter::RewardRole(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    // Each role paid for its own part (Raid.*): the tank for what it holds, the healer for its group kept up, and
    // every seat charged for standing idle in a fight.
    CurriculumTuning::RaidTuning const& tuning = _scenario.Tuning().Raid;
    EnvState const& data = _scenario.Data(env);
    SeatParty& seat = _envs[env.Index].Seats[seatIndex];
    SeatState const& state = data.Seats[seatIndex];
    Aptitude const& apt = state.Apt;
    AgentStats const& step = env.StepStats[seatIndex];
    float const scale = _scenario.StepScale(env);
    if (!bot->IsAlive())
        return;

    uint32 onBot = 0;
    uint32 onOthers = 0;
    bool enemyNear = false;
    for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
        if (Unit* enemy = env.FindTargetUnit(enemySlot); enemy && enemy->IsAlive() && enemy->IsInCombat())
        {
            Unit const* victim = enemy->GetVictim();
            onBot += victim == bot ? 1 : 0;
            onOthers += victim && victim != bot && victim->IsPlayer() ? 1 : 0;
            enemyNear = enemyNear || (enemy->IsInMap(bot) && bot->GetExactDist(enemy) <= tuning.IdleReach);
        }
    // The party's tank also pays for
    // every enemy on somebody else (Raid.TankLoose): holding the pull is its job, and the Deadmines' parties lost
    // their fights with two of eight enemies on the tank (2026-10-01). A dungeon's drawn tank is the tank.
    bool const tank = IsTank(state);
    bool const healer = state.DungeonRole == DUNGEON_HEALER || (state.DungeonRole == DUNGEON_ANY && Heals(apt));
    ArenaDefinition const& arena = _scenario.Arena(env);
    // A roles arena's drilled seat (G1, Opposition::Roles) is paid its whole lesson by RolesEncounter -- as Outcome,
    // tier-scaled -- so its hold, focus and keep terms are not paid here as well: the readings below are still taken.
    bool const rolesDrilled = seatIndex == 0 && arena.Against == Opposition::Roles && arena.DrillRole
        && arena.DrillRole == state.DungeonRole;
    // What this ledger pays the seat for its role: nothing for a roles arena's drilled seat (RolesEncounter does).
    float const rolePay = rolesDrilled ? 0.0f : 1.0f;
    // The tank in its tanking stance, form or aura while it fights: what a protection warrior, a bear or a paladin with
    // Righteous Fury takes far less from, and holds a pull with.
    if (tank && bot->IsInCombat() && InTankingStance(bot))
        ledger.Add(RewardTerm::Threat, tuning.TankStance * scale);
    if (tank && bot->IsAlive() && bot->IsInCombat())
    {
        SeatParty& reading = _envs[env.Index].Seats[seatIndex];
        reading.TankFightMs += _scenario.StepMs(env);
        reading.TankModeMs += InTankingStance(bot) ? _scenario.StepMs(env) : 0;
    }
    if (tank)
    {
        seat.EnemiesHeld += onBot;
        seat.EnemiesOnParty += onBot + onOthers;
        ledger.Add(RewardTerm::Threat, rolePay * tuning.TankHold * float(onBot) * scale);
        ledger.Add(RewardTerm::Threat, -rolePay * tuning.TankLoose * float(onOthers) * scale);
    }

    // A damage dealer of a party with a tank: paid for the damage it puts on the tank's target, charged for each
    // enemy it has taken off the tank (Raid.TankTarget, Raid.PulledOff). The Deadmines' damage dealers hit whatever
    // was nearest and died with the enemies on them (2026-10-01).
    if (!tank && !healer)
        if (Player* partyTank = Tank(env); partyTank && partyTank != bot && partyTank->IsAlive())
        {
            Unit const* tankTarget = partyTank->GetVictim();
            Unit const* own = bot->GetVictim();
            seat.DamageDealt += step.Damage;
            if (tankTarget && own == tankTarget)
            {
                seat.TankTargetDamage += step.Damage;
                ledger.Add(RewardTerm::DamageDealt, rolePay * tuning.TankTarget * state.LastStepDamage);
            }
            if (onBot)
                seat.PulledOffMs += _scenario.StepMs(env);
            ledger.Add(RewardTerm::Threat, -rolePay * tuning.PulledOff * float(onBot) * scale);
        }

    // A damage dealer or the healer with enemies on it while the party's tank is alive and has not engaged: the pull
    // was opened before the tank was there to take it (Raid.EarlyPull, a cost). A linked pack then turns on whoever
    // opened -- the stage6 parties' damage dealers kept enemies ~8 s a fight (2026-10-03).
    if (!tank && onBot)
        if (Player* partyTank = Tank(env); partyTank && partyTank != bot && partyTank->IsAlive()
            && !partyTank->IsInCombat())
            ledger.Add(RewardTerm::EarlyPull, -tuning.EarlyPull * float(onBot) * scale);

    // The healer keeps its group up: in the Deadmines a level-20 healer cast about five
    // heals a run and its party wiped at the first packs (2026-09-30).
    if (healer)
    {
        uint32 const first = GroupFirstSeat(seatIndex);
        int32 kept = 0;
        for (uint32 member = first; member < first + GROUP_SEATS && member < _scenario.SeatCount(); ++member)
            if (Player* mate = _scenario.SeatBotInWorld(env, member); mate && data.Seats[member].L && mate->IsAlive())
                kept += mate->GetHealthPct() > 50.0f ? 1 : mate->GetHealthPct() < 35.0f ? -1 : 0;
        // Members kept above half pay at the protecting share (Party.HealOffGoal); those let fall below 35% are
        // charged in full whatever the goal.
        float const keptPay = kept > 0 ? HealShare(state) * float(kept) : float(kept);
        ledger.Add(RewardTerm::TeammateHealing, rolePay * tuning.KeepUp * keptPay * scale);

        // Healing that landed on no missing health: what it cast, less what it healed on itself, its allies and the
        // other seats. Charged at a share of what effective healing pays (Raid.Overheal), so it discounts a heal
        // rather than outweighing it: at 2.0 against the pay's 0.5 a heal half wasted cost more than it earned.
        uint64 effective = step.SelfHealing + step.AllyHealing;
        for (uint64 healed : step.AgentHealingBy)
            effective += healed;
        if (step.HealingRaw > effective)
            ledger.Add(RewardTerm::TeammateHealing,
                -rolePay * _scenario.Tuning().Party.TeammateHealing * tuning.Overheal
                * float(step.HealingRaw - effective) / float(std::max<uint32>(1, bot->GetMaxHealth())));
    }

    // Idle: in a fight, an enemy in reach, and nothing done -- no press that served or was neutral, no damage, no
    // healing -- for IdleMs.
    uint64 healed = step.AllyHealing;
    for (uint64 amount : step.AgentHealingBy)
        healed += amount;
    if (step.Damage || healed)
        seat.ActiveMs = env.EpisodeElapsedMs;
    uint64 const active = std::max<uint64>(seat.ActiveMs, state.PurposefulMs);
    if (bot->IsInCombat() && enemyNear && env.EpisodeElapsedMs >= active + tuning.IdleMs)
    {
        seat.IdleMs += _scenario.StepMs(env);
        ledger.Add(RewardTerm::Stall, -tuning.Idle * scale);
    }
}

void Animus::Curriculum::PartyEncounter::Teardown(Env& env)
{
    Disband(env);
}
