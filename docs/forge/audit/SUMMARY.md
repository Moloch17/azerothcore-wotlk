# Audit summary: what to fix, in what order, and what the owner has to decide

Written 2026-10-08 from three audits and the ten reference documents, all of which are in this repository:

| Report | What it is |
|---|---|
| `efficiency.md` | where the time goes, 21 findings, a 12-step measurement plan |
| `operator-experience.md` | 28 console and operations pain points (OX-01 to OX-28), ten designs, a roadmap |
| `repo-hygiene.md` | the fork as a whole: the upstream gap, leftovers, security, build and tooling (R-01 to R-35) |
| `../reference/known-issues.md` and the "Observed issues" at the end of every `../reference/*.md` | code-level findings with `path:line` |

The reports were written by reading the code and a few CPU-only measurements; nothing was run on the cluster. Items
marked HYPOTHESIS or UNVERIFIED in the source reports are guesses until measured. There are no tests (see
`../reference/tests.md`), so nothing in this repository will tell you if a fix breaks something else.

## 1. Decide these first (they block or change everything else)

**Decided 2026-10-08:** (1) the public fork's exposure of LAN addresses and machine names is accepted as is; (2) the LAN is
trusted, so the cluster network hardening (item 2) is optional and drops to low priority; (3) is explained to the owner and
still open.

1. **The public fork.** `origin` is a public GitHub fork. Since 2026-10-07 06:15 it has carried the LAN addresses and SSH
   login names of the cluster (36 lines in 7 tracked files) and machine names that may be people's first names. No
   secrets. Options: accept; redact the tip (cluster.toml becomes untracked with a tracked example; docs say `host` and
   `worker-1..3`); or also rewrite history and force-push (destructive; cannot recall copies). Pushes to `origin` are on
   hold until you choose; `lan` is up to date.
2. **Cluster network security.** The learner's weight hub listens on `0.0.0.0:7702` and unpickles whatever any peer sends
   (`animus/async_sync.py:99-111,212`): anyone on the LAN can run code on a training machine. Ports 7700 and 7701 have no
   authentication either (R-17). Decide whether the LAN is trusted. If not: bind to one interface, add a shared secret,
   replace pickle with a plain tensor format, and load checkpoints with `weights_only=True`.
3. **Upstream.** AzerothCore is 494 commits and 519 files ahead of the fork, not 206 (R-03), growing by about 90 a week.
   A merge applies cleanly in places but does not compile (R-01: both sides added `PCQueue::Reset()`), silently drops a
   forge guard (R-02), and upstream has meanwhile added headless sessions that overlap the forge's own (R-05). Decide:
   merge now, later, or never, and who does it. Every week of delay costs more.

## 2. Findings, ranked

### A. Correctness and broken project rules (principles in `../principles.md`)

