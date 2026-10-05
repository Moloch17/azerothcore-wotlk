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

#ifndef ANIMUS_LIB_CURRICULUM_HUMAN_POOLS_H
#define ANIMUS_LIB_CURRICULUM_HUMAN_POOLS_H

#include "Define.h"
#include <boost/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

/// **Trips and starts from real human play** (human-play-data plan 2.6, 7). `animus.human` writes two files
/// (apps/forge/python/animus/human/FORMAT.md section 5): human_trips.json, where players actually went between two
/// places, and human_hard_spots.json, where they got stuck, fell, drowned or died. The travel arenas draw a share of
/// their training trips and starts from them (TravelEncounter, AnimusForge.Human.*).
///
/// What is here is pure -- the parsing and the per-arena filter -- so it is tested on its own (HumanPoolsTest). What
/// needs the world (does the start stand on ground, is the end reachable on foot) is TravelEncounter's.
namespace Animus::Curriculum::HumanPools
{
    /// The files' version. Anything else is refused whole: a reader that guesses at a format it does not know trains
    /// on whatever the guess makes of it.
    constexpr int64 FORMAT = 1;

    /// How a trip was made. A bit each, so an arena can accept several (ModesFor).
    enum class Mode : uint8
    {
        Ground  = 0x1,
        Swim    = 0x2,
        Fly     = 0x4,
        Mounted = 0x8,
    };

    enum class SpotKind : uint8
    {
        Death,
        Stuck,
        Fall,
        Drown,
    };

    struct Point
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    struct Trip
    {
        Point Start;
        Point End;
        float Seconds = 0.0f;
        Mode How = Mode::Ground;
        std::vector<Point> Path;
    };

    struct HardSpot
    {
        Point Pos;
        SpotKind Kind = SpotKind::Death;
        uint32 Count = 1;
    };

    /// A file's content by map id, or why there is none. `Error` set means the whole file was refused and `Maps` is
    /// empty: the feature stays off rather than training on part of a file whose writer and reader disagree.
    template <class T>
    struct Parsed
    {
        std::map<uint32, std::vector<T>> Maps;
        std::string Error;

        [[nodiscard]] bool Ok() const { return Error.empty(); }
        [[nodiscard]] std::size_t Count() const
        {
            std::size_t count = 0;
            for (auto const& [map, entries] : Maps)
                count += entries.size();
            return count;
        }
    };

    [[nodiscard]] inline float Distance2d(Point const& a, Point const& b)
    {
        return std::hypot(a.X - b.X, a.Y - b.Y);
    }

    namespace Detail
    {
        inline bool ToFloat(boost::json::value const& value, float& out)
        {
            if (value.is_double())
                out = float(value.get_double());
            else if (value.is_int64())
                out = float(value.get_int64());
            else if (value.is_uint64())
                out = float(value.get_uint64());
            else
                return false;
            return std::isfinite(out);
        }

        inline bool ToPoint(boost::json::value const& value, Point& out)
        {
            boost::json::array const* xyz = value.if_array();
            return xyz && xyz->size() == 3 && ToFloat((*xyz)[0], out.X) && ToFloat((*xyz)[1], out.Y)
                && ToFloat((*xyz)[2], out.Z);
        }

        inline bool ToMode(boost::json::value const& value, Mode& out)
        {
            boost::json::string const* name = value.if_string();
            if (!name)
                return false;
            if (*name == "ground")
                out = Mode::Ground;
            else if (*name == "swim")
                out = Mode::Swim;
            else if (*name == "fly")
                out = Mode::Fly;
            else if (*name == "mounted")
                out = Mode::Mounted;
            else
                return false;
            return true;
        }

        inline bool ToKind(boost::json::value const& value, SpotKind& out)
        {
            boost::json::string const* name = value.if_string();
            if (!name)
                return false;
            if (*name == "death")
                out = SpotKind::Death;
            else if (*name == "stuck")
                out = SpotKind::Stuck;
            else if (*name == "fall")
                out = SpotKind::Fall;
            else if (*name == "drown")
                out = SpotKind::Drown;
            else
                return false;
            return true;
        }

