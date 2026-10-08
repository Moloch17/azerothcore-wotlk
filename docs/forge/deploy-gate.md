# The deploy gate

The ordered checks a person runs before a new build goes to the cluster, and the steps that put it there. Written for
the cleanup deploy (the `cleanup` branch onto the build tagged `pre-cleanup-2026-10-07`, while `move2_seek` trains on
sarah) and reusable for any later one: replace the tag and the stage name. Every step has the command and what success
looks like.

> ### Stop conditions (read before step 0)
>
> - **Stop at the first step that does not match its success line.** Do not go on "to see what happens".
> - **Write down what you saw** (the command, its output, the time) in `var/gate/NOTES.txt`.
> - **Do not improvise a fix mid-deploy.** Apply the step's row in [the failure table](#the-failure-table) and nothing
>   else. A fix that is not in the table waits for the owner.
> - **A cancelled, backed-up cluster is a safe state.** If you are past step 6 and unsure, stay there: M2 resumes from
>   its `latest.pt` on whatever build the machines are on, the backup from step 6 is on sarah, and nothing is lost
>   but time.
> - **Who decides:** the gate has one owner for go and no-go, **the project owner (Alex Trowell)**. Tell them a deploy
>   is starting (step 0). If they are away, nobody decides mid-gate: each step's row in the failure table says what to
>   do without them, and "stop" means stop in a safe state, not "ask someone else".