| # | Finding | Where | Why it matters |
|---|---|---|---|
| A1 | Some policy masks go beyond "only the physically impossible" (principle 5): timer-based pacing in `SeatMemory`, Core masking heal-on-full-health and refresh-with-plenty-left | `StageScenario.cpp:3711`; `cpp-layout-character.md` | The bot cannot learn what it is not allowed to try |
| A2 | Four charge terms (`GoalSwitch`, `Hazard`, `HealingMana`, `CombatClock`) and the wing crowd threat charge are costs in meaning but categorised as Shaping, so the fade removes them and they never enter the score | `cpp-rewards-routing.md` | Same failure that collapsed stage 3 in the first curriculum |
| A3 | The dungeon stages may show the policy enemy positions through walls (`UpdateWingEnemies`, server-side, within 45 yd) | `cpp-encounters.md` (reach UNVERIFIED) | Breaks principles 1 and 2 |
| A4 | The combat and group stages' class `DifficultyLadder` still steps down below 60% wins and is not persisted | `DifficultyLadder.cpp:93` | Contradicts decision 0010; a restart resets every class to rung 0 |
| A5 | Sim-side ladders (wing rung, drill rung, class tiers) live only in worldserver memory; a resume after any rebuild resets them, and the key to set the wing rung by hand (`Instance.WingRungStart`) is in the cluster fingerprint, so setting it breaks every worker | `StageScenario.cpp:302`; OX-03 | Dungeon training cannot be resumed cleanly |
| A6 | M2's tuning stops at M4: `combat1_fight.yaml` is a second full root and resets lr to 3e-4 and look entropy to 0.001 | `config-yaml.md` | The lessons from M2 do not reach C1 to D3 |
| A7 | Models trained on the live curriculum cannot be exported: `export.py:440` and distillation refuse any camera checkpoint, and all 12 stages have a camera | `py-learner-export-tools.md` | Nothing trained now can ship to the realm |
| A8 | Loot goals (`SeatGoal::Loot`, `Goals.WorldValue`) still exist | `cpp-encounters.md` | Principle 6 (no looting) |
| A9 | Roles drill terms are Outcome but carry negative amounts; `DeathPaid` charges a twice-dead seat twice; `PartyEncounter::Build` dereferences without null checks and a failed `Group::Create` still returns true | `cpp-encounters.md` | Reward bookkeeping and a possible crash |
| A10 | `Aptitude::Of` counts tactical and sustain spells twice (fix needs a Core block revision bump) | `cpp-layout-character.md` | Core columns 11-36 are wrong |
| A11 | The sight block is 2071 wide in move3 and 2903 in every combat stage and no code path was found that carries its weights from M3 to C1 | `cpp-layout-character.md` (UNVERIFIED) | C1 may restart its sight weights from zero |
| A12 | The slow-goal optimiser (`slow_opt`) is not checkpointed and not learning-rate scaled; partner-pool scores are not saved, so a resume restarts both | `trainer.py:2027-2053` | Silent change in behaviour at every resume |
| A13 | A `WEIGHTS` size bound is wrong (`layouts x 4` bytes vs `x 16`); it works only because another term is larger | `AnimusForge.cpp:2494` | A pool with few envs and many layouts would drop the learner |
| A14 | `MentalMap` clears itself at 65000 s: a realm bot loses its whole map after about 18 hours | `cpp-vision-memory.md` | Realm behaviour |
| A15 | `convergence.patience=0` does not force a full budget although the C++ comment says it does | `ForgeConfig.cpp:725` | A "budget run" can end early |

### B. Efficiency (see `efficiency.md`)

| # | Finding | Estimate |
|---|---|---|
| B1 | **M2 on the host is update-bound, not sim-bound.** The cycle (11.1-11.5 s) equals the update's compute time in every window; the sim idles about 49% of each cycle. The logged 4.3-5.0k steps/s counts only the rollout; the real host rate was about 2.17k. Sim-side optimisation is worth nothing here until the update drops below the rollout (about 5 s) | VERIFIED from 2021 updates |
| B2 | `epochs: 2` re-uses every row twice. `epochs: 1`, or epochs balanced per machine, is the largest single lever | up to +90% end to end; effect on learning UNKNOWN: A/B it |
| B3 | The camera is encoded three times per minibatch when `vision_chunk_rows` is set; M2's yaml sets it for small cards so the 24 GB host pays too. Choose the chunk from free VRAM | about 13% of the update |
| B4 | `nn.Embedding` backward on 8,192 camera and 2x2,304 map indices per row | 38% of the update on CPU, GPU 10-25% (HYPOTHESIS) |
| B5 | 88% of seats are cast below 128x64 and upscaled before the wire; the first layer folds exactly for 32x16 and 64x32 | 10-15% of the update (HYPOTHESIS) |
| B6 | Add a wall-clock steps/s to the logs and status: decisions have been made on a number twice the real one | prevents wrong work |
| B7 | A follower that goes non-finite is never repaired: 371 dropped pushes from one worker in one session | a machine's whole contribution |
| B8 | `conf.dist` runs movement stages at 5 ticks per decision (half-batch off); the live run used 1 tick with half-batch; `Envs`, `Cpus` and `HalfBatch` are untracked per-machine keys | a deploy could change sim cost several-fold unnoticed |
| B9 | The learner sits on the SMT siblings of the map threads; the map pool spans both CCDs | one `forge bench` A/B |
| B10 | The mental-map write does an uncached hash lookup per cell step; a last-tile cache fixes it | 20-60% of that function; matters only when rollout-bound |

### C. Operations (see `operator-experience.md`)

