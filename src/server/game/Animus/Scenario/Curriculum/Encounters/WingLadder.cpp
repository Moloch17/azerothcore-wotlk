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

#include "WingLadder.h"
#include "StringFormat.h"
#include <algorithm>

namespace
{
    float Mean(std::vector<float> const& values)
    {
        float sum = 0.0f;
        for (float value : values)
            sum += value;
        return values.empty() ? 0.0f : sum / float(values.size());
    }
}

Animus::Curriculum::WingLadder::WingLadder(uint32_t rungs, uint32_t window, float target, uint32_t start)
    : _rungs(std::max<uint32_t>(1, rungs)), _window(std::max<uint32_t>(1, window)), _target(target)
{
    _rung.store(std::min(start, _rungs - 1), std::memory_order_relaxed);
}

void Animus::Curriculum::WingLadder::Follow(uint32_t rung)
{
    _rung.store(std::min(rung, _rungs - 1), std::memory_order_relaxed);
}

Animus::Curriculum::WingLadder::Result Animus::Curriculum::WingLadder::Note(uint32_t rung, bool probe,
    float progress)
{
    Result result;
    uint32_t const now = Rung();
    if (rung != now)
        return result;

    std::vector<float>& runs = probe ? _probes : _others;
    runs.push_back(progress);
    if (runs.size() > _window)
        runs.erase(runs.begin());
    if (probe)
        ++_sinceRead;
    if (!probe || _probes.size() < _window)
        return result;

    float const probes = Mean(_probes);
    if (now + 1 < _rungs && probes >= _target)
    {
        result.Moved = Step{ now, now + 1, probes, Mean(_others) };
        _rung.store(now + 1, std::memory_order_relaxed);
        _probes.clear();
        _others.clear();
        _reads.clear();
        _sinceRead = 0;
        _lower = probes;
        _collapsed.store(-1, std::memory_order_relaxed);
        return result;
    }

    // One read for each `window` fresh probes: the sliding mean above overlaps itself, and three reads of mostly the
    // same runs are one.
    if (_sinceRead < _window)
        return result;
    _sinceRead = 0;
    _reads.push_back(probes);
    if (now == 0)
        return result;

    float const floor = std::max(COLLAPSE_FLOOR, COLLAPSE_SHARE * _lower);
    bool collapsed = _reads.size() >= COLLAPSE_READS;
    for (std::size_t i = _reads.size() - std::min<std::size_t>(_reads.size(), COLLAPSE_READS); i < _reads.size(); ++i)
        collapsed = collapsed && _reads[i] < floor;
    if (collapsed && _collapsed.load(std::memory_order_relaxed) < 0)
    {
        std::string reads;
        for (std::size_t i = _reads.size() - COLLAPSE_READS; i < _reads.size(); ++i)
            reads += Acore::StringFormat("{}{:.3f}", reads.empty() ? "" : ", ", _reads[i]);
        result.Alarm = Acore::StringFormat("WARNING the dungeon ladder's rung {} collapsed: the probes made {} of the "
            "dungeon over {} reads of {} runs, under {:.3f} (the rung below earned {:.3f}); the ladder does not step "
            "back by itself -- roll back to a checkpoint or change the rung", now, reads, COLLAPSE_READS, _window,
            floor, _lower);
    }
    _collapsed.store(collapsed ? int32_t(now) : -1, std::memory_order_relaxed);
    return result;
}
