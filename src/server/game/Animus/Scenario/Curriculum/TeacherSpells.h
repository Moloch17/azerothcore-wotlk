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

#ifndef ANIMUS_LIB_CURRICULUM_TEACHER_SPELLS_H
#define ANIMUS_LIB_CURRICULUM_TEACHER_SPELLS_H

#include "Layout.h"
#include "WingTeacher.h"
#include <optional>

namespace Animus::Curriculum
{
    struct SpecProfile;

    /// **The teacher's spells** (revived from the removed Baselines' SpellFor, 2026-10-27): the seat's first allowed
    /// spell of the kind the teacher asks for, as a layout action, read off its row -- the catalog's spells by what
    /// they do, the core block's mask for whether one can be cast now. `spec` is the seat's own (the teacher may know
    /// it; the old baseline read it off the talent points); null leaves the spec's form out.
    [[nodiscard]] std::optional<int32> TeacherSpellFor(WingTeacher::Spell spell, Layout const& layout,
        SpecProfile const* spec, float const* obs, uint8 const* mask);
}

#endif
