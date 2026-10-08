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

#include "CombatBlock.h"
#include "Creature.h"
#include "EncoderSupport.h"
#include "EntityActions.h"
#include "Group.h"
#include "IncomingSpell.h"
#include "Layout.h"
#include "Player.h"
#include "SeatView.h"
#include "StringFormat.h"
#include "ThreatManager.h"
#include <algorithm>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <cmath>

namespace
{
    using Combat = Animus::Curriculum::CombatBlock;

    constexpr char const* FRAME_NAMES[Combat::FRAME_FEATURES] = { "present", "alive", "health", "power", "mana_user",
        "in_range", "in_combat", "debuffs", "dispellable", "aggro", "selected", "focused" };
    constexpr char const* TARGET_NAMES[Combat::TARGET_FEATURES] = { "present", "hostile", "friendly", "in_view",
        "dead", "threat", "threat_pct", "tot_self", "tot_pet", "tot_party", "tot_other", "debuffs" };
    constexpr char const* SLOT_NAMES[Combat::COMBAT_SLOT_FEATURES] = { "casting", "cast_left", "interruptible",
        "cast_heal", "cast_area", "cast_at_me", "controlled", "elite", "in_combat", "attacks_me", "attacks_party",
        "threat", "debuffs" };

    /// Revision 0's names for the two frames that stayed, so their columns and presses carry by name.
    std::string FrameName(uint32 frame)
    {
        return frame == Combat::FRAME_SELF ? "self" : "pet";
    }

    /// A party frame's click: its unit wherever it is on the seat's map (a frame is not a nameplate: it needs no
    /// sight of it).
    WorldObject* FrameResolve(Player* bot, ObjectGuid guid)
    {
        Unit* unit = Animus::Curriculum::Encoding::UnitThrough(*bot, guid);
        return unit && unit->IsInWorld() && unit->GetMap() == bot->GetMap() ? unit : nullptr;
    }
}

uint32 Animus::Curriculum::CombatBlock::ThreatStatus(bool onList, bool isVictim, float mine, float victimThreat,
    float othersTop)
{
    if (!onList)
        return 0;
    if (isVictim)
        return mine > othersTop ? 3 : 2;
    return mine > victimThreat && mine > 0.0f ? 1 : 0;
}

uint32 Animus::Curriculum::CombatBlock::ThreatStatusOf(Unit const* enemy, Unit const* unit)
{
    if (!enemy || !unit || !enemy->IsAlive())
        return 0;
    ThreatManager const& threat = enemy->GetThreatMgr();
    if (!threat.CanHaveThreatList() || !threat.IsThreatenedBy(unit))
        return 0;
    Unit const* victim = threat.GetLastVictim();
    float const mine = threat.GetThreat(unit);
    float othersTop = 0.0f;
    for (ThreatReference const* ref : threat.GetUnsortedThreatList())
        if (ref->GetVictim() != unit)
            othersTop = std::max(othersTop, ref->GetThreat());
    float const victimThreat = victim && victim != unit ? threat.GetThreat(victim) : othersTop;
    return ThreatStatus(true, victim == unit, mine, victimThreat, othersTop);
}

bool Animus::Curriculum::CombatBlock::IsPartyOf(Player const* bot, Unit const* unit)
{
    if (!bot || !unit || unit == bot)
        return false;
    if (unit->GetCharmerOrOwnerGUID() == bot->GetGUID())
        return true;
    Player const* player = unit->ToPlayer();
    return player && bot->GetGroup() && bot->GetGroup() == player->GetGroup();
}

std::array<Unit*, Animus::Curriculum::CombatBlock::OWN_FRAMES> Animus::Curriculum::CombatBlock::FrameUnits(
    Player* bot)
{
    std::array<Unit*, OWN_FRAMES> units{};
    if (!bot)
        return units;
    units[FRAME_SELF] = bot;
    units[FRAME_PET] = Encoding::FirstPet(bot);
    return units;
}

