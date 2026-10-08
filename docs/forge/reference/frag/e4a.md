## E4. InstanceEncounter (second half), the wing ladder, EntranceRespawn, SeenPlaces

Scope: `src/server/game/Animus/Scenario/Curriculum/Encounters/InstanceEncounter.cpp` lines 1395-2752 (the first half,
1-1400, and `InstanceBosses` are in the E3 fragment: `Build`, `Update`, `RiseDead`, `TraceWing`, `WingRoute` up to the
navmesh fallback, the episode-info columns and the first reward table), `WingLadder.h/.cpp`, `WingRun.h`,
`EntranceRespawn.h/.cpp`, `SeenPlaces.h`, and the call sites in `StageScenario.cpp`, `AnimusForge.cpp` and
`Bridge/ClusterLink.*` that make the wing ladder the cluster's. Paths are relative to
`src/server/game/Animus/Scenario/Curriculum/Encounters/` unless they start with another directory. Section numbers
E3.x refer to the E3 fragment. Defaults quoted for tuning keys are those in `CurriculumTuning.h`; a live conf file can
override them (see [cpp-tuning-keys.md](cpp-tuning-keys.md) and [config-keys.md](config-keys.md)); I did not read the
live conf, so every "default" below is UNVERIFIED as the live value.

### E4.0 Map of the files

| Path | Lines | Role |
|---|---|---|
| `InstanceEncounter.cpp` (1395-2752 here) | 2752 | field route, per-decision enemy upkeep, Go-Explore/corridor/drill starts, drill ladder, seen-places view, rewards, terminal |
| `WingLadder.h` | 98 | the whole-dungeon difficulty ladder state machine (one way, probes only, collapse alarm) |
| `WingLadder.cpp` | 111 | its implementation (`Note`, `Follow`) |
| `WingRun.h` | 175 | pure helpers: tier of rung, leader seat, stray/away predicates, seeded picks, corridor bookkeeping, chain-pull tracker |
| `EntranceRespawn.h` | 147 | `RespawnClock`, `WipeLatch`, `RiseAtEntrance`, `ForgetFrame` declarations |
| `EntranceRespawn.cpp` | 58 | `RiseAtEntrance`, `ForgetFrame` |
| `SeenPlaces.h` | 248 | pure goal-place choice from what a seat has seen (no live creature data) |

Logical parts of `InstanceEncounter.cpp` 1395-2752:

| Lines | Part |
|---|---|
| 1395-1465 | tail of `WingRoute`'s navmesh fallback (packs by `PACK_REACH`, route points every `WingWaypointYards`, cache store) |
| 1467-1752 | `FieldWingRoute` (route on the field-route graph: spine, packs, gaps, dense route, drill bands, corner tables) |
| 1754-1980 | `UpdateWingEnemies` (usable objects, closed doors, pack clearing, kills, enemy slots, crowd past the slots) |
| 1982-2004 | `PlaceParty` |
| 2006-2092 | `StartAt` (Go-Explore cell start) |
| 2094-2118 | `ClearedWords`, `MarkCell` (the cell archive) |
| 2120-2154 | `StartCorridor` |
| 2156-2186 | `UpdateDrawnPacks` (chain-pull tracking) |
| 2188-2206 | `FullClear`, `Succeeded` |
| 2208-2335 | `StartDrill` |
| 2337-2409 | `UpdateDrill` |
| 2411-2430 | `NoteDrill` (the drill's own ladder) |
| 2432-2527 | `View`, `SeenWorld` |
| 2529-2693 | `Reward` |
| 2695-2721 | `SelectTarget` |
| 2723-2732 | `TierScale` |
| 2734-2737 | `TimeIsUp` |
| 2739-2743 | `WriteState` |
| 2745-2752 | `IsTerminal` |

### E4.1 `FieldWingRoute` (`:1467-1752`)

Called from `WingRoute` (E3.10) when `FieldRoute::Covers(MapId, x, y)` is true. It builds a `WingPlan` (declared
`Encounters.h`; fields used here: `Route`, `RouteDense`, `Dense`, `Packs`, `Reachable`, `CornerAhead`, `CornerBack`,
`Field`) and returns it; the caller caches it per `{MapId, Entry}`. It never returns a partial plan: on the first
unwalkable leg of the boss spine it returns the empty plan (`plan.Field` stays false, `:1507-1508`), and the caller
logs and falls back to the navmesh route.

1. **Walk helper** (`:1478-1502`): `walk(from, stops, dense, log)` appends `FieldRoute::Plan(mapId, from, stop, leg)` legs
   to `dense`; false (with a `LOG_WARN "field route: no way from ... to ..."`) at the first leg with no path.
2. **Spine** (`:1505-1507`): the door (the seat's position) then every boss in the order the dungeon opens up (`bosses`),
   walked on the field graph.
3. **Packs** (only with `Instance.WingFullClear` = 1, `:1519-1594`): every `Hostile` creature in the map's spawn store
   other than the last boss is grouped by home position within `PACK_REACH = 15` yd of the first remaining one
   (greedy clustering, `:1530-1552`). The pack's stand point is the member nearest the spine; the pack is kept only if
   `FieldRoute::Plan(...)` succeeds from the spine point to it **and back** with a 400000 node budget (`:1566-1570`), else
   its members are counted in `left` and dropped ("a pit a seat can drop into but not climb out of"). Kept packs get
   `WingPack{At, Members}`, their spawn ids go to `plan.Reachable`, and packs are stable-sorted by the spine index
   (`along`) they hang off (`:1586-1593`).
4. **Stops** (`:1595-1621`): kept packs and all bosses but the last are merged and stable-sorted by where the spine
   passes them; the last boss is appended. `stopPack[i]` says which stop is which pack (-1 for a boss).
5. **Each pack's gap** (`:1624-1647`): the distance from the pack's members to the nearest creature home that is neither
   in the pack nor in a pack the route reaches before it (`WingPack::Gap`, `float max` if none). This is what the pull
   drill's ladder orders packs by.
6. **Dense route** (`:1649-1684`): starting at the door, `FieldRoute::Plan` from the cursor to each stop; a stop the field
   cannot reach from the last one is skipped with a count (`skipped`, `:1668-1669`); if the last boss itself is
   unreachable after the packs, the dense route falls back to the bare spine and every pack's `Yard` is zeroed
   (`:1655-1664`), so no pack is drillable. `WingPack::Yard` is the index in `dense` where a pack stop was reached.
7. **Route points** (`:1686-1704`): one point every `max(5, Instance.WingWaypointYards)` yards (default 30) along `dense`,
   `RouteDense[i]` = the dense index of route point `i`, the last boss's own position last.
8. **Drill bands log** (`:1707-1722`): per `PULL_GAPS` rung, how many drillable packs fall in it (a pack counts in the
   first rung whose gap it meets), logged `"pull drill packs by gap: ..."`.
9. **Corner tables** (`:1737-1750`): `RouteShortcut::Corners` ahead and back over `dense`, with a visibility predicate
   `clear(from, to)` that needs `|dz| <= index distance`, a VMAP line of sight at chest height 1.5 yd, ground within
   1.5 yd at every 1-yd sample (`GetHeight(..., z + 2, true, 4)`), and no liquid. Logged with the number of straight legs
   and milliseconds. See `RouteShortcut.h` (cpp-movement.md).

The plan is a **function of the map data and the creature spawns of the first instance that builds it**, cached
process-wide, so it is computed once per boss per process (E3.10). Quirk: a Wailing Caverns or Deadmines plan is built
on a worker thread inside `Build` (map thread), costs seconds (the corner pass and many A* runs), and holds
`routesLock` only for the cache store (`:1458-1461`), so two envs building at once compute it twice; UNVERIFIED how
likely (depends on `Build` ordering across envs, E3.4).

### E4.2 `UpdateWingEnemies` (`:1754-1980`), run from `UpdateEnemies`

Per decision, for the whole party. Uses seat 0's player as the visitor anchor; returns early when seat 0 is not in
the world. Steps:

1. **Fighting flag** (`:1766-1769`): any enemy slot unit alive and in combat.
2. **Usable objects and doors** (`:1770-1823`): clears `fight.Objects` and `fight.ClosedDoors`. One
   `GameObjectListSearcher` from the middle of the living seats, radius `OBJECT_SIGHT (40) + spread`. Every spawned
   closed `GAMEOBJECT_TYPE_DOOR` (`GO_STATE_READY`) is recorded with radius `max(4, GetObjectSize())` (the A8 movement
   stop, "a spline walks through one"); an object that is `Usable` (E3.11), not yet in `fight.Used`, and within 40 yd of
   some living seat goes to `fight.Objects`. Nothing opens by itself (principle 4).
3. **Route packs** (`:1825-1862`): for the first `WorldView::JOURNAL_PLACES` (8) uncleared `RoutePacks`, look their
   members up by spawn id in `GetCreatureBySpawnIdStore()`; `Resolved = Resolved || found`, `Cleared = found &&
   !standing`. So a pack none of whose members is in the store (grid not loaded) stays unknown, not cleared. Outside a
   drill it then calls `MarkCell` (the Go-Explore archive).
4. **Kills** (`:1864-1888`): each watched guid now dead moves to `Counted`, bumps `TrashKills`, `LastKillMs`, and for a
   dungeon boss or world boss `BossKills` and `BossesKilled`. (`TrashKills` therefore also counts side bosses; the last
   boss is `BossDead`, handled in `Update`.)
5. **Enemy slots** (`:1890-1950`): viewed from the tank while it lives (else seat 0), unfriendly non-player, non-totem,
   non-evading units within `WING_SIGHT = 45` yd, ranked by `RankEnemy(unit, tank)` (`Encounters.h:60`): tank's target,
   on a player, fighting, standing, gone. A fighting rank keeps the slot index it had in `env.Targets`; a standing unit
   sorts by distance. The first `PACK_SLOTS` fill `env.Targets`; the boss replaces the last slot if it is in combat
   and not in the list. Every slotted guid not yet `Counted` is added to `Watched`.
6. **Crowd past the slots** (`:1952-1980`): `fight.Overflow` (up to `CROWD_SLOTS` more, fight first), `HasAhead`/`Ahead`
   (the nearest idle unit), `AheadSize` (idle units within `PACK_REACH = 12` yd of it), `HasSecond`/`Second` (the first
   idle one further). These feed `SeatView::Crowd`, which is the encounter's own bookkeeping and is documented as
   never reaching the goal places (see E4.9). UNVERIFIED: whether `Crowd` is blanked in the stage layouts, since the
   dungeon stages have no crowd block (`Stages.cpp:604-616` comment); check `CrowdBlock` use in
   `cpp-layout-character.md`.

Perception note (principles 1 and 2): the enemy slots are filled from the server's unit list within 45 yd, **through
walls**, and viewed from the tank. They feed the pack block and the critic state; whether the policy's pack block still
carries them in the dungeon stages is a layout question (`DungeonBlocks()` includes `Pack`; `Stages.cpp:604-616` says
the pack block's slots "in a sight stage, are what the seat saw: StageScenario::ViewSeat"). UNVERIFIED: that
`ViewSeat` replaces these server-side slots with seen ones for every dungeon stage. This is the single most important
principle-1 question for a reviewer of this function.

### E4.3 Starts: `PlaceParty`, `StartAt`, `StartCorridor`, `StartDrill`

All four are called from `Build` after the route is known (E3.4; `StartDrill` at `:652`, `StartAt` at `:658`,
`StartCorridor` at `:664`). They return false when their preconditions fail (no packs, `Dense` empty, `RouteDense`
size different from `Route`), which fails the build and so triggers the scenario's retry (`SPAWN_ATTEMPTS`, cpp-
stagescenario.md).

**`PlaceParty(env, start, yard)`** (`:1982-2004`): records `EpisodeSpawn`, then for every active seat sets `Walk` and
`WaypointsPaid` to the next route point (so the route already walked is not paid), `DenseAt = yard`, and teleports it
(`BotFactory::TeleportWithinMap`; the one place seats are moved by teleport, which principle 3 allows for placement,
not movement) to `start` offset by `(index % 3 - 1, index / 3 - 1)` yards. Sets `ProgressMs = now` (the idle clock).

**`StartAt`** (`:2006-2092`), the Go-Explore start: the learner sends cells (`ExploreStart`: arena, tier, 4 words of
cleared-pack bits, yard/16, weight); `Build` copies the drawn cell into `StartPacks`, `StartYard` (`:471`, E3.4).
`StartAt` decodes the bits into pack indexes (fail if a bit is past the plan's or the route's packs), loads each such
pack's grid, marks them `Cleared`/`Resolved`, and over the spawn store: living creatures of those packs are
**despawned** (`DespawnOrUnsummon(0ms, TRASH_RESPAWN 3600 s)`) except dungeon/world bosses, which `KillSelf(false)` so
the boss script opens what its death opens (`:2058-2064`); `HostileTotal` is decremented for despawned creatures that
were counted. The party goes to `StartYard * 16` on the dense route, walked **back** along it until no living hostile
is within `PULL_START_CLEARANCE = 25` yd, at most `PULL_START_MAX_YARDS = 120` yd. `RouteNext` becomes the first
route point past that yard; a mark for the start is pushed (the archive counts the visit).

**`ClearedWords` / `MarkCell`** (`:2094-2118`): the cleared flags of the first `EXPLORE_PACKS` (96) route packs as 4
words of 24 bits (24 so each word survives a float32 round trip: `Encounters.h:150`). `MarkCell` is called every decision
in a non-drill run; it records `(words, yard / 16, elapsed)` whenever the set of cleared packs changes and is not
empty, keeping the last `EXPLORE_MARKS` = 8 per run. The yard is the **furthest** yard any seat stands at. The marks
leave the env through the episode info / protocol (E3.8 and [protocol.md](protocol.md)); the learner's archive is
`apps/forge/python/animus/explore.py` (py-learner.md). UNVERIFIED: the exact wire path of `Marks`.

**`StartCorridor`** (`:2120-2154`): `length = CorridorPacks`; `packs = min(plan.Packs, RoutePacks, 96)`. First pack =
`WingRun::CorridorFirst(packs, length, evaluating, EpisodeSeedIndex, rand32())`: starts = `packs - length + 1`
(1 if the route is shorter), an evaluation picks `SeededPick(starts, seed)` (a multiplicative hash,
`(seed + 1) * 2654435761`, so the same seed gives the same corridor every evaluation), training `roll % starts`. If the
first pack is not 0, the packs before it are cleared by `StartAt` using `StartPacks` made of bits `0..first-1` and the
party stands at pack `first`'s yard; then `Marks`, `StartPacks`, `StartYard` are reset (a corridor start is no
Go-Explore cell). `fight.Corridor.Begin(first, min(packs, first + length))`. Logged with `WingTrace`.

**`StartDrill`** (`:2208-2335`): the pull drill (D1). Picks the ladder rung: an evaluation uses the last rung
(`PULL_GAPS.size()-1`, any pack); training uses `_drillRung` under `_drillLock`. The candidate packs are those with a
non-zero `Yard` inside `Dense` and `Gap >= PULL_GAPS[rung]` (`{30, 22, 14, 0}` yd, `:119`); if none, any drillable
pack. An evaluation takes `SweepPick(open.size(), seed)` (seed mod count, so every pack in turn), training `urand`.
The packs before the chosen one are despawned (their grids loaded first so absent creatures do not stand up behind the
party) and marked cleared; the chosen pack's members are `DrillPack`; every other standing creature gets a
`DrillGroups` entry (which later pack of the plan it belongs to, else a unique group), so a second pack can be named
whole. The party starts `Instance.PullStartYards` (35) back along the route from the pack and further, up to 120 yd,
until nothing standing is within 25 yd. `DrillPoint` is the first route point at or past the pack's yard. Fails if
`DrillPack` is empty.

### E4.4 `UpdateDrill` and the drill's ladder (`:2337-2430`)

`UpdateDrill` (called from `Update` when `fight.Drill`, `:753`): the anchor is the tank if alive, else the last
living seat. `DrillEngaged` latches when a drill-pack creature is in combat. For every living seat, units within
`DRILL_SIGHT = 50` yd that are alive creatures, not summons, totems or pets, whose victim belongs to a player, are "on
the party". The **first** creature on the party locks the pack (`DrillLocked`): if it is not in `DrillPack` then the
pull is "another pack" (`DrillOther`), `DrillPack` is replaced by that creature plus its `DrillGroups` mates, and the
drill is judged on that pack instead. After the lock, any on-party creature outside `DrillPack` sets `DrillExtra`
(and `DrillExtraEntry`). `DrillPeak` = most creatures on the party at once. `DrillCleared` latches when no drill-pack
creature is alive, the party is not fighting and nothing is on it.

`NoteDrill(rung, clean)` (`:2411`), called from `ResetEpisode` for a training drill (E3.4 at `:417`): a deque of the last
`Instance.PullRungRuns` (100) outcomes (clean = cleared and not extra), ignored unless `rung == _drillRung`; when the
window is full and the clean share >= `Instance.PullRungTarget` (0.7) the rung steps up (`++_drillRung`, window cleared,
logged "the pull drill steps to rung N"). It never steps back and stops at the last rung (`PULL_GAPS.size() - 1` = 3).
**This ladder is per process** ("each machine climbs its own", `CurriculumTuning.h:267`): it is **not** shared with the
cluster the way the wing ladder is, and there is no alarm of any kind for it. `_drillRung` starts at
`Instance.PullRungStart`. A resumed run restarts it at that key.

### E4.5 `UpdateDrawnPacks`, `FullClear`, `Succeeded` (`:2156-2206`)

`UpdateDrawnPacks` (called each decision for a non-drill run, `:755`): when not fighting, `fight.Drawn.End()`. Otherwise
the route packs of every creature in the slots and in `Overflow` that is alive, in combat and whose **victim is a
player or a player's pet/charm** (`GetCharmerOrOwnerPlayerOrPlayerItself`), via `PackOf[spawnId]`, are passed to
`WingRun::FightPacks::Note`, which counts a pack joining a fight that already had a pack as one **chain pull**
(`fight.ChainPulls += ...`). `FullClear`: the last boss dead and either every route pack `Cleared` (field route) or, on
a navmesh route with no packs, `TrashKills + 1 >= HostileTotal`. `Succeeded`: drill = cleared and not extra; corridor =
`Corridor.Done()`; else `BossDead`. (`Succeeded` has no caller inside this file's range; UNVERIFIED whether the first
half or `StageScenario` calls it: grep before relying on it.)

### E4.6 `View` and `SeenWorld` (`:2432-2527`) and `SeenPlaces.h`

`View` (only when the route is known) copies the seat's food and drink item ids into the view and calls `SeenWorld`.
`SeenWorld` fills `view.World.Places` (8 slots) and the assignment from `SeenPlaces::Choose`, and sets
`world.RoutePlaces = true`. Inputs:

- **Memory**: the seat's `Recall.Entries()` (entity memory: last seen position, reaction < 0 = hostile, dead flag,
  gameobject flag).
