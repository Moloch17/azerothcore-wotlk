# The MAPPO package: overview, build, parameters, shapes

Purpose and scope. This is the entry document for `apps/forge/python/animus/mappo/`: the actor and critic networks, the
rollout buffer, the MAPPO trainer and the value normaliser. It says how the networks are built from a
`stage.json`, lists every state-dict key of the live M2 stage and reconciles the counts (actor 153, critic 80), derives
the tensor shapes at every boundary, and tabulates every switchable feature. The detail is split:

| Document | Covers |
|---|---|
| this file (`py-mappo.md`) | map table, build from stage.json, parameter inventory, shape chains, feature switches, issues |
| [py-mappo-networks.md](py-mappo-networks.md) | every class of `mappo/networks.py` |
| [py-mappo-trainer.md](py-mappo-trainer.md) | `MappoConfig`, rollout path, graphs, the update, the slow goal update, save/load |
| [py-mappo-buffer.md](py-mappo-buffer.md) | `buffer.py` (storage, GAE variants), `valuenorm.py` |
| [py-human-and-misc.md](py-human-and-misc.md) | `animus/human/*` and the remaining top-level modules |

Related: [00-architecture.md](00-architecture.md), [py-learner.md](py-learner.md) (train.py, stage.py, config.py),
[protocol.md](protocol.md), [file-formats.md](file-formats.md), [metrics.md](metrics.md),
[config-yaml.md](config-yaml.md), [config-keys.md](config-keys.md), [tests.md](tests.md),
[known-issues.md](known-issues.md), [glossary.md](glossary.md).

Everything below is derived from the code at `bd32b9dc8` (branch `forge`). Line numbers are of that tree. "Live" means
one of the twelve stages in `apps/forge/python/configs/` (the yaml files are `move1_controls` ... `dungeon3_deadmines`;
`fast.yaml` is an overlay, not a stage). Nothing was run: shapes come from reading the code and from the checked-in
golden file `apps/forge/python/tests/golden/learner_update.json`.

## Map table

| Path | Lines | Role |
|---|---|---|
| `apps/forge/python/animus/mappo/__init__.py` | 1 | package marker (one line) |
| `apps/forge/python/animus/mappo/networks.py` | 2382 | all network classes, the stage.json readers (`vision_of`), image/map decoders, sampling helpers |
| `apps/forge/python/animus/mappo/trainer.py` | 2053 | `MappoConfig`, `MappoTrainer` (rollout path, CUDA/HIP graphs, PPO update, slow goal update, checkpoint state) |
| `apps/forge/python/animus/mappo/buffer.py` | 398 | `RolloutBuffer`, GAE, foresight targets, span GAE |
| `apps/forge/python/animus/mappo/valuenorm.py` | 46 | `ValueNorm`, the running return normaliser |

## What the package is, in one paragraph

One set of weights serves every agent of every layout ("parameter sharing", `networks.py:1`). A layout is one class
(`warrior`, `paladin`, ... ten of them in the live stages, `seek_spec.json`). Each network has an input *adapter* per
layout, a *trunk* shared by all layouts, and (actor only) an action *head* per layout. Around that sit the shared
camera encoder, the recurrent core (a GRU cell), the goal machinery and the look head. The critic is centralised: it
adds a state encoder over the global state vector. Training is PPO with a clipped value loss, run over whole rollouts
replayed in order through the GRUs (`trainer.py:1603`). The flat (non-recurrent) update was removed; a config with
`recurrent_size` 0 is refused (`trainer.py:1496`).

## How the networks are built from a stage.json

Call chain (`train.py:502-531`, also used by `tools/resume_check.py` and `tests/test_golden_update.py`):

1. The sim writes `<layouts_dir>/<scenario>/stage.json` (`stages.py:22 load_stage`). The learner connects, reads SPEC
   (`protocol.Spec`: `layouts` as (name, obs_dim, num_actions), `state_dim`, `image_bytes`, `map_bytes`,
   `look_heads`).
