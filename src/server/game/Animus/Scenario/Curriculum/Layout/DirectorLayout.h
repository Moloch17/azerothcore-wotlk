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
    /// Features a seat of the commanded side contributes.
    enum SeatFeature : uint32
    {
        SEAT_PRESENT        = 0,
        SEAT_ALIVE          = 1,
        SEAT_HEALTH         = 2,
        SEAT_POWER          = 3,
        /// What this seat's build can do, as the six-number brief of its Aptitude. This is how the director
        /// chooses who to give an order to: not "the tank" but "the one whose build can hold this", which is a
        /// question a label could not answer for a character nobody planned. It was a three-way role one-hot,
        /// under which every damage seat of every class looked identical.
        SEAT_APTITUDE_FIRST = 4,
        SEAT_IN_COMBAT      = 10,
        SEAT_CASTING        = 11,
        SEAT_SPREAD         = 12,    // its distance from the side's centre / DISTANCE_SCALE
        /// And which way, as a sine and cosine about the side's own axis (the centre towards the enemy, or
        /// towards the objective when it knows of no enemy). Distances alone told the director how far apart
        /// its side was and nothing about its shape, so it could not have learned to call a place: "left of
        /// the flag room" is unusable by something that cannot tell left from right.
        SEAT_BEARING_SIN    = 13,
        SEAT_BEARING_COS    = 14,
        SEAT_TO_FOCUS       = 15,   // its distance to the called target / DISTANCE_SCALE
        SEAT_ON_FOCUS       = 16,   // it is already fighting the called target
        SEAT_IS_DUTY        = 17,
        SEAT_AT_PLACE       = 18,   // it is standing where the side was told to be
        SEAT_FEATURES       = 19
    };

    /// Features an enemy slot contributes. The same slots the side's seats select between, so a called focus and a
    /// seat's own choice mean the same index.
    enum EnemyFeature : uint32
    {
        /// The side has seen this one at some point. Not "it exists": a director is told how many it faces by
        /// the scoreboard, but an enemy nobody has laid eyes on is not something it can call a focus on.
        ENEMY_PRESENT       = 0,
        ENEMY_ALIVE         = 1,
        ENEMY_HEALTH        = 2,
        ENEMY_APTITUDE_FIRST = 3,   // what it can do, the same six-number brief a seat is described by
        ENEMY_IN_COMBAT     = 9,
        ENEMY_CASTING       = 10,
        ENEMY_SPREAD        = 11,    // its distance from the commanded side's centre / DISTANCE_SCALE
        ENEMY_IS_FOCUS      = 12,
        /// A seat of the side can see it right now. When it cannot, health, role and position are what the
        /// side last saw and ENEMY_UNSEEN_TIME says how old that is; combat and casting read zero rather than
        /// their remembered values, because those are instantaneous facts and a stale one is a lie.
        ENEMY_SEEN          = 13,
        ENEMY_UNSEEN_TIME   = 14,   // time since the side last saw it / MAX_UNSEEN_TIME_MS, clamped
        ENEMY_BEARING_SIN   = 15,   // which way it lies, about the same axis as SEAT_BEARING_*
        ENEMY_BEARING_COS   = 16,
        ENEMY_FEATURES      = 17
    };

    enum Observation : uint32
    {
        OBS_ACTIVE              = 0,
        OBS_EPISODE_TIME        = 1,    // / EPISODE_TIME_SCALE_MS, clamped
        OBS_OWN_STANDING        = 2,    // living seats of the side / its seats
        OBS_ENEMY_STANDING      = 3,
        OBS_OWN_HEALTH          = 4,    // mean health left of its living seats
        OBS_ENEMY_HEALTH        = 5,
        OBS_OWN_SCORE           = 6,    // the objective, where the arena keeps one
        OBS_ENEMY_SCORE         = 7,
        OBS_HAS_OBJECTIVE       = 8,
        OBS_SINCE_CALL          = 9,    // decisions since the order last changed / CALL_AGE_SCALE, clamped
        OBS_HAS_FOCUS           = 10,
        OBS_HAS_DUTY            = 11,
        OBS_POSTURE_FIRST       = 12,   // one-hot: the posture standing now
        OBS_RALLY_FIRST         = OBS_POSTURE_FIRST + TEAM_POSTURE_COUNT,
        // The place as it stands: which anchor, which way off it, how far, whether it resolved to somewhere
        // the side can actually stand, and how far the side is from it.
        OBS_ANCHOR_FIRST        = OBS_RALLY_FIRST + TEAM_RALLY_COUNT,
        OBS_OFFSET_FIRST        = OBS_ANCHOR_FIRST + PLACE_ANCHOR_COUNT,
        OBS_RING_FIRST          = OBS_OFFSET_FIRST + PLACE_OFFSET_COUNT,
        OBS_PLACE_VALID         = OBS_RING_FIRST + PLACE_RING_COUNT,
        OBS_PLACE_DISTANCE,             // the side's centre to the place / DISTANCE_SCALE
        OBS_SEAT_FIRST,
        OBS_ENEMY_FIRST         = OBS_SEAT_FIRST + TEAM_SEATS * SEAT_FEATURES,
        OBS_COUNT               = OBS_ENEMY_FIRST + PACK_SLOTS * ENEMY_FEATURES
    };

    /// One call per decision, each naming the single field of the standing order it changes.
    enum Action : uint32
    {
        ACTION_HOLD             = 0,    // let the order stand
        ACTION_POSTURE_FIRST    = 1,
        ACTION_RALLY_FIRST      = ACTION_POSTURE_FIRST + TEAM_POSTURE_COUNT,
        // A place is named a field at a time like everything else here, so it costs three small groups
        // rather than one action per reachable spot.
        ACTION_ANCHOR_FIRST     = ACTION_RALLY_FIRST + TEAM_RALLY_COUNT,
        ACTION_OFFSET_FIRST     = ACTION_ANCHOR_FIRST + PLACE_ANCHOR_COUNT,
        ACTION_RING_FIRST       = ACTION_OFFSET_FIRST + PLACE_OFFSET_COUNT,
        ACTION_FOCUS_FIRST      = ACTION_RING_FIRST + PLACE_RING_COUNT,
        ACTION_DUTY_FIRST       = ACTION_FOCUS_FIRST + PACK_SLOTS,
        ACTION_COUNT            = ACTION_DUTY_FIRST + TEAM_SEATS
    };

    /// Distances are a director's only geometry: it names places, never coordinates, so the same network reads a
    /// 2 v 2 arena and a battleground.
    constexpr float DISTANCE_SCALE = 100.0f;
    constexpr float CALL_AGE_SCALE = 40.0f;     // decisions
    /// How stale a sighting can get before ENEMY_UNSEEN_TIME saturates. The same twenty seconds a seat's own
    /// memory of a hidden target uses (StageScenario's MAX_UNSEEN_TIME_MS), so both cite one number.
    constexpr float MAX_UNSEEN_TIME_MS = 20000.0f;

    /// What one side's director is looking at. Built by DirectorEncounter; nothing here knows a class or a spell.
    struct DirectorView
    {
        bool Active = false;
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
            bool IsDuty = false;
            bool AtPlace = false;
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
            bool Seen = false;          // visible to the side right now
            float UnseenTime = 0.0f;
            float BearingSin = 0.0f;
            float BearingCos = 0.0f;
        };

        /// Where the side was told to be, and whether it resolved to ground it can stand on.
        PlaceAnchor Anchor = PlaceAnchor::TeamCentre;
        PlaceOffset Offset = PlaceOffset::Toward;
        PlaceRing Ring = PlaceRing::Near;
        bool PlaceValid = false;
        float PlaceDistance = 0.0f;
        /// Whether this arena offers places at all (ArenaDefinition::Places): the group is masked out where
        /// it does not, so a stage that has no use for them pays nothing to explore them.
        bool PlacesAllowed = false;

        std::array<SeatSlot, TEAM_SEATS> Seats{};
        uint32 SeatCount = 0;
        std::array<EnemySlot, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;

        // The order as it stands, so the director sees what it has already said.
        TeamPosture Posture = TeamPosture::Attack;
        TeamRally Rally = TeamRally::None;
        bool HasFocus = false;
        bool HasDuty = false;
        float SinceCall = 0.0f;

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

    /// The layout's name, which is the same in every stage that has a director.
    [[nodiscard]] char const* Name();
}

#endif
