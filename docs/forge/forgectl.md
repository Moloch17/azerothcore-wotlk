# forgectl: the command line for the cluster

`forgectl` is one program that does what used to be a list of ssh and docker steps: see the cluster, start and stop a
stage, deploy a build, keep the machines' settings in step, run the tests. It is plain Python (3.11 or newer, nothing
to install) and lives in this repository.

```
./forgectl --help               from the repository root
python3 apps/forge/forgectl ...   the same thing, without the shim (needs apps/forge on the path: use ./forgectl)
```

**Rules every command follows.** It prints what it is about to do and what happened. `--help` has an example. It
exits 0 on success and non-zero on failure (1 = it failed or you declined, 2 = a mistake in the command line). Anything
that changes a machine (a console command, a build, a conf write) prints a plan and asks `Proceed? [y/N]` first;
`--yes` answers for you. Without a terminal and without `--yes` it refuses rather than guess. It uses ssh keys only,
never a password (`ssh -o BatchMode=yes`), and a machine that does not answer in time is reported, never waited on
forever.

**`--yes` is for the human operator's own scripts. An automated assistant that uses it is subject to the permission
system exactly as before; it must not construct a way around a refusal.** (A refusal here, such as the build under
a running stage or `--yes` without `--archive-ok`, is the tool doing its job: it is not an obstacle to route around.)

**The cluster file.** The machines, the host, the lan remote and branch, the ports, the container names and the dev
container are in one tracked file, [`apps/forge/cluster.toml`](../../apps/forge/cluster.toml). To add a worker, add a
`[[machine]]` block with `in_cluster = true`; to drop one (like eli), set `in_cluster = false`. `--config FILE` or
`$FORGECTL_CONFIG` points at another file. A mistake in the file is reported with the key's name.

Run it from a checkout on a machine that has ssh keys to the others (the dev machine does). The machine you run it on
may itself be listed (`local = true` there: no ssh).

| Command | What it is for | Changes things? |
|---|---|---|
| `forgectl cluster [--json]` | the health table of the machines | no |
| `forgectl status [--json]` | the host's `forge status` and the learner's latest numbers; `--json` is the stable machine-readable document | no |
| `forgectl logs [machine]` | worldserver and learner logs, problems first | no |
| `forgectl conf-sync [--check]` | keep the workers' `AnimusForge.Curriculum.*` keys equal to the host's | writes workers' confs |
| `forgectl stage status\|start\|resume\|pause\|cancel` | drive a stage | console commands |
| `forgectl build [--cluster]` | rebuild here, or push and rebuild every machine | restarts worldservers |
| `forgectl cluster move-host <machine> [<stage>]` | move the host role and a run | everything |
| `forgectl videos <stage>` | collect evaluation videos from the workers | writes the run folder |

## `forgectl cluster`

```
$ forgectl cluster
Asking 4 machines (sarah, spencer, thomas, moloch); a learner is sampled for 5 s to see whether it advances.
machine  role    rev        worldserver  learner                                           load     disk free  gpu mem
sarah    host    64b7c7dc5  up           stepping (log 3s old), step 171,356,160, 2,134/s  5.03/24  220 GB     -
spencer  worker  64b7c7dc5  up           stepping (log 6s old), step 171,239,424, 648/s    3.82/8   228 GB     -
thomas   worker  64b7c7dc5  up           stepping (log 1s old), step 171,341,824, 1,124/s  4.09/16  948 GB     3390/8192 MiB
moloch   worker  64b7c7dc5  up           stepping (log 3s old), step 171,337,728, 418/s    5.51/24  1120 GB    1717/5120 MiB

No 'refused the worker' lines on the host in the last 6 h.
```

- **rev**: `git rev-parse --short HEAD` of the machine's checkout. A `*` after it means the machine is not on the host's
  revision, so the host will refuse it (the cluster fingerprint includes the code).
- **worldserver**: `up` if the worldserver container is running, `DOWN` if not.
- **learner**: `stepping` if the last `update N | steps S | R sps` line of `env/dist/logs/animus-learner.log` advanced
  during the 5-second sample or the log was written in the last minute (updates are 5 to 15 seconds apart, so a sample
  can miss one). `STALLED` means no write for over a minute: look at `forgectl logs <machine>`. `no log` means there
  is none yet. The numbers are the step count and steps per second of the latest update.
