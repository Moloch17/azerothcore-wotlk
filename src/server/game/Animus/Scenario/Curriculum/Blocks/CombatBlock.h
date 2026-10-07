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

#ifndef ANIMUS_LIB_CURRICULUM_COMBAT_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_COMBAT_BLOCK_H

#include "Block.h"
#include "Identity.h"
#include "ObjectGuid.h"
#include <array>
#include <functional>

class Player;
class Unit;

namespace Animus::Curriculum
{
    /// **Perception-true combat inputs** (dungeon-curriculum I3): what a player's client tells it about a fight, and
    /// nothing a server list knows that the client does not show.
    ///
    /// **The enemies and friends to act on are the sight block's slots** -- the camera's visible entities and the
    /// remembered ones (SightBlock, I1 and I2): a target is chosen by its select press and a spell goes at the
    /// selection (EntityActions::CastThroughClient). In a layout with this block each sight slot carries
    /// COMBAT_SLOT_FEATURES more columns after the sight block's own (SightBlock::Size), written for a **visible** unit
    /// only, from what its nameplate shows: the cast bar (casting, time left, whether it can be interrupted -- the
    /// shield on the bar -- and what the spell is: a heal, an area, aimed at the seat), the crowd control over it,
    /// elite, in combat, whom it is hitting (the seat, or one of its party or its pet), and the seat's threat status on
    /// it (the nameplate's threat colouring). A remembered slot keeps them 0: what it was doing when last seen is not
    /// what it is doing now. And the scenario's enemy list (SeatView::Enemies, which the goal and gauntlet blocks read)
    /// is the frame's visible living hostiles in slot order (VisibleEnemies), never the encounter's spawn list.
    ///
    /// **The party frames, always known** (PARTY_FRAMES): the seat itself, its pet and its group's members in group
    /// order -- present, alive, health, power, in range (40 yd: the client fades a frame past it), in combat, the
    /// debuffs and the dispellable ones on it, aggro (something is attacking it: the frame's red border), selected,
    /// focused. A player sees these whether or not the member is on screen, so they need no camera.
    ///
    /// **The target frame**: the selection's reaction, whether the camera shows it now, the seat's **threat status on
    /// it** (the client's UnitThreatSituation, 0-3: the target frame's threat indicator) and its threat percentage
    /// against whoever holds it, and the target of its target (the seat, its pet, a party member, someone else).
    ///
    /// **The presses**: select and focus a party frame (PARTY_FRAMES of each), as clicking one does -- the client's
    /// CMSG_SET_SELECTION, the focus client-side (EntityActions::Apply). An empty frame is the only mask.
    class CombatBlock final : public Block
    {
    public:
        /// The seat, its pet, its group's other members.
        static constexpr uint32 PARTY_FRAMES = 2 + GROUP_MEMBERS;
        static constexpr uint32 FRAME_SELF = 0;
        static constexpr uint32 FRAME_PET = 1;
        static constexpr uint32 FRAME_MEMBER_FIRST = 2;
        /// The client fades a party frame past this many yards (the range the frame's spells are checked at).
        static constexpr float FRAME_RANGE = 40.0f;

        enum FrameFeature : uint32
        {
            FRAME_PRESENT       = 0,
            FRAME_ALIVE         = 1,
            FRAME_HEALTH        = 2,
            FRAME_POWER         = 3,    // its primary power's share
            FRAME_MANA_USER     = 4,    // the power is mana
            FRAME_IN_RANGE      = 5,    // within FRAME_RANGE of the seat (1 for the seat itself)
            FRAME_IN_COMBAT     = 6,
            FRAME_DEBUFFS       = 7,    // harmful auras / 5
            FRAME_DISPELLABLE   = 8,    // ... of a kind a dispel removes / 5
            FRAME_AGGRO         = 9,    // something is attacking it
            FRAME_SELECTED      = 10,   // the seat's selection
            FRAME_FOCUSED       = 11,   // the seat's focus
            FRAME_FEATURES      = 12
        };

