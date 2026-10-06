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
    /// How far under a buried body a floor is looked for before it is taken to have none (a cave's floor is closer).
    constexpr float BURIED_SEARCH = 1000.0f;
    /// A drop under the centre this deep in one move is an edge the footprint is read for (a gutter, a crack's lip):
    /// shallower is a slope walked down, read from the centre alone (the footprint is eight height queries).
    constexpr float FOOTPRINT_DROP = 0.5f;

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
    /// The highest floor under the edge of a body's footprint at (x, y) -- eight points at its radius -- between `top`
    /// and `lowest`, or INVALID_FLOOR. A body is a cylinder: where its centre is over a crack narrower than itself,
    /// or over the lip of a drop, its edge still stands on the floor beside it.
    float FootprintFloor(float x, float y, float top, float lowest, Mv::Body const& shape, Mv::WorldQuery const& world)
    {
        constexpr float DIAGONAL = 0.70710678f;
        static constexpr float POINTS[8][2] = { { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f },
            { DIAGONAL, DIAGONAL }, { -DIAGONAL, DIAGONAL }, { DIAGONAL, -DIAGONAL }, { -DIAGONAL, -DIAGONAL } };
        float best = Mv::INVALID_FLOOR;
        if (shape.Radius <= 0.0f || top < lowest)
            return best;
        for (auto const& point : POINTS)
        {
            float const floor = world.FloorBelow(x + point[0] * shape.Radius, y + point[1] * shape.Radius, top,
                top - lowest);
            if (HasFloor(floor) && floor >= lowest)
                best = std::max(best, floor);
        }
        return best;
    }

    /// Nothing at all below (x, y, z): no floor, no terrain's surface under it, no water -- a step there could only
    /// fall to the map's floor and die (the M1 Stockades hallway, 2026-10-05: strips along the corridor with no
    /// collision in the vmaps, where the client has the WMO's floor).
    bool OverVoid(float x, float y, float z, Mv::WorldQuery const& world)
    {
        if (HasFloor(world.FloorBelow(x, y, z, BURIED_SEARCH)))
            return false;
        float const terrain = world.TerrainHeight(x, y);
        if (HasFloor(terrain) && terrain <= z + SNAP)
            return false;
        return !world.LiquidAt(x, y, z).Present;
    }

    /// The share of a move along which the knee stays out of the terrain, when it starts above the terrain's surface:
    /// the terrain is in no collision tree, so the sweep's rays do not see a hillside, and a step up it that the floor
    /// search answered with something else -- a rock model buried in the hill, whose top Map::GetHeight hands back once
    /// the terrain is out of reach above -- walked the body into the hill (the M1/M2 dry check: seats 20-60 yd under
    /// Kalimdor's hills, then off the buried floor's edge and down to the map's floor). As the client's terrain
    /// collision does, the surface is crossed from above only: a move that starts over a hole (a cave's mouth: the
    /// terrain has no height there) or already under the surface (inside the cave) is the cave's own and goes on.
    /// 1 when the knee does not cross; else where it does, found by halving.
    float TerrainShare(Mv::BodyState const& body, float dx, float dy, float dz, Mv::WorldQuery const& world)
    {
        float const knee = Mv::STEP_UP + 0.05f;
        if (!HasFloor(world.TerrainHeight(body.X, body.Y)) || world.InTerrain(body.X, body.Y, body.Z + knee)
            || !world.InTerrain(body.X + dx, body.Y + dy, body.Z + dz + knee))
            return 1.0f;
        float lo = 0.0f;
        float hi = 1.0f;
        for (int i = 0; i < 10; ++i)
        {
            float const mid = 0.5f * (lo + hi);
            if (world.InTerrain(body.X + dx * mid, body.Y + dy * mid, body.Z + dz * mid + knee))
                hi = mid;
            else
                lo = mid;
        }
        return lo;
    }

    /// The share of a move that is free: the collision trees' sweep, and the terrain's surface (TerrainShare).
    float FreeShare(Mv::BodyState const& body, float dx, float dy, float dz, Mv::Body const& shape,
        Mv::WorldQuery const& world)
    {
        float const swept = world.Sweep(body.X, body.Y, body.Z, body.X + dx, body.Y + dy, body.Z + dz, shape);
        return std::min(swept, TerrainShare(body, dx, dy, dz, world));
    }

    /// Whether a move to (x, y) with the feet ending at z goes through the terrain's surface: the feet would be inside
    /// the terrain there while the body now stands, swims or flies above a surface. The ground step's floor search
    /// reaches a step up and its knee test starts just above that, so a rise of the terrain between the two (1.19 to
    /// ~1.29 yd within one sub-step, on a steep hillside) found neither a floor nor a wall and was taken for an edge:
    /// the body fell from under the surface and on under the hill (the M1/M2 dry check, 2026-10-05). A body over a
    /// hole (a cave's mouth: no terrain height there) or already under the surface (a cave) is not crossing it.
    bool IntoTerrain(Mv::BodyState const& body, float x, float y, float z, Mv::WorldQuery const& world)
    {
        return world.InTerrain(x, y, z + SNAP) && HasFloor(world.TerrainHeight(body.X, body.Y))
            && !world.InTerrain(body.X, body.Y, body.Z + SNAP);
    }

    void SweptMove(Mv::BodyState& body, float dx, float dy, float dz, Mv::Body const& shape,
        Mv::WorldQuery const& world, float& outX, float& outY, float& outZ)
    {
        float const free = FreeShare(body, dx, dy, dz, shape, world);
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
            float const f = FreeShare(body, rx, ry, dz, shape, world);
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
                // The smallest turn free all the way goes furthest along the wanted way: no larger one is tried
                // (six rays a turn; a corridor's walls are met every step).
                break;
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

        // The floor under the new place, from a step above the feet down to a step below; where the centre has none,
        // or only one FOOTPRINT_DROP or more below the feet, the footprint's edge on a floor at the feet holds the body
        // there (FootprintFloor): it does not drop through, or into, a gap narrower than itself. Only a supporting
        // floor (at or below the feet) is taken from the edge: stepping up stays the centre's, as the sweep's.
        float floor = world.FloorBelow(nx, ny, body.Z + Mv::STEP_UP, 2.0f * Mv::STEP_UP);
        if (!HasFloor(floor) || floor < body.Z - FOOTPRINT_DROP)
            floor = std::max(floor, FootprintFloor(nx, ny, body.Z + SNAP, body.Z - Mv::STEP_UP, shape, world));
        Mv::Liquid const liquid = world.LiquidAt(nx, ny, body.Z);
        if (speeds.WaterWalk && liquid.Present && !liquid.Deadly && liquid.Level <= body.Z + Mv::STEP_UP
            && (!HasFloor(floor) || liquid.Level > floor))
            floor = liquid.Level;

        if (IntoTerrain(body, nx, ny, HasFloor(floor) ? floor : body.Z, world))
        {
            // The step would leave the feet inside the terrain -- no floor found up a rise just over a step, or a model
            // buried in the hill answering for the floor: a hillside, a wall.
            body.AgainstWall = true;
            return;
        }

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

        if (!HasFloor(floor) && (dx != 0.0f || dy != 0.0f) && OverVoid(nx, ny, body.Z, world))
        {
            // Over nothing at all: an edge the body cannot step off (it could only fall to the map's floor). A wall,
            // counted (OverVoid) so the place is reported.
            body.AgainstWall = true;
            body.OverVoid = true;
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
        float const nz = body.Z + rise;
        if ((dx != 0.0f || dy != 0.0f) && IntoTerrain(body, body.X + dx, body.Y + dy, std::max(body.Z, nz), world))
        {
            // Sideways into a hillside in the air: the run stops against it, the fall goes on (down onto the slope).
            dx = dy = 0.0f;
            body.Vx = body.Vy = 0.0f;
            body.AgainstWall = true;
        }
        float const above = std::max(body.Z, nz) + Mv::STEP_UP;
        if ((dx != 0.0f || dy != 0.0f) && OverVoid(body.X + dx, body.Y + dy, above, world)
            && (!OverVoid(body.X, body.Y, above, world)
                || HasFloor(FootprintFloor(body.X, body.Y, above, above - BURIED_SEARCH, shape, world))))
        {
            // Sideways over nothing at all in the air (a jump across a strip the world query has no floor in): an edge,
            // as on the ground -- the run stops, the fall goes on down where there is a floor. Where the body was is
            // over a floor if its footprint is (a body at the edge stands with its centre over the strip).
            dx = dy = 0.0f;
            body.Vx = body.Vy = 0.0f;
            body.AgainstWall = true;
            body.OverVoid = true;
        }
        float const nx = body.X + dx;
        float const ny = body.Y + dy;

        // Landing: a floor between where the feet were and where they are going -- or one risen above the feet where
        // the body has moved to, within a step (running up a ramp in the air: the ramp comes up under the feet faster
        // than they come down, and searched only between the two heights it was never crossed, so the body fell on
        // through it -- the M1 Stockades entrance ramp, 2026-10-05).
        float const top = std::max(body.Z, nz) + Mv::STEP_UP;
        float floor = world.FloorBelow(nx, ny, top, top - nz + SNAP);
        // The footprint's edge lands too, on a floor the feet go down past in this step (a jump onto a crack's lip) --
        // never on the one the body is leaving, which its trailing edge still overlaps as it steps off.
        if (!HasFloor(floor) && body.Vz <= 0.0f && nz < body.Z - SNAP)
            floor = FootprintFloor(nx, ny, body.Z - SNAP, nz - SNAP, shape, world);
        Mv::Liquid const liquid = world.LiquidAt(nx, ny, nz);
        if (speeds.WaterWalk && liquid.Present && !liquid.Deadly && liquid.Level <= top
            && (!HasFloor(floor) || liquid.Level > floor))
            floor = liquid.Level;

        body.Moved += std::sqrt(dx * dx + dy * dy + rise * rise);
        float const z0 = body.Z;
        body.X = nx;
        body.Y = ny;
        uint32_t const fallBefore = body.FallMs;
        body.FallMs += uint32_t(dt * 1000.0f + 0.5f);
        // The fall's time to the moment the feet reach `level` inside this step (z0 + vz0 t - g t^2 / 2 = level), as
        // the client stamps a landing: FALL_LAND's fall time, which the server's fall damage reads with the height.
        auto landedAt = [&](float level)
        {
            float const a = 0.5f * Mv::GRAVITY;
            float const disc = vz0 * vz0 + 4.0f * a * (z0 - level);
            float t = disc >= 0.0f ? (vz0 + std::sqrt(disc)) / (2.0f * a) : dt;
            t = std::clamp(t, 0.0f, dt);
            body.FallMs = fallBefore + uint32_t(t * 1000.0f + 0.5f);
        };

        // Into deep water on the way down: swimming, no damage. (On the way up -- a breach out of the water -- the feet
        // are still under the surface and must not count as a landing.)
        if (!speeds.WaterWalk && !liquid.Deadly && liquid.Present && body.Vz <= 0.0f && nz <= liquid.Level
            && SwimDepth(liquid, world.FloorBelow(nx, ny, liquid.Level, 1000.0f), nz, shape, Mv::SWIM_ENTER))
        {
            body.Z = std::max(nz, HasFloor(floor) ? floor : nz);
            landedAt(liquid.Level);
            body.Landed = true;
            body.LandedInWater = true;
            body.FallHeight = std::max(0.0f, body.FallApexZ - liquid.Level);
            body.Kind = Mv::Mode::Swimming;
            body.Vx = body.Vy = body.Vz = 0.0f;
            return;
        }
        if (HasFloor(floor) && floor > nz && body.Vz > 0.0f)
        {
            // Rising, a floor has come up under the feet: they are carried up onto it, still rising.
            body.Z = floor;
            return;
        }
        if (HasFloor(floor) && nz <= floor && body.Vz <= 0.0f)
        {
            body.Z = floor;
            landedAt(floor);
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
        if (IntoTerrain(body, nx, ny, nz, world))
        {
            body.AgainstWall = true;
            return;
        }

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
        if (IntoTerrain(body, nx, ny, nz, world))
        {
            body.AgainstWall = true;
            return;
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
        // Half the turn, the move, the other half: the body moves along the sub-step's middle heading, the chord of
        // the arc a steady turn walks. Turned whole before the move it ran on the sub-step's end heading, a sub-step's
        // turn ahead of the arc (18 degrees at 360 deg/s in 50 ms), so where a turning body went depended on the tick
        // (ClientTest.TheCadenceAndTheBodyDoNotDependOnTheTick; the realm's tick is variable).
        Turn(body, control, 0.5f * dt);
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
        Turn(body, control, 0.5f * dt);
    }
}

void Animus::Movement::Step(BodyState& body, ControlState& control, Speeds const& speeds, Body const& shape,
    WorldQuery const& world, float dt)
{
    body.AgainstWall = false;
    body.SteepSlope = false;
    body.OverVoid = false;
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
    bool overVoid = false;
    for (uint32_t i = 0; i < steps; ++i)
    {
        SubStep(body, control, speeds, shape, world, sub);
        landed = landed || body.Landed;
        landedInWater = landedInWater || body.LandedInWater;
        jumped = jumped || body.Jumped;
        fallHeight = std::max(fallHeight, body.FallHeight);
        wall = wall || body.AgainstWall;
        steep = steep || body.SteepSlope;
        overVoid = overVoid || body.OverVoid;
        body.Landed = body.LandedInWater = body.AgainstWall = body.SteepSlope = body.Jumped = body.OverVoid = false;
    }
    body.Landed = landed;
    body.Jumped = jumped;
    body.LandedInWater = landedInWater;
    body.FallHeight = fallHeight;
    body.AgainstWall = wall;
    body.SteepSlope = steep;
    body.OverVoid = overVoid;
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
    // The floor is looked for from a step above the feet, as a ground step looks (GroundStep): a body the server put a
    // little under the walkable surface -- a spawn point or a teleport's landing read off the terrain a few tenths low,
    // as the M1 dry check's held-out plains point (4012, -788) was, 0.8 yd under -- stands on it, as a client's does,
    // where searched from the feet down it saw nothing and fell through the world. STEP_UP is the reach because it is
    // the most the client climbs without a jump: nothing is taken that it could not have walked onto.
    float const floor = world.FloorBelow(x, y, z + STEP_UP, 2.0f * STEP_UP);
    Liquid const liquid = world.LiquidAt(x, y, z);
    if (liquid.Present && !liquid.Deadly && z <= liquid.Level
        && SwimDepth(liquid, world.FloorBelow(x, y, liquid.Level, 1000.0f), z, shape, SWIM_ENTER))
        body.Kind = Mode::Swimming;
    else if (HasFloor(floor) && std::fabs(z - floor) <= STEP_UP)
    {
        body.Z = floor;
        body.Kind = Mode::Ground;
    }
    else
        body.Kind = Mode::Falling;

    // The last rung: put deeper than a step inside the ground with nothing at all under it -- a spawn point or the
    // server's position buried in the terrain -- the body would fall to the map's floor and die there. It stands on
    // the terrain instead. A cave (under the terrain's surface, with its own floor below) is left to fall onto that.
    body.Unburied = false;
    if (body.Kind == Mode::Falling && world.InTerrain(x, y, z)
        && !HasFloor(world.FloorBelow(x, y, z, BURIED_SEARCH)))
        if (float const terrain = world.TerrainHeight(x, y); HasFloor(terrain) && terrain > z)
        {
            body.Z = terrain;
            body.FallApexZ = terrain;
            body.Kind = Mode::Ground;
            body.Unburied = true;
        }
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