- **Frontier**: `SeenPlaces::Frontier(seatPos, 40 yd, 2 yd step, ROAM_PLACES = 6 points, probe)` over the seat's own
  `MentalMap`, cached per seat and refreshed every 2000 ms of episode time (`FRONTIER_MS`). A cell is `Unknown` if
  absent or not `Vision::Known`, `Shut` with a wall-low/high or hazard flag, `Open` if it has a floor or is
  free/visited.
- **Layout** (only `GoalPlaces == SeenAndLayout` and `fight.MapLayout` non-empty): the dungeon map's nodes, with a per-
  node "explored" flag from the seat's map. Today `MapLayout` is built in `Build` (`:645`) by `SeenPlaces::Layout(ground,
  LAYOUT_SPACING)` from the route's ground, so, as `Stages.cpp` and the docs say, it follows the boss route and is off
  by default (`SeenOnly`).
- **Leader**: `WingRun::LeaderSeat(standInSeat, standInLeads, tankSeat)`; present if it is another seat, alive, in the
  same map. Position is the leader's exact server position (its frame and map dot show it to a player).

`SeenPlaces::Choose` (pure, `SeenPlaces.h:115-175`): place slots 0..2 (`MEMORY_PLACES`) are remembered **living
hostile non-gameobject** entries sorted nearest first and de-duplicated within `SAME_PLACE = 10` yd; slots up to 5
(`ROAM_PLACES`) are then the frontier points and unexplored layout nodes nearest first, de-duplicated; slot 6
(`WAY_ON`) is the single nearest frontier/layout point (even if already used as a roam place); slot 7 (`LEADER`) is the
leader. The assignment is the first present of slot 0, `WAY_ON`, `LEADER`. The `static_assert(sizeof(Point) ==
3 * sizeof(float))` is the code's guard that a place carries no creature data (`:42`). A remembered hostile that has
died but whose death the seat has not seen is still a place; a dead-as-last-seen entry is skipped. `Layout()` sorts the
ground by position and keeps nodes at least `spacing` apart, so nothing of the walk order survives. Tested by
`DungeonStagesTest.AnUnseenPackOrBossNeverReachesTheGoalPlaces`, `TheLayoutHoldsNoCreatureDataAndNoOrder`,
`TheFrontierIsOpenGroundBesideTheUnseen`.

Quirk: `SeenWorld` writes `own.Frontier`, `FrontierMs`, `FrontierReady` through a `const` env reference (the members
are evidently `mutable`); observation of seat `s` therefore mutates state, from the map thread that observes it. The
leader's position is also read from other seats' player objects from within one seat's observation, on the same map.
UNVERIFIED: that every seat of one env is observed on one thread (it should be: one env, one map).

### E4.7 `Reward` (`:2529-2693`): the exact terms

Called per seat per decision (after `BeforeRewards`). `tierScale = TierScale(env)` (E4.8). `seconds = DecisionMs / 1000`.
Kinds are from `RewardTermCategory` (`Rewards/RewardLedger.h:162-250`). `ledger.Add(term, value, tier)` multiplies by the
tier argument and, for Shaping, by the fade scale; the **score** takes `value` without the tier. A cost "over the tier
scale" is passed `1 / tierScale` so it shrinks as the rung climbs.

Paid every decision to every seat (`:2538-2566`):

| Term (kind) | Condition and amount | Key (default) |
|---|---|---|
| `Idle` (Cost) | `EpisodeElapsedMs > ProgressMs + grace`, where the grace is `PullGraceMs` for a drill, else `WingStallGraceMs`: `-WingStall * (tank ? 1 : WingStallOthers) * seconds`. `ProgressMs` is bumped by seat 0 when `TrashKills + waypoints` changed or the env's `StepEngaged` is true | `Instance.WingStall` 0.1, `WingStallGraceMs` 60000, `WingStallOthers` 0.2, `PullGraceMs` 20000 |
| `StepCost` (Cost) | `-WingClock * seconds` | `Instance.WingClock` 0.002 |
| `ReadyPull` (Outcome) | for each new fight started with every living member ready (`fight.ReadyEngages`, capped by `ReadyPaidCap`, counted in the first half): `+WingEngage * newEngages`, tier `tierScale` | `Instance.WingEngage` 1.0 (the ready share is `WingReadyShare` 0.8, read in `Update`) |
| `PullExtra` (Cost) | each new chain pull: `-WingChainPull * new`, tier `1/tierScale` | `Instance.WingChainPull` 3.0 |
| `Clear` (Outcome) | corridor run, each pack cleared **in route order** since last paid: `+CorridorPack * new`, tier `tierScale` | `Instance.CorridorPack` 4.0 |
| `Threat` (Shaping) | `OnParty > WingCrowdFree`: `-WingCrowd * (OnParty - WingCrowdFree) * seconds` | `Instance.WingCrowd` 0.15, `WingCrowdFree` 4 |
| `Lost` (Cost) | `WingRun::Strays(alive, isLeader, walkingBack, leaderAlive, yards, WingStrayYards)`: `-WingStray * seconds` | `Instance.WingStray` 0.02, `WingStrayYards` 25 |
| `Away` (Cost) | `WingRun::Away(alive, walkingBack)` = dead or `Clock.Rejoining`: `-WingAway * seconds` | `Instance.WingAway` 0.02 |

Paid only to a living bot (`:2568-2592`): `Kill` (Outcome) `+WingTrashKill * newTrashKills`, tier `tierScale`; `Approach`
(Shaping) `+WingWaypoint * tierScale * newWaypoints`; `Kill` `+WingMidBoss * newBossKills`, tier `tierScale`; and the
forward potential: when not fighting and the route ahead is known, `potential = -(distance to next route point +
RouteRemain[next]) / RouteRemain[0]`, paid `+WingProgress * tierScale * (potential - previous)` when it rises (the max
is kept; the first reading only seeds it). Keys: `WingTrashKill` 1.0, `WingWaypoint` 0.5, `WingMidBoss` 8.0,
`WingProgress` 60.0. Note the tier is multiplied into a Shaping term here as an argument, not as an outcome scale.

Deaths (`:2599-2610`): the first decision a bot is found dead, `Death` (Cost) `-WingDeath` (3.0) tier `1/tierScale`; for
each new wipe `Death` `-WingWipe` (5.0) times the number, tier `1/tierScale`. Both `paid.DeathPaid` and `Deaths` reset
how E3.7 describes.

**Terminal outcome**, paid once per seat when `over` (`BossDead || Wiped || TimeIsUp || drill done || corridor done`,
`:2612-2614`), guarded by `OutcomePaid`:

- Drill: share = 1 for the tank (`DungeonRole == DUNGEON_TANK`), else `PullOthers` (0.5). Extra pack: `PullExtra`
  `-PullExtra * share`, tier `1/tierScale`; else cleared: `PullClean` (Outcome) `+PullClean * share`, tier `tierScale`;
  else time up: `Timeout` (Cost) `-PullTimeout * share`, tier `1/tierScale`. Then returns. Keys `PullExtra` 5.0,
  `PullClean` 5.0, `PullTimeout` 2.0, `PullOthers` 0.5.
- Seat 0 only, once: a `LOG_DEBUG` of route packs never found (`:2632-2643`).
- Corridor: time up and not done: `Timeout` `-WingTimeout * (1 - Corridor.Share())`, tier `1/tierScale`; returns unless
  `BossDead` (a corridor can end on the last boss too).
- `BossDead`: `Kill` `+WingBoss`, tier `tierScale`; and for a whole run (not corridor) with `FullClear` once:
  `Clear` `+WingClear`, tier `tierScale`. Keys `WingBoss` 25.0, `WingClear` 25.0.
- Else time up with a route: `Timeout` `-WingTimeout * (1 - waypoints / Route.size())`, tier `1/tierScale`;
  `WingTimeout` 30.0.

A **wipe ends nothing here** (the wipe latch and its counting are in the first half, `Update`); the run ends when
`fight.Wiped` is set by exceeding the allowed wipes (`Instance.WingWipes` 2 plus the rung's `ExtraWipes`, E3.4).

### E4.8 `SelectTarget`, `TierScale`, `TimeIsUp`, `WriteState`, `IsTerminal` (`:2695-2752`)

- `SelectTarget`: returns the slot the seat has selected if alive; else the nearest living enemy of the slots, with
  slot 0 (the boss while it fights) winning. **Never reached in the dungeon stages**, whose layout carries the sight
  block (the scenario uses the seat's own client selection; cpp-stagescenario.md, `StageScenario.cpp:3062`). It is
  vestigial for the live stages. Always returns true.
- `TierScale`: `tier = Drill ? DrillRung : WingRun::TierOfRung(Rung)` (rung / 2, so rungs 0-1 tier 0 ... rung 8 tier
  4); then `CombatReward::TierScale(Difficulty.TierScale, min(tier, Instance.MaxTierScale))` (`MaxTierScale` default 6,
  never binding for tiers 0-4). The formula is in cpp-rewards-routing.md; the header says `1 + Difficulty.TierScale *
  tier` (`WingRun.h:32`). A training run's tier is the **rung it was drawn on** (`fight.Rung`), not the ladder's current
  one.
- `TimeIsUp`: `EpisodeLengthMs && Elapsed >= Length`.
- `WriteState`: `state[STATE_TIER] = fight.Tier / (max(2, Rows.size()) - 1)`. `Tier` here is the boss-row index, not the
  ladder rung (E3.4).
- `IsTerminal`: `BossDead || Wiped || TimeIsUp || drill cleared/extra || corridor done`. The same expression as `over`
  in `Reward`; the two copies must agree.

### E4.9 `WingRun.h` (pure helpers; tests: `DungeonStagesTest.cpp`)

- `RUNGS_PER_TIER = 2`; `TierOfRung(rung) = rung / 2`.
- `LeaderSeat(standInSeat, standInLeads, tankSeat)`: the stand-in's seat when it leads and exists, else the tank's.
- `Strays(alive, isLeader, walkingBack, leaderAlive, yards, strayYards)`: alive, not leader, not walking back, leader
  alive, `yards > strayYards`. `Away(alive, walkingBack)`: `!alive || walkingBack`. Their disjointness (the same seconds
  never charged twice) is tested by `ARisenSeatWalkingBackIsAwayNotLost` and `LostIsPricedFromTheActualLeader`.
- `SeededPick(count, seed)`, `SweepPick(count, seed)`, `CorridorFirst(...)` as in E4.3.
- `Corridor` (`First`, `End`, `Next`, `InOrder`, `OutOfOrder`, `Noted`): `Note(cleared)` notes each newly cleared pack
  of `[First, End)`, in order if it equals `Next`; `Done()` when `Next >= End`; `Share()` = cleared / length. A pack
  cleared out of order is counted in `OutOfOrder` and never pays `Clear` (only `InOrder` is paid). Note `Done()` needs
  every pack noted, but `Next` skips ahead over out-of-order ones, so a corridor can be `Done` with `InOrder <
  Length`.
- `FightPacks::Note(fighting)`: counts packs joining a fight already holding one. Tested by
  `AChainPullIsAPackJoiningAnotherPacksFight`.
