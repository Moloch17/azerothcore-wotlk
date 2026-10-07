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

from .stages import Span, block_revisions, block_spans

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
# core block is seeded action by action by name (stage.json action_names). Must equal CoreBlock::OBS_GLOBAL_COUNT
# (tests/test_bootstrap.py reads the header): it was 67 long after the globals had grown to 94, which seeded every
# changed catalog's action features 27 columns off. 91 since the move block's option clocks left (core revision 1,
# player-controller C3); a checkpoint from before is a revision-0 core block, which starts fresh rather than matched.
CORE_GLOBAL_FEATURES = 91
# Per catalog action (CoreBlock::ACTION_FEATURES): 6 until ready and affordable joined them (2026-10-04). A stage.json
# says its own (the core block's "action_features"); one that does not is from before, and had 6.
CORE_ACTION_FEATURES = 8
CORE_ACTION_FEATURES_BEFORE = 6
# The rank tiers (CoreBlock::ACTION_RANK_TIERS) close the core block's actions, and have no features of their own.
CORE_RANK_TIER_PREFIX = "rank_"


def core_action_features(stage: dict | None, layout: str) -> int:
    """The core block's features per catalog action in `stage` (its stage.json), 6 for one from before they were
    written down."""
    entry = (stage or {}).get("layouts", {}).get(layout) or {}
    for block in entry.get("blocks", ()):
        if block.get("name") == "core":
            return int(block.get("action_features", CORE_ACTION_FEATURES_BEFORE))
    return CORE_ACTION_FEATURES_BEFORE


def _core_by_name(old_spans: tuple[Span, Span], new_spans: tuple[Span, Span], old_names: list[str],
                  new_names: list[str], old_features: int = CORE_ACTION_FEATURES,
                  new_features: int = CORE_ACTION_FEATURES):
    """Segments of a core block whose catalog or per-action width changed: the globals, each action both catalogs
    name (the features both widths have, and its action row), the rank tiers' rows, and the talents after them; None
    when the rest of the block does not line up."""
    (old_obs, old_actions), (new_obs, new_actions) = old_spans, new_spans
    old_catalog = old_names[old_actions[0] : old_actions[0] + old_actions[1]]
    new_catalog = new_names[new_actions[0] : new_actions[0] + new_actions[1]]
    if len(old_catalog) != old_actions[1] or len(new_catalog) != new_actions[1]:
        return None

    def featured(catalog: list[str]) -> int:
        """The actions with features: all but the rank tiers that close the block."""
        tiers = 0
        while tiers < len(catalog) and catalog[len(catalog) - 1 - tiers].startswith(CORE_RANK_TIER_PREFIX):
            tiers += 1
        return len(catalog) - tiers

    old_featured, new_featured = featured(old_catalog), featured(new_catalog)
    old_tail = old_obs[1] - CORE_GLOBAL_FEATURES - old_featured * old_features
    new_tail = new_obs[1] - CORE_GLOBAL_FEATURES - new_featured * new_features
    if old_tail != new_tail or old_tail < 0:
        return None

    # A segment is (old spans, new spans) like a whole block's; a count of 0 moves nothing.
    def segment(old_obs_first, new_obs_first, obs_count, old_action, new_action, action_count):
        return (((old_obs_first, obs_count), (old_action, action_count)),
                ((new_obs_first, obs_count), (new_action, action_count)))

    kept = min(old_features, new_features)
    segments = [segment(old_obs[0], new_obs[0], CORE_GLOBAL_FEATURES, 0, 0, 0)]
    where = {action: position for position, action in enumerate(old_catalog)}
    for position, action in enumerate(new_catalog):
        if action in where:
            old_position = where[action]
            features = kept if position < new_featured and old_position < old_featured else 0
            segments.append(segment(old_obs[0] + CORE_GLOBAL_FEATURES + old_position * old_features,
                                    new_obs[0] + CORE_GLOBAL_FEATURES + position * new_features,
                                    features, old_actions[0] + old_position, new_actions[0] + position, 1))
    segments.append(segment(old_obs[0] + CORE_GLOBAL_FEATURES + old_featured * old_features,
                            new_obs[0] + CORE_GLOBAL_FEATURES + new_featured * new_features,
                            new_tail, 0, 0, 0))
    return segments


