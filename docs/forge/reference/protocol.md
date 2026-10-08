# The sim-learner protocol (version 26) and the cluster messages

Purpose and scope: a byte-exact description of what the C++ sim and the Python learner say to each other, and of the
text lines the cluster machines exchange. Written from the code at `forge` bd32b9dc8. "Wire" means the lock-step
socket between one sim and one learner; "control" means the cluster's line protocol. Layouts of the observation
columns themselves (what each float means) are in [cpp-blocks.md](cpp-blocks.md) and [cpp-layout-character.md](cpp-layout-character.md);
the files the learner writes are in [file-formats.md](file-formats.md); the columns of episode info are in
[metrics.md](metrics.md). Context: [00-architecture.md](00-architecture.md), [cpp-runtime.md](cpp-runtime.md),
[py-learner.md](py-learner.md), [tests.md](tests.md), [known-issues.md](known-issues.md).

## Map of files

| Path | Lines | Role |
|---|---|---|
| src/server/game/Animus/Bridge/Protocol.h | 401 | Message types, packed structs, `PROTOCOL_VERSION`, `CutAct`, `BadLookRow`, and the long wire comment (the C++ half of the contract). |
| src/server/game/Animus/Bridge/LockstepServer.h | 95 | Single-client (per rank) blocking socket server interface (Unix or `tcp://`). |
| src/server/game/Animus/Bridge/LockstepServer.cpp | 396 | Listen, accept + HELLO check, `Send` (writev), `Receive`/`ReceiveAny`, poll loop that watches `World::IsStopped()`. |
| src/server/game/Animus/Bridge/ClusterLink.h | 158 | Control channel interface (host and worker roles) and the line grammar in its comment. |
| src/server/game/Animus/Bridge/ClusterLink.cpp | 463 | The control channel: accept, REGISTER/FINGERPRINT/CAPS/PROGRESS parsing, REFUSED, reconnect. |
| apps/forge/python/animus/protocol.py | 558 | The Python half: structs, `Spec`, `Step`, `encode_*`/`decode_*`, `step_layout`. |
| apps/forge/python/animus/env.py | 515 | `ForgeEnv` (one sim connection) and `ClusterEnv` (several sims as one pool): handshake, send/receive, dropout handling. |
| apps/forge/python/animus/device.py | 111 | `DeviceBuffers`: opens the sim's GPU buffers via HIP IPC (protocol 15 DEVICE). |
| apps/forge/python/animus/parallel.py | 204 | `Ranks`: torch.distributed collectives between data-parallel learners (port 7702 rendezvous). |

The wire is *only* defined in Protocol.h and protocol.py; the sim-side use is in `src/server/game/Animus/AnimusForge.cpp`
(`SendSpec` 2780, `OfferDevice`, `AwaitDeviceAnswer`, `SendStep` 2930s, the receive loop around 2440-2730, `ApplyMode`).

## 1. Transport, framing, endianness

* Every message is `MsgHeader { u32 Type; u32 Length; }` (8 bytes, little-endian, no padding) then `Length` payload bytes
  (Protocol.h:212-217, protocol.py:56 `HEADER = "<II"`). The structs are under `#pragma pack(push, 1)` (Protocol.h:212-361),
  and `static_assert(std::endian::native == std::endian::little)` (Protocol.h:186) refuses a big-endian build. There is no
  alignment padding anywhere on the wire; numpy dtypes are explicitly `<f4`, `<u2`, `<u4`, `<i4`.
* Transport: a Unix stream socket path, or `tcp://address:port` (LockstepServer.cpp:54). Over TCP the accepted socket gets
  `TCP_NODELAY`; over Unix, `SO_SNDBUF` 8 MiB is requested (LockstepServer.cpp:83, 169) and a warning is logged once if the
  kernel grants less (the kernel cap is `net.core.wmem_max`). The listener backlog is 1 (`listen(_listener, 1)`).
* Unix listen: a stale socket file is unlinked, but if another sim answers a `connect()` on the path the listen is
  refused ("Another sim is listening ... give this one its own AnimusForge.Socket", LockstepServer.cpp:103-117).
* The server is blocking and driven from the world thread; every wait polls in 200 ms slices and checks
  `World::IsStopped()` (LockstepServer.cpp:38, 348-369). `ReceiveAny` takes an `onIdle` callback that the sim uses to keep
  answering console commands while the learner thinks; returning false abandons the receive and the connection must be
  dropped.
* `Receive` (fixed size) and `ReceiveAny` (bounded size) treat a `CLOSE` header as "drop the client, return true" and do
  not read any payload. A payload longer than the bound or the wrong size drops the client with an error log.
