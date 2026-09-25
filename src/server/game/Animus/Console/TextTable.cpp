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

#include "TextTable.h"
#include "StringFormat.h"
#include <algorithm>
#include <cmath>

AnimusForge::TextTable::TextTable(std::vector<Column> columns) : _columns(std::move(columns)) { }

void AnimusForge::TextTable::AddRow(std::vector<std::string> cells)
{
    cells.resize(_columns.size());
    _rows.push_back(std::move(cells));
}

std::vector<std::string> AnimusForge::TextTable::Lines(std::string const& indent) const
{
    std::vector<std::size_t> widths(_columns.size(), 0);
    for (std::size_t c = 0; c < _columns.size(); ++c)
    {
        widths[c] = _columns[c].Header.size();
        for (std::vector<std::string> const& row : _rows)
            widths[c] = std::max(widths[c], row[c].size());
    }

    auto const line = [&](std::vector<std::string> const& cells)
    {
        std::string text = indent;
        for (std::size_t c = 0; c < _columns.size(); ++c)
        {
            if (c)
                text += "  ";

            std::string const padding(widths[c] - cells[c].size(), ' ');
            text += _columns[c].Alignment == Align::Right ? padding + cells[c] : cells[c] + padding;
        }

        text.erase(text.find_last_not_of(' ') + 1);
        return text;
    };

    std::vector<std::string> lines;
    lines.reserve(_rows.size() + 2);

    std::vector<std::string> headers;
    std::vector<std::string> rules;
    for (std::size_t c = 0; c < _columns.size(); ++c)
    {
        headers.push_back(_columns[c].Header);
        rules.emplace_back(widths[c], '-');
    }

    lines.push_back(line(headers));
    lines.push_back(line(rules));
    for (std::vector<std::string> const& row : _rows)
        lines.push_back(line(row));

    return lines;
}

void AnimusForge::TextTable::Write(LineSink const& sink, std::string const& indent) const
{
    for (std::string const& text : Lines(indent))
        sink(text);
}

std::string AnimusForge::Format::Count(uint64 value)
{
    std::string digits = std::to_string(value);
    for (int i = int(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(std::size_t(i), ",");

    return digits;
}

std::string AnimusForge::Format::Compact(double value)
{
    if (!std::isfinite(value))
        return Fixed(value, 0);

    double const magnitude = std::abs(value);
    if (magnitude >= 1e9)
        return Acore::StringFormat("{:.1f}B", value / 1e9);
    if (magnitude >= 1e6)
        return Acore::StringFormat("{:.1f}M", value / 1e6);
    if (magnitude >= 1e4)
        return Acore::StringFormat("{:.1f}k", value / 1e3);

    return Acore::StringFormat("{:.0f}", value);
}

std::string AnimusForge::Format::Fixed(double value, uint32 decimals)
{
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value > 0 ? "inf" : "-inf";

    return Acore::StringFormat("{:.{}f}", value, decimals);
}

std::string AnimusForge::Format::Metric(double value)
{
    if (!std::isfinite(value))
        return Fixed(value, 0);

    double const magnitude = std::abs(value);
    if (magnitude >= 1e4)
        return Compact(value);
    if (magnitude >= 100)
        return Fixed(value, 1);
    if (magnitude >= 1 || magnitude == 0)
        return Fixed(value, 2);
    if (magnitude >= 0.001)
        return Fixed(value, 4);

    return Acore::StringFormat("{:.2e}", value);
}

std::string AnimusForge::Format::Duration(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        return "-";

    uint64 const total = uint64(std::llround(seconds));
    uint64 const days = total / 86400;
    uint64 const hours = (total % 86400) / 3600;
    uint64 const minutes = (total % 3600) / 60;
    uint64 const secs = total % 60;

    if (days)
        return Acore::StringFormat("{}d {:02}h", days, hours);
    if (hours)
        return Acore::StringFormat("{}h {:02}m", hours, minutes);
    if (minutes)
        return Acore::StringFormat("{}m {:02}s", minutes, secs);

    return Acore::StringFormat("{}s", secs);
}

std::string AnimusForge::Format::Percent(double fraction, bool sign)
{
    if (!std::isfinite(fraction))
        return "-";

    return sign ? Acore::StringFormat("{:+.1f}%", fraction * 100.0) : Acore::StringFormat("{:.1f}%", fraction * 100.0);
}

std::string AnimusForge::Format::OrDash(std::optional<double> value, std::function<std::string(double)> const& format)
{
    return value ? format(*value) : "-";
}