def _layout_sets(stage: dict | None, layout: str) -> list[dict]:
    """The seat sets stage.json gives `layout` (Blocks::DescribeSeatSets): name, slots, segments (first, stride)."""
    return list(((stage or {}).get("layouts", {}).get(layout) or {}).get("sets", ()))


def _slots_grown(old_spans: tuple[Span, Span], new_spans: tuple[Span, Span], old_sets: list[dict],
                 new_sets: list[dict], old_names: list[str], new_names: list[str]):
    """Segments of a block whose seat set changed only in how many slots it has (the enemies, PACK_SLOTS 4 -> 24,
    2026-10-03): what comes before the slots, the slots both have, what comes after them, and each of the block's
    actions both name (target_slot_0..3, hold_interrupt, which moved past the new slots); None when the block is not
    that."""
    (old_obs, old_actions), (new_obs, new_actions) = old_spans, new_spans

    def inside(sets: list[dict], obs: Span) -> list[tuple[dict, dict]]:
        return [(entry, segment) for entry in sets for segment in entry.get("segments", ())
                if obs[0] <= segment["first"] < obs[0] + obs[1]]

    old_in, new_in = inside(old_sets, old_obs), inside(new_sets, new_obs)
    if len(old_in) != 1 or len(new_in) != 1:
        return None
    (old_set, old_segment), (new_set, new_segment) = old_in[0], new_in[0]
    stride = old_segment["stride"]
    if old_set["name"] != new_set["name"] or new_segment["stride"] != stride:
        return None
    head = old_segment["first"] - old_obs[0]
    tail = old_obs[1] - head - old_set["slots"] * stride
    if new_segment["first"] - new_obs[0] != head or new_obs[1] - head - new_set["slots"] * stride != tail or tail < 0:
        return None
    old_block = old_names[old_actions[0] : old_actions[0] + old_actions[1]]
    new_block = new_names[new_actions[0] : new_actions[0] + new_actions[1]]
    if len(old_block) != old_actions[1] or len(new_block) != new_actions[1]:
        return None

    def segment(old_obs_first, new_obs_first, obs_count, old_action=0, new_action=0, action_count=0):
        return (((old_obs_first, obs_count), (old_action, action_count)),
                ((new_obs_first, obs_count), (new_action, action_count)))

    kept = min(old_set["slots"], new_set["slots"])
    segments = [segment(old_obs[0], new_obs[0], head + kept * stride),
                segment(old_obs[0] + head + old_set["slots"] * stride, new_obs[0] + head + new_set["slots"] * stride,
                        tail)]
    where = {action: position for position, action in enumerate(old_block)}
    for position, action in enumerate(new_block):
        if action in where:
            segments.append(segment(0, 0, 0, old_actions[0] + where[action], new_actions[0] + position, 1))
    return segments


def _rescaled(stage: dict | None, layout: str) -> dict[str, list[tuple[int, int, str]]]:
    """Block name -> its rescaled columns (Block::DescribeRescaled) as (first, count, tag), first relative to the
    block."""
    entry = (stage or {}).get("layouts", {}).get(layout) or {}
    return {block["name"]: [(int(c["first"]), int(c["count"]), str(c["tag"])) for c in block.get("rescaled", ())]
            for block in entry.get("blocks", ())}


def _seed_rescaled_norms(new: dict, prefix: str, old_stage: dict | None, stage: dict | None, layout: str) -> list[str]:
    """Columns whose reading changed its scale in place (the talent trees as a share of the points spent, 2026-10-03):
    where the parent's block lacks the tag, their normaliser statistics start at mean 0, variance 1 and the count is
    capped, as for a block that gained features -- the parent's were measured on the old scale, and with its count of
    tens of millions and no clipping the new values would read several deviations off for many updates. The adapter
    columns carry: the reading means what it meant, more points in that tree. Returns the tags reset."""
    old, new_cols = _rescaled(old_stage, layout), _rescaled(stage, layout)
    spans = block_spans(stage, layout) or {}
    reset = []
    for block, columns in new_cols.items():
        had = {tag for _, _, tag in old.get(block, ())}
        first = spans.get(block, ((0, 0), (0, 0)))[0][0]
        for at, count, tag in columns:
            if tag in had:
                continue
            new[f"{prefix}.mean"][first + at : first + at + count] = 0.0
            new[f"{prefix}.var"][first + at : first + at + count] = 1.0
            new[f"{prefix}.count"].clamp_(max=SEED_COUNT_CAP)
            reset.append(f"{block}.{tag}")
    return reset


