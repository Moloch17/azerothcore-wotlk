# Efficiency audit of the forge

Audit of `forge` at `93cbc38ea` (2026-10-07). Documentation and analysis only: no code, test or config was changed. The
owner asked for every inefficiency, ranked by expected gain, with evidence. This file is the answer; sections are:
(0) what the evidence is and how to read the labels, (1) the ranked table, (2) one section per finding, (3) quick wins,
(4) not worth doing, (5) the ordered measurement plan, (6) where the time goes, (7) appendix: what was run.

## 0. Method, labels and the headline

**The headline.** The cluster host's training is **update-bound, not sim-bound**. In the live `move2_seek` run
(`var/animus-forge/shared/runs/move2_seek/metrics-before-20261007-110448.csv`, 2021 updates, six hours) the wall time
of one update cycle is 11.1 to 11.5 s in every window of the run, and it equals `update_compute_seconds` (11.1 to
11.4 s), whatever the rollout does. In updates 1000 to 1400 the rollout slowed from 5 s to 8.2 s and the cycle stayed at
11.3 s. The sim and the actor loop therefore sit idle for about half of every cycle (the learner's "wait" is 5.4 to
6.3 s). Every sim-side saving is worth nothing on this host until the update is below the rollout (about 5 s), and the
reported `env_steps_per_sec` (4.3k to 5.0k) is the rollout phase only: the real rate of the host's own learner is 2.15k
to 2.2k env steps/s. The ranking below follows from that.

**Labels.**

