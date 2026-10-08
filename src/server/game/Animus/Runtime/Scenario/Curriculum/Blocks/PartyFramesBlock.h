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

#ifndef ANIMUS_LIB_CURRICULUM_PARTY_FRAMES_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PARTY_FRAMES_BLOCK_H

#include "Block.h"
#include "ObjectGuid.h"
#include "SeatView.h"

class Player;
class Unit;

namespace Animus::Curriculum
{
    /// **What a player's UI shows of its party** (dungeon-curriculum I5, the party follow; revision 2, G1: the one
    /// source of party-member state). GROUP_MEMBERS slots, the leader first, then the others in group (or seat) order
    /// (SeatView::Frames). The seat's own frame and its pet's are the combat block's (CombatBlock, revision 1: the
    /// player frame and the pet frame); the frames of the others in its group are only here.
    ///
    /// **The party frames** are always there, as they are on a player's screen: alive, health, power (and whether it
    /// is mana), in combat, which member leads, within the frame's range (FRAME_RANGE: the client fades a frame past
    /// it), the debuffs on it and how many of them a dispel removes, aggro (something attacks it: the red border),
    /// whether it is the seat's selection or focus, and **its target** (what /assist would take): whether it has one
    /// the seat's client has too, hostile, the seat's own selection, in the seat's camera's frame now. They say nothing
    /// of where a member is: a 3.3.5 party frame shows no position.
    ///
    /// **The minimap's party dots** (the choice, 2026-10-06): the one place a 3.3.5 client shows a party member's
    /// position without seeing it is the minimap, which draws a dot for each party member within its radius
    /// (PartyFollow.MinimapYards, 60 by default: about what the default zoom shows indoors) and nothing for one
    /// beyond it -- through walls, since the minimap is a map, not a view. No height: a dot is flat. The minimap is
    /// modelled heading-up (the client's "rotate minimap" option): right and forward of the seat's facing, which is
    /// the frame every other bearing the seat reads is in; the default north-up minimap would need the seat to know
    /// where north is, which nothing else tells it. Beyond the radius a member is known only by the frames, by what
    /// the camera shows (the entities block) and by what the seat remembers.
    ///
    /// **The presses** (revision 2), as clicking a frame does: select the member (the client's CMSG_SET_SELECTION),
    /// focus it (client-side), assist it (select its target, as /assist does). An empty frame is the only mask; an
    /// assist on a member with no target is refused and priced as any refused press, never masked.
    ///
    /// **Revision 2** (G1, 2026-10-07): revision 1 (M4's, written to stage.json as no revision) had the first ten
    /// columns and no presses; the combat block's member frames (its revision 0) moved here. The columns are named
    /// (DescribeColumns), so a checkpoint of either seeds them by name: M4's member{i}_* where they were; the combat
    /// block's member features start fresh (the combat stages had no party, so those columns never read anything).
    class PartyFramesBlock final : public Block
    {
    public:
        static constexpr uint32 REVISION = 2;
        /// The client fades a party frame past this many yards (the range the frame's spells are checked at).
        static constexpr float FRAME_RANGE = 40.0f;

        enum Feature : uint32
        {
            // Revision 1's ten, in their order.
            FRAME_PRESENT = 0,
            FRAME_ALIVE,
            FRAME_LEADER,
            FRAME_IN_COMBAT,
            FRAME_HEALTH,
            FRAME_POWER,
            FRAME_DOT,                  // on the minimap
            FRAME_DOT_RIGHT,            // / the minimap's radius, in [-1, 1]
            FRAME_DOT_FORWARD,
            FRAME_DOT_DISTANCE,         // / the radius, in [0, 1]
            // The combat block's member frames' (its revision 0).
            FRAME_MANA_USER,            // the power is mana
            FRAME_IN_RANGE,             // within FRAME_RANGE of the seat
            FRAME_DEBUFFS,              // harmful auras / 5
            FRAME_DISPELLABLE,          // ... of a kind a dispel removes / 5
            FRAME_AGGRO,                // something is attacking it
            FRAME_SELECTED,             // the seat's selection
            FRAME_FOCUSED,              // the seat's focus
            // Its target, as /assist would take it.
            FRAME_TARGET,               // it has a selection the seat's client has too
            FRAME_TARGET_HOSTILE,
            FRAME_TARGET_MINE,          // the seat's own selection
            FRAME_TARGET_IN_VIEW,       // in the seat's camera's frame now
            FRAME_FEATURES
        };

        enum Action : uint32
        {
            ACTION_SELECT_FIRST = 0,
            ACTION_FOCUS_FIRST  = GROUP_MEMBERS,
            ACTION_ASSIST_FIRST = 2 * GROUP_MEMBERS,
            ACTION_COUNT        = 3 * GROUP_MEMBERS
        };

        /// Where the minimap draws a party member: shown within `radius` yards (2D) of the seat, and where, in the
        /// seat's facing frame. `facing` is the seat's yaw (the core's: 0 along +x, counter-clockwise).
        struct Dot
        {
            bool Shown = false;
            float Right = 0.0f;
            float Forward = 0.0f;
            float Distance = 0.0f;
        };
        [[nodiscard]] static Dot DotOf(float selfX, float selfY, float facing, float otherX, float otherY,
            float radius);

        /// **One member's frame**, as `bot`'s client shows it (the one place a member's frame is read): `leads` it
        /// leads the party; its dot from the seat at (selfX, selfY) facing `facing`, within `radius`; `focus` the
        /// seat's focus and `seen` its camera's last frame (null: none).
        static void FillFrame(SeatView::PartyFrame& out, Unit const* member, bool leads, Player const* bot,
            float selfX, float selfY, float facing, float radius, ObjectGuid focus, Vision::SeenList const* seen);
        /// **The frames from the seat's core group** (StageScenario::ViewSeat, in a stage with this block): its leader
        /// first, then the others in the group's order, the seat itself aside; the frames empty without a group. An
        /// encounter whose party has no core group fills them itself (PartyFollowEncounter::View).
        static void FillFromGroup(SeatView& view);

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] uint32 Revision() const override { return REVISION; }
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        [[nodiscard]] static char const* FeatureName(uint32 feature);
    };
}

#endif