def _seed_grown_slot_norms(new: dict, prefix: str, old_sets: list[dict], new_sets: list[dict]) -> None:
    """A seat set that gained slots: each new slot's normaliser statistics start as the last old slot's, carried
    already, so the set encoder reads the new slots at the scale it reads the old ones from the first rollout."""
    old_slots = {entry["name"]: entry["slots"] for entry in old_sets}
    for entry in new_sets:
        had = old_slots.get(entry["name"], 0)
        if not had or entry["slots"] <= had:
            continue
        for segment in entry.get("segments", ()):
            first, stride = segment["first"], segment["stride"]
            for name in ("mean", "var"):
                stat = new[f"{prefix}.{name}"]
                source = stat[first + (had - 1) * stride : first + had * stride].clone()
                for slot in range(had, entry["slots"]):
                    stat[first + slot * stride : first + (slot + 1) * stride] = source


#: Blocks whose observation only ever grows at its end (new features after the old ones, the actions unchanged), so a
#: checkpoint from before the growth seeds their old columns as they were. CrowdBlock.h's tail features (2026-10-03).
GROWS_AT_END = frozenset({"crowd"})


def _common_blocks(old: dict[str, tuple[Span, Span]], new: dict[str, tuple[Span, Span]], name: str,
                   old_names: list[str] | None = None, new_names: list[str] | None = None,
                   revisions: tuple[dict[str, int], dict[str, int]] | None = None, source: str = "",
                   sets: tuple[list[dict], list[dict]] | None = None,
                   core_features: tuple[int, int] = (CORE_ACTION_FEATURES, CORE_ACTION_FEATURES)):
    """(old spans, new spans) of every block both layouts have, sizes checked. A core block whose catalog changed is
    matched action by action by name, when both stages name their actions. `revisions` (old, new; Block::Revision):
    a block whose revision differs re-laid its columns, so it starts fresh like one that changed shape -- even at the
    same width, where the size check alone would carry stale weights onto columns that now mean something else."""
    common = []
    old_revisions, new_revisions = revisions or ({}, {})
    for block, (new_obs, new_actions) in new.items():
        if block not in old:
            continue
        old_obs, old_actions = old[block]
        old_revision, new_revision = old_revisions.get(block, 0), new_revisions.get(block, 0)
        if old_revision != new_revision:
            print(f"seeding {source or name}: {name} block {block} revision {old_revision} -> {new_revision} (width "
                  f"{old_obs[1]} -> {new_obs[1]}): its columns start fresh, but for those it names (by name below)",
                  flush=True)
            continue
        if old_obs[1] != new_obs[1] or old_actions[1] != new_actions[1]:
            segments = None
            if (block in GROWS_AT_END and old_actions[1] == new_actions[1] and new_obs[1] > old_obs[1]):
                # Columns added after the old ones: those carry over where they were, the new ones start at zero.
                print(f"  {name}: block {block} grew from {old_obs[1]} to {new_obs[1]} features: the first "
                      f"{old_obs[1]} carry over", flush=True)
                common.append(((old_obs, old_actions), ((new_obs[0], old_obs[1]), new_actions)))
                continue
            if block == "core" and old_names and new_names:
                segments = _core_by_name((old_obs, old_actions), (new_obs, new_actions), old_names, new_names,
                                         *core_features)
            elif sets and old_names and new_names:
                segments = _slots_grown((old_obs, old_actions), (new_obs, new_actions), sets[0], sets[1], old_names,
                                        new_names)
                if segments is not None:
                    print(f"  {name}: block {block} grew from {old_obs[1]} to {new_obs[1]} features and "
                          f"{old_actions[1]} to {new_actions[1]} actions with its seat set's slots: the slots both "
                          f"have and the actions both name carry over", flush=True)
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


