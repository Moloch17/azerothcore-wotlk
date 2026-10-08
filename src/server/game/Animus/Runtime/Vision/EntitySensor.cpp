/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#include "EntitySensor.h"
#include <algorithm>
#include <cmath>

namespace
{
    using namespace Animus::Vision;

    /// An angle in (-pi, pi].
    [[nodiscard]] float WrapPi(float angle)
    {
        while (angle > PI)
            angle -= 2.0f * PI;
        while (angle <= -PI)
            angle += 2.0f * PI;
        return angle;
    }

    /// A local axis of a candidate's box, in the world: a column of its row-major rotation.
    [[nodiscard]] Vec3 Axis(float const* rot, uint32_t axis)
    {
        return { rot[axis], rot[3 + axis], rot[6 + axis] };
    }

    /// The radius of a sphere that holds the candidate, for the first cull.
    [[nodiscard]] float BoundingRadius(SensorCandidate const& candidate)
    {
        switch (candidate.Shape)
        {
            case SensedShape::Unit:
                return std::sqrt(candidate.Radius * candidate.Radius
                    + 0.25f * candidate.Height * candidate.Height);
            case SensedShape::Hazard:
                return candidate.Radius;
            default:
                return std::sqrt(candidate.Half[0] * candidate.Half[0] + candidate.Half[1] * candidate.Half[1]
                    + candidate.Half[2] * candidate.Half[2]);
        }
    }

    /// Whether any of a candidate can be in the frame: its bounding sphere's centre within the field of view, give or
    /// take the sphere's angular radius (more in yaw near the poles, where a yaw is a short walk).
    [[nodiscard]] bool MayBeInFrame(Rig const& rig, Settings const& settings, SensorCandidate const& candidate)
    {
        Vec3 const offset = candidate.Centre - rig.Camera;
        float const distance = Length(offset);
        float const radius = BoundingRadius(candidate);
        if (distance <= radius)
            return true;
        float yawRight = 0.0f;
        float pitchUp = 0.0f;
        ViewAngles(rig, candidate.Centre, yawRight, pitchUp);
        float const slack = std::asin(std::min(1.0f, radius / distance));
        float const elevation = std::fabs(pitchUp + rig.Elevation);
        float const pole = std::cos(std::min(elevation + slack, 0.5f * PI));
        if (pole < 0.05f)
            return std::fabs(pitchUp) <= settings.FovV * DEGREES / 2.0f + slack;
        return std::fabs(yawRight) <= settings.FovH * DEGREES / 2.0f + slack / pole
            && std::fabs(pitchUp) <= settings.FovV * DEGREES / 2.0f + slack;
    }

    struct Ranked
    {
        float Distance;
        uint64_t Guid;
        uint32_t Index;
    };
}

bool Animus::Vision::ViewAngles(Rig const& rig, Vec3 point, float& yawRight, float& pitchUp)
{
    Vec3 const offset = point - rig.Camera;
    float const length = Length(offset);
    if (!(length > 1e-4f))
    {
        yawRight = 0.0f;
        pitchUp = 0.0f;
        return false;
    }
    float const azimuth = std::atan2(offset.Y, offset.X);
    float const elevation = std::asin(std::clamp(offset.Z / length, -1.0f, 1.0f));
    yawRight = WrapPi(rig.Azimuth - azimuth);
    pitchUp = elevation - rig.Elevation;
    return true;
}

bool Animus::Vision::InFrame(Settings const& settings, float yawRight, float pitchUp)
{
    return std::fabs(yawRight) <= settings.FovH * DEGREES / 2.0f
        && std::fabs(pitchUp) <= settings.FovV * DEGREES / 2.0f;
}

