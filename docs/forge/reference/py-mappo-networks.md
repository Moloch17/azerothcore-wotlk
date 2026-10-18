# `mappo/networks.py`: every class, in file order

Purpose and scope. A unit-by-unit reference for `apps/forge/python/animus/mappo/networks.py` (2382 lines), the only file
that defines the policy and critic networks. Parameter lists and shape chains for the live M2 stage are in
[py-mappo.md](py-mappo.md); the training use is in [py-mappo-trainer.md](py-mappo-trainer.md). There are no tests (removed 2026-10-07; see
[tests.md](tests.md)).

## Map table (one file)

| Path | Lines | Role |
|---|---|---|
| `apps/forge/python/animus/mappo/networks.py` | 2382 | the networks and the stage.json readers; see the unit index below |

Unit index (line of the definition):

| Unit | Line | | Unit | Line |
|---|---|---|---|---|
| `masked_logits`, `log_prob_of`, `_entropy`, `sample_logits` | 41-63 | | `VisionEncoder` | 1541 |
| `RunningNorm`, `fold_into` | 75, 158 | | `VisionJoin` | 1732 |
| `DenseLayouts`, `SharedInputDense` | 169, 204 | | `LookHead`, `look_hold_indices` | 1763, 1755 |
| `_Trunk` | 234 | | `_attach_vision`, `share_vision`, `vision_term` | 1821-1856 |
| grouping helpers (`per_layout`, ...) | 257-301 | | `GoalEmbedding`, `goal_pair`, `split_goal_pair` | 1868, 1960, 1965 |
| `_carry_sequence(_loop)`, `_pieces` | 304-379 | | `load_actor_state`, `without_blind_columns` | 1943, 1936 |
| `GoalHead`, `_Factored` | 382, 591 | | `LayoutActor` | 1971 |
| `LayoutCritic` | 2262 |
| `attach_blind_columns`, `clear_blind_columns` | 800, 821 | | stage.json readers `_map_of`, `_entities_of`, `_sight_of`, `vision_of` | 878-1056 |
| `MapValueIteration`, `MapEncoder` | 1164, 1208 | | decoders `decode_image/slots/map` | 1110-1152 |
| `VisibleEntities`, `SightEntities`, `SightPointers` | 1288, 1363, 1531 | | checks `check_look_heads`, `check_image_bytes` | 1080, 1089 |

## Sampling and distribution helpers (`networks.py:31-72`)

- `MASKED_LOGIT = -1e9` (`:31`). `masked_logits(logits, mask)` (`:41`) fills disallowed actions with it; a row with
  nothing allowed is given action 0 (chosen on the device with `torch.where`, no host read). This is the only mask the
  network applies ("nothing is masked except the physically impossible", [principles](../principles.md) 5; the mask
  itself comes from the sim).
- `log_prob_of`, `_entropy`: log-softmax gather and entropy computed directly.
- `sample_logits(logits, deterministic, greedy=None)` (`:63`): Gumbel-max draw `argmax(logits - log(-log U))`; `U` in [0,1) so a
  masked logit cannot win. Returns `(choice, log_prob)`. `greedy` (bool `[N]`, exploration v3): those rows take `logits.argmax` through a
  `torch.where` AFTER the draw is made for every row, so the device generator consumes the same numbers whatever the mask; ignored with
  `deterministic`. `LookHead.sample(..., greedy)` and `LayoutActor.look(..., greedy)` pass it on, so a greedy seat's camera is argmax too. Used by the graph path and the goal head; the eager path
  uses `Categorical.sample()` (`trainer.py:1066`), a different random stream.
- `skip_distribution_checks()` (`:248`) disables torch's distribution validation globally; called in
  `MappoTrainer.__init__` (`trainer.py:614`).

## `RunningNorm` (`:75`)

Per-feature running mean/var of observations, one per layout per network (plus `state_norm` in the critic).
Buffers `mean [D]`, `var [D]`, `count []` (persistent), `hidden_columns` (non-persistent, built from the stage).
`forward` returns `where(count > 0, (x-mean)/sqrt(var+1e-5), x)`: raw before anything has been seen (`:146`).
`update(rows, ranks)` is Chan's parallel update; with data-parallel ranks all ranks must call it for every layout
(`:115`, `:258 update_norms`). `hide(columns)` pins columns at mean 0 / var 1 forever: used for the camera's and
sight list's columns, which the adapters never read (`_attach_vision` `:1838`). `bypass=True` (set by `fold_into`) makes
`forward` the identity; `fold_into(norm, linear)` (`:158`) rewrites `W' = W/sigma`, `b' = b - W(mu/sigma)`.
Used only on the rollout copies (`fold_normalisation`, `trainer.py:852`), which are re-synced and re-folded after every
update. Invariant: the statistics are updated only **after** an update's epochs (`trainer.py:2013`), so the PPO ratio at
epoch 0 is 1.
Reviewer notes: `scale()` reads the count back to the host (`float(self.count)`, `:152`); it is only called by
`fold_into` at sync time, which is acceptable. The `if count > 0` select means a brand-new network sees raw features.

## `DenseLayouts` and `SharedInputDense` (`:169`, `:204`)

Rollout-copy-only (GPU) speedups: all layouts' adapters (or heads) as one wide matmul `[in_width, layouts*out]` and a
per-row `gather` of the row's own layout slice (`DenseLayouts.__call__` `:198`). `SharedInputDense` stacks the actor's
and critic's dense adapters into one transposed product (`:204`). Both are refreshed **in place** (`refresh`) because a
captured graph holds their tensor addresses (`:190`). `densify` is called from `MappoTrainer._sync_rollout`
(`trainer.py:854`) only when `rollout_device` is CUDA. Never used in training.
Quirks: `DenseLayouts` assumes padding columns are zero; a layout's slice is exactly its own layer only for that
reason (`:171`). Heads are padded to `max_actions` with `-1e9` (`action_logits :2170`).

## `_Trunk` (`:234`)

`tanh`, then for each pair in `hidden`: `Linear` + `tanh`; orthogonal init gain sqrt(2), zero bias (`_linear :34`).
For `hidden [256, 512, 512]` this is `Linear 256->512`, `Linear 512->512`. The adapters produce `hidden[0]` and the
first `tanh` is applied by the trunk, so adapters are linear.