#: The move block's columns at its revision 4 (MoveBlock.h before the compass split, 63 of them), whose stage.json
#: carries no obs_names: the names revision 5's move block and the compass block (CompassBlock.h) give the same
#: columns, so a revision 4 checkpoint -- every M1 trained before 2026-10-06 -- seeds them where they now are.
MOVE_REVISION_4_COLUMNS = (
    "moving", "speed", "facing_sin", "facing_cos", "pitch_sin", "pitch_cos",
    "held_forward", "held_strafe", "held_vertical", "held_turn", "held_pitch", "held_walk",
    "velocity_ahead", "velocity_left", "velocity_up", "progress",
    "mode_ground", "mode_falling", "mode_swimming", "mode_flying",
    "fall_time", "fall_height", "against_wall", "steep_slope", "depth", "can_jump",
    "target_bearing_sin", "target_bearing_cos", "target_distance",
    "hazard_bearing_sin", "hazard_bearing_cos", "hazard_distance", "hazard_radius",
    "objective", "objective_bearing_sin", "objective_bearing_cos", "objective_distance",
    "in_water", "submerged", "submerged_time", "swim_speed", "airborne",
    "detour", "move_rate", "close_rate", "objective_near",
    *(f"trail_{sample}_{axis}" for sample in range(8) for axis in ("ahead", "left")),
    "trail_dwell",
)


def _column_names(stage: dict | None, layout: str) -> dict[str, list[str]]:
    """Block name -> its columns' names (Block::DescribeColumns, stage.json obs_names), for the blocks that name them;
    a revision 4 move block of 63 columns, from before names were written, by MOVE_REVISION_4_COLUMNS."""
    entry = (stage or {}).get("layouts", {}).get(layout) or {}
    names = {}
    for block in entry.get("blocks", ()):
        if block.get("obs_names"):
            names[block["name"]] = list(block["obs_names"])
        elif (block["name"] == "move" and int(block.get("revision", 0)) == 4
              and int(block["obs"][1]) == len(MOVE_REVISION_4_COLUMNS)):
            names[block["name"]] = list(MOVE_REVISION_4_COLUMNS)
    return names


def _by_name(common, old_stage: dict | None, stage: dict | None, layout: str, old_names: list[str] | None,
             new_names: list[str] | None):
    """Segments for what `common` (whole blocks) left behind, matched by name: each named column of a new block that
    no segment fills, from the old layout's column of the same name in any block (the compass split: revision 4's
    move block's objective columns now the compass block's, the rest the revision 5 move block's), and each action of
    such a block from the old block of the same name's action of the same name. Returns (segments, {block: (columns,
    actions)} carried this way)."""
    old_spans, new_spans = block_spans(old_stage, layout) or {}, block_spans(stage, layout) or {}
    old_columns, new_columns = _column_names(old_stage, layout), _column_names(stage, layout)
    filled_obs = set()
    filled_actions = set()
    used_obs = set()
    for ((old_first, count), (old_action, actions)), ((new_first, _), (new_action, _)) in common:
        filled_obs.update(range(new_first, new_first + count))
        used_obs.update(range(old_first, old_first + count))
        filled_actions.update(range(new_action, new_action + actions))

    where = {}
    for block, names in old_columns.items():
        first = old_spans[block][0][0]
        for offset, name in enumerate(names):
            if first + offset not in used_obs:
                where.setdefault(name, first + offset)

    segments, carried = [], {}
    for block, names in new_columns.items():
        (first, count), (action_first, action_count) = new_spans[block]
        columns = actions = 0
        for offset, name in enumerate(names[:count]):
            if first + offset in filled_obs or name not in where:
                continue
            segments.append((((where[name], 1), (0, 0)), ((first + offset, 1), (0, 0))))
            columns += 1
        if block in old_spans and old_names and new_names and not any(
                action in filled_actions for action in range(action_first, action_first + action_count)):
            old_first, old_count = old_spans[block][1]
            old_block = {name: old_first + index
                         for index, name in enumerate(old_names[old_first : old_first + old_count])}
            for index, name in enumerate(new_names[action_first : action_first + action_count]):
                if name in old_block:
                    segments.append((((0, 0), (old_block[name], 1)), ((0, 0), (action_first + index, 1))))
                    actions += 1
        if columns or actions:
            carried[block] = (columns, actions)
    return segments, carried


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