* Python side: `ForgeEnv._connect` retries every second until `connect_timeout` (default 600 s); `tcp://` gets `TCP_NODELAY`
  (env.py:60-80). `_read_into` loops `recv_into` and raises `ConnectionError("sim closed the connection")` on EOF.

## 2. Message types (`MsgType`, Protocol.h:188-210; `protocol.MsgType`)

| Id | Name | Direction | Payload |
|---|---|---|---|
| 1 | Hello | learner -> sim | `HelloMsg` |
| 2 | Spec | sim -> learner | SpecMsg + layouts + names |
| 3 | Step | sim -> learner | `StepHeader` + arrays |
| 4 | Act | learner -> sim | `ActHeader` + actions [+ goals] [+ look] |
| 5 | Close | learner -> sim | empty |
| 6 | Mode | learner -> sim | `ModeMsg` |
| 7 | Weights | learner -> sim | `WeightsHeader` + floats |
| 8 | Replay | learner -> sim | `ReplayHeader` + u32 seeds |
| 9 | Device | sim -> learner | `DeviceMsg` [+ image handle] |
| 10 | DeviceAck | learner -> sim | `DeviceAckMsg` |
| 11 | Progress | learner -> sim | `ProgressMsg` |
| 12 | (unused) | - | retired 2026-10-08, decision 0019 |

Sim-to-learner messages are only SPEC, DEVICE and STEP. Everything else is learner-initiated; WEIGHTS, REPLAY and PROGRESS
get no answer. MODE is answered with fresh STEPs. A type the sim does not expect at that point drops the
learner with "sent message type N with M bytes where ACT, MODE, WEIGHTS or REPLAY was expected"
(AnimusForge.cpp; the text omits PROGRESS, which is accepted).

## 3. Handshake and manifest

1. Learner connects and sends HELLO: `HelloMsg { u32 Version; u32 Rank; u32 Ranks; }` = `"<III"` (12 bytes). Python sends
   `PROTOCOL_VERSION, rank, ranks` (env.py:34). Rank/Ranks are the data-parallel learner's place (0 of 1 alone).
2. The sim (`AcceptClients(ranks)`, LockstepServer.cpp:146) waits until `ranks` clients have each sent a valid HELLO with a
   distinct rank of that many. A first message that is not HELLO, a wrong `Version`, or a rank/ranks mismatch closes that
   one connection and keeps waiting (logged). `Ranks` must equal the sim's expectation (`PoolRanks(LearnerRanks)`).
3. For every rank in order the sim sends SPEC; then, for every rank, the DEVICE offer (if any); then waits for every
   rank's DEVICE_ACK (AnimusForge.cpp:2460-2470; the ordering exists so no data-parallel rank waits behind another).
4. A new learner always starts in training mode: the sim clears evaluation and replay state, ends any eval video, resets
   every env and sends every group's STEP (AnimusForge.cpp:2473-2480).

### SPEC payload

`SpecMsg` (96 bytes, `"<12I32s4I"`, static_assert Protocol.h:260, pinned by `test_spec_matches_cpp_layout`):

| Field | Type | Meaning |
|---|---|---|
| Version | u32 | `PROTOCOL_VERSION` (26). The learner refuses a mismatch (env.py:41). |
| NumEnvs | u32 | This rank's envs (`RankEnvs(rank)`), not the pool's. |
| AgentsPerEnv | u32 | Agent rows per env (seats plus any cast owner row). |
| ObsDim | u32 | The largest layout's observation width; all rows padded to it. |
| StateDim | u32 | Critic state width, per env. |
| NumActions | u32 | The largest layout's action count; masks padded to it. |
| EpisodeInfoDim | u32 | Episode-info floats per agent (K). |
| GoalCount | u32 | `GOAL_JOINT_COUNT` = kinds x targets (stage.json goals); 0 = no goals. |
| TickMs | u32 | `max(1, DecisionMs / ticks)`. |
| DecisionTicks | u32 | World ticks per decision. Decision length is `TickMs * DecisionTicks` (`Spec.decision_ms`). |
| EpisodeSeconds | u32 | Longest episode: `max(conf EpisodeSeconds, scenario LongestEpisodeSeconds)`. |
| EnvGroups | u32 | 1, or 2 for half-batch (STEPs and ACTs cover contiguous halves, first half first). |
| Scenario | char[32] | NUL-padded name; at most 31 chars (`strncpy(..., 31)`). |
| KinematicsDim | u32 | `Kinematics::SAMPLE_DIM` = 10 (protocol 20). |
| ImageBytes | u32 | Bytes per agent of the camera image: height x width x 4 (x 5 at 23-25); 0 without a vision block (21, 26). |
| LookHeads | u32 | `FreeLook::HEADS` (3) with a vision block, else 0 (22). |
| MapBytes | u32 | 13,824 (48 x 48 x 6) with a map block, else 0 (24). |