        enum TargetFeature : uint32
        {
            TARGET_PRESENT      = 0,    // the seat has a selection its client still has
            TARGET_HOSTILE      = 1,
            TARGET_FRIENDLY     = 2,
            TARGET_IN_VIEW      = 3,    // the camera's frame shows it now
            TARGET_DEAD         = 4,
            TARGET_THREAT       = 5,    // the seat's threat status on it (ThreatStatus) / 3
            TARGET_THREAT_PCT   = 6,    // the seat's threat over its victim's, / 2, clamped to 1 (0.5: even)
            TARGET_TOT_SELF     = 7,    // the target of its target: the seat ...
            TARGET_TOT_PET      = 8,    // ... its pet ...
            TARGET_TOT_PARTY    = 9,    // ... a member of its group ...
            TARGET_TOT_OTHER    = 10,   // ... anyone else
            TARGET_FEATURES     = 11
        };

        enum Obs : uint32
        {
            OBS_FRAMES_FIRST    = 0,
            OBS_TARGET_FIRST    = PARTY_FRAMES * FRAME_FEATURES,
            OBS_COUNT           = OBS_TARGET_FIRST + TARGET_FEATURES
        };

        /// The per-slot columns the sight block carries in a layout with this block, after its own.
        enum SlotFeature : uint32
        {
            SLOT_CASTING        = 0,    // its cast bar is up (a cast or a channel with a cast time)
            SLOT_CAST_LEFT      = 1,    // seconds left of it / 3
            SLOT_INTERRUPTIBLE  = 2,    // an interrupt would stop it (no shield on the bar)
            SLOT_CAST_HEAL      = 3,    // the spell heals
            SLOT_CAST_AREA      = 4,    // ... hits an area
            SLOT_CAST_AT_ME     = 5,    // ... is aimed at the seat
            SLOT_CONTROLLED     = 6,    // stunned, feared, polymorphed, rooted, silenced (Encoding::IsCrowdControlled)
            SLOT_ELITE          = 7,
            SLOT_IN_COMBAT      = 8,
            SLOT_ATTACKS_ME     = 9,    // its victim is the seat
            SLOT_ATTACKS_PARTY  = 10,   // ... the seat's pet or a member of its group
            SLOT_THREAT         = 11,   // the seat's threat status on it / 3 (the nameplate's colouring)
            COMBAT_SLOT_FEATURES = 12
        };

        enum Action : uint32
        {
            ACTION_SELECT_FRAME_FIRST   = 0,
            ACTION_FOCUS_FRAME_FIRST    = PARTY_FRAMES,
            ACTION_COUNT                = 2 * PARTY_FRAMES
        };

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        /// "combat": { party_frames, frame_features, target_features, slot_features [names] }.
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// **The client's threat status** of a unit on a creature's threat list (UnitThreatSituation): 0 not on it or
        /// below whoever it is on, 1 above the victim's threat without being the victim (about to pull it), 2 the
        /// victim with someone at or above its threat, 3 the victim and alone at the top.
        [[nodiscard]] static uint32 ThreatStatus(bool onList, bool isVictim, float mine, float victimThreat,
            float othersTop);
        /// `unit`'s threat status on `enemy`'s list, read off its threat manager.
        [[nodiscard]] static uint32 ThreatStatusOf(Unit const* enemy, Unit const* unit);

        /// Who a unit's frames call its party: the seat's pet and its group's other members.
        [[nodiscard]] static bool IsPartyOf(Player const* bot, Unit const* unit);

        /// The party frames' units, in frame order (null for an empty frame): the seat, its pet, its group's members.
        [[nodiscard]] static std::array<Unit*, PARTY_FRAMES> FrameUnits(Player* bot);

        /// One sight slot's combat columns for a visible unit, from `bot`'s point of view.
        static void WriteSlot(Unit const* unit, Player const* bot, float* out);

        /// **The enemies a player could be fighting**: the frame's visible living hostile units, in slot order, at
        /// most `cap`, resolved by `resolve` (null: gone); never a remembered one (its live state would leak) and never
        /// anything the frame did not show. How many.
        using UnitResolver = std::function<Unit*(uint64 guid)>;
        static uint32 VisibleEnemies(Vision::SeenList const& seen, UnitResolver const& resolve, Unit** out, uint32 cap);

        /// Whether `guid` is among the frame's visible entities.
        [[nodiscard]] static bool InView(Vision::SeenList const& seen, uint64 guid);
    };
}

#endif
