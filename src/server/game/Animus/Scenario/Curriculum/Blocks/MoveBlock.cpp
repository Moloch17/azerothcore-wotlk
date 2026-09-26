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

#include "MoveBlock.h"
#include "GroundSense.h"
#include "SeatEncoder.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "DetourExtended.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "MovementTypedefs.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SeatView.h"
#include "TravelBlock.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace
{
    using Animus::Curriculum::MoveBlock;
    namespace Encoding = Animus::Curriculum::Encoding;
    using Animus::Curriculum::TravelBlock;
    using Animus::Curriculum::SeatOptionKind;
    namespace Ground = Animus::Curriculum::GroundSense;
    using Ground::NavRay;

    constexpr uint32 MOVE_POINT_ID = 0x4D56;    // "MV": this block's spline, distinct from the duel block's
    constexpr float YARD_SCALE = 40.0f;         // distances are reported as a fraction of this
    constexpr float OBJECTIVE_SCALE = 500.0f;   // an objective is further off than anything else it looks at
    constexpr float RUN_SPEED = 7.0f;           // yards a second, unmounted and unhasted (TravelBlock's)

    /// The world angle a bearing points at, given where the seat is looking. Bearings run clockwise from straight
    /// ahead, and WoW orientation runs counter-clockwise, so the eighth-turns are subtracted.
    float HeadingOf(float facing, uint32 bearing)
    {
        float const heading = facing - float(bearing) * float(M_PI) / 4.0f;
        return Position::NormalizeOrientation(heading);
    }

    /// The same for a sensing ray: sixteenths of a turn, so ray 2 * b lies along bearing b and the odd rays fall
    /// half way between two bearings, where a doorway sits as often as not.
    float RayHeading(float facing, uint32 ray)
    {
        float const heading = facing - float(ray) * float(M_PI) / 8.0f;
        return Position::NormalizeOrientation(heading);
    }

    /// `to`'s direction in the seat's own frame: 0 straight ahead, wrapped to (-pi, pi].
    float RelativeBearing(Position const& from, float facing, WorldObject const& to)
    {
        float const relative = from.GetAngle(to.GetPositionX(), to.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }

    float RelativeBearing(Position const& from, float facing, Position const& to)
    {
        float const relative = from.GetAngle(to.GetPositionX(), to.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }

    /// Off the ground, where the third dimension is real and pitch steers: swimming, or flying.
    bool Airborne(Player const* bot)
    {
        return bot && (bot->IsInWater() || bot->CanFly());
    }

    /// Bring the seat's own heading up to date for this decision, before anything is measured off it.
    ///
    /// `view.Facing` is the frame, not `bot->GetOrientation()`. The orientation belongs to whatever spline is
    /// running -- it is overwritten with the direction of travel every tick -- so measuring a bearing off it made
    /// the frame rotate by the bearing's own angle every decision, and a seat holding anything but straight ahead
    /// walked a spiral. Only BEARING_FORWARD was a fixed point, which is exactly the shape the failures had.
    ///
    /// Holding is therefore the default, including the 0xFF a seat starts an episode with: keep the heading you
    /// have unless you asked for something else. Facing along the path is the opt-in now, not the fallback.
    void UpdateFacing(Animus::Curriculum::SeatView& view)
    {
        Player const* bot = view.Bot;
        if (!bot)
            return;

        switch (view.FacingMode)
        {
            case MoveBlock::ACTION_FACE_TARGET:
                if (view.Target)
                    view.Facing = bot->GetAngle(view.Target);
                return;
            case MoveBlock::ACTION_FACE_HEADING:
                // Turn to face the way the feet are going -- and then, by construction, the way the feet are
                // going is straight ahead. Leaving the bearing where it was would rotate the frame again on the
                // next decision and re-create the spiral this whole change exists to remove: "face where I am
                // going" is a snap to a heading, not a standing instruction to keep turning.
                if (view.HeldBearing < MoveBlock::BEARING_COUNT)
                {
                    view.Facing = HeadingOf(view.Facing, view.HeldBearing);
                    view.HeldBearing = MoveBlock::BEARING_FORWARD;
                }
                return;
            case MoveBlock::ACTION_FACE_HOLD:
            default:
                return;                     // keep the heading it has, which is the point of both
        }
    }

    /// How long a jump hangs in the air, and how far it carries.
    ///
    /// Rising and falling take the same time, so the whole arc is 2 * speedZ / gravity -- about 825 ms at the
    /// player's own launch speed, which is three decisions at DecisionMs.
    uint64 JumpFlightMs()
    {
        return uint64(2000.0f * MoveBlock::JUMP_SPEED_Z / float(Movement::gravity));
    }

    float JumpRange(Player const* bot)
    {
        float const speedXY = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
        return 2.0f * (MoveBlock::JUMP_SPEED_Z / float(Movement::gravity)) * speedXY;
    }

    /// Whether the seat is on its way down right now: a fall spline running. Not Unit::IsFalling, which also reads
    /// MOVEMENTFLAG_FALLING -- a flag the core sets on a player when it starts a fall and clears only when a client
    /// reports the landing, which a seat never does, so on a bot it sticks from the first step down a slope to the
    /// end of the episode. Masking the feet on it masked them for good (stage1_move on format 7, first run: the
    /// scripted baseline arrived 0.23 against 0.84 the run before, stalling after ninety yards).
    bool Descending(Player const* bot)
    {
        return !bot->movespline->Finalized() && bot->movespline->Initialized() && bot->movespline->isFalling();
    }

    /// How high the arc rises above the launch: what JumpTo builds the parabola from.
    float JumpApex()
    {
        float const halfTime = MoveBlock::JUMP_SPEED_Z / float(Movement::gravity);
        return -Movement::computeFallElevation(halfTime, false, -MoveBlock::JUMP_SPEED_Z);
    }

    /// Where a jump along `heading` would come down, and how far below the seat that is.
    struct JumpAim
    {
        Position Landing;
        float Drop = 0.0f;      // launch height minus the landing's; negative is a step up
        bool Ok = false;
    };

    /// The landing test, one function for the mask and the press so the two cannot disagree.
    ///
    /// It used to be the core's CanReachPositionAndGetValidCoords, which is a Detour raycast *along the
    /// navmesh*: at a lip it clipped the landing back to the near edge, and its slope test refused anything
    /// more than the mesh's 1.6 yd climb below the start. So a jump could never drop off a ledge, and the header
    /// comment about gaps described a thing the code did not do. This looks at the landing point itself:
    ///
    ///   - the ground under the end of the arc, searched `search` yards down (Actions.JumpDropSearch). No ground
    ///     that deep is the void, and the one thing a jump is refused for. There is no upper bound on the drop
    ///     on purpose: what a fall costs is the seat's to learn (OBS_JUMP_DROP, and what happens), and with Slow
    ///     Fall or Levitate it costs nothing;
    ///   - up to JUMP_RISE_MAX above the launch, which a step clears anyway; higher is a wall;
    ///   - on the navmesh, within a step of the ground found (a landing the mesh does not cover is a fall onto
    ///     something the seat cannot walk on), water allowed only for a hop -- a fall into deep water ends at
    ///     the surface and costs nothing (Encoding::FallToGround), so a lake is never a drop to learn from;
    ///   - clear of collision in two legs: across at the apex, for a wall in the way, and straight down over
    ///     the landing, for a lip that overhangs it. One diagonal ray would cut every cliff face and refuse
    ///     every drop.
    ///
    /// A gap works the same way once the mesh resumes on the far side: the landing is there, the drop is small,
    /// and the arc is clear. Nothing here needs to change for it.
    JumpAim JumpLandingTest(Map* map, dtNavMeshQuery const* query, Player const* bot, float heading, float search)
    {
        JumpAim aim;
        if (!map || !query)
            return aim;

        float const range = JumpRange(bot);
        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float const bz = bot->GetPositionZ();
        float const ax = bx + range * std::cos(heading);
        float const ay = by + range * std::sin(heading);
        if (range < MoveBlock::JUMP_MIN_YARDS)
            return aim;

        uint32 const phase = bot->GetPhaseMask();
        float const groundZ = map->GetHeight(phase, ax, ay, bz + MoveBlock::MAX_STEP, true,
            std::max(search, MoveBlock::MAX_STEP) + MoveBlock::MAX_STEP);
        if (groundZ <= INVALID_HEIGHT)
            return aim;

        float const drop = bz - groundZ;
        if (drop < -MoveBlock::JUMP_RISE_MAX)
            return aim;

        // Detour's axes are {y, z, x}.
        dtQueryFilterExt filter;
        filter.setIncludeFlags(drop <= MoveBlock::DROP_ABOVE ? NAV_GROUND | NAV_WATER : NAV_GROUND);
        filter.setExcludeFlags(0);
        float const at[3] = { ay, groundZ, ax };
        float const extents[3] = { 1.5f, MoveBlock::MAX_STEP, 1.5f };
        float nearest[3] = { 0.0f, 0.0f, 0.0f };
        dtPolyRef ref = 0;
        if (dtStatusFailed(query->findNearestPoly(at, extents, &filter, &ref, nearest)) || !ref
            || std::fabs(nearest[1] - groundZ) > MoveBlock::MAX_STEP)
            return aim;

        float const apex = bz + JumpApex();
        float const collision = bot->GetCollisionHeight();
        if (!map->isInLineOfSight(bx, by, apex, ax, ay, apex, phase, LINEOFSIGHT_ALL_CHECKS,
                VMAP::ModelIgnoreFlags::Nothing)
            || !map->isInLineOfSight(ax, ay, bz + collision, ax, ay, groundZ + collision, phase,
                LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            return aim;

        aim.Landing.Relocate(ax, ay, groundZ);
        aim.Drop = drop;
        aim.Ok = true;
        return aim;
    }

    /// Redo the march if it has stopped describing where the seat is standing, and say whether it is usable.
    ///
    /// Eighty height samples and forty-eight rays against the eight queries the old probe made is too much to
    /// repeat every 250 ms for 128 environments, and it does not need repeating: the ground does not move, only
    /// the seat does. Movement is
    /// the trigger that matters -- at seven yards a second a one-second-old march is seven yards stale and its
    /// nearest cell is six -- with a turn threshold because the grid is egocentric, and a clock as a backstop.
    void RefreshProbe(Animus::Curriculum::SeatView const& view, Player* bot, float facing)
    {
        Animus::Curriculum::GroundProbe* probe = view.Probe;
        if (!probe)
            return;

        Map* map = bot->GetMap();   // non-const: Map::GetLiquidData is not a const member
        if (!map)
            return;

        if (probe->Valid)
        {
            float const moved = bot->GetExactDist(&probe->From);
            float const turned = std::fabs(std::atan2(std::sin(facing - probe->Facing),
                std::cos(facing - probe->Facing)));
            if (moved < MoveBlock::MARCH_REFRESH_YARDS && turned < MoveBlock::MARCH_REFRESH_RADIANS
                && view.NowMs - probe->Ms < MoveBlock::MARCH_REFRESH_MS)
                return;
        }

        namespace Encoder = Animus::Curriculum::SeatEncoder;
        auto probeMark = std::chrono::steady_clock::now();
        auto partMark = probeMark;

        // The seat's own polygon, once for all sixteen rays.
        Ground::Origin const at{ bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetPhaseMask(),
            bot->GetCollisionHeight() };
        dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
        dtPolyRef const startRef = Ground::StartPoly(query, at);

        for (uint32 ray = 0; ray < MoveBlock::RAY_COUNT; ++ray)
        {
            float const heading = RayHeading(facing, ray);
            partMark = std::chrono::steady_clock::now();
            Ground::March const march = Ground::MarchBearing(map, at, heading, 0.0f);
            Encoder::ChargeObserve(Encoder::OBSERVE_PROBE_MARCH, partMark);
            Ground::Rays const rays = Ground::CastRays(query, startRef, at, heading);
            Encoder::ChargeObserve(Encoder::OBSERVE_PROBE_RAYS, partMark);

            Ground::Bearing const bearing = Ground::Combine(march, rays);
            probe->Reach[ray] = bearing.Reach;
            probe->Step[ray] = bearing.Step;
            probe->Shore[ray] = bearing.Shore;
            probe->Burns[ray] = bearing.Burns;
        }

        // How much room the seat has, and which way is out: one query, from the same polygon as the rays.
        Ground::Room const room = Ground::MeasureRoom(query, startRef, at);
        probe->Clearance = room.Clearance;
        probe->ClearanceSin = room.Directed ? std::sin(room.Away - facing) : 0.0f;
        probe->ClearanceCos = room.Directed ? std::cos(room.Away - facing) : 0.0f;

        // Where a jump would come down, cached with the rest. The mask reads this; the press reads it too while
        // the cache still describes where the seat stands, and measures again once it has moved or turned.
        JumpAim const aim = JumpLandingTest(map, query, bot, facing, view.JumpDropSearch);
        probe->CanJump = !Descending(bot) && !probe->JumpDropPending && aim.Ok;
        probe->JumpLanding = aim.Landing;
        probe->JumpDrop = aim.Ok ? aim.Drop : 0.0f;

        probe->From.Relocate(bot);
        probe->Facing = facing;
        probe->Ms = view.NowMs;
        probe->Valid = true;
        Encoder::ChargeObserve(Encoder::OBSERVE_PROBE, probeMark);
    }

    /// Take a trail sample when one is due, then write the trail into the block's row: each sample as an offset
    /// from where the seat stands now, in its own frame (ahead, left) over YARD_SCALE, oldest first with the newest
    /// in the last pair, then the share of the samples it is still within DWELL_YARDS of. The first observation of
    /// an episode takes the first sample, so the trail always knows where the seat set out from.
    void ObserveTrail(Animus::Curriculum::SeatView const& view, Player const* bot, float* out)
    {
        using Animus::Curriculum::MovementTrail;
        using Animus::Curriculum::TRAIL_SAMPLES;

        MovementTrail* trail = view.Trail;
        if (!trail)
            return;

        if (!trail->Started || view.NowMs < trail->LastMs
            || view.NowMs - trail->LastMs >= MovementTrail::INTERVAL_MS)
        {
            trail->X[trail->Next] = bot->GetPositionX();
            trail->Y[trail->Next] = bot->GetPositionY();
            trail->Next = (trail->Next + 1) % TRAIL_SAMPLES;
            trail->Count = std::min(trail->Count + 1, TRAIL_SAMPLES);
            trail->LastMs = view.NowMs;
            trail->Started = true;
        }

        float const cosFacing = std::cos(view.Facing);
        float const sinFacing = std::sin(view.Facing);
        uint32 dwelling = 0;
        for (uint32 i = 0; i < trail->Count; ++i)
        {
            // Oldest first: with the ring full, the oldest sample is the slot Next points at.
            uint32 const slot = (trail->Next + TRAIL_SAMPLES - trail->Count + i) % TRAIL_SAMPLES;
            float const dx = trail->X[slot] - bot->GetPositionX();
            float const dy = trail->Y[slot] - bot->GetPositionY();
            // Into the seat's frame: ahead is +x and left is +y, orientation running counter-clockwise.
            float const ahead = dx * cosFacing + dy * sinFacing;
            float const left = dy * cosFacing - dx * sinFacing;
            uint32 const feature = MoveBlock::OBS_TRAIL_FIRST + 2 * (TRAIL_SAMPLES - trail->Count + i);
            out[feature] = std::clamp(ahead / YARD_SCALE, -1.0f, 1.0f);
            out[feature + 1] = std::clamp(left / YARD_SCALE, -1.0f, 1.0f);
            if (dx * dx + dy * dy <= MovementTrail::DWELL_YARDS * MovementTrail::DWELL_YARDS)
                ++dwelling;
        }
        out[MoveBlock::OBS_TRAIL_DWELL] = float(dwelling) / float(TRAIL_SAMPLES);
    }

    /// One step of a held turn, and one of a held pitch. Exactly once per decision, whoever asks: BeforeApply
    /// on a decision the key is merely held, Apply on the decision it goes down, so the first press does
    /// something rather than waiting 250 ms for the next decision to notice it.
    void StepTurn(Animus::Curriculum::SeatView& view)
    {
        Player const* bot = view.Bot;
        if (!bot || view.Turning == 0 || !bot->IsAlive() || bot->HasUnitState(Encoding::IMMOBILE_STATES))
            return;

        // Straight onto the seat's own heading. SetFacingTo launches an orientation-only spline, and the move
        // spline Steer issues does MotionMaster::Clear() and replaces it before a single world tick can apply
        // it -- so a held turn did nothing at all while the seat was walking, which is every decision that
        // matters. It also left GetOrientation() unchanged for the heading Steer computes, so the turn did not
        // even steer the decision it was pressed on.
        view.Facing = Position::NormalizeOrientation(view.Facing + float(view.Turning) * MoveBlock::TURN_STEP);
    }

    void StepPitch(Animus::Curriculum::SeatView& view)
    {
        if (view.PitchTurning != 0 && Airborne(view.Bot))
            view.Pitch = std::clamp(view.Pitch + float(view.PitchTurning) * MoveBlock::PITCH_STEP,
                -MoveBlock::PITCH_MAX, MoveBlock::PITCH_MAX);
    }

    /// Settle where the seat is looking and carry it -- on the move spline if the feet are going somewhere, as a
    /// turn on the spot if they are not.
    ///
    /// Split out of BeforeApply so that it can be re-run the moment an action changes the steering, without
    /// re-running the things that must happen exactly once a decision. Pressing a facing or a bearing used to
    /// call the whole of BeforeApply again, which stepped a held turn a second time in the same 250 ms: the turn
    /// rate doubled whenever the policy did anything else while turning.
    void Steer(Animus::Curriculum::SeatView& view)
    {
        Player* bot = view.Bot;
        if (!bot || !view.Option)
            return;

        bool const alive = bot->IsAlive() && !bot->HasUnitState(Encoding::IMMOBILE_STATES);

        // Where the head is pointing this decision, settled before a single bearing is measured off it.
        UpdateFacing(view);

        if (!view.Option->Running(SeatOptionKind::MoveBearing, view.NowMs)
            || view.HeldBearing >= MoveBlock::BEARING_COUNT)
        {
            view.HeldBearing = 0xFF;
            // Standing still, so no move spline will carry the heading: push it onto the unit here instead, or a
            // turn and a FACE_* would be things the seat believed about itself that the world did not share.
            // UpdatePosition with the same coordinates is a pure turn, and it is a no-op when the angle is unchanged.
            if (alive)
                bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), view.Facing);
            return;
        }

        if (!alive)
            return;

        // Re-aimed from where the seat is now, every decision it keeps walking. Aiming once at a point chosen when the
        // key went down would walk it into the first wall the ground put in the way; recomputing lets the path bend.
        float const heading = HeadingOf(view.Facing, view.HeldBearing);
        bool const airborne = Airborne(bot);
        float const pitch = airborne ? view.Pitch : 0.0f;
        float const reach = MoveBlock::STEP_YARDS * std::cos(pitch);

        Position destination = *bot;
        destination.Relocate(bot->GetPositionX() + reach * std::cos(heading),
            bot->GetPositionY() + reach * std::sin(heading),
            bot->GetPositionZ() + MoveBlock::STEP_YARDS * std::sin(pitch));

        if (airborne)
        {
            // Swimming and flying are steered in three dimensions and must not be snapped to the ground: the whole
            // point of a pitch is to leave it. A climb still stops at the ceiling the air has.
            float const facing = view.Facing;
            if (!bot->CanFly())
            {
                // In the water. Keep the seat under the surface rather than skimming along the top of it, and swim
                // rather than fly: a spline with the fly flag on a swimmer is a different animal.
                Encoding::SwimTo(bot, destination.GetPositionX(), destination.GetPositionY(),
                    destination.GetPositionZ(), &facing);
                return;
            }

            float const ceiling = bot->GetPositionZ()
                + (TravelBlock::MAX_ALTITUDE - TravelBlock::HeightAboveGround(bot));
            destination.Relocate(destination.GetPositionX(), destination.GetPositionY(),
                std::min(destination.GetPositionZ(), ceiling));

            Encoding::FlyTo(bot, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                &facing);
            return;
        }

        // On land, but the step leads into water. This is the one move the seat could never make: the walkable mesh
        // ends at the waterline, so a pathfound ground step into a lake has nowhere to land and the seat stops on
        // the shore -- and it could not start swimming, because swimming was only ever reached by already being in
        // the water. Over 200 sampled decisions across a whole run, no seat ever got its feet below the surface;
        // every sample near water sat 0.1 to 0.4 yards above it. Entering is therefore its own case: go straight in,
        // to just under the surface, and from the next decision `airborne` is true and the seat is swimming.
        if (Map* map = bot->GetMap())
        {
            LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), destination.GetPositionX(),
                destination.GetPositionY(), bot->GetPositionZ(), bot->GetCollisionHeight(), {});
            // Water and ocean only: stepping into magma or slime is not a crossing, it is a death, and the probe
            // reports it as no reach for that reason.
            if (liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT
                && (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0
                && liquid.Level >= bot->GetPositionZ() - MoveBlock::MAX_STEP)
            {
                // With a water-walking aura the lake is a floor: the step lands on the surface, and the movement
                // flag the core would only set on a client's acknowledgement (Unit::SetWaterWalking sends a packet
                // to a client-controlled player and waits) is set here, the way AllowFlight sets CAN_FLY.
                if (bot->HasWaterWalkAura())
                {
                    bot->AddUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
                    Encoding::SwimTo(bot, destination.GetPositionX(), destination.GetPositionY(),
                        liquid.Level + MoveBlock::WATER_WALK_ABOVE, &view.Facing);
                    return;
                }
                Encoding::SwimTo(bot, destination.GetPositionX(), destination.GetPositionY(),
                    liquid.Level - bot->GetCollisionHeight() * 0.5f, &view.Facing);
                return;
            }
        }

        // Put the destination on the ground where there is ground to put it on. Where there is not -- the bearing
        // leads off the map, or over a drop deeper than a step -- the move is issued anyway, at the unsnapped point.
        //
        // Refusing to move in that case is what it looked like it should do, and it is wrong: the seat then stands
        // still, and standing still is the one outcome this whole block exists to prevent. The direction is still the
        // policy's; only the height of a point eight yards away is being guessed at, and the path the spline takes
        // sorts that out. The ground probe is how the seat learns not to choose such a bearing in the first place.
        // Deliberately discarded: see above -- a failure is a reason to move anyway, not a reason to stand still.
        (void)Encoding::SnapToGround(bot->GetMap(), bot->GetPhaseMask(), destination, bot->GetPositionZ(),
            MoveBlock::STEP_YARDS);

        // Pathfinding on, which is the default: a bearing is where the seat wants to go, not a licence to walk through
        // a wall to get there.
        Encoding::MoveTo(bot, MOVE_POINT_ID, destination.GetPositionX(), destination.GetPositionY(),
            destination.GetPositionZ(), &view.Facing);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::MoveBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::MoveBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["bearings"] = uint32(BEARING_COUNT);
    block["rays"] = uint32(RAY_COUNT);
    block["trail_samples"] = uint32(TRAIL_SAMPLES);
    block["trail_interval_ms"] = uint32(MovementTrail::INTERVAL_MS);
    block["step_yards"] = double(STEP_YARDS);
    block["turn_step"] = double(TURN_STEP);
    block["pitch_step"] = double(PITCH_STEP);
    block["pitch_max"] = double(PITCH_MAX);
    block["probe_yards"] = double(PROBE_YARDS);
    boost::json::array ranges;
    for (float range : MARCH_RANGES)
        ranges.push_back(double(range));
    block["march_ranges"] = std::move(ranges);
    block["march_max"] = double(MARCH_MAX);
    block["clearance_range"] = double(CLEARANCE_RANGE);
    block["jump_speed_z"] = double(JUMP_SPEED_Z);
    block["jump_rise_max"] = double(JUMP_RISE_MAX);
    block["jump_drop_scale"] = double(JUMP_DROP_SCALE);
}

std::string Animus::Curriculum::MoveBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "move_forward", "move_forward_right", "move_right", "move_back_right",
        "move_back", "move_back_left", "move_left", "move_forward_left",
        "halt", "face_target", "face_heading", "face_hold",
        "turn_left", "turn_right", "pitch_up", "pitch_down", "pitch_level", "jump",
    };

    return local < NAMES.size() ? NAMES[local] : std::string();
}

