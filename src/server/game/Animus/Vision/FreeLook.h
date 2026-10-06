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

#ifndef ANIMUS_VISION_FREE_LOOK_H
#define ANIMUS_VISION_FREE_LOOK_H

#include "Camera.h"
#include "VisionCaster.h"
#include <algorithm>
#include <array>
#include <cstdint>

/// **Free look** (camera-vision.FREELOOK.md B): the seat turns its own camera, as a player does with the mouse. The
/// look head chooses three things every decision -- a yaw rate, a pitch rate and a zoom step -- which are held, as
/// the move block's controls are, and the camera integrates them at the next observation by the decision's length.
///
/// The camera is the client's alone: its state lives beside the seat's controls (SeatState::Look), never in the body
/// or the server's facing, and sends no packet. Looking is free (FREELOOK R1): a look choice never reaches
/// ApplySeatAction, so nothing that prices, tallies or judges an action ever sees it. Every choice is always allowed;
/// there is no look mask. Pure -- no core types -- so it is tested on its own (VisionTest).
namespace Animus::Vision::FreeLook
{
    /// The look head's three categoricals, in the wire's order (ACT's look section, SPEC's LookHeads).
    constexpr uint32_t HEADS = 3;
    enum Head : uint32_t
    {
        HEAD_YAW_RATE = 0,
        HEAD_PITCH_RATE,
        HEAD_ZOOM,
    };

    /// Yaw rates, degrees a second, + left (counter-clockwise, WoW's yaw).
    constexpr uint32_t YAW_RATE_COUNT = 7;
    constexpr std::array<float, YAW_RATE_COUNT> YAW_RATES_DEG = { -180.0f, -90.0f, -30.0f, 0.0f, 30.0f, 90.0f,
        180.0f };
    constexpr uint32_t YAW_STOP_INDEX = 3;
    /// Pitch rates, degrees a second, + up.
    constexpr uint32_t PITCH_RATE_COUNT = 5;
    constexpr std::array<float, PITCH_RATE_COUNT> PITCH_RATES_DEG = { -60.0f, -20.0f, 0.0f, 20.0f, 60.0f };
    constexpr uint32_t PITCH_STOP_INDEX = 2;
    /// The zoom head: hold; in or out one level (clamped); recentre (yaw offset 0, the conf's pitch, both rates let
    /// go; the zoom is kept).
    enum Zoom : uint32_t
    {
        ZOOM_HOLD = 0,
        ZOOM_IN,
        ZOOM_OUT,
        ZOOM_RECENTRE,
        ZOOM_CHOICES
    };

    constexpr std::array<uint32_t, HEADS> HEAD_SIZES = { YAW_RATE_COUNT, PITCH_RATE_COUNT, ZOOM_CHOICES };
    constexpr std::array<char const*, HEADS> HEAD_NAMES = { "yaw_rate", "pitch_rate", "zoom" };
    /// What a row that chooses nothing holds: both rates let go, the zoom held. The pool's look buffer starts so, and
    /// a local policy (random, a scripted baseline) leaves its agents' rows so, rather than at index 0 (-180 deg/s).
    constexpr std::array<int32_t, HEADS> NEUTRAL = { int32_t(YAW_STOP_INDEX), int32_t(PITCH_STOP_INDEX),
        int32_t(ZOOM_HOLD) };

    /// The zoom levels, yards from the pivot back to the camera (0: from the eyes).
    constexpr uint32_t ZOOM_LEVEL_COUNT = 4;
    constexpr std::array<float, ZOOM_LEVEL_COUNT> ZOOM_LEVELS = { 0.0f, 3.0f, 6.0f, 12.0f };
    /// The pitch's limits, either way.
    constexpr float PITCH_LIMIT = 80.0f * DEGREES;
    /// Follow mode: the yaw offset eases back toward 0 at this rate while the forward key is held and the yaw rate
    /// is let go.
    constexpr float FOLLOW_RATE = 180.0f * DEGREES;

    /// A seat's camera (SeatState::Look), reset at every episode.
    struct State
    {
        float YawOffset = 0.0f;     // radians from the facing, (-pi, pi], + left
        float Pitch = 0.0f;         // radians, [-PITCH_LIMIT, PITCH_LIMIT], + up
        uint32_t ZoomLevel = 2;     // index into ZOOM_LEVELS
        float YawRate = 0.0f;       // radians a second, held until the look head changes it
        float PitchRate = 0.0f;
        /// The episode's first observation has been made: before it, the camera does not advance (dt = 0).
        bool Observed = false;
        /// The size this episode's frames are cast at (DrawRenderSize); 0 x 0 casts at the canonical size.
        Resolution Render;
    };

