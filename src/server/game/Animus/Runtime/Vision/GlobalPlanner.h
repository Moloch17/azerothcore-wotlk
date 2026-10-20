/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef ANIMUS_VISION_GLOBAL_PLANNER_H
#define ANIMUS_VISION_GLOBAL_PLANNER_H

#include "MapPlanner.h"
#include <cstdint>
#include <utility>
#include <vector>

/// **The global route** (2026-10-18): the route to the goal over the WHOLE of the seat's mental map at 1 yd, not only
/// the 96-yd window round the body (MapPlanner.h's PlanOnMap, whose goal past ~48 yd is reached through the 8-yd coarse
/// plan's blur even when the 1-yd memory holds the whole dungeon). A search from the goal outward (an A* steered at the
/// body), kept between decisions and recomputed only when the map changed under the route, so the body's cost and its
/// route are a lookup.
///
/// **The graph is 2.5D**: a node is a (cell, floor layer) pair -- MapCell's two floors -- so a deck and the hall under
/// it are two nodes with their own values (one value a cell lets the lower storey overwrite the upper's). A cell with
/// no floor known (unknown, or seen free with no floor hit) is one node that carries the height of the node it was
/// reached from (the last to lower its value; the search settles it once). A step from a node at height h into a
/// neighbouring cell goes to each of that cell's floor layers the body could walk between -- walking from the neighbour
/// onto h it may rise at most GLOBAL_STEP_UP and step down at most GLOBAL_STEP_DOWN, or drop further off a ledge short
/// of a lethal fall at a drop's price (GLOBAL_DROP_*; the search runs from the goal, so the walk is the reverse of the
/// relaxation) -- or to its floorless node at h. A floor layer out of that band is no wall either: it is another
/// storey, and no edge.
///
/// **Costs as the fine plan's** (PLAN_COSTS): floor and door 1, unknown PLAN_UNKNOWN 2 (GlobalUnknownCost: the hook a
/// learned price of unseen cells plugs into), hazard 5, wall 100, a node's wall the wall of ITS layer (MAP_WALL_LOW for
/// Floor[0], MAP_WALL_HIGH for Floor[1]; either for a floorless cell); a step from s to n costs its yards x (factor(s)
/// + factor(n)) / 2. **The configuration space** as ConfigurationSpace's, per node: a neighbour is a wall at height h
/// when its floor layer nearest h is within PLAN_HEIGHT_BAND of it and walled (a floorless neighbour with either wall
/// flag); a node walled on two opposite sides is closed (PLAN_BLOCKED), one next to a wall costs PLAN_INFLATED at least
/// and one of those with a wall within two cells on both sides along an axis or a diagonal (a 2-yd passage)
/// PLAN_PINCHED (unknown ground and a hazard keep their own, dearer factors). Known passable ground is never dearer
/// than unknown (MapPlanner.h's static_assert), so a route keeps to remembered floor rather than cut through the fog.
/// The one difference: a cell the seat itself stood on (MAP_VISITED) is never closed -- the body was there, so the gap
/// is one it fits. The shared memory keeps no MAP_VISITED (a body's ground is the seat's own) and its MAP_FREE is any
/// pass in the band, head height included, so a memory holds no stronger "walked" signal; the local plan closes the
/// same gaps, so near the body the two agree on what is passable. Inflation and the pinch count the walls as read, not
/// the closed holes (ConfigurationSpace counts both): every cell 8-adjacent to a closed hole is 8-adjacent to one of
/// the walls that closed it, except two corners of a diagonal pair's hole.
///
/// **The domain** is the map's kept 1-yd tiles' span (MentalMap::TileBounds) with the body's and the goal's tiles, plus
/// GLOBAL_MARGIN_TILES all round, at most GLOBAL_SPAN_TILES tiles either side of the body-goal box; a cell in it whose
/// tile is not kept is unknown, so a route may cut through space nothing is known of. The folded coarse tiles (a
/// realm's cap) and the far tier are not read: unknown here. Storage is a block of nodes per tile the search reaches.
///
/// **Settled lazily, as an A***: the search from the goal is steered at the body (the heap's key is a node's value plus
/// the octile distance from it to the body at the floor's factor, a consistent lower bound, so every settled node's
/// value is exact whatever the target) and runs until the body's nodes are settled and then every node whose key is
/// within GLOBAL_SETTLE_MARGIN of the body's value -- a band round the route, so the labels' last pose and a body a few
/// yards off the route are on settled nodes. A later read of a node not settled (a body that strayed, a rise elsewhere)
/// re-aims the kept heap at it (Retarget) and resumes where the search stopped.
///
/// **Recomputed** (GlobalRoute::Update) when the goal moved; when a node on the route from the body is now a wall of
/// its layer (or its layer is gone) where it was passable when reached -- within GLOBAL_CONTRADICTION_YARDS of walk a
/// contradiction of the route the bot is about to take, further on a route walled -- or a door on the route is known
/// closed (the GlobalBlocking hook); when GLOBAL_REPLAN_KNOWN cells were first seen since the plan
/// (MentalMap::KnownTotal: new ground may open a shorter way); when the body bumped a wall since the plan
/// (MentalMap::BumpedTotal, the bumper: a wall felt beside the route as well as on it -- the walk's own cells may be a
/// yard over from the face the body met -- says the way it was pushing does not go through); every
/// GLOBAL_REPLAN_SECONDS of the map's clock; and when the body left the domain. Between recomputes the field is fixed,
/// so the labels' C(last) and C(body) read the same field and their difference is movement alone.
namespace Animus::Vision
{
    /// The most a step from a cell onto its neighbour may rise and drop, yards (the walk's direction).
    constexpr float GLOBAL_STEP_UP = 1.5f;
    constexpr float GLOBAL_STEP_DOWN = 3.0f;
    /// **Drops** (2026-10-19): past GLOBAL_STEP_DOWN a step down is a drop off a ledge onto a floor the map holds
    /// below -- planned as the bake plans one (TravBake's DropSafe and DropHurt: one way, up to the lethal height
    /// FALL_LETHAL_FROM, not included), priced at the step's length plus GLOBAL_DROP_SURCHARGE yards plus the fall's
    /// damage (Player::HandleFall's line, FALL_DAMAGE_*) at GLOBAL_DROP_HURT_YARDS a whole health bar (the bake's
    /// PointNav.DropHurtYards). A landing the map has never seen is no floor and plans no drop; climbing back stays
    /// GLOBAL_STEP_UP. Before, any step down past 3 yd was refused and every drop a bot took was an accident.
    constexpr float GLOBAL_DROP_SURCHARGE = 2.0f;
    constexpr float GLOBAL_DROP_HURT_YARDS = 100.0f;
    /// A body or goal stands on a cell's floor layer within this of its feet (the fine plan's band).
    constexpr float GLOBAL_ATTACH_BAND = PLAN_HEIGHT_BAND;
    /// The domain's margin past the kept tiles, and its cap either side of the body-goal box, in tiles.
    constexpr int32_t GLOBAL_MARGIN_TILES = 1;
    constexpr int32_t GLOBAL_SPAN_TILES = 48;
    /// Cost yards settled past the body's before the search pauses (A*'s key: the band round the route), and how far
    /// the cell a read needs may be from the search's target before the heap is re-aimed at it.
    constexpr float GLOBAL_SETTLE_MARGIN = 24.0f;
    constexpr int32_t GLOBAL_RETARGET_CELLS = 4;
    /// The margin a single point's read settles past its value (the four cells round it), and how far past the least
    /// value round a cell the search goes before a cell with no node at the feet is given up on.
    constexpr float GLOBAL_POINT_MARGIN = 8.0f;
    constexpr float GLOBAL_GIVE_UP = 3.0f * PLAN_BLOCKED;
    /// The recompute triggers: map seconds since the plan, cells first seen since it, the route nodes checked for a new
    /// wall (from the body on), the walk within which a new wall is a contradiction, and how far the goal may move
    /// before it is another goal (yards).
    constexpr float GLOBAL_REPLAN_SECONDS = 20.0f;
    constexpr uint64_t GLOBAL_REPLAN_KNOWN = 2000;
    constexpr uint32_t GLOBAL_PATH_CHECK = 4096;
    constexpr float GLOBAL_CONTRADICTION_YARDS = 30.0f;
    /// **The route is kept** (2026-10-18): a recompute only for new ground seen or the clock (Known, Clock) replaces
    /// the field the body is following only when its cost from the body is lower by more than GLOBAL_KEEP_YARDS and
    /// GLOBAL_KEEP_SHARE of the kept one's (KeepRoute's margin) -- otherwise two near-equal ways round a pillar or a
    /// cliff trade places as each new look tips them, and the body walks back and forth between them (the nav
    /// evaluation's failed trips: 46 replans a trip against 4.7 for the trips that arrived). A contradiction, a bump,
    /// leaving the domain or another goal still replace it outright.
    constexpr float GLOBAL_KEEP_YARDS = 5.0f;
    constexpr float GLOBAL_KEEP_SHARE = 0.1f;
    constexpr float GLOBAL_GOAL_MOVED = 1.0f;
    /// The global route's coarse waypoint: the route's cell this many yards of walk from the body (the window plan's
    /// coarse walk reaches PLAN_COARSE_STEPS x COARSE_CELL).
    constexpr float GLOBAL_COARSE_YARDS = float(PLAN_COARSE_STEPS) * float(COARSE_CELL);

