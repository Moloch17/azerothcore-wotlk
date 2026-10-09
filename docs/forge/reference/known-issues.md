# Reference: known issues, ranked, for the refactor

Purpose and scope: one consolidated list for the owner's manual review and refactor, written from the facts the owner
and
the other documentation agents gave, plus what was found while writing [stages.md](stages.md), the decisions and the
glossary. It is self-contained: other reference documents have their own "Observed issues" sections, and the owner
merges
those. Each item gives what, where (`path:line`), why it matters, a suggested direction and how to verify a fix. Line
numbers are of branch `forge` at commit bd32b9dc8. `UNVERIFIED` marks what was not checked. Related:
[stages.md](stages.md), [tests.md](tests.md), [protocol.md](protocol.md), [config-keys.md](config-keys.md),
[tools-and-ops.md](tools-and-ops.md), [glossary.md](glossary.md), [../principles.md](../principles.md).

## Map of the files this list points at

| Path | Lines | Role |
|---|---|---|
| `src/server/game/Animus/Scenario/Curriculum/StageScenario.cpp` | 5192 | The scenario that runs any stage (the brief says about 6,000: the trim reduced it) |
| `src/server/game/Animus/Scenario/Curriculum/Encounters/InstanceEncounter.cpp` | 2752 | Whole-dungeon encounter (brief: about 3,300) |
| `src/server/game/Animus/AnimusForge.cpp` | 3087 | The forge's plan, state machine, cluster and learner glue |
| `src/server/game/Animus/Runtime/Scenario/Curriculum/Stages/Stages.cpp` | 1393 | Stage definitions plus map data tables |
| `src/server/game/Animus/Scenario/Curriculum/CurriculumTuning.h` | 1185 | All tuning keys with defaults |
| `src/server/game/Animus/Scenario/Curriculum/Encounters/DifficultyLadder.cpp` | 104 | Per-class tier ladder |
| `src/server/game/Animus/Scenario/Curriculum/Encounters/WingLadder.h` | 98 | Whole-dungeon ladder |
| `src/server/game/Animus/Scenario/Curriculum/Rewards/CombatReward.cpp` | 91 | Holds `RewardTermName` |
| `src/server/game/Animus/Bridge/Protocol.h` | 401 | Wire protocol, `MODE_FLAG_*` |
| `src/server/game/Animus/Bridge/ClusterLink.cpp` | 463 | Cluster registration and fingerprint |
| `apps/forge/python/animus/train.py`, `mappo/trainer.py`, `mappo/networks.py` | 2309, 2053, 2382 | The learner's largest modules |
| `apps/forge/python/animus/stage.py`, `config.py`, `bootstrap.py` | 829, 853, 903 | Ladders and convergence, config, seeding |
| `apps/forge/python/configs/*.yaml` | 1351 | Stage configs |

## A. Correctness risks

**A1. The sim's ladder state is not checkpointed.** The wing ladder rung (`StageScenario::_wingLadder`, constructed from
`Instance.WingRungStart` at `StageScenario.cpp:302`), and the
per-class `DifficultyLadder` tiers (`DifficultyLadder.h`: "Rungs start at 0 with the worldserver") live in worldserver
memory. A restart (every cluster rebuild, then `resume`) returns them to the conf value or 0; the learner's `latest.pt`
does not carry them, and only the host's rung is broadcast to workers (`AnimusForge.cpp:1084`). Matters most for D2/D3
after the
rebuild planned before the dungeon stages. Direction: persist the rungs in `progress.json`/the run directory and restore
on
`resume`, or document that the conf key must be set by hand. Verify: a resume dry run.
`UNVERIFIED`: whether `forgectl stage resume` sets `WingRungStart`.

**A2. A score-based ladder remains in the sim.** `DifficultyLadder::Record` moves a class down when a 200-fight window
is under
60% won (`Encounters/DifficultyLadder.cpp:93`), for C1 to C3 and G1. Principle 10 says difficulty ladders never step
back on a
score; the learner side obeys, this sim side does not.
Direction:
decide whether the exception is intended; if not, remove `LowerBelow` or make it an alarm.

