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

#include "SeatMemory.h"
#include "ActionCatalog.h"
#include "DuelBlock.h"
#include "Layout.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
    constexpr float PRESSED_SCALE_MS = 10000.0f;
    constexpr float MOVE_SCALE_MS = 5000.0f;
    constexpr float MODE_SCALE_MS = 10000.0f;

    /// Time since `ms` / `scale`, clamped; 1 when it never happened.
    float Since(uint64 ms, uint64 nowMs, float scale)
    {
        return ms && nowMs >= ms ? std::min(1.0f, float(nowMs - ms) / scale) : 1.0f;
    }
}

void Animus::Curriculum::SeatMemory::Reset(uint32 actions)
{
    *this = SeatMemory();
    _readyMs.assign(actions, 0);
    _pressedMs.assign(actions, 0);
}

void Animus::Curriculum::SeatMemory::Observe(Player* bot, Unit* target, uint64 nowMs)
{
    uint32 spellId = 0;
    if (bot && bot->IsAlive())
    {
        if (Spell const* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            cast && cast->getState() == SPELL_STATE_PREPARING)
            spellId = cast->m_spellInfo->Id;
        else if (Spell const* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            channel && channel->getState() == SPELL_STATE_CASTING)
            spellId = channel->m_spellInfo->Id;
    }

    if (spellId != _castSpellId)
    {
        _castSpellId = spellId;
        _castStartMs = nowMs;
    }

    // Exponential averages of health, whatever the decision interval: the trend is how far health is from where it
    // has been over the last few seconds.
    float const self = bot && bot->IsAlive() ? bot->GetHealthPct() / 100.0f : 0.0f;
    float const other = target && target->IsAlive() ? target->GetHealthPct() / 100.0f : 0.0f;
    ObjectGuid const targetGuid = target ? target->GetGUID() : ObjectGuid::Empty;

    if (!_seeded)
    {
        _seeded = true;
        _selfAverage = self;
        _targetAverage = other;
        _averagedTarget = targetGuid;
    }
    else
    {
        float const elapsed = float(nowMs > _observedMs ? nowMs - _observedMs : 0);
        float const weight = 1.0f - std::exp(-elapsed / HEALTH_TREND_MS);
        _selfAverage += (self - _selfAverage) * weight;
        if (targetGuid != _averagedTarget)
        {
            _averagedTarget = targetGuid;
            _targetAverage = other;
        }
        else
            _targetAverage += (other - _targetAverage) * weight;
    }

    _observedMs = nowMs;
    _selfTrend = std::clamp(self - _selfAverage, -1.0f, 1.0f);
    _targetTrend = std::clamp(other - _targetAverage, -1.0f, 1.0f);
}

bool Animus::Curriculum::SeatMemory::Paced(Layout const& layout, uint32 action, uint64 nowMs,
    CurriculumTuning::ActionTuning const& tuning) const
{
    if (action < _readyMs.size() && nowMs < _readyMs[action])
        return true;

    // A player stops a cast for something it saw happen, which takes longer than a decision.
    if (_castSpellId && layout.Has(BlockId::Duel)
        && action == layout.Slice(BlockId::Duel).ActionFirst + DuelBlock::ACTION_STOP_CASTING
        && nowMs < _castStartMs + tuning.StopCastMinMs)
        return true;

    // Dancing between stances, aspects or pet stances is not a plan: a change of one kind holds for ModeLockMs.
    uint8 const group = action < layout.ModeGroups.size() ? layout.ModeGroups[action] : 0;
    return group && _modeChangeMs[group] && nowMs < _modeChangeMs[group] + tuning.ModeLockMs;
}

void Animus::Curriculum::SeatMemory::Press(Layout const& layout, uint32 action, uint64 nowMs,
    CurriculumTuning::ActionTuning const& tuning, Player* bot, std::vector<SpellInfo const*> const* knownRanks)
{
    std::optional<BlockId> const block = layout.BlockOfAction(action);
    if (!block || action >= _readyMs.size())
        return;

    uint32 const local = action - layout.Slice(*block).ActionFirst;
    bool const movement = GetBlock(*block).IsMovement(local);
    _readyMs[action] = nowMs + (movement ? tuning.MoveRepeatMs : tuning.RepeatMs);
    _pressedMs[action] = std::max<uint64>(1, nowMs);

    if (movement)
        _lastMoveMs = std::max<uint64>(1, nowMs);

    if (uint8 const group = action < layout.ModeGroups.size() ? layout.ModeGroups[action] : 0)
    {
        _modeChangeMs[group] = std::max<uint64>(1, nowMs);
        _lastModeChangeMs = _modeChangeMs[group];
    }

    // Stopping a cast to start the same one again: the spell it stopped waits RecastAfterStopMs.
    if (*block != BlockId::Duel || local != DuelBlock::ACTION_STOP_CASTING || !_castSpellId)
        return;

    std::vector<ActionCatalog::Action> const& catalog = layout.Catalog().Actions();
    uint32 const coreFirst = layout.Slice(BlockId::Core).ActionFirst;
    for (uint32 i = 0; i < catalog.size() && coreFirst + i < _readyMs.size(); ++i)
    {
        if (catalog[i].Type != ActionCatalog::Kind::Spell)
            continue;

        SpellInfo const* info = knownRanks && i < knownRanks->size() ? (*knownRanks)[i]
            : bot ? ActionCatalog::KnownRank(bot, catalog[i].FirstRank) : nullptr;
        if (!info || info->Id != _castSpellId)
            continue;

        uint64& ready = _readyMs[coreFirst + i];
        ready = std::max(ready, nowMs + tuning.RecastAfterStopMs);
    }
}

float Animus::Curriculum::SeatMemory::SincePressed(uint32 action, uint64 nowMs) const
{
    return action < _pressedMs.size() ? Since(_pressedMs[action], nowMs, PRESSED_SCALE_MS) : 1.0f;
}

float Animus::Curriculum::SeatMemory::SinceMove(uint64 nowMs) const
{
    return Since(_lastMoveMs, nowMs, MOVE_SCALE_MS);
}

float Animus::Curriculum::SeatMemory::SinceModeChange(uint64 nowMs) const
{
    return Since(_lastModeChangeMs, nowMs, MODE_SCALE_MS);
}