2. `trainer_inputs(config, spec, stage)` (`train.py:502`) calls
   - `vision_of(stage, names)` (`networks.py:968`): one dict per layout with a `vision` block, else `None`;
   - `check_image_bytes` and `check_look_heads` (`networks.py:1080, 1089`) against SPEC; a mismatch is a `SystemExit`
     (`train.py:514`);
3. `make_trainer` (`train.py:519`) passes `[(obs_dim, num_actions)...]`, `state_dim`, `config.mappo`, the two devices,
   and `vision` to `MappoTrainer.__init__` (`trainer.py:602`).
4. `MappoTrainer.__init__` constructs, in order (`trainer.py:644-696`):
   - derived sizes: `foresight_outputs = len(foresight_horizons_seconds) + 1 (+ 3 if foresight_obs_targets)` when
     `foresight_coef > 0`, else 0; `goal_count = goal_count * goal_targets`; `slow_goal_size` only if goals exist;
     `goal_slots > 1` requires `slow_goal_size` and at least 3 slots (`trainer.py:658`);
   - `LayoutActor(...)` (`networks.py:1971`) with `hidden`, foresight, recurrent size, goal kinds/targets,
     slow size, foresight feedback, lookahead, slots and `vision`;
   - `LayoutCritic(...)` (`networks.py:2262`), given the actor's `VisionEncoder` so the critic reads it by reference;
   - `ValueNorm` if `use_value_norm`;
   - optimizers (`reset_optimizers`, `trainer.py:740`);
   - **rollout copies**: `copy.deepcopy((actor, critic))` moved to `rollout_device`, a `ValueNorm` copy, and the list
     of (trained tensor, rollout tensor) pairs for in-place sync (`_pair_tensors`, `trainer.py:703`).
