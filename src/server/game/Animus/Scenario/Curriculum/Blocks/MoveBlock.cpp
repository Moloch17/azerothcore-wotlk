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
#include "EncoderSupport.h"
#include "Forge.h"
#include "GroundSense.h"
#include "LayeredField.h"
#include "Layout.h"
#include "MapWorldQuery.h"
#include "ProbeBake.h"
#include "SeatEncoder.h"
#include "SeatView.h"
#include "TravelBlock.h"
#include "UnitBody.h"
#include "DetourExtended.h"
#include "DetourNavMeshQuery.h"
#include "Map.h"
#include "MapCollisionData.h"
#include "MapDefines.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace
{
    using Animus::Curriculum::MoveBlock;
    namespace Encoding = Animus::Curriculum::Encoding;
    namespace Ground = Animus::Curriculum::GroundSense;
    namespace LayeredField = Animus::Curriculum::LayeredField;
    namespace MC = Animus::Curriculum::MoveControls;
    namespace Mv = Animus::Movement;
    using Ground::NavRay;

    constexpr float YARD_SCALE = 40.0f;         // distances are reported as a fraction of this
    constexpr float OBJECTIVE_SCALE = 500.0f;   // an objective is further off than anything else it looks at
    constexpr float RUN_SPEED = 7.0f;           // yards a second, unmounted and unhasted (TravelBlock's)

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

    void RefreshLive(Animus::Curriculum::GroundProbe* probe, Map* map, dtNavMeshQuery const* query,
        Animus::Curriculum::GroundSense::Origin const& at, float facing,
        std::chrono::steady_clock::time_point& partMark);

    /// Redo the march if it has stopped describing where the seat is standing, and say whether it is usable.
    ///
    /// Eighty height samples and forty-eight rays against the eight queries the old probe made is too much to
    /// repeat every 250 ms for 128 environments, and it does not need repeating: the ground does not move, only
    /// the seat does. Movement is
    /// the trigger that matters -- at seven yards a second a one-second-old march is seven yards stale and its
    /// nearest cell is six -- with a turn threshold because the grid is egocentric, and a clock as a backstop.
    void RefreshProbe(Animus::Curriculum::SeatView const& view, Player* bot, Position const& self, float facing)
    {
        Animus::Curriculum::GroundProbe* probe = view.Probe;
        if (!probe)
            return;

        Map* map = bot->GetMap();   // non-const: Map::GetLiquidData is not a const member
        if (!map)
            return;

        namespace Bake = Animus::Curriculum::ProbeBake;
        namespace Field = Animus::Curriculum::LayeredField;
        bool stale = !probe->Valid;
        if (probe->Valid)
        {
            float const moved = self.GetExactDist(&probe->From);
            float const turned = std::fabs(std::atan2(std::sin(facing - probe->Facing),
                std::cos(facing - probe->Facing)));
            stale = moved >= MoveBlock::MARCH_REFRESH_YARDS || turned >= MoveBlock::MARCH_REFRESH_RADIANS
                || view.NowMs - probe->Ms >= MoveBlock::MARCH_REFRESH_MS;
            if (moved >= MoveBlock::MARCH_REFRESH_YARDS)
                MoveBlock::StaleMoved.fetch_add(1, std::memory_order_relaxed);
            else if (turned >= MoveBlock::MARCH_REFRESH_RADIANS)
                MoveBlock::StaleTurned.fetch_add(1, std::memory_order_relaxed);
            else if (stale)
                MoveBlock::StaleClock.fetch_add(1, std::memory_order_relaxed);
        }

        // A baked probe is a lookup, so it is read every decision and is never stale; only the march's own
        // bookkeeping below keeps the refresh cadence. A field probe is turned to the facing every decision, and worked out again
        // only where the seat has walked to (below). A live one is measured when it has gone stale.
        bool const baked = Bake::Store::Baked();
        bool const fields = Field::Store::Enabled();
        if (!baked && !fields && !stale)
            return;

        namespace Encoder = Animus::Curriculum::SeatEncoder;
        auto probeMark = std::chrono::steady_clock::now();
        auto partMark = probeMark;

        Ground::Origin const at{ self.GetPositionX(), self.GetPositionY(), self.GetPositionZ(), bot->GetPhaseMask(),
            bot->GetCollisionHeight() };
        dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();

        // Where there is no table the probe is the old live one, on its own cadence: a grid missing from the shipped
        // tables costs what it always did, not the dense probe every decision. It is counted, and logged once.
        bool fromTable = false;
        if (baked)
        {
            // The rays from the nearest cell and the clearance blended over the four around the seat -- what the
            // bake measured best.
            Bake::Reading reading;
            Bake::Reading room;
            std::shared_ptr<Bake::Table const> const table = Bake::Store::Find(map->GetId(), at.X, at.Y);
            fromTable = table
                && Bake::Lookup(*table, at.X, at.Y, at.Z, facing, Bake::Turn::Nearest, false, reading)
                && Bake::Lookup(*table, at.X, at.Y, at.Z, facing, Bake::Turn::Nearest, true, room);
            if (fromTable)
            {
                Bake::Store::Reads.fetch_add(1, std::memory_order_relaxed);
                for (uint32 ray = 0; ray < MoveBlock::RAY_COUNT; ++ray)
                {
                    probe->Reach[ray] = reading.Rays[ray].Reach;
                    probe->Step[ray] = reading.Rays[ray].Step;
                    probe->Shore[ray] = reading.Rays[ray].Shore;
                    probe->Burns[ray] = reading.Rays[ray].Burns;
                }
                probe->Clearance = room.Room.Clearance;
                probe->ClearanceSin = room.Room.Directed ? std::sin(room.Room.Away - facing) : 0.0f;
                probe->ClearanceCos = room.Room.Directed ? std::cos(room.Room.Away - facing) : 0.0f;
                Encoder::ChargeObserve(Encoder::OBSERVE_PROBE_MARCH, partMark);
            }
            else
                Bake::Store::Fallbacks.fetch_add(1, std::memory_order_relaxed);

            if (!stale)
            {
                Encoder::ChargeObserve(Encoder::OBSERVE_PROBE, probeMark);
                return;
            }
        }
        // Worked out from the layered fields where the seat stands, along the compass: the dense probe the tables
        // hold, at the seat's own place. Only walking makes it stale -- the ground does not move, and a turn is the
        // same headings read from another start (RaysFor) -- so a seat turning in place, or standing still, never
        // works it out again. Counted as worked out only when it is.
        bool fromField = false;
        if (!fromTable && fields)
        {
            if (!probe->CompassValid || self.GetExactDist(&probe->CompassFrom) >= MoveBlock::MARCH_REFRESH_YARDS)
            {
                Field::Store::Neighbourhood around;
                Field::Compass compass;
                if (Field::Store::Gather(map->GetId(), at.X, at.Y, around)
                    && Field::SenseCompass(around.View, at.X, at.Y, at.Z, compass))
                {
                    Field::Store::Reads.fetch_add(1, std::memory_order_relaxed);
                    for (uint32 index = 0; index < 2 * Animus::Curriculum::SENSE_RAYS; ++index)
                    {
                        probe->CompassReach[index] = compass.Headings[index].Reach;
                        probe->CompassStep[index] = compass.Headings[index].Step;
                        probe->CompassShore[index] = compass.Headings[index].Shore;
                        probe->CompassBurns[index] = compass.Headings[index].Burns;
                    }
                    probe->CompassClearance = compass.Room.Clearance;
                    probe->CompassDirected = compass.Room.Directed;
                    probe->CompassAway = compass.Room.Away;
                    probe->CompassFrom.Relocate(self);
                    probe->CompassValid = true;
                }
                else
                    Field::Store::Fallbacks.fetch_add(1, std::memory_order_relaxed);
            }
            if (probe->CompassValid)
            {
                Animus::Curriculum::GroundSense::Bearing headings[2 * Animus::Curriculum::SENSE_RAYS];
                for (uint32 index = 0; index < 2 * Animus::Curriculum::SENSE_RAYS; ++index)
                    headings[index] = { probe->CompassReach[index], probe->CompassStep[index],
                        probe->CompassShore[index], probe->CompassBurns[index] };
                Bake::Reading reading;
                Field::RaysFor(headings, facing, reading);
                for (uint32 ray = 0; ray < MoveBlock::RAY_COUNT; ++ray)
                {
                    probe->Reach[ray] = reading.Rays[ray].Reach;
                    probe->Step[ray] = reading.Rays[ray].Step;
                    probe->Shore[ray] = reading.Rays[ray].Shore;
                    probe->Burns[ray] = reading.Rays[ray].Burns;
                }
                probe->Clearance = probe->CompassClearance;
                probe->ClearanceSin = probe->CompassDirected ? std::sin(probe->CompassAway - facing) : 0.0f;
                probe->ClearanceCos = probe->CompassDirected ? std::cos(probe->CompassAway - facing) : 0.0f;
                fromField = true;
            }
            Encoder::ChargeObserve(Encoder::OBSERVE_PROBE_MARCH, partMark);

            // Only the march's bookkeeping keeps the refresh cadence.
            if (!stale)
            {
                Encoder::ChargeObserve(Encoder::OBSERVE_PROBE, probeMark);
                return;
            }
        }
        if (!fromTable && !fromField && !Field::Store::Enabled())
            RefreshLive(probe, map, query, at, facing, partMark);

        probe->From.Relocate(self);
        probe->Facing = facing;
        probe->Ms = view.NowMs;
        probe->Valid = true;
        Encoder::ChargeObserve(Encoder::OBSERVE_PROBE, probeMark);
    }

    /// The probe measured where the seat stands: the five-cell march and the rays along its sixteen headings, and
    /// the room around it.
    void RefreshLive(Animus::Curriculum::GroundProbe* probe, Map* map, dtNavMeshQuery const* query,
        Animus::Curriculum::GroundSense::Origin const& at, float facing,
        std::chrono::steady_clock::time_point& partMark)
    {
        namespace Encoder = Animus::Curriculum::SeatEncoder;
        // The seat's own polygon, once for all sixteen rays.
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
    }

    /// Take a trail sample when one is due, then write the trail into the block's row: each sample as an offset
    /// from where the seat stands now, in its own frame (ahead, left) over YARD_SCALE, oldest first with the newest
    /// in the last pair, then the share of the samples it is still within DWELL_YARDS of. The first observation of
    /// an episode takes the first sample, so the trail always knows where the seat set out from.
    void ObserveTrail(Animus::Curriculum::SeatView const& view, Position const& self, float* out)
    {
        using Animus::Curriculum::MovementTrail;
        using Animus::Curriculum::TRAIL_SAMPLES;

        MovementTrail* trail = view.Trail;
        if (!trail)
            return;

        if (!trail->Started || view.NowMs < trail->LastMs
            || view.NowMs - trail->LastMs >= MovementTrail::INTERVAL_MS)
        {
            trail->X[trail->Next] = self.GetPositionX();
            trail->Y[trail->Next] = self.GetPositionY();
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
            float const dx = trail->X[slot] - self.GetPositionX();
            float const dy = trail->Y[slot] - self.GetPositionY();
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
Animus::Curriculum::BlockSize Animus::Curriculum::MoveBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ OBS_COUNT, MC::ACTION_COUNT };
}

void Animus::Curriculum::MoveBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["controls"] = "player-controller";
    block["rays"] = uint32(RAY_COUNT);
    block["trail_samples"] = uint32(TRAIL_SAMPLES);
    block["trail_interval_ms"] = uint32(MovementTrail::INTERVAL_MS);
    boost::json::array turns;
    for (float rate : MC::TURN_RATES_DEG)
        turns.push_back(double(rate));
    block["turn_rates_deg"] = std::move(turns);
    boost::json::array pitches;
    for (float rate : MC::PITCH_RATES_DEG)
        pitches.push_back(double(rate));
    block["pitch_rates_deg"] = std::move(pitches);
    block["turn_rate_max"] = double(MC::TURN_RATE_MAX);
    block["pitch_rate_max"] = double(MC::PITCH_RATE_MAX);
    block["jump_speed"] = double(Mv::JUMP_SPEED);
    block["swim_jump_speed"] = double(Mv::SWIM_JUMP_SPEED);
    block["step_up"] = double(Mv::STEP_UP);
    block["fall_time_scale_ms"] = double(FALL_TIME_SCALE_MS);
    block["fall_height_scale"] = double(FALL_HEIGHT_SCALE);
    block["probe_yards"] = double(PROBE_YARDS);
    boost::json::array ranges;
    for (float range : MARCH_RANGES)
        ranges.push_back(double(range));
    block["march_ranges"] = std::move(ranges);
    block["march_max"] = double(MARCH_MAX);
    block["clearance_range"] = double(CLEARANCE_RANGE);

    // Which ground probe the model was trained on: the observation's reach, step, shore, burns and clearance are
    // its definition. "baked" (tables) and "geometry" (layered fields) are the same dense probe, taken at 2 yd cells
    // and 16 compass bearings, or worked out at the seat; "live" is the old five-cell one, a different observation.
    // A runtime reading the model with another should know it is fine-tuning territory.
    namespace Bake = Animus::Curriculum::ProbeBake;
    boost::json::object probe;
    probe["source"] = Bake::Store::Baked() ? "baked"
        : Animus::Curriculum::LayeredField::Store::Enabled() ? "geometry" : "live";
    probe["dense"] = Bake::Store::Baked() || Animus::Curriculum::LayeredField::Store::Enabled();
    Bake::Settings const standard = Bake::StandardSettings();
    probe["table_cell"] = double(standard.Cell);
    probe["table_bearings"] = standard.Bearings;
    probe["wedge_rays"] = standard.WedgeRays;
    probe["march_pitch"] = double(standard.Pitch);
    probe["march_window"] = double(GroundSense::MARCH_WINDOW);
    probe["field_cell"] = double(Animus::Curriculum::LayeredField::STANDARD_CELL);
    // The fields are sensed along the compass and turned to the facing to the nearest of these (RaysFor).
    probe["field_compass_headings"] = 2 * Animus::Curriculum::SENSE_RAYS;
    block["ground_probe"] = std::move(probe);
}

