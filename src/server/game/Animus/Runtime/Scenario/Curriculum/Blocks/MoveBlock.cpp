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

#include "MoveBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "MapWorldQuery.h"
#include "SeatView.h"
#include "UnitBody.h"
#include "Map.h"
#include "MapDefines.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <algorithm>
#include <cmath>

namespace
{
    using Animus::Curriculum::MoveBlock;
    namespace Encoding = Animus::Curriculum::Encoding;
    namespace MC = Animus::Curriculum::MoveControls;
    namespace Mv = Animus::Movement;

    constexpr float YARD_SCALE = 40.0f;         // distances are reported as a fraction of this
    constexpr float RUN_SPEED = 7.0f;           // yards a second, unmounted and unhasted (TravelBlock's)

    /// `to`'s direction in the seat's own frame: 0 straight ahead, wrapped to (-pi, pi].
    float RelativeBearing(Position const& from, float facing, WorldObject const& to)
    {
        float const relative = from.GetAngle(to.GetPositionX(), to.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }

    /// Off the ground, where the third dimension is real and pitch steers: swimming, or flying.
    bool Airborne(Player const* bot)
    {
        return bot && (bot->IsInWater() || bot->CanFly());
    }

    /// Take a trail sample when one is due, then write the trail into the block's row: each sample as an offset
    /// from where the seat stands now, in its own frame (ahead, left) over YARD_SCALE, oldest first with the newest
    /// in the last pair, then the share of the samples it is still within DWELL_YARDS of. The first observation of
    /// an episode takes the first sample, so the trail always knows where the seat set out from.
    void ObserveTrail(Animus::Curriculum::SeatView const& view, Position const& self, float* out)
    {
        using Animus::Curriculum::MovementTrail;
        using Animus::Curriculum::TRAIL_SAMPLES;

        MovementTrail* trail = view.Trail;
        if (!trail)
            return;

        if (!trail->Started || view.NowMs < trail->LastMs
            || view.NowMs - trail->LastMs >= MovementTrail::INTERVAL_MS)
        {
            trail->X[trail->Next] = self.GetPositionX();
            trail->Y[trail->Next] = self.GetPositionY();
            trail->Next = (trail->Next + 1) % TRAIL_SAMPLES;
            trail->Count = std::min(trail->Count + 1, TRAIL_SAMPLES);
            trail->LastMs = view.NowMs;
            trail->Started = true;
        }

        float const cosFacing = std::cos(view.Facing);
        float const sinFacing = std::sin(view.Facing);
        uint32 dwelling = 0;
        for (uint32 i = 0; i < trail->Count; ++i)
        {
            // Oldest first: with the ring full, the oldest sample is the slot Next points at.
            uint32 const slot = (trail->Next + TRAIL_SAMPLES - trail->Count + i) % TRAIL_SAMPLES;
            float const dx = trail->X[slot] - self.GetPositionX();
            float const dy = trail->Y[slot] - self.GetPositionY();
            // Into the seat's frame: ahead is +x and left is +y, orientation running counter-clockwise.
            float const ahead = dx * cosFacing + dy * sinFacing;
            float const left = dy * cosFacing - dx * sinFacing;
            uint32 const feature = MoveBlock::OBS_TRAIL_FIRST + 2 * (TRAIL_SAMPLES - trail->Count + i);
            out[feature] = std::clamp(ahead / YARD_SCALE, -1.0f, 1.0f);
            out[feature + 1] = std::clamp(left / YARD_SCALE, -1.0f, 1.0f);
            if (dx * dx + dy * dy <= MovementTrail::DWELL_YARDS * MovementTrail::DWELL_YARDS)
                ++dwelling;
        }
        out[MoveBlock::OBS_TRAIL_DWELL] = float(dwelling) / float(TRAIL_SAMPLES);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::MoveBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, MC::ACTION_COUNT };
}

void Animus::Curriculum::MoveBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["controls"] = "player-controller";
    block["trail_samples"] = uint32(TRAIL_SAMPLES);
    block["trail_interval_ms"] = uint32(MovementTrail::INTERVAL_MS);
    boost::json::array turns;
    for (float rate : MC::TURN_RATES_DEG)
        turns.push_back(double(rate));
    block["turn_rates_deg"] = std::move(turns);
    boost::json::array pitches;
    for (float rate : MC::PITCH_RATES_DEG)
        pitches.push_back(double(rate));
    block["pitch_rates_deg"] = std::move(pitches);
    block["turn_rate_max"] = double(MC::TURN_RATE_MAX);
    block["pitch_rate_max"] = double(MC::PITCH_RATE_MAX);
    block["jump_speed"] = double(Mv::JUMP_SPEED);
    block["swim_jump_speed"] = double(Mv::SWIM_JUMP_SPEED);
    block["step_up"] = double(Mv::STEP_UP);
    block["fall_time_scale_ms"] = double(FALL_TIME_SCALE_MS);
    block["fall_height_scale"] = double(FALL_HEIGHT_SCALE);
    // No ground rays, flight rays or clearance since revision 4: the camera (the vision block) is how a seat sees.
    block["ground_probe"] = "none";
    // The contact-side columns' scales (revision 6).
    block["hold_age_scale_s"] = double(HOLD_AGE_SCALE_S);
    block["pinned_age_scale_s"] = double(PINNED_AGE_SCALE_S);
}

