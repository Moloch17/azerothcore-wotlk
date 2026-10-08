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

### 2.6 Auto-recovery

Principle: recover from the boring failures without a human, never from a deliberate stop, and never in a loop.

**Level 0, compose (OX-01).** `restart: unless-stopped` on `ac-worldserver` in `docker-compose.yml` (the database and
dev services already have it). `docker compose stop` still stops it for good, and a reboot brings it back: `docker`
must itself start at boot (`systemctl is-enabled docker`; UNVERIFIED on the four machines, and part of `doctor`). A
restarted worldserver is idle (`AnimusForge.cpp:246`), so a reboot cannot restart training behind anyone's back. The
`forge-worldserver.sh` start only builds when there is no binary, a build request, or a different CPU
(`forge-worldserver.sh`, `cpu_signature`), so a reboot is a fast start. Also add the log rotation block here
(OX-17):

```
    restart: unless-stopped
    logging:
      driver: json-file
      options: { max-size: "50m", max-file: "5" }
```

**Worker rejoin after a reboot.** Sequence in the code: container up -> worldserver idle -> connects to the host every
3 s (`ClusterLink.cpp:38`) -> `REGISTER` + fingerprint -> the host orders it back onto the running stage if it was one
of the stage's sims (`AnimusForge.cpp:1062-1072`) -> the host's learner takes the envs back "between rollouts". Whether
this machine's own learner rejoins the cross-machine weight exchange (`AnimusForge.Cluster.Learner = "auto"`: each
worker trains its own learner and only weights cross) is UNVERIFIED: `DealClusterLearners` is the code that sets the
ranks (`AnimusForge.cpp:1274-1340`), and it runs when the stage starts. Rehearse it once (stop one worker's container,
start it, watch `forgectl cluster` and the learner log) before relying on it, and record the result in `cluster.md`.
`forgectl rejoin <machine>` is a checklist command, not magic: it checks the revision (no `*`), the conf line count
(the `forgectl.md` warning), starts the container if it is down, waits for `Cluster: joined the host`, then for the
host's `ordering it onto <stage>`, and prints what is left to do by hand if it did not happen.

**Level 1, the watcher (`forgectl watch --arm <stage-or-chain>`).** Arming is explicit. While armed, the watcher
(section 2.5) may act on three events, each with a budget:

| Event | Action | Budget | Always |
|---|---|---|---|
| `learner_exited` (stage not finished) | `forgectl stage resume <stage>` (restarts the learner from `latest.pt`) | 2 per 6 h | notify |
| host back after a reboot, plan idle, armed stage unfinished, `latest.pt` newer than 10 min before the outage | `doctor`, then `forgectl stage resume <stage>` | 1 per outage | notify |
| `worker_lost` | nothing (compose restarts it); after 15 min, notify again | - | notify |

It disarms itself on any `forgectl stage cancel|pause|start`, on `forge cancel` seen in the host log (once OX-19
exists), on a budget being used up, and on `forgectl watch --disarm`. A hand-typed cancel that the watcher did not see
is the reason the default is disarmed and the reason OX-19 matters. Auto-resume never changes a conf, never rebuilds,
never starts a stage fresh.

**Unit, for the always-on machine** (a user unit; the dev machine is the natural place, since it holds the `lan`
repository and is outside the cluster):

```
# ~/.config/systemd/user/forgectl-watch.service
[Unit]
Description=forgectl watcher (alerts; auto-resume only while armed)
[Service]
ExecStart=%h/mlac/azerothcore/forgectl watch --serve
Restart=on-failure
RestartSec=30
[Install]
WantedBy=default.target
```

`loginctl enable-linger <user>` makes it survive logout. `forgectl watch --serve` is the same loop without a terminal,
logging to the journal.

**Resume after reboot of the host, honestly.** The sim's wing-ladder rung is lost on any worldserver restart (OX-03).
Until that is fixed, auto-resume for a stage with a wing ladder must be off (the watcher refuses to arm it and says
why): a silent reset to rung 0 is worse than a stopped run.

### 2.7 Config management, `config check`, `doctor`

**What is wrong.** Tuning that must be identical everywhere (the 313 `CurriculumTuning` values; 239 of them present in
the confs) lives in an untracked per-machine file, next to values that must differ (role, host, threads, envs,
paths). The fingerprint punishes any difference; `conf-sync` repairs it after the fact.

**Design.**

1. **Split the keys by who owns them.** *Shared*: `AnimusForge.Curriculum.*` and anything else the fingerprint hashes.
   *Per machine*: `Cluster.Role`, `Cluster.Host`, `MapUpdate.Threads`, `AnimusForge.Envs`, `Stage.<name>.Envs`,
   `Learner.TorchThreads`, `OutputDir`, paths, logging.
