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
#include "EnvPool.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Player.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "Creature.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <algorithm>
#include <limits>

Animus::Curriculum::PartyEncounter::PartyEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PartyEncounter::RewardTerms() const
{
    // The reward structure (2026-10-29): the role pay -- Threat (pulled, stance, hold, loose, pulled off),
    // TeammateHealing (kept up, overheal), TeammateThreat, TeammateDamageTaken and the tank's-target DamageDealt --
    // is the fight tracker's hold and keep potentials and focus weight now; the readings behind it stay columns.
    std::vector<RewardTerm> terms = { RewardTerm::TeammateDeath, RewardTerm::Revive, RewardTerm::Stall,
        RewardTerm::EarlyPull };
    if (_scenario.Stage().LeaderAnyRole)
    {
        terms.push_back(RewardTerm::OutOfTurnPull);
        terms.push_back(RewardTerm::LeaderAway);
    }
    return terms;
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

    // The leader and the tank (StageDefinition::LeaderAnyRole): whether the seat led, the leader's role (1 tank, 2
    // healer, 3 damage: the evaluation per leader role), the pulls it started and those out of turn, and its share of
    // the quiet time spent beside the leader (1 for the leader itself).
    if (!_scenario.Stage().LeaderAnyRole)
        return;
    table.Add("is_leader", [this](Env const& env, uint32 seat)
    {
        return _envs[env.Index].Leader == seat ? 1.0f : 0.0f;
    });
    table.Add("leader_role", [this](Env const& env, uint32) { return float(LeaderRole(env)); });
    // ... and by the leader's role (the evaluation per leader role, the user's rule): 1 when the episode's leader is a
    // tank, a healer, a damage dealer -- the weights the per-leader measures are read over (episode_means.PER_EVENT),
    // here the kept-with-leader share and the out-of-turn pulls.
    for (auto const& [suffix, role] : { std::pair{ "tank", DUNGEON_TANK }, std::pair{ "healer", DUNGEON_HEALER },
        std::pair{ "damage", DUNGEON_DAMAGE } })
    {
        uint32 const want = uint32(role);
        table.Add(Acore::StringFormat("leader_{}", suffix), [this, want](Env const& env, uint32)
        {
            return LeaderRole(env) == want ? 1.0f : 0.0f;
        });
        table.Add(Acore::StringFormat("kept_with_leader_{}", suffix), [this, want](Env const& env, uint32 seat)
        {
            SeatParty const& party = _envs[env.Index].Seats[seat];
            if (LeaderRole(env) != want)
                return 0.0f;
            return _envs[env.Index].Leader == seat || !party.QuietMs ? 1.0f
                : float(party.KeptMs) / float(party.QuietMs);
        });
        table.Add(Acore::StringFormat("out_of_turn_leader_{}", suffix), [this, want](Env const& env, uint32 seat)
        {
            return LeaderRole(env) == want ? float(_envs[env.Index].Seats[seat].OutOfTurn) : 0.0f;
        });
    }
    table.Add("pulls_started", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].Pulls);
    });
    table.Add("out_of_turn_pulls", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].OutOfTurn);
    });
    table.Add("kept_with_leader_share", [this](Env const& env, uint32 seat)
    {
        SeatParty const& party = _envs[env.Index].Seats[seat];
        return _envs[env.Index].Leader == seat || !party.QuietMs ? 1.0f
            : float(party.KeptMs) / float(party.QuietMs);
    });
}

void Animus::Curriculum::PartyEncounter::ResetEpisode(Env& env)
{
    EnvParty& party = _envs[env.Index];
    party.Seats.fill(SeatParty());
    party.Engaged.clear();
    party.Fighting = false;
}

namespace
{
    using namespace Animus::Curriculum;

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

    // The party is five learned seats: the first one leads, or (StageDefinition::LeaderAnyRole) one drawn from all of
    // them whatever its role, an evaluation's by its seed.
    EnvParty& party = _envs[env.Index];
    party.Leader = 0;
    if (_scenario.Stage().LeaderAnyRole && data.ActiveSeats > 1)
        party.Leader = env.EpisodeSeedIndex != NO_EPISODE_SEED ? uint32(env.EpisodeSeedIndex) % data.ActiveSeats
            : urand(0, data.ActiveSeats - 1);
    Player* leader = _scenario.SeatBot(env, party.Leader);
    if (!leader)
    {
        party.Leader = 0;
        leader = lead;
    }

    if (party.PartyGroup)
    {
        if (party.PartyGroup->GetLeaderGUID() != leader->GetGUID() && party.PartyGroup->IsMember(leader->GetGUID()))
            party.PartyGroup->ChangeLeader(leader->GetGUID());
        return true;
    }

