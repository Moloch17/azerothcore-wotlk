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

#ifndef ANIMUS_LIB_CURRICULUM_ENTITIES_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_ENTITIES_BLOCK_H

#include "Block.h"
#include "Identity.h"

namespace Animus::Curriculum
{
    /// **What is visible now** (perception-goals 1b): the entities the seat's last camera frame showed -- each with at
    /// least one pixel of it, never anything else -- nearest first, Vision::ENTITY_SLOTS of them at most, in the
    /// frame's slot order, so pixel slot s (byte 4) is this block's slot s - 1. Comes with a vision block (after it:
    /// the vision block renders the frame and leaves its list, Vision::SeenList, on the seat; StageDefinition's
    /// stages with a camera are given this block). No actions.
    ///
    /// Every fact is one the seat's own client shows (the UI rule, amendment 7: Vision::Classify): the class, the
    /// template (a creature's or game object's entry, raw: the learner hashes it into an embedding), the level, the
    /// health, how its nameplate reads, quest relevance, lootable, usable; and where it is: its distance and
    /// direction from the camera, and its pixels' centroid and share of the frame.
    ///
    /// The learner reads the slots as a set (stage.json's vision block "entities", in seat-set form), joined with
    /// the camera's patch features under each slot's pixels (perception-goals 1c); the block's columns are raw, kept
    /// out of the adapters and normalisers as the camera's are.
    class EntitiesBlock final : public Block
    {
    public:
        enum Feature : uint32
        {
            ENTITY_PRESENT      = 0,
            ENTITY_CLASS        = 1,    // Vision::Class, raw (an index: the learner embeds it)
            ENTITY_TYPE         = 2,    // the creature or game object entry, raw; 0 a player
            ENTITY_OBJECT       = 3,    // a game object (else a unit)
            ENTITY_LEVEL        = 4,    // its level / LEVEL_SCALE; 0 a game object
            ENTITY_LEVEL_DELTA  = 5,    // its level less the seat's, / LEVEL_DELTA_SCALE, clamped to [-1, 1]
            ENTITY_HEALTH       = 6,    // its health's share; 1 a game object
            ENTITY_REACTION     = 7,    // -1 hostile, 0 neutral, 1 friendly (its nameplate)
            ENTITY_QUEST        = 8,    // this seat's quests concern it (a mark, a target, the glow)
            ENTITY_LOOTABLE     = 9,    // this seat may loot it
            ENTITY_USABLE       = 10,   // this seat can use or talk to it
            ENTITY_DISTANCE     = 11,   // from the camera, log-scaled as the pixels' distance (Vision::NEAR..1000)
            ENTITY_YAW_SIN      = 12,   // its direction from the camera's view: yaw (+ left) and pitch (+ up)
            ENTITY_YAW_COS      = 13,
            ENTITY_PITCH_SIN    = 14,
            ENTITY_PITCH_COS    = 15,
            ENTITY_CENTROID_X   = 16,   // its pixels' mean, -1 left to 1 right
            ENTITY_CENTROID_Y   = 17,   // ... -1 bottom to 1 top
            ENTITY_SHARE        = 18,   // its pixels' share of the frame
            ENTITY_MEMORY       = 19,   // its entity memory id (perception-goals 3): 0 until entity memory exists
            ENTITY_FEATURES     = 20
        };

        static constexpr float LEVEL_SCALE = 80.0f;
        static constexpr float LEVEL_DELTA_SCALE = 10.0f;
        /// The learner's hash table for the type id (a suggestion it may override): entry % TYPE_BUCKETS.
        static constexpr uint32 TYPE_BUCKETS = 4096;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] uint32 Revision() const override { return 1; }
        /// "entities": { name "visible", slots, width (ENTITY_FEATURES), first (the block's first column), present,
        /// class_column, type_column, classes (Vision::CLASS_LIMIT), type_buckets, features [names] }.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;

        /// The block's columns from a seat's list: slot s's features at s * ENTITY_FEATURES, the slots past Count 0.
        static void Write(Vision::SeenList const& seen, float* obs);
    };
}

#endif