    /// An angle wrapped to (-pi, pi].
    [[nodiscard]] inline float Wrap(float angle)
    {
        float wrapped = std::remainder(angle, 2.0f * PI);
        if (wrapped <= -PI)
            wrapped += 2.0f * PI;
        return wrapped;
    }

    /// The level nearest `yards` (the lower on a tie).
    [[nodiscard]] inline uint32_t NearestZoomLevel(float yards)
    {
        uint32_t best = 0;
        for (uint32_t level = 1; level < ZOOM_LEVEL_COUNT; ++level)
            if (std::fabs(ZOOM_LEVELS[level] - yards) < std::fabs(ZOOM_LEVELS[best] - yards))
                best = level;
        return best;
    }

    /// The camera at an episode's start: yaw offset 0, the conf's pitch, the zoom level nearest the conf's zoom,
    /// both rates let go, not yet observed. The render size is the caller's to draw (it keeps the one given).
    inline void Reset(State& state, Settings const& settings, Resolution render = {})
    {
        state = State();
        state.Pitch = std::clamp(settings.Pitch * DEGREES, -PITCH_LIMIT, PITCH_LIMIT);
        state.ZoomLevel = NearestZoomLevel(settings.Zoom);
        state.Render = render;
    }

    /// Whether every value of a row is within its head.
    [[nodiscard]] inline bool Valid(int32_t const* choice)
    {
        for (uint32_t head = 0; head < HEADS; ++head)
            if (choice[head] < 0 || uint32_t(choice[head]) >= HEAD_SIZES[head])
                return false;
        return true;
    }

    /// The look head's choice, taken with the movement actions (ApplyActions): the rates are held from now, the zoom
    /// steps now; so it shows in the next frame. A row out of range is left alone (ACT refuses one before it gets
    /// here). Recentre lets go of both rates, whatever this choice held.
    inline void Apply(State& state, int32_t const* choice, Settings const& settings)
    {
        if (!Valid(choice))
            return;
        state.YawRate = YAW_RATES_DEG[choice[HEAD_YAW_RATE]] * DEGREES;
        state.PitchRate = PITCH_RATES_DEG[choice[HEAD_PITCH_RATE]] * DEGREES;
        switch (uint32_t(choice[HEAD_ZOOM]))
        {
            case ZOOM_IN:
                state.ZoomLevel = state.ZoomLevel > 0 ? state.ZoomLevel - 1 : 0;
                break;
            case ZOOM_OUT:
                state.ZoomLevel = std::min(state.ZoomLevel + 1, ZOOM_LEVEL_COUNT - 1);
                break;
            case ZOOM_RECENTRE:
                state.YawOffset = 0.0f;
                state.Pitch = std::clamp(settings.Pitch * DEGREES, -PITCH_LIMIT, PITCH_LIMIT);
                state.YawRate = 0.0f;
                state.PitchRate = 0.0f;
                break;
            default:
                break;
        }
    }

    /// The held rates over `dt` seconds: the yaw offset turns (wrapped), the pitch turns (clamped); then follow mode,
    /// while `forwardHeld` (the forward key in the seat's controls, never the body's motion: FREELOOK R2) and the yaw
    /// rate is let go, eases the yaw offset toward 0 at FOLLOW_RATE without passing it.
    inline void Integrate(State& state, float dt, bool forwardHeld)
    {
        if (dt <= 0.0f)
            return;
        state.YawOffset = Wrap(state.YawOffset + state.YawRate * dt);
        state.Pitch = std::clamp(state.Pitch + state.PitchRate * dt, -PITCH_LIMIT, PITCH_LIMIT);
        if (forwardHeld && state.YawRate == 0.0f)
        {
            float const step = FOLLOW_RATE * dt;
            state.YawOffset = std::fabs(state.YawOffset) <= step ? 0.0f
                : state.YawOffset - std::copysign(step, state.YawOffset);
        }
    }

    /// An observation: the camera advances by the decision's length, except at the episode's first (dt = 0).
    inline void Advance(State& state, float decisionSeconds, bool forwardHeld)
    {
        Integrate(state, state.Observed ? decisionSeconds : 0.0f, forwardHeld);
        state.Observed = true;
    }

    /// What the renderer takes.
    [[nodiscard]] inline CameraState CameraOf(State const& state)
    {
        CameraState camera;
        camera.YawOffset = state.YawOffset;
        camera.Pitch = state.Pitch;
        camera.Zoom = ZOOM_LEVELS[std::min(state.ZoomLevel, ZOOM_LEVEL_COUNT - 1)];
        camera.YawRate = state.YawRate;
        camera.PitchRate = state.PitchRate;
        camera.RenderWidth = state.Render.Width;
        camera.RenderHeight = state.Render.Height;
        return camera;
    }
}

#endif
