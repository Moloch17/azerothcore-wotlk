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

#ifndef ANIMUS_MOVEMENT_PLAYER_CONTROLLER_H
#define ANIMUS_MOVEMENT_PLAYER_CONTROLLER_H

#include <cstdint>

/// **The player controller** (player-controller plan): a seat moves the way a player's client moves a player -- held
/// movement keys, a turn and a pitch rate, a jump -- through the client's own movement physics, reimplemented here,
/// rather than through server splines on the navmesh. Pure and deterministic: no core types, a WorldQuery for the
/// geometry, so it is tested on fake worlds (PlayerControllerTest) and run on a live map (MapWorldQuery).
///
/// The constants are the 3.3.5a client's (build 12340), read out of Wow.exe by static analysis
/// (.agents/plans/player-controller/client-constants.md): the virtual address of each is beside it.
///
/// **Motion the core imposes** -- a root, stun, fear or confuse, a charge, a knockback, any spline or jump a spell
/// launches -- is the core's, not the controller's: while it lasts the caller does not Step the body (it yields),
/// and when it ends the caller takes the body back where the core left it with Resync, which keeps nothing of the
/// controller's own velocity. The controls the policy holds are kept, so a seat still holding forward runs on.
namespace Animus::Movement
{
    // ---------------------------------------------------------------------------------------------- the client's
    /// Gravity, yd/s^2 (Wow.exe 0xa32f74; the core's Movement::gravity).
    constexpr float GRAVITY = 19.2911053f;
    /// Terminal fall speed, and under slow fall / feather fall (0xb2d9e8, 0xb2d9ec).
    constexpr float TERMINAL_VELOCITY = 60.1480026f;
    constexpr float SAFE_FALL_VELOCITY = 7.0f;
    /// A jump's launch speed up, yd/s (0xaa33dc, stored negative there: the client's vertical speed is down-positive).
    /// The client's own value, not the core's 7.955 (MoveBlock::JUMP_SPEED_Z rounded it): its apex, 1.640 yd, is what
    /// decides which ledges a player can jump onto, and a controller that is to move like a client must jump like one.
    constexpr float JUMP_SPEED = 7.9555473f;
    /// A jump while swimming at the surface: the breach out of the water (0xaa33e0, taken when SWIMMING).
    constexpr float SWIM_JUMP_SPEED = 9.0967484f;
    /// Walkable: a floor whose normal is at most 50 degrees from up (normal.z >= cos 50, 0xa37f0c).
    constexpr float WALKABLE_NORMAL_Z = 0.6427876f;
    /// Forward/back and strafe held together: each component times this, so a diagonal is not faster (0x9e8d48).
    constexpr float DIAGONAL = 0.7071068f;
    /// Keyboard turning while moving is this share of the turn rate (0x9e9ee4). The condition the client tests is
    /// interpreted from its flag masks (C6 to confirm), so it is a constant C6's replay can switch off.
    constexpr float KEYBOARD_TURN_WHILE_MOVING = 0.75f;
    /// Pitch limit, radians (the client clamps the look pitch to a quarter turn either way, 0x9e8d88).
    constexpr float PITCH_LIMIT = 1.5707964f;

    /// tan 50 degrees (0xa37f78): the climb of the steepest walkable slope over a yard.
    constexpr float TAN_WALKABLE = 1.1917536f;
    /// The highest rise walked onto without a jump: the client's max(radius + 1/720, B x tan 50) (fn 0x761b00, C0c),
    /// with the mover's B at its default 1.0 (0x6ebd7f) and a player's radius 1/3 (0x6ebd5c): 1.1917536 yd. The
    /// value's role as the step limit is read from its use beside the walkable test (interpreted; C6 confirms it on
    /// 0.5 / 1.0 / 1.5 yd steps). B scales with the model (0x6e9570); a scaled body is C6's.
    constexpr float STEP_UP = TAN_WALKABLE;
    /// Ascending or descending in water or the air goes at 45 degrees: the facing direction and the vertical each
    /// times 0.7071 of the speed in force (fn 0x987700, C0c).
    constexpr float VERTICAL_SHARE = 0.7071068f;

