# Python learner: seeding, cast, partners, distillation

Part of [py-learner.md](py-learner.md). Covers `bootstrap.py` (903 lines), `cast.py` (326), `partners.py` (456),
`distill.py` (456), `stages.py` (115), and the seed logic in `train.py:276-389, 941-1118`. Line numbers: commit
`bd32b9dc8`. Networks (`LayoutActor`, `load_actor_state`) are in [py-mappo.md](py-mappo.md); the stage.json fields in
[file-formats.md](file-formats.md); the live stage order in [stages.md](stages.md).

## Which checkpoint a stage starts from

`TrainConfig.resolved_init_from(stage)` (`config.py:688`): with `init_from: auto` it is
`<runs_dir>/<name>/best.pt` for each name in stage.json `seed_chain` (closest first); otherwise the listed paths with
`{runs_dir}`, `{run_name}`, `{shared_runs}` filled in. `merge_from` likewise from stage.json `merges`.
`finetune_from` (default `{runs_dir}/_finetune/{run_name}/best.pt`) is prepended when that file exists
(`train.py:976-981`).

`init_from_checkpoint(path, prefer)` (`train.py:276`): a candidate whose file name is `best.pt` or `latest.pt` is
replaced by the run's `prefer` file (`seed_from`, default `latest`; all live yamls use `latest`), falling back to the other, or None. Any other path is taken if it exists. So a path written as
`best.pt` yields `latest.pt` by default. This is intended, but comments
disagree:
`config.py:627-630` ("a best.pt that does not exist falls back to the latest.pt") and `config.py:649-650` ("each merged
stage's best.pt (else latest.pt)") understate it. It also applies to `finetune_from` and (with the default prefer)
named `distill.teachers` (`train.py:373`, `1027`). Partners are different: a bare stage name means `latest.pt` if it
exists, and `name:best` / `name:latest` ask for that file (below).

`TrainingRun.seed_candidate` (`train.py:1093`): the first candidate that exists is loaded (`load_parent`: also attaches
the run directory's stage.json if the checkpoint carries none). On the automatic chain (`init_from == "auto"`) a
checkpoint that lacks some of this run's layouts is stepped over and remembered in `restricted` (a restricted stage
such as a stealth drill that played only some classes); with an explicit path it is never stepped over and
`seed_trainer` raises instead.

Then (fresh start only, `train.py:996-1010`): `seed_trainer(trainer, base, spec, stage)`; `seed_merges` for the
`merge_from` checkpoints; for each restricted checkpoint, farthest first, `seed_trainer(..., overlay=True)`. If nothing
was found but candidates were listed it prints "starting from scratch". A resume skips all of it.

## `seed_trainer` (`bootstrap.py:673`)

Works on clones of the new actor's and critic's state dicts and loads them back at the end (then
`trainer._sync_rollout()`, a private call; `train.py` itself calls the public `sync_rollout`).

Non-overlay (the base): `_seed_shared` copies every key that starts with `trunk.` (must exist and match in shape, else
`ValueError`), and `memory.`, `goal_head.`, `goal_embedding.` where the old checkpoint has the key at the same shape
(silently skipped otherwise). The same for the critic. Then, each only where the new network has the part:
`_seed_vision` (camera encoder:
copied only if the old checkpoint has it, the vision block revision is equal in both stage.jsons and every shape equal;
otherwise fresh with the join zeroed), `_seed_map` (key by key, join zeroed when fresh -- and, since exploration v3, also when any encoder tensor is fresh even though the join's own shape agreed: a six-channel map's `patch`/`embed` shapes differ from the seven-channel one's, and a carried join would read a fresh encoder's output through weights trained on another; the seed from M1, which has no map, is unchanged), `_seed_sight`
(sight encoder and pointer queries, the pool zeroed when fresh; fresh too when the sight block revision differs from the checkpoint's, entity sensing's 2 -> 3; a narrower `sight.extra` is widened with zeros),
`_seed_look` (look head, same rule as the camera). Lines are printed for each.
**Goal block revision change** (`_reseed_goals`, m2-goals W3b, after `_seed_shared`): when the checkpoint's stage.json and
this stage's give the `goal` block different revisions (M2's room goals, 2 -> 3), `goal_head.*` (and `slow_memory.*`,
`slow_value.*`, which `SHARED_PREFIXES` never carries; the slow optimiser always starts fresh) are put back to the new
networks' own initial values -- the old head chose among the old block's offers and may have collapsed -- while
`goal_embedding.*` is kept, and the Fight/none rows (kind `fight`, target 0) are copied into kind `travel_to` and the
place targets `goals.place_slots.first ..+count-1` of the actor's `kind`, `target`, `kind_scale`, `target_scale` tables
and the critic's `kind`, `target`, wherever the destination row is still all zero (never trained). Printed:
the revisions, the number of tensors reset, the rows copied. Same revision, an absent revision on either side (no
stage.json) or an overlay seed: exactly as before. A manifest without `place_slots` resets but copies nothing.
The new goal-block columns reach the adapter at zero weight (the block's revision change makes `_common_blocks` start it
fresh, then by name: `_seed_adapter_blocks` zeroes what is not carried), and the pointer's last layer is zero.
**Revision 3 -> 4** (free-choice-goals, cell goals) and **4 -> 5** (search-kind) are the pairs that **keep** the head: `goal_head.*` comes with the
shared weights, `slow_memory.*` and `slow_value.*` are copied from the checkpoint (`_reseed_goals(..., old_actor)`; they
are not in `SHARED_PREFIXES`), `goal_head.cell.*` is absent from the checkpoint and stays as initialised (score and chain
at zero: uniform over the choosable blocks), and the embeddings are warm-copied for the cell goal's target
(`goals.cells.target`, 21, appended to `_goal_ids`' place targets) wherever the row is still zero. Printed: what was kept
and what is fresh. Any other revision pair, including 2 -> 4 and 3 -> 5, resets as above.

**A grown goal space** (search-kind, goal block revision 5: the tenth kind `search`; `_grow_goal_kinds`, before `_seed_shared`).
The kind count is read off the checkpoint's `goal_head.kind.weight` against the new network's. Equal: nothing is done (every
other seed). Fewer in the checkpoint (9 -> 10): kinds are only appended, so a joint id `kind * 23 + target` keeps its row; the
checkpoint's kind-indexed goal tensors (`networks.GOAL_KIND_ROW_KEYS`: `goal_head.kind.*`, `pair`, `drawn.weight`, `accepts`,
the lookahead's `success.*`/`duration.*`, `goal_embedding.kind` and `kind_scale`; the critic's `goal_embedding.kind`) are
padded to the new shapes with the new network's own rows (`networks.grow_goal_rows`), so `_seed_shared` carries them like any
other; then each new kind named in `GOAL_KIND_WARM` (`search` <- `travel_to`) takes its source's rows (kind embedding and
scale, kind logit and bias, the pair row, the joint ids' `drawn` embeddings, the lookahead's rows; actor and critic), so a head
that chose `travel_to` about a place now chooses `search` about it with the same logit, and the policy it conditioned reads the
goal as it did. The kind-count logic is by tensor shape, so it also covers a checkpoint whose stage.json predates the manifest;
the names (`goals.kinds`) are used for the warm copy and checked: the checkpoint's must be the first of the stage's. **More**
kinds in the checkpoint than the stage (a newer checkpoint into an older sim) is refused (`ValueError`, "cannot seed a sim with
fewer goal kinds"); so are kind lists that are not a prefix. Printed: the kinds, the tensors grown, the warm copies. The goal
block's columns that moved are carried by name; `goal_kind_search` and `goal_achieved_kind_search` are new names and reach
the adapter at zero. A multi-revision seed that grows the space while the old `travel_to` embedding row is still zero (a
checkpoint of revision 2) warm-copies zeros: `search` starts untrained, as `travel_to` was. Resume: `MappoTrainer.load_state_dict`
and `load_actor_state` pad a smaller goal space the same way, tolerantly (new rows as initialised, the optimisers then start
fresh, a line printed); a resume of a revision-4 checkpoint is refused anyway by `layout_changes`. Teachers and partners
(`distill.frozen_actor`, `cast`) are rebuilt in their checkpoint's own 9-kind space and decide their goals in it; ids of the
first nine kinds are unchanged, so a 9-kind partner's goals mean the same on the wire (a 9-kind cell goal, `travel_to / 21`,
is not offered by a `Seek.SearchGoals` 1 sim and ends lost). The new columns (108..121) are new names and
reach the adapter at zero. A new M2 run is a seeded start, not a resume (protocol 28 and the wider block): `forge` overlay
`configs/overlays/move2_seek_cells.yaml`.
Everything else stays freshly initialised: critic state encoder and value head, the value normaliser, and any module
not named above (UNVERIFIED which actor modules that includes: foresight and slow-goal modules are not in
`SHARED_PREFIXES`; check `mappo/networks.py` key names).