Then `u32 layoutCount`, then `layoutCount` x `LayoutMsg { u32 ObsDim; u32 NumActions; char Name[48]; }` (`"<II48s"`), then the
episode info column names as comma-separated ASCII filling the rest of the payload, no terminator and no count. Layout
names are truncated to 47 characters. A name containing a comma would corrupt the list (UNVERIFIED that none does; the
names are C++ literals and generated `reward_<term>` families, see metrics.md).

`protocol.decode_spec` (protocol.py:323) rebuilds `Spec`; `Spec.layouts` and `Spec.episode_info_names` are tuples.
Note `Spec.image_bytes` comment (protocol.py:133) still says "height x width x 4"; it is 5 since 23.

### DEVICE (protocol 15; only if the sim loaded the GPU library and `GpuObserve`)

`DeviceMsg { u32 Device; u32 Envs; u8 ObsHandle[64]; u8 StateHandle[64]; u8 MaskHandle[64]; }` = `"<II64s64s64s"` (204
bytes), followed, in a stage with a camera (ImageBytes > 0), by one more 64-byte image handle (`DEVICE_IMAGE "<64s"`). Handles
are `hipIpcMemHandle_t` bytes. The buffers hold this rank's envs, env-major: obs `[E,A,O]` f32, state `[E,S]` f32, mask
`[E,A,N]` u8 (read as bool), images `[E,A,I]` u8. The map crop is NOT in the device buffers (always on the socket;
the comment says "G3 has to carry it there"). The learner replies DEVICE_ACK `u32 Accepted` (1 if it opened them). The
learner declines (0) unless its rollouts run on the same HIP GPU index and torch is a HIP build and `envs` matches
(device.py `open_buffers`). On acceptance the sim copies the rank's rows into the buffers before each STEP
(`UploadRows`) and leaves obs/state/mask (and image) out of the STEP. On decline it frees the buffers.
If the sim cannot allocate them it logs a warning and simply sends no DEVICE message; the Python constructor handles
both (env.py:52-60: if the next header is not DEVICE it is kept for the first STEP).

## 4. STEP (sim -> learner)

Header `StepHeader { u64 Decision; u32 EnvBegin; u32 EnvCount; }` = `"<QII"` (16 bytes). `Decision` is a counter shared
by all ranks and groups of one decision (incremented once per `SendStep`). `EnvBegin` is in the *learner's own* numbering
(`rows.Local`), and `EnvCount` the rows of that group for that rank. With `EnvGroups = 2` one decision is two STEPs
(first half, then second half); the rank's envs are the concatenation of its halves.

Array order after the header, with E = EnvCount, A = AgentsPerEnv, O/S/N/K/M from SPEC, D = number of envs with done = 1
among those E (the code is `Spec.step_layout`, protocol.py:157, and the chunk list in `SendStep`):

| # | Name | dtype | shape | Notes |
|---|---|---|---|---|
| 1 | obs | f32 | [E,A,O] | After any auto-reset. Absent with device buffers. |
| 2 | state | f32 | [E,S] | Critic state (may be privileged). Absent with device buffers. |
| 3 | mask | u8 | [E,A,N] | 1 = allowed. Absent with device buffers. |
| 4 | layout | u16 | [E,A] | Index into SPEC layouts; constant within an episode. |
| 5 | present | u8 | [E,A] | 0 empty seat (only no-op, reward 0, not a sample); 1 has a character; 2 the "human" stand-in's row (protocol 25). |
| 6 | reward | f32 | [E,A] | For the transition just ended; the first STEP after SPEC/MODE has zeros. |
| 7 | done | u8 | [E] | 1 = episode ended on this transition. |
| 8 | terminated | u8 | [E] | 1 = terminal (no bootstrap); done without terminated is a time-limit truncation. |
| 9 | final_obs | f32 | [D,A,O] | Last obs of each ended env, env order, only done envs. |
| 10 | final_state | f32 | [D,S] | Likewise. |
| 11 | episode_info | f32 | [D,A,K] | Per-agent totals of each ended episode (protocol 18: ended envs only). |
| 12 | episode_seed | u32 | [E] | Evaluation seed index of the ended episode (valid if done); `NO_EPISODE_SEED` = 0xFFFFFFFF for training. Sent for all E envs. |
| 13 | kinematics | f32 | [E,A,10] | `[t, x, y, z, yaw, pitch, mode, mounted, speed, in_combat]` after the transition; the new episode's first sample where done; zeros for an agent without a body. Kinematics.h. |
| 14 | image | u8 | [E,A,I] | Only if I > 0 and not in device buffers. `[row][col][byte]`, row 0 top, 4 bytes a pixel (distance, height, normal, class + objective; the static world only). Rows without a frame are `Vision::FillNoFrame` = pixel `(255,128,0,0)` (`NO_FRAME_PIXEL`, protocol.py). |
| 15 | final_image | u8 | [D,A,I] | Whenever the stage has a vision block (sent even with device buffers). |
| 16 | map | u8 | [E,A,M] | Only if M > 0; always on the socket. 48 x 48 cells of 2 yd, heading-up, `[row][col][channel]`, 6 channels (code, height, visited, age, class, frontier); zeros for no map. |
| 17 | final_map | u8 | [D,A,M] | Likewise for ended envs. |