Commands run from the repository root on the dev machine unless a line says `ssh`. `DEV` below means the dev container,
`docker exec -w /azerothcore claude-syntax` (the dev container; `ac-animus-forge-dev-server` is the dev
machine's worldserver build container and `--build` recreates it); its Python is
`apps/forge/python/.venv/bin/python`. The machines are `sarah@192.168.0.68` (host) and the workers
`spencer@192.168.0.66`, `thomas@192.168.0.67` and `moloch@192.168.0.117` (cluster.md); each keeps its checkout at
`~/animus-forge`. The cluster itself (machines, users, ports, how a stage is started) is in [cluster.md](cluster.md);
the rules a change must keep are in [principles.md](principles.md).

## forgectl, and the manual fallback

`forgectl` ([forgectl.md](forgectl.md), `./forgectl --help`, on the branch `worktree-agent-ad539c94cd2d2a8f4` until it
is merged) does most of the cluster steps below in one command each. Each step names its `forgectl` command and keeps
the manual commands as the fallback: use the fallback when `forgectl` is not on the branch you are deploying from, or
when it does something the step's success line does not match. Every `forgectl` command that changes a machine prints
a plan and asks `Proceed? [y/N]`; read the plan against the step.

| Step | forgectl | Manual fallback in the step |
|---|---|---|
| 0 pre-flight, and the look at the cluster after every later step | `forgectl cluster` (add `forgectl status`, `forgectl logs <machine> --errors`) | `ssh` + `docker logs`, step 0 |
| 6 stop the stage | `forgectl stage cancel` (the host, then every worker's console) | `forge cancel` in each console |
| 6 conf keys equal | `forgectl conf-sync --check` (read-only; `conf-sync` writes the workers' Curriculum keys) | the `grep`/`sha256sum` loop |
| 7 push, pull, rebuild | `forgectl build --cluster` (pushes, rebuilds all machines in parallel, waits for each `ready`) | the `for` loop over `cluster-pull.sh` |
| 8 resume | `forgectl stage resume move2_seek` | `forge resume move2_seek` in the host console |
| 9 rollback | `forgectl stage cancel`, then `forgectl build --cluster` on the tag's branch | the `ssh` lines in step 9 |

**First use of forgectl's state-changing commands (do these before gate day; "Rehearsals" below).** They have never been
used on this cluster. The reversible ones are exercised once, supervised, so that the gate is not their first run:
`forgectl stage pause` then `forgectl stage resume` on the running M2, and a local `forgectl build` on the dev machine.
Only then is `forgectl build --cluster` allowed to be used on the gate, and **never first-use `build --cluster` or a
`conf-sync` write without a read-only `forgectl conf-sync --check` the same day**.

| Tool (in `apps/forge/tools`) | What it answers | Test |
|---|---|---|
| `sim_metrics.py` | which metric names a stage's sim produces (read from the C++); `--check stage.json` compares that with a built binary's own list | `test_metric_names.py` |
| `stage_json_diff.py old new` | what a new build changed about a stage's layouts, columns, reward terms, tuning, arenas | `test_stage_json_diff.py` |
| `resume_check.py` | will this checkpoint resume on this build (a dry run of `forge resume`, on CPU, no sim); `--fresh`: does a stage that never ran start on the learner | `test_resume_check.py` |
| `conf_prune.py` | which `AnimusForge.*` keys a build dropped; which keys of a machine's conf it no longer reads; `--list-backups` and `--restore <stamp>` | `test_conf_prune.py` |
| `run_snapshot.py` | a run's headline, KL, steps/s in one table, and before/after | `test_run_snapshot.py` |

## Expected durations

Estimates, so that a long wait is recognised as normal. Replace them with the measured numbers after the rehearsals
(the local `forgectl build` gives the dev build's time; the rollback rehearsal gives one worker's). Allow **a whole
working day, 6 to 9 hours, and do not start in the evening**: the cluster is cancelled from step 6 to step 8.

| Step | Expected | A wait that is still normal |
|---|---|---|
| 0 pre-flight | 10 min | |
| 1 tag, style | 5 min | |
| 4 dev build (`./forge.sh --build`) | 30 to 60 min (`-march=native` worldserver compile) | no output while the container compiles |
| 4 twelve `forge run <stage> random 1` | 1 to 3 min a stage; a dungeon stage up to 10 min (it plays a whole episode) | `forge status` shows the episode count |
| 4 `stage_json_diff`, `sim_metrics --check`, `resume_check --fresh --all` | under 5 min together (the trainers build on CPU: a minute for the largest) | |
| 5 resume dry run | 2 to 5 min | |
| 6 cancel, backup, snapshot, prune, key check | cancel under 2 min; backup 5 min; prune and checks 10 min | `Plan ended: cancelled` waits for the learner's last save |
| 7 rebuild every machine | one machine 20 to 60 min (the slowest is the 8-thread spencer); `build --cluster` runs them in parallel, so the slowest sets the time; the serial `for` loop is the **sum**: up to 3 to 4 hours | `ready` is a log line, not an exit |
| 8 resume | 5 min to the first update; **the first evaluation at 10M env steps is about 40 minutes later** | the first evaluation after a resume is the long wait of the gate |
| 8 the 20 updates and the comparison | with the first evaluation | |

## Pre-flight, one screen (step 0)

Do this the morning of the gate, and again just before step 6. All of it must be true; if any line is not, it is a
**stop** (failure table, step 0).

```
forgectl cluster                      # fallback: for each machine, the ssh lines below
ssh -o BatchMode=yes <m> 'git -C ~/animus-forge rev-parse --short HEAD; df -h ~/animus-forge | tail -1'
```

- [ ] **Four machines, one revision, all on the tagged sha.** `forgectl cluster` lists sarah (host), spencer, thomas and
  moloch, each with a `rev` equal to the sha the tag will name (step 1) and **no `*` after it**, each worldserver `up`
  and each learner `stepping` (not `STALLED`, not `no log`). The footer has no `refused the worker` line.
- [ ] **M2 is training.** `forgectl status` (or `forge status` in the host console) shows `move2_seek` running, an
  update number and env steps that advance between two looks a minute apart, and the ladder rung you expect.
- [ ] **Nobody else's job on the cluster.** No other stage queued (`forge status` shows no other plan), no other
  `forge fast`, `bench` or `run`, and no person logged in to a machine doing work there (`who`, `docker ps` on each).
  Ask in the channel the cluster is discussed in; silence is not a yes: if anyone answers, wait.
- [ ] **Disk on every machine: at least 30 GB free** (`df`; `forgectl cluster` prints it). The rebuild writes a compile
  tree and an image layer (the dev machine's equivalent, a RelWithDebInfo tree, is 1.6 to 1.7 GB of objects and the
  container's build cache holds many times that), the backup of a run is its whole `runs/move2_seek` (without the camera
  and tensorboard folders, ask `du -sh` on sarah first and keep three times that free), and the conf backups are tiny.
  The cluster's smallest disks are sarah's 221 GB and spencer's 228 GB; if one has under 30 GB free, **clear it before
  the gate** (`docker builder prune` on that machine, old `archive/` runs), never during.
- [ ] **The dev card is idle and nothing else uses the dev build directory.** `rocm-smi` on the dev machine shows no
  process on the card (a busy card spoils any measurement), and no other agent, forge run
  or build is using the dev GPU or `var/gate-build` / `var/animus-forge/gate` (`docker ps`, `ps aux | grep -E
  'forge|cmake|pytest'`).
- [ ] **The owner has been told** a deploy is starting, at what time, and that M2 will be cancelled for about
  `<duration>` hours; their reply (or that they are away) is written in `var/gate/NOTES.txt`.
- [ ] **Rehearsals done** (next section): pause/resume on M2, local `forgectl build`, the rollback rehearsal on one
  worker, and a read-only `conf_prune --check --ssh` on every machine. Each result is in `var/gate/NOTES.txt`.
- [ ] `var/gate/` exists on the dev machine and is empty of last gate's files (`mkdir -p var/gate`; move old ones
  aside).

## Rehearsals (before gate day; reversible)

These are the first uses of tools the gate depends on. Each is supervised and leaves the cluster as it found it. **The
rollback rehearsal is the first thing that should work when something goes wrong, so it is rehearsed first.**

**R1. Rollback rehearsal on ONE idle, stopped worker** (you cannot run this from this page; it is a checklist for a
person). Choose the weakest worker (spencer). The aim is to prove, on a machine that holds nothing the run needs, that
step 9's lines work: the tag checks out, a conf comes back from its backup, the container recreates and the
fingerprint equals the host's.

- [ ] Write down the machine's revision (`git -C ~/animus-forge rev-parse HEAD`) and `git status --short` (it must be
  clean; if not, stop and look).
- [ ] Stop **only that worker**: `ssh <worker> 'docker stop ac-animus-forge-worldserver'`. Confirm on the host
  (`forgectl status` or `forge status`) that M2 carries on without it (updates keep arriving for 5 minutes). If M2
  stalls, `docker start` the container at once and stop the rehearsal: report it, do not retry.
- [ ] Check the tag is on the worker: `git fetch moloch@192.168.0.69:git/animus-forge.git --tags && git tag -l
  'pre-*'`. (Rehearse with the *current* tag, which is what the worker runs, and the *new* revision if it is already
  pushed: either way the point is the sequence.)
- [ ] Take a conf backup the way the deploy will: `python3 apps/forge/tools/conf_prune.py --prune --ssh
  <worker>:'~/animus-forge/env/dist/etc/modules/mod_animus_forge.conf'` (with the checkout's template: it comments out
  the keys the new build dropped). **Copy the printed stamp and the `to undo:` line into `var/gate/NOTES.txt`**, and
  run `--list-backups` on the same file: the stamp must be listed.
- [ ] Check the tag out on the worker: `ssh <worker> 'cd ~/animus-forge && git checkout pre-<change>-<date>'`.
- [ ] Restore the conf **by the stamp you wrote down**: `conf_prune.py --restore <stamp> --ssh <worker>:<path>`, then
  `diff` it against the pre-prune copy you took (`--list-backups` names it; the restore also keeps the pruned file as
  `<conf>.pre-restore-<time>`). The diff must be empty.
- [ ] Recreate: `ssh <worker> 'cd ~/animus-forge && touch env/dist/.forge-build && docker compose up -d --force-recreate
  ac-worldserver'` (the compose service is `ac-worldserver`, the container `ac-animus-forge-worldserver`). Wait for the
  `AzerothCore rev. <sha> ... ready` line (timed: this is the number for the durations table).
- [ ] Fingerprint: `docker logs ac-animus-forge-worldserver 2>&1 | grep "Cluster fingerprint" | tail -1` on the worker
  equals the host's line (all five parts). A mismatch is a finding to give the owner **before** the gate.
- [ ] Put the checkout back to `forge` (`git checkout forge`), `forgectl cluster` shows the worker on the same rev as
  the others. The worker rejoins the run at the next resume; if it does not rejoin on its own that is normal: say so
  in the notes.

**R2. `forgectl stage pause` then `stage resume`, on the running M2, once.** It proves the console path to all four
machines. `forgectl stage pause` (read its plan: it names the host and every worker), wait for the learner to stop
advancing (`forgectl cluster` shows it), then `forgectl stage resume`. Success: every machine answered, `forgectl
cluster` shows all four `stepping` within 2 minutes, and M2's `update` number carries on. Any machine that did not
answer is a finding; `forge pause`/`forge resume` by hand in that machine's console is the fallback the gate uses.

**R3. A local `forgectl build` on the dev machine** (not `--cluster`): it recreates the dev machine's worldserver
container, so cancel anything the dev machine trains first, and do it when step 4's dev build is needed anyway (it is
the same build). Success: `forgectl build` ends with the container's `ready` line for the dev checkout's revision.
Only after this does the gate use `forgectl build --cluster`. The manual cluster-pull flow (step 7's loop) is the
documented fallback; use it if R3 or the same-day `--check` shows anything unexpected.

**R4. Read-only checks the same day.** `forgectl conf-sync --check` (all workers match the host's Curriculum keys),
and `conf_prune.py --check --ssh <m>:<path> --old <tag>` for **both conf files of every machine**. `conf_prune --ssh`
has never run on a machine: its first use is the read-only `--check`; **`--prune` is run only in step 6, after the
stage is cancelled**.

## 1. Tag what is deployed; style

The tag is the rollback (step 9) and the baseline for every comparison below. Tag the revision the cluster runs, not
the one you are about to deploy:

```
ssh sarah@192.168.0.68 'git -C ~/animus-forge rev-parse HEAD'      # the deployed revision (every machine: same)
git tag pre-<change>-<date> <that revision>                          # e.g. pre-cleanup-2026-10-07
git push lan pre-<change>-<date> && git push origin pre-<change>-<date>
python3 apps/codestyle/codestyle-cpp.py
git rev-parse cleanup | tee var/gate/tested-sha.txt      # the revision every step tests; step 7 checks it
```

Success: the tag is on `lan` and `origin` (`git ls-remote lan 'refs/tags/pre-*'`), the codestyle run ends with
`Everything looks good` (exit status 0), and `var/gate/tested-sha.txt` holds the 40-character sha of the branch under
test. **If the branch moves after this line, the gate starts again at step 1.**

## 2. and 3. Tests: none (removed 2026-10-07)

The owner removed every forge test (the 46 GTest files under `src/test/server/game/Animus/` and the Python suite under
`apps/forge/python/tests/`, 2026-10-07: "I would prefer to just not have any tests at all for now"). Nothing runs them
and nothing in the gate depends on them. They are kept in git history: `git checkout archive/with-tests -- <path>`
brings any of them back.

What stands in for them in this gate: step 4 (the build compiles; the stage.json files are diffed against the old
build's), step 5 (`resume_check.py` on the real checkpoint) and step 8 (the first minutes of the resumed run). The
rollout-graph code can only be confirmed by that last step: look for the `rollout graph captured` log line (and its
batch shape) in the first minutes. If it says graphs are NOT used, that is a throughput loss, not a crash: note it.

## 4. Build the new revision and compare stage.json with the old build's

`stage.json` is what the learner resumes against: block spans, revisions, widths, action names, the reward terms, the
tuning in force, the arenas. The sim writes one per stage to `<OutputDir>/layouts/<stage>/stage.json` whenever it builds
the stage. The new build must be run **on the dev machine, with an output directory of its own**, never on sarah and
never with a cluster's output directory: the files and the run directories there are the live run's.

```
AC_ANIMUS_FORGE_OUTPUT_DIR=/azerothcore/var/animus-forge/gate ./forge.sh --build
```

(`forgectl build` is the same build with a plan and a wait for `ready`; R3 above is its first use. Not run in the
session that wrote this page. `AnimusForge.*` keys can be set in a container's environment as `AC_...`
variables (AzerothCore's config reads `AC_`-prefixed environment variables); if `docker-compose.yml` does not pass this one through, set
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
with the sim's own random policy: no learner starts and nothing is trained, scored or compared (principles.md, 14);
the only thing it is used for is the scenario constructor. Do **not** use `forge start <stage>` for this on an output
directory that holds a run you want: it archives the stage's previous run.

**Follow-up, not part of this deploy.** A command that writes only the files, `forge stagefiles <stage>|all`, is built
and tested on the branch `stagefiles` (idle-only): it builds each scenario exactly as starting
it does, writes `layouts/<stage>/stage.json` and the manifests, and drops the scenario, with no episode and no learner.
It is **not in this deploy's build**: a new console command is a new variable in the build the gate verifies. Merge it
after the deploy; the next gate's step 4 is then `forge stagefiles all` once, with `forge run <stage> random 1` kept
as the fallback.

The live stages: `move1_controls move2_seek move3_interact move4_follow combat1_fight combat2_packs combat3_survive
group1_roles group2_corridor dungeon1_pulls dungeon2_ragefire dungeon3_deadmines`.

Success: 12 files, `ls var/animus-forge/gate/layouts/*/stage.json | wc -l` prints 12.

**What is mandatory and what is recommended.** The comparison with the old build's file (below) is **mandatory for M1
and M2** (they have real old files and checkpoints: a mismatch there is a resume that would be refused or would read
old weights as new columns). It is **recommended for the other ten** (the layout pin test, `LiveLayoutPinTest`,
already guards their layouts, and no checkpoint exists that a mismatch could break); their new files are needed
anyway, for the metric check and for `resume_check.py --fresh --all` below. A difference in one of the ten is read and
recorded, and is not by itself a stop unless it also fails one of the checks that are mandatory.

The old build's files: a stage that has run has one in its run directory (`runs/<stage>/stage.json` on the host, copied
out); a stage that has not run needs the tagged build to write it the same way (check the tag out in a second
worktree, build it with another `AC_ANIMUS_FORGE_OUTPUT_DIR`, and repeat the loop). Put them in `var/gate/old/<stage>/`.
Then, per stage (M1 and M2 first):

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

And the learner side of **all twelve stages**, from their yamls and the files just written. M3 to D3 have never run on
the learner (the M4 held-out bug was found only because a test read the yamls); this is the only check that they start:

```
DEV apps/forge/python/.venv/bin/python apps/forge/tools/resume_check.py --fresh --all \
    --stage-json-dir var/animus-forge/gate/layouts
```

(one stage: `--fresh --stage move3_interact --stage-json var/animus-forge/gate/layouts/move3_interact/stage.json`.)
Success: twelve lines `PASS` and `12 of 12 stages start on the learner`, exit 0. A `FAIL` line names the stage, the
check (held-out arenas, `eval.mask_actions`, the ladders' gate column, the trainer) and the first error.
`--fresh` builds the spec from `stage.json`; the sim's state width (1958) and goal count (348) are constants in the tool
that a test pins against the real M2 checkpoint (`--state-dim`, `--goal-count` override them if the sim's change).

## 5. Resume dry run for the current stage

This is the **first** resume check, on a copy of the checkpoint taken while the run trains. It is repeated on the final
checkpoint in step 6. Copy the live run's checkpoint out (never point a tool at the live directory) and run the dry
resume against the new build's `stage.json`, with the stage's learner yaml from the new revision:

```
mkdir -p var/gate/move2_seek
scp sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek/latest.pt var/gate/move2_seek/
scp sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek/stage.json var/gate/move2_seek/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/resume_check.py var/gate/move2_seek \
    --stage-json var/animus-forge/gate/layouts/move2_seek/stage.json
```

(`scp` of the old stage.json is only for reading; the tool compares the checkpoint's own saved stage with the new one.)
Success, exit 0 and a last line `OK: every check that can run here passed`, with these lines `PASS`:
`resume compatibility` (**empty**), `layout check` (**empty**), both networks with `missing 0 / unexpected 0 / wrong
shape
0`, `MappoTrainer.load_state_dict`, and `evaluation state`. **The evaluation-state line must show
`plateau_env_steps None` and `lr_scale 1`** (the learning rate the resumed run starts at is `actor_lr` times that),
**and
the printed note that the checkpoint's `plateau_env_steps` was cleared** (`NOTE: plateau_env_steps was <N> in the
checkpoint and is None after the restore ... a gate-stepped ladder's easier rungs`). That note and those two values are
how you know the convergence fix is active on the real checkpoint. If the line reads `plateau_env_steps <N>` and
`lr_scale` below 1 (the old build's M2 checkpoint reads 30,255,104 and 0.557), **the build does not carry the
convergence fix: stop** (failure table). The `fade ladder` line shows the rung, falls, held and
settled the resumed run will start with: compare them with `forge status` on the host. Config values that differ from
the checkpoint's are listed; keys the cleanup removed (`mappo.hint_coef`, `eval.opponent_baseline`) show as
`saved ... -> now "<absent>"` and are harmless. Two lines are `UNVERIFIED` by design: the sim's SPEC (the numbers
`stage.json` does not carry; pass `--spec <a spec.json the new build's learner wrote>` once you have one) and one
rollout
step (it needs the sim): step 8's first updates are that check.

## 6. Stop the stage, check the final checkpoint, back it up, prune the confs

Nothing below can happen while the old worldserver runs a stage: step 7 recreates every worldserver container, and the
live learner imports some Python modules lazily, so **no file under a running learner may change** (this includes the
`git merge` in step 7: it goes after this cancel). `forgectl stage cancel` (host, then every worker's console), or on
the host console (`docker attach ac-animus-forge-worldserver`) and each worker's:

```
forge cancel
```

Wait for `Plan ended: cancelled` (the learner has saved `latest.pt`) on the host. `forge pause` is not enough, and a
worker whose console you did not touch keeps running (`forgectl stage cancel` reaches them all; `forgectl cluster`
should then show no learner `stepping`). Then back the run up and take the last headline readings that step 8 compares
with:

```
ssh sarah@192.168.0.68 'cp -a ~/animus-forge/var/animus-forge/shared/runs/move2_seek \
    ~/animus-forge/var/backups/<date>-pre-deploy'
rsync -a --exclude camera --exclude tb --exclude videos \
    sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek var/gate/before/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/run_snapshot.py var/gate/before/move2_seek \
    --json var/gate/before.json
```

Success: the backup exists, and `run_snapshot.py` prints the stage's headline columns, the medians of the last updates'
`approx_kl` and `env_steps_per_sec`, and the rung.

**Repeat the resume check on the FINAL `latest.pt`.** Step 5 ran on a copy taken while the run trained; the checkpoint
that gets resumed is the one the cancel just wrote, and it is the one that must pass:

```
mkdir -p var/gate/final
cp var/gate/before/move2_seek/latest.pt var/gate/before/move2_seek/stage.json var/gate/final/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/resume_check.py var/gate/final \
    --stage-json var/animus-forge/gate/layouts/move2_seek/stage.json
```

Success: the same as step 5 (exit 0, `OK:`, both compat checks empty, `plateau_env_steps None`, `lr_scale 1`, the note),
and its `update` and env steps equal the ones `run_snapshot.py` just printed. (After the resume, the checks of step 8
are the other half: the re-baseline note, `lr_scale` 1.0 and `best.pt`.)

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
did not read either; keys of stages that no longer exist are listed too). **`conf_prune --ssh` has never run on a
machine: its first use is `--check` (read-only; R4 did it before the gate), and `--prune` runs only now, with the stage
cancelled.**

```
for m in sarah@192.168.0.68 spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117; do
  for f in env/dist/etc/modules/mod_animus_forge.conf env/dist/etc/worldserver.conf; do
    python3 apps/forge/tools/conf_prune.py --check --ssh $m:'~/animus-forge/'$f --old pre-cleanup-2026-10-07
  done
done
```

(a machine without one of the two files is an `ssh ... could not read` error for that file; skip it.) `--check` exits 1
while unknown keys remain. Then `--prune` instead of `--check`, **saving each machine's output**
(`... --prune --ssh ... | tee -a var/gate/prune.<machine>.txt`): each unknown assignment is commented out in place
(`#pruned <stamp> (<why>): <the original line>`), never deleted, after `<file>.bak-<stamp>` is written on that machine,
and the tool prints the stamp and an `to undo: conf_prune.py --restore <stamp> --ssh ...` line. **That stamp, per
machine and per file, is what step 9 restores by; it is written to `var/gate/NOTES.txt` now**, and
`conf_prune.py --list-backups --ssh <m>:<path>` lists the stamps a file has. Success: a second `--check` per file ends
`0 unknown key(s)` and exits 0, and `--list-backups` lists the stamp you wrote down.

**Conf keys outside `AnimusForge.Curriculum.*`.** `forgectl conf-sync` copies only the host's
`AnimusForge.Curriculum.*` keys to the workers, and the cluster fingerprint hashes only those (and `DecisionMs` and the
global `TicksPerDecision`). Every other key is per machine and **nothing compares it**. These are the ones the live
stages and the gate's tools depend on; each must be present with the same value on every machine that plays episodes
(the host and all three workers), because a worker's episodes are pooled into the host's training:

| Key | Value in the template | Depends on it |
|---|---|---|
| `AnimusForge.Vision.EvalVideos` | 8 | the evaluation videos each machine films (`forgectl videos`, `collect-videos.sh`); a machine without it films the code default, also 8, but a conf that says 0 films none |
| `AnimusForge.Vision.EvalVideoScale` | 4 | how large those frames are scaled; a different scale makes videos that do not compare |
| `AnimusForge.Stage.<stage>.TicksPerDecision` for the 12 stages (`move1_controls move2_seek move3_interact move4_follow combat1_fight combat2_packs combat3_survive group1_roles group2_corridor dungeon1_pulls dungeon2_ragefire dungeon3_deadmines`) | 5 each (a 50 ms world tick under 250 ms decisions) | the player controller's facing and heartbeat run once a world tick (`test_stage_ticks.py`); **a machine without the key runs the stage at the global `TicksPerDecision`, 1: a 250 ms world tick, different dynamics, silently, in the pooled data** |
| `AnimusForge.Vision.Width`, `Height`, `RenderSizes`, `FovH`, `FovV`, `Range`, `Zoom`, `Pitch` | 128, 64, "32x16, 48x24, 64x32, 128x64:0.4", 120, 60, 100, 6, -15 | the camera of every stage with a vision block; only the image's width and height are checked against the learner (the image byte count), the rest are not |
| `AnimusForge.Map.MaxTiles`, `CoarseTiles`, `KeepShare`, `AgeOffsetSeconds`, `AnimusForge.Memory.MaxEntities` | 4096, 0, 0.5, 600, 64 | the mental map and the entity memory the observation carries (M2 on) |
| `AnimusForge.Classes`, `EpisodeSeconds`, `SpawnPoint.MapId/X/Y/Z/O`, `ContinentReplicas`, `HalfBatch` | per the host's conf | what every episode is built from |

(`DecisionMs` and the global `TicksPerDecision` are in the fingerprint: they are compared already. Per machine and
allowed to differ: `Envs` and `Stage.<stage>.Envs`, `Cluster.*`, `Learner.*`, `Gpu.*`, `OutputDir`, `Probe.Dir`,
`Socket`.) Check them on **every** machine, comparing with the host's, and fix the difference before step 7 (the conf is
read at start: step 7's restart applies it):

```
KEYS='^AnimusForge\.(Vision\.|Map\.|Memory\.|Stage\.[a-z0-9_]+\.TicksPerDecision|Classes|EpisodeSeconds|'
KEYS=$KEYS'SpawnPoint\.|ContinentReplicas|HalfBatch|DecisionMs|TicksPerDecision)'
for m in sarah@192.168.0.68 spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117; do
  ssh -o BatchMode=yes $m "cd ~/animus-forge/env/dist/etc && cat worldserver.conf modules/mod_animus_forge.conf \
     2>/dev/null | grep -E '$KEYS' | sed 's/[[:space:]]//g' | sort" > var/gate/other-keys.$m.txt
done
sha256sum var/gate/other-keys.*.txt; diff var/gate/other-keys.sarah@192.168.0.68.txt var/gate/other-keys.<worker>.txt
```

Success: the same hash on all four, and `grep -c TicksPerDecision` of the file prints 13 (12 stages and the global key)
and `grep -c EvalVideo` prints 2 on each machine. A key a conf lacks is set by appending it, **after a backup**, on that
machine only:

```
ssh <m> 'cd ~/animus-forge/env/dist/etc/modules && \
   cp -p mod_animus_forge.conf mod_animus_forge.conf.bak-$(date +%Y%m%d-%H%M%S) && \
   printf "%s\n" "AnimusForge.Vision.EvalVideos = 8" "AnimusForge.Vision.EvalVideoScale = 4" \
   "AnimusForge.Stage.move3_interact.TicksPerDecision = 5" >> mod_animus_forge.conf'
```

(one `Stage.<stage>.TicksPerDecision = 5` line for each stage that lacks it; the template,
`src/server/apps/worldserver/worldserver.conf.dist` lines `AnimusForge.Stage.*.TicksPerDecision` and
`AnimusForge.Vision.*`, is the reference). The key is read at the worldserver's start, so it takes effect at step 7's
restart; `forgectl conf-sync` does not do it. **Then, on every machine whose conf was written** (this append, a prune,
a restore), check the line count before anything restarts: `ssh <m> wc -l '~/animus-forge/env/dist/etc/modules/mod_animus_forge.conf'`
must print a number greater than zero (and about the count before the write, plus the lines appended). An empty conf
reads as "no keys set" at the next start, and nothing says so.

Last, the keys that remain must be the same on every machine (the fingerprint hashes their effective values;
`forgectl conf-sync --check` is this loop's read-only equivalent):

```
for m in sarah@192.168.0.68 spencer@192.168.0.66 thomas@192.168.0.67 moloch@192.168.0.117; do
  ssh -o BatchMode=yes $m 'cd ~/animus-forge/env/dist/etc && \
     cat worldserver.conf modules/mod_animus_forge.conf 2>/dev/null \
     | grep -E "^[[:space:]]*AnimusForge\.Curriculum\." | sed "s/[[:space:]]//g" | sort' \
     > var/gate/curriculum.$m.txt
done
sha256sum var/gate/curriculum.*.txt
```

Success (`forgectl conf-sync --check` ends `All workers match the host.`): the same hash on all four (a worker with a
different tuned value is refused by the host after step 7; this finds it first). A difference in a *removed* key does
not matter; one in a kept key does.

## 7. Put the tested code on `forge`, push, pull, rebuild every machine

**First the merge.** The branch under test is `cleanup`; the cluster pulls `forge`. Until `forge` holds the tested
revision, nothing below deploys what steps 1 to 6 verified. Do it from the main checkout, **after step 6's cancel** (the
live learner imports Python modules lazily; no file under a running learner may change), and check what you are about
to push is what passed:

```
git checkout forge && git merge --ff-only cleanup
echo "tested:   $(cat var/gate/tested-sha.txt)"
echo "deployed: $(git rev-parse HEAD)"
test "$(git rev-parse HEAD)" = "$(cat var/gate/tested-sha.txt)" && echo SAME || echo DIFFERENT
```

Success: `--ff-only` merges without a merge commit (if it refuses, `forge` has moved since step 1: **stop**) and the
two printed shas are identical (`SAME`). Print them in the notes. A `DIFFERENT` is a stop: what you would deploy is
not what was tested.

Then push and rebuild. `forgectl build --cluster` checks the checkout is on `forge`, pushes it to `lan`, runs
`cluster-pull.sh` on every machine **in parallel** and waits for each `AzerothCore rev. <sha> ... ready`, then prints
one table (ready / PULL FAILED / UNREACHABLE / TIMED OUT, with times). The manual fallback, which **serialises** hours of
`-march=native` builds (use it only if `forgectl build --cluster` is not available or R3 raised a doubt):

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
that is expected, only equality across machines matters. (`forgectl cluster` should then show one revision, the new
one, with no `*`.)

## 8. Resume the stage on the new build and read the first numbers

`forgectl stage resume move2_seek`, or on the host console:

```
forge resume move2_seek
```

Success, in the host's learner log (`env/dist/logs/animus-learner.log`): `Resumed move2_seek from .../latest.pt at
update <U>, <E> env steps` with the update and env steps the checkpoint had (step 6 printed them), the workers joining
(`N worker learners join this run`), and updates arriving. Also, in that log:

- **the re-baseline note** for the convergence state the resume cleared (`grep -i -E 'baseline|plateau'
  env/dist/logs/animus-learner.log | tail`; the first lines after the `Resumed` line);
- **`lr_scale` reads 1.0** in the first updates' lines (`update N | ... lr_scale 1`; not 0.55);
- **the first evaluation, at the doorway or front-room rung, writes `best.pt`** (the old checkpoint's best belonged to
  the old convergence state, so the first evaluation after the resume is the new best): `ls -l runs/move2_seek/best.pt`
  has a new modification time at that evaluation, and the learner log says it saved a best. `best_rung<k>.pt` files
  (one per ladder rung) appear at the **next ladder step** after that, not at the first evaluation.

**The first resume after a deploy reports a changed tuning fingerprint and rebuilds the baseline
cache; that is harmless** (the cache key includes the tuning in force, and the tuning's key list changed).

After the first evaluation (`eval.every_env_steps`: 10M env steps for M2, about 40 minutes) and at least 20 updates:

```
rsync -a --exclude camera --exclude tb --exclude videos \
    sarah@192.168.0.68:~/animus-forge/var/animus-forge/shared/runs/move2_seek var/gate/after/
DEV apps/forge/python/.venv/bin/python apps/forge/tools/run_snapshot.py var/gate/after/move2_seek \
    --json var/gate/after.json
DEV apps/forge/python/.venv/bin/python apps/forge/tools/run_snapshot.py --compare \
    var/gate/before.json var/gate/after.json
```

Success: the new build continues where the old one stopped, **with a different learning rate on purpose**:

- **Learning rate.** M2's `actor_lr` is now 1.5e-4: the rate the old run really ran at, because of a convergence bug
  that annealed the schedule to 0.41 of 3e-4 (the old checkpoint's last `lr_scale` was about 0.56 and falling). At that
  rate with `lr_scale` back at 1.0, **`kl_move` should be near the earlier 0.0136 and not above 0.02**. The old
  "`approx_kl` of the same order, not 2x either way" is wrong for this deploy: the joint `approx_kl` includes the look
  heads and compares against a run that was at half this rate. A `kl_move` over 0.02 is a failure (failure table).
- The evaluation's headline columns (the stage's `status.headline`, with its targets) within their noise of the old
  build's last (the score's `stderr` is printed); `env_steps_per_sec` and `update_seconds` where the same machines put
  them (a cleaned build should not be slower; a load difference explains small ones); `wall_seconds` and the ladder rung
  unchanged (a resumed ladder restarts its window, so a step may come a little later).
- `best.pt` rewritten at the first evaluation, `best_rung<k>.pt` at the next ladder step (see above).

A run whose first evaluation falls well below the old one, or whose `kl_move` is above 0.02, is **not** a pass: pause
(`forgectl stage pause`), and go to step 9 if the cause is not obvious.

Only then start the next stage (`forge start <next stage>` archives the old run of that stage; for M2 the next stage
starts from M2's checkpoint when the convergence signals end it). Never `forge start` the stage you just resumed.

## 9. Rollback

Rolling back is the tag plus the confs, on every machine, with the stage cancelled first (`forgectl stage cancel`, or
`forge cancel` in each console). **Rehearse it first (R1).** Restore each conf **by the stamp printed at step 6's
prune**
(in `var/gate/NOTES.txt`, one per machine and file): never "the oldest backup", `ls -t | tail` or a guess.

```
ssh <machine> 'cd ~/animus-forge && git fetch moloch@192.168.0.69:git/animus-forge.git --tags && \
    git checkout pre-<change>-<date>'
ETC='~/animus-forge/env/dist/etc'
python3 apps/forge/tools/conf_prune.py --list-backups --ssh <machine>:$ETC/modules/mod_animus_forge.conf
python3 apps/forge/tools/conf_prune.py --restore <stamp> --ssh <machine>:$ETC/modules/mod_animus_forge.conf
python3 apps/forge/tools/conf_prune.py --restore <stamp> --ssh <machine>:$ETC/worldserver.conf
ssh <machine> 'cd ~/animus-forge && touch env/dist/.forge-build && docker compose up -d --force-recreate ac-worldserver'
```

(`--list-backups` must show the stamp you wrote down; each `--restore` prints the backup it used and keeps the conf it
replaced as `<conf>.pre-restore-<time>`, so a wrong restore is undone by copying that file back. The machine's two
confs have their own stamps: a file the prune did not touch has no backup and needs no restore. The keys step 6 added
(the non-Curriculum ones) are in the file that was restored from before them or after them according to the order you
did things: re-run step 6's key check after the restore.) Then the fingerprint check of step 7 and `forge resume
<stage>`. The run directory needs nothing if the new build only trained on: its `latest.pt` has the old shapes (step 4
proved that), and the old build resumes it; if a run was damaged, restore `var/backups/<date>-pre-deploy` over it with
the stage cancelled. The old build with M2's old `actor_lr` runs on the old convergence state, which the new
`latest.pt` has already cleared: the rollback is to a working run, not to the exact old schedule, and the owner decides
whether that matters.
When the machines are back on `forge` afterwards, `git checkout forge` before the next `cluster-pull.sh` (it pulls
fast-forward only and a detached checkout cannot). On the dev machine the merged `forge` is not undone by a cluster
rollback: do not push anything more to `forge` until the owner decides.

## What changes at the deploy

Numbers that read differently on the new build than on the old one, so a first reading is not mistaken for a
regression:

- **`engaged`.** The dungeon stages used to have two `engaged` columns: the core's (never written, first by name) and
  InstanceEncounter's. The stage.json now has one, InstanceEncounter's real value. No live yaml reads it (checked
  against every key that names metrics: gates, convergence, headline, targets, report, episode_means, fade and costs
  `gate_metric`, `layout_sampling`), so no gate or headline changes meaning; only `eval_episodes.jsonl` rows carry it.
- **Episode columns the C++ trim removed** (`killed`, `time_to_kill`, `casts_cancelled`, ...) and the derived
  `livelocked` and `clean_kill` are gone from the report and the logs; nothing live gated on them.
- **The stall warning** now fires after 4 evaluations (`fade.stall_evals`, was 6) without a new best at a rung.

## Before the dungeon stages train (not before the deploy)

None of these blocks the deploy; each must be done before the stage it concerns trains.

- **(a) A stall alarm for the wing ladder, built from reads.** The ladder has the collapse alarm (the gate under a
  floor); a wing ladder that sits flat needs the stall alarm too, written from the evaluation reads the way the collapse
  alarm is: warn when six reads in a row at a rung bring no new best read and none reaches the target.
- **(b) The wing rung-0 alarm** exists already; check that it fires on a real G2 first read (it has only been tested
  on synthetic reads).
- **(c) G3** (`GpuVision::Renderer::Forget` has no production caller): moot, removed with the GPU camera (tag `archive/gpu-camera`).

## What can wait

- (Done: a local `forgectl build` now refuses under a running stage on this machine's own worldserver, like `--cluster`,
  and takes `--stop-running`.)
- The goal block's always-zero order columns come out at the next goal-block revision (a revision bump, so seeding
  by name carries on).
- `CombatReward.cpp`'s file name no longer fits `RewardTermName` (the one-on-one reward is gone); rename it with the
  next change that touches the file.

## The failure table

What to do at 3 a.m., alone. **Stop** = stop the gate and fix the cause (the step's success line is a gate; nothing
after it is allowed until it holds). **Deploy anyway** = go on, with a note in `var/gate/NOTES.txt` and to the owner.
**Roll back** = step 9. In every row, if the cluster is cancelled, **leave it cancelled and backed up** and write down
what you saw; do not try a second fix.

| Step | What fails | Class | What to do |
|---|---|---|---|
| 0 | a machine is `UNREACHABLE`, `STALLED`, `DOWN`, or shows a `*` on its rev | **Stop** | Nothing is cancelled yet, so nothing is at risk. `forgectl logs <machine> --errors`; a `*` rev: that machine is on other code, re-run `forgectl build --cluster` **later**, not now. Reschedule the gate. |
| 0 | disk under 30 GB on a machine, the dev card busy, someone else's job on the cluster, owner not told | **Stop** | Clear the disk (`docker builder prune`, old `archive/` runs) or wait for the card or the job; tell the owner; start step 0 again. |
| 1 | the tag cannot be pushed, or codestyle fails | **Stop** | Fix the push (`git remote -v`, the `lan` repo is on the dev machine) or the style complaint; the tag must be on both remotes before anything is built. |
| 2 | the build or the link fails | **Stop** | The link error `cannot find -l...` has the documented workaround in the step; any other error is the branch's. |
| 3 | the card was busy | **Stop** | Not a result. Wait for an idle card and run it again. |
| 3 | a GPU test fails, or the run prints no summary line (a segfault) | **Stop** | A failing GPU test blocks the deploy. Run once more; a second identical failure is real. Write the test name down for the owner. |
| 4 | `--build` fails or `layouts/` does not fill | **Stop** | The dev build is its own; read `docker logs` of the dev worldserver. Check `var/animus-forge/shared/` did not change. |
| 4 | `stage_json_diff` on **M1 or M2** shows a group other than `ALLOWED` | **Stop** | A resume would be refused or misread the old weights. Do not deploy. Give the owner the diff output. |
| 4 | `stage_json_diff` on one of the **other ten** shows a difference | **Deploy anyway** with a note | Recommended check only: read it, write it down, and tell the owner. It is a stop only if the same difference fails `resume_check --fresh` for that stage. |
| 4 | `sim_metrics --check` fails | **Stop** | The metric reader and the build disagree: a gate or a headline would read a missing column. Do not deploy. |
| 4 | `resume_check --fresh --all` shows a `FAIL` for a stage | **Stop** if the stage is M1 or M2; otherwise **deploy anyway** with a note naming the stage | M1 and M2 are what runs at the deploy. For M3 to D3 the failure is real and will stop that stage when it starts, but does not endanger the running run: write the stage and the first error down, deploy, and give the owner the stage before it is started. |
| 5 | `resume_check` fails a compatibility check (`resume compatibility`, `layout check`, the networks) | **Stop** | The checkpoint would be refused or misread: do not cancel anything. Give the owner the output. |
| 5 | `plateau_env_steps` is not None, `lr_scale` is below 1, or the NOTE is missing | **Stop** | The build lacks the convergence fix (or it did not take). Do not cancel or deploy; the old run keeps training. |
| 6 | `Plan ended: cancelled` does not appear in 5 minutes | **Stop** | `forgectl logs sarah` for the learner; wait 5 minutes more (it is saving). If it never ends, `forge pause`, then tell the owner; do not kill the container: the checkpoint may be half written. |
| 6 | the backup or the snapshot fails | **Stop** | The cluster is cancelled and unbacked: that is the one state not to leave. Retry the `cp -a` once; if the disk is full, free it (never delete the run). Do not go on without the backup. |
| 6 | `resume_check` on the final `latest.pt` fails | **Stop** | Leave the cluster cancelled and backed up. Restart the old build's run on the old build (`forge resume move2_seek` on the machines as they are, i.e. step 9 without the checkout) if the owner is not reachable within 30 minutes: it runs the old code on the old checkpoint. |
| 6 | `conf_prune --prune` errors or `--check` still lists keys | **Stop** | Restore by the stamp (`--list-backups`, `--restore`), re-check. A prune is housekeeping: **deploy anyway** with a note if only a prune fails and the key check below it holds. |
| 6 | a non-Curriculum key differs between machines | **Stop** | Fix it on the machine that differs (append or correct the key, after the backup) and re-run the loop; it takes effect at step 7's restart. |
| 7 | `merge --ff-only` is refused, or the two shas differ | **Stop** | `forge` moved or the branch moved after step 1. Nothing is pushed. Start again at step 1 (a new tag is not needed; the tested sha is). |
| 7 | one machine's build fails or times out (`PULL FAILED`, `TIMED OUT`) | **Stop** | Wait the full time first (the slowest build takes up to an hour). Then `forgectl logs <machine> --errors`. The rule is that all four machines run one revision or none: a worker on another revision is refused. Fix that machine (disk, ssh) and re-run its pull; if it cannot be fixed, **roll back all four** (step 9). |
| 7 | fingerprints differ, or `refused the worker` | **Stop** | Compare the five parts. `src`: stale checkout, re-pull. `curriculum`: step 6's key loop. `fields`: the probe data pull. Fix that machine and restart only its container. |
| 8 | no `Resumed` line, learner errors, no workers joining, or no updates | **Roll back** | Cancel (`forgectl stage cancel`), step 9, then `forge resume move2_seek` on the old build. Nothing is lost: the run directory is the old one plus nothing. |
| 8 | `lr_scale` is not 1.0, or no re-baseline note | **Stop** | Pause. The convergence state was not cleared: the resumed run is on the schedule that annealed to 0.41. Do not let it train on; roll back if the owner is away. |
| 8 | `kl_move` above 0.02, or the first evaluation well below the old one | **Roll back** if you cannot see the cause; otherwise pause and tell the owner | A wrong learning rate or a misread checkpoint. Pause first; do not let a bad update chain run. |
| 8 | `best.pt` is not rewritten at the first evaluation | **Deploy anyway** with a note | Look again after the second evaluation; tell the owner. |
| 8 | `best_rung<k>.pt` has not appeared | **Deploy anyway** | It appears at the next ladder step, not before; absence now is not a failure. |
| any | a step's success line is unclear, or something not in this table | **Stop** | Leave the cluster as it is (cancelled and backed up is safe). Write down what you saw; the owner decides. |

## Unknown keys in a live conf are harmless

A conf that still holds keys the new build dropped neither refuses startup nor changes the cluster fingerprint.
From the source:

- **The loader does not look for unknown keys in these files.** `ConfigMgr::AddKey` rejects a key only when the file
  was loaded as *optional* (`src/common/Configuration/Config.cpp:245`, `if (isOptional && itr ==
  _configOptions.end())`).
  `worldserver.conf` is loaded by `LoadInitial` with `isOptional = false` (`Config.cpp:455`); the modules' confs by
  `LoadModulesConfigs` with `false` (`Config.cpp:770`); and the forge's own `modules/mod_animus_forge.conf` by
  `ForgeMain.cpp:361`, `LoadAdditionalFile(forgeConf, false)`, deliberately in full (the comment at
  `ForgeMain.cpp:354-358`: loaded as optional the config drops every key its templates do not define, which broke the
  per-stage keys). There is no template of known keys at run time; `worldserver.conf.dist` is not loaded.
- **Even where the check applies, it does not stop startup.** An unknown option is logged at
  `ConfigPolicy::unknownOptionSeverity`, `Error` by default (`src/common/Configuration/Config.h:40`), and skipped
  (`Config.cpp:247-249`). It aborts only when the core is compiled with `CONFIG_ABORT_INCORRECT_OPTIONS`
  (`Config.cpp:251`), an option that defaults to 0 (`conf/dist/config.cmake:138`) and that nothing in the tree turns on.
- **The curriculum keys are read by name, never scanned.** `CurriculumTuning::Load` asks for each key `Visit` lists,
  with
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
