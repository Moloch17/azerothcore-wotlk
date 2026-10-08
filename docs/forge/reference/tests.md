# Tests: there are none

On 2026-10-07 the owner removed every test the forge had added ("I would prefer to just not have any tests at all for
now"). Nothing runs tests and nothing in the operating procedures depends on them. Upstream AzerothCore's own tests
under `src/test/` (the Spells, Collision, Config and other suites) were never modified by the forge and stay as upstream
has them.

## What was removed

| Suite | Files |
|---|---|
| The sim's GTests, `src/test/server/game/Animus/` | 46 |
| Two more GTests (`WmoLiquidTest.cpp`, `SealedWriteTest.cpp`) | 2 |
| The learner's pytest suite, `apps/forge/python/tests/` (with its fixtures and golden files) | 106 |
| `apps/forge/tools/forgectl-test.sh`, `forgectl test`, the pytest settings in `pyproject.toml` | 3 |

Every one of them is in git history. The state with the tests is the tag `archive/with-tests` (on `lan` and `origin`):

```
git ls-tree -r archive/with-tests --name-only | grep -i test          # list them
git checkout archive/with-tests -- apps/forge/python/tests            # bring a suite back
git checkout archive/with-tests -- src/test/server/game/Animus       # bring the GTests back
```

Bringing the GTests back also needs the cmake `-DBUILD_TESTING=ON` build; the old procedure (a throwaway tree, a relink
against the llvm-17 profile runtime because the dev container's clang 18 has none) is in `git show
archive/with-tests:apps/forge/tools/forgectl-test.sh`.

## The safety nets that went with them

The tests were also guards. Each row says what could now break without anything noticing, and how to check it by hand.

| Guard (removed) | What could now break silently | How to check by hand |
|---|---|---|
| `LiveLayoutPinTest` and its golden file | A block's columns, actions, id or revision change (or the critic state width, 1927 since 2026-10-08) and old checkpoints no longer seed or load | Generate stage.json from the built sim (`forge run <stage> random 1`) and compare with the old build's using `apps/forge/tools/stage_json_diff.py`; run `apps/forge/tools/resume_check.py` on the real checkpoint |
| `BotAccountsTest` | The seat account id range moves | Read `BotAccounts` (`BASE`, `SEATS_PER_ENV`, `SESSIONS_PER_BOT`, `MAX_ENVS`) before and after a change |
| `test_conf_covers_tuning` | `worldserver.conf.dist` and the tuning's `Visit` list disagree (a key documented but unread, or read but undocumented) | `python3 apps/forge/tools/conf_prune.py --removed <old-rev> <new-rev>` lists keys added or removed in conf.dist; compare with `CurriculumTuning.h` |
| `test_metric_names` | A live yaml gate, headline or measure names a column the stage never reports (it then reads as 0 or never fires) | `python3 apps/forge/tools/sim_metrics.py --check <stage.json>` against the stage's real stage.json |
| `test_golden_update` | The learner's network shapes or one-update numbers change after a refactor | `resume_check.py` (state-dict keys and shapes against the real checkpoint); compare a short run's first losses with the old build's |
| Stage validation tests (`CurriculumProblems`) | A stage definition becomes inconsistent | `CurriculumProblems()` still runs at startup (`Stages.cpp`): start the stage and read the console |
| `WingLadderTest`, `test_shaping_fade`, `test_rung_rebaseline` | The wing ladder steps back on a score, the convergence state is not re-baselined at a gate-stepped rung, the stall or collapse alarms stop firing | Read `WingLadder.cpp` and `animus/stage.py` (`rebaseline`, `_watch_collapse`, `_watch_stall`) after any change; watch the first evaluations of a stage |
| `test_forgectl` (mocks for ssh, docker, console) | forgectl's confirmations, audit log, signal handling and conf writes stop behaving | Run the reversible rehearsals in `docs/forge/deploy-gate.md` (R2: `stage pause` then `resume`; R4: the read-only checks) before relying on a change |
| Vision, mental-map and entity-memory GTests | The pixel format, map cells, or memory entries drift | Compare a camera snapshot (`forge camera snapshot`) and the audit PNGs of a short run against the last known good ones |
| GPU-only tests (rollout graphs, fused GRU, free look, vision encoder) | The rollout graph path stops capturing (a silent fall-back to the eager path costs throughput) or the fused-GRU path crashes | The first minutes of a resumed run: the learner prints one line when a rollout graph is captured and why graphs are off when they are |
| `test_human_reader` | The human capture reader and the capture format drift apart | Open a capture sample with `animus.human` and read `FORMAT.md` |
| `SealedWriteTest` | The sealed database pool logs every dropped write instead of once | Read `NoteSealedWrite` in `DatabaseWorkerPool.cpp`; check the server log for repeated "dropped on sealed" lines |

## Working without tests

- Verify a change by reading it, by building it when a build is wanted (`./forge.sh --build`), and by the manual checks in
  the table above and in `docs/forge/deploy-gate.md` (stage.json diff, `resume_check.py`, the first minutes of a run).
- Be most careful in the areas of the table: the layout machinery (`cpp-blocks.md`, `cpp-layout-character.md`), the
  tuning and conf agreement (`cpp-tuning-keys.md`), the ladders and convergence (`py-learner-stage.md`) and the
  wire protocol (`protocol.md`).
- If a suite is wanted again, bring back only the part that guards something that has just bitten you.