**A3. Comment and config disagree on live stage settings.** `configs/move1_controls.yaml` has `fade.rungs: [0.0]` (a
single rung)
while its comments describe the path x0.5, x0.25, x0; `configs/move2_seek.yaml` says the noise prices keep M1's cost
ladder, but
`costs.enabled` is false by inheritance. A refactor of the config chain that "fixes" the comment direction would change
training.
Direction: make the yaml say what runs. Verify: read each stage's resolved `fade.enabled`, `costs.enabled`,
`require_plateau`.

**A4. Learner imports modules lazily, so editing files under a running learner can mix versions.** Function-level
imports include
`bootstrap.py:464`, `export.py:365` and `:396`, `env.py:203`, `bench_learner.py:155`, `mappo/trainer.py:838` and
`:2039`,
`train.py:124`, `protocol.py:253` and `:292`. A `git pull` while a learner runs (the cluster's `cluster-pull.sh` does
this
before recreating the container) can load new code into an old process. Direction: always stop the learner before
changing
the tree (deploy-gate.md does); consider hoisting the imports. Verify: grep for indented imports.

**A5. Rollout-graph code edited in the learner trim was covered only by GPU tests.** (Moot: the tests were removed 2026-10-07; see [tests.md](tests.md) for the hand check, the first minutes of a resumed run.)

**A6. `forge pause` does not reach cluster workers.** Only the host's console command exists; `forgectl stage pause` and
`cancel`
visit each worker's console over ssh (forgectl.md). A person typing `forge pause` by hand pauses the host only.
Direction: the
cluster-wide fan-out in decision 0001 (proposal, not built). Verify: pause on the host leaves workers paused.

**A7. The cluster fingerprint is broad and unforgiving.** It hashes source, protocol version, decision
timing and the
curriculum tuning values (`Bridge/ClusterLink.cpp:226` onward; 239 keys on 2026-10-07). Conf files are per machine and
untracked, so
a hand edit on one machine refuses that worker. Direction: `forgectl conf-sync --check` before every start; consider
tracking the
stage tuning in files (the human-operable plan's Phase 3). Verify: `forgectl cluster` shows no "refused the worker"
lines.

**A8. Dungeon-stage rewards that are costs in meaning are categorised Shaping.** `Threat` (the `WingCrowd` charge,
`InstanceEncounter.cpp:2582`)
and `HealingMana` are negative prices but `RewardCategory::Shaping`, so the fade removes them and they are not in
`score_outcome`
(`RewardLedger.h`). Same family as the 2026-10-07 fixes that moved Stall to Idle and Approach to Lost. Direction: decide
per term.

**A9. Death knights are absent from dungeon training.** Decision 0004; the final models would play dungeons without a
death-knight
policy tuned for the band. Not a bug, a gap to decide. `UNVERIFIED`: the sim's redraw of the party (see StageScenario
casting draw).

## B. Dead or kept-suspicious code

**B1.** Removed with the GPU camera (tag `archive/gpu-camera`): `GpuVision::Renderer::Forget` no longer exists.

**B2.** (Resolved as documentation, 2026-10-08.) Flag value 1 (bit 0) of `ModeMsg.Flags` is unused: the old
`MODE_FLAG_SCRIPTED_OPPONENTS` is gone (commit aa303bc33) and `Bridge/Protocol.h` defines only `MODE_FLAG_STAND_IN = 2`
(bit 1; the old comment said "Bit 1 is unused", which read the wrong way round). Nothing reads bit 0, and the sim does
not check unknown bits. It is kept reserved on purpose: renumbering `STAND_IN` to 1 would change the wire value inside
protocol 26 on both sides at once (sim `Protocol.h`, learner `protocol.py`) for no gain, so it waits for a protocol
bump that changes the wire anyway. Comments in `Protocol.h` and `protocol.py` now say this.

**B3.** (Resolved, 2026-10-08.) `GoalBlock`'s order columns (`OBS_FROM_ORDER`, `OBS_ORDER_KIND_FIRST`,
`OBS_ORDER_TARGET_FIRST`) were always zero since the director was deleted. They left the block (101 -> 68 observations,
goal block revision 1 -> 2) in one change with every reader: `GoalHead.columns`/`block_width`/`signals`, the `given` /
`primary_given` path of `draw` and `decide_goals`, `trainer.py`'s slow update, `export.py`'s reference decision,
stage.json `goals.columns`, the column names. See section H. (Loot, Gather, Interact and the objective/giver/turn-in
targets were fixed earlier.)

