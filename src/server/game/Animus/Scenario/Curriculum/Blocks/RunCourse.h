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

#ifndef ANIMUS_LIB_CURRICULUM_RUN_COURSE_H
#define ANIMUS_LIB_CURRICULUM_RUN_COURSE_H

#include "MoveSpline.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace Animus::Curriculum
{
    /// The direction (radians, atan2 of the leg) of the leg a running spline will be on `aheadMs` from now; none for
    /// a leg with no length. A leg's end is reached a millisecond after the decision that should start the next (the
    /// spline's timestamps start at 1), so the leg under way now reads the previous leg's direction at every decision
    /// boundary; half a decision ahead reads the leg the coming decision walks (Encoding::CourseAhead, RunCourseTest).
    [[nodiscard]] inline std::optional<float> RunCourse(Movement::MoveSpline const& run, uint32 aheadMs)
    {
        auto const& spline = run._Spline();
        int32 const at = std::min(run.timePassed() + int32(aheadMs), run.Duration());
        int32 leg = spline.first();
        while (leg + 1 < spline.last() && spline.length(leg + 1) <= at)
            ++leg;
        G3D::Vector3 const& from = spline.getPoint(leg);
        G3D::Vector3 const& to = spline.getPoint(std::min(leg + 1, spline.last()));
        if (std::fabs(to.x - from.x) + std::fabs(to.y - from.y) < 0.01f)
            return std::nullopt;
        return std::atan2(to.y - from.y, to.x - from.x);
    }

    /// The climb (radians, up positive) of the leg a running spline will be on `aheadMs` from now, read as RunCourse
    /// reads its direction; none for a leg with no length. What the keep check holds a swim or a flight's pitch to:
    /// on a turn or a pitch laid out as one run, the run's end is not where the coming decision goes.
    [[nodiscard]] inline std::optional<float> RunClimb(Movement::MoveSpline const& run, uint32 aheadMs)
    {
        auto const& spline = run._Spline();
        int32 const at = std::min(run.timePassed() + int32(aheadMs), run.Duration());
        int32 leg = spline.first();
        while (leg + 1 < spline.last() && spline.length(leg + 1) <= at)
            ++leg;
        G3D::Vector3 const& from = spline.getPoint(leg);
        G3D::Vector3 const& to = spline.getPoint(std::min(leg + 1, spline.last()));
        float const flat = std::hypot(to.x - from.x, to.y - from.y);
        if (flat + std::fabs(to.z - from.z) < 0.01f)
            return std::nullopt;
        return std::atan2(to.z - from.z, flat);
    }
}

#endif