Animus::Curriculum::Encoding::Debuffs Animus::Curriculum::CombatBlock::ShownDebuffs(Unit const* unit,
    Player const* bot, Vision::SeenList const* seen, ObjectGuid focus)
{
    if (!unit || !bot)
        return {};
    return DebuffsShown(unit->GetGUID().GetRawValue(), bot->GetTarget().GetRawValue(), focus.GetRawValue(), seen)
        ? Encoding::IncomingDebuffs(unit) : Encoding::Debuffs();
}

bool Animus::Curriculum::CombatBlock::DebuffsShown(uint64 guid, uint64 selection, uint64 focus,
    Vision::SeenList const* seen)
{
    return guid && (guid == selection || guid == focus || (seen && InView(*seen, guid)));
}

void Animus::Curriculum::CombatBlock::WriteSlot(Unit const* unit, Player const* bot, Vision::SeenList const* seen,
    ObjectGuid focus, float* out)
{
    std::fill(out, out + COMBAT_SLOT_FEATURES, 0.0f);
    if (!unit || !bot || !unit->IsAlive())
        return;

    // The cast bar: IncomingSpell reads the spell's own properties, as a player reads the spell's name off it.
    std::array<float, IncomingSpell::FEATURE_COUNT> cast{};
    if (IncomingSpell::Observe(unit, bot, cast.data()))
    {
        out[SLOT_CASTING] = cast[IncomingSpell::FEATURE_CASTING];
        out[SLOT_CAST_LEFT] = cast[IncomingSpell::FEATURE_CAST_REMAINING];
        out[SLOT_INTERRUPTIBLE] = cast[IncomingSpell::FEATURE_INTERRUPTIBLE];
        out[SLOT_CAST_HEAL] = cast[IncomingSpell::FEATURE_HEALS];
        out[SLOT_CAST_AREA] = cast[IncomingSpell::FEATURE_AREA];
        out[SLOT_CAST_AT_ME] = cast[IncomingSpell::FEATURE_AIMED_AT_ME];
    }
    out[SLOT_CONTROLLED] = Encoding::IsCrowdControlled(unit) ? 1.0f : 0.0f;
    Creature const* creature = unit->ToCreature();
    out[SLOT_ELITE] = creature && creature->isElite() ? 1.0f : 0.0f;
    out[SLOT_IN_COMBAT] = unit->IsInCombat() ? 1.0f : 0.0f;
    Unit const* victim = unit->GetVictim();
    out[SLOT_ATTACKS_ME] = victim == bot ? 1.0f : 0.0f;
    out[SLOT_ATTACKS_PARTY] = IsPartyOf(bot, victim) ? 1.0f : 0.0f;
    out[SLOT_THREAT] = float(ThreatStatusOf(unit, bot)) / 3.0f;
    out[SLOT_DEBUFFS] = std::min(1.0f, float(ShownDebuffs(unit, bot, seen, focus).Count) / 5.0f);
}

uint32 Animus::Curriculum::CombatBlock::VisibleEnemies(Vision::SeenList const& seen, UnitResolver const& resolve,
    Unit** out, uint32 cap)
{
    uint32 count = 0;
    uint32 const listed = std::min<uint32>(seen.Count, Vision::ENTITY_SLOTS);
    for (uint32 slot = 0; slot < listed && count < cap; ++slot)
    {
        Vision::EntityInfo const& info = seen.Info[slot];
        if (info.GameObject || info.Dead || info.Reaction >= 0 || !info.Guid)
            continue;
        Unit* unit = resolve ? resolve(info.Guid) : nullptr;
        if (unit && unit->IsAlive())
            out[count++] = unit;
    }
    return count;
}

bool Animus::Curriculum::CombatBlock::InView(Vision::SeenList const& seen, uint64 guid)
{
    if (!guid)
        return false;
    uint32 const listed = std::min<uint32>(seen.Count, Vision::ENTITY_SLOTS);
    for (uint32 slot = 0; slot < listed; ++slot)
        if (seen.Info[slot].Guid == guid)
            return true;
    return false;
}

