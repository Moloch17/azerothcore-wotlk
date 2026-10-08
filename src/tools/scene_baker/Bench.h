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

#ifndef SCENE_BAKER_BENCH_H
#define SCENE_BAKER_BENCH_H

#include <cstdint>
#include <string>
#include <vector>

namespace SceneBaker
{
    /// A camera pose as `forge camera snapshot` takes it: feet, yaw and pitch in degrees, zoom in yards.
    struct PoseSpec
    {
        std::string Name;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float YawDeg = 0.0f;
        float PitchDeg = 0.0f;
        float Zoom = 0.0f;
    };

    /// Poses from a text file: one `name x y z yawDeg pitchDeg zoom` a line (# starts a comment).
    [[nodiscard]] bool ReadPoses(std::string const& path, std::vector<PoseSpec>& poses, std::string& error);

    /// Renders each pose from the baked scene at `scenePath` with the real Vision::Render (128 x 64, 120 x 60, no
    /// units), times it `reps` times after a warm-up, and writes <outDir>/newcam-<name>-{depth.pgm,kind.ppm,height.pgm}
    /// and <outDir>/new_timing.json. Prints a line a pose.
    [[nodiscard]] bool RunBench(std::string const& scenePath, std::vector<PoseSpec> const& poses,
        std::string const& outDir, uint32_t reps, std::string& error);

    /// Checks the tracer against a brute-force double-precision test of every triangle, for each pose's pixel rays
    /// and `randomRays` random segments inside the scene. Prints the result; false on any disagreement.
    [[nodiscard]] bool RunVerify(std::string const& scenePath, std::vector<PoseSpec> const& poses,
        uint32_t randomRays, std::string& error);
}

#endif