std::string Animus::Curriculum::MoveBlock::RayReport(Map* map, float x, float y, float z, float facing)
{
    if (!map)
        return "no map\n";

    dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    if (!query)
        return "no navmesh query on this map -- mmaps are not loaded for it\n";

    dtQueryFilterExt filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
    filter.setExcludeFlags(0);
    float const at[3] = { y, z, x };
    float const extents[3] = { 3.0f, 5.0f, 3.0f };
    dtPolyRef startRef = 0;
    if (dtStatusFailed(query->findNearestPoly(at, extents, &filter, &startRef, nullptr)) || !startRef)
        return "that point is not on the navmesh within {3, 5, 3} of itself\n";

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    // Whether this is a room, by the same test the objective generator applies, and where the floor under it
    // actually is. Both are here because a spawn point taken from a table of coordinates is a guess until
    // something stands on it: an areatrigger's centre can sit in a courtyard, on a roof, or -- as the Astranaar
    // inn's does -- in the gap between two storeys, where it is on no floor at all.
    {
        uint32 mogpFlags = 0;
        int32 adtId = 0;
        int32 rootId = 0;
        int32 groupId = 0;
        if (!map->GetAreaInfo(PHASEMASK_NORMAL, x, y, z, mogpFlags, adtId, rootId, groupId))
            out << "  inside       no -- no building here at all\n";
        else if ((mogpFlags & 0x8) != 0)
            out << "  inside       no -- in a building, but this group is flagged outdoors\n";
        else
            out << "  inside       yes\n";

        // Downward from just above the point, which is the core's own idiom: GetHeight cannot see a floor above
        // where it starts, so a point given too high finds the storey below and one given too low finds nothing.
        float const floor = map->GetHeight(PHASEMASK_NORMAL, x, y, z + 2.0f, true, 20.0f);
        if (floor > INVALID_HEIGHT)
            out << "  floor        z " << floor << ", which is " << (z - floor) << " below the point given\n";
        else
            out << "  floor        none within 20 yd below z " << (z + 2.0f) << "\n";
        out << "\n";
    }

    out << "  ray          heading     dry     wet     all  beyond   burn\n";
    out << "  -----------  -------  ------  ------  ------  ------  -----\n";

    // The eight bearings by name and, between each pair, the ray that lies half way: "fwd|fr" is the ray
    // between forward and forward-right.
    static constexpr char const* NAMES[RAY_COUNT] =
    {
        "forward", "fwd|fr", "fwd-right", "fr|right", "right", "right|br", "back-right", "br|back",
        "back", "back|bl", "back-left", "bl|left", "left", "left|fl", "fwd-left", "fl|fwd"
    };

    for (uint32 bearing = 0; bearing < RAY_COUNT; ++bearing)
    {
        float const heading = RayHeading(facing, bearing);
        float const wet = NavRay(query, startRef, x, y, z, heading, MARCH_MAX, NAV_GROUND | NAV_WATER);
        float const dry = NavRay(query, startRef, x, y, z, heading, MARCH_MAX, NAV_GROUND);
        float const all = NavRay(query, startRef, x, y, z, heading, MARCH_MAX,
            NAV_GROUND | NAV_WATER | NAV_MAGMA | NAV_SLIME);

        out << "  " << std::setw(11) << std::left << (bearing < RAY_COUNT ? NAMES[bearing] : "?") << std::right
            << "  " << std::setw(7) << (heading * 180.0f / float(M_PI))
            << "  " << std::setw(6) << dry
            << "  " << std::setw(6) << wet
            << "  " << std::setw(6) << all;

        // How much further the wet ray got than the dry one. This is NOT the width of the water, though the
        // plan that asked for these rays said it was, and the first bench run at the Barrens oasis is what
        // showed otherwise: the wet filter crosses water *and* ground, so once past a shore it keeps going over
        // whatever is on the far side and stops only at a wall. A two yard channel and a two yard shore of a
        // forty yard lake both report the same thirty-eight. What the seat actually gets from the pair is the
        // distance to the water's edge, which is `dry`, and that is real and is new -- the plane it replaced
        // was a yes or no sampled at five fixed ranges. Width would need a ray that starts past the shore and
        // is filtered to water alone; it is not derived here and is not claimed anywhere.
        if (wet >= 0.0f && dry >= 0.0f && wet > dry)
            out << "  " << std::setw(6) << (wet - dry);
        else
            out << "       -";

        if (all >= 0.0f && wet >= 0.0f && all > wet + BURN_EDGE_MARGIN)
            out << "  " << std::setw(5) << wet;
        else
            out << "      -";
        out << "\n";
    }

    // Clearance, which is the one number a wrong swizzle shows up in on its own: in a corridor it must be
    // small, in open country it must run to the full search radius, and the way out must point at the middle
    // of the corridor rather than into the wall.
    float distance = 0.0f;
    float hit[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 0.0f, 0.0f };
    if (dtStatusSucceed(query->findDistanceToWall(startRef, at, CLEARANCE_RANGE, &filter, &distance, hit,
        normal)))
    {
        bool const directed = distance < CLEARANCE_RANGE && std::isfinite(normal[0]) && std::isfinite(normal[2])
            && normal[0] * normal[0] + normal[2] * normal[2] > 1e-6f;
        float const away = directed ? std::atan2(normal[0], normal[2]) : 0.0f;
        if (!directed)
        {
            out << "\n  clearance    nothing within " << CLEARANCE_RANGE << " yd, so no way out to point at\n";
        }
        else
        {
            out << "\n  clearance    " << distance << " yd of " << CLEARANCE_RANGE
                << ", the way out bears " << (away * 180.0f / float(M_PI)) << " deg world, "
                << (std::atan2(std::sin(away - facing), std::cos(away - facing)) * 180.0f / float(M_PI))
                << " deg from the facing\n";
            out << "  nearest edge (" << hit[2] << ", " << hit[0] << ", " << hit[1] << ") world xyz\n";
        }

        // Three ways of measuring one wall, which must agree.
        //
        // This is the check the whole command exists for. Detour's axes are {y, z, x}, and a swizzle that is
        // wrong returns plausible numbers about the wrong place, which no amount of staring at terrain will
        // settle -- the ground around a lake is irregular enough to explain almost any reading. So the wall is
        // measured three independent ways instead: the distance findDistanceToWall reports, the distance to the
        // coordinates it hands back, and how far a ray cast at that wall's own bearing runs before it stops.
        //
        // The first two agreeing proves the input point and the returned position use the convention this code
        // thinks they do. The third agreeing proves the ray's direction does too, because it is built from
        // sin and cos of a bearing rather than from a position. Nothing here depends on knowing the terrain,
        // so it is a real test anywhere there is a wall within range.
        if (distance < CLEARANCE_RANGE)
        {
            float const edgeX = hit[2];
            float const edgeY = hit[0];
            float const measured = std::sqrt((edgeX - x) * (edgeX - x) + (edgeY - y) * (edgeY - y));
            float const toEdge = std::atan2(edgeY - y, edgeX - x);
            float const along = NavRay(query, startRef, x, y, z, toEdge, MARCH_MAX, NAV_GROUND | NAV_WATER);
            out << "  self-check   reported " << distance << " yd, its coordinates are " << measured
                << " yd away, a ray at its bearing stops at " << along << " yd -- these must agree\n";
        }
    }
    else
    {
        out << "\n  clearance    no wall within " << CLEARANCE_RANGE << " yd\n";
    }

    return out.str();
}

