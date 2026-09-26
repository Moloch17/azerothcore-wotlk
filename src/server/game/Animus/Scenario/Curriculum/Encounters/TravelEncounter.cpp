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

#include "Encounters.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Map.h"
#include "MapDefines.h"
#include "MapCollisionData.h"
#include "DetourNavMeshQuery.h"
#include "DetourExtended.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "MoveBlock.h"
#include "TravelBlock.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    /// How many places are thrown at the ground before an episode gives up on finding one.
    ///
    /// Raised from 32 with the feasibility cap and the broken arena's real terrain: both reject more draws, and a
    /// draw that fails costs only the throw, because the loop stops at the first place that passes. Easy ground
    /// still succeeds on the first or second attempt and pays nothing for the higher ceiling; rough ground gets
    /// the tries it needs rather than falling through to the spawn-point retry, which moves the seats.
    constexpr uint32 OBJECTIVE_ATTEMPTS = 48;
    constexpr float MAX_PATH_DETOUR = 1.8f;         // a path at most this many times the straight distance
    // A water arena wants the detour the others refuse: the way round has to be far enough longer than the way
    // through that swimming is a real choice. Swimming is about 4.7 yd/s against 7 running, so the crossing pays
    // at roughly 1.5x and this leaves a margin on either side of that -- some of these trips are worth swimming
    // and some are not, which is what makes it a decision rather than a reflex.
    constexpr float MIN_DETOUR_ACROSS = 1.35f;
    /// How much of an episode's clock the walk to the objective may need, at the character's own run speed.
    ///
    /// A trip that fills the clock is winnable only by a seat that already walks it perfectly, and the standard
    /// is that the bot always arrives -- so the generator has to stop setting trips that a policy still learning
    /// to steer cannot finish. At the common case this rejects nothing: 160 yards (FootMax) at 7 yards a second
    /// is 23 seconds against a 120 second clock, a share of 0.19. What it cuts is the tail -- a long trip behind
    /// a 1.8x detour for a character with no speed to spare.
    constexpr float FEASIBLE_SHARE = 0.45f;
    constexpr uint32 WATER_SAMPLES = 12;            // points along the straight line, looking for water
    /// Ledge trips: the straight line is walked a yard at a time to find the edge, and a seat that has dropped
    /// off one is this many yards above or below the corner it was walking towards, which the way's own
    /// two-dimensional Advance cannot see -- so the way is planned again from the foot at once.
    constexpr float LEDGE_SAMPLE = 1.0f;
    constexpr float LEDGE_LANDING_REACH = 8.0f;    // how far past the edge the landing may be: a jump's carry
    constexpr float LEDGE_STRAY_Z = 6.0f;
    constexpr float HEIGHT_SEARCH = 120.0f;
    /// What an interior arena probes with instead: a step up from the seat's feet, searching down far enough to
    /// find a cellar stair but never far enough up to find the storey above.
    constexpr float INDOOR_RISE = 2.5f;
    constexpr float INDOOR_SEARCH = 12.0f;
    /// WMO group flag 0x8: this part of the building is open to the sky. Map::GetFullTerrainStatusForPosition
    /// reads the same bit to decide whether a unit is outdoors.
    /// The WMO group's own "this group is outdoors" bit.
    ///
    /// Knowingly the weaker of the two tests the core itself uses. GetFullTerrainStatusForPosition also consults
    /// the WMO area table's flags -- bit 2 forces indoors, bit 4 forces outdoors -- and either can overrule the
    /// group bit. An inn whose area table says indoors while its group flag does not would be turned down here.
    /// That costs a spawn point, never a wrong one, so it stays the test: this code is choosing where to put an
    /// objective, and refusing a real room is cheap while accepting a hillside is not.
    constexpr uint32 WMO_GROUP_OUTDOORS = 0x8;
    constexpr float BODY_HEIGHT = 2.0f;             // for the water check
    /// A stall, for stall_seconds and stalls: the route distance has not fallen by STALL_GAIN_YARDS for
    /// STALL_MIN_MS. Two yards is more than the wobble of walking a corner and less than a decision's travel at
    /// run speed; three seconds outlasts a mount cast and a jump, which gain nothing and are not stalls.
    constexpr float STALL_GAIN_YARDS = 2.0f;
    constexpr uint32 STALL_MIN_MS = 3000;
}

Animus::Curriculum::TravelEncounter::TravelEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::TravelEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::Progress, RewardTerm::Arrive, RewardTerm::DamageTaken,
        RewardTerm::Death, RewardTerm::Clearance };
}

