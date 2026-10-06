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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_MOVE_BLOCK_H

#include "Block.h"
#include "MoveControls.h"
#include <atomic>

class Map;
class Player;

namespace Animus::Curriculum
{
    /// **Where the seat puts its feet: a player's keys and mouse** (player-controller plan §4, C3).
    ///
    /// The seat holds forward or back, a strafe, a turn rate and a pitch rate (the mouse), ascend or descend in water
    /// and air, and walk; it presses jump. Each action changes one held control (MoveControls), and the controls stay
    /// held across decisions until the seat changes them. The player controller (Animus/Movement) turns them into
    /// motion every world tick with the client's own physics, and the server is told of it as a client would tell it.
    /// Nothing is pathfound, no spline is laid for a seat, and nothing faces it at its target: walls, slopes, ledges,
    /// water and where to look are the policy's to learn.
    ///
    /// This replaced the bearing / turn-lattice design (revisions 0 and 1: eight egocentric bearings walked as
    /// navmesh splines, chosen turns and pitches, facing modes), which the user had stripped outright (plan §0). The
    /// engine still keeps the trail and still measures; it no longer moves. Nor does it sense the ground for the seat
    /// any more (revision 4): the sixteen ground and flight rays and the clearance are gone, and the camera (the
    /// vision block, free look) is how a seat sees what is around it.
    ///
    /// The block needs no target, no enemy and no objective: only legs (the hazard drill's lesson).
    class MoveBlock final : public Block
    {
    public:
        using Action = MoveControls::Action;

        enum Obs : uint32
        {
            /// Whether it is under way: a translation key held, or the body moving.
            OBS_MOVING              = 0,
            OBS_SPEED,                      // current run speed / 14 yards a second (twice unmounted), clamped
            /// Which way the body is looking, and how far up or down, each as sin and cos: an angle wraps, and a
            /// network asked to learn that 6.28 is 0.01 learns a seam instead.
            OBS_FACING_SIN,
            OBS_FACING_COS,
            OBS_PITCH_SIN,
            OBS_PITCH_COS,
            /// **The controls it holds** (MoveControls::SeatControls): forward/back and strafe (-1, 0, 1; strafe + is
            /// right), ascend/descend, the turn rate over TURN_RATE_MAX (+ left) and the pitch rate over
            /// PITCH_RATE_MAX (+ up), and walk. A policy has to know its own hands are on the keys.
            OBS_HELD_FORWARD,
            OBS_HELD_STRAFE,
            OBS_HELD_VERTICAL,
            OBS_HELD_TURN,
            OBS_HELD_PITCH,
            OBS_HELD_WALK,
            /// **What the body is actually doing**: its velocity in its own frame (ahead, left, up) over twice the
            /// unhasted run speed, and how much of what the held keys asked for the last step it got (moved over
            /// commanded; 1 when nothing was asked). Under 1 with a key held is a wall, a slope or a root: the
            /// stuck and sliding signal.
            OBS_VELOCITY_AHEAD,
            OBS_VELOCITY_LEFT,
            OBS_VELOCITY_UP,
            OBS_PROGRESS,
            /// The controller's mode, one-hot: ground, falling, swimming, flying (Movement::Mode).
            OBS_MODE_FIRST,
            OBS_MODE_COUNT          = 4,
            /// How long it has been falling (/ FALL_TIME_SCALE_MS) and how far below the fall's highest point it is
            /// (/ FALL_HEIGHT_SCALE): what the landing will cost is the seat's to learn from these and from what
            /// happens, and it differs by class and by Slow Fall.
            OBS_FALL_TIME           = OBS_MODE_FIRST + OBS_MODE_COUNT,
            OBS_FALL_HEIGHT,
            /// The last step met a wall (a move blocked, slid or stopped) or a rise refused for its slope.
            OBS_AGAINST_WALL,
            OBS_STEEP_SLOPE,
            /// How deep the feet are under the surface, over the body's height, clamped: 0 dry, 1 fully under.
            OBS_DEPTH,
            /// Whether a jump would do anything now (the mask's reason, which the seat cannot otherwise see).
            OBS_CAN_JUMP,
            /// Where the target is, in the seat's own frame: sin and cos of the bearing to it, and its distance.
            /// All zero without one -- which is the case this block exists for.
            OBS_TARGET_BEARING_SIN,
            OBS_TARGET_BEARING_COS,
            OBS_TARGET_DISTANCE,            // yards / 40
            /// The nearest hostile ground effect the seat is not standing in (SeatView::NearestHazard), in the same
            /// frame: which way it lies, how far, and how wide.
            OBS_HAZARD_BEARING_SIN,
            OBS_HAZARD_BEARING_COS,
            OBS_HAZARD_DISTANCE,            // yards / 40
            OBS_HAZARD_RADIUS,              // yards / 40
            OBS_IN_WATER,
            OBS_SUBMERGED,
            OBS_SUBMERGED_TIME,
            OBS_SWIM_SPEED,                 // / 7 yards a second, so under 1 means water is slower
            /// It is off the ground -- swimming or flying -- so pitch steers and the third dimension is real.
            OBS_AIRBORNE,
            /// **Whether the legs are getting anywhere.** How far the seat moved over the last second against
            /// how far running would have carried it, and how much of the distance to the target closed (the
            /// scenario's TrackMotion, every seat in every arena; the travel encounter's own objective reading is
            /// the travel arenas'). A seat wedged against a rock reads 0 here while its keys are held.
            ///
            /// The objective's presence, bearing, distances and detour are the compass block's since revision 5
            /// (CompassBlock): a stage without the compass (M2 seek) has nothing here that points at its objective.
            OBS_MOVE_RATE,
            OBS_CLOSE_RATE,
            /// **Where it has been** (MovementTrail): its last TRAIL_SAMPLES positions, one a second, each as an
            /// offset from where it stands now in its own frame (ahead, left) over YARD_SCALE, oldest first with
            /// the newest in the last pair, then the share of them it is still within six yards of.
            ///
            /// The episodes a trained policy loses are lost rather than wedged: they cover three times the route
            /// and end where they began, in a dozen places the pathfinder is trapped in too. A recurrent state is
            /// a poor place to keep a map, so this is the concrete thing the policy can hold against a loop --
            /// the spot it stood on eight seconds ago is behind it and to the left, or it is under its feet again.
            /// The offsets alone cannot tell "stood still for eight seconds" from "no history yet", both being all
            /// zero; the dwell share is what tells them apart.
            OBS_TRAIL_FIRST,
            OBS_TRAIL_DWELL         = OBS_TRAIL_FIRST + 2 * TRAIL_SAMPLES,
            OBS_COUNT
        };