        inline bool ToMapId(std::string_view key, uint32& out)
        {
            if (key.empty())
                return false;
            auto const [end, error] = std::from_chars(key.data(), key.data() + key.size(), out);
            return error == std::errc() && end == key.data() + key.size();
        }

        /// The envelope both files share: {"format": 1, "maps": {"<map id>": [entry, ...]}}, each entry read by
        /// `read` (false: malformed). The first problem refuses the file and says where it was.
        template <class T, class Read>
        Parsed<T> Envelope(std::string_view text, char const* what, Read&& read)
        {
            Parsed<T> parsed;
            auto const fail = [&parsed](std::string message)
            {
                parsed.Maps.clear();
                parsed.Error = std::move(message);
                return parsed;
            };

            boost::system::error_code error;
            boost::json::value const root = boost::json::parse(boost::json::string_view(text.data(), text.size()),
                error);
            if (error)
                return fail(std::string("not JSON: ") + error.message());
            boost::json::object const* object = root.if_object();
            if (!object)
                return fail("not a JSON object");

            boost::json::value const* format = object->if_contains("format");
            if (!format || !format->is_int64() || format->get_int64() != FORMAT)
                return fail("\"format\" is missing or not " + std::to_string(FORMAT));

            boost::json::value const* maps = object->if_contains("maps");
            if (!maps || !maps->is_object())
                return fail("\"maps\" is missing or not an object");

            for (auto const& [key, entries] : maps->get_object())
            {
                std::string_view const name(key.data(), key.size());
                uint32 mapId = 0;
                if (!ToMapId(name, mapId))
                    return fail("map key \"" + std::string(name) + "\" is not a map id");
                boost::json::array const* list = entries.if_array();
                if (!list)
                    return fail("map " + std::string(name) + " is not a list of " + what);

                std::vector<T>& out = parsed.Maps[mapId];
                out.reserve(list->size());
                for (std::size_t i = 0; i < list->size(); ++i)
                {
                    T entry;
                    boost::json::object const* fields = (*list)[i].if_object();
                    if (!fields || !read(*fields, entry))
                        return fail("map " + std::string(name) + ", entry " + std::to_string(i) + ": not a valid "
                            + what);
                    out.push_back(std::move(entry));
                }
            }

            return parsed;
        }
    }

    /// human_trips.json: {"format": 1, "maps": {"<map id>": [{"start": [x, y, z], "end": [x, y, z], "seconds": s,
    /// "mode": "ground|swim|fly|mounted", "path": [[x, y, z], ...]}, ...]}}. `path` may be absent.
    [[nodiscard]] inline Parsed<Trip> ParseTrips(std::string_view text)
    {
        return Detail::Envelope<Trip>(text, "trip", [](boost::json::object const& fields, Trip& trip)
        {
            boost::json::value const* start = fields.if_contains("start");
            boost::json::value const* end = fields.if_contains("end");
            boost::json::value const* seconds = fields.if_contains("seconds");
            boost::json::value const* mode = fields.if_contains("mode");
            if (!start || !end || !seconds || !mode || !Detail::ToPoint(*start, trip.Start)
                || !Detail::ToPoint(*end, trip.End) || !Detail::ToFloat(*seconds, trip.Seconds)
                || trip.Seconds < 0.0f || !Detail::ToMode(*mode, trip.How))
                return false;

            if (boost::json::value const* path = fields.if_contains("path"))
            {
                boost::json::array const* points = path->if_array();
                if (!points)
                    return false;
                trip.Path.resize(points->size());
                for (std::size_t i = 0; i < points->size(); ++i)
                    if (!Detail::ToPoint((*points)[i], trip.Path[i]))
                        return false;
            }
            return true;
        });
    }