void Animus::Curriculum::TravelEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // On a chain (ArenaDefinition::Checkpoints) arriving means the first objective was reached: the episode goes on
    // to the clock, and `checkpoints` is how far it got.
    table.Add("arrived", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.Arrived || travel.Checkpoints > 0 ? 1.0f : 0.0f;
    });
    table.Add("checkpoints", [this](Env const& env, uint32) { return float(_envs[env.Index].Checkpoints); });
    table.Add("chain_broken", [this](Env const& env, uint32) { return _envs[env.Index].ChainBroken ? 1.0f : 0.0f; });
    table.Add("travel_seconds", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return float(travel.Arrived ? travel.ArriveMs : env.EpisodeElapsedMs) / 1000.0f;
    });
    table.Add("start_distance", [this](Env const& env, uint32) { return _envs[env.Index].StartDistance; });
    // How far short the seat finished, in yards. `distance_at_end` is the distance to the *target*, and a
    // movement stage has none by design, so it reads 0 for every episode of the four stages that are only about
    // getting somewhere -- which left no way to tell a seat wedged against geometry forty yards out from one
    // orbiting the objective at eight, never inside the six ARRIVE_YARDS wants. Those are different bugs with
    // different fixes, and this is the column that separates them. 0 on an episode that arrived.
    // What the legs did, against WalkDistance's what-they-were-asked-for. A timeout with distance_travelled near
    // zero is a seat that never moved; one with distance_travelled far above walk_distance is a seat that moved
    // plenty and in the wrong directions. Stuck and lost want opposite fixes.
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });
    // What the trip costs on dry land against what it costs straight, and whether there is a dry way at all.
    //
    // FindPlace calls a destination reachable when PathGenerator says PATHFIND_NORMAL, and a player's filter is
    // NAV_GROUND | NAV_WATER | NAV_MAGMA -- so "reachable on foot" has always included swimming. An open or
    // broken arena can therefore place an objective whose only route crosses a river: both ends dry, the water
    // in the middle, and nothing naming it. That would look exactly like the failures these stages have -- a
    // seat pacing a bank, covering seven hundred yards, finishing forty short, on the two arenas where the water
    // is incidental and never on the one where it is the lesson. These two columns are what tell that apart:
    // dry_distance 0 means no dry route exists, and a detour far above 1 means the dry way is much the longer.
    table.Add("dry_distance", [this](Env const& env, uint32) { return _envs[env.Index].DryDistance; });
    table.Add("dry_detour", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.StartDistance > 0.0f && travel.DryDistance > 0.0f
            ? travel.DryDistance / travel.StartDistance : 0.0f;
    });
    table.Add("objective_distance_at_end", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        if (!travel.HasObjective || travel.Arrived)
            return 0.0f;

        Player* bot = _scenario.SeatBot(env, 0);
        return bot ? bot->GetExactDist2d(&travel.Objective) : 0.0f;
    });
    // Whether the episode was built as a water crossing at all, and how long the seat spent in the water. The
    // first says what the arena offered, the second what the seat did with it -- and a water arena where
    // swim_seconds stays at zero is either a policy that always goes round or spawn points with no water in
    // reach, which the two columns together tell apart.
    // Reported raw, for arrivals as well as failures, and deliberately without the sentinel that
    // objective_distance_at_end uses: these are read per episode rather than as a mean, and the whole question
    // is the shape of the distribution among the episodes that did not make it.
    table.Add("objective_distance_nearest", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.HasObjective && travel.Nearest >= 0.0f ? travel.Nearest : 0.0f;
    });
    table.Add("nearest_at_seconds", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].NearestMs) / 1000.0f;
    });
    // Coordinates, in a table of measurements, for one reason: to find out whether the episodes that fail all
    // fail in the same places. A stall scattered across the world is a rule that is wrong everywhere; a stall
    // that repeats at a handful of points is a handful of bad places, and the two want completely different
    // fixes. These are what `forge rays` is then pointed at.
    table.Add("nearest_x", [this](Env const& env, uint32) { return _envs[env.Index].NearestX; });
    table.Add("nearest_y", [this](Env const& env, uint32) { return _envs[env.Index].NearestY; });
    table.Add("objective_x", [this](Env const& env, uint32) { return _envs[env.Index].Objective.GetPositionX(); });
    table.Add("objective_y", [this](Env const& env, uint32) { return _envs[env.Index].Objective.GetPositionY(); });
    // How often the trip was measured against a straight line instead of a route. A number above zero here
    // means some share of every arrival rate ever reported was judged on a trip that was never checked.
    // The way itself: how long it was, whether it arrived, and whether one could be found at all. Without the
    // last of these a routing regression is indistinguishable from a policy that got worse.
    table.Add("route_length", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Way.Valid ? _envs[env.Index].Way.Length : 0.0f;
    });
    table.Add("route_complete", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Way.Valid && _envs[env.Index].Way.Complete ? 1.0f : 0.0f;
    });
    table.Add("route_failed", [this](Env const& env, uint32)
    {
        return _envs[env.Index].WayFailed ? 1.0f : 0.0f;
    });
    table.Add("route_shortcut", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Shortcut ? 1.0f : 0.0f;
    });
    table.Add("dry_shortcut", [this](Env const& env, uint32)
    {
        return _envs[env.Index].DryShortcut ? 1.0f : 0.0f;
    });
    table.Add("crossing", [this](Env const& env, uint32) { return _envs[env.Index].Crossing ? 1.0f : 0.0f; });
    table.Add("walk_distance", [this](Env const& env, uint32) { return _envs[env.Index].WalkDistance; });
    // Flying against not, split per episode: an update's mean mixes a handful of flights into a hundred rides, so
    // divide each conditional sum by `flew` (or 1 - flew) to read what a trip of that kind actually cost.
    table.Add("flew", [this](Env const& env, uint32) { return _envs[env.Index].Flew ? 1.0f : 0.0f; });
    table.Add("saved_if_flew", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Flew ? Saved(_envs[env.Index]) : 0.0f;
    });
    table.Add("saved_if_ground", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Flew ? 0.0f : Saved(_envs[env.Index]);
    });
    // Does a flying mount actually fly at flight speed, and how high does the seat take it?
    table.Add("flight_speed", [this](Env const& env, uint32) { return _envs[env.Index].FlightSpeedSeen; });
    table.Add("flight_yps", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.FlightMs ? float(travel.FlightDistance / (double(travel.FlightMs) / 1000.0)) : 0.0f;
    });
    table.Add("flight_yps_peak", [this](Env const& env, uint32)
    {
        return _envs[env.Index].FlightPeakYps;
    });
    // Is MOVEMENTFLAG_FLYING actually set while the seat is in the air? 1 = always, 0 = never.
    table.Add("flying_flag_share", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.AloftSteps ? float(travel.AloftFlagged) / float(travel.AloftSteps) : 0.0f;
    });
    // How much of the clock the trip needed at the seat's own speed. The cap in FindPlace is a ceiling on this;
    // the column is how the distribution under that ceiling stays visible.
    table.Add("trip_share", [this](Env const& env, uint32) { return _envs[env.Index].TripShare; });
    table.Add("flight_height", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.HeightSamples ? float(travel.HeightSum / travel.HeightSamples) : 0.0f;
    });
    // Why a flying arena does or does not fly, in three steps: does the seat know a flying mount, would the mask
    // have offered it at the start, and did the seat ride one (in the air or not, unlike flying_fraction).
    table.Add("knows_flying_mount", [this](Env const& env, uint32)
    {
        return _envs[env.Index].KnowsFlyer ? 1.0f : 0.0f;
    });
    table.Add("could_mount_flying", [this](Env const& env, uint32)
    {
        return _envs[env.Index].CouldMountFlyer ? 1.0f : 0.0f;
    });
    table.Add("flying_zone", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].FlyerZone);
    });
    table.Add("flying_area", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].FlyerArea);
    });
    table.Add("flying_refusal", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].FlyerRefusal);
    });
    table.Add("flying_mount_fraction", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].FlyingMountMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    // What the trip beat the walk by: 0 walked it, 0.3 arrived in 70% of the time walking would have taken.
    table.Add("saved", [this](Env const& env, uint32) { return Saved(_envs[env.Index]); });
    table.Add("mounted_fraction", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].MountedMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    table.Add("flying_fraction", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].FlyingMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    // Where the trip stopped gaining: the longest stretch without closing on the objective, in seconds, and how
    // many stretches of STALL_MIN_MS or more there were. `lost` and `wedged` (evaluation.py) say how far a failure
    // wandered; this says how long it spent getting nowhere, arrivals included.
    table.Add("stall_seconds", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].StallLongestMs) / 1000.0f;
    });
    table.Add("stalls", [this](Env const& env, uint32) { return float(_envs[env.Index].Stalls); });
    // What the trip was asked to be: the detour band it was drawn for (-1 where none was), and whether it can
    // only be reached by air.
    table.Add("detour_band", [this](Env const& env, uint32) { return float(_envs[env.Index].Band); });
    table.Add("air_only", [this](Env const& env, uint32) { return _envs[env.Index].AirOnly ? 1.0f : 0.0f; });
    // And whether the objective was placed below a ledge on the straight line, with the height of that edge: the
    // jump is the shortcut, the ramp the way round. What was achieved, like `crossing`.
    table.Add("ledge", [this](Env const& env, uint32) { return _envs[env.Index].Ledge ? 1.0f : 0.0f; });
    table.Add("ledge_drop", [this](Env const& env, uint32) { return _envs[env.Index].LedgeDrop; });
    // And whether it was placed on a lakebed, under how much water: the dive is the trip, and whether the seat
    // came up for air on the way is in the scenario's breath columns.
    table.Add("dive", [this](Env const& env, uint32) { return _envs[env.Index].Dive ? 1.0f : 0.0f; });
    table.Add("dive_depth", [this](Env const& env, uint32) { return _envs[env.Index].DiveDepth; });
}

