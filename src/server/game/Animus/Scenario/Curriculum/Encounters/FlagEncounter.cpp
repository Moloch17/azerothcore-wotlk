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
#include "BotFactory.h"
#include "Env.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundWS.h"
#include "CombatReward.h"
#include "GameObject.h"
#include "DBCStores.h"
#include "EpisodeInfoTable.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "SeatView.h"

namespace
{
    using State = Animus::Curriculum::SeatView::FlagState;

    constexpr float BASE_SPREAD = 4.0f;     // yards between a side's seats where they start
}

Animus::Curriculum::FlagEncounter::FlagEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::FlagEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::FlagCapture, RewardTerm::FlagPickup, RewardTerm::FlagReturn,
        RewardTerm::CarrierKill, RewardTerm::FlagLost, RewardTerm::Progress, RewardTerm::Death };
}

uint32 Animus::Curriculum::FlagEncounter::SideOf(Env const& env, uint32 seat) const
{
    return _scenario.SideOf(env, seat);
}

void Animus::Curriculum::FlagEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    auto const side = [this](Env const& env, uint32 seat) -> Side const&
    {
        return _envs[env.Index].Sides[SideOf(env, seat)];
    };

    table.Add("flag_captures", [side](Env const& env, uint32 seat) { return float(side(env, seat).Captures); });
    table.Add("flag_pickups", [side](Env const& env, uint32 seat) { return float(side(env, seat).Pickups); });
    table.Add("flag_returns", [side](Env const& env, uint32 seat) { return float(side(env, seat).Returns); });
    table.Add("carrier_kills", [side](Env const& env, uint32 seat) { return float(side(env, seat).CarrierKills); });
    table.Add("flag_deaths", [side](Env const& env, uint32 seat) { return float(side(env, seat).Deaths); });
    table.Add("match_won", [this, side](Env const& env, uint32 seat)
    {
        return side(env, seat).Captures >= _scenario.Tuning().Flag.CapturesToWin ? 1.0f : 0.0f;
    });
    table.Add("team_seat", [this](Env const& env, uint32 seat) { return float(SideOf(env, seat)); });
    table.Add("flag_in_reach", [this](Env const& env, uint32 seat)
    {
        SeatFlagState const& state = _envs[env.Index].Seats[std::min<uint32>(seat, TEAM_MATCH_SEATS - 1)];
        return state.Steps ? float(state.ReachSteps) / float(state.Steps) : 0.0f;
    });
}

void Animus::Curriculum::FlagEncounter::ResetEpisode(Env& env)
{
    Disband(env);
    EndMatch(env);
    _envs[env.Index] = EnvFlags();
}

void Animus::Curriculum::FlagEncounter::Teardown(Env& env)
{
    Disband(env);
    EndMatch(env);
}

void Animus::Curriculum::FlagEncounter::EndMatch(Env& env)
{
    Battleground*& match = _envs[env.Index].Match;
    if (!match)
        return;

    // The seats are rebuilt from scratch every episode, so the match leaves with them: take the players out
    // without the script's own leave path, which would teleport them to an entry point they never came from.
    match->SetStatus(STATUS_WAIT_LEAVE);
    for (auto const& [guid, player] : match->GetPlayers())
        if (player)
            player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE, 0, false, false, TEAM_NEUTRAL);

    // Deleted, not only taken off the manager's list: the destructor also removes the match's creatures and objects,
    // detaches it from its map and lets the map unload. Only the last episode's bots stand on that map (each match
    // has its own, see StageScenario::Rebuild), and they are destroyed before the map is next updated.
    delete match;
    match = nullptr;
}

