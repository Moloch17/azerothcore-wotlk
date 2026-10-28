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
#include "Creature.h"
#include "EntityActions.h"
#include "Env.h"
#include "HintBlock.h"
#include "GameObject.h"
#include "Identity.h"
#include "Log.h"
#include "Map.h"
#include "MapVisionWorld.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "StageScenario.h"
#include "StringFormat.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    /// How near a seat has to be to the thing its order names to count as "within reach" (the order block and the
    /// director's SEAT_ORDER_NEAR): a unit's melee reach, an object's interaction distance.
    float ReachOf(WorldObject const* object)
    {
        if (GameObject const* go = object ? object->ToGameObject() : nullptr)
            return go->GetInteractionDistance();
        return 8.0f;
    }

    bool DeadUnit(Unit const* unit)
    {
        return !unit || !unit->IsAlive();
    }

    DirectorLayout::ObjectKind KindOf(GameObject const* object)
    {
        switch (object->GetGoType())
        {
            case GAMEOBJECT_TYPE_DOOR:      return DirectorLayout::OBJECT_DOOR;
            case GAMEOBJECT_TYPE_BUTTON:    return DirectorLayout::OBJECT_LEVER;
            case GAMEOBJECT_TYPE_CHEST:     return DirectorLayout::OBJECT_KEY_CHEST;
            default:                        return DirectorLayout::OBJECT_GOOBER;
        }
    }

    bool OpenObject(GameObject const* object)
    {
        if (object->GetGoType() == GAMEOBJECT_TYPE_DOOR)
            return object->GetGoState() != GO_STATE_READY;
        // Looted or used -- not one still waiting for its first tick (GO_NOT_READY).
        return object->getLootState() == GO_ACTIVATED || object->getLootState() == GO_JUST_DEACTIVATED
            || object->GetGoState() != GO_STATE_READY;
    }
}

Animus::Curriculum::DirectorEncounter::DirectorEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::DirectorEncounter::RewardTerms() const
{
    return { RewardTerm::OrderDone, RewardTerm::OrderFailed, RewardTerm::LootUnordered, RewardTerm::OrderChurn,
        RewardTerm::OrderImpossible, RewardTerm::OrderIssued };
}

void Animus::Curriculum::DirectorEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // What the director did, on every seat's row (the director's own row holds no character and is left zero).
    auto const orders = [this](Env const& env) -> DirectorOrders const& { return _envs[env.Index].Orders; };
    table.Add("director_turns", [orders](Env const& env, uint32) { return float(orders(env).Turns); });
    table.Add("director_calls", [orders](Env const& env, uint32) { return float(orders(env).Calls); });
    table.Add("director_orders", [orders](Env const& env, uint32) { return float(orders(env).Orders); });
    table.Add("director_orders_replaced", [orders](Env const& env, uint32) { return float(orders(env).Replaced); });
    table.Add("director_orders_churned", [orders](Env const& env, uint32) { return float(orders(env).Churned); });
    table.Add("director_orders_impossible", [orders](Env const& env, uint32)
    {
        return float(orders(env).Impossible);
    });
    table.Add("director_orders_done", [orders](Env const& env, uint32) { return float(orders(env).Done); });
    table.Add("director_orders_failed", [orders](Env const& env, uint32) { return float(orders(env).Failed); });
    table.Add("director_posture_changes", [orders](Env const& env, uint32)
    {
        return float(orders(env).PostureChanges);
    });
    table.Add("director_loot_unordered", [orders](Env const& env, uint32)
    {
        return float(orders(env).LootUnordered);
    });
    for (uint32 kind = 1; kind < ORDER_KIND_COUNT; ++kind)
    {
        table.Add(Acore::StringFormat("order_{}_given", OrderKindName(OrderKind(kind))),
            [orders, kind](Env const& env, uint32) { return float(orders(env).Given[kind]); });
        table.Add(Acore::StringFormat("order_{}_done", OrderKindName(OrderKind(kind))),
            [orders, kind](Env const& env, uint32) { return float(orders(env).DoneOf[kind]); });
    }
    // The share of the orders given that were carried out: the one number to read the director's use by.
    table.Add("order_done_share", [orders](Env const& env, uint32)
    {
        uint32 const given = orders(env).Done + orders(env).Failed;
        return given ? float(orders(env).Done) / float(given) : 0.0f;
    });
    // The director's own terms, by its own ledger (on every seat's row too): the reward_order_churn and
    // reward_order_impossible columns read the seats' ledgers, which never hold them.
    for (RewardTerm term : { RewardTerm::OrderChurn, RewardTerm::OrderImpossible, RewardTerm::OrderIssued })
        table.Add("director_reward_" + std::string(RewardTermName(term)), [this, term](Env const& env, uint32)
        {
            return _envs[env.Index].Rewards.Episode(term);
        });
    // The orders' pace (technique 2), and the orders carried out that the pay cap left unpaid (DonePerMinute).
    table.Add("director_orders_per_minute", [orders](Env const& env, uint32)
    {
        return float(orders(env).Orders) / std::max(1.0f, float(env.EpisodeElapsedMs) / 60000.0f);
    });
    table.Add("order_done_unpaid", [this](Env const& env, uint32) { return float(_envs[env.Index].DoneUnpaid); });
    // The director's teacher (Teach): the share of the director's decisions it played.
    table.Add("director_teacher_played_share", [this](Env const& env, uint32)
    {
        EnvDirector const& director = _envs[env.Index];
        return director.TeacherDecisions ? float(director.TeacherPlayed) / float(director.TeacherDecisions) : 0.0f;
    });
}

void Animus::Curriculum::DirectorEncounter::ResetEpisode(Env& env)
{
    EnvDirector& director = _envs[env.Index];
    director = EnvDirector();
    director.Rewards.ResetEpisode();
    // A full hand of OrderDone tokens to begin with (Director.DonePerMinute).
    director.DoneTokens.fill(_scenario.Tuning().Director.DonePerMinute);
}

