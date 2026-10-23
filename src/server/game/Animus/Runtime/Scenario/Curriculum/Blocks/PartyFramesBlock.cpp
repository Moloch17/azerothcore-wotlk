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

#include "PartyFramesBlock.h"
#include "CombatBlock.h"
#include "EncoderSupport.h"
#include "EntityActions.h"
#include "Group.h"
#include "Player.h"
#include "PlayerController.h"
#include "StringFormat.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <algorithm>
#include <cmath>

namespace
{
    using Frames = Animus::Curriculum::PartyFramesBlock;

    /// A frame's unit wherever it is on the seat's map (a frame is not a nameplate: it needs no sight of it).
    WorldObject* FrameResolve(Player* bot, ObjectGuid guid)
    {
        Unit* unit = Animus::Curriculum::Encoding::UnitThrough(*bot, guid);
        return unit && unit->IsInWorld() && unit->GetMap() == bot->GetMap() ? unit : nullptr;
    }

    char const* PressName(uint32 local)
    {
        return local < Frames::ACTION_FOCUS_FIRST ? "select" : local < Frames::ACTION_ASSIST_FIRST ? "focus"
            : "assist";
    }
}

Animus::Curriculum::PartyRecall::Entry* Animus::Curriculum::PartyRecall::Find(uint64 guid, bool make)
{
    Entry* free = nullptr;
    Entry* oldest = nullptr;
    for (Entry& entry : Entries)
    {
        if (entry.Known && entry.Guid == guid)
            return &entry;
        if (!entry.Known)
            free = free ? free : &entry;
        else if (!oldest || entry.SeenMs < oldest->SeenMs)
            oldest = &entry;
    }
    if (!make)
        return nullptr;
    Entry* slot = free ? free : oldest;
    *slot = Entry();
    slot->Guid = guid;
    return slot;
}

void Animus::Curriculum::PartyRecall::See(uint64 guid, float x, float y, uint64 nowMs)
{
    // A clock that ran backwards is a new episode: nothing seen before it counts.
    if (nowMs < LastMs)
        Clear();
    LastMs = nowMs;
    Entry* entry = Find(guid, true);
    if (entry->Known && nowMs > entry->SeenMs)
    {
        // The pace between two sightings, smoothed; one that implies more than a run is a jump (a rise at the door,
        // a teleport), not a pace, and forgets the old one.
        float const seconds = float(nowMs - entry->SeenMs) / 1000.0f;
        float const vx = (x - entry->X) / seconds;
        float const vy = (y - entry->Y) / seconds;
        if (vx * vx + vy * vy > MAX_SPEED * MAX_SPEED)
        {
            entry->Vx = 0.0f;
            entry->Vy = 0.0f;
        }
        else
        {
            entry->Vx = 0.5f * entry->Vx + 0.5f * vx;
            entry->Vy = 0.5f * entry->Vy + 0.5f * vy;
        }
    }
    else if (!entry->Known)
    {
        entry->Vx = 0.0f;
        entry->Vy = 0.0f;
    }
    if (nowMs >= entry->SeenMs || !entry->Known)
    {
        entry->X = x;
        entry->Y = y;
        entry->SeenMs = nowMs;
    }
    entry->Known = true;
}

bool Animus::Curriculum::PartyRecall::Believe(uint64 guid, uint64 nowMs, float& x, float& y, float& ageSeconds,
    float& speed)
{
    if (nowMs < LastMs)
        Clear();
    Entry const* entry = Find(guid, false);
    if (!entry)
        return false;
    ageSeconds = float(nowMs - std::min(nowMs, entry->SeenMs)) / 1000.0f;
    float const carried = std::min(ageSeconds, PREDICT_SECONDS);
    x = entry->X + entry->Vx * carried;
    y = entry->Y + entry->Vy * carried;
    speed = std::sqrt(entry->Vx * entry->Vx + entry->Vy * entry->Vy);
    return true;
}

Animus::Curriculum::PartyFramesBlock::Dot Animus::Curriculum::PartyFramesBlock::DotOf(float selfX, float selfY,
    float facing, float otherX, float otherY, float radius)
{
    Dot dot;
    float const dx = otherX - selfX;
    float const dy = otherY - selfY;
    dot.Distance = std::sqrt(dx * dx + dy * dy);
    if (radius <= 0.0f || dot.Distance > radius)
        return dot;

    // Forward along the facing; right is clockwise of it (the core's yaw turns counter-clockwise, to the left).
    float const c = std::cos(facing);
    float const s = std::sin(facing);
    dot.Shown = true;
    dot.Forward = dx * c + dy * s;
    dot.Right = dx * s - dy * c;
    return dot;
}

