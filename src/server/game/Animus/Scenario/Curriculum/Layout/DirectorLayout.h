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

#ifndef ANIMUS_LIB_CURRICULUM_DIRECTOR_LAYOUT_H
#define ANIMUS_LIB_CURRICULUM_DIRECTOR_LAYOUT_H

#include "Aptitude.h"
#include "Block.h"
#include "ClassProfile.h"
#include <array>
#include <boost/json/object.hpp>
#include <string>
#include <vector>

/*
 * What the agent commanding a side sees and says. One layout, shared by every scenario that has a team: the network
 * that learns to focus fire in a party fight is the one that calls a kill target in arena and an interrupt rotation
 * in a raid, so nothing here may name a class, a spell or a piece of content.
 *
 * The director says one thing per decision. Four heads chosen at once would need three more categorical channels on
 * the wire, in the rollout buffer, in the actor and in the exported model; instead the standing order is state the
 * director edits, and an action names the one field it is changing -- "switch to that one", "stack on me", "you
 * interrupt next". The remaining fields keep what they were, which is what makes a call an order rather than a
 * fresh opinion every 250 ms, and it costs one small flat action space (ACTION_COUNT) that a 2 v 2 and a raid share.
 */
namespace Animus::Curriculum::DirectorLayout
{
    /// **Any group, any size** (long-horizon plan, Component E): the director commands every seat of its side up to a
    /// raid's forty, in groups of five. Seat and enemy slots are sets -- the learner reads them with one shared
    /// encoder and pooling (and scores the per-slot actions from each slot's own encoding), so the same network
    /// directs a pair, a party and a raid, and does not care which slot a member landed in.
    constexpr uint32 DIRECTOR_SEATS = MAX_SEATS;

    /// Features a seat of the commanded side contributes.
    enum SeatFeature : uint32
    {
        SEAT_PRESENT        = 0,
        SEAT_ALIVE          = 1,
        SEAT_HEALTH         = 2,
        SEAT_POWER          = 3,
        /// What this seat's build can do, as the six-number brief of its Aptitude. This is how the director
        /// chooses who to give an order to: not "the tank" but "the one whose build can hold this", which is a
        /// question a label could not answer for a character nobody planned.
        SEAT_APTITUDE_FIRST = 4,
        SEAT_IN_COMBAT      = 10,
        SEAT_CASTING        = 11,
        SEAT_SPREAD         = 12,    // its distance from the side's centre / DISTANCE_SCALE
        /// And which way, about the side's own axis (the centre towards the enemy).
        SEAT_BEARING_SIN    = 13,
        SEAT_BEARING_COS    = 14,
        SEAT_TO_FOCUS       = 15,   // its distance to the side's focus / DISTANCE_SCALE
        SEAT_ON_FOCUS       = 16,   // it is already fighting the side's focus
        SEAT_AT_PLACE       = 17,   // it is standing where the side was told to be
        SEAT_GROUP          = 18,   // its group / RAID_GROUPS
        SEAT_ADDRESSED      = 19,   // the next order goes to it (addressed by name or by group)
        SEAT_ATTACKED       = 20,   // enemies attacking it / PACK_SLOTS
        SEAT_ORDER_FIRST    = 21,   // one-hot over OrderKind: the order it holds
        SEAT_ORDER_AGE      = SEAT_ORDER_FIRST + ORDER_KIND_COUNT,     // decisions since / CALL_AGE_SCALE
        /// The goals it holds (one-hot over SeatGoal, all zero for none): the primary, which an order the director
        /// gave may be, and the secondary it chose itself. One planner per group reads what the other level chose.
        SEAT_GOAL_FIRST,
        SEAT_GOAL2_FIRST    = SEAT_GOAL_FIRST + GOAL_COUNT,
        SEAT_FEATURES       = SEAT_GOAL2_FIRST + GOAL_COUNT
    };

    /// Features an enemy slot contributes. The same slots the side's seats select between, so a called enemy and a
    /// seat's own choice mean the same index.
    enum EnemyFeature : uint32
    {
        /// The side has seen this one at some point.
        ENEMY_PRESENT       = 0,
        ENEMY_ALIVE         = 1,
        ENEMY_HEALTH        = 2,
        ENEMY_APTITUDE_FIRST = 3,
        ENEMY_IN_COMBAT     = 9,
        ENEMY_CASTING       = 10,
        ENEMY_SPREAD        = 11,
        ENEMY_IS_FOCUS      = 12,
        /// A seat of the side can see it right now; when not, the rest is what it last saw.
        ENEMY_SEEN          = 13,
        ENEMY_UNSEEN_TIME   = 14,
        ENEMY_BEARING_SIN   = 15,
        ENEMY_BEARING_COS   = 16,
        ENEMY_ON_SEAT       = 17,   // it is attacking one of the side's seats
        ENEMY_ORDERED       = 18,   // seats holding an order about it / GROUP_SEATS, clamped
        ENEMY_FEATURES
    };

