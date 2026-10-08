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
`Instance.WingRungStart` at `StageScenario.cpp:302`), the pull-drill rung (`InstanceEncounter.cpp:143`, `PullRungStart`)
and the
per-class `DifficultyLadder` tiers (`DifficultyLadder.h`: "Rungs start at 0 with the worldserver") live in worldserver
memory. A restart (every cluster rebuild, then `resume`) returns them to the conf value or 0; the learner's `latest.pt`
does not carry them, and only the host's rung is broadcast to workers (`AnimusForge.cpp:1084`). Matters most for D2/D3
after the
rebuild planned before the dungeon stages. Direction: persist the rungs in `progress.json`/the run directory and restore
on
`resume`, or document that the conf key must be set by hand. Verify: GTest for save and restore plus a resume dry run.
`UNVERIFIED`: whether `forgectl stage resume` sets `WingRungStart`.

**A2. A score-based ladder remains in the sim.** `DifficultyLadder::Record` moves a class down when a 200-fight window
is under
60% won (`Encounters/DifficultyLadder.cpp:93`), for C1 to C3 and G1. Principle 10 says difficulty ladders never step
back on a
score; the learner side obeys, this sim side does not, and there is no test for it (no GTest names `DifficultyLadder`).
Direction:
decide whether the exception is intended; if not, remove `LowerBelow` or make it an alarm. Verify: a GTest on `Record`.

**A3. Comment and config disagree on live stage settings.** `configs/move1_controls.yaml` has `fade.rungs: [0.0]` (a
single rung)
while its comments describe the path x0.5, x0.25, x0; `configs/move2_seek.yaml` says the noise prices keep M1's cost
ladder, but
`costs.enabled` is false by inheritance. A refactor of the config chain that "fixes" the comment direction would change
training.
Direction: make the yaml say what runs; keep
`test_metric_names.py::test_a_move4_costs_ladder_is_off_and_names_no_gate`-style tests for
each stage's ladder switches. Verify: a test that loads every stage and asserts `fade.enabled`, `costs.enabled`,
`require_plateau`.

**A4. Learner imports modules lazily, so editing files under a running learner can mix versions.** Function-level
imports include
`bootstrap.py:464`, `export.py:365` and `:396`, `env.py:203`, `bench_learner.py:155`, `mappo/trainer.py:838` and
`:2039`,
`train.py:124`, `protocol.py:253` and `:292`. A `git pull` while a learner runs (the cluster's `cluster-pull.sh` does
this
before recreating the container) can load new code into an old process. Direction: always stop the learner before
changing
the tree (deploy-gate.md does); consider hoisting the imports. Verify: grep for indented imports; a test that fails on
new ones.

**A5. Rollout-graph code edited in the learner trim is covered only by GPU tests.** `tests/test_rollout_graph.py:11` and
`test_rollout_graph_log.py` skip without CUDA/ROCm (`pytest.mark.skipif(not torch.cuda.is_available())`); the CPU suite
cannot
catch a regression. Direction: run the GPU tests on a free card before each deploy (`forgectl test --gpu`, step 3 of the
deploy
gate). Verify: the GPU run reports zero skips.

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
per term; add a
test listing every Shaping term that is only ever negative. Verify: `test_stage_purpose.py` style check.

**A9. Death knights are absent from dungeon training.** Decision 0004; the final models would play dungeons without a
death-knight
policy tuned for the band. Not a bug, a gap to decide. `UNVERIFIED`: the sim's redraw of the party (see StageScenario
casting draw).

## B. Dead or kept-suspicious code

**B1.** Removed with the GPU camera (tag `archive/gpu-camera`): `GpuVision::Renderer::Forget` no longer exists.

**B2.** `MODE_FLAG` bit 1 is unused (`Bridge/Protocol.h:274` defines only `MODE_FLAG_STAND_IN = 2`); the old
`MODE_FLAG_SCRIPTED_OPPONENTS` is gone but the value is reserved until the next protocol change (commit aa303bc33).
Decide whether to renumber at the next bump (protocol 25 now).

**B3.** `GoalBlock`'s order columns (`OBS_FROM_ORDER`, `OBS_ORDER_KIND_FIRST`, `OBS_ORDER_TARGET_FIRST`,
`Blocks/GoalBlock.h:50-52`) are always zero since the director was deleted, but stay in the layout (128 observations in
every stage, pinned by `LiveLayoutPinTest`). Removing them changes every checkpoint; only do it with a revision bump and
a deliberate re-pin.

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

