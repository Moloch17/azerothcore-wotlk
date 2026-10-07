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

#ifndef ANIMUS_LIB_CURRICULUM_SIGHT_DRAW_H
#define ANIMUS_LIB_CURRICULUM_SIGHT_DRAW_H

#include "Camera.h"
#include "Position.h"
#include "VisionCaster.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

/// **The sight stage's draws** (SightEncounter, M1 redesigned): the compass's withholding by rung, the evaluation's
/// (pair, compass) for a seed, and where the object goes -- in sight of the spawn, or just round a corner -- as
/// functions of the hallway table, a VisionWorld and uniform numbers, so they are tested with a fake world and fixed
/// numbers (SightEncounterTest) and the encounter only supplies the map and the randomness.
namespace Animus::Curriculum::SightDraw
{
    /// The withholding ladder's rungs: the shaping fade's scales they sit on (configs/move1_controls.yaml fade.rungs),
    /// the first rung first. The compass is withheld with Controls.Withhold<i> at scale FADE_SCALES[i], and linearly
    /// between (the fade only ever sends these four).
    constexpr uint32 RUNGS = 4;
    constexpr std::array<float, RUNGS> FADE_SCALES = { 1.0f, 0.5f, 0.25f, 0.0f };

    /// The rung a shaping scale is on: the nearest of FADE_SCALES.
    inline uint32 Rung(float shaping)
    {
        uint32 best = 0;
        for (uint32 rung = 1; rung < RUNGS; ++rung)
            if (std::fabs(FADE_SCALES[rung] - shaping) < std::fabs(FADE_SCALES[best] - shaping))
                best = rung;
        return best;
    }

    /// The chance an episode withholds the compass at shaping scale `shaping`: chances[i] at FADE_SCALES[i], linear
    /// between, clamped to [0, 1].
    inline float WithholdChance(float shaping, std::array<float, RUNGS> const& chances)
    {
        float const scale = std::clamp(shaping, 0.0f, 1.0f);
        for (uint32 rung = 0; rung + 1 < RUNGS; ++rung)
        {
            float const high = FADE_SCALES[rung];
            float const low = FADE_SCALES[rung + 1];
            if (scale <= high && scale >= low)
            {
                float const t = high > low ? (high - scale) / (high - low) : 0.0f;
                return std::clamp(chances[rung] + (chances[rung + 1] - chances[rung]) * t, 0.0f, 1.0f);
            }
        }
        return std::clamp(chances[RUNGS - 1], 0.0f, 1.0f);
    }

    struct EvaluationEpisode
    {
        uint32 Pair = 0;
        bool Withheld = false;
    };

    /// An evaluation's pair and compass for seed `seed`. The scenario plays casting `seed mod castings`
    /// (StageScenario::DrawCasting), so the seeds go round the castings in rounds of `castings`: round r withholds
    /// the compass when r is odd, and casting c plays pair ((r / 2) x castings + c) mod pairs in it. Rounds 2k and
    /// 2k + 1 play the same pairs, so every casting meets each of its pairs once with the compass and once without;
    /// the pairs follow on from one round pair to the next, so an evaluation of whole round pairs plays every pair
    /// within one time of every other. A compass bit on the seed's parity would give every casting the same bit when
    /// the casting count is even.
    inline EvaluationEpisode EvaluationPick(uint32 seed, uint32 castings, uint32 pairs)
    {
        castings = std::max<uint32>(1, castings);
        uint32 const round = seed / castings;
        uint64 const slot = uint64(round / 2) * castings + seed % castings;
        return { uint32(slot % std::max<uint32>(1, pairs)), round % 2 == 1 };
    }

    /// Whether `centre` is in sight from `eye`, the camera's way: one ray toward it, cast as the camera casts its
    /// pixels (Vision::CastRay, the static and dynamic trees, the WMO liquids and the terrain; no units), meets
    /// nothing before it is within `radius` of the centre -- the object's own bounding radius, so the object itself,
    /// once it stands there, is not what blocks the view of it.
    inline bool InSight(Vision::Vec3 eye, Vision::Vec3 centre, float radius, Vision::VisionWorld const& world)
    {
        Vision::Vec3 const toward = centre - eye;
        float const length = Vision::Length(toward);
        if (length <= radius)
            return true;
        Vision::Hit const hit = Vision::CastRay(eye, toward * (1.0f / length), world, Vision::Sight());
        return hit.Distance >= length - radius;
    }

    /// Where a placement looks from and at.
    struct Viewing
    {
        float EyeRise = 1.8f;       // the eye over the feet: Vision::PIVOT_SHARE x the body's height
        float CentreRise = 0.5f;    // the object's centre over its base: half its height
        float Radius = 0.5f;        // its bounding radius
        float Nearest = 10.0f;      // yards from the spawn, straight
        float Furthest = 120.0f;
        float StoreyRise = 4.0f;    // a corner's stepping point is on the spawn's floor: within this of its height
        float CornerStep = 8.0f;    // ... and within this many yards of it
        uint32 Attempts = 64;       // candidates tested at most
    };