    /// human_hard_spots.json: {"format": 1, "maps": {"<map id>": [{"pos": [x, y, z], "kind":
    /// "death|stuck|fall|drown", "count": n}, ...]}}, n a whole number of at least 1.
    [[nodiscard]] inline Parsed<HardSpot> ParseHardSpots(std::string_view text)
    {
        return Detail::Envelope<HardSpot>(text, "hard spot", [](boost::json::object const& fields, HardSpot& spot)
        {
            boost::json::value const* pos = fields.if_contains("pos");
            boost::json::value const* kind = fields.if_contains("kind");
            boost::json::value const* count = fields.if_contains("count");
            if (!pos || !kind || !count || !Detail::ToPoint(*pos, spot.Pos) || !Detail::ToKind(*kind, spot.Kind))
                return false;
            if (count->is_int64() && count->get_int64() >= 1
                && count->get_int64() <= int64(std::numeric_limits<uint32>::max()))
                spot.Count = uint32(count->get_int64());
            else if (count->is_uint64() && count->get_uint64() >= 1
                && count->get_uint64() <= uint64(std::numeric_limits<uint32>::max()))
                spot.Count = uint32(count->get_uint64());
            else
                return false;
            return true;
        });
    }

    /// The trip modes an arena takes, from its own flags: a flight arena flies, a water or lakebed arena swims, an
    /// on-foot arena walks, and an arena that lets the seat ride takes the walks and the rides.
    [[nodiscard]] inline uint8 ModesFor(bool flying, bool water, bool underwater, bool onFoot)
    {
        if (flying)
            return uint8(Mode::Fly);
        if (water || underwater)
            return uint8(Mode::Swim);
        if (onFoot)
            return uint8(Mode::Ground);
        return uint8(Mode::Ground) | uint8(Mode::Mounted);
    }

    /// What an arena is, as far as a trip or a start can be judged without the world.
    struct ArenaTerrain
    {
        uint32 MapId = 0;
        float Least = 0.0f;                 // the arena's trip distance band, straight line on the ground, yards
        float Most = 0.0f;
        uint8 Modes = 0;                    // ModesFor
        /// The arena's training ground (its spawn points, or the stage's) and its evaluation ground. A start has to be
        /// within Reach of the first; neither end may be nearer the second than the first, so nothing a pool hands
        /// training ever stands on the ground the evaluation is scored on.
        std::vector<Point> Training;
        std::vector<Point> HeldOut;
        float Reach = 0.0f;
        float Seconds = 0.0f;               // the arena's clock; a trip a human took longer than this over is refused
        /// Ledge arenas: the end is this far below the start (with DROP_SLACK either way, for where a recorded
        /// position sits over the ground it stands on).
        bool Descent = false;
        float DropMin = 0.0f;
        float DropMax = 0.0f;
        /// Water and lakebed arenas: a start or a spot may be in the water, and a drowning is one of theirs.
        bool Wet = false;
    };

    constexpr float DROP_SLACK = 1.0f;

    /// Why a trip or a spot was not taken, in the order they are tested.
    enum class Verdict : uint8
    {
        Taken,
        Map,
        Mode,
        Distance,
        Clock,
        Drop,
        Terrain,
        HeldOut,
        Kind,
        Stands,             // TravelEncounter's: the start does not stand on the arena's ground
        Count
    };

    [[nodiscard]] inline char const* VerdictName(Verdict verdict)
    {
        switch (verdict)
        {
            case Verdict::Taken: return "taken";
            case Verdict::Map: return "map";
            case Verdict::Mode: return "mode";
            case Verdict::Distance: return "distance";
            case Verdict::Clock: return "clock";
            case Verdict::Drop: return "drop";
            case Verdict::Terrain: return "terrain";
            case Verdict::HeldOut: return "held-out ground";
            case Verdict::Kind: return "kind";
            case Verdict::Stands: return "does not stand";
            default: return "?";
        }
    }

    /// How many of each verdict, for the startup line.
    struct Tally
    {
        uint32 Counts[uint8(Verdict::Count)] = {};

        void Add(Verdict verdict) { ++Counts[uint8(verdict)]; }
        [[nodiscard]] uint32 Of(Verdict verdict) const { return Counts[uint8(verdict)]; }
        /// "mode 3, distance 12" -- the refusals only, those that happened.
        [[nodiscard]] std::string Refusals() const
        {
            std::string text;
            for (uint8 verdict = uint8(Verdict::Map); verdict < uint8(Verdict::Count); ++verdict)
                if (Counts[verdict])
                    text += (text.empty() ? "" : ", ") + std::string(VerdictName(Verdict(verdict))) + " "
                        + std::to_string(Counts[verdict]);
            return text.empty() ? "none" : text;
        }
    };