Animus::Curriculum::CombatBlock::SeenHazards Animus::Curriculum::CombatBlock::ReadHazards(
    Vision::SeenList const& seen, float x, float y, float facing)
{
    SeenHazards out;
    float deepestLeft = -1.0f;
    float nearestEdge = 0.0f;
    uint32 const listed = std::min<uint32>(seen.Count, Vision::ENTITY_SLOTS);
    for (uint32 slot = 0; slot < listed; ++slot)
    {
        Vision::EntityInfo const& info = seen.Info[slot];
        if (info.Id.What != Vision::Class::GroundHazard || info.Radius <= 0.0f)
            continue;
        float const dx = info.Centre.X - x;
        float const dy = info.Centre.Y - y;
        float const distance = std::sqrt(dx * dx + dy * dy);
        float const bearing = std::remainder(std::atan2(dy, dx) - facing, 2.0f * float(M_PI));
        Hazard hazard;
        hazard.Present = true;
        hazard.Distance = distance;
        hazard.Radius = info.Radius;
        hazard.Bearing = bearing;
        hazard.Centre.Relocate(info.Centre.X, info.Centre.Y, info.Centre.Z);
        if (distance <= info.Radius)
        {
            ++out.Standing;
            float const left = info.Radius - distance;
            if (left > deepestLeft)
            {
                deepestLeft = left;
                out.Deepest = hazard;
            }
        }
        else if (!out.Nearest.Present || distance - info.Radius < nearestEdge)
        {
            nearestEdge = distance - info.Radius;
            out.Nearest = hazard;
        }
    }
    return out;
}

Animus::Curriculum::BlockSize Animus::Curriculum::CombatBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::CombatBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["own_frames"] = OWN_FRAMES;
    block["frame_features"] = uint32(FRAME_FEATURES);
    block["target_features"] = uint32(TARGET_FEATURES);
    boost::json::array slot;
    for (char const* name : SLOT_NAMES)
        slot.push_back(boost::json::string(name));
    block["slot_features"] = std::move(slot);
}

void Animus::Curriculum::CombatBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    for (uint32 frame = 0; frame < OWN_FRAMES; ++frame)
        for (char const* name : FRAME_NAMES)
            names.push_back(boost::json::string(Acore::StringFormat("frame_{}_{}", FrameName(frame), name)));
    for (char const* name : TARGET_NAMES)
        names.push_back(boost::json::string(Acore::StringFormat("target_{}", name)));
}

