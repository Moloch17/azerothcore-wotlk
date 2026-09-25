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

#ifndef MOD_ANIMUS_FORGE_TEXT_TABLE_H
#define MOD_ANIMUS_FORGE_TEXT_TABLE_H

#include "Define.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace AnimusForge
{
    /// Where console output goes: one call per line, without a trailing newline. The periodic report logs each
    /// line; a command's reply sends each line to the console that typed it.
    using LineSink = std::function<void(std::string const&)>;

    /// Plain-text table for the console and log files.
    ///
    /// Every column is as wide as its widest cell, columns are separated by two spaces and the header is
    /// underlined with dashes. ASCII only, and each row is its own line, so a log prefix in front of every line
    /// keeps the columns aligned. Trailing spaces are trimmed.
    class TextTable
    {
    public:
        enum class Align : uint8
        {
            Left,
            Right,
        };

        struct Column
        {
            std::string Header;
            Align Alignment = Align::Left;
        };

        explicit TextTable(std::vector<Column> columns);

        /// A row with fewer cells than columns leaves the rest blank; extra cells are ignored.
        void AddRow(std::vector<std::string> cells);

        [[nodiscard]] bool Empty() const { return _rows.empty(); }

        /// Header, rule and rows, each prefixed with `indent`.
        [[nodiscard]] std::vector<std::string> Lines(std::string const& indent = "") const;

        void Write(LineSink const& sink, std::string const& indent = "") const;

    private:
        std::vector<Column> _columns;
        std::vector<std::vector<std::string>> _rows;
    };

    /// Number and time formatting shared by every console table.
    namespace Format
    {
        /// 1234567 -> "1,234,567".
        std::string Count(uint64 value);

        /// 27000000 -> "27.0M", 34512 -> "34.5k", 912 -> "912".
        std::string Compact(double value);

        /// Fixed decimals; NaN and infinity print as "nan"/"inf".
        std::string Fixed(double value, uint32 decimals);

        /// Significant-digit style for metrics of any magnitude: 0.01234 -> "0.0123", 184.23 -> "184.2".
        std::string Metric(double value);

        /// Seconds -> "45s", "12m 04s", "3h 55m", "2d 07h". Negative or non-finite -> "-".
        std::string Duration(double seconds);

        /// 0.4512 -> "45.1%"; with `sign`, "+45.1%" / "-3.0%".
        std::string Percent(double fraction, bool sign = false);

        /// A value, or "-" when it is missing.
        std::string OrDash(std::optional<double> value, std::function<std::string(double)> const& format);
    }
}

#endif