    namespace Detail
    {
        [[nodiscard]] inline float Nearest(Point const& at, std::vector<Point> const& points)
        {
            float best = std::numeric_limits<float>::max();
            for (Point const& point : points)
                best = std::min(best, Distance2d(at, point));
            return best;
        }

        /// On the arena's ground: within reach of training ground (when `reach`), and nearer it than any held-out.
        [[nodiscard]] inline Verdict Ground(Point const& at, ArenaTerrain const& arena, bool reach)
        {
            float const training = Nearest(at, arena.Training);
            if (reach && training > arena.Reach)
                return Verdict::Terrain;
            if (!arena.HeldOut.empty() && Nearest(at, arena.HeldOut) <= training)
                return Verdict::HeldOut;
            return Verdict::Taken;
        }
    }

    /// Whether `trip`, made on map `mapId`, is one `arena` could have set.
    [[nodiscard]] inline Verdict Judge(uint32 mapId, Trip const& trip, ArenaTerrain const& arena)
    {
        if (mapId != arena.MapId)
            return Verdict::Map;
        if (!(arena.Modes & uint8(trip.How)))
            return Verdict::Mode;
        float const distance = Distance2d(trip.Start, trip.End);
        if (distance < arena.Least || distance > arena.Most)
            return Verdict::Distance;
        if (arena.Seconds > 0.0f && trip.Seconds > arena.Seconds)
            return Verdict::Clock;
        if (arena.Descent)
        {
            float const drop = trip.Start.Z - trip.End.Z;
            if (drop < arena.DropMin - DROP_SLACK || drop > arena.DropMax + DROP_SLACK)
                return Verdict::Drop;
        }
        if (Verdict const start = Detail::Ground(trip.Start, arena, true); start != Verdict::Taken)
            return start;
        // The end only has to stay off the evaluation ground: it is within the band of a start that is in reach.
        return Detail::Ground(trip.End, arena, false);
    }

    /// Whether a hard spot on map `mapId` is a start `arena` could use: on its ground, and a drowning only where the
    /// arena has water.
    [[nodiscard]] inline Verdict Judge(uint32 mapId, HardSpot const& spot, ArenaTerrain const& arena)
    {
        if (mapId != arena.MapId)
            return Verdict::Map;
        if (spot.Kind == SpotKind::Drown && !arena.Wet)
            return Verdict::Kind;
        return Detail::Ground(spot.Pos, arena, true);
    }

    /// The entries of `parsed` that `arena` takes, every one judged into `tally` (other maps' entries included, as
    /// Map).
    template <class T>
    [[nodiscard]] std::vector<T> Filter(Parsed<T> const& parsed, ArenaTerrain const& arena, Tally& tally)
    {
        std::vector<T> taken;
        for (auto const& [mapId, entries] : parsed.Maps)
            for (T const& entry : entries)
            {
                Verdict const verdict = Judge(mapId, entry, arena);
                tally.Add(verdict);
                if (verdict == Verdict::Taken)
                    taken.push_back(entry);
            }
        return taken;
    }

    /// Cumulative counts over `spots`, for a draw weighted by how often humans came to grief there (Pick).
    [[nodiscard]] inline std::vector<uint64> Weights(std::vector<HardSpot> const& spots)
    {
        std::vector<uint64> cumulative;
        cumulative.reserve(spots.size());
        uint64 total = 0;
        for (HardSpot const& spot : spots)
            cumulative.push_back(total += std::max<uint32>(1, spot.Count));
        return cumulative;
    }

    /// The spot a roll in [0, total) lands on.
    [[nodiscard]] inline std::size_t Pick(std::vector<uint64> const& cumulative, uint64 roll)
    {
        auto const found = std::upper_bound(cumulative.begin(), cumulative.end(), roll);
        return std::min<std::size_t>(std::size_t(found - cumulative.begin()), cumulative.empty() ? 0
            : cumulative.size() - 1);
    }
}

#endif
