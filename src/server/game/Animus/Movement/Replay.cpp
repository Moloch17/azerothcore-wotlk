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

#include "Replay.h"
#include "ReportCadence.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace
{
    namespace Mv = Animus::Movement;
    namespace Cap = Animus::Movement::Capture;
    namespace Cd = Animus::Movement::Cadence;
    namespace Rp = Animus::Movement::Replay;

    constexpr uint32_t AIRBORNE = Mv::Flag::FALLING | Mv::Flag::SWIMMING | Mv::Flag::FLYING;
    constexpr uint32_t TRANSLATING = Mv::Flag::FORWARD | Mv::Flag::BACKWARD | Mv::Flag::STRAFE_LEFT
        | Mv::Flag::STRAFE_RIGHT;

    uint32_t Since(Cap::MoveRecord const& from, Cap::MoveRecord const& to)
    {
        return to.ClientMs - from.ClientMs;         // unsigned: a client clock wrapping is still a small step
    }

    float Flat(Cap::MoveRecord const& a, Cap::MoveRecord const& b)
    {
        return std::hypot(b.X - a.X, b.Y - a.Y);
    }

    /// Step `ms` milliseconds in pieces of at most 50 ms, each a whole number of milliseconds: a recording made at a
    /// 50 ms tick is replayed in exactly the steps it was made in.
    void StepMs(Mv::BodyState& body, Mv::ControlState& control, Mv::Speeds const& speeds, Mv::Body const& shape,
        Mv::WorldQuery const& world, uint32_t ms, float& moved, float& commanded, bool& wall)
    {
        uint32_t const pieces = std::max<uint32_t>(1, (ms + 49) / 50);
        for (uint32_t i = 0; i < pieces; ++i)
        {
            uint32_t const piece = uint32_t(uint64_t(ms) * (i + 1) / pieces - uint64_t(ms) * i / pieces);
            Mv::Step(body, control, speeds, shape, world, float(piece) / 1000.0f);
            moved += body.Moved;
            commanded += body.Commanded;
            wall = wall || body.AgainstWall;
        }
    }

    struct Stats
    {
        std::vector<float> Values;
        void Add(float value) { if (std::isfinite(value)) Values.push_back(value); }
        [[nodiscard]] Rp::Measure Make(std::string name, float constant, std::string note, bool largest = false) const
        {
            Rp::Measure m;
            m.Name = std::move(name);
            m.Constant = constant;
            m.Count = uint32_t(Values.size());
            m.Note = std::move(note);
            if (Values.empty())
                return m;
            if (largest)
            {
                m.Mean = *std::max_element(Values.begin(), Values.end());
                return m;
            }
            double sum = 0.0;
            for (float v : Values)
                sum += v;
            m.Mean = float(sum / double(Values.size()));
            double sq = 0.0;
            for (float v : Values)
                sq += (v - m.Mean) * (v - m.Mean);
            m.Sd = Values.size() > 1 ? float(std::sqrt(sq / double(Values.size() - 1))) : 0.0f;
            return m;
        }
    };
}

void Rp::Spread::Fill(std::vector<float> values)
{
    Count = uint32_t(values.size());
    if (values.empty())
        return;
    std::sort(values.begin(), values.end());
    Median = values[values.size() / 2];
    P90 = values[std::min(values.size() - 1, std::size_t(float(values.size()) * 0.9f))];
    Max = values.back();
}

Mv::ControlState Rp::ControlsOf(uint32_t flags, Speeds const& speeds)
{
    ControlState control;
    control.Forward = (flags & Flag::FORWARD) ? 1 : (flags & Flag::BACKWARD) ? -1 : 0;
    control.Strafe = (flags & Flag::STRAFE_RIGHT) ? 1 : (flags & Flag::STRAFE_LEFT) ? -1 : 0;
    control.Vertical = (flags & Flag::ASCENDING) ? 1 : (flags & Flag::DESCENDING) ? -1 : 0;
    control.Walk = (flags & Flag::WALKING) != 0;
    // The turn and pitch keys at the unit's rates; the mouse comes in as each packet's facing.
    if (flags & (Flag::LEFT | Flag::RIGHT))
    {
        control.KeyboardTurn = true;
        control.TurnRate = (flags & Flag::LEFT) ? speeds.TurnRate : -speeds.TurnRate;
    }
    if (flags & (Flag::PITCH_UP | Flag::PITCH_DOWN))
    {
        control.KeyboardTurn = true;
        control.PitchRate = (flags & Flag::PITCH_UP) ? speeds.PitchRate : -speeds.PitchRate;
    }
    return control;
}