| # | Finding | Fix | Effort |
|---|---|---|---|
| C1 | A worker's container never returns after a reboot (`restart: "no"`, `docker-compose.yml:126`, with a stale comment) | `restart: unless-stopped` plus log rotation | an afternoon |
| C2 | No alert when a stage converges, stalls, collapses, the learner dies or a worker drops | a Python watcher with desktop / webhook / command sinks | M |
| C3 | The console only through `docker attach`; replies buried in log noise (the uncapped flood is the dungeon `WingTrace` family) | a control socket (C++), log routing (conf) | L |
| C4 | No machine-readable status; forgectl scrapes colour codes and log text | `status --json` and a schema | M |
| C5 | `forge pause` does not reach workers; `forge clean scenario|exports|all` deletes runs and models without confirmation; idle-only refusals sometimes return success | fix in the same C++ batch | S each |
| C6 | 239 curriculum keys hand-synced; the cluster fingerprint covers only `Animus/` sources (not the core, flags, the learner, the yamls) and a missing hash header becomes `"unhashed"` | tracked per-stage tuning file; widen the fingerprint (one rebuild per machine) | L |
| C7 | `-march=native` on five different CPUs: sim results are not reproducible across machines | portable baseline or contraction off, if reproducibility matters | S/M |
| C8 | The dev machine's conf is stale (`Role = "host"`, 13 archived-stage `Envs`, `MoveRevision`, `Bench.Scenario = "stage4_duel"`); template and code defaults disagree (`Fast.Envs` 32 vs 16, `Bench.MaxEnvs` 256 vs 512) | refresh | S |
| C9 | Build and image: `.dockerignore` omits the 15 GB venv, 3 GB probes and 580 MB models from a context that does `COPY apps`; three different build-type defaults; the Dockerfile says an explicit `--offload-arch=gfx1100` yields unloadable kernels and CMake passes exactly that; 15 of 16 CI workflows are upstream-only | clean up | S/M |

### D. Dead code and leftovers (detail in the reference docs and `repo-hygiene.md`)

> **Resolved 2026-10-08 (dead-code pass, commits on `worktree-agent-a3445328c9b384d68`)** for the C++ items of this list that were
> not part of a pinned layout or the GPU camera: see the "Update 2026-10-08" note of
> [01-forge-core-delta.md](reference/01-forge-core-delta.md) and the "fixed 2026-10-08" marks in the reference docs. The
> audit itself is kept as the dated snapshot it is.

- About 3,600 lines of GPU camera code are used only by `forge gpu scene` and `forge camera diff`; `OfferDevice` ignores
  `Gpu.Observe`; a HIP stream is leaked each time the device alternates; `Renderer::Forget` has no production caller.
- Dead core additions: `Battleground::SetSimOwned`/`IsSimOwned`, `Group::IsSimGroup`, `PathGenerator::SetIncludeFlags`,
  `PCQueue::Reset`, `GetBGObject(..., false)`, `MapUpdater::ParallelFor`; `SetMapUpdateInterval` is a no-op that is still
  called; `Pet::SavePetToDB` keeps a dead body under an early `return`; `OutdoorPvPMgr` reads `AnimusForge.Enable` from
  core code (a gate); the playtest gate (`Forge.Playtest`) is 48 lines in 9 files for a feature abandoned 2026-10-05;
  `RemoteAccess/` and `libsidecar` are compiled but never started.
- Dead data and keys: goal-block order columns 45-86 (always zero, pinned in layouts); the `Duel`, `Raid` and `Party.SizeWeight`
  tuning groups; `Arena.*.MaxRung` (documented, unread); `Arena.*.WeightFinal` (read, undocumented); duplicate
  columns (`respawns`/`rises`, `difficulty`/`combat_rung`).
- Learner features that are off in every live yaml: self-imitation (also refused whenever a camera is present),
  seat sets, the map value-iteration network, the style reward; distillation and `.amdl` export cannot run on camera policies.
- Tools and patches: `spec_builds/generate.py` (writes to a path that does not exist), `forge_classes.py`,
  `rename_runs.py`, `mod-animus-movement.patch` (marked OBSOLETE in its first line), `apps/forge/patches/` as a whole.
  (2026-10-08: `forge_classes.py`, `rename_runs.py`, `spec_builds/` and `mod-animus-movement.patch` were deleted.)
- Documentation: the generators for the Stockade and Deadmines data tables live in gitignored plan folders (the tables
  cannot be regenerated); `camera-vision.GPU.md` is cited on 65 lines in 44 files and is not in the tree; `AGENTS.md`
  has no forge pointer and there is no root README; many comments name deleted stages.
- Good news: zero TODO/FIXME/HACK/XXX/`#if 0` in forge code; the codestyle check passes; no large blob was ever
  committed (largest forge-only blob 634 KB); no secrets in tracked files.

## 3. Suggested order of work