float Animus::Curriculum::TravelEncounter::Saved(EnvTravel const& travel)
{
    if (!travel.Arrived || travel.WalkDistance <= 0.0f)
        return 0.0f;

    float const walkSeconds = travel.WalkDistance / TravelBlock::BASE_RUN_SPEED;
    float const tripSeconds = float(travel.ArriveMs) / 1000.0f;
    return std::clamp((walkSeconds - tripSeconds) / walkSeconds, 0.0f, 1.0f);
}

void Animus::Curriculum::TravelEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvTravel();
}

bool Animus::Curriculum::TravelEncounter::FindPlace(Player* bot, Map* map, float nearest, float furthest, bool flying,
    Position& place, float budgetSeconds, float* walk, bool across, float* dry, bool indoors, bool* shortcut,
    TravelPlaceRules const& rules,
    float* ledgeDrop, float* diveDepth)
{
    // What the seat can actually cover in the time it has. Reachability was the only test until now -- a path of
    // type PATHFIND_NORMAL, no longer than MAX_PATH_DETOUR times the straight line -- and reachable is not the
    // same claim as reachable before the clock runs out at this character's speed. The gap between the two is
    // where an unwinnable episode comes from, and an unwinnable episode is the one thing a gate at 1.0 cannot
    // survive: it would halt the whole queue over a trip no policy could have made.
    float const speed = std::max(1.0f, bot->GetSpeed(flying ? MOVE_FLIGHT : MOVE_RUN));
    float const affordable = budgetSeconds > 0.0f ? budgetSeconds * speed : std::numeric_limits<float>::max();

    // A crossing is a much narrower thing to ask for than a trip -- it wants water on the straight line and a dry
    // way round at least MIN_DETOUR_ACROSS longer -- so it gets more tries before it gives up and the arena falls
    // back to an ordinary trip. At 32 it found one in 0.65 of its episodes; the ones it missed were not bad ground
    // but too few throws at it. An air-only place is as narrow: a plateau or an island, not any dry ground.
    uint32 const attempts = across || rules.AirOnly || rules.Ledge || rules.Underwater
        ? OBJECTIVE_ATTEMPTS * 4 : OBJECTIVE_ATTEMPTS;
    for (uint32 attempt = 0; attempt < attempts; ++attempt)
    {
        // Later attempts settle for shorter trips rather than failing the episode.
        float const reach = attempt < attempts / 2 ? furthest : (nearest + furthest) * 0.5f;
        float const distance = frand(nearest, std::max(nearest, reach));
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const x = bot->GetPositionX() + distance * std::cos(angle);
        float const y = bot->GetPositionY() + distance * std::sin(angle);

        map->LoadGrid(x, y);
        // Outdoors, look from well above the seat and search a long way down: ground forty yards up is still
        // ground, and the broken arena's ridges span seventy yards of relief. Inside a building the same probe
        // returns the roof, because Map::GetHeight casts a strictly downward ray from the z it is given -- so an
        // interior arena looks from a step above its own feet instead, and finds the floor it is standing on.
        // An air-only place may sit on a plateau or an island far above the seat, so that probe starts from as
        // high as the seat may fly and searches the same relief downwards.
        // A ledge place is below the seat, never above: the probe starts a step over its feet and searches the
        // deepest drop the arena offers.
        float const from = indoors ? bot->GetPositionZ() + INDOOR_RISE
            : rules.AirOnly ? bot->GetPositionZ() + TravelBlock::MAX_ALTITUDE
            : rules.Ledge ? bot->GetPositionZ() + MoveBlock::MAX_STEP
            : bot->GetPositionZ() + HEIGHT_SEARCH * 0.5f;
        float const search = indoors ? INDOOR_SEARCH
            : rules.AirOnly ? TravelBlock::MAX_ALTITUDE + HEIGHT_SEARCH * 0.5f
            : rules.Ledge ? rules.DropMax + 2.0f * MoveBlock::MAX_STEP : HEIGHT_SEARCH;
        float const z = map->GetHeight(bot->GetPhaseMask(), x, y, from, true, search);
        if (z <= INVALID_HEIGHT)
            continue;
        if (rules.Ledge && (bot->GetPositionZ() - z < rules.DropMin || bot->GetPositionZ() - z > rules.DropMax))
            continue;

        // A place inside has to actually be inside. The probe can still land in a courtyard or on a roof edge
        // through a doorway, and only the WMO data tells them apart: GetAreaInfo returns false where no building
        // was hit at all, and mogpFlags bit 0x8 is the group's own "this part is outdoors". Then the core's own
        // reachability test, which is a Detour raycast plus both collision trees, corrects the point or rejects
        // it -- inside a building that is the difference between the floor and the inside of a table.
        float placeX = x;
        float placeY = y;
        float placeZ = z;
        if (indoors)
        {
            uint32 mogpFlags = 0;
            int32 adtId = 0;
            int32 rootId = 0;
            int32 groupId = 0;
            if (!map->GetAreaInfo(bot->GetPhaseMask(), x, y, z, mogpFlags, adtId, rootId, groupId))
                continue;
            if ((mogpFlags & WMO_GROUP_OUTDOORS) != 0)
                continue;
            if (!map->CanReachPositionAndGetValidCoords(bot, placeX, placeY, placeZ, true, true))
                continue;
        }

        // The objective itself always stands on dry land -- arriving is standing somewhere, not treading water --
        // except on a dive, where it stands on the lakebed under the water the arena asked for: Map::GetHeight is
        // blind to liquid and returned the bed, and what is over it is the liquid data's to say.
        float depth = 0.0f;
        if (rules.Underwater)
        {
            LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), placeX, placeY, placeZ,
                bot->GetCollisionHeight(), {});
            if (liquid.Status == LIQUID_MAP_NO_WATER || liquid.Level <= INVALID_HEIGHT
                || (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) == 0)
                continue;
            depth = liquid.Level - placeZ;
            if (depth < rules.DepthMin || depth > rules.DepthMax)
                continue;
        }
        else if (map->IsInWater(bot->GetPhaseMask(), x, y, z, BODY_HEIGHT))
            continue;

        // On the ground it has to be reachable on foot, by a path not much longer than the straight line.
        float walked = distance;
        float dryWalk = 0.0f;
        float edgeDrop = 0.0f;
        if (rules.Underwater)
        {
            // A dive is swum straight: no ground route is asked for, and the clock is measured at swimming speed
            // (about two thirds of running) over the straight line plus the way down.
            float const swim = std::max(1.0f, bot->GetSpeed(MOVE_SWIM));
            walked = (distance + depth) * (speed / swim);
        }
        else if (rules.Ledge)
        {
            // Below a ledge: the way round on foot has to exist -- a class without Slow Fall must still be able
            // to arrive, or the floor of 1.0 is unwinnable -- and be far enough longer than the straight line that
            // the drop is the shortcut, and the straight line has to cross an edge the seat can actually drop
            // off. RoutePlanner for the way round, as the air-only branch, with the grids along the line loaded
            // first.
            for (uint32 step = 1; step < WATER_SAMPLES; ++step)
            {
                float const along = float(step) / float(WATER_SAMPLES);
                map->LoadGrid(bot->GetPositionX() + (x - bot->GetPositionX()) * along,
                    bot->GetPositionY() + (y - bot->GetPositionY()) * along);
            }

            Route ground;
            Position const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 0.0f);
            Position const to(placeX, placeY, placeZ, 0.0f);
            if (!RoutePlanner::Instance().Plan(map, from, to, ground) || !ground.Complete
                || ground.Length < distance * rules.LedgeDetour)
                continue;
            if (!LedgeOnLine(bot, map, placeX, placeY, placeZ, rules, edgeDrop))
                continue;

            walked = ground.Length;
        }
        else if (!flying)
        {
            PathGenerator path(bot);
            if (!path.CalculatePath(x, y, z) || !(path.GetPathType() & PATHFIND_NORMAL))
                continue;

            // PATHFIND_NORMAL on its own is not a route. A missing tile, a hole in the mesh, or a start too far
            // from it all make PathGenerator fall back to BuildShortcut and answer
            // PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH -- two points in a straight line, whose length is the
            // distance as the crow flies. Every check below then measures a trip that was never routed: the
            // detour is exactly 1.0 by construction and the feasibility budget is meaningless.
            //
            // It was 11.08% of stage1_move's evaluation episodes, and they arrived 0.4934 against 0.8567 for
            // the episodes that had a real route -- 27.94% of the `open` arena, 0.71% of `broken`, none of
            // `water`. So better than a tenth of every arrival rate this stage has ever reported was measured on
            // trips that were never checked, which is most of the distance between the gate and 1.0.
            //
            // Rejected now. There is a retry above this: a spawn point no objective can be found from is
            // abandoned for another (StageScenario's SPAWN_ATTEMPTS), so refusing here costs an attempt rather
            // than an episode, and a spawn point that is off the mesh entirely simply stops being used.
            if (path.GetPathType() & PATHFIND_NOT_USING_PATH)
                continue;

            walked = path.getPathLength();

            // A water arena wants the opposite of what every other one wants. Elsewhere a long detour means the
            // straight line was a lie and the objective is rejected; here it is the whole point -- the way round
            // is longer than the way through, and whether the difference is worth swimming for is the lesson. The
            // path is what a runner would cover, so the ratio is the choice the seat is being asked to make.
            if (across)
            {
                // Water is worth getting into only when there is a dry way round and swimming beats it, so the
                // episode has to offer both and know how long each is. The path above is no use for the dry one:
                // PathGenerator::CreateFilter hands a player NAV_GROUND | NAV_WATER | NAV_MAGMA whatever the
                // ground, so the "walk" is allowed to swim and comes back the same length as the straight line
                // (measured over four bodies of water: the longest way round found was 1.15x the way through,
                // even where every straight line crossed water). NAV_GROUND alone is the way round on foot.
                if (!CrossesWater(bot, map, place, x, y))
                    continue;

                PathGenerator dry(bot);
                dry.SetIncludeFlags(NAV_GROUND);
                if (!dry.CalculatePath(x, y, z) || !(dry.GetPathType() & PATHFIND_NORMAL)
                    || (dry.GetPathType() & PATHFIND_NOT_USING_PATH))
                    continue;                      // no dry route: getting in is not a choice, it is the only way

                dryWalk = dry.getPathLength();

                // Measured at the Dustwallow banks: a dry way round of 145 yards against a 75 yard swim
                // (1.94x: running 7 yd/s against swimming 4.7, the swim pays above 1.49x) next to candidates at
                // 1.02x where walking plainly wins.
                // The floor keeps trips of both kinds, which is what makes the crossing a decision.
                if (dryWalk < distance * MIN_DETOUR_ACROSS)
                    continue;                      // the way round is barely longer: nothing to decide
            }
            else
            {
                if (walked > distance * MAX_PATH_DETOUR)
                    continue;

                // The band this episode asked for (TravelPlaceRules::Band), insisted on for the first half of the
                // attempts and let go after, so an arena whose ground has no long way round still builds. The
                // ratio is the walking path over the straight line, which is what the seat is asked to cover.
                if (rules.Band >= 0 && attempt < attempts / 2)
                {
                    float const detour = walked / std::max(1.0f, distance);
                    int32 const band = detour < rules.DetourEasy ? 0 : detour < rules.DetourHard ? 1 : 2;
                    if (band != rules.Band)
                        continue;
                }
            }
        }
        else if (rules.AirOnly)
        {
            // Reachable by air only: no complete ground route, or one so much longer than the straight line that
            // the wings are the way. RoutePlanner rather than PathGenerator, whose point cap answers "incomplete"
            // for any ground trip of this length whether the ground allows it or not -- and with the grids along
            // the straight line loaded first, because the planner can only walk mesh that is in memory, and a
            // route that stopped at a tile boundary would read as no route.
            for (uint32 step = 1; step < WATER_SAMPLES; ++step)
            {
                float const along = float(step) / float(WATER_SAMPLES);
                map->LoadGrid(bot->GetPositionX() + (x - bot->GetPositionX()) * along,
                    bot->GetPositionY() + (y - bot->GetPositionY()) * along);
            }

            Route ground;
            Position const from(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 0.0f);
            Position const to(placeX, placeY, placeZ, 0.0f);
            if (RoutePlanner::Instance().Plan(map, from, to, ground) && ground.Complete
                && ground.Length <= distance * rules.AirDetour)
                continue;                      // walkable: a ride would do, and the wings would teach nothing
        }

        // Far enough inside the clock to be winnable by a seat that is still learning to steer, rather than only
        // by one that walks the path perfectly. FEASIBLE_SHARE is what "far enough" means, and the trip's actual
        // share of the clock is reported per episode so the margin can be read rather than trusted.
        if (walked > affordable)
            continue;

        place.Relocate(placeX, placeY, placeZ);
        if (walk)
            *walk = walked;
        if (dry)
            *dry = dryWalk;
        if (ledgeDrop)
            *ledgeDrop = edgeDrop;
        if (diveDepth)
            *diveDepth = depth;
        if (shortcut)
            *shortcut = false;      // nothing that got here was one; the column stays as the regression alarm
        return true;
    }

    return false;
}

