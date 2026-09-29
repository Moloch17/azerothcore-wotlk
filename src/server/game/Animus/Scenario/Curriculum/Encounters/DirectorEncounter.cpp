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

#include "CombatReward.h"
#include "Encounters.h"
#include "EpisodeInfoTable.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "EncoderSupport.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include <algorithm>

namespace
{
    /// How often the scripted director re-reads the fight. Every decision would let the call flicker between two
    /// equally hurt enemies; a seat cannot act on an order that changes under it.
    constexpr uint32 THINK_EVERY = 10;          // decisions, so 2.5 s at the usual 250 ms

    /// Below this the side is told to recover rather than press.
    constexpr float HURT = 0.4f;
}

Animus::Curriculum::DirectorEncounter::DirectorEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::DirectorEncounter::RewardTerms() const
{
    return { RewardTerm::OrderMatch, RewardTerm::PlaceMatch };
}

void Animus::Curriculum::DirectorEncounter::BeforeRewards(Env& env)
{
    _envs[env.Index].Shaping.fill(0.0f);
}

float Animus::Curriculum::DirectorEncounter::ShapingPaid(Env const& env, uint32 seat) const
{
    return seat < MAX_SEATS ? _envs[env.Index].Shaping[seat] : 0.0f;
}

void Animus::Curriculum::DirectorEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    if (!bot || !bot->IsAlive() || seat >= MAX_SEATS)
        return;

    EnvDirector& state = _envs[env.Index];
    SideOrder const& order = state.Sides[_scenario.SideOf(env, seat)];
    RewardPlace(env, seat, bot, ledger);

    // Compliance shaping, per decision and small (Order.Focus): the seat's own order when it holds one, else the
    // side's focus. Only an order the seat could be following: a dead or vanished target is not one.
    uint32 const slot = SlotOf(env, seat);
    DirectorOrders::MemberOrder const own = slot < order.Members.size() ? order.Members[slot]
        : DirectorOrders::MemberOrder();
    Unit const* target = _scenario.SeatTarget(env, seat);
    float const paid = _scenario.Tuning().Order.Focus * _scenario.DecisionScale();
    bool complied = false;
    if (own.Kind != OrderKind::None && own.Kind != OrderKind::GoTo && own.Kind != OrderKind::Objective)
    {
        Unit const* ordered = own.Target ? ObjectAccessor::GetUnit(*bot, own.Target) : nullptr;
        if (!ordered || !ordered->IsAlive())
            return;
        switch (own.Kind)
        {
            case OrderKind::Focus:
            case OrderKind::Interrupt:
                complied = target && target->GetGUID() == own.Target;
                break;
            case OrderKind::Tank:
                complied = ordered->GetVictim() == bot;
                break;
            case OrderKind::Control:
                complied = Encoding::IsCrowdControlled(ordered);
                break;
            case OrderKind::Heal:
                complied = env.StepStats[seat].AllyHealing > 0 || bot->IsNonMeleeSpellCast(false);
                break;
            default:
                break;
        }
    }
    else if (order.Focus)
    {
        Unit const* focus = ObjectAccessor::GetUnit(*bot, order.Focus);
        complied = focus && focus->IsAlive() && target && target->GetGUID() == order.Focus;
    }

    if (!complied)
        return;
    ledger.Add(RewardTerm::OrderMatch, paid);
    state.Shaping[seat] += paid;
}

/// Paid for arriving where the side was sent, once, on the crossing.
///
/// Never per decision. Nothing pays a seat for going where it is told today, so the place channel would stay
/// as inert as it has been since it was written -- but a per-decision payment for standing in the right spot
/// is the exact shape that made the focus nudge 23.7% of gross before it was cut, and a seat could farm it by
/// stepping over the edge and back. A crossing with a cooldown can be earned once and then only by going
/// somewhere else first.
void Animus::Curriculum::DirectorEncounter::RewardPlace(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    EnvDirector& state = _envs[env.Index];
    SideOrder& order = state.Sides[_scenario.SideOf(env, seat)];
    if (seat >= MAX_SEATS)
        return;

    uint32 const slot = SlotOf(env, seat);
    bool const sent = order.Rally == TeamRally::Point
        || (slot < order.Members.size() && order.Members[slot].Kind == OrderKind::GoTo);
    bool const inside = bot && bot->IsAlive() && order.HasPlace && sent
        && bot->GetExactDist2d(&order.Place) <= _scenario.Tuning().Order.PlaceRadius;
    bool const arrived = inside && !order.WasAtPlace[seat];
    order.WasAtPlace[seat] = inside ? 1 : 0;
    if (!arrived)
        return;

    uint32 const cooldown = _scenario.Tuning().Order.PlaceCooldownMs;
    if (order.PlacePaidMs[seat] && env.EpisodeElapsedMs < order.PlacePaidMs[seat] + cooldown)
        return;

    order.PlacePaidMs[seat] = std::max<uint32>(1, env.EpisodeElapsedMs);
    float const paid = _scenario.Tuning().Order.PlaceMatch;
    ledger.Add(RewardTerm::PlaceMatch, paid);
    state.Shaping[seat] += paid;
}

