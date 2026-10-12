# 0030: Systematic search -- the object is in the last place you look

**Date:** 2026-10-11 (owner: "i really want to teach it to search systematically" -> "write up the plan and then build
it"). Plan: `.agents/plans/systematic-search/systematic-search.PLAN.md`. Follows 0028 and 0029.

**Why.** With the object placed at the episode's start, a lucky glance pays exactly what a careful sweep pays, so a
policy that searches at random finds it about as often as one that sweeps. The run says that is what it learned:
`random_cell` cost 2-5 points and `no_searched` 4-5 (135.4M-175.5M), about half of every episode was on ground already
covered, ~2 rooms entered. Side rewards (0028, 0029) moved sighting and speed, not the search.

**Decision.** In a share of training episodes the object is placed late, out of sight, in the last room the seat has
not searched -- so the find comes only after the search, the time to find is the time to cover, the order of the
rooms matters and forgetting a room costs searching it again. Arrive pays it; no new reward term. Nothing tells the
seat how to search (no script): it only takes the luck away.

- `Seek.DeferShare` 0.4: share of the training episodes (unseeded; never the sweep, the trap drill or a seeded
  episode, so evaluation and the ladder's `found` are unchanged) placed at `Seek.DeferFromRung` (2, the room rung) or
  above that defer.
- Candidates: the placed rung's rooms (`Draw::RungRooms`); fewer than 2, no deferral. Searched: `RoomTrack::Checked`
  (0029: `CheckedShare` 0.6 of the floor seen).
- `Seek.DeferLeft` 1: placed once that many candidates or fewer are unsearched; `Seek.DeferLatestShare` 0.75: by that
  share of the clock it is placed anyway, in the least covered unsearched candidate (`defer_forced` says so; 0.75,
  not 0.6, so waiting for the deadline leaves too little clock to pay).
- Where: `Place` with `hideFrom` -- a floor point passing the usual checks whose cover cell this episode's rays never
  hit and out of the seat's line of sight, else any point out of sight, else the usual draw.
- `Seek.DeferSeconds` 420: a deferred episode's clock (covering takes longer than the room rung's 200 s).
- Until placed the episode runs as any search: step cost, the exploring terms, room tracking, Revisit, and the goals
  are offered (`WorldView::RoomGoals` reads `Placed || Deferred`: from the inside it looks like any search, the
  object not yet in view); no objective (`HasObjective`), no critic slot, no found check.

**Measures** (episode info, learner conditionals in `episode_means.py`): `deferred`, `defer_placed`, `defer_placed_s`
and `defer_forced` (over the placed), `found_deferred` (over the deferred), and every episode `checked_share` (the
placed rung's rooms checked), `half_checked` / `half_checked_s`, `searched_revisit_s` (Revisit's weighed seconds in
rooms already checked: wasted travel), `yards_per_room_checked`. The trace records the map block's `searched_cells`
per decision and the route viewer draws it as a coverage curve beside the replay.

New episode-info columns change `metrics.csv`'s header: the learner rotates it (`metrics-before-<stamp>.csv`), so
the before/after split is the resume update, recorded in the audit summary.

**Judge it by** `found_deferred` climbing (training), `defer_placed_s` falling and `defer_forced` low (a seat waiting
for the deadline shows as forced placements at 0.75 of the clock); in evaluation `searched_revisit_s` and
`revisit_share` down, `checked_share` up, `found` held; the ablations (every 4th evaluation) `no_searched` and
`random_cell` costing clearly more than 5 points (target > 10) within ~40M steps.

**Revert.** `Seek.DeferShare 0` in every conf (conf-sync, restart the worldservers); no rebuild. If `found_deferred`
stays ~0 after ~20M steps, raise `DeferLeft` to 2 first.
