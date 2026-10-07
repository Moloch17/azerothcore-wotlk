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

#include "StageScenario.h"
#include "Env.h"
#include "Log.h"
#include "Random.h"
#include "StandIn.h"
#include <vector>

/*
 * The "human" stand-in's seat (dungeon-curriculum I7; its style is StandIn.h). It is an ordinary party seat -- built,
 * grouped, rewarded and moved by the player controller like the others, observed like them -- whose row a frozen
 * learned partner plays (the learner's partner pool) instead of the policy being trained: the sim only picks the seat
 * and marks it (AgentPresence reports 2, the `present` column 0), and the learner never trains on it.
 */

namespace
{
    using namespace Animus::Curriculum;

    bool CanTank(SeatState const& seat)
    {
        return seat.DungeonRole == DUNGEON_TANK
            || (seat.DungeonRole == DUNGEON_ANY && AptitudeDemand::HoldsThePull().MetBy(seat.Apt)
                && seat.Apt[Aptitude::TAUNT] > 0.0f);
    }

    bool CanHeal(SeatState const& seat)
    {
        return seat.DungeonRole == DUNGEON_HEALER
            || (seat.DungeonRole == DUNGEON_ANY && AptitudeDemand::KeepsThemUp().MetBy(seat.Apt));
    }

    bool FitsRole(SeatState const& seat, StandIn::Role role)
    {
        switch (role)
        {
            case StandIn::Role::Tank: return CanTank(seat);
            case StandIn::Role::Healer: return CanHeal(seat);
            default: return !CanTank(seat) && !CanHeal(seat);
        }
    }
}

int32 Animus::Curriculum::StageScenario::StandInSeat(Env const& env) const
{
    return Data(env).StandInPlay.Seat;
}

bool Animus::Curriculum::StageScenario::StandInLeads(Env const& env) const
{
    EnvState::StandInSeat const& play = Data(env).StandInPlay;
    return play.Seat >= 0 && play.Style.Leads;
}

void Animus::Curriculum::StageScenario::SetStandIn(bool standIn)
{
    bool const before = _standIn.exchange(standIn, std::memory_order_relaxed);
    if (before != standIn)
        LOG_INFO("module.animus", "Animus forge: stage {}: {}", _stage.Name, standIn
            ? "the learner fields the human stand-in (a frozen partner) in its share of the parties"
            : "no human stand-in (the learner has no partner to field)");
}

int32 Animus::Curriculum::StageScenario::StandInShare(uint32 arena) const
{
    // The arena's own (ArenaDefinition::StandInShare and its conf key: the party stages from G2 on), else a roles
    // arena's Roles.StandInShare (G1), else StandIn.Share.
    int32 const own = arena < _arenaStandInShare.size() ? _arenaStandInShare[arena] : -1;
    if (own >= 0)
        return own;
    if (arena < _stage.Arenas.size() && _stage.Arenas[arena].Against == Opposition::Roles)
        return _tuning.Roles.StandInShare;
    return _tuning.StandIn.Share;
}

void Animus::Curriculum::StageScenario::DrawStandIn(Env& env)
{
    EnvState& data = Data(env);
    data.StandInPlay = EnvState::StandInSeat();

    // Only a party or a raid of the stage's own seats, with someone beside it, and only while the learner has a frozen
    // partner to play the seat (MODE_FLAG_STAND_IN). Training: the arena's share of the episodes -- a roles arena's own
    // share, Roles.StandInShare (G1), so the stage that wants the stand-in has it without the others having it too.
    // Evaluation: every episode of the stand-in arm, none otherwise (StandIn::Fields).
    ArenaDefinition const& arena = Arena(env);
    if (!StandIn::Fields(_standIn.load(std::memory_order_relaxed),
        arena.Seats == SeatPlan::Party, data.ActiveSeats, env.Evaluating,
        StandInShare(data.Arena), [](int32 percent) { return roll_chance_i(percent); }))
        return;

    // Its own seed: an evaluation's from the seed index, so the same index meets the same person in every evaluation.
    uint64 const seed = env.Evaluating ? StandIn::EvaluationSeed(env.EpisodeSeedIndex)
        : (uint64(rand32()) << 32) | uint64(rand32());
    // Only a party without an owner can be led by it: an owner leads its own party. Nor a drill's: seat 0 is the
    // drilled role's (ArenaDefinition::DrillRole), and a leading stand-in would sit there.
    StandIn::Style const style = StandIn::Draw(seed, _tuning.StandIn, !arena.DrillRole);

    // A leader sits in seat 0, the group's leader (PartyEncounter::Build). A follower takes a seat whose build plays
    // the role it wants, when there is one, else any; its own random numbers pick among them.
    uint32 chosen = 0;
    if (!style.Leads)
    {
        std::vector<uint32> fits;
        std::vector<uint32> any;
        for (uint32 index = 1; index < data.ActiveSeats; ++index)
        {
            SeatState const& seat = data.Seats[index];
            if (!seat.L || !SeatBot(env, index))
                continue;
            any.push_back(index);
            if (FitsRole(seat, style.Wanted))
                fits.push_back(index);
        }
        std::vector<uint32> const& from = fits.empty() ? any : fits;
        if (from.empty())
            return;
        StandIn::Rng pick(seed ^ 0x5EA7ull);
        chosen = from[pick.Between(0u, uint32(from.size()) - 1)];
    }
    else if (!data.Seats[0].L || !SeatBot(env, 0))
        return;

    SeatState const& seat = data.Seats[chosen];
    data.StandInPlay.Seat = int32(chosen);
    data.StandInPlay.Style = style;
    data.StandInPlay.Role = StandIn::RoleFor(style.Wanted, CanTank(seat), CanHeal(seat));
    LOG_DEBUG("module.animus", "{}: env {} stand-in in seat {}: {} {}", Name(), env.Index, chosen,
        style.Leads ? "leading" : "following", StandIn::ROLE_NAMES[uint32(data.StandInPlay.Role)]);
}

void Animus::Curriculum::StageScenario::AddStandInEpisodeInfo()
{
    if (!_stage.AnyArena([](ArenaDefinition const& arena)
        {
            return arena.Seats == SeatPlan::Party;
        }))
        return;

    // Every row of an episode says whether a stand-in played beside it and how: the learned seats' rows are the ones
    // reported (the stand-in's own is not present), so its style is read off theirs.
    auto const play = [this](Env const& env) -> EnvState::StandInSeat const& { return Data(env).StandInPlay; };
    _info.Add("with_stand_in", [play](Env const& env, uint32) { return play(env).Seat >= 0 ? 1.0f : 0.0f; });
    _info.Add("stand_in_leads", [play](Env const& env, uint32)
    {
        return play(env).Seat >= 0 && play(env).Style.Leads ? 1.0f : 0.0f;
    });
    // Its role, 1 tank, 2 healer, 3 damage (0 none).
    _info.Add("stand_in_role", [play](Env const& env, uint32)
    {
        return play(env).Seat >= 0 ? float(uint32(play(env).Role) + 1) : 0.0f;
    });
}
