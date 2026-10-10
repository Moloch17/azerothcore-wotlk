# The training cluster

Where the forge trains, what each machine is, and how to move code and a run between them. Verified 2026-10-07; the
parts marked **to confirm** were not checked. Every step below has a `forgectl` command
([forgectl.md](forgectl.md)); the by-hand steps are kept in the last section, "What forgectl does underneath", for
when the tool itself is in doubt. The machines are listed in [`apps/forge/cluster.toml`](../../apps/forge/cluster.toml),
which forgectl reads.

## Machines

| Name | Address | Role now | CPU | Threads | RAM | Disk free | GPU |
|---|---|---|---|---|---|---|---|
| dev (this machine) | 192.168.0.69 | development; holds the `lan` git repo; **in the cluster** as a worker (owner, 2026-10-09; the biggest card, 20 GB) | Ryzen 9 9950X3D | 32 | 30 GB | 1.3 TB | gfx1100 (two device lines in docker-compose.override.yml) |
| sarah | 192.168.0.68 | **cluster host** | Ryzen 9 7900X | 24 | 30 GB | 221 GB | to confirm |
| spencer | 192.168.0.66 | worker | i7-6700K | 8 | 15 GB | 228 GB | to confirm |
| thomas | 192.168.0.67 | worker | Ryzen 7 3800X | 16 | 31 GB | 948 GB | about 8 GB |
| (moloch) | 192.168.0.117 | worker | Xeon E5-2640 | 24 | 62 GB | 1.1 TB | about 5 GB |
| eli | 192.168.0.65 | **in the cluster** since 2026-10-10 (owner: all 6 machines train); 4 cores, the slowest | | 4 | | | |

- Every machine keeps its checkout at `~/animus-forge` (branch `forge`) and reaches the others over SSH with keys,
  never passwords (`ssh -o BatchMode=yes user@address`). User names: sarah, spencer, thomas, moloch (the .117 machine),
  eli.
- The worldserver on each machine runs in the docker container `ac-animus-forge-worldserver`. Its console is reached
  with `docker attach`.
- The roles come from `AnimusForge.Cluster.Role` ("host" or "worker") and `AnimusForge.Cluster.Host`
  ("<host address>:7700" on a worker) in each machine's `env/dist/etc/modules/mod_animus_forge.conf`. These conf files
  are per machine and not tracked in git.
- Ports: 7700 control, 7701 data, 7702 weight exchange between learners.
- With `AnimusForge.Cluster.Learner = "auto"` every machine trains its own learner and only weights cross the network.
- `forgectl cluster` shows each machine's role, revision, worldserver, learner and load in one table.

## Where the code comes from

Machines pull from the bare repo on the dev machine, **not** from GitHub:

- remote `lan` is `moloch@192.168.0.69:git/animus-forge.git`;
- push the work there (`git push lan forge`) and to `origin` (GitHub);
- tags that matter: `curriculum-v1` (the first curriculum, before it was archived) and `pre-cleanup-2026-10-07`.

## Deploying a new build to the cluster

```
forgectl stage cancel                 stop the plan first: a build restarts every worldserver
forgectl build --cluster              push forge to lan, cluster-pull on every machine, wait for each "ready"
forgectl conf-sync --check            the AnimusForge.Curriculum.* keys must match the host's (drop --check to fix them)
forgectl cluster                      one revision everywhere, every worldserver up, no refused workers
forgectl stage resume <stage>
```

Merge into `forge` and `git push origin forge` yourself; `forgectl build --cluster` pushes to `lan` (the bare repo on
the dev machine the workers pull from). **The cluster fingerprint** must match: the host refuses a worker whose source
hash, protocol version or curriculum settings differ (`Cluster: refused the worker at ...`, which
`forgectl cluster` shows). The curriculum settings are the `AnimusForge.Curriculum.*` keys in
`mod_animus_forge.conf`, identical on every machine (239 of them on 2026-10-07).

## Running and stopping a stage

```
forgectl status                       the stage's headline measures, ladder rung, evaluation and cluster
forgectl stage start <stage>          start fresh (archives the previous run of that stage)
forgectl stage resume <stage>         continue from the stage's latest.pt
forgectl stage pause                  pause (on the host and every worker)
forgectl stage cancel                 stop and save latest.pt (a cancelled run resumes where it left off), everywhere
```

Runs live in `var/animus-forge/shared/runs/<stage>/` on the host (`progress.json`, `metrics.csv`, `eval.jsonl`,
`latest.pt`, `best.pt`, `stage.json`, camera images, videos). Workers train and send weights; the host holds the run.

## Moving the host to another machine

`forgectl cluster move-host <machine> <stage>` (done by hand on 2026-10-07, dev to sarah). It prints the plan and asks.

## Collecting evaluation videos

`forgectl videos <stage>` (`--check` lists without copying).

## What forgectl does underneath

The by-hand versions, for when forgectl cannot be used.

**Deploy.** Merge into `forge`, then `git push lan forge` and `git push origin forge`. On each machine:
`cd ~/animus-forge && apps/forge/tools/cluster-pull.sh`. It pulls, touches `env/dist/.forge-build` and recreates the
worldserver container, which builds from source with `-march=native`. The slowest machines take the longest; the log
line `AzerothCore rev. <sha> ... ready` (`docker logs ac-animus-forge-worldserver`) means the build is done. Copy the
host's `AnimusForge.Curriculum.*` keys to each worker's `mod_animus_forge.conf` (keep a `.bak-<date>` copy), then
restart.

**The console.** `docker attach ac-animus-forge-worldserver` on the machine (over ssh for a worker); type `forge
status`, `forge start <stage>`, `forge resume <stage>`, `forge pause`, `forge cancel`; detach with Ctrl-P Ctrl-Q, never
Ctrl-C (it stops the server). `forge pause` does not reach the workers: pause each worker's console as well.

**Moving the host.**

1. `forge cancel` on the old host; wait for "Plan ended: cancelled".
2. Copy the stage's run directory (`runs/<stage>/`, without `camera/` and `tb/`) to the same place on the new host.
3. In each machine's `mod_animus_forge.conf`: the new host gets `Role = "host"`, `Host = ""`; the others get
   `Role = "worker"`, `Host = "<new host>:7700"`.
4. Pull and rebuild every machine (the config change needs a worldserver restart).
5. `forge resume <stage>` on the new host. The log should say "N worker learners join this run". Change `host =` in
   `apps/forge/cluster.toml`.

**Videos.** Evaluation videos are written under each machine's `runs/<stage>/videos/`.
`apps/forge/tools/collect-videos.sh <stage>` pulls them into the host's run directory (`--check` lists without
copying).
