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
#include "ReportCadence.h"
#include <algorithm>
#include <cmath>

namespace
{
    namespace Mv = Animus::Movement;
    namespace Cd = Animus::Movement::Cadence;

    float WrapYaw(float yaw)
    {
        yaw = std::fmod(yaw, Cd::TWO_PI);
        return yaw < 0.0f ? yaw + Cd::TWO_PI : yaw;
    }

    /// The signed turn from `from` to `to`, the short way.
    float TurnBetween(float from, float to)
    {
        return std::remainder(to - from, Cd::TWO_PI);
    }
}

uint32_t Mv::Client::FlagsOf(ControlState const& control, Speeds const& speeds) const
{
    uint32_t flags = (MovementFlags(Body, control, speeds) & ~GRANTED_MASK) | _granted;
    if (_rooted)
        flags = (flags & ~MOVING_MASK) | Flag::ROOT;
    return flags;
}

Mv::Report Mv::Client::Snapshot(uint16_t opcode, uint32_t timeMs, uint32_t flags) const
{
    Report report;
    report.Opcode = opcode;
    report.TimeMs = timeMs;
    report.X = Body.X;
    report.Y = Body.Y;
    report.Z = Body.Z;
    report.Yaw = Body.Yaw;
    report.Pitch = Body.Pitch;
    report.Flags = flags;
    // Every report during a fall carries the time fallen, and every other one 0: the core keeps the fall's top
    // (Player::UpdateFallInformationIfNeed) only while the fall time grows, and resets it on anything else.
    if (flags & Flag::FALLING)
    {
        report.FallMs = Body.FallMs;
        report.JumpZSpeed = _jumpZSpeed;
        report.JumpSin = _jumpSin;
        report.JumpCos = _jumpCos;
        report.JumpXYSpeed = _jumpXYSpeed;
    }
    return report;
}

void Mv::Client::TakeFromServer(ServerState const& state)
{
    if (_shape && _world)
        Resync(Body, state.X, state.Y, state.Z, state.Yaw, *_shape, *_world);
    _reportedX = state.X;
    _reportedY = state.Y;
    _reportedZ = state.Z;
    _reportedYaw = Body.Yaw;
    _reportedPitch = Body.Pitch;
}

bool Mv::Client::Send(Report const& report, ServerLink& link)
{
    ++Counts.Reports;
    _lastSendMs = report.TimeMs;
    if (!link.Apply(report))
    {
        // Refused. A real client never learns of it: it sent the packet and carries on at its cadence (the next change
        // or heartbeat). The body is taken back where the server holds it, so the two do not drift apart.
        ++Counts.Refused;
        TakeFromServer(link.State());
        _lastFlags = report.Flags;
        return false;
    }
    _reportedX = report.X;
    _reportedY = report.Y;
    _reportedZ = report.Z;
    _reportedYaw = report.Yaw;
    _reportedPitch = report.Pitch;
    _lastFlags = report.Flags;
    return true;
}

void Mv::Client::Start(ServerLink& link, Movement::Body const& shape, WorldQuery const& world, uint32_t nowMs)
{
    _shape = &shape;
    _world = &world;
    TakeFromServer(link.State());
    _started = true;
    uint32_t flags = _granted;
    if (_rooted)
        flags |= Flag::ROOT;
    if (Body.Kind == Mode::Swimming)
        flags |= Flag::SWIMMING;
    else if (Body.Kind == Mode::Flying)
        flags |= Flag::FLYING;
    else if (Body.Kind == Mode::Falling)
        flags |= Flag::FALLING;
    Send(Snapshot(Cd::Op::HEARTBEAT, nowMs, flags), link);
}

void Mv::Client::Order(Animus::Client::Order const& order, ServerLink& link, Movement::Body const& shape,
    WorldQuery const& world, uint32_t nowMs)
{
    namespace Co = Animus::Client;
    _shape = &shape;
    _world = &world;
    if (order.Kind == Co::OrderKind::TimeSync)
        return;
    if (order.Kind == Co::OrderKind::Teleport)
    {
        // The server has put the seat somewhere: take the body from there.
        TakeFromServer(link.State());
        ++Counts.Resyncs;
    }

    // The client takes the order, then acknowledges it with the flags it now has (the shared rule).
    uint32_t const flags = Co::FlagsAfter(order, _lastFlags);
    _granted = flags & GRANTED_MASK;
    _rooted = (flags & Flag::ROOT) != 0;
    switch (order.Kind)
    {
        case Co::OrderKind::Root:
            // It stops where it stands; a fall under way goes on straight down.
            Body.Vx = Body.Vy = 0.0f;
            break;
        case Co::OrderKind::Knockback:
            // Launched as SMSG_MOVE_KNOCK_BACK asks -- the horizontal speed along its direction, the vertical one
            // (sent in the client's sign, negative up) -- and acknowledged with the MovementInfo after the launch;
            // the controller flies the arc from here.
            Launch(Body, order.Cos * order.SpeedXY, order.Sin * order.SpeedXY, -order.SpeedZ);
            _jumpZSpeed = order.SpeedZ;
            _jumpCos = order.Cos;
            _jumpSin = order.Sin;
            _jumpXYSpeed = order.SpeedXY;
            break;
        default:
            break;
    }

    Report report = Snapshot(Co::AckOpcode(order), nowMs, flags);
    report.Counter = order.Counter;
    report.MoveType = uint8_t(order.Speed);
    report.Speed = order.Value;
    report.Applied = Co::Applies(order.Kind);
    ++Counts.Acks;
    Send(report, link);
}

