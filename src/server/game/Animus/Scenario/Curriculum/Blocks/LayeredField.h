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

#ifndef ANIMUS_CURRICULUM_LAYERED_FIELD_H
#define ANIMUS_CURRICULUM_LAYERED_FIELD_H

#include "Define.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class Map;

/// A layered height field of a grid: per cell, the intervals of open air above each floor -- how high the floor is,
/// how much room there is above it before the next solid, and whether liquid fills the bottom of it. Everything else
/// in the column is solid. It is the world's geometry, not a reading of it: the dungeon wings' routes are planned over
/// it (FieldRoute).
///
/// A column is every surface a downward ray finds in it (the core's own height query, repeated from under each surface
/// it finds), every navmesh floor (walkable ground and water) and the terrain, with less than MIN_HEADROOM of room
/// above dropped: the collision geometry is two-sided, so the scan also finds the undersides of floors and roofs,
/// and those have the solid they belong to right above them. Headroom is an upward collision
/// ray from each floor, and each floor carries the flags of the navmesh polygons on it, which is what walls,
/// shores and burning edges are made of. What is baked is static: grids are created for a bake without their
/// creatures or gameobjects, so no door or elevator is in a field.
///
/// A grid's field is about 0.3 MB on disk at 1 yd cells and bakes in a fraction of a second. The fields ship in
/// AnimusForge.Probe.Dir (`forge fieldstage`, `forge fieldworld`). The move block's ground probe used to be worked out
/// of them too; it is measured live now, and nothing reads a field but the routes.
namespace Animus::Curriculum::LayeredField
{
    /// Headroom that means open sky.
    constexpr float OPEN_SKY = 1.0e9f;

    /// One interval, as a field holds it in memory and on disk: eight bytes, heights in eighths of a yard.
    struct Interval
    {
        static constexpr uint16 SKY = 0xFFFF;               // Headroom8: open sky
        static constexpr int16 NO_LIQUID = -32768;          // Liquid8: none
        static constexpr uint8 NAV_MASK = 0x0F;             // Flags: NAV_GROUND / NAV_MAGMA / NAV_SLIME / NAV_WATER
        static constexpr uint8 OPEN_ABOVE = 0x80;           // Flags: OpenAbove

        int16 Floor8 = 0;
        uint16 Headroom8 = SKY;
        int16 Liquid8 = NO_LIQUID;      // the liquid's surface against the floor, when liquid fills or lies under it
        uint8 LiquidFlags = 0;          // MAP_LIQUID_TYPE_* of that liquid
        uint8 Flags = 0;

        [[nodiscard]] float Floor() const { return float(Floor8) * 0.125f; }
        [[nodiscard]] float Headroom() const { return Headroom8 == SKY ? OPEN_SKY : float(Headroom8) * 0.125f; }
        [[nodiscard]] bool HasLiquid() const { return Liquid8 != NO_LIQUID; }
        [[nodiscard]] float Liquid() const { return Floor() + float(Liquid8) * 0.125f; }
        /// The navmesh polygons' flags on this floor; 0 where the floor is not on the mesh (a roof, a treetop,
        /// terrain the mesh left out).
        [[nodiscard]] uint8 NavFlags() const { return Flags & NAV_MASK; }
        /// Whether what is above the floor is known to be open air: a navmesh floor, the terrain, the topmost
        /// surface. The rest of a column's surfaces are what a downward scan hit, and the geometry is two-sided, so
        /// some of them are the inner faces of something solid -- a canopy, a roof slab -- which a march must
        /// still see (the core's height query finds them too) but a flight sense must not fly through.
        [[nodiscard]] bool OpenAbove() const { return (Flags & OPEN_ABOVE) != 0; }
    };
    static_assert(sizeof(Interval) == 8);

    struct Grid
    {
        uint32 MapId = 0;
        int32 GridX = 0;                // FieldGrids::GridIndex of its x and y
        int32 GridY = 0;
        float MinX = 0.0f;              // the first cell's centre
        float MinY = 0.0f;
        float Cell = 1.0f;
        uint32 Side = 0;
        std::vector<uint32> First;      // per cell, into Intervals; Side * Side + 1
        std::vector<Interval> Intervals;
        double Seconds = 0.0;

        /// Memory it takes.
        [[nodiscard]] std::size_t Bytes() const;
    };

    /// Bake the grid holding (x, y); its terrain, collision and navmesh (and its neighbours') must be loaded.
    Grid Bake(Map* map, float x, float y, float cell, uint32 threads = 0);

    /// The cell size of the fields a running sim reads.
    constexpr float STANDARD_CELL = 1.0f;

    /// A field as a file: the header, then per cell its interval count and the intervals as they are,
    /// zstd-compressed.
    bool Write(Grid const& grid, std::string const& path);
    bool Read(std::string const& path, Grid& grid);

    /// The fields, one file a grid in AnimusForge.Probe.Dir, loaded the first time a route reads the grid. At most
    /// AnimusForge.Probe.CacheGrids are held: past that the one read longest ago is let go, and read from disk again
    /// if a route comes back to it.
    namespace Store
    {
        void Configure(std::string const& dir, uint32 cacheGrids);
        /// Configured with a directory (FieldRoute plans nothing without one).
        bool Enabled();
        std::string const& Dir();
        std::string FileFor(uint32 mapId, int32 gridX, int32 gridY);

        /// The field of one grid, or nullptr when there is no file for it. Thread safe; the pointer keeps the field
        /// alive while it is used even if the cache lets it go meanwhile.
        std::shared_ptr<Grid const> Find(uint32 mapId, int32 gridX, int32 gridY);

        /// Field files read.
        inline std::atomic<uint64> FileReads{ 0 };
        /// Fields held now, and the memory they take.
        uint32 Loaded();
        std::size_t Bytes();
    }
}

#endif
