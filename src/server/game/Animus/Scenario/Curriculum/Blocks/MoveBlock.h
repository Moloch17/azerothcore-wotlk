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
    /// engine still senses -- the ground rays, the clearance, the trail -- and still measures; it no longer moves.
    ///
    /// The block needs no target, no enemy and no objective: only legs (the hazard drill's lesson).
    class MoveBlock final : public Block
    {
    public:
        /// Rays the ground is sensed along, sixteen round the seat (SENSE_RAYS), clockwise from straight ahead.
        static constexpr uint32 RAY_COUNT = SENSE_RAYS;

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
            /// Where it is trying to get to, in the same frame.
            OBS_OBJECTIVE,                  // there is one
            OBS_OBJECTIVE_BEARING_SIN,
            OBS_OBJECTIVE_BEARING_COS,
            OBS_OBJECTIVE_DISTANCE,         // yards / 500
            /// **What the ground ahead is like along each of the RAY_COUNT rays**: how far it runs before the first
            /// thing that stops it, over MARCH_MAX. Ray r lies r sixteenths of a turn clockwise from straight
            /// ahead, so a doorway or a gully's mouth is seen at 22.5 degrees and not only at 45. Without this the
            /// seat steers blind: one that holds forward into a cliff should be able to tell that it did.
            OBS_GROUND_FIRST,
            /// **How the ground changes along each ray**, signed, / MAX_STEP and clamped: positive is a step up,
            /// negative a drop, zero flat. In flight (OBS_AIRBORNE, not swimming) OBS_GROUND_FIRST is instead the
            /// level flight reach along the ray, and this is what climbing FLIGHT_CLIMB higher would add to it.
            ///
            /// The reach above collapses a wall, a cliff, a lava lake and the edge of the map into one number,
            /// and this is what tells the first two apart -- which matters because they are opposite things to a
            /// pair of legs. A step up is a wall to walk round; a drop is a shortcut worth taking when it is
            /// shallow and a death when it is not, and a seat that cannot see which is which can only treat every
            /// descent as forbidden.
            OBS_STEP_FIRST          = OBS_GROUND_FIRST + RAY_COUNT,
            /// **How far dry ground runs along each ray**, against OBS_GROUND_FIRST's "how far anything runs" --
            /// so the gap between the two is water that way.
            ///
            /// Both come from the same navmesh raycast under different filters: NAV_GROUND alone stops at the
            /// shore, NAV_GROUND | NAV_WATER swims on. Reading the pair together is the whole encoding -- equal
            /// means a wall or a cliff (or a lava edge, which OBS_BURNS_FIRST names), and shore short of reach
            /// means water that many yards away.
            ///
            /// How many yards *across* it is, this does not say. The wet filter crosses ground as well as water,
            /// so past a shore it runs on over the far bank and stops at a wall: a two yard channel and the near
            /// edge of a forty yard lake report the same thing. The bench at the Barrens oasis is what caught it
            /// (`forge rays`). Width would need a ray starting past the shore under a water-only filter, and is
            /// not measured.
            OBS_SHORE_FIRST         = OBS_STEP_FIRST + RAY_COUNT,
            /// **How near the liquid that burns is** along each ray -- magma or slime -- as 1 at the seat's feet
            /// falling to 0 at the far end of the march, and exactly 0 where there is none.
            ///
            /// Reported apart from water because they are not the same lesson: water is somewhere to go and be
            /// slowed, and magma is somewhere to die. Both come back as no reach, so without this the seat cannot
            /// tell a lava lake from a cliff, and the arena that teaches crossing one at its narrow point has
            /// nothing to teach with. It comes from a third ray whose filter may cross magma: where that one runs
            /// past the ray that may not, the shorter one stopped at the burning edge.
            OBS_BURNS_FIRST         = OBS_SHORE_FIRST + RAY_COUNT,
            /// Water it is already in. Whether it is in it, whether its head is under it, and how long its head
            /// has been under -- against the breath a character has, and zero for one that does not need to
            /// breathe. Without the last of these, going in is free and "is this crossing worth it" has no
            /// downside to weigh.
            OBS_IN_WATER            = OBS_BURNS_FIRST + RAY_COUNT,
            OBS_SUBMERGED,
            OBS_SUBMERGED_TIME,
            OBS_SWIM_SPEED,                 // / 7 yards a second, so under 1 means water is slower
            /// It is off the ground -- swimming or flying -- so pitch steers and the third dimension is real.
            OBS_AIRBORNE,
            /// **How much longer the way round is than the way through**: the walking route to the objective over
            /// the straight line to it, / 4 and clamped. 0 without an objective, and about 0.25 (a ratio of 1)
            /// when the straight line is the route.
            ///
            /// This is the one thing a seat cannot see for itself at any probe length: that the barrier in front
            /// of it runs for two hundred yards and the way past is backwards. Measured on foot, with water and
            /// magma excluded, so it is the ground's answer and not the pathfinder's -- a player's path filter
            /// admits both, which would have this read "straight shot" across a lake or a lava field.
            OBS_DETOUR,
            /// **Whether the legs are getting anywhere.** How far the seat moved over the last second against
            /// how far running would have carried it, and how much of the distance to the objective -- or, in an
            /// arena with none, to the target -- that closed. Measured by the scenario for every seat in every
            /// arena; the travel encounter used to be the only source, so every other arena read 0 and a seat
            /// wedged against a rock in a fight looked, from the inside, exactly like one walking freely.
            OBS_MOVE_RATE,
            OBS_CLOSE_RATE,
            /// **The last forty yards, at a resolution that can see them.** The same distance as
            /// OBS_OBJECTIVE_DISTANCE but over YARD_SCALE rather than OBJECTIVE_SCALE, so arriving
            /// (TravelBlock::ARRIVE_DISTANCE, 6 yards) sits at 0.15 instead of 0.012 and the 20-45 yard band
            /// every lost episode dies in spans half the range instead of a twelfth of it. A coarse feature and
            /// a fine one, which is the only way one number covers both five hundred yards and six.
            OBS_OBJECTIVE_NEAR,
            /// **How much room the seat has**: yards to the nearest edge of walkable space, over
            /// CLEARANCE_RANGE, and which way is out -- sine and cosine of the direction away from it, in the
            /// seat's own frame.
            ///
            /// The rays say how far it could go each way; this says how close the nearest thing already is,
            /// which is a different question and the one that matters in a corridor. It is one
            /// dtNavMeshQuery::findDistanceToWall, which returns the distance, the point and a normal pointing
            /// back at the seat -- so the direction out comes free with the distance.
            ///
            /// Measured against the navmesh, which rcErodeWalkableArea already shrank by one agent radius
            /// (walkableRadius 2 cells, about 0.53 yd), and whose edges are simplified to within
            /// maxSimplificationError (1.8 yd). It is a coarse signal by construction: it shapes where the seat
            /// puts itself, and is never allowed to forbid a move -- a doorway is narrower than any margin worth
            /// keeping in open ground.
            OBS_CLEARANCE,
            OBS_CLEARANCE_SIN,
            OBS_CLEARANCE_COS,
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

        /// How far ahead the ground is read along each bearing, and the height change a seat can walk up or drop
        /// down without it counting as a wall.
        ///
        /// PROBE_YARDS was one sample, twelve yards out, compared against the seat's own height: "is the point
        /// twelve yards that way roughly level with me". That collapses a gentle rise into a wall -- three
        /// yards of climb over twelve reads as no reach at all -- and it cannot see anything at thirteen. It is
        /// kept as the nearest march cell and as the manifest's idea of a probe.
        static constexpr float PROBE_YARDS = 12.0f;
        static constexpr float MAX_STEP = 2.5f;
        /// **How far the seat can see along a bearing, and where it looks on the way.** Five cells rather than
        /// one, each judged against the cell before it, so a slope is a slope and only a real step is a step.
        /// What is reported is the distance to the first thing that stops the ray, which is a number that means
        /// the same at six yards and at forty -- unlike the old reach, where a wall and a cliff and the edge of
        /// the map were all 0 and everything else was 1.
        ///
        /// Geometry, not a route: the march says what is there, and choosing a bearing stays the policy's job.
        static constexpr uint32 MARCH_CELLS = 5;
        static constexpr float MARCH_RANGES[MARCH_CELLS] = { 6.0f, 12.0f, 20.0f, 30.0f, 40.0f };
        static constexpr float MARCH_MAX = 40.0f;
        /// In the air the rays are flown, not marched (Observe): looked along every FLIGHT_PITCH yards, and again
        /// FLIGHT_CLIMB higher for whether climbing opens the way.
        static constexpr float FLIGHT_PITCH = 0.5f;
        static constexpr float FLIGHT_CLIMB = 8.0f;
        /// When a march stops describing where the seat is. Forty map queries is too many to repeat every
        /// decision, and it does not have to be repeated: the ground does not move. Redone when the seat has
        /// walked MARCH_REFRESH_YARDS from where it was marched, turned MARCH_REFRESH_RADIANS from the heading
        /// it was marched along, or MARCH_REFRESH_MS have passed -- movement first, because at seven yards a
        /// second a clock alone goes stale inside the nearest cell.
        static constexpr float MARCH_REFRESH_YARDS = 3.0f;
        static constexpr float MARCH_REFRESH_RADIANS = 0.3926991f;      // half a bearing, 22.5 degrees
        static constexpr uint32 MARCH_REFRESH_MS = 500;
        /// Which of the three made a probe stale (the first that holds, in that order), counted for `forge status`.
        static inline std::atomic<uint64> StaleMoved{ 0 };
        static inline std::atomic<uint64> StaleTurned{ 0 };
        static inline std::atomic<uint64> StaleClock{ 0 };
        /// How far up or down the ground is looked for at a march cell, and how much of a rise or drop between
        /// two cells is still walkable.
        ///
        /// The old probe looked for ground only within MAX_STEP of the seat and called everything else a wall,
        /// which is why broken ground read as cliffs in every direction: three yards of climb over twelve was
        /// "no reach at all". A cell is judged against the cell before it now, and the allowance grows with the
        /// gap between them -- MAX_STEP for the discontinuity a step really is, plus MARCH_SLOPE for the ground
        /// simply going uphill. Over a six yard gap that admits 5.5 yards of rise; over ten, 7.5.
        static constexpr float MARCH_SEARCH = 20.0f;
        static constexpr float MARCH_SLOPE = 0.5f;
        /// How far out clearance is measured and reported against. Kept small on purpose: findDistanceToWall
        /// searches outward through the polygon graph and the shared query has a 1024-node pool, and room
        /// beyond a few yards is not a thing a seat needs to tell apart.
        static constexpr float CLEARANCE_RANGE = 8.0f;
        /// How much further the ray that may cross magma must run than the ray that may not, before the gap
        /// between them is called a burning edge rather than float noise. Both rays start from one polygon and
        /// share the mesh's 1.8 yd simplification error, so that error cancels and this only has to cover the
        /// arithmetic.
        static constexpr float BURN_EDGE_MARGIN = 0.5f;

        /// Its layout revision: MoveControls::REVISION (2), past the bearing design's 0 and 1.
        [[nodiscard]] uint32 Revision() const override { return MoveControls::REVISION; }

        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        /// What the navmesh senses read standing at one point, as a table, for a console to print.
        ///
        /// This exists because every one of those senses is a Detour call, Detour's axes are {y, z, x} rather
        /// than the world's, and a swizzle that is wrong is completely silent: the rays simply go somewhere
        /// else and come back with plausible numbers about the wrong place. Nothing downstream can catch it.
        /// An eval cannot catch it either -- it would show only as a policy that learns worse than it should,
        /// after hours.
        ///
        /// So the rays are run here against geometry whose answer is already known -- a wall at a measured
        /// distance, a corridor, the width of a lake that has been swum -- and read directly. It calls the same
        /// NavRay and findDistanceToWall the probe calls, from the same kind of start polygon, so what it
        /// prints is what a seat standing there would sense and not a second implementation of it.
        static std::string RayReport(Map* map, float x, float y, float z, float facing);

        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// Every action is movement: it is never charged for repeating (Actions.Repeat is not levied on movement), and
        /// a held key re-pressed is a no-op anyway.
        [[nodiscard]] bool IsMovement(uint32 /*local*/) const override { return true; }
    };
}

#endif
