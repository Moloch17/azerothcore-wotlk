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

#ifndef ANIMUS_LIB_CURRICULUM_ROLES_DRAW_H
#define ANIMUS_LIB_CURRICULUM_ROLES_DRAW_H

#include "CurriculumTuning.h"
#include "StageDefinition.h"
#include <algorithm>
#include <cstdint>

/// The roles stage's draws and pays (RolesEncounter, dungeon-curriculum G1), pure: what a drill's packs are at a rung,
/// how many stand and how far apart, which role a drill is about, what the drilled seat's lesson pays a decision, and
/// whether an episode was won. The encounter supplies the world (who the enemies are on, the healths, the damage); the
/// tests write it by hand (RolesStageTest).
namespace Animus::Curriculum::RolesDraw
{
    /// The roles a drill is about, as StageState's DungeonRole numbers them (1 tank, 2 healer, 3 damage).
    constexpr uint8 ROLE_TANK = 1;
    constexpr uint8 ROLE_HEALER = 2;
    constexpr uint8 ROLE_DAMAGE = 3;

    /// The seat whose lesson the drill is: the drilled role sits there (StageScenario's DrillRole makeup).
    constexpr uint32 DRILLED_SEAT = 0;

    [[nodiscard]] inline uint8 DrilledRole(RolesDrill drill)
    {
        switch (drill)
        {
            case RolesDrill::Hold:
            case RolesDrill::Pull:  return ROLE_TANK;
            case RolesDrill::Keep:  return ROLE_HEALER;
            case RolesDrill::Focus: return ROLE_DAMAGE;
            default:                return 0;
        }
    }

    /// What one pack is at a rung.
    struct Pack
    {
        int32 LevelOffset = 0;      // over the party's level
        uint32 Size = 1;
        bool Caster = false;        // one of them casts (an interrupt can stop it)
        bool Linked = false;        // one engaged brings the rest
        bool Elite = false;
        uint32 HealthPct = 100;     // of the creatures' own health
    };

    /// The pack of `drill` at rung `tier`: the party's level + LevelBase + half a level a rung, PackSizeFirst creatures
    /// growing by one every PackGrowEvery rungs up to PackSizeMax; heal_keep's at KeepHealthPct of their health, so a
    /// fight outlasts a mana bar.
    [[nodiscard]] inline Pack PlanPack(RolesDrill drill, uint32 tier, CurriculumTuning::RolesTuning const& tuning)
    {
        Pack pack;
        pack.LevelOffset = tuning.LevelBase + int32(tier * tuning.LevelsPerTier / 2);
        pack.Size = std::max<uint32>(1, std::min(tuning.PackSizeMax,
            tuning.PackSizeFirst + tier / std::max<uint32>(1, tuning.PackGrowEvery)));
        pack.Caster = tier >= tuning.CasterTier;
        pack.Linked = tier >= tuning.LinkedTier;
        pack.Elite = tier >= tuning.EliteTier;
        pack.HealthPct = drill == RolesDrill::Keep ? std::max<uint32>(1, tuning.KeepHealthPct) : 100;
        return pack;
    }

    /// How many packs stand at once: the pull drill's camp (CampPacksFirst, one more every two rungs, CampPacksMax at
    /// most), else the pack in front and the next one on -- pulling that one early is the drills' extra pull.
    [[nodiscard]] inline uint32 StandingPacks(RolesDrill drill, uint32 tier,
        CurriculumTuning::RolesTuning const& tuning)
    {
        if (drill != RolesDrill::Pull)
            return 2;
        return std::max<uint32>(2, std::min(tuning.CampPacksMax, tuning.CampPacksFirst + tier / 2));
    }

    /// Yards from one standing pack to the next: the camp's, closing from CampSpacingFirst at rung 0 to CampSpacingLast
    /// at the top (harder to pull alone); the other drills' next pack at `nextNearest`.
    [[nodiscard]] inline float PackSpacing(RolesDrill drill, uint32 tier, uint32 maxTier,
        CurriculumTuning::RolesTuning const& tuning, float nextNearest)
    {
        if (drill != RolesDrill::Pull)
            return nextNearest;
        float const t = maxTier ? std::min(1.0f, float(tier) / float(maxTier)) : 1.0f;
        return tuning.CampSpacingFirst + (tuning.CampSpacingLast - tuning.CampSpacingFirst) * t;
    }

    /// The factor an outcome is worth at rung `tier` (a loss divided by it): 1 + step x tier.
    [[nodiscard]] inline float TierWeight(float step, uint32 tier)
    {
        return 1.0f + step * float(tier);
    }

    /// The drilled seat's lesson in one decision: what it earned (an Outcome, already times w) and what it lost (a
    /// Cost, negative, already over w).
    struct Pay
    {
        float Earned = 0.0f;
        float Lost = 0.0f;
    };