bool Animus::Curriculum::TravelEncounter::CrossesWater(Player const* bot, Map* map, Position const& /*place*/,
    float x, float y)
{
    // Sample the straight line: a detour long enough to be interesting could be a cliff or a canyon as easily as a
    // lake, and only one of those can be swum. Cheap, and only ever asked while an episode is being built.
    float const fromX = bot->GetPositionX();
    float const fromY = bot->GetPositionY();
    float const fromZ = bot->GetPositionZ();
    for (uint32 step = 1; step < WATER_SAMPLES; ++step)
    {
        float const along = float(step) / float(WATER_SAMPLES);
        float const sampleX = fromX + (x - fromX) * along;
        float const sampleY = fromY + (y - fromY) * along;
        map->LoadGrid(sampleX, sampleY);
        float const sampleZ = map->GetHeight(bot->GetPhaseMask(), sampleX, sampleY, fromZ + HEIGHT_SEARCH * 0.5f,
            true, HEIGHT_SEARCH);
        if (sampleZ > INVALID_HEIGHT && map->IsInWater(bot->GetPhaseMask(), sampleX, sampleY, sampleZ, BODY_HEIGHT))
            return true;
    }

    return false;
}

bool Animus::Curriculum::TravelEncounter::LedgeOnLine(Player const* bot, Map* map, float x, float y, float z,
    TravelPlaceRules const& rules, float& drop)
{
    // Walk the straight line a yard at a time. The approach is every sample the seat could walk to: ground within
    // a step and a slope of the last, and on the navmesh. The edge is where that stops -- which is the mesh's
    // word, not a height march's, because a slope too steep to walk (the southern Barrens escarpment) drops a
    // yard a yard and no height test calls that a cliff, while the mesh has already refused it. The landing is
    // the deep ground under the samples just past the edge, as far as a jump carries: it has to be there, more
    // than a step but no more than DropMax below the edge, on the mesh, and the way on from it to the place has
    // to be an ordinary walk (RoutePlanner, complete, at most MAX_PATH_DETOUR).
    drop = 0.0f;
    dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    if (!query)
        return false;

    auto const onMesh = [query](float px, float py, float pz)
    {
        dtQueryFilterExt filter;
        filter.setIncludeFlags(NAV_GROUND);
        filter.setExcludeFlags(0);
        float const at[3] = { py, pz, px };
        float const extents[3] = { 1.5f, MoveBlock::MAX_STEP, 1.5f };
        float nearest[3] = { 0.0f, 0.0f, 0.0f };
        dtPolyRef ref = 0;
        return !dtStatusFailed(query->findNearestPoly(at, extents, &filter, &ref, nearest)) && ref
            && std::fabs(nearest[1] - pz) <= MoveBlock::MAX_STEP;
    };

    uint32 const phase = bot->GetPhaseMask();
    float const fromX = bot->GetPositionX();
    float const fromY = bot->GetPositionY();
    float const dx = x - fromX;
    float const dy = y - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < 2.0f * LEDGE_SAMPLE)
        return false;

    float const allowance = MoveBlock::MAX_STEP + LEDGE_SAMPLE * MoveBlock::MARCH_SLOPE;
    float previousZ = bot->GetPositionZ();
    for (float along = LEDGE_SAMPLE; along < length; along += LEDGE_SAMPLE)
    {
        float const sampleX = fromX + dx * along / length;
        float const sampleY = fromY + dy * along / length;
        float const shallow = map->GetHeight(phase, sampleX, sampleY, previousZ + MoveBlock::MAX_STEP, true,
            allowance + MoveBlock::MAX_STEP);
        if (shallow > INVALID_HEIGHT && std::fabs(shallow - previousZ) <= allowance && onMesh(sampleX, sampleY, shallow))
        {
            previousZ = shallow;
            continue;
        }

        // The edge. The landing is the deep ground under the next few yards, as far as the arc carries.
        float const edgeZ = previousZ;
        for (float past = along; past < std::min(length, along + LEDGE_LANDING_REACH); past += LEDGE_SAMPLE)
        {
            float const landX = fromX + dx * past / length;
            float const landY = fromY + dy * past / length;
            float const landing = map->GetHeight(phase, landX, landY, edgeZ + MoveBlock::MAX_STEP, true,
                rules.DropMax + 2.0f * MoveBlock::MAX_STEP);
            if (landing <= INVALID_HEIGHT)
                continue;
            float const fall = edgeZ - landing;
            if (fall <= MoveBlock::MAX_STEP || fall > rules.DropMax || !onMesh(landX, landY, landing))
                continue;

            Route rest;
            Position const foot(landX, landY, landing, 0.0f);
            Position const to(x, y, z, 0.0f);
            float const straight = foot.GetExactDist2d(&to);
            if (!RoutePlanner::Instance().Plan(map, foot, to, rest) || !rest.Complete
                || rest.Length > std::max(straight, LEDGE_SAMPLE) * MAX_PATH_DETOUR)
                return false;

            drop = fall;
            return true;
        }

        return false;                       // an edge with nothing to land on within a jump of it
    }

    return false;                           // no edge on the line: the place is walkable straight to
}

