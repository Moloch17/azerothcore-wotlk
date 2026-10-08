# Operator experience audit

Audience: the owner, who wants the forge run and maintained by one human without an LLM. Scope: the `forge` console in
the worldserver, `forgectl`, the compose files, and the run files the learner and the sim write. Analysis and design
only; nothing here is built. Everything below is read from the code of `forge` at commit `93cbc38ea`; where a claim
could not be checked from the repository it says `UNVERIFIED`. Paths are relative to the repository root.

Terms: **host** is the cluster machine that holds the run (sarah); **worker** is another machine; **console** is the
worldserver's `AC> ` prompt; **stage** is one curriculum step (`move2_seek`, ...); **rung** is one step of a
difficulty ladder.

## 0. The short version

- The biggest relief per hour of work is not the control socket. It is four small things: let the worker containers come
  back after a reboot (`restart: unless-stopped`), send an alert when a run stalls or ends, stop the console being
  flooded (a conf change plus a handful of log-level edits), and persist the wing-ladder rung so a resume cannot reset
  it. Each is days, not weeks, and none needs the socket.
- The control socket (decision 0001) is still the right answer for the console's structural problems (no exit status,
  no structure, a fragile keystroke transport), and a cheaper path to it exists: the code already has a second
  producer of console commands, SOAP, and the socket can be a third producer of the same `CliCommandHolder`.
- Several "known" pain points are more precise than the brief states. Section 0.1 corrects them with line numbers, so
  the fixes are aimed at the real defect.

### 0.1 Corrections to the brief, from the code

| Brief says | Code says | Consequence |
|---|---|---|
| Idle-only refusals return success to the console and SOAP | Only the helper handlers in `cs_forge.cpp` do (`return true` after `SendSysMessage`: `:357-360`, `:471-474`, `:608-611`, `:666-669`, `:713-716`, `:826-829`, `:1146-1149`). `CommandStart`, `CommandPause`, `CommandCancel`, `CommandSkip` and `CommandClean` return `false` on refusal (`ForgeCommands.cpp:432-436`, `:700-712`, `:717-740`), and `World::ProcessCliCommands` turns that into `success = false` (`World.cpp:1518`), which SOAP reports as a fault (`ACSoap.cpp:137-140`). The console ignores the flag (`CliRunnable.cpp:89`). | Two distinct bugs, see OX-08 and OX-15. |
| Thousands of "Press refused" and "Seat died" lines | `Press refused` is capped at 600 per process (`EncoderSupport.cpp:189-198`); `Seat died` at 8 per layout and kind (`MayLog`, `StageScenario.cpp:2892-2896`). The uncapped flood is the `Instance.WingTrace` family, default `1` (`CurriculumTuning.h:246`): `Wing run`, `Wing start`, `Wing wipe` and `Pull drill` lines, one or more per episode per env (`InstanceEncounter.cpp:406`, `:423`, `:1155`, `:2088`, `:2151`, `:2331`). | Cap the per-episode families, not only the two named ones. OX-05. |
| `forge start` silently archives | The console prints a notice (`ForgeCommands.cpp:487-488`). The archive itself is done by the learner at its start (`runs.py: archive_run`, `train.py:575-576`), nothing is deleted. `forgectl stage start` already reads the run and demands `--archive-ok` under `--yes` (`stage.py:63-121`). | The gap is no confirmation on the console path, not silence. OX-11. |
| A worker that reboots must be rejoined by hand | A returning worker is re-ordered onto the running stage by the host (`AnimusForge.cpp:1062-1072`) and the worker reconnects every 3 s (`ClusterLink.cpp:38`, `:366`). The root cause is `restart: "no"` on the worldserver service (`docker-compose.yml:126`): the container never starts after a reboot. No plan auto-starts on boot (the sim logs "idle", `AnimusForge.cpp:246`; a grep for an auto-start key found none), so the comment justifying `no` is stale. | OX-01 is a one-line change plus a check. Whether the worker's learner rejoins the gloo group is UNVERIFIED. |
| No history or audit of commands | `forgectl` writes an intent and a result line per state-changing command to `~/.forgectl/audit.log` on the operator's computer (`audit.py`). Commands typed by hand in `docker attach` leave nothing: readline `add_history` only (`CliRunnable.cpp:233`), no `write_history` anywhere (grep), and no server-side log of executed CLI commands. | OX-19. |
| `Server.log` is almost empty | The shipped template routes `module.animus` to `Console Server` at Info (`worldserver.conf.dist:734`, appender levels at `:701-704`), so `Server.log` should hold what the console shows. The deployed `worldserver.conf` is per machine and untracked; `env/dist/etc` is empty in this checkout. | UNVERIFIED. Owner question 1: it decides whether log routing is a conf fix or needs C++. |
| No way to compare two runs or checkpoints | `apps/forge/tools/run_snapshot.py --compare BEFORE.json AFTER.json` exists (deploy-gate tool). | It compares two snapshots of one run around a deploy, not two runs, and is not in `forgectl`. OX-21. |
| Stage chains need a runner | `forge start a b c` is already a multi-entry plan; `FinishCurrent` chains the entries, a failed start of the next ends the plan (`AnimusForge.cpp:886-915`), and `Queue.SkipFinished` skips finished stages. | Missing: gates between entries (`RunAdvanced` treats every finish as advanced, `:977-986`), notifications, and resume semantics (entries 2+ start fresh and archive). Section 2.8. |
| Decision 0001: SOAP is never started by the forge | `ForgeMain.cpp:475-486` starts it when `SOAP.Enabled` (default off, `127.0.0.1:7878`); `docker-compose.yml:134` says so too. | 0001 is stale on this point. SOAP is a zero-C++ stopgap transport (section 2.1). Whether an administrator account exists for its login is UNVERIFIED. |