bool Animus::Curriculum::DirectorEncounter::Active(Env const& env) const
{
    if (!_scenario.Stage().Directed || !_scenario.Arena(env).PartyGroup)
        return false;
    EnvState const& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < MAX_SEATS; ++seat)
        if (data.Seats[seat].L)
            return true;
    return false;
}

uint32 Animus::Curriculum::DirectorEncounter::ObjectState(GameObject const* object)
{
    if (!object)
        return 0xFFFF;
    return uint32(object->GetGoState()) * 8 + uint32(object->getLootState()) + 1;
}

bool Animus::Curriculum::DirectorEncounter::Ready(Player const* bot) const
{
    if (!bot || !bot->IsAlive())
        return false;
    float const share = _scenario.Tuning().Instance.WingReadyShare;
    if (bot->GetHealthPct() / 100.0f < share)
        return false;
    if (bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) / 100.0f < share)
        return false;
    return true;
}

void Animus::Curriculum::DirectorEncounter::PrepareTurn(Env& env)
{
    EnvDirector& director = _envs[env.Index];
    if (!Active(env))
        return;
    EnvState const& data = _scenario.Data(env);
    bool alive[DirectorLayout::DIRECTOR_SEATS]{};
    bool low[DirectorLayout::DIRECTOR_SEATS]{};
    uint32 const count = std::min<uint32>(data.ActiveSeats, DirectorLayout::DIRECTOR_SEATS);
    for (uint32 seat = 0; seat < count; ++seat)
    {
        Player const* bot = _scenario.SeatBot(env, seat);
        alive[seat] = bot && bot->IsAlive();
        low[seat] = alive[seat] && bot->GetHealthPct() / 100.0f < _scenario.Tuning().Director.LowHealth;
    }
    uint32 onParty = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit const* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->GetVictim()
            && enemy->GetVictim()->IsPlayer())
            ++onParty;
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    DirectorRules::PrepareTurn(director.Orders, director.Step, alive, low, count, onParty, tuning.ClockDecisions,
        tuning.Calls);
}

