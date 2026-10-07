# The deploy gate

The ordered checks a person runs before a new build goes to the cluster, and the steps that put it there. Written for
the cleanup deploy (the `cleanup` branch onto the build tagged `pre-cleanup-2026-10-07`, while `move2_seek` trains on
sarah) and reusable for any later one: replace the tag and the stage name. Every step has the command and what success
looks like; **stop at the first step that does not look like its success line**, and fix the cause before going on.

Until `forgectl` exists (in progress, see `.agents/plans/human-operable/human-operable.PLAN.md`), every step is by hand.
The cluster itself (machines, users, ports, how a stage is started) is in [cluster.md](cluster.md); the rules a change
must keep are in [principles.md](principles.md).

Commands run from the repository root on the dev machine unless a line says `ssh`. `DEV` below means the dev container,
`docker exec -w /azerothcore ac-animus-forge-dev-server`; its Python is `apps/forge/python/.venv/bin/python`.
The machines are `sarah@192.168.0.68` (host) and the workers `spencer@192.168.0.66`, `thomas@192.168.0.67` and
`moloch@192.168.0.117` (cluster.md); each keeps its checkout at `~/animus-forge`.

| Tool (in `apps/forge/tools`) | What it answers | Test |
|---|---|---|
| `sim_metrics.py` | which metric names a stage's sim produces (read from the C++); `--check stage.json` compares that with a built binary's own list | `test_metric_names.py` |
| `stage_json_diff.py old new` | what a new build changed about a stage's layouts, columns, reward terms, tuning, arenas | `test_stage_json_diff.py` |
| `resume_check.py` | will this checkpoint resume on this build (a dry run of `forge resume`, on CPU, no sim) | `test_resume_check.py` |
| `conf_prune.py` | which `AnimusForge.*` keys a build dropped; which keys of a machine's conf it no longer reads | `test_conf_prune.py` |
| `run_snapshot.py` | a run's headline, KL, steps/s in one table, and before/after | `test_run_snapshot.py` |

## 1. Tag what is deployed; style

The tag is the rollback (step 9) and the baseline for every comparison below. Tag the revision the cluster runs, not
the one you are about to deploy:

```
ssh sarah@192.168.0.68 'git -C ~/animus-forge rev-parse HEAD'      # the deployed revision (every machine: same)
git tag pre-<change>-<date> <that revision>                          # e.g. pre-cleanup-2026-10-07
git push lan pre-<change>-<date> && git push origin pre-<change>-<date>
python3 apps/codestyle/codestyle-cpp.py
```

Success: the tag is on `lan` and `origin` (`git ls-remote lan 'refs/tags/pre-*'`), and the codestyle run ends with
`Everything looks good` (exit status 0).

## 2. GTests and the CPU pytest

The C++ unit tests, in a throwaway tree of your own (never a tree another run uses). The dev container links the test
binary with a clang resource directory, which `link.txt` does not carry, so the link is repeated by hand:

```
DEV cmake -S /azerothcore -B /azerothcore/var/gate-build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
   -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DMODULES=static -DSCRIPTS=static \
   -DAPPS_BUILD=all -DTOOLS_BUILD=none -DUSE_COREPCH=OFF -DUSE_SCRIPTPCH=OFF
DEV nice -n 15 cmake --build /azerothcore/var/gate-build --target unit_tests -j16
```

If the build stops at the link of `unit_tests` (`cannot find -l...` or a missing clang runtime), re-run the link line
from `var/gate-build/src/test/CMakeFiles/unit_tests.dir/link.txt` with
`-resource-dir=/usr/lib/llvm-17/lib/clang/17` after `clang++`. Then:

```
DEV /azerothcore/var/gate-build/src/test/unit_tests
```

Success: `[  PASSED  ] N tests.` and no `[  FAILED  ]` line.

The learner's tests on CPU (the dev container's GPU stays hidden so nothing depends on a card):

```
docker exec -e HIP_VISIBLE_DEVICES= -w /azerothcore/apps/forge/python ac-animus-forge-dev-server \
   .venv/bin/python -m pytest -q -rs -p no:cacheprovider
```

