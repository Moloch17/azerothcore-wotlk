# The human-capture tooling and the remaining Python modules

Purpose and scope. Two groups under `apps/forge/python/animus/` that are not the networks ([py-mappo.md](py-mappo.md))
and not the learner's core (`train.py`, `stage.py`, `config.py`, ... in [py-learner.md](py-learner.md)):

1. `animus/human/`: reading the files `mod-animus` records of players on the live realm, building training and judging
   data from them, and comparing bots with players. Mostly an **offline** tool set.
2. The top-level helper modules: `async_sync.py`, `parallel.py`, `env.py`, `device.py`, `blas.py`, `explore.py`,
   `style.py`, `rewards.py`, `stages.py`, `__init__.py`.

Everything is from the tree at `bd32b9dc8`. Nothing was run. `UNVERIFIED` marks what could not be checked.
Related: [protocol.md](protocol.md), [file-formats.md](file-formats.md), [metrics.md](metrics.md),
[config-yaml.md](config-yaml.md), [tests.md](tests.md), [tools-and-ops.md](tools-and-ops.md),
[known-issues.md](known-issues.md), [glossary.md](glossary.md).

## Map table

| Path (under `apps/forge/python/animus/`) | Lines | Role |
|---|---|---|
| `__init__.py` | 7 | sets `HSA_ENABLE_IPC_MODE_LEGACY=0` before torch loads (device buffers) |
| `async_sync.py` | 450 | asynchronous learner trading: `Hub` (leader), `Link` (follower), flat-vector networks, shared files |
| `parallel.py` | 204 | `Ranks` (data-parallel collectives), `share`, `weighted_share`, `Silent` |
| `env.py` | 515 | `ForgeEnv` (one sim socket), `ClusterEnv` (several sims as one pool) |
| `device.py` | 111 | the sim's GPU buffers opened by IPC (`DeviceBuffers`), `host()` |
| `blas.py` | 78 | ROCm gfx12 matmul library selection and TunableOp |
| `explore.py` | 126 | Go-Explore archive of wing cells |
| `style.py` | 341 | movement-style reward (adversarial motion prior) |
| `rewards.py` | 88 | audit that no shaping term out-earns the outcome |
| `stages.py` | 115 | helpers over `stage.json` (spans, revisions, signatures, seed chain) |
| `human/__init__.py` | 4 | package docstring |
| `human/__main__.py` | 271 | the `python -m animus.human` CLI (nine commands) |
| `human/FORMAT.md` | 219 | the capture file format contract with mod-animus |
| `human/README.md` | 59 | tool overview |
| `human/motion.py` | 201 | the one definition of motion features (live: imported by the learner) |
| `human/realism.py` | 136 | realism score against a reference, `eval_motion.npz` writer (live) |
| `human/reader.py` | 595 | capture file framing and parsing |
| `human/tracks.py` | 411 | packets to tracks on the 250 ms grid |
| `human/segment.py` | 281 | clips, trips, fights, deaths, stuck spans |
| `human/dataset.py` | 123 | `human_motion_windows.npz` builder |
| `human/reference.py` | 300 | `human_reference.json` builder and per-unit metrics |
| `human/trips.py` | 100 | `human_trips.json` |
| `human/hard_spots.py` | 104 | `human_hard_spots.json` |
| `human/build.py` | 100 | one pass producing the four files |
| `human/fit.py` | 434 | executor-fit emulator and beam search |
| `human/mapper.py` | 328 | inverse action mapper, `spell_ranks` export |
| `human/prices.py` | 217 | proposed noise prices |
| `human/companions.py` | 105 | per-model companion feedback |
| `human/parity.py` | 1022 | bots vs players statistical comparison |

Tests: `test_human_cli.py`, `test_human_fit.py`, `test_human_mapper.py`, `test_human_motion.py`, `test_human_parity.py`,
`test_human_reader.py`, `test_human_tracks.py` (+ writer `human_capture_writer.py`), `test_realism.py`, `test_style.py`,
`test_explore.py`, `test_async_sync.py`, `test_parallel.py`, `test_rewards.py`, `test_vision_bytes.py` (device images),
`test_protocol.py`, `test_train_run.py`, `test_evaluation.py`.

---

# Part 1: `animus/human/`

## What is live and what is offline

Imported by the learner: `motion.py` (by `style.py:38`, `train.py:56`, `config.py:18`; the protocol comment
`protocol.py:24`)
and `realism.py` (by `train.py:56`). Everything else is reached only through `python -m animus.human` (or tests). In a
training run:

