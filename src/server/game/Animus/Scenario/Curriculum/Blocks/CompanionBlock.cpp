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

    /// **Following like a player** (Component H). The seat trails the owner along the owner's own path, TRAIL
    /// yards of it back (OwnerTrail), at the owner's pace, starting only when it has fallen more than START yards
    /// behind and settling within STOP of the owner once the owner stands still. The old follow aimed two yards
    /// behind the owner's *facing* and re-aimed whenever that spot moved two yards: the spot swung round with every
    /// turn and strafe, the run restarted about every decision, and on screen the companion lurched after its owner
    /// (in-game testing, 2026-09-28). Along the path, it takes the door the owner took rather than the wall beside it.
    constexpr float FOLLOW_TRAIL = 3.5f;
    constexpr float FOLLOW_START_DISTANCE = 6.0f;
    constexpr float FOLLOW_STOP_DISTANCE = 3.0f;
    /// A running follow is re-aimed at most once a second, or sooner when its run is about to end with the owner
    /// still moving (so the feet never stop between two runs) or when the spot has moved far from where it heads.
    constexpr uint32 FOLLOW_REAIM_MS = 1000;
    constexpr float FOLLOW_REAIM_YARDS = 2.0f;
    constexpr float FOLLOW_RUN_OUT_YARDS = 2.0f;
    constexpr float FOLLOW_FAR_YARDS = 6.0f;
    /// Within this far past the trail distance the seat moves at the owner's own speed; further back it runs to
    /// catch up.
    constexpr float FOLLOW_PACE_BAND = 3.0f;

    /// Where the seat should be: TRAIL yards back along the owner's path (the trail's samples, newest first), so a
    /// turn or a strafe does not swing it and a corner is taken where the owner took it. With too little path yet
    /// (a fresh episode, an owner that has stood still), the point TRAIL yards back towards the seat.
    Position TrailSpot(Player const* bot, Player const* owner, OwnerTrail const* trail)
    {
        if (bot->GetExactDist2d(owner) <= FOLLOW_TRAIL)
            return bot->GetPosition();

        if (trail && trail->Count)
        {
            Position from = owner->GetPosition();
            float walked = 0.0f;
            for (uint32 i = 0; i < trail->Count; ++i)
            {
                Position const& to = trail->Back(i);
                float const leg = from.GetExactDist2d(&to);
                if (walked + leg >= FOLLOW_TRAIL && leg > 0.0f)
                {
                    float const t = (FOLLOW_TRAIL - walked) / leg;
                    return Position(from.GetPositionX() + (to.GetPositionX() - from.GetPositionX()) * t,
                        from.GetPositionY() + (to.GetPositionY() - from.GetPositionY()) * t,
                        from.GetPositionZ() + (to.GetPositionZ() - from.GetPositionZ()) * t);
                }
                walked += leg;
                from = to;
            }
        }

        float const angle = owner->GetAbsoluteAngle(bot);
        float x = owner->GetPositionX() + FOLLOW_TRAIL * std::cos(angle);
        float y = owner->GetPositionY() + FOLLOW_TRAIL * std::sin(angle);
        float z = owner->GetPositionZ();
        owner->UpdateAllowedPositionZ(x, y, z);
        return Position(x, y, z);
    }

    /// The pace: the owner's own speed (walking, slowed, running) when the seat is close behind it, its own full
    /// run when it has fallen back. Never faster than the seat can run.
    float Pace(Player const* bot, Player const* owner)
    {
        float const own = bot->GetSpeed(MOVE_RUN);
        if (!owner->isMoving() || bot->GetExactDist2d(owner) > FOLLOW_TRAIL + FOLLOW_PACE_BAND)
            return own;

        float const theirs = owner->GetSpeed(owner->IsWalking() ? MOVE_WALK : MOVE_RUN);
        return std::clamp(theirs, own * 0.25f, own);
    }

    /// Run to the trail spot, unless the run under way still serves: re-aimed on a fresh follow, when the run has
    /// finished or is about to with the owner moving, and otherwise at most once a second when the spot has moved.
    /// Returns whether a run was started.
    bool Aim(Player* bot, Player const* owner, SeatOptionSet* options, uint64 nowMs, bool fresh)
    {
        SeatOption* option = options ? &options->Of(SeatOptionKind::Follow) : nullptr;
        Position const spot = TrailSpot(bot, owner, options ? &options->Trail : nullptr);
        if (!fresh && !bot->movespline->Finalized())
        {
            G3D::Vector3 const heading = bot->movespline->FinalDestination();
            float const drift = spot.GetExactDist2d(heading.x, heading.y);
            bool const runningOut = owner->isMoving() && bot->GetExactDist2d(heading.x, heading.y) <= FOLLOW_RUN_OUT_YARDS;
            bool const due = option && nowMs >= option->AimedMs + FOLLOW_REAIM_MS && drift > FOLLOW_REAIM_YARDS;
            if (!runningOut && !due && drift <= FOLLOW_FAR_YARDS)
                return false;
        }
        else if (!fresh && bot->GetExactDist2d(owner) <= FOLLOW_STOP_DISTANCE)
            return false;           // close enough: stand, rather than shuffle up to the owner's heels

        if (bot->GetExactDist2d(&spot) < 0.5f)
            return false;

        // Walking only at a walking pace: the walk animation at a catching-up run is a slide.
        float const pace = Pace(bot, owner);
        Encoding::FollowTo(bot, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), pace,
            owner->IsWalking() && pace <= bot->GetSpeed(MOVE_WALK) + 0.01f);
        if (option)
            option->AimedMs = nowMs;
        return true;
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
                    && bot->GetDistance(owner) > FOLLOW_START_DISTANCE
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