A layout the checkpoint lacks raises `ValueError` unless `overlay` (`:717-724`).

Per layout (matched by name, `:726-769`): adapters (actor and critic), observation normalisers
(`norms.<i>.mean/var/count`)
and the actor head. With block spans in both stage.jsons (`stages.block_spans`) the move is block by block through
`_layout_segments`; without them (older runs) `_seed_adapter`/`_seed_norm`/`_seed_head` copy a prefix.

### Segments (`_layout_segments`, `:646`)

`_common_blocks` yields (old spans, new spans) for every block both layouts have:
- Revision differs (stage.json `blocks[].revision`): the block starts fresh (printed), later possibly carried by name.
- Same size: whole block copied.
- Size changed: the
  `core` block with both action-name lists is matched action by action (`_core_by_name`, using
  `CORE_GLOBAL_FEATURES = 91`, per-action features from stage.json `action_features` default 6, `CORE_ACTION_FEATURES =
8`,
  rank-tier actions named `rank_*` closing the block); a block with a seat set that only gained slots uses
  `_slots_grown`; otherwise the block is seeded from scratch and the rest still carries.
- `_by_name` then fills still-empty new columns and actions from old columns of the same name in any block
  (`obs_names`; a revision-4 move block of 63 columns uses `MOVE_REVISION_4_COLUMNS`, the compass split).