void Animus::Curriculum::MoveBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    // `obs` and `mask` are already this block's own slice of the seat's row: SeatEncoder::Observe offsets them
    // before it calls a block. Offsetting again wrote the whole block past the end of its slice, so every bearing
    // stayed masked and no seat could steer (stage1_move, 2026-09-21).
    float* out = obs;
    Player* bot = view.Bot;

    // Everything a seat needs to place its feet, and nothing about whether it has an enemy: this block is the one
    // that still works when there is nothing to fight.
    // A jump owns the feet until it lands. Every movement action begins by calling DisableSpline, so a step or
    // a second jump pressed mid-arc would cancel the parabola from wherever the seat had got to and leave it
    // walking on air -- and the core's own IsFalling cannot see this coming, because MoveSplineFlag's
    // EnableParabolic clears the Falling bit it tests. The arc is a known 825 ms, so the honest fix is to hold
    // the clock ourselves and mask the feet for as long as they are not under the seat.
    bool const inFlight = view.Probe && view.NowMs < view.Probe->JumpUntilMs;

    // A mount cast owns the feet the same way a jump arc does, and for the same reason: it takes several
    // decisions and any movement in them destroys it. Without this the mount was unreachable by construction --
    // the scripted policy cancelled its own every time, and a learned one has to find a run of consecutive
    // decisions in which it presses nothing that moves, while potential shaping charges it for the pause.
    //
    // Only a mount. Protecting casts in general would stop a seat walking out of fire mid-spell, and that is a
    // thing it must always be able to do.
    bool const mounting = bot && Encoding::MountCastInProgress(bot);
    // And a fall: a bearing pressed on the way down would Clear() the fall spline from mid-air and start a second
    // fall from there, with a second HandleFall at the bottom. The spline's own word and this block's drop flag,
    // never the core's falling flag (Descending).
    bool const falling = bot && (Descending(bot) || (view.Probe && view.Probe->JumpDropPending));
    bool const canMove = bot && bot->IsAlive() && !bot->HasUnitState(Encoding::IMMOBILE_STATES)
        && !inFlight && !mounting && !falling;
    bool const airborne = Airborne(bot);

    if (bot)
    {
        // Every bearing in this block is measured off the seat's own heading, so the observation has to report
        // that and not the spline's idea of it.
        float const facing = view.Facing;
        out[OBS_MOVING] = bot->isMoving() ? 1.0f : 0.0f;
        // Over twice the unhasted run speed and clamped: a mounted seat read 2.0 here and a flying one nearly 4,
        // which are not features in [0, 1] and were the widest inputs the row had.
        out[OBS_SPEED] = std::min(1.0f, bot->GetSpeed(MOVE_RUN) / (2.0f * RUN_SPEED));
        out[OBS_FACING_SIN] = std::sin(facing);
        out[OBS_FACING_COS] = std::cos(facing);

        if (view.HeldBearing < BEARING_COUNT)
            out[OBS_BEARING_HELD + view.HeldBearing] = 1.0f;
        else
            out[OBS_BEARING_NONE] = 1.0f;

        out[OBS_TURNING_LEFT] = view.Turning > 0 ? 1.0f : 0.0f;
        out[OBS_TURNING_RIGHT] = view.Turning < 0 ? 1.0f : 0.0f;
        out[OBS_PITCH_SIN] = std::sin(view.Pitch);
        out[OBS_PITCH_COS] = std::cos(view.Pitch);

        if (Unit const* target = view.Target)
        {
            float const relative = RelativeBearing(*bot, facing, *target);
            out[OBS_TARGET_BEARING_SIN] = std::sin(relative);
            out[OBS_TARGET_BEARING_COS] = std::cos(relative);
            out[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(target) / YARD_SCALE);
        }

        // The nearest ground effect it is not standing in, in the same frame: which way it lies and how wide, so
        // the seat can walk round one rather than only out of one. Its bearing is already relative to facing.
        if (view.NearestHazard.Present)
        {
            out[OBS_HAZARD_BEARING_SIN] = std::sin(view.NearestHazard.Bearing);
            out[OBS_HAZARD_BEARING_COS] = std::cos(view.NearestHazard.Bearing);
            out[OBS_HAZARD_DISTANCE] = std::min(1.0f, view.NearestHazard.Distance / YARD_SCALE);
            out[OBS_HAZARD_RADIUS] = std::min(1.0f, view.NearestHazard.Radius / YARD_SCALE);
        }

        if (view.HasObjective)
        {
            float const relative = RelativeBearing(*bot, facing, view.Objective);
            out[OBS_OBJECTIVE] = 1.0f;
            out[OBS_OBJECTIVE_BEARING_SIN] = std::sin(relative);
            out[OBS_OBJECTIVE_BEARING_COS] = std::cos(relative);
            float const range = bot->GetExactDist2d(&view.Objective);
            out[OBS_OBJECTIVE_DISTANCE] = std::min(1.0f, range / OBJECTIVE_SCALE);
            // The same distance again, over forty yards rather than five hundred. Every episode this stage loses
            // ends twenty to forty-five yards short, which is a twelfth of the coarse feature's range and half
            // of this one's.
            out[OBS_OBJECTIVE_NEAR] = std::min(1.0f, range / YARD_SCALE);
        }

        // What the ground is like each way it could go. Skipped in the air and in the water, where the ground is
        // not what the seat is steering against and the samples would only report the bottom.
        if (!airborne)
        {
            RefreshProbe(view, bot, facing);
            if (GroundProbe const* probe = view.Probe)
                for (uint32 ray = 0; ray < RAY_COUNT; ++ray)
                {
                    out[OBS_GROUND_FIRST + ray] = probe->Reach[ray];
                    out[OBS_STEP_FIRST + ray] = probe->Step[ray];
                    out[OBS_SHORE_FIRST + ray] = probe->Shore[ray];
                    out[OBS_BURNS_FIRST + ray] = probe->Burns[ray];
                }

            if (GroundProbe const* probe = view.Probe)
            {
                out[OBS_CLEARANCE] = probe->Clearance;
                out[OBS_CLEARANCE_SIN] = probe->ClearanceSin;
                out[OBS_CLEARANCE_COS] = probe->ClearanceCos;
                out[OBS_CAN_JUMP] = probe->CanJump ? 1.0f : 0.0f;
                out[OBS_JUMP_DROP] = probe->CanJump
                    ? std::clamp(probe->JumpDrop / JUMP_DROP_SCALE, 0.0f, 1.0f) : 0.0f;
            }
        }
        else
        {
            // Off the ground there is nothing underfoot to walk onto or refuse: every way is open, the ground
            // changes by nothing, and a seat that is swimming is surrounded by the water it is in. The march
            // is dropped rather than kept, so the first one made after coming ashore is a fresh one -- and the
            // clearance it held goes with it, because the travel encounter's clearance charge reads the probe
            // too, and was charging a swimmer for the bank it stood beside before it got in.
            for (uint32 ray = 0; ray < RAY_COUNT; ++ray)
            {
                out[OBS_GROUND_FIRST + ray] = 1.0f;
                out[OBS_SHORE_FIRST + ray] = bot->IsInWater() ? 0.0f : 1.0f;
            }
            out[OBS_CLEARANCE] = 1.0f;
            if (view.Probe)
            {
                view.Probe->Valid = false;
                view.Probe->Clearance = 1.0f;
                view.Probe->ClearanceSin = 0.0f;
                view.Probe->ClearanceCos = 0.0f;
            }
        }

        out[OBS_FALLING] = (view.Probe && view.Probe->JumpDropPending) || bot->IsFalling() ? 1.0f : 0.0f;
        out[OBS_IN_WATER] = bot->IsInWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED] = bot->IsUnderWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED_TIME] = std::min(1.0f, view.BreathSpent);
        out[OBS_SWIM_SPEED] = bot->GetSpeed(MOVE_SWIM) / RUN_SPEED;
        out[OBS_AIRBORNE] = airborne ? 1.0f : 0.0f;
    }

    // The way round against the way through, and whether the legs are getting anywhere. All three are the
    // scenario's to measure -- one at the episode's build, two over the last second -- because none of them can
    // be seen from a probe of any length.
    // Which way it has told itself to look. The FACE_* actions are masked while they are the mode being held, so
    // without this the policy could only infer its own steering state from what it was forbidden to press.
    out[OBS_FACING_MODE_FIRST + (view.FacingMode >= ACTION_FACE_TARGET && view.FacingMode <= ACTION_FACE_HOLD
        ? 1 + view.FacingMode - ACTION_FACE_TARGET : 0)] = 1.0f;

    out[OBS_DETOUR] = std::clamp(view.Detour / 4.0f, 0.0f, 1.0f);
    out[OBS_MOVE_RATE] = std::clamp(view.MoveRate, 0.0f, 1.0f);
    out[OBS_CLOSE_RATE] = std::clamp(view.CloseRate, -1.0f, 1.0f);

    // Where it has been, in its own frame, sampled here once a second because this is the one place that runs
    // for every seat every decision, in training and in play alike.
    if (bot)
        ObserveTrail(view, bot, out);

    if (!mask)
        return;

    uint8* allowed = mask;
    for (uint32 bearing = 0; bearing < BEARING_COUNT; ++bearing)
        allowed[ACTION_BEARING_FIRST + bearing] = canMove ? 1 : 0;

    // The bearing already being walked is masked: pressing it again would be a repeat of a key already held, and
    // the option machinery re-issues the spline each decision without being asked.
    if (canMove && view.HeldBearing < BEARING_COUNT && view.Option
        && view.Option->Running(SeatOptionKind::MoveBearing, view.NowMs))
        allowed[ACTION_BEARING_FIRST + view.HeldBearing] = 0;

    // Halting is only worth offering while something is being walked.
    allowed[ACTION_HALT] = canMove && view.HeldBearing < BEARING_COUNT ? 1 : 0;

    // Where it looks is a choice it can always make, alive and able to turn. Facing the target needs one.
    bool const canTurn = bot && bot->IsAlive() && !bot->HasUnitState(Encoding::STUN_STATES);
    allowed[ACTION_FACE_TARGET] = canTurn && view.Target && view.Target->IsAlive() ? 1 : 0;
    allowed[ACTION_FACE_HEADING] = canTurn ? 1 : 0;
    allowed[ACTION_FACE_HOLD] = canTurn ? 1 : 0;
    for (uint32 face = ACTION_FACE_TARGET; face <= ACTION_FACE_HOLD; ++face)
        if (view.FacingMode == face)
            allowed[face] = 0;              // already holding its head that way

    // Turning is the mouse-look, and it means nothing while the head is aimed at something in the world:
    // FACE_TARGET recomputes the heading from the target's position every decision, so a turn under it is
    // overwritten before it can steer anything. Masking says what is true: there is no heading to choose while
    // something else is choosing it. To look elsewhere, take the head back with FACE_HOLD first.
    //
    // FACE_OBJECTIVE was the other aimed mode, and it is gone: a heading the engine snapped to the objective every
    // decision was a compass held for the policy, and the trained policy collapsed onto it -- the episodes that
    // failed pressed seven turns to an arrival's two, and in the worst of them a turn was followed by
    // face_objective 22 times out of 27 -- and learned nothing about the ground.
    bool const aimed = view.FacingMode == ACTION_FACE_TARGET;
    allowed[ACTION_TURN_LEFT] = canTurn && !aimed && view.Turning <= 0 ? 1 : 0;
    allowed[ACTION_TURN_RIGHT] = canTurn && !aimed && view.Turning >= 0 ? 1 : 0;

    // A jump is legs, so it goes with the other movement: on the ground, not already in the air, and only
    // where the cached probe found somewhere to land. Apply checks the landing again before it commits.
    allowed[ACTION_JUMP] = canMove && !airborne && bot
        && view.Probe && view.Probe->CanJump ? 1 : 0;   // canMove already excludes an arc or a fall in the air

    // Pitch only means something off the ground. On foot the ground decides the seat's height, so the three
    // actions are masked rather than merely useless -- a masked action cannot be explored into.
    bool const canPitch = canTurn && airborne;
    allowed[ACTION_PITCH_UP] = canPitch && view.Pitch < PITCH_MAX ? 1 : 0;
    allowed[ACTION_PITCH_DOWN] = canPitch && view.Pitch > -PITCH_MAX ? 1 : 0;
    allowed[ACTION_PITCH_LEVEL] = canPitch && std::fabs(view.Pitch) > 0.01f ? 1 : 0;
}

