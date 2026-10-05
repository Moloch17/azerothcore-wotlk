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

#include "Client.h"
#include "FlagRules.h"
#include "ReportCadence.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace Mv = Animus::Movement;
namespace Cd = Animus::Movement::Cadence;
namespace Co = Animus::Client;

namespace
{
    /// Flat ground at 0, or a ledge `High` yards up wherever x < LedgeX.
    class Flat final : public Mv::WorldQuery
    {
    public:
        float LedgeX = -1e9f;
        float High = 0.0f;

        [[nodiscard]] float Ground(float x) const { return x < LedgeX ? High : 0.0f; }
        [[nodiscard]] float FloorBelow(float x, float /*y*/, float z, float search) const override
        {
            float const ground = Ground(x);
            return ground <= z + 1e-3f && z - ground <= search ? ground : Mv::INVALID_FLOOR;
        }
        [[nodiscard]] float FloorNormalZ(float, float, float) const override { return 1.0f; }
        [[nodiscard]] Mv::Liquid LiquidAt(float, float, float) const override { return {}; }
        [[nodiscard]] float Sweep(float, float, float, float, float, float, Mv::Body const&) const override
        {
            return 1.0f;
        }
        [[nodiscard]] float Ceiling(float, float, float, float up) const override { return up; }
        [[nodiscard]] bool InTerrain(float x, float, float z) const override { return z < Ground(x) - 0.5f; }
    };

    /// The server: holds what was last accepted, and mirrors the two core functions fall damage is made of --
    /// Player::HandleFall (on MSG_MOVE_FALL_LAND, before relocation) and Player::UpdateFallInformationIfNeed.
    class FakeLink final : public Mv::ServerLink
    {
    public:
        std::vector<Mv::Report> Reports;
        bool Refuse = false;
        bool Imposed = false;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Yaw = 0.0f;
        float LastFallZ = 0.0f;
        uint32_t LastFallTime = 0;
        float FallDamage = -1.0f;      // share of maximum health, at the last landing

        bool Apply(Mv::Report const& report) override
        {
            Reports.push_back(report);
            if (Refuse)
                return false;
            if (report.Opcode == Cd::Op::FALL_LAND)
            {
                float const fell = LastFallZ - report.Z;
                FallDamage = fell >= 13.48f ? 0.018f * fell - 0.2426f : 0.0f;     // Player.cpp:14189-14191
            }
            X = report.X;
            Y = report.Y;
            Z = report.Z;
            Yaw = report.Yaw;
            if (LastFallTime >= report.FallMs || LastFallZ <= report.Z || report.Opcode == Cd::Op::FALL_LAND)
            {
                LastFallTime = report.FallMs;
                LastFallZ = report.Z;
            }
            return true;
        }

        [[nodiscard]] Mv::ServerState State() const override
        {
            Mv::ServerState state;
            state.Imposed = Imposed;
            state.X = X;
            state.Y = Y;
            state.Z = Z;
            state.Yaw = Yaw;
            return state;
        }

        [[nodiscard]] size_t Count(uint16_t opcode) const
        {
            return size_t(std::count_if(Reports.begin(), Reports.end(),
                [opcode](Mv::Report const& report) { return report.Opcode == opcode; }));
        }
    };

    struct Rig
    {
        Flat World;
        FakeLink Link;
        Mv::Body Shape;
        Mv::Speeds Speeds;
        Mv::ControlState Control;
        Mv::Client Client;
        uint32_t Now = 1000;

        void Start(float x = 0.0f, float z = 0.0f, float yaw = 0.0f)
        {
            Link.X = x;
            Link.Z = z;
            Link.LastFallZ = z;
            Link.Yaw = yaw;
            Client.Start(Link, Shape, World, Now);
        }

        void Run(uint32_t ms, uint32_t tick = 50)
        {
            for (uint32_t done = 0; done < ms; done += tick)
            {
                Now += tick;
                Client.Tick(Control, Speeds, Shape, World, tick, Now, Link);
            }
        }
    };

    float Turned(float a, float b)
    {
        return std::fabs(std::remainder(a - b, 6.2831853f));
    }
}