- **load**: the 1-minute load average over the CPU count. **disk free**: free space under the checkout. **gpu mem**:
  used/total VRAM if `rocm-smi` or `nvidia-smi` is on the machine's path, else `-` (none of the current machines has
  either on its non-interactive path).
- **The refused-worker lines** come from the host's docker log and `Server.log` (last 6 hours). Each one is a worker
  whose fingerprint differs (code, probe data, protocol or the curriculum keys): see `conf-sync --check` and the
  revision column.
- A machine that does not answer shows `UNREACHABLE: <why>` (the ssh timeout is a few seconds), and the exit code is 1
  if any machine in the cluster is unreachable. `--all` adds the machines that are out of the cluster.

## `forgectl status`

Sends `forge status` to the host's console and prints the table, then one line from the learner's log:

```
learner (sarah): update 3,802 | steps 172,177,408 | 2,560 steps/s | rollout 3.20s compute 4.90s | value_loss 0.1111 | entropy 0.9883 | episode_score_outcome -0.348
```

The table is the one the console prints (stage, progress, the sim's timing, the warnings, the connected workers). The
learner line is the newest decision: its step count and rate, how long the rollout and the update took, and the loss,
entropy and score. If the console does not answer, it prints that and still shows the learner line; exit 1.

## `forgectl status --json` and `forgectl cluster --json`

One JSON object on stdout, nothing else (no colour codes, no progress lines), for scripts, `forgectl watch` and a
dashboard. **Exit 0 when the host could be read** (`read_ok: true`), 1 when it could not; a worker that is
unreachable is data in `machines`, not an error. `--no-console` leaves out the one console read (below);
`status --json --stage NAME` reads that run directory instead of the newest one. `cluster --json` prints the
`schema`, `kind`, `time`, `source`, `host`, `read_ok`, `cluster` and `machines` keys only (`kind: "cluster"`).

**Where the data comes from.** Files, not the console: on the host, the newest run directory's `progress.json`
(`runs/*/progress.json` by modification time, archived `*.worker-*` directories skipped; keys in
[file-formats.md](reference/file-formats.md)), `finished.json`, `spec.json`, the last row of `metrics.csv` and the
newest `learner` row of `eval.csv`, all read over ssh with BatchMode; on every machine the same one-ssh probe `forgectl
cluster` uses. The host's `forge status` text is typed into its console once, only for `plan.last_plan` and
`workers_seen_by_host` (skipped, and those become null, with `--no-console` or when the console does not answer).
`source` is `"files"`.

**The rules of the schema.** `schema` is an integer (now `1`); changes only add keys, never rename or remove one inside
a schema number. Every key is always present and is `null` when unknown or absent. Times are UTC ISO-8601 (`...Z`),
durations in seconds (`*_s` or `*_seconds`), counts and steps are integers. `wall_steps_per_sec`, `update_bound`,
`rollout_seconds`, `update_compute_seconds` and `wait_seconds` are passed through as the learner logged them (a build
that does not write them gives `null`).

| Key | Meaning |
|---|---|
| `schema`, `kind`, `time`, `source`, `host` | the schema number, `"status"` or `"cluster"`, when it was taken (UTC), `"files"`, the host's name |
| `read_ok`, `read_problem` | the host and its run directory could be read; if not, why |
| `cluster.dev_revision`, `host_revision` | short revisions of this checkout and of the host |
| `cluster.revisions_equal` | every reachable cluster machine is on the host's revision (null if none could be read) |
| `cluster.refused`, `refused_window` | `refused the worker` lines the host logged in the last 6 h (the fingerprint differs; see [cluster.md](cluster.md)) |
| `plan.state` | `running` (phase `training`/`evaluating` and `progress.json` fresh: 5 min while training, 2 h while evaluating), `stale` (an active phase whose file stopped being written), `idle` (anything else, including no run) |
| `plan.stage`, `run_dir`, `phase`, `updated_at`, `progress_age_s` | the run read, `progress.json`'s `phase` (`training`, `evaluating`, `finished`, `stopped`), when it was written, its age by the host's clock |
| `plan.last_plan`, `console_available` | the console's `last plan` line (e.g. `move2_seek cancelled`); whether the console answered |
| `progress.update`, `env_steps`, `total_env_steps`, `fraction` | the learner's update count, steps so far, the stage's ceiling, the quotient |
| `progress.env_steps_per_sec` | the steps/s the learner logged for its last update |
| `progress.wall_steps_per_sec`, `update_bound`, `rollout_seconds`, `update_compute_seconds`, `wait_seconds` | the wall-clock rate and the split of an update, when the learner logs them; else `null` |
| `progress.update_seconds`, `elapsed_seconds`, `resumed_env_steps` | seconds of the last update, seconds of the run, steps the run resumed from |
| `ladder.rung` | the rung of the stage's ladder at the last evaluation (`eval_seek_rung` today; null for a stage with none); `episode_rung` is the mean over the last update's episodes |
| `ladder.shaping_scale`, `cost_scale`, `lr_scale` | the fade's scales and the learning-rate scale |
| `ladder.collapsed`, `ladder.stalled`, `alarm_rung` | `ladder_collapsed` / `ladder_stalled` from the learner (`true` when the rung is raised; `stalled` is `null` while the learner does not write it); the rung that raised it |
| `eval.count`, `last_env_steps`, `last_score`, `best_score`, `best_env_steps`, `evals_since_best`, `patience`, `eval_every` | the evaluation bookkeeping of `progress.json` |
| `eval.file_updated_at`, `eval.latest` | when `eval.csv` was last written; its newest `learner` row (`update, env_steps, policy, episodes, score, stderr, margin, best, evals_since_best, seconds`) |
| `eval.headline[]` | each headline measure of the stage: `metric`, `value` (its newest evaluation value), `target` (`{op, value}` from `status_targets`, or null) and `met` (null if there is no target or value) |
| `finished` | `finished.json` (`reason`, `advanced`, `env_steps`, `best_score`) or null while the stage is undecided |
| `spec.decision_ticks`, `tick_ms`, `env_groups`, `num_envs` | the run's `spec.json`: the cadence the sim actually ran |
| `learner` | the host's learner: `state` (`stepping`, `stalled`, `no_log`), `env_steps`, `steps_per_sec`, `log_age_s` (seconds since the learner log was written; a long idle gap reads `stalled` too: read it with `plan.state`) |
| `workers_seen_by_host[]` | what the host's console lists: `address`, `machine`, `state`, `scenario`, `envs`, `env_steps_per_sec`, `last_seen_s` |
| `machines[]` | per machine: `name, role, in_cluster, reachable, problem, revision, revision_matches_host, worldserver {up, status}, learner {state, env_steps, steps_per_sec, log_age_s}, load {one_minute, cpus}, disk_free_gb, gpu {used_mib, total_mib}` |

A real output (read-only, from the idle cluster on 2026-10-08, trimmed to three of seventeen headline values, one
worker of three and the host plus one unreachable machine of four):

```json
{
  "schema": 1, "kind": "status", "time": "2026-10-08T11:07:05Z", "source": "files", "host": "sarah",
  "read_ok": true, "read_problem": null,
  "cluster": {"host": "sarah", "dev_revision": "a06f5ad36", "host_revision": "64b7c7dc5", "revisions_equal": true,
              "refused": [], "refused_window": "6h"},
  "plan": {"state": "idle", "stage": "move2_seek", "run_dir": "move2_seek", "phase": "stopped",
           "updated_at": "2026-10-07T15:01:22Z", "progress_age_s": 72329, "last_plan": "move2_seek cancelled",
           "console_available": true},
  "progress": {"update": 4203, "env_steps": 179769344, "total_env_steps": 250000000, "fraction": 0.7191,
               "resumed_env_steps": 138510336, "env_steps_per_sec": 2508.9453979659193,
               "update_seconds": 1.6734974089995376, "elapsed_seconds": 10735.036196289002,
               "wall_steps_per_sec": null, "update_bound": null, "rollout_seconds": null,
               "update_compute_seconds": 4.874696118000429, "wait_seconds": null},
  "ladder": {"rung": 1.0, "episode_rung": 1.96875, "alarm_rung": null, "shaping_scale": 0.25, "cost_scale": 1.0,
             "lr_scale": 0.38774980967020956, "collapsed": false, "stalled": null},
  "eval": {"count": 18, "last_env_steps": 171218944, "last_score": 1.512293768234742,
           "best_score": 2.753656503481743, "best_env_steps": 40351744, "evals_since_best": 13, "patience": 3,
           "eval_every": 10000000, "file_updated_at": "2026-10-07T14:24:09Z",
           "latest": {"update": 3756, "env_steps": 171218944, "policy": "learner", "episodes": 78,
                      "score": 1.512293768234742, "stderr": 0.20714293188462887, "margin": 0.49322934515362243,
                      "best": 2.753656503481743, "evals_since_best": 13, "seconds": 22.4},
           "headline": [
             {"metric": "found", "value": 0.807692289352417, "target": {"op": ">=", "value": 0.95}, "met": false},
             {"metric": "found_hallway", "value": null, "target": null, "met": null},
             {"metric": "wall_seconds", "value": 6.580128192901611, "target": {"op": "<=", "value": 2.0},
              "met": false}]},
  "finished": null,
  "spec": {"decision_ticks": 1, "tick_ms": 250, "env_groups": 2, "num_envs": 64},
  "learner": {"state": "stalled", "env_steps": 179759104, "steps_per_sec": 2509.0, "log_age_s": 72333},
  "workers_seen_by_host": [
    {"address": "192.168.0.117", "machine": "moloch", "state": "idle", "scenario": "move2_seek", "envs": 0,
     "env_steps_per_sec": 0, "last_seen_s": 3}],
  "machines": [
    {"name": "sarah", "role": "host", "in_cluster": true, "reachable": true, "problem": null,
     "revision": "64b7c7dc5", "revision_matches_host": true, "worldserver": {"up": true, "status": "Up 23 hours"},
     "learner": {"state": "stalled", "env_steps": 179759104, "steps_per_sec": 2509.0, "log_age_s": 72333},
     "load": {"one_minute": 0.75, "cpus": 24}, "disk_free_gb": 219.5, "gpu": null},
    {"name": "spencer", "role": "worker", "in_cluster": true, "reachable": false,
     "problem": "unreachable: ssh: connect to host 192.168.0.66 port 22: No route to host", "revision": null,
     "revision_matches_host": null, "worldserver": {"up": null, "status": null},
     "learner": {"state": null, "env_steps": null, "steps_per_sec": null, "log_age_s": null},
     "load": {"one_minute": null, "cpus": null}, "disk_free_gb": null, "gpu": null}]
}
```

(The real output is the same data indented one key per line.) The idle cluster shows `learner.state: "stalled"`
because the learner's log has not been written for 20 hours: a stalled learner is a fault only while `plan.state` is
`running`. The phase 2 structured `status` of the audit will replace the file reads and keep this schema.

## `forgectl stage status|start|resume|pause|cancel [<stage> ...]`

```
forgectl stage start move2_seek       fresh run (the previous run of that stage is archived; the plan shows its step count)
forgectl stage resume move2_seek      continue from latest.pt (resume with no name: unpause, or continue where the plan stopped)
forgectl stage pause                  freeze after the current decision
forgectl stage cancel                 stop; the learner saves latest.pt, so a cancelled run resumes where it left off
forgectl stage status                 same as forgectl status without the learner line
```

**`start` shows what it archives.** A fresh start moves the stage's existing run (`runs/<stage>/`, if it holds
anything) to `archive/`. Before it asks, forgectl reads that directory on the host (the last `env_steps` in its
`metrics.csv`) and puts it in the plan: `move2_seek has a run at 178M steps; start archives it` (or "has no run ...;
nothing is archived", or "could not be read" if the host or the file does not answer). The same line goes in the audit
log. A person at the prompt sees it and answers. **`--yes` cannot be asked, so it refuses to archive a run of more
than 1,000,000 steps (or one whose size could not be read) unless you also pass `--archive-ok`**; the refusal prints
the line and the audit record carries it. Example: `forgectl stage start move2_seek --yes --archive-ok`.

`start` and `resume` go to the host. **`pause` and `cancel` go to the host and then to every worker's console over
ssh**, because the host's pause does not reach the workers today. The plan lists each machine before it asks. Each machine's
reply is printed under its name; a machine that does not answer is named and the exit code is 1 (the others still
got the command). If the host does not answer, the workers are left alone. For `pause`/`cancel` a stage name is
optional; if you give one it must be the one running (`pause` and `cancel` act on the whole plan).

How the console is reached: `docker attach` in a pty (over `ssh -tt` for a worker), typing the line, reading until the
console prompt `AC> ` comes back (with a timeout), stripping colour codes and the interleaved log lines, and leaving
with Ctrl-P Ctrl-Q. It never closes the console's input (end-of-file would shut the server down) and attaches with
`--sig-proxy=false` so a signal to the client cannot reach the server.

**One run per console.** Two forgectl runs typing into the same machine's console at once would interleave their
characters. Each send holds an exclusive `flock` on `~/.forgectl/locks/<machine>.lock` (on the machine forgectl runs
on, one lock per target machine, so different machines do not wait for each other) from before the attach until after
the detach. A second run prints "another forgectl is typing into <machine>'s console; waiting up to 90 s", and if the
first is still going it stops with that machine's `FAILED` line (the lock file holds the holder's pid) and sends
nothing. The kernel releases the lock if the holder dies. The lock only covers forgectl runs on the same computer: a
person typing in `docker attach` by hand, or forgectl on another computer, is not excluded.