**B4.** (Resolved.) The learner's seat-set network (`EntitySets`, `mappo.seat_sets`, `entity_attention`, `SEAT_SET_NAMES`, the
seat-set seeding) was deleted. `StageScenario.cpp:1346` still writes `sets` into `stage.json` (the learner ignores it for
the networks; `bootstrap._layout_sets` still reads it to re-match a block whose set only gained slots), and `export.py`'s
seat-set branches now name symbols that no longer exist (the export is held for an owner decision).

**B5.** `BlockId::Duel` and its class `DuelBlock` keep the name of the deleted one-on-one stage but now carry movement,
auto-attack, pets and stopping casts for every fighting stage (`Layout/Block.h:49`). Also `Opposition::Instance`-era
comments mention PvE/PvP (`StageDefinition.h`, `ArenaDefinition` summary) and the Sight enum comment describes a compass
ladder withhold that the encounter does by rung. Rename only with a revision plan, since names are the seeding key.

**B6.** `CombatReward.cpp` holds `RewardTermName` (all terms), but its name and the namespace `CombatReward` (now only
`DesiredRange` and `TierScale`) no longer fit; rename to `RewardTerms.cpp`.

**B7.** `PartyFollowEncounter` keeps `CastShare` (conf `PartyFollow.CastShare` = 0) and the owner-slot machinery for a
learned leader that no config uses (`PartyFollowEncounter.h:40-47`). It is documented as "not dead code" because the
scripted leader uses the slot; the learned-leader half is unexercised.

**B8.** `eval.mask_actions`, `style.*` (the movement-style reward, `enabled: false` in M1 and inherited), `distill.*`
and `cast.agents` are config surface with no live stage using them (`animus/config.py`). Keep or delete per decision 17
of principles ("dead code is deleted, not gated").

## C. Coverage gaps

**C1.** (Moot: the tests were removed 2026-10-07; see [tests.md](tests.md).)

**C2.** The full reward arithmetic of the dungeon stages (`InstanceEncounter::Reward`, 2530-2700) has no test (tests removed 2026-10-07; see [tests.md](tests.md)).

**C3.** Stages M3 to D3 have never trained; their configs are checked for self-consistency (gates exist,
columns exist) only by `CurriculumProblems()` at startup and by reading, not by a run. The first run of each is the test.

**C4.** The map data tables are not validated against vmaps any more (the data tests were removed 2026-10-07). Their authoring scripts are in gitignored plan folders (`perception-goals/tools/*.py`;
`dungeon-curriculum/tools` is empty here), so the tables cannot be regenerated from the repository.

**C5.** (Moot: the tests were removed 2026-10-07.)

## D. Operational gaps

**D1.** Resume after a rebuild loses the sim ladders (A1). **D2.** `docker attach` is the only control path and `forge
pause` is host-only (A6, decision 0001). **D3.** Conf files are per machine, untracked and hand-synced for 239 keys.
**D4.** No CI for the forge, and no tests (removed 2026-10-07; see [tests.md](tests.md)). **D5.** The wing ladder has a
collapse alarm but no stall alarm (`Encounters/WingLadder.h`), unlike the learner's gate-stepped fade (`animus/stage.py`
`_watch_stall`); D2 and D3 could sit flat on a rung unnoticed. **D6.** `docs/forge/README.md` and several chapters still
describe the first curriculum (not this chapter set). **D7.** Evaluation of D2/D3 takes hours of sim time per heldout
run; `heldout_every: 4` (default) is the guard.

**D8.** Baked camera (decision 0020, stage 3): (a) interior frames of a map with a big static scene (Deadmines, 1.7 to
2.2 ms) are bound by the static BVH trace (about 1.2 ms of it) and the liquid cast, not by the terrain; (b) open-air
frames over hilly ground cost about 3 to 3.7 ms: a ray grazing the relief visits about 16 cells that the block ranges
cannot reject, and a finer range level (4 x 4 cells) would cut that; (c) the scene-wide liquid band takes the extreme
levels of the map files, and Deadmines carries a -500 liquid level (probably a sentinel), which widens the height clip
of descending rays on that map; (d) the digest check cannot see a source file changed in place with the same size and
nanosecond mtime; (e) after a copy of the data the continent identity check pays the content hash once (about 0.2 s for
map 0, cached) and rewrites the digest, then the next start is a few milliseconds; (f) the culled and the reference
cell walks differ in float formulation (reciprocal multiply against divide), so a ray through the exact shared corner
of four cells could in theory pick a different first cell: none in 3,000,000 random rays.

