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

#include "Capture.h"
#include <cstring>
#include <zlib.h>

namespace
{
    namespace Cap = Animus::Movement::Capture;

    class In
    {
    public:
        In(uint8_t const* data, std::size_t size) : _data(data), _size(size) { }

        template <typename T>
        T Get()
        {
            T value{};
            if (_at + sizeof(T) <= _size)
                std::memcpy(&value, _data + _at, sizeof(T));
            _at += sizeof(T);
            return value;
        }

    private:
        uint8_t const* _data;
        std::size_t _size;
        std::size_t _at = 0;
    };

    constexpr std::size_t MOVE_BYTES = 8 + 8 + 4 + 2 + 4 + 2 + 4 * 5 + 4 + 4 * 4 + 4 + 1;
    constexpr std::size_t SPEEDS_BYTES = 8 + 8 + 4 * 9;
}

Animus::Movement::Speeds Cap::ToSpeeds(SpeedsRecord const& record, Speeds base)
{
    base.Walk = record.Walk;
    base.Run = record.Run;
    base.RunBack = record.RunBack;
    base.Swim = record.Swim;
    base.SwimBack = record.SwimBack;
    base.Flight = record.Flight;
    base.FlightBack = record.FlightBack;
    base.TurnRate = record.TurnRate;
    base.PitchRate = record.PitchRate;
    return base;
}

bool Cap::Decode(uint8_t const* data, std::size_t size, Recording& out, std::string& error)
{
    std::size_t at = 0;
    bool header = false;
    while (at + 4 <= size)
    {
        uint16_t type = 0;
        uint16_t length = 0;
        std::memcpy(&type, data + at, 2);
        std::memcpy(&length, data + at + 2, 2);
        at += 4;
        if (at + length > size)
            break;                                  // a member cut short: the records before it stand
        uint8_t const* payload = data + at;
        at += length;
        if (type == TYPE_HEADER)
        {
            if (length < 12 || std::memcmp(payload, "ANCAP", 5) != 0)
            {
                error = "not a capture (bad magic)";
                return false;
            }
            header = true;
            continue;
        }
        In in(payload, length);
        if (type == TYPE_MOVE && length >= MOVE_BYTES)
        {
            MoveRecord m;
            m.Ms = in.Get<uint64_t>();
            m.Player = in.Get<uint64_t>();
            m.ClientMs = in.Get<uint32_t>();
            m.Opcode = in.Get<uint16_t>();
            m.Flags = in.Get<uint32_t>();
            m.Flags2 = in.Get<uint16_t>();
            m.X = in.Get<float>();
            m.Y = in.Get<float>();
            m.Z = in.Get<float>();
            m.O = in.Get<float>();
            m.Pitch = in.Get<float>();
            m.FallMs = in.Get<uint32_t>();
            m.JumpZSpeed = in.Get<float>();
            m.JumpSin = in.Get<float>();
            m.JumpCos = in.Get<float>();
            m.JumpXYSpeed = in.Get<float>();
            m.Map = in.Get<uint32_t>();
            m.Source = in.Get<uint8_t>();
            out.Moves.push_back(m);
        }
        else if (type == TYPE_SPEEDS && length >= SPEEDS_BYTES)
        {
            SpeedsRecord s;
            s.Ms = in.Get<uint64_t>();
            s.Player = in.Get<uint64_t>();
            for (float* value : { &s.Walk, &s.Run, &s.RunBack, &s.Swim, &s.SwimBack, &s.Flight, &s.FlightBack,
                    &s.TurnRate, &s.PitchRate })
                *value = in.Get<float>();
            out.Speeds.push_back(s);
        }
        // Any other type is skipped by its length (FORMAT.md §2).
    }
    if (!header)
    {
        error = "no FileHeader";
        return false;
    }
    return true;
}

bool Cap::ReadFile(std::string const& path, Recording& out, std::string& error)
{
    gzFile file = gzopen(path.c_str(), "rb");
    if (!file)
    {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buffer[1 << 16];
    int read = 0;
    while ((read = gzread(file, buffer, sizeof(buffer))) > 0)       // gzread reads on through every member
        bytes.insert(bytes.end(), buffer, buffer + read);
    gzclose(file);
    return Decode(bytes.data(), bytes.size(), out, error);
}
