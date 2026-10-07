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

#ifndef ANIMUS_LIB_CURRICULUM_BASELINES_H
#define ANIMUS_LIB_CURRICULUM_BASELINES_H

#include "Layout.h"
#include "WingTeacher.h"
#include <optional>
#include <string>

/*
 * Scripted baselines for smoke tests and evaluation: what the learner has to beat. They read a seat's row through its
 * layout's blocks, so a layout change moves them with it.
 */
namespace Animus::Curriculum::Baselines
{
    /// "greedy": the first usable spell or trinket in catalog order. Every layout.
    /// "fight": also start attacking, run to the target, eat or drink between gauntlet pulls, heal a hurt owner or
    /// teammate. Layouts with the duel block.
    /// "dungeon": the dungeon teacher (WingTeacher), which plays from the world rather than the row: layouts with the
    /// duel and sight blocks, and its presses reach a seat as the seat's script action (StageScenario::TeachSeat).
    [[nodiscard]] bool Supports(std::string const& policy, Layout const& layout);

    /// The baseline's action for one seat's row; 0 when nothing fits (always, for "dungeon").
    [[nodiscard]] int32 Choose(std::string const& policy, Layout const& layout, float const* obs, uint8 const* mask);

    /// The seat's first allowed spell of the kind the dungeon teacher asks for, read off its row as `fight` reads
    /// it: the catalog's spells by what they do, the core block's mask for whether one can be cast now.
    [[nodiscard]] std::optional<int32> SpellFor(WingTeacher::Spell spell, Layout const& layout, float const* obs,
        uint8 const* mask);

    /// The dungeon teacher's choice as one of the seat's own actions: its first option the layout has and the mask
    /// allows -- a move key, a sight-list press, a spell (SpellFor), the auto attack, eat or drink. -1: none (and the
    /// no-op is never one).
    [[nodiscard]] int32 TeacherPress(WingTeacher::Choice const& choice, Layout const& layout, float const* obs,
        uint8 const* mask);

    /// A hint block's columns (HintBlock) for the teacher's press: the press and its weight only for a press (> 0) --
    /// nothing is not a hint -- and whether the teacher played the seat, either way.
    void WriteHint(float* columns, int32 action, float weight, bool scripted);
}

#endif
