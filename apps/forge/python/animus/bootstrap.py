"""Seed a curriculum stage's networks from the stage it extends.

Networks are layout-aware (animus.mappo.networks): a per-layout input adapter, a shared trunk and (actor) a
per-layout action head. Layouts are matched by name (the class). A stage keeps some of its base's blocks, may
drop others and adds its own (the curriculum is a tree), so a layout is seeded block by block:

- Input adapters (actor and critic): each kept block's feature columns move to where the block sits now; new blocks'
  columns start at zero, so the seeded policy initially ignores them; dropped blocks' columns are left behind.
- Actor heads: each kept block's action rows move the same way; new actions keep their small initial weights.
- Trunk: copied (the hidden sizes must match).
- A layout the checkpoint does not have is refused, loudly. It used to keep its fresh adapter and head and take the
  copied trunk, which is silent and almost always wrong: a stage only some classes play (a stealth drill) leaves a
  partial checkpoint, and init_from: auto takes the first checkpoint in the chain that exists -- so the classes that
  did not play it would start from random weights with nothing said. The sim used to prevent this by refusing to let
  anything extend such a stage at all, which was too blunt: in a run of one class that plays it, the checkpoint
  covers every layout and there is nothing partial about it. Only here are the run's actual layouts known, so this is
  where the question can be asked properly.
- Critic state encoder and value head: kept freshly initialised, and the value normaliser is not copied, because
  the later stage's global state and reward differ.

Block positions come from the stages' stage.json (``layouts``), which every checkpoint carries. A checkpoint or stage
without them (older runs, standalone scenarios) is seeded as a prefix: the earlier layout's columns and rows first.

A merge stage (stage.json ``merges``) joins branches of the curriculum: after the stage it extends has seeded the
trunk and its blocks, each merged stage seeds the blocks of every layout that neither the extended stage nor an
earlier merge had (seed_merges) -- input columns and action rows only, never the trunk or the biases. Those weights
were trained against the merged stage's own trunk, so they are a warm start; distillation (animus.distill) aligns them.
"""

from __future__ import annotations

import torch

from .stages import Span, block_spans

#: The director layout's name (Curriculum::DirectorLayout::Name).
DIRECTOR_LAYOUT = "director"


def _seed_adapter(new: dict, old: dict, prefix: str) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    if new_w.shape[0] != old_w.shape[0]:
        raise ValueError(f"{prefix}: width {old_w.shape[0]} in the checkpoint, {new_w.shape[0]} now")
    if old_w.shape[1] > new_w.shape[1]:
        raise ValueError(f"{prefix}: {old_w.shape[1]} inputs in the checkpoint do not fit in {new_w.shape[1]}")
    new_w.zero_()
    new_w[:, : old_w.shape[1]] = old_w
    new[f"{prefix}.bias"].copy_(old[f"{prefix}.bias"])


def _seed_head(new: dict, old: dict, prefix: str) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    rows = old_w.shape[0]
    if new_w.shape[0] < rows or new_w.shape[1] != old_w.shape[1]:
        raise ValueError(f"{prefix}: {tuple(old_w.shape)} does not fit in {tuple(new_w.shape)}")
    new_w[:rows] = old_w
    new[f"{prefix}.bias"][:rows] = old[f"{prefix}.bias"]


# The core block's observation layout (CoreBlock.h): OBS_GLOBAL_COUNT global features, ACTION_FEATURES per catalog
# action in action order, then the talents and trees. When a catalog loses or gains spells (a spell rule changed), the
# core block is seeded action by action by name (stage.json action_names).
CORE_GLOBAL_FEATURES = 67
CORE_ACTION_FEATURES = 6