void Animus::Curriculum::CombatBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    if (mask)
        std::fill(mask, mask + ACTION_COUNT, uint8(0));
    Player* bot = view.Bot;
    if (!bot || !bot->IsInWorld())
        return;

    ObjectGuid const selected = bot->GetTarget();
    ObjectGuid const focus = view.Focus ? *view.Focus : ObjectGuid::Empty;

    // The player frame and the pet frame: always known, on screen or not.
    std::array<Unit*, OWN_FRAMES> const units = FrameUnits(bot);
    for (uint32 frame = 0; frame < OWN_FRAMES; ++frame)
    {
        Unit* unit = units[frame];
        if (!unit)
            continue;
        float* out = obs + OBS_FRAMES_FIRST + frame * FRAME_FEATURES;
        out[FRAME_PRESENT] = 1.0f;
        out[FRAME_ALIVE] = unit->IsAlive() ? 1.0f : 0.0f;
        out[FRAME_HEALTH] = unit->GetHealthPct() / 100.0f;
        Powers const power = unit->getPowerType();
        if (uint32 const top = unit->GetMaxPower(power))
            out[FRAME_POWER] = float(unit->GetPower(power)) / float(top);
        out[FRAME_MANA_USER] = power == POWER_MANA ? 1.0f : 0.0f;
        out[FRAME_IN_RANGE] = unit == bot || (unit->GetMap() == bot->GetMap()
            && bot->GetExactDist(unit) <= FRAME_RANGE) ? 1.0f : 0.0f;
        out[FRAME_IN_COMBAT] = unit->IsInCombat() ? 1.0f : 0.0f;
        Encoding::Debuffs const debuffs = Encoding::IncomingDebuffs(unit);
        out[FRAME_DEBUFFS] = std::min(1.0f, float(debuffs.Count) / 5.0f);
        out[FRAME_DISPELLABLE] = std::min(1.0f, float(debuffs.Dispellable) / 5.0f);
        out[FRAME_AGGRO] = unit->IsAlive() && !unit->getAttackers().empty() ? 1.0f : 0.0f;
        out[FRAME_SELECTED] = selected == unit->GetGUID() ? 1.0f : 0.0f;
        out[FRAME_FOCUSED] = focus == unit->GetGUID() ? 1.0f : 0.0f;
        if (mask)
        {
            mask[ACTION_SELECT_FRAME_FIRST + frame] = 1;
            mask[ACTION_FOCUS_FRAME_FIRST + frame] = 1;
        }
    }

    // The target frame: the selection, while the client still has it.
    Unit* target = selected.IsEmpty() ? nullptr : Encoding::UnitThrough(*bot, selected);
    if (!target || !target->IsInWorld() || target->GetMap() != bot->GetMap() || !EntityActions::AtClient(bot, target))
        return;
    float* out = obs + OBS_TARGET_FIRST;
    out[TARGET_PRESENT] = 1.0f;
    out[TARGET_HOSTILE] = bot->IsHostileTo(target) ? 1.0f : 0.0f;
    out[TARGET_FRIENDLY] = bot->IsFriendlyTo(target) ? 1.0f : 0.0f;
    out[TARGET_IN_VIEW] = view.Seen && InView(*view.Seen, selected.GetRawValue()) ? 1.0f : 0.0f;
    out[TARGET_DEAD] = target->IsAlive() ? 0.0f : 1.0f;
    out[TARGET_THREAT] = float(ThreatStatusOf(target, bot)) / 3.0f;
    out[TARGET_DEBUFFS] = std::min(1.0f, float(ShownDebuffs(target, bot, view.Seen, focus).Count) / 5.0f);
    if (target->IsAlive())
    {
        ThreatManager const& threat = target->GetThreatMgr();
        if (threat.CanHaveThreatList() && threat.IsThreatenedBy(bot))
        {
            Unit const* victim = threat.GetLastVictim();
            float const mine = threat.GetThreat(bot);
            float const held = victim ? threat.GetThreat(victim) : mine;
            out[TARGET_THREAT_PCT] = held > 0.0f ? std::min(1.0f, mine / held / 2.0f) : 0.5f;
        }
        Unit const* tot = target->GetVictim();
        out[TARGET_TOT_SELF] = tot && tot == bot ? 1.0f : 0.0f;
        out[TARGET_TOT_PET] = tot && tot != bot && tot->GetCharmerOrOwnerGUID() == bot->GetGUID() ? 1.0f : 0.0f;
        out[TARGET_TOT_PARTY] = tot && tot != bot && tot->IsPlayer() && IsPartyOf(bot, tot) ? 1.0f : 0.0f;
        out[TARGET_TOT_OTHER] = tot && tot != bot && !IsPartyOf(bot, tot) ? 1.0f : 0.0f;
    }
}

void Animus::Curriculum::CombatBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (local >= ACTION_COUNT || !view.Bot)
        return;
    uint32 const frame = local % OWN_FRAMES;
    Unit* unit = FrameUnits(view.Bot)[frame];
    if (!unit)
        return;
    ObjectGuid scratch;
    ObjectGuid& focus = view.Focus ? *view.Focus : scratch;
    EntityActions::ClientPort& port = view.Port ? *view.Port : EntityActions::SessionPort();
    EntityActions::Press const press = local < ACTION_FOCUS_FRAME_FIRST ? EntityActions::Press::Select
        : EntityActions::Press::Focus;
    EntityActions::Apply(press, view.Bot, unit->GetGUID().GetRawValue(), focus, result, port, FrameResolve);
}

std::string Animus::Curriculum::CombatBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    if (local >= ACTION_COUNT)
        return {};
    return Acore::StringFormat("{}_frame_{}", local < ACTION_FOCUS_FRAME_FIRST ? "select" : "focus",
        FrameName(local % OWN_FRAMES));
}