std::string Animus::Curriculum::MoveBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    return local < MC::ACTION_COUNT ? MC::NAMES[local] : std::string();
}

void Animus::Curriculum::MoveBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    // `obs` and `mask` are already this block's own slice of the seat's row: SeatEncoder::Observe offsets them
    // before it calls a block. Offsetting again wrote the whole block past the end of its slice, so every bearing
    // stayed masked and no seat could steer (stage1_move, 2026-09-21).
    float* out = obs;
    Player* bot = view.Bot;
    Mv::BodyState const* body = view.Body;
    Mv::ControlState const* held = view.Controls ? &view.Controls->Held : nullptr;
    bool const airborne = Airborne(bot);
    // Where the seat is, for everything it senses of itself: the controller's true body, as a client knows its own
    // position (§5A.1 point 1); the server's (others read it) lags it by up to a report.
    Position const self = body ? Position(body->X, body->Y, body->Z, body->Yaw)
        : bot ? bot->GetPosition() : Position();

    // Everything a seat needs to place its feet, and nothing about whether it has an enemy: this block is the one
    // that still works when there is nothing to fight.
    if (bot)
    {
        // Every bearing in this block is measured off the seat's own heading.
        float const facing = view.Facing;
        bool const keys = held && (held->Forward || held->Strafe || held->Vertical);
        float const speed = body ? std::sqrt(body->Vx * body->Vx + body->Vy * body->Vy + body->Vz * body->Vz) : 0.0f;
        out[OBS_MOVING] = keys || speed > 0.1f || bot->isMoving() ? 1.0f : 0.0f;
        // Over twice the unhasted run speed and clamped: a mounted seat read 2.0 here and a flying one nearly 4,
        // which are not features in [0, 1] and were the widest inputs the row had.
        out[OBS_SPEED] = std::min(1.0f, bot->GetSpeed(MOVE_RUN) / (2.0f * RUN_SPEED));
        out[OBS_FACING_SIN] = std::sin(facing);
        out[OBS_FACING_COS] = std::cos(facing);

        if (held)
        {
            out[OBS_HELD_FORWARD] = float(held->Forward);
            out[OBS_HELD_STRAFE] = float(held->Strafe);
            out[OBS_HELD_VERTICAL] = float(held->Vertical);
            out[OBS_HELD_TURN] = std::clamp(held->TurnRate / MC::TURN_RATE_MAX, -1.0f, 1.0f);
            out[OBS_HELD_PITCH] = std::clamp(held->PitchRate / MC::PITCH_RATE_MAX, -1.0f, 1.0f);
            out[OBS_HELD_WALK] = held->Walk ? 1.0f : 0.0f;
        }

        if (body)
        {
            out[OBS_PITCH_SIN] = std::sin(body->Pitch);
            out[OBS_PITCH_COS] = std::cos(body->Pitch);
            // Into the body's frame: ahead along the yaw, left counter-clockwise of it.
            float const scale = 2.0f * RUN_SPEED;
            float const ahead = body->Vx * std::cos(body->Yaw) + body->Vy * std::sin(body->Yaw);
            float const left = body->Vy * std::cos(body->Yaw) - body->Vx * std::sin(body->Yaw);
            out[OBS_VELOCITY_AHEAD] = std::clamp(ahead / scale, -1.0f, 1.0f);
            out[OBS_VELOCITY_LEFT] = std::clamp(left / scale, -1.0f, 1.0f);
            out[OBS_VELOCITY_UP] = std::clamp(body->Vz / scale, -1.0f, 1.0f);
            out[OBS_PROGRESS] = body->Commanded > 1e-4f ? std::clamp(body->Moved / body->Commanded, 0.0f, 1.0f)
                : 1.0f;
            out[OBS_MODE_FIRST + uint32(body->Kind)] = 1.0f;
            out[OBS_FALL_TIME] = body->Kind == Mv::Mode::Falling
                ? std::min(1.0f, float(body->FallMs) / FALL_TIME_SCALE_MS) : 0.0f;
            out[OBS_FALL_HEIGHT] = body->Kind == Mv::Mode::Falling
                ? std::clamp((body->FallApexZ - body->Z) / FALL_HEIGHT_SCALE, 0.0f, 1.0f) : 0.0f;
            out[OBS_AGAINST_WALL] = body->AgainstWall ? 1.0f : 0.0f;
            out[OBS_STEEP_SLOPE] = body->SteepSlope ? 1.0f : 0.0f;
        }

        // How deep the feet are, the core's own liquid query at the body.
        if (Map* map = bot->GetMap())
        {
            LiquidData const& liquid = map->GetLiquidData(bot->GetPhaseMask(), self.GetPositionX(),
                self.GetPositionY(), self.GetPositionZ(), bot->GetCollisionHeight(), MAP_ALL_LIQUIDS);
            if (liquid.Status != LIQUID_MAP_NO_WATER)
                out[OBS_DEPTH] = std::clamp((liquid.Level - self.GetPositionZ())
                    / std::max(0.1f, bot->GetCollisionHeight()), 0.0f, 1.0f);
        }

        if (Unit const* target = view.Target)
        {
            float const relative = RelativeBearing(self, facing, *target);
            out[OBS_TARGET_BEARING_SIN] = std::sin(relative);
            out[OBS_TARGET_BEARING_COS] = std::cos(relative);
            out[OBS_TARGET_DISTANCE] = std::min(1.0f, self.GetExactDist2d(target) / YARD_SCALE);
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
            float const relative = RelativeBearing(self, facing, view.Objective);
            out[OBS_OBJECTIVE] = 1.0f;
            out[OBS_OBJECTIVE_BEARING_SIN] = std::sin(relative);
            out[OBS_OBJECTIVE_BEARING_COS] = std::cos(relative);
            float const range = self.GetExactDist2d(&view.Objective);
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
            RefreshProbe(view, bot, self, facing);
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
            }
        }
        else
        {
            // Off the ground there is nothing underfoot to walk onto or refuse: every way is open, the ground
            // changes by nothing, and a seat that is swimming is surrounded by the water it is in. The march
            // is dropped rather than kept, so the first one made after coming ashore is a fresh one -- and the
            // clearance it held goes with it, because the travel encounter's clearance charge reads the probe
            // too, and was charging a swimmer for the bank it stood beside before it got in.
            // In the air the rays sense what a flyer steers against: how far it can fly level along each bearing
            // before something solid (a collision ray at the seat's altitude, and the terrain rising above it), in
            // the reach's slot, and in the step's whether climbing FLIGHT_CLIMB higher opens the way -- positive
            // where it does, negative where it closes it. OBS_AIRBORNE tells the policy which meaning it is
            // reading. With the layered fields on it is read from them every decision, as the ground probe is;
            // with Probe.Source = live it is measured live (~1.3 us a ray). A seat over a grid with no field sees
            // open air. A swimmer's rays stay open, as they were.
            bool const flying = !bot->IsInWater();
            Map* map = bot->GetMap();
            float const z = self.GetPositionZ() + bot->GetCollisionHeight() * 0.5f;
            bool const fields = LayeredField::Store::Enabled();
            LayeredField::Store::Neighbourhood around;
            bool const fromField = flying && map && fields
                && LayeredField::Store::Gather(map->GetId(), self.GetPositionX(), self.GetPositionY(), around);
            if (flying && map && fields)
                (fromField ? LayeredField::Store::Reads : LayeredField::Store::Fallbacks)
                    .fetch_add(1, std::memory_order_relaxed);
            for (uint32 ray = 0; ray < RAY_COUNT; ++ray)
            {
                float reach = 1.0f;
                float climb = 0.0f;
                if (fromField)
                {
                    float const heading = RayHeading(facing, ray);
                    float const level = LayeredField::FlightReach(around.View, self.GetPositionX(),
                        self.GetPositionY(), z, heading, MARCH_MAX, FLIGHT_PITCH);
                    float const above = LayeredField::FlightReach(around.View, self.GetPositionX(),
                        self.GetPositionY(), z + FLIGHT_CLIMB, heading, MARCH_MAX, FLIGHT_PITCH);
                    reach = level / MARCH_MAX;
                    climb = std::clamp((above - level) / MARCH_MAX, -1.0f, 1.0f);
                }
                else if (flying && map && !fields)
                {
                    float const heading = RayHeading(facing, ray);
                    float const level = LayeredField::LiveFlightReach(map, self.GetPositionX(), self.GetPositionY(),
                        z, heading, MARCH_MAX, FLIGHT_PITCH);
                    float const above = LayeredField::LiveFlightReach(map, self.GetPositionX(), self.GetPositionY(),
                        z + FLIGHT_CLIMB, heading, MARCH_MAX, FLIGHT_PITCH);
                    reach = level / MARCH_MAX;
                    climb = std::clamp((above - level) / MARCH_MAX, -1.0f, 1.0f);
                }
                out[OBS_GROUND_FIRST + ray] = reach;
                out[OBS_STEP_FIRST + ray] = climb;
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

        out[OBS_IN_WATER] = bot->IsInWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED] = bot->IsUnderWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED_TIME] = std::min(1.0f, view.BreathSpent);
        out[OBS_SWIM_SPEED] = bot->GetSpeed(MOVE_SWIM) / RUN_SPEED;
        out[OBS_AIRBORNE] = airborne ? 1.0f : 0.0f;
    }

    // The way round against the way through, and whether the legs are getting anywhere. All three are the
    // scenario's to measure -- one at the episode's build, two over the last second -- because none of them can
    // be seen from a probe of any length.
    out[OBS_DETOUR] = std::clamp(view.Detour / 4.0f, 0.0f, 1.0f);
    out[OBS_MOVE_RATE] = std::clamp(view.MoveRate, 0.0f, 1.0f);
    out[OBS_CLOSE_RATE] = std::clamp(view.CloseRate, -1.0f, 1.0f);

    // Where it has been, in its own frame, sampled here once a second because this is the one place that runs
    // for every seat every decision, in training and in play alike.
    if (bot)
        ObserveTrail(view, self, out);

    // The masks: only the presses that are physically impossible (MoveControls::Allowed). The jump and the vertical
    // controls read the body the controller moves.
    MC::MaskState state;
    state.Alive = bot && bot->IsAlive();
    if (bot && body)
    {
        Mv::Speeds const speeds = Mv::SpeedsOf(bot);
        state.CanSteerVertically = Mv::CanSteerVertically(*body, speeds);
        if (Map* map = bot->GetMap())
        {
            Mv::MapWorldQuery const world(map, bot->GetPhaseMask());
            state.CanJump = Mv::CanJump(*body, Mv::ShapeOf(bot), world);
        }
    }
    out[OBS_CAN_JUMP] = state.Alive && state.CanJump ? 1.0f : 0.0f;

    if (!mask)
        return;
    for (uint32 action = 0; action < MC::ACTION_COUNT; ++action)
        mask[action] = MC::Allowed(action, state) ? 1 : 0;
}

void Animus::Curriculum::MoveBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    Player* bot = view.Bot;
    if (!bot || !view.Controls || local >= MC::ACTION_COUNT)
        return;

    // One held control changed (MoveControls::Press), priced by what it takes back of a recent choice. The value
    // already held, pressed again, is the key kept down: no press, no charge (SeatActionResult::KeyStillHeld).
    MC::PressOutcome const pressed = MC::Press(*view.Controls, local, view.NowMs, view.Options.JitterDecayMs);
    if (!pressed.Changed)
    {
        result.KeyStillHeld = true;
        return;
    }
    result.ControlChanged = true;
    result.JitterWeight += pressed.JitterWeight;
    result.BearingFlip += pressed.FeetFlip;
    result.TurnReversals += pressed.TurnReversals;
    result.PitchReversals += pressed.PitchReversals;
    result.Weaves += pressed.Weaves;
    result.EffortWeight = pressed.Effort;
}