## E. Design debts

**E1.** Seed chain and config chain differ and nothing checks they agree in intent (`combat1_fight.yaml` standalone;
`move4_follow` extends `move2_seek` in both). **E2.** Values re-set by a standalone yaml silently reset an upstream
decision: M2's lr 1.5e-4 and look entropy 0.004 do not reach C1 to D3 (3e-4 and 0.001). Decide per stage and say so in
the yaml. **E3.** M4's scripted follow leader is the last script; the learned-leader path is unbuilt. **E4.** The goal
head's order, secondary-goal and slot machinery serve stages that no longer have directors (its order columns are gone: B3). **E5.** The stand-in is a
frozen partner from the pool and is absent while the pool is empty; G1 starts with only `combat3_survive` as a partner.
**E6.** The first curriculum's stage-numbered terms still appear in comments and docs (`stage6`, `stage8`) in code such
as `RewardLedger.h`.

## F. Naming and structure

**F1.** `Duel`, `Gauntlet`, `Pack`, `Roles`, `Instance` mix first-curriculum and current names. **F2.**
`CurriculumTuning.h` is one struct with ~725 keys and comments that retell history (dates 2026-09-28 and later); split
per encounter family. **F3.** `Stages.cpp` mixes definitions (about 400 lines) with 1,000 lines of map tables; move the
tables to their own file or to data files. **F4.** `configs/combat1_fight.yaml` (173 lines) duplicates the MAPPO block
that `move1_controls.yaml` also carries; make C1 extend a shared base or M3. **F5.** Docs under `docs/forge/` numbered
01 to 08 describe pieces that moved (`animus-lib` dissolved); the new `reference/` set is the replacement.

## G. Refactor candidates and their seams

**G1. `StageScenario.cpp` (5192 lines).** Seams by region, approximate (method start lines): construction and layouts
(to about 1500); seat lookups and weights (1520-1880); `Rebuild` and seat building (1886-2690); camera look, sub-ticks
and applying actions (2689-3530); `ObserveSeat` (3532-3800); goals (`GoalPotential` 3799 to about 4020); `JudgePress`
and the intent prices (4027-4620); evaluation pinning and episode tracking (4622-4900); `SeatReward` (4909-5070);
`WriteState`. Suggested split: Rebuild+character build; Observe+WriteState; Intent (JudgePress and goals); Reward.
Verify with a stage.json diff and a `forge run <stage> random 1` per stage.

**G2. `InstanceEncounter.cpp` (2752 lines).** Regions (method start lines): fight setup (about 181-1150), the
run loop and boss and pack tracking (1151-1980), views and goal places (1982-2450), `Reward` (2530-2700), selection and
teardown (2734+). The wing ladder and the run itself share one `EnvInstance`; split
by behaviour with `WingRun.h` as the pure core. Verify with a `forge run <stage> random 1` per dungeon stage.

**G3. `AnimusForge.cpp` (3087) and `ForgeCommands.cpp` (1304):** the state machine, cluster, learner supervision and
commands are intertwined; the control-socket proposal (decision 0001) needs a seam here first.

**G4. Python:** `mappo/networks.py` (2382), `train.py` (2309), `mappo/trainer.py` (2053) are the largest; `train.py`
mixes the rollout loop, evaluation, checkpointing and ladders. No tests cover it (removed 2026-10-07; see [tests.md](tests.md)).

**How to verify any refactor here:** `stage_json_diff.py` old versus new build shows no change for a stage you did not mean to
change, and `resume_check.py` against the live run's checkpoint.

## Decision time jitter, 2026-10-08 (decision 0021)

- The policy is not told the step. A decision lasts `DecisionMs` less the last overshoot plus its own (200-300 ms for the
  default body, up to ~700 ms with a spike); neither the observation nor the STEP says which. The module knows its
  accumulated tick time, so a `last_dt` input would be identical live, but it is a layout change (owner decision). Same for
  a per-step dt on the wire to make the learner's discount `gamma ** (dt / reference_decision_ms)`.