    /// Where a field reads the map's 1-yd tiles from: MAP_TILE_CELLS cells a tile, row by row, null for one not kept.
    class GlobalTiles
    {
    public:
        virtual ~GlobalTiles() = default;
        [[nodiscard]] virtual MapCell const* Tile(int32_t tx, int32_t ty) const = 0;
        /// The kept tiles' span, inclusive; false for none.
        virtual bool Bounds(int32_t& tx0, int32_t& ty0, int32_t& tx1, int32_t& ty1) const = 0;
    };

    /// A mental map's tiles (MentalMap::TileCells, TileBounds), bound afresh each decision.
    class MapTiles final : public GlobalTiles
    {
    public:
        void Bind(MentalMap const& map) { _map = &map; }
        [[nodiscard]] MapCell const* Tile(int32_t tx, int32_t ty) const override
        {
            return _map ? _map->TileCells(tx, ty) : nullptr;
        }
        bool Bounds(int32_t& tx0, int32_t& ty0, int32_t& tx1, int32_t& ty1) const override
        {
            return _map && _map->TileBounds(tx0, ty0, tx1, ty1);
        }

    private:
        MentalMap const* _map = nullptr;
    };

    /// **The hook for a learned price of unseen cells** (no behaviour of its own): the factor a cell the map knows
    /// nothing under is priced at -- an unknown cell, or one seen free with no floor and no wall, hazard or door -- for
    /// feet at z. The default is PLAN_COSTS' flat PLAN_UNKNOWN; a later source (the planner school's predicted costs)
    /// is set with GlobalField::SetUnknownCost before Begin. Read once a node, at its first touch, and never below the
    /// floor's factor (the search's heuristic counts every step at it at least).
    class GlobalUnknownCost
    {
    public:
        virtual ~GlobalUnknownCost() = default;
        [[nodiscard]] virtual float Factor(int32_t /*cx*/, int32_t /*cy*/, float /*z*/) const
        {
            return PLAN_COSTS[uint8_t(MapCode::Unknown)];
        }
    };

