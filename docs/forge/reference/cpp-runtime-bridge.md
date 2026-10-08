# C++ runtime: the bridge (protocol, lock-step server, device buffers, cluster link)

Scope: `A/Bridge/Protocol.h`, `LockstepServer.*`, `ClusterLink.*` and the code in `A/AnimusForge.cpp` that speaks them
(`RemoteDecision`, `SendSpec`, `OfferDevice`, `SendStep`, cluster polling). `A/` = `src/server/game/Animus/`. Entry point and
map table: [cpp-runtime.md](cpp-runtime.md). The Python mirror, field by field, is in [protocol.md](protocol.md); byte
layouts of files are in [file-formats.md](file-formats.md).

## Wire protocol (`A/Bridge/Protocol.h`)

Version 25 (`Protocol.h:180`; `apps/forge/python/animus/protocol.py:14` is also 25). Every message is a `MsgHeader {u32 Type,
u32 Length}` followed by `Length` bytes, little-endian, packed (`#pragma pack(1)`, `Protocol.h:212`); `static_assert` on
little-endian (`:186`). The sim is the server (the listening side), the learner the client.

`MsgType` (`:188-210`): 1 Hello, 2 Spec, 3 Step, 4 Act, 5 Close, 6 Mode, 7 Weights, 8 Replay, 9 Device, 10 DeviceAck,
11 Progress, 12 ExploreStarts.

| Message | Direction | Payload | Handled at |
|---|---|---|---|
| HELLO | learner -> sim | `HelloMsg {Version, Rank, Ranks}` (12 bytes) | `LockstepServer::AcceptClients` (`LockstepServer.cpp:186-210`) |
| SPEC | sim -> learner | `SpecMsg` (96 bytes, `static_assert` `:260`), `u32 layoutCount`, `LayoutMsg[n]` (`ObsDim`, `NumActions`, 48-byte name), then the episode-info column names comma-joined with no terminator | `Forge::SendSpec` (`AnimusForge.cpp:2780`) |
| DEVICE | sim -> learner | `DeviceMsg {Device, Envs, ObsHandle[64], StateHandle[64], MaskHandle[64]}` plus one more 64-byte handle when the stage has a camera | `OfferDevice` (`:2824`) |
| DEVICE_ACK | learner -> sim | `DeviceAckMsg {Accepted}` | `AwaitDeviceAnswer` (`:2879`) |
| STEP | sim -> learner | `StepHeader {u64 Decision, u32 EnvBegin, u32 EnvCount}` then the arrays below | `SendStep` (`:2938`) |
| ACT | learner -> sim | `ActHeader {EnvBegin, EnvCount}`, `i32 actions[rows]`, optional `i32 goals[rows*2]`, optional `i32 look[rows*LookHeads]` | `RemoteDecision` (`:2541-2588`), `CutAct` (`Protocol.h:373`) |
| MODE | learner -> sim | `ModeMsg {Mode, SeedBase, Episodes, Flags, FirstSeed, Arena, char Baseline[32]}` | `ApplyModes`/`ApplyMode` (`:3054`, `:2746`) |
| WEIGHTS | learner -> sim | `u32 Count`, `f32[Count]` (per layout x `MAX_SPECS`) | `:2615-2634`, `SetLayoutWeights` |
| PROGRESS | learner -> sim | `ProgressMsg {Progress, ShapingScale, CostScale}` (floats) | `:2636-2664`; scales clamped to [0,1], NaN -> 1, each clamp logged once per run |
| REPLAY | learner -> sim | `ReplayHeader {SeedBase, Fraction, Count}` + `u32[Count]` (max `MAX_REPLAY_SEEDS` = 65536) | `:2666-2686` |
| EXPLORE_STARTS | learner -> sim | `ExploreStartsHeader {Share, Count}` + `ExploreCell[Count]` (32 bytes, max 64) | `:2688-2713` |
| CLOSE | either | empty | `LockstepServer::Receive*` drops all clients |