How each side builds it: the sim keeps flat vectors in `EnvPool` (`Obs, State, Mask, Rewards, Done, Terminated, FinalObs,
FinalState, Layout, Present, EpisodeInfo, EpisodeSeed, KinematicSamples, Image, FinalImage, MapCrop, FinalMapCrop`,
EnvPool.h:216-239) and sends slices as writev chunks; for the `ended` arrays it gathers the done rows into scratch vectors
(`_endedObs`, ...). The learner decodes with `np.frombuffer` into copies; the ended arrays are placed back into
full-size zero arrays at the done rows (`decode_step`), so after decoding every field has shape [E,...] and only rows where
`done` is set are meaningful. Raises `ValueError` if the payload length differs from what E and the done count imply
(converted to `ConnectionError`).

Decoder specifics worth knowing: `mask`, `done`, `terminated` are read as `bool`; `present` is kept a byte, then
`present = raw > 0`, `stand_in = raw == 2` (protocol.py:424-426). In a stage with a map block, `Step.image` becomes the
concatenation image-bytes-then-map-bytes per row (`Spec.camera_bytes`) and `Step.map` also stays; `encode_step` splits it
back. A seat whose `episode_info[present]` column is 0 includes the stand-in (see metrics.md `present`), so the stand-in
row is present 2 on the wire but not an episode for scoring.

The first STEP after SPEC/MODE carries freshly reset envs: reward and done are zero and must not be recorded
(Protocol.h:128). A truncated episode bootstraps from final_state; a terminated one does not.

## 5. ACT (learner -> sim)

`ActHeader { u32 EnvBegin; u32 EnvCount; }` (`"<II"`) then, agent-major (env-major, agent within env), the group's rows:

1. `i32 actions[E*A]`.
2. Optionally `i32 goals[E*A*2]`: primary then secondary per agent; 0..GoalCount-1 or -1 for none (`GOAL_SLOTS_ON_WIRE = 2`,
   EnvPool.h:38). Present iff the policy has a goal head. Goals never mask an action.
3. If `LookHeads` L > 0 (a camera stage): `i32 look[E*A*L]`: yaw rate 0..6, pitch rate 0..4, zoom 0..4 (head sizes
   `FreeLook::HEAD_SIZES`). Required; every value must be in range or the learner is dropped. Rows without a camera send
   0s (index 0, "fastest turn right") but are range-checked. The Python "no opinion" choice is `LOOK_HOLD = (3, 2, 0)`.

The sim accepts an ACT only if `EnvBegin`/`EnvCount` equal the group it is waiting on (`RankGroup(rank, target)`) and the
body length is exactly one of the two sizes `CutAct` allows (with or without goals, each plus the look section); see
`CutAct` Protocol.h:373 and `protocol.decode_act` (Python mirror, used by tests). The ACT must also not arrive while MODE
messages are pending from another rank. Any mismatch falls to the generic "unexpected message" drop path.

Receive bound: the sim reads at most `max(sizeof(ActHeader) + (1 + 2 + L) * actionBytes, sizeof(ModeMsg), weightBytes,
replayBytes)` where `actionBytes = pool actions * 4` (AnimusForge.cpp:2494). See Observed issues for the
`weightBytes` term.

Half-batch: the learner answers STEPs in the order they came; the sim waits for the answer of the group whose maps tick next
(`_nextTurn`). A learner may answer both halves together (`ForgeEnv.step`) or pipelined (`send_act`/`receive_step`).

## 6. Other learner-to-sim messages