**One operator at a time.** The lock is per computer: it stops two forgectl runs on *this* machine from interleaving,
nothing more. It does not cover `docker attach` by hand, or another person's (or your own, from a second computer)
forgectl. While a deploy or a stage change is under way, one person drives the cluster; say so in the channel the
cluster is discussed in before you start.

**Signals.** Python does not run `finally` blocks when the default SIGTERM handler ends the process, which would
leave a `docker attach` client dangling on the worldserver's console. So during a send forgectl handles SIGTERM and
SIGHUP itself: it raises, the `finally` sends Ctrl-P Ctrl-Q and waits for the client to leave, and only then does
forgectl exit (status 128 + the signal number: 143 for SIGTERM, 129 for SIGHUP). A second signal during the detach
is held back until the detach is done. SIGKILL cannot be handled: if forgectl is killed that way, look for a stray
`docker attach` on the machine (`pgrep -a -f "docker attach"`). See
[decision 0001](decisions/0001-control-socket.md) for the proposal to replace this.

## `forgectl logs [machine] [--errors]`

`forgectl logs thomas --errors` reads the machine's worldserver output (`docker logs`), `Errors.log` and the learner
log (without its enormous `update` lines) and prints **problems first**: lines with error, warning, fatal, exception,
crash, "refused the worker" and whole Python tracebacks. The sim's routine `Press refused` lines are not problems
and are left out. Then the last lines of each source, and the learner's latest update. `--errors` stops after the
problems; `--lines N` (default 40); `--wide` does not cut long lines. The default machine is the host.

