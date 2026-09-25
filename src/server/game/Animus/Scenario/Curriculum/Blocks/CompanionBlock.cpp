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

#include "CompanionBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "MoveSpline.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    constexpr uint32 FOLLOW_MOVE_POINT_ID = 3;
    constexpr float FOLLOW_DISTANCE = 2.0f;
    constexpr float FOLLOW_MIN_DISTANCE = 4.0f;
    /// A running follow is re-aimed when where it is heading and where behind the owner now is have come this far
    /// apart. Every decision would path-find for every following seat of every env whether the owner had moved or
    /// not; a couple of yards is under what an owner covers in a decision at a run.
    constexpr float FOLLOW_REAIM_YARDS = 2.0f;

    /// Where "just behind the owner" is right now.
    Position BehindOwner(Player const* bot, Player const* owner)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        owner->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_DISTANCE,
            Position::NormalizeOrientation(owner->GetOrientation() + float(M_PI)));
        return Position(x, y, z);
    }

    /// Run to just behind the owner, unless the run already under way is heading there.
    void Aim(Player* bot, Player const* owner, bool fresh)
    {
        Position const spot = BehindOwner(bot, owner);
        if (!fresh && !bot->movespline->Finalized())
        {
            G3D::Vector3 const heading = bot->movespline->FinalDestination();
            if (spot.GetExactDist2d(heading.x, heading.y) <= FOLLOW_REAIM_YARDS)
                return;
        }

        Encoding::MoveTo(bot, FOLLOW_MOVE_POINT_ID, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ());
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        Player* owner = view.Owner;
        if (action >= CompanionBlock::ACTION_REVIVE_FIRST)
        {
            uint32 const revive = action - CompanionBlock::ACTION_REVIVE_FIRST;
            return revive < view.L->AllyRevives.size() && Encoding::CanRevive(view, view.L->AllyRevives[revive], owner);
        }

        if (!owner || !owner->IsAlive() || !bot->IsAlive() || !owner->IsInMap(bot))
            return false;

        switch (action)
        {
            case CompanionBlock::ACTION_FOLLOW:
                // Masked while it runs, as every option's own action is: nothing cancels itself.
                return !bot->IsNonMeleeSpellCast(false, false, true) && !bot->HasUnitState(Encoding::IMMOBILE_STATES)
                    && bot->GetDistance(owner) > FOLLOW_MIN_DISTANCE
                    && !(view.Option && view.Option->Running(SeatOptionKind::Follow, view.NowMs));
            case CompanionBlock::ACTION_ASSIST:
            {
                int32 const slot = Encoding::SlotOf(view, owner->GetVictim());
                return slot >= 0 && uint32(slot) != view.TargetSlot && owner->GetVictim()->IsAlive();
            }
            case CompanionBlock::ACTION_GUARD:
                return Encoding::SlotAttacking(view, owner, view.TargetSlot) >= 0;
            default:
                return false;
        }
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::CompanionBlock::Size(Layout const& layout) const
{
    uint32 const revives = uint32(layout.AllyRevives.size());
    return { OBS_GLOBAL_COUNT + revives * 2, ACTION_REVIVE_FIRST + revives };
}

void Animus::Curriculum::CompanionBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    block["ally_revives"] = SpellList(layout.AllyRevives);
}