- **VERIFIED**: measured here (CPU only, in `claude-syntax`, or read from the live run's own files) or provable from the
  code by counting.
- **HYPOTHESIS**: follows from the code and a mechanism, but needs a GPU, a build or a live run to confirm. The
  measurement plan (section 5) says how.
- Gains are *estimates* unless the finding says VERIFIED, and are given as a share of the relevant cost. Where a number
  is not known it says UNKNOWN. Gains of the update findings (U2 to U5) draw on the same pool (the camera and map
  encoders) and are **not additive**.

**The numbers the owner gave, checked.**

| Number | Verdict | Evidence |
|---|---|---|
| A decision costs about 24 ms, reset 6.4 ms, learner round trip 10 ms for 3 ms of compute | **OLD**, from 2026-09-17, `stage8_duel`, 128 envs, before the parallel-core work. Superseded: the same plan reached 4.9 ms a decision at 192 envs and reset 0.77 ms | `.agents/plans/forge-parallel-core/forge-parallel-core.PLAN.md:18-27`, `:562`, `:1313` |
| Camera and map encoders cost about 3.6 s of a ~5.8 s update at two epochs | **STALE as a total**: the live update is 11.2 s at two epochs. The 3.6 s is the encoders alone (2,048 rows: camera 74 ms, camera and map 151 ms); what the other 7.6 s is, is UNKNOWN until the update is profiled on the GPU | `apps/forge/python/configs/move2_seek.yaml:28-33`; metrics `update_compute_seconds` |
| M2 runs about 2-5k env steps/s on the cluster | **Rollout-phase rate only.** End to end the dev host's own learner does 2.17k/s; the cluster's counter advances 65,536 steps per 11.3 s (5.8k/s) because followers' steps are folded in | `train.py:2024` (rate = steps / `rollout_seconds`); section 6 |
| sarah 2.3k, thomas 1.1k, spencer 0.7k, .117 0.4k | Same caveat: `forgectl status` prints the rollout rate. The one sample in the docs (sarah: rollout 3.20 s, compute 4.90 s) is update-bound too | `docs/forge/forgectl.md:78` |
| The CPU camera caster does 170-600 M rays/s | **NOT CONFIRMED.** 170 M rays/s is 6 ns a ray, below what a bounding-interval-hierarchy ray cast costs (tens to hundreds of ns). It is probably the GPU line of `forge camera diff`, or a flat test world. The real-map one-thread rate is printed by `forge camera diff` | `Gpu/VisionDiff.cpp:456-461`; `VisionTest.cpp:1049` is a fake-world harness |
| Map threads on cores 0-15, learner on 16-31 | **CONFIRMED in the conf, and cores 16-31 are the SMT siblings of 0-15** (cpu n and n+16 share a core; 0-7/16-23 share the 96 MB V-cache L3, 8-15/24-31 the 32 MB one) | `env/dist/etc/worldserver.conf:1336-1337`, `mod_animus_forge.conf:324`; `/sys/devices/system/cpu/cpu0/topology/thread_siblings_list` = `0,16`; `cache/index3/size` 98304K and 32768K |
| Camera 40 KB and map 13.8 KB per seat per decision | **CONFIRMED**: image 40,960 B, map 13,824 B (spec.json of the live run); a STEP of 192 envs is 13.4 MB | measured: `encode_step` on the live spec |

One more fact that changes how the sim findings read: the live `move2_seek` spec says `decision_ticks` 1 and
`env_groups` 2, i.e. **half-batch on, one tick per decision, protocol 24**, whereas `worldserver.conf.dist` (the
template every machine's untracked conf is meant to follow) sets `TicksPerDecision = 5` for every movement stage, which
the code runs as one group with no half-batch (finding O1).

## 1. Ranked findings

Ranked by expected gain on end-to-end training throughput, weighted by how sure the evidence is. "Update" findings
matter on update-bound machines (all that were measured); "sim" findings matter once the update is faster than the
rollout, and on any machine where the sim is the slower half (not measured, UNKNOWN).

| Rank | Id | Area | Finding | Evidence | Estimated gain | Effort | Risk to correctness | Status |
|---|---|---|---|---|---|---|---|---|
| 1 | U1 | Learner | The cycle is the update: 11.2 s of update against 5 to 6 s of rollout, so the sim is idle about 50% of the time. `epochs: 2` x 4 minibatches re-uses every row twice at 456 us a row | `train.py:1760-1768`; metrics; `move2_seek.yaml:34` | Arithmetic: epochs 1 gives update about 5.6 s and cycle about 5.6-6 s, **up to +90-100% end-to-end on update-bound machines**. Learning per sample falls by an UNKNOWN amount; needs an A/B (plan step 6) | S | High for learning (quality), none for correctness | Arithmetic VERIFIED; quality HYPOTHESIS |
| 2 | U3 | Learner | The camera is encoded **three times** a minibatch when `vision_chunk_rows` is set (forward without graph, forward with graph, backward), and the stage yaml sets it for the cluster's 8 and 16 GB cards, so the 24 GB host recomputes too although M1 ran it unchunked | `trainer.py:1578-1601`; `move2_seek.yaml:46-49`; the owner's measure `move1_controls.yaml:97-99` "~1.7x the encoder's time (the forward is run twice)" | **U3a** (chunk from free VRAM, one-line policy): about 41% of the encoder time on a host that fits the minibatch, **about 1.5 s of 11.2 s (13%) if the encoder is 3.6 s (estimate)**. **U3b** (checkpoint only the decode/cut) 7-15% on small cards | S (a) / M (b) | Low | 1.7x VERIFIED (owner's own measurement); memory fit HYPOTHESIS |
| 3 | U2 | Learner | `nn.Embedding` backward on huge index sets: the camera class embedding (8,192 indices a row) and the map's code and class embeddings (2 x 2,304 a row) | `networks.py:1261`, `:1674`; comment `:1257-1259` | CPU: `embedding_dense_backward` is **38% of the whole M2 update** and a masked-sum backward is 4x faster on the op (67 ms to 16 ms for 2.1 M indices). GPU: the code's own comment says it is two thirds of the map encoder's update. **10-25% of the update (estimate)** | S-M | Low (same gradient up to summation order) | CPU VERIFIED; GPU HYPOTHESIS |
| 4 | U4 | Learner | 88% of seats are cast at 32x16, 48x24 or 64x32 (59% at the two exact ones) and nearest-upscaled to 128x64 **before** the wire; the learner decodes, embeds, patchifies and backpropagates all 8,192 pixels of every row | `Camera.h:377`; `networks.py:1110`; `Camera.h:89` | For rows cast at 32x16 and 64x32 the first layer folds exactly onto the source pixels (16x and 4x fewer pixels). **10-15% of the update (estimate)** | L | Moderate (must stay bit-for-bit comparable with the full path; a golden test exists for the pixel contract) | HYPOTHESIS |
| 5 | U5 | Learner | The decode chain (uint8 to float, where, stack, class-embed, cat, permute, copy) is memory-bound and unfused: about 7 MB of traffic a row a pass against 12 us ideal, measured 36-74 us | `networks.py:1110-1125`, `:1664-1686` | CPU: `copy_` + `cat` are 17.5% of the update. A fused path (torch.compile or one custom kernel; bf16 for the encoder only) **10-20% of the update (estimate)**; overlaps U2-U4 | M-L | Low (exact for fp32; bf16 changes numbers) | HYPOTHESIS |
| 6 | R3 | Metrics | `env_steps_per_sec` (and `forgectl status`, and the quoted per-machine rates) is the rollout phase only; the true rate is steps / wall. Decisions about the sim have been made on a number that is 2x the real one | `train.py:2024-2025`; section 0 | None directly; it prevents wrong work. Add `wall_steps_per_sec` | S | None | VERIFIED |
| 7 | S1 | Sim | `MentalMap::WriteFrame` walks every ray over the 1-yard grid up to 64 yards and does an **uncached hash lookup per step** (`FindCell` -> `unordered_map::find`) | `MentalMap.cpp:420`, `:308-316`; `MentalMap.h:76,286` | A last-tile cache (as `Touch` already has) removes most lookups; **20-60% of `WriteFrame` (estimate)**, which is itself a large share of the per-seat map cost. Only moves the wall on rollout-bound machines | S | None | HYPOTHESIS (code reading) |
| 8 | S6 | Sim / ops | The learner is pinned to the SMT siblings of the map threads (cpu 16-31 vs 0-15), and the map pool spans both CCDs, only one of which has the V-cache | `worldserver.conf:1336-1337`; `mod_animus_forge.conf:324`; sysfs | UNKNOWN, plausibly +-10-20% of rollout speed either way; one `forge bench` A/B | S | None | HYPOTHESIS |
| 9 | S2 | Sim | Caster: a third BIH traversal (`ModelLiquid`) for every descending ray whether or not the map has WMO liquid; rising rays cast 4,000 yards of tree; every unit and box tested by every ray; four trig calls a pixel | `VisionCaster.cpp:172-249`, `:470-500` | UNKNOWN until `Breakdown` is read; **5-25% of frame time (estimate)** | M | Low-moderate (CPU/GPU parity tests exist) | HYPOTHESIS |
| 10 | O4 | Cluster | A follower whose weights go non-finite is never repaired: the leader drops each push and the follower keeps training and pushing garbage until its learner restarts. In the live log rank 5 (192.168.0.65, eli) did this 371 times in one session, 6,144 steps each | `async_sync.py:316-320` (drop), `:420` (the follower rebases onto its own NaN); `animus-learner.log` lines 186709-187940 | The whole contribution of that machine for the session: 2.28 M dropped env steps (1.6% of the stage's 138 M); more if a fast machine diverges. Detect in the follower (`isfinite(now)`) and take the centre whole | S | None | VERIFIED (log); cause UNKNOWN |
| 11 | O1 | Ops | Config drift: `worldserver.conf.dist` runs every movement stage at 5 ticks a decision, which disables half-batch for it; the live run used 1 tick and half-batch. Also `Envs`, `Cpus` and `HalfBatch` are per-machine untracked keys | `worldserver.conf.dist:5181-5231`; spec.json; `mod_animus_forge.conf:171-186` | UNKNOWN; the next deploy could change the sim's cost by several x without anyone choosing it | S | Moderate | VERIFIED (the mismatch); effect UNKNOWN |
| 12 | R1 | Learner | Eager rollout path (CPU learners, or graphs off) runs the camera and map encoder **twice per decision**: once in the actor, once in the critic | `trainer.py:997`; `networks.py:1849-1856` | VERIFIED count: 2 encoder forwards a decision; **-25% of `act_and_value`** on the CPU proxy | S | None (same embedding) | VERIFIED |
| 13 | U6 | Learner | The whole rollout buffer (1.76 GB at M2, 1.35 GB of it images) is uploaded to the GPU from pageable memory at the start of every update, and held twice in host RAM | `trainer.py:1638`; `buffer.py:176-186` | 0.2-0.4 s of the 11.2 s (2-3%, estimate) | S-M | Low | HYPOTHESIS |
| 14 | R2 | Rollout | STEP decode copies 13.4 MB a decision (2.45 ms measured, 6% of a 44 ms decision); the image travels upscaled | `protocol.py:393-429`; measured | Decode: -1 ms; sending cast-size images: **-56% of camera bytes** (24,104 vs 54,784 B a row) | S / M | Low / moderate (protocol bump) | VERIFIED (sizes, ms) |
| 15 | S3 | Sim | `MentalMap::Crop` builds a 141 x 141 cell window and 9,216 samples a seat a decision, in doubles, from scratch | `MentalMap.cpp:565-678` | UNKNOWN; 0.3-0.6 ms a seat a decision (estimate) | M | Low | HYPOTHESIS |
| 16 | S4 | Sim | A terminated episode is observed (full camera, map) once as its final observation and once as the new episode's first; the learner then values it although a terminal value is never used | `EnvPool.cpp:266-272`; `train.py:1960-1969`; `buffer.py:35-56` | 1 / episode length of observe cost: **3.7% in M2's hallway rung** | S-M | Moderate | HYPOTHESIS |
| 17 | U7 | Learner | The update's per-layout matmuls have data-dependent shapes; on gfx12 TunableOp stops after 20 updates and new shapes fall back to the default kernel; it also blocks a captured update | `blas.py:30-48`; `networks.py:2081-2104` | UNKNOWN (cluster cards only) | M | Low | HYPOTHESIS |
| 18 | U8 | Learner | `maybe_checkpoint` writes 123 MB twice (numbered and `latest.pt`) after `drain_update()`, which ends the overlap for that cycle; evaluation drains likewise | `train.py:1129-1147`, `:1247` | About 1% of wall (5 evaluation gaps = 300 s of 21,662 s; a checkpoint is 1-3 s a 25 updates) | S | None | VERIFIED (cost small) |
| 19 | O3 | Ops | An evaluation of 78 episodes runs all 192 envs; 114 envs play unscored episodes | `evaluation.py:451-470` | Free precision: 192 scored episodes for the same wall time; or a quarter of the wall for 78 | S | None | VERIFIED |
| 20 | O2 | Ops | Every machine builds `-O3 -march=native -g` with LTO from source on every deploy; header fan-out and per-CPU ccache keys defeat reuse | `forge-worldserver.sh:25-60`; `settings.cmake`; `ConfigureLTO.cmake:72-74` | UNKNOWN (hours on slow machines per the owner) | M | Moderate | HYPOTHESIS |
| 21 | S5 | Design | 47% of all cast rays come from the 12% of seats drawn at 128x64 (weights 1,1,1,0.4) | `Camera.h:89`; `cpp-vision.md:96` | Dropping the 128x64 weight to 0.2 costs 25% fewer rays; changes the training distribution, so it is the owner's call | S | Training distribution | VERIFIED (arithmetic) |

## 2. The findings

### U1. The cycle is the update; the sim idles half the time

**Evidence.** `rollout()` hands the buffer of rollout N to a one-thread executor and goes on collecting rollout N+1;
at the end of N+1 `finish_update()` joins update N (`train.py:1767-1768`, `:1570-1581`). With one update in flight the
cycle is `max(rollout, update)`. From the live run's `metrics-before-20261007-110448.csv`, per window of updates, each
row decomposed as `wall = rollout + wait` (the residual is 0.0 s in every window, so nothing else costs time):

| Updates | wall a cycle | rollout | wait on update | `update_compute_seconds` | own steps/s end to end |
|---|---|---|---|---|---|
| 10-200 | 11.5 s | 4.9 s | 5.8 s | 11.3 s | 2,145 |
| 200-600 | 11.5 s | 5.2 s | 5.4 s | 11.4 s | 2,142 |
| 600-1000 | 11.2 s | 4.8 s | 6.3 s | 11.1 s | 2,197 |
| 1000-1400 | 11.3 s | **8.2 s** | **0.1 s** | 11.1 s | 2,170 |
| 1400-1800 | 11.3 s | 5.7 s | 5.6 s | 11.2 s | 2,168 |
| 1800-2020 | 11.3 s | 5.8 s | 5.4 s | 11.2 s | 2,165 |

The 1000-1400 window is the natural experiment: the sim got 3 s slower and the cycle did not move. The run has 5
gaps of 43 to 102 s (300 s of 21,662 s, 1.4%), which bounds evaluations, checkpoints and restarts together. Each update is 24,576 own rows, so 456 us a row; M2 does 2 epochs of 4
minibatches (8 gradient steps) on them (`move2_seek.yaml:34`, `move1_controls.yaml:90`). `target_kl` never cut an epoch
short (`epochs_run` 2.0, `approx_kl` 0.002 to 0.018).

**Change.** Treat update cost per row as the budget. In order of risk: (a) U2-U5 and U3a below make the same epochs
cheaper; (b) `mappo.epochs: 1` (update about 5.6 s, balanced with the 5-6 s rollout, cycle about 6 s, **+90%
end to end** if nothing else moves) or keep 2 epochs on a random half of the minibatches; (c) for a cluster of
update-bound machines, give each machine the epochs that balance *its* rollout and update (log shows both).

**Verify.** Two arms from the same `latest.pt`, one at `epochs: 2`, one at `epochs: 1`, same seeds, 30 minutes of wall
each, compared on the stage's gate metric (`found` rung by rung, `eval.jsonl`) against wall-clock, not against env
steps. The arithmetic is already verified; the quality is the open question, and it is the owner's call.

**What could go wrong.** Fewer epochs lowers the learning per sample; PPO at 1 epoch with 4 minibatches is a common
setting, but this stage's `approx_kl` is small (0.002-0.018 against a target 0.02), which suggests it can afford the
reuse it has, not that it needs it. Gate-stepped ladders count env steps, so a faster wall rate moves the rungs
sooner, which is the intent.

### U2. Embedding backward over millions of indices

**Evidence.** The camera embeds each pixel's class (`networks.py:1674`, 8,192 indices a row, 24 classes, 6 wide) and the
map embeds each cell's code and class (`:1261`, 2 x 2,304 indices a row, tables of 5 and 32). The backward of
`nn.Embedding` is a scatter-add with enormous duplication (24 distinct targets). The map encoder's own comment says it
is "about two thirds of the encoder's update on the host's card" (`networks.py:1258-1260`), and a one-hot product was
tried and was no faster or slower there.

Measured here, CPU only (8 torch threads, `claude-syntax`; see appendix): the real M2 update, scaled to 16 envs x 32
decisions, under `torch.profiler` (update 2 of 3, one camera forward and one backward a minibatch because the 128-row minibatch
is not above the 128-row chunk; the share of the encoder passes in the same run was 78% of the update, timed with
wrappers on `_encode_vision` and `_backward_vision`):

| Op | Self CPU | Share of the update |
|---|---|---|
| `aten::embedding_dense_backward` | 412 ms (276 calls, 1.56 ms each) | **38.1%** |
| `aten::mm` | 108 ms | 9.9% |
| `aten::copy_` | 97 ms | 8.9% |
| `aten::cat` | 93 ms | 8.6% |
| `aten::addmm` | 71 ms | 6.5% |

and, on the op alone for 2.1 M indices (256 rows of camera pixels, 24 classes x 6): `F.embedding` forward+backward
67 ms; a custom function whose backward is `index_add_` 55 ms, one-hot matmul 185 ms (slower on CPU), `bincount` per
dimension 17 ms, **masked sums per class 15.6 ms**, forward gather alone 3.5 ms.

**Change.** Replace the lookup with a function whose forward is the same gather and whose backward reduces by class
without a duplicated scatter: on the GPU a per-class segment sum (sort once, `segment_reduce`), or a masked reduction
over the 24 or 32 values, or a matmul against a bf16/uint8 one-hot built in the kernel. The gradient is identical up to
summation order. For the camera, the class path can also be dropped to table-sized work by folding the embedding into
the first patch layer (the contribution of a patch's classes is `sum over positions of table[position, class]`).

**Verify.** `torch.profiler` with `ProfilerActivity.CUDA` over one update on the dev GPU (plan step 1): the kernel
names `embedding_dense_backward_kernel`, `radix_sort`, `index_put` and their total. Gate: gradients of the table equal
the current ones to 1e-5 relative on one minibatch; `test_golden_update.py` unchanged within tolerance.

**What could go wrong.** The GPU result may differ from the CPU one (atomics are cheaper on some cards), in which case
the gain is the GPU share and not 38%. A hand-written backward must keep `torch.compile`/graph capture working (it is
shape-static, so it does).

### U3. The encoder runs three times; and the dev host does not need to chunk

**Evidence.** With `vision_chunk_rows` set and more rows than that, `_encode_vision` runs the whole encoder without a
graph, chunk by chunk (`trainer.py:1578-1588`), and `_backward_vision` runs it again for its gradient (`:1590-1601`):
two forwards and one backward per minibatch. The owner measured the cost himself: "1024 chunks it ... for ~1.7x the
encoder's time (the forward is run twice)" (`move1_controls.yaml:97-99`). M2 sets 2048 for the cluster's 8 and 16 GB
cards (`move2_seek.yaml:46-49`), and the setting is in the stage yaml, so the 24 GB dev host pays the 1.7x as well
although M1 ran it unchunked on the same card (`move1_controls.yaml:97-98` records 5.6 GiB for the encoder alone and 9.3 GB in use). A minibatch is 48 envs x 128 steps = 6,144 rows
(`trainer.py:1685`); `vision_chunk_rows >= 6144` disables chunking.

**Change.** (a) **U3a**: choose the chunk size at start from free device memory (`torch.cuda.mem_get_info`), so a
card that can hold the minibatch does not recompute; this is a one-line policy with the 1.7x measured. (b) **U3b**: for
cards that must chunk, checkpoint only the decode and cut (`decode_image` -> `planes` -> `patches`, recomputed from the
bytes in backward) and keep each chunk's 64-wide features and keypoints, so the recompute is the cheap elementwise
part and not the two linear layers.

**Verify.** `bench_learner` on the GPU with `--set mappo.vision_chunk_rows=0/2048/6144` and `overlap_updates=false`:
`update work` seconds and `peak device memory` in its last two lines. If the encoder is 3.6 s of the update, U3a is
worth 1.5 s (13% of the update) on the host (estimate: 3.6 x (1 - 1/1.7)).

**What could go wrong.** The memory the map adds ("the bench ran out of memory at 192 envs unchunked", the difference
"unexplained", `move2_seek.yaml:46-48`) may make the unchunked minibatch not fit even on 24 GB; U3a must then pick the
largest chunk that fits and U3b carries the gain.

### U4. Low-resolution rows are processed at full resolution

**Evidence.** A seat's frame is cast at one of 32x16, 48x24, 64x32 or 128x64 (weights 1, 1, 1, 0.4) and scaled up by
nearest pixel to 128 x 64 in the sim (`Camera.h:377`, `VisionCaster.cpp` `Upscale`), and the 40,960 upscaled bytes
travel to the learner, where every row is decoded to floats, embedded, cut into 8 x 8 patches (128 patches, 640 inputs)
and backpropagated at 8,192 pixels (`networks.py:1110-1125`, `:1664-1686`). 29% of seats are cast at 32x16 (an exact
4 x 4 upscale: each 8 x 8 patch holds 2 x 2 distinct source pixels) and 29% at 64x32 (exact 2 x 2 upscale: 4 x 4
distinct pixels per patch). The first layer `Linear(640, 64)` is linear in the planes, so for these rows it can be
evaluated on the source pixels with the weights summed over the replicated positions: 16x and 4x fewer pixels, the
same numbers.

**Change.** Group rows by their render width (it is the camera scalar `render_width` in the observation, so the group
is known on the host before the update) and run each group through a folded first layer. 48x24 (128/48 is not an
integer) stays on the full path. The cut also needs the sim to send the cast-size frame (R2).

**Estimate.** Encoder work falls to about `0.29/16 + 0.29/4 + 0.29 + 0.12` = 0.50 of the current in the memory-bound
part; with the camera at roughly a third to a half of the update that is **10-15% of the update**.

**Verify.** A test that the folded path equals the full path on random frames of each size (exact for fp32 up to
summation order); `bench_learner` before and after with the real spec.

**What could go wrong.** Ragged batches break rollout graph capture (shapes are fixed); keep the rollout on the full
path (192 rows, not the cost) and use the grouped path in the update only. The checkpoint format is unchanged because
the folded weights are derived each pass.

### U5. The decode chain is unfused and memory-bound

**Evidence.** Per row: `decode_image` produces five float planes (164 KB), `planes` concatenates the class embedding
to ten (328 KB), `patches` permutes and copies (328 KB), the Linear reads them, and every one of those tensors is
written again in backward. Roughly 7 MB of traffic a row a pass, which is 12 us at 600 GB/s; the owner's profile is
74 us a row for the camera and 77 us more for the map (2,048 rows in 151 ms, `move2_seek.yaml:28-30`). On the CPU
proxy `copy_` and `cat` are 17.5% of the update. The network itself is small (10.4 M parameters for actor and critic
together, measured from `latest.pt`: 5.2 M each, 41.6 MB as float32), so its FLOPs are not the cost.

**Change.** One fused op from bytes to patch features: `torch.compile` of `decode -> planes -> patches` (shape-static),
or a small Triton/HIP kernel; keep fp32 arithmetic. An encoder-only bf16 would halve the traffic again but is a numerics
change: the full-update bf16 test learned worse (`.agents/plans/forge-parallel-core/forge-parallel-core.PLAN.md:1329-1330`)
and the encoder alone was not tested.

**Verify.** Kernel count and bytes in the profile (plan step 1) before and after; equality of `features` to 1e-6.

**What could go wrong.** `torch.compile` on ROCm adds a minutes-long compile at every learner start on every machine
and can recompile for each render-size group; a custom kernel is more code to keep in the deploy gate's GPU tests.

### U6. Buffer upload and host copies of the rollout

**Evidence.** `_update_recurrent` does `torch.as_tensor(value, device=...)` for every buffer array
(`trainer.py:1638`) from pageable numpy memory: obs 157 MB, state 193 MB, image + map 1.346 GB, memory and critic
memory 25 MB each, about 1.76 GB, before the first kernel of the update. Two buffers exist (`train.py:768`, `:798`, the spare),
so 2.7 GB of host RAM hold image bytes.

**Change.** Keep a device copy of each decision's image as it is recorded (it is already on the device for the rollout
graph: `_Packed.upload`), or upload through pinned memory in per-decision slices during the rollout, so the update starts
with the data resident.

**Estimate.** 1.76 GB at 4 to 8 GB/s pageable is 0.2-0.4 s, 2-3% of the update. **Verify** with the profile's first
`copy_` / `Memcpy HtoD` events.

### U7. Data-dependent matmul shapes in the update

**Evidence.** The update groups each minibatch's rows by layout and runs each layout's adapter on its own rows
(`networks.py:2081-2104`, `groups=` from `per_layout_host`); the number of rows per layout changes every minibatch.
`blas.py:30-48` documents what that did on gfx12 (TunableOp met 114,888 shapes; tuning now stops after 20 updates and
"anything new runs on rocBLAS's default kernel"). The rollout side already uses fixed-shape dense adapters
(`DenseLayouts`).

**Change.** Pad each layout's rows to a bucket (multiple of 64 or 256) or use the dense path in the update, so shapes
repeat and a captured update becomes possible (the plan's open research item, `PLAN.md:1331-1332`).

**Verify.** Count distinct matmul shapes per update with the profiler (`record_shapes=True`); compare update seconds
on a gfx12 card. **Risk**: padding wastes FLOPs on tiny layouts; masked rows must not reach the loss.

### U8. Drains: checkpoints and evaluations end the overlap

**Evidence.** `_save` begins with `drain_update()` (`train.py:1129-1131`), so every checkpoint and every best-model save
waits for the update in flight and then runs the next rollout without an update to hide: the cost is one rollout's
overlap plus two `torch.save` of 123 MB (`maybe_checkpoint`, `train.py:1136-1147`: numbered and `latest.pt`, every 25
updates). `evaluate()` drains too (`:1247`). Measured: 5 gaps over 40 s in 2,021 updates, 300 s of 21,662 s (1.4%).

**Change.** Snapshot the weights to a CPU copy at the safe point and write from a thread; write once and hard-link
`latest.pt`. **Gain** under 1% of wall; listed for completeness (see "Not worth doing").

### R1. The eager rollout runs the camera and map encoder twice a decision

**Evidence.** Without a rollout graph (a CPU learner, `rollout_device: cpu`, or graphs off) `act_and_value` runs the
actor's `features(..., image=...)`, which calls `vision_term` and so the encoder (`networks.py:1849-1856`), and then
`self._rollout_critic.step(..., image=image_t)` (`trainer.py:997`), which calls the same shared encoder again
(`share_vision` makes the critic's encoder the actor's, so it is the same compute twice). The captured graph avoids
this by encoding once (`trainer.py:455-460`).

Measured, CPU only: a 2-update `bench_learner` run of 8 envs x 16 decisions with a counter on `VisionEncoder.forward`
reported `{'rollout': 72, 'update': 18}` encoder calls: 2 rollouts x (16 decisions x 2 + 4 bootstrap passes) = 72, so
**two forwards per decision**; the encoder took 0.109 s of the 0.202 s spent in `act_and_value` (a little under half once the 8 bootstrap passes
that run outside it are set aside).

**Change.** Compute `seen = actor.vision(...)` once and hand it to the critic as `vision_embedding=` (the parameter
exists on `encode`/`encode_goal_free`), as the graph path does.

**Gain.** About -25% of `act_and_value` on a CPU learner (half of it is the encoder, half of that is the duplicate).
Matters on workers without a GPU, where the rollout is the slow half; nothing on the dev host. **Risk** none: it is
the same tensor.

### R2. STEP decode, copies and the upscaled image on the wire

**Evidence.** Measured, CPU only, on a STEP with the live spec (192 envs, obs 1,595, state 1,958, image 40,960, map
13,824; 2 ended envs): payload 13.41 MB; `decode_step` 2.45 ms; a Unix-socket send+`recv_into` of 13.4 MB 0.89 ms;
`np.concatenate` of image and map 0.19 ms; a 10.5 MB copy 0.12-0.2 ms. So 3.3 ms of Python/kernel work a decision, 6-7%
of the 44 ms a decision takes on the host (rollout 5.6 s / 128). `decode_step` copies every array out of the receive
buffer (`protocol.py:393-429`), zero-fills four full-size "final" arrays and re-concatenates image and map.

**Change.** (a) Cheap: decode into preallocated arrays and skip the concatenation by keeping the map as its own field
until the buffer store. (b) The sim already knows each seat's cast size: send the cast-size frame (mean 2,056 of 8,192
pixels, 10,280 + 13,824 = 24,104 B against 54,784 B a row, **-56%**) plus the size, and upscale on the device in the
learner (a gather by the known nearest-pixel map). This shrinks the socket, the decode, the pinned copy, the host
buffer (1.35 GB to about 0.6 GB) and the upload in U6, and is the same change U4 needs.

**Gain.** (a) about 1 ms a decision (2%); (b) another 1-2 ms and about 0.7 GB of RAM. Both matter only when
rollout-bound. **Risk**: (b) is a protocol bump (the cluster fingerprint forces every machine to rebuild together).

### R3. The headline rate is the wrong number

**Evidence.** `env_steps_per_sec = rollout_length * envs * agents / rollout_seconds` (`train.py:2024`) leaves out
`update_seconds`. `forgectl status`, `forge status` and every per-machine rate quoted in the owner's brief come from it
(`docs/forge/forgectl.md:60-78`). On the host it reads 4.3k to 5.0k while the cycle delivers 2.15k to 2.2k, and
the sim-side budget "M2 is slow, so speed up the sim" is the wrong conclusion from it.

**Change.** Log `wall_steps_per_sec = steps / (time since the previous update's log)` beside it and show both in
`forgectl status` with `rollout_s`, `update_compute_s`, `wait_s`, and the verdict "update-bound" / "sim-bound". No
risk; it makes U1 visible on every machine. **Verify**: it must reproduce the 11.3 s cycle above.

### S1. `MentalMap::WriteFrame`: a hash lookup per cell step

**Evidence.** For every cast ray, "seen free" walks the 1-yard grid from the camera to the hit or to `WRITE_REACH` 64
yards (`MentalMap.cpp:379-470`, `MentalMap.h:76`). Each step calls `FindCell(cx, cy)` (`MentalMap.cpp:420`), which is
`FloorDiv`, `TileKey` and `_tiles.find(key)` on a `std::unordered_map<int64_t, std::unique_ptr<Tile>>`
(`:308-316`, `MentalMap.h:286`), with no last-tile cache; only `Touch` (the writer) has one (`:237-258`). A tile is 32 x
32 cells, so consecutive steps of a ray stay in one tile for about 16-32 steps. With a mean of 2,056 rays and, say, 20-40
steps a ray (the Stockade's corridors end rays early; open ground and sky rays run the full 64) that is 40-80 thousand
hash lookups a seat a decision, 1-2 ms at 25 ns each (ESTIMATE; the step count is not measured). Also: adjacent rays
walk nearly the same cells, so most `FindCell` answers repeat within a frame.

**Change.** Cache the last tile pointer (and key) in `FindCell` and the walk (a `mutable` member or a local in
`WriteFrame`), exactly as `Touch` does; a per-frame 3 x 3 tile window is the next step. Keep the result identical: the
function stays const in meaning. For the 4-neighbour lookups of `WriteFloor` the same cache applies.

**Verify.** `VisionTest`/mental-map tests unchanged (the writes are deterministic); `forge status` map ns
(`Vision::Cost::AddMap`, `VisionCost.h`) before and after; a `perf record` of the map threads (plan step 4) showing
`unordered_map::find` under `WriteFrame`. **Risk** none beyond cache invalidation on `Evict` (`_lastTile` is already
reset there).

### S2. The caster's per-ray work

**Evidence** (`VisionCaster.cpp`, `Nearest` `:159-250`, `CastRay` `:493-500`, `Render` `:668-737`).

1. Every descending ray calls `world.ModelLiquid` (`:205`), a third bounding-interval-hierarchy traversal after the
   static and dynamic surface casts (`:172-173`), cast to the current best distance. A map with no WMO liquid (most
   dungeons, the Stockade) can never return a hit, but the traversal is paid.
2. A rising ray is cast to `Reach`, up to 4,000 yards (`:339`), against both trees (`:172-173`) and the terrain walk,
   though it is above everything on an instance map once it is above the map's highest collision (known from the tiles'
   `MaxHeight` and the model bounds). In open-world stages the top quarter of the frame is such rays; in a dungeon they
   hit the ceiling quickly and cost little.
3. Every unit cylinder and box is tested by every ray (`:223-249`, "no culling anywhere": `cpp-vision.md:148`). A unit's
   angular footprint in the equal-angle frame is a small rectangle; with 30 units in range this is 60,000 cylinder
   tests a seat a decision against 2,056 tree casts.
4. `PixelDirection` computes two `sin` and two `cos` per pixel (`:477-482`, `Direction`), while the azimuth and
   elevation are separable (row and column tables plus the angle-addition identities need none).
5. `Reach` and `CastTerrain` both walk the same tiles through the virtual `Tile()` per ray (`:339`, `:356`).

**Change.** (1) Test once per map whether the static tree holds any liquid model and skip the call; (2) clip a rising
ray at the map's top; (3) bin units by the pixel rectangle they cover and test only those rays; (4) row/column trig
tables; (5) share the tile walk. Each is small; the parity tests (`VisionTest.*`, `forge camera diff` CPU vs GPU)
guard that nothing changed.

**Gain.** UNKNOWN until `Breakdown` (`TreeNs`, `LiquidNs`, `TerrainNs`, `UnitNs`, `VisionCaster.h`) is read on the live
maps; 5-25% of frame time is the range the code suggests. **Verify**: `VisionTest.TimingHarness` is a fake-world harness
(`VisionTest.cpp:1049`); the real number is `forge camera diff <map> <x> <y> <z> <N> [radius]` (one-thread CPU
ms a frame and M rays/s, printed by `Gpu/VisionDiff.cpp:456-458`) at a Stockade point.

### S3. `MentalMap::Crop`

**Evidence.** Each decision `Crop` (`MentalMap.cpp:565-678`) allocates `window.assign(141 x 141 MapCell)` (159 KB zero
fill), copies the tile rows into it, then for each of the 2,304 crop cells takes four rotated 1-yard samples, each
with `CodeOf`, `NearestFloor`, `EntityAge` (in double), a frontier test over four neighbours, `std::cos/sin` once. About
9,200 samples a seat a decision at an estimated 40-80 ns each: 0.4-0.7 ms (ESTIMATE). `MapBlock::Scalars` then scans the
crop again (`MapBlock.cpp:77-91`).

**Change.** Float arithmetic, a precomputed per-cell offset table rotated by one 2 x 2 matrix, no zero-fill of cells that
the copy overwrites, and fold the scalars into the crop loop. **Verify**: byte-identical crops on recorded maps (the
crop bytes are the learner's input; a changed byte is a changed policy), `forge status` map ns.

### S4. The final observation of a terminated episode

**Evidence.** On `done`, `ObserveEnv` observes the ended episode in full (camera, map, state) into `FinalObs`/
`FinalImage` (`EnvPool.cpp:266-272`), and `FinishEnv` observes the new episode's first state again
(`:299-301`). The learner then runs `trainer.value(final_state, final_obs, ...)` for every ended env
(`train.py:1960-1969`). But `compute_gae` uses the final value only when the episode was **truncated**
(`done * (1 - terminated) * final_values`, `buffer.py:47`; the same for the foresight bootstrap, `buffer.py:78`). M2's
episodes mostly end by success: 919 episodes in 24,576 steps (27 decisions each) and found is terminal.

**Change.** Skip the final render and map for a terminated episode (send the no-frame row), and skip the value pass for
those rows. **Gain**: one in 27 observations in the hallway rung (3.7% of camera and map sim time and of the end-of-step
critic pass), less at the deeper rungs (300 s episodes). **Risk**: anything that reads `final_obs` of a terminated
episode (hindsight `achieved` reads the next observation, `trainer.py:1126-1140`; the evaluation trace) must be checked
first; hence S-M effort and moderate risk.

### S5. The render-size mix (a design lever, the owner's call)

At weights 1, 1, 1, 0.4 the mean cast is 2,056 rays: 32x16 contributes 151, 48x24 339, 64x32 602, and the 12% drawn at
128x64 contribute 964, **47% of all rays**. Lowering that weight to 0.2 gives 1,540 rays (-25%) and changes what the
policy trains on (`cpp-vision.md:96`). It changes the CPU and learner cost only through the sim (the encoder cost is per
row, not per cast ray), so it matters on rollout-bound machines. No recommendation; it is listed because the lever is
the largest single sim-side knob and is a conf key (`AnimusForge.Vision.RenderSizes`).

### S6. Where the threads sit

**Evidence.** The conf has `MapUpdate.Threads = 16`, `MapUpdate.Cpus = "0-15"` and `AnimusForge.Learner.Cpus = "16-31"`.
On this CPU (Ryzen 9 9950X3D) cpu n and n+16 are the two threads of one core, so the learner's Python, copy and launch
threads run on the SMT siblings of the busy map threads; and 0-7 is the V-cache CCD (96 MB L3, `cpu0 index3/size`
98304K) while 8-15 is the plain one (32 MB). The sim is pointer-chasing code (BIH trees, hash maps, Map objects) that
likes cache. The owner's +21% over "auto" is real but compares with the unpinned layout, not with the alternatives.

**Candidates** (one `forge bench` each, plan step 5): A: map pool on 0-7,16-23 (the V-cache CCD, 16 threads), learner
on 8-15,24-31; B: map pool on 0-15 (today); C: map pool 0-7,16-23 with 12 threads and the learner on the second CCD.
**Expected**: UNKNOWN; the learner is update-bound so what matters for it is that its few CPU threads are not starved;
the sim gains if its working set stays in the 96 MB L3. Zero risk, zero code.

### O1. Configuration drift between the template and the live run

**Evidence.** `worldserver.conf.dist` sets `AnimusForge.Stage.<move|combat|group|dungeon stage>.TicksPerDecision = 5`
(`:5181-5216`) and `HalfBatch = 0` (`:5231`) and says a stage with its own tick runs as one group. The live run's spec
says `decision_ticks 1` and `env_groups 2`, and `env/dist/etc/modules/mod_animus_forge.conf:171-186` has
`TicksPerDecision = 1`, `HalfBatch = 1` and no stage ticks. The five-tick stages run the map update, the controller
and the aura ticks five times a decision with no sim/learner overlap; the template's own comment says throughput "falls
roughly with" the tick count. Nothing in the repository says which machines run which, and `conf-sync` covers only the
239 curriculum keys.

**Change.** Decide once which tick and group mode the movement stages run, put the machine-dependent keys (`Envs`,
`Cpus`, `HalfBatch`, stage ticks) in the fingerprint or in `forgectl conf-sync --check`, and record the choice in the
stage docs. **Verify**: `forge status` prints `decision_ticks`/groups; add them to `forgectl cluster`. The effect on
throughput of 5 ticks with half-batch off is UNKNOWN (plan step 3).

### O2. Builds on every machine

**Evidence.** A deploy touches `env/dist/.forge-build` and each machine runs `acore.sh compiler configure` +
`compile` (`cluster-pull.sh:47-50`, `forge-worldserver.sh:27-60`) with `-O3 -march=native`, `RelWithDebInfo` (`-g`) and
link-time optimisation for every non-Debug configuration (`settings.cmake` 44-69, `ConfigureLTO.cmake:72-74`), ccache per
machine (each CPU's `-march=native` is a different key). The Animus tree is 96 `.cpp` files, 43,773 lines; headers fan
out (`CurriculumTuning.h` is included by 18 files, `StageScenario.h`/`StageState.h` by 16), and a tuning-key change
recompiles all of them, then relinks with LTO.

**Options.** (a) Build once per ISA class on the dev machine (`x86-64-v2` for the Xeon E5-2640, `-v3` for the
i7-6700K and Ryzen 3800X, `-v4` for Zen 4 and 5) and ship the binary; the machines then only restart. (b) Drop `-g` and
LTO for the cluster profile (`FORGE_CTYPE=Release` already exists, `forge-worldserver.sh:25`). (c) Split the tuning
struct per encounter family (known issue F2) so a key does not rebuild the world. **Gain** UNKNOWN (the owner says hours;
nothing here measures it). **Risk**: floating-point results can differ across ISA classes (FMA); every machine already
differs from the others, since `-march=native` differs.

### O3. Evaluation shape

**Evidence.** `eval.episodes: 78` with `set_mode(True, seed, 78, ...)`; the sim keeps all 192 envs ticking and the
learner keeps choosing for all of them (`evaluation.py:451-470`, `max_decisions` allows `ceil(78/192) + 2` episodes of
time). 114 envs play unscored episodes. Wall cost is small (5 evaluations took 300 s of 21,662 s) but the information per
second is 40% of what the same time could give.

**Change.** Evaluate 192 episodes (a multiple of the pool; the amendment-9 design needs "each room in turn" so 195 or
a multiple of the room x object cycle) for the same wall time, or accept 78 and stop paying for the idle 114. Zero risk.

### O4. A diverged follower is never repaired

**Evidence.** The leader's `Hub.at_safe_point` drops a push whose delta is not finite and prints one line
(`async_sync.py:316-320`). The follower, when the reply lands, keeps "the progress it made while the push was out" on
top of the new centre: `rebased[:count] += now[:count] - self.pushed[:count]` (`:420`). If `now` holds a NaN the sum
does too, `assign` writes it back, and the next push is non-finite again; nothing tells the follower to take the centre
whole. In the live learner log (`env/dist/logs/animus-learner.log`, 194,280 lines, 743 MB) rank 5 (192.168.0.65, the
4-core eli) pushed non-finite weights 371 times between lines 186,709 and 187,940, each time 6,144 env steps dropped
(2.28 M env steps; the stage had 138 M), and rank 3 once (40,960). The same machine had joined three times that
session. The failure class is also documented in `async_sync.py`'s own header (a variance pushed below zero: "within
two updates every weight of every rank was NaN").

**Change.** In `Link.at_safe_point`, test `np.isfinite(now)` before rebasing and, if it fails, take `center` whole (and
log it); have the leader answer a non-finite push with a `reset` control flag. Also worth a line in `forgectl cluster`:
the number of dropped pushes a machine has made. **Verify**: a unit test that poisons one weight and checks the next
reply heals it. **Risk**: none; the alternative is a machine producing nothing.

## 3. Quick wins

Small, safe, high value. None changes what a policy learns except where marked.

| Id | Change | Where | Why it is safe | Expected effect |
|---|---|---|---|---|
| R3 | Log and show `wall_steps_per_sec`, `rollout_s`, `update_compute_s`, `wait_s` next to the rollout rate; say "update-bound" when wait > 20% of the cycle | `train.py:2020-2075`, `forgectl status` | Logging only | Makes every later decision on the right number; the host's 4.3k/s reading is really 2.17k/s |
| U3a | Choose `vision_chunk_rows` from free device memory at learner start (`torch.cuda.mem_get_info`) instead of one stage-wide 2048 | `trainer.py:1578`, `move2_seek.yaml:49` | Same maths, fewer recomputes; falls back to today's value when memory is short | The owner's own figure: chunking costs ~1.7x the encoder; about 13% of the update on the 24 GB host if the encoder is 3.6 s (estimate) |
| O4 | A follower that finds non-finite weights takes the centre whole | `async_sync.py:420` | The alternative is a dead machine | A machine's whole contribution after a divergence (2.28 M dropped steps in one session) |
| R1 | In the eager rollout, encode the image once and pass `vision_embedding=` to the critic | `trainer.py:997`, `networks.py:2309-2325` | Same tensor | -25% of `act_and_value` on CPU learners (verified count: 2 encoder forwards a decision today) |
| S1 | Last-tile cache in `MentalMap::FindCell` and the free-space walk | `MentalMap.cpp:308-316,420` | Deterministic writes unchanged | 20-60% of `WriteFrame` (estimate); matters when the sim is the slow half |
| S6 | Pin A/B/C: map pool on the V-cache CCD (0-7,16-23), learner on the other; compare `forge bench` | `worldserver.conf:1336-1337`, `mod_animus_forge.conf:324` | Config only | UNKNOWN, plausibly +-10-20% of rollout speed |
| U1 | Epochs A/B: `mappo.epochs: 1` against 2 from the same checkpoint, compared on wall-clock | `move2_seek.yaml:34` | Reversible; the quality cost is measured, not guessed | Up to +90% end to end on update-bound machines |
| O1 | Print `decision_ticks`, `env_groups`, `Envs`, `Cpus` in `forgectl cluster`; add them to `conf-sync --check` | `forgectl/cluster.py`, `confsync.py` | Read-only | Stops a deploy changing the sim's cost by accident |
| O3 | Evaluate a multiple of the pool (192) or accept 78 and stop running 114 idle envs | `move2_seek.yaml` `eval:` | Statistics only | 2.5x the episodes for the same wall time |
| R2a | `decode_step` into preallocated arrays; keep the map crop as its own field until the buffer store | `protocol.py:393-429`, `train.py:1883` | Pure copies | About 1 ms of 44 ms a decision, only when rollout-bound |

## 4. Not worth doing

With the reason, so nobody spends a week on them.

- **Moving the camera caster to the GPU for training** (`Gpu/VisionGpu.cpp`, `VisionDevice.h`, 786 + 1,680 lines, used by
  two console commands only). The host is update-bound, so a faster rollout buys nothing, and the GPU is exactly the
  resource the update is short of. Revisit only when the update is below the rollout. (`Renderer::Forget` has no
  production caller, known issue B1.)
- **The cluster weight exchange.** 41.6 MB a trade (10.4 M float32 parameters of actor and critic, measured from
  `latest.pt`), about six trades per leader update (12,400 trades in the log), so about 15 MB/s each way on a 125 MB/s
  link, and the leader's safe point (`flatten`, an `isfinite` scan, `mix`, `assign`) is an estimated 50-100 ms of an 11.3 s
  cycle. `weight_sync_every` already exists if a thin link ever needs it (followers trading 2,048 steps per 41.6 MB push
  are the case to watch).
- **Checkpoint size and frequency, `metrics.csv`, `progress.json`, TensorBoard scalars, `layouts.csv`.** 123 MB twice a
  25 updates (283 s) is under 1 MB/s; the rows are flushed once an update; the TensorBoard scalars are about 300 floats.
  `layouts.csv` is 100 MB and `animus-learner.log` 743 MB with no rotation: disk, not time.
- **Evaluation frequency.** Five gaps over 40 s in 21,662 s (1.4%) including checkpoints and restarts; evaluation of 78
  episodes every 10 M steps is cheap. D2/D3 evaluations, which the known-issues file says take hours of sim time, are the
  exception: measure them (plan step 9) before deciding.
- **The reset path for the movement stages.** The old 6.4 ms (2026-09-17) became 0.77 ms in the parallel-core work
  (`PLAN.md:562`), `Characters.KeepCasting` keeps a character for up to 4 episodes (`CurriculumTuning.h:64-71`), and M2's
  hallway rung resets 3.7% of steps. Re-check it when the dungeon stages run (their resets build whole instances).
- **`LayeredField::Store::Find`'s global shared lock, clock and `shared_ptr` copy** (`LayeredField.cpp:407-420`): three
  contended atomics a call, but the only caller keeps a per-route map of what it already holds (`FieldRoute.cpp:63`), so
  it runs once a route plan, not once a step.
- **`EntityMemory` linear scans** (`EntityMemory.cpp:56-110`): at most 32 seen entities against a cap of at most 1,024
  entries, a few microseconds a seat.
- **Rollout graph fallbacks.** The only fallback condition is seat sets, which are off in every live yaml
  (`trainer.py:883-895`; known issue B4). On a GPU the rollout is one launch and one wait.
- **`isfinite().all()` per decision** (`train.py:1815-1817`): a device sync only for device observations; the host path is
  a numpy call on 1.2 MB.
- **A deeper update/rollout pipeline.** With one update in flight the cycle is already `max(rollout, update)`; a second
  one cannot make the update shorter, only staler (U1 is the lever).
- **Whole-update bf16, TunableOp, 48 replicas, the geometry probe** were measured and rejected in the parallel-core
  plan (`PLAN.md:1326-1337`).
- **`torch_threads`.** The GPU learner's CPU work is Python and copies on a few threads; `torch_threads: 4` against
  `Learner.Cpus` 16 is not where the time is.

## 5. Measurement plan

Ordered by how much each answer changes what to do next. Nothing here was run by this audit unless it says so; steps 2-12
need the GPU, a build, a live sim or another machine. "Stop the plan" means `forgectl stage cancel` (a profile on the
dev GPU while a learner trains on it is meaningless).

1. **Cycle decomposition on every machine (no GPU; done for the dev host in this audit).** Parse each learner's
   `update N | steps S | R sps | rollout 3.20s compute 4.90s` line and its metrics.csv: wall per update from
   `elapsed_seconds`, rollout = rows / `env_steps_per_sec`, wait = `update_seconds`. Script A in the appendix reproduces
   the table in U1 (residual 0.0 s). For the workers, `ssh <machine> 'grep -a "^update" ~/animus-forge/env/dist/logs/animus-learner.log | tail -200'`
   and compare `rollout` with `compute`. **Confirms U1** if compute exceeds rollout; **sizes epochs per machine**.
2. **Profile one live-shaped update on the GPU (U2, U3, U4, U5, U6, U7).** Stop the plan. In the dev container with the
   GPU, make a layouts directory holding `layouts/move2_seek/*_seek.json` plus `runs/move2_seek/stage.json`, then run
   script B (the profile wrapper in the appendix, CUDA activity on) with 192 envs, `overlap_updates=false`,
   `mappo.vision_chunk_rows=2048`, 3 updates. Read: (a) total CUDA time against `update_compute_seconds`; (b) kernels
   matching `embedding_dense_backward`, `radix_sort`, `index_put`, `scatter` and their sum; (c) elementwise
   (`copy`, `cat`, `where`, `permute`) against GEMM time; (d) `Memcpy HtoD` at the start; (e) distinct matmul shapes with
   `record_shapes=True`; (f) the wrapper timings for `_encode_vision`/`_backward_vision` with `torch.cuda.synchronize()`.
   **Confirms U2** if the embedding-backward family is at least 10% of the update, **U5** if elementwise exceeds GEMM,
   **U6** if the first events are a 1.7 GB host-to-device copy of more than 0.15 s, **U7** if several hundred distinct
   shapes appear. This also settles the open 7.6 s: what the update spends beyond the encoders.
3. **The chunk and epoch matrix (U1, U3a).** Same script as step 2 without the profiler:
   `python -m animus.bench_learner --config configs/move2_seek.yaml --spec spec.json --layouts <dir> --envs 192 --sim-ms 0 --updates 4 --set overlap_updates=false --set mappo.vision_chunk_rows=0|2048|6144 --set mappo.epochs=1|2`.
   Its last lines give `update work` seconds and `peak device memory`. **Confirms U3a** if `6144` fits and is about 35-40%
   faster on the encoder part; **U1's arithmetic** if epochs 1 halves `update work`.
4. **Sim-only speed and the camera's share (S1, S2, S5).** In the console, `forge bench move2_seek` (it times the sim at
   every `AnimusForge.Bench.Threads` x `Bench.Envs` with policy random; set `Bench.Threads = "8, 12, 16"`,
   `Bench.Envs = "96, 192"`). Then restart with `AnimusForge.Vision.RenderSizes = "32x16"` and repeat: the difference is
   the cost of the other three sizes' rays; with the map unchanged the remainder bounds the map and game costs. During a
   run, `forge status` shows the vision row (`Vision::Cost`: frame ns, rays, map ns, STEP bytes) and `forge tasks` the
   slowest map tasks. **Gives** the per-seat frame and map milliseconds that this audit could only estimate.
5. **Where the map threads spend cycles (S1, S2, S3).** Owner-run, because the classifier blocks `docker exec` on the live
   worldserver: `perf record -F 499 -g -p $(pgrep -x worldserver) -- sleep 30` then
   `perf report --no-children --sort symbol | head -60` in the worldserver container (RelWithDebInfo has symbols). **Confirms
   S1** if `std::_Hashtable::find` under `MentalMap::WriteFrame` is above 10% of map-thread cycles, **S2** if
   `GetLiquidHit` (via `ModelLiquid`) is a visible fraction of `GetSurfaceHit` on a map without WMO liquid, **S3** if
   `MentalMap::Crop` exceeds the frame cast.
6. **Real-map caster rate.** `forge camera diff 34 <x> <y> <z> 64` at a Stockade hallway point while the forge is idle
   (map 34; it prints one-thread CPU ms a frame and M rays/s, `Gpu/VisionDiff.cpp:456-458`). **Settles** the unconfirmed
   "170-600 M rays/s": a BIH-traced frame at 8,192 rays that takes 30 us would be 270 M rays/s.
7. **Pinning A/B/C (S6).** Three restarts of the dev host with the same stage: A `MapUpdate.Cpus = "0-15"`,
   `Learner.Cpus = "16-31"` (today); B `MapUpdate.Cpus = "0-7,16-23"` (the V-cache CCD, `MapUpdate.Threads = 16`),
   `Learner.Cpus = "8-15,24-31"`; C as B with `Threads = 12`. Compare the learner log's `rollout` seconds over 20 updates
   (the sim half) and `forge bench`. **Decides** whether the V-cache and SMT placement matter.
8. **Epoch A/B for quality (U1).** Two arms from the same `latest.pt`: `mappo.epochs` 2 and 1, same stage, 30 to 45
   minutes of wall, read `found` per rung from `eval.jsonl` and `found` in `metrics.csv` against `elapsed_seconds`.
9. **Dungeon-stage costs (D2, D3).** After the first dungeon run: `forge status` (reset parts, p50/p95 resets, route
   plans), the learner's `rollout` against `compute`, and the `eval.jsonl` gaps. The M-stage conclusions (update-bound,
   resets small) must be re-checked for stages whose resets build instances and whose evaluations take hours.
10. **Config and cadence audit (O1).** On every machine: `python3 -c "import json;s=json.load(open('<runs>/<stage>/spec.json'));print(s['decision_ticks'],s['tick_ms'],s['env_groups'],s['num_envs'])"`
    and `grep -nE "TicksPerDecision|HalfBatch|\.Envs|Cpus" env/dist/etc/modules/mod_animus_forge.conf`. **Confirms O1** if two
    machines differ, or if the host's conf lacks the stage ticks `conf.dist` sets.
11. **Build time (O2).** `docker logs --timestamps ac-animus-forge-worldserver | grep -nE "Configuring|Compiling|Linking|ready"`
    on each machine for the last deploy; `ccache -s` inside the container (hit rate, cache size); `ninja -C var/build -t
    commands | wc -l` after touching `CurriculumTuning.h` to count what a tuning edit rebuilds. Time a build with
    `FORGE_CTYPE=Release` and with LTO off on the slowest machine.
12. **Startup and resume.** `deploy-gate.md:79` quotes 5 minutes to the first update after a resume. From the learner log
    and `Server.log` timestamps: worldserver start to "ready", learner spawn to `Connecting`, `seeding` to the first
    `update` line (checkpoint load, rollout-graph capture, MIOpen/TunableOp first-shape work). UNKNOWN until read.

## 6. Where the time goes, as best the evidence allows

### One update cycle on the dev host (M2, 24,576 own steps, 192 envs x 128 decisions)

| Part | Time | Basis |
|---|---|---|
| Whole cycle | **11.3 s** (2.17k own steps/s) | VERIFIED, metrics, 2021 updates |
| Update compute (8 gradient steps, 456 us a row) | **11.2 s**; equals the cycle in every window | VERIFIED |
| Rollout (128 decisions, 43.8 ms each) | 4.8-5.8 s (8.2 s in updates 1000-1400) | VERIFIED; hidden behind the update |
| Sim and actor idle, waiting on the update | 5.4-6.3 s, about 49% of the cycle | VERIFIED |
| Evaluations, checkpoints, restarts | 300 s of 21,662 s (1.4%) | VERIFIED |
| Camera and map encoders inside the update | 3.6 s by the 2026-10-06 profile (32%); 78% on the CPU proxy | Old GPU figure; CPU proxy VERIFIED; current GPU split UNKNOWN |
| `embedding_dense_backward` | 38% of the CPU update; GPU share UNKNOWN (code comment: two thirds of the map encoder) | CPU VERIFIED |
| Elementwise decode chain (`copy_`, `cat`) | 17.5% on the CPU proxy | CPU VERIFIED |
| GEMMs (`mm`, `addmm`) | 16.4% on the CPU proxy; the network has 10.4 M parameters | CPU VERIFIED |
| Buffer upload (1.76 GB) | 0.2-0.4 s | ESTIMATE |
| GRU, trunk, heads, optimiser, foresight, goal terms | UNKNOWN (the rest of the 7.6 s) | needs step 2 |

### One rollout decision on the dev host (192 envs, 43.8 ms)

| Part | Time | Basis |
|---|---|---|
| Whole decision | 43.8 ms (5.6 s / 128) | VERIFIED |
| Same stage family without a camera | 4.9 ms (stage8_duel) and 6.2 ms (stage1_move) a decision | `PLAN.md:1313` (the plan marks its own figures from one window inflated) |
| Socket receive of the 13.4 MB STEP | 0.9 ms | measured, CPU, Unix socketpair |
| `decode_step` and camera concatenation | 2.45 ms | measured, CPU |
| Copies into the pinned buffer and the rollout buffer | 0.4 ms | measured, CPU (cache-hot; more in practice) |
| Actor and critic inference with camera and map, 192 rows, one graph replay | UNKNOWN; about 3-6 ms by scaling the 151 ms / 2,048-row figure (ESTIMATE) | needs step 2 |
| Sim half: frame, map write, crop, controller, game tick, reward, resets | the remainder, about 35 ms wall if lock-stepped, i.e. about 2.9 ms of thread time a step on 16 threads (ESTIMATE; half-batch overlaps the halves) | needs steps 4 and 5 |
| of which resets | 3.7% of steps end an episode; at 0.77-4 ms each this is 0.03-0.15 ms a step | ESTIMATE from `PLAN.md:562`, `:1336` |

What the pictures say together: the camera stage's rollout costs about 7 to 9 times a non-camera stage's decision, and
its update costs about 19 times per row (456 us against about 24 us for the duel stage's update of 0.58 s on the same
24,576 rows, `PLAN.md:1324-1325`, itself the wall there), and the update is the part that sets the pace. The sim-side findings (S1-S4, R2) are second-order today and
become the pace-setters once the update is below the rollout; the order of work is the update first, then the sim.

## 7. Appendix: what was run

All on the dev machine, CPU only, read-only on the live run's files, nothing started on the cluster or the GPU. The
dev container is `claude-syntax` (`HIP_VISIBLE_DEVICES=""`, `/azerothcore/apps/forge/python/.venv/bin/python`); the
worktree was linked at `/tmp/wt`; scratch scripts lived in `/tmp` (not in the repository).

1. **Cycle decomposition** (script A below) over `metrics-before-20261007-110448.csv` and `metrics.csv`: the table in U1.
2. **`bench_learner` on CPU**, 8 envs, `rollout_length=16`, `mappo.chunk_length=16`, `torch_threads=8`,
   `overlap_updates=false`, with the live `spec.json`/`stage.json` of `move2_seek`: 128 rows per update took 0.31 s of
   update work (2.4 ms a row) and 0.10-0.12 s of rollout (8 envs a decision). This is the CPU-learner cost (spencer-class
   machines): about 60 s of update for 24,576 rows at 8 threads, before any slower CPU.
3. **Update profile**, 16 envs x 32 decisions (script B, CPU activity): the table in U2. Encoder wrappers in the same run:
   `_encode_vision` 1.11 s, `_backward_vision` 2.26 s of an update total of 4.35 s over 3 updates (78%).
4. **Embedding micro-benchmark** (script C): 2.1 M indices, 24 x 6 table.
5. **Encoder call counter**: patched `VisionEncoder.forward`; 72 rollout calls and 18 update calls over 2 rollouts of 16
   decisions: two per decision.
6. **STEP decode benchmark**: `encode_step` of a live-spec STEP then `decode_step`, a `socketpair` send/`recv_into`, and
   numpy copies: the R2 figures.
7. **Checkpoint sizes**: `torch.load` of `latest.pt` (123 MB): actor 5,212,042 and critic 5,197,460 parameters and buffers.
8. **Topology and cache**: `lscpu`, `thread_siblings_list`, `cache/index3/size`.
9. **Logs**: `animus-learner.log` greps for non-finite pushes, joins, trades; `Server.log` for vision lines (the periodic
   report goes to the console only).

Script A (cycle decomposition, any run directory):

```python
import csv, statistics as st
rows = list(csv.DictReader(open("metrics.csv")))
f = lambda r, k: float(r[k]) if r.get(k) not in (None, "") else float("nan")
OWN = 24576   # rollout_length * envs * agents of this learner
for lo, hi in ((0, 200), (200, 600), (600, 1000), (1000, 1400), (1400, 1800)):
    part = rows[lo:hi]
    wall, roll, wait, comp = [], [], [], []
    for a, b in zip(part, part[1:]):
        d = f(b, "elapsed_seconds") - f(a, "elapsed_seconds")
        if 0 < d < 100:
            wall.append(d); roll.append(OWN / f(b, "env_steps_per_sec"))
            wait.append(f(b, "update_seconds")); comp.append(f(b, "update_compute_seconds"))
    print(lo, hi, "wall", st.median(wall), "rollout", st.median(roll), "wait", st.median(wait),
          "compute", st.median(c for c in comp if c == c), "e2e", OWN / st.median(wall))
```

Script B (update profile; for the GPU use `ProfilerActivity.CUDA` too, `sort_by="cuda_time_total"`, 192 envs, and
`torch.cuda.synchronize()` inside any timing wrapper):

```python
import sys, os
sys.path.insert(0, "/tmp/wt/apps/forge/python"); os.chdir("/tmp/wt/apps/forge/python")
from torch.profiler import profile, ProfilerActivity
from animus.mappo import trainer as T
orig, n = T.MappoTrainer.update, {"i": 0}
def patched(self, buffer, auxiliary=None, sync=True):
    n["i"] += 1
    if n["i"] != 2:
        return orig(self, buffer, auxiliary, sync)
    with profile(activities=[ProfilerActivity.CPU]) as prof:
        out = orig(self, buffer, auxiliary, sync)
    print(prof.key_averages().table(sort_by="self_cpu_time_total", row_limit=28, max_name_column_width=60))
    return out
T.MappoTrainer.update = patched
if __name__ == "__main__":        # bench_learner spawns the fake sim, so the guard is required
    from animus import bench_learner
    sys.argv = ["bench_learner", "--config", "configs/move2_seek.yaml", "--spec", "/tmp/spec_m2.json",
                "--layouts", "/tmp/lay", "--envs", "16", "--sim-ms", "0", "--updates", "2",
                "--set", "rollout_length=32", "--set", "mappo.chunk_length=32", "--set", "torch_threads=8",
                "--set", "overlap_updates=false", "--set", "mappo.vision_chunk_rows=128"]
    bench_learner.main()
```

(`/tmp/lay/move2_seek/` held the run's `*_seek.json` layout manifests and its `runs/move2_seek/stage.json`; the older
`layouts/move2_seek/stage.json` lacks the map block and makes the fake sim refuse the spec.)

Script C (embedding backward alternatives): 2,097,152 random class indices (80% in {0,1,2}), a 24 x 6 table, 8 threads;
`F.embedding(...).backward(g)` against `index_add_`, a one-hot matmul, `bincount` per dimension and per-class masked sums
`torch.stack([g[idx == c].sum(0) for c in range(24)])`, all checked equal to the reference to 1e-3; results in U2.
<!-- END -->