void Animus::Curriculum::DirectorEncounter::ViewDirector(Env& env, DirectorLayout::DirectorView& view)
{
    using namespace DirectorLayout;
    EnvDirector& director = _envs[env.Index];
    view = DirectorView();
    director.EnemySlots.fill(ObjectGuid::Empty);
    director.ObjectSlots.fill(ObjectGuid::Empty);
    if (!Active(env))
        return;
    EnvState const& data = _scenario.Data(env);
    DirectorOrders const& orders = director.Orders;
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    Map* map = env.FindMap();
    uint32 const mapId = map ? map->GetId() : 0;

    view.Active = true;
    view.MayCall = orders.CallsLeft > 0;
    view.CallsLeft = orders.CallsLeft;
    view.CallsBudget = std::max<uint32>(1, tuning.Calls);
    view.ByEvent = orders.ByEvent;
    view.EpisodeTime = std::min(1.0f, float(env.EpisodeElapsedMs) / EPISODE_TIME_SCALE_MS);
    view.SinceCall = float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, orders.LastCallMs)) / 1000.0f;
    view.Posture = orders.Posture;
    view.Address = orders.Address;

    // The party's centre and axis: the living seats' mean, facing as the leader faces (else the first living seat).
    uint32 const count = std::min<uint32>(data.ActiveSeats, DIRECTOR_SEATS);
    uint32 const leader = _scenario.Party() ? _scenario.Party()->LeaderSeat(env) : 0;
    float centreX = 0.0f;
    float centreY = 0.0f;
    float axis = 0.0f;
    bool axisSet = false;
    uint32 living = 0;
    uint32 present = 0;
    float health = 0.0f;
    float power = 0.0f;
    uint32 manaUsers = 0;
    for (uint32 seat = 0; seat < count; ++seat)
    {
        if (!data.Seats[seat].L)
            continue;
        ++present;
        Player const* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsAlive() || !bot->IsInWorld())
            continue;
        ++living;
        centreX += bot->GetPositionX();
        centreY += bot->GetPositionY();
        health += bot->GetHealthPct() / 100.0f;
        if (bot->GetMaxPower(POWER_MANA))
        {
            power += bot->GetPowerPct(POWER_MANA) / 100.0f;
            ++manaUsers;
        }
        if (seat == leader || !axisSet)
        {
            axis = bot->GetOrientation();
            axisSet = true;
        }
        view.InCombat = view.InCombat || bot->IsInCombat();
    }
    if (living)
    {
        centreX /= float(living);
        centreY /= float(living);
    }
    view.PartyAlive = present ? float(living) / float(present) : 0.0f;
    view.PartyHealth = living ? health / float(living) : 0.0f;
    view.PartyPower = manaUsers ? power / float(manaUsers) : 1.0f;
    auto const bearing = [&](float x, float y, float& sin, float& cos, float& distance)
    {
        float const dx = x - centreX;
        float const dy = y - centreY;
        distance = std::sqrt(dx * dx + dy * dy);
        float const angle = std::atan2(dy, dx) - axis;
        sin = std::sin(angle);
        cos = std::cos(angle);
    };

    // The enemies: the env's target slots, the party's pack block's order; what each is attacking.
    uint32 const enemies = std::min<uint32>(uint32(env.Targets.size()), DIRECTOR_ENEMIES);
    std::array<uint32, DIRECTOR_SEATS> attackedBy{};
    for (uint32 slot = 0; slot < enemies; ++slot)
    {
        Unit* unit = env.FindTargetUnit(slot);
        if (!unit || !unit->IsInWorld())
            continue;
        DirectorView::EnemySlot& out = view.Enemies[slot];
        director.EnemySlots[slot] = unit->GetGUID();
        out.Present = true;
        out.Alive = unit->IsAlive();
        out.Health = out.Alive ? unit->GetHealthPct() / 100.0f : 0.0f;
        out.InCombat = out.Alive && unit->IsInCombat();
        out.Casting = out.Alive && unit->HasUnitState(UNIT_STATE_CASTING);
        if (Creature const* creature = unit->ToCreature())
        {
            out.Elite = creature->isElite();
            out.Boss = creature->IsDungeonBoss();
        }
        Player const* any = living ? _scenario.SeatBot(env, 0) : nullptr;
        out.Level = any ? float(int32(unit->GetLevel()) - int32(any->GetLevel())) / 10.0f : 0.0f;
        bearing(unit->GetPositionX(), unit->GetPositionY(), out.BearingSin, out.BearingCos, out.Distance);
        if (Unit const* victim = out.Alive ? unit->GetVictim() : nullptr; victim && victim->IsPlayer())
        {
            out.OnParty = true;
            ++view.EnemiesOnParty;
            for (uint32 seat = 0; seat < count; ++seat)
                if (Player const* bot = _scenario.SeatBot(env, seat); bot && bot == victim)
                    ++attackedBy[seat];
        }
        if (out.Alive)
            ++view.EnemiesAlive;
        uint32 ordered = 0;
        for (uint32 seat = 0; seat < count; ++seat)
            if (orders.Members[seat].Kind != OrderKind::None && orders.Members[seat].Target == unit->GetGUID())
                ++ordered;
        out.Ordered = float(ordered) / float(DIRECTOR_SEATS);
    }

    // The objects: what the party can use near it (InstanceEncounter), nearest the centre first, the way's next
    // gate always among them.
    InstanceEncounter const* instance = _scenario.Instance();
    ObjectGuid const gate = instance ? instance->NextGate(env) : ObjectGuid::Empty;
    std::vector<std::pair<float, GameObject*>> near;
    if (instance && map)
    {
        for (ObjectGuid const& guid : instance->PartyObjects(env))
            if (GameObject* object = map->GetGameObject(guid); object && object->IsInWorld())
                near.emplace_back(object->GetExactDist2d(centreX, centreY), object);
        if (!gate.IsEmpty() && std::none_of(near.begin(), near.end(),
            [&gate](auto const& entry) { return entry.second->GetGUID() == gate; }))
            if (GameObject* object = map->GetGameObject(gate); object && object->IsInWorld())
                near.emplace_back(-1.0f, object);
        std::sort(near.begin(), near.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    }
    bool keyHeld = false;
    for (uint32 seat = 0; seat < count; ++seat)
        if (Player const* bot = _scenario.SeatBot(env, seat); bot && InstanceEncounter::CarriesKey(bot, mapId))
            keyHeld = true;
    view.KeyHeld = keyHeld;
    for (uint32 slot = 0; slot < DIRECTOR_OBJECTS && slot < near.size(); ++slot)
    {
        GameObject* object = near[slot].second;
        DirectorView::ObjectSlot& out = view.Objects[slot];
        director.ObjectSlots[slot] = object->GetGUID();
        out.Present = true;
        out.Kind = KindOf(object);
        out.Open = OpenObject(object);
        uint32 const key = InstanceEncounter::KeyOfLock(object);
        out.NeedsKey = key != 0;
        out.KeyHeld = false;
        for (uint32 seat = 0; key && seat < count; ++seat)
            if (Player const* bot = _scenario.SeatBot(env, seat); bot && bot->HasItemCount(key, 1))
                out.KeyHeld = true;
        bearing(object->GetPositionX(), object->GetPositionY(), out.BearingSin, out.BearingCos, out.Distance);
        out.IsGate = object->GetGUID() == gate;
        view.GateAhead = view.GateAhead || out.IsGate;
        uint32 ordered = 0;
        for (uint32 seat = 0; seat < count; ++seat)
            if (orders.Members[seat].Kind != OrderKind::None && orders.Members[seat].Target == object->GetGUID())
                ++ordered;
        out.Ordered = float(ordered) / float(DIRECTOR_SEATS);
    }

    // The seats.
    for (uint32 seat = 0; seat < count; ++seat)
    {
        SeatState const& state = data.Seats[seat];
        if (!state.L)
            continue;
        DirectorView::SeatSlot& out = view.Seats[seat];
        out.Present = true;
        out.Apt = state.Apt;
        out.Leader = seat == leader;
        out.Role = state.DungeonRole;
        out.Addressed = seat == orders.Address;
        out.Goal = state.Holds[0].Goal;
        DirectorOrders::MemberOrder const& order = orders.Members[seat];
        out.Order = order.Kind;
        out.OrderAge = order.Kind != OrderKind::None
            ? float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, order.IssuedMs)) / 1000.0f : 0.0f;
        out.OrderDone = director.DoneFlash[seat] != 0;
        if (order.Kind != OrderKind::None)
            ++view.OrdersStanding;
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsInWorld())
            continue;
        out.Alive = bot->IsAlive();
        out.Health = out.Alive ? bot->GetHealthPct() / 100.0f : 0.0f;
        out.Power = bot->GetMaxPower(POWER_MANA) ? bot->GetPowerPct(POWER_MANA) / 100.0f : 1.0f;
        out.InCombat = out.Alive && bot->IsInCombat();
        out.Casting = out.Alive && bot->HasUnitState(UNIT_STATE_CASTING);
        out.Moving = out.Alive && bot->isMoving();
        bearing(bot->GetPositionX(), bot->GetPositionY(), out.BearingSin, out.BearingCos, out.Spread);
        out.CarriesKey = InstanceEncounter::CarriesKey(bot, mapId);
        out.Attacked = float(attackedBy[seat]) / ENEMY_COUNT_SCALE;
        if (order.Kind != OrderKind::None && !order.Target.IsEmpty())
            if (WorldObject const* target = ObjectAccessor::GetWorldObject(*bot, order.Target))
                out.OrderNear = bot->GetExactDist(target) <= ReachOf(target);
    }
}