    /// tank_hold and pull (DrillHold): `onSeat` living enemies in a fight whose victim is the tank, `onOthers` on
    /// another of the party. `scale` the decision's share of the reward tuning's (StageScenario::DecisionScale).
    [[nodiscard]] inline Pay HoldPay(uint32 onSeat, uint32 onOthers, CurriculumTuning::RolesTuning const& tuning,
        float scale, float w)
    {
        return { tuning.Hold * float(onSeat) * scale * w, -tuning.Loose * float(onOthers) * scale / w };
    }

    /// damage_discipline (DrillFocus): `onTankTarget` the seat's damage this decision on the tank's own target, as a
    /// share of its damage scale (SeatState::LastStepDamage); `onSeat` enemies it has taken off the tank.
    [[nodiscard]] inline Pay FocusPay(float onTankTarget, uint32 onSeat, CurriculumTuning::RolesTuning const& tuning,
        float scale, float w)
    {
        return { tuning.Focus * std::max(0.0f, onTankTarget) * w, -tuning.PulledOff * float(onSeat) * scale / w };
    }

    /// How a healer's party stands (the seat itself among them): above half health is up, under 35% or dead is low.
    struct Kept
    {
        uint32 Up = 0;
        uint32 Low = 0;
        uint32 Members = 0;
    };
    inline void Count(Kept& kept, bool alive, float healthFraction)
    {
        ++kept.Members;
        if (!alive || healthFraction < 0.35f)
            ++kept.Low;
        else if (healthFraction > 0.5f)
            ++kept.Up;
    }

    /// heal_keep (DrillKeep): the party kept up, members let fall, and `wastedHealths` of healing that landed on no
    /// missing health (in the seat's maximum healths), charged at Overheal x `teammateHealing` (Party.TeammateHealing).
    [[nodiscard]] inline Pay KeepPay(Kept const& kept, float wastedHealths, float teammateHealing,
        CurriculumTuning::RolesTuning const& tuning, float scale, float w)
    {
        return { tuning.Keep * float(kept.Up) * scale * w,
            -(tuning.KeepLow * float(kept.Low) * scale + teammateHealing * tuning.Overheal
                * std::max(0.0f, wastedHealths)) / w };
    }

    /// Several packs in a fight at once: every one of them past the first is an extra pull (counted once a pack),
    /// and none of those fighting is clean any more. Returns the packs newly counted as extra.
    struct PackFight
    {
        bool Fighting = false;      // a member alive and in combat now
        bool Clean = true;          // never in a fight beside another pack
        bool ExtraCounted = false;
    };
    template <typename Packs>
    uint32 NoteFights(Packs& packs)
    {
        uint32 fighting = 0;
        for (auto const& pack : packs)
            fighting += pack.Fight.Fighting ? 1 : 0;
        if (fighting < 2)
            return 0;
        uint32 extra = 0;
        bool first = true;
        for (auto& pack : packs)
        {
            if (!pack.Fight.Fighting)
                continue;
            pack.Fight.Clean = false;
            if (first)
            {
                first = false;
                continue;
            }
            if (!pack.Fight.ExtraCounted)
            {
                pack.Fight.ExtraCounted = true;
                ++extra;
            }
        }
        return extra;
    }

    /// An episode's readings, for `won` and the measures.
    struct Tally
    {
        RolesDrill Drill = RolesDrill::None;
        uint32 Clears = 0;
        uint32 Wipes = 0;
        uint32 PartyDeaths = 0;         // deaths of any seat
        uint32 ExtraPulls = 0;
        uint64 Held = 0;                // enemy-decisions on the drilled tank ...
        uint64 OnParty = 0;             // ... and on the whole party
        float FocusDamage = 0.0f;       // the drilled damage dealer's damage on the tank's target ...
        float Damage = 0.0f;            // ... and all of it
        float PulledOffSeconds = 0.0f;  // ... and its seconds with an enemy on it
    };

    [[nodiscard]] inline float HoldShare(Tally const& tally)
    {
        return tally.OnParty ? float(tally.Held) / float(tally.OnParty) : 0.0f;
    }

    [[nodiscard]] inline float FocusShare(Tally const& tally)
    {
        return tally.Damage > 0.0f ? tally.FocusDamage / tally.Damage : 0.0f;
    }

    /// **Won** (the ladder's window and the evaluation's `won`): a pack cleared and no wipe, and the drill's own
    /// measure -- the tank held WinHold of the enemy-decisions; nobody in the party died; the damage dealer put
    /// WinFocus of its damage on the tank's target and had enemies on it at most WinPulledSeconds; no second pack in a
    /// fight.
    [[nodiscard]] inline bool Won(Tally const& tally, CurriculumTuning::RolesTuning const& tuning)
    {
        if (!tally.Clears || tally.Wipes)
            return false;
        switch (tally.Drill)
        {
            case RolesDrill::Hold:  return HoldShare(tally) >= tuning.WinHold;
            case RolesDrill::Keep:  return tally.PartyDeaths == 0;
            case RolesDrill::Focus: return FocusShare(tally) >= tuning.WinFocus
                && tally.PulledOffSeconds <= tuning.WinPulledSeconds;
            case RolesDrill::Pull:  return tally.ExtraPulls == 0;
            default:                return false;
        }
    }
}

#endif
