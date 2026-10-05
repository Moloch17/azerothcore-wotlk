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

#include "ResetTiming.h"
#include "gtest/gtest.h"
#include <thread>
#include <vector>

using namespace Animus;

namespace
{
    constexpr uint64 MS = 1000000;

    ResetSamples::Sample Reset(uint64 placementMs, uint64 routeMs, uint64 resetMs, uint32 routes = 1)
    {
        return { placementMs * MS, routeMs * MS, routes, resetMs * MS };
    }
}

// Nearest rank: of 1..100 ms the p50 is 50, the p95 95, the largest 100.
TEST(ResetSamplesTest, QuantilesAreNearestRank)
{
    std::vector<uint64> ns;
    for (uint64 ms = 100; ms >= 1; --ms)
        ns.push_back(ms * MS);
    ResetSamples::Quantiles const q = ResetSamples::Of(ns);
    EXPECT_DOUBLE_EQ(q.P50Ms, 50.0);
    EXPECT_DOUBLE_EQ(q.P95Ms, 95.0);
    EXPECT_DOUBLE_EQ(q.MaxMs, 100.0);

    std::vector<uint64> one{ 7 * MS };
    ResetSamples::Quantiles const single = ResetSamples::Of(one);
    EXPECT_DOUBLE_EQ(single.P50Ms, 7.0);
    EXPECT_DOUBLE_EQ(single.P95Ms, 7.0);

    std::vector<uint64> none;
    EXPECT_DOUBLE_EQ(ResetSamples::Of(none).MaxMs, 0.0);
}

// One stall in twenty is the p95 a mean hides: 19 cheap resets and one 600 yd route.
TEST(ResetSamplesTest, OneLongRouteInTwentyIsThePercentile)
{
    ResetSamples samples;
    for (int i = 0; i < 19; ++i)
        samples.Add(Reset(2, 1, 5));
    samples.Add(Reset(400, 390, 410, 3));
    ResetSamples::Summary const s = samples.Summarise();
    EXPECT_EQ(s.Count, 20u);
    EXPECT_DOUBLE_EQ(s.Placement.P50Ms, 2.0);
    EXPECT_DOUBLE_EQ(s.Placement.P95Ms, 2.0);    // 19 of 20 cheap: the 95th is still cheap ...
    EXPECT_DOUBLE_EQ(s.Placement.MaxMs, 400.0);  // ... and the stall is the largest
    EXPECT_DOUBLE_EQ(s.RoutesPerReset, 22.0 / 20.0);

    samples.Add(Reset(400, 390, 410, 3));       // two in 21: now the p95
    EXPECT_DOUBLE_EQ(samples.Summarise().Reset.P95Ms, 410.0);
}

// The window keeps the last WINDOW resets, and a new scenario forgets the last one's.
TEST(ResetSamplesTest, TheWindowRollsAndClears)
{
    ResetSamples samples;
    for (std::size_t i = 0; i < ResetSamples::WINDOW; ++i)
        samples.Add(Reset(100, 0, 100));
    for (std::size_t i = 0; i < ResetSamples::WINDOW; ++i)
        samples.Add(Reset(1, 0, 1));
    ResetSamples::Summary const s = samples.Summarise();
    EXPECT_EQ(s.Count, ResetSamples::WINDOW);
    EXPECT_DOUBLE_EQ(s.Placement.MaxMs, 1.0);

    samples.Clear();
    EXPECT_EQ(samples.Summarise().Count, 0u);
    samples.Add(Reset(3, 0, 3));
    EXPECT_EQ(samples.Summarise().Count, 1u);
    EXPECT_DOUBLE_EQ(samples.Summarise().Placement.P95Ms, 3.0);
}

// The world thread and the map threads' reset tasks add at once.
TEST(ResetSamplesTest, ConcurrentAddsAreAllCounted)
{
    ResetSamples samples;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&samples]
        {
            for (int i = 0; i < 200; ++i)
                samples.Add(Reset(1, 0, 2));
        });
    for (std::thread& thread : threads)
        thread.join();
    ResetSamples::Summary const s = samples.Summarise();
    EXPECT_EQ(s.Count, 800u);
    EXPECT_DOUBLE_EQ(s.Reset.MaxMs, 2.0);
}

// A stall is a p95 reset over a whole decision and the floor, named for what took most of it.
TEST(ResetSamplesTest, StallsAreNamedByWhatTookTheTime)
{
    auto summary = [](uint64 placementMs, uint64 routeMs, uint64 resetMs, uint32 count = 40)
    {
        ResetSamples samples;
        for (uint32 i = 0; i < count; ++i)
            samples.Add(Reset(placementMs, routeMs, resetMs));
        return samples.Summarise();
    };
    // M5: the route is the reset.
    EXPECT_EQ(Stall(summary(300, 280, 320), 25.0), StallCause::Routes);
    // M3: a ledge search with no route.
    EXPECT_EQ(Stall(summary(150, 0, 170), 25.0), StallCause::Placement);
    // Characters and kit.
    EXPECT_EQ(Stall(summary(5, 0, 90), 25.0), StallCause::Reset);
    // Under a decision, under the floor, or too few resets to tell: no stall.
    EXPECT_EQ(Stall(summary(300, 280, 320), 400.0), StallCause::None);
    EXPECT_EQ(Stall(summary(15, 10, 19), 5.0), StallCause::None);
    EXPECT_EQ(Stall(summary(300, 280, 320, STALL_MIN_RESETS - 1), 25.0), StallCause::None);
}