void Animus::Curriculum::PartyFramesBlock::FillFrame(SeatView::PartyFrame& out, Unit const* member, bool leads,
    Player const* bot, float selfX, float selfY, float facing, float radius, ObjectGuid focus,
    Vision::SeenList const* seen, PartyRecall* recall, uint64 nowMs)
{
    out = SeatView::PartyFrame();
    if (!member || !bot)
        return;
    out.Present = true;
    out.Guid = member->GetGUID();
    out.Leader = leads;
    out.Alive = member->IsAlive();
    out.InCombat = member->IsInCombat();
    out.Health = member->GetMaxHealth() ? float(member->GetHealth()) / float(member->GetMaxHealth()) : 0.0f;
    Powers const power = member->getPowerType();
    out.Power = member->GetMaxPower(power) ? float(member->GetPower(power)) / float(member->GetMaxPower(power))
        : 0.0f;
    out.ManaUser = power == POWER_MANA;
    Encoding::Debuffs const debuffs = Encoding::IncomingDebuffs(member);
    out.Debuffs = debuffs.Count;
    out.Dispellable = debuffs.Dispellable;
    out.Aggro = member->IsAlive() && !member->getAttackers().empty();
    out.Selected = bot->GetTarget() == member->GetGUID();
    out.Focused = !focus.IsEmpty() && focus == member->GetGUID();

    // Where it is, only within reach of the client's views of it: the frame's range, and the minimap's dot -- both
    // on the seat's own map and instance alone.
    bool const sameMap = member->GetMapId() == bot->GetMapId() && member->GetInstanceId() == bot->GetInstanceId();
    if (sameMap)
    {
        out.InRange = bot->GetExactDist(member) <= FRAME_RANGE;
        Dot const dot = DotOf(selfX, selfY, facing, member->GetPositionX(), member->GetPositionY(), radius);
        out.DotShown = dot.Shown;
        out.DotRight = dot.Right;
        out.DotForward = dot.Forward;
    }

    // Where the seat believes it is (revision 3): seen now -- its dot, or in the camera's frame -- is a sighting;
    // otherwise the memory's guess.
    if (recall)
    {
        uint64 const raw = member->GetGUID().GetRawValue();
        bool const live = sameMap && (out.DotShown || (seen && CombatBlock::InView(*seen, raw)));
        if (live)
            recall->See(raw, member->GetPositionX(), member->GetPositionY(), nowMs);
        float x = 0.0f;
        float y = 0.0f;
        float age = 0.0f;
        float speed = 0.0f;
        if (sameMap && recall->Believe(raw, nowMs, x, y, age, speed))
        {
            float const dx = x - selfX;
            float const dy = y - selfY;
            float const c = std::cos(facing);
            float const s = std::sin(facing);
            out.RecallKnown = true;
            out.RecallLive = live;
            out.RecallForward = dx * c + dy * s;
            out.RecallRight = dx * s - dy * c;
            out.RecallAgeSeconds = age;
            out.RecallSpeed = speed;
        }
    }

    // Its target, where the seat's client has it too (what /assist would select).
    ObjectGuid const its = member->GetTarget();
    if (its.IsEmpty() || !sameMap)
        return;
    Unit const* target = Encoding::UnitThrough(*bot, its);
    if (!target || !target->IsInWorld() || target->GetMap() != bot->GetMap()
        || !EntityActions::AtClient(const_cast<Player*>(bot), target))
        return;
    out.HasTarget = true;
    out.TargetHostile = bot->IsHostileTo(target);
    out.TargetMine = bot->GetTarget() == its;
    out.TargetInView = seen && CombatBlock::InView(*seen, its.GetRawValue());
}

void Animus::Curriculum::PartyFramesBlock::FillFromGroup(SeatView& view)
{
    view.Frames.fill(SeatView::PartyFrame());
    Player* bot = view.Bot;
    if (!bot || !bot->IsInWorld())
        return;
    Group* group = bot->GetGroup();
    view.SelfLeads = false;
    if (!group)
        return;
    view.SelfLeads = group->GetLeaderGUID() == bot->GetGUID();

    // The others in the group's order (in a raid, the seat's own subgroup: a 3.3.5 party frame shows only those),
    // then the leader rotated to the front when it is one of them.
    std::array<Player*, GROUP_MEMBERS> members{};
    uint32 count = 0;
    ObjectGuid const leader = group->GetLeaderGUID();
    for (GroupReference* ref = group->GetFirstMember(); ref && count < GROUP_MEMBERS; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == bot || !member->IsInWorld() || !group->SameSubGroup(bot, member))
            continue;
        members[count++] = member;
    }
    auto const lead = std::find_if(members.begin(), members.begin() + count,
        [&leader](Player const* member) { return member->GetGUID() == leader; });
    std::rotate(members.begin(), lead, lead + (lead != members.begin() + count ? 1 : 0));

    Movement::BodyState const* body = view.Body;
    float const selfX = body ? body->X : bot->GetPositionX();
    float const selfY = body ? body->Y : bot->GetPositionY();
    ObjectGuid const focus = view.Focus ? *view.Focus : ObjectGuid::Empty;
    for (uint32 slot = 0; slot < count; ++slot)
        FillFrame(view.Frames[slot], members[slot], members[slot]->GetGUID() == leader, bot, selfX, selfY,
            view.Facing, view.MinimapYards, focus, view.Seen, view.Party, view.NowMs);
}