    // ------------------------------------------------------------------------------- calibrated (C6), with reasons
    /// Water this deep (of the body's height) is swum, and is walked again only once it is this shallow (the
    /// shore hysteresis the old steering used, its 0.75 / 0.4 of the height).
    constexpr float SWIM_ENTER = 0.75f;
    constexpr float SWIM_LEAVE = 0.4f;
    /// A swimmer floats with this share of its body under the surface (not found in the client, C0c: C6 confirms).
    constexpr float FLOAT_DEPTH = 0.5f;
    /// The longest step the integrator takes: a world tick longer than this is cut into equal sub-steps of at most
    /// this much, so a 250 ms tick moves a body exactly as five 50 ms ticks do (no tunnelling, falls timed right).
    constexpr float MAX_SUBSTEP = 0.05f;
    /// Wall sliding: when a move is blocked, these turns of it (degrees) are tried, kept the one that gets furthest
    /// along the wanted direction.
    constexpr float SLIDE_ANGLES[] = { 30.0f, -30.0f, 60.0f, -60.0f };

    enum class Mode : uint8_t
    {
        Ground,
        Falling,
        Swimming,
        Flying,
    };

    /// What the seat holds: a player's keys and mouse. Persists across ticks until changed; `jump` is one-shot and
    /// cleared by the step that takes it.
    struct ControlState
    {
        int8_t Forward = 0;         // +1 forward, -1 back, 0 neither
        int8_t Strafe = 0;          // +1 right, -1 left
        int8_t Vertical = 0;        // +1 ascend, -1 descend (water and air)
        float TurnRate = 0.0f;      // rad/s, + left (counter-clockwise, the client's yaw direction)
        float PitchRate = 0.0f;     // rad/s, + up
        bool Jump = false;
        bool Walk = false;
        /// Turning by the keyboard (the turn keys), which the client slows while moving; the policy's turn rates are
        /// the mouse's and are not slowed. Set by replays of a human's TURN_LEFT/RIGHT flags (C6).
        bool KeyboardTurn = false;
    };

    /// The unit's own speeds in force (mount, form, snare and buffs included), yd/s and rad/s.
    struct Speeds
    {
        float Walk = 2.5f;
        float Run = 7.0f;
        float RunBack = 4.5f;
        float Swim = 4.722222f;
        float SwimBack = 2.5f;
        float Flight = 7.0f;
        float FlightBack = 4.5f;
        float TurnRate = 3.141594f;
        float PitchRate = 3.14f;
        bool CanFly = false;        // a flying mount or form in a flyable zone
        bool SlowFall = false;      // slow fall / feather fall / levitate: the safe-fall terminal
        bool WaterWalk = false;     // water is a floor
        float CeilingAboveGround = 150.0f; // TravelBlock::MAX_ALTITUDE
    };

    struct Body
    {
        float Radius = 0.389f;
        float Height = 2.0f;
    };

    struct BodyState
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;             // the feet
        float Yaw = 0.0f;           // radians, 0..2pi
        float Pitch = 0.0f;
        float Vx = 0.0f;
        float Vy = 0.0f;
        float Vz = 0.0f;            // up positive
        Mode Kind = Mode::Ground;  // ground, falling, swimming or flying
        float FallApexZ = 0.0f;     // highest point of the current fall
        uint32_t FallMs = 0;