bool Animus::Curriculum::TravelEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);
    EnvTravel& travel = _envs[env.Index];
    if (!bot || !map)
        return false;

    CurriculumTuning::TravelTuning const& tuning = _scenario.Tuning().Travel;
    ArenaDefinition const& arena = _scenario.Arena(env);
    bool const flying = arena.Flying;
    float walk = 0.0f;
    // On foot the trip is shorter: the lesson is how well the seat covers ground with what it has, not
    // whether a ride is worth summoning.
    float const least = arena.Indoors ? tuning.IndoorMin
        : arena.Ledges ? tuning.LedgeMin
        : arena.Underwater ? tuning.DiveMin
        : flying ? tuning.FlyingMin : arena.OnFoot ? tuning.FootMin : tuning.ObjectiveMin;
    float const most = arena.Indoors ? tuning.IndoorMax
        : arena.Ledges ? tuning.LedgeMax
        : arena.Underwater ? tuning.DiveMax
        : flying ? tuning.FlyingMax : arena.OnFoot ? tuning.FootMax : tuning.ObjectiveMax;
    // A water arena asks for a crossing: an objective whose way round is much longer than the way through, with
    // water in between. Where the ground offers none within reach, fall back to an ordinary trip rather than
    // failing the env -- a scenario that cannot build an episode takes the whole run down with it, and one
    // spawn point without a lake nearby is not a reason to stop training.
    //
    // Falling back silently would be worse than failing, though, because the arena would quietly become a second
    // copy of the open one and still be reported as teaching swimming. So the episode records whether it got a
    // crossing at all (`crossing`), and the stage gates the water arena on the seat actually swimming: a run
    // whose spawn points have no water in reach fails that gate and says so.
    travel.Indoors = arena.Indoors;
    travel.AirOnly = false;
    travel.Ledge = false;
    travel.LedgeDrop = 0.0f;
    travel.Dive = false;
    travel.DiveDepth = 0.0f;
    travel.Crossing = false;
    travel.DryDistance = 0.0f;
    travel.Travelled = 0.0f;
    travel.HasLastPos = false;
    travel.MarkMs = 0;
    travel.MarkDistance = -1.0f;
    travel.Nearest = -1.0f;
    travel.NearestMs = 0;
    travel.CloseRate = 0.0f;
    // What kind of trip is wanted beyond its length (TravelPlaceRules). The ground arenas draw a detour band first and
    // then look for an objective in it -- drawn uniformly, real detours were the tail: 51% of stage1_move's trips
    // and 82% of stage6_travel's had a dry detour under 1.15 -- while a crossing, a room and a flight each ask for
    // their own kind of trip and draw none. An air-only arena asks for a place the ground route does not reach.
    TravelPlaceRules rules;
    rules.DetourEasy = tuning.DetourEasy;
    rules.DetourHard = tuning.DetourHard;
    rules.AirOnly = arena.AirOnly;
    rules.AirDetour = tuning.AirDetour;
    rules.Ledge = arena.Ledges;
    rules.LedgeDetour = tuning.LedgeDetour;
    rules.DropMin = tuning.LedgeDropMin;
    rules.DropMax = tuning.LedgeDropMax;
    rules.Underwater = arena.Underwater;
    rules.DepthMin = tuning.DiveDepthMin;
    rules.DepthMax = tuning.DiveDepthMax;
    if (!flying && !arena.Water && !arena.Indoors && !arena.Ledges && !arena.Underwater)
    {
        float const roll = frand(0.0f, 1.0f);
        rules.Band = roll < tuning.DetourEasyShare ? 0
            : roll < tuning.DetourEasyShare + tuning.DetourMidShare ? 1 : 2;
    }
    travel.Band = rules.Band;
    // The clock this arena actually runs, not the stage's default: `open` and `broken` do not have to agree, and
    // a share of it rather than all of it, because arriving with one second to spare is not a trip a seat can be
    // asked to make every time.
    float const budget = float(env.EpisodeLengthMs) / 1000.0f * FEASIBLE_SHARE;
    // What the arena asked for first -- a crossing, or a place only the air reaches -- and an ordinary trip when
    // this spawn point has none within reach, rather than an env that cannot build an episode and takes the run
    // down with it. `crossing` and `air_only` report what was achieved, not what was asked, so a spawn point
    // with no plateau in range shows up as an air-only arena that offered none, and can be gated on like the
    // water arena's crossing.
    if (arena.Water
        && FindPlace(bot, map, least, most, flying, travel.Objective, budget, &walk, true, &travel.DryDistance,
            false, &travel.Shortcut, rules))
        travel.Crossing = true;
    else if (arena.AirOnly
        && FindPlace(bot, map, least, most, flying, travel.Objective, budget, &walk, false, nullptr, false,
            &travel.Shortcut, rules))
        travel.AirOnly = true;
    else if (arena.Ledges
        && FindPlace(bot, map, least, most, flying, travel.Objective, budget, &walk, false, nullptr, false,
            &travel.Shortcut, rules, &travel.LedgeDrop))
        travel.Ledge = true;
    else if (arena.Underwater
        && FindPlace(bot, map, least, most, flying, travel.Objective, budget, &walk, false, nullptr, false,
            &travel.Shortcut, rules, nullptr, &travel.DiveDepth))
        travel.Dive = true;
    else
    {
        TravelPlaceRules plain = rules;
        plain.AirOnly = false;
        plain.Ledge = false;
        plain.Underwater = false;
        // A detour band is a wish, not a condition: some spawn points have no hard detour within reach, and the
        // stage's retries move the seats but keep the band, so four draws of the hard band could all come up empty
        // and end the env with no episode (stage1_move logged a few a minute). The easiest band then, rather than
        // nothing; `detour_band` reports the band the episode actually got.
        if (!FindPlace(bot, map, least, most, flying, travel.Objective, budget, &walk, false, nullptr,
            arena.Indoors, &travel.Shortcut, plain))
        {
            if (plain.Band <= 0)
                return false;
            plain.Band = 0;
            travel.Band = 0;
            if (!FindPlace(bot, map, least, most, flying, travel.Objective, budget, &walk, false, nullptr,
                arena.Indoors, &travel.Shortcut, plain))
                return false;
        }
    }

    // What the way round costs on foot, for every arena rather than only the ones built around a crossing:
    // OBS_DETOUR is how a seat learns that the barrier in front of it runs for two hundred yards, and that is
    // as much use on broken ground as it is at a lake. One path at the build, against a reset that already
    // takes several; nothing per decision. Water and magma are excluded, so it is the ground's answer.
    if (travel.DryDistance <= 0.0f && !flying)
    {
        PathGenerator dry(bot);
        dry.SetIncludeFlags(NAV_GROUND);
        if (dry.CalculatePath(travel.Objective.GetPositionX(), travel.Objective.GetPositionY(),
            travel.Objective.GetPositionZ()) && (dry.GetPathType() & PATHFIND_NORMAL))
        {
            // Same caveat as FindPlace's: a shortcut answers PATHFIND_NORMAL too, and DryDistance is then the
            // straight line -- which makes OBS_DETOUR exactly 1.0 and tells the seat there is nothing in the way.
            travel.DryShortcut = (dry.GetPathType() & PATHFIND_NOT_USING_PATH) != 0;
            if (!travel.DryShortcut)
                travel.DryDistance = dry.getPathLength();
        }
    }

    travel.HasObjective = true;
    travel.Chain = arena.Checkpoints;
    travel.StartDistance = bot->GetExactDist2d(&travel.Objective);
    travel.WalkDistance = walk > 0.0f ? walk : travel.StartDistance;
    {
        float const speed = std::max(1.0f, bot->GetSpeed(flying ? MOVE_FLIGHT : MOVE_RUN));
        float const seconds = float(env.EpisodeLengthMs) / 1000.0f;
        travel.TripShare = seconds > 0.0f ? travel.WalkDistance / speed / seconds : 0.0f;
    }
    travel.KnowsFlyer = TravelBlock::FlyingMount(bot) != nullptr;
    travel.CouldMountFlyer = TravelBlock::CanSummonFlying(bot, &travel.FlyerRefusal);
    travel.FlyerZone = bot->GetZoneId();
    travel.FlyerArea = bot->GetAreaId();
    travel.CouldMountFlyer = TravelBlock::CanSummonFlying(bot);
    _scenario.PrepareFighter(bot, data.Seats[0]);
    return true;
}