Animus::Curriculum::BlockSize Animus::Curriculum::PartyFramesBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ GROUP_MEMBERS * FRAME_FEATURES + SELF_FEATURES, ACTION_COUNT };
}

void Animus::Curriculum::PartyFramesBlock::DescribeManifest(Layout const& /*layout*/,
    boost::json::object& block) const
{
    block["members"] = GROUP_MEMBERS;
    block["member_features"] = uint32(FRAME_FEATURES);
    block["self_features"] = SELF_FEATURES;
}

char const* Animus::Curriculum::PartyFramesBlock::FeatureName(uint32 feature)
{
    switch (feature)
    {
        case FRAME_PRESENT:         return "present";
        case FRAME_ALIVE:           return "alive";
        case FRAME_LEADER:          return "leader";
        case FRAME_IN_COMBAT:       return "in_combat";
        case FRAME_HEALTH:          return "health";
        case FRAME_POWER:           return "power";
        case FRAME_DOT:             return "dot";
        case FRAME_DOT_RIGHT:       return "dot_right";
        case FRAME_DOT_FORWARD:     return "dot_forward";
        case FRAME_DOT_DISTANCE:    return "dot_distance";
        case FRAME_MANA_USER:       return "mana_user";
        case FRAME_IN_RANGE:        return "in_range";
        case FRAME_DEBUFFS:         return "debuffs";
        case FRAME_DISPELLABLE:     return "dispellable";
        case FRAME_AGGRO:           return "aggro";
        case FRAME_SELECTED:        return "selected";
        case FRAME_FOCUSED:         return "focused";
        case FRAME_TARGET:          return "target";
        case FRAME_TARGET_HOSTILE:  return "target_hostile";
        case FRAME_TARGET_MINE:     return "target_mine";
        case FRAME_TARGET_IN_VIEW:  return "target_in_view";
        case FRAME_RECALL:          return "recall";
        case FRAME_RECALL_LIVE:     return "recall_live";
        case FRAME_RECALL_RIGHT:    return "recall_right";
        case FRAME_RECALL_FORWARD:  return "recall_forward";
        case FRAME_RECALL_DISTANCE: return "recall_distance";
        case FRAME_RECALL_AGE:      return "recall_age";
        case FRAME_RECALL_SPEED:    return "recall_speed";
        default:                    return "";
    }
}

void Animus::Curriculum::PartyFramesBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    // member{i}_<feature>: revision 1's names, so M4's columns carry by name.
    for (uint32 member = 0; member < GROUP_MEMBERS; ++member)
        for (uint32 feature = 0; feature < FRAME_FEATURES; ++feature)
            names.emplace_back(Acore::StringFormat("member{}_{}", member, FeatureName(feature)));
    names.emplace_back("self_leader");
}

