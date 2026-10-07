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

#ifndef ANIMUS_LIB_CURRICULUM_SIGHT_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_SIGHT_BLOCK_H

#include "Block.h"
#include "EntitiesBlock.h"
#include "EntityActions.h"
#include "EntityMemory.h"
#include <array>

namespace Animus::Curriculum
{
    /// **What the seat sees and remembers, and acting on it** (dungeon-curriculum I1 and I2; perception-goals 2a, 3).
    ///
    /// **The list**: SIGHT_SLOTS slots, the first SIGHT_VISIBLE_SLOTS the camera's visible entities in the entities
    /// block's slot order (slot s is pixel slot s + 1), the rest the SIGHT_RECALLED_SLOTS most relevant entities the
    /// seat's entity memory holds that this frame did not show (EntityMemory::Recall), most relevant first. A slot's
    /// first EntitiesBlock::ENTITY_FEATURES columns mean what the entities block's do -- for a remembered entity,
    /// its class, template, level, health and nameplate as last seen, its direction and distance from the camera to
    /// where it was last seen, no pixels -- so the learner reads both halves with the entity list's own encoder; then
    /// the memory's: visible now, how long since it was seen, dead, open, used, the way it faced and its course
    /// (speed and direction, from its last sightings: a patrol's), and whether it is the selection or the focus.
    /// Every remembered fact is a last-seen one (EntityMemory: written only from sight), never the server's.
    ///
    /// **The presses** (EntityActions): SIGHT_SLOTS of each -- select, interact or use, use the key item on, assist,
    /// focus -- each naming a slot, sent as the client sends them through the session's handlers. A slot with no
    /// entity cannot be pressed, and a game object cannot be selected, assisted or focused (the client cannot target
    /// one): the only masks. Anything else -- a remembered entity out of reach, out of sight or gone, a door too far,
    /// a hostile to talk to -- is a legal press the world refuses, priced (Actions.Aimless.ActRefused).
    ///
    /// **Spells in a stage with this block** go as the client sends them, CMSG_CAST_SPELL through the handler
    /// (EntityActions::CastThroughClient): a harmful one at the selection, a beneficial one at the focus when it is a
    /// living friend, else the selection when it is one, else the seat itself (the client's self-cast); and the
    /// stage's target is the seat's own selection (StageScenario::CurrentTarget). No looting: no press loots, and
    /// none opens a loot window.
    class SightBlock final : public Block
    {
    public:
        enum Feature : uint32
        {
            // 0 .. 19: EntitiesBlock::Feature.
            SIGHT_VISIBLE = EntitiesBlock::ENTITY_FEATURES,     // shown by this frame
            SIGHT_AGE,              // since last seen: log2(1 + s) / log2(1 + AGE_SCALE), clamped to 1; 0 visible
            SIGHT_DEAD,             // as last seen
            SIGHT_OPEN,             // a door or button standing open, as last seen
            SIGHT_USED,             // a game object used (activated, or its loot gone), as last seen
            SIGHT_HEADING_SIN,      // the way it faced, off the view's azimuth (+ left)
            SIGHT_HEADING_COS,
            SIGHT_SPEED,            // its course's speed / SPEED_SCALE, clamped to 2
            SIGHT_COURSE_SIN,       // its course's direction off the view's azimuth; 0 and 0 standing
            SIGHT_COURSE_COS,
            SIGHT_SELECTED,         // the seat's selection
            SIGHT_FOCUSED,          // the seat's focus
            SIGHT_FEATURES
        };

        enum Action : uint32
        {
            ACTION_SELECT_FIRST     = 0,
            ACTION_INTERACT_FIRST   = SIGHT_SLOTS,
            ACTION_USE_ITEM_FIRST   = 2 * SIGHT_SLOTS,
            ACTION_ASSIST_FIRST     = 3 * SIGHT_SLOTS,
            ACTION_FOCUS_FIRST      = 4 * SIGHT_SLOTS,
            ACTION_COUNT            = 5 * SIGHT_SLOTS
        };
        static_assert(SIGHT_VISIBLE_SLOTS == Vision::ENTITY_SLOTS, "the visible half is the entity list");
        static_assert(uint32(EntityActions::Press::Count) == ACTION_COUNT / SIGHT_SLOTS, "a group per press");

        static constexpr float AGE_SCALE = 3600.0f;
        static constexpr float SPEED_SCALE = 7.0f;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] uint32 Revision() const override { return 1; }
        /// "sight": { name "sight", slots, visible_slots, recalled_slots, width (SIGHT_FEATURES), first, present,
        /// class_column, type_column, object_column, memory_column, visible_column, classes, type_buckets,
        /// memory_ids (the id table the learner embeds, MEMORY_TRAINING_CAP), features [names], pointers [{press,
        /// first (the layout's action), count}] }.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// The press and the slot of action `local`.
        [[nodiscard]] static EntityActions::Press PressOf(uint32 local)
        {
            return EntityActions::Press(local / SIGHT_SLOTS);
        }
        [[nodiscard]] static uint32 SlotOf(uint32 local) { return local % SIGHT_SLOTS; }

        /// The block's columns from the seat's frame and memory (`memory` already written with this frame), each
        /// slot's GUID into `guids` (0 an empty slot): `selected` and `focus` are the seat's (raw GUIDs). `width` is a
        /// slot's stride (Width): the columns past SIGHT_FEATURES are left 0 here.
        static void Write(Vision::SeenList const& seen, Vision::EntityMemory const& memory, uint64 selected,
            uint64 focus, float* obs, std::array<uint64, SIGHT_SLOTS>& guids, uint32 width = SIGHT_FEATURES);

        /// A slot's columns in `layout`: SIGHT_FEATURES, and CombatBlock::COMBAT_SLOT_FEATURES more after them in a
        /// layout with the combat block (dungeon-curriculum I3: each visible unit's cast bar, crowd control, elite,
        /// whom it hits and the seat's threat on it, as its nameplate shows them; 0 for a remembered slot). A layout
        /// without one -- M3's -- keeps the narrower list.
        [[nodiscard]] static uint32 Width(Layout const& layout);
    };
}

#endif