void Animus::Curriculum::FlagEncounter::ReadMatch(Env& env)
{
    EnvFlags& flags = _envs[env.Index];
    BattlegroundWS* match = static_cast<BattlegroundWS*>(flags.Match);
    if (!match)
        return;

    // The script's score and flag state, turned into the side view the seats already read. A capture is the
    // score going up; the step counters are what the reward pays on, and Reward clears nothing, so both sides
    // of a decision see the same events.
    for (Side& side : flags.Sides)
    {
        side.StepCaptures = side.StepPickups = side.StepReturns = 0;
        side.StepCarrierKills = side.StepLost = 0;
    }

    for (uint32 side = 0; side < TEAM_COUNT; ++side)
    {
        TeamId const team = side == 0 ? TEAM_ALLIANCE : TEAM_HORDE;
        Side& own = flags.Sides[side];

        // Where the flag actually is, from the object itself. The stage names the battleground's arrival points,
        // which are its tunnel mouths -- the flag rooms are further in, so steering by them left the seats at the
        // door with the flag out of reach, half an opportunity a seat a match. The objects only exist once the
        // script has spawned them, so this is taken here rather than in Build.
        uint32 const object = side == 0 ? BG_WS_OBJECT_A_FLAG : BG_WS_OBJECT_H_FLAG;
        if (GameObject const* flag = match->GetBGObject(object, false); flag && flag->IsInWorld())
        {
            Position const at(flag->GetPositionX(), flag->GetPositionY(), flag->GetPositionZ());
            if (match->GetFlagState(team) == BG_WS_FLAG_STATE_ON_GROUND)
                own.Dropped = at;
            else if (match->GetFlagState(team) == BG_WS_FLAG_STATE_ON_BASE)
                own.Base = at;
        }

        uint32 const score = match->GetTeamScore(team);
        own.StepCaptures = score > own.Captures ? score - own.Captures : 0;
        own.Captures = score;

        // A side's own flag: at its base, carried off by the enemy, or lying where the carrier fell. The script
        // reports a state, not events, so the events are the changes in it -- and a side's own flag being taken
        // is the other side's pickup.
        uint8 const state = match->GetFlagState(team);
        State const was = own.State;
        own.State = state == BG_WS_FLAG_STATE_ON_PLAYER ? State::Carried
            : state == BG_WS_FLAG_STATE_ON_GROUND ? State::Dropped : State::AtBase;

        Side& enemy = flags.Sides[1 - side];
        if (was != State::Carried && own.State == State::Carried)
            ++enemy.StepPickups;
        if (was == State::Carried && own.State == State::Dropped)
            ++enemy.StepCarrierKills;
        if (was == State::Dropped && own.State == State::AtBase)
            ++own.StepReturns;

        // Who has it, as a seat rather than a guid, so the goals can escort or chase them.
        ObjectGuid const keeper = match->GetFlagPickerGUID(team);
        own.CarriedBy = NO_SEAT;
        if (keeper)
            for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
                if (Player const* bot = _scenario.SeatBot(env, seat); bot && bot->GetGUID() == keeper)
                {
                    own.CarriedBy = seat;
                    break;
                }
    }
}

Battleground* Animus::Curriculum::FlagEncounter::Match(Env const& env) const
{
    return _envs[env.Index].Match;
}

void Animus::Curriculum::FlagEncounter::BeforeSeats(Env& env, uint8 level)
{
    if (_scenario.Arena(env).Seats != SeatPlan::Teams)
        return;

    EnvFlags& flags = _envs[env.Index];
    if (flags.Match)
        return;

    // The real thing: a Warsong Gulch of this env's own, from the template the server loaded, with its doors,
    // its flags, its graveyards and its score. BattlegroundMgr::Update already runs on the sim's world tick
    // (ForgeWorld), so once this exists the script plays the match.
    PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(MAP_WARSONG_GULCH, level);
    if (!bracket)
    {
        LOG_ERROR("module.animus", "{}: env {} has no Warsong bracket for level {}", _scenario.Name(), env.Index,
            level);
        return;
    }

    Battleground* match = sBattlegroundMgr->CreateNewBattleground(BATTLEGROUND_WS, bracket, 0, false);
    if (!match)
    {
        LOG_ERROR("module.animus", "{}: env {} could not create its battleground", _scenario.Name(), env.Index);
        return;
    }

    // The episode ends the match and EndMatch deletes it: the manager must not delete it first (it did once the
    // match emptied, leaving flags.Match dangling and its map unloading around bots it could not send home).
    match->SetSimOwned(true);
    sBattlegroundMgr->AddBattleground(match);
    flags.Match = match;
}