- The observation's own clocks stay nominal on purpose (`SeatView::DecisionMs`: entity and map memory age, free look, the
  breath spent), because the realm's module passes `DecisionMs` for them; the module could pass its real accumulated time
  and training follow, together.
- The jitter defaults are an assumption (a stock world update and a rare spike), not a measurement: the audit could not
  read the realm's tick. `python -m animus.human parity` reports the realm tick and decision intervals once captured;
  set `Decision.JitterMs` / `SpikeMaxMs` from it.
- All envs of a pool share a decision's length (the game clock is global). Under `HalfBatch` the mean decision drifts
  about +7 ms with the default spikes (a spike swallows 125 ms periods); `HalfBatch` is off by default.
- Changing `Decision.*` under a run you resume: the sim does not refuse it (the fingerprint compares only the machines
  running now); the learner warns on start when `spec.json` of the run held other values (`train.py`). Verdict: warn, not
  refuse -- a shift in the distribution the value function re-fits, not a shape mismatch. Resuming a run that trained on
  exact ticks (M1 before this change) with the defaults is that shift; set `Decision.JitterMs 0` and `SpikeProb 0` to
  keep it as it was.
- About 2% of decisions at the defaults are followed by one of 5-50 ms (the carry after a spike); a floor is an option
  (decision 0021), not done.
- `forge run ... random` and `forge status` show the `decision time` row; the evaluation videos play at the nominal
  `DecisionMs` per frame, so their game-time pace varies with the jitter.

## M2 route recording: what Python records and the C++ half still missing (2026-10-09)

Python half (done): every plain evaluation and held-out sweep writes `eval_motion_<env_steps>[_heldout_<arena>].npz` ([file-formats.md](file-formats.md)): the raw kinematic track of each scored seat, with its `seed`, layout, `found` and episode info row.

What a track already holds. The kinematic sample (protocol 20, `Animus/Env/Kinematics.h`, filled by `StageScenario::AgentKinematics`, `StageScenario.cpp` ~3448) is 10 floats: `t` (`EpisodeElapsedMs` / 1000), `x`, `y`, `z` (`Player::GetPositionX/Y/Z`: absolute map coordinates of the instance map, not deltas), `yaw` (`GetOrientation`), `pitch` (the seat's `Mover.Body.Pitch`), `mode`, `mounted`, `speed`, `in_combat`; zeros for a seat with no body. `motion.SAMPLE_DIM` = 10 includes world position. So the spawn (first sample), the route and the end position (last sample, the decision before the done) need no C++ change; the "body-frame only" reading in m2-routes was about `eval_motion.npz`'s windows, not the sample.

Still missing: the object's position and the placed spot. `SeekEncounter::EnvSeek` (`SeekEncounter.h`) holds `Position Spot` (the object's base) and `Position Centre` (the objective point the flag marks); neither is an episode info column (`SeekEncounter::AddEpisodeInfo`, `SeekEncounter.cpp` ~102 has `seek_room`, `seek_object`, `room_depth`, ... only). Minimal change: in `AddEpisodeInfo` add

    table.Add("seek_object_x", [this](Env const& env, uint32) { return _envs[env.Index].Centre.GetPositionX(); });

and `seek_object_y`, `seek_object_z` the same way (and `seek_spot_x/y/z` from `Spot` if the base is wanted), in world coordinates like the samples (float32; the Stockades coordinates are in the hundreds, so exact to well under a yard). No spawn column is needed (first sample); `_scenario.SpawnPointFor(env)` is the same point. Checks before building: that `Centre` is set for every placement path including the hallway and fallback rungs (`Place`, `PlaceHallway`, `Fallback`); that the new names are classed as per-episode values in `animus/episode_means.py` (not per-event means) and listed in `docs/forge/reference/cpp-encounters.md` and the stage's `episode_info` list. `episode_info_dim` grows by 3 (6): the learner reads names and width from the handshake, so no protocol number or layout signature changes, but a rebuilt sim and learner must go together (a new `spec.json`), so it rides the next planned rebuild. Until then a route's target is only known as `seek_room` and `seek_object`; room positions are in `Stages.cpp` (`StockadeRooms()`).

