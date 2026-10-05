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

#include "TravelBlock.h"
#include "EncoderSupport.h"
#include "Map.h"
#include "Player.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    constexpr float MAX_GROUND_SEARCH = 200.0f;
    constexpr float AIRBORNE_ABOVE = 2.0f;      // higher than this without flight is falling

    struct MountSpell
    {
        uint8 Level;        // a player of this level has it
        uint32 Alliance;
        uint32 Horde;
        bool Flying;
    };

    /// Riding as players learn it in 3.3.5: skill spells by level, then the side's mounts of each speed.
    constexpr std::array<std::pair<uint8, uint32>, 5> RIDING_SKILLS = { {
        { 20, 33388 },      // Apprentice Riding: 60% ground mounts
        { 40, 33391 },      // Journeyman Riding: 100%
        { 60, 34090 },      // Expert Riding: 150% flying mounts
        { 68, 54197 },      // Cold Weather Flying: flying in Northrend (trainable at 68 since patch 3.2)
        { 70, 34091 },      // Artisan Riding: 280% flying
    } };

    constexpr std::array<MountSpell, 4> MOUNTS = { {
        { 20, 458, 580, false },            // Brown Horse, Timber Wolf
        { 40, 23229, 23250, false },        // Swift Brown Steed, Swift Brown Wolf
        { 60, 32235, 32243, true },         // Golden Gryphon, Tawny Wind Rider
        { 70, 32242, 32246, true },         // Swift Blue Gryphon, Swift Red Wind Rider
    } };

    SpellInfo const* FastestMount(Player const* bot, bool flying)
    {
        SpellInfo const* best = nullptr;
        for (MountSpell const& mount : MOUNTS)
        {
            uint32 const spellId = bot->GetTeamId() == TEAM_HORDE ? mount.Horde : mount.Alliance;
            if (mount.Flying == flying && bot->HasSpell(spellId))
                best = sSpellMgr->GetSpellInfo(spellId);
        }

        return best;
    }

    /// Whether `bot` could summon `mount` -- running included: choosing it stops the bot and casts, as pressing
    /// the key does. Gating this on a finished spline instead made mounting unreachable on the only trips worth
    /// mounting for: the seat moves every decision, so the spline is live from the first one to the last, and the
    /// action was masked out of every decision but the one before the bot had started.
    /// `reason` reports the SpellCastResult when the cast is what refused it, SPELL_CAST_OK when the refusal
    /// was one of the cheap checks above it, and NO_MOUNT_KNOWN when there is no such mount to summon.
    constexpr uint32 NO_MOUNT_KNOWN = 0xFFFF;

    bool CanSummon(Player* bot, SpellInfo const* mount, uint32* reason = nullptr)
    {
        if (reason)
            *reason = mount ? uint32(SPELL_CAST_OK) : NO_MOUNT_KNOWN;

        if (!mount || bot->IsMounted() || !bot->IsAlive() || Encoding::CastInProgress(bot)
            || bot->GetGlobalCooldownMgr().HasGlobalCooldown(mount))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        return Animus::SpellChecks::CheckCast(bot, mount, targets, nullptr, reason);
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        if (!bot->IsAlive())
            return false;

        switch (action)
        {
            case TravelBlock::ACTION_MOUNT_GROUND:
                // An air-only arena masks the ground mount (ArenaDefinition::AirOnly): its objective cannot be
                // walked to, and a ride that cannot arrive is not a choice worth exploring into.
                return view.MountsAllowed && view.GroundMountAllowed && CanSummon(bot, TravelBlock::GroundMount(bot));
            case TravelBlock::ACTION_MOUNT_FLYING:
                return view.MountsAllowed && CanSummon(bot, TravelBlock::FlyingMount(bot));
            case TravelBlock::ACTION_DISMOUNT:
                return bot->IsMounted();
            default:
                return false;
        }
    }
}

SpellInfo const* Animus::Curriculum::TravelBlock::GroundMount(Player const* bot)
{
    return FastestMount(bot, false);
}

bool Animus::Curriculum::TravelBlock::CanSummonFlying(Player* bot, uint32* reason)
{
    return CanSummon(bot, FlyingMount(bot), reason);
}

SpellInfo const* Animus::Curriculum::TravelBlock::FlyingMount(Player const* bot)
{
    return FastestMount(bot, true);
}

float Animus::Curriculum::TravelBlock::HeightAboveGround(Player const* bot)
{
    float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), true,
        MAX_GROUND_SEARCH);
    return ground > INVALID_HEIGHT ? std::max(0.0f, bot->GetPositionZ() - ground) : 0.0f;
}

bool Animus::Curriculum::TravelBlock::AtObjective(Player const* bot, Position const& objective, float maxRise,
    float within)
{
    return bot->GetExactDist2d(&objective) <= within && HeightAboveGround(bot) <= AIRBORNE_ABOVE
        && std::fabs(bot->GetPositionZ() - objective.GetPositionZ()) <= maxRise;
}