void Animus::Curriculum::DirectorEncounter::Call(Env& env, int32 action)
{
    using namespace DirectorLayout;
    EnvDirector& director = _envs[env.Index];
    if (!Active(env))
        return;
    DirectorOrders& orders = director.Orders;
    DirectorRules::Call const call = DirectorRules::Decode(action);
    if (call.Kind == DirectorRules::Call::None)
        return;
    // Only on its turn; a hold ends it.
    if (!DirectorRules::Spend(orders, call.Kind == DirectorRules::Call::Hold))
        return;
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    EnvState const& data = _scenario.Data(env);
    uint32 const count = std::min<uint32>(data.ActiveSeats, DIRECTOR_SEATS);
    uint32 const ms = env.EpisodeElapsedMs;
    orders.LastCallMs = ms;
    switch (call.Kind)
    {
        case DirectorRules::Call::Posture:
            if (orders.Posture != call.Stance)
                ++orders.PostureChanges;
            orders.Posture = call.Stance;
            return;
        case DirectorRules::Call::Address:
            if (call.Member < count && data.Seats[call.Member].L)
                orders.Address = call.Member;
            return;
        case DirectorRules::Call::Clear:
            // Dropped, not failed: the seat is not charged for the director's change of mind (churn is).
            if (orders.Address < count && orders.Members[orders.Address].Kind != OrderKind::None)
            {
                uint32 const churned = orders.Churned;
                DirectorRules::Order(orders, director.Step, ms, orders.Address, OrderKind::None, ObjectGuid::Empty,
                    tuning.HoldDecisions);
                if (orders.Churned > churned)
                    ++director.ChurnDue;
            }
            return;
        case DirectorRules::Call::Order:
            break;
        default:
            return;
    }

    // An order to the member addressed. Its target: an enemy, member or object slot of the view the call was made
    // on. Impossible as the director could see it (OrderImpossible, a Cost at issue; never masked): a dead enemy,
    // a dead member or the seat itself, a chest that is no key chest or is looted, an object open already or
    // whose key the seat does not carry, a rest when ready. The order still stands: the seat may yet be right.
    uint32 const member = orders.Address;
    if (member >= count || !data.Seats[member].L)
        return;
    Player* bot = _scenario.SeatBot(env, member);
    Map* map = env.FindMap();
    ObjectGuid target;
    bool impossible = !bot || !bot->IsAlive();
    switch (call.Verb)
    {
        case OrderKind::Focus:
        case OrderKind::Pull:
        {
            target = call.Slot < DIRECTOR_ENEMIES ? director.EnemySlots[call.Slot] : ObjectGuid::Empty;
            Unit const* unit = bot && !target.IsEmpty() ? ObjectAccessor::GetUnit(*bot, target) : nullptr;
            impossible = impossible || DeadUnit(unit) || (call.Verb == OrderKind::Pull && unit->IsInCombat());
            break;
        }
        case OrderKind::Heal:
        case OrderKind::Follow:
        {
            Player const* other = call.Slot < count ? _scenario.SeatBot(env, call.Slot) : nullptr;
            target = other ? other->GetGUID() : ObjectGuid::Empty;
            impossible = impossible || !other || !other->IsAlive() || call.Slot == member
                || (call.Verb == OrderKind::Heal && other->GetHealthPct() / 100.0f >= tuning.HealTo);
            break;
        }
        case OrderKind::Loot:
        case OrderKind::Use:
        {
            target = call.Slot < DIRECTOR_OBJECTS ? director.ObjectSlots[call.Slot] : ObjectGuid::Empty;
            GameObject const* object = map && !target.IsEmpty() ? map->GetGameObject(target) : nullptr;
            if (!object)
                impossible = true;
            else if (call.Verb == OrderKind::Loot)
                impossible = impossible || !EntityActions::KeyChest(object) || OpenObject(object);
            else
            {
                uint32 const key = InstanceEncounter::KeyOfLock(object);
                impossible = impossible || EntityActions::KeyChest(object) || OpenObject(object)
                    || (key && bot && !bot->HasItemCount(key, 1));
            }
            break;
        }
        case OrderKind::Rest:
            impossible = impossible || Ready(bot);
            break;
        default:
            return;
    }
    if (impossible)
    {
        ++orders.Impossible;
        ++director.ImpossibleDue;
    }
    // What was there is replaced, not failed (the seat is not charged for the director's change of mind); a
    // replacement within HoldDecisions is churn, the director's to pay.
    uint32 const churned = orders.Churned;
    DirectorRules::Order(orders, director.Step, ms, member, call.Verb, target, tuning.HoldDecisions);
    if (orders.Churned > churned)
        ++director.ChurnDue;
    DirectorOrders::MemberOrder& order = orders.Members[member];
    director.HadKey[member] = bot && InstanceEncounter::CarriesKey(bot, map ? map->GetId() : 0) ? 1 : 0;
    director.ObjectStateAtIssue[member] = map && !target.IsEmpty() ? ObjectState(map->GetGameObject(target)) : 0;
    director.Pressed[member] = 0;
    if (call.Verb == OrderKind::Heal)
        if (Player const* other = call.Slot < count ? _scenario.SeatBot(env, call.Slot) : nullptr)
            order.WasLow = other->IsAlive() && other->GetHealthPct() / 100.0f < tuning.HealFrom;
    if (_scenario.Tuning().Instance.WingTrace && env.Index == 0)
        LOG_DEBUG("module.animus", "{}: env {} director orders seat {}: {} slot {} ({}{})", _scenario.Name(),
            env.Index, member, OrderKindName(call.Verb), call.Slot, target.ToString(),
            impossible ? ", impossible" : "");
}