void Animus::Curriculum::FlagEncounter::FormTeams(Env& env)
{
    EnvFlags& flags = _envs[env.Index];
    if (_scenario.Arena(env).Seats != SeatPlan::Teams)
        return;

    uint32 const seats = _scenario.SeatCount();
    for (uint32 side = 0; side < TEAM_COUNT; ++side)
    {
        if (flags.Groups[side])
            continue;

        // The side's first living seat leads it; the rest join.
        Player* leader = nullptr;
        uint32 const perSide = _scenario.Arena(env).TeamSeats;
        for (uint32 seat = side * perSide; seat < seats && SideOf(env, seat) == side && !leader; ++seat)
            leader = _scenario.SeatBot(env, seat);

        if (!leader)
            continue;

        Group* group = new Group();
        group->SetSimGroup(true);
        if (!group->Create(leader))
        {
            LOG_ERROR("module.animus", "{}: env {} could not create team {}", _scenario.Name(), env.Index, side);
            delete group;
            continue;
        }

        sGroupMgr->AddGroup(group);
        for (uint32 seat = 0; seat < seats; ++seat)
        {
            Player* bot = _scenario.SeatBot(env, seat);
            if (!bot || bot == leader || SideOf(env, seat) != side)
                continue;

            if (!group->AddMember(bot))
                LOG_ERROR("module.animus", "{}: env {} could not add seat {} to team {}", _scenario.Name(),
                    env.Index, seat, side);
        }

        flags.Groups[side] = group;
    }
}

void Animus::Curriculum::FlagEncounter::Disband(Env& env)
{
    for (Group*& group : _envs[env.Index].Groups)
    {
        if (group)
            group->Disband(true);
        group = nullptr;
    }
}

bool Animus::Curriculum::FlagEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    CurriculumTuning::FlagTuning const& tuning = _scenario.Tuning().Flag;
    EnvFlags& flags = _envs[env.Index];
    Player* first = _scenario.SeatBot(env, 0);
    if (!first || !map)
    {
        LOG_ERROR("module.animus", "{}: env {} flag build has no seat 0 ({}) or map ({})", _scenario.Name(),
            env.Index, first != nullptr, map != nullptr);
        return false;
    }

    // The first seat's base is where it stands; the other's a walk away, where it goes now.
    // A scripted match: hand the seats to the battleground and let it start. Its own SetupBattleground has
    // already put the doors and the flags where they belong, BattlegroundMgr::Update runs on the world tick, and
    // from here the script owns the match -- the score, the graveyards, the end.
    if (Battleground* match = flags.Match)
    {
        // Where the flags are, before anything else: the goals and the seat view are built on these, and left at
        // the origin every seat is told to run to (0, 0, 0). The battleground keeps its flags at the two arrival
        // points the stage names, so the stage's bases are the script's bases.
        std::vector<Position> const& bases = _scenario.Stage().FlagBases;
        if (bases.size() < TEAM_COUNT)
        {
            LOG_ERROR("module.animus", "{}: a scripted match needs {} flag bases, the stage names {}",
                _scenario.Name(), TEAM_COUNT, bases.size());
            return false;
        }

        for (uint32 side = 0; side < TEAM_COUNT; ++side)
            flags.Sides[side].Base = bases[side];

        match->SetBgMap(map->ToBattlegroundMap());
        for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
            if (Player* bot = _scenario.SeatBot(env, seat))
            {
                match->AddPlayer(bot);
                // The battleground makes its own raid of each side, which is what a healer needs to see.
                match->AddOrSetPlayerToCorrectBgGroup(bot, bot->GetBgTeamId());
            }

        // Left in STATUS_WAIT_JOIN on purpose, which is what the script needs: Battleground::_ProcessJoin runs
        // only in that state, and it is what calls SetupBattleground -- the flags and the doors. Forcing the
        // match straight to IN_PROGRESS skipped it, so 2,560 matches were played with no flag in either base and
        // not one pickup among them. The countdown it starts is Battleground.PrepTime, which the sim host sets
        // short: the sequence is the real one, it just does not spend two minutes of every episode behind a gate.
        flags.Built = true;
        return true;
    }

    // A stage that knows where its bases are says so; Warsong Gulch's are the battleground's own. Otherwise the
    // first seat stands on base 0 and base 1 is searched for, which is what a stage without a map of its own does.
    std::vector<Position> const& bases = _scenario.Stage().FlagBases;
    if (bases.size() >= TEAM_COUNT)
    {
        for (uint32 side = 0; side < TEAM_COUNT; ++side)
            flags.Sides[side].Base = bases[side];

        if (!BotFactory::TeleportWithinMap(first, flags.Sides[0].Base))
        {
            LOG_ERROR("module.animus", "{}: env {} could not put seat 0 on base 0", _scenario.Name(), env.Index);
            return false;
        }
    }
    else
    {
        flags.Sides[0].Base.Relocate(first);
        // No time budget: this is where the other base stands, a fixed feature of the arena, not a trip a seat
        // has to finish before a clock runs out.
        if (!TravelEncounter::FindPlace(first, map, tuning.BaseMin, tuning.BaseMax, false, flags.Sides[1].Base,
            0.0f))
            return false;

        flags.Sides[1].Base.SetOrientation(flags.Sides[1].Base.GetAngle(&flags.Sides[0].Base));
    }

    // Every seat starts at its own base, spread so ten do not stand in one another. Seat 0 is already at base 0,
    // which is where it was built.
    uint32 const seats = _scenario.SeatCount();
    for (uint32 seat = 1; seat < seats; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot)
        {
            LOG_ERROR("module.animus", "{}: env {} has no seat {} of {}", _scenario.Name(), env.Index, seat, seats);
            return false;
        }

        uint32 const mine = SideOf(env, seat);
        uint32 const perSide = std::max<uint32>(1, _scenario.Arena(env).TeamSeats);
        uint32 const place = seat % perSide;
        Position start = flags.Sides[mine].Base;
        if (place)
        {
            // A ring a few yards out, facing the way the base faces.
            float const angle = float(place) / float(perSide) * 2.0f * float(M_PI);
            float const reach = BASE_SPREAD * (1.0f + float(place % 3) * 0.5f);
            start.m_positionX += reach * std::cos(angle);
            start.m_positionY += reach * std::sin(angle);
        }

        if (!BotFactory::TeleportWithinMap(bot, start))
        {
            LOG_ERROR("module.animus", "{}: env {} could not place seat {} at its base", _scenario.Name(),
                env.Index, seat);
            return false;
        }
    }

    FormTeams(env);
    flags.Built = true;
    return true;
}