## `forgectl build [--cluster]`

- `forgectl build`: in the checkout forgectl is run from, `touch env/dist/.forge-build` and recreate the
  worldserver container (which
  recompiles the checkout with `-march=native`), then wait for its `ready` line. Asks first.
- `forgectl build --cluster`: checks this checkout is on the cluster branch (`forge`), **pushes it to the lan remote**,
  then runs `apps/forge/tools/cluster-pull.sh` on every machine in the cluster **in parallel**, and waits for each to
  print `AzerothCore rev. <sha> ... ready` for the new revision (up to `--timeout` minutes each, default 60; the
  slowest machines take the longest). Then one table: each machine's result, time and detail.

```
machine  result       time      detail
sarah    ready        6.2 min   AzerothCore rev. 0123456789ab (forge branch) (Animus Forge) ready...
spencer  TIMED OUT    60.0 min  no 'ready' line for 012345678 within 60 min (built so far, not ready yet)
```

A machine that fails its pull (`PULL FAILED`, with the last lines of its output), cannot be reached (`UNREACHABLE`) or
is not ready in time (`TIMED OUT`) is named and the exit code is 1.

**A build restarts every worldserver, which would kill a training stage without its final checkpoint save** (a
`stage cancel` saves `latest.pt` first). So `build --cluster` first reads the host's `forge status` and **refuses if a
stage is running, even with `--yes`**, and also if the console does not answer while the worldserver container is up
(it cannot tell). Two ways forward: `forgectl stage cancel` yourself first, or pass **`--stop-running`**: the plan
then starts with "cancel the running stage on every machine ... and wait for 'Plan ended'", and after you confirm it
sends the cancel to the host and every worker (a machine that does not take it stops the build before anything is
pushed), waits for "Plan ended" on the host, and only then pushes and builds. The audit line says the stage was
stopped. Afterwards `forgectl stage resume <stage>` continues the run from `latest.pt`. A host whose worldserver
container is not running at all can be rebuilt without the flag. After a build, a
change to the curriculum keys still needs `forgectl conf-sync`; then `forgectl cluster` should show one revision.