def _core_by_name(old_spans: tuple[Span, Span], new_spans: tuple[Span, Span], old_names: list[str],
                  new_names: list[str]):
    """Segments of a core block whose catalog changed: the globals, each action both catalogs name (its features and
    its action row) and the talents after them; None when the rest of the block does not line up."""
    (old_obs, old_actions), (new_obs, new_actions) = old_spans, new_spans
    old_catalog = old_names[old_actions[0] : old_actions[0] + old_actions[1]]
    new_catalog = new_names[new_actions[0] : new_actions[0] + new_actions[1]]
    if len(old_catalog) != old_actions[1] or len(new_catalog) != new_actions[1]:
        return None

    old_tail = old_obs[1] - CORE_GLOBAL_FEATURES - old_actions[1] * CORE_ACTION_FEATURES
    new_tail = new_obs[1] - CORE_GLOBAL_FEATURES - new_actions[1] * CORE_ACTION_FEATURES
    if old_tail != new_tail or old_tail < 0:
        return None

    # A segment is (old spans, new spans) like a whole block's; a count of 0 moves nothing.
    def segment(old_obs_first, new_obs_first, obs_count, old_action, new_action, action_count):
        return (((old_obs_first, obs_count), (old_action, action_count)),
                ((new_obs_first, obs_count), (new_action, action_count)))

    segments = [segment(old_obs[0], new_obs[0], CORE_GLOBAL_FEATURES, 0, 0, 0)]
    where = {action: position for position, action in enumerate(old_catalog)}
    for position, action in enumerate(new_catalog):
        if action in where:
            old_position = where[action]
            segments.append(segment(old_obs[0] + CORE_GLOBAL_FEATURES + old_position * CORE_ACTION_FEATURES,
                                    new_obs[0] + CORE_GLOBAL_FEATURES + position * CORE_ACTION_FEATURES,
                                    CORE_ACTION_FEATURES, old_actions[0] + old_position, new_actions[0] + position, 1))
    segments.append(segment(old_obs[0] + CORE_GLOBAL_FEATURES + old_actions[1] * CORE_ACTION_FEATURES,
                            new_obs[0] + CORE_GLOBAL_FEATURES + new_actions[1] * CORE_ACTION_FEATURES,
                            new_tail, 0, 0, 0))
    return segments


def _common_blocks(old: dict[str, tuple[Span, Span]], new: dict[str, tuple[Span, Span]], name: str,
                   old_names: list[str] | None = None, new_names: list[str] | None = None):
    """(old spans, new spans) of every block both layouts have, sizes checked. A core block whose catalog changed is
    matched action by action by name, when both stages name their actions."""
    common = []
    for block, (new_obs, new_actions) in new.items():
        if block not in old:
            continue
        old_obs, old_actions = old[block]
        if old_obs[1] != new_obs[1] or old_actions[1] != new_actions[1]:
            segments = None
            if block == "core" and old_names and new_names:
                segments = _core_by_name((old_obs, old_actions), (new_obs, new_actions), old_names, new_names)
            if segments is None:
                # A block that changed shape cannot be copied column by column, but it is the only part of the
                # layout that cannot: seeding everything else and leaving this one to start from nothing is worth
                # far more than refusing the whole checkpoint. It reaches the trunk at zero and has to be learned,
                # which is the honest cost of having changed it.
                print(f"  {name}: block {block} was {old_obs[1]} features and {old_actions[1]} actions, is "
                      f"{new_obs[1]} and {new_actions[1]}: seeded from scratch, the rest of the layout carries "
                      f"over", flush=True)
                continue
            common.extend(segments)
            continue
        common.append(((old_obs, old_actions), (new_obs, new_actions)))
    return common


# Rows of observation a seeded normaliser is treated as having seen, for a block that gained features. About one
# rollout: the new columns are half described after a single update and all but settled after a handful, while the
# inherited ones barely move. See _seed_norm_blocks.
SEED_COUNT_CAP = 16384.0


def _action_names(stage: dict | None, layout: str) -> list[str]:
    return list(((stage or {}).get("layouts", {}).get(layout) or {}).get("action_names", []))


def _seed_adapter_blocks(new: dict, old: dict, prefix: str, common) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    if new_w.shape[0] != old_w.shape[0]:
        raise ValueError(f"{prefix}: width {old_w.shape[0]} in the checkpoint, {new_w.shape[0]} now")
    new_w.zero_()
    for ((old_first, count), _), ((new_first, _), _) in common:
        new_w[:, new_first : new_first + count] = old_w[:, old_first : old_first + count]
    new[f"{prefix}.bias"].copy_(old[f"{prefix}.bias"])


def _seed_norm_blocks(new: dict, old: dict, prefix: str, common) -> None:
    """Carry an observation normaliser's statistics across, feature by feature, for the blocks both stages have.

    They are per input feature, so they remap exactly like the adapter columns beside them. A feature the new
    stage adds keeps its starting mean 0 and variance 1 until the first rollouts describe it.

    `count` is one scalar for the whole normaliser while mean and var are per feature, so it cannot say "certain
    about these columns, ignorant of those". Inheriting it whole applies the parent's confidence to features that
    have never been observed: RunningNorm.update moves the mean by batch_count / (count + batch_count), so a
    rollout's rows against a parent's tens of millions move a new feature by a fraction of a percent per update,
    and it spends most of the stage feeding tanh a value it never learned the scale of. Where the block gained
    features, the count is capped so the new columns are described within the first few rollouts; the inherited
    columns are already close, so re-weighting them towards new data costs nothing. It stays above zero because
    RunningNorm.forward passes rows through raw at count 0, which would throw the seeded statistics away for a
    rollout."""
    for name in ("mean", "var"):
        new_stat, old_stat = new[f"{prefix}.{name}"], old[f"{prefix}.{name}"]
        new_stat.copy_(torch.zeros_like(new_stat) if name == "mean" else torch.ones_like(new_stat))
        for ((old_first, count), _), ((new_first, _), _) in common:
            new_stat[new_first : new_first + count] = old_stat[old_first : old_first + count]

    carried = sum(count for ((_, count), _), _ in common)
    inherited = old[f"{prefix}.count"]
    gained = carried < new[f"{prefix}.mean"].numel()
    new[f"{prefix}.count"].copy_(inherited.clamp(max=SEED_COUNT_CAP) if gained else inherited)