std::vector<float> Rp::Follow(std::vector<Capture::MoveRecord> const& moves, std::size_t start, uint32_t untilMs,
    Speeds const& speeds, Body const& shape, WorldQuery const& world, uint32_t gapMs, uint32_t* walls)
{
    std::vector<float> drift;
    if (start >= moves.size())
        return drift;
    Capture::MoveRecord const& first = moves[start];
    BodyState body;
    Resync(body, first.X, first.Y, first.Z, first.O, shape, world);
    body.Pitch = first.Pitch;
    ControlState control;
    for (std::size_t k = start; k < moves.size(); ++k)
    {
        Capture::MoveRecord const& record = moves[k];
        uint32_t dt = 0;
        if (k > start)
        {
            dt = Since(moves[k - 1], record);
            // On to the first packet at or past `untilMs` (a horizon is read at the first packet that reaches it).
            if (dt > gapMs || Since(first, moves[k - 1]) >= untilMs)
                break;
            float moved = 0.0f;
            float commanded = 0.0f;
            bool wall = false;
            StepMs(body, control, speeds, shape, world, dt, moved, commanded, wall);
            // A wall the controller met over an interval the player crossed freely.
            if (walls && wall && commanded > 0.1f && Flat(moves[k - 1], record) > 0.8f * commanded)
                ++*walls;
        }
        drift.push_back(std::sqrt((body.X - record.X) * (body.X - record.X) + (body.Y - record.Y) * (body.Y - record.Y)
            + (body.Z - record.Z) * (body.Z - record.Z)));
        // This packet's keys from here on, and its facing (the mouse).
        bool const jump = record.Opcode == Cd::Op::JUMP || (dt == 0 && control.Jump);
        control = ControlsOf(record.Flags, speeds);
        control.Jump = jump;
        body.Yaw = record.O;
        body.Pitch = record.Pitch;
    }
    return drift;
}

