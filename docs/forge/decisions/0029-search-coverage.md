# 0029: A room is searched by what was seen of it; re-searching costs, the cell choice follows the searched layer

**Date:** 2026-10-11 (owner: "Make the code changes, rebuild, and bring in the whole cluster"). Follows 0028 (the
configuration-only retune), whose four evaluations (160.4M-175.5M) moved sighting (misses that ever saw the object
24% -> 38%, finds 72 s -> 63 s) but not the search itself: ~2 rooms entered, revisit share ~0.5, found ~72%.

**Why code.** Three things no setting could reach:

1. *"Seen" and "checked" were glimpse and dwell.* RoomSeen paid on RoomSeenRays rays of one frame and a room was
   checked after a 1 s dwell, so a corridor glance and a run through a doorway counted as searching it. The per-room
   floor coverage (`RoomTrack::Hit` / `Floor`) was already kept and paid nothing. RoomSeen was also Shaping, at a
   shaping scale of ~0.16 by 150M: a coverage reward paid as Shaping would have paid almost nothing.
2. *Revisit priced the wrong ground.* It charged every 2-yd cell stood on again, so the corridors a search must recross
   cost the same as going back over a searched room; 0028's 0.01 cost a miss only ~-0.25.
3. *The cell head had no reason to read the searched layer.* A choice was stale only if the seat had stood on it,
   and CellGoal (Aid) had faded to 0 by 175M (`reward_cell_goal` mean 0.0, `reward_cell_stale` -0.035): the ablations
   showed it (`random_cell` -2 and `no_searched` -4 points at 135.4M; -5 / -5 at 155.4M).

**Change** (build defaults, `CurriculumTuning.h` = `worldserver.conf.dist`):

- `Seek.RoomSeenShare` 0.6 (new): RoomSeen pays once a room's floor is 60% seen this episode (0 = the old ray rule);
  `RoomSeen` 0.1 -> 0.3 and its category Shaping -> **Exploring** (paid at max(shaping, ExploreFloor 0.5)).
  `rooms_looked` counts these.
- `Seek.CheckedByVisit` 0 (new): checked only by `CheckedShare` (0.6) of the floor seen, never by a dwell alone.
  `rooms_checked` is not comparable across this change.
- `Seek.Revisit` 0.002 -> 0.03 is the price inside a checked room; `Seek.RevisitCorridor` 0.01 (new) elsewhere (a
  negative value is the old everywhere rule); `RevisitAgeMs` 60000 -> 30000.
- `Seek.StaleSearched` 1 (new): a chosen block is stale when stood on OR wholly searched (every floor cell's
  CROP_SEARCHED at SEARCHED_MAX, none frontier: `CellGrid::Searched`). `CellStale` 0.01 -> 0.05.
- `CellGoal` 0.05 -> 0.1 and its category Aid -> **Exploring**: reaching a chosen block that was not stale pays.

No block, observation, action or shape changes: a run resumes from its checkpoint (`RESUME_SPEC_KEYS` are shapes);
`stage.json` differs in `reward_terms` categories and `tuning` only. The learner's score reads Outcome and Cost
categories, so the two category moves do not touch it. 0028's conf lines stay except `Revisit`, set to 0.03 in
the machine confs so it does not pin the in-room price at 0.01; `RoomSeenRays` 12 is now dead (harmless).

**Read it by.** `revisit_share`, `rooms_entered`, the share of misses that ever saw the object, `found`,
`reward_room_seen` / `reward_cell_goal` (both should now be non-zero), `look_entropy` and `sighted`; the real test
is the ablation gaps at the arms evaluations (every 4th): `no_searched` and `random_cell` should cost clearly more
than 5 points.

**Revert.** Without a rebuild, by keys in every conf (conf-sync, restart the worldservers): RoomSeenShare 0,
CheckedByVisit 1, RevisitCorridor -1 with Revisit 0.01, StaleSearched 0. The two category moves need a rebuild to
undo. Revert if `found` drops below ~0.5 and has not recovered by the second evaluation.