// §5A.1 points 1 and 2: the client knows where it is; the server, and everyone reading it, has the last report.
TEST(ClientTest, TheBodyIsTheTruthTheServerHasTheLastReport)
{
    Rig rig;
    rig.Start();
    rig.Control.Forward = 1;
    rig.Run(400);
    EXPECT_NEAR(rig.Client.Body.X, 2.8f, 0.05f);
    EXPECT_FLOAT_EQ(rig.Link.X, 0.0f);                              // START_FORWARD at the press, at the start
    EXPECT_EQ(rig.Link.Count(Cd::Op::START_FORWARD), 1u);
    EXPECT_EQ(rig.Link.Count(Cd::Op::HEARTBEAT), 1u);               // only Start's
}

// Point 3: what is credited is what the server accepted: the heartbeat 500 ms after the press.
TEST(ClientTest, TheServerHasWhatWasReported)
{
    Rig rig;
    rig.Start();
    rig.Control.Forward = 1;
    rig.Run(600);
    EXPECT_EQ(rig.Link.Count(Cd::Op::HEARTBEAT), 2u);
    EXPECT_NEAR(rig.Link.X, 3.5f, 0.05f);
    EXPECT_EQ(rig.Link.Reports.back().TimeMs, 1500u);
}

// Point 4: an episode's start reports at once, and its end forces a last report, so an arrival 400 ms after the last
// heartbeat is credited rather than lost.
TEST(ClientTest, StartReportsAtOnceAndFinishCreditsTheLastStretch)
{
    Rig rig;
    rig.Start(5.0f);
    ASSERT_EQ(rig.Link.Reports.size(), 1u);
    EXPECT_EQ(rig.Link.Reports[0].Opcode, Cd::Op::HEARTBEAT);
    EXPECT_FLOAT_EQ(rig.Client.Body.X, 5.0f);
    rig.Control.Forward = 1;
    rig.Run(900);
    EXPECT_NEAR(rig.Link.X, 5.0f + 3.5f, 0.05f);                    // the heartbeat at 500 ms
    rig.Client.Finish(rig.Link, rig.Now);
    EXPECT_NEAR(rig.Link.X, 5.0f + 6.3f, 0.05f);
    EXPECT_FLOAT_EQ(rig.Link.X, rig.Client.Body.X);
}

// Point 5: what the server imposes is read every tick; the client yields at once to the server's position (here the
// stale report's, mid-heartbeat), sends nothing while it lasts, and starts again with the keys it still holds.
TEST(ClientTest, ImposedMotionStopsTheBodyAtTheServersPosition)
{
    Rig rig;
    rig.Start();
    rig.Control.Forward = 1;
    rig.Run(250);
    size_t const sent = rig.Link.Reports.size();
    rig.Link.Imposed = true;
    rig.Run(500);
    EXPECT_FLOAT_EQ(rig.Client.Body.X, 0.0f);
    EXPECT_EQ(rig.Link.Reports.size(), sent);
    rig.Link.Imposed = false;
    rig.Run(50);
    EXPECT_EQ(rig.Link.Count(Cd::Op::START_FORWARD), 2u);
    EXPECT_GT(rig.Client.Body.X, 0.0f);
}

// A root ordered mid-run (SMSG_FORCE_MOVE_ROOT) stops the body within the tick, is acknowledged with ROOT and no moving
// flag, and no move is reported across it; the unroot is acknowledged and the held key goes on.
TEST(ClientTest, ARootStopsTheBodyAndNoMoveCrossesIt)
{
    Rig rig;
    rig.Start();
    rig.Control.Forward = 1;
    rig.Run(200);
    Co::Order root;
    root.Kind = Co::OrderKind::Root;
    root.Counter = 7;
    rig.Client.Order(root, rig.Link, rig.Shape, rig.World, rig.Now);
    Mv::Report const ack = rig.Link.Reports.back();
    EXPECT_EQ(ack.Opcode, Co::Op::CMSG_FORCE_MOVE_ROOT_ACK);
    EXPECT_EQ(ack.Counter, 7u);
    EXPECT_TRUE(ack.Flags & Mv::Flag::ROOT);
    EXPECT_FALSE(ack.Flags & Mv::Client::MOVING_MASK);
    size_t const sent = rig.Link.Reports.size();
    float const at = rig.Client.Body.X;
    rig.Run(1000);
    EXPECT_EQ(rig.Link.Reports.size(), sent);
    EXPECT_FLOAT_EQ(rig.Client.Body.X, rig.Link.X);
    EXPECT_LE(rig.Client.Body.X, at);
    root.Kind = Co::OrderKind::Unroot;
    rig.Client.Order(root, rig.Link, rig.Shape, rig.World, rig.Now);
    EXPECT_EQ(rig.Link.Reports.back().Opcode, Co::Op::CMSG_FORCE_MOVE_UNROOT_ACK);
    rig.Run(50);
    EXPECT_EQ(rig.Link.Count(Cd::Op::START_FORWARD), 2u);
}