def _seed_norm(new: dict, old: dict, prefix: str) -> None:
    """The same statistics, when the layouts line up feature for feature."""
    for name in ("mean", "var", "count"):
        if new[f"{prefix}.{name}"].shape == old[f"{prefix}.{name}"].shape:
            new[f"{prefix}.{name}"].copy_(old[f"{prefix}.{name}"])


def _seed_head_blocks(new: dict, old: dict, prefix: str, common) -> None:
    new_w, old_w = new[f"{prefix}.weight"], old[f"{prefix}.weight"]
    if new_w.shape[1] != old_w.shape[1]:
        raise ValueError(f"{prefix}: {tuple(old_w.shape)} does not fit in {tuple(new_w.shape)}")
    new_b, old_b = new[f"{prefix}.bias"], old[f"{prefix}.bias"]
    for (_, (old_first, count)), (_, (new_first, _)) in common:
        new_w[new_first : new_first + count] = old_w[old_first : old_first + count]
        new_b[new_first : new_first + count] = old_b[old_first : old_first + count]


#: Weights that are not per layout and whose shape does not depend on the stage: the shared trunk, the GRU that
#: carries memory between decisions, and the goal head and its embedding. All of them read the trunk's output, whose
#: width every stage shares, so a stage can inherit them from the one it extends. Left fresh, a stage that inherits
#: the trunk would still relearn how to remember and what its goals mean from scratch.
SHARED_PREFIXES = ("trunk.", "memory.", "goal_head.", "goal_embedding.")


def _seed_shared(new: dict, old: dict) -> None:
    for key, tensor in new.items():
        if not key.startswith(SHARED_PREFIXES):
            continue
        if key.startswith("trunk."):
            if key not in old or old[key].shape != tensor.shape:
                raise ValueError(f"{key}: the trunk in the checkpoint does not match (hidden sizes must be equal)")
            tensor.copy_(old[key])
        elif key in old and old[key].shape == tensor.shape:
            # A checkpoint from before these existed, or from a stage with the part turned off, simply leaves the
            # fresh weights alone rather than failing: the trunk is what a stage must agree on.
            tensor.copy_(old[key])


def seed_trainer(trainer, checkpoint: dict, spec, stage: dict | None = None) -> list[str]:
    """Seed a fresh MappoTrainer for `spec` (whose stage.json is `stage`) from an earlier stage's checkpoint; returns
    the layouts seeded."""
    old_names = [layout["name"] for layout in checkpoint["spec"].get("layouts", ())]
    old_stage = checkpoint.get("stage")
    old = checkpoint["trainer"]

    actor = {key: tensor.clone() for key, tensor in trainer.actor.state_dict().items()}
    critic = {key: tensor.clone() for key, tensor in trainer.critic.state_dict().items()}

    _seed_shared(actor, old["actor"])
    _seed_shared(critic, old["critic"])

    # Every layout this run has must be in the checkpoint it is seeding from. A missing one is not a thing to work
    # around quietly: the alternative is starting that class from scratch in the middle of a curriculum, which looks
    # exactly like a class that has simply not learned anything yet.
    # The director is the exception, and the only one: it is an agent the stage adds rather than a class the run
    # plays, so the first directed stage in a chain necessarily seeds from one without it.
    missing = [layout.name for layout in spec.layouts
               if layout.name not in old_names and layout.name != DIRECTOR_LAYOUT]
    if missing:
        raise ValueError(
            f"the checkpoint has no {', '.join(missing)}: it was trained on "
            f"{', '.join(old_names) or 'nothing'}. Seeding would start "
            f"{'them' if len(missing) > 1 else 'it'} from random weights in the middle of the curriculum. Train "
            f"this stage with the classes the checkpoint has, or seed from one that covers the classes this run "
            f"plays (init_from).")

    seeded = []
    for index, layout in enumerate(spec.layouts):
        if layout.name not in old_names:
            continue
        old_index = old_names.index(layout.name)
        seeded.append(layout.name)

        # Checkpoint keys use the checkpoint's own layout order.
        adapters = [(network, {
            f"adapters.{index}.weight": old_network[f"adapters.{old_index}.weight"],
            f"adapters.{index}.bias": old_network[f"adapters.{old_index}.bias"],
        }) for network, old_network in ((actor, old["actor"]), (critic, old["critic"]))]
        # The observation statistics belong to the same features as the adapter columns.
        norms = [(network, {
            f"norms.{index}.{name}": old_network[f"norms.{old_index}.{name}"]
            for name in ("mean", "var", "count")
        }) for network, old_network in ((actor, old["actor"]), (critic, old["critic"]))
            if f"norms.{old_index}.mean" in old_network]
        head = {
            f"heads.{index}.weight": old["actor"][f"heads.{old_index}.weight"],
            f"heads.{index}.bias": old["actor"][f"heads.{old_index}.bias"],
        }

        old_blocks = block_spans(old_stage, layout.name)
        new_blocks = block_spans(stage, layout.name)
        if old_blocks is not None and new_blocks is not None:
            common = _common_blocks(old_blocks, new_blocks, layout.name, _action_names(old_stage, layout.name),
                                    _action_names(stage, layout.name))
            for network, remapped in adapters:
                _seed_adapter_blocks(network, remapped, f"adapters.{index}", common)
            for network, remapped in norms:
                _seed_norm_blocks(network, remapped, f"norms.{index}", common)
            _seed_head_blocks(actor, head, f"heads.{index}", common)
        else:
            for network, remapped in adapters:
                _seed_adapter(network, remapped, f"adapters.{index}")
            for network, remapped in norms:
                _seed_norm(network, remapped, f"norms.{index}")
            _seed_head(actor, head, f"heads.{index}")

    trainer.actor.load_state_dict(actor)
    trainer.critic.load_state_dict(critic)
    trainer._sync_rollout()
    return seeded