    struct Placement
    {
        int32 Point = -1;           // the object's point, an index into the table; -1 none
        bool Corner = false;
        int32 Step = -1;            // a corner's stepping point: in sight of the spawn, and the object in its sight
    };

    /// Whether table point `object` is in sight of a seat standing at `from` (the feet), the object standing on it.
    inline bool Seen(std::vector<Position> const& points, Position const& from, uint32 object, Viewing const& viewing,
        Vision::VisionWorld const& world)
    {
        Position const& at = points[object];
        return InSight({ from.GetPositionX(), from.GetPositionY(), from.GetPositionZ() + viewing.EyeRise },
            { at.GetPositionX(), at.GetPositionY(), at.GetPositionZ() + viewing.CentreRise }, viewing.Radius, world);
    }

    /// The table's points a seat at `from` could step to on its way round a corner: on its floor, within CornerStep,
    /// and in its sight (a point seen is a point walked to straight; nothing is asked of the navmesh).
    inline std::vector<uint32> Steps(std::vector<Position> const& points, Position const& from, Viewing const& viewing,
        Vision::VisionWorld const& world)
    {
        std::vector<uint32> steps;
        for (uint32 index = 0; index < points.size(); ++index)
        {
            Position const& step = points[index];
            float const away = from.GetExactDist2d(step.GetPositionX(), step.GetPositionY());
            if (away < 0.5f || away > viewing.CornerStep
                || std::fabs(step.GetPositionZ() - from.GetPositionZ()) > viewing.StoreyRise)
                continue;
            Viewing eyeLevel = viewing;
            eyeLevel.CentreRise = viewing.EyeRise;
            eyeLevel.Radius = 0.0f;
            if (Seen(points, from, index, eyeLevel, world))
                steps.push_back(index);
        }
        return steps;
    }

    /// The stepping point of `steps` the object at `object` is in sight of (a seat there sees it), or -1.
    inline int32 SeenFromStep(std::vector<Position> const& points, std::vector<uint32> const& steps, uint32 object,
        Viewing const& viewing, Vision::VisionWorld const& world)
    {
        for (uint32 step : steps)
            if (Seen(points, points[step], object, viewing, world))
                return int32(step);
        return -1;
    }

    /// **Where the object goes**: a table point Nearest to Furthest yards (straight) from `from`, drawn uniformly by
    /// `uniform` (a callable giving numbers in [0, 1)) among those that qualify: in sight of the seat's eye, or with
    /// `corner` asked, not in its sight but in sight of a stepping point a few yards' walk away (Steps). At most
    /// Attempts candidates are tested; a corner that finds none falls back to a point in sight (Placement::Corner
    /// says which was got), and none at all is Point -1.
    template <typename Uniform>
    Placement Place(std::vector<Position> const& points, Position const& from, bool corner, Viewing const& viewing,
        Vision::VisionWorld const& world, Uniform&& uniform)
    {
        std::vector<uint32> band;
        for (uint32 index = 0; index < points.size(); ++index)
        {
            float const away = from.GetExactDist2d(points[index].GetPositionX(), points[index].GetPositionY());
            if (away >= viewing.Nearest && away <= viewing.Furthest)
                band.push_back(index);
        }
        // A uniform order (Fisher-Yates), tested from the front.
        for (std::size_t i = band.size(); i > 1; --i)
            std::swap(band[i - 1], band[std::min(i - 1, std::size_t(uniform() * float(i)))]);

        std::vector<uint32> const steps = corner ? Steps(points, from, viewing, world) : std::vector<uint32>();
        uint32 tested = 0;
        int32 inSight = -1;
        for (uint32 candidate : band)
        {
            if (tested++ >= viewing.Attempts)
                break;
            bool const seen = Seen(points, from, candidate, viewing, world);
            if (!corner && seen)
                return { int32(candidate), false, -1 };
            if (corner && seen)
            {
                if (inSight < 0)
                    inSight = int32(candidate);
                continue;
            }
            if (corner && !seen)
                if (int32 const step = SeenFromStep(points, steps, candidate, viewing, world); step >= 0)
                    return { int32(candidate), true, step };
        }
        if (inSight >= 0)
            return { inSight, false, -1 };
        // A corner that met no point in sight among its candidates: one more pass for the fallback, in the same order.
        if (corner)
            for (uint32 candidate : band)
                if (Seen(points, from, candidate, viewing, world))
                    return { int32(candidate), false, -1 };
        return {};
    }
}

#endif