* **MODE** `ModeMsg` = `"<IIIIII32s"` (56 bytes): `Mode` (0 training, 1 evaluation; anything else is an error), `SeedBase`,
  `Episodes` (seeded evaluation episodes), `Flags`, `FirstSeed`, `Arena` (held-out arena = stage.json arena index + 1; 0 = the
  stage's own), `Baseline[32]` (policy name; only "random" is known, `KnowsPolicy`, AnimusForge.cpp:2755; empty = learner). The
  sim resets every env and answers with every group's fresh STEP (zero reward/done). With data-parallel ranks the sim waits
  for all ranks' MODE and requires them equal in Mode, SeedBase, Flags, Arena, Baseline (`ApplyModes`); each rank's own
  `FirstSeed`/`Episodes` define its run of seeds. Evaluation hands out seed indexes `FirstSeed .. FirstSeed+Episodes-1` in
  order as envs reset; the characters and opponents come from reseeding with `(SeedBase, index)`; envs that reset after
  the seeds are used up run unseeded episodes (`NO_EPISODE_SEED`). Seeded episode i plays (class, spec) pair `i % pairs`,
  so an evaluation is always evenly spread whatever WEIGHTS says. `MODE_FLAG_STAND_IN` = 2 (bit 1 is unused, "it was
  SCRIPTED_OPPONENTS, goes at the next protocol change", Protocol.h:269; test asserts `encode_mode` never sets it): in an
  evaluation, every party has the stand-in in one seat; in a training MODE the learner says it can field one. The sim
  applies the flag with `SetStandIn` on every MODE (training MODE without the flag clears it). With a Baseline the sim ignores
  the ACT actions and runs that policy. Eval videos are begun by `ApplyModes` -> `BeginEvalVideos`.
* **WEIGHTS** `WeightsHeader { u32 Count }` + `f32[Count]`: one weight per (layout, spec), layout-major in SPEC order, spec-minor,
  `Count = layouts * MAX_SPECS` (4, `Curriculum::MAX_SPECS`, protocol.py:34). A wrong count is logged and ignored (the
  scenario keeps its weights, `StageScenario::SetLayoutWeights`); an empty vector clears them; non-finite, negative, or all-zero
  is refused. Takes effect as envs reset. No answer. The payload length must be exactly `4 + 4*Count` else the learner is dropped.
* **REPLAY** `ReplayHeader { u32 SeedBase; f32 Fraction; u32 Count }` (`"<IfI"`) + `u32 seeds[Count]`, `Count <= 65536`
  (`MAX_REPLAY_SEEDS`). That share of training resets rebuilds one of these evaluation seeds (fresh combat rolls); count 0 or
  fraction 0 stops it. A replayed episode reports as training (`NO_EPISODE_SEED`). `encode_replay` sorts and dedups.
* **PROGRESS** `ProgressMsg { f32 Progress; f32 ShapingScale; f32 CostScale }` (`"<fff"`): sent after every update. All three
  clamped to [0,1] by the Python encoder; the sim clamps again and logs once if the scales were outside [0,1] (NaN is taken
  as 1). `Progress` selects arena weights that change over a stage (`ArenaDefinition::WeightFinal`); `ShapingScale` multiplies
  every Shaping reward term (`RewardLedger::SetShaping`); `CostScale` multiplies every noise price (`SetCosts`).
* **CLOSE**: no payload; the server drops the client and waits for a new one.

Applying is *without answer*: after WEIGHTS/PROGRESS/REPLAY the sim loops and waits for the ACT.

## 7. Where the two implementations must agree

Each of these is a duplicated definition; change both and bump `PROTOCOL_VERSION`:

| Item | C++ | Python | Pinned by |
|---|---|---|---|
| SPEC size and field order | Protocol.h:233-260 | protocol.py:59, `encode_spec`/`decode_spec` | `test_spec_matches_cpp_layout` (test_protocol.py:74), `test_spec_is_96_bytes_with_the_look_heads_before_the_map_bytes` (test_free_look.py:385), `test_spec_carries_the_map_bytes_last` (test_mental_map.py:120), `test_spec_carries_the_image_bytes_as_the_sim_packs_them` (test_vision_bytes.py:117); C++ `static_assert(sizeof(SpecMsg) == 96)`. |
| MODE size/flags | `ModeMsg`, `MODE_FLAG_STAND_IN` | `MODE`, `encode_mode` | `test_mode_matches_cpp_layout`, `test_the_stand_in_flag_rides_on_mode` (test_partners.py:99). |
| STEP array order and dtypes | `SendStep` chunk list | `Spec.step_layout` | `test_step_round_trip_and_size`, `test_episode_info_travels_for_the_ended_envs_only`, `test_kinematics_travel_with_every_step`, `test_a_step_with_a_camera_reads_its_images_as_the_sim_writes_them`, `test_a_step_with_a_map_round_trips_its_own_section_after_the_images`, `test_a_stage_without_a_camera_is_protocol_20_on_the_wire`, `test_a_stage_without_a_map_is_protocol_23_on_the_wire`, `test_with_device_buffers_the_images_leave_the_step_but_the_final_images_stay`. These test Python against Python (a fake sim); there is no byte-level test of `SendStep` itself (UNVERIFIED: check src/test for one; none found). |
| ACT cuts incl. look | `CutAct`, `BadLookRow` | `encode_act`/`decode_act` | C++ `VisionProtocolTest` (`SpecCarriesLookHeads`, `ActCarriesTheLook`, `ActWithoutVisionIsProtocol21`, `ActRefusesLookOutOfRange`, `LookingIsFree`); Python `test_act_round_trips_with_the_look_agent_major_after_the_goals`, `test_a_stage_without_look_heads_sends_protocol_21s_act`, `test_a_sim_with_look_heads_over_the_socket`, `test_a_sim_speaking_protocol_21_is_refused`. |
| Version refusal | `AcceptClients` | `ForgeEnv.__init__` | `test_a_sim_speaking_another_protocol_is_refused` (test_protocol.py:269). |
| TCP sim | `tcp://` Listen | `_connect` | `test_a_sim_on_another_machine_is_reached_over_tcp`. |
| Pixel/class bytes, map channels | Vision::EncodePixel, CropChannel | vision encoder decode | `test_decoding_every_byte_is_the_sims_decode_pixel_exactly`, `test_the_sims_encoding_round_trips_through_the_learners_decoding`, `test_the_crop_decodes_as_the_sim_encodes_it`. |
| Kinematics columns | Kinematics.h | human/motion.py | KinematicsTest (C++), `test_kinematics_travel_with_every_step`. |
| Image/map size vs stage.json | SPEC ImageBytes/MapBytes | `check_image_bytes`/`check_look_heads` (networks.py) | `test_the_sim_and_stage_json_must_agree_about_the_camera`. |

The Python fake sims used by the tests are in `apps/forge/python/tests/sim_threads.py`. The learner checks SPEC against
stage.json (image bytes, map bytes, look heads) at start and exits (`SystemExit("vision: ...")`, train.py `trainer_inputs`).

## 8. Version history (what each bump changed)

From the comment block at Protocol.h:142-179, protocol.py:14-31 and git. Versions 1-9 predate the fold of mod-animus-forge
into the core (`359b303c4`); their content is UNVERIFIED (no comment survives; check the mod-animus-forge history).

| Ver | Change | Evidence |
|---|---|---|
| 10 | WEIGHTS per (class, spec) instead of per (class, role): same bytes, different length and meaning. | Protocol.h:142 |
| 11 | STEP/ACT carry `EnvBegin/EnvCount` (headers); SPEC gains `EnvGroups` (half-batch). | Protocol.h:144; commit d5364900d (2026-09-25) |
| 12 | MODE names `FirstSeed` so a cluster's sims share one evaluation's seeds. | Protocol.h:146 |
| 13 | HELLO carries Rank and Ranks (data-parallel learners). | Protocol.h:147; 09a5086e7 |
| 14 | final_obs/final_state only for done envs. | 7f8a1a8e8 |
| 15 | DEVICE / DEVICE_ACK (GPU IPC buffers). | 57f0d742e |
| 16 | A goal is `kind * GOAL_TARGETS + target`; SPEC GoalCount is the joint count. | 4efaec381 |
| 17 | ACT carries two goals per agent; twelfth goal kind (Resurrect); PROGRESS message introduced (`Progress` float). | 3d696b97e, 2a30f118d |
| 18 | PROGRESS adds ShapingScale; episode_info only for ended envs; MODE gains `Arena` (held-out); message type 12 added (retired 2026-10-08, see below). | c0439d055, d75ba4882, c51815bb0, e5c9fa74e |
| 19 | PROGRESS adds CostScale. | 9ec9735b1 |
| 20 | SPEC KinematicsDim; every STEP ends with kinematics. | 25ce5a6fa |
| 21 | SPEC ImageBytes; vision stages append image/final_image to STEP and the image handle to DEVICE. | d86feef62 |
| 22 | SPEC LookHeads; vision stages' ACT ends with look. | 01e1207fb |
| 23 | Pixel = 5 bytes (class + entity slot); layout otherwise protocol 22's. | c20dc0c1a |
| 24 | SPEC MapBytes; map/final_map after the images. | 8452ff458 |
| 25 | `present` 2 = stand-in row played by a frozen partner; `MODE_FLAG_STAND_IN` in training MODE. | Protocol.h:178; ac9873986, 641cf015c, aa303bc33 (scripted stand-in removed) |
| 26 | Entity sensing (vision block 6): pixel = 4 bytes again (no entity slot), the static world only, so ImageBytes is height x width x 4; entities block 2 (columns 16-18 los, ang_width, ang_height), sight block 3. Message layout otherwise protocol 25's, less message type 12. The layout cleanup of 2026-10-08 folds in with no change of structure: SPEC `GoalCount` 348 -> 207 (goal block revision 1), `StateDim` 1958 -> 1927, and stage.json gains `state.dim`. | entity-sensing (this change) |

Message type 12 (added at 18) is unused since 2026-10-08 (decision 0019, vision-only movement). It was folded into the 26 bump
(`PROTOCOL_VERSION` is 26 in Protocol.h and protocol.py).

Commit-date mapping for 15-25 was taken from `git log` subjects and is approximate: the commit that sets the constant
(`git log -S"PROTOCOL_VERSION = N;"`) was checked only for 24 and 25 (both 641cf015c on 2026-10-07; 24 first at 8452ff458,
2026-10-06) and 11 (413e174dc, d5364900d, 2026-09-25). A pre-25 `spec.json`/checkpoint is still resumable because
`resume_mismatch` compares only scenario, agents_per_env, obs_dim, state_dim, num_actions and layouts (runs.py), not `version`.

## 9. The cluster (control, data, learner exchange)

Roles come from `AnimusForge.Cluster.Role` ("host"/"worker"/standalone). Ports (ForgeConfig.cpp:415-418, defaults): control
**7700** (`ClusterControlPort`), data **7701** (`ClusterDataPort`, the worker sim's `tcp://` listen), learner rendezvous
**7702** (`ClusterDistPort`, torch.distributed/the async Hub meeting point on the host). The control channel carries
orders only; STEP/ACT go straight between the learner and each sim over the wire above.

### Control lines (ClusterLink)

Plain `\n`-terminated ASCII lines over non-blocking TCP (`TCP_NODELAY`), polled from the world thread every tick. Sends are a
single `send()`; a short send is treated as a dead peer. Reads use a 512-byte buffer and accumulate partial lines.

| Direction | Line | Meaning |
|---|---|---|
| worker -> host | `REGISTER <dataPort> <advertise or ->` | Opens registration. The host forms `tcp://<advertise, else the TCP peer address>:<dataPort>` as the worker's sim and holds it as *pending*. Port must be 1..65535. |
| worker -> host | `FINGERPRINT <key=value ...>` | Must follow REGISTER. Matched by exact string equality to the host's own; on a match the worker is registered (`TakeRegistrations`), else `REFUSED <differences>` is sent and the connection closed. |
| host -> worker | `REFUSED <keys that differ>` | The differences are `key(host X, worker Y)` space separated (`Differences`). Also `REFUSED no fingerprint` if none arrives within 15 s of REGISTER. The worker waits 60 s before retrying. |
| worker -> host | `CAPS learner=<0\|1>` | Whether the worker runs a learner of its own (`AnimusForge.Cluster.Learner`). Substring-matched (`learner=1`). |
| host -> worker | `START <scenario> <resume 0\|1> <fast 0\|1> [envs=<n>] [ticks=<n>] [rank=<r> world=<n> dist=<addr:port> sync=<weights\|async>]` | Order to run a scenario. `envs` = host's per-learner env count for the stage (worker runs at most that); `ticks` = the stage's world ticks per decision (worker obeys). The `rank=` tail goes only to a worker with its own learner: its rank among all learners, world size, rendezvous `host:7702`, sync mode (default "weights" if absent). A worker already running that scenario ignores it unless it is a rank (world > 1). `resume` sent to workers is 0 in the stored restart line `_clusterStart`. |
| host -> worker | `STOP` | End the plan (worker cancels). Also sent before a restart START to a rank worker. |
| worker -> host | `PROGRESS <key=value ...>` | Every few seconds: `state scenario envs env_steps_per_s decision_ms world_ms sim_ms learner_ms reset_p95_ms place_p95_ms` and optionally ` wing=<rung>/<probes>/<others>` (progress values of finished dungeon runs, comma lists, `-` when empty). The host strips `wing=` into tallies (`TakeTallies` -> `AddClusterTally`) and keeps the rest for `forge status`. |
| host -> worker | `RUNG <n>` | The cluster's dungeon ladder rung, sent on change and every 30 s while training (AnimusForge.cpp:1080-1090). The worker calls `FollowClusterRung` and keeps no ladder of its own. |

Fingerprint (AnimusForge.cpp:75-97): `src=<FORGE_SOURCE_HASH or "unhashed"> protocol=25
curriculum=<FNV-1a 64 of the JSON of CurriculumTuning::Load("AnimusForge.Curriculum.")> decision=<DecisionMs>/<TicksPerDecision>`.
`FORGE_SOURCE_HASH` comes from `ForgeSourceHash.h`, generated by CMake at configure time (not in this tree: UNVERIFIED how it is
computed; see cpp-runtime.md / tools-and-ops.md).

Host side per tick (`Forge::Pump`, AnimusForge.cpp ~1060): `TakeRegistrations` (a returning worker of the running stage is
sent `_clusterStart`; others "join the next scenario"), tallies into the ladder, `RUNG` broadcast, and automatic restart
of learners if a rank fails (at most 3 times, then waits for `forge resume`). `DealClusterLearners` partitions registered
workers: those with `Learner = 1`, a `LearnerAutoStart`, and a known host-side address become learner ranks (local ranks first,
then `local + index`), others become sims of the host's learner (`ClusterSims`). Worker sims are reached by the learner as
`tcp://addr:7701` through `ClusterEnv` (env.py:225), which treats sim 0 (the host's) as mandatory and others as droppable.

`ClusterEnv` behaviours (env.py): per-worker socket timeout `cluster_timeout` (default 60 s); a failing worker is dropped, its
envs report `present = 0`, no character, `sat_out` set; `rejoin()` is tried every 10 s between rollouts, only if the returning sim has
an identical `Spec` (otherwise it "stays out"); weights and replay tables are re-sent to a rejoined sim; `set_mode` shares an
evaluation's `episodes` among live sims in proportion to env counts in consecutive seed runs (`_shares`).

### Learner exchange (7702)

`Ranks` (parallel.py) uses `torch.distributed` (`MASTER_ADDR/PORT` from the `dist=` address; `GLOO_SOCKET_IFNAME` from an `iface`
name; nccl only if every rank has its own CUDA device, else gloo; timeout `dist_timeout`). `sync=async` uses `async_sync.Hub/Link`
(py-learner.md / py-mappo.md) instead of collectives. The `iface` is derived by the host (`ClusterLink::InterfaceOf`) but the START
line in the code carries no `iface=` token although the comment in ClusterLink.h:41 lists one: UNVERIFIED how the worker learns
its interface (check `ForgeConfig` / `LearnerProcess` for `DistIface`).

## 10. Observed issues

* The comment at ClusterLink.h:41 lists `START ... iface=<name>` and omits `sync=`; the code sends `sync=` and no `iface=`
  (AnimusForge.cpp:1298). Stale comment.
* AnimusForge.cpp:2494-2496 bounds the receive by `sizeof(WeightsHeader) + Layouts.size() * 4`, but a valid WEIGHTS carries
  `Layouts.size() * 4 * 4` bytes. It works only because the ACT term (`(3+L)*agents*envs*4`) is larger in practice. A tiny pool
  (few envs, many layouts) would reject a valid WEIGHTS and drop the learner. Latent bug.
* The error text at AnimusForge.cpp:2717 lists the expected types but omits PROGRESS.
* Protocol.h:119-121 comment text about the stand-in ("every party has the 'human' stand-in") is split over a stray line break; harmless.
* protocol.py:133 comment says image bytes are height x width x 4 (5 since 23). protocol.py header lists versions 20-25 only; 10-19 live in Protocol.h.
* `protocol.decode_mode` ignores flags/first_seed/arena; separate `decode_mode_*` helpers exist for tests only.
* Duplicated knowledge: `MAX_SPECS = 4` in protocol.py:34 and `Curriculum::MAX_SPECS` in C++ are not checked against each other on the wire
  other than by the WEIGHTS length refusal.
* `ClusterLink::Send` treats any partial send as a dead peer; a very long REFUSED text could therefore be lost (control lines are tiny, low risk).
* The wire has no checksum, no per-message sequence number and no schema hash: only the version integer guards against drift.
* The `Decision` counter is shared by ranks but the learner never checks continuity (UNVERIFIED: grep for `decision` use in train.py).

## 11. Reviewer notes

* Adding a column to SPEC or STEP: change `SpecMsg`, `SendSpec`, `SendStep` chunk order, `Spec.step_layout`, `encode_*`, `decode_*`, the
  device-field list, the fake sims in tests, and bump the version in both files; the cluster fingerprint then refuses mixed builds.
* `DEVICE_FIELDS` includes "image" even for a stage without a camera; `decode_step` zips them with `device.rows(...)` which returns 3 or 4
  tensors, so it is safe only because `rows` returns 3 when `image is None` (zip stops at the shorter).
* Half-batch and ranks multiply the cases: `EnvBegin` is *local* per rank, `RankGroup` splits each group evenly across ranks.
* Question: should `present = 2` stay a magic number shared by `StandIn::Presence` (C++) and `PRESENT_STAND_IN` (Python)?
* Question: the bit-1 gap in `Flags` is waiting for a protocol change; fold it into the next bump.