void Animus::Curriculum::FlagEncounter::Update(Env& env)
{
    EnvFlags& flags = _envs[env.Index];
    if (!flags.Built)
        return;

    // A scripted match keeps its own flags, score and graveyards, and BattlegroundMgr ticks it. Nothing here
    // runs the rules; what it does is read them back, so the seats are rewarded for what the script counted.
    if (flags.Match)
    {
        ReadMatch(env);
        return;
    }

    CurriculumTuning::FlagTuning const& tuning = _scenario.Tuning().Flag;
    uint32 const now = env.EpisodeElapsedMs;

    // A decision's events are paid to every seat of the side, so they are cleared here rather than by whichever
    // seat is rewarded first.
    for (Side& side : flags.Sides)
    {
        side.StepCaptures = side.StepPickups = side.StepReturns = 0;
        side.StepCarrierKills = side.StepLost = 0;
    }

    uint32 const seats = _scenario.SeatCount();
    for (uint32 seat = 0; seat < seats; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        uint32 const mine = SideOf(env, seat);
        Side& own = flags.Sides[mine];
        Side& enemy = flags.Sides[1 - mine];
        SeatFlagState& state = flags.Seats[std::min<uint32>(seat, TEAM_MATCH_SEATS - 1)];
        if (!bot)
            continue;

        // Only the seat actually holding it is carrying.
        bool const carrying = enemy.State == State::Carried && enemy.CarriedBy == seat;

        // Death: a carrier drops the flag where it fell, and the other side is paid for stopping it.
        if (!bot->IsAlive())
        {
            if (!state.Dead)
            {
                state.Dead = true;
                state.RespawnMs = now + tuning.RespawnMs;
                ++state.StepDeaths;
                ++own.Deaths;
                if (carrying)
                {
                    enemy.State = State::Dropped;
                    enemy.CarriedBy = NO_SEAT;
                    enemy.Dropped.Relocate(bot);
                    enemy.DroppedMs = now;
                    ++enemy.CarrierKills;
                    ++enemy.StepCarrierKills;
                }
            }
            else if (now >= state.RespawnMs)
            {
                // A graveyard wave: back at the base, whole.
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
                BotFactory::TeleportWithinMap(bot, own.Base);
                state.Dead = false;
                state.LastDistance = -1.0f;
            }
            continue;
        }

        // A carrier cannot ride.
        if (carrying && bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);

        auto const touches = [bot, &tuning](Position const& place)
        {
            return bot->GetExactDist2d(&place) <= tuning.TouchDistance;
        };

        // Its own flag lying on the ground: return it.
        if (own.State == State::Dropped && touches(own.Dropped))
        {
            own.State = State::AtBase;
            ++own.Returns;
            ++own.StepReturns;
        }

        // The other side's flag, at its base or dropped: take it.
        if ((enemy.State == State::AtBase && touches(enemy.Base))
            || (enemy.State == State::Dropped && touches(enemy.Dropped)))
        {
            enemy.State = State::Carried;
            enemy.CarriedBy = seat;
            ++own.Pickups;
            ++own.StepPickups;
            if (bot->IsMounted())
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }

        // Home with it while its own flag is there: a capture.
        if (carrying && own.State == State::AtBase && touches(own.Base))
        {
            enemy.State = State::AtBase;
            enemy.CarriedBy = NO_SEAT;
            ++own.Captures;
            ++own.StepCaptures;
            ++enemy.StepLost;
        }
    }

    // A dropped flag nobody touched goes home.
    for (Side& side : flags.Sides)
        if (side.State == State::Dropped && now >= side.DroppedMs + tuning.DroppedReturnMs)
        {
            side.State = State::AtBase;
            side.CarriedBy = NO_SEAT;
        }
}