// Point 6, the mouse-look (overseer ruling): a held turn rate reports SET_FACING at each 0.1 rad from the last packet's
// facing, at its exact moment -- every 63.7 ms at 90 degrees a second, 191 ms at 30 -- and never START/STOP_TURN.
TEST(ClientTest, AMouseTurnReportsItsFacingEveryTenthOfARadian)
{
    for (float const degrees : { 90.0f, 30.0f })
    {
        Rig rig;
        rig.Start(0.0f, 0.0f, 1.0f);
        rig.Control.TurnRate = degrees * 0.017453292f;
        rig.Run(2000, 250);
        std::vector<uint32_t> times;
        for (Mv::Report const& report : rig.Link.Reports)
            if (report.Opcode == Cd::Op::SET_FACING)
                times.push_back(report.TimeMs);
        float const interval = 100.0f / (degrees * 0.017453292f);    // ms
        ASSERT_GE(times.size(), 5u) << degrees;
        EXPECT_EQ(times.size(), size_t(2000.0f / interval)) << degrees;
        for (size_t i = 1; i < times.size(); ++i)
            EXPECT_NEAR(float(times[i] - times[i - 1]), interval, 1.5f) << degrees;
        EXPECT_EQ(rig.Link.Count(Cd::Op::START_TURN_LEFT), 0u);
        EXPECT_EQ(rig.Link.Count(Cd::Op::STOP_TURN), 0u);
    }
}

// The overseer's replacement for the pre-cast report: at any tick boundary -- a cast's moment -- the server's facing is
// within 0.1 rad of the body's yaw, even at 360 degrees a second and a 250 ms tick, because the crossings are found
// inside the step, not at its end.
TEST(ClientTest, TheServersFacingIsAlwaysWithinATenthOfTheBody)
{
    Rig rig;
    rig.Start();
    rig.Control.TurnRate = -6.2831853f;
    for (int tick = 0; tick < 12; ++tick)
    {
        rig.Run(250, 250);
        EXPECT_LE(Turned(rig.Link.Yaw, rig.Client.Body.Yaw), 0.1f + 1e-4f) << tick;
    }
    rig.Control.TurnRate = 0.0f;
    rig.Run(250, 250);
    EXPECT_LE(Turned(rig.Link.Yaw, rig.Client.Body.Yaw), 0.1f + 1e-4f);
}

// Turning past 0 / 2pi is a report at once: the client's raw, unwrapped difference jumps.
TEST(ClientTest, TurningPastNorthSendsAtOnce)
{
    Cd::Crossing const wrap = Cd::NextFacingCrossing(6.25f, 0.1f, 6.25f, 2.0f);
    EXPECT_NEAR(wrap.At, (6.2831853f - 6.25f) / 0.1f, 1e-3f);
    EXPECT_NEAR(wrap.Value, 0.0f, 1e-6f);
    Cd::Crossing const back = Cd::NextFacingCrossing(0.02f, -0.1f, 0.02f, 2.0f);
    EXPECT_NEAR(back.At, 0.2f, 1e-3f);
    EXPECT_GT(back.Value, 6.28f);
    EXPECT_LT(Cd::NextFacingCrossing(1.0f, 0.1f, 1.0f, 0.5f).At, 0.0f);   // 0.05 rad in the horizon: nothing
    // Pitch: no wrap, and none past the limit.
    EXPECT_NEAR(Cd::NextPitchCrossing(0.0f, 0.5f, 0.0f, 1.0f, 1.5f).At, 0.2f, 1e-4f);
    EXPECT_LT(Cd::NextPitchCrossing(1.45f, 0.5f, 1.45f, 1.0f, 1.5f).At, 0.0f);
}

// A pitch rate on the ground reports nothing (SET_PITCH only swimming or flying, and on the ground it does nothing).
TEST(ClientTest, NoPitchReportsOnTheGround)
{
    Rig rig;
    rig.Start();
    rig.Control.PitchRate = 1.0f;
    rig.Run(1000);
    EXPECT_EQ(rig.Link.Count(Cd::Op::SET_PITCH), 0u);
}