void Animus::Curriculum::DirectorEncounter::Finish(Env& env, uint32 seat, bool done)
{
    EnvDirector& director = _envs[env.Index];
    if (seat >= director.Orders.Members.size())
        return;
    DirectorOrders::MemberOrder& order = director.Orders.Members[seat];
    if (order.Kind == OrderKind::None)
        return;
    // Paid at this decision's reward (Reward reads DoneNow / FailedNow and clears the order); an event for the
    // director's next turn.
    if (done)
        order.DoneNow = true;
    else
        order.FailedNow = true;
    director.Orders.Event = true;
    (void)env;
}

void Animus::Curriculum::DirectorEncounter::Judge(Env& env, uint32 seat, Player* bot)
{
    EnvDirector& director = _envs[env.Index];
    DirectorOrders::MemberOrder& order = director.Orders.Members[seat];
    if (order.Kind == OrderKind::None || order.DoneNow || order.FailedNow)
        return;
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    uint32 const now = env.EpisodeElapsedMs;
    uint32 const age = now - std::min(now, order.IssuedMs);
    if (!bot || !bot->IsAlive() || !bot->IsInWorld())
    {
        // A dead seat carries nothing out; its order lapses when the clock does.
        if (age >= tuning.OrderTimeoutMs)
            Finish(env, seat, false);
        return;
    }
    Map* map = env.FindMap();
    uint32 const stepMs = _scenario.StepMs(env);
    switch (order.Kind)
    {
        case OrderKind::Focus:
        {
            // Carried out on the seat's first damage to the named enemy (its selection is bound to it, so damage
            // anywhere else is not it); one that dies untouched by the seat was not.
            Unit const* unit = ObjectAccessor::GetUnit(*bot, order.Target);
            if (unit && env.DamagedThisStep(seat, order.Target))
            {
                order.Damaged = true;
                Finish(env, seat, true);
                return;
            }
            if (DeadUnit(unit))
            {
                Finish(env, seat, false);
                return;
            }
            break;
        }
        case OrderKind::Pull:
        {
            Unit const* unit = ObjectAccessor::GetUnit(*bot, order.Target);
            if (DeadUnit(unit))
            {
                Finish(env, seat, order.Engaged);
                return;
            }
            if (Unit const* victim = unit->GetVictim(); unit->IsInCombat() && victim && victim->IsPlayer())
            {
                order.Engaged = true;
                Finish(env, seat, true);
                return;
            }
            if (age >= tuning.PullMs)
            {
                Finish(env, seat, false);
                return;
            }
            break;
        }
        case OrderKind::Heal:
        {
            Player const* other = ObjectAccessor::GetPlayer(*bot, order.Target);
            if (!other || !other->IsAlive())
            {
                Finish(env, seat, false);
                return;
            }
            float const health = other->GetHealthPct() / 100.0f;
            order.WasLow = order.WasLow || health < tuning.HealFrom;
            if (env.StepStats[seat].AllyHealing)
                order.Damaged = true;       // the seat healed somebody since the order
            if (order.WasLow && order.Damaged && health >= tuning.HealTo)
            {
                Finish(env, seat, true);
                return;
            }
            break;
        }
        case OrderKind::Follow:
        {
            Player const* other = ObjectAccessor::GetPlayer(*bot, order.Target);
            if (!other || !other->IsAlive())
            {
                Finish(env, seat, false);
                return;
            }
            if (bot->GetExactDist(other) <= tuning.FollowYards)
                order.NearMs += stepMs;
            if (order.NearMs >= tuning.FollowMs)
            {
                Finish(env, seat, true);
                return;
            }
            break;
        }
        case OrderKind::Loot:
        {
            GameObject const* object = map ? map->GetGameObject(order.Target) : nullptr;
            uint32 const item = object ? EntityActions::KeyChestItem(object) : 0;
            bool const carries = item ? bot->HasItemCount(item, 1)
                : (!director.HadKey[seat] && InstanceEncounter::CarriesKey(bot, map ? map->GetId() : 0));
            if (carries && (!director.HadKey[seat] || director.Pressed[seat]))
            {
                director.Orders.Event = true;
                Finish(env, seat, true);
                return;
            }
            // Looted by another, or gone: nothing left to carry out.
            if (!object || (OpenObject(object) && !director.Pressed[seat]))
            {
                Finish(env, seat, false);
                return;
            }
            break;
        }
        case OrderKind::Use:
        {
            GameObject const* object = map ? map->GetGameObject(order.Target) : nullptr;
            // The object gone after the seat's press (the Iron Clad Door blown away), or its state changed since
            // the order with the seat's press on it: used.
            if (director.Pressed[seat] && (!object || ObjectState(object) != director.ObjectStateAtIssue[seat]))
            {
                Finish(env, seat, true);
                return;
            }
            if (!object || (!director.Pressed[seat] && OpenObject(object)))
            {
                Finish(env, seat, false);
                return;
            }
            break;
        }
        case OrderKind::Rest:
            if (Ready(bot))
            {
                Finish(env, seat, true);
                return;
            }
            break;
        default:
            break;
    }
    if (age >= tuning.OrderTimeoutMs)
        Finish(env, seat, false);
}

void Animus::Curriculum::DirectorEncounter::Update(Env& env)
{
    EnvDirector& director = _envs[env.Index];
    ++director.Step;
    director.DoneFlash.fill(0);
    if (!Active(env))
        return;
    EnvState const& data = _scenario.Data(env);
    uint32 const count = std::min<uint32>(data.ActiveSeats, DirectorLayout::DIRECTOR_SEATS);
    for (uint32 seat = 0; seat < count; ++seat)
        if (data.Seats[seat].L)
        {
            Player* bot = _scenario.SeatBot(env, seat);
            Judge(env, seat, bot);
            Bind(env, seat, bot);
        }
}