**C1.** No GTest names `DifficultyLadder`, `CombatEncounter`, `PartyEncounter`, `StandInSeat` or `OpponentPool` (grep of
`src/test/server/game/Animus`); combat is reached only through `CombatPerceptionTest.cpp` and `RolesStageTest.cpp`. Add
tests before splitting those files.

**C2.** The full reward arithmetic of the dungeon stages (`InstanceEncounter::Reward`, 2530-2700) is covered by
`DungeonStagesTest.cpp` only at the level of `WingRun` helpers (`UNVERIFIED`: check it asserts ledger totals).

**C3.** Stages M3 to D3 have never trained; their configs are checked by tests for self-consistency (gates exist,
columns exist) but not by a run. The first run of each is the test.

**C4.** The map data tables are validated against vmaps by data tests that need `FORGE_VISION_DATA`
(`StockadeHallwaysDataTest.cpp`, `StockadeRoomsDataTest.cpp`, `DeadminesSitesDataTest.cpp`); where that data is absent
they skip. Their authoring scripts are in gitignored plan folders (`perception-goals/tools/*.py`;
`dungeon-curriculum/tools` is empty here), so the tables cannot be regenerated from the repository.

**C5.** `test_combat_stages.py::test_the_seed_chain_runs_from_m2` asserts C1 extends `move3_interact`; the name is stale
and would mislead a reader.

## D. Operational gaps

**D1.** Resume after a rebuild loses the sim ladders (A1). **D2.** `docker attach` is the only control path and `forge
pause` is host-only (A6, decision 0001). **D3.** Conf files are per machine, untracked and hand-synced for 239 keys.
**D4.** No CI for the forge; tests run through `forgectl test` in the dev container. **D5.** The wing ladder has a
collapse alarm but no stall alarm (`Encounters/WingLadder.h`), unlike the learner's gate-stepped fade (`animus/stage.py`
`_watch_stall`); D2 and D3 could sit flat on a rung unnoticed. **D6.** `docs/forge/README.md` and several chapters still
describe the first curriculum (not this chapter set). **D7.** Evaluation of D2/D3 takes hours of sim time per heldout
run; `heldout_every: 4` (default) is the guard.

## E. Design debts

**E1.** Seed chain and config chain differ and nothing checks they agree in intent (`combat1_fight.yaml` standalone;
`move4_follow` extends `move2_seek` in both). **E2.** Values re-set by a standalone yaml silently reset an upstream
decision: M2's lr 1.5e-4 and look entropy 0.004 do not reach C1 to D3 (3e-4 and 0.001). Decide per stage and say so in
the yaml. **E3.** M4's scripted follow leader is the last script; the learned-leader path is unbuilt. **E4.** The goal
head's order, secondary-goal and slot machinery serve stages that no longer have directors. **E5.** The stand-in is a
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
Verify with `LiveLayoutPinTest` unchanged, the full GTest set and a `forge run <stage> random 1` per stage.

**G2. `InstanceEncounter.cpp` (2752 lines).** Regions (method start lines): route and fight setup (about 181-1150), the
run loop and boss and pack tracking (1151-1980), views and goal places (1982-2450), `Reward` (2530-2700), selection and
teardown (2734+). Wing ladder, pull drill, corridor and Go-Explore are four behaviours sharing one `EnvInstance`; split
by behaviour with `WingRun.h` as the pure core. Verify with `DungeonStagesTest.cpp`, `WingLadderTest.cpp`.

**G3. `AnimusForge.cpp` (3087) and `ForgeCommands.cpp` (1304):** the state machine, cluster, learner supervision and
commands are intertwined; the control-socket proposal (decision 0001) needs a seam here first.

**G4. Python:** `mappo/networks.py` (2382), `train.py` (2309), `mappo/trainer.py` (2053) are the largest; `train.py`
mixes the rollout loop, evaluation, checkpointing and ladders. Cover with `tests/golden/` and `test_golden_update.py`
before moving code.

**How to verify any refactor here:** `forgectl test` (GTests and CPU pytest), `forgectl test --gpu` on a free card,
`LiveLayoutPinTest` unchanged, `stage_json_diff.py` old versus new build shows no change for a stage you did not mean to
change, and `resume_check.py` against the live run's checkpoint.