5. `train.py:731` then calls `set_goal_space(stage, names)` (`trainer.py:806`), which writes the goal `accepts` table
   and each layout's goal-block start column into the actor's `GoalHead` buffers (and the rollout copy's).
6. After seed or resume `train.py:735-738` checks `director_columns_clear()` (resume) or calls `clear_blind_columns()`
   (fresh/seeded): the adapters' camera and set columns are zeroed again (see the networks document, "blind columns").

What the `stage.json` has to contain for each network feature (`networks.py` readers):

| Feature | stage.json source | Reader | Refusals |
|---|---|---|---|
| layout widths | `layouts.<name>.obs_dim`, `num_actions` (via SPEC) | `make_trainer` | none |
| camera | block `vision` with `image` (`height,width,channels,classes,class_channel,scalars,bytes_per_pixel`, `patch`, `render_sizes`, `class_limit`) and `look.heads` | `vision_of` (`networks.py:968`) | no image; transport not `bytes`; no `classes` (before revision 5); 5 bytes/pixel and 5 channels only; images differing between layouts; scalars != block width |
| entity list | block `entities` with `entities` description | `_entities_of` (`:902`) | span mismatch |
| mental map | block `map` with `map` description | `_map_of` (`:878`) | not 6 channels and 5 codes; bytes != h*w*6; scalars != block width; map in some layouts but not all (`:1053`) |
| sight list | block `sight` | `_sight_of` (`:923`) | needs the entity list; visible half and leading columns must equal the entity list's; named row must follow the slots |
| goal space | `goals.{kinds,targets,accepts,block}` and the goal block's `obs[0]` per layout | `MappoTrainer.set_goal_space` (`:806`) | kinds/targets differ from `mappo.goal_count/goal_targets` |

## Parameter inventory: the live M2 stage (`move2_seek`)

Source: `tests/golden/learner_update.json`, case `move2_seek` (`actor_shapes`, `critic_shapes`); the case is built by
`tests/test_golden_update.py:48` from `fixtures/seek_stage.json`, `fixtures/seek_spec.json` and
`configs/move2_seek.yaml` (which `extends` `move1_controls.yaml`). Ten layouts. Config in force:
`hidden [256, 512, 512]`, `recurrent_size 128`, `goal_count 12`, `goal_targets 29`, `goal_slots 4`,
`slow_goal_size 128`, `foresight_coef 0.25` with `foresight_horizons_seconds [5, 30]`, `foresight_obs_targets`,
`foresight_feedback`, `goal_lookahead`; the stage has camera (patch 8), entity
list (32 slots x 20), mental map (48x48x6, 4 scalars), look heads (7,5,5); no sight list.

The golden file lists **state_dict keys** (parameters and buffers). Both counts verified from the file: actor 153,
critic 80.

### Actor, 153 keys

Per-layout keys, 8 per layout x 10 layouts = 80:

| Key pattern | Count | Kind | Shape (layout i) |
|---|---|---|---|
| `adapters.i.weight`, `adapters.i.bias` | 2 | parameters | `[256, obs_i]`, `[256]` |
| `heads.i.weight`, `heads.i.bias` | 2 | parameters | `[actions_i, 128]`, `[actions_i]` |
| `norms.i.mean`, `norms.i.var`, `norms.i.count` | 3 | **buffers** (`RunningNorm`) | `[obs_i]`, `[obs_i]`, `[]` |
| `vision_keep_i` | 1 | **buffer** (blind-column mask, `attach_blind_columns`) | `[1, obs_i]` |

with `obs_i` = 1555, 1548, 1543, 1449, 1528, 1526, 1518, 1516, 1559, 1595 and `actions_i` = 95, 95, 94, 82, 92, 91,
91, 90, 96, 100 for warrior, paladin, hunter, rogue, priest, deathknight, shaman, mage, warlock, druid
(`fixtures/seek_spec.json`).

Shared keys, 73:

| Group | Keys | Count | Shapes |
|---|---|---|---|
| `trunk.layers.0/1` | weight, bias | 4 | `[512,256]`,`[512]`,`[512,512]`,`[512]` |
| `memory` (GRUCell 512->128) | weight_ih, weight_hh, bias_ih, bias_hh | 4 | `[384,512]`,`[384,128]`,`[384]`,`[384]` |
| `foresight` | weight, bias | 2 | `[6,128]`,`[6]` |
| `foresight_proj` | weight, bias | 2 | `[128,6]`,`[128]` |
| `goal_embedding` | gate, kind.weight, kind_scale.weight, target.weight, target_scale.weight | 5 | `[]`, `[12,128]` x2, `[29,128]` x2 |
| `goal_head` | accepts, block_at (buffers), drawn.weight, kind.{w,b}, target.{w,b}, pair, slot_bias, none_bias, lookahead_weight, success.{kind.w,kind.b,pair,target.w,target.b}, duration.{same five} | 21 | `[12,29]`, `[10]`, `[349,128]`, `[12,128]`,`[12]`, `[29,128]`,`[29]`, `[12,29]`, `[3,128]`, `[3]`, `[2]`, success/duration `[12,128]`,`[12]`,`[12,29]`,`[29,128]`,`[29]` |
| `slow_memory` (GRUCell 128->128) | 4 keys | 4 | `[384,128]` x2, `[384]` x2 |
| `slow_value` | weight, bias | 2 | `[1,128]`,`[1]` |
| `look_head.linear` | weight, bias | 2 | `[17,128]`,`[17]` |
| `vision.*` (shared camera) | see below | 25 | see below |
| `vision_join.linear` | weight, bias | 2 | `[256,256]`,`[256]` |

`vision.*`: `class_embed.weight [32,6]`; `patch.{w [64,640], b}`; `mix.{w [64,64], b}`; `embed.{w [256,139], b}`
(that is 1 + 2 + 2 + 2 = 7); `entities.encoders.visible.0.{w [64,32],b}`, `entities.encoders.visible.2.{w [64,64],b}`,
`entities.link.{w [64,64],b}`, `entities.pool.{w [256,128],b}`, `entities.type_embed.weight [4096,8]` (9);
`map.code_embed.weight [5,4]`, `map.patch.{w [64,240],b}`, `map.mix.{w [64,64],b}`, `map.embed.{w [128,196],b}`,
`map.join.{w [256,128],b}` (9). 7 + 9 + 9 = 25.

Reconciliation: 80 + (4+4+2+2+5+21+4+2+2+25+2) = 80 + 73 = **153**. Which of the 153 are buffers: all `norms.*` (30),
all `vision_keep_*` (10), `goal_head.accepts`, `goal_head.block_at`; 42 in all. The rest are parameters.

Not in the state dict, by design (non-persistent buffers or plain attributes): the `VisionEncoder` tables `start`,
`has_vision`, `offsets`, `grid_x/y`, `patch_of`; `MapEncoder.start/offsets/grid_*`; `VisibleEntities.kept_columns`,
`column_keep`; `VisionJoin.has_vision`; `LookHead.has_vision`; `VisibleEntities.columns_visible`, `present_visible` and `has_sets`. They are
rebuilt from the `stage.json` at construction.

### Critic, 80 keys

Per-layout, 6 per layout x 10 = 60: `adapters.i.{weight,bias}` (same shapes as the actor's), `norms.i.{mean,var,count}`,
`vision_keep_i`. There are no heads.

Shared, 20: `trunk.layers.0/1` (4), `memory` GRUCell 512->128 (4), `head.{weight [1,128], bias [1]}` (2),
`state_encoder.{weight [256,1958], bias [256]}` (2), `state_norm.{mean [1958], var [1958], count}` (3),
`goal_embedding.{gate, kind.weight [12,256], target.weight [29,256]}` (3), `vision_join.linear` (2).
60 + 20 = **80**. The critic holds the camera encoder by reference, not as a module (`networks.py:1842 share_vision`),
so it has no `vision.*` keys; its gradients still reach the encoder (it is stepped by `vision_opt`, see the trainer
document). Note the critic's goal embedding is 256 wide (`hidden[0]`, `networks.py:2283`) while the actor's is 128
wide (`head_width`, `networks.py:2025`): different by construction (the critic adds it before the trunk, the actor
after the GRU).

### Which parts are shared across classes, which are per class

| Part | Actor | Critic |
|---|---|---|
| per class (index = layout id) | `norms`, `adapters`, `heads`, `vision_keep_*`; one `block_at` entry in `goal_head` | `norms`, `adapters`, `vision_keep_*` |
| shared by all classes | trunk, GRU, goal head and embedding, slow loop, foresight, look head, `vision` (camera, entity list, map), `vision_join` | trunk, GRU, value head, state encoder and norm, goal embedding, `vision_join` |
| shared actor/critic | the `VisionEncoder` object (owned by the actor; the critic holds a reference) | same object |

Rows of a batch carry their layout id; every layout's adapter reads only its own `obs_dim` leading columns; padded
columns past it are zeros written by the sim (`networks.py:171`).

## Shape chains

### M2 (`move2_seek`), per decision, E envs x A=1 agent (`agents_per_env 1`, `fixtures/seek_spec.json`)

Wire (SPEC, see [protocol.md](protocol.md)): `obs [E,A,1595]` float32 (the widest layout, padded), `state [E,1958]`,
`mask [E,A,100]` bool, `layout [E,A]`, `image [E,A,54784]` uint8 (the camera row, `spec.camera_bytes`).

Camera row: `image_bytes = 64*128*5 = 40960` (`height x width x bytes_per_pixel`, `networks.py:1044`) then the map
crop `48*48*6 = 13824` (`networks.py:888`); 40960 + 13824 = 54784.

Actor, per flat row (N = E*A rows):

| Step | Operation | Shape out | Evidence |
|---|---|---|---|
| select columns | obs rows `[:obs_i]` of layout i, `RunningNorm` | `[n_i, obs_i]` | `networks.py:2100` |
| adapter | `Linear(obs_i -> 256)` | `[N,256]` | `:1994` |
| camera decode | bytes -> 5 channels `[N,64,128,5]`; class embedded (6) -> 4+6 = 10 planes | `[N,64,128,10]` | `decode_image :1110`, `planes :1669` |
| patches | 8x8 patches, grid 8 x 16 = 128 patches x 640 | `[N,128,640]` | `patches :1676` |
| patch MLP | `Linear 640->64`, SiLU, `Linear 64->64`, SiLU | `[N,128,64]` | `features :1684` |
| keypoints | spatial softmax of 64 channels -> (x,y) | `[N,128]` | `keypoints :1689` |
| scalars | 11 columns of the layout's vision block | `[N,11]` | |
| embed | `Linear(128+11=139 -> 256)` | `[N,256]` | golden `vision.embed [256,139]` |
| + map | map crop `[N,48,48,6]` -> planes 4+6+5=15, 4x4 patches, grid 12x12 x 240 -> `Linear 240->64`, `64->64`; pooled `3*64 + 4 scalars = 196` -> `Linear 196->128`; join `128->256` | `[N,256]` added | `MapEncoder :1208`, golden `map.embed [128,196]`, `map.patch [64,240]` |
| + entity list | 32 slots x 20 columns; token = 18 kept cols + class embed 6 + type embed 8 = 32 -> `32->64->64` tanh MLP, + link (`64->64` of patch features under the slot's pixels); mean+max pooled over present slots 128 -> `Linear 128->256` | `[N,256]` added | `VisibleEntities :1288`, golden `entities.encoders.visible.0 [64,32]`, `pool [256,128]` |
| SiLU | camera embedding | `[N,256]` | `:1729` |
| vision join | `Linear 256->256`, zero for layouts without a camera | `[N,256]` added to the adapter output | `VisionJoin :1732` |
| trunk | tanh, `Linear 256->512`+tanh, `Linear 512->512`+tanh | `[N,512]` | `_Trunk :234` |
| GRU | `GRUCell(512 -> 128)` from `memory [N,128]` | `[N,128]` | `:1997` |
| foresight feedback | `foresight(128->6)` detached -> `foresight_proj(6->128)`, added | `[N,128]` | `with_foresight :2135` |
| goal conditioning | `features*(1+scale)+shift` (FiLM), embeddings 128 wide | `[N,128]` | `GoalEmbedding.condition :1918` |
| head | `Linear(128 -> actions_i)` into a `[N,100]` buffer of `-1e9`, then mask | `[N,100]` logits | `action_logits :2164` |
| look head | `Linear(128->17)` split 7/5/5 | 3 logit sets | `LookHead :1763` |
| goal head | slow GRUCell(128->128) over the fast features; joint logits `[N, 348]` (12 kinds x 29 targets), plus a "none" column for later slots `[N,349]` | | `GoalHead :382` |

Goal block width 128 per layout: `(kinds 12 + targets 29 + 2) + 3 + 2*(12+29) = 43 + 3 + 82 = 128`
(`GoalHead.block_width :433`, stage.json `goals.columns.width`). `drawn` embedding rows: `12*29 + 1 = 349`.
Foresight outputs: 2 horizons + 1 (episode left) + 3 observation targets = 6. GRU gate rows: `3 x 128 = 384`.
Layout obs width check: warrior 1555 = core 715 + move 57 + vision 11 + entities 640 + map 4 + goal 128 (stage.json
block spans: `core [0,715]`, `move [715,57]`, `vision [772,11]`, `entities [783,640]`, `map [1423,4]`, `goal
[1427,128]`).

Critic, per flat row: `state [N,1958]` -> `RunningNorm` -> `Linear 1958->256`; plus the layout's adapter output
(`obs_i -> 256`) plus the camera join (`Linear 256->256` of the shared embedding) plus (if goals) the goal embedding
256; summed, then trunk `256->512->512`; GRU `512->128`; `head 128->1` (`networks.py:2300-2332, 2369`). The output is
a value in the normalised scale; `ValueNorm.denormalize` is applied in the rollout (`trainer.py:497`).

Update-time tensors (`_update_recurrent`, `trainer.py:1603`): rollout arrays `[T, E, A, ...]` with T = `rollout_length`
(128 in the yamls, `move1_controls.yaml:34`); with `chunk_length` L < T they are re-cut to `[L, (T/L)*E, A, ...]`
(`chunked`, `trainer.py:25`). M2 sets `chunk_length 128` (no chunking, `move2_seek.yaml`); M1 and the combat stages
differ
(M1: 32, combat1: 128). The minibatch is a subset of the (chunked) envs (`order = randperm(envs)`, `tensor_split` into
`minibatches` parts), flattened to `rows = steps * envs_here * agents`.

### M1 (`move1_controls`), from `fixtures/stage_move1_controls.json`

Same layouts but no map block and a `compass` block: warrior obs 1557 = core 715 + move 57 + compass 6 + vision 11 +
entities 640 + goal 128 (spans `core [0,715]`, `move [715,57]`, `compass [772,6]`, `vision [778,11]`,
`entities [789,640]`, `goal [1429,128]`), 95 actions (`core` 70 + `move` 25). Patch is 8; the vision scalars 11; the
look heads (7,5,5). The network has no `vision.map.*` keys (so fewer than 153 actor keys): the exact M1 key count is
UNVERIFIED (there is no golden for it; derive it by deleting the 9 `vision.map.*` keys from the M2 list, giving 144,
if M1's layout count and features are the same, which is also UNVERIFIED).

### Other live stages

Widths of combat1..3, group1/2 and dungeon1..3 (these have a sight block; the `combat1_fight.yaml:42-45` comment says
the enemies and friends are "the sight list (its own encoder and pointer heads)") are **UNVERIFIED**: no fixture holds
their `stage.json`. Derive them from `<layouts_dir>/<stage>/stage.json` written by the sim, or from the golden-style
procedure in `tests/test_golden_update.py` (do not run it from this document's reader's point of view unless intended).
M3 (`move3_interact`) and M4 (`move4_follow`) inherit M2's `mappo` section (`extends: move2_seek.yaml`).

## Switchable features

Config keys are `MappoConfig` fields (`trainer.py:41`) unless stated. "Live yaml" is what the checked-in configs set
(after `extends` resolution, `config.py`); per-machine confs and `--set` overrides are not visible here.

| Feature | Keys (default) | Live yaml | Depends on it |
|---|---|---|---|
| Style reward | `style.enabled` (false), `style.dataset`, `style.reference`, ... (`config.StyleConfig`, `config.py:271`) | `style: enabled: false` in `move1_controls.yaml:208` and `combat1_fight.yaml:172` | `style.py`, `human/motion.py`; the realism columns work with `style.reference` alone |
| Go-Explore | `explore.enabled` (false), `share`, `table_size`, `max_cells`, `depth_weight` (`config.ExploreConfig`, `config.py:321`) | `true`, `share 0.5` in `dungeon2_ragefire.yaml:35`; inherited by `dungeon3_deadmines` (no `explore` key there, checked); `false` in `group2_corridor.yaml:36`, inherited by `dungeon1_pulls` | `explore.py`, `ForgeEnv.set_explore_starts`, the wing episode-info columns |
| Rank sync | `rank_sync` ("gradients"), `weight_sync_every` (1) | not in any yaml. Injected by the worldserver: `LearnerProcess.cpp:138` passes `mappo.rank_sync=` the config's `DistSync` or "weights". The live cluster value is UNVERIFIED (per-machine conf, `ForgeConfig.h:212`) | `parallel.py` ("gradients", "weights"), `async_sync.py` ("async") |
| Rollout graphs | `rollout_graphs` (true) | not set; default | `_RolloutGraph`; only effective when `rollout_device` resolves to CUDA/HIP (worker GPUs are UNVERIFIED per [cluster.md](../cluster.md)) |
| Hindsight | `hindsight_coef` (0) | 0.1 in `move1_controls.yaml` and `combat1_fight.yaml` | `_achieved_of`, needs `goal_slots > 1` |
| Foresight | `foresight_coef` (0) and related | 0.25 in both bases | foresight head, `compute_foresight` |
| Goal lookahead | `goal_lookahead` (false), `lookahead_coef` (0.5) | true in both bases | `GoalHead.success/duration` |
| Slow goal loop | `slow_goal_size` (0) | 128 in both bases | `slow_memory`, `slow_value`, `slow_opt`, `_update_goals` |
| Look head | none (present whenever the vision block has `look.heads`); `look_entropy_coef` (None) | 0.001 (M1, combat), 0.004 (M2) | `LookHead`, `look_terms` |
| Chunked camera update | `vision_chunk_rows` (0) | 0 in M1; 2048 in M2 and combat1 | `_encode_vision`, `_backward_vision` |

The camera, entity list, map and sight list have **no switch**: they are on whenever the stage's `stage.json` has the
block (`trainer.py:620`).

## Observed issues

1. `MappoTrainer.state_dict` / `load_state_dict` (`trainer.py:2027-2053`) save `actor_opt`, `critic_opt` and
   `vision_opt` but not `slow_opt`. The slow goal loop's Adam state restarts on every resume, and every live stage
   has `slow_goal_size 128`. `reset_optimizers` (`:740`) builds `slow_opt`.
2. `set_learning_rate_scale` (`trainer.py:789-796`) scales `actor_opt`, `critic_opt` and `vision_opt` only. `slow_opt`
   keeps `slow_goal_lr` (0.0003) whatever `lr_final_fraction`/the controller's `lr_scale` say. Whether this is
   intended is UNVERIFIED (nothing says so).
3. `_update_recurrent`'s docstring says "The critic has the global state and stays feed-forward" (`trainer.py:1608`);
   the code replays the critic's GRU (`:1899-1903`). Comment and code disagree.
4. `GoalHead.ended()` (`networks.py:583`) has no caller anywhere in `animus/`, `tests/` or `tools/`: dead code.
5. `_graphs_apply` (`trainer.py:897`) is a one-line wrapper over `_graphs_off_reason` (`:883`) and is called only by
   tests; production code calls `_graphs_off_reason` via `_rollout_graph` (`:905`). See the trainer document.
6. `MappoTrainer.director_columns_clear` (`trainer.py:825`) is named after the removed director; it checks the camera
   blind columns. The name is kept because `tools/resume_check.py:242` calls it.
7. `update()` creates `_updates_since_sync` lazily with `getattr` (`trainer.py:1480`); it is not set in `__init__`.
8. The long comment explaining the foresight heads (`trainer.py:90-94`) sits above `recurrent_size` (`:102`), not above
   `foresight_coef` (`:173`).
9. `fixtures/seek_spec.json` is protocol 24 and `tiny_case` hardcodes `version=24` (`test_golden_update.py:79`) while
   `PROTOCOL_VERSION` is 25 (`protocol.py:14`). UNVERIFIED whether protocol 25 changed the SPEC layout the fixture
   mirrors; see [protocol.md](protocol.md).
11. The module docstring of `networks.py` (`:1-19`) says an adapter + trunk + head of one layout "is a plain MLP too
    (see
    animus.export)"; with the camera and the GRU this is no longer true, and `export.py:440-449` refuses a camera
    checkpoint. No live model can be exported to the realm format (the realm is parked).
12. The only test pinning M2's key set is the golden file; any intended shape change must regenerate it with
    `python tests/test_golden_update.py write` (the `__main__` block of that test).

## Reviewer notes

- The golden test (`tests/test_golden_update.py`) is the safety net for any refactor of `networks.py` or the update:
  it pins key names, shapes, one update's statistics and the parameter checksums on CPU. It does **not** exercise the
  CUDA/HIP graphs, the fused GRU path (CPU uses `_carry_sequence_loop`), `DenseLayouts`, `SharedInputDense`, two-stream
  updates or the chunked camera; those are covered only by tests that skip without a GPU (`test_rollout_graph.py`,
  `test_recurrent.py::test_fused_gru_*`, `test_update_on_two_streams...`).
- The by-name loading rules (what a checkpoint may lack) are in the networks document ("Loading").
- Dead features deleted: self-imitation, the map value-iteration network and the seat-set network (`EntitySets`) were removed;
  `SharedInputDense` (GPU only) is the one path left to judge.
