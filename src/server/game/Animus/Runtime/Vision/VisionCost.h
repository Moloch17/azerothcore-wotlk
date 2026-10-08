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

#ifndef ANIMUS_VISION_VISION_COST_H
#define ANIMUS_VISION_VISION_COST_H

#include <atomic>
#include <cstdint>

/// What the camera costs the sim, for the status line's vision row: thread time rendering frames, the frames and
/// the rays cast. Every map thread adds once per frame (a few hundred times a second), as ControllerCost does.
namespace Animus::Vision::Cost
{
    inline std::atomic<uint64_t> Ns{ 0 };
    inline std::atomic<uint64_t> Frames{ 0 };
    inline std::atomic<uint64_t> Rays{ 0 };

    inline void Add(uint64_t ns, uint64_t rays)
    {
        Ns.fetch_add(ns, std::memory_order_relaxed);
        Frames.fetch_add(1, std::memory_order_relaxed);
        Rays.fetch_add(rays, std::memory_order_relaxed);
    }

    /// The entity sensor's shadow rays (entity-sensing), beside the pixel rays above.
    inline std::atomic<uint64_t> SensorRays{ 0 };

    inline void AddSensor(uint64_t rays)
    {
        SensorRays.fetch_add(rays, std::memory_order_relaxed);
    }

    /// The mental map's (perception-goals REDESIGN §3, amendment 2: measured in the vision row): thread time writing
    /// a frame and the body into the map and cropping it, the writes, and the tiles kept (summed over the writes, for
    /// the mean a seat keeps).
    inline std::atomic<uint64_t> MapNs{ 0 };
    inline std::atomic<uint64_t> MapWrites{ 0 };
    inline std::atomic<uint64_t> MapTiles{ 0 };

    inline void AddMap(uint64_t ns, uint64_t tiles)
    {
        MapNs.fetch_add(ns, std::memory_order_relaxed);
        MapWrites.fetch_add(1, std::memory_order_relaxed);
        MapTiles.fetch_add(tiles, std::memory_order_relaxed);
    }

    /// The STEPs sent to the learner (amendment 4: the STEP's size and the sim's send time per decision go in the
    /// status row): bytes, the sends' wall time, and the STEPs.
    inline std::atomic<uint64_t> StepBytes{ 0 };
    inline std::atomic<uint64_t> StepNs{ 0 };
    inline std::atomic<uint64_t> Steps{ 0 };

    inline void AddStep(uint64_t bytes, uint64_t ns)
    {
        StepBytes.fetch_add(bytes, std::memory_order_relaxed);
        StepNs.fetch_add(ns, std::memory_order_relaxed);
        Steps.fetch_add(1, std::memory_order_relaxed);
    }
}

#endif