2. **Track the shared keys.** `apps/forge/tuning/tuning.conf` in git holds only the keys that differ from the C++
   default; `ForgeMain.cpp` loads it with `LoadAdditionalFile` after the machine conf, the same call it uses for
   `mod_animus_forge.conf` (`ForgeMain.cpp:355-358`), and the tracked file wins. A machine conf that contains a
   `Curriculum` key is then a configuration error the sim reports at start with the key name, not a silent override.
   A pull is the sync; `conf-sync` is retired. Because the fingerprint hashes effective values (defaults included,
   `AnimusForge.cpp:91-100`), nothing about the hash changes, which keeps the change inside what the cluster already
   checks.
3. **Per-stage overlays (second step).** `tuning/<stage>.conf` applied when a stage's scenario is built, so a tuning
   value is "for this stage", visible in review, and not a global that every other stage inherits. Requires the sim to
   load tuning per scenario instead of once (`CurriculumTuning::Load` is called at fingerprint time and at scenario
   construction; how many sites is UNVERIFIED). Do step 2 first; step 3 only if a real need appears.
4. **Say why.** Each line in a tuning file carries a comment with the date and the reason, as `CurriculumTuning.h`
   comments already do for defaults. `git log -p apps/forge/tuning` is the history of every change to what the bots
   are asked to do.
5. **Fingerprint message.** The worker also sends its tuning as sorted `key=value` (a few KB at 313 keys), so the host
   can print `Instance.WingTimeout host 420 worker 300` instead of two hashes (OX-12).

**`forgectl config check`** (py, works today against the confs, better after step 2):

```
$ forgectl config check
machine  conf lines  Curriculum keys  against tracked tuning  per-machine keys                 problems
sarah    1,412       0                 same                    role=host threads=16 envs=192    -
spencer  1,411       0                 same                    role=worker threads=6 envs=48    -
thomas   1,411       3                 3 DIFFERENT             role=worker threads=12 envs=96   Curriculum keys in a machine conf
moloch   0           -                 UNREADABLE              -                                conf is EMPTY: do not restart
  thomas: Instance.WingTimeout = 300 here, 420 tracked (line 880); remove it from the conf
exit 1
```

It reuses `confsync.py` (key parsing, the line-count rule) and `conf_prune.py` (keys the build no longer reads).
`--fix` is absent on purpose: it prints the one-line edit; a conf is changed by `forgectl conf ...` with the backup
and atomic write that `conf-sync` already has.

**`forgectl doctor [--stage <s>]`**: the "can I start?" answer, from the checks that `deploy-gate.md`'s pre-flight page
makes a person do by hand:

```
$ forgectl doctor --stage move3_interact
machines      4/4 reachable, one revision (93cbc38ea), no '*'                                    ok
containers    4/4 worldserver up, restart policy unless-stopped on 3, 'no' on moloch                WARN
config        tracked tuning identical on 4; no machine conf empty                                ok
resume        move3_interact latest.pt resumes on this build (resume_check.py)                    ok
stage change  stage_json_diff vs the run: 2 reward terms added                                    NOTE
disk          sarah 220 GB, spencer 228 GB, thomas 948 GB, moloch 1120 GB (need >= 30)            ok
logs          docker log rotation set on 3 of 4                                                   WARN
ports         7700-7702 reachable host<->workers                                                  ok
runtime       docker enabled at boot on 4; torch version equal (2.7.1)                            ok / UNKNOWN on moloch
notify        a test message reached 'ntfy' in 2 s                                                ok
audit         ~/.forgectl/audit.log writable                                                      ok
2 warnings, 0 failures. `forgectl stage resume move3_interact` is safe to run.
```

Exit 0 (all ok or notes), 1 (a warning the caller should read), 2 (a failure; the start would fail or damage a run).
`forgectl stage start|resume` call the relevant subset automatically and print its failures in the plan, so the
operator cannot forget it. `--dry-run` on `stage start|resume|build --cluster|conf` prints the plan and runs the checks
and sends nothing (today only `videos` and `conf-sync --check` have a read-only form).

**Effort.** `doctor` and `config check`: 3 days py. Tracked tuning, step 2: 2 days C++ plus moving 239 keys (a review
task, with `test_conf_covers_tuning.py` as the safety net - it already checks the conf template covers the keys).
Fingerprint key list: 1 day C++. Per-stage overlays: 3 days, optional.

### 2.8 A stage queue and chain runner

**What exists.** `forge start a b c` builds one plan; `FinishCurrent` runs the entries one after another
(`AnimusForge.cpp:886-915`); entries 2 and later do not resume (`Resume = false`) and archive a prior run; there is no
gate (a stage that stops at its step ceiling is as good as a converged one, `RunAdvanced` defaults to advanced,
`:977-986`); an entry that ends `Failed` does not stop the plan either: `FinishCurrent` tears it down and starts the
next entry (`:901-915`).

