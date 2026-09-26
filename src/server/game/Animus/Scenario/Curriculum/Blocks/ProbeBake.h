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

#ifndef ANIMUS_CURRICULUM_PROBE_BAKE_H
#define ANIMUS_CURRICULUM_PROBE_BAKE_H

#include "Block.h"
#include "GroundSense.h"
#include <atomic>
#include <string>
#include <vector>

class Map;

/// The ground probe, measured ahead of time: every cell of a grid, every floor the navmesh has there, every
/// compass bearing -- so a seat reads its surroundings from a table instead of casting a hundred and twenty
/// queries each time it moves three yards.
///
/// `forge probebake` bakes one grid in memory and measures how far a lookup is from the probe measured live at
/// the same spot; `forge probestage` bakes the grids a stage plays on to disk; and with AnimusForge.Probe.Source
/// = baked the move block reads those files (Store) instead of measuring.
namespace Animus::Curriculum::ProbeBake
{
    struct Settings
    {
        float Cell = 2.0f;              // yards between cell centres
        uint32 Bearings = 16;           // compass bearings baked per floor
        uint32 WedgeRays = 3;           // rays across each bearing's wedge (1: the bearing's own line)
        float Pitch = 0.5f;             // the dense march's sample spacing (0: the five legacy cells)
        uint32 Threads = 0;             // 0: every hardware thread
    };

    /// One grid's probes. Cells run in rows of Side from (MinX, MinY); a cell's floors are First[cell] up to
    /// First[cell + 1], each with Bearings readings and one room.
    struct Table
    {
        uint32 MapId = 0;
        Settings Bake;
        float MinX = 0.0f;
        float MinY = 0.0f;
        uint32 Side = 0;
        std::vector<uint32> First;
        std::vector<float> FloorZ;
        std::vector<GroundSense::Bearing> Readings;
        std::vector<GroundSense::Room> Rooms;
        double Seconds = 0.0;
    };

    /// How a seat's sixteen rays are read off a table baked on compass bearings: the nearest bearing, or the worst
    /// of every baked wedge the ray's own wedge overlaps.
    enum class Turn
    {
        Nearest,
        Overlap,
    };

    struct Reading
    {
        GroundSense::Bearing Rays[SENSE_RAYS];
        GroundSense::Room Room;
    };

    /// The floors a seat could stand on at (x, y): the heights of the navmesh's ground and water polygons in that
    /// column, lowest first, each snapped to the ground the core itself reports within a step of it.
    std::vector<float> Floors(Map* map, dtNavMeshQuery const* query, float x, float y);

    /// Bake the grid that holds (x, y). Its terrain, collision and navmesh tiles, and its neighbours', must be
    /// loaded.
    Table Bake(Map* map, float x, float y, Settings const& settings);

    /// The probe for a seat at (x, y, z) facing `facing`, from the table; false when the table has no floor
    /// there within a step of z. `blend` mixes the four nearest cells rather than taking the nearest one.
    bool Lookup(Table const& table, float x, float y, float z, float facing, Turn turn, bool blend,
        Reading& out);

    /// Bake-quality report: `samples` random places on the grid's floors with random facings, each measured
    /// live the old way, live the dense way, and read from the table, and how far each pair is apart. With
    /// `radius` above 0 the places are drawn within it of (nearX, nearY) -- a room rather than the whole grid.
    std::string Compare(Map* map, Table const& table, uint32 samples, uint32 seed, float nearX = 0.0f,
        float nearY = 0.0f, float radius = 0.0f);

    /// The bake a running sim reads: 2 yd cells, 16 bearings, three rays a wedge, a half-yard march. A table is
    /// the observation's definition, so the settings are fixed rather than configured.
    Settings StandardSettings();

    /// The grid a point is in, as the table files name it.
    int32 GridIndex(float coordinate);

    /// A table as a file: the readings quantised to a byte a field (Step signed), the room to two. A table read
    /// back holds exactly what the file does, so a baked seat sees the quantised values, the same everywhere.
    bool Write(Table const& table, std::string const& path);
    bool Read(std::string const& path, Table& table);

    /// The live stand-in where no table answers: the same dense wedge measurement the bake makes, at the seat.
    Reading SenseLive(Map* map, dtNavMeshQuery const* query, GroundSense::Origin const& at, float facing);

    /// The tables of AnimusForge.Probe.Source = baked, one file a grid in AnimusForge.Probe.Dir, loaded the first
    /// time a seat stands on the grid and kept.
    namespace Store
    {
        void Configure(bool baked, std::string const& dir);
        bool Baked();
        std::string const& Dir();
        std::string FileFor(uint32 mapId, int32 gridX, int32 gridY);

        /// The table for the grid holding (x, y), or nullptr when there is no file for it. Thread safe.
        Table const* Find(uint32 mapId, float x, float y);

        /// Seat probes answered from a table, and answered live because none did.
        inline std::atomic<uint64> Reads{ 0 };
        inline std::atomic<uint64> Fallbacks{ 0 };
        uint32 Loaded();
    }
}

#endif
