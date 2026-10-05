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

#include "Capture.h"
#include "Client.h"
#include "Replay.h"
#include "ReportCadence.h"
#include "gtest/gtest.h"
#include <cmath>
#include <cstdio>
#include <unistd.h>

namespace Mv = Animus::Movement;
namespace Cap = Animus::Movement::Capture;
namespace Cd = Animus::Movement::Cadence;
namespace Rp = Animus::Movement::Replay;

namespace
{
    /// Flat ground at 0, with a 0.8 yd step up at x = 12 and a 6 yd drop at y = 30 (a ledge to walk off).
    class Course final : public Mv::WorldQuery
    {
    public:
        [[nodiscard]] float Ground(float x, float y) const
        {
            if (y > 30.0f)
                return -6.0f;
            return x > 12.0f ? 0.8f : 0.0f;
        }
        [[nodiscard]] float FloorBelow(float x, float y, float z, float search) const override
        {
            float const ground = Ground(x, y);
            return ground <= z + 1e-3f && z - ground <= search ? ground : Mv::INVALID_FLOOR;
        }
        [[nodiscard]] float FloorNormalZ(float, float, float) const override { return 1.0f; }
        [[nodiscard]] Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float, float, float, float up) const override { return up; }
        [[nodiscard]] bool InTerrain(float x, float y, float z) const override { return z < Ground(x, y) - 0.5f; }
    };

    /// A server that takes every report, as the capture would see it.
    class Recorder final : public Mv::ServerLink
    {
    public:
        std::vector<Mv::Report> Reports;
        Mv::ServerState Held;

        bool Apply(Mv::Report const& report) override
        {
            Reports.push_back(report);
            Held.X = report.X;
            Held.Y = report.Y;
            Held.Z = report.Z;
            Held.Yaw = report.Yaw;
            return true;
        }
        [[nodiscard]] Mv::ServerState State() const override { return Held; }
    };

    /// The controller driven through a little course -- run, a keyboard turn on the move, strafe, the step up, a
    /// jump, back, walk, off the ledge -- at a 50 ms tick, recorded as a capture would record a client.
    Cap::Recording RecordTheController(Course const& world, Mv::Speeds const& speeds)
    {
        Recorder link;
        Mv::Body const shape;
        Mv::Client client;
        uint32_t now = 1000;
        client.Start(link, shape, world, now);
        Mv::ControlState control;
        auto run = [&](uint32_t ms)
        {
            for (uint32_t done = 0; done < ms; done += 50)
            {
                now += 50;
                client.Tick(control, speeds, shape, world, 50, now, link);
            }
        };
        control.Forward = 1;
        run(1200);
        control.KeyboardTurn = true;
        control.TurnRate = speeds.TurnRate;             // the turn key, at the unit's rate
        run(450);
        control.TurnRate = 0.0f;
        control.KeyboardTurn = false;
        control.Strafe = 1;
        run(700);
        control.Strafe = 0;
        control.Jump = true;
        run(1500);
        control.Forward = -1;
        run(800);
        control.Forward = 1;
        control.Walk = true;
        run(600);
        control.Walk = false;
        control.KeyboardTurn = true;
        control.TurnRate = -speeds.TurnRate;
        run(500);
        control.KeyboardTurn = false;
        control.TurnRate = 0.0f;
        run(6000);                                      // on to the ledge and off it
        control.Forward = 0;
        run(1500);

        Cap::Recording recording;
        recording.Speeds.push_back(Cap::FromSpeeds(speeds, 1000, 7));
        for (Mv::Report const& report : link.Reports)
            recording.Moves.push_back(Cap::FromReport(report, 7, 0));
        return recording;
    }
}