uint32_t Animus::Vision::SamplePoints(Rig const& rig, SensorCandidate const& candidate, Vec3* out, float& shorten)
{
    // The view's horizontal direction to the candidate, and the line across it (lateral samples go along it).
    Vec3 const toward = candidate.Centre - rig.Camera;
    float const flat = std::sqrt(toward.X * toward.X + toward.Y * toward.Y);
    Vec3 const ahead = flat > 1e-4f ? Vec3{ toward.X / flat, toward.Y / flat, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
    Vec3 const side{ -ahead.Y, ahead.X, 0.0f };
    uint32_t count = 0;
    shorten = 0.0f;

    switch (candidate.Shape)
    {
        case SensedShape::Unit:
        {
            // Feet, middle and head along the axis, the feet SAMPLE_LIFT over the ground.
            float const span = std::max(0.0f, candidate.Height - SAMPLE_LIFT);
            auto const at = [&](float share)
            {
                return Vec3{ candidate.Feet.X, candidate.Feet.Y, candidate.Feet.Z + SAMPLE_LIFT + share * span };
            };
            shorten = std::min(candidate.Radius, 0.5f);
            Vec3 const middle = at(0.5f);
            if (candidate.Height < FLAT_HEIGHT)
            {
                // A corpse: its width, across the view.
                out[count++] = middle;
                out[count++] = middle + side * candidate.Radius;
                out[count++] = middle - side * candidate.Radius;
                return count;
            }
            out[count++] = at(0.0f);
            out[count++] = middle;
            out[count++] = at(0.95f);
            if (candidate.Radius > BIG_RADIUS)
            {
                out[count++] = middle + side * (0.8f * candidate.Radius);
                out[count++] = middle - side * (0.8f * candidate.Radius);
            }
            return count;
        }
        case SensedShape::Hazard:
        {
            // The middle and four points of the rim, on the ground.
            Vec3 const centre{ candidate.Feet.X, candidate.Feet.Y, candidate.Feet.Z + SAMPLE_LIFT };
            float const rim = 0.9f * candidate.Radius;
            out[count++] = centre;
            out[count++] = centre - ahead * rim;
            out[count++] = centre + ahead * rim;
            out[count++] = centre + side * rim;
            out[count++] = centre - side * rim;
            return count;
        }
        case SensedShape::Model:
        case SensedShape::Box:
        {
            uint32_t const wide = candidate.Half[0] >= candidate.Half[1] ? 0u : 1u;
            Vec3 const across = Axis(candidate.Rot, wide) * (0.4f * candidate.Half[wide]);
            Vec3 const up = Axis(candidate.Rot, 2);
            out[count++] = candidate.Centre;
            if (candidate.Shape == SensedShape::Model)
                out[count++] = candidate.Centre + up * (0.8f * candidate.Half[2]);
            else
            {
                shorten = std::min(candidate.Half[wide], 0.5f);
                out[count++] = candidate.Centre + up * (0.4f * candidate.Half[2]);
                out[count++] = candidate.Centre - up * (0.4f * candidate.Half[2]);
            }
            out[count++] = candidate.Centre + across;
            out[count++] = candidate.Centre - across;
            return count;
        }
    }
    return count;
}

void Animus::Vision::Sense(Rig const& rig, Settings const& settings, VisionWorld const& world,
    std::span<SensorCandidate const> candidates, SensorOutput& out, Breakdown* breakdown)
{
    out.Seen.clear();
    out.Hazards.clear();
    out.Rays = 0;

    // Cheap first: the hazards (every one is painted), then the candidates whose bounding sphere reaches the frame,
    // nearest the camera first, so the rays go to the ones the list will hold.
    thread_local std::vector<Ranked> ranked;
    ranked.clear();
    for (uint32_t i = 0; i < candidates.size(); ++i)
    {
        SensorCandidate const& candidate = candidates[i];
        if (candidate.Shape == SensedShape::Hazard)
            out.Hazards.push_back({ candidate.Feet.X, candidate.Feet.Y, candidate.Feet.Z, candidate.Radius });
        if (!MayBeInFrame(rig, settings, candidate))
            continue;
        Vec3 const offset = candidate.Centre - rig.Camera;
        ranked.push_back({ Dot(offset, offset), candidate.Guid, i });
    }
    std::sort(ranked.begin(), ranked.end(), [](Ranked const& a, Ranked const& b)
    {
        if (a.Distance != b.Distance)
            return a.Distance < b.Distance;
        return a.Guid != b.Guid ? a.Guid < b.Guid : a.Index < b.Index;
    });

    for (Ranked const& entry : ranked)
    {
        if (out.Seen.size() >= ENTITY_SLOTS)
            break;
        SensorCandidate const& candidate = candidates[entry.Index];
        Vec3 samples[MAX_SAMPLES];
        float shorten = 0.0f;
        uint32_t const count = SamplePoints(rig, candidate, samples, shorten);
        uint32_t clear = 0;
        for (uint32_t s = 0; s < count; ++s)
        {
            float yawRight = 0.0f;
            float pitchUp = 0.0f;
            ViewAngles(rig, samples[s], yawRight, pitchUp);
            if (!InFrame(settings, yawRight, pitchUp))
                continue;
            // The segment ends short of the sample (a body against a wall); a sample that near is in plain view.
            Vec3 const offset = samples[s] - rig.Camera;
            float const length = Length(offset);
            if (length <= shorten + 1e-3f)
            {
                ++clear;
                continue;
            }
            Vec3 const end = samples[s] - offset * (shorten / length);
            ++out.Rays;
            if (!SegmentBlocked(world, rig.Camera, end, candidate.Model, breakdown))
                ++clear;
        }
        if (clear)
            out.Seen.push_back({ entry.Index, float(clear) / float(count) });
    }
}
