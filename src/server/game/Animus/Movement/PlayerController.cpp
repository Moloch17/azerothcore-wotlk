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

#include "PlayerController.h"
#include <algorithm>
#include <cmath>

namespace
{
    namespace Mv = Animus::Movement;

    constexpr float PI = 3.14159265f;
    constexpr float TWO_PI = 2.0f * PI;
    constexpr float DEG = PI / 180.0f;
    /// A move ending this close to a wall stops short of it, so the next sweep does not start inside it.
    constexpr float SKIN = 0.02f;
    /// A slide is taken only when it gets this much further along the wanted direction than the straight move:
    /// square on to a wall the turned moves gain only rounding, and a body pressed into a wall must not creep.
    constexpr float SLIDE_GAIN = 0.01f;
    /// How far below the feet a standing body still counts as standing (rounding, a floor's own bumps).
    constexpr float SNAP = 0.1f;

    float WrapYaw(float yaw)
    {
        float const wrapped = std::fmod(yaw, TWO_PI);
        return wrapped < 0.0f ? wrapped + TWO_PI : wrapped;
    }

    bool HasFloor(float floor)
    {
        return floor > Mv::INVALID_FLOOR + 1.0f;
    }

    struct Wish
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Speed = 0.0f;     // yd/s along (X, Y, Z), which is a unit vector or zero
    };

    /// The body-frame direction the keys ask for, as the client composes it (0x987700): forward/back and strafe each
    /// times DIAGONAL when both are held. `pitched` tilts forward along the look (water and air); `vertical` adds
    /// ascend/descend. The speed is the mode's, back speeds for anything holding back.
    Wish WishOf(Mv::BodyState const& body, Mv::ControlState const& control, Mv::Speeds const& speeds, Mv::Mode mode)
    {
        Wish wish;
        float forward = float(control.Forward);
        float strafe = float(control.Strafe);
        if (forward != 0.0f && strafe != 0.0f)
        {
            forward *= Mv::DIAGONAL;
            strafe *= Mv::DIAGONAL;
        }
        bool const pitched = mode == Mv::Mode::Swimming || mode == Mv::Mode::Flying;
        float const cy = std::cos(body.Yaw);
        float const sy = std::sin(body.Yaw);
        float const cp = pitched ? std::cos(body.Pitch) : 1.0f;
        float const sp = pitched ? std::sin(body.Pitch) : 0.0f;
        // Forward along the look; strafe right is the body's right (-left), level.
        float x = forward * cp * cy + strafe * sy;
        float y = forward * cp * sy - strafe * cy;
        float z = forward * sp;
        if (pitched && control.Vertical != 0)
        {
            // Ascend / descend: the client moves at 45 degrees, the horizontal part and the vertical each 0.7071
            // (0x987700); a vertical press alone goes straight up or down at that share (interpreted, C6).
            x *= Mv::VERTICAL_SHARE;
            y *= Mv::VERTICAL_SHARE;
            z = z * Mv::VERTICAL_SHARE + float(control.Vertical) * Mv::VERTICAL_SHARE;
        }
        float const length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-6f)
            return wish;
        if (length > 1.0f)
        {
            x /= length;
            y /= length;
            z /= length;
        }
        float const scale = std::min(1.0f, length);
        // The speed in force, the client's choice (fn 0x987570): flying and swimming take the back speed (the lower of
        // the two) when holding back; on the ground walking caps every direction at min(run, walk), and holding back
        // (with or without a strafe) runs at min(run, run back).
        bool const back = control.Forward < 0;
        switch (mode)
        {
            case Mv::Mode::Swimming:
                wish.Speed = back ? std::min(speeds.Swim, speeds.SwimBack) : speeds.Swim;
                break;
            case Mv::Mode::Flying:
                wish.Speed = back ? std::min(speeds.Flight, speeds.FlightBack) : speeds.Flight;
                break;
            default:
                wish.Speed = control.Walk ? std::min(speeds.Run, speeds.Walk)
                    : back ? std::min(speeds.Run, speeds.RunBack) : speeds.Run;
                break;
        }
        wish.X = x / scale;
        wish.Y = y / scale;
        wish.Z = z / scale;
        wish.Speed *= scale;
        return wish;
    }

    /// Move (dx, dy, dz) from the body with the wall rule: the whole move if it is free; else the turned variants of
    /// it (SLIDE_ANGLES, shortened by the cosine) kept for the most progress along the original; else as far as the
    /// original goes. Returns the move made.
    void SweptMove(Mv::BodyState& body, float dx, float dy, float dz, Mv::Body const& shape,
        Mv::WorldQuery const& world, float& outX, float& outY, float& outZ)
    {
        float const free = world.Sweep(body.X, body.Y, body.Z, body.X + dx, body.Y + dy, body.Z + dz, shape);
        if (free >= 1.0f)
        {
            outX = dx;
            outY = dy;
            outZ = dz;
            return;
        }
        body.AgainstWall = true;
        float const length = std::sqrt(dx * dx + dy * dy);
        float bestProgress = std::max(0.0f, free * length - SKIN);
        float const keep = length > 1e-6f ? bestProgress / length : 0.0f;
        outX = dx * keep;
        outY = dy * keep;
        outZ = dz * std::max(0.0f, free);
        if (length < 1e-6f)
            return;
        for (float angle : Mv::SLIDE_ANGLES)
        {
            float const a = angle * DEG;
            float const c = std::cos(a);
            float const s = std::sin(a);
            float const rx = (dx * c - dy * s) * c;
            float const ry = (dx * s + dy * c) * c;
            float const f = world.Sweep(body.X, body.Y, body.Z, body.X + rx, body.Y + ry, body.Z + dz, shape);
            // Only a turned move that is clear all the way is a slide along the wall: one that also runs into it
            // is the same wall met at another angle (square on, every turn of the move is blocked too).
            if (f < 1.0f)
                continue;
            float const reach = std::max(0.0f, f * length * c - SKIN);
            // Progress along the original direction: the turned move's length times the cosine to it.
            float const progress = reach * c;
            if (progress > bestProgress + SLIDE_GAIN)
            {
                bestProgress = progress;
                float const k = (f * length * c - SKIN) / std::max(1e-6f, length * c);
                outX = rx * std::max(0.0f, k);
                outY = ry * std::max(0.0f, k);
                outZ = dz * std::max(0.0f, f);
            }
        }
    }

    void StartFalling(Mv::BodyState& body, float vx, float vy, float vz)
    {
        body.Kind = Mv::Mode::Falling;
        body.Vx = vx;
        body.Vy = vy;
        body.Vz = vz;
        body.FallApexZ = body.Z;
        body.FallMs = 0;
    }

    /// Deep enough to swim at the feet's column: the liquid's level against the floor under it.
    bool SwimDepth(Mv::Liquid const& liquid, float floor, float atZ, Mv::Body const& shape, float share)
    {
        if (!liquid.Present || liquid.Level < atZ - Mv::STEP_UP)
            return false;
        float const bottom = HasFloor(floor) ? floor : atZ;
        return liquid.Level - bottom >= share * shape.Height;
    }

    void Turn(Mv::BodyState& body, Mv::ControlState const& control, float dt)
    {
        float rate = control.TurnRate;
        bool const moving = control.Forward != 0 || control.Strafe != 0;
        if (control.KeyboardTurn && moving)
            rate *= Mv::KEYBOARD_TURN_WHILE_MOVING;
        body.Yaw = WrapYaw(body.Yaw + rate * dt);
        if (body.Kind == Mv::Mode::Swimming || body.Kind == Mv::Mode::Flying)
            body.Pitch = std::clamp(body.Pitch + control.PitchRate * dt, -Mv::PITCH_LIMIT, Mv::PITCH_LIMIT);
    }

    void FallStep(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, float dt);

    void GroundStep(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, float dt)
    {
        Wish const wish = WishOf(body, control, speeds, Mv::Mode::Ground);

        if (control.Jump)
        {
            // From the ground: up at the client's jump speed, the run frozen at launch (no air control).
            control.Jump = false;
            StartFalling(body, wish.X * wish.Speed, wish.Y * wish.Speed, Mv::JUMP_SPEED);
            body.Jumped = true;
            FallStep(body, control, speeds, shape, world, dt);
            return;
        }
        if (speeds.CanFly && control.Vertical > 0)
        {
            body.Kind = Mv::Mode::Flying;
            body.Vx = body.Vy = body.Vz = 0.0f;
            return;
        }

        body.Commanded += wish.Speed * dt;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (wish.Speed > 0.0f)
            SweptMove(body, wish.X * wish.Speed * dt, wish.Y * wish.Speed * dt, 0.0f, shape, world, dx, dy, dz);
        float const nx = body.X + dx;
        float const ny = body.Y + dy;

        // The floor under the new place, from a step above the feet down to a step below.
        float floor = world.FloorBelow(nx, ny, body.Z + Mv::STEP_UP, 2.0f * Mv::STEP_UP);
        Mv::Liquid const liquid = world.LiquidAt(nx, ny, body.Z);
        if (speeds.WaterWalk && liquid.Present && !liquid.Deadly && liquid.Level <= body.Z + Mv::STEP_UP
            && (!HasFloor(floor) || liquid.Level > floor))
            floor = liquid.Level;

        if (HasFloor(floor) && floor > body.Z + SNAP && world.FloorNormalZ(nx, ny, floor) < Mv::WALKABLE_NORMAL_Z)
        {
            // Up a slope steeper than 50 degrees: refused, as a wall is.
            body.SteepSlope = true;
            body.AgainstWall = true;
            return;
        }

        float const knee = body.Z + Mv::STEP_UP + 0.05f;
        if (!HasFloor(floor) && world.InTerrain(nx, ny, knee) && !world.InTerrain(body.X, body.Y, knee))
        {
            // No floor within a step, and the knee would be inside the terrain: a rise of the ground too high to step
            // onto (the terrain is in no collision tree, so no sweep sees a hillside's face). A wall. A body already
            // under the terrain's surface (a cave) is not walled in by it.
            body.AgainstWall = true;
            return;
        }

        body.Moved += std::sqrt(dx * dx + dy * dy);
        body.X = nx;
        body.Y = ny;
        if (!HasFloor(floor))
        {
            // Off an edge: falling with the run it had.
            StartFalling(body, wish.X * wish.Speed, wish.Y * wish.Speed, 0.0f);
            return;
        }
        body.Z = floor;
        body.Vx = body.Vy = body.Vz = 0.0f;

        if (!speeds.WaterWalk && !liquid.Deadly && SwimDepth(liquid, floor, body.Z, shape, Mv::SWIM_ENTER))
            body.Kind = Mv::Mode::Swimming;
    }

    void FallStep(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, float dt)
    {
        control.Jump = false;   // a jump in the air is spent, as the client refuses it (flags & FALLING)
        if (speeds.CanFly && control.Vertical > 0)
        {
            body.Kind = Mv::Mode::Flying;
            body.Vz = 0.0f;
            return;
        }

        // Exact under constant gravity: the height changes by the mean of the speeds at either end of the step (a
        // trapezoid), so a jump's apex and air time do not depend on the step's length; clamped at terminal.
        float const terminal = speeds.SlowFall ? Mv::SAFE_FALL_VELOCITY : Mv::TERMINAL_VELOCITY;
        float const vz0 = body.Vz;
        body.Vz = std::max(body.Vz - Mv::GRAVITY * dt, -terminal);
        float const meanVz = 0.5f * (vz0 + body.Vz);

        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (body.Vx != 0.0f || body.Vy != 0.0f)
        {
            SweptMove(body, body.Vx * dt, body.Vy * dt, 0.0f, shape, world, dx, dy, dz);
            if (body.AgainstWall)
            {
                // A wall in the air stops the run against it (the client keeps no momentum along a wall it hit).
                body.Vx = dt > 0.0f ? dx / dt : 0.0f;
                body.Vy = dt > 0.0f ? dy / dt : 0.0f;
            }
        }
        float rise = meanVz * dt;
        if (rise > 0.0f)
        {
            float const room = world.Ceiling(body.X + dx, body.Y + dy, body.Z + shape.Height, rise);
            if (room < rise)
            {
                rise = std::max(0.0f, room);
                body.Vz = 0.0f;
            }
        }
        float const nx = body.X + dx;
        float const ny = body.Y + dy;
        float const nz = body.Z + rise;

        // Landing: a floor between where the feet were and where they are going.
        float const top = std::max(body.Z, nz) + SNAP;
        float floor = world.FloorBelow(nx, ny, top, top - nz + SNAP);
        Mv::Liquid const liquid = world.LiquidAt(nx, ny, nz);
        if (speeds.WaterWalk && liquid.Present && !liquid.Deadly && liquid.Level <= top
            && (!HasFloor(floor) || liquid.Level > floor))
            floor = liquid.Level;

        body.Moved += std::sqrt(dx * dx + dy * dy + rise * rise);
        body.X = nx;
        body.Y = ny;
        body.FallMs += uint32_t(dt * 1000.0f + 0.5f);

        // Into deep water on the way down: swimming, no damage. (On the way up -- a breach out of the water -- the feet
        // are still under the surface and must not count as a landing.)
        if (!speeds.WaterWalk && !liquid.Deadly && liquid.Present && body.Vz <= 0.0f && nz <= liquid.Level
            && SwimDepth(liquid, world.FloorBelow(nx, ny, liquid.Level, 1000.0f), nz, shape, Mv::SWIM_ENTER))
        {
            body.Z = std::max(nz, HasFloor(floor) ? floor : nz);
            body.Landed = true;
            body.LandedInWater = true;
            body.FallHeight = std::max(0.0f, body.FallApexZ - liquid.Level);
            body.Kind = Mv::Mode::Swimming;
            body.Vx = body.Vy = body.Vz = 0.0f;
            return;
        }
        if (HasFloor(floor) && nz <= floor && body.Vz <= 0.0f)
        {
            body.Z = floor;
            body.Landed = true;
            body.FallHeight = std::max(0.0f, body.FallApexZ - floor);
            body.Kind = Mv::Mode::Ground;
            body.Vx = body.Vy = body.Vz = 0.0f;
            return;
        }
        body.Z = nz;
        body.FallApexZ = std::max(body.FallApexZ, body.Z);
    }

    void SwimStep(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, float dt)
    {
        if (control.Jump)
        {
            // The client's swimming jump (fn 0x9883f0, C0c) has no depth or surface test: at any depth it launches the
            // body up at the swim-jump speed -- out of the water at the surface, a short hop up through the water below
            // it -- and back to swimming on the way down.
            control.Jump = false;
            Wish const level = WishOf(body, control, speeds, Mv::Mode::Ground);
            float const speed = level.Speed > 0.0f ? (control.Forward < 0 ? std::min(speeds.Swim, speeds.SwimBack)
                : speeds.Swim) : 0.0f;
            StartFalling(body, level.X * speed, level.Y * speed, Mv::SWIM_JUMP_SPEED);
            body.Jumped = true;
            FallStep(body, control, speeds, shape, world, dt);
            return;
        }

        Wish const wish = WishOf(body, control, speeds, Mv::Mode::Swimming);
        body.Commanded += wish.Speed * dt;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (wish.Speed > 0.0f)
            SweptMove(body, wish.X * wish.Speed * dt, wish.Y * wish.Speed * dt, wish.Z * wish.Speed * dt, shape,
                world, dx, dy, dz);
        float const nx = body.X + dx;
        float const ny = body.Y + dy;
        float nz = body.Z + dz;

        Mv::Liquid const there = world.LiquidAt(nx, ny, nz);
        float const bed = world.FloorBelow(nx, ny, nz + Mv::STEP_UP, 1000.0f);
        float const knee = body.Z + Mv::STEP_UP + 0.05f;
        if (!HasFloor(bed) && world.InTerrain(nx, ny, knee) && !world.InTerrain(body.X, body.Y, knee))
        {
            // A bank of terrain higher than a step above the swimmer: a wall, as on the ground (out over it takes the
            // swim jump).
            body.AgainstWall = true;
            return;
        }
        if (there.Present)
            nz = std::min(nz, there.Level - Mv::FLOAT_DEPTH * shape.Height);
        if (HasFloor(bed))
            nz = std::max(nz, bed);

        body.Moved += std::sqrt(dx * dx + dy * dy + (nz - body.Z) * (nz - body.Z));
        body.X = nx;
        body.Y = ny;
        body.Z = nz;
        body.Vx = body.Vy = body.Vz = 0.0f;

        // Out of the water: shallow enough to walk (the shore), or no water at all.
        if (!there.Present || !SwimDepth(there, bed, nz, shape, Mv::SWIM_LEAVE))
        {
            if (HasFloor(bed) && nz - bed <= Mv::STEP_UP)
            {
                body.Z = bed;
                body.Kind = Mv::Mode::Ground;
            }
            else
                StartFalling(body, 0.0f, 0.0f, 0.0f);
        }
    }

    void FlyStep(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, float dt)
    {
        control.Jump = false;   // the client refuses a jump while FLYING
        if (!speeds.CanFly)
        {
            // The mount or the zone's flight is gone: falling from here.
            StartFalling(body, 0.0f, 0.0f, 0.0f);
            return;
        }
        Wish const wish = WishOf(body, control, speeds, Mv::Mode::Flying);
        body.Commanded += wish.Speed * dt;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (wish.Speed > 0.0f)
            SweptMove(body, wish.X * wish.Speed * dt, wish.Y * wish.Speed * dt, wish.Z * wish.Speed * dt, shape,
                world, dx, dy, dz);
        float const nx = body.X + dx;
        float const ny = body.Y + dy;
        float nz = body.Z + dz;

        float const ground = world.FloorBelow(nx, ny, std::max(body.Z, nz) + SNAP, 1000.0f);
        if (HasFloor(ground))
            nz = std::min(nz, ground + speeds.CeilingAboveGround);
        if (dz > 0.0f)
        {
            float const room = world.Ceiling(nx, ny, body.Z + shape.Height, dz);
            if (room < dz)
                nz = body.Z + std::max(0.0f, room);
        }

        body.Moved += std::sqrt(dx * dx + dy * dy + (nz - body.Z) * (nz - body.Z));
        body.X = nx;
        body.Y = ny;
        body.Vx = body.Vy = body.Vz = 0.0f;

        Mv::Liquid const liquid = world.LiquidAt(nx, ny, nz);
        if (liquid.Present && !liquid.Deadly && nz < liquid.Level - Mv::FLOAT_DEPTH * shape.Height)
        {
            body.Z = nz;
            body.Kind = Mv::Mode::Swimming;
            return;
        }
        if (HasFloor(ground) && nz <= ground + SNAP)
        {
            // Down onto the ground: landed (descending or level; a climb off it stays in the air).
            body.Z = ground;
            if (dz <= 0.0f)
                body.Kind = Mv::Mode::Ground;
            return;
        }
        body.Z = nz;
    }

    void SubStep(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, float dt)
    {
        Turn(body, control, dt);
        switch (body.Kind)
        {
            case Mv::Mode::Ground:
                GroundStep(body, control, speeds, shape, world, dt);
                break;
            case Mv::Mode::Falling:
                FallStep(body, control, speeds, shape, world, dt);
                break;
            case Mv::Mode::Swimming:
                SwimStep(body, control, speeds, shape, world, dt);
                break;
            case Mv::Mode::Flying:
                FlyStep(body, control, speeds, shape, world, dt);
                break;
        }
    }
}

