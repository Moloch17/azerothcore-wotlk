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
    [[nodiscard]] bool Supports(std::string const& policy, Layout const& layout);

    /// The baseline's action for one seat's row; 0 when nothing fits.
    [[nodiscard]] int32 Choose(std::string const& policy, Layout const& layout, float const* obs, uint8 const* mask);
}

#endif