void Animus::Curriculum::CompanionBlock::BeforeApply(SeatView& view, SeatActionResult& result) const
{
    // The running follow: re-aimed at where the owner is now, over until the seat is there and the owner has
    // stopped. An owner that is gone, dead or on another map ends it too; the encoder ends it when the feet are told
    // something else, and its clock ends it on its own.
    Player* bot = view.Bot;
    Player* owner = view.Owner;

    // The owner's path, one sample a decision, whether or not a follow is running: a follow that starts has the
    // way the owner went to trail along.
    if (view.Option && owner && bot && owner->IsInMap(bot))
    {
        OwnerTrail& trail = view.Option->Trail;
        if (!trail.Count || trail.Back(0).GetExactDist2d(owner) > 0.25f)
            trail.Add(owner->GetPosition());
    }

    if (!view.Option || !view.Option->Running(SeatOptionKind::Follow, view.NowMs))
        return;

    if (!bot || !bot->IsAlive() || !owner || !owner->IsAlive() || !owner->IsInMap(bot))
    {
        view.Option->Stop(SeatOptionKind::Follow);
        return;
    }

    result.FollowDistance = bot->GetExactDist2d(owner);
    if (result.FollowDistance <= FOLLOW_STOP_DISTANCE + FOLLOW_TRAIL && !owner->isMoving()
        && bot->movespline->Finalized())
    {
        view.Option->Stop(SeatOptionKind::Follow);
        return;
    }

    if (bot->IsNonMeleeSpellCast(false, false, true) || bot->HasUnitState(Encoding::IMMOBILE_STATES))
        return;

    if (Aim(bot, owner, view.Option, view.NowMs, false))
        ++result.FollowAims;
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
            if (view.Option && !view.Option->Running(SeatOptionKind::Follow, view.NowMs))
                ++result.FollowStarts;
            if (view.Option)
                view.Option->Start(SeatOptionKind::Follow, view.NowMs + view.Options.FollowMs);
            if (Aim(bot, owner, view.Option, view.NowMs, true))
                ++result.FollowAims;
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
