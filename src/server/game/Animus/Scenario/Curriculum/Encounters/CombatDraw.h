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

#ifndef ANIMUS_LIB_CURRICULUM_COMBAT_DRAW_H
#define ANIMUS_LIB_CURRICULUM_COMBAT_DRAW_H

#include "CurriculumTuning.h"
#include "StageDefinition.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

/// The combat stages' draws (CombatEncounter, dungeon-curriculum C1-C3), pure: what a rung's pull is, where the seat
/// may start, what the episode's outcome terms are worth at a rung, and when a respawned seat is back. The encounter
/// supplies the world (the creature pool, the dungeon's spawn points, a path's length); the tests write it by hand.
namespace Animus::Curriculum::CombatDraw
{
    /// What one pull is at a rung.
    struct Pull
    {
        int32 LevelOffset = 0;      // over the seat's level
        uint32 Size = 1;            // creatures in it
        bool Elite = false;         // one of them is an elite
        bool Caster = false;        // one of them casts (an interrupt can stop it)
        bool Hazard = false;        // one of them puts fire on the ground
        bool Linked = false;        // one engaged brings the rest
    };

    /// The pull of `drill` at rung `tier`. `fireRoll`: this pack drew fire (Combat.HazardChance, or an arena that
    /// always has it); `casterRoll`: C1's creature drew a caster (Difficulty.CasterChance), from Combat.CasterTier on.
    [[nodiscard]] inline Pull PlanPull(CombatDrill drill, uint32 tier, CurriculumTuning::CombatTuning const& tuning,
        bool fireRoll, bool casterRoll)
    {
        Pull pull;
        int32 const steps = int32(tier * tuning.LevelsPerTier);
        switch (drill)
        {
            case CombatDrill::Fight:
                pull.LevelOffset = tuning.LevelBase + steps;
                pull.Size = 1;
                pull.Elite = tier >= tuning.EliteTier;
                pull.Caster = !pull.Elite && tier >= tuning.CasterTier && casterRoll;
                break;
            case CombatDrill::Packs:
                // The pack grows on the way up instead of its level: two, three, then four.
                pull.LevelOffset = tuning.LevelBase + steps / 2;
                pull.Size = std::min<uint32>(4, 2 + tier / 2);
                pull.Elite = tier >= tuning.EliteTier;
                pull.Caster = tier >= tuning.CasterTier;
                pull.Hazard = fireRoll;
                pull.Linked = tier >= tuning.LinkedTier;
                break;
            case CombatDrill::Survive:
                pull.LevelOffset = tuning.LevelBase + steps + int32(tuning.SurviveLevels);
                pull.Size = std::max<uint32>(1, tuning.SurviveSize) + (tier >= 3 ? 1 : 0);
                pull.Elite = tier >= tuning.EliteTier;
                pull.Caster = tier >= tuning.CasterTier;
                pull.Hazard = fireRoll;
                pull.Linked = true;
                break;
            case CombatDrill::None:
                break;
        }
        return pull;
    }

    /// The level a creature of `pull` is, for a seat of `seatLevel`: never below 1, never above the game's.
    [[nodiscard]] inline uint8 CreatureLevel(uint8 seatLevel, Pull const& pull)
    {
        return uint8(std::clamp<int32>(int32(seatLevel) + pull.LevelOffset, 1, 83));
    }

    /// The factor an outcome is worth at rung `tier` (and a death divided by): 1 + step x tier.
    [[nodiscard]] inline float TierWeight(float step, uint32 tier)
    {
        return 1.0f + step * float(tier);
    }

    /// A place on the dungeon's ground.
    struct Point
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    /// **Where a seat may start**: the dungeon's own creature spawn points (its corridors, which the stage clears),
    /// in the order given, kept when `walk` -- the length of the way from the entrance, negative for none -- is at
    /// most `reach` and the point is at least `spacing` yards from every point kept before it.
    [[nodiscard]] inline std::vector<Point> CorridorPoints(std::vector<Point> const& candidates,
        std::function<float(Point const&)> const& walk, float reach, float spacing)
    {
        std::vector<Point> kept;
        for (Point const& point : candidates)
        {
            bool const near = std::any_of(kept.begin(), kept.end(), [&point, spacing](Point const& other)
            {
                float const dx = other.X - point.X;
                float const dy = other.Y - point.Y;
                float const dz = other.Z - point.Z;
                return dx * dx + dy * dy + dz * dz < spacing * spacing;
            });
            if (near)
                continue;
            float const length = walk(point);
            if (length < 0.0f || length > reach)
                continue;
            kept.push_back(point);
        }
        return kept;
    }

    /// A respawned seat is back at the fight: within `yards` of where it fell, on its floor (`rise` yards up or down).
    [[nodiscard]] inline bool Rejoined(Point const& at, Point const& fight, float yards, float rise = 6.0f)
    {
        float const dx = at.X - fight.X;
        float const dy = at.Y - fight.Y;
        return dx * dx + dy * dy <= yards * yards && std::fabs(at.Z - fight.Z) <= rise;
    }

    /// A dead seat's moment to come back at the entrance (dungeon-curriculum I4): `delayMs` after it fell.
    [[nodiscard]] inline bool RespawnDue(uint32 deadSinceMs, uint32 nowMs, uint32 delayMs)
    {
        return deadSinceMs && nowMs >= deadSinceMs + delayMs;
    }

    /// An episode won, for the rung's window (DifficultyLadder::Record): it took something down and nothing took it
    /// down -- a creature killed or a pack cleared, and no death.
    [[nodiscard]] inline bool Won(uint32 killsOrClears, uint32 deaths)
    {
        return killsOrClears > 0 && deaths == 0;
    }
}

#endif
