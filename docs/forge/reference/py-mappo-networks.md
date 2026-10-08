# `mappo/networks.py`: every class, in file order

Purpose and scope. A unit-by-unit reference for `apps/forge/python/animus/mappo/networks.py` (2382 lines), the only file
that defines the policy and critic networks. Parameter lists and shape chains for the live M2 stage are in
[py-mappo.md](py-mappo.md); the training use is in [py-mappo-trainer.md](py-mappo-trainer.md). Tests are in
`apps/forge/python/tests/`; those that cover many units are listed once here:

| Test file | Covers |
|---|---|
| `test_golden_update.py` | key names and shapes of the live M2 actor/critic, one update's numbers (CPU) |
| `test_vision_encoder.py`, `test_vision_identity.py`, `test_vision_bytes.py` | camera: decode, patches, class embedding, shared encoder, blind columns, link to the entity list, bytes on the wire |
| `test_free_look.py` | patch 8 at 128x64, render sizes, look head, camera chunking, seeding |
| `test_mental_map.py` | map decode, `MapEncoder` |
| `test_sight.py`, `test_interact.py` | sight list, pointer heads, named row |
| `test_recurrent.py`, `test_normalisation.py`, `test_masking.py`, `test_goals.py`, `test_goal_queue.py`, `test_goal_targets.py`, `test_two_clock.py`, `test_foresight.py` | recurrent core, normalisers, masks, goal head and slots |
| `test_rollout_graph.py`, `test_rollout_graph_log.py` | graph path (GPU) and log lines (CPU) |

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
- `sample_logits(logits, deterministic)` (`:63`): Gumbel-max draw `argmax(logits - log(-log U))`; `U` in [0,1) so a
  masked logit cannot win. Returns `(choice, log_prob)`. Used by the graph path and the goal head; the eager path
  uses `Categorical.sample()` (`trainer.py:1066`), a different random stream (test
  `test_the_lean_sampler_draws_what_categorical_draws`).
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
Tests: `test_normalisation.py`, `test_masking.py`.
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
Tests: `test_recurrent.py::test_fused_gru_matches_the_step_loop_at_the_real_size`, `..._at_odd_shapes` (need a GPU),
`test_carrying_a_sequence_matches_stepping_through_it`.
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

Public methods:

- `logits(features, obs, layout)` (`:560`): masked joint logits `[rows, kinds*targets]`; allowed = `accepts` AND (goal
  block's kinds-there x targets-there, only when `targets > 1`). A layout with no goal block gets all goals masked
  except index 0 when `has_space`. `allowed[:, 0] = True` always (goal 0 "Fight about no one" is never masked).
  With lookahead the predictions (detached) times `lookahead_weight` are added to the logits (`:566`).
- `signals(obs, layout)` (`:464`): reads the goal block's columns > 0.5: `ended`, `secondary_ended`, `event`,
  `from_order`, `order_goal`, `achieved` (-1 none). With `slots <= 1` only `ended` is real.
- `slot_logits(features, slot, drawn, obs, layout)` (`:494`): logits `[rows, count+1]` for a slot after the primary,
  last column = none; masked by the goal block like the primary; a row without a goal block has only none.
- `draw(features, obs, layout, primary_given, given, deterministic, slots=None)` (`:523`): draws (or, with `slots`,
  scores) primary then secondary/queue slots, each conditioned on the earlier ones. Returns `(slots [rows,S] with -1
  for none, log_prob, entropy)`. The primary's entropy counts fully; later slots are weighted by `slot_entropy_weight`
  (default 0.1, overwritten from `MappoConfig.goal_slot_entropy_weight` by `trainer.py:665`, not by the constructor).
  A primary `given` by an order is conditioned on but contributes no log-prob or entropy.
- `predictions(features)` (`:555`): `(success logits, sigmoid(duration))` per candidate goal.
- `set_space(accepts, block_at)` (`:448`), `block_width` (`:433`), `columns` (`:442`): goal block layout
  (secondary_ended
  = base, event, from_order, order_kind, order_target, achieved_kind, achieved_target with base = kinds+targets+2).
- `ended(obs, layout)` (`:583`): **no caller anywhere**; dead.

Data flow: built in `LayoutActor.__init__` (`:2021`) with width = `slow_size or head_width`. Called from the rollout
(`decide_goals`) and the update (`_update_goals`, and the fast-loop `goal_distribution` only when there is no slow
loop).
Contract: the goal block's column positions must equal `GoalBlock::Obs` in the sim; there is no checksum other than
`set_goal_space`'s kinds/targets count check ([cpp-blocks.md](cpp-blocks.md)).
Known quirk: "the goal block's order columns are always zero" ([known-issues.md](known-issues.md)): `from_order` and
`order_goal` are therefore always False/0 on live stages; the `given` branches of `draw`/`decide_goals` are exercised
only by tests (`test_goal_queue.py::test_the_directors_primary_is_held_and_not_scored`).
Tests: `test_goals.py`, `test_goal_queue.py`, `test_goal_targets.py`, `test_two_clock.py`.

## Blind columns: `attach_blind_columns`, `clear_blind_columns`, `without_blind_columns` (`:800-829`, `:1936`)

A layout's adapter must not read some columns (camera scalars, entity-list slots, map scalars, sight slots) because dedicated encoders read them. `attach_blind_columns(network, columns, tag)` registers a buffer
`<tag>_keep_<index> [1, obs_i]` (1 = keep, 0 = blind), multiplies the adapter weight by it and registers a gradient hook
that multiplies the gradient by it, so those weight columns stay exactly zero through every update. Tags: `"set"` and
`"vision"` (`BLIND_KEEP_PREFIXES` `:1933` also lists the retired `"blind_keep_"`). `clear_blind_columns(network)`
(`:821`)
re-zeroes after a seed or load. The keep buffers are **saved** in checkpoints (they are persistent buffers; see the
golden key list) but **dropped on load** (`without_blind_columns`, `:1936`; `load_actor_state` `:1943`;
`MappoTrainer.load_state_dict` `trainer.py:2039`): the network's own masks, built from the current stage, win.
Resume safety check: `MappoTrainer.camera_columns_clear()` (`trainer.py:825`) fails the resume if any adapter weight
is non-zero at a blind column (`train.py:735`).
Quirk: the gradient hook captures the network and looks the buffer up by name (`:816`); a deepcopy of the network (the
rollout copy) carries its own copy of the hook closure's `network` argument default, which is the original network. The
masks are identical, so the result is the same, but it is a trap if masks ever differ between copies. UNVERIFIED:
whether the deepcopied hook binds to the copy or the original (default-argument binding makes it the original);
irrelevant for the rollout copies, which never backpropagate.

## stage.json readers (`:843-1107`)

Constants: `VISION_BLOCK="vision"`, `ENTITIES_BLOCK="entities"`, `SIGHT_BLOCK="sight"`, `MAP_BLOCK="map"`,
`IMAGE_BYTES_PER_PIXEL=4`, `IMAGE_CHANNELS=5`, `IMAGE_CLASS_CHANNEL=3`, `CLASS_MASK=0x1F`,
`CLASS_LIMIT=32`, `DEFAULT_PATCH=4`, `MAP_CHANNELS=6`, `MAP_CODES=5`, `MAP_HEIGHT_ZERO=128`.
`vision_of(stage, layout_names)` (`:968`) returns per layout `{first, height, width, channels, classes, class_channel,
scalars, bytes_per_pixel, pixel_classes, patch, class_limit, render_sizes, look, look_names, image_bytes, entities, map,
camera_bytes, [sight]}` or None; None altogether if no layout has a camera. Revisions older than 6 (no `pixel_classes`; 4
bytes/pixel with kinds, or 5 with an entity slot) are refused by name (`bytes_per_pixel` must be 4). `vision_image_bytes` returns `camera_bytes` (image + map crop) (`:1068`); `vision_look_heads` the
`(7,5,5)` tuple. Duplicate knowledge hazard: the byte layout (4 bytes/pixel, class in the low 5 bits of byte 3, the
objective in bit 5; map channels order) is re-implemented here and in C++ `Vision::DecodePixel` /
`DecodeCropCell`; `test_vision_bytes.py::test_decoding_every_byte_is_the_sims_decode_pixel_exactly` and
`test_mental_map.py::test_the_crop_decodes_as_the_sim_encodes_it` pin it.

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
Tests: `test_mental_map.py`. 

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
Tests: `test_sight.py`, `test_interact.py`, `test_vision_identity.py`.
Pointer logits use the same tokens as the camera embedding (`tokens(obs, layout)`: both are from the observation alone).

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
Tests: `test_vision_encoder.py`, `test_free_look.py`, `test_vision_identity.py`.
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
Tests: `test_free_look.py`.

## `GoalEmbedding` (`:1868`)

Embeds the goal held: `kind` Embedding(kinds,width) + `target` Embedding(targets,width) (zero-init); `paired`
(goal_slots
> 1): the goal number is a pair `primary*(count+1)+secondary+1` (`goal_pair :1960`, `split_goal_pair :1965`) and the
secondary adds its embedding through a learned `gate` (init 0.5). The actor builds it `scaled=True`: extra
`kind_scale`/`target_scale` tables (zero-init) used as FiLM `features*(1+scale)+shift` (`condition :1918`); the critic
adds the shift only (`forward :1914`). `_GOAL_SCALE_KEYS` (`:1928`): an actor saved before the scale existed loads with
the scale at zero (`load_actor_state`).
Tests: `test_goal_queue.py::test_an_actor_saved_before_the_goal_scale_loads_with_it_at_zero`.

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
Tests: `test_recurrent.py`, `test_goals.py`, `test_goal_queue.py`, `test_masking.py`, `test_free_look.py`.

## `LayoutCritic` (`:2262`)

`V(global state, own observation)`. Members: `state_norm`, `state_encoder` Linear(state_dim -> hidden[0]), `norms`,
`adapters`, `trunk`, `memory` GRUCell(hidden[-1] -> recurrent_size), `head` Linear(head_width -> 1, gain 1),
`goal_embedding` (width hidden[0], shift only), `vision` (reference) + `vision_join`.
`encode_goal_free` (`:2309`) = `state_encoder(state_norm(state))` and the seat's own adapter output + camera;
`encode_goal` (`:2328`) adds the goal embedding and runs `trunk(hidden + own)`. A rollout decision runs the first half
beside the actor choosing the goal (`_RolloutGraph._body`, `trainer.py:452`). `step_encoded` (`:2369`) runs the GRU and
the head. Output is a **normalised** value when `ValueNorm` is used; the trainer denormalises.
Note the critic has a GRU of its own cleared with the actor's (`ActingState.critic_memory`).
Tests: `test_recurrent.py::test_critic_*`.

## Loading: what a by-name load tolerates

- `load_actor_state` (`:1943`): `strict=False`, then raises on any missing or unexpected key except (a) the blind-column
  masks and (b) `goal_embedding.kind_scale.weight` / `target_scale.weight` (zeroed). **Any other missing or extra
  key is an error**; shape mismatches raise inside torch.
- The critic: `MappoTrainer.load_state_dict` (`trainer.py:2038`) does the same without the goal-scale exemption.
- Cross-stage seeding (a new stage from a parent checkpoint) is not this path; it is by block name in
  `bootstrap.py` ([py-learner.md](py-learner.md)).
- Checkpoint contents (`train.save_checkpoint`, `train.py:170`): `trainer.state_dict()` = `actor`, `critic`,
  `value_norm`,
  `actor_opt`, `critic_opt`, optional `vision_opt`; plus `config`, `spec`, `update`, `env_steps` and `extra`
  (`style`, `explore`, controller and tracker state, `train.py:1124-1127`). `slow_opt` is saved when it exists (optional key); the rollout copies are not.

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
  format; the golden test fails on any rename, and `bootstrap.py` seeds by these names.
- `networks.py` mixes five concerns (primitives, goal head, entity list, camera/map/sight, the two networks). A split
  by file is safe for imports because everything is imported by name from `.networks` (`train.py:49`, `distill.py:23`,
  `evaluate.py:30`, `trainer.py:18`, `cast.py`, `partners.py`, `export.py`).
- Dead-feature question list: `SharedInputDense` (GPU only). (`EntitySets`, its attention and pointer heads, and `MapValueIteration` were deleted.)