## `forgectl conf-sync [--check]`

The cluster fingerprint hashes the host's `AnimusForge.Curriculum.*` keys, and the conf files are per machine and not
in git, so the workers' keys must equal the host's.

```
$ forgectl conf-sync --check
machine  keys (host: 239)  against the host
spencer  239 keys          same
thomas   239 keys          same
moloch   239 keys          same
All workers match the host.
```

`--check` only compares (exit 1 if any worker differs, and it lists the first differing, missing and extra keys).
Without it, for each worker that differs it asks, then backs the conf up as `mod_animus_forge.conf.bak-<timestamp>`,
replaces the differing values, appends missing keys under a comment, removes keys the host does not have, and
writes the result (see below), reads it back and checks the counts and values match. Nothing else in the worker's
file is touched (its own role, threads, envs). A worker reads its conf at start: restart it (a build does) for the
change to count. Phase 3 of the human-operable plan removes the need for this command.

**After any conf write, check the line count before anything restarts.** A conf that is empty, or that lost most of
its lines, is read at the next start as "no keys set": the worldserver runs on defaults (or refuses to start) and
nothing says the conf was the cause. So after `conf-sync` (or a hand edit, a `conf_prune`, a restore) run
`ssh <machine> wc -l '~/animus-forge/env/dist/etc/modules/mod_animus_forge.conf'` on every machine written, and
proceed only when the count is greater than zero (and about what it was: `conf-sync` prints the key count it
verified). Restart nothing until it is.