STEP array order, exactly as `SendStep` builds `chunks` (`AnimusForge.cpp:2991-3015`): header; obs `f32[E*A*O]`; state
`f32[E*S]`; mask `u8[E*A*N]`; layout `u16[E*A]`; present `u8[E*A]` (0 empty seat, 1 played, 2 the "human" stand-in's row);
reward `f32[E*A]`; done `u8[E]`; terminated `u8[E]`; final_obs, final_state, episode_info for the **ended envs only** (rows
of envs whose `Done` is set, gathered into `_endedObs/_endedState/_endedInfo`); episode_seed `u32[E]`; kinematics
`f32[E*A*10]`; then, only with a camera, image `u8[E*A*I]` (omitted when device buffers carry it) and the ended envs'
final images; then, only with a map block, the map crops `u8[E*A*M]` (always on the socket) and the ended envs' final
crops. obs/state/mask are replaced by empty chunks when the rank's device buffers are on (`device ? none : chunk(...)`).
E here is the rank's share of the group (`RankGroup`).

ACT acceptance (`RemoteDecision`): an ACT is taken only when `EnvBegin == rows.Local`, `EnvCount == rows.Count`, no MODE is
pending and `CutAct` finds a valid length (`actions` alone, or `actions + goals`, each optionally followed by the look
section). With `LookHeads > 0` the look section is required and every value must be inside its head's size
(`BadLookRow` against `Vision::FreeLook::HEAD_SIZES`); otherwise the learner is dropped. Without goals the goals array is
filled with -1 (`AnimusForge.cpp:2580-2583`). Anything else, a wrong length or an unexpected type, logs an error and drops
every client (`:2715-2722`).

`ModeMsg.Flags`: `MODE_FLAG_STAND_IN = 2` (`Protocol.h:274`). Flag value 1 (bit 0) is unused (it was
`SCRIPTED_OPPONENTS`); the comment calls it "Bit 1", which reads as bit index 1, the opposite of what the constant does:
ambiguous wording. The sim does not check that unknown flag bits are zero (`ApplyModes` only compares the flags of the
ranks with each other).

Version history is in the comment `Protocol.h:142-179` (10 .. 25). A mismatch is refused in HELLO
(`LockstepServer.cpp:195-199`).

### `LockstepServer` (`A/Bridge/LockstepServer.{h,cpp}`)

POSIX sockets, world thread only. API: `Listen(path)`, `Shutdown`, `AcceptClients(ranks, onIdle)`, `DropClient` (drops
**all** ranks), `HasClient`, `Clients`, `Use(rank)`, `Send(type, chunks)`, `Receive(type, dst, size)`, `ReceiveAny(type,
payload, maxSize, onIdle)`.

- `Listen("tcp://addr:port")` binds a TCP listener (`SO_REUSEADDR`, backlog 1, `addr` empty or `0.0.0.0` = any) (`:54-87`).
  Any other string is a Unix socket path (must fit `sun_path`); an existing socket file is `connect`-probed: if a sim
  answers, `Listen` fails ("give this one its own AnimusForge.Socket"), else the stale file is unlinked (`:103-118`).
  `Listen` on the same path while open is a no-op (`:48`).
- `AcceptClients` blocks (poll 200 ms, `World::IsStopped()` and `onIdle` between polls) until `ranks` learners connected,
  each with a valid HELLO naming a distinct rank of exactly `ranks` (`:146-228`). TCP sockets get `TCP_NODELAY`; Unix ones
  ask for `SO_SNDBUF` 8 MiB and warn once if the kernel caps it (`net.core.wmem_max`).
- `Send` is `writev` of header plus chunks, looping over partial writes; a failure drops all clients (`:239-290`). It has
  no `onIdle`: while the learner does not read, the world thread blocks in `writev`, and the console is not served.
- `ReadExact` polls with the idle callback, so the console stays alive while the learner thinks.
- The HELLO read in `AcceptClients` uses `Receive` without an idle callback: a client that connects and never sends HELLO
  blocks the world thread until `World::IsStopped()`.

Quirks: the header comment (`LockstepServer.h:37`) says "Single-client Unix domain socket server", but it serves one client
per rank and TCP. Listen backlog 1 with several ranks connecting at once: whether a rank can be refused is UNVERIFIED.

### The decision exchange (`Forge::RemoteDecision`, `AnimusForge.cpp:2438-2739`)

First call of a scenario (no client): `_ranks = PoolRanks(LearnerRanks)` (clamped to the envs per group,
`:2433`); `AcceptClients(_ranks, onAccepting)` where `onAccepting` also stops on a pending pause or when the learner has
finished (`LearnerFinished`, `:947`); then for every rank `SendSpec`, then for every rank `OfferDevice`, then for every rank
`AwaitDeviceAnswer` (so no rank waits behind another's answer); then `SetEvaluation(false)`, `SetReplay(0)`, `ResetAll()`,
and `SendEveryGroup()` (a STEP per group). Later calls: `FinishCollect(group)`, `SendStep(group)`.

Then, for the group whose maps tick next (`target`: the other half in half-batch), it reads each rank in order until the ACT
arrives, handling WEIGHTS, PROGRESS, REPLAY, EXPLORE_STARTS without an answer, and MODE (all ranks must send MODE before it
applies: `ApplyModes` requires identical mode/seed base/flags/arena/baseline, then `ResetAll` and every group's fresh STEP).
Receive buffers are capped by the largest expected message (`:2527-2530`). Half-batch: a group's STEP goes out when its
decision closes and its answer is read one world tick later, after the other half's maps ticked.

Each rank gets a contiguous slice of each group: `RankGroup(rank, group)` (`:3027`) with `first = begin + count*rank/ranks`;
`Local` is the first env in that learner's own numbering (its shares of earlier groups come first).

### Device buffers (protocol 15+)

`OfferDevice` only offers when `Animus::Gpu::Api()` is non-null, which is true if `AnimusForge.Gpu.Observe = 1` loaded the
library at startup (`AnimusForge.cpp:203-210`) **or** if any earlier console command (`forge gpu scene`, `forge camera
diff`) loaded it (`cs_forge.cpp:587-598`): the config switch is not consulted in `OfferDevice`. Per rank it allocates obs,
state and mask (and image) buffers on `_config.Gpus[rank]` (or 0), exports IPC handles, sends DEVICE; if the learner
accepts, `UploadRows` copies the rank's rows before each STEP (`CopyToDevice` per array then `Synchronize`) and the STEP
omits those arrays (`:2909-2926`, `:2985-3000`). Any failure falls back to the socket path with a warning. Default is off
(measured slower, conf.dist `AnimusForge.Gpu.Observe`). See [cpp-runtime-process-gpu.md](cpp-runtime-process-gpu.md).

## `ClusterLink` (`A/Bridge/ClusterLink.{h,cpp}`)

Roles come from `AnimusForge.Cluster.Role` (`standalone` default, `host`, `worker`; a worker with no `Cluster.Host` falls
back to standalone, `ForgeConfig.cpp:425-429`). The control channel carries orders only; STEP/ACT go directly between the
host's learner and each worker's sim over TCP (`LockstepServer` on `tcp://0.0.0.0:<DataPort>`).

Wire (text lines, `\n`, non-blocking TCP, `TCP_NODELAY`):

| Line | Direction | Meaning |
|---|---|---|
| `REGISTER <dataPort> <addr or ->` | worker -> host | where the worker's sim listens; `-` = use the connection's source address |
| `FINGERPRINT <k=v ...>` | worker -> host | what it runs; only honoured after a REGISTER (`Pending` non-empty, `ClusterLink.cpp:293`) |
| `CAPS learner=<0|1>` | worker -> host | worker runs a learner of its own (`AnimusForge.Cluster.Learner`) |
| `REFUSED <differences>` | host -> worker | fingerprint mismatch or none within 15 s; host closes; worker retries after 60 s |
| `START <scenario> <resume> <fast> [envs=N] ticks=N [rank=r world=n dist=addr:port sync=weights|async]` | host -> worker | run this scenario; the `rank=` tail only to workers that run a learner |
| `STOP` | host -> worker | end the plan (also sent before a rank restart) |
| `RUNG <n>` | host -> worker | the dungeon wing ladder's rung |
| `PROGRESS k=v ...` | worker -> host | every 5 s: `state scenario envs env_steps_per_s decision_ms world_ms sim_ms learner_ms reset_p95_ms place_p95_ms [wing=<tally>]` |

The header comment (`ClusterLink.h:41`) says START carries `iface=<name>`; the code sends `rank= world= dist= sync=`
(`AnimusForge.cpp:1308`) and no `iface`.

Host behaviour: `Listen(port)` (backlog 16); each accepted connection is a `Peer`; `Poll()` accepts, reads lines, drops
closed peers, and handles REGISTER/FINGERPRINT/CAPS/PROGRESS. A PROGRESS line's `wing=` field is cut out and queued in
`_tallies` for `TakeTallies()`. `RegisteredWorkers()` lists peers with a sim. Worker behaviour: `Join(host, dataPort,
advertise)` only records settings; `Poll()` calls `ConnectToHost()` every 3 s when disconnected (blocking `getaddrinfo` and
`connect`), then sends REGISTER, FINGERPRINT, CAPS. Orders land in `_orders`, taken by `NextOrder()`.

### The fingerprint (`AnimusForge.cpp:75-101`)

`src=<FORGE_SOURCE_HASH> protocol=25 fields=<count>/<bytes> curriculum=<FNV-1a of the serialized CurriculumTuning::Load(
"AnimusForge.Curriculum.") JSON> decision=<DecisionMs>/<TicksPerDecision>`.

- `FORGE_SOURCE_HASH`: first 16 hex digits of the SHA-256 of the concatenated SHA-256 of every `Animus/**/*.cpp`, `*.h`,
  `*.hip` (sorted), computed by CMake at configure time (`src/server/game/CMakeLists.txt:60-75`). The `Animus/` tree
  contains only those three extensions, so every file is hashed. It does **not** cover `ForgeMain.cpp`, `cs_forge.cpp`,
  core files (`Unit.cpp`, `Spell.cpp`, `MapMgr.cpp` ...), the Python learner, YAML configs or the `src/test` golden
  files. Without the generated header the value is `unhashed` (`AnimusForge.cpp:64-68`).
- `fields`: number and total bytes of `*.field` files in `ProbeDir` (not a content hash).
- The fingerprint is set in `OnStartup` only when the role is not standalone (`:214-224`). The mismatch message lists the
  keys that differ (`Differences`, `ClusterLink.cpp:46`).
- The wing ladder `RUNG` and worker settings other than those above (envs per stage, ticks) are not fingerprinted; the
  host's START carries `envs=` (a cap, `min(worker's own, host's)`) and `ticks=` (taken as-is) to compensate
  (`DealClusterLearners` `:1260-1270`, `WorkerPlan` `:1344-1361`).

### Learner ranks and the host

`DealClusterLearners(learnerConfig, scenario, resume, restart)` (`:1257`): for each registered worker, if it runs a learner
(`CAPS learner=1`), `learner.AutoStart` is on and its `HostAddress` is known, it becomes a learner rank; otherwise its sim
joins `ClusterSims` (passed to this machine's learner as `--set cluster_sims=[...]`, `LearnerProcess.cpp:114-120`) and gets
`START ... <cap>` unless this is a restart. This machine's own ranks are `0..LearnerRanks-1`; worker learners get ranks
`LearnerRanks + index`, `DistWorld = local + workers`, `DistAddress = <first worker's host address>:<DistPort>`,
`DistIface = InterfaceOf(address)`, `DistSync = Cluster.Sync` (default `async`, else `weights`). On restart each worker
learner gets `STOP` first, because a worker ignores a START for the scenario it already runs (`:1200-1206`).
`WorkerPlan` (`:1324`) builds a remote plan with `LearnerAutoStart = false` and `SocketPath = tcp://0.0.0.0:<DataPort>`
unless it was dealt a rank, in which case its own learner starts (`LearnerArgs` rewrites the socket to
`tcp://127.0.0.1:<port>`, `LearnerProcess.cpp:88`).

Host-side polling (`PollCluster`, `:1058-1116`): a worker that registers again during Training and was in `_clusterSims`
gets `_clusterStart` (a START with resume 0) again; `TakeTallies` feed `Scenario::AddClusterTally`; the rung
`Scenario::ClusterRung()` is broadcast as `RUNG n` on change and every 30 s while Training and `rung >= 0`; a failed learner
set restarts up to 3 times via `CommandResume({})` (`_autoResumes`) when `_clusterLearners` is nonzero. Worker side
(`:1118-1227`): report every 5 s (rates are deltas since the last report); orders `START` (builds the worker plan, queues
`Request::Cancel` if something runs, then starts it on a later tick via `_clusterOrder`), `RUNG`, `STOP`. A `START` for the
scenario already running with the same `fast` flag and no new rank is ignored (`:1200-1206`).

The `forge pause` command is not cluster-aware; `forge cancel` on the host ends the plan, and `EndPlan` broadcasts `STOP`
(`:924-929`).

## Observed issues

- `ClusterLink.cpp:212-214` and `:209`: `getaddrinfo` and a blocking `connect` run on the world thread every 3 s while the
  host is unreachable; a dropped SYN can block the sim for the OS connect timeout.
- `ClusterLink.cpp:157-162`: a partial `send` on a non-blocking socket is treated as failure without closing the peer;
  the line is then truncated on the wire.
- `ClusterLink.cpp:264-335`: a connected peer that never sends REGISTER stays open forever (only `Pending` has a timeout).
- `ClusterLink.h:41` stale START description (`iface=`); `Protocol.h:26` still lists `HELLO { u32 version }` though it
  carries rank and ranks; `Protocol.h:326-327` has an orphaned sentence ("Then the arrays of envs ...") above `DEVICE`;
  the file-header message list omits PROGRESS, EXPLORE_STARTS, DEVICE.
- `AnimusForge.cpp:2829,2884`: `OfferDevice` ignores `AnimusForge.Gpu.Observe` (see above): behaviour depends on whether a
  GPU console command ran earlier in the process.
- `Runtime.hip:38-47` (device library): `Init(device)` makes a new stream whenever `device` differs from the last one and
  never destroys the old; `UploadRows` calls `Init` each STEP, so ranks on different GPUs would create a stream per STEP.
  Latent while `Gpu.Observe = 0`.
- `AnimusForge.cpp:2527-2530`: receive limit is the max of ACT/MODE/WEIGHTS/REPLAY/EXPLORE sizes for the *current* rank
  slice; a legitimate larger message is a protocol error that drops all learners.
- The C++ side has no unit test for `SendSpec`/`SendStep` byte layout; only the static_asserts (`Protocol.h:260,314`) and
  Python's `test_spec_matches_cpp_layout` / `test_mode_matches_cpp_layout` (`test_protocol.py:74,85`) pin it.

## Reviewer notes

- Any change to `Protocol.h` must change `protocol.py` and bump `PROTOCOL_VERSION` (both files say so).
- `DropClient` closes every rank: one bad rank ends the whole data-parallel connection, by design.
- The wire is lock-step: every `Send` that the learner does not read blocks the world thread. Question: should STEP sends
  be given an idle callback so `forge cancel` works when the learner is wedged?
- The cluster fingerprint hashes sources but not the core; a worker with a different `Unit.cpp` is accepted.