// The capture format is lossless: a recording encoded and decoded, and written to a gzip file and read back, is the
// same records, every field.
TEST(ReplayTest, ACaptureRoundTripsEveryField)
{
    Course const world;
    Mv::Speeds const speeds;
    Cap::Recording const recording = RecordTheController(world, speeds);
    ASSERT_GT(recording.Moves.size(), 20u);
    char path[] = "/tmp/replaytest-XXXXXX";
    int const fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    std::string error;
    ASSERT_TRUE(Cap::WriteFile(path, recording, 1000, error)) << error;
    Cap::Recording read;
    ASSERT_TRUE(Cap::ReadFile(path, read, error)) << error;
    std::remove(path);
    ASSERT_EQ(read.Moves.size(), recording.Moves.size());
    ASSERT_EQ(read.Speeds.size(), 1u);
    EXPECT_EQ(read.Speeds[0].TurnRate, speeds.TurnRate);
    for (std::size_t i = 0; i < read.Moves.size(); ++i)
    {
        Cap::MoveRecord const& a = recording.Moves[i];
        Cap::MoveRecord const& b = read.Moves[i];
        EXPECT_EQ(a.ClientMs, b.ClientMs);
        EXPECT_EQ(a.Opcode, b.Opcode);
        EXPECT_EQ(a.Flags, b.Flags);
        EXPECT_EQ(a.X, b.X);
        EXPECT_EQ(a.Y, b.Y);
        EXPECT_EQ(a.Z, b.Z);
        EXPECT_EQ(a.O, b.O);
        EXPECT_EQ(a.FallMs, b.FallMs);
        EXPECT_EQ(a.JumpZSpeed, b.JumpZSpeed);
    }
    // Not a capture: refused.
    Cap::Recording none;
    uint8_t const junk[] = { 1, 0, 2, 0, 0, 0 };
    EXPECT_FALSE(Cap::Decode(junk, sizeof(junk), none, error));
}

// The self-test (C6, no human needed): the controller recorded and replayed through itself drifts exactly 0 at every
// packet -- record and replay are lossless and deterministic -- and the report reads the course's own constants back.
TEST(ReplayTest, TheControllerReplaysItselfWithNoDrift)
{
    Course const world;
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Cap::Recording const recording = RecordTheController(world, speeds);

    std::vector<float> const drift = Rp::Follow(recording.Moves, 0, 60000, speeds, shape, world, 60000);
    ASSERT_EQ(drift.size(), recording.Moves.size());
    for (std::size_t i = 0; i < drift.size(); ++i)
        EXPECT_EQ(drift[i], 0.0f) << "packet " << i << " opcode 0x" << std::hex << recording.Moves[i].Opcode;

    Rp::Options options;
    options.SegmentEveryMs = 1000;
    options.GapMs = 60000;
    Rp::Report const report = Rp::Run(recording.Moves, recording.Speeds, shape, world, options);
    ASSERT_EQ(report.Drift.size(), 4u);
    for (Rp::Horizon const& horizon : report.Drift)
    {
        EXPECT_GT(horizon.All.Count, 0u) << horizon.Ms;
        EXPECT_EQ(horizon.All.Max, 0.0f) << horizon.Ms;
    }
    EXPECT_EQ(report.Jumps, 1u);
    EXPECT_EQ(report.JumpLanding.Max, 0.0f);
    EXPECT_GE(report.StepUps, 1u);
    EXPECT_EQ(report.StepUpsRefused, 0u);
    EXPECT_EQ(report.WallDisagreements, 0u);
    for (Rp::Measure const& m : report.Calibration)
    {
        if (m.Name.rfind("JUMP_SPEED", 0) == 0)
            EXPECT_FLOAT_EQ(m.Mean, Mv::JUMP_SPEED);
        if (m.Name.rfind("GRAVITY", 0) == 0)
            EXPECT_NEAR(m.Mean, Mv::GRAVITY, 0.05f);        // fall times are whole milliseconds
        if (m.Name == "HEARTBEAT_MS")
            EXPECT_FLOAT_EQ(m.Mean, 500.0f);
        if (m.Name == "KEYBOARD_TURN_WHILE_MOVING")
            EXPECT_NEAR(m.Mean, Mv::KEYBOARD_TURN_WHILE_MOVING, 0.01f);
    }
    EXPECT_NE(report.Text().find("calibration"), std::string::npos);
    std::printf("%s", report.Text().c_str());       // what `forge controller replay` prints, for this course
}

// The replay is not a copy: a recording whose packets were moved shows the move as drift.
TEST(ReplayTest, ADisplacedRecordingDrifts)
{
    Course const world;
    Mv::Speeds const speeds;
    Mv::Body const shape;
    Cap::Recording recording = RecordTheController(world, speeds);
    for (std::size_t i = recording.Moves.size() / 2; i < recording.Moves.size(); ++i)
        recording.Moves[i].X += 0.3f;
    std::vector<float> const drift = Rp::Follow(recording.Moves, 0, 60000, speeds, shape, world, 60000);
    EXPECT_NEAR(drift.back(), 0.3f, 1e-3f);
    EXPECT_EQ(drift.front(), 0.0f);
}