`CORE_GLOBAL_FEATURES` must equal `CoreBlock::OBS_GLOBAL_COUNT` in C++ (`bootstrap.py:60-66`); nothing
checks it since the tests were removed 2026-10-07. It was once 67 when the C++ had 94.

Adapter weights for the segments move (`_seed_adapter_blocks`: new weights zeroed, then segments copied, bias copied);
head rows move (`_seed_head_blocks`); normalisers move per feature with `count` capped at `SEED_COUNT_CAP = 16384` if
the
block gained features (`_seed_norm_blocks`), rescaled columns (stage.json `rescaled` tags the parent lacks) restart at
mean 0 var 1 (`_seed_rescaled_norms`), new seat-set slots copy the last old slot's statistics.

## `seed_merges` (`bootstrap.py:822`)

After the base: for each layout, the blocks that the base's stage.json does not have ("wanted"), from each merged
checkpoint in order: whole blocks via `_common_blocks`; then `_merge_by_name` fills named columns and actions that no
earlier source filled (G1 takes M4's party frames at revision 1 into revision 2's). Adapter columns, observation
statistics (mean, var; not count) and head rows move; the trunk and biases never. Returns `{layout: blocks}`.

## `seed_from` summary: what carries, what is fresh

| Part | Carried from the parent | Fresh |
|---|---|---|
| Trunk (actor, critic) | yes, shapes must match | |
| GRU memory, goal head, goal embedding | if present at the same shape | else fresh |
| Per-layout adapter columns and head rows | by block and by name; layouts by name | new blocks' columns zero, new actions small init |
| Observation normalisers | per feature with the weights | new features mean 0 var 1, count capped |
| Entity sets, camera, map, sight, look | when shapes and vision revision agree | else fresh (pool or join zeroed) |
| Critic state encoder, value head, value norm | | always fresh |
| Optimisers, update and env counters, tracker, controller | | always fresh (counters start at 0) |
| A merged stage's blocks | adapter columns and head rows only | trunk stays the base's |
| A restricted stage's layouts | overlay of adapters, norms, heads | trunk stays the base's |

Seeding does not carry the partner pool, the cast, or `best.pt` scores.

## Resume versus seed

Resume (`train.py:949-972`) loads everything via `MappoTrainer.load_state_dict` (optimisers too) after the shape and
layout
checks, plus `restore_evaluation_state`. Loader tolerance for old keys: `load_actor_state` ignores blind-column masks,
pads goal tensors of a smaller goal space (`grow_goal_rows`), zeroes the missing goal-scale parameters (`_GOAL_SCALE_KEYS`) and raises on any other missing or unexpected actor key;
the critic is loaded non-strictly with the same exception list and raises likewise; `value_norm` and optimisers are
loaded
only if present; top-level keys use `.get` defaults (`update`, `env_steps`, `convergence`, `controller`, `score_kind`,
`style`, `partner_scores`). The config saved in a checkpoint is not compared with the current one by the learner.