void Animus::Curriculum::CompanionBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    Player* owner = view.Owner;

    if (owner && owner->IsInMap(bot))
    {
        float const bearing = bot->GetRelativeAngle(owner);
        obs[OBS_OWNER_PRESENT] = 1.0f;
        obs[OBS_OWNER_ALIVE] = owner->IsAlive() ? 1.0f : 0.0f;
        obs[OBS_OWNER_HEALTH] = owner->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = owner->GetMaxPower(POWER_MANA))
            obs[OBS_OWNER_MANA] = float(owner->GetPower(POWER_MANA)) / float(maxMana);
        obs[OBS_OWNER_DISTANCE] = std::min(1.0f, bot->GetDistance(owner) / 40.0f);
        obs[OBS_OWNER_BEARING_SIN] = std::sin(bearing);
        obs[OBS_OWNER_BEARING_COS] = std::cos(bearing);
        obs[OBS_OWNER_IN_COMBAT] = owner->IsInCombat() ? 1.0f : 0.0f;
        // The movement flags, not the spline: a real owner moves by its client's packets and never has a spline
        // running, so asking the spline reported every human owner as standing still. A scripted owner's spline sets
        // the same flags (MoveSplineInit::Launch), so training and play agree.
        obs[OBS_OWNER_MOVING] = owner->isMoving() ? 1.0f : 0.0f;
        obs[OBS_OWNER_LEVEL_DIFF] = (float(owner->GetLevel()) - float(bot->GetLevel())) / 5.0f;
        WriteOneHot(PLAYABLE_CLASSES, owner->getClass(), obs + OBS_OWNER_CLASS_FIRST);

        int32 const ownerTarget = Encoding::SlotOf(view, owner->GetVictim());
        if (ownerTarget >= 0)
            obs[OBS_OWNER_TARGET_FIRST + ownerTarget] = 1.0f;
        else
            obs[OBS_OWNER_NO_TARGET] = 1.0f;

        uint32 attackers = 0;
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
        {
            Unit* enemy = view.Enemies[slot];
            if (enemy && enemy->IsAlive() && enemy->GetVictim() == owner)
            {
                obs[OBS_SLOT_ON_OWNER_FIRST + slot] = 1.0f;
                ++attackers;
            }
        }

        obs[OBS_OWNER_ATTACKERS] = float(attackers) / float(PACK_SLOTS);
    }

    if (view.Option && view.Option->Running(SeatOptionKind::Follow, view.NowMs))
        obs[OBS_FOLLOWING] = std::min(1.0f, float(view.Option->Of(SeatOptionKind::Follow).UntilMs - view.NowMs)
            / float(std::max<uint32>(1, view.Options.FollowMs)));

    Encoding::WriteRevives(view, obs + OBS_GLOBAL_COUNT);

    uint32 const actions = view.L->Slice(BlockId::Companion).ActionCount;
    for (uint32 action = 0; mask && action < actions; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::CompanionBlock::BeforeApply(SeatView& view, SeatActionResult& /*result*/) const
{
    // The running follow: re-aimed at where the owner is now, over until the seat is there and the owner has
    // stopped. An owner that is gone, dead or on another map ends it too; the encoder ends it when the feet are told
    // something else, and its clock ends it on its own.
    if (!view.Option || !view.Option->Running(SeatOptionKind::Follow, view.NowMs))
        return;

    Player* bot = view.Bot;
    Player* owner = view.Owner;
    if (!bot || !bot->IsAlive() || !owner || !owner->IsAlive() || !owner->IsInMap(bot))
    {
        view.Option->Stop(SeatOptionKind::Follow);
        return;
    }

    if (bot->GetDistance(owner) <= FOLLOW_MIN_DISTANCE && !owner->isMoving())
    {
        view.Option->Stop(SeatOptionKind::Follow);
        return;
    }

    if (bot->IsNonMeleeSpellCast(false, false, true) || bot->HasUnitState(Encoding::IMMOBILE_STATES))
        return;

    Aim(bot, owner, false);
}

void Animus::Curriculum::CompanionBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;
    Player* owner = view.Owner;
    if (local >= ACTION_REVIVE_FIRST)
    {
        Encoding::Revive(view, view.L->AllyRevives[local - ACTION_REVIVE_FIRST], owner, result);
        return;
    }

    switch (local)
    {
        case ACTION_FOLLOW:
            if (view.Option)
                view.Option->Start(SeatOptionKind::Follow, view.NowMs + view.Options.FollowMs);
            Aim(bot, owner, true);
            return;
        case ACTION_ASSIST:
        case ACTION_GUARD:
        {
            int32 const slot = local == ACTION_ASSIST ? Encoding::SlotOf(view, owner->GetVictim())
                : Encoding::SlotAttacking(view, owner, view.TargetSlot);
            if (slot >= 0 && view.Enemies[slot])
                Encoding::SelectEnemy(view, uint32(slot));
            return;
        }
        default:
            break;
    }
}
