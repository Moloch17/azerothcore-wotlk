/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * Portions of this file are derived from the AzerothCore Project.
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

#ifndef ANIMUS_RESET_TIMING_H
#define ANIMUS_RESET_TIMING_H

#include "Define.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace Animus
{
    /// Where an episode reset's time goes, accumulated by the scenario on the thread that resets and read by the
    /// env pool right after: creating the seats' characters and placing them in the map (BotSlot::CreateNext),
    /// phasing, position and talent setup, configuring talents/kit/gear, and destroying the previous seats.
    struct ResetTiming
    {
        uint64 CreateNs = 0;
        uint64 PlaceNs = 0;
        uint64 ConfigureNs = 0;
        uint64 DestroyNs = 0;
        uint64 EncounterNs = 0;     // the encounters' Build: opponents, objectives, spawn retries
        uint64 StockNs = 0;         // supplies and pets
        uint64 PrepareNs = 0;       // Rebuild before the seats: the draws, the encounters' episode resets
        uint64 DespawnNs = 0;       // the previous episode's targets despawned
        uint64 SeatsNs = 0;         // the seats' loop as a whole (create, place and configure are inside it)
        uint64 ScenarioNs = 0;      // Scenario::Reset as a whole, from the pool
        uint64 RouteNs = 0;         // RoutePlanner::Plan, wherever in the reset it was asked (inside EncounterNs
                                    // when an encounter's Build plans)
        uint32 Routes = 0;          // ... and how many plans
    };

    inline thread_local ResetTiming CurrentReset;

    /// The last WINDOW resets one by one, for the status line's p50 / p95 (player-controller C8). A mean per decision
    /// hides a reset that stalls among the cheap ones -- a 500 yard route planned on the thread that resets (M5), a
    /// ledge search that tries again and again (M3) -- and the decision waits for it all the same. Added to by the
    /// world thread and by the map threads' reset tasks; a reset is rare next to a lock.
    class ResetSamples
    {
    public:
        static constexpr std::size_t WINDOW = 1024;

        struct Sample
        {
            uint64 PlacementNs = 0;     // the encounters' Build: objectives, routes, spawn retries (EncounterNs)
            uint64 RouteNs = 0;
            uint32 Routes = 0;
            uint64 ResetNs = 0;         // the reset as a whole
        };

        struct Quantiles
        {
            double P50Ms = 0.0;
            double P95Ms = 0.0;
            double MaxMs = 0.0;
            double MeanMs = 0.0;        // where the time goes in sum: what a stall's cause is read from
        };

        struct Summary
        {
            uint32 Count = 0;           // resets in the window
            Quantiles Placement;
            Quantiles Route;            // per reset, every plan it made together
            Quantiles Reset;
            double RoutesPerReset = 0.0;
        };

        void Add(Sample const& sample)
        {
            std::lock_guard<std::mutex> guard(_lock);
            _ring[_added % WINDOW] = sample;
            ++_added;
        }

        /// A new scenario: the last one's resets say nothing about this one's.
        void Clear()
        {
            std::lock_guard<std::mutex> guard(_lock);
            _added = 0;
        }

        [[nodiscard]] Summary Summarise() const
        {
            std::vector<uint64> placement;
            std::vector<uint64> route;
            std::vector<uint64> reset;
            uint64 routes = 0;
            {
                std::lock_guard<std::mutex> guard(_lock);
                std::size_t const count = std::min(_added, WINDOW);
                placement.reserve(count);
                route.reserve(count);
                reset.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    placement.push_back(_ring[i].PlacementNs);
                    route.push_back(_ring[i].RouteNs);
                    reset.push_back(_ring[i].ResetNs);
                    routes += _ring[i].Routes;
                }
            }
            Summary out;
            out.Count = uint32(placement.size());
            out.Placement = Of(placement);
            out.Route = Of(route);
            out.Reset = Of(reset);
            out.RoutesPerReset = out.Count ? double(routes) / double(out.Count) : 0.0;
            return out;
        }

        /// The nearest-rank p50 and p95 and the largest of `ns` (sorted in place), in ms.
        [[nodiscard]] static Quantiles Of(std::vector<uint64>& ns)
        {
            Quantiles out;
            if (ns.empty())
                return out;
            std::sort(ns.begin(), ns.end());
            auto const rank = [&ns](double q)
            {
                std::size_t const at = std::size_t(std::ceil(q * double(ns.size())));
                return double(ns[std::max<std::size_t>(at, 1) - 1]) / 1e6;
            };
            out.P50Ms = rank(0.50);
            out.P95Ms = rank(0.95);
            out.MaxMs = double(ns.back()) / 1e6;
            double sum = 0.0;
            for (uint64 value : ns)
                sum += double(value);
            out.MeanMs = sum / double(ns.size()) / 1e6;
            return out;
        }

    private:
        mutable std::mutex _lock;
        std::array<Sample, WINDOW> _ring{};
        std::size_t _added = 0;
    };

    /// Every env pool's resets (one pool runs at a time).
    inline ResetSamples RecentResets;

    /// What a window of resets that stalls the sim stalls on, for the status line's warning (player-controller C8).
    enum class StallCause : uint8
    {
        None = 0,
        Routes,         // RoutePlanner::Plan is at least half of the reset time (M5's long trips, M3's ledge reaches)
        Placement,      // the encounters' Build less its routes is (M3's ledge search, objectives, spawn retries)
        Reset,          // the rest of the reset is: characters, kit, despawns
    };

    /// Fewer resets than this say nothing about a p95.
    constexpr uint32 STALL_MIN_RESETS = 20;
    /// A p95 reset under this is never a tail stall, however short the decisions.
    constexpr double STALL_FLOOR_MS = 20.0;
    /// Resets taking this share of a decision's wall time or more stall the sim: the rest of the decision waits for
    /// them (the world thread's resets are serial; the map threads' sit inside their map's task).
    constexpr double STALL_SHARE = 0.25;

    struct StallVerdict
    {
        StallCause Cause = StallCause::None;
        double Share = 0.0;             // reset time per decision over the decision's wall time
        bool Tail = false;              // one reset in twenty longer than a whole decision (and the floor)
    };

    /// Whether the resets stall the sim (the M3 dry check, 2026-10-05: route plans of ~150 ms at p95, under the 250 ms
    /// decision cadence the old rule compared against while the sim ran 17 ms decisions, and sps halved -- reported
    /// only at the stage's end). Two ways: the reset time summed per decision (`resetMsPerDecision`: the world
    /// thread's and the map threads' resets per decision, from the pool's timings) is STALL_SHARE or more of the
    /// decision's wall time (`decisionMs`: world + sim + learner), which is throughput lost; or one reset in twenty
    /// takes longer than a whole decision, which is a decision held up. The cause is where the reset time goes in sum
    /// (the means): route planning, the rest of placement, or the rest of the reset.
    [[nodiscard]] inline StallVerdict Stall(ResetSamples::Summary const& resets, double resetMsPerDecision,
        double decisionMs)
    {
        StallVerdict out;
        if (resets.Count < STALL_MIN_RESETS || decisionMs <= 0.0)
            return out;
        out.Share = resetMsPerDecision / decisionMs;
        out.Tail = resets.Reset.P95Ms > std::max(STALL_FLOOR_MS, decisionMs);
        if (out.Share < STALL_SHARE && !out.Tail)
            return out;
        double const total = std::max(resets.Reset.MeanMs, 1e-9);
        out.Cause = resets.Route.MeanMs * 2.0 >= total ? StallCause::Routes
            : resets.Placement.MeanMs * 2.0 >= total ? StallCause::Placement : StallCause::Reset;
        return out;
    }

    /// Nanoseconds since `from`, and move `from` to now.
    inline uint64 ResetSinceNs(std::chrono::steady_clock::time_point& from)
    {
        auto const now = std::chrono::steady_clock::now();
        uint64 const ns = uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - from).count());
        from = now;
        return ns;
    }
}

#endif