// A refused report: the body is taken back to where the server holds it, and the refusal counted.
TEST(ClientTest, ARefusedReportPutsTheBodyBackWhereTheServerHasIt)
{
    Rig rig;
    rig.Start();
    rig.Run(100);
    rig.Link.Refuse = true;
    rig.Control.Forward = 1;
    rig.Run(50);
    EXPECT_EQ(rig.Client.Counts.Refused, 1u);
    EXPECT_FLOAT_EQ(rig.Client.Body.X, rig.Link.X);
}

// A jump is MSG_MOVE_JUMP at the press: FALLING, the fall's clock at 0, the launch in the jump info (down positive).
TEST(ClientTest, AJumpIsReportedAtThePress)
{
    Rig rig;
    rig.Start();
    rig.Control.Jump = true;
    rig.Run(50);
    ASSERT_EQ(rig.Link.Count(Cd::Op::JUMP), 1u);
    Mv::Report const& jump = rig.Link.Reports[1];
    EXPECT_EQ(jump.Opcode, Cd::Op::JUMP);
    EXPECT_TRUE(jump.Flags & Mv::Flag::FALLING);
    EXPECT_EQ(jump.FallMs, 0u);
    EXPECT_NEAR(jump.JumpZSpeed, -Mv::JUMP_SPEED, 1e-6f);
    rig.Run(1000);
    EXPECT_EQ(rig.Link.Count(Cd::Op::FALL_LAND), 1u);
    EXPECT_FLOAT_EQ(rig.Link.FallDamage, 0.0f);
}

// The fall-damage worked number: running off a 30 yd ledge, the reports keep the core's fall top at the ledge (every
// falling packet carries a growing fall time, every ground packet 0), and the landing costs the core's
// 0.018 * 30 - 0.2426 = 29.74% of maximum health (Player::HandleFall; no damage under 13.48 yd).
TEST(ClientTest, ThirtyYardsOffALedgeCostsTheCoresShare)
{
    Rig rig;
    rig.World.LedgeX = 1.0f;
    rig.World.High = 30.0f;
    rig.Start(0.0f, 30.0f);
    rig.Control.Forward = 1;
    rig.Run(3000);
    ASSERT_EQ(rig.Link.Count(Cd::Op::FALL_LAND), 1u);
    EXPECT_NEAR(rig.Link.FallDamage, 0.018f * 30.0f - 0.2426f, 1e-3f);
    EXPECT_NEAR(rig.Client.Body.Z, 0.0f, 1e-3f);
    uint32_t fallen = 0;
    for (Mv::Report const& report : rig.Link.Reports)
    {
        if (!(report.Flags & Mv::Flag::FALLING) && report.Opcode != Cd::Op::FALL_LAND)
            EXPECT_EQ(report.FallMs, 0u);
        else
        {
            EXPECT_GE(report.FallMs, fallen);
            fallen = report.FallMs;
        }
    }
}

// A knockback order (SMSG_MOVE_KNOCK_BACK) is launched as a fall with its speeds and acknowledged once, with the
// MovementInfo after the launch; the body flies the ballistic arc (apex vz^2 / 2g) and lands.
TEST(ClientTest, AKnockbackFliesItsArcAndIsAcknowledgedOnce)
{
    Rig rig;
    rig.Start();
    Co::Order knock;
    knock.Kind = Co::OrderKind::Knockback;
    knock.Cos = 0.0f;
    knock.Sin = 1.0f;
    knock.SpeedXY = 4.0f;
    knock.SpeedZ = -10.0f;                                          // as sent: the client's sign, negative up
    rig.Client.Order(knock, rig.Link, rig.Shape, rig.World, rig.Now);
    Mv::Report const ack = rig.Link.Reports.back();
    EXPECT_EQ(ack.Opcode, Co::Op::CMSG_MOVE_KNOCK_BACK_ACK);
    EXPECT_TRUE(ack.Flags & Mv::Flag::FALLING);
    EXPECT_FLOAT_EQ(ack.JumpZSpeed, -10.0f);
    float apex = 0.0f;
    for (int tick = 0; tick < 200 && rig.Link.Count(Cd::Op::FALL_LAND) == 0; ++tick)
    {
        rig.Run(10, 10);
        apex = std::max(apex, rig.Client.Body.Z);
    }
    EXPECT_NEAR(apex, 100.0f / (2.0f * Mv::GRAVITY), 0.02f);
    EXPECT_EQ(rig.Link.Count(Cd::Op::FALL_LAND), 1u);
    EXPECT_NEAR(rig.Client.Body.Y, 4.0f * 2.0f * 10.0f / Mv::GRAVITY, 0.1f);
    EXPECT_EQ(rig.Link.Count(Co::Op::CMSG_MOVE_KNOCK_BACK_ACK), 1u);
}