**Design: orchestrate from forgectl, one single-stage plan at a time.** The sim's multi-entry plan cannot hold a gate,
a notification or a human decision. forgectl can, and each stage start is already guarded. A chain is a small tracked
file:

```
# apps/forge/chains/movement.toml
name = "movement"
doctor = true                       # run `doctor` before every stage; stop the chain if it fails
[[stage]]
name = "move3_interact"
require = ["advanced", "headline_met"]      # finished.json advanced=true and every headline target met
max_hours = 30                              # wall-clock ceiling (the step budget is the learner's)
on_fail = "stop"                            # stop | skip | resume_once
[[stage]]
name = "move4_follow"
require = ["advanced"]
on_fail = "stop"
```

```
$ forgectl chain run movement
Chain 'movement': move3_interact -> move4_follow          (state: ~/.forgectl/chain.json)
  doctor ok. start move3_interact? it has a run at 12M steps; start archives it  [y/N] y
  ...
03:40  move3_interact converged at 96.4M steps (advanced, headline met): gate passed
03:41  doctor ok; resuming nothing, starting move4_follow fresh (parent checkpoint move3_interact/best.pt)
03:41  notify: "movement: move3_interact done, move4_follow started"
$ forgectl chain status
$ forgectl chain pause        # finish the current stage, do not start the next
$ forgectl chain abort        # cancel the running stage (saves latest.pt) and end the chain
```

Rules: gates read `finished.json` and `progress.json` only; a failed gate stops the chain with a notification, never
skips quietly; `on_fail = resume_once` is allowed only for `learner_exited`; the chain state file lets the armed
watcher (section 2.6) continue a chain after a reboot; a chain asks the same archive question as `stage start`, once,
up front, for every stage that already has a run. The queue (`Queue`, `Queue.SkipFinished`) stays as the sim's own
default for people who type `forge start`; the chain runner does not use it.

**Effort.** 3 days py; depends on status JSON (2.2), `doctor` (2.7), notifications (2.5). A hands-off week of
curriculum is worth the cost only after the individual stages are routine (section 6).

### 2.9 "Look at what the bot sees"

**What exists.** `forge camera snapshot <map> <x> <y> <z> <yaw> [pitch] [zoom] [file]` renders one frame of the vision
block's camera from a point and writes `<file>-depth.pgm`, `-kind.ppm`, `-height.pgm`; idle only, no units, no
objective (`cs_forge.cpp:467-480`, `HandleCameraSnapshot`; the help row says so). The sim writes a "camera audit" set of
frames (`AnimusForge.cpp:1530`) and evaluation videos (`EvalVideo.cpp:493`, `runs/<stage>/videos/`), which
`forgectl videos` already collects. `camera diff` compares the CPU and GPU casters.

**Design: three modes, all wrappers.**

```
$ forgectl look point dev --map 34 --at -149.3,47.1,-22.4 --yaw 90      # an idle machine: the dev machine
rendering on dev (idle) ... 256 x 144, 31 ms
wrote var/look/point-1.png (kind), point-1-depth.png, point-1-height.png   open: file:///.../look/index.html

$ forgectl look run move2_seek                                          # what the running stage already saved
camera audit frames (newest 8) and evaluation videos (3), from sarah, converted to PNG/GIF
var/look/move2_seek/index.html   (a contact sheet: audit frames, eval video stills, with the update they came from)

$ forgectl look bot move2_seek --env 17      # OPTIONAL, needs C++: dump the frame the policy is given right now
```

- `look point` needs a worldserver that is idle. Never on the host during a stage: the dev machine, or a worker with
  its container stopped from the plan (`cluster.toml` already marks `dev` as out of the cluster). forgectl checks
  `idle` first (via status JSON) and refuses otherwise.
- Conversion is stdlib: PGM/PPM are trivial formats and PNG needs `zlib` and `struct`; `forgectl` stays dependency-free
  (`forgectl.md`: "plain Python, nothing to install").