    /// **The hook for doors and gates known closed** (an object memory's BlockingAt): whether the 1-yd cell (cx, cy) is
    /// blocked for feet at z by something the map's cells do not say (a closed door is a MAP_DOOR, priced as a floor).
    /// Asked of the route's cells within GLOBAL_CONTRADICTION_YARDS each decision (a contradiction) and of every node
    /// at its first touch (priced a wall). Null: nothing is.
    class GlobalBlocking
    {
    public:
        virtual ~GlobalBlocking() = default;
        [[nodiscard]] virtual bool Blocked(int32_t cx, int32_t cy, float z) const = 0;
    };

    /// The factors a node is priced at (the class comment): MapPlanner.h's constants, the local plan's, so the two
    /// plans and the potential agree. A field's own copy so a harness can price both ways; the sim never changes it.
    struct GlobalPricing
    {
        float Codes[MAP_CODES] = { PLAN_COSTS[0], PLAN_COSTS[1], PLAN_COSTS[2], PLAN_COSTS[3], PLAN_COSTS[4] };
        float Inflated = PLAN_INFLATED;
        float Pinched = PLAN_PINCHED;
    };

    /// The field: one goal's cost-to-go over the 2.5D graph, settled lazily. Every decision takes the tiles afresh
    /// (Use) before any other call: a map's tile pointers do not outlive its next write.
    class GlobalField
    {
    public:
        /// The tiles (and the blocking hook, may be null) this decision's reads go through.
        void Use(GlobalTiles const& tiles, GlobalBlocking const* blocking = nullptr);
        /// The unknown-cell price (null: the default flat factor); held by pointer, read at a node's first touch.
        void SetUnknownCost(GlobalUnknownCost const* cost) { _unknownCost = cost; }
        void SetPricing(GlobalPricing const& pricing) { _pricing = pricing; }
        /// The body's clearance (BodyClearance): a node's centre nearer a measured face is blocked. Before Begin.
        void SetClearance(float clearance) { _clearance = clearance; }
        /// Whether a step down past GLOBAL_STEP_DOWN may be a drop (GLOBAL_DROP_*). Before Begin.
        void SetDrops(bool drops) { _drops = drops; }
        /// Starts the field to the goal at (gx, gy, gz), its domain round the goal and the body at (bx, by).
        void Begin(float gx, float gy, float gz, float bx, float by);
        /// Forgets the field (its blocks kept for reuse).
        void Clear();
        [[nodiscard]] bool Active() const { return _active; }