bool Animus::Curriculum::TravelEncounter::RefreshWay(EnvTravel& travel, Player* bot, float stray,
    float refreshSeconds, float corner, uint32 nowMs)
{
    if (!travel.HasObjective || !bot || !bot->IsAlive())
        return false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();

    if (travel.Way.Valid)
    {
        travel.Way.Advance(x, y, z, corner);

        bool const moved = travel.Way.Next < travel.Way.Count
            && (bot->GetExactDist2d(travel.Way.X[travel.Way.Next], travel.Way.Y[travel.Way.Next]) > stray
                || std::fabs(z - travel.Way.Z[travel.Way.Next]) > LEDGE_STRAY_Z);
        bool const elapsed = nowMs > travel.WayMs
            && float(nowMs - travel.WayMs) / 1000.0f >= refreshSeconds;
        bool const elsewhere = travel.Way.To.GetExactDist2d(&travel.Objective) > 1.0f;

        // A way that arrives and is still being walked is left alone. Re-planning it would cost a search to
        // produce the same corners, and would reset the shaping for nothing.
        if (!moved && !elapsed && !elsewhere)
            return false;
    }

    Position const from(x, y, z, bot->GetOrientation());
    bool const planned = RoutePlanner::Instance().Plan(map, from, travel.Objective, travel.Way);
    travel.WayMs = nowMs;
    travel.WayFailed = !planned;
    return true;
}

float Animus::Curriculum::TravelEncounter::WayDistance(EnvTravel const& travel, Player const* bot)
{
    float const straight = bot->GetExactDist2d(&travel.Objective);
    if (!travel.Way.Valid)
        return straight;

    float const along = travel.Way.RemainingFrom(bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ());
    if (along < 0.0f)
        return straight;

    // A partial route stops short of the objective, so what is left along it is short by the gap at the end.
    // Adding that gap back keeps the number a distance to the objective rather than to the end of the map the
    // seat can currently see, and keeps it comparable across a re-plan that reaches further.
    if (!travel.Way.Complete && travel.Way.Count > 0)
    {
        uint32 const last = travel.Way.Count - 1;
        float const dx = travel.Way.X[last] - travel.Objective.GetPositionX();
        float const dy = travel.Way.Y[last] - travel.Objective.GetPositionY();
        return along + std::sqrt(dx * dx + dy * dy);
    }

    return along;
}

