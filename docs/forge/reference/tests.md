# Tests: there are none

Purpose and scope. The forge has no tests. The owner decided (2026-10) to carry none for now. This note records what was
removed, how to get it back, and which safety nets are gone, with a hand check for each. Related:
[tools-and-ops.md](tools-and-ops.md), [config-yaml.md](config-yaml.md), [known-issues.md](known-issues.md).

## What was removed

On `forge`: the 46 Animus GTest files under `src/test/server/game/Animus` (with `LiveLayoutPin.golden.inc`),
`src/test/common/Collision/WmoLiquidTest.cpp`, `src/test/server/database/SealedWriteTest.cpp`, the whole Python suite
`apps/forge/python/tests` (106 files including fixtures and goldens), `apps/forge/tools/forgectl-test.sh`,
`apps/forge/forgectl/testcmd.py` (`forgectl test`) and the pytest configuration in `apps/forge/python/pyproject.toml`.
Nothing is lost: the tag `archive/with-tests` holds all of it.

Bring any of it back (read-only listing first):

```
git ls-tree -r archive/with-tests --name-only | grep <name>
git checkout archive/with-tests -- <path>
```

The GTests build into the `unit_tests` target when present (`src/test/CMakeLists.txt` collects by glob; re-configure
after
adding a file). The pytests need the learner venv (`apps/docker/animus-venv.sh`) and the whole checkout, because many
read C++ sources through `parents[4]`.

## What the removed suites guarded

| Removed | Used to guard | What can now break silently | Hand check |
|---|---|---|---|
| `LiveLayoutPinTest.cpp` + `LiveLayoutPin.golden.inc` | block order, `BlockId`s, revisions, widths, column and action names of the 12 live stages | a block edit changes every checkpoint's layout; seeding by name or resume then misloads or refuses | diff `stage.json` of the last run against the new build with `apps/forge/tools/stage_json_diff.py old new`; never renumber `BlockId`s |
| `test_conf_covers_tuning.py`, `test_conf_prune.py`, `test_stage_ticks.py` | conf.dist agrees with `CurriculumTuning::Visit`; every movement stage ticks at 50 ms | a tuning key readable but undocumented, or documented but dead; a stage running 250 ms ticks | `python3 apps/forge/tools/conf_prune.py --check <conf>`; `python3 apps/forge/tools/gen_config_reference.py --check`; grep `Stage.*.TicksPerDecision` in conf.dist |
| `test_metric_names.py`, `test_gates.py`, `test_status_headline.py` | every metric a live yaml names exists for its stage | a gate on a missing column never fires: the ladder never steps and nothing says why | `python3 apps/forge/tools/sim_metrics.py --stage <stage>` against the yaml's `gate_metric`, `measure`, `headline`, `report` |
| `test_golden_update.py`, `test_update_stats.py`, `golden/learner_update.json` | the learner's shapes and one update's numbers on the real M2 stage.json | a learner refactor changes training numbers unnoticed | `python3 apps/forge/tools/resume_check.py` on a copy of a checkpoint (shapes, resume compatibility); compare `run_snapshot.py` before/after on a live run |
| `test_stage_validation.py`, `test_stage_names.py`, `test_stage_purpose.py`, `test_manual.py` | `Stages.cpp` passes `Problem`/`ArenaProblem`; configs equal stages; purpose paid as Outcome/Cost; the budget table in docs | an invalid stage only shows at `forge start`; a stage without a yaml; stale doc numbers | start the worldserver and read the log for `CurriculumSound`; `ls configs` against `Stages.cpp` `.Name` entries |
| `test_forgectl.py` (129 tests, mocked ssh/docker) | forgectl's parsing, plans, confirmation, audit, conf writers, move-host | a regression in a command that rewrites confs or restarts machines is found on a live cluster | use `--check`/`--dry-run` forms first (`conf-sync --check`, `videos --dry-run`), read the printed plan, check `wc -l` of each conf after a write |
| `test_human_*.py`, `human_capture_writer.py` | `animus.human`: capture reader, motion features, fit, parity, tracks | a reader/format drift corrupts the human dataset silently | run `python -m animus.human` commands on a small capture and inspect the outputs |
| `test_recurrent.py` (fused-GRU, GPU), `test_rollout_graph.py`, GPU cases in `test_free_look.py` and `test_vision_encoder.py` | captured GPU rollout graphs equal the eager path; the fused GRU call | wrong rollout actions or silently slower eager fallback on GPU; the CPU suite never covered these | a short `forge run` / one learner update on the card with `mappo.rollout_graphs` true and false, comparing losses |
| Vision/movement GTests (`Vision*`, `PlayerController`, `Client`, `MentalMap`, `Sight*`, `Seek*`, `Interact*`, `Dungeon*`, `WingLadder`, `Roles*`, `PartyFollow*`) | camera, controller, memory, encounters, ladders | behaviour changes in the sim with no signal until a training run degrades | `forge run <stage> random 1` per stage (smoke); read `forge status` columns |
| `SealedWriteTest.cpp`, `WmoLiquidTest.cpp` | sealed-database writes discarded; WMO liquid lookups | a core change breaking the seal or liquid queries | start the sim with `Forge.SealStrict` 1 and read the "sealed" log lines; `forge camera snapshot` at a lake |

Never covered even by the old suites: the real cluster paths (`ClusterLink`, fingerprint exchange, distributed learner),
the
device renderer on a GPU with real map data, `ForgeConfig::Load`, and forgectl write paths on real machines.
