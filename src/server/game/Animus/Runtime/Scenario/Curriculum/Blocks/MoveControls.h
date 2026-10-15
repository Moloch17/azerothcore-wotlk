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

/// **The move block's action space** (player-controller plan §4, C3): a player's keys and pitch, held until changed.
/// Each action changes one control of the seat's ControlState; the player controller (Animus/Movement) turns the held
/// controls into motion every world tick. Pure -- no core types -- so the presses, the masks and their pricing are
/// tested on their own (MoveControlsTest), and MoveBlock is the thin part that reads a Player.
namespace Animus::Curriculum::MoveControls
{
    /// The move block's layout revision (Block::Revision). 0 and 1 were the bearing / turn-lattice design, which is
    /// gone (§0); a checkpoint or a manifest of either describes a model that no longer fits, and is refused on resume
    /// and seeded fresh (bootstrap: a block whose revision differs starts from nothing). 3 (never shipped): the
    /// ground probe measured live, the clearance columns gone. 4: no ground rays, flight rays or clearance at all --
    /// the camera (the vision block, free look) is how a seat sees; the probe they came from is gone. 5: the objective's
    /// presence, bearing and distances left for the compass block (perception-goals P1): a stage without the
    /// compass is not told where its objective is. Seeding from 4 maps the kept columns by name (bootstrap). 6: the
    /// contact-side columns contact_side, blocked_ahead, hold_age and pinned_age appended (movement pacing M1,
    /// decision 0027); the earlier columns carry by name, the four start at zero. 7: walk_toggle removed (the user,
    /// 2026-10-14: "walking speed should never be an option"); it was the last action, so the other 24 keep their
    /// indexes and carry by name, and the walk state stays at run. 8: the nine turn-rate actions (turn_stop with
    /// them) removed (heading.CONTRACT.md, 2026-10-15): the body's heading is the policy's continuous output, an
    /// angle from the facing the controller eases the body to (ControlState::TargetYaw), because held turn rates were
    /// held for good and nav1's seats spun rather than run. The pitch actions and everything after them move down
    /// nine indexes; held_turn's column is target_remaining_sin, and target_remaining_cos is appended.
    constexpr uint32_t REVISION = 8;

    /// Pitch rates, degrees a second, + up (water and air). 0 lets go.
    constexpr uint32_t PITCH_COUNT = 5;
    constexpr std::array<float, PITCH_COUNT> PITCH_RATES_DEG = { -90.0f, -30.0f, 0.0f, 30.0f, 90.0f };
    constexpr uint32_t PITCH_STOP_INDEX = 2;
    constexpr float DEG = 0.017453292f;
    /// The fastest pitch rate, for the observation's scale. Turning has no rate any more (revision 8): the heading is
    /// an angle (Movement::HEADING_*).
    constexpr float PITCH_RATE_MAX = 90.0f * DEG;

    enum Action : uint32_t
    {
        ACTION_MOVE_FORWARD = 0,
        ACTION_MOVE_BACK,
        ACTION_MOVE_STOP,
        ACTION_STRAFE_LEFT,
        ACTION_STRAFE_RIGHT,
        ACTION_STRAFE_STOP,
        ACTION_PITCH_FIRST,
        ACTION_PITCH_STOP = ACTION_PITCH_FIRST + PITCH_STOP_INDEX,
        ACTION_ASCEND = ACTION_PITCH_FIRST + PITCH_COUNT,
        ACTION_DESCEND,
        ACTION_VERTICAL_STOP,
        ACTION_JUMP,
        ACTION_COUNT
    };
    static_assert(ACTION_COUNT == 15, "the plan's 25 actions (§4) less walk_toggle (revision 7) and the nine turn "
        "rates (revision 8)");

    constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "move_forward", "move_back", "move_stop", "strafe_left", "strafe_right", "strafe_stop",
        "pitch_down_90", "pitch_down_30", "pitch_stop", "pitch_up_30", "pitch_up_90",
        "ascend", "descend", "vertical_stop", "jump",
    };

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
        float LastPitch = 0.0f;
        uint64_t PitchMs = 0;
        /// When any control last changed (a press that took: Press's Changed), for the move block's hold_age.
        uint64_t ChangedMs = 0;

        void Clear() { *this = SeatControls(); }
    };

    /// What a press did and what it costs (SeatActionResult's steering columns).
    struct PressOutcome
    {
        bool Changed = false;           // false: the value already held, pressed again -- a no-op, not charged
        float JitterWeight = 0.0f;      // quarter turns' worth taken back, weighed by recency (Actions.Jitter)
        float FeetFlip = 0.0f;          // forward/back or left/right reversed within MovePrice::COUNT_MS
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
    /// the feet (forward to back, left to right) or of a climb is half a turn, two quarters; of a pitch rate, the
    /// smaller rate over a quarter turn a second. JUMP is one-shot (the controller clears it when it takes it). The
    /// heading is no press (revision 8): it is set every decision (StageScenario::ApplyContinuous), so a new angle is
    /// not a key changed, and what it costs is the turning the body actually does (PointNav.Turning).
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
            default:
                if (IsPitch(action))
                    rate(held.PitchRate, PITCH_RATES_DEG[action - ACTION_PITCH_FIRST] * DEG, controls.LastPitch,
                        controls.PitchMs, out.PitchReversals, PITCH_RATE_MAX);
                break;
        }
        if (out.Changed)
            controls.ChangedMs = nowMs;
        return out;
    }
}

#endif