bool Animus::Curriculum::TravelEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seats act without a target (SeatEncoder::ActsWithoutTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::TravelEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    EnvTravel const& travel = _envs[env.Index];
    view.HasObjective = travel.HasObjective;
    view.Objective = travel.Objective;
    view.Detour = travel.HasObjective && travel.StartDistance > 0.0f && travel.DryDistance > 0.0f
        ? travel.DryDistance / travel.StartDistance : 0.0f;
    // The closing rate toward the objective replaces the scenario's toward the target; the moving rate is the
    // scenario's, measured for every seat (StageScenario::TrackMotion). The route the reward is shaped on stays
    // the encounter's: the actor never sees it.
    if (travel.HasObjective)
        view.CloseRate = travel.CloseRate;
    view.MountsAllowed = !_scenario.Arena(env).OnFoot;
    view.ArriveWithin = travel.Indoors ? TravelBlock::ARRIVE_INDOORS : TravelBlock::ARRIVE_DISTANCE;
    // An air-only trip keeps the flying mount and masks the ground one: its objective cannot be walked to. The
    // trip's, not the arena's: an air-only arena whose spawn point offered no such place fell back to an ordinary
    // flight, and a ride may arrive there.
    view.GroundMountAllowed = !travel.AirOnly;
}

void Animus::Curriculum::TravelEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::TravelTuning const& tuning = _scenario.Tuning().Travel;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvTravel& travel = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (!bot || !travel.HasObjective)
        return;

    uint32 const stepMs = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, travel.LastRewardMs);
    travel.LastRewardMs = env.EpisodeElapsedMs;

    // Ground actually covered. The first decision of an episode only records where the seat is; a jump from
    // wherever the last episode ended is not travel.
    if (travel.HasLastPos)
    {
        float const dx = bot->GetPositionX() - travel.LastX;
        float const dy = bot->GetPositionY() - travel.LastY;
        travel.Travelled += std::sqrt(dx * dx + dy * dy);
    }

    travel.LastX = bot->GetPositionX();
    travel.LastY = bot->GetPositionY();
    travel.HasLastPos = true;

    // The closing rate toward the objective, refreshed about once a second rather than every decision: a quarter
    // of a second of walking is 1.75 yards, which is mostly noise, and the question is whether the seat has been
    // getting anywhere. The moving rate is the scenario's now (StageScenario::TrackMotion), for every arena.
    if (!travel.MarkMs || env.EpisodeElapsedMs < travel.MarkMs)
    {
        travel.MarkMs = env.EpisodeElapsedMs;
        travel.MarkDistance = travel.LastDistance;
    }
    else if (env.EpisodeElapsedMs - travel.MarkMs >= 1000)
    {
        float const seconds = float(env.EpisodeElapsedMs - travel.MarkMs) / 1000.0f;
        travel.CloseRate = travel.MarkDistance > 0.0f && travel.LastDistance >= 0.0f
            ? (travel.MarkDistance - travel.LastDistance) / (seconds * TravelBlock::BASE_RUN_SPEED) : 0.0f;
        travel.MarkMs = env.EpisodeElapsedMs;
        travel.MarkDistance = travel.LastDistance;
    }
    if (bot->IsMounted())
        travel.MountedMs += stepMs;
    if (bot->IsMounted() && bot->CanFly())
    {
        travel.FlyingMountMs += stepMs;
        travel.Flew = true;
        travel.FlightSpeedSeen = std::max(travel.FlightSpeedSeen, bot->GetSpeed(MOVE_FLIGHT));
        travel.HeightSum += double(TravelBlock::HeightAboveGround(bot));
        ++travel.HeightSamples;
    }

    // Measured between two decisions that were both aloft, so nothing but flight is counted.
    bool const aloft = bot->IsMounted() && bot->CanFly()
        && TravelBlock::HeightAboveGround(bot) > 2.0f;
    if (aloft)
    {
        ++travel.AloftSteps;
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING))
            ++travel.AloftFlagged;
    }
    if (aloft && travel.LastAloft)
    {
        float const step = bot->GetExactDist2d(&travel.LastPos);
        travel.FlightDistance += double(step);
        travel.FlightMs += stepMs;
        if (stepMs)
            travel.FlightPeakYps = std::max(travel.FlightPeakYps, step / (float(stepMs) / 1000.0f));
    }
    travel.LastPos.Relocate(bot);
    travel.LastAloft = aloft;
    if (bot->IsMounted() && bot->CanFly() && TravelBlock::HeightAboveGround(bot) > 2.0f)
        travel.FlyingMs += stepMs;

    // Potential-based: what is closed pays, what is given back costs, so wandering cannot be farmed. A flying
    // arena shapes on the distance in three dimensions, because there the objective is a point in space and the
    // last part of the trip is downwards: arriving wants AtObjective's AIRBORNE_ABOVE as well as its six yards,
    // and a seat shaped on the ground distance alone flew to directly above the marker and hovered there until
    // the clock ran out. Every episode that flew failed to arrive, so flight paid 0.009 against a ground mount's
    // 0.388, and the policy sensibly stopped flying. The climb costs here and the descent pays it back, which
    // telescopes to nothing over the trip -- the point is that the seat can see the axis it has to close.
    bool const flyingArena = _scenario.Arena(env).Flying;

    // The distance that is shaped on is the distance along the way there, not the distance through whatever
    // lies between. Shaping on the straight line is what made a detour cost: every yard spent walking round a
    // ridge increases it and is charged for, so the seat was being taught not to go round things -- which is
    // exactly what the failures do. They run at 99% of run speed for the whole clock, cover 8.6x the straight
    // line, and never get closer than they were twelve seconds in. Potential shaping on the true remaining
    // distance has no local minimum to sit in; shaping on the crow's flight is made of them.
    //
    // Flying arenas keep the straight line, in three dimensions, because there is no mesh to walk and the
    // comment below still applies: a seat shaped on the ground distance alone flew to directly above the
    // marker and hovered there.
    bool replanned = false;
    if (!flyingArena)
        replanned = RefreshWay(travel, bot, tuning.RouteStray, tuning.RouteRefresh, tuning.RouteCorner,
            env.EpisodeElapsedMs);

    float const distance = flyingArena ? bot->GetExactDist(&travel.Objective) : WayDistance(travel, bot);

    // A re-planned route is a new potential function, and the difference between the old one and the new one is
    // not progress the seat made. FlagEncounter has the same rule where a flag changes hands: start over and
    // pay nothing for the change itself.
    // Spread over the trip rather than paid per 100 yd, so closing the whole way pays Travel.Progress once
    // whatever the trip's length. Per 100 yd, a 350-700 yd flight paid 4.3 for progress against 3.0 for arriving
    // -- shaping worth more than the outcome, which rewards.py's own audit flagged every twenty-five updates --
    // and a ground ride that closed most of the way was paid nearly as well as one that arrived. The ground
    // stages' trips are 40-160 yd, so they barely move.
    float const trip = std::max(100.0f, travel.WalkDistance);
    if (travel.LastDistance >= 0.0f && !travel.Arrived && !replanned)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (travel.LastDistance - distance) / trip);
    travel.LastDistance = distance;

    // The high-water mark of the trip, kept whether the seat arrives or not.
    if (travel.Nearest < 0.0f || distance < travel.Nearest)
    {
        travel.Nearest = distance;
        travel.NearestMs = env.EpisodeElapsedMs;
        travel.NearestX = bot->GetPositionX();
        travel.NearestY = bot->GetPositionY();
    }

    // How long it has been since the trip last gained on the objective (stall_seconds, stalls). A re-plan that
    // finds a shorter way counts as gaining and a longer one as not, which is the honest reading of both.
    if (!travel.Arrived)
    {
        if (travel.StallBest < 0.0f || distance < travel.StallBest - STALL_GAIN_YARDS)
        {
            travel.StallBest = distance;
            travel.StallSinceMs = env.EpisodeElapsedMs;
            travel.Stalling = false;
        }
        else
        {
            uint32 const stretch = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, travel.StallSinceMs);
            travel.StallLongestMs = std::max(travel.StallLongestMs, stretch);
            if (!travel.Stalling && stretch >= STALL_MIN_MS)
            {
                travel.Stalling = true;
                ++travel.Stalls;
            }
        }
    }

    // Room to move, charged by the second like a hazard and capped the same way. A seat scraping a wall is not
    // doing anything wrong in open country -- there is nothing out there to scrape -- but it is how a seat wedges
    // itself in a doorway, and the charge has to stay small enough that going through the doorway still plainly
    // wins. Measured off the seat's own probe, so it costs nothing extra to ask.
    if (!travel.Arrived && bot->IsAlive() && stepMs)
    {
        float const clearance = seat.Probe.Clearance * MoveBlock::CLEARANCE_RANGE;
        if (tuning.ClearanceMargin > 0.0f && clearance < tuning.ClearanceMargin)
        {
            float const seconds = float(stepMs) / 1000.0f;
            float const crowding = (tuning.ClearanceMargin - clearance) / tuning.ClearanceMargin;
            float const room = std::max(0.0f, tuning.ClearanceMax + seat.Rewards.Episode(RewardTerm::Clearance));
            ledger.Add(RewardTerm::Clearance, -std::min(tuning.Clearance * seconds * crowding, room));
        }
    }

    seat.Combat.DamageTaken += env.StepStats[seatIndex].DamageTaken;
    ledger.Add(RewardTerm::DamageTaken, -tuning.DamageTaken * seat.LastStepDamageTaken);

    // Indoors, arriving has to mean the right floor: two-dimensional arrival puts a seat under a staircase six
    // yards from an objective it has not reached. Air-only, it has to mean the plateau and not the cliff foot six
    // yards under its edge (Travel.AirArriveRise).
    // Below a ledge, the objective's own floor: the lip six yards above it is not there yet, and a seat that
    // arrived from the lip would never have to choose the drop.
    float const maxRise = travel.Indoors || travel.Ledge ? TravelBlock::ARRIVE_SAME_FLOOR
        : travel.AirOnly ? tuning.AirArriveRise : TravelBlock::ARRIVE_ANY_RISE;
    float const within = travel.Indoors ? TravelBlock::ARRIVE_INDOORS : TravelBlock::ARRIVE_DISTANCE;
    if (!travel.Arrived && !travel.ChainBroken && bot->IsAlive()
        && TravelBlock::AtObjective(bot, travel.Objective, maxRise, within))
    {
        if (travel.Chain)
        {
            // A checkpoint: paid the arrival (the flat part only -- Saved reads the clock against the first leg, and
            // means nothing from the second on), remembered as the first arrival if it is, and replaced with the
            // next leg from where the seat stands. The episode runs on to its clock either way: a leg that cannot
            // be placed (the clock nearly out, or no bed in reach) leaves the seat with nothing more to reach and
            // says so (chain_broken), because the outcome the chain measures is being alive at the end, and an
            // episode cut short by the placer would answer that for free.
            ++travel.Checkpoints;
            if (travel.Checkpoints == 1)
                travel.ArriveMs = env.EpisodeElapsedMs;
            ledger.Add(RewardTerm::Arrive, tuning.Arrive);
            if (!NextLeg(env, travel, bot))
                travel.ChainBroken = true;
        }
        else
        {
            travel.Arrived = true;
            travel.ArriveMs = env.EpisodeElapsedMs;
            ledger.Add(RewardTerm::Arrive, tuning.Arrive + tuning.FastArrive * Saved(travel));
        }
    }

    CombatTally& tally = seat.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }

    // The clock ran out short of the objective. Only the combat encounters used to set this, so on a travel stage
    // `timed_out` was 0 for every episode however it ended -- a trip that ran its full 150 s without arriving
    // reported neither arriving nor timing out, and stage1_move's `timed_out` floor could not fail. Nothing is
    // charged for it here: not arriving already forgoes RewardTerm::Arrive, and a second penalty would be a
    // change to what the stage teaches rather than to what it reports.
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (!travel.Arrived && !tally.TimedOut && timeIsUp)
        tally.TimedOut = true;
}