Animus::Curriculum::FlagEncounter::Goal Animus::Curriculum::FlagEncounter::CurrentGoal(Env const& env, uint32 seat,
    Position& place) const
{
    EnvFlags const& flags = _envs[env.Index];
    uint32 const mine = SideOf(env, seat);
    Side const& own = flags.Sides[mine];
    Side const& enemy = flags.Sides[1 - mine];

    if (enemy.State == State::Carried)
    {
        // The seat carrying it takes it home; the rest of its side escort the carrier.
        if (enemy.CarriedBy == seat)
        {
            place = own.Base;
            return Goal::CaptureHome;
        }

        if (Player* mate = enemy.CarriedBy != NO_SEAT ? _scenario.SeatBot(env, enemy.CarriedBy) : nullptr;
            mate && mate->IsAlive())
        {
            place.Relocate(mate);
            return Goal::CaptureHome;
        }

        place = own.Base;
        return Goal::CaptureHome;
    }

    if (own.State == State::Dropped)
    {
        place = own.Dropped;
        return Goal::ReturnOwn;
    }

    if (enemy.State == State::AtBase)
    {
        // With its own flag carried off, stopping the carrier comes first.
        if (own.State == State::Carried && own.CarriedBy != NO_SEAT)
            if (Player* carrier = _scenario.SeatBot(env, own.CarriedBy); carrier && carrier->IsAlive())
            {
                place.Relocate(carrier);
                return Goal::ChaseCarrier;
            }

        place = enemy.Base;
        return Goal::TakeEnemy;
    }

    place = enemy.Dropped;
    return Goal::PickUpEnemy;
}