- At every evaluation that has motion tracks (the sim sends kinematic samples, `kinematics_dim == 10`), `score_motion`
  (`train.py:1329`) writes `<run>/eval_motion.npz` (windows of the evaluation's motion, `realism.write_motion`). This
  happens whether or not style is enabled. With `style.reference` set, it adds `realism_emd*` columns; with the style
  reward on, `realism_disc`.
- The style reward needs `style.enabled` and a human windows file (`style.dataset`).
- No live yaml enables either: `style: enabled: false` in `move1_controls.yaml:208` and `combat1_fight.yaml:172`;
  neither
  sets `dataset` or `reference`. Per the owner's notes the human-play-data plan was implemented 2026-10-04 and "not
  deployed" (UNVERIFIED here: nothing in the tree says whether a capture exists).

The writer side is in **mod-animus**, a separate repository: `FORMAT.md:5-6` says mod-animus keeps a mirror at
`doc/capture-format.md` and both must change together with `FORMAT_VERSION`. No mod-animus checkout exists in this tree
(`modules/` holds only the module scaffolding), so the writer's output cannot be verified from here: UNVERIFIED.
The test writer `tests/human_capture_writer.py` is "built from FORMAT.md, not from the reader's dtypes" (README).
`test_human_reader.py::test_reads_the_cpp_serializers_sample_when_present` reads a C++ sample when
`apps/forge/tools/capture-sample.bin` exists (it does not in this tree: UNVERIFIED; the test skips otherwise).

## What is captured (FORMAT.md summary)

Directory `<Animus.Capture.Dir>/<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz` (UTC), `index.json` per closed hour, a `salt`
file; streams `session`, `move`, `action`, `snapshot`, `outcome`, `companion`. Files are sequences of gzip members; each
record is `u16 type, u16 length, payload`; readers skip unknown types by length and take only the known prefix of a
longer record (fields are only ever appended). Format version 3 (`reader.py:38`). Record types:

| Stream | Types (id) | Content |
|---|---|---|
| any | FileHeader (0) | magic `ANCAP\0\0\0`, format, stream, opened_ms, module_revision, realm_build |
| session | SessionStart/Context (1,2), SessionEnd (3), GroupState (4), KnownSpells (5), Latency (6) | who the player is (class, race, level, talent points, item level, map, zone, kind human/companion), group, spells, latency |
| move | Move (10), Speeds (11), MotionEvent (12), MoverState (13), MoveTally (14), MapUpdate (15, PROPOSED, not yet written) | every movement packet unquantised with two clocks (`client_ms`, `server_ms` from format 3), speeds in force, mount/taxi/teleport/stun/... events, per-mover state, cumulative sent/kept counts |
| action | CastRequest..Interact (20-26) | casts, results, ends, selects, item use, attacks, interactions |
| snapshot | Snapshot (30) | self state plus up to 24 hostile and 10 friendly units, auras, cooldowns |
| outcome | Damage..Area (40-47) | damage, heals, kills, deaths, quests, encounters, PvP, area |
| companion | CompanionDecision, Command, Rating (50-52) | the shipped companions' decisions and the owners' feedback |

Identity is pseudonymous: `SHA-256(salt || "player" || guid)` first 8 bytes; no names, chat, accounts or IPs (FORMAT
§4).
Companions (`kind` 1, Move `source` 2, or format 1's synthesised `source` 1) are never human data: every `human_*`
output leaves them out (README "Choices").

## `reader.py` (595 lines)

Framing and parsing. `PREFIX` (`:79`) maps each record type to a numpy structured dtype of its fixed prefix; four types
have variable tails (GroupState members, KnownSpells ids, Snapshot units/auras/cooldowns, CompanionDecision actions;
`_tail :346`, `Batch.snapshot_details :181`). `frame(buf)` (`:217`) finds runs of identical (type, length) records with
numpy chunks, `parse` (`:316`) gathers prefixes per type (strided view for runs of 8+, byte gather for short runs),
`_members` (`:363`) decompresses gzip members one at a time (a truncated final member is dropped whole and counted),
`read_file` (`:395`) yields `Batch`es of about `BLOCK_BYTES` (32 MiB) with the header checked (`_check_header :446`:
wrong magic raises `CaptureError`; a newer format warns and reads the known fields). `APPENDED` (`:140`) lists fields a
later format appended (`MOVE: server_ms`): an older record is read with 0 there (`_rows_with_appended :285`).
`CaptureDir(root, start, end)` (`:535`) walks hour directories in a UTC range; `iter_stream`, `index_report`.
Public surface: `Batch.get/records/tails`, `FileStats`, `read_file`, `read_all` (one hour shard only), `merge` (loses
Snapshot tails), `HourDir.files/index`, `stream_map`.
Tests: `test_human_reader.py` (12 tests incl. truncation, straddling members, unknown types, format 1-3 moves).
Observed: `read_all` of a whole capture would not fit memory (documented "for one hour shard only"); `merge` drops
`_raw` so `snapshot_details` is lost after a merge (documented).

## `tracks.py` (411 lines)

Movement packets to per-player kinematic tracks on the 250 ms decision grid (`motion.DECISION_SECONDS`). `Track`
(`:84`): `samples [T,10]`, `involuntary [T]`, `flags [T]`, `jumps [T]`, `companion`, `latency_ms`, `hour`, `info`.
`player_tracks` (`:214`): per packet a raw sample (mode from flags: flying over swimming over falling; mounted from
mount
events or snapshot flags; speed in force from `Speeds` for the mode, `BASE_SPEEDS` fallback; in combat from snapshot
flag
or recent damage), cuts (taxi, death..resurrect, loading, vehicle, transport spans; teleports; map changes; packets
ONTRANSPORT), involuntary spans (root, stun, fear, knockback up to `KNOCKBACK_MAX` 3 s) flagged not cut, **stand
filling**
(`_fill_and_split :295`: a gap over `max_gap` 1.5 s after a packet with no movement flag, with the next packet within
`HOLD_DRIFT` 0.5 yd and at most `HOLD_MAX` 10 s, is filled with held samples every `HOLD_STEP` 0.5 s), then
`motion.resample`. `SessionTable` (`:126`) holds the latest SessionStart/Context and latency per player;
`CombatIndex` (`:160`) in-combat/mounted from snapshots (trusted `SNAPSHOT_STALE` 3 s) or damage (`COMBAT_LINGER` 6 s).
`iter_shards(cap)` (`:390`) yields `(SessionTable, Shard)` one hour of one map at a time; `build_tracks` (`:327`) leaves
companions out unless `include_companions`.
Duplicated constants (sync hazard): `MF_*` movement flags copied from `UnitDefines.h` (`:39`), `BASE_SPEEDS` from
`Unit.cpp baseMoveSpeed` (`:54`), opcodes from `Opcodes.h` (`:51`).
Tests: `test_human_tracks.py`.

## `segment.py` (281 lines)

`clips(track)` (`:109`): runs of samples with no involuntary and no idle (`idle_mask :75`: no movement and no turn for
`IDLE_SECONDS` 8 s) at least `WINDOW + 1` long, tagged terrain (water/air/ground), mounted and combat shares, purpose
(`fight` if combat share >= 0.5, `trip` if inside a trip, else `other`); `indoors` is always None. `trips` (`:149`):
hindsight destinations (a stay of `STAY_SECONDS` 5 s within `STAY_RADIUS` 3 yd, or an Interact record), at least
`MIN_TRIP_YARDS` 15 and `MIN_TRIP_SECONDS` 3, path point every 8 samples; mode ground/swim/fly or `mounted` when 50% of
a
ground trip is mounted. `fights` (`:212`, gap `FIGHT_GAP` 8 s), `deaths` (`:241`, with corpse-run seconds and yards from
raw packets), `stuck_spans` (`:264`, keys held `STUCK_SECONDS` 3 s moving under `STUCK_YARDS` 1.5).
Tests: `test_human_tracks.py`. Observed: in `_trip` the mode tuple `("ground","swim","fly","ground")` is indexed by an
`argmax` over three counts, so its fourth entry is never used (`:206`).

## `dataset.py` (123 lines): `human_motion_windows.npz`

`DatasetBuilder(stride, max_windows=2_000_000, seed)`: per clip `motion.features`, `step_contexts`, `motion.windows`;
latency weight `latency_weight` (`:39`: 1 up to 150 ms falling linearly to 0.25 at 500 ms); a bottom-k uniform sample
bounded at `max_windows` (random keys, `_shrink :79`). `write` stores arrays `windows f32 [N,8,17]`, `context i16 [N]`,
`weight f32 [N]`, `meta` (a JSON string with per-context `windows`, `seen` and `balance`) via a temp file and rename
(`:96`); `load` (`:117`). Balancing across contexts is left to the consumer (`style.HumanWindows` draws by weight within
a context only).

## `reference.py` (300 lines): `human_reference.json`

Format 1 (`FORMAT`). `ReferenceBuilder` accumulates motion histograms per context (counts in the fixed
`motion.HIST_BINS`) and per-unit metrics (one unit = one player in one hour shard) whose p10/p50/p90 across units are
stored
under `<class>/<spec tree>/<level band>`, `<class>/all/all` and `all/all/all` (`group_key :85`; `level_band :76`: 1-9,
10-19, ..., "80"). Movement metrics (`movement_counts :127`, `movement_metrics :165`): turn reversals/min, stop
starts/min,
strafe and backpedal share, jumps/min, planar speed ratio, swim and fly share; action metrics (`action_metrics :181`):
casts/min, cast failure rate and per code, overheal share, dps. `realism(reference, windows)` (`:273`) per-context
per-feature EMD of bot window steps against the reference (string keys, JSON as read). `CLASS_NAMES` and `TREE_NAMES`
duplicate the class/spec tables.
Tests: `test_human_tracks.py` (`test_movement_metrics_count_reversals_stop_starts_and_strafes`,
`test_realism_is_zero_against_itself`).

## `trips.py`, `hard_spots.py`, `build.py`

`TripPool` (`trips.py`) keeps at most `per_map` (5000) trips per map, a bottom-k uniform sample, writes
`{"format": 1, "maps": {map: [{start,end,seconds,mode,path?}]}}` (`valid_trip` makes the writer refuse malformed entries
because "the forge's reader refuses the whole file over one malformed entry") plus a `.meta.json` sidecar.
`HardSpots` (`hard_spots.py`) clusters deaths, stuck spots, falls (death by falling, or a landing after 1500 ms in the
air) and drownings on an 8-yard grid per map and kind, writing `human_hard_spots.json` (spots sorted by count) and a
sidecar of death causes. `build.build(...)` (`build.py:46`) is the single pass over shards producing
`human_motion_windows.npz`, `human_reference.json`, `human_trips.json` (+meta) and `human_hard_spots.json` (+meta).
**Who reads trips and hard spots is unclear**: FORMAT.md §5 says "forge arenas (TravelEncounter trip pools)" and
"Go-Explore / start pools"; a grep for `human_trips` / `human_hard_spots` across the repository finds only the files in
`animus/human`, `config.py`, `style.py` (a path mention) and tests. The C++ readers are not in this tree and travel
encounters were trimmed with the first curriculum. So these two outputs have no known consumer: UNVERIFIED, likely dead
output (see issues).
Tests: `test_human_tracks.py::test_build_writes_the_four_files`, `test_human_cli.py`.

## `fit.py` (434 lines): the executor-fit study

A numpy emulator of the move block's 25 actions (`MoveControls.h`: `ACTION_COUNT == 25`, `REVISION = 5`, checked in the
C++): held forward/back/strafe, 9 turn rates (+-360, 180, 90, 30, 0 deg/s), 5 pitch rates, ascend/descend, jump, walk
toggle (`_controller :107`, `step :155`, constants `:52-76`). `beam_fit(space, human, beam, start_feet, jumps)` (`:245`)
finds the action sequence reproducing a human clip with a cost of squared position error + `(2.0*heading error)^2` +
`1e-4` per press; `FitStudy` summarises drift at 1/2/5 s, heading error and EMD of feature distributions per context for
two spaces (`controller`, 250 ms; `controller_125ms`). Output `human_fit.json` and `human_fit.md`. Its purpose was the
decision-cadence question and the controller design (player-controller plan).
Duplication hazard: `JUMP_SPEED_Z`, `GRAVITY`, `DIAGONAL`, `WALK_RATIO`, `BACK_RATIO` copy
`PlayerController.h`/`Unit.cpp` (`fit.py:52-61`) and are copied again in `parity.py:69-84`. The emulator ignores terrain
and collision.
Tests: `test_human_fit.py`.

## `mapper.py` (328 lines), `prices.py` (217 lines), `companions.py` (105 lines)

`mapper.map_movement` (`:97`) maps a clip back to the nearest move action per decision with a confidence (20 s segments,
beam fit, cost of each alternative over `LOOKAHEAD` 4 decisions); `map_cast` maps a spell through the `spell_ranks`
chain
to a catalog action of a layout manifest; `export_spell_ranks` (`:206`) reads `spell_ranks` from the forge DB container
(`ac-animus-forge-database`, `docker exec ... mysql`, password from `DOCKER_DB_ROOT_PASSWORD` or the container's own
`MYSQL_ROOT_PASSWORD`) or parses an SQL dump; `validate_movement` / `validate_casts` give accuracy targets
(95% / 99%). The docstring says "MoveControls.h, revision 2" (`mapper.py:6`) while the live move block is revision 5
(`stage.json` and `MoveControls.h:40`); the action list (25) is unchanged, the docstring is stale.
`prices.propose` (`prices.py`) proposes `Actions.Jitter`, `Options.JitterDecayMs`, `Actions.Effort`, `Actions.Repeat`,
`Actions.RepeatFree`, `Actions.Fidget`, `Actions.SettleGraceMs` so that the median human pays at most `budget` (0.005)
per minute per term. It **never writes config**. `CURRENT` (`prices.py:38`) hardcodes the current values (0.05, 2500,
0.004, 0.03,
3, 0.01, 500); the C++ defaults match for the ones checked (`Jitter 0.05`, `JitterDecayMs 2500`, `Fidget 0.01`,
`SettleGraceMs 500`, `RepeatFree 3`, `CurriculumTuning.h:366-456`; `Effort`, `Repeat` UNVERIFIED). Fidget is a proxy.
`companions.report` aggregates per model ratings, commands, dismissals, overrides and deaths from the companion stream;
the companion feature is parked per the owner's notes, so this has no current use.
Tests: `test_human_mapper.py`, `test_human_cli.py`.

## `parity.py` (1022 lines): bots against players in one capture

Not used by training. `study_capture(cap)` (`:507`) reads a capture and records per mover (human or companion) a long
list of measurements (`measure_mover :293`): speeds relative to the speed in force per mode, turn and pitch rates,
starts/
stops and run lengths, heartbeat spacing, facing threshold, keyboard-turn ratio, jump speed, gravity, jump air-time
error,
terminal velocity, vertical share, rises and slopes walked, falls, refusals (`MoveTally`), plus per-session motion
histograms. `compare` runs `SPECS` (`:143-185`, each a `Spec` with a threshold and normalisation) over strata
(class, level band, mount, form, zone, mode, combat), pooled upwards (zone, band, form, class) when small, with a
bootstrap by session (default 1000, 95%) and Benjamini-Hochberg at FDR 0.1 per section. A row is `attention` when its
interval lies wholly above the threshold, `pass` when wholly at or below, `inconclusive` otherwise or when too few
samples (30/3 for distributions, 10/2 for rates). `controller` rows test the controller and the client rules;
`behaviour` rows inform training only. `timing` (`:882`) recommends training jitter if the realm's tick p95 exceeds 50
ms
or decision intervals deviate more than 20% from 250 ms. `report` + `write` give `human_parity.json` and `.md`.
Its definitions are in `.agents/plans/player-controller/parity.METRICS.md` (gitignored, not in the tree: UNVERIFIED).
Duplicated constants: `parity.py:69-84` (`GRAVITY`, `JUMP_SPEED`, `SWIM_JUMP_SPEED`, `STEP_UP`, `TERMINAL_VELOCITY`,
`WALKABLE_DEG`, `HEARTBEAT_MS`, `MOUSE_FACING_THRESHOLD`, ...) copy `PlayerController.h`/`ReportCadence.h`; the
proposals
section is how the owner decides to change the C++ constants.
Tests: `test_human_parity.py` (7 tests with synthetic clients).

## `motion.py` (201 lines): the one definition of motion features (LIVE)

Kinematic sample columns (`T,X,Y,Z,YAW,PITCH,MODE,MOUNTED,SPEED,IN_COMBAT`, `SAMPLE_DIM 10`). `features(samples)`
(`:63`) turns `[T,10]` samples of one unbroken track into `[T-1, F=17]` features in the body's own frame and in units of
the speed in force: `fwd, lat, up, planar, moving, yaw_rate, course_sin, course_cos, pitch, pitch_rate, accel`, four
mode one-hots, `mounted`, `in_combat`; the continuous ones clipped to +-3. `context_id` = `mode*4 + mounted*2 +
in_combat`
(16 contexts), `context_name`. `windows(feats, contexts, window=8, stride)`; `resample` (irregular packets to the grid,
cut at gaps over 1.5 s); `histograms` and `histogram_distance` (EMD on fixed `HIST_BINS`: 61 edges over [-3,3], planar
31
over [0,3]); `features_of_tracks(samples, starts)` (`:179`) is `features` over many tracks laid end to end (how the bot
side reads every seat at once). Invariant: "nothing else may compute motion features" so the human and bot sides never
drift. `DECISION_SECONDS = 0.25` is a constant that must equal `AnimusForge.DecisionMs` (no check).
Tests: `test_human_motion.py`, `test_style.py::test_the_seats_windows_are_the_players_features_exactly`.
Observed: the docstring of `histograms` (`:156`) is garbled ("moving steps only for the course-dependent features is not
needed").

## `realism.py` (136 lines) (LIVE)

`load_reference(path)` (`:26`): format 1 only, histograms must have `len(HIST_BINS)-1` bins; context keys become ints.
`score(feats, contexts, reference)` (`:51`): per context and feature EMD, `realism_emd_<context>` = mean over features,
`realism_emd` = mean over contexts weighted by the seats' steps; unscored contexts listed. `columns(reference)`;
`tracks_features`, `motion_windows(feats, contexts, window, cap, rng)` (uniform draw of `cap`, weight = inverse share),
`write_motion(path, windows, context, weight, meta)` atomically writes `eval_motion.npz`.
Tests: `test_realism.py` (6 tests).
Two implementations of the same distance exist: `reference.realism()` (string-keyed raw JSON) and `realism.score()`
(int-keyed loaded reference). `__main__.cmd_realism` (`:160`) runs both and overwrites the headline with
`realism.score`'s so
`realism.json` agrees with `eval.csv`.

## `__main__.py` (271 lines): the CLI

`python -m animus.human <command>`: `summary`, `build`, `fit`, `mapper-validate`, `spell-ranks`, `prices`, `companions`,
`realism`, `parity`. All take `--out`; most take `--capture` and `--from/--to` (`yyyy-mm-dd[Thh]` UTC). `parity` is the
odd one: it takes a positional `capture_dir` and an `--out` that is a file path (`.json` or `.md`; both are written)
(`cmd_parity :182`, parser in `main :192`). Defaults: `build` `--stride 1`, `--max-windows 2_000_000`, `--trips-per-map
5000`; `fit` `--max-clips 500
--beam 32`; `prices` `--budget 0.005 --max-clips 2000 --beam 16`; `parity` `--bootstrap 1000`.
Tests: `test_human_cli.py` (4 tests), `test_human_parity.py::test_the_cli_writes_both_reports`.

## Observed issues (human)

1. `human_trips.json` and `human_hard_spots.json` have no consumer in the tree; FORMAT.md §5 still names
   "TravelEncounter"
   (trimmed with the first curriculum). Likely dead output; owner to decide.
2. Physics and controller constants copied into `fit.py` and `parity.py` from C++ headers: a sync hazard (no test ties
   them to the headers; `test_human_mapper.py::test_move_local_indices_follow_movecontrols` pins only the action order).
3. Two EMD/realism implementations (`reference.realism`, `realism.score`).
4. `parity` CLI signature differs from the other commands.
5. `mapper.py` docstring says move revision 2; live is 5.
6. `motion.histograms` docstring garbled; `segment._trip` mode tuple has an unreachable entry.
7. `MapUpdate` (type 15) is documented as PROPOSED and "not yet written by mod-animus" but `parity.timing` reads it; the
   realm-timing section is empty until the writer exists (UNVERIFIED which side is current).
8. `FORMAT.md` references `.agents/plans/human-play-data/` (gitignored, absent).
9. `companions.py` and the companion records serve a parked feature.

## Reviewer notes (human)

Decide first whether the offline pipeline stays: only `motion.py` and `realism.py` are on a live path, and the live
curriculum has the style reward off. If it stays, keep `motion.py` as the single feature definition and add a test that
the C++ kinematic sample (protocol 20, `SAMPLE_DIM 10`, `Spec.kinematics_dim`) matches `FORMAT.md` §3.

---

# Part 2: the remaining modules

## `__init__.py` (7 lines)

Sets `os.environ.setdefault("HSA_ENABLE_IPC_MODE_LEGACY", "0")` before anything imports torch (ROCm IPC buffers open
only
in non-legacy mode). The worldserver sets it for the learners it spawns.

## `env.py` (515 lines): the sim client

**`ForgeEnv(socket_path, connect_timeout=600, rank=0, ranks=1, device=None)`** (`:18`). Connects (Unix path or
`tcp://host:port`, retry every second until the timeout, `_connect :58`), sends `HELLO(version, rank, ranks)`, reads
`SPEC`, refuses a protocol mismatch (`:37`), sizes one receive buffer for the largest group STEP, and answers an
optional
`DEVICE` offer (`_answer_device :201`: opens the sim's GPU buffers if rollouts run on that GPU, replies `DEVICE_ACK`).
Methods: `reset()`, `step(actions, goals, look)`, `send_act(env_begin, ...)` (a stage with look heads always sends a
look,
`LOOK_HOLD` by default), `receive_step()`, `set_mode(evaluate, seed_base, episodes, baseline, first_seed, arena,
stand_in)`, `set_layout_weights`, `set_stage_progress(progress, shaping_scale, cost_scale)`, `set_replay`,
`set_explore_starts`, `close`. The sim is lock-step: it blocks until each group's ACT arrives. The wire encodings are in
`protocol.py` ([protocol.md](protocol.md)). Tests: `test_protocol.py`, `test_vision_bytes.py`, `test_free_look.py`
(a sim over a socket), `test_train_run.py`, `test_evaluation.py`.

**`ClusterEnv(endpoints, connect_timeout, rank, ranks, timeout=60)`** (`:245`): several sims as one pool (the host's own
first, then each cluster worker's) with their envs laid end to end and every sim's groups as groups of the whole. A
worker
that fails (socket error or no answer within `timeout`) is dropped (`_drop :302`): its envs sit out (rows come back with
no character present via `_absent :315`, so they are no samples) and `rejoin()` (`:352`, called between rollouts, every
10 s per worker) reconnects it, resends the last weights/replay/explore starts and hands the fresh STEP to the caller.
The
host's own sim (index 0) is not optional. All sims must have the same SPEC apart from `num_envs`/`env_groups` (`:289`).
`set_mode` shares an evaluation's seeds among live sims in proportion to envs (`_shares :507`). `sat_out` is a per-env
bool for the caller.
Observed: `ForgeEnv.step` and `ClusterEnv.step` accept `goals` and `look` positionally in the same order; `set_mode`
still
carries a `baseline: str` name (the only baseline the sim has is `random`; protocol field `protocol.py:65`).
Quirk: `ClusterEnv._owner` does a list `.index` lookup per call (`:396`), O(groups).

## `device.py` (111 lines)

`host(array)` (`:23`): a device tensor copied down, anything else unchanged (used all over the trainer). `DeviceBuffers`
(`:41`): opens the sim's exported IPC handles via `hipIpcOpenMemHandle` on torch's own `libamdhip64` through ctypes and
wraps them as tensors without copying (`__cuda_array_interface__`): `obs [E,A,O]`, `state [E,S]`, `mask [E,A,N]`, and
with a
camera `image [E,A,I]` uint8. Views stay valid until the sim writes that group's next STEP, which it does only after the
ACT, so a decision must read them before it answers (this is why `_Downloads` waits before the action goes back).
`open_buffers(spec, device, envs, handles, rollout_device)` (`:88`) returns `(buffers, "")` or `(None, reason)`:
declined
if the rollout is not on the GPU, torch is not a HIP build, the GPU index differs, or the env count differs from the
SPEC's. HIP only (the name `hipIpc*` is hardcoded).
Tests: `test_vision_bytes.py::test_with_device_buffers_the_images_leave_the_step_but_the_final_images_stay` (decoding
side); the IPC open itself needs a GPU.

## `blas.py` (78 lines)

`prepare(device)` (`:50`): on a ROCm gfx12 device only, sets `DISABLE_ADDMM_CUDA_LT=1`, selects rocBLAS
(`preferred_blas_library("cublas")`), enables TunableOp with a per-arch CSV in `var/animus-forge/tunableop` (override
`ANIMUS_TUNABLEOP_DIR`) and registers an atexit writer. `save()` (`:35`) is called after each update
(`train.py:1577, 1760`) and writes the tunings for the first `TUNING_UPDATES = 20` updates, then disables tuning so new
shapes use rocBLAS defaults (a measured memory/time blow-up, comment `:21-31`). Called once from `train.py:2304`.
Reason: hipBLASLt has no tuned fp32 kernels on RDNA4; a learner step took 111 ms against 28 ms (comment). Returns None
and
does nothing on CUDA, CPU, and pre-gfx12 ROCm. No test.

## `parallel.py` (204 lines): data-parallel ranks

`Ranks(rank, world, address, device, iface, timeout)` (`:22`): with `world == 1` everything is a no-op and
`torch.distributed` is not imported. Otherwise `nccl` if every rank has its own GPU (device count >= world) else `gloo`;
sets `MASTER_ADDR/PORT`, `GLOO_SOCKET_IFNAME`; initialises a process group with a timeout (default 300 s) and a **second
group for the update thread** (`update` attribute, `:60`) so the run thread's broadcasts and the overlap worker's
reductions cannot interleave across ranks. Methods: `sum`, `mean`, `average_gradients(parameters)` (one flat all-reduce,
all parameters included whether or not they have a gradient here, with a presence count so an unused head is left
without a gradient as one learner would), `average_parameters(modules)` (rank_sync "weights"), `broadcast_module`,
`broadcast` (pickled objects), `gather`, `any`, `barrier`, `close`. `share(total, world, rank)` and
`weighted_share(total, weights, rank)` split seeds/sims; `Silent` swallows every call (follower ranks' writers).
Used when `mappo.rank_sync` is "gradients" or "weights"; "async" uses `async_sync.py` instead (`train.py:549`:
`async_ranks = ranks > 1 and rank_sync == "async"`). Which mode the live cluster runs is UNVERIFIED (the sim injects
`mappo.rank_sync`, `LearnerProcess.cpp:138`).
Tests: `test_parallel.py` (two real processes against a fake sim).
Reviewer notes: `Ranks.update` is created with `object.__new__` and a copied `__dict__` (`:55-60`), which shares
`_dist`;
`any()` uses `torch.cuda.current_device()` under nccl; `average_gradients` calls `.tolist()` on a device tensor (a
sync).

## `async_sync.py` (450 lines): asynchronous learners (`rank_sync = async`)

Nobody waits for anybody. The leader (rank 0, the host's learner) owns the run; followers train on their own sims at
their
own pace and trade with it over plain TCP (length-prefixed pickles, `_send/_receive :94-111`, trusted LAN).

- Wire vector: `flatten(modules)` (`:65`) = every parameter then every floating-point buffer (normaliser statistics and
  the
  value normaliser) as one float32 vector; `assign` loads it back (`:72`); `parameter_count`. Only the parameters are
  traded as deltas; the statistics are the leader's and a follower takes them whole (a variance moved by another rank's
  delta once went negative and made every weight NaN; comment `:25-30`).
- **Mixing rule** `mix(center, delta, pushed_steps, since_base)` (`:85`): `center += alpha * delta`,
  `alpha = min(0.5, pushed_steps / (pushed_steps + steps the centre took since the follower's base))`. `MAX_ALPHA =
  0.5`.
- `Hub(address, modules, shared)` (`:194`): listens on `0.0.0.0:<port>`, daemon thread per connection (`_serve :248`),
  messages `hello`, `push`, `files`, `fetch`; keeps one waiting push per (rank, base_steps) (`_post :277`, merging step
  counts and episode lists so the leader's memory does not grow while it is busy); `at_safe_point(run)` (`:307`) folds
  the
  pushes in when no update is running, skipping a push with non-finite weights, adds the follower's env steps and
  finished episodes to the run, assigns the new centre, and calls `run.trainer.sync_rollout()`; `set(**control)`
  publishes
  the leader's decisions (entropy, lr scale, held classes, layout weights, replay seeds, stop); `share(listing)` adds
  files and bumps a `shared` counter so followers fetch; `close()` sends stop, sleeps 2 s, closes.
- `Link(address, rank, modules, every, timeout)` (`:348`): `hello()` takes the leader's networks, then a thread `_trade`
  sends the push (delta of weights since the base, env steps and episodes since the last push) and receives the reply;
  at
  `at_safe_point` the follower keeps the progress made while the push was out and puts it on the new centre
  (`weights = centre + (now - pushed)`), applies the leader's control (`run.apply_control`), and pushes again after
  `every` updates (`mappo.weight_sync_every`).
- `fetch_shared(address, rank, root, timeout)` (`:160`) and `shared_listing(root, paths)` (`:141`): the leader serves
  the
  checkpoints the run read (parents, teachers, cast agents, partners), keyed by path relative to the runs directory,
  with a
  SHA-256 each; a follower fetches what it lacks before it sets up, written to `*.rank<k>.part` then renamed. A fetched
  path must resolve under `root` (`:170`); the hub looks the name up in its own listing, never joins it onto a path.
Data flow: constructed in `TrainingRun.__init__` (`train.py:748-760`) with `[actor, critic, value_norm, style.disc]`.
Config: `mappo.rank_sync`, `mappo.weight_sync_every`, `dist_address`, `dist_timeout` (`TrainConfig`).
Tests: `test_async_sync.py` (8 tests including a real socket exchange, statistics never traded as deltas, pushes kept as
one). Security note: pickle over an unauthenticated TCP port (`Hub` binds all interfaces); acceptable only on the
cluster
LAN, as the module says.
Observed: a lost leader stops a follower (`_trade`, `:395`); a follower's `Link.at_safe_point` assumes `self.pushed` is
set
whenever a reply lands (it is set when the push is queued, `:424`).

## `explore.py` (126 lines): Go-Explore starts for the wings (live: `dungeon2_ragefire`, `dungeon3_deadmines`)

A wing (dungeon) run is long and successes are rare, so cells reached are archived and a share of later resets start
from
one. A **cell** (`Cell`, frozen) is `(arena index, tier, cleared-pack bitset as 4 words of 24 bits, party yard / 16)`.
`mark_columns(episode_info_names)` (`:50`) returns the column indexes of `wing_started, wing_arena, wing_tier,
wing_marks`
and, for 8 marks, `wing_mark<k>_packs<0..3>`, `_yard`, `_seconds`, or None if the stage reports none (then
`train.py:714` prints that no wing arena exists and disables the feature). `cells_of(rows, columns)` (`:61`) extracts
(cell, seconds into the run) from ended-episode rows, taking each run once even though its rows come once per seat.
`ExploreArchive(max_cells=4096, depth_weight=1.0)` (`:83`): `add(cell, seconds, env_steps)` counts visits and keeps the
soonest time; past `max_cells` the shallowest, most-visited cell is evicted; `table(size=64)` returns the cells with
depth
> 0 by weight `(1 + depth_weight * depth/deepest) / sqrt(visits + 1)`; `state_dict`/`load_state_dict` (saved in the
checkpoint as `explore`, `train.py:1126`). The sim receives the table with `EXPLORE_STARTS`
(`ForgeEnv.set_explore_starts`)
and starts `share` of training resets there; evaluation always starts at the door (`train.py:2086-2093`).
Config: `explore.enabled/share/table_size (1-64)/max_cells/depth_weight` (`config.ExploreConfig`).
Tests: `test_explore.py` (2 tests).
Quirks: the 24-bit words exist because an episode-info float holds 24 bits exactly (comment `:23`); `TABLE_SIZE = 64`
and
`MARKS = 8` mirror C++ constants (`InstanceEncounter::EXPLORE_MARKS`, `MAX_EXPLORE_STARTS`) with no cross-check;
`cells_of` dedups by the whole row's values (`:73`), so two different seats' identical runs in one batch collapse
(intended).

## `style.py` (341 lines): the movement-style reward (switch: `style.enabled`, off everywhere live)

An adversarial motion prior. `Discriminator(window, hidden)` (`:104`): MLP over a flattened window of 17-feature steps
plus the one-hot of the context (16), output one number (+1 player, -1 bot). Trained least-squares:
`(D(human) - 1)^2 + (D(bot) + 1)^2 + (grad_penalty/2) * |dD/dwindow|^2` on human windows. A seat is paid
`reward_of(D) = max(0, 1 - 0.25 (D - 1)^2)` (`:46`) on every decision that ends a full window of its own unbroken
motion,
in a context the players showed; paid `coef * scale` into the buffer's rewards (`train.py:1786-1789`; `scale` = the cost
ladder's scale when `style.ladder`). It never enters the outcome score.
`HumanWindows` (`:52`): loads `human_motion_windows.npz`, draws by weight per context (`sample :93`); refuses a file
whose
window differs from `style.window`. `StyleReward` (`:123`): `begin(step)`, `record(t, rows, part)` per STEP (the sim's
kinematic samples, protocol 20), `finish(dones, samples, last_step)` (`:207`) computes each seat's features track by
track with `motion.features_of_tracks` (a track is cut at an episode end, an absent seat, or a clock that did not
advance),
pays, and trains D (`train :263`, `minibatches` x `batch` windows, every rank takes every step, gradients averaged);
carries
state across rollouts (`prev_sample`, `carry_feats`, `carry_run`, `tail`). `state_dict` saves D and its optimizer with
`window` and `hidden`; `load_state_dict` returns False (D left fresh) on a shape mismatch. Async learners trade D with
the
policy networks (`train.py:748`).
Config: `style.{enabled,dataset,reference,coef 0.02,lr 1e-4,hidden (256,256),grad_penalty 10,window 8,minibatches
4,batch 512,ladder true,eval_windows 200000}`
(`config.py:271`; `__post_init__` validates).
Depends on: `human/motion.py`, protocol 20 kinematics (`Spec.kinematics_dim`), `human_motion_windows.npz`.
Tests: `test_style.py` (11 tests).
Observed: `StyleReward.train` always calls `optimizer.step()` even for a rank with no windows (needed so averaged
gradients
line up, comment `:268`); `disc_mean` is used by `train.score_motion` only. Unlike SIL it has no camera restriction.
Cost: windows of the whole rollout `E*A*T` are featurised on the host each update (numpy).

## `rewards.py` (88 lines): the reward-mix audit (live)

`outcome_terms(stage)` (`:41`): terms whose category in `stage.json "reward_terms"` is `outcome` or `cost`, else the
fallback `OUTCOME_TERMS = (kill, clear, arrive)`. `reward_mix(row)` reads `episode_reward_*` columns of a metrics row.
`audit(mix, outcome_terms, max_shaping_share=0.5)` returns the largest shaping term that earns more than half the
largest
outcome term (earnings only, never charges), `describe` formats the warning. `train.py:2119-2138` calls it and repeats
the
warning every `WARN_EVERY = 25` updates. The module docstring records three past faults (a farmable resurrection, a goal
paid per decision held, an order nudge) that motivated it. Tests: `test_rewards.py`.
Observed: `rewards.audit` compares against the *largest* outcome term, so a stage with a small purpose term and a large
unrelated outcome term (e.g. `kill` in a movement stage, if reported) would mask a shaping runaway; UNVERIFIED how
often.

## `stages.py` (115 lines): helpers over `stage.json` (live)

`load_stage(layouts_dir, scenario)` (`:22`), `seed_chain` (`:28`), `merges` (`:33`), `arena_names/arena_state_span`,
`block_spans(stage, layout)` -> `{block: (obs span, action span)}`, `block_revisions`, `revised_blocks(old, new,
layout)`,
`layout_signature` (`:74`; sha1 of the blocks' names, spans and revisions, 12 hex chars), `layout_changes(old, new)`
(`:85`;
human-readable changes per layout, used by `evaluate.py:85`, `tools/resume_check.py`), `model_names` (used by export).
Consumers: `config.py`, `bootstrap.py`, `distill.py`, `partners.py`, `export.py`, `evaluate.py`, `train.py`. Tests:
`test_stage.py`, `test_stage_json_diff.py`, `test_layout_revisions.py`, `test_compass_split.py`. `stage_dir` is called
by
`load_stage` and many tests. `Span` is defined after its first use in an annotation (`:43` vs `:50`), fine under
`from __future__ import annotations`.

## Observed issues (misc)

1. `env.ForgeEnv.set_mode(..., baseline="")` keeps a baseline-name parameter although `random` is the only baseline.
2. `ClusterEnv._owner` is O(groups) per call.
3. `async_sync.Hub` pickles over an unauthenticated socket bound to all interfaces.
4. `explore.py` mirrors C++ constants without a check.
5. `blas.py` has no test; `device.py`'s IPC path has no CPU test.
6. `style.py` and `human/*` are switched off live; their value depends on a capture that may not exist.
7. `parallel.Ranks.any` and `average_gradients` do host reads (a sync) per call.
8. See the human issues list above.