bool Animus::Curriculum::TravelEncounter::NextLeg(Env const& env, EnvTravel& travel, Player* bot) const
{
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map)
        return false;

    CurriculumTuning::TravelTuning const& tuning = _scenario.Tuning().Travel;
    ArenaDefinition const& arena = _scenario.Arena(env);

    // The same kind of place as the arena's first objective, drawn the same way, from here. Only the dive kind is
    // chained so far: an open-ground chain would want the detour bands, and nothing asks for one.
    TravelPlaceRules rules;
    rules.Underwater = arena.Underwater;
    rules.DepthMin = tuning.DiveDepthMin;
    rules.DepthMax = tuning.DiveDepthMax;

    // Within what is left of the clock, at the same share of it the first leg was held to.
    uint32 const leftMs = env.EpisodeLengthMs > env.EpisodeElapsedMs ? env.EpisodeLengthMs - env.EpisodeElapsedMs : 0;
    float const budget = float(leftMs) / 1000.0f * FEASIBLE_SHARE;
    if (budget <= 0.0f)
        return false;

    Position place;
    float walk = 0.0f;
    float depth = 0.0f;
    if (!FindPlace(bot, map, tuning.ChainMin, tuning.ChainMax, false, place, budget, &walk, false, nullptr, false,
        &travel.Shortcut, rules, nullptr, &depth))
        return false;

    // A new potential function: the shaping starts over from here, and so does the stall detector, or the jump in
    // distance would be charged as ground given back. RefreshWay re-plans on its own, since the way's end no
    // longer matches the objective.
    travel.Objective.Relocate(place);
    travel.Dive = arena.Underwater;
    travel.DiveDepth = depth;
    travel.WalkDistance = walk > 0.0f ? walk : bot->GetExactDist2d(&place);
    travel.LastDistance = -1.0f;
    travel.Nearest = -1.0f;
    travel.NearestMs = 0;
    travel.MarkMs = 0;
    travel.MarkDistance = -1.0f;
    travel.CloseRate = 0.0f;
    travel.StallBest = -1.0f;
    travel.StallSinceMs = 0;
    travel.Stalling = false;
    return true;
}

bool Animus::Curriculum::TravelEncounter::IsTerminal(Env const& env) const
{
    return _envs[env.Index].Arrived || _scenario.DeadForGood(env, 0);
}