void Animus::Curriculum::TravelBlock::LearnRiding(Player* bot)
{
    uint8 const level = bot->GetLevel();
    for (auto const& [requiredLevel, spellId] : RIDING_SKILLS)
        if (level >= requiredLevel && !bot->HasSpell(spellId))
            bot->learnSpell(spellId);

    for (MountSpell const& mount : MOUNTS)
    {
        uint32 const spellId = bot->GetTeamId() == TEAM_HORDE ? mount.Horde : mount.Alliance;
        if (level >= mount.Level && !bot->HasSpell(spellId))
            bot->learnSpell(spellId);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::TravelBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

std::string Animus::Curriculum::TravelBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "mount_ground", "mount_flying", "dismount",
    };

    return local < NAMES.size() ? NAMES[local] : std::string();
}

void Animus::Curriculum::TravelBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;

    obs[OBS_MOUNTED] = bot->IsMounted() ? 1.0f : 0.0f;
    obs[OBS_FLYING_MOUNT] = bot->IsMounted() && bot->CanFly() ? 1.0f : 0.0f;
    obs[OBS_RIDING_SKILL] = std::min(1.0f, float(bot->GetSkillValue(SKILL_RIDING)) / 300.0f);
    obs[OBS_INDOORS] = bot->IsOutdoors() ? 0.0f : 1.0f;
    obs[OBS_HEIGHT] = std::min(1.0f, HeightAboveGround(bot) / 50.0f);
    obs[OBS_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
    obs[OBS_MOVING] = bot->isMoving() ? 1.0f : 0.0f;

    UnitMoveType const moveType = bot->CanFly() ? MOVE_FLIGHT : MOVE_RUN;
    obs[OBS_SPEED] = std::min(1.0f, bot->GetSpeed(moveType) / TravelBlock::BASE_RUN_SPEED / 4.0f);

    if (view.HasObjective)
    {
        // In the seat's own frame (SeatView::Facing), the frame every bearing the move block reports is measured
        // in. GetRelativeAngle reads the unit's orientation, and a flight spline -- which is not orientation-fixed
        // -- writes the direction of travel onto that every tick, so the two blocks disagreed about where the
        // objective lay exactly when it mattered most: in the air.
        float const relative = bot->GetAngle(view.Objective.GetPositionX(), view.Objective.GetPositionY())
            - view.Facing;
        float const bearing = std::atan2(std::sin(relative), std::cos(relative));
        obs[OBS_OBJECTIVE] = 1.0f;
        obs[OBS_OBJECTIVE_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(&view.Objective) / 500.0f);
        obs[OBS_OBJECTIVE_BEARING_SIN] = std::sin(bearing);
        obs[OBS_OBJECTIVE_BEARING_COS] = std::cos(bearing);
        obs[OBS_OBJECTIVE_HEIGHT] = std::clamp((view.Objective.GetPositionZ() - bot->GetPositionZ()) / 50.0f, -1.0f,
            1.0f);
        // The same radius the reward pays arrival at (SeatView::ArriveWithin: two yards indoors, six outside), so the
        // feature and the reward never disagree about being there.
        obs[OBS_AT_OBJECTIVE] = AtObjective(bot, view.Objective, ARRIVE_ANY_RISE, view.ArriveWithin) ? 1.0f : 0.0f;
    }

    // Asked here rather than of the mask: the policy sees whether mounting is possible even when no mask is wanted.
    obs[OBS_CAN_MOUNT] = IsAllowed(view, ACTION_MOUNT_GROUND) ? 1.0f : 0.0f;
    obs[OBS_CAN_FLY] = IsAllowed(view, ACTION_MOUNT_FLYING) ? 1.0f : 0.0f;

    for (uint32 action = 0; mask && action < ACTION_COUNT; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::TravelBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;

    switch (local)
    {
        case ACTION_MOUNT_GROUND:
        case ACTION_MOUNT_FLYING:
        {
            SpellInfo const* mount = local == ACTION_MOUNT_GROUND ? GroundMount(bot) : FlyingMount(bot);
            // A mount has a cast time, and Spell::prepare refuses one from a moving caster: standing still for it is
            // the seat's own (its keys), as a player's is.
            SpellCastTargets targets;
            targets.SetUnitTarget(bot);
            Spell* spell = new Spell(bot, mount, TRIGGERED_NONE);
            if (spell->prepare(&targets) == SPELL_CAST_OK)
            {
                ++result.SpellCasts;
                // The keys stay the seat's own (MoveBlock): a mount cast with one held is cancelled by the core,
                // as a player's is, and the seat learns to stand still for it.
            }
            return;
        }
        case ACTION_DISMOUNT:
            // As CMSG_CANCEL_MOUNT_AURA. In the air, the controller falls from there (its flight is gone) and the
            // server lands it with its own damage.
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            return;
        default:
            return;
    }
}