void Animus::Movement::Step(BodyState& body, ControlState& control, Speeds const& speeds, Body const& shape,
    WorldQuery const& world, float dt)
{
    body.AgainstWall = false;
    body.SteepSlope = false;
    body.Landed = false;
    body.LandedInWater = false;
    body.Jumped = false;
    body.FallHeight = 0.0f;
    body.Moved = 0.0f;
    body.Commanded = 0.0f;
    if (!(dt > 0.0f))
        return;
    uint32_t const steps = std::max<uint32_t>(1, uint32_t(std::ceil(dt / MAX_SUBSTEP - 1e-4f)));
    float const sub = dt / float(steps);
    // A landing is reported for the whole step even if a later sub-step walks on.
    bool landed = false;
    bool landedInWater = false;
    bool jumped = false;
    float fallHeight = 0.0f;
    bool wall = false;
    bool steep = false;
    for (uint32_t i = 0; i < steps; ++i)
    {
        SubStep(body, control, speeds, shape, world, sub);
        landed = landed || body.Landed;
        landedInWater = landedInWater || body.LandedInWater;
        jumped = jumped || body.Jumped;
        fallHeight = std::max(fallHeight, body.FallHeight);
        wall = wall || body.AgainstWall;
        steep = steep || body.SteepSlope;
        body.Landed = body.LandedInWater = body.AgainstWall = body.SteepSlope = body.Jumped = false;
    }
    body.Landed = landed;
    body.Jumped = jumped;
    body.LandedInWater = landedInWater;
    body.FallHeight = fallHeight;
    body.AgainstWall = wall;
    body.SteepSlope = steep;
}