void Animus::Curriculum::DirectorEncounter::Bind(Env& env, uint32 seat, Player* bot)
{
    DirectorOrders::MemberOrder const& order = _envs[env.Index].Orders.Members[seat];
    if (order.Kind == OrderKind::None || order.Target.IsEmpty() || !bot || !bot->IsAlive() || !bot->IsInWorld())
        return;
    switch (order.Kind)
    {
        case OrderKind::Focus:
        case OrderKind::Pull:
        {
            Unit* unit = ObjectAccessor::GetUnit(*bot, order.Target);
            if (!unit || !unit->IsAlive() || bot->GetTarget() == order.Target)
                return;
            WorldPacket packet = EntityActions::SetSelection(order.Target);
            EntityActions::SessionPort().Send(bot, packet);
            // Swinging already: the auto attack turns onto the named enemy, as a selection press does.
            if (bot->GetVictim() && bot->GetVictim() != unit && bot->IsValidAttackTarget(unit))
            {
                WorldPacket swing = EntityActions::AttackSwing(order.Target);
                EntityActions::SessionPort().Send(bot, swing);
            }
            return;
        }
        case OrderKind::Heal:
            _scenario.Data(env).Seats[seat].Focus = order.Target;
            return;
        default:
            return;
    }
}

bool Animus::Curriculum::DirectorEncounter::RouteGoal(Env const& env, uint32 seat, Position& goal) const
{
    if (!Active(env) || seat >= DirectorLayout::DIRECTOR_SEATS)
        return false;
    DirectorOrders::MemberOrder const& order = _envs[env.Index].Orders.Members[seat];
    if (order.Target.IsEmpty())
        return false;
    switch (order.Kind)
    {
        case OrderKind::Follow:
        case OrderKind::Pull:
        case OrderKind::Loot:
        case OrderKind::Use:
            break;
        default:
            return false;
    }
    Player* bot = _scenario.SeatBotInWorld(env, seat);
    if (!bot)
        return false;
    WorldObject const* target = ObjectAccessor::GetWorldObject(*bot, order.Target);
    if (!target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId())
        return false;
    if (Unit const* unit = target->ToUnit(); unit && !unit->IsAlive())
        return false;
    goal = target->GetPosition();
    return true;
}

void Animus::Curriculum::DirectorEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    EnvDirector& director = _envs[env.Index];
    if (!Active(env) || seat >= DirectorLayout::DIRECTOR_SEATS || result.ActedOn.IsEmpty() || result.ActRefused)
        return;
    if (result.ActPress != uint8(EntityActions::Press::Interact)
        && result.ActPress != uint8(EntityActions::Press::UseItem))
        return;
    DirectorOrders::MemberOrder const& order = director.Orders.Members[seat];
    if (order.Kind != OrderKind::None && order.Target == result.ActedOn)
        director.Pressed[seat] = 1;
    // A key chest opened by a seat not told to (LootUnordered): "the correct bot needs to loot it".
    if (result.ActPress == uint8(EntityActions::Press::Interact) && result.ActedOn.IsGameObject())
        if (Map* map = env.FindMap())
            if (GameObject const* object = map->GetGameObject(result.ActedOn);
                object && EntityActions::KeyChest(object) && !OpenObject(object)
                && !(order.Kind == OrderKind::Loot && order.Target == result.ActedOn))
            {
                director.LootUnorderedDue[seat] = 1;
                ++director.Orders.LootUnordered;
            }
}

void Animus::Curriculum::DirectorEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvDirector const& director = _envs[env.Index];
    if (!Active(env) || seat >= DirectorLayout::DIRECTOR_SEATS)
        return;
    DirectorOrders const& orders = director.Orders;
    DirectorOrders::MemberOrder const& order = orders.Members[seat];
    SeatView::TeamOrder& out = view.Order;
    out.Active = true;
    out.Posture = orders.Posture;
    out.Kind = order.Kind;
    out.DoneNow = director.DoneFlash[seat] != 0;
    if (order.Kind == OrderKind::None)
        return;
    out.AgeSeconds = float(env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, order.IssuedMs)) / 1000.0f;
    Player* bot = view.Bot;
    if (order.Target.IsEmpty() || !bot || !bot->IsInWorld())
        return;
    WorldObject const* target = ObjectAccessor::GetWorldObject(*bot, order.Target);
    if (!target)
        return;
    out.HasTarget = true;
    // Where it is, only when the seat sees or remembers it (its sight list, SeatState::SightGuids).
    SeatState const& state = _scenario.Data(env).Seats[seat];
    uint64 const raw = order.Target.GetRawValue();
    out.TargetKnown = std::find(state.SightGuids.begin(), state.SightGuids.end(), raw) != state.SightGuids.end()
        || target->IsPlayer();
    out.Where = target->GetPosition();
    out.TargetNear = bot->GetExactDist(target) <= ReachOf(target);
    out.TargetIsSelection = bot->GetTarget() == order.Target;
    if (order.Kind == OrderKind::Loot || order.Kind == OrderKind::Use)
        if (target->ToGameObject())
            out.BoundObject = order.Target;
    if (Unit const* unit = target->ToUnit())
        out.TargetHealth = unit->IsAlive() ? unit->GetHealthPct() / 100.0f : 0.0f;
    if (GameObject const* object = target->ToGameObject())
    {
        out.TargetOpen = OpenObject(object);
        uint32 const key = InstanceEncounter::KeyOfLock(object);
        out.NeedsKey = key != 0;
        out.CarriesKey = key && bot->HasItemCount(key, 1);
        // The sight block's named row: what the seat is to find and act on, when no goal names anything (M3's
        // task codes: 2 use it, 3 use the key item on it).
        if (!view.NamedTask && (order.Kind == OrderKind::Loot || order.Kind == OrderKind::Use))
        {
            view.NamedTask = order.Kind == OrderKind::Use && key ? 3 : 2;
            view.NamedEntry = object->GetEntry();
            view.NamedObject = true;
            view.NamedClass = uint8(Vision::Classify(Vision::FactsOf(bot, const_cast<GameObject*>(object))).What);
        }
    }
}