void Animus::Curriculum::DirectorEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // What the director did, so a directed stage can be read against the undirected one it came from.
    table.Add("order_changes", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Changes);
    });
    table.Add("order_posture", [this](Env const& env, uint32 seat)
    {
        return float(uint32(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Posture));
    });
    table.Add("order_has_focus", [this](Env const& env, uint32 seat)
    {
        return _envs[env.Index].Sides[_scenario.SideOf(env, seat)].Focus ? 1.0f : 0.0f;
    });
    // Whether the call was worth following, which is upstream of whether it was followed. A seat that ignores a
    // director and a director that names nothing worth fighting look identical in order_focus_kept alone.
    auto const share = [this](Env const& env, uint32 seat, auto pick)
    {
        SideOrder const& side = _envs[env.Index].Sides[_scenario.SideOf(env, seat)];
        return side.Decisions ? float(pick(side)) / float(side.Decisions) : 0.0f;
    };
    table.Add("order_focus_alive", [share](Env const& env, uint32 seat)
    {
        return share(env, seat, [](SideOrder const& side) { return float(side.FocusAlive); });
    });
    table.Add("order_focus_lowest", [share](Env const& env, uint32 seat)
    {
        return share(env, seat, [](SideOrder const& side) { return float(side.FocusLowest); });
    });
    table.Add("order_focus_chance", [share](Env const& env, uint32 seat)
    {
        return share(env, seat, [](SideOrder const& side) { return side.ChanceSum; });
    });
    // The call pointed at an enemy the side could not see: a director working from memory, not sight.
    table.Add("order_focus_unseen", [share](Env const& env, uint32 seat)
    {
        return share(env, seat, [](SideOrder const& side) { return float(side.FocusUnseen); });
    });
    // Enemy slots the side could see, averaged over the decisions it had something to call.
    table.Add("director_enemies_seen", [share](Env const& env, uint32 seat)
    {
        return share(env, seat, [](SideOrder const& side) { return side.SeenSum; });
    });
    table.Add("order_place_called", [share](Env const& env, uint32 seat)
    {
        return share(env, seat, [](SideOrder const& side) { return float(side.PlaceCalled); });
    });
    // Seat-decisions spent inside the called place, against the decisions one was standing.
    table.Add("order_place_reached", [this](Env const& env, uint32 seat)
    {
        SideOrder const& side = _envs[env.Index].Sides[_scenario.SideOf(env, seat)];
        return side.PlaceCalled ? float(side.PlaceReached) / float(side.PlaceCalled) : 0.0f;
    });
    table.Add("order_place_distance", [this](Env const& env, uint32 seat)
    {
        SideOrder const& side = _envs[env.Index].Sides[_scenario.SideOf(env, seat)];
        return side.PlaceCalled ? side.PlaceDistanceSum / float(side.PlaceCalled) : 0.0f;
    });
    // The director's turns, the calls it made in them, and the orders it gave members by name or by group.
    table.Add("director_turns", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Turns);
    });
    table.Add("director_calls", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Calls);
    });
    table.Add("director_member_orders", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].MemberOrders);
    });
    // The seat held an order of its own at the episode's end (a snapshot of whether they are being used).
    table.Add("order_own_kind", [this](Env const& env, uint32 seat)
    {
        uint32 const slot = SlotOf(env, seat);
        return slot < DirectorLayout::DIRECTOR_SEATS
            ? float(uint32(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Members[slot].Kind)) : 0.0f;
    });
    // Whether the side was doing what it was told: the share of its seats on the called target.
    table.Add("order_focus_kept", [this](Env const& env, uint32 seat)
    {
        SideOrder const& side = _envs[env.Index].Sides[_scenario.SideOf(env, seat)];
        Unit const* target = _scenario.SeatTarget(env, seat);
        return side.Focus && target && target->GetGUID() == side.Focus ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::DirectorEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvDirector();
}

bool Animus::Curriculum::DirectorEncounter::Learned(Env const& env) const
{
    ArenaDefinition const& arena = _scenario.Arena(env);
    return arena.Directed && arena.DirectorLearned;
}

/// What the side can see this decision, and what it keeps of what it has seen.
///
/// This is the whole of the director's fog of war. Before it, ViewSide read every enemy's exact health,
/// position, casting state and even its class/role straight out of the world with no visibility check at all,
/// which made the commander of a side strictly better informed than every seat in it.
void Animus::Curriculum::DirectorEncounter::Observe(Env& env, uint32 side)
{
    SideKnowledge& known = _envs[env.Index].Knowledge[side];

    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const count = Enemies(env, side, theirs);

    known.Seen.fill(0);
    for (uint32 slot = 0; slot < count; ++slot)
    {
        Unit* enemy = theirs[slot];
        EnemyMemory& memory = known.Enemies[slot];
        if (!enemy)
            continue;

        // A slot reused by someone else starts again: what the side remembers is about whoever is there now.
        if (memory.Guid != enemy->GetGUID())
            memory = EnemyMemory();

        if (!_scenario.SideCanSee(env, side, enemy))
            continue;

        known.Seen[slot] = 1;
        memory.Guid = enemy->GetGUID();
        memory.Known = true;
        memory.LastSeen.Relocate(enemy);
        memory.LastSeenMs = env.EpisodeElapsedMs;
        memory.Health = CombatReward::HealthLeft(enemy);
        memory.Alive = enemy->IsAlive();
        // A player's build is its seat's; a creature has none the side could read.
        for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
            if (Player const* bot = _scenario.SeatBot(env, seat); bot && bot == enemy && _scenario.Data(env).Seats[seat].L)
                memory.Apt = _scenario.Data(env).Seats[seat].Apt;
    }
}

uint32 Animus::Curriculum::DirectorEncounter::Enemies(Env const& env, uint32 side,
    std::array<Unit*, PACK_SLOTS>& out) const
{
    out.fill(nullptr);
    SeatPlan const plan = _scenario.Arena(env).Seats;
    if (plan == SeatPlan::Teams || plan == SeatPlan::Mirror)
    {
        std::array<uint32, TEAM_SEATS> theirs{};
        uint32 const count = std::min(_scenario.SideSeats(env, side ? 0 : 1, theirs), PACK_SLOTS);
        for (uint32 slot = 0; slot < count; ++slot)
            out[slot] = _scenario.SeatBot(env, theirs[slot]);
        return count;
    }

    // Against creatures: the env's target slots, which every seat's pack block selects between.
    uint32 const count = std::min<uint32>(uint32(env.Targets.size()), PACK_SLOTS);
    for (uint32 slot = 0; slot < count; ++slot)
        out[slot] = env.FindTargetUnit(slot);
    return count;
}

uint32 Animus::Curriculum::DirectorEncounter::Members(Env const& env, uint32 side,
    std::array<uint32, DirectorLayout::DIRECTOR_SEATS>& out) const
{
    out.fill(NO_SEAT);
    uint32 count = 0;
    for (uint32 seat = 0; seat < _scenario.SeatCount() && count < out.size(); ++seat)
        if (_scenario.SideOf(env, seat) == side && _scenario.Data(env).Seats[seat].L)
            out[count++] = seat;
    return count;
}

/// Rebuild the standing place from the three fields that name it.
///
/// Recomputed every decision rather than stored once, so a place hung on the focus or on the side's own centre
/// follows them as they move -- "stay behind him" has to mean behind him now, not behind where he was when it
/// was said. HasPlace is derived here and nowhere else.
void Animus::Curriculum::DirectorEncounter::ResolvePlace(Env& env, uint32 side)
{
    EnvDirector& state = _envs[env.Index];
    SideOrder& order = state.Sides[side];
    SideKnowledge const& known = state.Knowledge[side];

    order.HasPlace = false;
    bool sending = order.Rally == TeamRally::Point;
    for (DirectorOrders::MemberOrder const& member : order.Members)
        sending = sending || member.Kind == OrderKind::GoTo;
    if (!sending)
        return;

    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, side, mine);

    // The side's own centre, and a living seat to read the map and the phase from.
    Player const* anySeat = nullptr;
    float centreX = 0.0f, centreY = 0.0f, centreZ = 0.0f;
    uint32 standing = 0;
    for (uint32 slot = 0; slot < own; ++slot)
        if (Player const* bot = _scenario.SeatBot(env, mine[slot]); bot && bot->IsAlive())
        {
            centreX += bot->GetPositionX();
            centreY += bot->GetPositionY();
            centreZ += bot->GetPositionZ();
            anySeat = bot;
            ++standing;
        }

    if (!standing || !anySeat)
        return;

    centreX /= float(standing);
    centreY /= float(standing);
    centreZ /= float(standing);

    // Where the enemy is, as far as the side knows: the axis every offset is measured along, and the anchor
    // for the two that name an enemy.
    float enemyX = 0.0f, enemyY = 0.0f;
    uint32 enemies = 0;
    Position lastSeen;
    uint32 lastSeenMs = 0;
    bool haveLastSeen = false;
    for (uint32 slot = 0; slot < PACK_SLOTS; ++slot)
    {
        EnemyMemory const& memory = known.Enemies[slot];
        if (!memory.Known)
            continue;

        enemyX += memory.LastSeen.GetPositionX();
        enemyY += memory.LastSeen.GetPositionY();
        ++enemies;
        if (!haveLastSeen || memory.LastSeenMs >= lastSeenMs)
        {
            lastSeen = memory.LastSeen;
            lastSeenMs = memory.LastSeenMs;
            haveLastSeen = true;
        }
    }

    Position anchor;
    switch (order.Anchor)
    {
        case PlaceAnchor::TeamCentre:
            anchor.Relocate(centreX, centreY, centreZ);
            break;
        case PlaceAnchor::Focus:
        {
            bool found = false;
            for (uint32 slot = 0; slot < PACK_SLOTS && !found; ++slot)
                if (known.Enemies[slot].Known && known.Enemies[slot].Guid == order.Focus)
                {
                    anchor = known.Enemies[slot].LastSeen;
                    found = true;
                }

            if (!found)
                return;
            break;
        }
        case PlaceAnchor::LastSeenEnemy:
            if (!haveLastSeen)
                return;
            anchor = lastSeen;
            break;
        // The objective and the bases are the flag match's to know (Encounter::ViewDirector), and nothing
        // fills them in yet. Naming one resolves to nothing rather than to somewhere wrong.
        case PlaceAnchor::Objective:
        case PlaceAnchor::OwnBase:
        case PlaceAnchor::EnemyBase:
        case PlaceAnchor::Count:
            return;
    }

    // The axis: from the anchor towards where the enemy is. With no enemy known at all, the side's own facing
    // is the only orientation it has.
    float axis = anySeat->GetOrientation();
    if (enemies)
    {
        float const dx = enemyX / float(enemies) - anchor.GetPositionX();
        float const dy = enemyY / float(enemies) - anchor.GetPositionY();
        if (dx != 0.0f || dy != 0.0f)
            axis = std::atan2(dy, dx);
    }

    float const radius = order.Ring == PlaceRing::Far ? _scenario.Tuning().Director.PlaceFarYards
        : _scenario.Tuning().Director.PlaceNearYards;

    float bearing = axis;
    switch (order.Offset)
    {
        case PlaceOffset::At:     break;
        case PlaceOffset::Toward: break;
        case PlaceOffset::Away:   bearing = axis + float(M_PI); break;
        case PlaceOffset::Left:   bearing = axis + float(M_PI) / 2.0f; break;
        case PlaceOffset::Right:  bearing = axis - float(M_PI) / 2.0f; break;
        case PlaceOffset::Count:  return;
    }

    Position place = anchor;
    if (order.Offset != PlaceOffset::At)
        place.Relocate(anchor.GetPositionX() + radius * std::cos(bearing),
            anchor.GetPositionY() + radius * std::sin(bearing), anchor.GetPositionZ());

    // Somewhere the side could actually stand. An unsnapped point sends everyone into a wall.
    if (!Encoding::SnapToGround(anySeat->FindMap(), anySeat->GetPhaseMask(), place, anchor.GetPositionZ()))
        return;

    order.Place = place;
    order.HasPlace = true;
}