void Animus::Movement::Launch(BodyState& body, float vx, float vy, float vz)
{
    StartFalling(body, vx, vy, vz);
}

void Animus::Movement::Resync(BodyState& body, float x, float y, float z, float yaw, Body const& shape,
    WorldQuery const& world)
{
    body.X = x;
    body.Y = y;
    body.Z = z;
    body.Yaw = WrapYaw(yaw);
    body.Vx = body.Vy = body.Vz = 0.0f;
    body.FallMs = 0;
    body.FallApexZ = z;
    float const floor = world.FloorBelow(x, y, z + SNAP, STEP_UP + SNAP);
    Liquid const liquid = world.LiquidAt(x, y, z);
    if (liquid.Present && !liquid.Deadly && z <= liquid.Level
        && SwimDepth(liquid, world.FloorBelow(x, y, liquid.Level, 1000.0f), z, shape, SWIM_ENTER))
        body.Kind = Mode::Swimming;
    else if (HasFloor(floor) && z - floor <= STEP_UP)
    {
        body.Z = floor;
        body.Kind = Mode::Ground;
    }
    else
        body.Kind = Mode::Falling;
}

bool Animus::Movement::CanJump(BodyState const& body, Body const& /*shape*/, WorldQuery const& /*world*/)
{
    // The client refuses a jump only when FLYING, ROOT or FALLING (0x2001800, fn 0x9883f0): on the ground and
    // swimming at any depth it jumps (C0c).
    return body.Kind == Mode::Ground || body.Kind == Mode::Swimming;
}