## GRU helpers: `_carry_sequence_loop`, `_pieces`, `_carry_sequence` (`:304-379`)

`_carry_sequence(cell, size, encoded [T,N,H], memory [N,R], dones [T,N]) -> [T,N,R]`. On CPU (or `T == 0`) it calls
`_carry_sequence_loop` (a Python loop of cell steps, zeroing the state after a done, `:304`). On CUDA/HIP it cuts every
row at episode ends into pieces (`_pieces :317`, computed on the CPU from `dones`), packs them with
`pack_padded_sequence` and runs `torch._VF.gru` with the cell's own weights (`:373`), then un-packs back to `[T,N]`.
The only place the learner touches a private torch API (`torch._VF.gru`). The positional arguments
`(…, True, 1, 0.0, cell.training, False)` are `has_biases, num_layers, dropout, train, bidirectional`.
Reviewer notes: torch upgrades can break `_VF.gru`'s signature; the loop is the reference. Pieces start from the carried
memory only for the row's first piece (`first = piece_start == 0`, `:366`).

## `GoalHead` (`:382`)

Goals as `kind * targets + target` (one integer on the wire and in buffers). Logits are
`kind(features)[:, :, None] + pair[None] (+ target(features)[:, None, :])`, a joint categorical parametrised in parts.
Constructor args `(width, kinds, targets, layout_count, lookahead, slots)`. Parameters: `kind` Linear(width,kinds,gain
0.01), `target` Linear(width,targets) (only if targets > 1), `pair [kinds,targets]` (zeros); with `slots > 1`:
`slot_bias [slots-1,width]`, `drawn` Embedding(kinds*targets+1, width) zero-init (index 0 = none, goal g at g+1),
`none_bias [slots-1]`; with lookahead: `success`, `duration` (`_Factored`, `:591`) and `lookahead_weight [2]` init
`[1.0, -0.5]`. Buffers: `accepts [kinds,targets] bool` (which targets each kind accepts, set by `set_space`) and
`block_at [layouts] long` (-1 = layout has no goal block). `has_space` is a host bool copy of `any(block_at >= 0)` so a
captured graph need not read the device (`:454`); it is recomputed in `_load_from_state_dict` (`:427`).

**The pointer over place slots** (m2-goals W3a, 2026-10-09). `pointer` = `Sequential(Linear(5, 16), Tanh, Linear(16, 1))`
(`PLACE_FEATURES` 5, `POINTER_HIDDEN` 16; only when `targets > 1`), the first layer orthogonal, **the last layer's weight
and bias zero**, so a seeded head chooses exactly as before until it trains. Buffer `place_spec [4] long`
`(first feature column inside the goal block, first place target, slots, features)`, -1 throughout = no pointer, with host
copies `place_at / place_first / place_count` (`pointer_on`) mirrored in `set_space` and `_load_from_state_dict`, so a
captured graph never reads the device. `set_space(accepts, block_at, goals)` takes the manifest's `goals` object and reads
`columns.place_features` and `place_slots {first, count, features}` (`_place_spec_of`; nothing hard-coded; absent keys, a
goal block before revision 3, turn the pointer off and say so, a `features` other than 5 or a span past
`columns.width` raises). `place_scores` gathers each slot's 5 values from the goal block (rows without a goal block read
zeros), `_with_places` adds the MLP's score to each place target's logit for every kind alike (the kinds that cannot take a place are masked, which leaves `search` and `travel_to`), in `logits()` and
`slot_logits()` (so the primary, the secondary and the queue), before the goal block's target-there mask: an unbound or
checked slot is never chosen. No data-dependent Python branch, so the rollout graph captures it.

**The cell pointer** (free-choice-goals, 2026-10-09; the plan in `.agents/plans/free-choice-goals/`). A cell goal is the joint goal `search / target 21`
(joint 228 since goal block revision 5, 159 as `travel_to / 21` before and at `Seek.SearchGoals` 0; always `goals.cells.joint`)
plus a block of the mental map's crop: the 48 x 48 heading-up crop pooled `pool` 2 into 24 x 24 blocks of 4 yd, the
index row-major, 576 candidates. `CellPointer` (`goal_head.cell.*`): `local` Linear(21, 32) (SiLU in the head), `query`
Linear(width, 32), `score` Linear(32, 1) **zero weight and bias**, `chain` Sequential(Linear(3, 16), Tanh, Linear(16, 1))
with a zero last layer. Buffer `cell_spec [4] long` `(byte the crop starts at in a row's image bytes, grid, pool, joint)`, -1
throughout = no cell head, with host mirrors `cell_at / cell_grid / cell_pool / cell_joint` (`cell_on`), set by
`set_space(accepts, block_at, goals, image_offset, map_side)` from `goals.cells` (absent = no cell head, a revision-3
manifest; a manifest whose crop, codes, channels or field widths this learner does not implement raises) plus
`cell_rise`, `cell_min_floor` and `cell_from` (`goals.columns.from`, for the hindsight) as host numbers. Module functions:
`cell_valid(crops, grid, pool, rise, min_floor)` -- the contract's one definition of a choosable block (no Wall or Hazard
cell; at least 2 floor-like cells (Floor or Door); every floor-like cell with a known height within 16 units = 4 yd of the
feet; Unknown is never floor), a pure function of the crop bytes -- `cell_features` (22 per block: shares of
Floor/Door/Wall/Hazard/Unknown, visited, frontier, any entity, newest look age, never seen, mean height; the 3 x 3 average
of the floor, wall, unknown and frontier shares; forward, right and distance over 48; and, appended in exploration v3 so the first 18 keep
their index, the block's `searched` share (`MAP_SEARCHED` byte sum over 4 x pool x pool: the share of its 1-yd cells looked at this
episode) and that share averaged over the 3 x 3 and the 7 x 7 blocks round it, `avg_pool2d` with `count_include_pad=False`: fixed
shapes, graph-safe), and, appended in general search so the first 21 keep their index, `frontier_7x7`: the frontier share
averaged over the 7 x 7 blocks round it (the frontier mass of the 28-yd neighbourhood, the graph-safe stand-in for the size
of the frontier cluster a block belongs to; true components are data-dependent and not capturable) and `cell_geometry`. A slot's score
per block is `score(tanh(local(x_j) + query(shifted_s))) + chain([dr, dc, dist] / 24 from the previous drawn cell of this
choice, the seat's own block for the first)`, `shifted_s` the same vector `slot_logits` builds (`_shifted`). Masked by
`cell_valid` and, for a slot after the first, by every block within Chebyshev distance 1 of a cell drawn before it in the
choice (queue de-dup by cell); `masked_logits` falls back to block 0 on an empty row. The cell goal is also withdrawn from a
later slot's joint logits (`slot_logits(cell_ok=)`) where no block is left, so a plan never holds a cell the sim would find
invalid (an addition to the contract). Graph-safe: every slot's cell logits are computed every decision and selected with
`where`.