void Animus::Curriculum::DirectorEncounter::Forget(Env& env, uint32 side)
{
    EnvDirector& state = _envs[env.Index];
    SideOrder& order = state.Sides[side];

    // Whether an enemy is still worth an order: for a learned director, as far as its side knows -- a call that
    // quietly cleared itself the instant its target died would be ground truth arriving through the back door.
    // The scripted director reads the world: it is the fixed yardstick the learned one is scored against.
    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const count = Enemies(env, side, theirs);
    auto const worth = [&](ObjectGuid guid)
    {
        if (Learned(env))
        {
            SideKnowledge const& known = state.Knowledge[side];
            for (uint32 slot = 0; slot < count && slot < PACK_SLOTS; ++slot)
                if (known.Enemies[slot].Known && known.Enemies[slot].Alive && known.Enemies[slot].Guid == guid)
                    return true;
            return false;
        }
        for (uint32 slot = 0; slot < count; ++slot)
            if (theirs[slot] && theirs[slot]->GetGUID() == guid && theirs[slot]->IsAlive())
                return true;
        return false;
    };

    if (order.Focus && !worth(order.Focus))
    {
        order.Focus = ObjectGuid::Empty;
        order.Changed(state.Steps);
    }

    // A member's own order ends with its target, or with the member.
    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, side, mine);
    for (uint32 slot = 0; slot < own; ++slot)
    {
        DirectorOrders::MemberOrder& member = order.Members[slot];
        if (member.Kind == OrderKind::None)
            continue;
        Player const* bot = _scenario.SeatBot(env, mine[slot]);
        bool keep = bot && bot->IsAlive();
        if (keep && member.Kind == OrderKind::Heal)
        {
            Unit const* friendUnit = member.Target ? ObjectAccessor::GetUnit(*bot, member.Target) : nullptr;
            keep = friendUnit && friendUnit->IsAlive();
        }
        else if (keep && member.Kind != OrderKind::GoTo && member.Kind != OrderKind::Objective)
            keep = worth(member.Target);
        if (!keep)
            member = DirectorOrders::MemberOrder();
    }
}