Success: `N passed, M skipped` with no failures and no errors, where every skip is one of three kinds, and `-rs`
lists them: a GPU test (`rollout graphs need a GPU`, `the fused GRU call runs on the GPU`), `test_human_reader` without
its capture sample, `test_stage_validation` without a configured build to borrow compile flags from. (At the cleanup:
888 passed, 13 skipped, 7 slow deselected.) `test_conf_prune.py::test_between_the_tag_and_head...` needs git to read
the tag: it runs from a plain checkout, and from a worktree too (it finds the worktree's git directory); it skips with
its reason otherwise. `-m ""` also runs the
slow multi-process tests; run them once before a deploy that touches the learner.

## 3. The GPU pytest

The same suite on a card, so the GPU tests run instead of skipping. Use a card nothing is training on
(`HIP_VISIBLE_DEVICES=0` is the dev machine's; the cluster's cards are busy):

```
docker exec -e HIP_VISIBLE_DEVICES=0 -w /azerothcore/apps/forge/python ac-animus-forge-dev-server \
   .venv/bin/python -m pytest -q -rs -p no:cacheprovider; echo exit=$?
```

Success: `exit=0`, a summary line `N passed` with **zero failures and zero skips caused by a missing GPU** (`-rs` prints
no skip whose reason says a GPU is needed), and no `Segmentation fault`. The fused-GRU tests (`test_recurrent.py`, the ones
marked `requires_gpu`) once crashed the interpreter; a segfault ends pytest with no summary line at all, so **a run
that prints no summary is a failure**, not a pass. Run it a second time: it must be the same.

## 4. Build the new revision and compare stage.json with the old build's

`stage.json` is what the learner resumes against: block spans, revisions, widths, action names, the reward terms, the
tuning in force, the arenas. The sim writes one per stage to `<OutputDir>/layouts/<stage>/stage.json` whenever it builds
the stage. The new build must be run **on the dev machine, with an output directory of its own**, never on sarah and
never with a cluster's output directory: the files and the run directories there are the live run's.

```
AC_ANIMUS_FORGE_OUTPUT_DIR=/azerothcore/var/animus-forge/gate ./forge.sh --build
```

(Not run in the session that wrote this page. `AnimusForge.*` keys can be set in a container's environment as `AC_...`
variables (`forge_classes.py`'s docstring); if `docker-compose.yml` does not pass this one through, set
`AnimusForge.OutputDir = /azerothcore/var/animus-forge/gate` in the dev machine's own `mod_animus_forge.conf`
instead. `--build` recreates the dev machine's worldserver container: cancel anything it is training first.
The first thing to check is that `var/animus-forge/gate/layouts/` fills and `var/animus-forge/shared/` does not change.)

`--build` compiles the worldserver from the current source (minutes to an hour), and returns when it is installed.
Attach (`./forge.sh attach`; detach with Ctrl-P Ctrl-Q, never Ctrl-C), and for each live stage type, one at a time, and
wait for the plan to end before the next:

```
forge run move1_controls random 1
```

`forge run <stage> <policy> <episodes>` builds the scenario, which writes its `stage.json`, and plays one episode
with the sim's own random policy: no learner starts and nothing is trained, scored or compared (principles.md, 14). Do
**not** use `forge start <stage>` for this on an output directory that holds a run you want: it archives the stage's
previous run. (A `forge` command that writes only stage.json does not exist; this is the cheapest way, and the thing
`forgectl` should wrap.)

The live stages: `move1_controls move2_seek move3_interact move4_follow combat1_fight combat2_packs combat3_survive
group1_roles group2_corridor dungeon1_pulls dungeon2_ragefire dungeon3_deadmines`.

Success: 12 files, `ls var/animus-forge/gate/layouts/*/stage.json | wc -l` prints 12.

The old build's files: a stage that has run has one in its run directory (`runs/<stage>/stage.json` on the host, copied
out); a stage that has not run needs the tagged build to write it the same way (check the tag out in a second
worktree, build it with another `AC_ANIMUS_FORGE_OUTPUT_DIR`, and repeat the loop). Put them in `var/gate/old/<stage>/`.
Then, per stage:

```
python3 apps/forge/tools/stage_json_diff.py var/gate/old/<stage>/stage.json \
    var/animus-forge/gate/layouts/<stage>/stage.json \
    --allow-removed-terms --allow-removed-keys --allow-removed-columns
```

Success: exit 0, and every line under `ALLOWED` is something the build was meant to remove (the cleanup removes
scripted-player and first-curriculum reward terms, tuning keys, episode columns and arena fields). **Any other group
fails the step**: `layouts`, `layout-shape`, `actions`, `specs`, `blocks` and `obs-names` mean a resume would be refused
or would read old weights as new columns (do not deploy, or plan a seeded restart); `reward-terms` category changes, an
added term or a changed `tuning` value mean the run would be paid differently; `header`, `categories`, `other` need
reading. Run it without the flags first to see everything.

Also re-verify the metric extraction against the real files (a stale reader would otherwise pass step 2):

```
python3 apps/forge/tools/sim_metrics.py --check var/animus-forge/gate/layouts/*/stage.json
```

Success: exit 0 (it lists any column a build reports that the C++ reader missed; "extraction has N exact the file lacks"
is the conditional columns of a stage and is not a failure).

## 5. Resume dry run for the current stage

Copy the live run's checkpoint out (never point a tool at the live directory) and run the dry resume against the new
build's `stage.json`, with the stage's learner yaml from the new revision:

```
mkdir -p var/gate/move2_seek
scp sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek/latest.pt var/gate/move2_seek/
scp sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek/stage.json var/gate/move2_seek/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/resume_check.py var/gate/move2_seek \
    --stage-json var/animus-forge/gate/layouts/move2_seek/stage.json
```

(`scp` of the old stage.json is only for reading; the tool compares the checkpoint's own saved stage with the new one.)
Success, exit 0 and a last line `OK: every check that can run here passed`, with these lines `PASS`:
`resume compatibility` (**empty**), `layout check` (**empty**), both networks with `missing 0 / unexpected 0 / wrong shape
0`, `MappoTrainer.load_state_dict`, and `evaluation state`. The `fade ladder` line shows the rung, falls, held and
settled the resumed run will start with: compare them with `forge status` on the host. Config values that differ from
the checkpoint's are listed; keys the cleanup removed (`mappo.hint_coef`, `eval.opponent_baseline`) show as
`saved ... -> now "<absent>"` and are harmless. Two lines are `UNVERIFIED` by design: the sim's SPEC (the numbers
`stage.json` does not carry; pass `--spec <a spec.json the new build's learner wrote>` once you have one) and one rollout
step (it needs the sim): step 8's first updates are that check.

## 6. Stop the stage, back it up, prune the confs

Nothing below can happen while the old worldserver runs a stage: step 7 recreates every worldserver container. On the
host console (`docker attach ac-animus-forge-worldserver`):

```
forge cancel
```

Wait for `Plan ended: cancelled` (the learner has saved `latest.pt`). `forge pause` is not enough, and a worker whose
console you did not touch keeps running. Then back the run up and take the last headline readings that step 8 compares
with:

```
ssh sarah@192.168.0.68 'cp -a ~/animus-forge/var/animus-forge/shared/runs/move2_seek ~/animus-forge/var/backups/<date>-pre-deploy'
rsync -a --exclude camera --exclude tb --exclude videos \
    sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek var/gate/before/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/run_snapshot.py var/gate/before/move2_seek --json var/gate/before.json
```

Success: the backup exists, and `run_snapshot.py` prints the stage's headline columns, the medians of the last updates'
`approx_kl` and `env_steps_per_sec`, and the rung.

**Conf pruning.** Each machine's `env/dist/etc/modules/mod_animus_forge.conf` (and `worldserver.conf`) holds
`AnimusForge.*` keys the new build no longer reads. They are harmless (see "Unknown keys" below), so pruning is
housekeeping, but it removes any doubt about which keys act. What the build removed, from the template:

```
python3 apps/forge/tools/conf_prune.py --removed pre-cleanup-2026-10-07 HEAD
```

For the cleanup: **378 keys**, none added. By group: Curriculum.Pulls 73, Travel 37, StandIn 33, Life 27,
ScriptedPlayers 22, Owner 19, Follow 16, MarkerVertical 15, MarkerWater 15, Flag 14, Markers 13, MarkerGround 13,
MarkerMounted 12, MarkerRoutes 12, Dummy 10, Director 7, Death 5, Instance 5, Ambush 4, Duel 4, Opponent 4, Order 4,
Raid 3, Evade 2, Support 2 (371 in `AnimusForge.Curriculum.*`), plus `AnimusForge.Human` 4, `AnimusForge.Stage`
(`teacher_ragefire` and `teacher_deadmines` `.TicksPerDecision`) 2 and `AnimusForge.TravelPools` 1.
The template diff **is** the tuning's `Visit` diff: `test_conf_covers_tuning.py` makes `worldserver.conf.dist` and
`CurriculumTuning::Visit` agree both ways, and `test_conf_prune.py` checks it between the tag and HEAD.

For every machine, first look, then prune (`--old` says which unknown keys this build removed and which the old build
did not read either; keys of stages that no longer exist are listed too):

```
for m in sarah@192.168.0.68 spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117; do
  for f in env/dist/etc/modules/mod_animus_forge.conf env/dist/etc/worldserver.conf; do
    python3 apps/forge/tools/conf_prune.py --check --ssh $m:'~/animus-forge/'$f --old pre-cleanup-2026-10-07
  done
done
```

(a machine without one of the two files is an `ssh ... could not read` error for that file; skip it.) `--check` exits 1
while unknown keys remain. Then `--prune` instead of `--check`: each unknown assignment is commented out in place
(`#pruned <stamp> (<why>): <the original line>`), never deleted, after `<file>.bak-<stamp>` is written on that machine.
The backup is how step 9 restores a tuned value. Success: a second `--check` per file ends `0 unknown key(s)` and exits 0.

Last, the keys that remain must be the same on every machine (the fingerprint hashes their effective values):

```
for m in sarah@192.168.0.68 spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117; do
  ssh -o BatchMode=yes $m 'cd ~/animus-forge/env/dist/etc && cat worldserver.conf modules/mod_animus_forge.conf 2>/dev/null \
     | grep -E "^[[:space:]]*AnimusForge\.Curriculum\." | sed "s/[[:space:]]//g" | sort' > var/gate/curriculum.$m.txt
done
sha256sum var/gate/curriculum.*.txt
```

Success: the same hash on all four (a worker with a different tuned value is refused by the host after step 7; this finds
it first). A difference in a *removed* key does not matter; one in a kept key does.

## 7. Push, pull, rebuild every machine

```
git push lan forge && git push origin forge
for m in sarah@192.168.0.68 spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117; do
  ssh -o BatchMode=yes $m 'cd ~/animus-forge && apps/forge/tools/cluster-pull.sh moloch@192.168.0.69'
done
```

`cluster-pull.sh` pulls `forge` (fast-forward only), pulls the probe data, touches `env/dist/.forge-build` and
recreates the worldserver container, which builds from source with `-march=native`; the slowest machine takes longest.
Watch each build finish:

```
ssh sarah@192.168.0.68 'docker logs --tail 5 ac-animus-forge-worldserver'
```

Success, on every machine: the log shows `AzerothCore rev. <the new short sha> ... ready`, no `error:` lines, and:

```
for m in ...; do ssh $m 'docker logs ac-animus-forge-worldserver 2>&1 | grep "Cluster fingerprint" | tail -1'; done
```

prints **identical** lines (`src=... protocol=25 fields=.../... curriculum=... decision=...`) on the host and every
worker. A worker that differs is refused (`Cluster: refused the worker at ...` in the host's log); compare the five
parts to see which differs (a stale checkout: `src`; the probe data: `fields`; tuned curriculum keys: `curriculum`).
The `curriculum=` value differs from before the deploy on every machine, because the removed keys left the hash:
that is expected, only equality across machines matters.

## 8. Resume the stage on the new build and read the first numbers

On the host console:

```
forge resume move2_seek
```

Success, in the host's learner log: `Resumed move2_seek from .../latest.pt at update <U>, <E> env steps` with the update
and env steps the checkpoint had (step 5 printed them), the workers joining (`N worker learners join this run`), and
updates arriving. **The first resume after a deploy reports a changed tuning fingerprint and rebuilds the baseline
cache; that is harmless** (the cache key includes the tuning in force, and the tuning's key list changed).

After the first evaluation (`eval.every_env_steps`: 10M env steps for M2, about 40 minutes) and at least 20 updates:

```
rsync -a --exclude camera --exclude tb --exclude videos \
    sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek var/gate/after/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/run_snapshot.py var/gate/after/move2_seek --json var/gate/after.json
DEV apps/forge/python/.venv/bin/python apps/forge/tools/run_snapshot.py --compare var/gate/before.json var/gate/after.json
```

Success: the new build continues where the old one stopped. The evaluation's headline columns (the stage's
`status.headline`, with its targets) within their noise of the old build's last (the score's `stderr` is printed);
`approx_kl` of the same order (not 2x either way); `env_steps_per_sec` and `update_seconds` where the same machines put
them (a cleaned build should not be slower; a load difference explains small ones); `wall_seconds` and the ladder rung
unchanged (a resumed ladder restarts its window, so a step may come a little later). A run whose first evaluation falls
well below the old one, or whose KL jumps, is **not** a pass: pause, and go to step 9 if the cause is not obvious.

Only then start the next stage (`forge start <next stage>` archives the old run of that stage; for M2 the next stage
starts from M2's checkpoint when the convergence signals end it). Never `forge start` the stage you just resumed.

## 9. Rollback

Rolling back is the tag plus the confs, on every machine, with the stage cancelled first (`forge cancel`):

```
ssh <machine> 'cd ~/animus-forge && git fetch moloch@192.168.0.69:git/animus-forge.git --tags && git checkout pre-<change>-<date>'
ssh <machine> 'cd ~/animus-forge/env/dist/etc && for f in modules/mod_animus_forge.conf worldserver.conf; do \
   b=$(ls -t $f.bak-* 2>/dev/null | tail -1); [ -n "$b" ] && cp -p "$b" $f; done'
ssh <machine> 'cd ~/animus-forge && touch env/dist/.forge-build && docker compose up -d --force-recreate ac-worldserver'
```

(`tail -1` of `ls -t` is the oldest backup, the file as it was before the first prune; restore the file named in
step 6's output if there are several.) Then the fingerprint check of step 7 and `forge resume <stage>`. The run
directory needs nothing if the new build only trained on: its `latest.pt` has the old shapes (step 4 proved that), and
the old build resumes it; if a run was damaged, restore `var/backups/<date>-pre-deploy` over it with the stage cancelled.
When the machines are back on `forge` afterwards, `git checkout forge` before the next `cluster-pull.sh` (it pulls
fast-forward only and a detached checkout cannot).

## Unknown keys in a live conf are harmless

A conf that still holds keys the new build dropped neither refuses startup nor changes the cluster fingerprint.
From the source:

- **The loader does not look for unknown keys in these files.** `ConfigMgr::AddKey` rejects a key only when the file
  was loaded as *optional* (`src/common/Configuration/Config.cpp:245`, `if (isOptional && itr == _configOptions.end())`).
  `worldserver.conf` is loaded by `LoadInitial` with `isOptional = false` (`Config.cpp:455`); the modules' confs by
  `LoadModulesConfigs` with `false` (`Config.cpp:770`); and the forge's own `modules/mod_animus_forge.conf` by
  `ForgeMain.cpp:361`, `LoadAdditionalFile(forgeConf, false)`, deliberately in full (the comment at
  `ForgeMain.cpp:354-358`: loaded as optional the config drops every key its templates do not define, which broke the
  per-stage keys). There is no template of known keys at run time; `worldserver.conf.dist` is not loaded.
- **Even where the check applies, it does not stop startup.** An unknown option is logged at
  `ConfigPolicy::unknownOptionSeverity`, `Error` by default (`src/common/Configuration/Config.h:40`), and skipped
  (`Config.cpp:247-249`). It aborts only when the core is compiled with `CONFIG_ABORT_INCORRECT_OPTIONS`
  (`Config.cpp:251`), an option that defaults to 0 (`conf/dist/config.cmake:138`) and that nothing in the tree turns on.
- **The curriculum keys are read by name, never scanned.** `CurriculumTuning::Load` asks for each key `Visit` lists, with
  its default (`Scenario/Curriculum/CurriculumTuning.cpp:58-71`, `sConfigMgr->GetOption(prefix + key, value, false)`):
  a key that `Visit` does not list is never requested. The only prefix scans are
  `GetKeysByString("AnimusForge.Stage.")` in `ForgeConfig.cpp:184` and `:230`, which keep entries ending `.Envs` or
  `.TicksPerDecision` by stage name: a leftover for a removed stage becomes a map entry nobody looks up (and a
  `.TicksPerDecision` that does not divide `DecisionMs` logs an error and is skipped, `ForgeConfig.cpp:239-243`).
- **The fingerprint hashes only known keys.** `ClusterFingerprint` (`src/server/game/Animus/AnimusForge.cpp:77-107`)
  is `src` (the hash of the forge's sources), `protocol`, `fields` (the probe data's file count and bytes),
  `curriculum` and the decision timing. `curriculum` is the FNV-1a hash of
  `CurriculumTuning::Load("AnimusForge.Curriculum.").Json()` (`AnimusForge.cpp:93-96`), and `Json()` is `Visit` again
  (`CurriculumTuning.cpp:96-115`): the effective value of every known key, written or defaulted. An unknown key is in
  neither. The converse matters: a *known* key with different values on two machines changes `curriculum`, which is why
  step 6 compares them.

So a stale key can only mislead a person into thinking it still acts; that is all `conf_prune.py` removes. No GTest was
added: a test of the loader needs the unit-test build (step 2) to run, and a test written without running it would
break the next deploy's build; `test_conf_prune.py` pins the tool, and the source references above are the evidence.
