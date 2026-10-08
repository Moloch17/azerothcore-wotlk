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

#ifndef ANIMUS_MOVEMENT_CAPTURE_H
#define ANIMUS_MOVEMENT_CAPTURE_H

#include "Client.h"
#include <cstdint>
#include <string>
#include <vector>

/// **Movement captures** (player-controller C6): the human-play capture format's move stream (apps/forge/python/
/// animus/human/FORMAT.md §2.3 -- a FileHeader, then Move (10) and Speeds (11) records, framed `u16 type, u16
/// length`, little-endian, in gzip members), read by `forge controller replay` -- which reads the realm's capture files
/// (move-<map>.bin.gz). A reader skips record types it does not know and fields past those it knows, as FORMAT.md
/// requires. Core-free but for zlib.
namespace Animus::Movement::Capture
{
    constexpr uint16_t TYPE_HEADER = 0;
    constexpr uint16_t TYPE_MOVE = 10;
    constexpr uint16_t TYPE_SPEEDS = 11;

    /// FORMAT.md's Move (10), every field.
    struct MoveRecord
    {
        uint64_t Ms = 0;            // server time
        uint64_t Player = 0;
        uint32_t ClientMs = 0;      // the client's own clock: what replay times by
        uint16_t Opcode = 0;
        uint32_t Flags = 0;
        uint16_t Flags2 = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        float Pitch = 0.0f;
        uint32_t FallMs = 0;
        float JumpZSpeed = 0.0f;    // down positive (the client's)
        float JumpSin = 0.0f;
        float JumpCos = 1.0f;
        float JumpXYSpeed = 0.0f;
        uint32_t Map = 0;
        uint8_t Source = 0;         // 0 a client packet, 1 a companion sample
    };

    /// FORMAT.md's Speeds (11).
    struct SpeedsRecord
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        float Walk = 2.5f;
        float Run = 7.0f;
        float RunBack = 4.5f;
        float Swim = 4.722222f;
        float SwimBack = 2.5f;
        float Flight = 7.0f;
        float FlightBack = 4.5f;
        float TurnRate = 3.141594f;
        float PitchRate = 3.14f;
    };

    struct Recording
    {
        std::vector<MoveRecord> Moves;
        std::vector<SpeedsRecord> Speeds;
    };

    [[nodiscard]] Speeds ToSpeeds(SpeedsRecord const& record, Speeds base = {});

    /// The records of a capture's decompressed bytes; false (and `error`) for one that is not a capture.
    bool Decode(uint8_t const* data, std::size_t size, Recording& out, std::string& error);

    /// Read a capture file (any number of gzip members).
    bool ReadFile(std::string const& path, Recording& out, std::string& error);
}

#endif