## Cast (`cast.py`)

A stage declares cast agents in stage.json `cast` (entries with `name` and `agent` seat index). `cast.agents`
(`{name: checkpoint path}`) names the frozen checkpoint for each. `train.py:1037-1041` raises if the stage declares an
`agent` entry with no path. `Cast.rows(step)` returns the present rows of those seats; `act_and_look` replaces the live
actions (and look) for them using `CastActor.decide`. Those rows are not samples. No live yaml sets `cast.agents`
(UNVERIFIED for dungeon3: the grep output was cut); M4's leader is still scripted in the sim.

`CastActor` (`cast.py:28`): loads a checkpoint and builds a `Teacher` (`distill.build_teacher`), so the checkpoint's
observation columns and actions are mapped onto the stage's by block name. Per row memory, goal, goal age, slow memory
and goal queue are host arrays, cleared with the episode (`clear`) or all at once (`reset_all`, which just drops the
arrays). `decide` runs per layout: gather rows, build the teacher's padded obs and mask, features, goal decision
(`decide_goals`) when the checkpoint has goals, distribution (sampled, or argmax if `deterministic`), map teacher action
back to the stage's index (`back`), and the look from its look head or `look_hold`. A row whose layout the checkpoint
lacks, or with no legal mapped action, keeps the fallback action and is counted in `fallback_rows`.
`Residency(limit)` offloads the least recently used frozen actors to the CPU (`place`) past a cap; only partners use it
(`Cast` does not, though a docstring mentions a `CastPool` that does not exist, `cast.py:44`).

Config: `cast.agents`, `cast.deterministic`.

## Partners (`partners.py`, `config.PartnerConfig`)

Purpose: in a share of party episodes some seats are played by frozen checkpoints so the learner works with anyone.
Only arenas whose stage.json `plan` is `party` or `raid` (`PARTY_PLANS`).

Members: `cast.partners.stages` (names) and `.paths`, resolved by `PartnerConfig.resolve` (`config.py:443`): a text with
`/` or ending `.pt` is a path (with `{runs_dir}`, `{run_name}` filled in); a bare stage name is
`<runs_dir>/<stage>/latest.pt` if that file exists, else `best.pt`; `<stage>:best` and `<stage>:latest` are that file
exactly (a missing one is skipped later with a line). `__post_init__` refuses any other word after a colon
(`PARTNER_CHECKPOINTS`). Plus snapshots of the run itself in `<run_dir>/partners/*.pt`, added when
`snapshot_every_env_steps` is crossed (`latest.pt` copied as `step_<env_steps>.pt`) and whenever `best.pt` improves
(`best_<env_steps>.pt`). `eval_partners` overrides the members used by the two evaluation arms.

`PartnerPool.add` builds the frozen actor at once so an unusable checkpoint is reported (`unusable`) and a missing file
(`missing`) is listed; duplicates by path are ignored. `prune` keeps `pool_size` snapshots: retires the best-carried
first (highest normalised score) except the newest `keep_newest`. A retired member keeps its loaded actor (no release).

