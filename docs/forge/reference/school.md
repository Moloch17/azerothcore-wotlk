# The planner school: `animus/school/`

Purpose and scope. The route planner trained on its own -- offline, supervised, against the sim's labels -- before a
bot steers by its waypoints (the plan in `.agents/plans/planner-school/`, 2026-10-17; stage 1 of three). The nav bots
steer by the route the sim plans with Dijkstra over the bot's own mental map ([cpp-vision-memory.md](cpp-vision-memory.md),
"The route on the map"): exact over what the bot knows, a straight-line guess through what it has not seen. The
school learns to plot the route through unseen space from the crops alone, and is measured beside that Dijkstra and
beside the oracle on the same samples. Stage 2 -- the school as the source of the policy's seventeen route scalars
-- is the last section; stage 3 (the curriculum) is not built here.

## Map of the files

| Path | Role |
|---|---|
| `apps/forge/python/animus/school/record.py` | the recorder in the learner's rollout; the shard format (`SAMPLE_FIELDS`, `ShardWriter`) |
| `apps/forge/python/animus/school/data.py` | shards as a streaming source of batches; the split; a batch's decoded planes, goal planes, targets and masks (`prepare`) |
| `apps/forge/python/animus/school/model.py` | `SchoolPlanner` (two-tier U-Net), `VinPlanner` (value iteration), `CoarseFrame` (the hierarchy's resample) |
| `apps/forge/python/animus/school/metrics.py` | `Scores` (the metrics), `Baselines` (Dijkstra over the crop, the oracle), `pass_mark` |
| `apps/forge/python/animus/school/train.py` | the trainer, `python -m animus.school.train` |
| `apps/forge/python/animus/school/synthetic.py` | synthetic samples in the shard format (random rooms; the smoke) |
| `apps/forge/python/configs/school.yaml` | the trainer's config (`SchoolTrainConfig`) |
| `apps/forge/python/animus/config.py` `SchoolConfig` | the recorder's keys, `school.*` in a stage yaml ([config-yaml.md](config-yaml.md)) |
| `apps/forge/python/animus/train.py` | the four hooks: built in `TrainingRun.__init__`, `observe` in `_act_on_rows` before the actions go, `episode_ended` in `_take_outcome_of`, `close` in `finish` |

## The recorder and the data format

Switched on by `school.record: true` in a nav stage's yaml (nothing else in the policy path changes; with it off no
object is built). Every `school.record_every` decisions of an env (each env's own clock, reset with its episode) the
present agents' rows are sampled. A sample:

| Array | dtype, shape | What |
|---|---|---|
| `crops` | u8 [36864] | the raw map crop bytes of both tiers, `[fine 48 x 48 x 8][coarse 48 x 48 x 8]`, the camera row's bytes after the image (`networks.decode_map` decodes a tier) |
| `labels` | u8 [13848] | the raw label bytes (protocol 33, `protocol.LABEL_*`): fine and coarse class grids u8, fine and coarse route cost-to-go f16 (65504 unreachable), six f32 scalars (C(body), C(start), the map costs, the goal's forward / right yards in the body frame) |
| `route` | f32 [17] | the sim's route scalars off the observation (`networks.ROUTE_COLUMNS` order, raw): its Dijkstra waypoint on the bot's own 1-yd map, for a later comparison |
| `memory` | i8 | 1 the seat adopted the map's shared memory, 0 blank, -1 unknown: the episode's `shared_memory` info column where the stage has one, else the map block's `kept` scalar |
| `kept` | f32 | that scalar as sent (-1 without the column) |
| `map`, `arena` | i32 | the game map id and the arena index, from the episode's info (`map`, `arena` columns) |
| `layout` | i16 | the layout index |
| `step` | i32 | the decision's index in its episode (0 the first) |
| `episode` | i64 | an id unique over the run (`rank << 40` + a count), for a hold-out by episode |
| `env` | i32 | the env |

The map, the arena and the memory flag are episode-end facts, so a decision's sample waits in its env's pending
list and is stamped when the episode ends; the samples of episodes still running when the learner stops, when an
evaluation restarts every env (the training MODE after it) or when a cluster worker rejoins are dropped
(`SchoolRecorder.clear`) and counted in the close line. Evaluations are never sampled: they run their own loop
(`evaluation.run_evaluation`), not `_act_on_rows`. The envs' clocks start staggered (`env % record_every`), so a
decision samples 1 / record_every of the envs from the first one. Shards are compressed `.npz` of `school.record_shard` samples (~50 KB a sample raw,
less compressed), named `rank<r>-<n>.npz` and numbered on from the writer's own `manifest-rank<r>.json` (counts per
map, layout and memory flag, per shard and in all; a reader merges every writer's, so learners sharing a directory
never write one file), so a resumed run adds to the directory and `school.record_max_samples` counts what is there
already. Default directory: `var/animus-forge/shared/school/<run_name>/`.

Cost. The hot path slices the sampled rows' crop and label bytes (a device buffer's rows cut on the device and copied
down on the rollout stream, as the finite check reads them) into per-layout batches; compression and writing run on
one background thread. Measured with fake STEPs in the dev container (192 envs x 1 agent, record_every 8, an idle CPU, 2026-10-17):
`observe` 0.41 ms a decision (24 rows' crops and labels copied, ~1.2 MB), `episode_ended` 0.07 ms, the shards 2.3 s
of the background thread over 3,560 samples -- against the ~24 ms a decision of the learner's loop about 1.7 % at
every 8, so the default is every 16 (~0.9 %; 12 samples a decision, 500k in ~20 min of rollouts at 40 decisions/s).
A device-buffer rollout (`rollout_device: auto`) adds the sampled rows' copy down the rollout stream, not measured.

`animus.school.record --from-eval-traces` was considered and is not possible: the evaluation traces carry no crops.
The recorder is the data source.

## The dataset (`data.py`)

- Decoding: `networks.decode_map` per tier (the encoder's own decode); the goal planes as the hierarchy builds them
  (`networks.goal_plane`: the coarse clamped to its border, the fine a one-hot inside the crop only, zero outside).
- Targets per tier: the cost-to-go (yards, sentinel-masked), `reachable`; the direction target `[N, 8, H, W]` and
  the graded cells, the aux pass's rule (`MappoTrainer._aux_pass`): graded where the label is reachable and a
  reachable neighbour is strictly lower; every lowest neighbour is a target (a plan's first of equals in
  `NEIGHBOURS` order is the one it descends to, so argmax hits are judged against the multi-hot); the classes
  (TRAV indexes, 0 unlabelled); `known` (the crop's code byte not unknown); `body` (the anchor cell, `MapEncoder.
  WAYPOINT_ANCHOR` = 23, is graded).
- `body_unseen`: the school's own convention for the body's seen / unseen split -- the label's own 10-step descent
  from the anchor crosses a fine cell the crop does not know (the true route goes into the unknown). The trainer
  never split the body (the body's own cell is nearly always known).
- Split: Wailing Caverns (map 43) held out entirely; where no shard holds a sample of it, every tenth episode by hash
  is held out instead and the source says so. Validation is every tenth remaining episode by hash; the rest trains.
- Streaming: shards are loaded `pool_shards` at a time in a shuffled order -- the epoch's draw for training, a fixed
  shuffle for the evaluation splits, whose `eval_samples` cap would otherwise read the run's earliest shards alone
  (no memory map for a compressed .npz;
  300-500k samples would be 15-25 GB), the pool's rows of the split kept, batches drawn from the pool -- with
  `balance`, weighted so every (map, memory) group of the pool is drawn equally often.
- `check_batch`: the sanity line the trainer prints first -- the fine label's centre four against C(body) (the
  generator's conventions, and a real stage's), the shares of reachable, known and goal-present rows.

## The models (`model.py`)

Inputs a tier (`tier_inputs`, 18 planes): the code one-hot (5), the entity class through a 4-wide embedding,
decode_map's seven values (height, known, visited, age, frontier, searched, walls), the goal plane, the memory flag
as a constant plane. The fine tier takes two more: the coarse pass's prediction resampled onto the fine crop by
`CoarseFrame` -- the mirror of `MapEncoder.coarse_frame`'s grid (bilinear at the fine cells' centres and the ring
past the border; `grid_sample`, kept in one module so an export can swap it for a fixed interpolation matrix) -- its
log cost-to-go (over log1p(600)) and its reachability. Outputs per tier: the cost-to-go `[N, H, W]` yards, a
reachable logit, eight direction logits `[N, 8, H, W]` (NEIGHBOURS order), TRAV_CLASSES class logits.

- `SchoolPlanner` ("unet", the primary): two U-Nets of four levels (48 -> 24 -> 12 -> 6; `widths` default
  `[32, 64, 96, 128]`), the coarse first; plain Conv2d + SiLU, max-pool down, nearest up, skips; 1,820,842
  parameters at the default widths. Its cost head predicts log(1 + cost): the walk (`networks.descend`) needs only the
  ordering, which log1p keeps; the body's neighbourhood, where a yard matters, is where log1p spends its
  resolution; the sentinel's cliff is the reachable head's job; cost / 100 would spend the same gradient on a 300-yd
  cell's 10 % as on a 3-yd cell's 1000 %.
- `VinPlanner` ("vin", the comparison): `networks.Planner` -- the value iteration network the map encoder carries --
  over the same planes plus a tier one-hot, `vin_k` iterations (default 40), one network for both tiers, the
  hierarchy as `MapEncoder.plan` runs it (the coarse value the fine tier's start and held ring, bounded at 0); cost
  = -value x 100 yards; directions the cost field's own; a 1 x 1 reachable head, a two-layer class head; 10,641
  parameters. The goal's cells are held at 0 every iteration (`AnchoredPlanner`) in place of the encoder's bound
  (value at or below 0): under the bound the smoke's VIN never moved -- at its start the value is 0 nearly
  everywhere and a value held at the clamp has no gradient; held at the goal, unclamped, it learns from the first
  update.

## The trainer (`train.py`) and its losses

`python -m animus.school.train --config configs/school.yaml [--set key=value ...] [--eval-only] [--checkpoint
<run>/best.pt]`. Per tier: Huber on the cost over the label's reachable cells (log1p space for the U-Net, yards / 100
for the VIN); BCE of the reachable logit over every cell; the direction cross-entropy over the graded cells (the
multi-hot logsumexp form; a cell the crop does not know weighted `unseen_weight`); the class cross-entropy over the
labelled cells, class-balanced (inverse frequency clipped at `balance_clip`) with the unseen weight again. AdamW,
gradient clip. Each epoch: the validation and held-out splits measured for the model and both baselines, a row to
`<out_dir>/<name>/metrics.csv` and the table to stdout, `latest.pt` and `best.pt` (validation `dir_acc_body`), the
pass mark last. `--eval-only` measures a checkpoint (or a fresh model) and stops.

## The metrics and the baselines (`metrics.py`)

Baselines, on the same batches: `dijkstra_*` -- `networks.classical_cost_to_go` over the crop's own codes, priced by
the "map" rule with no costmap head (`classical_factor` with the head's guess fixed at "unknown": unknown 1.5, floor
and door 1, wall 100, hazard 5 a yard, a known floor off the height band as unknown), the hierarchy as the encoder
runs it, to convergence; the learner's 2-yd crop plan, not the sim's 1-yd configuration-space plan (the recorded
`route` scalars are the sim's, kept for a third comparison later). `oracle_*` -- the same over the label's true
classes (unknown at 100).

| Metric | Definition |
|---|---|
| `dir_acc_body` | at the anchor cell, the plan's argmax direction (first of equals) is one of the label's downhill neighbours; rows whose anchor is graded. `_seen` / `_unseen` by `body_unseen`; `_mem_on` / `_mem_off` by the memory flag; `_coarse` the coarse tier's |
| `dir_acc_cells` | the same over every graded fine cell; `_seen` / `_unseen` by the cell's own crop code; `_mem_on` / `_mem_off` by the row |
| `cost_mae_known`, `cost_mae_unseen` | the fine cost-to-go's absolute error (yards) over reachable cells the crop knows / does not, counting cells the plan prices under `CLASSICAL_OPEN_CEILING` (600: above it the plan went through a wall; the trainer's rule); `cost_blocked_known` the share above it |
| `waypoint_regret_yards` | `networks.descend` 10 cells over the plan's fine cost from the anchor; the label's cost-to-go at the cell reached less the least label cost-to-go over the cells reachable from the anchor in 10 steps through the label's reachable cells (a masked dilation, so no wall is stepped through); rows whose anchor the label reaches. A walk ending on a cell the label cannot reach pays 100 yd and counts in `waypoint_unreachable_share`. `_seen` / `_unseen` / `_mem_on` / `_mem_off` as the body's |
| `reach_acc`, `cls_acc_known`, `cls_acc_unseen` | the model's own heads (not the baselines') |

Pass mark (printed; `pass_mark`): on the held-out split, the model's `dir_acc_body_unseen` at least 10 points over
Dijkstra's and its `dir_acc_body_seen` no more than 2 points under.

## The synthetic smoke (`synthetic.py`)

`python -m animus.school.synthetic --out <dir> --episodes 1000 --per-episode 4`: random axis-aligned rooms and 2-4
cell corridors on a 128 x 128 world of 2-yd cells, water pools and hazard patches, a body and a goal, the true
cost-to-go by the oracle's Dijkstra over the WHOLE world (so routes leave the crop through cells never seen), the
view by line of sight within a random radius plus, with the memory flag on, whole rooms remembered; the heading
always north, the height byte flat, the route scalars zero; maps 34 / 389 / 43 differ in the id alone. 4,000 samples
take 42 s on the dev container's CPU. **The numbers are synthetic** -- what they show is that the pipeline runs end
to end and learns, not what the school will do on dungeon data.

Smoke of 2026-10-17 (`--episodes 1000 --per-episode 4`, seed 7: 4,000 samples, 3,204 train / 796 held out (map 43)
/ the rest validation; `batch 32`, 12 CPU threads; the U-Net 20 epochs in 282 s, the VIN 8 epochs in 108 s; the
held-out split, 796 body rows of which 234 unseen):

| Metric (held out) | U-Net | VIN | Dijkstra (crop) | Oracle (truth) |
|---|---|---|---|---|
| `dir_acc_body` | 0.725 | 0.455 | 0.788 | 0.822 |
| `dir_acc_body_seen` | 0.751 | 0.468 | 0.802 | 0.790 |
| `dir_acc_body_unseen` | 0.662 | 0.423 | 0.752 | 0.897 |
| `dir_acc_body_mem_on` / `_off` | 0.726 / 0.723 | 0.426 / 0.487 | 0.805 / 0.769 | 0.824 / 0.819 |
| `dir_acc_cells` (seen / unseen) | 0.643 (0.719 / 0.571) | 0.442 (0.467 / 0.419) | 0.673 (0.758 / 0.592) | 0.787 (0.819 / 0.758) |
| `dir_acc_body_coarse` | 0.655 | 0.536 | 0.655 | 0.868 |
| `cost_mae_known` / `_unseen` (yd) | 36.5 / 33.8 | 39.9 / 38.2 | 32.4 / 35.7 | 40.3 / 38.4 |
| `waypoint_regret_yards` | 18.0 | 14.1 | 26.0 | 2.6 |
| `waypoint_unreachable_share` | 0.001 | 0.000 | 0.229 | 0.000 |
| model heads: `reach_acc`, `cls_acc_known`, `cls_acc_unseen` | 0.89, 0.98, 0.85 | 0.88, 0.99, 0.86 | | |

Pass mark: not passed (U-Net -9.0 / -5.2 points against Dijkstra at the body, unseen / seen; the VIN -33 / -34), and
not expected to be on 3,200 synthetic samples and 20 CPU epochs: the U-Net's loss was still falling (3.36 -> 1.63)
and its body accuracy rose every epoch (0.19 -> 0.73). What the smoke establishes: every stage runs (recorder format
-> shards -> dataset -> both models -> losses -> metrics -> both baselines -> CSV, checkpoints, the pass mark); the
U-Net already beats Dijkstra on the waypoint's regret (18 against 26 yd) and never walks into a wall (0.1 % against
23 %: the reachable head gates the walk) while Dijkstra's optimism prices unknown ground at 1.5; the VIN learns once
anchored. Earlier, the first smoke's U-Net at 12 epochs read 0.607 / 0.690 (unseen / seen) -- the runs are on
`/jobtmp/school-runs/{unet,vin}/metrics.csv` of the job, not kept.

## Running on real data

1. In the nav stage's yaml (nav2_detour on): `school: {record: true}` (defaults: every 16 decisions an env, shards
   of 4096, 500k samples at most, `var/animus-forge/shared/school/<run_name>/`). Every rank records its own shards.
   At 192 envs x 1 agent and 16 decisions, a learner samples 12 a decision: at ~40 decisions/s that is ~500
   samples/s, 500k in about twenty minutes of rollout time per learner -- the limit, not the run, decides; spread it
   over a long run with `record_every: 128` (~60 samples/s, about two hours) so the shards span the stage's rungs
   and both memory arms. Wailing Caverns is recorded (nav2's `wailing` arena) and held out by the dataset.
2. From `apps/forge/python`: `python -m animus.school.train --config configs/school.yaml` (the yaml's paths are
   relative to that directory; the default `data_dir` is nav2_detour's;
   on dev's card `device: auto`; on a CPU a 4,000-sample epoch of the U-Net is ~11 s at 12 threads, so 500k is
   ~25 min an epoch).
3. Read `metrics.csv` / the table: the pass mark, `cost_mae_unseen`, `waypoint_regret_yards_unseen` against
   `dijkstra_*`, and `_mem_on` / `_mem_off` apart.

## Stage 2: the school as the policy's route source (`mappo.planner_waypoint_source: school`, 2026-10-17)

The learner computes the seventeen route scalars (`networks.ROUTE_COLUMNS`) from the school U-Net's fields instead of
reading the sim's Dijkstra off the observation: the same columns, join and gating, so the policy's input does not
change. The code is the map encoder's (`networks.MapEncoder.school_route`, `route_from_fields`, `load_school`;
[py-mappo-networks.md](py-mappo-networks.md) "The school planner as the route's source"); the keys
`mappo.school_checkpoint` (a `best.pt` / `latest.pt` of this trainer, loaded frozen and saved with the policy),
`mappo.school_every` (1: every decision) and `mappo.school_finetune` (parsed, NOT implemented, true refused)
([config-yaml.md](config-yaml.md)); the export .amdl version 20 ([file-formats.md](file-formats.md)).

- **The goal at act time.** The seventeen cannot place the goal cell (route_dist is a path cost, the coarse waypoint
  a 16-yd point), and the labels do not reach the actor, so the sim has to write two more map-block scalars,
  `goal_forward_yards` and `goal_right_yards` (`networks.GOAL_COLUMNS`), immediately before the route's seventeen:
  the objective's yards in the body frame, the labels' convention (`MapBlock::Observe` computes exactly these for
  `PlanOnMap`; 0 and 0 with no objective or one withheld). **Not built in C++ yet**: a stage whose block lacks them
  refuses the source. They feed the school's goal plane and nothing else -- the embedding never reads them (the
  block's prefix shrinks by two whatever the source) and the VIN's goal plane reads the compass alone -- but they ARE
  in the observation vector; the policy sees the straight line to the goal only as the school's route.
- **The memory flag** is the block's `kept` scalar at act time (the recorder's `kept` field), not the shard's
  `memory` label (the episode's shared_memory column, which no observation carries): a school meant for a
  shared-memory stage should be trained with `kept` as its flag, or the flag's meaning differs between the school's
  training and its use.
- **The rule on 2-yd cells** (`networks.ROUTE_POINT_YARDS` and its comment): the fine walk `networks.descend`'s
  rule over the fine cost with every cell the reachable head rejects at the sentinel (`metrics.walk_field`'s gate),
  from the least of the centre four cells (the body lies on their corner; the sim starts on the body's own 1-yd
  cell), 15 moves at most (a move a cell, as the sim's 1-yd steps); the point at Y yards is the polyline of visited
  cell centres at Y / 2 moves -- 2.5, 5, 10, 15, the 5-yd point the midpoint of the cells after moves 2 and 3, a
  stopped walk repeating its end -- as bearings from the body; reach = moves x 2 / 30; the coarse waypoint 2 moves
  from the anchor on the gated coarse field; route_dist the least gated fine cost over the centre four, clamped at
  300, over 300; route_stall the sim's column as it is (the bot's own clock). The 1-yd offset of a 2-yd cell's centre
  from the body is in the bearings (a straight walk up column 23 reads right -1 yd).
- **Where and how often.** The school runs at act time alone, in `MappoTrainer._replan` (eager, outside the rollout
  graph) for every seat every `school_every` decisions, and its seventeen are the cached plan's waypoint slice, which
  the decision reads as it is and the rollout buffer records: the update's replay reads the recorded slice and never
  runs the U-Net (the sim source keeps overwriting the slice from the observation at every site). Measured on the
  dev container's CPU at 8 threads (2026-10-17): 1.18 GFLOP a row for both tiers, 143 ms per 64 rows, 532 ms per 192
  -- too slow for a CPU rollout every decision (a learner's decision is ~24 ms); on gfx1100 in float32 an estimated
  10-25 ms per 192 rows (227 GFLOP at an effective 10-20 TFLOPS), a large share of the decision. A held route
  (`school_every` > 1) goes stale as the body moves -- the fields are egocentric and the observation carries no pose
  delta to re-register them, and a running body crosses a fine cell a decision -- so the default is every decision;
  the knob is for a card that cannot afford it, and narrower widths or half precision are the better savings.
- **The arm `no_school`** (`eval.arms`): the same seeds on the sim's route scalars (`MapEncoder.school_off`), so the
  gap to the plain evaluation is what the school's route is worth like for like; `no_planner` still zeroes all
  seventeen. Skipped with a line on a policy whose route is not the school's.
- **Export** (.amdl 20): the school's weights, class table, widths and the rule's constants travel with the policy;
  the C++ runtime (not built) computes the fields and the seventeen itself from the objective it knows.

Smoke of 2026-10-17 (the dev container's CPU, a synthetic revision-7 stage with the goal columns inserted before
nav2's seventeen, the synthetic smoke's `best.pt`, seeded from nav1_sight): the rule's walks on hand-made fields read
as documented; the five config refusals and the missing-goal-columns refusal read as expected; the frozen school's 61
tensors are in the actor's state and in no optimizer; seeding and both load paths leave it alone; a 16-decision
rollout read finite seventeens whose stall equals the sim's, whose gates hold and whose no-goal rows are zero; the
`no_school` arm restored the sim's gated columns and `no_planner` zeroed all 35; one full update was finite and left
the school untouched while the join moved; the export round-tripped every school tensor bit for bit (13.6 MB a
layout) and a sim-route actor still exports as 19.

## Open questions

- The oracle's `dir_acc_body_seen` reads below Dijkstra's on the synthetic world: the hierarchy's 8-yd coarse ring
  (a coarse cell of majority wall seals a 2-cell corridor at 100 a yard) steers the oracle's fine plan, where
  Dijkstra's unknown coarse cells at 1.5 read as open ground. The same mechanism is the shared memory's 2-3 x route
  on the 2-yd crop that the sim's 1-yd plan fixed; on real data the coarse labels come from the bake's 8-yd fold.
- The real labels' coarse cost-to-go is the sim's own fold; the synthetic takes the least over the 16 world cells.
- A third baseline off the recorded `route` scalars (the sim's 1-yd plan) is a comparison at the body only
  (bearings, not a field); not built.
- Stage 2: the two goal columns in the map block (C++, `MapBlock`), and whether the school U-Net's cost on the rollout
  card is acceptable every decision at 192 seats, are both open; the memory flag's two meanings (above) too.