Rp::Report Rp::Run(std::vector<Capture::MoveRecord> const& moves, std::vector<Capture::SpeedsRecord> const& speeds,
    Body const& shape, WorldQuery const& world, Options const& options)
{
    Report report;
    report.Records = uint32_t(moves.size());
    if (moves.empty())
        return report;

    auto speedsAt = [&speeds](Capture::MoveRecord const& record)
    {
        Speeds in;
        for (Capture::SpeedsRecord const& s : speeds)
            if (s.Ms <= record.Ms || &s == &speeds.front())
                in = Capture::ToSpeeds(s, in);
        return in;
    };

    std::map<uint16_t, uint32_t> opcodes;
    for (Capture::MoveRecord const& record : moves)
        ++opcodes[record.Opcode];
    report.Opcodes.assign(opcodes.begin(), opcodes.end());

    // Drift: segments from ground packets, followed for the longest horizon.
    uint32_t const longest = options.HorizonsMs.empty() ? 0
        : *std::max_element(options.HorizonsMs.begin(), options.HorizonsMs.end());
    std::vector<std::vector<float>> all(options.HorizonsMs.size());
    std::vector<std::vector<float>> flat(options.HorizonsMs.size());
    bool started = false;
    uint32_t lastStart = 0;
    for (std::size_t i = 0; i < moves.size(); ++i)
    {
        Capture::MoveRecord const& record = moves[i];
        if ((record.Flags & AIRBORNE) || (started && record.ClientMs - lastStart < options.SegmentEveryMs))
            continue;
        started = true;
        lastStart = record.ClientMs;
        ++report.Segments;
        std::vector<float> const drift = Follow(moves, i, longest, speedsAt(record), shape, world, options.GapMs,
            &report.WallDisagreements);
        for (std::size_t h = 0; h < options.HorizonsMs.size(); ++h)
        {
            bool level = true;
            for (std::size_t j = 0; j < drift.size(); ++j)
            {
                Capture::MoveRecord const& at = moves[i + j];
                level = level && std::fabs(at.Z - record.Z) <= options.FlatYards && !(at.Flags & AIRBORNE);
                if (Since(record, at) >= options.HorizonsMs[h])
                {
                    all[h].push_back(drift[j]);
                    if (level)
                        flat[h].push_back(drift[j]);
                    break;
                }
            }
        }
    }
    for (std::size_t h = 0; h < options.HorizonsMs.size(); ++h)
    {
        Horizon horizon;
        horizon.Ms = options.HorizonsMs[h];
        horizon.All.Fill(all[h]);
        horizon.Flat.Fill(flat[h]);
        report.Drift.push_back(horizon);
    }

    Stats launch;
    Stats swimLaunch;
    Stats gravity;
    Stats airTime;
    std::vector<float> landing;
    Stats heartbeat;
    Stats facing;
    Stats keyboardMoving;
    Stats keyboardStill;
    Stats stepWalked;
    Stats slopeWalked;
    Stats floatDepth;
    Stats verticalShare;
    float steepest = 0.0f;
    for (std::size_t k = 1; k < moves.size(); ++k)
    {
        Capture::MoveRecord const& prev = moves[k - 1];
        Capture::MoveRecord const& record = moves[k];
        uint32_t const dt = Since(prev, record);
        Speeds const unit = speedsAt(record);

        if (!(record.Flags & (Flag::SWIMMING | Flag::FLYING)) && world.InTerrain(record.X, record.Y, record.Z))
            ++report.InTerrain;

        // The jump: its launch speed, and with its landing the gravity and the air time it implies.
        if (record.Opcode == Cd::Op::JUMP)
        {
            ++report.Jumps;
            ((prev.Flags & Flag::SWIMMING) ? swimLaunch : launch).Add(-record.JumpZSpeed);
            for (std::size_t l = k + 1; l < moves.size() && Since(record, moves[l]) < 4000; ++l)
            {
                if (moves[l].Opcode != Cd::Op::FALL_LAND)
                    continue;
                float const v = -record.JumpZSpeed;
                float const t = float(moves[l].FallMs) / 1000.0f;
                float const dz = moves[l].Z - record.Z;
                if (t > 0.1f)
                    gravity.Add(2.0f * (v * t - dz) / (t * t));
                float const disc = v * v - 2.0f * GRAVITY * dz;
                if (disc >= 0.0f && t > 0.1f)
                    airTime.Add(t * 1000.0f - 1000.0f * (v + std::sqrt(disc)) / GRAVITY);
                std::vector<float> const drift = Follow(moves, k, Since(record, moves[l]), unit, shape, world,
                    options.GapMs);
                if (drift.size() == l - k + 1)
                    landing.push_back(drift.back());
                break;
            }
        }

        // The heartbeat: how long after the last packet, while moving.
        if (record.Opcode == Cd::Op::HEARTBEAT && (prev.Flags & Cd::HEARTBEAT_FLAGS))
            heartbeat.Add(float(dt));
        // The mouse-look's facing report: how far the facing had come since the last packet.
        if (record.Opcode == Cd::Op::SET_FACING)
            facing.Add(std::fabs(record.O - prev.O));
        // Keyboard turning: the rate over the unit's, moving and standing.
        uint32_t const turn = Flag::LEFT | Flag::RIGHT;
        if ((prev.Flags & turn) && (prev.Flags & turn) == (record.Flags & turn) && dt >= 100 && unit.TurnRate > 0.0f)
        {
            float const rate = std::fabs(std::remainder(record.O - prev.O, Cd::TWO_PI)) * 1000.0f / float(dt)
                / unit.TurnRate;
            ((prev.Flags & TRANSLATING) ? keyboardMoving : keyboardStill).Add(rate);
        }

        // On foot, between two ground packets with no jump: the rises walked onto, the slopes walked up and down.
        bool const ground = !(prev.Flags & AIRBORNE) && !(record.Flags & AIRBORNE)
            && record.Opcode != Cd::Op::JUMP && prev.Opcode != Cd::Op::FALL_LAND;
        float const run = Flat(prev, record);
        float const rise = record.Z - prev.Z;
        // Within a heartbeat's run (3.5 yd at 7 yd/s), so the rise is one step and not a hill.
        if (ground && run > 0.05f && run < 4.0f && rise > 0.3f)
        {
            ++report.StepUps;
            stepWalked.Add(rise);
            std::vector<float> const drift = Follow(moves, k - 1, dt, unit, shape, world, options.GapMs);
            if (drift.size() == 2 && drift.back() > 0.5f)
                ++report.StepUpsRefused;
        }
        if (ground && run > 1.5f)
        {
            float const angle = std::atan2(rise, run);
            if (rise > 0.0f)
                slopeWalked.Add(angle * 57.29578f);
            if (rise < 0.0f && -rise / run > TAN_WALKABLE)
            {
                ++report.SteepDescents;
                steepest = std::max(steepest, -angle * 57.29578f);
            }
        }

        // Swimming with no vertical key: how deep the feet float (of the body's height).
        if ((record.Flags & Flag::SWIMMING) && !(record.Flags & (Flag::ASCENDING | Flag::DESCENDING)))
        {
            Liquid const liquid = world.LiquidAt(record.X, record.Y, record.Z);
            if (liquid.Present && shape.Height > 0.0f)
                floatDepth.Add((liquid.Level - record.Z) / shape.Height);
        }
        // Ascending or descending alone, swimming or flying: the vertical speed over the speed in force.
        if ((prev.Flags & (Flag::ASCENDING | Flag::DESCENDING)) && !(prev.Flags & TRANSLATING)
            && (prev.Flags & (Flag::SWIMMING | Flag::FLYING)) && dt >= 100)
        {
            float const speed = (prev.Flags & Flag::SWIMMING) ? unit.Swim : unit.Flight;
            if (speed > 0.0f)
                verticalShare.Add(std::fabs(rise) * 1000.0f / float(dt) / speed);
        }
    }
    report.JumpLanding.Fill(landing);

    report.Calibration.push_back(launch.Make("JUMP_SPEED (yd/s)", JUMP_SPEED, "the JUMP packets' launch"));
    report.Calibration.push_back(swimLaunch.Make("SWIM_JUMP_SPEED (yd/s)", SWIM_JUMP_SPEED, "jumps from swimming"));
    {
        Measure apex = launch.Make("jump apex (yd)", JUMP_SPEED * JUMP_SPEED / (2.0f * GRAVITY),
            "v^2 / 2g of the launch measured; target within 0.1 yd");
        apex.Mean = apex.Count ? apex.Mean * apex.Mean / (2.0f * GRAVITY) : 0.0f;
        apex.Sd = 0.0f;
        report.Calibration.push_back(apex);
    }
    report.Calibration.push_back(gravity.Make("GRAVITY (yd/s^2)", GRAVITY, "from each jump's launch, fall and drop"));
    report.Calibration.push_back(airTime.Make("jump air time error (ms)", 0.0f, "landed minus predicted"));
    report.Calibration.push_back(stepWalked.Make("STEP_UP (yd)", STEP_UP,
        "the highest rise walked onto without a jump (a lower bound)", true));
    report.Calibration.push_back(slopeWalked.Make("walkable slope (deg)", 50.0f,
        "the steepest slope walked up over 1.5 yd or more", true));
    {
        Measure steep;
        steep.Name = "steep descents (deg)";
        steep.Constant = 50.0f;
        steep.Mean = steepest;
        steep.Count = report.SteepDescents;
        steep.Note = "walked down faces past 50 degrees without falling (C6 a)";
        report.Calibration.push_back(steep);
    }
    report.Calibration.push_back(heartbeat.Make("HEARTBEAT_MS", float(Cd::HEARTBEAT_MS),
        "a heartbeat after the last packet, while moving"));
    report.Calibration.push_back(facing.Make("MOUSE_FACING_THRESHOLD (rad)", Cd::MOUSE_FACING_THRESHOLD,
        "the facing since the last packet at a SET_FACING (frame-limited: a little above)"));
    report.Calibration.push_back(keyboardMoving.Make("KEYBOARD_TURN_WHILE_MOVING", KEYBOARD_TURN_WHILE_MOVING,
        "keyboard turn rate moving, over the unit's"));
    report.Calibration.push_back(keyboardStill.Make("keyboard turn standing", 1.0f, "over the unit's"));
    report.Calibration.push_back(floatDepth.Make("FLOAT_DEPTH (of height)", FLOAT_DEPTH,
        "how deep a swimmer's feet float with no vertical key"));
    report.Calibration.push_back(verticalShare.Make("VERTICAL_SHARE", VERTICAL_SHARE,
        "ascending or descending alone, over the speed"));
    return report;
}