// The granted flags: a feather-fall order is acknowledged and then carried (FALLING_SLOW only under it), as are
// flying, water walking and hover.
TEST(ClientTest, GrantedFlagsAreAcknowledgedAndCarried)
{
    Rig rig;
    rig.Start();
    Co::Order order;
    order.Kind = Co::OrderKind::FeatherFall;
    rig.Client.Order(order, rig.Link, rig.Shape, rig.World, rig.Now);
    EXPECT_EQ(rig.Link.Reports.back().Opcode, Co::Op::CMSG_MOVE_FEATHER_FALL_ACK);
    order.Kind = Co::OrderKind::CanFly;
    rig.Client.Order(order, rig.Link, rig.Shape, rig.World, rig.Now);
    rig.Control.Forward = 1;
    rig.Run(50);
    EXPECT_EQ(rig.Link.Reports.back().Flags & Mv::Client::GRANTED_MASK, Mv::Flag::FALLING_SLOW | Mv::Flag::CAN_FLY);
    order.Kind = Co::OrderKind::NormalFall;
    rig.Client.Order(order, rig.Link, rig.Shape, rig.World, rig.Now);
    EXPECT_EQ(rig.Link.Reports.back().Flags & Mv::Flag::FALLING_SLOW, 0u);
    // Physics slow fall without the order (a hover aura, say) does not report FALLING_SLOW.
    rig.Speeds.SlowFall = true;
    EXPECT_EQ(rig.Client.FlagsOf(rig.Control, rig.Speeds) & Mv::Flag::FALLING_SLOW, 0u);
}

// Orders are pushed from any thread and drained by the seat's tick in the order they were sent (the shared inbox).
TEST(ClientTest, OrdersQueueAcrossThreadsInOrder)
{
    Co::Inbox inbox;
    constexpr uint32_t PER = 2000;
    std::vector<std::thread> pushers;
    for (uint8_t thread = 0; thread < 4; ++thread)
        pushers.emplace_back([&inbox, thread]
        {
            for (uint32_t i = 0; i < PER; ++i)
            {
                Co::Order order;
                order.Speed = Co::SpeedType(thread);
                order.Counter = i;
                inbox.Push(order);
            }
        });
    std::vector<Co::Order> drained;
    std::vector<Co::Order> batch;
    while (drained.size() < 4 * PER)
    {
        inbox.Drain(batch);
        drained.insert(drained.end(), batch.begin(), batch.end());
    }
    for (std::thread& pusher : pushers)
        pusher.join();
    inbox.Drain(batch);
    EXPECT_TRUE(batch.empty());
    ASSERT_EQ(drained.size(), size_t(4 * PER));
    uint32_t next[4] = {};
    for (Co::Order const& order : drained)
        EXPECT_EQ(order.Counter, next[uint8_t(order.Speed)]++);
}

// A Blink mid-run (the overseer's M1 case): a spell's teleport is the server's, imposed while it is under way (the
// near-teleport semaphore) -- the body takes the server's landing point, no report crosses it, the teleport is
// answered, and the run goes on from the landing with the key still held.
TEST(ClientTest, ABlinkMidRunResyncsToTheLandingAndNoReportCrossesIt)
{
    Rig rig;
    rig.Start();
    rig.Control.Forward = 1;
    rig.Run(300);
    size_t const sent = rig.Link.Reports.size();
    rig.Link.Imposed = true;                                        // IsBeingTeleportedNear
    rig.Link.X = 20.0f;                                             // where the Blink put it
    rig.Run(50);
    EXPECT_FLOAT_EQ(rig.Client.Body.X, 20.0f);
    EXPECT_EQ(rig.Link.Reports.size(), sent);
    Co::Order teleport;
    teleport.Kind = Co::OrderKind::Teleport;
    rig.Client.Order(teleport, rig.Link, rig.Shape, rig.World, rig.Now);
    EXPECT_EQ(rig.Link.Reports.back().Opcode, Co::Op::MSG_MOVE_TELEPORT_ACK);
    EXPECT_FLOAT_EQ(rig.Link.Reports.back().X, 20.0f);
    rig.Link.Imposed = false;
    rig.Run(50);
    EXPECT_EQ(rig.Link.Reports.back().Opcode, Cd::Op::START_FORWARD);
    EXPECT_NEAR(rig.Link.Reports.back().X, 20.0f, 0.01f);
    for (size_t i = sent; i < rig.Link.Reports.size(); ++i)
        EXPECT_GE(rig.Link.Reports[i].X, 20.0f - 1e-3f);
}

