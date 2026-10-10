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

#ifndef ANIMUS_LIB_CURRICULUM_CELL_GRID_H
#define ANIMUS_LIB_CURRICULUM_CELL_GRID_H

#include "Define.h"
#include "MentalMap.h"
#include "Position.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Animus::Curriculum
{
    /// Where a seat's mental-map crop was taken from: the body's position and heading at that observation, and how
    /// many blocks of it are choosable (CellGrid::Count). The crop is egocentric and heading-up, so a cell index means
    /// something only in the frame of the observation it was drawn from (free choice goals, contract sec 1.5).
    struct CropPose
    {
        bool Valid = false;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float Yaw = 0.0f;
        uint32 Choosable = 0;
    };

    /// A place the sim holds for a cell goal: the world point latched when the cell was chosen.
    struct CellPoint
    {
        bool Valid = false;
        Position Where;
    };

    /// **The cell grid of a cell goal** (free choice goals, contract sec 1): the learner picks a block of the seat's
    /// crop, POOL x POOL crop cells (4 yd) wide, as GRID x GRID of them. The learner (mappo/networks.py cell_valid) and
    /// the sim both implement Choosable exactly: seen floor only, never the navmesh.
    namespace CellGrid
    {
        constexpr uint32 POOL = 2;
        constexpr uint32 GRID = Vision::CROP / POOL;
        constexpr uint32 CELLS = GRID * GRID;
        /// A block's floor must sit within this many height units (CROP_HEIGHT_STEP each, 4 yd) of the feet.
        constexpr int32 RISE_UNITS = 16;
        /// ... and at least this many of its crop cells must be floor-like (floor or door).
        constexpr uint32 MIN_FLOOR = 2;
        /// A cell word is (ticket << CELL_BITS) | (cell + 1); 0 is none.
        constexpr uint32 TICKET_BITS = 11;
        constexpr uint32 CELL_BITS = 12;
        constexpr uint32 CELL_MASK = (1u << CELL_BITS) - 1;
        constexpr uint32 TICKET_MASK = (1u << TICKET_BITS) - 1;

        [[nodiscard]] constexpr uint32 MakeWord(uint32 ticket, uint32 cell)
        {
            return (ticket << CELL_BITS) | (cell + 1);
        }

        /// A cell word read back; false for none (0, no ticket, or no cell, or a cell past the grid).
        [[nodiscard]] constexpr bool ParseWord(int32 word, uint32& ticket, uint32& cell)
        {
            if (word <= 0)
                return false;
            ticket = (uint32(word) >> CELL_BITS) & TICKET_MASK;
            uint32 const plusOne = uint32(word) & CELL_MASK;
            if (!ticket || !plusOne || plusOne > CELLS)
                return false;
            cell = plusOne - 1;
            return true;
        }

        /// A block's centre in yards from the body: forward (row 0 the furthest ahead) and right (column 0 the left).
        [[nodiscard]] constexpr float Forward(uint32 row) { return 46.0f - 4.0f * float(row); }
        [[nodiscard]] constexpr float Right(uint32 col) { return 4.0f * float(col) - 46.0f; }

        [[nodiscard]] inline uint8 const* CellAt(uint8 const* crop, uint32 row, uint32 col)
        {
            return crop + (std::size_t(row) * Vision::CROP + col) * Vision::CROP_CHANNELS;
        }

        [[nodiscard]] inline bool FloorLike(uint8 code)
        {
            return code == uint8(Vision::MapCode::Floor) || code == uint8(Vision::MapCode::Door);
        }

        /// Whether block (r, c) can be chosen: none of its four crop cells is wall or hazard, at least MIN_FLOOR are
        /// floor-like, and every floor-like one has a height within RISE_UNITS of the feet (height byte 0 is none).
        [[nodiscard]] inline bool Choosable(uint8 const* crop, uint32 r, uint32 c)
        {
            uint32 floors = 0;
            for (uint32 dr = 0; dr < POOL; ++dr)
                for (uint32 dc = 0; dc < POOL; ++dc)
                {
                    uint8 const* cell = CellAt(crop, r * POOL + dr, c * POOL + dc);
                    uint8 const code = cell[Vision::CROP_CODE];
                    if (code == uint8(Vision::MapCode::Wall) || code == uint8(Vision::MapCode::Hazard))
                        return false;
                    if (!FloorLike(code))
                        continue;
                    int32 const height = cell[Vision::CROP_HEIGHT];
                    if (height == 0 || std::abs(height - int32(Vision::CROP_HEIGHT_ZERO)) > RISE_UNITS)
                        return false;
                    ++floors;
                }
            return floors >= MIN_FLOOR;
        }

        /// The blocks of `crop` that can be chosen.
        [[nodiscard]] inline uint32 Count(uint8 const* crop)
        {
            uint32 count = 0;
            for (uint32 r = 0; r < GRID; ++r)
                for (uint32 c = 0; c < GRID; ++c)
                    count += Choosable(crop, r, c) ? 1 : 0;
            return count;
        }

        /// Whether the body stood on any crop cell of block (r, c) (CROP_VISITED).
        [[nodiscard]] inline bool Stood(uint8 const* crop, uint32 r, uint32 c)
        {
            for (uint32 dr = 0; dr < POOL; ++dr)
                for (uint32 dc = 0; dc < POOL; ++dc)
                    if (CellAt(crop, r * POOL + dr, c * POOL + dc)[Vision::CROP_VISITED])
                        return true;
            return false;
        }

        /// The world point of block (r, c) of a crop taken at `pose` (the crop's own rotation, MentalMap::Crop): its
        /// centre, at the mean height of its floor-like cells over the feet.
        [[nodiscard]] inline Position WorldPoint(uint8 const* crop, CropPose const& pose, uint32 r, uint32 c)
        {
            float const forward = Forward(r);
            float const right = Right(c);
            float const cosYaw = std::cos(pose.Yaw);
            float const sinYaw = std::sin(pose.Yaw);
            float height = 0.0f;
            uint32 floors = 0;
            for (uint32 dr = 0; dr < POOL; ++dr)
                for (uint32 dc = 0; dc < POOL; ++dc)
                {
                    uint8 const* cell = CellAt(crop, r * POOL + dr, c * POOL + dc);
                    if (!FloorLike(cell[Vision::CROP_CODE]))
                        continue;
                    height += float(int32(cell[Vision::CROP_HEIGHT]) - int32(Vision::CROP_HEIGHT_ZERO))
                        * Vision::CROP_HEIGHT_STEP;
                    ++floors;
                }
            return Position(pose.X + forward * cosYaw + right * sinYaw, pose.Y + forward * sinYaw - right * cosYaw,
                pose.Z + (floors ? height / float(floors) : 0.0f), pose.Yaw);
        }

        /// The block of a crop taken at `pose` that holds the world point (x, y): the inverse of WorldPoint's centre.
        /// False when it lies outside the grid.
        [[nodiscard]] inline bool Locate(CropPose const& pose, float x, float y, uint32& r, uint32& c)
        {
            float const dx = x - pose.X;
            float const dy = y - pose.Y;
            float const forward = dx * std::cos(pose.Yaw) + dy * std::sin(pose.Yaw);
            float const right = dx * std::sin(pose.Yaw) - dy * std::cos(pose.Yaw);
            float const row = std::floor(float(GRID) * 0.5f - forward / float(POOL * 2));
            float const col = std::floor(float(GRID) * 0.5f + right / float(POOL * 2));
            if (row < 0.0f || col < 0.0f || row >= float(GRID) || col >= float(GRID))
                return false;
            r = uint32(row);
            c = uint32(col);
            return true;
        }
    }
}

#endif