def seed_merges(trainer, merges: list[dict], spec, stage: dict | None, base: dict) -> dict[str, list[str]]:
    """After seed_trainer from `base` (the extended stage's checkpoint), seed each layout's blocks that only the merged
    stages' checkpoints (`merges`, in order) have; returns {layout: [block, ...]} for what was seeded."""
    actor = {key: tensor.clone() for key, tensor in trainer.actor.state_dict().items()}
    critic = {key: tensor.clone() for key, tensor in trainer.critic.state_dict().items()}
    seeded: dict[str, list[str]] = {}

    for index, layout in enumerate(spec.layouts):
        new_blocks = block_spans(stage, layout.name)
        if new_blocks is None:
            continue
        taken = set(block_spans(base.get("stage"), layout.name) or {})

        for merge in merges:
            names = [entry["name"] for entry in merge["spec"].get("layouts", ())]
            old_blocks = block_spans(merge.get("stage"), layout.name)
            if layout.name not in names or old_blocks is None:
                continue
            old_index = names.index(layout.name)
            wanted = {block: spans for block, spans in new_blocks.items() if block not in taken}
            common = _common_blocks(old_blocks, wanted, layout.name, _action_names(merge.get("stage"), layout.name),
                                    _action_names(stage, layout.name))
            if not common:
                continue

            old = merge["trainer"]
            for network, old_network in ((actor, old["actor"]), (critic, old["critic"])):
                new_w, old_w = network[f"adapters.{index}.weight"], old_network[f"adapters.{old_index}.weight"]
                if new_w.shape[0] != old_w.shape[0]:
                    raise ValueError(f"adapters.{index}: width {old_w.shape[0]} in a merged checkpoint, "
                                     f"{new_w.shape[0]} now")
                for ((old_first, count), _), ((new_first, _), _) in common:
                    new_w[:, new_first : new_first + count] = old_w[:, old_first : old_first + count]

            new_head, old_head = actor[f"heads.{index}.weight"], old["actor"][f"heads.{old_index}.weight"]
            new_bias, old_bias = actor[f"heads.{index}.bias"], old["actor"][f"heads.{old_index}.bias"]
            if new_head.shape[1] != old_head.shape[1]:
                raise ValueError(f"heads.{index}: {tuple(old_head.shape)} in a merged checkpoint does not fit "
                                 f"{tuple(new_head.shape)}")
            for (_, (old_first, count)), (_, (new_first, _)) in common:
                new_head[new_first : new_first + count] = old_head[old_first : old_first + count]
                new_bias[new_first : new_first + count] = old_bias[old_first : old_first + count]

            blocks = [block for block in wanted if block in old_blocks]
            taken.update(blocks)
            seeded.setdefault(layout.name, []).extend(blocks)

    trainer.actor.load_state_dict(actor)
    trainer.critic.load_state_dict(critic)
    trainer._sync_rollout()
    return seeded