Draw weights (`probabilities`): each member's `score` is the exponential moving average (alpha `1/min(episodes,
rate_window)`) of the party score (mean of `partners.score` column, default `score_outcome`, else `won`, over the live
seats) in episodes it partnered. Normalised across the pool: best 1, worst 0, unmet 0, one met 0.5, all equal 0.5.
Weight
`(1 - normalised)^2 + floor`; if `newest_share > 0` the newest snapshot's probability is raised to at least that share.
`draw_for(layout)` draws among active members whose checkpoint has that layout.

`Partners.draw` (called from `rows()` at an env's first decision of an episode, `fresh` flag): first the stand-in seats
(rows the sim marked `present == 2`): each gets a member whatever `share` is; then, with probability `share`, up to
`max_partners` other present seats of a party env (never a drill seat from stage.json `drill_seat`, never the
stand-in's,
always leaving one live seat) get members. Rows with members are masked out of the training samples in `_act_on_rows`.
`observe_ended` scores each member that partnered an ended episode. `clear` and `reset_all` reset memories and redraw.
Counters feed the metrics columns `partner_rows`, `partner_fallback_rows`, `partner_members`, `partner_episodes`,
`stand_in_episodes`, `stand_in_unfielded`.

The "human" stand-in: the sim picks a seat (present 2); the learner plays it with a pool member and never trains on it.
The sim fields one only if the learner's MODE says it can (`can_field_stand_in`: a party stage and an active member).
`field_stand_in()` also writes the progress note. `partners.json` (`PartnerPool.write`) is informational. The pool's scores, episode counts and retired flags are saved in the checkpoint
(`partner_scores`, `PartnerPool.scores_state`, keyed `<kind>:<name>`) and put back on the members present at a resume
(`restore_scores`); a checkpoint without the key (older) restarts the pool unmet, as before.

Evaluation arms (`train.py:1420-1479`): `with_partners` (share 1) and `with_human` (share 0, stand-in only) build their
own
`Partners` with the eval members, argmax, no snapshots, seeded `eval.seed * 1000 + rank`; their rows are `excluded` from
scoring (`with_partners_chooser`). Readings only.

Config: `cast.partners.{stages, paths, snapshot_every_env_steps, newest_share, share, max_partners, pool_size,
keep_newest, rate_window, floor, score, deterministic, eval_partners, resident_members}`. `enabled` is true only with
`share > 0` and some source.

## Distillation (`distill.py`)

Config `distill`: `teachers` ("" off, "auto", or `{arena: path}`), `coef`, `half_life_env_steps`, `min_coef`.
`coef_at(env_steps) = max(min_coef, coef * 0.5^(env_steps/half_life))`. The learner sets `distiller.coef` every
rollout (`train.py:1743`). No live yaml sets `distill`.

`make_distiller` (`train.py:364`) picks `auto_teachers(stage, parents)` (each arena goes to the first parent whose own
stage.json has that arena) or the named checkpoints; `build_teacher(checkpoint, spec, stage, device)` rebuilds the
parent's actor from its own saved config and stage.json (`frozen_actor`, frozen, eval mode), maps layouts by name and
columns by block (`_index_pairs`: blocks with equal sizes; a revised block is dropped from the mapping),
and runs `check_camera` for camera teachers. `Distiller(stage, teachers)` needs the stage's `state.arena_first` span and
**refuses any teacher with a camera** (`distill.py:256-259`). Every live stage has a vision
block (`move1_controls` and `move2_seek` stage.json: all 10 layouts), so distillation is unusable in the live
curriculum,
and the restricted-stage overlay (`train.py:1019-1024`) would raise the same error.

In the update the trainer calls only `Distiller.sequence_loss` (`mappo/trainer.py:1753-1763`) and refuses an auxiliary
without it (`:1499`). It returns `coef * mean KL(teacher || policy)` over taught rows (the arena's rows, over actions
both have and the stage's mask allows, renormalised). Recurrent teachers are replayed in order per sequence.
(`Distiller.__call__`, `.kl` and `.begin_sequence`, never called in production, were deleted.)

## `stages.py` (the stage.json reader)

`load_stage(layouts_dir, scenario)` reads `<layouts_dir>/<scenario>/stage.json` or None. Helpers: `seed_chain`,
`merges`, `arena_names`, `arena_state_span`, `block_spans` (name to (obs span, action span)), `block_revisions`,
`revised_blocks`, `layout_signature` (sha1 of block names, spans, revisions, first 12 hex), `layout_changes` (the
resume guard: one text entry per layout whose signature differs, empty if either side has no spans), `model_names`.

## Observed issues

- `bootstrap.py:773` calls the private `trainer._sync_rollout()`; `train.py` uses `sync_rollout`.
- `bootstrap.py:60-66`: a hand-kept constant that must equal a C++ constant; guarded only by a test that reads the
  header.
- `cast.py:44` mentions `CastPool`, which does not exist.
- `partners.py:246`: retired members keep their actors (device memory) for the rest of the run.
- (Fixed) Pool scores survive a resume through the checkpoint's `partner_scores`; only a checkpoint saved before it restarts the pool "unmet" (normalised 0). The cast keeps only counters and is not saved.
- `config.py:649-650` and `:627-630` comments contradict `init_from_checkpoint` (see above).
- `distill.py`: `stage21_ship` mentioned in a comment; unusable with cameras.
- `partners.py:323-355`: an episode where only the stand-in seat is assigned (share draw skipped) is not counted in
  `episodes_with` (`:355` is after the `continue`).
