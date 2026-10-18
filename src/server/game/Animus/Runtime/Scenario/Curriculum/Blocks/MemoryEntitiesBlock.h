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

#ifndef ANIMUS_LIB_CURRICULUM_MEMORY_ENTITIES_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_MEMORY_ENTITIES_BLOCK_H

#include "Block.h"

namespace Animus::Vision
{
    struct RememberedObject;
    struct SeenList;
}

namespace Animus::Curriculum
{
    /// **What the seat remembers and does not see now** (object memory, ObjectMemory.h, 2026-10-18): the
    /// MEMORY_SLOTS most relevant entries of the seat's object memory (Vision::ObjectMemory::Recall from the body:
    /// nearest, sooner seen, a fixed thing or an NPC first, an Absent one last) not listed by this decision's frame,
    /// one slot each. A slot leads with the entity list's ENTITY_FEATURES columns (EntitiesBlock::WriteSlot, from this
    /// frame's camera to where the thing was: the class, template, distance, direction from the view, angular size;
    /// los 0 and memory id 0, as a remembered entity's) -- so the learner reads it with the entity list's own token
    /// encoder -- then the memory's columns (MemoryFeature). No actions. After the entities block, whose write fills
    /// the memory; zeros without a frame, without an object memory, or under the no_object_memory arm.
    ///
    /// No live stage declares it (a stage opts in by listing BlockId::MemoryEntities after the entities block), so
    /// every layout without it is as it was.
    class MemoryEntitiesBlock final : public Block
    {
    public:
        static constexpr uint32 MEMORY_SLOTS = 16;
        /// Column offsets past the entity list's (at ENTITY_FEATURES + n).
        enum MemoryFeature : uint32
        {
            MEMORY_AGE          = 0,    // log2(1 + seconds since seen) / log2(1 + AGE_SCALE_S), clamped
            MEMORY_SIGHTINGS    = 1,    // log1p(sightings) / log1p(SIGHTINGS_LOG_MAX), clamped
            MEMORY_ABSENT       = 2,    // not where it was at the last looks that should have shown it
            MEMORY_KIND_FIRST   = 3,    // one-hot over Vision::ObjectKind: fixed, npc, creature, player (3..6)
            MEMORY_OPEN         = 7,    // a door or button standing open, as last seen
            MEMORY_USED         = 8,    // a game object used (gathered, looted, activated)
            MEMORY_DEAD         = 9,
            MEMORY_BODY_SIN     = 10,   // its bearing off the body's facing: sin = -right / d, cos = forward / d
            MEMORY_BODY_COS     = 11,
            MEMORY_BODY_DIST    = 12,   // log1p(2D yards from the body) / log1p(DISTANCE_LOG_MAX), clamped
            MEMORY_HEIGHT       = 13,   // (its middle's z - the feet's) / HEIGHT_SCALE, clamped to [-1, 1]
            MEMORY_TRACE_SPAN   = 14,   // its trace's 2D extent / TRACE_SCALE, clamped (a patrol's reach)
            MEMORY_EXTRA        = 15
        };
        static constexpr float AGE_SCALE_S = 3600.0f;
        static constexpr float SIGHTINGS_LOG_MAX = 64.0f;
        static constexpr float DISTANCE_LOG_MAX = 500.0f;
        static constexpr float HEIGHT_SCALE = 10.0f;
        static constexpr float TRACE_SCALE = 40.0f;

        /// A slot's width: the entity list's columns, then the memory's.
        [[nodiscard]] static uint32 Width();

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] uint32 Revision() const override { return 1; }
        /// "memory_entities": { name "remembered", slots, width, base_width (the entity list's), first, present,
        /// class_column, type_column, object_column, memory_column, classes, type_buckets, kinds, the scales,
        /// features [names] }.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        /// One remembered entry's Width() columns into `out`: the entity list's from `seen`'s camera, then the
        /// memory's from the body at `feet` facing `facing` (radians), its age `age` seconds.
        static void WriteSlot(Vision::SeenList const& seen, Vision::RememberedObject const& entry, double age,
            float feetX, float feetY, float feetZ, float facing, float* out);
    };
}

#endif