void Animus::Curriculum::PartyFramesBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    std::fill(obs, obs + GROUP_MEMBERS * FRAME_FEATURES + SELF_FEATURES, 0.0f);
    obs[GROUP_MEMBERS * FRAME_FEATURES] = view.SelfLeads ? 1.0f : 0.0f;
    if (mask)
        std::fill(mask, mask + ACTION_COUNT, uint8(0));
    float const radius = std::max(1.0f, view.MinimapYards);
    for (uint32 member = 0; member < GROUP_MEMBERS; ++member)
    {
        SeatView::PartyFrame const& frame = view.Frames[member];
        if (!frame.Present)
            continue;
        float* out = obs + member * FRAME_FEATURES;
        out[FRAME_PRESENT] = 1.0f;
        out[FRAME_ALIVE] = frame.Alive ? 1.0f : 0.0f;
        out[FRAME_LEADER] = frame.Leader ? 1.0f : 0.0f;
        out[FRAME_IN_COMBAT] = frame.InCombat ? 1.0f : 0.0f;
        out[FRAME_HEALTH] = std::clamp(frame.Health, 0.0f, 1.0f);
        out[FRAME_POWER] = std::clamp(frame.Power, 0.0f, 1.0f);
        out[FRAME_MANA_USER] = frame.ManaUser ? 1.0f : 0.0f;
        out[FRAME_IN_RANGE] = frame.InRange ? 1.0f : 0.0f;
        out[FRAME_DEBUFFS] = std::min(1.0f, float(frame.Debuffs) / 5.0f);
        out[FRAME_DISPELLABLE] = std::min(1.0f, float(frame.Dispellable) / 5.0f);
        out[FRAME_AGGRO] = frame.Aggro ? 1.0f : 0.0f;
        out[FRAME_SELECTED] = frame.Selected ? 1.0f : 0.0f;
        out[FRAME_FOCUSED] = frame.Focused ? 1.0f : 0.0f;
        out[FRAME_TARGET] = frame.HasTarget ? 1.0f : 0.0f;
        out[FRAME_TARGET_HOSTILE] = frame.HasTarget && frame.TargetHostile ? 1.0f : 0.0f;
        out[FRAME_TARGET_MINE] = frame.HasTarget && frame.TargetMine ? 1.0f : 0.0f;
        out[FRAME_TARGET_IN_VIEW] = frame.HasTarget && frame.TargetInView ? 1.0f : 0.0f;
        if (mask)
        {
            // A frame that is there can be clicked; whether the click does anything is the seat's to learn.
            mask[ACTION_SELECT_FIRST + member] = 1;
            mask[ACTION_FOCUS_FIRST + member] = 1;
            mask[ACTION_ASSIST_FIRST + member] = 1;
        }
        if (frame.RecallKnown)
        {
            out[FRAME_RECALL] = 1.0f;
            out[FRAME_RECALL_LIVE] = frame.RecallLive ? 1.0f : 0.0f;
            out[FRAME_RECALL_RIGHT] = std::clamp(frame.RecallRight / RECALL_YARDS, -1.0f, 1.0f);
            out[FRAME_RECALL_FORWARD] = std::clamp(frame.RecallForward / RECALL_YARDS, -1.0f, 1.0f);
            out[FRAME_RECALL_DISTANCE] = std::clamp(std::sqrt(frame.RecallRight * frame.RecallRight
                + frame.RecallForward * frame.RecallForward) / RECALL_YARDS, 0.0f, 1.0f);
            out[FRAME_RECALL_AGE] = std::clamp(frame.RecallAgeSeconds / RECALL_AGE_SECONDS, 0.0f, 1.0f);
            out[FRAME_RECALL_SPEED] = std::clamp(frame.RecallSpeed / PartyRecall::MAX_SPEED, 0.0f, 1.0f);
        }
        if (!frame.DotShown)
            continue;
        out[FRAME_DOT] = 1.0f;
        out[FRAME_DOT_RIGHT] = std::clamp(frame.DotRight / radius, -1.0f, 1.0f);
        out[FRAME_DOT_FORWARD] = std::clamp(frame.DotForward / radius, -1.0f, 1.0f);
        out[FRAME_DOT_DISTANCE] = std::clamp(std::sqrt(frame.DotRight * frame.DotRight
            + frame.DotForward * frame.DotForward) / radius, 0.0f, 1.0f);
    }
}

void Animus::Curriculum::PartyFramesBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (local >= ACTION_COUNT || !view.Bot)
        return;
    SeatView::PartyFrame const& frame = view.Frames[local % GROUP_MEMBERS];
    if (!frame.Present || frame.Guid.IsEmpty())
        return;
    ObjectGuid scratch;
    ObjectGuid& focus = view.Focus ? *view.Focus : scratch;
    EntityActions::ClientPort& port = view.Port ? *view.Port : EntityActions::SessionPort();
    EntityActions::Press const press = local < ACTION_FOCUS_FIRST ? EntityActions::Press::Select
        : local < ACTION_ASSIST_FIRST ? EntityActions::Press::Focus : EntityActions::Press::Assist;
    // The member itself wherever it is on the map (its frame needs no sight of it); its target, for an assist, only
    // where the client has it.
    ObjectGuid const member = frame.Guid;
    EntityActions::Apply(press, view.Bot, member.GetRawValue(), focus, result, port,
        [member](Player* bot, ObjectGuid guid) -> WorldObject*
        {
            return guid == member ? FrameResolve(bot, guid) : EntityActions::ResolveAtClient(bot, guid);
        });
}

std::string Animus::Curriculum::PartyFramesBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    if (local >= ACTION_COUNT)
        return {};
    return Acore::StringFormat("{}_member{}", PressName(local), local % GROUP_MEMBERS);
}