Public methods:

- `logits(features, obs, layout)` (`:560`): masked joint logits `[rows, kinds*targets]`; allowed = `accepts` AND (goal
  block's kinds-there x targets-there, only when `targets > 1`). A layout with no goal block gets all goals masked
  except index 0 when `has_space`. Goal 0 ("Fight about no one") is allowed **only when nothing else is**
  (`allowed[:, 0] |= ~allowed.any(-1)`, m2-goals): a no-op in every stage whose goal block always offers it, and in M2
  the placeholder once a place target is on offer is withdrawn so the head has to pick a place.
  With lookahead the predictions (detached) times `lookahead_weight` are added to the logits (`:566`).
- `signals(obs, layout)` (`:464`): reads the goal block's columns > 0.5: `ended`, `secondary_ended`, `event`,
  `achieved` (-1 none). With `slots <= 1` only `ended` is real.
- `cell_primary(features, image, block)`: the primary slot's cell distribution again -- `(log p(block), block choosable,
  entropy, any block choosable)` -- for the planner's hindsight and the update's statistics.
- `slot_logits(features, slot, drawn, obs, layout, cell_ok=None)` (`:494`): logits `[rows, count+1]` for a slot after the primary,
  last column = none; masked by the goal block like the primary; a row without a goal block has only none. From
  `slot >= 2` (the queue) the goals already drawn at this choice (primary, secondary, earlier queue entries) are masked
  too, so a queue never repeats the primary (which ended lost on promotion) -- except the cell joint, which every cell
  goal shares (the draw de-duplicates the cells by block instead; the sim's `secondary == primary` drop and
  `decide_goals`' likewise skip it).
- `draw(features, obs, layout, deterministic, slots=None, uniform=False, cells=None, image=None, uniform_cells=False,
  single=False)`: `uniform` (the eval arm `random_goal`, `decide_goals(..., uniform=True)`) draws the primary uniformly
  over the goals on offer, the cell uniformly over the choosable blocks, and holds nothing beside it; `uniform_cells`
  (`random_cell`) only the cells uniform; `single` (`no_plan`) nothing held beside the primary or queued; never set by
  training. Draws (or, with `slots` and `cells`, scores) primary then secondary/queue slots, each conditioned on the earlier
  ones; with the cell head and `image` a slot whose joint is the cell joint also draws a block (log probability: the joint
  plus the block; entropy: plus `p(cell joint) * cell_entropy_weight * H(block)` inside the slot's weight). Returns `(slots
  [rows,S] with -1 for none, log_prob, entropy, cells [rows,S] with -1 where the slot is not a cell goal)`. The primary's entropy counts fully; later slots are weighted by `slot_entropy_weight`
  (default 0.1, overwritten from `MappoConfig.goal_slot_entropy_weight` by `trainer.py:665`, not by the constructor).
  (The order-given primary path was removed with the goal block's order columns, goal block revision 2.)
- `predictions(features)` (`:555`): `(success logits, sigmoid(duration))` per candidate goal.
- `set_space(accepts, block_at, goals=None)` (`:448`; returns the pointer's log line), `block_width` (`:433`),
  `columns` (`:442`): goal block layout (the revision-3 columns, 68..107, are read from the manifest, not here)
  (secondary_ended
  = base, event, achieved_kind, achieved_target with base = kinds+targets+2; `block_width` = base + 2 + kinds +
  targets with slots > 1, goal block revision 2).
- `ended(obs, layout)` (`:583`): **no caller anywhere**; dead.

Data flow: built in `LayoutActor.__init__` (`:2021`) with width = `slow_size or head_width`. Called from the rollout
(`decide_goals`) and the update (`_update_goals`, and the fast-loop `goal_distribution` only when there is no slow
loop).
Contract: the goal block's column positions must equal `GoalBlock::Obs` in the sim; there is no checksum other than
`set_goal_space`'s kinds/targets count check ([cpp-blocks.md](cpp-blocks.md)).

## Blind columns: `attach_blind_columns`, `clear_blind_columns`, `without_blind_columns` (`:800-829`, `:1936`)

A layout's adapter must not read some columns (camera scalars, entity-list slots, map scalars, sight slots) because dedicated encoders read them. `attach_blind_columns(network, columns, tag)` registers a buffer
`<tag>_keep_<index> [1, obs_i]` (1 = keep, 0 = blind), multiplies the adapter weight by it and registers a gradient hook
that multiplies the gradient by it, so those weight columns stay exactly zero through every update. Tags: `"set"` and
`"vision"` (`BLIND_KEEP_PREFIXES` `:1933` also lists the retired `"blind_keep_"`). `clear_blind_columns(network)`
(`:821`)
re-zeroes after a seed or load. The keep buffers are **saved** in checkpoints (they are persistent buffers) but **dropped on load** (`without_blind_columns`, `:1936`; `load_actor_state` `:1943`;
`MappoTrainer.load_state_dict` `trainer.py:2039`): the network's own masks, built from the current stage, win.
Resume safety check: `MappoTrainer.camera_columns_clear()` (`trainer.py:825`) fails the resume if any adapter weight
is non-zero at a blind column (`train.py:735`).
Quirk: the gradient hook captures the network and looks the buffer up by name (`:816`); a deepcopy of the network (the
rollout copy) carries its own copy of the hook closure's `network` argument default, which is the original network. The
masks are identical, so the result is the same, but it is a trap if masks ever differ between copies. UNVERIFIED:
whether the deepcopied hook binds to the copy or the original (default-argument binding makes it the original);
irrelevant for the rollout copies, which never backpropagate.

## stage.json readers (`:843-1107`)

Constants: `VISION_BLOCK="vision"`, `ENTITIES_BLOCK="entities"`, `SIGHT_BLOCK="sight"`, `MAP_BLOCK="map"`, `COVERAGE_BLOCK="coverage"`
(`COVERAGE_LAYOUT="row_col_channel"`),
`IMAGE_BYTES_PER_PIXEL=4`, `IMAGE_CHANNELS=5`, `IMAGE_CLASS_CHANNEL=3`, `CLASS_MASK=0x1F`,
`CLASS_LIMIT=32`, `DEFAULT_PATCH=4`, `MAP_CHANNELS=8` (7 before map block revision 6, 6 before exploration v3; `MAP_SEARCHED=6`, `MAP_SEARCHED_MAX=4`, `MAP_WALLS=7`, `MAP_WALLS_MAX=4`, `MAP_VALUE_PLANES=7`, `MAP_BLOCK_REVISION=7`), `MAP_CODES=5`, `MAP_HEIGHT_ZERO=128`, `ROUTE_COLUMNS` (the sim's seventeen route scalars; `ROUTE_GROUPS` their valid gates), `WAYPOINT_SOURCES=("learned", "classical", "sim")`.
**The walls channel and the sim's waypoint (map block revision 6, 2026-10-17; the memory investigation: the 2-yd crop's any-wall-sub-cell rule sealed corridors, so the map's route read 2-3 x the true one).** The crop has an eighth channel, `walls` (0..4: how many of the crop cell's four 1-yd cells are wall; the coarse tier's count scaled to the same range), and its code byte calls a cell `wall` by majority (3 or 4 of 4) alone. `decode_map` returns `values` `[N, H, W, 7]` (`walls / 4` last); `MapEncoder.planes_per_cell` is `CODE_EMBED + class embedding + 7` (17), so the patch weight is `[64, 16 x 17]`, the costmap head's conv1 reads 17 + 8 (+ the splat's 9) channels and the planner's fixed conv 11 + 17 + 1. A seven-channel checkpoint (nav1's) seeds through `grow_map_planes` (the new plane's inputs at zero, inserted where the plane sits: the end of each patch cell's group, after the old planes in conv1, after the costmap's classes and the old planes in fixed), run before `grow_costmap_inputs`' splat padding, in both `bootstrap._seed_map` and `load_actor_state`; the encoder computes exactly what it did. `_map_of` refuses a map block before revision 6 or a crop that is not 8 channels, and cross-checks `walls_channel` / `walls_max` when the manifest names them. The map block also gained eight scalars at its end, `ROUTE_COLUMNS` (`route_fine_sin, _cos, _dist, _valid, route_coarse_sin, _cos, _dist, _valid`: the sim's route on the bot's own map, planned at 1 yd every decision, the walk's numbers a tier -- sin = -right / d, cos = forward / d, dist over the reach of 10 / 16 yd, valid; all 0 without a waypoint), located by `route_of` from the block's `obs_names` or its manifest's `scalar_names` and required to be the block's last eight; `MapEncoder.scalars` (the embedding's input) is the block's count LESS those eight (`embed_scalars`, 15 as before), whatever the waypoint's source, so the policy sees the sim's route only through `waypoint_join` under `mappo.planner_waypoint_source: sim` (`MapEncoder.sim_waypoints`: the eight columns gathered per layout, each tier times its valid flag; `plan()` reads them in place of a walk, `forward()` overwrites the cached plan's waypoint slice with the decision's own observation every decision, in the rollout, the eval and the update's replay; `planner_off` zeroes them; the `waypoint_source` buffer reads 2).
**The seventeen route scalars and no compass (map block revision 7, 2026-10-17).** The map block's route scalars are now the LAST SEVENTEEN of the block, `ROUTE_COLUMNS` in this order: `route_p5_sin, route_p5_cos, route_p10_sin, route_p10_cos, route_p20_sin, route_p20_cos, route_p30_sin, route_p30_cos` (the fine 1-yd plan's points at 5/10/20/30 yd as bearing sin = -right / d, cos = forward / d; a later point repeats the walk's end where it ended early), `route_fine_reach` (yards walked / 30), `route_fine_valid`, `route_coarse_sin, route_coarse_cos, route_coarse_dist, route_coarse_valid` (as revision 6), `route_dist` (min(own-map cost-to-go at the body, 300) / 300), `route_dist_valid`, `route_stall` (seconds since that cost-to-go last set a new episode minimum, / 30, clamped to 1). `route_of` requires all seventeen as the block's last seventeen (revision 6's eight are "some of the names": refused); `_map_of` refuses a map block before revision 7; `embed_scalars` is the block's count less seventeen (15 as before). `MapEncoder.waypoint_width` is 17 under `mappo.planner_waypoint_source: sim` (else the walk's `tiers x WAYPOINT_FEATURES` = 8), so `readout_width` is 18 + 17 = 35 and `waypoint_join` is `Linear(17 -> embed)`, a shape of its own: `bootstrap._seed_map` and `load_actor_state` leave a checkpoint's eight-wide join behind (fresh at zero, a printed line), as they do a missing one, so a nav1 or a pre-revision-7 nav2 checkpoint seeds and the policy computes what it did. `sim_waypoints` returns all seventeen, each gated by its group's valid flag (`ROUTE_GROUPS`: the first ten by `route_fine_valid`, the coarse four by `route_coarse_valid`, `route_dist` by `route_dist_valid`; `route_dist_valid` and `route_stall` as they are), times 0 for a layout without the columns. The `no_planner` arm (`planner_off`) zeroes all 35.

The nav stages carry NO compass block since the same change (nothing in the observation is the straight-line bearing or distance to the goal; the goal block's held-goal direction / distance columns, where a nav layout has them, carry the route's fine waypoint bearing and the route distance). Every reader of the compass takes its absence as "no goal in the observation": `compass_of` returns None, `MapEncoder.compass` holds -1 and `goal()` says absent, the export's header writes 0xFFFFFFFF, `evaluation.trace_columns` resolves the `objective*` trace fields to -1 (NaN rows), `arrived_at_rung` is not computed, and `Run.evaluate_arms` skips a block ablation whose block no layout has (`evaluation.ABLATION_BLOCKS`: `no_compass` prints "skipped" instead of refusing). The learned planner's goal plane comes from the LABELS in the trainer's aux pass: `_aux_pass` decodes the labels' `goal_forward` / `goal_right` yards (`LABEL_SCALAR_GOAL_FORWARD/RIGHT`; present where either is not 0, `WriteLabels` memsets them to 0 without a goal) and passes them as `label_goal` through `VisionEncoder.forward` -> `MapEncoder.forward` -> `plan`, so the costmap head, the VIN and the classical measurement (`_classical_directions` reads the same `plan["goal_plane"]`) keep training and being measured against the route to the real goal. At act time nothing carries the goal, so on a map whose layouts all lack a compass under the sim's route (`MapEncoder.goal_blind`) `plan()` runs no costmap head and no value iteration: the readouts' VIN slice (18) and the pointer's pool are zero and the seventeen route scalars are the plan (the exported runtime must do the same: waypoint source 2 with every compass column none means zero readouts and a zero pointer bonus). A layout with a compass (M1) plans off it at act time as before; a withheld compass is a zero goal plane as before.

**The school planner as the route's source (planner school stage 2, 2026-10-17; `mappo.planner_waypoint_source: school`, `mappo.school_checkpoint`, `mappo.school_every`; `WAYPOINT_SOURCES` gains `"school"`, `ROUTE_SOURCES = ("sim", "school")`).** The same seventeen route scalars, join (`waypoint_join` 17 wide) and gating, computed by the learner from the planner school's U-Net (`school.model.SchoolPlanner`; docs [school.md](school.md)) instead of read off the sim's Dijkstra. `load_school` loads the checkpoint `animus.school.train` wrote as the frozen submodule `MapEncoder.school` (eval mode, no parameter requires a gradient, left out of `vision_parameters()` and `_actor_parameters()`, saved under `vision.map.school.*` = `SCHOOL_PREFIX` in the policy's checkpoint; refused: no file, a `vin` school, another protocol, another crop than 48 cells of (2, 8) yd). **The goal at act time**: the map block carries two new scalars, `GOAL_COLUMNS = ("goal_forward_yards", "goal_right_yards")`, the objective's yards in the body frame (the labels' convention; 0/0 without an objective or withheld), which `goal_of` locates by name and requires to be the two columns immediately before the route's seventeen (`embed_scalars` is the block's count less 19: the embedding never reads them, whatever the source; the VIN's goal plane still reads the compass alone, so on a nav stage the straight line to the goal reaches the policy only as the school's route); a block without them refuses the school source. The memory flag is the block's `kept` scalar (`KEPT_COLUMN`, `kept_of`; 0 without). `MapEncoder.school_route` builds the hierarchy's goal planes (fine unclamped, coarse clamped, as `school.data.prepare`), runs the U-Net on the decoded crops under no_grad outside autocast, and `route_from_fields` turns the fields into the seventeen (**the rule on 2-yd cells**, `ROUTE_POINT_YARDS` and its comment: every cell whose reachable logit is at or below 0 at `CLASSICAL_SENTINEL` (`school.metrics.walk_field`'s gate); the fine walk `descend_path` from the least of the centre four (row-major first of equals) for 15 moves at most, a move a cell as the sim's 1-yd steps are; the point at Y yards the polyline of visited cell centres at Y / 2 moves -- 2.5, 5, 10, 15, the 5-yd point the midpoint of the cells after moves 2 and 3, a stopped walk repeating its end; bearings from the body (the crop's centre); reach = moves x 2 / 30; the coarse waypoint 2 moves from the anchor on the gated coarse field, dist over 16; `route_dist` the least gated fine cost over the centre four, clamped 300 / 300, valid where one is reachable; `route_stall` the sim's column as it is). `route_scalars` chooses the school's or, while `school_off` (the eval arm `no_school`, `MappoTrainer.set_school_off`), the sim's columns; the sim's seventeen are in `plan_out["sim_route"]` either way. **Where it runs**: at act time alone, in `plan()`'s goal-blind branch, which `MappoTrainer._replan` calls for every seat every `school_every` decisions (the plan's clock under the school in place of `planner_every`; eager, outside the rollout graph); the seventeen are the cached plan's waypoint slice, which `forward()` reads AS IT IS under the school (under `sim` it keeps overwriting the slice with the observation's) -- the rollout, the evaluation and the update's replay all read the slice the decision was made with (the rollout buffer records it) and the U-Net never runs on the update's rows; the aux pass's `plan(label_goal=...)` keeps the sim's columns in the waypoint slice. Cost: 1.18 GFLOP a row for both tiers at widths 32-128; 143 ms per 64 rows and 532 ms per 192 on the dev container's CPU at 8 threads (2026-10-17), an estimated 10-25 ms per 192 rows on gfx1100 in float32 against the ~24 ms decision; a held route (`school_every` > 1) goes stale as the body moves (the fields are egocentric and the observation carries no pose delta), so the default is every decision. `load_actor_state` tolerates a checkpoint without the school's tensors (the file's stand) and drops them into an actor without one (a printed line), `bootstrap._seed_map` never carries or counts them, and a planner-schedule buffer both the checkpoint and the actor have now reads the CONFIGURED value (the docstring's rule, which the plain copy broke: a sim-route checkpoint resumed under the school is the school's). The `waypoint_source` buffer reads 3; export .amdl version 20 ([file-formats.md](file-formats.md)).

**Searched state (exploration v3, map block revision 2, protocol 30).** The crop has a seventh channel, `searched` (0..4: how many of a crop
cell's four 1-yd cells were looked at THIS episode, unlike `known`/`age`/`visited`, which a kept map carries across episodes). `decode_map`
returns `values` `[N, H, W, 6]`: height, known, visited, age, frontier, then `searched / 4`; `MapEncoder.planes_per_cell` =
`CODE_EMBED + class embedding + 6`, the new plane last, so the patch weight is `[64, 16 x (P + 1)]`. The block's three new scalars
(`searched`, `new_age`, `total`) arrive through `MapEncoder.scalars` (the manifest's 7), read at `obs[0] + index`, never by constant. `_map_of`
refuses a crop that is not `MAP_CHANNELS` channels / `map_bytes == h x w x MAP_CHANNELS` (8 since map block revision 6) and cross-checks `map.searched_channel` when the manifest has it (a
manifest without the new keys is judged on its channel count alone). `_cell_spec_of` checks `goals.cells.channels == MAP_CHANNELS`. A
stage.json or checkpoint of the six-channel map does not load into this learner (resume refuses on `layout_changes`; a seed carries the
encoder tensors whose shapes still agree and leaves the rest fresh, see [py-learner-seeding.md](py-learner-seeding.md)).
**Map block revision 3 (general search sec 3).** Eight coverage scalars appended after `total` (`frontier_sin/cos/dist`, the nearest
frontier cluster's bearing off the facing and distance over 48; `region_sin/cos/dist`, the largest cluster's; `clusters` over 8;
`searched_cells`, the 1-yd cells looked at this episode over 6000): `MapEncoder.scalars` is the manifest's count (15), so no learner
constant changed; the manifest keys `coverage_cell_yards`, `coverage_reach_yards`, `cluster_scale`, `floor_scale`, `cluster_min_cells`,
`pocket_min_cells` are not read. The `no_coverage` arm zeroes them by `scalar_names` (`evaluation.COVERAGE_SCALARS`, those the manifest has:
a revision 2 manifest has none and the arm is refused without a coverage block either).
**The coverage block (general search sec 5, `BlockId::Coverage` 27, revision 1).** `coverage_of(entry, name)` reads stage.json block
`coverage` -- `{"first": obs[0], "grid" (12), "channels" (3: known, visited, searched tile counters over the manifest's scale 1024),
"width" (432)}` -- refusing a width that is not grid x grid x channels or a `layout` other than `row_col_channel`; every other manifest key
(`cell_yards` 32, `heading_up`, `scale`, `channel_names`) is optional. `vision_of` carries it as `"coverage"` (None without the block) and
refuses a mix of layouts with and without it, or a coverage block without the map (the sim's `Problem()` rule). `CoverageEncoder` (a
module on `VisionEncoder`, `vision.coverage.*`, as `vision.map.*` is): the 432 columns gathered raw per layout by `first` (the
`MapEncoder` gather pattern), reshaped `[N, 3, 12, 12]` from the block's `[row][col][channel]` order, `Conv2d(3, 16, 3, padding 1)` SiLU,
`Conv2d(16, 16, 3, stride 2, padding 1)` SiLU (6 x 6), flatten 576 -> `Linear(576, 64)` SiLU -> `join = _linear(64, 256, sqrt 2)` added to
the camera's embedding before its SiLU exactly as the map's join is; a layout without the block (`first` -1) contributes 0. The span is
blinded from the adapters and normalisers (`VisionEncoder.blind`). Seeding: `bootstrap._seed_coverage` (fresh with the join zeroed from a
checkpoint without it, M1's). `update_bytes_per_row` is unchanged: the grid is observation columns, not camera bytes.

`vision_of(stage, layout_names)` (`:968`) returns per layout `{first, height, width, channels, classes, class_channel,
scalars, bytes_per_pixel, pixel_classes, patch, class_limit, render_sizes, look, look_names, image_bytes, entities, map,
camera_bytes, coverage, [sight]}` or None; None altogether if no layout has a camera. Revisions older than 6 (no `pixel_classes`; 4
bytes/pixel with kinds, or 5 with an entity slot) are refused by name (`bytes_per_pixel` must be 4). `vision_image_bytes` returns `camera_bytes` (image + map crop) (`:1068`); `vision_look_heads` the
`(7,5,5)` tuple. Duplicate knowledge hazard: the byte layout (4 bytes/pixel, class in the low 5 bits of byte 3, the
objective in bit 5; map channels order) is re-implemented here and in C++ `Vision::DecodePixel` /
`DecodeCropCell`; change both together.

`decode_image`: `[N, H*W*4] uint8 -> [N,H,W,5] float32`: distance `255 -> 1.0 else b/254`, height
`(b-128)/125`, normal `b/255`, class `b & 31`, objective `(b>>5)&1`. (`decode_slots` and `SLOT_BYTE` are gone with the
entity slot.) `decode_map`
(`:1133`): code (clamped to 0..4), class, and 5 float values (height `(b-128)/127` where known, known, visited, age/255,
frontier).

## `MapValueIteration` (`:1164`) and `MapEncoder` (`:1208`)

`MapEncoder` (shared by all layouts): per cell the code embedded (4), the entity class through the **camera's class
embedding** (held by reference via `__dict__`, `:1238`), five values: 4+6+5 = 15 planes; 4x4 patches (`PATCH=4`) of a
48x48 crop -> 12x12 grid; `Linear(240->64)` SiLU `Linear(64->64)` SiLU; features pooled as spatial-softmax keypoints
(128) + mean (64) + the block's 4 scalars = 196 -> `Linear(196->128)`; then
`join = Linear(128 -> 256)` (orthogonal init gain sqrt(2)) after a SiLU, added to the camera embedding before its SiLU
(`VisionEncoder.forward :1716`). Seeding from a checkpoint without a map zeroes the
join
(`bootstrap._seed_map`, [py-learner.md](py-learner.md)).

## `VisibleEntities` (`:1288`), `SightEntities` (`:1363`), `SightPointers` (`:1531`)

`VisibleEntities` is the camera's entity list (a standalone module since the seat-set base class was removed; its
state-dict keys and shapes are unchanged) (32 slots x 20 columns for the live M2/M1): its slot
token is `[kept columns (18), class embedding (6, the camera's table), type embedding (8, hashed
`(entry*2 + is_object) mod 4096`)]` -> `32 -> 64 -> 64` tanh MLP; the `memory` column is zeroed (read as 0 here) and the
token is its columns alone (`encode`; the old `link` of the patch features under the slot's pixels is gone: the image carries
no entities, and the list's `los`, `ang_width` and `ang_height` columns, entities revision 2, say where and how big). The pooled mean+max goes through
`pool` to 256 and is added to the camera embedding. No pointer heads.
`SightEntities` (dungeon stages only; absent in the M1/M2 fixtures): reads a sight list (visible half first = the
entity list, then remembered entities) with **the entity list's encoder** (`self.shared`, held by reference), plus
`extra` Linear over the memory columns, a `memory_embed` (ids folded onto the table); the
optional **named row** (sight revision 2, M3): a token for "what the goal names" with a `named_task`, a match score
softmax over present slots, `named_pool`, and per-press `named_gain` (starts at 0) added to the pointer scores.
`SightPointers` holds one query Linear per press (head_width -> 64, gain 0.01), the actor's own. `with_pointers`
(`:1507`) overwrites the press action ranges' logits with `tokens . query`. 
Invariants: all layouts' lists must agree on slots/width/presses (raises otherwise, `:1391`); the sight list's leading
columns equal the entity list's (`_sight_of`).
Pointer logits use the same tokens as the camera embedding (`tokens(obs, layout)`: both are from the observation alone).

`MemoryEntities` (object memory, 2026-10-18; only where a layout has the `memory_entities` block, `_memory_entities_of`
refuses one whose first `base_width` columns are not the entity list's): token = `shared.token(raw[..., :20])` (the
entity list's encoder by reference) + `extra` Linear(15 -> 64) over the memory columns; present slots pooled (mean +
max, zero when none present) through `pool` Linear(128 -> 256), added to the camera embedding after the sight list's;
its columns are in `blind`. Seeding (`bootstrap._seed_memory_entities`, prefix `vision.memory_entities.`, kept out of
`_seed_vision`'s camera keys so a new module never resets the camera's join): carried at the same revision and shapes,
else fresh with `pool` zeroed -- a seeded policy computes exactly what it did (tmp/objmem/smoke.py: outputs
bit-identical whatever the block holds; a stage without the block bit-identical to the code before). Not exportable
(`export.py` refuses it, like the sight list).

## `VisionEncoder` (`:1541`)

Camera for all layouts. Constants: `PATCH` (class default 4, per instance the manifest's `patch`, 8 live; the instance
attribute shadows the class attribute `:1582`), `WIDTHS=(64,64)`, `EMBED=256`, `CLASS_EMBED=6` (the entity tokens' and the map's class table, not
the pixels'). Parameters (M2): `class_embed [32,6]`, `patch` (`PATCH^2 x (4 + P)` inputs: 704 at patch 8 with P = 7 pixel classes), `mix`,
`embed [256, 2*64+scalars]`, plus sub-modules `entities`, `map`, `sight`.
`forward(obs, layout, image)` (`:1710`): gather the layout's scalars and decode; planes = 4 non-class channels + the
class as a one-hot over `pixel_classes` (`planes`: a comparison against a buffer, float data planes, no learned embedding and no scatter
backward on the pixel path); patches row-major; features; keypoints; `embed`; add map, entities, sight; `silu`. A layout
without a camera reads garbage columns and is zeroed by `VisionJoin`/`has_vision` (`:1654`). The encoder is **owned by
the actor** (its state dict) and referenced by the critic; `vision_parameters()` (`trainer.py:726`) feeds a separate
Adam (`vision_opt`) stepped once per minibatch. `blind` collects columns the adapters/normalisers must not read
(`:1638`).
Mixed resolution: the sim renders at several sizes (`render_sizes` 32x16 ... 128x64) and scales to the canonical
128x64 before sending, so the learner always sees 128x64 (`vision_of` validates each size is within the canonical one).
Reviewer notes: `forward` recomputes `decode_image` and (twice) `pixels(image)`; the `seen` objective test uses channel
index `IMAGE_CHANNELS-1` (`:1719`).

## `VisionJoin` (`:1732`), `LookHead` (`:1763`)

`VisionJoin`: `Linear(256 -> width)` (gain sqrt(2)); zeroes rows of layouts without a camera. Each network has its own.
Seeding from a checkpoint without a camera zeroes it (`bootstrap._seed_vision`).
`LookHead(width, heads, layouts_with)`: `Linear(width -> sum(heads))` split into one categorical per head (yaw rate 7,
pitch rate 5, zoom/command 5), init gain 0.01 and a bias `LOOK_HOLD_BIAS = 2.0` toward each head's hold choice
(`look_hold_indices :1755`: rate heads' middle choice, the last head's choice 0), so a fresh head holds ~55% of the
time. `sample` returns choices `[N,heads]` and the joint log-prob; rows without a camera give 0/0 and no gradient.
`evaluate` returns log-prob and entropy. Enters the PPO ratio as part of the joint log-prob (`trainer.py:1748`).
Not exportable (`export.py:440`).

## `GoalEmbedding` (`:1868`)

Embeds the goal held: `kind` Embedding(kinds,width) + `target` Embedding(targets,width) (zero-init); `paired`
(goal_slots
> 1): the goal number is a pair `primary*(count+1)+secondary+1` (`goal_pair :1960`, `split_goal_pair :1965`) and the
secondary adds its embedding through a learned `gate` (init 0.5). The actor builds it `scaled=True`: extra
`kind_scale`/`target_scale` tables (zero-init) used as FiLM `features*(1+scale)+shift` (`condition :1918`); the critic
adds the shift only (`forward :1914`). `_GOAL_SCALE_KEYS` (`:1928`): an actor saved before the scale existed loads with
the scale at zero (`load_actor_state`). `_GOAL_POINTER_PREFIXES` (`goal_head.pointer.`, `goal_head.place_spec`) likewise:
an actor saved before the pointer loads with it as initialised (a no-op) and no place slots.
`GOAL_KIND_ROW_KEYS` / `grow_goal_rows(state, current)` (search-kind, 2026-10-10): the goal tensors with a row per kind (or per joint,
`drawn`) of a checkpoint with FEWER kinds are padded with the network's own rows before loading (`load_actor_state` and the critic in
`MappoTrainer.load_state_dict`; the optimisers then start fresh); MORE kinds than the network raises `ValueError`. `load_actor_state`
returns the keys grown.

## `LayoutActor` (`:1971`)

Constructor `(layouts, hidden, foresight_outputs, recurrent_size, goal_count, goal_targets, slow_size,
foresight_feedback, lookahead, goal_slots, vision)`. Members: `norms`, `adapters`, `trunk`,
`memory` (GRUCell(hidden[-1] -> recurrent_size)), `heads` (per layout, gain 0.01), `foresight`, `goal_head`,
`goal_embedding`, `foresight_proj`, `slow_memory` (GRUCell(head_width -> slow_size)), `slow_value`,
`vision`, `vision_join`, `sight_pointers`, `look_head`, and the rollout-only `dense_adapters`/`dense_heads`.
`head_width` = recurrent size when recurrent, else `hidden[-1]`.

Flow for one decision (`_forward :2248`, used by `forward`/`step`):
`encode` (`:2081`) = adapters (per-layout loop over `per_layout` groups, or dense on the GPU rollout copy) + camera join, then trunk -> `features_from` (GRU) -> `policy_features` (`:2146`: foresight feedback, then goal
FiLM)
-> `action_logits` (`:2164`: heads, then sight pointers, then `masked_logits`).
`decide_goals` (`:2204`) is the single implementation of the goal decision used by the eager path and the graph:
drop an ended secondary; promote the queue's head if the primary ended; choose on the clock, on `ended`-without-queue,
or on an `event`; an order overrides. `carry` runs the GRU over a replayed sequence. `slow_step` runs the slow GRU on
the detached (and foresight-fed) features. `initial_memory`, `fold_normalisation`, `densify` as described above.
Config keys: all through constructor args from `MappoConfig` (see the trainer document).
Quirks: `LayoutActor.forward` and `.step` are used by `distill.py:320` (`teacher.actor.step`) and `cast.py`; the update
and rollout use `encode`/`features_from`/`action_logits` directly.

## `LayoutCritic` (`:2262`)

`V(global state, own observation)`. Members: `state_norm`, `state_encoder` Linear(state_dim -> hidden[0]), `norms`,
`adapters`, `trunk`, `memory` GRUCell(hidden[-1] -> recurrent_size), `head` Linear(head_width -> 1, gain 1),
`goal_embedding` (width hidden[0], shift only), `vision` (reference) + `vision_join`.
`encode_goal_free` (`:2309`) = `state_encoder(state_norm(state))` and the seat's own adapter output + camera;
`encode_goal` (`:2328`) adds the goal embedding and runs `trunk(hidden + own)`. A rollout decision runs the first half
beside the actor choosing the goal (`_RolloutGraph._body`, `trainer.py:452`). `step_encoded` (`:2369`) runs the GRU and
the head. Output is a **normalised** value when `ValueNorm` is used; the trainer denormalises.
Note the critic has a GRU of its own cleared with the actor's (`ActingState.critic_memory`).

## Loading: what a by-name load tolerates

- `load_actor_state` (`:1943`): `strict=False`, then raises on any missing or unexpected key except (a) the blind-column
  masks, (b) `goal_embedding.kind_scale.weight` / `target_scale.weight` (zeroed) and (c) `goal_head.pointer.*` and
  `goal_head.place_spec` (left as initialised). **Any other missing or extra key is an error**; shape mismatches raise
  inside torch.
- The critic: `MappoTrainer.load_state_dict` (`trainer.py:2038`) does the same without the goal-scale exemption; the
  pointer is the actor's alone, so the critic has no new key to tolerate.
- Cross-stage seeding (a new stage from a parent checkpoint) is not this path; it is by block name in
  `bootstrap.py` ([py-learner.md](py-learner.md)).
- Checkpoint contents (`train.save_checkpoint`, `train.py:170`): `trainer.state_dict()` = `actor`, `critic`,
  `value_norm`,
  `actor_opt`, `critic_opt`, optional `vision_opt`; plus `config`, `spec`, `update`, `env_steps` and `extra`
  (`style`, controller and tracker state, `train.py:1124-1127`). `slow_opt` is saved when it exists (optional key); the rollout copies are not.

## Observed issues

1. `LayoutActor.features` (`:2115`) takes no `vision_embedding` although `encode` does; callers that want the shared
   embedding (the update) call `encode` directly.
3. `VisionEncoder.PATCH` is both a class constant and per-instance state (`:1570`, `:1582`).
4. `GoalHead.slot_entropy_weight` is a public attribute defaulting to 0.1 that the trainer overwrites after construction
   (`trainer.py:665`); a caller that builds a `LayoutActor` without the trainer (cast, distill, evaluate) keeps 0.1
   whatever the config says. Harmless for acting; wrong for any scoring.
6. `_carry_sequence` depends on the private `torch._VF.gru` (`:373`).
7. `attach_blind_columns` registers a hook referencing the network by default argument (`:816`).
8. The first lines of the file describe an architecture (adapter + trunk + head = plain MLP) that no longer holds.
9. `RunningNorm.hide` ignores columns >= the width silently (`:103`).
10. `masked_logits` falls back to action 0 on an all-masked row; action 0 is "no press" in every layout (UNVERIFIED:
    confirm action 0 semantics in [cpp-layout-character.md](cpp-layout-character.md)).

## Reviewer notes

- Refactor hazard: the tensor names (`adapters.N`, `heads.N`, `norms.N`, `vision.*`, `goal_head.*`) are the checkpoint
  format; a rename breaks old checkpoints, and `bootstrap.py` seeds by these names.
- `networks.py` mixes five concerns (primitives, goal head, entity list, camera/map/sight, the two networks). A split
  by file is safe for imports because everything is imported by name from `.networks` (`train.py:49`, `distill.py:23`,
  `evaluate.py:30`, `trainer.py:18`, `cast.py`, `partners.py`, `export.py`).
- Dead-feature question list: `SharedInputDense` (GPU only). (`EntitySets`, its attention and pointer heads, and `MapValueIteration` were deleted.)