## 1. Pain points, ranked

Ranked by operator relief per unit of effort, not by severity alone. Severity: **H** stops or silently ruins a run, or
wastes a day; **M** costs minutes each time or risks a mistake; **L** annoyance. Effort: **S** under a day, **M** a few
days, **L** over a week. "py" means forgectl or learner Python only; "C++" needs a worldserver rebuild of every machine
(the cluster fingerprint includes the source, so it is also a redeploy).

| Id | Who, when | What happens | Evidence | Sev | Fix | Effort |
|---|---|---|---|---|---|---|
| OX-01 | Anyone, after a worker reboots or loses power | The worldserver container stays down; the worker is out of the cluster until someone ssh-es in and runs `./forge.sh`. The host would re-order it onto the stage on its own. | `docker-compose.yml:126` `restart: "no"` with a comment that "every start retrains the queue from scratch" - no longer true (`AnimusForge.cpp:246`: idle at start). The database and dev services already use `unless-stopped` (`:68`, `:187`). | H | `restart: unless-stopped` for `ac-worldserver` (keep `stop_grace_period: 1m`); docs say how to stop for good (`docker compose stop`). Check the build step in `forge-worldserver.sh` does not rebuild on restart (it builds only if no binary, a build request, or a CPU change). | S |
| OX-02 | Whoever started an overnight stage | Nothing tells them a stage converged, stalled, collapsed a rung, the learner died or a worker dropped. They find out by running `forgectl cluster` and reading. | No notify, webhook or mail code anywhere (grep). Warnings exist only in `forge status` text (`Progress.cpp` warnings list) and the periodic report is off by default (`AnimusForge.Progress.Interval` 0). `events.log` holds only ladder alarms (`StageScenario.cpp:1717-1726`). The learner raises `ladder_collapsed` / `ladder_stalled` into `progress.json` (`stage.py:299-335`). | H | A watcher in forgectl (`forgectl watch`) that polls `progress.json`, `finished.json`, the learner log age and `forgectl cluster` facts, deduplicates, and notifies (section 2.5). | M, py |
| OX-03 | Anyone resuming a dungeon stage (D1-D3) after a rebuild | The sim's wing ladder restarts at rung 0 (or the conf value) while the learner resumes at its saved rung. Nothing says so. The workaround, `Instance.WingRungStart`, is itself a fingerprinted key, so setting it by hand on the host makes every worker mismatch until all confs are edited and every worldserver restarts. | `StageScenario.cpp:302-303` builds the ladder from `WingRungStart`; `CurriculumTuning.h:243` and `:924` list it in the visitor that `Json()` hashes into the fingerprint (`AnimusForge.cpp:91-100`); `known-issues.md` A1, D1. The learner side is checkpointed (`stage.py:351-374`). `forgectl` has no reference to the rung (grep). | H | The sim writes `ladder.json` in the run directory on every rung change and reads it on `resume`, before the fingerprint; exclude the start key from the fingerprint. Interim, py: `forgectl stage resume` warns for stages with a wing ladder and shows the last rung in `events.log`. | M, C++ |
| OX-04 | Anyone driving the console | The only transport is `docker attach` on a tty. Detach is Ctrl-P Ctrl-Q; Ctrl-C or end-of-file stops the server. Two people (or `forgectl` on two computers) type into one prompt. | `docker-compose.yml:118-121`; `ForgeMain.cpp:489-495` (console only when stdin is a terminal); `CliRunnable.cpp:236-239` (`feof` -> `StopNow`); `forge.sh:42-45` warns; the forgectl lock covers one computer only (`console.py:machine_lock`, `forgectl.md` "One operator at a time"). | H | A control socket (section 2.1). Interim: forgectl already hides the detach; SOAP over an ssh tunnel is an optional stopgap. | L, C++ |
| OX-05 | Anyone watching the console | Replies to commands are buried among log lines. | Replies are printed through `utf8print` to stdout (`CliRunnable.cpp:77-85`), the log goes to the same terminal via `Appender.Console` (`worldserver.conf.dist:701`) for `Logger.module` (`:734`). Uncapped sources: the `WingTrace` family above; `Cluster:` lines; periodic plan lines. Calendar and guild messages: their source is core startup code, not Animus code (grep of `Animus/` finds none) - UNVERIFIED which logger. | H | Section 2.3: conf-only routing first (Console shows Warn and up for `module.animus`, everything stays in files), then split the single `module.animus` logger by area and cap the per-episode families. | S conf, M C++ |
| OX-06 | forgectl, scripts, dashboards | There is no machine-readable status. forgectl scrapes text: it drops console log lines by their colour escape (`console.py:LOG_LINE`), finds the reply by the echoed command and the `AC> ` prompt, decides idle with `"idle" in first_line.lower()` (`stage.py:host_plan_state`), and reads the learner's progress from a regex over `update N \| steps ...` lines of `animus-learner.log` (`cluster.py:UPDATE`, `stage.py:parse_learner_line`). Changing a column name or a colour setting silently breaks the tool. | `TextTable.h` is the only reply form; `LineSink` is line-only. `progress.json` is flat and unversioned (`progress.py:1-10`). | H | Stable status schema (section 2.2) and a structured `status` in C++ (0001 item 3). Interim, py: `forgectl status --json` built from `progress.json` and `finished.json` read over ssh, not from text. | M py, then M C++ |
| OX-07 | Whoever changes a tuning value or adds a machine | 239 `AnimusForge.Curriculum.*` keys are hand-synced across untracked per-machine confs; `conf-sync` exists but writes files that a restart then reads. A key typed on one machine only refuses that worker. | `cluster.md` (239 keys); `confsync.py`; the fingerprint hashes about 313 effective tuning values, defaults included (`CurriculumTuning.h:852`, `CurriculumTuning.cpp:96-101`, `AnimusForge.cpp:91-100`). | H | Tracked per-stage tuning (section 2.7); `forgectl config check`. | L, C++ and py |
| OX-08 | Anyone using `forgectl stage ...` | The command reports "Done: accepted" and exits 0 when the console printed a refusal ("X is running: cancel it first", "Nothing is running"). | `stage.py:run` returns 0 when every `send_checked` saw the prompt; `ConsoleResult.ok` means only `prompt_seen` (`console.py:ConsoleResult`). The reply text is shown but never classified. | M | Classify replies against the known refusal phrases now (py, S); return an `ok` flag from the socket later (OX-04). | S py |
| OX-09 | Anyone pausing | `forge pause` typed by hand pauses the host only; workers keep running and the stage drifts. forgectl compensates by visiting each worker's console over ssh, serially. | `ForgeCommands.cpp:695-709` sets `_pauseRequested` only; `stage.py:run` `targets = [host] + workers`. The host already has a push channel to each worker (`ClusterLink::Broadcast`, `AnimusForge.cpp:1088`; a `STOP` order on cancel per `cpp-runtime-console.md`). Whether cancel needs the extra worker visit is UNVERIFIED. | M | Add `PAUSE` / `RESUME` orders next to `RUNG` and `STOP`; the host's pause fans out; forgectl then talks to the host only. | M, C++ |
| OX-10 | Anyone cleaning up | `forge clean scenario <s>`, `clean exports` and `clean all` delete run directories (checkpoints included) and exported models at once, with no confirmation, no listing of what is lost and no undo. `forgectl` has no `clean` and so no guard. | `ForgeCommands.cpp:1146-1270`; `cpp-runtime-console.md` "Observed issues". | M | Rename into `archive/` instead of deleting, print the size first, and require `forge clean ... confirm` (a second typed word) or `--yes`. | S, C++ |
| OX-11 | Anyone typing `forge start <stage>` in the console | A run of any size is archived at the learner's start, after a one-line notice. Nothing asks. | `ForgeCommands.cpp:487-488`; `train.py:575-576`; `runs.py:archive_run`. | M | The sim checks `runs/<stage>/` is non-empty (it already reads `finished.json` there, `RunAdvanced`) and refuses without a `fresh` word, printing steps and age from `progress.json`. forgectl's guard stays. | S, C++ |
| OX-12 | Whoever sees "refused the worker" | The message names two hashes, not what differs: `curriculum(host 1a2b..., worker 9f...)` and the source hash. The fix is found by running `conf-sync --check`, which compares file keys, not effective values. | `ClusterLink.cpp:44-80` (`Differences` compares whole tokens); `AnimusForge.cpp:91-109` puts one FNV hash of 313 values in the token. | M | The worker also sends its sorted `key=value` list (or per-section hashes); the host names the differing keys. Interim, py: `forgectl cluster` prints "run `forgectl config check`" under a refused line. | M, C++ |
| OX-13 | The unattended run | A learner that dies leaves the sim waiting. The status shows "learner exited unexpectedly" and `forge resume` restarts the learner only; nobody is told. A crash loop is possible if automated blindly. | `cpp-runtime-console.md` (warnings list; resume row). | H | Watcher detects (log age, process, `phase`); notify; optional single automatic `forgectl stage resume` per hour with a budget (section 2.6). | M, py |
| OX-14 | Whoever asks "how is the run doing, what happened" | The story is in nine places: `progress.json`, `metrics.csv`, `eval.csv`, `eval.jsonl`, `eval_episodes.jsonl`, `finished.json`, `events.log`, camera PNGs and videos; `RUNLOG.md` is not written by any code in the repository (UNVERIFIED who writes it). | `07-operations.md` section 7.4; grep for `RUNLOG`. | M | A run registry and `forgectl runs show` (section 2.4). | M, py |
| OX-15 | Scripts, SOAP clients, future socket clients | The plan-control commands and the idle-guarded helper commands disagree about failure; `HandleStatus` and `HandleScenarios` always return true; a refusal that returns `false` without `SendErrorMessage` also makes the dispatcher print the command's help text (`ChatCommand.cpp:338-341`; its text for these commands is UNVERIFIED). | See table 0.1. | M | One helper `Refuse(handler, text)` that uses `SendErrorMessage`; use it in every refusal. | S, C++ |
| OX-16 | Before every deploy or long run | No single "can I start?" check. The checks exist as separate tools (`resume_check.py`, `conf_prune.py`, `stage_json_diff.py`, `sim_metrics.py`, `run_snapshot.py`) and as a prose checklist in `deploy-gate.md` ("Pre-flight, one screen"). | `apps/forge/tools/`; `deploy-gate.md`. | M | `forgectl doctor` (section 2.7) wrapping them and the checklist. | M, py |
| OX-17 | Anyone, weeks into a project | Disks fill silently: `docker logs` grows without bound (no `logging:` block in `docker-compose.yml`), `archive/` is never pruned ("nothing is deleted", `runs.py:3-4`), camera images and videos accumulate, numbered checkpoints keep 5 (`config.py:606`) per run. `forgectl cluster` shows free space but does not warn. | `docker-compose.yml` (no `logging`); `runs.py`; `cluster.py` disk column. | M | Docker log rotation (`max-size`, `max-file`) in compose; a retention policy and `forgectl runs prune --dry-run` (section 5.4); disk-low alert. | S compose, M py |
| OX-18 | Anyone looking for a log | Logs are in: `docker logs` (the console stream), `Server.log` (opened `w`, so a restart overwrites it, `worldserver.conf.dist:702`), `Errors.log` (also `w`), `animus-learner.log` (one file appended across every stage and restart, `ChildProcess.cpp:74`), `animus-learner.rank<k>.log`, the export log, `tensorboard.log`, and `runs/<stage>/events.log`. The learner has no severity; it uses `print()`. | As cited. | M | One log directory per machine, per-run learner log inside the run directory, append mode for server logs, `forgectl logs --stage --since --follow` (section 2.3). | M, py and conf |
| OX-19 | Whoever asks "who stopped the run" | A hand-typed console command is not recorded anywhere. forgectl's audit is on the operator's computer, not the cluster. | `CliRunnable.cpp:233`; `ProcessCliCommands` logs only at Debug (`World.cpp:1512`). | M | Log every executed CLI line at Info with its origin (`console`, `soap`, `socket:peer`) in `ProcessCliCommands`; later the socket does it. | S, C++ |
| OX-20 | Anyone typing in a hurry | `forge fieldstage`, `forge fieldworld` and `forge fieldroute` have no idle check, create grids and block the world thread for the whole bake, which stalls a running plan (and the workers wait). `floorscan` and `fieldroute` are also missing from `forge help`. | `cpp-runtime-console.md` "Observed issues"; `HandleHelp` (`cs_forge.cpp:231-290`) has no `floorscan` or `fieldroute` row. | M | Add the guard; add the help rows. | S, C++ |
| OX-21 | Whoever changed something and wants to know if it helped | No run-to-run or checkpoint-to-checkpoint comparison; `run_snapshot.py --compare` is the nearest and is not integrated. Checkpoint names (`latest.pt`, `best.pt`, `best_rung<k>.pt`, `checkpoint_NNNNNN.pt`) carry no explanation. | `runs.py:62-70`; `run_snapshot.py`. | M | `forgectl runs compare` and a checkpoint table (section 2.4). | M, py |
| OX-22 | Whoever wants to see what the bot sees | Possible only with an idle worldserver (`forge camera snapshot` is idle-only), as raw `.pgm` / `.ppm` files, with no units. During a stage the only views are the "camera audit" frames and the evaluation videos. | `cs_forge.cpp:467-480`; `AnimusForge.cpp:1530`; `EvalVideo.cpp:493`. | M | `forgectl look` (section 2.9) wrapping the existing outputs, converting to PNG, no new renderer. | M, py |
| OX-23 | Whoever tunes a value | Any tuning change needs a conf edit on every machine and a restart, which ends the stage until `resume`. | Config is read once at start (`ForgeConfig::Load`); fingerprint (OX-12). | M | Do not build a general runtime-tuning command (section 6). Allow only log level and trace toggles at runtime. | S, C++ |
| OX-24 | Anyone | `forge help`, `docs/forge/reference/cpp-runtime-console.md` and `decisions/0001` disagree with the code: `forge fast` default queue (`AnimusForge.Fast.Queue`, `ForgeCommands.cpp:522`) versus the help text; `clean archive` path; `clean all` "all of the above" but does not delete `archive`; SOAP "never started"; four reference pages are linked and do not exist (`tools-and-ops.md`, `py-learner.md`, `metrics.md`, `file-formats.md`). | `cs_forge.cpp:238-239`, `:284`; `cpp-runtime-console.md`; `ls docs/forge/reference`. | L | Generate the help from the same table the docs are generated from (section 5.6); fix the strings. | S |
| OX-25 | Anyone viewing training curves from another computer | TensorBoard is published to the host's loopback only. | `docker-compose.yml:131-136`; `docker-compose.cluster.yml` `FORGE_LOCAL_ONLY`. | L | `forgectl tunnel` that prints the `ssh -L` line, or the dashboard (section 2.10). | S |
| OX-26 | Anyone running a command on four machines | Each console visit is a separate `ssh -tt` + `docker attach` + 1.5 s settle (`console.py:send`), executed one machine after another (`stage.py:run`), so a cluster-wide cancel takes tens of seconds, and there is no "wait until the plan has ended" primitive (move-host polls a log, `deploy.py:344`). | As cited. | L | Parallel visits now (py, S); `plan.wait` on the socket later. | S py |
| OX-27 | Two operators, or one on two computers | `forgectl`'s console lock is per computer; a second computer or a hand-typed `docker attach` interleaves characters. | `forgectl.md` "One operator at a time". | L | The socket serialises server-side; interim, a lock file on the host (over ssh). | S py |
| OX-28 | Whoever runs a long chain | `Queue.SkipFinished` treats "finished" as "advanced" and a missing `advanced` field as advanced (`AnimusForge.cpp:983-986`); a stage that hit its step ceiling without converging still lets the chain go on. | `AnimusForge.cpp:977-986`; `train.py:2247-2255` (`advanced` = `outcome.action == ADVANCE`). | M | Gate in the chain runner: continue only on `advanced = true` and a headline target (section 2.8). | M, py |