        /// Settles the nodes round (x, y) at feet z, then GLOBAL_SETTLE_MARGIN past the body's value.
        void SettleAround(float x, float y, float z);
        /// The cost-to-go at (x, y), feet z, yards of cost: bilinear over the four cells round the point, each read on
        /// its layer nearest z (within GLOBAL_ATTACH_BAND), blocked and unreached nodes left out and the weights
        /// renormalised (PlanValueAt's rule); settles on demand. TRAV_UNREACHABLE outside the domain or with nothing
        /// reached.
        [[nodiscard]] float ValueAt(float x, float y, float z);
        /// The route points from the body (FineRoute's rule on the field: the walk follows each node's next node
        /// towards the goal, the cell after 5, 10, 20, 30 steps in the body's heading-up frame for `yaw`).
        [[nodiscard]] PlanRoute Route(float x, float y, float z, float yaw);
        /// The coarse waypoint on the field: the route's cell GLOBAL_COARSE_YARDS of walk from the body (or its end),
        /// as CoarseWaypoint's scalars (dist over GLOBAL_COARSE_YARDS).
        [[nodiscard]] PlanWaypoint Coarse(float x, float y, float z, float yaw);
        /// The route's first PLAN_KEPT_CELLS cells from the body as a kept walk (KeepRoute's), so a decision that falls
        /// back on the window plan follows on from where the global route was going; Count 0 for none.
        void Walk(float x, float y, float z, PlanWalk& out);
        /// Whether the route from the body crosses a node that is now a wall of its layer, or whose layer is gone,
        /// where it was passable when reached, or a cell the blocking hook calls blocked within `hookYards` of walk
        /// (GLOBAL_PATH_CHECK nodes at most); `yards` is the walk to the first such node.
        [[nodiscard]] bool RouteChanged(float x, float y, float z, float hookYards, float& yards);
        /// Whether (x, y) lies in the domain.
        [[nodiscard]] bool Contains(float x, float y) const;
        /// The route's walk from the body to the goal, yards (the harness's measure), and its cells' (x, y) when
        /// `cells` is given; TRAV_UNREACHABLE without one.
        [[nodiscard]] float WalkYards(float x, float y, float z, uint32_t maxSteps = 1u << 20,
            std::vector<std::pair<int32_t, int32_t>>* cells = nullptr);