        static constexpr float FALL_TIME_SCALE_MS = 3000.0f;
        static constexpr float FALL_HEIGHT_SCALE = 50.0f;

        /// The height change a seat can walk up or drop down without it counting as a wall, and how much more a
        /// rise may be per yard of ground between two samples (a slope rather than a step): the travel encounter's
        /// ledges and the layered fields' floors are judged by them.
        static constexpr float MAX_STEP = 2.5f;
        static constexpr float MARCH_SLOPE = 0.5f;
        /// How far out clearance is measured (the travel encounter's clearance charge; never observed). Kept small on
        /// purpose: findDistanceToWall searches outward through the polygon graph and the shared query has a
        /// 1024-node pool, and room beyond a few yards is not a thing a seat needs to tell apart.
        static constexpr float CLEARANCE_RANGE = 8.0f;
        /// Its layout revision: MoveControls::REVISION (5), past the bearing design's 0 and 1, the controls' 2, the
        /// rays' removal (4) and the compass's leaving (5).
        [[nodiscard]] uint32 Revision() const override { return MoveControls::REVISION; }
        /// Each column's name (stage.json obs_names), so a seed can follow a column that moved: revision 4's
        /// objective columns are the compass block's names, and bootstrap maps a revision 4 move block by them.
        [[nodiscard]] static std::string ColumnName(uint32 column);
        void DescribeColumns(Layout const& layout, boost::json::array& names) const override;

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// Every action is movement: it is never charged for repeating (Actions.Repeat is not levied on movement), and
        /// a held key re-pressed is a no-op anyway.
        [[nodiscard]] bool IsMovement(uint32 /*local*/) const override { return true; }
    };
}

#endif
