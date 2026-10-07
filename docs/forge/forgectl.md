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

**The cluster file.** The machines, the host, the lan remote and branch, the ports, the container names and the dev
container are in one tracked file, [`apps/forge/cluster.toml`](../../apps/forge/cluster.toml). To add a worker, add a
`[[machine]]` block with `in_cluster = true`; to drop one (like eli), set `in_cluster = false`. `--config FILE` or
`$FORGECTL_CONFIG` points at another file. A mistake in the file is reported with the key's name.

Run it from a checkout on a machine that has ssh keys to the others (the dev machine does). The machine you run it on
may itself be listed (`local = true` there: no ssh).

| Command | What it is for | Changes things? |
|---|---|---|
| `forgectl cluster` | the health table of the machines | no |
| `forgectl status` | the host's `forge status` and the learner's latest numbers | no |
| `forgectl logs [machine]` | worldserver and learner logs, problems first | no |
| `forgectl conf-sync [--check]` | keep the workers' `AnimusForge.Curriculum.*` keys equal to the host's | writes workers' confs |
| `forgectl stage status\|start\|resume\|pause\|cancel` | drive a stage | console commands |
| `forgectl build [--cluster]` | rebuild here, or push and rebuild every machine | restarts worldservers |
| `forgectl cluster move-host <machine> [<stage>]` | move the host role and a run | everything |
| `forgectl test [--gpu]` | GTests and the CPU pytest in the dev container | no (builds in the container) |
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

## `forgectl stage status|start|resume|pause|cancel [<stage> ...]`

```
forgectl stage start move2_seek       fresh run (the previous run of that stage is archived)
forgectl stage resume move2_seek      continue from latest.pt (resume with no name: unpause, or continue where the plan stopped)
forgectl stage pause                  freeze after the current decision
forgectl stage cancel                 stop; the learner saves latest.pt, so a cancelled run resumes where it left off
forgectl stage status                 same as forgectl status without the learner line
```

`start` and `resume` go to the host. **`pause` and `cancel` go to the host and then to every worker's console over
ssh**, because the host's pause does not reach the workers today. The plan lists each machine before it asks. Each machine's
reply is printed under its name; a machine that does not answer is named and the exit code is 1 (the others still
got the command). If the host does not answer, the workers are left alone. For `pause`/`cancel` a stage name is
optional; if you give one it must be the one running (`pause` and `cancel` act on the whole plan).

How the console is reached: `docker attach` in a pty (over `ssh -tt` for a worker), typing the line, reading until the
console prompt `AC> ` comes back (with a timeout), stripping colour codes and the interleaved log lines, and leaving
with Ctrl-P Ctrl-Q. It never closes the console's input (end-of-file would shut the server down) and attaches with
`--sig-proxy=false` so a signal to the client cannot reach the server. See
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
is not ready in time (`TIMED OUT`) is named and the exit code is 1. **A build restarts every worldserver, which stops
a training stage**: `forgectl stage cancel` first and `forgectl stage resume <stage>` afterwards. After a build, a
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
replaces the differing values in place, appends missing keys under a comment, removes keys the host does not have,
writes it over the same file, reads it back and checks the counts and values match. Nothing else in the worker's
file is touched (its own role, threads, envs). A worker reads its conf at start: restart it (a build does) for the
change to count. Phase 3 of the human-operable plan removes the need for this command.

## `forgectl cluster move-host <machine> [<stage>]`

The recipe from [cluster.md](cluster.md) as one command. It prints the plan and asks once, then: (1) cancels the plan
on the old host and its workers and waits for "Plan ended" (skipped if the host is idle); (2) copies
`runs/<stage>/` without `camera/` and `tb/` to the same place on the new host (an existing run of that name there is
renamed, not overwritten); (3) sets the roles and host address in every machine's conf (backups); (4) pulls and
rebuilds every machine and waits for each; (5) edits `host = ` in `apps/forge/cluster.toml`, which you then commit and
push; (6) resumes the stage on the new host and looks for "N worker learners join this run". If a step fails it stops,
says which steps were done and rolls nothing back; redo the rest by hand from the "underneath" section of cluster.md.
Without `<stage>` no run is copied or resumed. The target must have `in_cluster = true`.

## `forgectl test [--gpu]`

Runs [`apps/forge/tools/forgectl-test.sh`](../../apps/forge/tools/forgectl-test.sh) inside the dev container named in
`cluster.toml` (`docker exec`): configures a build tree if there is none, builds `unit_tests`, relinks it against the
llvm-17 profile runtime (the container's clang 18 has no compiler-rt libraries, so the plain link fails), runs the
GTests, and runs the pytest suite with `HIP_VISIBLE_DEVICES=""` (`--gpu`: on the card). The tree is found from the
container's mounts and the build directory is `<mount>/var/forgectl-build-<tree name>` (override with `--tree` and
`--build-dir`; `--jobs N`). The first build takes a long time; later ones are incremental. It prints one summary:

```
== forgectl test summary ==
GTests: 810 passed, 0 failed, 2 skipped (exit 0)
pytest: 1500 passed, 0 failed, 30 skipped (exit 0)
RESULT: PASS
```

Failing tests are listed by name; a build failure shows its first compiler errors and stops (a stale binary is never
run); a crashed test binary is a failure. Exit 0 only on PASS. The logs stay in the container next to the build
directory (`.build.log`, `.unit.log`, `.pytest.log`). forgectl's own tests (`apps/forge/python/tests/test_forgectl.py`)
are part of that pytest run.

## `forgectl videos <stage>`

Wraps `apps/forge/tools/collect-videos.sh`: each worker's `runs/<stage>/videos/` is copied into this checkout's run
folder under `videos/from-<worker>/`. `--check` lists what each worker would send, `--dry-run` prints the commands.
Run it on the host, or from another machine with `--on-host`, which runs the script on the host over ssh (the host
then needs ssh keys to the workers).

## When something does not work

| You see | Meaning |
|---|---|
| `UNREACHABLE: timed out` / `No route to host` | the machine is off, on another address, or ssh keys are missing: try `ssh -o BatchMode=yes user@address true` |
| `the console did not answer ... with its prompt` | the container is not running, or the worldserver is stuck: `forgectl cluster`, then `forgectl logs` |
| `Nothing was ... (not a terminal ...)` | run it from a terminal or add `--yes` |
| a `*` after a revision | that machine is on different code from the host: `forgectl build --cluster` |
| `Cluster: refused the worker` | fingerprint differs: `forgectl conf-sync --check`, then the revision column |
