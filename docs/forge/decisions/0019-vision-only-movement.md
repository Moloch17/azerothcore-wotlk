# 0019: Bot movement is vision-only; stock pathfinding serves rewards and setup alone

**Date:** 2026-10-08

**Decision.** A seat is moved by the policy's keys through the player controller and by nothing else, and everything that
tells it where to go comes through what a player perceives (the camera, the entity list, the mental map, the party frames
and minimap, the sight list). No route hint, waypoint, path corner, route-derived column or route-ordered goal reaches the
policy. The stock AzerothCore navmesh pathfinding (`PathGenerator` and `MMapMgr` as upstream ships them, no forge diff)
**may** be used for two things only: a reward term's geometry and reset-time setup (an opponent spawn that must have a
path back for its creature, the M4 leader's legs). It is never called per decision from a parallel phase, and nothing the
forge added on top of stock navigation survives.

**Reason.** The owner, 2026-10-08. A model that follows route points or a route-derived shaping term learns the route, not
how to find a way, and it cannot play beside a human on a real realm, where no such hint exists (decision 0006, 0007).

**What was removed.**

- The dungeon route: `WingRoute` (boss ordering, spawn-to-spawn breadcrumbs, back-paths, the pack spine), the route
  bookkeeping of `InstanceEncounter` (`Route`, `Dense`, `RoutePacks`, `PackOf`, per-seat `Walk`), the `WingWaypoint` and
  `WingProgress` rewards (and their keys `WingWaypointYards`, `WingFullClear`), the route-built goal layout
  (`SeenAndLayout`, `GoalPlaces`; decision 0005), `RouteShortcut`, and the private-query `RoutePlanner`.
- The stages that stood on the route: `group2_corridor` and `dungeon1_pulls` (corridor, pull drill, chain pulls) and
  Go-Explore (`animus/explore.py`, the learner's archive, the `EXPLORE_STARTS` message). `dungeon2_ragefire` now seeds
  from `group1_roles`: the live stages are ten, the chain C3 -> G1 -> D2 -> D3.
- The forge's `MMapData::ThreadQueryScope` (no users) and the `forge route` console tool.
- The compass block's dead `detour` column (M1 only; compass revision 2, five columns; seeds carry by name, decision
  0012).

**What stays, and how it is kept honest.**

- M4's scripted leader (decision 0002) walks the corners of stock `PathGenerator` legs computed once per map when an
  episode is built, on the world thread outside `MapMgr::Update` (a PartyFollow stage resets there); `Steer` does no
  pathfinding. It is the thing followed, not an input of the policy under training.
- M1's straight-line compass (bearing and distance to the mark, withheld at the rungs of the fade, never fully) is not a
  route; it stays for M1 only.
- `WingTimeout` and the metric `wing_cleared_share` are a kill/clear share of the dungeon (creatures killed over those a
  full clear counts), with no navmesh. The old `wing_route_share` is gone (it was the same number).
- The leader's goal place in the dungeon stages is present only within the minimap's range (`PartyFollow.MinimapYards`, 60
  yd, 2D): what a minimap dot shows.
- `PathGenerator` also stays where a stock creature needs it (`Opponents.cpp`'s `Walkable`, `CombatEncounter`'s corridor
  starting points within `Combat.CorridorWalk`); both are reset-time setup.

**What it constrains.** A new term that measures progress along a path calls stock `PathGenerator` per leg at build or
reset time, caches it, and never feeds it to an observation, a goal place or a mask. Nothing in the forge builds a second
navmesh query, a route cache the policy can see, or a heuristic that steps between creature spawns.

**Behaviour changes for training.** The dungeon stages lose the waypoint and progress shaping; the stall clock (`Idle`)
now resets on kills and fights alone, so a long walk between packs reads as standing about after `WingStallGraceMs`. The
clock-out `Timeout` scales with the share of the dungeon left, not the route left. The ready-pull payment is capped at one
a run, as it already effectively was once the field route's packs were gone. The Instance reward terms lose `Approach`,
`PullClean` and `PullExtra`.

See also [0002](0002-no-scripted-teachers.md), [0005](0005-goal-places-seen-only.md),
[0006](0006-controller-only-movement.md), [principles.md](../principles.md) and the [index](README.md).