void Animus::Curriculum::FlagEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvFlags const& flags = _envs[env.Index];
    if (!flags.Built)
        return;

    uint32 const mine = SideOf(env, seat);
    Side const& own = flags.Sides[mine];
    Side const& enemy = flags.Sides[1 - mine];

    SeatView::FlagMatch& match = view.Flags;
    match.Active = true;
    match.Own = own.State;
    match.Enemy = enemy.State;
    match.OwnBase = own.Base;
    match.EnemyBase = enemy.Base;
    match.OwnDropped = own.Dropped;
    match.EnemyDropped = enemy.Dropped;
    match.OwnScore = own.Captures;
    match.EnemyScore = enemy.Captures;

    // The flag the seat could use right now. A scripted match scores a pickup or a return only when the player
    // uses the object, so the seat needs to know which one is in reach -- the other side's to take, its own to
    // return. A dropped flag is the same object, moved. Stage 11's own rules take by proximity and want none of
    // this, so it stays empty there.
    if (Battleground* scripted = flags.Match)
    {
        Player const* bot = _scenario.SeatBot(env, seat);
        float const reach = _scenario.Tuning().Flag.TouchDistance;
        uint32 const ours = mine == 0 ? BG_WS_OBJECT_A_FLAG : BG_WS_OBJECT_H_FLAG;
        uint32 const theirs = mine == 0 ? BG_WS_OBJECT_H_FLAG : BG_WS_OBJECT_A_FLAG;

        // Only a flag there is something to do with. The other side's is always worth using -- at their base it
        // is a pickup, on the ground it is a pickup. A side's own is worth using only where it fell: at its own
        // base it is just scenery, and the seats start standing on it, so offering it made almost all of what
        // the reach metric counted an action that does nothing.
        bool const ownIsDropped = own.State == State::Dropped;
        for (uint32 which : { theirs, ours })
        {
            if (which == ours && !ownIsDropped)
                continue;

            if (GameObject* flag = scripted->GetBGObject(which, false); flag && bot && flag->IsInWorld()
                && bot->IsWithinDistInMap(flag, reach))
            {
                match.Usable = flag->GetGUID();
                break;
            }
        }
    }

    // Was a flag ever close enough to use? If this stays at zero the seats never reach one and the action is
    // beside the point; if it does not, the action is offered and never taken, which is a different problem.
    SeatFlagState& counting = const_cast<EnvFlags&>(flags).Seats[std::min<uint32>(seat, TEAM_MATCH_SEATS - 1)];
    ++counting.Steps;
    if (match.Usable)
        ++counting.ReachSteps;

    // What its side is doing. Ten seats that cannot see each other play as ten individuals: this is the same
    // RaidView a party reads, over the seat's own team -- how much of it is standing, how much of it is fighting,
    // and how badly the worst of it is hurt.
    uint32 const seats = _scenario.SeatCount();
    uint32 side = 0, alive = 0, fighting = 0, tanks = 0, healers = 0;
    float lowest = 1.0f;
    for (uint32 other = 0; other < seats; ++other)
    {
        if (SideOf(env, other) != mine)
            continue;

        ++side;
        Player const* mate = _scenario.SeatBot(env, other);
        if (!mate || !mate->IsAlive())
            continue;

        ++alive;
        if (mate->IsInCombat())
            ++fighting;

        Aptitude const& apt = _scenario.Data(env).Seats[other].Apt;
        if (AptitudeDemand::HoldsThePull().MetBy(apt))
            ++tanks;
        else if (AptitudeDemand::KeepsThemUp().MetBy(apt))
            ++healers;

        lowest = std::min(lowest, CombatReward::HealthLeft(mate));
    }

    view.Raid.Group = mine;
    view.Raid.Alive = side ? float(alive) / float(side) : 0.0f;
    view.Raid.GroupAlive = view.Raid.Alive;
    view.Raid.InCombat = alive ? float(fighting) / float(alive) : 0.0f;
    view.Raid.LowestHealth = lowest;
    view.Raid.TanksAlive = std::min(1.0f, float(tanks) / float(std::max<uint32>(1, side)));
    view.Raid.HealersAlive = std::min(1.0f, float(healers) / float(std::max<uint32>(1, side)));

    view.HasObjective = CurrentGoal(env, seat, view.Objective) != Goal::None;
}

void Animus::Curriculum::FlagEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::FlagTuning const& tuning = _scenario.Tuning().Flag;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvFlags& flags = _envs[env.Index];
    if (!flags.Built)
        return;

    Side& side = flags.Sides[SideOf(env, seat)];
    SeatFlagState& state = flags.Seats[std::min<uint32>(seat, TEAM_MATCH_SEATS - 1)];

    // What the side did pays the whole side -- Update clears these once a decision, so all of it reads the same
    // events -- and what the seat did costs the seat.
    ledger.Add(RewardTerm::FlagCapture, tuning.Capture * float(side.StepCaptures));
    ledger.Add(RewardTerm::FlagPickup, tuning.Pickup * float(side.StepPickups));
    ledger.Add(RewardTerm::FlagReturn, tuning.Return * float(side.StepReturns));
    ledger.Add(RewardTerm::CarrierKill, tuning.CarrierKill * float(side.StepCarrierKills));
    ledger.Add(RewardTerm::FlagLost, -tuning.Lost * float(side.StepLost));
    ledger.Add(RewardTerm::Death, -tuning.Death * float(state.StepDeaths));
    state.StepDeaths = 0;

    // Potential shaping toward the current goal; a new goal starts it over, so a flag changing hands pays nothing.
    Position place;
    Goal const goal = CurrentGoal(env, seat, place);
    if (!bot || !bot->IsAlive() || goal != state.LastGoal)
    {
        state.LastGoal = goal;
        state.LastDistance = bot && bot->IsAlive() ? bot->GetExactDist2d(&place) : -1.0f;
        return;
    }

    float const distance = bot->GetExactDist2d(&place);
    if (state.LastDistance >= 0.0f)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (state.LastDistance - distance) / 100.0f);
    state.LastDistance = distance;
}

bool Animus::Curriculum::FlagEncounter::IsTerminal(Env const& env) const
{
    EnvFlags const& flags = _envs[env.Index];

    // An episode is a match: it is over when the battleground says so, by captures or by its own clock.
    if (Battleground const* match = flags.Match)
        return match->GetStatus() >= STATUS_WAIT_LEAVE;

    uint32 const toWin = _scenario.Tuning().Flag.CapturesToWin;
    return flags.Sides[0].Captures >= toWin || flags.Sides[1].Captures >= toWin;
}