        // What the last step met (for the observation and the columns); reset by each Step.
        bool AgainstWall = false;   // a move was blocked (and slid, or stopped)
        bool SteepSlope = false;    // a rise was refused for its slope
        bool Landed = false;        // a fall ended on a floor this step
        bool LandedInWater = false;
        bool Jumped = false;        // a jump (or a swim jump) was taken this step
        float FallHeight = 0.0f;    // apex to landing, when Landed
        float Moved = 0.0f;         // yards actually travelled this step (3D)
        float Commanded = 0.0f;     // yards the held controls asked for this step
    };

    struct Liquid
    {
        bool Present = false;
        float Level = 0.0f;
        bool Deadly = false;        // magma / slime: never swum into by choice, still entered if walked into
    };

    /// The geometry a body moves in. Heights are the feet's.
    class WorldQuery
    {
    public:
        virtual ~WorldQuery() = default;
        /// The highest floor at or below `z` within `search` yards, or INVALID (below) when there is none.
        [[nodiscard]] virtual float FloorBelow(float x, float y, float z, float search) const = 0;
        /// The floor's normal's up component at (x, y) near height z (1 flat, 0 a wall).
        [[nodiscard]] virtual float FloorNormalZ(float x, float y, float z) const = 0;
        [[nodiscard]] virtual Liquid LiquidAt(float x, float y, float z) const = 0;
        /// The share (0..1) of the straight move from (x0,y0,z0) to (x1,y1,z1) a body of `body` can make before it
        /// meets a wall, swept at the knee (above STEP_UP, so steps are not walls) and at the chest.
        [[nodiscard]] virtual float Sweep(float x0, float y0, float z0, float x1, float y1, float z1,
            Body const& body) const = 0;
        /// Open air above (x, y, z) up to `up` yards (the distance to the first solid, or `up`).
        [[nodiscard]] virtual float Ceiling(float x, float y, float z, float up) const = 0;
        /// Whether (x, y, z) is inside the terrain (below its surface). The terrain is in no collision tree, so a
        /// hillside's face is seen only this way; models are walls through Sweep.
        [[nodiscard]] virtual bool InTerrain(float x, float y, float z) const = 0;
    };

    constexpr float INVALID_FLOOR = -200000.0f;

    /// Move a body `dt` seconds under the controls. Splits dt into ceil(dt / MAX_SUBSTEP) equal sub-steps. Clears
    /// `control.Jump` when it takes the jump (or when the jump is impossible: the press is spent).
    void Step(BodyState& body, ControlState& control, Speeds const& speeds, Body const& shape, WorldQuery const& world,
        float dt);

    /// Take the body back after motion the core imposed (a fear, a knockback, a spell's jump): its position and
    /// facing as the core left them, nothing of the controller's velocity; on the ground if there is a floor within a
    /// step, swimming if in deep water, else falling from there.
    void Resync(BodyState& body, float x, float y, float z, float yaw, Body const& shape, WorldQuery const& world);

    /// Throw the body into a fall with this velocity (yd/s, up positive): a knockback the server ordered, launched by
    /// the client as SMSG_MOVE_KNOCK_BACK asks, its fall timed from here.
    void Launch(BodyState& body, float vx, float vy, float vz);

    /// Whether a jump would do anything now (the action mask): on the ground, or swimming at the surface.
    [[nodiscard]] bool CanJump(BodyState const& body, Body const& shape, WorldQuery const& world);
    /// Whether the body's height is the policy's to change (ascend / descend / pitch): swimming or flying, or on the
    /// ground with flight to take off into.
    [[nodiscard]] bool CanSteerVertically(BodyState const& body, Speeds const& speeds);

    /// The MOVEMENTFLAG_* bits (the core's MovementInfo flags, the client's mover +0x44) of a body under controls.
    [[nodiscard]] uint32_t MovementFlags(BodyState const& body, ControlState const& control, Speeds const& speeds);

    // MovementInfo flag bits, the core's values (Entities/Object/MovementInfo.h), mirrored so this stays core-free.
    namespace Flag
    {
        constexpr uint32_t FORWARD = 0x00000001;
        constexpr uint32_t BACKWARD = 0x00000002;
        constexpr uint32_t STRAFE_LEFT = 0x00000004;
        constexpr uint32_t STRAFE_RIGHT = 0x00000008;
        constexpr uint32_t LEFT = 0x00000010;
        constexpr uint32_t RIGHT = 0x00000020;
        constexpr uint32_t PITCH_UP = 0x00000040;
        constexpr uint32_t PITCH_DOWN = 0x00000080;
        constexpr uint32_t WALKING = 0x00000100;
        constexpr uint32_t ROOT = 0x00000800;
        constexpr uint32_t FALLING = 0x00001000;
        constexpr uint32_t SWIMMING = 0x00200000;
        constexpr uint32_t ASCENDING = 0x00400000;
        constexpr uint32_t DESCENDING = 0x00800000;
        constexpr uint32_t CAN_FLY = 0x01000000;
        constexpr uint32_t FLYING = 0x02000000;
        constexpr uint32_t WATERWALKING = 0x10000000;
        constexpr uint32_t FALLING_SLOW = 0x20000000;
        constexpr uint32_t HOVER = 0x40000000;
    }
}

#endif