void Animus::Curriculum::MoveBlock::BeforeApply(SeatView& view, SeatActionResult& result) const
{
    if (!view.Bot || !view.Option)
        return;

    // The end of a drop jump: the arc ran out at the launch height over the edge, and what happens next is the
    // core's own fall with the core's own damage. Here rather than only in the travel block, which does the same
    // for a dismount, because this block is in every stage and a drop in a pack stage must not leave the seat
    // standing on air. Idempotent with the travel block's call: the second sees the fall spline running.
    // A water-walking flag this block set outlives its aura on a client-controlled player (the core only clears
    // it on a client's acknowledgement), so it comes off here when the aura has.
    if (view.Bot->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING) && !view.Bot->HasWaterWalkAura())
        view.Bot->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);

    // The core's falling flag, stuck on a seat that is standing on the ground (Descending): taken off here, every
    // decision, so nothing downstream that still asks Unit::IsFalling -- the core's own movement code among them --
    // sees a seat that landed a minute ago as still in the air.
    if (view.Bot->IsAlive() && view.Bot->movespline->Finalized()
        && view.Bot->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR)
        && TravelBlock::HeightAboveGround(view.Bot) <= DROP_ABOVE)
        view.Bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);

    if (view.Probe && view.Probe->JumpDropPending && view.Bot->movespline->Finalized())
    {
        view.Probe->JumpDropPending = false;
        float fell = 0.0f;
        float cost = 0.0f;
        if (Encoding::FallToGround(view.Bot, &fell, &cost))
        {
            ++result.Falls;
            result.FallYards += fell;
            result.FallDamage += cost;
        }
    }

    // The held keys come up on their own when their clocks run out, and what they turned to is kept.
    if (!view.Option->Running(SeatOptionKind::MoveTurn, view.NowMs))
        view.Turning = 0;
    if (!view.Option->Running(SeatOptionKind::MovePitch, view.NowMs))
        view.PitchTurning = 0;

    // A held turn swings the seat a little further every decision it stays down, which is what makes every
    // heading between two compass points reachable. It happens before the feet are re-aimed, so a bearing walked
    // under a turn curves rather than stepping.
    StepTurn(view);
    StepPitch(view);
    Steer(view);
}