- A page, not a viewer: an `index.html` with the images and the parameters that made them.
- `look bot` is the only new C++: a command that writes the current camera observation of env N (`forge camera dump
  <env> <file>`), because "what the bot sees during training" is not otherwise available. Feasibility (whether the
  observation tensor is still on the CPU side per env when `Gpu` observation writing is on) is UNVERIFIED; if it is
  hard, skip it, the first two modes give most of the value. Principle 1 ("a bot perceives only what a player
  perceives") is untouched: this shows the policy's input, not more.

**Effort.** `look point` and `look run`: 2 days py. `look bot`: 2 days C++, optional.

### 2.10 A local read-only dashboard

**Shape.** `forgectl dashboard [--port 8099]` serves one page on `127.0.0.1` from the always-on machine (stdlib
`http.server`, one thread polling every 30 s, a JSON endpoint and a static page with inline script; no framework, no
build step, no login because it is loopback only and changes nothing). TensorBoard (`127.0.0.1:16006` on each machine)
stays the tool for curves; the dashboard is the operations view.

**Panels and where each gets its data.**

| Panel | Shows | From |
|---|---|---|
| Now | stage, state, step bar, ETA, rung, steps/s, alarm banner | status JSON (2.2): `progress.json`, `finished.json`, sim status |
| Cluster | one row per machine: revision, worldserver, learner, load, disk, last seen | `forgectl cluster` probe (`cluster.py:PROBE`) |
| Targets | the stage's headline metrics against their targets, last eval with stderr | `progress.json` (`status_headline`, `status_targets`, `eval_*`) |
| Trend | reward, entropy, KL, eval score vs steps for the live run (small SVG) | tail of `metrics.csv` and `eval.csv` over ssh |
| Ladder | rung history and the time each rung took | `events.log`, `eval.jsonl`, later `ladder.json` |
| Events | last 50 events, newest first | `~/.forgectl/events.jsonl` (2.5) |
| Runs | the registry table (2.4) with links to `show` text | `run.json` index |
| Look | latest camera audit frames, eval video stills | `look run` output |
| Problems | `forgectl logs --errors` for the host, collapsed | `logs.py` |
| Commands | the exact `forgectl` line for each next action, copy-only | static |

Read-only on purpose: a button that cancels a stage is a second way to damage a run. If a button is ever wanted, it
calls the same `forgectl stage ...` path with its prompt.

**Effort.** 3-4 days py, after status JSON, runs index and events exist; it is the last item of the roadmap because it
only displays what the earlier items produce.

## 3. Roadmap

Rules the order follows: do the cheap, no-rebuild things first (every C++ change costs a rebuild of all machines,
and the cluster fingerprint includes the source; principle 18 says build the whole approved plan, then rebuild once),
so all the C++ goes in **one batch**; Python can ship at any time because forgectl runs on the operator's computer and
the learner only restarts with a stage. "py" = forgectl or learner Python; "conf" = compose or `.conf` only.

| Phase | Items | Kind | Effort | Depends on | Relief |
|---|---|---|---|---|---|
| 0. Today | OX-01 `restart: unless-stopped` and docker log rotation; log routing step 1 (2.3); classify refusals in `forgectl stage` (OX-08); parallel console visits (OX-26); fix stale docs and help strings that need no C++ (OX-24 docs part); rehearse a worker reboot once and write down what rejoins | conf, py, docs | 1-2 days | nothing | Reboots stop costing a manual rejoin; the console becomes readable; refusals stop looking like success |
| 1. Python batch | `forgectl status --json` from files (2.2); learner writes `run.json` (2.4); `forgectl runs` list/show/tail/compare; `forgectl watch` + notify sinks (2.5, 2.6 level 1 without auto-resume); `forgectl doctor` and `config check` (2.7); `forgectl logs --since/--grep/--follow/--where`; `runs prune --dry-run` (5.4); `forgectl look run` and `point` (2.9) | py | 8-10 days | Phase 0 (rotation, routing) is independent; `watch` needs the status JSON; `doctor` reuses the existing tools | Alerts overnight; one place for a run's story; one answer to "can I start?" |
| 2. The one C++ batch | OX-15 `Refuse` helper; OX-10 and OX-11 confirmations; OX-20 guards and help rows; OX-19 command logging with origin; logger split and per-episode caps (2.3 step 2); `ladder.json` persistence and fingerprint exclusion (OX-03); `PAUSE`/`RESUME` cluster orders (OX-09); worker sends tuning key list (OX-12); tracked-tuning load (2.7 step 2); control socket stage 1 then 2 (2.1); structured `status`, `events.jsonl`, `plan.wait` | C++ (+ GTests) | 10-14 days | OX-15 before the socket's `ok` flag means anything; the structured status before the socket's `status`; one rebuild and `resume` of every machine at the end | The console and socket tell the truth; a resume cannot silently reset a ladder; no more key syncing |
| 3. On top | forgectl socket client (keep `console.py` as fallback); `chain` runner (2.8); armed auto-resume (2.6); dashboard (2.10); optional `look bot` | py (+ small C++) | 8-10 days | Phase 1 and 2 | Unattended multi-stage weeks; a view for the owner |

**What to do first, in order of relief per hour.**

1. `restart: unless-stopped` + log rotation + log routing step 1 (Phase 0, an afternoon). Biggest immediate relief.
2. The watcher with notifications (Phase 1). Turns "check the cluster" into "get told".
3. Persist the wing rung (Phase 2, a small slice that can ship alone if a rebuild is already planned). A silent
   correctness hazard for D1-D3, the stages that matter next.
4. `doctor` and `config check` (Phase 1). Prevents the failures that cost days (an empty conf, a drifted key).
5. Tracked tuning (Phase 2). Removes the hand-sync permanently.
6. The control socket (Phase 2). The cleanest fix, but after 1-5 the console is tolerable, so it can wait for the batch.

**Pure Python (can start now, no cluster rebuild):** everything in Phases 0, 1 and 3 except `look bot`. **Needs C++:**
the Phase 2 list; of it, the cheapest standalone slices are OX-15, OX-10/11, OX-19 and OX-20 (each under a day).

## 4. A day in the life

The example is an overnight stage: start `move3_interact` in the evening, collect it in the morning, then deploy a fix
and resume. Times are illustrative; the steps are what the code requires today.

### 4.1 Today

1. 18:00. `forgectl cluster`: 5 s sample, a table. Reads the "No refused lines" footer. Checks `git rev` column for
   `*`. `forgectl conf-sync --check` (a separate command, because nothing else says the confs differ). Disk: reads a
   column and compares with 30 GB from memory or from `deploy-gate.md`.
2. 18:10. `forgectl stage start move3_interact`. Reads the archive line. Answers `y`. forgectl types `forge start ...`
   into the host's console through `docker attach` and reads the reply by guessing the prompt. The reply is shown;
   the exit status is 0 whether or not the sim accepted it (OX-08).
3. 18:20. `forgectl status`. The table includes a learner line scraped from the log. If the learner has not started
   yet, "no update line yet". Repeats until it moves.
4. 18:40. Goes to bed. No alert is configured (OX-02).
5. 03:00. A worker's machine loses power. Its container does not come back (OX-01). The host keeps training with the
   others, nobody knows.
6. 07:30. `forgectl cluster`: one machine `UNREACHABLE`. ssh to it after it is back; `./forge.sh`; waits; `forgectl
   cluster`. Whether its learner is back in the weight exchange: unknown (UNVERIFIED).
7. 07:45. `forgectl status`: the stage converged at 04:10; the plan ended and the sim has been idle for 3.5 hours.
   The log scroll is thousands of `Wing wipe` and `Seat died` lines (OX-05); the finish is a few lines in the middle.
8. 08:00. Wants to know how this run compares with the previous: opens `eval.csv` of both, or runs `run_snapshot.py`
   twice and `--compare` by hand. `RUNLOG.md` is edited by hand.
9. 09:00. Finds a bug in the new stage's reward; fixes the C++. `forgectl test`, `forgectl build --cluster
   --stop-running`: cancels (host and every worker console, serially), pushes, rebuilds. ~1 h.
10. 10:30. `forgectl conf-sync --check`: a worker differs by a key typed during the night; sync, restart that worker.
11. 11:00. `forgectl stage resume move3_interact`. For a dungeon stage the sim's wing rung would be lost here, and
    the operator would edit `Instance.WingRungStart` on every machine and restart them all (OX-03); whether M3 has a
    sim-side ladder of its own is UNVERIFIED. Then `forgectl cluster` again to see the workers re-joined.

### 4.2 After the proposed changes

1. 18:00. `forgectl doctor --stage move3_interact`: one screen, ok/warn/fail (2.7). Fixes the single warning it names.
2. 18:10. `forgectl chain run movement` (or `forgectl stage start move3_interact`). The plan includes the archive line
   and the doctor result; `y`. The reply is a structured `ok` with the host's text; the exit status is the sim's.
3. 18:15. `forgectl watch --arm movement`. Phone: "movement: move3_interact started on 4 machines".
4. 18:40. Goes to bed. The dashboard (`http://127.0.0.1:8099`) is open on the second monitor if wanted.
5. 03:00. Power loss on a worker. Docker brings the container back at boot, it reconnects, the host re-orders it
   (2.6). Phone, 03:02: "worker_lost moloch"; 03:09: "worker_back moloch (rejoined move3_interact)". Nobody woke up.
6. 04:10. Phone: "move3_interact converged at 96.4M steps, advanced, headline met". The chain starts `move4_follow`
   after its doctor check. Phone: "movement: move4_follow started".
7. 07:30. `forgectl runs`: both runs, state, steps, best score. `forgectl runs show move3_interact`: rung history,
   checkpoints with scores, events, the alarms. `forgectl runs compare move3_interact:previous move3_interact` at equal
   steps. `forgectl runs note move3_interact "reward fix pending"` replaces the hand-written log.
8. 07:45. The console, if opened, shows replies and plan events; the per-seat flood is in `Seats.log` (2.3).
9. 09:00. Fix, `forgectl test`, `forgectl build --cluster --stop-running`. The cancel goes to the host and fans out
   (one reply naming each worker), `plan.wait` returns when it ended. The tuning is tracked, so the only conf check is
   `doctor`'s "tracked tuning identical on 4".
10. 11:00. `forgectl stage resume move3_interact` (or `forgectl chain resume`): the sim reads `ladder.json`, the learner
    reads `latest.pt`, both on the same rung (OX-03). `forgectl watch` sees `plan training` and clears its alerts.

The difference is not speed; it is that the operator is told instead of looking, and that the tool says whether a
command worked.

## 5. Other ways to make the forge work better

### 5.1 Reproducibility and seeds

- Evaluation is seeded and reproducible by design: the same `--seed` (default 1000, `config.py:69`) and `--episodes`
  give the same characters and opponents (`evaluate.py` docstring). Record the seed in `run.json` (2.4).
- Training reproducibility: the learner has a `seed: int = 1` field (`config.py:596`); a repository-wide search for
  `manual_seed` in `animus/` found no call, so whether and where training RNGs are seeded is UNVERIFIED. A multi-thread
  sim with asynchronous cluster learners will not be bitwise reproducible; the useful promise is "same revision,
  same config, same seed, statistically equivalent". Say that in the docs and do not chase bitwise equality.
- What is cheap and worth it: `run.json` records revision, config hash, tuning-file hash, fingerprint, seed, parent
  checkpoint and every resume; `forgectl runs show` prints it. That answers "what exactly was this run" two months
  later.

### 5.2 Regression evaluation on every build

`forgectl test` covers code (GTests, pytest, `forgectl test --gpu`). It does not answer "did this build make the bots
worse". `animus.evaluate` scores a checkpoint on seeded episodes against a running sim (`evaluate.py`, needs
`Policy = remote` and `Learner.AutoStart = 0`). Design: after `build --cluster`, `forgectl regress` picks the frozen
`best.pt` of the last finished stage per stage family, runs `animus.evaluate` with a fixed seed and episode count on one
idle machine (the dev machine), and compares with a stored result (`apps/forge/golden/regress/<stage>.json`, written
by the same command with `--update` and reviewed in a commit; `tests/golden` and `test_golden_update.py` already show
the repository's convention for golden data). It prints "within noise" when the difference is under the stderr sum
(the rule in `run_snapshot.py`). Principle 14 holds: the evaluated policy is the learner's, no scripted baseline. Run
it before the resume of a long stage, in `doctor`'s "stage change" row, and nightly on an idle machine if one exists.
Effort 3 days py; needs an idle sim (the dev machine). Whether the evaluation is stable enough across a rebuild to be
a gate is UNVERIFIED until it has been run twice on one build.

### 5.3 Experiment tracking

Do not add a tracking service. The run directory plus `run.json`, TensorBoard (already served per machine) and
`forgectl runs compare` are the tracker. What is missing is a place for the human's reasoning: `forgectl runs note
<run> "text"` appends a timestamped line to `runs/<stage>/notes.md` (and the run's `run.json` links the git revision),
which replaces `RUNLOG.md` (not written by any code in the repository, UNVERIFIED who writes it).

### 5.4 Checkpoint retention and cleanup

Today: `keep_checkpoints = 5` numbered files per run (`config.py:606`); `latest.pt`, `best.pt`, `best_rung*.pt` never
pruned (`runs.py`); `archive/` never pruned; camera frames and videos accumulate; sizes unknown (UNVERIFIED; measure
first with `forgectl runs du`). Proposed policy, applied only by `forgectl runs prune` after a dry run and a prompt:

| Item | Keep | Delete after |
|---|---|---|
| live run | everything | - |
| archived run that finished and advanced | `best.pt`, `best_rung*.pt`, `latest.pt`, `finished.json`, `eval.*`, `metrics.csv`, `run.json`, `config.yaml` | the numbered checkpoints, `tb/`, camera frames immediately; videos after 30 days |
| archived run cancelled or crashed | the same as above, plus the newest 2 numbered checkpoints | everything else after 14 days |
| a stage's parent checkpoint (the one a later stage seeds from, principle 15) | always, by name, in `models/` registry (5.5) | never automatically |
| `archive/` older than 90 days with no registry entry | list for the owner | owner confirms |

Disk-low alert at 30 GB (2.5). `runs prune` moves to a trash directory first and empties it on a second run a day later.

### 5.5 Model export and registry

`forge export` writes `.amdl` files into `AnimusForge.ModelDir` (`export.py`; the format has a version, `AMDL_VERSION`),
and "the exported files are copied to a server by hand". Nothing records which run, checkpoint or revision a model came
from, and `forge clean exports` deletes them all (OX-10). Design: `forgectl models export <stage> [--checkpoint
best|latest]` calls `forge export`, then writes `models/<stage>/<run_id>/manifest.json` (sha256 of each `.amdl`, stage,
run id, checkpoint name, update, evaluation score and stderr, git revision, `AMDL_VERSION`, layout revisions) and keeps
the files. `forgectl models list` shows them; `forgectl models diff a b` compares manifests. Putting a model on the live
realm stays a manual step by the owner (principle 19); the registry only makes "what is running there" answerable.
Effort 2 days py. Open point: how `mod-animus` picks up models is outside this repository (its own checkout), so the
publish step is UNVERIFIED and deliberately not automated.

### 5.6 Documentation hooks

- Generate the command reference from the command table. `HandleHelp` is a hand-written list (`cs_forge.cpp:231-290`)
  that has drifted (OX-20, OX-24). Make the table the single source: help text, the command table of
  `docs/forge/reference/cpp-runtime-console.md` and `forgectl`'s known console commands are produced from it by a
  small script and checked by a GTest ("every registered subcommand has a help row") and a `forgectl docs check`
  (links to existing pages: four reference pages are linked and absent today).
- `forgectl --help` already has an example per command; add `forgectl help <topic>` that prints the relevant
  `forgectl.md` section, so the operator does not leave the terminal.
- A one-page "operator's card" (`docs/forge/operator-card.md`): ten commands, the three alarms that matter, where logs
  are, what to do when. Written after Phase 1, from what the tools actually print.

### 5.7 Scheduling across heterogeneous machines

The cluster has unequal machines (the status example shows 2,134, 648, 1,124 and 418 steps per second; an 8-thread
i7-6700K and a 24-thread Xeon). Each worker runs its own learner and only weights cross the network
(`cluster.md`; `LearnerProcess.cpp:256`), so a slow machine does not stall the others in lock-step; how a very slow
one affects the averaged weights (staleness) is UNVERIFIED. Do not build a scheduler. Do:
(a) report per-machine steps/s and share of the total in the status (2.2), and raise `worker_slow` when a machine runs
under a quarter of the median for an hour (a machine thrashing, not a slow one);
(b) use `forge bench` per machine to set `AnimusForge.Envs` and `MapUpdate.Threads` (`forge bench apply` exists) and
record the result in `cluster.toml` comments, so a rebuilt machine gets its numbers back;
(c) an `in_cluster` switch per stage is already enough to keep a machine out of a stage (`cluster.toml`).

### 5.8 The weekly and overnight workflow

A written routine beats a clever tool for a single operator. Proposed, to put in the operator's card:

| When | Do |
|---|---|
| Friday evening | `forgectl doctor`; `forgectl chain run <week's chain>`; `forgectl watch --arm`; check the phone gets a test notification |
| Saturday-Sunday | nothing; read alerts only |
| Monday morning | `forgectl runs`; `forgectl runs show` for what finished; `forgectl runs compare`; `forgectl runs note`; decide the next chain |
| Weekly | `forgectl runs prune --dry-run`; `forgectl cluster` disk columns; `git log` of `apps/forge/tuning`; export and register models if a stage advanced |
| Before any deploy | `deploy-gate.md`, but its pre-flight page is `forgectl doctor` |
| Monthly | rehearse a worker reboot and a rollback on the weakest worker (R1 in the deploy gate) |

## 6. Risks and what not to build

**Do not build.**

- **A scheduler or orchestrator** (Slurm, Ray, Kubernetes, a job queue service). Four machines and one operator; the
  chain file plus roles in `cluster.toml` is enough.
- **A tracking service** (MLflow, W&B) or a run database. Directories, JSON and CSV are queryable by `forgectl`; a
  database adds a thing to keep alive.
- **A web UI that changes state**, accounts, roles or login. The dashboard is read-only and loopback; one operator.
- **A TCP or HTTP control listener in v1**, TLS, a REST API. Unix socket + ssh. Revisit only if a second computer needs
  to drive the cluster without ssh.
- **A general runtime-tuning command** (`forge set key value`). Tuning goes through the fingerprint; a value changed on
  one machine at runtime breaks the "every machine identical" guarantee the cluster is built on. Tracked files and a
  restart are right. Runtime-settable: log level and trace toggles only.
- **Self-healing that changes things**: no auto-rebuild, no auto conf edit, no auto start of a fresh stage, no
  auto-rollback. Auto-resume only when armed, with a budget.
- **A Prometheus/Grafana/alertmanager stack, a chat bot or chat-ops.** One webhook and desktop notifications cover it.
- **A rewrite of the console or `forgectl` in another language.** forgectl is stdlib Python that works and has tests.
- **Bitwise reproducibility work.**

**Risks.**

| Risk | Why it matters | Mitigation |
|---|---|---|
| The C++ batch is big and every machine must rebuild (source hash in the fingerprint) | A mistake costs a day of cluster time | One batch, GTests for each slice, the deploy gate, land OX-15 and OX-19 first because they are small |
| Control socket becomes a new door | It can stop a run | Off by default, Unix 0660, same authority as the console, logged with peer credentials, no TCP |
| Alarm fatigue | A noisy watcher is ignored, which is worse than none | Dedupe, quiet periods, `min_severity`, two-check rule for worker loss, a weekly review of what fired |
| Auto-resume hides a real bug | A crash loop burns GPU hours | Armed only, budget 2 per 6 h, refuses wing-ladder stages until OX-03 is fixed, always notifies |
| Tracked tuning changes semantics for live machines | A pulled file changes a running worker's fingerprint only at restart, so a pull on a running worker is harmless; but a machine conf that still holds a Curriculum key now fails at start | `config check` first; error names the key |
| A schema that ossifies | Every consumer depends on it | Additive-only rule, version integer, one JSON Schema file with a test |
| Docs rot again | The help text, the reference and 0001 already disagree | Generate from the table, `forgectl docs check`, review rule: a console change updates the table in the same commit |
| Bus factor of one | The owner is the operator | The operator's card and `doctor` exist so the next person does not need the owner's memory; they are part of the work, not an extra |
| Over-investing before the curriculum is stable | Stages M3-D3 have not trained (`known-issues.md` C3); tools built around them may churn | Phase 0 and 1 are generic; hold the chain runner and dashboard until two stages have run end to end |

## 7. Unverified items and decisions needed

**Unverified (not checkable from the repository):**

1. The deployed per-machine `worldserver.conf` (`Logger.module`, `Appender.*`), and so whether `Server.log` really is
   nearly empty, and why (OX-05; owner question 1).
2. Whether `AC_LOGGER_*` / `AC_APPENDER_*` environment overrides work for the keys in section 2.3 (the core's
   `AC_<KEY>` mechanism is general).
3. Which logger prints the calendar and guild messages (their source is not in `Animus/`).
4. Whether a rebooted worker's own learner rejoins the cross-machine weight exchange (2.6); how a very slow worker
   affects averaged weights (5.7).
5. Whether the forge's databases hold an administrator account for a SOAP login (2.1).
6. The owner and mode of a socket created inside the container, as seen from the host's ssh user (2.1); whether the
   machines have a Python 3 for a relay (forgectl needs 3.11 only on the operator's computer).
7. Whether `docker` starts at boot on every machine (2.6).
8. What the dispatcher prints as "help" after a plan-command refusal that returns `false` (OX-15).
9. Whether `forge cancel` on the host already stops the workers' learners, making the per-worker console visit in
   `forgectl stage cancel` redundant (OX-09); `cpp-runtime-console.md` says "workers get `STOP`".
10. Who writes `RUNLOG.md`; where `forge camera snapshot` writes relative paths; how often the camera audit runs.
11. Whether the learner logs during a long evaluation (the `learner_silent` threshold, 2.5).
12. Feasibility of `forge camera dump <env>` with GPU observation writing (2.9).
13. Whether training is seeded at all (5.1) and whether evaluation is stable across a rebuild (5.2).
14. Disk use of `archive/`, camera frames and videos (5.4).

**Decisions needed from the owner.**

1. What do the deployed `Logger.module` and `Appender.*` say on the four machines (decides whether routing is a conf
   fix or needs the logger split first)?
2. Is anything meant to start a plan on boot? If not, may the worldserver service become `restart: unless-stopped`?
3. Does a SOAP administrator account exist, and do you want SOAP as a stopgap while the socket is built?
4. Control socket: Unix socket only (recommended), or also the opt-in LAN listener with a token (0001's open question)?
5. Is the `DifficultyLadder` score-based step-down intended (`known-issues.md` A2)? It decides whether a "collapsed
   rung" alert can fire for the combat stages and how to word it.
6. Which notification channel: phone push (ntfy or similar), e-mail, desktop, or all; and which machine runs the
   watcher (the dev machine is proposed)?
7. Is armed auto-resume wanted at all, and with what budget? (Default proposed: off.)
8. Tracked tuning: one global file first with per-stage overlays later (proposed), or per-stage from the start?
9. Retention: may archived runs lose their numbered checkpoints and `tb/` immediately, and videos after 30 days?
10. May `RUNLOG.md` be replaced by `forgectl runs note`?