#: The seat sets' shared encoder (EntitySets, mappo.seat_sets): per set an encoder, the pool onto the adapters'
#: output, the pointer heads' queries.
ENTITY_SETS = "entity_sets."


def _seed_entity_sets(new: dict, old: dict, new_layouts: list[str] | None = None,
                      old_layouts: list[str] | None = None) -> tuple[list[str], list[str]] | None:
    """Carry the seat sets from the stage before: each set's encoder where its shape is the same (a set whose slot
    gained features -- the enemies once the hostiles block joins the pack's -- starts fresh), its pool columns with
    it, the pool's bias and the pointer queries. A set that starts fresh, or a checkpoint without seat sets, gets pool
    columns of zero: the seeded policy starts as it was and the sets enter as they learn, as a new block's adapter
    columns do. Returns (carried, fresh) set names, or None when this network has no seat sets."""
    if f"{ENTITY_SETS}pool.weight" not in new:
        return None
    from .mappo.networks import SEAT_SET_NAMES

    def names_of(state: dict) -> list[str]:
        present = {key.split(".")[2] for key in state if key.startswith(f"{ENTITY_SETS}encoders.")}
        return [name for name in SEAT_SET_NAMES if name in present] + sorted(present - set(SEAT_SET_NAMES))

    new_names, old_names = names_of(new), names_of(old)
    embed = int(new[f"{ENTITY_SETS}encoders.{new_names[0]}.0.weight"].shape[0])
    carried = []
    for name in new_names:
        prefix = f"{ENTITY_SETS}encoders.{name}."
        keys = [key for key in new if key.startswith(prefix)]
        if name in old_names and all(key in old and old[key].shape == new[key].shape for key in keys):
            for key in keys:
                new[key].copy_(old[key])
            carried.append(name)

    pool, old_pool = new[f"{ENTITY_SETS}pool.weight"], old.get(f"{ENTITY_SETS}pool.weight")
    pool.zero_()
    for name in carried:
        to, at = new_names.index(name) * 2 * embed, old_names.index(name) * 2 * embed
        if old_pool is not None and old_pool.shape[0] == pool.shape[0]:
            pool[:, to : to + 2 * embed] = old_pool[:, at : at + 2 * embed]
    bias, old_bias = new[f"{ENTITY_SETS}pool.bias"], old.get(f"{ENTITY_SETS}pool.bias")
    if old_bias is not None and old_bias.shape == bias.shape:
        bias.copy_(old_bias)
    else:
        bias.zero_()
    for key, tensor in new.items():
        if key.startswith(f"{ENTITY_SETS}queries.") and key in old and old[key].shape == tensor.shape:
            tensor.copy_(old[key])
    # Attention (mappo.entity_attention): from a parent without it the layer keeps its identity start; from one with
    # it the layer carries, each set's type embedding by its name, each layout's own token by its name, and the own
    # token's pool columns (the last embed of the pool's) with them.
    if f"{ENTITY_SETS}type_embed" in new and f"{ENTITY_SETS}type_embed" in old:
        for key, tensor in new.items():
            layer = key[len(ENTITY_SETS):].startswith(("norm_attend.", "attend.", "norm_mix.", "mix_in.", "mix_out."))
            if layer and key in old and old[key].shape == tensor.shape:
                tensor.copy_(old[key])
        for name in carried:
            new[f"{ENTITY_SETS}type_embed"][new_names.index(name)] = old[f"{ENTITY_SETS}type_embed"][
                old_names.index(name)]
        for index, layout in enumerate(new_layouts or ()):
            if layout in (old_layouts or ()):
                new[f"{ENTITY_SETS}self_token"][index] = old[f"{ENTITY_SETS}self_token"][old_layouts.index(layout)]
        if old_pool is not None and old_pool.shape[0] == pool.shape[0]:
            pool[:, -embed:] = old_pool[:, -embed:]
    return carried, [name for name in new_names if name not in carried]


#: The camera: its encoder (VisionEncoder, in the actor alone: the critic reads the actor's) and each network's join
#: onto its adapters' output (VisionJoin).
VISION = ("vision.", "vision_join.")
VISION_JOIN = "vision_join."
#: The mental map's encoder inside the camera's (MapEncoder, perception-goals REDESIGN §3): seeded on its own, so a
#: camera carries from a checkpoint without a map, and the map starts fresh with its join (and the VIN's output)
#: zeroed.
MAP = "vision.map."
MAP_ZEROED = ("vision.map.join.", "vision.map.vin.out.")


