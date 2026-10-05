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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_CONTROLS_H
#define ANIMUS_LIB_CURRICULUM_MOVE_CONTROLS_H

#include "MovePrice.h"
#include "PlayerController.h"
#include <array>
#include <cstdint>

/// **The move block's action space** (player-controller plan §4, C3): a player's keys and mouse, held until changed.
/// Each action changes one control of the seat's ControlState; the player controller (Animus/Movement) turns the held
/// controls into motion every world tick. Pure -- no core types -- so the presses, the masks and their pricing are
/// tested on their own (MoveControlsTest), and MoveBlock is the thin part that reads a Player.
namespace Animus::Curriculum::MoveControls
{
    /// The move block's layout revision (Block::Revision). 0 and 1 were the bearing / turn-lattice design, which is
    /// gone (§0); a checkpoint or a manifest of either describes a model that no longer fits, and is refused on resume
    /// and seeded fresh (bootstrap: a block whose revision differs starts from nothing).
    constexpr uint32_t REVISION = 2;

    /// Turn rates, degrees a second, + left (counter-clockwise, the client's yaw direction): the mouse's, not slowed
    /// while moving. 0 lets go of the turn.
    constexpr uint32_t TURN_COUNT = 9;
    constexpr std::array<float, TURN_COUNT> TURN_RATES_DEG = { -360.0f, -180.0f, -90.0f, -30.0f, 0.0f, 30.0f, 90.0f,
        180.0f, 360.0f };
    constexpr uint32_t TURN_STOP_INDEX = 4;
    /// Pitch rates, degrees a second, + up (water and air). 0 lets go.
    constexpr uint32_t PITCH_COUNT = 5;
    constexpr std::array<float, PITCH_COUNT> PITCH_RATES_DEG = { -90.0f, -30.0f, 0.0f, 30.0f, 90.0f };
    constexpr uint32_t PITCH_STOP_INDEX = 2;
    constexpr float DEG = 0.017453292f;
    /// The fastest rate of each, for the observation's scale.
    constexpr float TURN_RATE_MAX = 360.0f * DEG;
    constexpr float PITCH_RATE_MAX = 90.0f * DEG;

    enum Action : uint32_t
    {
        ACTION_MOVE_FORWARD = 0,
        ACTION_MOVE_BACK,
        ACTION_MOVE_STOP,
        ACTION_STRAFE_LEFT,
        ACTION_STRAFE_RIGHT,
        ACTION_STRAFE_STOP,
        ACTION_TURN_FIRST,
        ACTION_TURN_STOP = ACTION_TURN_FIRST + TURN_STOP_INDEX,
        ACTION_PITCH_FIRST = ACTION_TURN_FIRST + TURN_COUNT,
        ACTION_PITCH_STOP = ACTION_PITCH_FIRST + PITCH_STOP_INDEX,
        ACTION_ASCEND = ACTION_PITCH_FIRST + PITCH_COUNT,
        ACTION_DESCEND,
        ACTION_VERTICAL_STOP,
        ACTION_JUMP,
        ACTION_WALK_TOGGLE,
        ACTION_COUNT
    };
    static_assert(ACTION_COUNT == 25, "the plan's 25 actions (§4)");

    constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "move_forward", "move_back", "move_stop", "strafe_left", "strafe_right", "strafe_stop",
        "turn_right_360", "turn_right_180", "turn_right_90", "turn_right_30", "turn_stop",
        "turn_left_30", "turn_left_90", "turn_left_180", "turn_left_360",
        "pitch_down_90", "pitch_down_30", "pitch_stop", "pitch_up_30", "pitch_up_90",
        "ascend", "descend", "vertical_stop", "jump", "walk_toggle",
    };

    [[nodiscard]] constexpr bool IsTurn(uint32_t action)
    {
        return action >= ACTION_TURN_FIRST && action < ACTION_TURN_FIRST + TURN_COUNT;
    }

    [[nodiscard]] constexpr bool IsPitch(uint32_t action)
    {
        return action >= ACTION_PITCH_FIRST && action < ACTION_PITCH_FIRST + PITCH_COUNT;
    }

    /// A press that moves the feet somewhere (judged at the reward by the gap it closed): forward, back, a strafe, a
    /// climb or a dive, a jump. The stops and the walk toggle are neither steps nor aim.
    [[nodiscard]] constexpr bool IsStep(uint32_t action)
    {
        return action == ACTION_MOVE_FORWARD || action == ACTION_MOVE_BACK || action == ACTION_STRAFE_LEFT
            || action == ACTION_STRAFE_RIGHT || action == ACTION_ASCEND || action == ACTION_DESCEND
            || action == ACTION_JUMP;
    }

    /// What the masks need to know of the seat (§4: masking only for presses that are physically impossible).
    struct MaskState
    {
        bool Alive = true;
        bool CanJump = false;               // Movement::CanJump: on the ground, or swimming (at any depth, C0c)
        bool CanSteerVertically = false;    // Movement::CanSteerVertically: swimming, flying, or able to take off
    };

    /// Whether `action` may be pressed. A dead seat presses nothing. Pitch rates, ASCEND and DESCEND do physically
    /// nothing on the ground (ASCEND is never a second jump); JUMP is impossible falling or flying. The stops stay
    /// legal everywhere, since they release a held control. Pressing the value already held is legal and free.
    [[nodiscard]] constexpr bool Allowed(uint32_t action, MaskState const& state)
    {
        if (!state.Alive || action >= ACTION_COUNT)
            return false;
        if ((IsPitch(action) && action != ACTION_PITCH_STOP) || action == ACTION_ASCEND || action == ACTION_DESCEND)
            return state.CanSteerVertically;
        if (action == ACTION_JUMP)
            return state.CanJump;
        return true;
    }

    /// The controls a seat holds, and the memory the jitter charge reads: each axis's last non-zero value and when it
    /// was let go of (or 0 while it is still held). Kept on the seat across decisions (SeatState::Controls).
    struct SeatControls
    {
        Movement::ControlState Held;
        int8_t LastForward = 0;
        uint64_t ForwardMs = 0;
        int8_t LastStrafe = 0;
        uint64_t StrafeMs = 0;
        int8_t LastVertical = 0;
        uint64_t VerticalMs = 0;
        float LastTurn = 0.0f;
        uint64_t TurnMs = 0;
        float LastPitch = 0.0f;
        uint64_t PitchMs = 0;

        void Clear() { *this = SeatControls(); }
    };

    /// What a press did and what it costs (SeatActionResult's steering columns).
    struct PressOutcome
    {
        bool Changed = false;           // false: the value already held, pressed again -- a no-op, not charged
        float JitterWeight = 0.0f;      // quarter turns' worth taken back, weighed by recency (Actions.Jitter)
        float FeetFlip = 0.0f;          // forward/back or left/right reversed within MovePrice::COUNT_MS
        uint32_t TurnReversals = 0;     // ... a turn rate against the last one
        uint32_t PitchReversals = 0;    // ... a pitch rate or a climb against the last one
        uint32_t Weaves = 0;            // any of them 1.5 to 4 s on
        float Effort = 1.0f;            // the share of a full press (Actions.Effort): a rate by its size
    };

    namespace Detail
    {
        /// A held axis changed to `next` from `held`; `last` / `atMs` remember the last non-zero value and when it
        /// was let go. Returns how recently (ms) the value it reverses was held, or ~0 for no reversal.
        template <typename T>
        uint64_t Change(T held, T next, T& last, uint64_t& atMs, uint64_t nowMs, bool& reversed)
        {
            bool const opposite = next != T(0) && last != T(0) && ((next > T(0)) != (last > T(0)));
            reversed = opposite;
            uint64_t const since = held != T(0) ? 0 : nowMs - std::min(nowMs, atMs);
            if (held != T(0) && next != held)
                atMs = nowMs;                   // the held value is let go of now
            if (next != T(0))
                last = next;
            return since;
        }

        inline void Count(PressOutcome& out, uint64_t since, uint32_t& column)
        {
            uint32_t const counted = MovePrice::CountAs(since);
            column += counted == 1 ? 1 : 0;
            out.Weaves += counted == 2 ? 1 : 0;
        }
    }

    /// Apply `action` to the held controls at `nowMs`, pricing a reversal of a recent choice by what it takes back
    /// (in quarter turns, as every steering reversal is: MovePrice) weighed by its recency on `decayMs`. A reversal of
    /// the feet (forward to back, left to right) or of a climb is half a turn, two quarters; of a turn or a pitch rate,
    /// the smaller rate over a quarter turn a second. JUMP is one-shot (the controller clears it when it takes it).
    inline PressOutcome Press(SeatControls& controls, uint32_t action, uint64_t nowMs, uint32_t decayMs)
    {
        PressOutcome out;
        Movement::ControlState& held = controls.Held;
        bool reversed = false;

        auto feet = [&](int8_t& axis, int8_t next, int8_t& last, uint64_t& atMs, bool vertical)
        {
            if (axis == next)
                return;
            uint64_t const since = Detail::Change<int8_t>(axis, next, last, atMs, nowMs, reversed);
            axis = next;
            out.Changed = true;
            if (!reversed)
                return;
            out.JitterWeight += 2.0f * MovePrice::Recency(since, decayMs);
            if (vertical)
                Detail::Count(out, since, out.PitchReversals);
            else
            {
                uint32_t flips = 0;
                Detail::Count(out, since, flips);
                out.FeetFlip += float(flips);
            }
        };

        auto rate = [&](float& axis, float next, float& last, uint64_t& atMs, uint32_t& column, float full)
        {
            if (axis == next)
                return;
            bool const opposite = next != 0.0f && last != 0.0f && ((next > 0.0f) != (last > 0.0f));
            uint64_t const since = axis != 0.0f ? 0 : nowMs - std::min(nowMs, atMs);
            out.Effort = MovePrice::EffortOf(next != 0.0f ? next : axis, full);
            if (opposite)
            {
                out.JitterWeight += std::min(std::fabs(next), std::fabs(last)) / MovePrice::QUARTER_TURN
                    * MovePrice::Recency(since, decayMs);
                Detail::Count(out, since, column);
            }
            if (axis != 0.0f)
                atMs = nowMs;                   // the held rate is let go of now
            if (next != 0.0f)
                last = next;
            axis = next;
            out.Changed = true;
        };

        switch (action)
        {
            case ACTION_MOVE_FORWARD: feet(held.Forward, 1, controls.LastForward, controls.ForwardMs, false); break;
            case ACTION_MOVE_BACK: feet(held.Forward, -1, controls.LastForward, controls.ForwardMs, false); break;
            case ACTION_MOVE_STOP: feet(held.Forward, 0, controls.LastForward, controls.ForwardMs, false); break;
            case ACTION_STRAFE_LEFT: feet(held.Strafe, -1, controls.LastStrafe, controls.StrafeMs, false); break;
            case ACTION_STRAFE_RIGHT: feet(held.Strafe, 1, controls.LastStrafe, controls.StrafeMs, false); break;
            case ACTION_STRAFE_STOP: feet(held.Strafe, 0, controls.LastStrafe, controls.StrafeMs, false); break;
            case ACTION_ASCEND: feet(held.Vertical, 1, controls.LastVertical, controls.VerticalMs, true); break;
            case ACTION_DESCEND: feet(held.Vertical, -1, controls.LastVertical, controls.VerticalMs, true); break;
            case ACTION_VERTICAL_STOP: feet(held.Vertical, 0, controls.LastVertical, controls.VerticalMs, true); break;
            case ACTION_JUMP:
                held.Jump = true;
                out.Changed = true;
                break;
            case ACTION_WALK_TOGGLE:
                held.Walk = !held.Walk;
                out.Changed = true;
                break;
            default:
                if (IsTurn(action))
                    rate(held.TurnRate, TURN_RATES_DEG[action - ACTION_TURN_FIRST] * DEG, controls.LastTurn,
                        controls.TurnMs, out.TurnReversals, TURN_RATE_MAX / 2.0f);
                else if (IsPitch(action))
                    rate(held.PitchRate, PITCH_RATES_DEG[action - ACTION_PITCH_FIRST] * DEG, controls.LastPitch,
                        controls.PitchMs, out.PitchReversals, PITCH_RATE_MAX);
                break;
        }
        return out;
    }
}

#endif