        [[nodiscard]] uint32_t SettledNodes() const { return _settled; }
        [[nodiscard]] std::size_t Blocks() const { return _blockTile.size(); }
        [[nodiscard]] std::size_t Bytes() const;

    private:
        static constexpr int32_t SLOT_NODES = MAP_TILE_CELLS;
        static constexpr int32_t BLOCK_NODES = 2 * SLOT_NODES;
        enum : uint8_t
        {
            NODE_TOUCHED = 0x01,    // its factor and height are set
            NODE_SETTLED = 0x02,
            NODE_NEXT = 0x04,       // it has a next node towards the goal (the goal's own has none)
            NODE_FLOOR = 0x08,      // it was a floor layer's node when touched (else a floorless cell's)
        };
        /// A heap entry: its key (the value plus the heuristic to the target), the value it was pushed at, the node.
        struct Entry
        {
            float Key;
            float Value;
            int32_t Node;
            bool operator>(Entry const& other) const { return Key > other.Key; }
        };

        [[nodiscard]] MapCell const& CellAt(int32_t cx, int32_t cy);
        [[nodiscard]] int32_t TileIndex(int32_t cx, int32_t cy) const;
        [[nodiscard]] int32_t Block(int32_t tileIndex);
        [[nodiscard]] int32_t NodeOf(int32_t cx, int32_t cy, int32_t slot);
        [[nodiscard]] int32_t FindNode(int32_t cx, int32_t cy, int32_t slot) const;
        void CellOfNode(int32_t node, int32_t& cx, int32_t& cy, int32_t& slot) const;
        /// The node a body at feet z stands on in cell (cx, cy): its floor layer nearest z within GLOBAL_ATTACH_BAND,
        /// else, for a cell with no floor, its node when one was reached at a height within the band; -1 for none.
        [[nodiscard]] int32_t Attach(int32_t cx, int32_t cy, float z);
        /// Sets a node's factor (its code, the closing, the inflation) and height z, at its first touch only.
        void Touch(int32_t node, int32_t cx, int32_t cy, int32_t slot, float z);
        [[nodiscard]] bool WallAt(int32_t cx, int32_t cy, float z);
        /// One entry off the heap, its node settled and its neighbours relaxed; false when the heap is empty.
        bool Step();
        [[nodiscard]] float Heuristic(int32_t cx, int32_t cy) const;
        /// Aims the search at cell (cx, cy): the heap re-keyed when it is more than GLOBAL_RETARGET_CELLS from the
        /// target the keys were made for.
        void Retarget(int32_t cx, int32_t cy);
        void SettleNode(int32_t cx, int32_t cy, float z);
        /// Settles the point's cell, then every node keyed within `margin` of its value; the point's value (the least
        /// settled round it when its own cell has none; PLAN_SENTINEL for none).
        float SettlePoint(float x, float y, float z, float margin);
        /// The least settled value in the 5 x 5 cells round (cx, cy), either layer; PLAN_SENTINEL for none.
        [[nodiscard]] float LeastNear(int32_t cx, int32_t cy) const;
        void SettleTo(float limit);
        /// The node a walk from the body starts on: the body's own, else the lowest settled unblocked node round it.
        [[nodiscard]] int32_t StartNode(float x, float y, float z);
        [[nodiscard]] int32_t NextOf(int32_t node) const;