def _vision_revision(stage: dict | None) -> int | None:
    """The vision block's revision in the first layout of `stage` that has one; None for a stage without a camera."""
    for entry in ((stage or {}).get("layouts") or {}).values():
        for block in entry.get("blocks", ()):
            if block.get("name") == "vision":
                return int(block.get("revision", 0))
    return None


def _seed_vision(new: dict, old: dict, new_stage: dict | None, old_stage: dict | None) -> str | None:
    """Carry the camera's encoder from a checkpoint that has one of the same shape and the same vision block revision
    (a revision re-laid the image, so the encoder starts fresh like the block's columns). Returns what happened, or
    None when this network has no camera. Left fresh, its join is zeroed: the seeded policy starts as it was, and the
    camera comes in as the join learns (a network trained from scratch keeps its join's ordinary initialisation)."""
    keys = [key for key in new if key.startswith(VISION) and not key.startswith(MAP)]
    if not keys:
        return None
    fresh = None
    if not any(key.startswith(VISION) and not key.startswith(MAP) for key in old):
        fresh = "fresh (the checkpoint has none)"
    elif _vision_revision(new_stage) != _vision_revision(old_stage):
        fresh = f"fresh (vision revision {_vision_revision(old_stage)} -> {_vision_revision(new_stage)})"
    elif not all(key in old and old[key].shape == new[key].shape for key in keys):
        fresh = "fresh (its shape changed)"
    if fresh:
        for key in keys:
            if key.startswith(VISION_JOIN):
                new[key].zero_()
        return fresh
    for key in keys:
        new[key].copy_(old[key])
    return "carried"


def _seed_map(new: dict, old: dict) -> str | None:
    """Carry the mental map's encoder from a checkpoint that has one, key by key where the shapes agree (the VIN,
    switched on later, starts fresh beside a carried map); else it starts fresh. Whatever starts fresh with an output
    into the camera's embedding -- the map's join, the VIN's read-out -- is zeroed, so the seeded policy starts as it
    was. None when this network has no map."""
    keys = [key for key in new if key.startswith(MAP)]
    if not keys:
        return None
    carried = [key for key in keys if key in old and old[key].shape == new[key].shape]
    for key in carried:
        new[key].copy_(old[key])
    fresh = [key for key in keys if key not in carried]
    for key in fresh:
        if key.startswith(MAP_ZEROED):
            new[key].zero_()
    if not carried:
        return "fresh (the checkpoint has none), its join at zero"
    if fresh:
        return f"carried, but for {len(fresh)} new tensors ({', '.join(sorted({k.split('.')[2] for k in fresh}))})"
    return "carried"


#: The free look's head (LookHead, camera-vision.FREELOOK.md D): the actor's own, not a shared part.
LOOK_HEAD = "look_head."


def _seed_look(new: dict, old: dict, new_stage: dict | None, old_stage: dict | None) -> str | None:
    """Carry the look head from a checkpoint that has one of the same heads at the same vision block revision, as the
    camera's encoder is; else it starts fresh (as it was initialised: small, leaning toward holding still). None when
    this actor has no look head."""
    keys = [key for key in new if key.startswith(LOOK_HEAD)]
    if not keys:
        return None
    if not any(key.startswith(LOOK_HEAD) for key in old):
        return "fresh (the checkpoint has none)"
    if _vision_revision(new_stage) != _vision_revision(old_stage):
        return f"fresh (vision revision {_vision_revision(old_stage)} -> {_vision_revision(new_stage)})"
    if not all(key in old and old[key].shape == new[key].shape for key in keys):
        return "fresh (its heads changed)"
    for key in keys:
        new[key].copy_(old[key])
    return "carried"


