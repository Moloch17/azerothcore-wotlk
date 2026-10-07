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
#include "Baselines.h"
#include "Camera.h"
#include "Env.h"
#include "WingTeacher.h"
#include "ObjectAccessor.h"
#include "EntityActions.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "StandIn.h"
#include <algorithm>
#include <cmath>
#include <vector>

/*
 * The "human" stand-in's seat (dungeon-curriculum I7; its styles and behaviour are StandIn.h). It is an ordinary party
 * seat -- built, grouped, rewarded and moved by the player controller like the others -- whose row the scenario plays
 * instead of the learner: its keys are the seek helper's toward where its style takes it, its press the seat's own
 * action from its own row, and the learner is handed a no-op row it neither plays nor trains on (AgentPresence and the
 * `present` column read 0 for it).
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

    bool Ranged(SeatState const& seat)
    {
        return seat.L && seat.L->Profile && seat.Spec < seat.L->Profile->Specs.size()
            && seat.L->Profile->Specs[seat.Spec].Range == RangeBand::Ranged;
    }
}

Unit* Animus::Curriculum::StageScenario::SightTarget(Env const& env, uint32 seatIndex, Player* bot) const
{
    // Only what its sight list names: seen now or remembered, and still there to its client.
    SeatState const& seat = Data(env).Seats[seatIndex];
    if (!bot || !bot->IsInWorld())
        return nullptr;
    if (Unit* selected = ObjectAccessor::GetUnit(*bot, bot->GetTarget()); selected && selected->IsAlive()
        && bot->IsValidAttackTarget(selected)
        && std::find(seat.SightGuids.begin(), seat.SightGuids.end(), selected->GetGUID().GetRawValue())
            != seat.SightGuids.end())
        return selected;
    Unit* fighting = nullptr;
    Unit* nearest = nullptr;
    for (uint64 raw : seat.SightGuids)
    {
        if (!raw)
            continue;
        ObjectGuid const guid(raw);
        if (!guid.IsUnit())
            continue;
        Unit* unit = ObjectAccessor::GetUnit(*bot, guid);
        if (!unit || !unit->IsAlive() || !unit->IsInMap(bot) || !EntityActions::AtClient(bot, unit)
            || !bot->IsValidAttackTarget(unit))
            continue;
        bool const onParty = unit->IsInCombat() && unit->GetVictim() && unit->GetVictim()->IsPlayer();
        if (onParty && (!fighting || bot->GetExactDist(unit) < bot->GetExactDist(fighting)))
            fighting = unit;
        if (!nearest || bot->GetExactDist(unit) < bot->GetExactDist(nearest))
            nearest = unit;
    }
    return fighting ? fighting : nearest;
}

Animus::Curriculum::WingTeacher::Facts Animus::Curriculum::StageScenario::StandInFacts(SeatState const& seat,
    StandIn::Role role)
{
    // Its role is its style's; and it acts only on what it sees: the enemies its sight list names.
    WingTeacher::Facts facts = seat.StandInSeen;
    facts.Is = role == StandIn::Role::Tank ? WingTeacher::Role::Tank : role == StandIn::Role::Healer
        ? WingTeacher::Role::Healer : WingTeacher::Role::Damage;
    uint32 kept = 0;
    for (uint32 i = 0; i < facts.EnemyCount; ++i)
        if (facts.Enemies[i].At.Slot >= 0)
            facts.Enemies[kept++] = facts.Enemies[i];
    facts.EnemyCount = kept;
    if (facts.Pull.Slot < 0)
        facts.Pull = WingTeacher::Place();
    if (facts.Object.Slot < 0)
        facts.Object = WingTeacher::Place();
    return facts;
}

int32 Animus::Curriculum::StageScenario::StandInSeat(Env const& env) const
{
    return Data(env).StandInPlay.Seat;
}

bool Animus::Curriculum::StageScenario::StandInLeads(Env const& env) const
{
    EnvState::StandInSeat const& play = Data(env).StandInPlay;
    return play.Seat >= 0 && play.Plays.GetStyle().Leads;
}

void Animus::Curriculum::StageScenario::SetEvaluationStandIn(bool standIn)
{
    bool const before = _evaluationStandIn.exchange(standIn, std::memory_order_relaxed);
    if (before != standIn)
        LOG_INFO("module.animus", "Animus forge: stage {} evaluates {}", _stage.Name,
            standIn ? "with the human stand-in in every party" : "all-bot parties");
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

    // Only a party or a raid of the stage's own seats, and one with someone beside it.
    ArenaDefinition const& arena = Arena(env);
    if ((arena.Seats != SeatPlan::Party && arena.Seats != SeatPlan::Raid) || data.ActiveSeats < 2)
        return;

    // Training: StandIn.Share of the episodes (no random number is drawn while it is off, so a stage without the
    // stand-in builds exactly the episodes it did) -- a roles arena's own share, Roles.StandInShare (G1), so the stage
    // that wants the stand-in has it without the others having it too. Evaluation: every episode of the stand-in arm,
    // none otherwise.
    StandIn::Tuning const& tuning = _tuning.StandIn;
    int32 const share = StandInShare(data.Arena);
    bool const plays = env.Evaluating ? _evaluationStandIn.load(std::memory_order_relaxed)
        : share > 0 && roll_chance_i(share);
    if (!plays)
        return;

    // Its own seed: an evaluation's from the seed index, so the same index meets the same person in every evaluation.
    uint64 const seed = env.Evaluating ? StandIn::EvaluationSeed(env.EpisodeSeedIndex)
        : (uint64(rand32()) << 32) | uint64(rand32());
    // Only a party without an owner can be led by it: an owner leads its own party. Nor a drill's: seat 0 is the
    // drilled role's (ArenaDefinition::DrillRole), and a leading stand-in would sit there.
    StandIn::Style const style = StandIn::Draw(seed, tuning, !arena.Owner && !arena.DrillRole);

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
    StandIn::Role const role = StandIn::RoleFor(style.Wanted, CanTank(seat), CanHeal(seat));
    data.StandInPlay.Seat = int32(chosen);
    data.StandInPlay.Plays = StandIn::Behaviour(style, role);
    LOG_DEBUG("module.animus", "{}: env {} stand-in in seat {}: {} {}, {} pace, quirks{}{}{}{}{}", Name(), env.Index,
        chosen, style.Leads ? "leading" : "following", StandIn::ROLE_NAMES[uint32(role)],
        style.Speed == StandIn::Pace::Slow ? "slow" : "fast",
        style.Has(StandIn::Quirk::PullEarly) ? " pull_early" : "", style.Has(StandIn::Quirk::Wander) ? " wander" : "",
        style.Has(StandIn::Quirk::Rest) ? " rest" : "", style.Has(StandIn::Quirk::Lag) ? " lag" : "",
        style.Has(StandIn::Quirk::Afk) ? " afk" : "");
}

void Animus::Curriculum::StageScenario::DecideStandIn(Env& env, uint32 seatIndex, float* obs, uint8* mask,
    uint8* image, uint8* map)
{
    EnvState& data = Data(env);
    SeatState& seat = data.Seats[seatIndex];
    EnvState::StandInSeat& play = data.StandInPlay;
    StandIn::Tuning const& tuning = _tuning.StandIn;
    Player* bot = env.FindBot(seatIndex);
    seat.ScriptAction = 0;

    if (bot && seat.L && bot->IsInWorld())
    {
        // What it sees: its own state, the party frames, the enemy its seat is given (the next pull, or what the
        // party fights) and whom it follows -- the group's leader, seat 0, or the owner who leads the party. With the
        // sight block, the enemy is one it sees or remembers: its selection, else the nearest of its sight list in a
        // fight with the party, else the nearest there at all -- never a server list's.
        bool const sight = seat.L->Has(BlockId::Sight);
        Unit* target = sight ? SightTarget(env, seatIndex, bot) : DecisionTarget(env, seatIndex);
        if (target && (!target->IsAlive() || !target->IsInWorld() || target->GetMapId() != bot->GetMapId()))
            target = nullptr;
        Player* leader = Owner(env);
        if (!leader)
            leader = seatIndex != 0 ? SeatBotInWorld(env, 0) : nullptr;
        if (leader && (!leader->IsAlive() || leader == bot || leader->GetMapId() != bot->GetMapId()))
            leader = nullptr;

        StandIn::Situation seen;
        seen.NowMs = env.EpisodeElapsedMs;
        seen.DecisionMs = _decisionMs;
        seen.Alive = bot->IsAlive();
        seen.InCombat = bot->IsInCombat();
        for (uint32 index = 0; index < data.ActiveSeats && !seen.PartyInCombat; ++index)
            if (Player* member = SeatBotInWorld(env, index); member && member->IsAlive())
                seen.PartyInCombat = member->IsInCombat();
        seen.HasTarget = target != nullptr;
        seen.TargetYards = target ? bot->GetExactDist(target) : 0.0f;
        seen.HasLeader = leader != nullptr;
        seen.LeaderYards = leader ? bot->GetExactDist(leader) : 0.0f;
        seen.Health = bot->GetMaxHealth() ? float(bot->GetHealth()) / float(bot->GetMaxHealth()) : 1.0f;
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        seen.Mana = maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f;
        seen.Ranged = Ranged(seat);

        StandIn::Intent const intent = play.Plays.Decide(seen, tuning);
        if (play.Plays.Started() && intent.Doing == StandIn::Mode::Wander)
        {
            play.WanderFromX = bot->GetPositionX();
            play.WanderFromY = bot->GetPositionY();
        }

        // Its feet: held keys the player controller runs every tick, as the follow stage's scripted leader's are.
        float goalX = 0.0f;
        float goalY = 0.0f;
        bool move = seen.Alive && seat.Mover.Started();
        switch (intent.Go)
        {
            case StandIn::Intent::Goal::Target:
                move = move && target;
                if (target)
                {
                    goalX = target->GetPositionX();
                    goalY = target->GetPositionY();
                }
                break;
            case StandIn::Intent::Goal::Leader:
                move = move && leader;
                if (leader)
                {
                    goalX = leader->GetPositionX();
                    goalY = leader->GetPositionY();
                }
                break;
            case StandIn::Intent::Goal::Spot:
                goalX = play.WanderFromX + play.Plays.SpotYards() * std::cos(play.Plays.SpotAngle());
                goalY = play.WanderFromY + play.Plays.SpotYards() * std::sin(play.Plays.SpotAngle());
                break;
            default:
                move = false;
                break;
        }
        bool const face = seen.Alive && intent.Fight && target && seat.Mover.Started();
        Movement::ControlState held = StandIn::Keys(seat.Mover.Body, move, goalX, goalY, intent.StopYards, face,
            target ? target->GetPositionX() : 0.0f, target ? target->GetPositionY() : 0.0f, intent.Walk);
        // A face turn the controller applied and the camera has not yet taken off its offset is kept.
        held.FaceTurnApplied = seat.Controls.Held.FaceTurnApplied;
        seat.Controls.Held = held;

        // Its hands: one press of the seat's own actions, through ApplySeatAction like a learned seat's, never a
        // movement press -- the keys above are its feet. With the sight block (dungeon-curriculum I1, I6) they are the
        // dungeon teacher's hands (WingTeacher::Hands) for its role, on what it sees: it selects, assists, focuses
        // and heals a friend, casts (through the client's handler) and uses objects through the sight list's presses,
        // as a player does. Without it, the "fight" baseline's choice from its own row (the greedy one for a layout
        // without the duel block).
        Layout const& layout = *seat.L;
        if (intent.Fight && sight && obs && mask && seen.Alive)
        {
            int32 const action = Baselines::TeacherPress(WingTeacher::Hands(StandInFacts(seat, play.Plays.GetRole())),
                layout, obs, mask);
            seat.ScriptAction = std::max(action, 0);
            play.Presses += action > 0 ? 1 : 0;
        }
        else if (intent.Fight && target && obs && mask && seen.Alive)
        {
            int32 action = Baselines::Choose(Baselines::Supports("fight", layout) ? "fight" : "greedy", layout, obs,
                mask);
            std::optional<BlockId> const block = action > 0 ? layout.BlockOfAction(uint32(action)) : std::nullopt;
            if (action <= 0 || action >= int32(layout.NumActions) || !mask[action]
                || (block && *block == BlockId::Move))
                action = 0;
            seat.ScriptAction = action;
            play.Presses += action > 0 ? 1 : 0;
        }
    }

    // The learner's row: an empty observation that allows only the no-op, as an inactive owner's is.
    std::fill(obs, obs + _spec.ObsDim, 0.0f);
    if (image)
        Vision::FillNoFrame(image, _spec.ImageBytes);
    if (map)
        std::fill(map, map + _spec.MapBytes, uint8(0));
    if (mask)
    {
        std::fill(mask, mask + _spec.NumActions, uint8(0));
        mask[0] = 1;
    }
}

void Animus::Curriculum::StageScenario::AddStandInEpisodeInfo()
{
    if (!_stage.AnyArena([](ArenaDefinition const& arena)
        {
            return arena.Seats == SeatPlan::Party || arena.Seats == SeatPlan::Raid;
        }))
        return;

    // Every row of an episode says whether a stand-in played beside it and how: the learned seats' rows are the ones
    // reported (the stand-in's own is not present), so its style is read off theirs.
    auto const play = [this](Env const& env) -> EnvState::StandInSeat const& { return Data(env).StandInPlay; };
    _info.Add("with_stand_in", [play](Env const& env, uint32) { return play(env).Seat >= 0 ? 1.0f : 0.0f; });
    _info.Add("stand_in_leads", [play](Env const& env, uint32)
    {
        return play(env).Seat >= 0 && play(env).Plays.GetStyle().Leads ? 1.0f : 0.0f;
    });
    // Its role, 1 tank, 2 healer, 3 damage (0 none).
    _info.Add("stand_in_role", [play](Env const& env, uint32)
    {
        return play(env).Seat >= 0 ? float(uint32(play(env).Plays.GetRole()) + 1) : 0.0f;
    });
    _info.Add("stand_in_slow", [play](Env const& env, uint32)
    {
        return play(env).Seat >= 0 && play(env).Plays.GetStyle().Speed == StandIn::Pace::Slow ? 1.0f : 0.0f;
    });
    // Each quirk: how often it fired this episode.
    for (uint32 index = 0; index < StandIn::QUIRKS; ++index)
        _info.Add(std::string("stand_in_") + StandIn::QUIRK_NAMES[index], [play, index](Env const& env, uint32)
        {
            return play(env).Seat >= 0 ? float(play(env).Plays.Fired(StandIn::Quirk(index))) : 0.0f;
        });
    _info.Add("stand_in_presses", [play](Env const& env, uint32) { return float(play(env).Presses); });
}
