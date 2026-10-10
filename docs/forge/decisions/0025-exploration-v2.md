# 0025: Exploration v2 pays for going in, lasts the whole search, and charges stale ground

**Date:** 2026-10-10 (owner order from the measured diagnosis of the explore-unstuck run, `var/m2-failures/new-run/explore.md`)

**Decision.** In the seek stage (M2), all behind `Seek.*` keys (defaults in `worldserver.conf.dist`):

1. `Seek.ExploreCap` 1.0 -> 5.0 (nominal, before the scale). At the 0.5 floor a full episode banks at most 2.5, under
   `Arrive` 3.0.
2. Pay for going in. A cell first seen while the seat's own position is inside a room polygon of the table, the cell
   being in that same room, is worth `Seek.ExploreInsideBonus` (3.0) instead of the cell weight 1 (and instead of
   `Seek.ExploreRoomBonus` 2.0, which stays for a cell of a room not yet entered seen from anywhere else: looking in).
   `Seek.RoomEntry` (0.1 nominal, `Exploring`, paid x the floor too) pays the first entry of each table room once an
   episode (`Seek.EnterDwellMs` continuous inside the polygon), x `Seek.RoomEntryBackMult` (2.0) for a back or end-back
   room (`_back` suffix or `_end_` in the name, as `back_room_visits`), in its own cap `Seek.RoomEntryCap` (2.0). The
   room the seat started in is never paid.
3. `Seek.FrontierPull` 0.004 -> 0.015 a yard, `Seek.FrontierCap` 1.0 -> 2.0, and only toward a frontier that lies in a
   room polygon of the table or within 3 yd of a room's opening: a frontier in the corridor does not pull. The nearest
   eligible one of the seat's mental map is the target.
4. `Seek.Stale` (Cost, `AddFixed`, never faded, in the score), 0.005 a second (halved from 0.01 at the merge: a fully stale 400 s would otherwise cost 4.0, more than Arrive) once `Seek.StaleMs` (20000) pass with no
   newly seen floor cell of the episode's record; not in the trap drill's escape window, not on a decision that paid
   `Stuck` or `Wall`.
5. Both sweeps play the deep rung's clock (`Seek.RungSeconds3`, 420 s); `found_300` keeps the old numbers comparable.
6. A second held-out set, `sweep_rotating`, plays the frozen sweep's placements on seed indexes shifted each
   evaluation; the frozen `sweep` is unchanged for the history.

**Why.** The explore-unstuck run (10-80M) fixed the local faults (circling, spin, the frozen seat) but not where the bots
go: a failed search enters about 3 of 39 rooms, back rooms 0.1-0.2%, with 4-5 full hub-spine traversals. The measured
cause: the exploration reward was a one-off 0.5 collected in about 13 s (96% of learner and 92% of sweep episodes hit
the 1.0 cap; 77% of the pre-cap pay was hallway floor and none came from back or end rooms; after the cap 81-82% of what
a failed search saw paid nothing); the room bonus was paid for cells seen from the corridor, so it rewarded looking
into a room and nothing extra for entering it; `FrontierPull` at 0.004 a yard banked a median 0.12 nominal; and the hub
ping-pong has a large net displacement, so the `Circling` cost never fires. A whole-map sweep is worth 10.5 nominal;
observed episodes bank 3.9 on average, 7.1 at p95. The 300 s sweep and 420 s learner sets were not on one clock, and the
frozen sweep gives the same start the same pair at every evaluation.

**Consistency with 0023.** The exploration floor rule is unchanged: `Explore`, `FrontierPull` and the new `RoomEntry` are
`Exploring` (paid x `max(shaping scale, Seek.ExploreFloor)`), not in the score; every cap is per episode and nominal;
`Stale` joins `Circling` as a fixed-price Cost the fade does not take. Principle 9's exception stays the one owner-ordered
exception. Not touched: the observation, the action set, the trap drill.

**Prices against `Arrive` 3.0.** Per term: `Explore` 2.5 paid at most, `RoomEntry` 1.0, `FrontierPull` 1.0, `Stale` 0.005 a
second. Together 4.5 is the ceiling at the floor, reachable only by a near-complete tour of the map; the reward audit
(`rewards.py`) will warn on `Explore` alone (it is over 0.5 of `Arrive` by design). If a policy tours past the object,
the caps are the first thing to lower.

**What it constrains / risks.** `RoomEntry` is once a room an episode behind a dwell, so entering and leaving pays once;
`ExploreInsideBonus` pays each cell once, and only in the room the seat stands in, so a spin inside a room pays at most
the room's floor. A seat cannot gain by oscillating: the cells are gone, the entries are gone, the frontier ratchet only
falls, and `Stale` charges the quiet. Measure: `room_entries_new` (target several times the 3 of the diagnosis),
`back_room_entries`, `explore_cap_hit_ms` (should move out from 13 s), `stale_seconds`, hub switches per minute.

See also [0023](0023-exploration-floor.md), [0009](0009-outcome-versus-shaping-and-the-fade.md) and the
[index](README.md).
