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

#ifndef ANIMUS_LIB_ENV_DECISION_CLOCK_H
#define ANIMUS_LIB_ENV_DECISION_CLOCK_H

#include "Define.h"
#include <algorithm>
#include <array>
#include <vector>

namespace Animus
{
    /// How long each decision of the sim lasts in game time (docs/forge/decisions/0021-decision-time-jitter.md).
    ///
    /// The realm's companions decide when their accumulated world-tick time reaches DecisionMs and keep the remainder
    /// (mod-animus CompanionParty: `SinceDecisionMs += diff; if (>= DecisionMs) { %= DecisionMs; Decide }`). The tick
    /// that crosses the threshold overshoots it by `o`, so the decision after it comes
    ///
    ///     dt = DecisionMs - carry + o        carry = the last decision's `o` mod DecisionMs
    ///
    /// after the one before: a mean of DecisionMs however long the ticks are, a spread of a tick either side, and a
    /// load spike (one long tick) makes one long interval and a short one after it. The clock draws `o` from a uniform
    /// body U(0, JitterMs) plus, with SpikeProb, a spike of U(50, SpikeMaxMs); with both off, dt is exactly the
    /// nominal one.
    ///
    /// A decision is `ticks` world updates of the nominal `tickMs`. Plan() sizes all of them from the decision's dt
    /// (dt / ticks each, the remainder on the last), so a long carry never makes a tick negative. Under half-batch
    /// the host plans one tick at a time (ticks = 1, tickMs = half a decision): a group's interval is then the sum of
    /// two consecutive ticks, which is the same formula over every second overshoot.
    ///
    /// The stream is the clock's own -- a hash of (seed, draw index) -- and never the world thread's random numbers:
    /// an evaluation episode reseeds those, and a draw here would move every roll after it.
    class DecisionClock
    {
    public:
        struct Params
        {
            uint32 JitterMs = 0;
            float SpikeProb = 0.0f;
            uint32 SpikeMaxMs = 0;
        };

        /// Statistics buckets: one millisecond each, the last one open.
        static constexpr uint32 BUCKETS = 2048;

        struct Stats
        {
            uint64 Count = 0;
            uint64 SumMs = 0;
            uint32 MaxMs = 0;
            uint32 MinMs = 0;
            uint64 Spikes = 0;
            [[nodiscard]] double Mean() const { return Count ? double(SumMs) / double(Count) : 0.0; }
        };

        /// Start a stream: a new run. Forgets the carry and the statistics.
        void Reset(Params const& params, uint64 seed)
        {
            _params = params;
            _seed = seed;
            _draw = 0;
            _carry = 0;
            _previous = 0;
            _plan.clear();
            _stats = Stats();
            _histogram.fill(0);
        }

        [[nodiscard]] bool Active() const
        {
            return _params.JitterMs > 0 || (_params.SpikeProb > 0.0f && _params.SpikeMaxMs > 50);
        }

        /// Size the ticks of the next decision: `ticks` updates of a nominal `tickMs`. `pairWithPrevious` is half-batch:
        /// the interval a group lives is this tick's and the last one's.
        void Plan(uint32 ticks, uint32 tickMs, bool pairWithPrevious)
        {
            ticks = std::max<uint32>(1, ticks);
            tickMs = std::max<uint32>(1, tickMs);
            uint32 const nominal = ticks * tickMs;

            uint32 overshoot = 0;
            bool spike = false;
            if (_params.JitterMs)
                overshoot = uint32(Next() % (uint64(_params.JitterMs) + 1));
            if (_params.SpikeProb > 0.0f && _params.SpikeMaxMs > 50 && Uniform() < double(_params.SpikeProb))
            {
                overshoot += 50 + uint32(Next() % (uint64(_params.SpikeMaxMs) - 50 + 1));
                spike = true;
            }

            // Never less than a millisecond a tick.
            uint32 const length = std::max<uint32>(ticks, nominal + overshoot - std::min(nominal + overshoot, _carry));
            _carry = overshoot % nominal;

            _plan.assign(ticks, length / ticks);
            _plan.back() = length - (ticks - 1) * (length / ticks);

            uint32 const seen = pairWithPrevious ? length + _previous : length;
            if (!pairWithPrevious || _previous)
                Note(seen, spike);
            _previous = length;
        }

        /// The ms of tick `index` (< ticks) of the decision planned last.
        [[nodiscard]] uint32 TickMs(uint32 index) const
        {
            return index < _plan.size() ? _plan[index] : (_plan.empty() ? 1 : _plan.back());
        }

        [[nodiscard]] Stats const& Statistics() const { return _stats; }
        [[nodiscard]] Params const& Settings() const { return _params; }

        /// The q-quantile (0..1) of the decision intervals seen, in ms.
        [[nodiscard]] uint32 Quantile(double q) const
        {
            if (!_stats.Count)
                return 0;
            uint64 const target = std::max<uint64>(1, uint64(q * double(_stats.Count) + 0.999999));
            uint64 seen = 0;
            for (uint32 ms = 0; ms < BUCKETS; ++ms)
            {
                seen += _histogram[ms];
                if (seen >= target)
                    return ms == BUCKETS - 1 ? _stats.MaxMs : ms;
            }
            return _stats.MaxMs;
        }

    private:
        void Note(uint32 ms, bool spike)
        {
            ++_stats.Count;
            _stats.SumMs += ms;
            _stats.MaxMs = std::max(_stats.MaxMs, ms);
            _stats.MinMs = _stats.Count == 1 ? ms : std::min(_stats.MinMs, ms);
            _stats.Spikes += spike ? 1 : 0;
            ++_histogram[std::min<uint32>(ms, BUCKETS - 1)];
        }

        /// SplitMix64 over (seed, draw index): a stream that needs no state beyond the index.
        uint64 Next()
        {
            uint64 z = _seed + 0x9E3779B97F4A7C15ull * (++_draw);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        double Uniform() { return double(Next() >> 11) * (1.0 / 9007199254740992.0); }

        Params _params;
        uint64 _seed = 0;
        uint64 _draw = 0;
        uint32 _carry = 0;
        uint32 _previous = 0;
        std::vector<uint32> _plan;
        Stats _stats;
        std::array<uint64, BUCKETS> _histogram{};
    };
}

#endif