def seed_trainer(trainer, checkpoint: dict, spec, stage: dict | None = None, overlay: bool = False,
                 source: str = "") -> list[str]:
    """Seed a fresh MappoTrainer for `spec` (whose stage.json is `stage`) from an earlier stage's checkpoint; returns
    the layouts seeded.

    With `overlay`, only the layouts the checkpoint has are written -- their adapters, normalisers and heads -- over
    networks already seeded, and the trunk is left as it is: a restricted stage (the stealth drill, two classes of
    ten) merged forward into the stage after it (Run._load_or_seed). Its trunk saw only those classes, so it is not
    taken; its layouts were trained against it, so they are a warm start the stage's distillation realigns."""
    old_names = [layout["name"] for layout in checkpoint["spec"].get("layouts", ())]
    old_stage = checkpoint.get("stage")
    old = checkpoint["trainer"]

    actor = {key: tensor.clone() for key, tensor in trainer.actor.state_dict().items()}
    critic = {key: tensor.clone() for key, tensor in trainer.critic.state_dict().items()}

    if not overlay:
        _seed_shared(actor, old["actor"])
        _seed_shared(critic, old["critic"])
        names = [layout.name for layout in spec.layouts]
        sets = _seed_entity_sets(actor, old["actor"], names, old_names)
        _seed_entity_sets(critic, old["critic"], names, old_names)
        vision = _seed_vision(actor, old["actor"], stage, old_stage)
        _seed_vision(critic, old["critic"], stage, old_stage)
        if vision is not None:
            print(f"  vision encoder: {vision}", flush=True)
        crop = _seed_map(actor, old["actor"])
        _seed_map(critic, old["critic"])
        if crop is not None:
            print(f"  map encoder: {crop}", flush=True)
        look = _seed_look(actor, old["actor"], stage, old_stage)
        if look is not None:
            print(f"  look head: {look}", flush=True)
        if sets is not None:
            carried, fresh = sets
            print(f"  seat sets: {', '.join(carried) or 'none'} carried, {', '.join(fresh) or 'none'} fresh (their "
                  f"pool columns at zero)", flush=True)

    # Every layout this run has must be in the checkpoint it is seeding from. A missing one is not a thing to work
    # around quietly: the alternative is starting that class from scratch in the middle of a curriculum, which looks
    # exactly like a class that has simply not learned anything yet.
    # The director is the exception, and the only one: it is an agent the stage adds rather than a class the run
    # plays, so the first directed stage in a chain necessarily seeds from one without it.
    missing = [layout.name for layout in spec.layouts
               if layout.name not in old_names and layout.name != DIRECTOR_LAYOUT]
    if missing and not overlay:
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
        sets = (_layout_sets(old_stage, layout.name), _layout_sets(stage, layout.name))
        if old_blocks is not None and new_blocks is not None:
            common = _common_blocks(old_blocks, new_blocks, layout.name, _action_names(old_stage, layout.name),
                                    _action_names(stage, layout.name),
                                    (block_revisions(old_stage, layout.name), block_revisions(stage, layout.name)),
                                    source, sets,
                                    (core_action_features(old_stage, layout.name),
                                     core_action_features(stage, layout.name)))
            # A block that started fresh (a changed revision) or is new keeps whatever columns and actions it shares by
            # name with the checkpoint: the compass split's move and compass blocks. The named segments join `common`,
            # so each column's normaliser mean and variance (actor's and critic's, _seed_norm_blocks) move with its
            # weights: the copied weights read the column at the scale they were trained on.
            named, carried = _by_name(common, old_stage, stage, layout.name, _action_names(old_stage, layout.name),
                                      _action_names(stage, layout.name))
            for block, (columns, actions) in carried.items():
                print(f"  {layout.name}: block {block}: {columns} columns and {actions} actions carried by name",
                      flush=True)
            common = common + named
            for network, remapped in adapters:
                _seed_adapter_blocks(network, remapped, f"adapters.{index}", common)
            for network, remapped in norms:
                _seed_norm_blocks(network, remapped, f"norms.{index}", common)
                _seed_grown_slot_norms(network, f"norms.{index}", *sets)
                reset = _seed_rescaled_norms(network, f"norms.{index}", old_stage, stage, layout.name)
                if reset and network is actor:
                    print(f"  {layout.name}: rescaled columns {', '.join(sorted(set(reset)))} start their "
                          f"normaliser afresh", flush=True)
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
                                    _action_names(stage, layout.name),
                                    (block_revisions(merge.get("stage"), layout.name),
                                     block_revisions(stage, layout.name)))
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