std::string Animus::Curriculum::MoveBlock::ColumnName(uint32 column)
{
    static char const* const MODES[OBS_MODE_COUNT] = { "mode_ground", "mode_falling", "mode_swimming", "mode_flying" };
    switch (column)
    {
        case OBS_MOVING:                return "moving";
        case OBS_SPEED:                 return "speed";
        case OBS_FACING_SIN:            return "facing_sin";
        case OBS_FACING_COS:            return "facing_cos";
        case OBS_PITCH_SIN:             return "pitch_sin";
        case OBS_PITCH_COS:             return "pitch_cos";
        case OBS_HELD_FORWARD:          return "held_forward";
        case OBS_HELD_STRAFE:           return "held_strafe";
        case OBS_HELD_VERTICAL:         return "held_vertical";
        case OBS_HELD_TURN:             return "held_turn";
        case OBS_HELD_PITCH:            return "held_pitch";
        case OBS_HELD_WALK:             return "held_walk";
        case OBS_VELOCITY_AHEAD:        return "velocity_ahead";
        case OBS_VELOCITY_LEFT:         return "velocity_left";
        case OBS_VELOCITY_UP:           return "velocity_up";
        case OBS_PROGRESS:              return "progress";
        case OBS_FALL_TIME:             return "fall_time";
        case OBS_FALL_HEIGHT:           return "fall_height";
        case OBS_AGAINST_WALL:          return "against_wall";
        case OBS_STEEP_SLOPE:           return "steep_slope";
        case OBS_DEPTH:                 return "depth";
        case OBS_CAN_JUMP:              return "can_jump";
        case OBS_TARGET_BEARING_SIN:    return "target_bearing_sin";
        case OBS_TARGET_BEARING_COS:    return "target_bearing_cos";
        case OBS_TARGET_DISTANCE:       return "target_distance";
        case OBS_HAZARD_BEARING_SIN:    return "hazard_bearing_sin";
        case OBS_HAZARD_BEARING_COS:    return "hazard_bearing_cos";
        case OBS_HAZARD_DISTANCE:       return "hazard_distance";
        case OBS_HAZARD_RADIUS:         return "hazard_radius";
        case OBS_IN_WATER:              return "in_water";
        case OBS_SUBMERGED:             return "submerged";
        case OBS_SUBMERGED_TIME:        return "submerged_time";
        case OBS_SWIM_SPEED:            return "swim_speed";
        case OBS_AIRBORNE:              return "airborne";
        case OBS_MOVE_RATE:             return "move_rate";
        case OBS_CLOSE_RATE:            return "close_rate";
        case OBS_TRAIL_DWELL:           return "trail_dwell";
        case OBS_CONTACT_SIDE:          return "contact_side";
        case OBS_BLOCKED_AHEAD:         return "blocked_ahead";
        case OBS_HOLD_AGE:              return "hold_age";
        case OBS_PINNED_AGE:            return "pinned_age";
        default:                        break;
    }
    if (column >= OBS_MODE_FIRST && column < OBS_MODE_FIRST + OBS_MODE_COUNT)
        return MODES[column - OBS_MODE_FIRST];
    if (column >= OBS_TRAIL_FIRST && column < OBS_TRAIL_DWELL)
    {
        uint32 const sample = (column - OBS_TRAIL_FIRST) / 2;
        return "trail_" + std::to_string(sample) + ((column - OBS_TRAIL_FIRST) % 2 ? "_left" : "_ahead");
    }
    return {};
}

void Animus::Curriculum::MoveBlock::DescribeColumns(Layout const& /*layout*/, boost::json::array& names) const
{
    for (uint32 column = 0; column < OBS_COUNT; ++column)
        names.emplace_back(ColumnName(column));
}

std::string Animus::Curriculum::MoveBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    return local < MC::ACTION_COUNT ? MC::NAMES[local] : std::string();
}