Unverified: nothing here was run on a GPU or against a sim; the Python writers were exercised with a fake environment (var/verify_arms.py in the worktree that wrote this).

## H. Layout and protocol cleanup, 2026-10-08 (lands with the next layout bump)

Fixed (commits `Layout cleanup: ...` and `Drop the duplicate epochs_done metric`):

- Goal space: `SeatGoal::Loot`, `Gather`, `Interact` and the targets journal objective (4), giver and turn-in removed
  (never offered by `GoalBlock::Available`, no looting: decision 0003). 12 kinds x 29 targets (348) became 9 x 23 (207);
  goal block revision 0 -> 1, 128 -> 101 columns, columns now named; `mappo.goal_count`/`goal_targets` 9/23 in
  `move1_controls.yaml` and `combat1_fight.yaml`; `Goals.WorldValue` deleted. The episode columns
  `goal_{loot,gather,interact}_share` and `goal_success_{loot,gather,interact}` are gone.
- Critic state 1958 -> 1927 wide: global columns pull active, pulls cleared, next pull, elite pull, linked pull, owner
  mana, owner in combat and enemy "victim is the owner" were written by no encounter. `STATE_TIER` is now 6 and the
  arena one-hot starts at 7 (stage.json `state.arena_first` follows); stage.json gains `state.dim`.
- `epochs_done` (duplicate of `epochs_run`) removed from metrics.csv.

Seeding: the actor's goal head and goal embedding change shape with the kind/target counts, so `_seed_shared` (shape
check) leaves them fresh in the first stage of the chain; the goal block's adapter columns start fresh (revision 0 -> 1,
and revision 0 named no columns); everything else carries as before. The critic state encoder is always seeded fresh, so
the state width costs nothing but a resume of a run trained at width 1958 (refused by `resume_mismatch`).

Not done (not provable): Duel `OBS_BOT_MOVING` and the `movespline->Finalized()` reads (not constant: fear, knockback,
Charge and taxis still start splines).

**Leftover cleanup, 2026-10-08 (second pass; same unreleased layout generation, protocol stays 26).**

- Goal block revision 1 -> 2, 101 -> 68 columns: the order columns and all their readers are gone (B3). The remaining
  columns keep their names, so a revision 1 checkpoint seeds them by name (`bootstrap._common_blocks` starts the block
  fresh by position and `_by_name` then carries every named column). stage.json `goals.columns` no longer has
  `from_order`, `order_kind`, `order_target`. `trainer.py` and `export.py` (still held) were updated in step.
- `respawns` (the same quantity as `rises`: both `Clock.Rises`) and `combat_rung` (the same as `difficulty`: both the
  tier) are gone from `CombatEncounter`'s episode columns. No yaml gate, fade, target or headline used `respawns`; the
  three combat yamls that listed `combat_rung` now list `difficulty`, and the evaluation videos (which found the rung
  by a column name ending `_rung`) fall back to `difficulty` when a stage has no such column
  (`Vision::EvalVideoRungColumn`); PartyFollow's videos gain a rung label as a side effect.
- B2 documented (above). The `RoutePlaces` flag of `WorldView` is `HasSeenPlaces` (C++ only; no manifest, stage.json or
  python name carried it).
- Not removed, on purpose: the `lootable` entity feature (`ENTITY_LOOTABLE`, column 9 of the 20-wide entity feature
  vector in the entities and sight blocks). It is not always zero (a killed creature the seat may loot sparkles, and
  so does a ready chest: `Vision::FactsOf`, `Identity`), and removing it would shift `ENTITY_FEATURES` from 20 to 19,
  which the learner's entity encoder, adapters and the camera's class table all size from; keeping the width and
  repurposing the slot is not wanted. Looting stays out of scope (decision 0003); the column just reports what the
  client shows. Revisit only with a deliberate entity-width revision.
- Not removed: the `detour` entry of `bootstrap.MOVE_REVISION_4_COLUMNS`. The table is the positional name list of the
  63 columns of a revision 4 move block (checked against the block's width), so deleting one name would shift every
  later column's name. It is unreachable only by M1 runs newer than 2026-10-06; older checkpoints still seed by it.