**How a conf is written** (here, and for the roles in `move-host`): the text goes to a temporary file in the conf's
own directory (`mod_animus_forge.conf.forgectl-new.<pid>`, owner and mode copied from the conf), its checksum is
compared with the text forgectl meant to write, and only then is it `mv`d over the conf. A rename is atomic, so a
dropped ssh leaves the old conf or the new one, never a truncated one (a failed check leaves the conf untouched and
removes the temporary file). **The conf may be a single file bind-mounted into the worldserver container** (the
first-time checklist's inode check is about this): a rename would then give the host a new file while the container
keeps looking at the old inode. So forgectl asks `docker inspect` whether the conf file itself is a mount, and if it is,
or if the `mv` fails for any reason (a bind-mounted file is "busy" to a rename from inside the container's view), it
writes the checked temporary file over the conf in place (the same inode) and **says so in the output**: `spencer: mv
over the conf failed (bind-mounted file?): wrote it in place instead, after checking the temporary copy`. A directory
mount is not a problem: the rename is seen. The in-place fallback is the old behaviour, so a dropped ssh in that
moment can still truncate the conf: the timestamped backup is the way back.

## `forgectl cluster move-host <machine> [<stage>]`

The recipe from [cluster.md](cluster.md) as one command. It prints the plan and asks once, then: (1) cancels the plan
on the old host and its workers and waits for "Plan ended" (skipped if the host is idle); (2) copies
`runs/<stage>/` without `camera/` and `tb/` to the same place on the new host (an existing run of that name there is
renamed, not overwritten); (3) sets the roles and host address in every machine's conf (backups); (4) pulls and
rebuilds every machine and waits for each; (5) edits `host = ` in `apps/forge/cluster.toml`, which you then commit and
push; (6) resumes the stage on the new host and looks for "N worker learners join this run". If a step fails it stops,
says which steps were done and rolls nothing back; redo the rest by hand from the "underneath" section of cluster.md.
If it stops after it began rewriting the confs (step 3 or 4) it also prints **THE CLUSTER IS IN A MIXED STATE**: the
confs on disk may say the new roles while the running worldservers still hold the old ones and not every machine is
rebuilt. It lists each machine's conf backup path and the exact command that restores it (`ssh user@address 'cp -p
<backup> <conf>'`, to paste), and says how to finish (`forgectl build --cluster`, then set `host =` in cluster.toml)
or undo (restore, then `forgectl build --cluster` so the worldservers read the restored confs). Do not resume a stage
until it is settled. The audit line notes the mixed stop.
Without `<stage>` no run is copied or resumed. The target must have `in_cluster = true`.

## `forgectl videos <stage>`

Wraps `apps/forge/tools/collect-videos.sh`: each worker's `runs/<stage>/videos/` is copied into this checkout's run
folder under `videos/from-<worker>/`. `--check` lists what each worker would send, `--dry-run` prints the commands.
Run it on the host, or from another machine with `--on-host`, which runs the script on the host over ssh (the host
then needs ssh keys to the workers).

## The audit log

Every command that changes something appends two lines to `~/.forgectl/audit.log` (an intent line when it starts, a result line when it ends) on the machine it was run from
(the directory is created, mode 0700; `$FORGECTL_HOME` moves it). Logged: `stage start|resume|pause|cancel`,
`build`, `conf-sync` (without `--check`), `cluster move-host`, `videos` (without `--check`/`--dry-run`). Not logged:
`cluster`, `status`, `stage status`, `logs`, `test`, and the `--check`/`--dry-run` forms.

```
2026-10-07T12:31:07+0100 kind=intent user=moloch machines=sarah,spencer,thomas,moloch confirm=prompt-pending cmd="forgectl stage cancel"
2026-10-07T12:31:08+0100 kind=result user=moloch machines=sarah,spencer,thomas,moloch confirm=prompt outcome=done cmd="forgectl stage cancel"
2026-10-07T12:40:11+0100 kind=intent user=moloch machines=sarah confirm=--yes cmd="forgectl stage start move2_seek --yes"
2026-10-07T12:40:12+0100 kind=result user=moloch machines=sarah confirm=--yes outcome=done cmd="forgectl stage start move2_seek --yes" notes="..."
```

An `intent` line with no `result` line after it (the same command line, later in the file) is a command that did not
finish: killed with SIGKILL, or the computer lost power. Look at the machines it names.

| Field | Values |
|---|---|
| time | local time with the UTC offset |
| `kind` | `intent` (written at the start, before anything is touched) or `result` (written when the command ends) |
| `user` | the local user who ran forgectl |
| `machines` | intent: the machines the command is expected to touch (`conf-sync` names every worker, though only the out-of-sync ones are written). result: the machines it acted on (`-` if it ended before acting: declined, refused) |
| `confirm` | intent: `--yes` or `prompt-pending`. result: `--yes` (the flag answered), `prompt` (a person typed `y`), `declined` (a person said no, or there was no terminal and no `--yes`), `not-reached` (it failed or was refused before it asked) |
| `outcome` | (result lines only) `done` (exit 0), `failed`, `declined` |
| `cmd` | the command line, as JSON text |
| `notes` | facts the operator was shown that matter later (the run a `stage start` archives, the error that stopped the command) |

Each line is one append, so two forgectl runs at once do not interleave. **The intent line is the check: if it cannot
be written, the command is refused** and nothing is sent. A command killed with SIGKILL leaves its intent line and no
result line; SIGTERM and SIGHUP during a console send still leave a result (`failed`, noted).

## When something does not work

| You see | Meaning |
|---|---|
| `UNREACHABLE: timed out` / `No route to host` | the machine is off, on another address, or ssh keys are missing: try `ssh -o BatchMode=yes user@address true` |
| `the console did not answer ... with its prompt` | the container is not running, or the worldserver is stuck: `forgectl cluster`, then `forgectl logs` |
| `Nothing was ... (not a terminal ...)` | run it from a terminal or add `--yes` |
| a `*` after a revision | that machine is on different code from the host: `forgectl build --cluster` |
| `Cluster: refused the worker` | fingerprint differs: `forgectl conf-sync --check`, then the revision column |