void Animus::Curriculum::DirectorEncounter::Measure(Env& env, uint32 side)
{
    SideOrder& order = _envs[env.Index].Sides[side];
    SideKnowledge const& known = _envs[env.Index].Knowledge[side];

    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const count = Enemies(env, side, theirs);

    Unit const* lowest = nullptr;
    Unit const* focus = nullptr;
    float least = 2.0f;
    uint32 living = 0;
    for (uint32 slot = 0; slot < count && slot < PACK_SLOTS; ++slot)
    {
        Unit const* bot = theirs[slot];
        if (!bot || !bot->IsAlive())
            continue;

        ++living;
        if (float const left = CombatReward::HealthLeft(bot); left < least)
        {
            least = left;
            lowest = bot;
        }
        if (bot->GetGUID() == order.Focus)
            focus = bot;
    }

    // Nothing to call: not a decision the call can be judged on either way.
    if (!living)
        return;

    ++order.Decisions;
    // What naming one of the living at random would have scored, so a side of two and a side of ten are read on
    // the same scale and a director that calls well is told apart from one the arena makes look good.
    order.ChanceSum += 1.0f / float(living);

    // Measured against ground truth on purpose -- this is instrumentation, not an observation, and a yardstick
    // that could only see what the director sees would measure nothing. Alongside it, how much the side could
    // actually see, so a call that was hopeless for want of information is distinguishable from a poor one.
    uint32 seen = 0;
    for (uint32 slot = 0; slot < count && slot < PACK_SLOTS; ++slot)
        seen += known.Seen[slot] ? 1 : 0;
    order.SeenSum += float(seen);

    // Whether the place channel is being used at all, and whether anyone goes. It has never carried a value
    // in its life, so its first non-zero reading is the test that it works.
    if (order.HasPlace)
    {
        ++order.PlaceCalled;
        std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
        uint32 const own = Members(env, side, mine);
        float const radius = _scenario.Tuning().Order.PlaceRadius;
        for (uint32 slot = 0; slot < own; ++slot)
            if (Player const* bot = _scenario.SeatBot(env, mine[slot]); bot && bot->IsAlive())
            {
                float const away = bot->GetExactDist2d(&order.Place);
                order.PlaceDistanceSum += away;
                if (away <= radius)
                    ++order.PlaceReached;
            }
    }

    if (!focus)
        return;

    ++order.FocusAlive;
    if (!_scenario.SideCanSee(env, side, focus))
        ++order.FocusUnseen;
    if (focus == lowest)
        ++order.FocusLowest;
}