    Group* group = new Group();
    group->SetSimGroup(true);
    if (!group->Create(leader))
    {
        LOG_ERROR("module.animus", "{}: env {} could not create its party", _scenario.Name(), env.Index);
        delete group;
        return true;
    }

    sGroupMgr->AddGroup(group);
    // Free for all, as a party sets it (2026-10-27): under the default group loot the barrel's gunpowder, a common
    // item, went round-robin -- the seat that opened the barrel was seldom the one allowed to take it, and the
    // party's gunpowder order was carried out 0.5% of the times it was given. The seat that loots it holds it.
    group->SetLootMethod(FREE_FOR_ALL);

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

uint32 Animus::Curriculum::PartyEncounter::LeaderRole(Env const& env) const
{
    uint32 const leader = _envs[env.Index].Leader;
    return leader < MAX_SEATS ? uint32(_scenario.Data(env).Seats[leader].DungeonRole) : 0;
}

bool Animus::Curriculum::PartyEncounter::RouteGoal(Env const& env, uint32 seat, Position& goal) const
{
    EnvParty const& party = _envs[env.Index];
    if (!_scenario.Stage().LeaderAnyRole || seat == party.Leader)
        return false;
    Player* leader = _scenario.SeatBotInWorld(env, party.Leader);
    if (!leader || !leader->IsAlive())
        return false;
    goal = Position(*leader);
    return true;
}

void Animus::Curriculum::PartyEncounter::BeforeRewards(Env& env)
{
    if (_scenario.Stage().LeaderAnyRole)
        JudgePulls(env);
}

void Animus::Curriculum::PartyEncounter::JudgePulls(Env& env)
{
    EnvParty& party = _envs[env.Index];
    EnvState const& data = _scenario.Data(env);
    CurriculumTuning::PartyTuning const& tuning = _scenario.Tuning().Party;
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < MAX_SEATS; ++seat)
        party.Seats[seat].NewOutOfTurn = 0;

    // What fights the party now: the attackers of its living seats.
    std::vector<Creature*> fighting;
    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
    {
        Player* bot = _scenario.SeatBotInWorld(env, seat);
        if (!bot || !bot->IsAlive())
            continue;
        for (Unit* attacker : bot->getAttackers())
            if (Creature* creature = attacker ? attacker->ToCreature() : nullptr; creature && creature->IsAlive()
                && std::find(fighting.begin(), fighting.end(), creature) == fighting.end())
                fighting.push_back(creature);
    }

    // A pull: a fight starting from none. Its puller is the first creature's victim (the seat that hit it or walked
    // into it); out of turn when that is not the tank, or the tank went in where the leader had not taken the party.
    if (!party.Fighting && !fighting.empty())
    {
        Creature const* first = fighting.front();
        for (Creature* creature : fighting)
            if (std::find(party.Engaged.begin(), party.Engaged.end(), creature->GetGUID()) == party.Engaged.end())
            {
                first = creature;
                break;
            }
        // A pet's pull is its owner's (a hunter's or warlock's pet sent in, or walking ahead).
        Unit const* victim = first->GetVictim();
        if (victim)
            victim = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player const* tank = Tank(env);
        Player const* leader = _scenario.SeatBotInWorld(env, party.Leader);
        for (uint32 seat = 0; seat < data.ActiveSeats && seat < MAX_SEATS; ++seat)
        {
            Player const* bot = _scenario.SeatBotInWorld(env, seat);
            if (!bot || bot != victim)
                continue;
            SeatParty& puller = party.Seats[seat];
            ++puller.Pulls;
            bool const notTank = tank && bot != tank;
            bool const ahead = bot == tank && leader && leader != tank && leader->IsAlive()
                && leader->GetExactDist(first) > tuning.LeaderPullYards;
            if (notTank || ahead)
            {
                ++puller.OutOfTurn;
                ++puller.NewOutOfTurn;
            }
        }
    }
    party.Fighting = !fighting.empty();
    party.Engaged.clear();
    for (Creature* creature : fighting)
        party.Engaged.push_back(creature->GetGUID());
}