void Animus::Curriculum::MoveBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    Player* bot = view.Bot;
    if (!bot || !view.Option)
        return;

    if (local < ACTION_HALT)
    {
        view.HeldBearing = uint8(local - ACTION_BEARING_FIRST);
        view.Option->Start(SeatOptionKind::MoveBearing, view.NowMs + view.Options.MoveBearingMs);
        // Steer walks it on the next decision anyway; do it now so the seat is not still for one.
        Steer(view);
        return;
    }

    if (local == ACTION_HALT)
    {
        view.HeldBearing = 0xFF;
        view.Option->Stop(SeatOptionKind::MoveBearing);
        bot->StopMoving();
        return;
    }

    if (local <= ACTION_FACE_HOLD)
    {
        view.FacingMode = uint8(local);
        // A turn already held would otherwise keep running under a mode that overwrites it, and the turn actions
        // are masked from here on, so nothing could stop it.
        if (local == ACTION_FACE_TARGET)
        {
            view.Turning = 0;
            view.Option->Stop(SeatOptionKind::MoveTurn);
        }

        // Steer settles the heading and carries it, whether the seat is walking (on the move spline) or standing
        // still (as a turn on the spot). Choosing where to look should do something on the decision it is
        // chosen, not on the next one the seat happens to move.
        Steer(view);
        return;
    }

    if (local == ACTION_TURN_LEFT || local == ACTION_TURN_RIGHT)
    {
        // Left is counter-clockwise, which is the positive way round in WoW's orientation -- the same convention
        // HeadingOf subtracts for a clockwise bearing. It was the other way round: turn_left turned right. A
        // policy learns whichever it is, but a scripted baseline and a reader of the traces do not.
        view.Turning = local == ACTION_TURN_LEFT ? 1 : -1;
        view.Option->Start(SeatOptionKind::MoveTurn, view.NowMs + view.Options.MoveTurnMs);
        // Turn now rather than a decision from now, so the first press of a key does something. Only the turn:
        // re-running the whole of BeforeApply would step the turn a second time in the same 250 ms.
        StepTurn(view);
        Steer(view);
        return;
    }

    if (local == ACTION_PITCH_UP || local == ACTION_PITCH_DOWN)
    {
        view.PitchTurning = local == ACTION_PITCH_UP ? 1 : -1;
        view.Option->Start(SeatOptionKind::MovePitch, view.NowMs + view.Options.MovePitchMs);
        StepPitch(view);
        Steer(view);
        return;
    }

    if (local == ACTION_PITCH_LEVEL)
    {
        view.Pitch = 0.0f;
        view.PitchTurning = 0;
        view.Option->Stop(SeatOptionKind::MovePitch);
        return;
    }

    if (local == ACTION_JUMP)
    {
        if (Descending(bot) || (view.Probe && view.Probe->JumpDropPending))
            return;

        // The probe's landing while it still describes where the seat stands; measured again once the seat has
        // moved or turned since, so the press never trusts a stale yes. A press with nowhere to land is counted
        // (jumps_refused) rather than silently doing nothing: it is the one way the mask and the press can still
        // disagree, and the number says how often.
        JumpAim aim;
        GroundProbe* probe = view.Probe;
        bool const fresh = probe && probe->Valid && bot->GetExactDist(&probe->From) < 0.5f
            && std::fabs(std::atan2(std::sin(view.Facing - probe->Facing), std::cos(view.Facing - probe->Facing)))
                < 0.05f;
        if (fresh && probe->CanJump)
        {
            aim.Landing = probe->JumpLanding;
            aim.Drop = probe->JumpDrop;
            aim.Ok = true;
        }
        else if (Map* map = bot->GetMap())
            aim = JumpLandingTest(map, map->GetMapCollisionData().GetMMapData().GetNavMeshQuery(), bot,
                view.Facing, view.JumpDropSearch);

        if (!aim.Ok)
        {
            ++result.JumpsRefused;
            return;
        }

        // The feet stop doing whatever they were doing: a jump is the whole move for as long as it lasts, and
        // the clock that says so is what keeps the next decision from pressing a step and cancelling the arc.
        // Over a drop the arc ends at the launch height above the edge and the fall does the rest
        // (BeforeApply, Encoding::FallToGround): a spline aimed thirty yards down would glide there at run
        // speed and never call HandleFall, so the damage would be nothing whatever the height. The clock is a
        // floor for a drop -- computeFallTime for the plain fall, and Slow Fall is slower -- and BeforeApply ends
        // the drop on the spline, not on the clock.
        bool const dropping = aim.Drop > DROP_ABOVE;
        float const landZ = dropping ? bot->GetPositionZ() : aim.Landing.GetPositionZ();
        view.HeldBearing = 0xFF;
        view.Option->Stop(SeatOptionKind::MoveBearing);
        if (probe)
        {
            probe->JumpUntilMs = view.NowMs + JumpFlightMs()
                + (dropping ? uint64(1000.0f * Movement::computeFallTime(aim.Drop, false)) : 0);
            probe->JumpDropPending = dropping;
        }
        Encoding::JumpTo(bot, aim.Landing.GetPositionX(), aim.Landing.GetPositionY(), landZ,
            std::max(1.0f, bot->GetSpeed(MOVE_RUN)), JUMP_SPEED_Z, &view.Facing);
        ++result.Jumps;
        result.JumpDrop = std::max(result.JumpDrop, aim.Drop);
        result.JumpFeatherFall = bot->HasAuraType(SPELL_AURA_FEATHER_FALL) || bot->HasAuraType(SPELL_AURA_HOVER);
    }
}
