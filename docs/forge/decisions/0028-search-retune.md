# 0028: Search retune at rung 2: going in pays, glimpsing and re-running do not

**Date:** 2026-10-11 (owner: "make the changes yourself and resume the run"). Configuration only: seven
`AnimusForge.Curriculum.Seek.*` lines in each machine's `mod_animus_forge.conf` (host, then `forgectl conf-sync`); the
build defaults in `CurriculumTuning.h` and `worldserver.conf.dist` are unchanged, so no rebuild. Applied by cancel,
restarting every worldserver (the cluster fingerprint hashes the curriculum values at startup), and resume at update
7865 (157,777,920 env steps); the before/after split in `metrics.csv` is that update.

**Why.** The ladder stepped to rung 2 (objects inside rooms) at ~120M; at 150.4M the evaluation found 69% and the
misses showed a corridor search, not a room search:

- Missed Stockade episodes looked into ~22 rooms (a frame with `RoomSeenRays` 3 rays on a room's floor) but entered 3
  distinct ones; 76% of misses (72% on Ragefire) never had the object in view; 56-58% of their time was on ground
  stood on before; wall time 4-6x the found episodes'.
- Explore paid the misses +2.5 (at the cap in 80% of them) plus frontier pull +0.55 and room seen +0.35: more than the
  find's Arrive 3 net of nothing, while Revisit charged -0.04. Exploring is off below rung 2 (`ExploreFromRung` 2,
  0027's doorway-plateau amendment), so the corridor sweep was paid from the step to rung 2 on.
- The ablations at 135.4M: `no_searched` -4 points and `random_cell` -2 against 60%: the searched layer and the
  cell choice are not used; `no_map`, `no_plan` -24, `no_coverage` -12.

**Change.**

| Key | Was | Now | Meant to |
|---|---|---|---|
| `Seek.ExploreRoomBonus` | 2.0 | 0.5 | a room seen from the corridor pays less than plain floor (`ExploreInsideBonus` 3 kept: going in pays) |
| `Seek.ExploreCap` | 5.0 | 2.0 | Explore stays well under Arrive (1.0 at the 0.5 floor) |
| `Seek.Revisit` | 0.002 | 0.01 | old ground costs: ~-0.8 on a typical rung-2 miss (200 s clock, ~55% revisit) |
| `Seek.RevisitAgeMs` | 60000 | 30000 | running back over a corridor within the minute is charged |
| `Seek.CellStale` | 0.01 | 0.05 | choosing a block already stood on costs something the cell head feels |
| `Seek.RoomSeenRays` | 3 | 12 | a room counts as seen on a real look, not a few rays (true coverage-based "searched" needs code) |
| `Seek.RoomEntry` | 0.3 | 0.5 | entering a room pays more (Stockades; Ragefire has no table, pays none by design) |

Left alone: Arrive, StepCost, ExploreFromRung, TrapShare, GoalsFromRung, the rung clocks.

**Read it by.** At the next evaluations: `revisit_share` (was 0.47-0.56), `rooms_entered`, the share of misses that ever
saw the object (~25%), `found`; and the training-side `look_entropy` and `sighted` beside them (a heavier
Revisit/CellStale clock can push the policy to run rather than look). The real test is the ablation gaps
(`no_searched`, `random_cell`, now 2-4 points), every 4th evaluation (arms_every 4).

**Revert.** If `found` drops below ~0.5 and has not recovered by the second evaluation after the resume, delete the
seven lines (the host's conf says where), `forgectl conf-sync`, restart the worldservers, resume. Four earlier Seek
reward redesigns (0023-0027) each fixed a local fault without changing where the bot goes; this one is measured the
same way.