void Animus::Curriculum::PartyEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    // The leader's rules (StageDefinition::LeaderAnyRole): a pull out of turn costs its puller; away from the leader
    // while nothing fights costs every other seat by the second.
    if (_scenario.Stage().LeaderAnyRole && seatIndex < MAX_SEATS)
    {
        EnvParty& party = _envs[env.Index];
        SeatParty& seat = party.Seats[seatIndex];
        CurriculumTuning::PartyTuning const& tuning = _scenario.Tuning().Party;
        if (seat.NewOutOfTurn)
            ledger.Add(RewardTerm::OutOfTurnPull, -tuning.OutOfTurnPull * float(seat.NewOutOfTurn));
        Player* leader = _scenario.SeatBotInWorld(env, party.Leader);
        if (seatIndex != party.Leader && bot && bot->IsAlive() && !party.Fighting && leader && leader->IsAlive())
        {
            uint32 const stepMs = _scenario.StepMs(env);
            seat.QuietMs += stepMs;
            if (bot->GetExactDist(leader) <= tuning.LeaderAwayYards)
                seat.KeptMs += stepMs;
            else
                ledger.Add(RewardTerm::LeaderAway, -tuning.LeaderAway * float(stepMs) / 1000.0f);
        }
    }

    // A dead teammate the seat resurrected stood up: paid here.
    SeatState& reviver = _scenario.Data(env).Seats[seatIndex];
    if (reviver.StepRevivedAlly)
    {
        ledger.Add(RewardTerm::Revive, _scenario.Tuning().Resurrection.ReviveAlly);
        reviver.StepRevivedAlly = false;
    }

    if (!bot)
        return;

    EnvState const& data = _scenario.Data(env);
    SeatParty& seat = _envs[env.Index].Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];

    // Taking aggro off the tank (Party.PulledThreat until 2026-10-29) is the tank's hold potential's now: no charge
    // here.

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

        uint64 const taken = env.StepStats[teammateSeat].DamageTaken;
        // Healing, and what the seat's absorbs soaked and its reductions prevented on the teammate, count alike.
        uint64 const healed = step.AgentHealingBy[teammateSeat] + step.AgentProtectionBy[teammateSeat];

        // Measured, no longer paid (the reward structure: the healer's keep potential, 2026-10-29).
        seat.TeammateDamageTaken += taken;
        seat.TeammateHealing += healed;

        if (!IsTank(data.Seats[teammateSeat]) && teammate->IsAlive())
        {
            uint32 onTeammate = 0;
            for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
                if (Unit* enemy = env.FindTargetUnit(enemySlot);
                    enemy && enemy->IsAlive() && enemy->IsInCombat() && enemy->GetVictim() == teammate)
                    ++onTeammate;

            seat.ThreatOnTeammates += onTeammate;
        }

        if (teammate->IsAlive())
            seat.TeammateDeathSeen[teammateSeat] = false;
        else if (!seat.TeammateDeathSeen[teammateSeat])
        {
            seat.TeammateDeathSeen[teammateSeat] = true;
            ++seat.TeammatesDied;
            // Each living seat's (Reward.TeammateDeath): the dead one pays its own Death.
            if (bot->IsAlive())
                ledger.Add(RewardTerm::TeammateDeath, -_scenario.Tuning().Reward.TeammateDeath);
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
    // Each role measured for its own part (the tank what it holds, damage its focus; paid by the fight tracker's role
    // potentials since 2026-10-29), and every seat charged for an early pull and for standing idle in a fight.
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
    // The role pay (Raid.TankStance, TankHold, TankLoose, TankTarget, PulledOff, KeepUp, Overheal) is the reward
    // structure's role potentials since 2026-10-29 (the fight tracker's hold and keep, the focus weight): what is
    // below only measures the roles, for the columns, and charges the early pull and standing idle.
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
    }

    // A damage dealer of a party with a tank: the damage it puts on the tank's target, and the time it has enemies
    // taken off the tank (measured; the focus weight and the tank's hold pay it now). The Deadmines' damage dealers
    // hit whatever was nearest and died with the enemies on them (2026-10-01).
    if (!tank && !healer)
        if (Player* partyTank = Tank(env); partyTank && partyTank != bot && partyTank->IsAlive())
        {
            Unit const* tankTarget = partyTank->GetVictim();
            Unit const* own = bot->GetVictim();
            seat.DamageDealt += step.Damage;
            if (tankTarget && own == tankTarget)
                seat.TankTargetDamage += step.Damage;
            if (onBot)
                seat.PulledOffMs += _scenario.StepMs(env);
        }

    // A damage dealer or the healer with enemies on it while the party's tank is alive and has not engaged: the pull
    // was opened before the tank was there to take it (Raid.EarlyPull, a cost). A linked pack then turns on whoever
    // opened -- the stage6 parties' damage dealers kept enemies ~8 s a fight (2026-10-03).
    if (!tank && onBot)
        if (Player* partyTank = Tank(env); partyTank && partyTank != bot && partyTank->IsAlive()
            && !partyTank->IsInCombat())
            ledger.Add(RewardTerm::EarlyPull, -tuning.EarlyPull * float(onBot) * scale);

    // The healer keeping its group up (Raid.KeepUp) and its overheal (Raid.Overheal) were paid here until
    // 2026-10-29: the healer's keep potential (the fight tracker) pays it now.

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