void Animus::Curriculum::DirectorEncounter::Update(Env& env)
{
    EnvDirector& state = _envs[env.Index];
    uint32 const steps = state.Steps++;

    // What the side can see, before anything reads it: Forget below asks whether a called target is still
    // worth calling, and for a learned director that question has to be answered from what its side knows.
    for (uint32 side = 0; side < TEAM_COUNT; ++side)
        Observe(env, side);

    // After the sighting and before anything reads the order: the place follows whatever it is anchored to.
    for (uint32 side = 0; side < TEAM_COUNT; ++side)
        ResolvePlace(env, side);

    // Before anything reads the order: a call at a corpse is not a call. The order stands between the
    // director's decisions -- ten of them, and longer still if it never spends another action on the focus --
    // so a target that dies mid-span would otherwise go on being asked for. Measured before this was added,
    // 37% of all decisions carried an order aimed at someone dead or gone, which no seat could follow at any
    // price, and which dragged order_focus_kept below chance on its own: a seat fighting someone alive cannot
    // match a call pointing at someone dead.
    for (uint32 side = 0; side < TEAM_COUNT; ++side)
        Forget(env, side);

    // Measured every decision and for either kind of director, so the scripted one is the yardstick.
    for (uint32 side = 0; side < TEAM_COUNT; ++side)
        Measure(env, side);

    // A learned director speaks through its own agent's actions (Call), on whatever cadence the learner gives
    // it; there is nothing to script.
    if (Learned(env))
        return;

    if (steps % THINK_EVERY)
        return;

    for (uint32 side = 0; side < TEAM_COUNT; ++side)
        Command(env, side);
}