void Animus::Curriculum::MoveBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    // `obs` and `mask` are already this block's own slice of the seat's row: SeatEncoder::Observe offsets them
    // before it calls a block. Offsetting again wrote the whole block past the end of its slice, so every bearing
    // stayed masked and no seat could steer (stage1_move, 2026-09-21).
    float* out = obs;
    Player* bot = view.Bot;
    Mv::BodyState const* body = view.Body;
    Mv::ControlState const* held = view.Controls ? &view.Controls->Held : nullptr;
    bool const airborne = Airborne(bot);
    // Where the seat is, for everything it senses of itself: the controller's true body, as a client knows its own
    // position (§5A.1 point 1); the server's (others read it) lags it by up to a report.
    Position const self = body ? Position(body->X, body->Y, body->Z, body->Yaw)
        : bot ? bot->GetPosition() : Position();

    // Everything a seat needs to place its feet, and nothing about whether it has an enemy: this block is the one
    // that still works when there is nothing to fight.
    if (bot)
    {
        // Every bearing in this block is measured off the seat's own heading.
        float const facing = view.Facing;
        bool const keys = held && (held->Forward || held->Strafe || held->Vertical);
        float const speed = body ? std::sqrt(body->Vx * body->Vx + body->Vy * body->Vy + body->Vz * body->Vz) : 0.0f;
        out[OBS_MOVING] = keys || speed > 0.1f || bot->isMoving() ? 1.0f : 0.0f;
        // Over twice the unhasted run speed and clamped: a mounted seat read 2.0 here and a flying one nearly 4,
        // which are not features in [0, 1] and were the widest inputs the row had.
        out[OBS_SPEED] = std::min(1.0f, bot->GetSpeed(MOVE_RUN) / (2.0f * RUN_SPEED));
        out[OBS_FACING_SIN] = std::sin(facing);
        out[OBS_FACING_COS] = std::cos(facing);

        if (held)
        {
            out[OBS_HELD_FORWARD] = float(held->Forward);
            out[OBS_HELD_STRAFE] = float(held->Strafe);
            out[OBS_HELD_VERTICAL] = float(held->Vertical);
            out[OBS_HELD_TURN] = std::clamp(held->TurnRate / MC::TURN_RATE_MAX, -1.0f, 1.0f);
            out[OBS_HELD_PITCH] = std::clamp(held->PitchRate / MC::PITCH_RATE_MAX, -1.0f, 1.0f);
            out[OBS_HELD_WALK] = held->Walk ? 1.0f : 0.0f;
        }

        if (body)
        {
            out[OBS_PITCH_SIN] = std::sin(body->Pitch);
            out[OBS_PITCH_COS] = std::cos(body->Pitch);
            // Into the body's frame: ahead along the yaw, left counter-clockwise of it.
            float const scale = 2.0f * RUN_SPEED;
            float const ahead = body->Vx * std::cos(body->Yaw) + body->Vy * std::sin(body->Yaw);
            float const left = body->Vy * std::cos(body->Yaw) - body->Vx * std::sin(body->Yaw);
            out[OBS_VELOCITY_AHEAD] = std::clamp(ahead / scale, -1.0f, 1.0f);
            out[OBS_VELOCITY_LEFT] = std::clamp(left / scale, -1.0f, 1.0f);
            out[OBS_VELOCITY_UP] = std::clamp(body->Vz / scale, -1.0f, 1.0f);
            out[OBS_PROGRESS] = body->Commanded > 1e-4f ? std::clamp(body->Moved / body->Commanded, 0.0f, 1.0f)
                : 1.0f;
            out[OBS_MODE_FIRST + uint32(body->Kind)] = 1.0f;
            out[OBS_FALL_TIME] = body->Kind == Mv::Mode::Falling
                ? std::min(1.0f, float(body->FallMs) / FALL_TIME_SCALE_MS) : 0.0f;
            out[OBS_FALL_HEIGHT] = body->Kind == Mv::Mode::Falling
                ? std::clamp((body->FallApexZ - body->Z) / FALL_HEIGHT_SCALE, 0.0f, 1.0f) : 0.0f;
            out[OBS_AGAINST_WALL] = body->AgainstWall ? 1.0f : 0.0f;
            out[OBS_STEEP_SLOPE] = body->SteepSlope ? 1.0f : 0.0f;
            out[OBS_CONTACT_SIDE] = float(body->ContactSide);
            out[OBS_BLOCKED_AHEAD] = std::clamp(body->BlockedShare, 0.0f, 1.0f);
        }
        // How long the hands have been where they are, and how long the keys have got nowhere (the scenario's Stuck
        // run, SeatView::PinnedMs).
        if (view.Controls)
            out[OBS_HOLD_AGE] = std::min(1.0f, float(view.NowMs - std::min<uint64>(view.NowMs,
                view.Controls->ChangedMs)) / (1000.0f * HOLD_AGE_SCALE_S));
        out[OBS_PINNED_AGE] = std::min(1.0f, float(view.PinnedMs) / (1000.0f * PINNED_AGE_SCALE_S));

        // How deep the feet are, the core's own liquid query at the body.
        if (Map* map = bot->GetMap())
        {
            LiquidData const& liquid = map->GetLiquidData(bot->GetPhaseMask(), self.GetPositionX(),
                self.GetPositionY(), self.GetPositionZ(), bot->GetCollisionHeight(), MAP_ALL_LIQUIDS);
            if (liquid.Status != LIQUID_MAP_NO_WATER)
                out[OBS_DEPTH] = std::clamp((liquid.Level - self.GetPositionZ())
                    / std::max(0.1f, bot->GetCollisionHeight()), 0.0f, 1.0f);
        }

        if (Unit const* target = view.Target)
        {
            float const relative = RelativeBearing(self, facing, *target);
            out[OBS_TARGET_BEARING_SIN] = std::sin(relative);
            out[OBS_TARGET_BEARING_COS] = std::cos(relative);
            out[OBS_TARGET_DISTANCE] = std::min(1.0f, self.GetExactDist2d(target) / YARD_SCALE);
        }
        // The nearest ground effect it is not standing in, in the same frame: which way it lies and how wide, so
        // the seat can walk round one rather than only out of one. Its bearing is already relative to facing.
        if (view.NearestHazard.Present)
        {
            out[OBS_HAZARD_BEARING_SIN] = std::sin(view.NearestHazard.Bearing);
            out[OBS_HAZARD_BEARING_COS] = std::cos(view.NearestHazard.Bearing);
            out[OBS_HAZARD_DISTANCE] = std::min(1.0f, view.NearestHazard.Distance / YARD_SCALE);
            out[OBS_HAZARD_RADIUS] = std::min(1.0f, view.NearestHazard.Radius / YARD_SCALE);
        }

        out[OBS_IN_WATER] = bot->IsInWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED] = bot->IsUnderWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED_TIME] = std::min(1.0f, view.BreathSpent);
        out[OBS_SWIM_SPEED] = bot->GetSpeed(MOVE_SWIM) / RUN_SPEED;
        out[OBS_AIRBORNE] = airborne ? 1.0f : 0.0f;
    }

    // Whether the legs are getting anywhere, over the last second: the scenario's to measure, since neither can be
    // seen from where the seat stands. (The way round against the way through is the compass block's.)
    out[OBS_MOVE_RATE] = std::clamp(view.MoveRate, 0.0f, 1.0f);
    out[OBS_CLOSE_RATE] = std::clamp(view.CloseRate, -1.0f, 1.0f);

    // Where it has been, in its own frame, sampled here once a second because this is the one place that runs
    // for every seat every decision, in training and in play alike.
    if (bot)
        ObserveTrail(view, self, out);

    // The masks: only the presses that are physically impossible (MoveControls::Allowed). The jump and the vertical
    // controls read the body the controller moves.
    MC::MaskState state;
    state.Alive = bot && bot->IsAlive();
    if (bot && body)
    {
        Mv::Speeds const speeds = Mv::SpeedsOf(bot);
        state.CanSteerVertically = Mv::CanSteerVertically(*body, speeds);
        if (Map* map = bot->GetMap())
        {
            Mv::MapWorldQuery const world(map, bot->GetPhaseMask());
            state.CanJump = Mv::CanJump(*body, Mv::ShapeOf(bot), world);
        }
    }
    out[OBS_CAN_JUMP] = state.Alive && state.CanJump ? 1.0f : 0.0f;

    if (!mask)
        return;
    for (uint32 action = 0; action < MC::ACTION_COUNT; ++action)
        mask[action] = MC::Allowed(action, state) ? 1 : 0;
}

void Animus::Curriculum::MoveBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    Player* bot = view.Bot;
    if (!bot || !view.Controls || local >= MC::ACTION_COUNT)
        return;

    // One held control changed (MoveControls::Press), priced by what it takes back of a recent choice. The value
    // already held, pressed again, is the key kept down: no press, no charge (SeatActionResult::KeyStillHeld).
    MC::PressOutcome const pressed = MC::Press(*view.Controls, local, view.NowMs, view.Options.JitterDecayMs);
    if (!pressed.Changed)
    {
        result.KeyStillHeld = true;
        return;
    }
    result.ControlChanged = true;
    result.JitterWeight += pressed.JitterWeight;
    result.BearingFlip += pressed.FeetFlip;
    result.TurnReversals += pressed.TurnReversals;
    result.PitchReversals += pressed.PitchReversals;
    result.Weaves += pressed.Weaves;
    result.EffortWeight = pressed.Effort;
}