**Phase 0: today, no rebuild.** Decide section 1. Set `restart: unless-stopped` and log rotation on the workers (C1). If the
LAN is not trusted, firewall or bind ports 7700-7702 per machine. Plan the `epochs: 1` A/B (B2) for the next time a stage is trained.

**Phase 1: Python and config only (no C++ rebuild), roughly 8-10 days.**
- Security: replace pickle in `async_sync.py`, bind and authenticate it, `torch.load(weights_only=True)`.
- Add the wall-clock steps/s (B6); choose `vision_chunk_rows` from free VRAM (B3); repair or drop a non-finite follower (B7).
- Checkpoint `slow_opt` and the partner scores; make `combat1_fight.yaml` inherit M2's tuning (A12, A6).
- forgectl: `watch` with notification sinks, `status --json`, `doctor` and `config check`, the missing guard on a local `build`.
- Remove the dead Python (distillation/export paths that cannot run, SIL, VIN, style, the dead helpers) once decided.

**Phase 2: one C++ batch with one rebuild of the cluster (10-14 days).** Everything in C++ goes in together, because every
change forces every machine to rebuild (`../decisions/0014-one-cluster-rebuild-per-plan.md`):
- a control socket and structured status (C3, C4); persist all sim-side ladders outside the fingerprint (A5); a stall
  alarm for the wing ladder; make the cluster `RUNG` broadcast wing-only and reset between stages;
- fix the principle breaks: masks (A1), term categories (A2), seen-only enemy slots (A3), loot goals (A8), the class
  ladder step-down (A4), the roles and party bugs (A9), `Aptitude` (A10, with a Core revision bump), `WEIGHTS` bound (A13);
- delete the dead code (section D), the playtest gate, the dead core additions; align the defaults (C8);
- widen the fingerprint (C6) if wanted, tracked per-stage tuning files, pause fan-out and `clean` confirmation (C5).

**Phase 3: afterwards.** The upstream merge as its own project (R-01 to R-05: do it on a branch against a fresh upstream;
port `Main.cpp` by hand; decide about headless sessions); CI for compile checks; the dashboard and the stage-chain runner;
upstreaming the seven groups of general fixes the audit found.

## 4. Questions for the owner (merged from all reports; the first ten matter most)