// A near teleport (MSG_MOVE_TELEPORT_ACK to the client) puts the body where the server has it and is answered once.
TEST(ClientTest, ANearTeleportIsTakenAndAnswered)
{
    Rig rig;
    rig.Start();
    rig.Link.X = 40.0f;
    Co::Order teleport;
    teleport.Kind = Co::OrderKind::Teleport;
    rig.Client.Order(teleport, rig.Link, rig.Shape, rig.World, rig.Now);
    EXPECT_FLOAT_EQ(rig.Client.Body.X, 40.0f);
    EXPECT_EQ(rig.Link.Count(Co::Op::MSG_MOVE_TELEPORT_ACK), 1u);
}

// Strip parity (overseer R1 review): the same rules strip a real client's packet (ReadMovementInfo) and a controller
// report (PlayerLink), in ReadMovementInfo's order. Every ack the shared rule gives (FlagsAfter, R1's oracle cases)
// keeps all its flags under the auras the order came with, except ROOT -- which no client report keeps -- and the
// FALLING of a knocked-back flyer, which the server takes off (it flies on).
TEST(ClientTest, TheServerStripsTheSameFlagsFromEveryReport)
{
    Mv::FlagFacts all;
    all.HoverAura = all.WaterWalkAura = all.FeatherFallAura = all.FlyAura = true;
    uint32_t const states[] = { 0u, 0x1u, 0x1u | 0x8u | 0x10u, 0x00200000u | 0x1u | 0x40u, 0x00001000u,
        0x01000000u | 0x02000000u | 0x00400000u };
    for (int kind = int(Co::OrderKind::Root); kind <= int(Co::OrderKind::Knockback); ++kind)
    {
        Co::Order order;
        order.Kind = Co::OrderKind(kind);
        for (uint32_t state : states)
        {
            Mv::FlagFacts facts = all;
            if (order.Kind == Co::OrderKind::UnsetCanFly)
                facts.FlyAura = false;
            if (order.Kind == Co::OrderKind::LandWalk)
                facts.WaterWalkAura = false;
            if (order.Kind == Co::OrderKind::NormalFall)
                facts.FeatherFallAura = false;
            if (order.Kind == Co::OrderKind::UnsetHover)
                facts.HoverAura = false;
            if ((state & (Mv::FlagBit::CAN_FLY | Mv::FlagBit::FLYING)) && !facts.FlyAura)
                continue;
            uint32_t const after = Co::FlagsAfter(order, state);
            uint32_t const kept = Mv::SanitizeFlags(after, facts);
            if (order.Kind == Co::OrderKind::Knockback && (after & Mv::FlagBit::CAN_FLY))
                EXPECT_EQ(kept, after & ~(Mv::FlagBit::ROOT | Mv::FlagBit::FALLING)) << kind << " " << state;
            else
                EXPECT_EQ(kept, after & ~Mv::FlagBit::ROOT) << kind << " " << state;
        }
    }
    // The rules run in order, each on what the last left: without the fly aura CAN_FLY goes first, so a falling
    // report keeps FALLING; with it, FALLING goes.
    uint32_t const flyingFall = Mv::FlagBit::CAN_FLY | Mv::FlagBit::FALLING;
    EXPECT_EQ(Mv::SanitizeFlags(flyingFall, {}), Mv::FlagBit::FALLING);
    EXPECT_EQ(Mv::SanitizeFlags(flyingFall, all), Mv::FlagBit::CAN_FLY);
    // Feather fall without the aura, contradictory keys, a GM's flight.
    EXPECT_EQ(Mv::SanitizeFlags(Mv::FlagBit::FALLING_SLOW | Mv::FlagBit::FALLING, {}), Mv::FlagBit::FALLING);
    EXPECT_EQ(Mv::SanitizeFlags(Mv::FlagBit::FORWARD | Mv::FlagBit::BACKWARD | Mv::FlagBit::LEFT, {}),
        Mv::FlagBit::LEFT);
    Mv::FlagFacts gm;
    gm.Privileged = true;
    EXPECT_EQ(Mv::SanitizeFlags(Mv::FlagBit::FLYING, gm), Mv::FlagBit::FLYING);
}
