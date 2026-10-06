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

#ifndef ANIMUS_CURRICULUM_FIELD_GRIDS_H
#define ANIMUS_CURRICULUM_FIELD_GRIDS_H

#include "Define.h"
#include <compare>
#include <vector>

namespace Animus::Curriculum
{
    struct StageDefinition;
}

/// **The grids the layered fields are kept by** (LayeredField, FieldRoute): which grid a point is in, as the field
/// files name it, and which grids a stage's seats can stand on -- what `forge fieldstage` bakes. Kept from the ground
/// probe's tables (ProbeBake), which were removed with the move block's probe; the fields remain for the dungeon
/// wings' routes.
namespace Animus::Curriculum::FieldGrids
{
    /// The grid a point is in, as the field files name it: floor(coordinate / SIZE_OF_GRIDS), counting up from 0.
    [[nodiscard]] int32 GridIndex(float coordinate);

    /// A grid a stage needs a field for.
    struct GridRef
    {
        uint32 MapId = 0;
        int32 X = 0;
        int32 Y = 0;

        auto operator<=>(GridRef const&) const = default;
    };

    /// The grids a stage's seats can stand on: on a continent the grids under its spawn points and the neighbours
    /// within reach of an objective; on an instanced map -- the stage's own, or its instance ladder's dungeons and
    /// raids -- every grid the map's navmesh covers. With `wholeMaps` a continent is covered whole too, as the fields
    /// are: a seat can walk anywhere on it, and a field is small enough to ship them all.
    [[nodiscard]] std::vector<GridRef> StageGrids(Animus::Curriculum::StageDefinition const& stage,
        bool wholeMaps = false);
}

#endif