1. Public fork: accept, redact the tip, or rewrite history? (Section 1.)
2. Is the cluster LAN trusted, or should ports 7700-7702 get a bind, a shared secret and a safe format instead of pickle?
3. Merge upstream now, later or never, and who runs it? Is upstream's e2e suite wanted (27 modify/delete conflicts per merge)?
4. May the first efficiency A/B be `epochs: 1`? It is the biggest lever but changes learning per sample.
5. Which machines run 1 tick with half-batch versus 5 ticks, and with what `Envs`, `Cpus` and `HalfBatch`? (Untracked
   per-machine confs; only the dev machine's was readable.)
6. May the worker containers become `restart: unless-stopped`? Does anything start a plan on boot, and would
   `Bench.AutoTune` start a benchmark on boot?
7. Control socket: Unix socket only, or also an opt-in LAN listener with a token? Always on, or with the `Control.Enable`
   switch of decision 0001? Is SOAP useful meanwhile (does an admin account exist)?
8. Which notification channel, and which machine runs the watcher? Is automatic resume after a crash wanted (default off)?
9. Tracked per-stage tuning: one global file first with per-stage overlays later, or per-stage from the start?
10. Is bit-for-bit reproducibility across machines wanted? If yes, `-march=native` needs a portable baseline.
11. Is the class `DifficultyLadder` score-based step-down intended (it contradicts decision 0010)?
12. Should the dungeon policy see server-side enemy slots at all (principles 1 and 2)? Should the roles drill losses be Costs?
13. Delete: the `Gpu/` camera path (3,600 lines), `Forge.Playtest`, self-imitation, seat sets, the VIN, the style reward,
    loot goals, the dead non-roles drill code in `PartyEncounter`, `forge_classes.py`, `rename_runs.py`, `spec_builds/`,
    `apps/forge/patches/`? (Principle 17 says yes; each needs your nod.)
14. Export and distillation for camera policies: revive (needs a format for the vision sections) or delete? Nothing live
    can ship until one of the two happens.
15. Should the learner's checkpoints keep the partner pool's scores, and the wing/drill rungs, so a resume continues cleanly?
16. What do the deployed `Logger.module` and `Appender.*` settings say on the four machines (the audit found Server.log
    "almost empty", which contradicts the shipped template)?
17. Is `Forge.Playtest` and the human-capture pipeline (`human/`, the capture format, `forge fieldworld`, 580 MB of
    first-curriculum models) still wanted?
18. Retention: may archived runs lose numbered checkpoints and `tb/` after N days, and videos after 30 days?
19. Licence header wording (198 files say "Animus Forge project") and an `AUTHORS` entry for the fork.
20. Where does the `claude-syntax` dev container come from (no tracked file defines it), and may a compose service replace it?

## 5. Corrections to things I said earlier in this project

- The "24 ms a decision, 6.4 ms reset, 10 ms learner round trip" figures are from 2026-09-17 on a different stage and
  config; the later parallel-core work measured 4.9 ms and 0.77 ms. Do not plan from the old numbers.
- "M2 runs 2-5k steps/s" counted only the rollout phase; the host's real rate is about 2.2k.
- "The upstream gap is 206 commits" was measured against the wrong reference; it is 494.
- "`Server.log` is almost empty and sim messages go to stdout" is the observed behaviour on this setup but contradicts the
  shipped logger template; see question 16.
- The M2 learning-rate story: the stage's score plateau was read against the easy rung's best, so the learning rate
  annealed from about 30M steps (not 140M); this was fixed in the convergence re-baseline (decision 0011) and M2's rate was
  then set to 1.5e-4 (decision 0013). Those changes are on `forge` but M2 has not been resumed on the new build.

Update 2026-10-08: the Gpu/ camera path (owner question 13) was removed; tag archive/gpu-camera.

Update 2026-10-08: the ground probe and layered fields were removed (owner order); the dungeon routes use the navmesh RoutePlanner.

Update 2026-10-08: vision-only movement (decision 0019): the dungeon route, `RoutePlanner`, `RouteShortcut`, `ThreadQueryScope`, `forge route`, the route-built goal layout, the waypoint/progress rewards, `group2_corridor`, `dungeon1_pulls` and Go-Explore were removed; 10 live stages; M4's leader walks stock `PathGenerator` legs.
Update 2026-10-08: layout and protocol cleanup (stream C): loot, gather and interact goals and the unused goal targets (goal block revision 1, 348 -> 207 joint goals), seven unwritten critic-state columns (1958 -> 1927) and the duplicate epochs_done column were removed; the goal order columns, the duplicate episode columns and the MODE_FLAG bit wait for their file owners (known-issues H).

Update 2026-10-08: leftover cleanup (second pass, same unreleased layout generation, protocol 26): the goal block lost its never-written order columns and every learner reader of them (goal block revision 2, 101 -> 68 columns); the duplicate `respawns` and `combat_rung` episode columns were removed (videos fall back to `difficulty`); `MODE_FLAG` value 1 documented as reserved; `RoutePlaces` became `HasSeenPlaces`; seven `[[nodiscard]]` warnings, an unused `fighting` local and three dead includes were fixed; kept on purpose: `ENTITY_LOOTABLE` (width 20, not always zero) and the `detour` name of the revision 4 move table; stale test citations were dropped from the reference docs (known-issues B2, B3 and section H have the details).

Update 2026-10-08: stage 1 Design A (stage1-vision): `move1_controls` restarts on the decoupled ladder: `fade.rungs [1.0, 0.5, 0.25, 0.0]` gated on `arrived_at_rung` 0.85 (no plateau, no step back), `Controls.Withhold0..3` defaults 0.25 / 0.6 / 0.9 / 0.9 (a quarter withheld from the first rung, shaping alone fades last), `layout_sampling.replay_fraction 0`, and three python evaluation arms (`no_flag`, `no_camera`, `no_compass`) in `eval.arms`; the stale `rungs [0.0]` text in stages.md, cpp-encounters.md, config-yaml.md and py-learner-config.md is corrected.

Update 2026-10-08: tick jitter (decision 0021, train/ship parity audit item 7, protocol 27): every decision of every stage lasts DecisionMs less the carried overshoot plus its own (U(0, 50) ms body, 2% spikes of U(50, 400) ms; AnimusForge.Decision.JitterMs / SpikeProb / SpikeMaxMs / Seed), one draw per decision for the pool because the game clock is global; per-second rewards use the real step (StepScale/StepMs), the observation keeps the nominal one; in the fingerprint, doctor, forge status and SPEC.

Update 2026-10-09: M2 search plan step 1: eval arms `no_map` and `no_memory` (Python only, `EVAL_ARMS`), `move2_seek.yaml` plays `no_flag`, `no_camera`, `no_map`, `no_memory` at 64 episodes each (M3/M4 restate `arms: {}`), `ablate_image` edits only the image part of a camera row (a stage with a map block would have had its map crop rewritten by `no_camera`/`no_flag`), and every evaluation (held-out sweeps included) writes `eval_motion_<env_steps>[_heldout_<arena>].npz` raw routes with seed ids, `eval.keep_motion_files` 12. The C++ half (object position in the seek episode info) is specified in reference/known-issues.md.

Update 2026-10-09: the overall tracker judges `convergence.measure` (a binomial share) and checkpoints carry an evaluation signature that resets the trackers and the anneal plateau on a format change (decision 0022); a fine-tune seed carries the value normaliser; `configs/overlays/move2_seek_reseed.yaml` starts move2_seek at its last rung. Still open: the plateau latch (known-issues.md).

Update 2026-10-09: M2 goals, C++ stream (branch m2-goals-cpp, syntax-checked only): goal block revision 3 (108 columns: the held goal's compass columns and five features for each of six room slots and the way on, stage.json `goals.columns.held/place_features` and `goals.place_slots`), seek room goals (glimpse, check by dwell or coverage, slots in glimpse order, the way on from the map's frontier, hindsight `Encounter::AchievedGoal`), the Aid reward category (`RewardLedger::SetAid`, scale `max(0, 1 - progress / Seek.AidUntil)`) with `RoomGoal`/`RoomSwitch` and the `Return` cost, eleven `Seek.*` keys (`Goals` is the temporary switch), 22 new seek episode columns (`revisit_rate` now counts returns); reference docs cpp-blocks, cpp-encounters, cpp-tuning-keys, file-formats, protocol and known-issues updated.
Update 2026-10-09: m2-goals Python stream (W3a-W3c, W4): `GoalHead.pointer` (MLP 5 -> 16 -> 1, last layer zero) over the goal block's revision-3 place slots, read from the manifest (`goals.columns.place_features`, `goals.place_slots`; absent = no pointer), applied in `logits`/`slot_logits`, queue de-duplication, index 0 allowed only when nothing else is offered, `load_actor_state` tolerates the new keys; `bootstrap._reseed_goals` resets `goal_head.*`/slow state and warm-copies the Fight/none embeddings when the goal block revision differs; eval arms `no_goal` and `random_goal` (`MappoTrainer.uniform_goals`); `move2_seek.yaml` `goal_every_decisions: 128` (M3/M4 restate 64), the two arms and headline additions, `configs/overlays/move2_seek_goals.yaml`. Uncompiled and unrun (no torch on the authoring host); needs the C++ goal block revision 3.

Update 2026-10-09: free choice goals, C++ stream (branch fcg-cpp, syntax-checked only): protocol 28 (ACT goal section 8 ints an agent: four plan positions' joint ids and cell words, `GOAL_SLOTS_ON_WIRE` 4, `GOAL_WIRE_INTS`), cell goals for the seek stage (`Seek.GoalSource` 1; `CellGrid.h`: 24 x 24 pooled blocks of the seat's crop, the choosable rule, cell words; the sim latches a block's world point once per ticket against the pose of the crop the learner chose from, plan table of four positions, free re-choice within `Seek.CellSame`, patience rule), goal block revision 4 (122 columns: held2, next, plan_left, goal_from_*), Aid terms `cell_goal`/`cell_progress` and Cost terms `cell_switch`/`cell_lost`/`cell_stale`, twelve `Seek.*` keys, 12 new episode columns, stage.json `goals.cells`/`wire_ints`; nothing run.
Update 2026-10-09: free-choice-goals Python stream (branch fcg-py, uncompiled and unrun, no torch on the authoring host): protocol 28 (ACT goal section 8 int32 an agent: four plan joints and four cell words), `GoalHead.cell` pointer over the 24 x 24 pooled blocks of the map crop (`cell_valid`, `cell_features`, chain feature, queue de-dup by block, joint 159 shared by every cell goal), tickets and `ActingState.plan/serial` in `decide_goals`/`wire_goals`, `RolloutBuffer.goal_cells`, planner hindsight toward `goal_from_*`, `_reseed_goals` 3 -> 4 keep branch, eval arms `random_cell` and `no_plan`, `PER_EVENT` tuple weights, `move2_seek.yaml` additions and `configs/overlays/move2_seek_cells.yaml`; needs the C++ stream (goal block revision 4, `goals.cells`, protocol 28).