void Animus::Curriculum::DirectorEncounter::BeforeRewards(Env& env)
{
    EnvDirector& director = _envs[env.Index];
    director.CompliancePaid.fill(0.0f);
}

void Animus::Curriculum::DirectorEncounter::Reward(Env& env, uint32 seat, Player* /*bot*/, RewardLedger& ledger)
{
    EnvDirector& director = _envs[env.Index];
    if (!Active(env) || seat >= DirectorLayout::DIRECTOR_SEATS)
        return;
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    DirectorOrders& orders = director.Orders;
    DirectorOrders::MemberOrder& order = orders.Members[seat];
    float paid = 0.0f;
    // The seat's tokens for OrderDone pay (Director.DonePerMinute): one each 60 / DonePerMinute seconds, DonePerMinute
    // held.
    float const cap = tuning.DonePerMinute;
    if (cap > 0.0f)
        director.DoneTokens[seat] = std::min(cap, director.DoneTokens[seat]
            + cap * float(_scenario.StepMs(env)) / 60000.0f);
    if (order.Kind != OrderKind::None && (order.DoneNow || order.FailedNow))
    {
        if (order.DoneNow)
        {
            if (cap <= 0.0f || director.DoneTokens[seat] >= 1.0f)
            {
                paid += ledger.Add(RewardTerm::OrderDone, tuning.OrderDone);
                if (cap > 0.0f)
                    director.DoneTokens[seat] -= 1.0f;
            }
            else
                ++director.DoneUnpaid;
            ++orders.Done;
            ++orders.DoneOf[uint32(order.Kind)];
            director.DoneFlash[seat] = 1;
        }
        else
        {
            paid += ledger.Add(RewardTerm::OrderFailed, -tuning.OrderFailed);
            ++orders.Failed;
        }
        order = DirectorOrders::MemberOrder();
    }
    if (director.LootUnorderedDue[seat])
    {
        paid += ledger.Add(RewardTerm::LootUnordered, -tuning.LootUnordered);
        director.LootUnorderedDue[seat] = 0;
    }
    director.CompliancePaid[seat] = paid;
}

float Animus::Curriculum::DirectorEncounter::DirectorReward(Env& env, float const* seatRewards)
{
    EnvDirector& director = _envs[env.Index];
    if (!Active(env))
        return 0.0f;
    CurriculumTuning::DirectorTuning const& tuning = _scenario.Tuning().Director;
    EnvState const& data = _scenario.Data(env);
    float total = 0.0f;
    uint32 seats = 0;
    for (uint32 seat = 0; seat < data.ActiveSeats && seat < DirectorLayout::DIRECTOR_SEATS; ++seat)
        if (data.Seats[seat].L)
        {
            total += seatRewards[seat] - director.CompliancePaid[seat];
            ++seats;
        }
    // Its own prices, through its own ledger (the cost scale as the seats'), so they have columns.
    director.Rewards.SetCosts(1.0f);
    float own = 0.0f;
    if (director.ChurnDue)
    {
        own += director.Rewards.Add(RewardTerm::OrderChurn, -tuning.OrderChurn * float(director.ChurnDue));
        director.ChurnDue = 0;
    }
    if (director.ImpossibleDue)
    {
        own += director.Rewards.Add(RewardTerm::OrderImpossible,
            -tuning.OrderImpossible * float(director.ImpossibleDue));
        director.ImpossibleDue = 0;
    }
    // Each order issued since the last reward (Director.OrderIssued): the asking priced.
    if (uint32 const issued = director.Orders.Orders - std::min(director.Orders.Orders, director.OrdersCharged);
        issued && tuning.OrderIssued > 0.0f)
        own += director.Rewards.Add(RewardTerm::OrderIssued, -tuning.OrderIssued * float(issued));
    director.OrdersCharged = director.Orders.Orders;
    return (seats ? total / float(seats) : 0.0f) + own;
}