void Animus::Curriculum::DirectorEncounter::PrepareTurn(Env& env, uint32 side)
{
    if (side >= TEAM_COUNT || !Learned(env))
        return;

    EnvDirector& state = _envs[env.Index];
    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, side, mine);
    std::array<Unit*, DirectorLayout::DIRECTOR_SEATS> members{};
    for (uint32 slot = 0; slot < own; ++slot)
        members[slot] = _scenario.SeatBot(env, mine[slot]);
    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const count = Enemies(env, side, theirs);
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    DirectorRules::PrepareTurn(state.Sides[side], state.Steps, members.data(), own, theirs.data(), count,
        tuning.ClockDecisions, tuning.LowHealth);
}

void Animus::Curriculum::DirectorEncounter::Call(Env& env, uint32 side, int32 action)
{
    if (side >= TEAM_COUNT)
        return;

    EnvDirector& state = _envs[env.Index];
    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, side, mine);
    std::array<Unit*, DirectorLayout::DIRECTOR_SEATS> members{};
    for (uint32 slot = 0; slot < own; ++slot)
        members[slot] = _scenario.SeatBot(env, mine[slot]);

    // An enemy slot a call may name: believed alive, not known alive -- refusing a call on one that died out of
    // sight would tell a learned director so through the refusal itself.
    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const count = Enemies(env, side, theirs);
    std::array<ObjectGuid, PACK_SLOTS> callable{};
    for (uint32 slot = 0; slot < count; ++slot)
    {
        if (!theirs[slot])
            continue;
        if (Learned(env))
        {
            EnemyMemory const& memory = state.Knowledge[side].Enemies[slot];
            if (memory.Known && memory.Alive)
                callable[slot] = theirs[slot]->GetGUID();
        }
        else if (theirs[slot]->IsAlive())
            callable[slot] = theirs[slot]->GetGUID();
    }

    DirectorRules::Apply(state.Sides[side], state.Steps, action, members.data(), own, callable.data(), count,
        Learned(env));
}

uint32 Animus::Curriculum::DirectorEncounter::SlotOf(Env const& env, uint32 seat) const
{
    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, _scenario.SideOf(env, seat), mine);
    for (uint32 slot = 0; slot < own; ++slot)
        if (mine[slot] == seat)
            return slot;
    return DirectorLayout::DIRECTOR_SEATS;
}

void Animus::Curriculum::DirectorEncounter::Command(Env& env, uint32 side)
{
    EnvDirector& state = _envs[env.Index];
    SideOrder& order = state.Sides[side];

    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, side, mine);
    float health = 0.0f;
    uint32 alive = 0;
    for (uint32 slot = 0; slot < own; ++slot)
        if (Player const* bot = _scenario.SeatBot(env, mine[slot]); bot && bot->IsAlive())
        {
            health += CombatReward::HealthLeft(bot);
            ++alive;
        }
    if (!alive)
        return;

    // Focus: the enemy with the least left. The simplest call a director can make, and the one a seat cannot
    // make for the side -- ten seats each choosing their own target is how a team loses a fight it should win.
    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const count = Enemies(env, side, theirs);
    ObjectGuid focus;
    float lowest = 2.0f;
    for (uint32 slot = 0; slot < count; ++slot)
        if (Unit const* enemy = theirs[slot]; enemy && enemy->IsAlive())
            if (float const left = CombatReward::HealthLeft(enemy); left < lowest)
            {
                lowest = left;
                focus = enemy->GetGUID();
            }

    // Posture follows the state of the side: press while it is whole, recover when it is not.
    float const average = health / float(alive);
    TeamPosture const posture = average < HURT ? TeamPosture::Recover : TeamPosture::Attack;
    TeamRally const rally = posture == TeamPosture::Recover ? TeamRally::OwnBase
        : focus ? TeamRally::Focus : TeamRally::None;

    if (order.Focus != focus || order.Posture != posture || order.Rally != rally)
        order.Changed(state.Steps);

    order.Focus = focus;
    order.Posture = posture;
    order.Rally = rally;
    order.HasPlace = false;
}