        GlobalTiles const* _tiles = nullptr;
        GlobalBlocking const* _blocking = nullptr;
        GlobalUnknownCost const* _unknownCost = nullptr;
        GlobalPricing _pricing;
        bool _active = false;
        int32_t _tx0 = 0;
        int32_t _ty0 = 0;
        int32_t _tw = 0;
        int32_t _th = 0;
        uint32_t _settled = 0;
        uint32_t _call = 0;
        int32_t _targetX = 0;
        int32_t _targetY = 0;
        float _floorFactor = 1.0f;
        float _clearance = PLAN_CLEARANCE;
        bool _drops = true;
        float _goalPointX = 0.0f;   // the goal (Begin), whose own cells stand clear (PLAN_GOAL_CLEAR)
        float _goalPointY = 0.0f;
        // Per domain tile: its block (-1 none), and its cells as fetched this call.
        std::vector<int32_t> _blockOf;
        std::vector<MapCell const*> _tileCells;
        std::vector<uint32_t> _fetched;
        // Per block: the domain tile it is; per node (BLOCK_NODES a block, slot-major): the value, the factor, the
        // height (quarter yards), the next node's direction (bits 0-2) and slot (bit 3), the state.
        std::vector<int32_t> _blockTile;
        std::vector<float> _value;
        std::vector<float> _factor;
        std::vector<int16_t> _height;
        std::vector<uint8_t> _next;
        std::vector<uint8_t> _state;
        std::vector<Entry> _heap;
    };

    /// Why the last recompute was made (GlobalRoute::Update).
    enum class GlobalReplan : uint8_t
    {
        None = 0,
        Fresh,          // no field, or another goal
        Contradiction,  // a new wall (or a door known closed) on the route within GLOBAL_CONTRADICTION_YARDS
        RouteWalled,    // a new wall on the route further on
        Known,          // GLOBAL_REPLAN_KNOWN cells first seen
        Clock,          // GLOBAL_REPLAN_SECONDS passed
        Left,           // the body left the domain
        Bumped,         // the body felt a wall the map lacked (MentalMap::MarkBumped) off the route's own cells
    };

    /// **A seat's global route** (the class comment): the field to its goal, kept between decisions, and the recompute
    /// triggers. Not thread safe: a seat's, on its map thread.
    class GlobalRoute
    {
    public:
        /// Forgets the field and the episode's counts: the next Update plans afresh (an episode's start).
        void Reset();
        /// This decision's field for the body at (x, y, z) and the goal at (gx, gy, gz), recomputed when due; true when
        /// it routes the body: the goal's cell (or one of its eight neighbours) is known on the map, and the body's
        /// cost-to-go is reached. Nothing is planned (the field is cleared) while the goal is not known. `map` must
        /// not be written between this and the decision's other reads, and `blocking` must outlive them.
        /// `clearance`: the body's (BodyClearance); `drops`: whether a drop may be planned (GLOBAL_DROP_*).
        bool Update(MentalMap const& map, float x, float y, float z, float gx, float gy, float gz,
            GlobalBlocking const* blocking = nullptr, float clearance = PLAN_CLEARANCE, bool drops = true);
        [[nodiscard]] GlobalField& Field() { return _field; }
        /// Recomputes this episode after its first plan, those of them a contradiction, and the last one's reason.
        [[nodiscard]] uint32_t Replans() const { return _replans; }
        [[nodiscard]] uint32_t Contradictions() const { return _contradictions; }
        [[nodiscard]] GlobalReplan LastReplan() const { return _last; }
        /// What the last Update did: GlobalReplan::None when it kept the field.
        [[nodiscard]] GlobalReplan ThisUpdate() const { return _this; }
        /// The route kept this update: a Known or Clock recompute's field did not undercut it by the margin
        /// (GLOBAL_KEEP_*), and the field the body was following stayed; and how many times since Reset.
        [[nodiscard]] bool KeptThisUpdate() const { return _keptThis; }
        [[nodiscard]] uint32_t Kept() const { return _kept; }

    private:
        GlobalField _field;
        GlobalField _trial;     // a Known or Clock recompute, made beside the field it may replace
        MapTiles _tiles;
        bool _keptThis = false;
        uint32_t _kept = 0;
        float _goalX = 0.0f;
        float _goalY = 0.0f;
        float _goalZ = 0.0f;
        double _planClock = 0.0;
        uint64_t _planKnown = 0;
        uint64_t _planBumped = 0;
        bool _goalKnown = false;
        uint32_t _plans = 0;
        uint32_t _replans = 0;
        uint32_t _contradictions = 0;
        GlobalReplan _last = GlobalReplan::None;
        GlobalReplan _this = GlobalReplan::None;
    };
}

#endif
