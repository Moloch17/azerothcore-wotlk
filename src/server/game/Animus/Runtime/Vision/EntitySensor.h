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

#ifndef ANIMUS_VISION_ENTITY_SENSOR_H
#define ANIMUS_VISION_ENTITY_SENSOR_H

#include "Camera.h"
#include "VisionCaster.h"
#include <cstdint>
#include <span>
#include <vector>

/// **The entity sensor** (entity-sensing plan): who is in view, by line of sight instead of by pixel. An entity is
/// listed when at least one of a few sample points on it is inside the frame (the equal-angle field of view the
/// pixels cover, PixelAngles) and has a clear segment from the camera (SegmentBlocked: the static tree, the dynamic
/// tree and the terrain; no units, no liquids). Entities hide behind walls, not behind each other (a client draws
/// nameplates through units). Geometric, so the same list at every render size, and no ray per pixel per unit.
///
/// Pure over VisionWorld and a candidate list: MapVisionWorld gathers the candidates off the core (cheap facts only),
/// the sensor ranks and tests them, and MapVisionWorld classifies the survivors.
namespace Animus::Vision
{
    /// The shapes the sensor samples, and what each is on the map.
    enum class SensedShape : uint8_t
    {
        Unit,       // a creature, player or corpse: a vertical cylinder from its feet
        Hazard,     // a ground effect: a disc on the floor
        Model,      // a game object in the dynamic tree (a closed door): its model's bounds
        Box,        // a game object with no collision model: its display's box, turned as the object is
    };

    /// The most sample points one candidate takes.
    constexpr uint32_t MAX_SAMPLES = 5;
    /// Sample points sit this far over the ground, so a slope under a body does not hide its own feet.
    constexpr float SAMPLE_LIFT = 0.15f;
    /// A unit shorter than this lies flat (a corpse): sampled across its width, not up its height.
    constexpr float FLAT_HEIGHT = 0.8f;
    /// A unit wider than this gets two lateral samples more (a boss that a pillar hides half of).
    constexpr float BIG_RADIUS = 1.5f;

    /// One candidate, as the gather found it (core facts only; its class and flags come later, for the survivors).
    /// A Unit and a Hazard stand at Feet (a hazard's Radius is the area's, its Height 0); a Model and a Box have
    /// their middle at Centre, half-extents Half along their local axes and the axes' directions in Rot (row-major,
    /// world = Centre + Rot x local: the columns are the axes).
    struct SensorCandidate
    {
        SensedShape Shape = SensedShape::Unit;
        uint64_t Guid = 0;
        Vec3 Feet;
        Vec3 Centre;
        float Radius = 0.0f;
        float Height = 0.0f;
        float Half[3] = {};
        float Rot[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        /// A Model's game object model (SurfaceHit::Object): the target itself, which does not block its own rays.
        void const* Model = nullptr;
    };

    /// A candidate the sensor found in view: its place in the list it was given and the share of its sample points
    /// that were inside the frame with a clear line, in (0, 1].
    struct SensedEntity
    {
        uint32_t Index = 0;
        float Los = 0.0f;
    };

    struct SensorOutput
    {
        /// In view, nearest the camera first (ties by GUID), ENTITY_SLOTS at most.
        std::vector<SensedEntity> Seen;
        /// Every hazard candidate, seen or not: the frame paints them on the floor (Render's `sight`).
        std::vector<HazardDisc> Hazards;
        /// Shadow rays cast.
        uint32_t Rays = 0;
    };

    /// The view angles of a world point from the camera, as the pixels have them: yaw' + to the right, pitch' + up,
    /// both off the view's centre, in radians. False for a point on the camera itself (the angles are 0).
    bool ViewAngles(Rig const& rig, Vec3 point, float& yawRight, float& pitchUp);
    /// Whether view angles are inside the frame.
    [[nodiscard]] bool InFrame(Settings const& settings, float yawRight, float pitchUp);

    /// The candidate's sample points into `out` (MAX_SAMPLES room), how many; `shorten` takes how far short of each
    /// the segment ends, so a body against a wall is not hidden by that wall.
    uint32_t SamplePoints(Rig const& rig, SensorCandidate const& candidate, Vec3* out, float& shorten);

    /// Finds who is in view. `candidates` is whatever the gather found within range, in any order; the output's
    /// indices point into it. Deterministic: sample points are functions of the candidates, the trees and the
    /// terrain are queried read-only, and the ranking is a total order. Fills `breakdown`'s SegmentRays.
    void Sense(Rig const& rig, Settings const& settings, VisionWorld const& world,
        std::span<SensorCandidate const> candidates, SensorOutput& out, Breakdown* breakdown = nullptr);
}

#endif