void Animus::Curriculum::DirectorEncounter::ViewSide(Env const& env, uint32 side,
    DirectorLayout::DirectorView& view) const
{
    view = DirectorLayout::DirectorView();
    if (side >= TEAM_COUNT)
        return;

    EnvDirector const& state = _envs[env.Index];
    SideOrder const& order = state.Sides[side];

    std::array<uint32, DirectorLayout::DIRECTOR_SEATS> mine{};
    uint32 const own = Members(env, side, mine);
    if (!own)
        return;         // a side with nobody to command (the creatures' side of a PvE arena)

    view.Active = true;
    view.MayCall = order.CallsLeft > 0;
    view.CallsLeft = order.CallsLeft;
    view.ByEvent = order.ByEvent;
    view.Raid = own > GROUP_SEATS;
    view.Groups = (own + GROUP_SEATS - 1) / GROUP_SEATS;
    view.EpisodeTime = std::min(1.0f, float(env.EpisodeElapsedMs) / EPISODE_TIME_SCALE_MS);
    view.Posture = order.Posture;
    view.Rally = order.Rally;
    view.HasFocus = bool(order.Focus);
    view.Anchor = order.Anchor;
    view.Offset = order.Offset;
    view.Ring = order.Ring;
    view.PlaceValid = order.HasPlace;
    view.PlacesAllowed = _scenario.Arena(env).Places;
    view.Address = order.Address;
    view.AddressGroup = order.AddressGroup;
    view.SinceCall = std::min(1.0f,
        float(state.Steps - std::min(state.Steps, order.CalledStep)) / DirectorLayout::CALL_AGE_SCALE);

    std::array<Unit*, PACK_SLOTS> theirs{};
    uint32 const enemy = Enemies(env, side, theirs);
    Unit const* focus = nullptr;
    for (uint32 slot = 0; slot < enemy; ++slot)
        if (theirs[slot] && theirs[slot]->GetGUID() == order.Focus)
            focus = theirs[slot];

    // The side's centre, which is all the geometry a director gets: it asks for a shape, never for a place.
    float centreX = 0.0f, centreY = 0.0f;
    uint32 standing = 0;
    for (uint32 slot = 0; slot < own; ++slot)
        if (Player const* bot = _scenario.SeatBot(env, mine[slot]); bot && bot->IsAlive())
        {
            centreX += bot->GetPositionX();
            centreY += bot->GetPositionY();
            ++standing;
        }
    if (standing)
    {
        centreX /= float(standing);
        centreY /= float(standing);
    }

    auto const spread = [&](Position const& at)
    {
        if (!standing)
            return 0.0f;
        float const dx = at.GetPositionX() - centreX;
        float const dy = at.GetPositionY() - centreY;
        return std::min(1.0f, std::sqrt(dx * dx + dy * dy) / DirectorLayout::DISTANCE_SCALE);
    };

    // The side's own axis: its centre towards where it believes the enemy is.
    float axis = 0.0f;
    {
        float enemyX = 0.0f, enemyY = 0.0f;
        uint32 enemies = 0;
        for (uint32 slot = 0; slot < PACK_SLOTS; ++slot)
            if (EnemyMemory const& memory = state.Knowledge[side].Enemies[slot]; memory.Known)
            {
                enemyX += memory.LastSeen.GetPositionX();
                enemyY += memory.LastSeen.GetPositionY();
                ++enemies;
            }
        if (enemies)
            axis = std::atan2(enemyY / float(enemies) - centreY, enemyX / float(enemies) - centreX);
    }
    auto const bearing = [&](Position const& at, float& out_sin, float& out_cos)
    {
        float const angle = std::atan2(at.GetPositionY() - centreY, at.GetPositionX() - centreX) - axis;
        out_sin = std::sin(angle);
        out_cos = std::cos(angle);
    };

    view.SeatCount = own;
    float health = 0.0f;
    for (uint32 slot = 0; slot < own; ++slot)
    {
        DirectorLayout::DirectorView::SeatSlot& out = view.Seats[slot];
        uint32 const seatIndex = mine[slot];
        Player const* bot = _scenario.SeatBot(env, seatIndex);
        SeatState const& seat = _scenario.Data(env).Seats[seatIndex];
        out.Present = bot && seat.L;
        if (!out.Present)
            continue;

        out.Alive = bot->IsAlive();
        out.Health = CombatReward::HealthLeft(bot);
        out.Power = bot->GetMaxPower(bot->getPowerType())
            ? float(bot->GetPower(bot->getPowerType())) / float(bot->GetMaxPower(bot->getPowerType())) : 0.0f;
        out.Apt = seat.Apt;
        out.InCombat = bot->IsInCombat();
        out.Casting = bot->IsNonMeleeSpellCast(false, false, true);
        out.Spread = spread(*bot);
        bearing(*bot, out.BearingSin, out.BearingCos);
        out.AtPlace = order.HasPlace && bot->GetExactDist2d(&order.Place) <= _scenario.Tuning().Order.PlaceRadius;
        out.Group = slot / GROUP_SEATS;
        out.Addressed = order.Address != OrderSource::Side && DirectorRules::Addressed(order, slot);
        uint32 attackers = 0;
        for (uint32 e = 0; e < enemy; ++e)
            attackers += theirs[e] && theirs[e]->IsAlive() && theirs[e]->GetVictim() == bot ? 1 : 0;
        out.Attacked = float(attackers) / float(PACK_SLOTS);
        {
            DirectorOrders::MemberOrder const& member = order.Members[slot];
            out.Order = member.Kind;
            out.OrderAge = member.Kind == OrderKind::None ? 0.0f : std::min(1.0f,
                float(state.Steps - std::min(state.Steps, member.IssuedStep)) / DirectorLayout::CALL_AGE_SCALE);
        }
        if (focus)
        {
            out.ToFocus = std::min(1.0f, bot->GetExactDist2d(focus) / DirectorLayout::DISTANCE_SCALE);
            Unit const* target = _scenario.SeatTarget(env, seatIndex);
            out.OnFocus = target && target->GetGUID() == order.Focus;
        }
        if (out.Alive)
            health += out.Health;
    }

    view.OwnStanding = float(standing) / float(own);
    view.OwnHealth = standing ? health / float(standing) : 0.0f;

    // Only the enemies a seat of this side could select between, and only what the side knows of them.
    SideKnowledge const& known = state.Knowledge[side];
    view.EnemyCount = std::min(enemy, PACK_SLOTS);
    float enemyHealth = 0.0f;
    uint32 enemyStanding = 0;
    for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
    {
        DirectorLayout::DirectorView::EnemySlot& out = view.Enemies[slot];
        EnemyMemory const& memory = known.Enemies[slot];
        out.Present = memory.Known;
        if (!out.Present)
            continue;

        out.Seen = known.Seen[slot] != 0;
        out.Alive = memory.Alive;
        out.Health = memory.Health;
        out.Apt = memory.Apt;
        out.IsFocus = memory.Guid == order.Focus;
        out.Spread = spread(memory.LastSeen);
        bearing(memory.LastSeen, out.BearingSin, out.BearingCos);
        out.UnseenTime = out.Seen ? 0.0f
            : std::min(1.0f, float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, memory.LastSeenMs))
                / DirectorLayout::MAX_UNSEEN_TIME_MS);
        uint32 ordered = 0;
        for (uint32 m = 0; m < own; ++m)
            if (order.Members[m].Target == memory.Guid && order.Members[m].Kind != OrderKind::None)
                ++ordered;
        out.Ordered = std::min(1.0f, float(ordered) / float(GROUP_SEATS));

        // Only while it is in sight: whether it was mid-cast a minute ago is not worth remembering.
        if (out.Seen)
            if (Unit const* unit = theirs[slot])
            {
                out.InCombat = unit->IsInCombat();
                out.Casting = unit->IsNonMeleeSpellCast(false, false, true);
                if (Unit const* victim = unit->GetVictim())
                    for (uint32 m = 0; m < own && !out.OnSeat; ++m)
                        out.OnSeat = _scenario.SeatBot(env, mine[m]) == victim;
            }

        if (out.Alive)
        {
            enemyHealth += out.Health;
            ++enemyStanding;
        }
    }

    view.EnemyStanding = view.EnemyCount ? float(enemyStanding) / float(view.EnemyCount) : 0.0f;
    view.EnemyHealth = enemyStanding ? enemyHealth / float(enemyStanding) : 0.0f;

    if (order.HasPlace && standing)
    {
        float const dx = order.Place.GetPositionX() - centreX;
        float const dy = order.Place.GetPositionY() - centreY;
        view.PlaceDistance = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / DirectorLayout::DISTANCE_SCALE);
    }

    // The objective, and the journal objectives, from whichever encounter keeps them.
    for (Encounter* encounter : _scenario.ActiveEncounters(env))
        encounter->ViewDirector(env, side, view);
}

void Animus::Curriculum::DirectorEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvDirector const& state = _envs[env.Index];
    SideOrder const& order = state.Sides[_scenario.SideOf(env, seat)];

    view.Order.Active = true;
    view.Order.Posture = order.Posture;
    view.Order.Rally = order.Rally;
    view.Order.HasRallyPlace = order.HasPlace;
    view.Order.RallyPlace = order.Place;
    view.Order.Focus = order.Focus ? ObjectAccessor::GetUnit(*view.Bot, order.Focus) : nullptr;
    uint32 const slot = SlotOf(env, seat);
    if (slot >= order.Members.size())
        return;

    DirectorOrders::MemberOrder const& member = order.Members[slot];
    view.Order.Kind = member.Kind;
    view.Order.Target = member.Target ? ObjectAccessor::GetUnit(*view.Bot, member.Target) : nullptr;
    view.Order.Objective = member.Objective;
    view.Order.Source = member.Source;
    view.Order.Age = std::min(1.0f,
        float(state.Steps - std::min(state.Steps, member.IssuedStep)) / DirectorLayout::CALL_AGE_SCALE);
    view.Order.IsDuty = member.Kind == OrderKind::Interrupt || member.Kind == OrderKind::Control;
}