void Mv::Client::Tick(ControlState& control, Speeds const& speeds, Movement::Body const& shape,
    WorldQuery const& world, uint32_t diffMs, uint32_t nowMs, ServerLink& link)
{
    _shape = &shape;
    _world = &world;
    TickMoved = TickCommanded = TickFallHeight = 0.0f;
    TickWall = false;
    TickJumps = TickLandings = 0;
    if (!_started || !diffMs)
        return;

    // What the server imposes, read every tick (§5A.1 point 5): the client stops and the body is the server's.
    ServerState const state = link.State();
    if (state.Imposed || _rooted)
    {
        TakeFromServer(state);
        control.Jump = false;
        _lastFlags = _granted | (_rooted ? Flag::ROOT : 0u);
        _lastSendMs = nowMs;
        ++Counts.YieldTicks;
        return;
    }
    // Moved by the server without an order the client saw (a sim teleport): start again from there.
    float const dx = state.X - _reportedX;
    float const dy = state.Y - _reportedY;
    float const dz = state.Z - _reportedZ;
    if (dx * dx + dy * dy + dz * dz > TELEPORT_YARDS * TELEPORT_YARDS)
    {
        ++Counts.Resyncs;
        Start(link, shape, world, nowMs >= diffMs ? nowMs - diffMs : 0u);
    }

    float const dt = float(diffMs) / 1000.0f;
    uint32_t const steps = std::max<uint32_t>(1, uint32_t(std::ceil(dt / MAX_SUBSTEP - 1e-4f)));
    float const h = dt / float(steps);
    for (uint32_t step = 0; step < steps; ++step)
    {
        // `nowMs` is the tick's end (the episode clock advances before the maps tick).
        uint32_t const startMs = (nowMs >= diffMs ? nowMs - diffMs : 0u)
            + uint32_t(std::lround(float(step) * h * 1000.0f));

        // A control changed: the client sends it at the key press, with the body as it is.
        uint32_t const before = FlagsOf(control, speeds);
        if ((before ^ _lastFlags) & CONTROL_MASK)
            for (uint16_t opcode : Cd::Changes(_lastFlags & CONTROL_MASK, before & CONTROL_MASK, false, false))
            {
                ++Counts.Changes;
                if (!Send(Snapshot(opcode, startMs, before), link))
                    return;
            }
        // A jump: MSG_MOVE_JUMP at the press, the fall's clock at 0 and its launch in the jump info.
        if (control.Jump && CanJump(Body, shape, world))
        {
            float const vz = Body.Kind == Mode::Swimming ? SWIM_JUMP_SPEED : JUMP_SPEED;
            float const xy = std::sqrt(Body.Vx * Body.Vx + Body.Vy * Body.Vy);
            _jumpZSpeed = -vz;
            _jumpXYSpeed = xy;
            _jumpCos = xy > 1e-4f ? Body.Vx / xy : std::cos(Body.Yaw);
            _jumpSin = xy > 1e-4f ? Body.Vy / xy : std::sin(Body.Yaw);
            Report jump = Snapshot(Cd::Op::JUMP, startMs, (before & ~(Flag::SWIMMING | Flag::FLYING)) | Flag::FALLING);
            jump.FallMs = 0;
            ++Counts.Changes;
            if (!Send(jump, link))
                return;
        }

        float const x0 = Body.X;
        float const y0 = Body.Y;
        float const z0 = Body.Z;
        float const yaw0 = Body.Yaw;
        float const pitch0 = Body.Pitch;
        uint32_t const fall0 = Body.FallMs;
        bool const steered = Body.Kind == Mode::Swimming || Body.Kind == Mode::Flying;
        float const yawRate = control.KeyboardTurn ? 0.0f : control.TurnRate;
        float const pitchRate = control.KeyboardTurn || !steered ? 0.0f : control.PitchRate;

        Step(Body, control, speeds, shape, world, h);
        uint32_t const after = FlagsOf(control, speeds);
        TickMoved += Body.Moved;
        TickCommanded += Body.Commanded;
        TickWall = TickWall || Body.AgainstWall;
        TickJumps += Body.Jumped ? 1 : 0;
        TickLandings += Body.Landed ? 1 : 0;
        TickFallHeight = std::max(TickFallHeight, Body.FallHeight);
        // Walked off an edge: a fall with no launch of its own, the run carried over it.
        if ((after & Flag::FALLING) && !(before & Flag::FALLING) && !Body.Jumped)
        {
            float const xy = std::sqrt(Body.Vx * Body.Vx + Body.Vy * Body.Vy);
            _jumpZSpeed = 0.0f;
            _jumpXYSpeed = xy;
            _jumpCos = xy > 1e-4f ? Body.Vx / xy : std::cos(Body.Yaw);
            _jumpSin = xy > 1e-4f ? Body.Vy / xy : std::sin(Body.Yaw);
        }

        // Inside the step, at their exact moments: the mouse-look's facing and pitch reports and the heartbeat, with
        // the body where it was then (the step is a straight line at a steady turn).
        auto at = [&](float t, uint16_t opcode)
        {
            float const share = h > 0.0f ? std::clamp(t / h, 0.0f, 1.0f) : 1.0f;
            Report report = Snapshot(opcode, startMs + uint32_t(std::lround(t * 1000.0f)), after);
            report.X = x0 + (Body.X - x0) * share;
            report.Y = y0 + (Body.Y - y0) * share;
            report.Z = z0 + (Body.Z - z0) * share;
            report.Yaw = WrapYaw(yaw0 + TurnBetween(yaw0, Body.Yaw) * share);
            report.Pitch = pitch0 + (Body.Pitch - pitch0) * share;
            if (after & Flag::FALLING)
            {
                uint32_t const before = uint32_t(std::lround((h - t) * 1000.0f));
                report.FallMs = Body.FallMs > before ? Body.FallMs - before : 0u;
            }
            return report;
        };
        float done = 0.0f;
        for (uint32_t guard = 0; guard < 256 && done <= h; ++guard)
        {
            float const yawNow = WrapYaw(yaw0 + yawRate * done);
            float const pitchNow = std::clamp(pitch0 + pitchRate * done, -PITCH_LIMIT, PITCH_LIMIT);
            Cd::Crossing const facing = yawRate != 0.0f
                ? Cd::NextFacingCrossing(yawNow, yawRate, _reportedYaw, h - done) : Cd::Crossing();
            Cd::Crossing const pitch = pitchRate != 0.0f
                ? Cd::NextPitchCrossing(pitchNow, pitchRate, _reportedPitch, h - done, PITCH_LIMIT) : Cd::Crossing();
            float heartbeat = -1.0f;
            if (after & Cd::HEARTBEAT_FLAGS)
            {
                float const due = (float(_lastSendMs) + float(Cd::HEARTBEAT_MS) - float(startMs)) / 1000.0f;
                if (due <= h)
                    heartbeat = std::max(due, done) - done;
            }

            float next = -1.0f;
            uint16_t opcode = 0;
            auto consider = [&](float when, uint16_t op)
            {
                if (when >= 0.0f && (next < 0.0f || when < next))
                {
                    next = when;
                    opcode = op;
                }
            };
            consider(facing.At, Cd::Op::SET_FACING);
            consider(pitch.At, Cd::Op::SET_PITCH);
            consider(heartbeat, Cd::Op::HEARTBEAT);
            if (next < 0.0f)
                break;

            float const t = done + next;
            Report report = at(t, opcode);
            if (opcode == Cd::Op::SET_FACING)
                report.Yaw = facing.Value;
            if (opcode == Cd::Op::SET_PITCH)
                report.Pitch = pitch.Value;
            if (opcode == Cd::Op::HEARTBEAT)
                ++Counts.Heartbeats;
            else
                ++Counts.Facings;
            if (!Send(report, link))
                return;
            done = t + 1e-5f;
        }

        // What the step did to the mode: a landing, into or out of the water -- sent at the step's end.
        if (Body.Landed)
        {
            Report land = Snapshot(Cd::Op::FALL_LAND, startMs + uint32_t(std::lround(h * 1000.0f)), after);
            land.FallMs = fall0 + uint32_t(std::lround(h * 1000.0f));
            ++Counts.Changes;
            if (!Send(land, link))
                return;
        }
        if ((after ^ _lastFlags) & Flag::SWIMMING)
        {
            ++Counts.Changes;
            if (!Send(Snapshot((after & Flag::SWIMMING) ? Cd::Op::START_SWIM : Cd::Op::STOP_SWIM,
                    startMs + uint32_t(std::lround(h * 1000.0f)), after), link))
                return;
        }
    }
}

void Mv::Client::Finish(ServerLink& link, uint32_t nowMs)
{
    if (!_started)
        return;
    float const dx = Body.X - _reportedX;
    float const dy = Body.Y - _reportedY;
    float const dz = Body.Z - _reportedZ;
    if (dx * dx + dy * dy + dz * dz > 1e-4f || std::fabs(TurnBetween(_reportedYaw, Body.Yaw)) > 1e-3f)
        Send(Snapshot(Cd::Op::HEARTBEAT, nowMs, _lastFlags), link);
}