std::string Rp::Report::Text() const
{
    std::string out;
    char line[256];
    auto add = [&](char const* text) { out += text; out += '\n'; };
    std::snprintf(line, sizeof(line), "replay: %u packets, %u segments, %u jumps", Records, Segments, Jumps);
    add(line);
    add("  drift (yd)   all: median  p90   max  (n)    flat: median  p90   max  (n)    target flat < 0.5 at 5 s");
    for (Horizon const& h : Drift)
    {
        std::snprintf(line, sizeof(line), "  %5.1f s          %6.3f %5.2f %5.2f %4u          %6.3f %5.2f %5.2f %4u",
            float(h.Ms) / 1000.0f, h.All.Median, h.All.P90, h.All.Max, h.All.Count, h.Flat.Median, h.Flat.P90,
            h.Flat.Max, h.Flat.Count);
        add(line);
    }
    std::snprintf(line, sizeof(line), "  jump landing (yd): median %.3f p90 %.2f max %.2f (%u)", JumpLanding.Median,
        JumpLanding.P90, JumpLanding.Max, JumpLanding.Count);
    add(line);
    std::snprintf(line, sizeof(line), "  steps walked onto %u, refused by the controller %u; steep descents %u; walls "
        "met where the player passed %u; packets inside the terrain %u", StepUps, StepUpsRefused, SteepDescents,
        WallDisagreements, InTerrain);
    add(line);
    add("  calibration: measured vs constant");
    for (Measure const& m : Calibration)
    {
        std::snprintf(line, sizeof(line), "    %-30s %9.4f +- %-8.4f vs %9.4f  (n %u) %s", m.Name.c_str(), m.Mean, m.Sd,
            m.Constant, m.Count, m.Note.c_str());
        add(line);
    }
    std::string opcodes = "  opcodes:";
    for (auto const& [opcode, count] : Opcodes)
    {
        std::snprintf(line, sizeof(line), " 0x%03X x%u", opcode, count);
        opcodes += line;
    }
    add(opcodes.c_str());
    return out;
}
