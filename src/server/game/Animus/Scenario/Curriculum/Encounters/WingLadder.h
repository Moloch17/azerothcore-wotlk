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

#ifndef ANIMUS_LIB_CURRICULUM_WING_LADDER_H
#define ANIMUS_LIB_CURRICULUM_WING_LADDER_H

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    /// **The whole dungeon's difficulty ladder** (StageScenario::WING_RUNGS): the host's, and the cluster's.
    ///
    /// It moves one way. Once `window` probes at a rung have made, on average, `target` of the dungeon, it steps
    /// down a rung; it never steps back on a score, because a harder rung scores lower by design (the same bug held
    /// M2 at its easiest rung, 2026-10-07). What it does keep is an alarm: the probes' mean, read once for each
    /// `window` fresh probes at a rung, under a floor -- COLLAPSE_FLOOR, or COLLAPSE_SHARE of the mean that earned
    /// the rung -- for COLLAPSE_READS reads running. The alarm is a warning for a person; the ladder does not move on
    /// it. It is computed here, in the ladder the host decides with, so it exists once for the whole cluster.
    ///
    /// Not thread-safe by itself (StageScenario holds its lock); Rung() and CollapsedRung() may be read anywhere.
    class WingLadder
    {
    public:
        static constexpr uint32_t COLLAPSE_READS = 3;
        static constexpr float COLLAPSE_FLOOR = 0.1f;
        static constexpr float COLLAPSE_SHARE = 0.25f;

        struct Step
        {
            uint32_t From = 0;
            uint32_t To = 0;
            float Probes = 0.0f;        // the probes' mean that earned it
            float Others = 0.0f;        // the rung's other runs' mean, for the log
        };

        struct Result
        {
            std::optional<Step> Moved;
            /// The line to log the first time the rung collapses (once per collapse).
            std::optional<std::string> Alarm;
        };

        WingLadder(uint32_t rungs, uint32_t window, float target, uint32_t start);

        [[nodiscard]] uint32_t Rung() const { return _rung.load(std::memory_order_relaxed); }
        /// A worker follows the host's rung and keeps nothing of its own.
        void Follow(uint32_t rung);
        /// A finished training run on `rung` (ignored unless it is the current one).
        Result Note(uint32_t rung, bool probe, float progress);
        /// The rung that has collapsed, or -1.
        [[nodiscard]] int32_t CollapsedRung() const { return _collapsed.load(std::memory_order_relaxed); }

    private:
        uint32_t _rungs;
        uint32_t _window;
        float _target;
        std::atomic<uint32_t> _rung{ 0 };
        std::atomic<int32_t> _collapsed{ -1 };
        std::vector<float> _probes;     // the rung's probes' progress, the latest `window`
        std::vector<float> _others;     // ... its other training runs'
        uint32_t _sinceRead = 0;        // probes since the last read
        std::vector<float> _reads;      // the probes' mean at each read on this rung
        float _lower = 0.0f;            // the mean that earned this rung
    };
}

#endif