    enum Observation : uint32
    {
        OBS_ACTIVE              = 0,
        /// **The director may speak now** (MAY_CALL_COLUMN): it is given a turn on its clock and on events -- a
        /// member down or badly hurt, a new enemy in the fight, the focus dead -- and keeps it for up to its
        /// budget of calls, or until it holds. Its actions at any other decision change nothing; the learner
        /// chooses only here and credits the choice over the span to the next.
        OBS_MAY_CALL            = 1,
        OBS_CALLS_LEFT          = 2,    // / RAID_CALLS
        OBS_BY_EVENT            = 3,    // this turn was opened by an event, not the clock
        OBS_RAID                = 4,    // the side is a raid (groups beyond one): its budget is RAID_CALLS
        OBS_EPISODE_TIME        = 5,
        OBS_OWN_STANDING        = 6,
        OBS_ENEMY_STANDING      = 7,
        OBS_OWN_HEALTH          = 8,
        OBS_ENEMY_HEALTH        = 9,
        OBS_OWN_SCORE           = 10,
        OBS_ENEMY_SCORE         = 11,
        OBS_HAS_OBJECTIVE       = 12,
        OBS_SINCE_CALL          = 13,
        OBS_HAS_FOCUS           = 14,
        OBS_ADDRESS_FIRST       = 15,   // one-hot over OrderSource: who the next order goes to
        OBS_ADDRESS_GROUP       = OBS_ADDRESS_FIRST + ORDER_SOURCE_COUNT,   // the group addressed / RAID_GROUPS
        OBS_OBJECTIVE_FIRST,            // per journal objective: there is one to send members to
        OBS_POSTURE_FIRST       = OBS_OBJECTIVE_FIRST + 4,
        OBS_RALLY_FIRST         = OBS_POSTURE_FIRST + TEAM_POSTURE_COUNT,
        OBS_ANCHOR_FIRST        = OBS_RALLY_FIRST + TEAM_RALLY_COUNT,
        OBS_OFFSET_FIRST        = OBS_ANCHOR_FIRST + PLACE_ANCHOR_COUNT,
        OBS_RING_FIRST          = OBS_OFFSET_FIRST + PLACE_OFFSET_COUNT,
        OBS_PLACE_VALID         = OBS_RING_FIRST + PLACE_RING_COUNT,
        OBS_PLACE_DISTANCE,
        OBS_GLOBAL_COUNT,
        OBS_SEAT_FIRST          = OBS_GLOBAL_COUNT,
        OBS_ENEMY_FIRST         = OBS_SEAT_FIRST + DIRECTOR_SEATS * SEAT_FEATURES,
        OBS_COUNT               = OBS_ENEMY_FIRST + PACK_SLOTS * ENEMY_FEATURES
    };

    /// The learner reads OBS_MAY_CALL to know when the director chooses (mappo.slow_choose_column).
    constexpr uint32 MAY_CALL_COLUMN = OBS_MAY_CALL;
    /// How many calls a turn allows: a group's director four, a raid's eight.
    constexpr uint32 GROUP_CALLS = 4;
    constexpr uint32 RAID_CALLS = 8;

    /// One call per turn decision. Posture, rally and place are the side's; an address says who the orders after
    /// it go to (the side, a group, a member), and each order kind names its target.
    enum Action : uint32
    {
        ACTION_HOLD             = 0,    // nothing more this turn
        ACTION_POSTURE_FIRST    = 1,
        ACTION_RALLY_FIRST      = ACTION_POSTURE_FIRST + TEAM_POSTURE_COUNT,
        ACTION_ANCHOR_FIRST     = ACTION_RALLY_FIRST + TEAM_RALLY_COUNT,
        ACTION_OFFSET_FIRST     = ACTION_ANCHOR_FIRST + PLACE_ANCHOR_COUNT,
        ACTION_RING_FIRST       = ACTION_OFFSET_FIRST + PLACE_OFFSET_COUNT,
        ACTION_ADDRESS_SIDE     = ACTION_RING_FIRST + PLACE_RING_COUNT,
        ACTION_ADDRESS_GROUP_FIRST,
        ACTION_ADDRESS_MEMBER_FIRST = ACTION_ADDRESS_GROUP_FIRST + RAID_GROUPS,
        ACTION_FOCUS_FIRST      = ACTION_ADDRESS_MEMBER_FIRST + DIRECTOR_SEATS,    // + enemy slot
        ACTION_TANK_FIRST       = ACTION_FOCUS_FIRST + PACK_SLOTS,
        ACTION_INTERRUPT_FIRST  = ACTION_TANK_FIRST + PACK_SLOTS,
        ACTION_CONTROL_FIRST    = ACTION_INTERRUPT_FIRST + PACK_SLOTS,
        ACTION_HEAL_FIRST       = ACTION_CONTROL_FIRST + PACK_SLOTS,                // + member
        ACTION_GO_TO            = ACTION_HEAL_FIRST + DIRECTOR_SEATS,
        ACTION_OBJECTIVE_FIRST,                                                     // + journal objective
        ACTION_COUNT            = ACTION_OBJECTIVE_FIRST + 4
    };

