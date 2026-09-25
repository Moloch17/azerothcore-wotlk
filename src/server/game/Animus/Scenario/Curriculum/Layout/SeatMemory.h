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

#ifndef ANIMUS_LIB_CURRICULUM_SEAT_MEMORY_H
#define ANIMUS_LIB_CURRICULUM_SEAT_MEMORY_H

#include "Block.h"
#include "CurriculumTuning.h"
#include "ObjectGuid.h"
#include <array>
#include <vector>

class Player;
class SpellInfo;
class Unit;

namespace Animus::Curriculum
{
    struct Layout;

    /// What a seat remembers from one decision to the next, the same for a forge seat and a live companion so a model
    /// plays with what it trained with: when each action may be pressed again (CurriculumTuning::ActionTuning), what
    /// it pressed and when, and how its and its target's health have been going.
    ///
    /// A policy sees one observation at a time. Without these it cannot tell a stance it just took from one it has
    /// held all fight, or a target it is wearing down from one that is healing.
    class SeatMemory
    {
    public:
        static constexpr float HEALTH_TREND_MS = 3000.0f;   // the time constant of the health averages

        /// Forget everything (a new episode, a new character). `actions`: the layout's action count.
        void Reset(uint32 actions);
        [[nodiscard]] uint32 Actions() const { return uint32(_readyMs.size()); }

        /// The decision's state, before the observation: the cast or channel the bot is in (and when it began), and
        /// the health averages.
        void Observe(Player* bot, Unit* target, uint64 nowMs);

        /// Whether layout action `action` may not be pressed now: pressed too recently (RepeatMs, MoveRepeatMs), a
        /// spell the bot stopped itself (RecastAfterStopMs), a stop of a cast that has only just begun
        /// (StopCastMinMs), or a stance, form, aspect, aura, seal, armor or pet stance within ModeLockMs of the last
        /// change of its kind.
        [[nodiscard]] bool Paced(Layout const& layout, uint32 action, uint64 nowMs,
            CurriculumTuning::ActionTuning const& tuning) const;

        /// The seat pressed `action`. `knownRanks`: per catalog action the highest rank the bot knows, or null to
        /// resolve them from `bot`.
        void Press(Layout const& layout, uint32 action, uint64 nowMs, CurriculumTuning::ActionTuning const& tuning,
            Player* bot, std::vector<SpellInfo const*> const* knownRanks);

        // Features (CoreBlock), each in [0, 1] or [-1, 1].
        [[nodiscard]] float SincePressed(uint32 action, uint64 nowMs) const;    // / 10 s; 1 = never
        [[nodiscard]] float SinceMove(uint64 nowMs) const;                      // / 5 s; 1 = never
        [[nodiscard]] float SinceModeChange(uint64 nowMs) const;                // / 10 s; 1 = never
        [[nodiscard]] float SelfHealthTrend() const { return _selfTrend; }      // health now - its average
        [[nodiscard]] float TargetHealthTrend() const { return _targetTrend; }

    private:
        std::vector<uint64> _readyMs;               // per action: when it may be pressed again
        std::vector<uint64> _pressedMs;             // per action: when it was last pressed; 0 = never
        uint32 _castSpellId = 0;
        uint64 _castStartMs = 0;
        uint64 _lastMoveMs = 0;
        std::array<uint64, std::size_t(ModeGroup::Count)> _modeChangeMs{};
        uint64 _lastModeChangeMs = 0;

        bool _seeded = false;
        uint64 _observedMs = 0;
        float _selfAverage = 0.0f;
        float _targetAverage = 0.0f;
        ObjectGuid _averagedTarget;
        float _selfTrend = 0.0f;
        float _targetTrend = 0.0f;
    };
}

#endif
