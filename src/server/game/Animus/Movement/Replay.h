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

#ifndef ANIMUS_MOVEMENT_REPLAY_H
#define ANIMUS_MOVEMENT_REPLAY_H

#include "Capture.h"
#include "PlayerController.h"
#include <cstdint>
#include <string>
#include <vector>

/// **Replaying a recorded player through the controller** (player-controller C6): the player's key states (the
/// movement flags of each packet) and facing (each packet's orientation and pitch -- the mouse) are fed through the
/// controller from the same start, over the same world, and where the controller's body goes is compared with where
/// the player's client said it was. The report is what C6 confirms the client constants against: drift at 1/2/5/10 s
/// (target: under 0.5 yd at 5 s on flat ground), the jump's launch, apex (within 0.1 yd), air time and landing, steps
/// and slopes, and each named constant against what the recording measured -- STEP_UP, gravity, the jump speeds, the
/// heartbeat, the facing threshold, the keyboard turn while moving, the float depth, the vertical share. A constant
/// changes only in a follow-up, after a recording says so. Pure (a WorldQuery for the geometry): ReplayTest records
/// the controller itself and replays it, drift exactly 0.
namespace Animus::Movement::Replay
{
    struct Options
    {
        std::vector<uint32_t> HorizonsMs{ 1000, 2000, 5000, 10000 };
        uint32_t SegmentEveryMs = 5000;     // a segment starts at the first ground packet this long after the last
        uint32_t GapMs = 2000;              // a gap this long in the packets ends a segment (a loading screen)
        float FlatYards = 1.0f;             // a segment is flat when the player's z stays within this of its start
    };

    struct Spread
    {
        uint32_t Count = 0;
        float Median = 0.0f;
        float P90 = 0.0f;
        float Max = 0.0f;
        void Fill(std::vector<float> values);
    };

    struct Horizon
    {
        uint32_t Ms = 0;
        Spread All;
        Spread Flat;
    };

    /// One of the controller's named constants against what the recording measured.
    struct Measure
    {
        std::string Name;
        float Constant = 0.0f;
        float Mean = 0.0f;
        float Sd = 0.0f;
        uint32_t Count = 0;
        std::string Note;
    };

    struct Report
    {
        uint32_t Records = 0;
        uint32_t Segments = 0;
        std::vector<Horizon> Drift;
        uint32_t Jumps = 0;
        Spread JumpLanding;                 // yards between the controller's landing and the player's
        uint32_t StepUps = 0;               // rises the player walked onto (no jump, no fall)
        uint32_t StepUpsRefused = 0;        // ... the controller did not
        uint32_t SteepDescents = 0;         // faces steeper than 50 degrees the player walked down without falling
        uint32_t WallDisagreements = 0;     // the controller met a wall where the player moved freely
        uint32_t InTerrain = 0;             // player packets the world query puts inside the terrain
        std::vector<std::pair<uint16_t, uint32_t>> Opcodes;
        std::vector<Measure> Calibration;

        [[nodiscard]] std::string Text() const;
    };

    /// The player's controls as a packet's flags say them (keyboard turns and pitch at the unit's rates).
    [[nodiscard]] ControlState ControlsOf(uint32_t flags, Speeds const& speeds);

    /// Where the controller's body is at each packet from `start` on, fed the packets' keys and facing, up to the
    /// first packet `untilMs` or more past the start, or a gap: the yards from the player's reported position at each
    /// (3D). `walls` counts the steps the controller met a wall over an interval the player crossed freely.
    [[nodiscard]] std::vector<float> Follow(std::vector<Capture::MoveRecord> const& moves, std::size_t start,
        uint32_t untilMs, Speeds const& speeds, Body const& shape, WorldQuery const& world, uint32_t gapMs = 2000,
        uint32_t* walls = nullptr);

    /// The whole report for one player's packets (sorted by the client's time), with its speeds.
    [[nodiscard]] Report Run(std::vector<Capture::MoveRecord> const& moves,
        std::vector<Capture::SpeedsRecord> const& speeds, Body const& shape, WorldQuery const& world,
        Options const& options = {});
}

#endif