    constexpr float DISTANCE_SCALE = 100.0f;
    constexpr float CALL_AGE_SCALE = 40.0f;     // decisions
    constexpr float MAX_UNSEEN_TIME_MS = 20000.0f;

    /// What one side's director is looking at. Built by DirectorEncounter; nothing here knows a class or a spell.
    struct DirectorView
    {
        bool Active = false;
        bool MayCall = false;
        uint32 CallsLeft = 0;
        bool ByEvent = false;
        bool Raid = false;
        float EpisodeTime = 0.0f;

        struct SeatSlot
        {
            bool Present = false;
            bool Alive = false;
            float Health = 0.0f;
            float Power = 0.0f;
            Aptitude Apt;
            bool InCombat = false;
            bool Casting = false;
            float Spread = 0.0f;
            float BearingSin = 0.0f;
            float BearingCos = 0.0f;
            float ToFocus = 0.0f;
            bool OnFocus = false;
            bool AtPlace = false;
            uint32 Group = 0;
            bool Addressed = false;
            float Attacked = 0.0f;
            OrderKind Order = OrderKind::None;
            float OrderAge = 0.0f;
            int32 Goal = NO_GOAL;               // the goals the member holds (SEAT_GOAL_FIRST, SEAT_GOAL2_FIRST)
            int32 Goal2 = NO_GOAL;
        };

        struct EnemySlot
        {
            bool Present = false;       // ever seen by this side
            bool Alive = false;
            float Health = 0.0f;
            Aptitude Apt;
            bool InCombat = false;
            bool Casting = false;
            float Spread = 0.0f;
            bool IsFocus = false;
            bool Seen = false;
            float UnseenTime = 0.0f;
            float BearingSin = 0.0f;
            float BearingCos = 0.0f;
            bool OnSeat = false;
            float Ordered = 0.0f;
        };

        PlaceAnchor Anchor = PlaceAnchor::TeamCentre;
        PlaceOffset Offset = PlaceOffset::Toward;
        PlaceRing Ring = PlaceRing::Near;
        bool PlaceValid = false;
        float PlaceDistance = 0.0f;
        bool PlacesAllowed = false;

        std::array<SeatSlot, DIRECTOR_SEATS> Seats{};
        uint32 SeatCount = 0;
        uint32 Groups = 1;
        std::array<EnemySlot, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;

        // The order as it stands, so the director sees what it has already said.
        TeamPosture Posture = TeamPosture::Attack;
        TeamRally Rally = TeamRally::None;
        bool HasFocus = false;
        float SinceCall = 0.0f;
        OrderSource Address = OrderSource::Side;
        uint32 AddressGroup = 0;
        /// The journal objectives the side could be sent to (Encounter::ViewDirector fills them).
        std::array<bool, 4> Objectives{};

        bool HasObjective = false;
        float OwnScore = 0.0f;
        float EnemyScore = 0.0f;
        float OwnStanding = 0.0f;
        float EnemyStanding = 0.0f;
        float OwnHealth = 0.0f;
        float EnemyHealth = 0.0f;
    };

    /// The director's observation row and action mask. `obs` holds OBS_COUNT floats and `mask` ACTION_COUNT
    /// entries; both are cleared here. An inactive view leaves everything zero but the hold action, which is what
    /// an undirected episode looks like to an agent whose arena happens to carry a director.
    void Observe(DirectorView const& view, float* obs, uint8* mask);

    /// Every action's name, by index, for the manifest and the stage viewer.
    [[nodiscard]] std::vector<std::string> ActionNames();

    /// Where the seat and enemy sets lie in the observation (first column, slots, per-slot width, the column that
    /// says a slot is present) and which actions are scored per slot of which set: stage.json's "director", for
    /// the learner's set encoder and pointer heads.
    [[nodiscard]] boost::json::object SetDescriptor();

    /// The layout's name, which is the same in every stage that has a director.
    [[nodiscard]] char const* Name();
}

#endif