bool Animus::Movement::CanSteerVertically(BodyState const& body, Speeds const& speeds)
{
    return body.Kind == Mode::Swimming || body.Kind == Mode::Flying || speeds.CanFly;
}

uint32_t Animus::Movement::MovementFlags(BodyState const& body, ControlState const& control, Speeds const& speeds)
{
    uint32_t flags = 0;
    if (control.Forward > 0)
        flags |= Flag::FORWARD;
    else if (control.Forward < 0)
        flags |= Flag::BACKWARD;
    if (control.Strafe > 0)
        flags |= Flag::STRAFE_RIGHT;
    else if (control.Strafe < 0)
        flags |= Flag::STRAFE_LEFT;
    // Only the turn and pitch keys set LEFT/RIGHT and PITCH_UP/DOWN; the mouse turns the body and reports its facing
    // (SET_FACING / SET_PITCH, ReportCadence), as the client's mouse-look does.
    if (control.KeyboardTurn && control.TurnRate > 0.0f)
        flags |= Flag::LEFT;
    else if (control.KeyboardTurn && control.TurnRate < 0.0f)
        flags |= Flag::RIGHT;
    bool const steered = body.Kind == Mode::Swimming || body.Kind == Mode::Flying;
    if (steered && control.KeyboardTurn && control.PitchRate > 0.0f)
        flags |= Flag::PITCH_UP;
    else if (steered && control.KeyboardTurn && control.PitchRate < 0.0f)
        flags |= Flag::PITCH_DOWN;
    if (steered && control.Vertical > 0)
        flags |= Flag::ASCENDING;
    else if (steered && control.Vertical < 0)
        flags |= Flag::DESCENDING;
    if (control.Walk)
        flags |= Flag::WALKING;
    switch (body.Kind)
    {
        case Mode::Falling:
            flags |= Flag::FALLING;
            break;
        case Mode::Swimming:
            flags |= Flag::SWIMMING;
            break;
        case Mode::Flying:
            flags |= Flag::FLYING;
            break;
        default:
            break;
    }
    if (speeds.CanFly)
        flags |= Flag::CAN_FLY;
    if (speeds.WaterWalk)
        flags |= Flag::WATERWALKING;
    if (speeds.SlowFall)
        flags |= Flag::FALLING_SLOW;
    return flags;
}