void Animus::Curriculum::DirectorEncounter::Teach(Env& env, float* obs, uint8 const* mask, float tau)
{
    using namespace DirectorLayout;
    EnvDirector& director = _envs[env.Index];
    director.TeacherAction = -1;
    director.TeacherPlays = false;
    CurriculumTuning const& tuning = _scenario.Tuning();
    if (!Active(env) || !mask || tau <= 0.0f || !tuning.Teacher.Director || !_scenario.Instance())
    {
        director.TeacherSegmentLeft = 0;
        return;
    }
    Map* map = env.FindMap();
    InstanceEncounter::TeacherWay const& way = _scenario.Instance()->Teacher(env, map);
    EnvState const& data = _scenario.Data(env);
    DirectorOrders const& orders = director.Orders;
    uint32 const count = std::min<uint32>(data.ActiveSeats, DIRECTOR_SEATS);
    std::array<Player*, DIRECTOR_SEATS> seats{};
    int32 tank = -1;
    int32 healer = -1;
    for (uint32 seat = 0; seat < count; ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && bot->IsInWorld() && bot->IsAlive())
        {
            seats[seat] = bot;
            if (tank < 0 && data.Seats[seat].DungeonRole == DUNGEON_TANK)
                tank = int32(seat);
            if (healer < 0 && data.Seats[seat].DungeonRole == DUNGEON_HEALER)
                healer = int32(seat);
        }
    auto const enemySlot = [&director](ObjectGuid guid) -> int32
    {
        for (uint32 slot = 0; slot < DIRECTOR_ENEMIES; ++slot)
            if (!guid.IsEmpty() && director.EnemySlots[slot] == guid)
                return int32(slot);
        return -1;
    };
    auto const objectSlot = [&director](ObjectGuid guid) -> int32
    {
        for (uint32 slot = 0; slot < DIRECTOR_OBJECTS; ++slot)
            if (!guid.IsEmpty() && director.ObjectSlots[slot] == guid)
                return int32(slot);
        return -1;
    };
    auto const seatOf = [&seats, count](ObjectGuid guid) -> int32
    {
        for (uint32 seat = 0; seat < count; ++seat)
            if (seats[seat] && seats[seat]->GetGUID() == guid)
                return int32(seat);
        return -1;
    };
    // The orders the party should be under, most urgent first: (member, kind, its action, its target).
    struct Want
    {
        uint32 Member;
        OrderKind Kind;
        uint32 Action;
        ObjectGuid Target;
    };
    std::vector<Want> wants;
    // In a fight: an enemy of the director's slots on a player.
    Player const* anyone = nullptr;
    for (uint32 seat = 0; seat < count && !anyone; ++seat)
        anyone = seats[seat];
    bool fight = false;
    for (uint32 slot = 0; slot < DIRECTOR_ENEMIES && anyone && !fight; ++slot)
        if (Unit const* enemy = director.EnemySlots[slot].IsEmpty() ? nullptr
            : ObjectAccessor::GetUnit(*anyone, director.EnemySlots[slot]))
            fight = enemy->IsAlive() && enemy->GetVictim() && enemy->GetVictim()->IsPlayer();
    // A heal for the most hurt member below HealFrom.
    if (healer >= 0)
    {
        int32 lowest = -1;
        for (uint32 seat = 0; seat < count; ++seat)
            if (seats[seat] && seats[seat]->GetHealthPct() / 100.0f < tuning.Director.HealFrom
                && (lowest < 0 || seats[seat]->GetHealthPct() < seats[uint32(lowest)]->GetHealthPct()))
                lowest = int32(seat);
        if (lowest >= 0)
            wants.push_back({ uint32(healer), OrderKind::Heal, ACTION_HEAL_FIRST + uint32(lowest),
                seats[uint32(lowest)]->GetGUID() });
    }
    // The way's object: the gunpowder used, the barrel looted, by its opener.
    if (!way.Object.IsEmpty() && way.ObjectOpens != WingTeacher::Opens::Click)
        if (int32 const opener = seatOf(way.Opener), slot = objectSlot(way.Object); opener >= 0 && slot >= 0)
        {
            bool const use = way.ObjectOpens == WingTeacher::Opens::UseKey;
            wants.push_back({ uint32(opener), use ? OrderKind::Use : OrderKind::Loot,
                (use ? ACTION_USE_FIRST : ACTION_LOOT_FIRST) + uint32(slot), way.Object });
        }
    if (fight && tank >= 0)
    {
        // The tank's target, for every damage dealer.
        ObjectGuid const target = seats[uint32(tank)]->GetTarget();
        if (int32 const slot = enemySlot(target); slot >= 0)
            for (uint32 seat = 0; seat < count; ++seat)
                if (seats[seat] && int32(seat) != tank && int32(seat) != healer)
                    wants.push_back({ seat, OrderKind::Focus, ACTION_FOCUS_FIRST + uint32(slot), target });
    }
    else if (tank >= 0)
    {
        // The next pack, the tank's to pull; then the stragglers back to the tank.
        if (int32 const slot = enemySlot(way.Pull); slot >= 0)
            wants.push_back({ uint32(tank), OrderKind::Pull, ACTION_PULL_FIRST + uint32(slot), way.Pull });
        constexpr float STRAY_YARDS = 25.0f;
        for (uint32 seat = 0; seat < count; ++seat)
            if (seats[seat] && int32(seat) != tank && seats[seat]->GetExactDist(seats[uint32(tank)]) > STRAY_YARDS)
                wants.push_back({ seat, OrderKind::Follow, ACTION_FOLLOW_FIRST + uint32(tank),
                    seats[uint32(tank)]->GetGUID() });
    }
    // The first the party lacks: its member addressed, then the order; nothing lacking ends the turn.
    int32 call = int32(ACTION_HOLD);
    for (Want const& want : wants)
    {
        DirectorOrders::MemberOrder const& held = orders.Members[want.Member];
        if (held.Kind == want.Kind && held.Target == want.Target)
            continue;
        call = int32(orders.Address == want.Member ? want.Action : ACTION_ADDRESS_FIRST + want.Member);
        break;
    }
    if (call < 0 || call >= int32(ACTION_COUNT) || !mask[call])
        call = -1;
    director.TeacherAction = call;
    if (!director.TeacherSegmentLeft)
    {
        director.TeacherSegmentLeft = std::max<uint32>(1, tuning.Teacher.SegmentDecisions);
        director.TeacherSegmentPlays = frand(0.0f, 1.0f) < tau;
    }
    --director.TeacherSegmentLeft;
    director.TeacherPlays = director.TeacherSegmentPlays;
    ++director.TeacherDecisions;
    director.TeacherPlayed += director.TeacherPlays ? 1 : 0;
    // The hint columns after the director's row: a call (a hold is not one), its weight, and whether it played.
    float* hint = obs + OBS_COUNT;
    bool const hinted = call > int32(ACTION_HOLD);
    hint[HintBlock::OBS_ACTION] = hinted ? float(call + 1) : 0.0f;
    hint[HintBlock::OBS_WEIGHT] = hinted ? tau : 0.0f;
    hint[HintBlock::OBS_SCRIPTED] = director.TeacherPlays ? 1.0f : 0.0f;
}
