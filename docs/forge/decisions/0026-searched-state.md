# 0026: The seat sees what it has searched this episode; stale means no new room; the trap and the wall pin cost more

**Date:** 2026-10-10 (owner order: "make all the changes and then restart stage 2 fresh"; design in
`.agents/plans/explore-v3/CONTRACT.md`, the lead's answers at its end)

**Decision.** Exploration v3, for the seek stage (M2) and the sim it runs in. The learner-side half (greedy rollout
share, 312-episode evaluation, traces for every policy, `no_searched` arm, the cell head's three new features) is the
Python stream's and is recorded in the learner docs; this file is the sim half and the reasons.

1. **A searched-state input.** The mental-map crop gets a seventh channel, `searched` (0..4: how many of a crop cell's
   four 1-yd cells were looked at THIS episode), and the map block (revision 2) three scalars: `searched` (the share of
   the window looked at), `new_age` (seconds since the body last stood on ground it had never stood on, over 120 s) and
   `total` (cells it has stood on this episode, over 3000). Protocol 30. `MentalMap` keeps an episode epoch: a look
   counts when its stamp is at least the epoch, so a map kept from the episode before (50% of training episodes) does
   not read as searched; the kept path advances at least one second so the 1 s stamp resolution cannot leak the last
   look of the old episode into the new one. The record is the seat's own, computed from its own map: no read of any
   encounter state (principles 1 and 2). It is not a goal-block journal: the room table and `EnvSeek::Seen` are
   training bookkeeping the shipped bot does not have.
   *Why:* at 110M steps the policy found ~0.92 but entered ~2.8 rooms, the hub ping-pong was unchanged, and the planner
   was no better than random (`random_cell` 0.92 = normal 0.92). The crop's `known`/`age`/`visited` already say "seen
   at some time"; the per-cell flag lines up with what the cell pointer chooses between (a 4-yd block), and the
   scalars are the one window-independent progress record (the crop spans 96 yd, the Stockades' spine about 300).
2. **Stale means no new room.** `Seek.StaleMs` (20 s without a newly seen floor cell) is deleted, `Seek.StaleRoomMs`
   (60 s) added: the clock restarts when any table room is first visited (the start room included, whether or not
   `RoomEntry` still pays). v2's detector never fired (the bot sees ~1,600 new cells an episode). It resets on room
   entries only, not on "opening approached": 78-83% of failed searches already come within 6 yd of the front doors,
   so an approach reset would blunt it exactly on the hub ping-pong (`rooms_approached` is logged as a reading).
   `Seek.Stale` stays 0.005 a second; worst case (420 - 60) s x 0.005 = 1.8 against `Arrive` 3.0.
3. **`RoomEntry` 0.3** (was 0.1; cap stays 2.0, back rooms x2). At the 0.5 floor the paid ceiling is Explore 2.5 +
   RoomEntry 1.0 + FrontierPull 1.0 = 4.5 against `Arrive` 3.0, as in 0025 (the lead kept the cap at 2.0; a cap of 3.0
   would have made 5.0).
4. **`Seek.ExploreFromRung` (2).** Explore, FrontierPull, RoomEntry (the payment), Stale and the trap drill apply only
   from the placed rung up. Below it (the hallway and the doorway) the object is in sight of the spawn; the Exploring
   scale is `max(shaping, 0.5)`, so at the early rungs of the restored ladder (x1.0, x0.5) the family would pay for
   lingering. A documented exception to the exception of 0023/0025 (principle 9). 0 restores v2.
5. **Hard starts.** `Seek.HardRoomWeight` (3.0; 1 = off) weighs nine rooms that always failed (`SeekDraw::HARD_ROOMS`)
   in the unseeded room draw at the room and deep rungs only. Seeded draws (the sweeps, the learner evaluation) are
   untouched, which is the proof they are unaffected. The hard share of draws is 9/19 = 0.474 at either rung (a "0.4
   uniform floor" would need weight 4.5). A start distribution, not an observation or a teacher.
6. **The trap drill is harder, and its escape stricter.** `TrapShare` 0.2; the pose lies `TrapGapNear`-`TrapGapFar`
   (0.45-0.8) yd centre to wall (the capsule's radius is 0.389 yd, so the plan's 0.3 would start it in the wall), facing
   the jamb within `TrapFacingSlack` 0.3 rad; `Escape` also needs the body to have turned `TrapEscapeTurnDeg` (90) from
   the pose heading inside the window. A seat can leave a jamb by strafing along the wall; that is a legal escape this
   payment does not reward (priced by Wall, Stuck, Stale and StepCost, never masked: principles 5 and 14);
   `trap_turned` shows how many escapes were turns. If most are strafes, set the key to 0.
7. **The Wall charge escalates.** A decision's Wall charge (Seek only; `Standing::WallCharge` is pure and unchanged) is
   multiplied by `min(WallEscalateMax, 1 + t / WallEscalateSeconds)` (4 s, cap x4), t the seconds of contiguous
   Wall-charged decisions before it (plus half the decision). The run resets on a decision whose charge is 0, not when
   contact ends: a corner slide that makes progress is discounted to 0 by `WallCharge`, so it cannot build a
   multiplier. `Stuck` is not escalated (the two co-occur in a pin; escalating one keeps the sum legible, and Stuck
   also covers non-wall stalls). A 5 s pin costs 0.16, 12 s 0.6, 20 s 1.24. Applies at every rung.

**Prices.** No new reward term. `Stale` and `Wall` are the existing Cost terms, `RoomEntry` the existing Exploring term;
`RewardLedger.h` and the reward audit are untouched. Telemetry (episode info, sim half): `stale_clock_end_seconds`,
`hard_room`, `rooms_approached`, `wall_pin_max_seconds`, `wall_pin_events`, `wall_extra_charge`, `trap_turned`,
`trap_pin_seconds`; `stale_seconds` and `stale_events` keep their names with the new meaning.

**What it constrains / risks.** (a) Reward hacking: the Exploring ceiling stays 4.5 at the floor; the first thing to
lower is `RoomEntryCap`. (b) The escalated Wall charge could make a seat avoid walls so strongly it will not thread a
3 yd doorway: watch door-room entries and `wall_seconds` (cap x4 and the progress-based reset are the guard). (c)
`HardRoomWeight` over-trains nine rooms; the uniform sweeps are the check. (d) `visited`, `known` and `age` stay
cross-episode in kept-map training episodes while `searched` is episode-local; the policy can learn to trust it over
them. (e) Shipping: the exported map input grows by a channel and three scalars; mod-animus ships six-channel models and
no realm model exists for this stage. (f) `new_age` and `total` under-count in kept-map episodes (the visited cells of
the episode before are not new ground).

See also [0019](0019-vision-only-movement.md), [0023](0023-exploration-floor.md), [0025](0025-exploration-v2.md) and the
[index](README.md).
