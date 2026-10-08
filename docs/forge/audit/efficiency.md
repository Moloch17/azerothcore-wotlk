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
| 2 | U2 | Learner | `nn.Embedding` backward on huge index sets: the camera class embedding (8,192 indices a row) and the map's code and class embeddings (2 x 2,304 a row) | `networks.py:1261`, `:1674`; comment `:1257-1259` | CPU: `embedding_dense_backward` is **38% of the whole M2 update** and a masked-sum backward is 4x faster on the op (67 ms to 16 ms for 2.1 M indices). GPU: the code's own comment says it is two thirds of the map encoder's update. **10-25% of the update (estimate)** | S-M | Low (same gradient up to summation order) | CPU VERIFIED; GPU HYPOTHESIS |
| 3 | U4 | Learner | 71% of seats are cast at 32x16, 48x24 or 64x32 and nearest-upscaled to 128x64 **before** the wire; the learner decodes, embeds, patchifies and backpropagates all 8,192 pixels of every row | `Camera.h:377`; `networks.py:1110`; `Camera.h:89` | For rows cast at 32x16 and 64x32 the first layer folds exactly onto the source pixels (16x and 4x fewer pixels). **10-15% of the update (estimate)** | L | Moderate (must stay bit-for-bit comparable with the full path; a golden test exists for the pixel contract) | HYPOTHESIS |
| 4 | U3 | Learner | With `vision_chunk_rows` the camera is encoded three times per minibatch: forward with no graph, then forward with graph, then backward | `trainer.py:1578-1601`, `:1718`, `:1928` | Checkpointing only the decode and patch cut and keeping the features removes about one third of the encoder work. **7-15% of the update (estimate)** | M | Low-moderate | HYPOTHESIS |
| 5 | U5 | Learner | The decode chain (uint8 to float, where, stack, class-embed, cat, permute, copy) is memory-bound and unfused: about 7 MB of traffic a row a pass against 12 us ideal, measured 36-74 us | `networks.py:1110-1125`, `:1664-1686` | CPU: `copy_` + `cat` are 17.5% of the update. A fused path (torch.compile or one custom kernel; bf16 for the encoder only) **10-20% of the update (estimate)**; overlaps U2-U4 | M-L | Low (exact for fp32; bf16 changes numbers) | HYPOTHESIS |
| 6 | R3 | Metrics | `env_steps_per_sec` (and `forgectl status`, and the quoted per-machine rates) is the rollout phase only; the true rate is steps / wall. Decisions about the sim have been made on a number that is 2x the real one | `train.py:2024-2025`; section 0 | None directly; it prevents wrong work. Add `wall_steps_per_sec` | S | None | VERIFIED |
| 7 | S1 | Sim | `MentalMap::WriteFrame` walks every ray over the 1-yard grid up to 64 yards and does an **uncached hash lookup per step** (`FindCell` -> `unordered_map::find`) | `MentalMap.cpp:420`, `:308-316`; `MentalMap.h:76,286` | A last-tile cache (as `Touch` already has) removes most lookups; **20-60% of `WriteFrame` (estimate)**, which is itself a large share of the per-seat map cost. Only moves the wall on rollout-bound machines | S | None | HYPOTHESIS (code reading) |
| 8 | S6 | Sim / ops | The learner is pinned to the SMT siblings of the map threads (cpu 16-31 vs 0-15), and the map pool spans both CCDs, only one of which has the V-cache | `worldserver.conf:1336-1337`; `mod_animus_forge.conf:324`; sysfs | UNKNOWN, plausibly +-10-20% of rollout speed either way; one `forge bench` A/B | S | None | HYPOTHESIS |
| 9 | S2 | Sim | Caster: a third BIH traversal (`ModelLiquid`) for every descending ray whether or not the map has WMO liquid; rising rays cast 4,000 yards of tree; every unit and box tested by every ray; four trig calls a pixel | `VisionCaster.cpp:172-249`, `:470-500` | UNKNOWN until `Breakdown` is read; **5-25% of frame time (estimate)** | M | Low-moderate (CPU/GPU parity tests exist) | HYPOTHESIS |
| 10 | O1 | Ops | Config drift: `worldserver.conf.dist` runs every movement stage at 5 ticks a decision, which disables half-batch for it; the live run used 1 tick and half-batch. Also `Envs`, `Cpus` and `HalfBatch` are per-machine untracked keys | `worldserver.conf.dist:5181-5231`; spec.json; `mod_animus_forge.conf:171-186` | UNKNOWN; the next deploy could change the sim's cost by several x without anyone choosing it | S | Moderate | VERIFIED (the mismatch); effect UNKNOWN |
| 11 | R1 | Learner | Eager rollout path (CPU learners, or graphs off) runs the camera and map encoder **twice per decision**: once in the actor, once in the critic | `trainer.py:997`; `networks.py:1849-1856` | VERIFIED count: 2 encoder forwards a decision; **-25% of `act_and_value`** on the CPU proxy | S | None (same embedding) | VERIFIED |
| 12 | U6 | Learner | The whole rollout buffer (1.76 GB at M2, 1.35 GB of it images) is uploaded to the GPU from pageable memory at the start of every update, and held twice in host RAM | `trainer.py:1638`; `buffer.py:176-186` | 0.2-0.4 s of the 11.2 s (2-3%, estimate) | S-M | Low | HYPOTHESIS |
| 13 | R2 | Rollout | STEP decode copies 13.4 MB a decision (2.45 ms measured, 6% of a 44 ms decision); the image travels upscaled | `protocol.py:393-429`; measured | Decode: -1 ms; sending cast-size images: **-56% of camera bytes** (24,104 vs 54,784 B a row) | S / M | Low / moderate (protocol bump) | VERIFIED (sizes, ms) |
| 14 | S3 | Sim | `MentalMap::Crop` builds a 141 x 141 cell window and 9,216 samples a seat a decision, in doubles, from scratch | `MentalMap.cpp:565-678` | UNKNOWN; 0.3-0.6 ms a seat a decision (estimate) | M | Low | HYPOTHESIS |
| 15 | S4 | Sim | A terminated episode is observed (full camera, map) once as its final observation and once as the new episode's first; the learner then values it although a terminal value is never used | `EnvPool.cpp:266-272`; `train.py:1960-1969`; `buffer.py:35-56` | 1 / episode length of observe cost: **3.7% in M2's hallway rung** | S-M | Moderate | HYPOTHESIS |
| 16 | U7 | Learner | The update's per-layout matmuls have data-dependent shapes; on gfx12 TunableOp stops after 20 updates and new shapes fall back to the default kernel; it also blocks a captured update | `blas.py:30-48`; `networks.py:2081-2104` | UNKNOWN (cluster cards only) | M | Low | HYPOTHESIS |
| 17 | U8 | Learner | `maybe_checkpoint` writes 123 MB twice (numbered and `latest.pt`) after `drain_update()`, which ends the overlap for that cycle; evaluation drains likewise | `train.py:1129-1147`, `:1247` | About 1% of wall (5 evaluation gaps = 300 s of 21,662 s; a checkpoint is 1-3 s a 25 updates) | S | None | VERIFIED (cost small) |
| 18 | O3 | Ops | An evaluation of 78 episodes runs all 192 envs; 114 envs play unscored episodes | `evaluation.py:451-470` | Free precision: 192 scored episodes for the same wall time; or a quarter of the wall for 78 | S | None | VERIFIED |
| 19 | O2 | Ops | Every machine builds `-O3 -march=native -g` with LTO from source on every deploy; header fan-out and per-CPU ccache keys defeat reuse | `forge-worldserver.sh:25-60`; `settings.cmake`; `ConfigureLTO.cmake:72-74` | UNKNOWN (hours on slow machines per the owner) | M | Moderate | HYPOTHESIS |
| 20 | S5 | Design | 47% of all cast rays come from the 12% of seats drawn at 128x64 (weights 1,1,1,0.4) | `Camera.h:89`; `cpp-vision.md:96` | Dropping the 128x64 weight to 0.2 costs 25% fewer rays; changes the training distribution, so it is the owner's call | S | Training distribution | VERIFIED (arithmetic) |

<!-- END -->