### 1.1 What forgectl already does well (keep it)

`forgectl` already prints a plan and asks before changing a machine, refuses to archive a large run under `--yes`
without `--archive-ok`, refuses to build under a running stage, writes an intent and a result line per command, never
closes the console's input, takes a per-machine lock, backs up a conf before writing it atomically, and prints a
"mixed state" report when `move-host` stops half way. The design below keeps those rules (`forgectl.md`, "Rules every
command follows") and adds to them; it does not replace them.

## 2. Designs

Each design says what it builds on that already exists, what is new, a mock-up, and what it deliberately leaves out.
Mock-ups are illustrative: names, numbers and wording are proposals, not output of any build.

### 2.1 A control channel for the worldserver

**Builds on.** Decision 0001 (a request/response JSON line protocol, a Unix socket by default, a token only for an
opt-in TCP listener). The code already has what the decision's section "The work in C++" step 2 needs: the SOAP thread
is a second producer of console commands. It queues `new CliCommandHolder(ctx, line, &print, &finished)` and waits
for `finished(success)` (`ACSoap.cpp:121-140`); the world thread runs it between ticks, or from `Forge::Pump()` while
it waits for the learner (`World.cpp:1505-1520`, `AnimusForge.cpp:1050-1056`). The socket is a third producer of the
same holder, so it needs no new posting mechanism and cannot disagree with the console. `ClusterLink` already speaks
line-based POSIX sockets (`ClusterLink.cpp`), the right style to copy (no new dependency).

**Stage 0 (zero C++): SOAP as a stopgap.** `SOAP.Enabled = 1` (`ForgeMain.cpp:475`) opens an HTTP console on
`127.0.0.1:7878`; the reply carries the success flag as a SOAP fault. It needs an administrator account that the
in-memory account snapshot knows (`ForgeMain.cpp:235`; whether the forge's databases hold one is UNVERIFIED) and it is
XML. Use it only if the socket slips, through `ssh -L`. It is correct about failure only after OX-15 is fixed.

**Stage 1: the socket, `exec` only.**

- Where: `<OutputDir>/control.sock` (the run directory's parent, `AnimusForge.OutputDir`), created at startup after the
  sealed-database step, removed at shutdown. Mode `0660`. Which user owns it inside the container, and whether the
  host's ssh user can open it through the bind mount, is UNVERIFIED (the container runs as whatever
  `docker-compose.yml` selects); the fallback is `docker exec` with a relay, which is what forgectl does today anyway.
- Conf: `AnimusForge.Control.Enable = 1` (default 0, as 0001 says), `AnimusForge.Control.Path`.
- Framing: one UTF-8 JSON object per line, at most 64 KB, one request at a time per connection.
- Request: `{"v":1,"id":7,"cmd":"exec","line":"forge resume move2_seek","confirm":false}`.
- Replies stream: zero or more `{"id":7,"line":"Resuming move2_seek from update 3802"}` frames (the same strings the
  console prints, with no log lines and no prompt, because only the holder's print callback feeds it), then one final
  frame `{"id":7,"done":true,"ok":true}` or `{"id":7,"done":true,"ok":false,"code":"refused_state"}`.
- `ok` is the holder's `success`, which makes OX-15 the prerequisite: every refusal uses `SendErrorMessage`.
- Codes: `ok`, `refused_state` (idle/busy), `bad_args`, `unknown_command`, `unknown_stage`, `needs_confirm`,
  `forbidden`, `too_large`, `timeout`, `shutting_down`, `internal`. A refusal always carries the text a person would
  have read.
- Destructive lines (`forge clean ...`, `forge bench apply`, `forge start` over a non-empty run) come back
  `needs_confirm` unless the request has `"confirm":true`; forgectl sets it only after its own prompt or `--yes`.

**Stage 2: typed commands.** `status` (structured, section 2.2), `plan.wait` (`{"timeout_s":600}` returns when the plan
ends or the time runs out, replacing the log polling in `deploy.py:wait_for_log`), `events.subscribe` (a stream of the
run events of section 2.5, from a byte offset so a reconnecting client misses nothing). `pause`, `resume` and `cancel`
fan out through `ClusterLink` (new `PAUSE` and `RESUME` orders beside `RUNG` and `STOP`, `AnimusForge.cpp:1084-1090`,
`:1214`), each worker acknowledging, so the reply names who did and who did not.

**Mock-up, raw protocol** (what a person sees with `socat` or a three-line script, which is the point: no docker, no
tty, no detach keys):

```
$ echo '{"v":1,"id":1,"cmd":"exec","line":"forge pause"}' | socat - UNIX-CONNECT:var/animus-forge/shared/control.sock
{"id":1,"line":"Pausing move2_seek after the current decision."}
{"id":1,"line":"Cluster: pause sent to spencer, thomas, moloch"}
{"id":1,"done":true,"ok":true}
$ echo '{"v":1,"id":2,"cmd":"exec","line":"forge cancel"}' | socat - UNIX-CONNECT:var/animus-forge/shared/control.sock
{"id":2,"line":"Nothing is running."}
{"id":2,"done":true,"ok":false,"code":"refused_state"}
```

**Mock-up, forgectl on top:**

```
$ forgectl stage cancel
Plan:
  stop the plan on the host and every worker; the learner saves latest.pt first
  send `forge cancel` to sarah's control socket (the host fans it out to spencer, thomas, moloch)
Proceed? [y/N] y
sarah    ok   Cancelling move2_seek: the learner saves its latest checkpoint, then the sim goes idle
         workers: spencer ok, thomas ok, moloch ok
waiting for "Plan ended" ... ended after 41 s (latest.pt saved at update 3,811)
$ forgectl stage cancel
sarah    REFUSED (refused_state)  Nothing is running.
exit status 1
```

**Security model.**

1. Off unless enabled; Unix socket only in v1. Reach it from another computer over ssh (`ssh -L` of a socket path, or
   `ssh host python3 relay.py`); the machine's ssh key is the credential, as for everything else in `cluster.md`.
2. Same authority as the console: every line runs as `SEC_ADMINISTRATOR`, i.e. nothing the console cannot do.
3. `SO_PEERCRED` (uid, pid) of each connection is logged with the line, so section 1's OX-19 is closed for the socket.
4. A TCP listener is not in v1. If it is ever wanted, follow 0001: bind only to an RFC 1918 or loopback address, a long
   token from the untracked conf compared in constant time, and tunnel over ssh on any network not fully trusted.
5. Length limits (64 KB), a cap of 8 connections, one command at a time per connection, idle timeout 60 s.
6. Stage names are validated against the stage list before use (`IsRunName`, `ForgeCommands.cpp:219`), never passed to
   a shell.

**Not in scope.** HTTP, WebSocket, TLS, user accounts, roles, a second command language. The console stays for
people. The socket never adds a command the console lacks; it adds an `ok` flag, framing and streaming.

**Effort.** Stage 1: about 300 lines of C++ plus tests, a day or two once OX-15 is done; the forgectl client replaces
`console.py` in its main path and keeps it as the fallback for a worldserver without a socket. Stage 2 adds the
structured `status`, `plan.wait`, events and the fan-out: three to five days. Every machine must be rebuilt (the source
hash is in the fingerprint), so land it with the other C++ changes of the roadmap (section 3).

### 2.2 Machine-readable status and a stable schema

**Problem.** `status` is a table for eyes (`Progress.cpp:390`). `progress.json` is flat, unversioned and written by the
learner only, so the sim's own view (decision timing, workers, controller) is only in the table.

**Design.**

1. A `ForgeStatus` struct in C++ filled by `CommandStatus`; the table becomes one renderer of it, JSON the other
   (0001 item 3). The socket's `status` returns it. `forge status --json` prints it on the console too.
2. Until that lands, `forgectl status --json` builds the same document from three things it can already read over ssh:
   `runs/<stage>/progress.json`, `finished.json`, and `docker ps` / log age (`cluster.py:PROBE`). It does not scrape the
   console or the learner log's `update` lines; it keeps those parsers only as a fallback and says so
   (`"source":"files"`).
3. Rules that make the schema stable: an integer `schema` field and additive changes only; never rename or remove a
   field inside a schema number; every field present, `null` when unknown (never omitted); durations in seconds with an
   `_s` suffix, steps as integers, scores as numbers; `alarms[].code` is a closed vocabulary; the document is validated
   by a JSON Schema file in the repository and a test.

**Mock-up.** `forgectl status --json` (shortened):

```json
{
  "schema": 1, "time": "2026-10-08T03:12:44Z", "source": "socket",
  "cluster": {"host": "sarah", "revision": "93cbc38ea", "fingerprint_ok": true},
  "plan": {"state": "training", "index": 1, "of": 3, "stages": [
    {"name": "move2_seek", "state": "done", "env_steps": 178000000, "best_score": 0.81},
    {"name": "move3_interact", "state": "training", "env_steps": 41200000, "total_env_steps": 150000000},
    {"name": "move4_follow", "state": "pending"}]},
  "stage": {"name": "move3_interact", "rung": {"ladder": "fade", "index": 1, "of": 4, "scale": 0.5},
            "headline": [{"metric": "reach_rate", "value": 0.62, "target": ">=0.8", "met": false}],
            "eval": {"last_env_steps": 40000000, "score": 0.44, "stderr": 0.02, "evals_since_best": 1}},
  "learner": {"phase": "training", "update": 3802, "steps_per_s": 2134, "log_age_s": 3, "alive": true},
  "workers": [{"name": "spencer", "state": "up", "last_seen_s": 2, "revision": "93cbc38ea", "steps_per_s": 648}],
  "alarms": [{"code": "ladder_stalled", "severity": "warn", "since": "2026-10-08T01:40:02Z",
              "text": "the fade rung 1 (x0.5) stalled: no gate gain in 8 evaluations"}],
  "disk": [{"machine": "sarah", "free_gb": 220}]
}
```

and the human form, which stays the default and is the same data:

```
$ forgectl status
move3_interact   training  41.2M / 150M steps  ETA 14 h 20 m   rung 1/4 (fade x0.5)   learner stepping 2,134 sps
headline         reach_rate 0.62 (target >= 0.8, not yet)         last eval 40.0M: 0.44 +/- 0.02
workers          spencer up   thomas up   moloch up                 disk free: sarah 220 GB (ok)
ALARMS           ladder_stalled since 01:40  (the fade rung 1 stalled: no gate gain in 8 evaluations)
```

**Alarm vocabulary (closed).** `ladder_collapsed`, `ladder_stalled`, `learner_silent`, `learner_exited`,
`worker_lost`, `worker_refused`, `reset_stall`, `step_rate_drop`, `entropy_floor`, `kl_high`, `nonfinite`,
`disk_low`, `fingerprint_mismatch`. Seven of them already exist as warnings in `Progress.cpp` (step-rate drop, learner
silent, learner exited, reset stall, entropy, KL/clip, non-finite) and two as learner flags (`ladder_collapsed`,
`ladder_stalled`); the schema names them.

**Effort.** py version: 2 days. C++ struct + renderer + `--json`: 2 days. Schema file and test: half a day.

### 2.3 Log routing: replies separate from noise

**Facts that shape it.** A command reply is written through `utf8print` to stdout (`CliRunnable.cpp:77-85`); it never
passes through the logger. Logs go to appenders by logger name. Every Animus message uses one logger,
`module.animus` (251 call sites), routed to `Console Server` at Info (`worldserver.conf.dist:734`). So the console is
a mix only because the console appender prints everything at Info. `forgectl` waits for the `ready` line in
`docker logs` (`deploy.py:ready_script`) and tells replies from logs by the log colour escape (`console.py:LOG_LINE`),
so any change must keep `server.worldserver` Info on the console.

**Step 1: conf only (no rebuild).** In each machine's `worldserver.conf` (or `AC_LOGGER_*` / `AC_APPENDER_*`
environment variables in compose, which makes it identical everywhere and tracked - UNVERIFIED that the env override
works for these keys; the core's config manager supports `AC_<KEY>` overrides in general):

```
Appender.Console=1,3,0,"1 9 3 6 5 8"          # console shows Warning and above only
Appender.Server=2,5,17,Server.log,w           # flag 17 = timestamp + backup of the previous file; all levels
Logger.server=4,Console Server                # keep the 'ready' line and startup on the console
Logger.module=4,Server                        # module.animus: Info to the file, nothing to the console
Logger.module.animus.plan=4,Console Server    # (step 2) plan start/end, cluster join/lost, alarms: console too
```

Result: the console shows replies, plan events and warnings; the Info flood (WingTrace, seats, route logs) is in
`Server.log`, which `forgectl logs` can search. This alone removes most of OX-05.

**Step 2: split the logger by area (C++, string edits).** Replace `"module.animus"` at each call site with a child:

| Logger | Content | Console | File |
|---|---|---|---|
| `module.animus.plan` | start, end, pause, resume, cancel, learner start/exit, `Plan ended` | Info | Server.log |
| `module.animus.cluster` | joined, lost, refused, rung broadcast | Info | Server.log |
| `module.animus.ladder` | ladder steps, collapse, stall alarms, `Pull drill steps to rung` | Info | Server.log, `events.log` |
| `module.animus.seat` | `Seat died`, `Press refused`, `Wing run/start/wipe/time/stuck`, route and corner lines | none | `Seats.log`, 512 MB cap |
| `module.animus` | everything else (config, warm-up, bench) | Warn | Server.log |

Append `Appender.Seats=2,4,0,Seats.log,a,536870912`. Because the per-episode families are the real flood, give them
the cap pattern `MayLog` already uses (a per-process, per-kind counter), as `LogEvery(kind, per_minute)`; default
`Instance.WingTrace` stays 1 but its output now lands in the file and is rate-limited.

**Step 3: severity in the learner.** The learner prints with `print(..., flush=True)` into an `O_APPEND` file
(`ChildProcess.cpp:74`). Add Python `logging` with a `[LEVEL]` prefix and the same names as the sim's loggers
(`learner.eval`, `learner.stage`, `learner.cluster`), keep the exact `update N | steps ...` line (forgectl and
`run_snapshot.py` read it) until the status schema replaces it, and write per run to `runs/<stage>/learner.log` (it
then moves to `archive/` with the run) while `env/dist/logs/animus-learner.log` becomes a symlink to the live one.

**One place for logs per machine.** `env/dist/logs/` for the machine, `runs/<stage>/` for the run:

```
env/dist/logs/Server.log          sim, Info+, previous file backed up on restart
env/dist/logs/Seats.log           per-seat diagnostics, rate-limited, 512 MB cap
env/dist/logs/Errors.log          Error and above only
env/dist/logs/animus-learner.log  -> runs/<stage>/learner.log (symlink to the live run's)
runs/<stage>/events.log           ladder events and alarms (the run's own story, section 2.5)
docker: json-file, max-size 50m, max-file 5          (compose logging block; the console stream, now short)
```

**forgectl side.**

```
$ forgectl logs sarah --since 2h --grep "Wing wipe" --stage dungeon2_ragefire
$ forgectl logs sarah --follow --level warn          # tail the files over ssh, filtered
$ forgectl logs --where                              # prints the paths above for each machine
```

**Effort.** Step 1: an hour plus a rollout of four confs (and `forgectl conf-sync` can carry it if the keys move under
a tracked file, section 2.7). Step 2: a day. Step 3: a day.

### 2.4 A run registry and `forgectl runs`

**Problem.** A run is a directory with at least nine kinds of file, plus `archive/<stage>-<time>/` siblings; "the
latest run of M2" and "which checkpoint do I resume from" need ssh and `ls`.

**Registry, no database.** A run's identity is its directory. Add one small file the learner writes at start and at
every state change, `runs/<stage>/run.json`, and let `forgectl` index the directories on the host (and `archive/`).
Fields: `schema`, `stage`, `run_id` (the archive stamp or `live`), `parent` (the stage and checkpoint it seeded from,
`seed_from`), `started_at`, `revision` (git sha of the sim and learner), `config_sha` (hash of `config.yaml`),
`fingerprint`, `seed`, `resumed_from` (list of `{update, env_steps, time}`), `state`
(`training|paused|converged|ceiling|cancelled|crashed`), `reason` (from `finished.json`), `machines`. `finished.json`
already carries `reason`, `advanced`, `env_steps`, `update`, `best_score`, `best_env_steps` (`train.py:2247-2255`);
`run.json` adds what it lacks (parent, revision, seed, resumes) so it is written once and never reconstructed.

**Commands.**

```
$ forgectl runs
stage             run                    state       steps    best    eval   age      where
move3_interact    live                   training    41.2M    0.44    0.44   2h 10m   sarah
move2_seek        20261006-1802          converged   178M     0.81    0.80   1d 9h    sarah/archive
move2_seek        20261003-0911          cancelled   32M      0.40    0.38   5d       sarah/archive
dungeon2_ragefire (no run)

$ forgectl runs show move3_interact
move3_interact  live  on sarah  rev 93cbc38ea  seed 1000  seeded from move2_seek/best.pt (run 20261006-1802)
state       training since 2026-10-08 01:02 (resumed 1x at 1.9M steps, 2026-10-08 01:40)
steps       41.2M of 150M ceiling   ETA 14 h 20 m    best 0.44 at 36.0M
rung        fade 1/4 (x0.5) since 38.1M; gate reach_rate >= 0.8 (now 0.62)
alarms      ladder_stalled since 01:40
checkpoints (what each is for, section below)
  latest.pt              update 3802   1 min ago   resume point
  best.pt                update 3470   36.0M       best evaluation (score 0.44) - what export uses
  best_rung0.pt          update 2210   25.0M       best of the rung the fade left
  checkpoint_003800.pt   update 3800   keep 5, oldest deleted
events      01:02 started | 01:40 resumed (rebuild 93cbc38ea) | 01:40 ladder_stalled (fade rung 1)
files       progress.json metrics.csv eval.jsonl ... (8 more, 1.9 GB)

$ forgectl runs compare move2_seek:20261006-1802 move2_seek:20261003-0911 --metric reach_rate
$ forgectl runs tail move3_interact          # eval rows and ladder events as they arrive
$ forgectl runs resume-points move3_interact # latest.pt, and what `resume_check.py` says about it on this build
```

**What the checkpoints mean (printed by `runs show`, written once in the docs).** `latest.pt`: saved at the end of
every checkpoint interval and on cancel; what `forge resume` continues from; never pruned. `best.pt`: the best
evaluation of the current rung; what an export uses by default; never pruned. `best_rung<k>.pt` (fade) and
`best_<ladder>_rung<k>.pt`: the best of a rung just left, copied at the step (`runs.py:rung_best_name`); never pruned.
`checkpoint_NNNNNN.pt`: numbered snapshots, newest `keep_checkpoints = 5` kept (`config.py:606`), taken every 25
updates or 5M steps (`:601-605`); the way back after a collapse. Which one to resume from after a collapse is a human
call; `runs show` lists the evaluation score at each, read from `eval.jsonl`.

**Compare.** Reuse `run_snapshot.py`: its snapshot (`--json`) is the unit; `runs compare` takes any two run ids or
`stage@update`, aligns on env steps and prints each headline metric at equal steps, the evaluation and its stderr, and
steps per second. It prints "within noise" only when the difference is under the sum of the stderrs; it does not
declare winners (as `run_snapshot.py` says, "the ratios are a reading, not a verdict").

**Effort.** `run.json` writer: half a day (learner, py). Index and `runs`/`show`/`tail`/`resume-points`: 2 days.
`compare`: 1 day.

### 2.5 An event stream and notifications

**Sources that already exist.** `progress.json` (`phase`, `ladder_collapsed`, `ladder_stalled`, `nonfinite`,
`last_eval_score`); `finished.json` (`reason`, `advanced`); the sim's `Plan ended: <reason>` and
`<stage> done|failed|cancelled after <time>` lines (`AnimusForge.cpp:892`, `:922`); `Cluster: ... lost the host`,
`is back`, `refused the worker` lines; the learner's log mtime (the STALLED test in `cluster.py`); `events.log`
(ladder alarm lines). Nothing needs to be invented to detect the incidents listed in the brief.

**Phase A, py only: `forgectl watch`.** A foreground (or systemd, section 2.6) process on the operator's always-on
machine (the dev machine). Every 30 s it takes one `forgectl status --json` snapshot, compares with the previous one,
and emits an event on each transition. Events are appended as JSON lines to `~/.forgectl/events.jsonl` (one file on the
watcher's machine; the same shape the sim will later write per run):

```
{"t":"2026-10-08T03:40:02Z","kind":"stage_converged","stage":"move3_interact","severity":"info","text":"converged at 96.4M steps, advanced"}
{"t":"2026-10-08T04:12:51Z","kind":"worker_lost","machine":"moloch","severity":"warn","text":"moloch: worldserver DOWN (3 checks)"}
```

Kinds and rules: `stage_converged` / `stage_ceiling` (from `finished.json`, `advanced` true/false), `plan_ended`,
`ladder_collapsed`, `ladder_stalled` (once per rung until it clears), `learner_exited`, `learner_silent` (log older
than 5 minutes while the plan is training; the cluster table uses 60 s, `cluster.py:STALE_SECONDS`, and the sim warns
at 120 s; whether the learner logs during a long evaluation is UNVERIFIED, so the threshold is a conf value),
`worker_lost` and `worker_back` (needs two failed checks, to ignore a flap), `worker_refused`, `disk_low` (under
30 GB, the number `deploy-gate.md` uses), `watcher_blind` (the watcher itself cannot reach the host for 10 minutes: the
alert that would otherwise be silence). Deduplicate by (kind, subject) with a quiet period; clear events on recovery.

**Notification sinks, in order of effort.** Configure in `~/.forgectl/notify.toml` (never in the tracked `cluster.toml`,
since it holds addresses and secrets):

```
[notify]
min_severity = "warn"                     # info events go to the file only
[[sink]]  kind = "desktop"                # notify-send, no account needed
[[sink]]  kind = "webhook"  url = "https://ntfy.sh/forge-alex"   # a push to a phone; Discord/Slack hooks take the same POST
[[sink]]  kind = "command"  run = "mail -s 'forge: {kind}' me@example.com"   # any local script, text on stdin
```

A webhook to a push service reaches a phone with no mail server; `command` covers e-mail through whatever the
machine already has. `forgectl notify --test` sends one of each kind. No retries beyond one; a failed send is itself
logged as an event, never fatal.

**Phase B, C++ (with the socket): `events.jsonl` per run, written by the sim,** replacing log scraping: plan
transitions, worker join and drop, the wing-ladder rung (closing OX-03's visibility), alarms; `events.subscribe` streams
it. The watcher then needs no polling for those. Keep the watcher for what only an outside observer can see (the host
being down, ssh unreachable, disk).

**Effort.** Phase A: 2-3 days (the status snapshot of 2.2 is the prerequisite). Phase B: 2 days.
