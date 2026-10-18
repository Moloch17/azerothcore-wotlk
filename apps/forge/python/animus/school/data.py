"""**The school's dataset**: the recorder's shards (record.SAMPLE_FIELDS) as a streaming source of batches, and the
tensors a batch becomes -- the decoded planes of both tiers (networks.decode_map, the encoder's own decode), the goal
planes as the hierarchy builds them (networks.goal_plane: the coarse clamped to its border, the fine a one-hot inside
the crop only), and the targets and masks of every cell.

**Split.** By map: Wailing Caverns (HELDOUT_MAP 43) is held out entirely -- never trained, evaluated only. Where no
shard holds a sample of it the hold-out is by episode instead (every tenth episode by hash), and the source says so.
The validation split is every tenth remaining episode by hash; the rest trains. A split is a per-sample rule, so a
shard may hold rows of all three.

**Streaming.** Shards are compressed .npz (no memory map): the source loads a pool of `pool_shards` shards at a time
in a shuffled order, keeps the rows of its split, and draws its batches from the pool -- with `balance`, weighted so
that every (map, memory flag) group of the pool is drawn equally often (with replacement); without, a permutation.

**Targets** (`prepare`), per tier, as the trainer's aux pass defines them (mappo.trainer.MappoTrainer._aux_pass):
`cost` [N, H, W] yards (LABEL_UNREACHABLE where unreachable), `reachable`; the direction: `target` [N, 8, H, W] the
label's downhill neighbours (NEIGHBOURS order; the reachable neighbours of the least cost-to-go, strictly below the
cell's -- the same tie rule as _classical_directions: every lowest neighbour is a hit, a plan's first of equals the
one it descends to) and `graded` [N, H, W] (the label reachable and a reachable neighbour strictly lower); `classes`
[N, H, W] (TRAV indexes, 0 unlabelled); `known` [N, H, W] (the crop's code byte not unknown); `body` [N] whether the
anchor cell (networks.MapEncoder.WAYPOINT_ANCHOR: the centre four's ahead-left cell, the one every walk starts from)
is graded; `body_unseen` [N], the body's split for the direction metric: the label's own 10-step descent from the
anchor crosses a cell the fine crop does not know (the true route goes into the unknown; a convention of the
school's, docs/forge/reference/school.md).
"""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch

from .. import protocol
from ..mappo import networks
from .record import CROP_BYTES, CROP_SIDE, MEMORY_ON, SAMPLE_FIELDS, read_manifest, read_shard

HELDOUT_MAP = 43
#: The tiers' cells, yards (the label manifest's fine_cell / coarse_cell).
TIER_CELLS = (2.0, 8.0)
ANCHOR = networks.MapEncoder.WAYPOINT_ANCHOR
#: The waypoint's walk from the body on the fine tier (regret and the body's split): 10 steps of a cell.
WALK_STEPS = 10
SPLITS = ("train", "val", "heldout")
SIDE = protocol.LABEL_SIDE
CELLS = SIDE * SIDE


def episode_bucket(episode: np.ndarray) -> np.ndarray:
    """A stable bucket 0..9 of each episode id (a hash, so the split survives shard order)."""
    out = np.empty(len(episode), dtype=np.int64)
    for index, value in enumerate(np.asarray(episode, dtype=np.int64)):
        out[index] = int(hashlib.blake2b(int(value).to_bytes(8, "little", signed=True), digest_size=2).hexdigest(),
                         16) % 10
    return out


@dataclass
class ShardList:
    """The shards of one or more directories and their manifests' counts."""

    files: list[Path]
    counts: dict
    heldout_by_map: bool

    @classmethod
    def scan(cls, directories: list[str | Path]) -> "ShardList":
        files: list[Path] = []
        counts: dict = {"samples": 0, "maps": {}, "memory": {}}
        for directory in directories:
            directory = Path(directory)
            manifest = read_manifest(directory)
            for shard in manifest.get("shards", ()):
                files.append(directory / shard["file"])
                counts["samples"] += int(shard["samples"])
                for key, value in shard.get("maps", {}).items():
                    counts["maps"][key] = counts["maps"].get(key, 0) + int(value)
                for key, value in shard.get("memory", {}).items():
                    counts["memory"][key] = counts["memory"].get(key, 0) + int(value)
        if not files:
            raise FileNotFoundError(f"no shards under {[str(d) for d in directories]} (no manifest-*.json, or empty)")
        return cls(files, counts, counts["maps"].get(str(HELDOUT_MAP), 0) > 0)

    def describe(self) -> str:
        maps = ", ".join(f"map {key}: {value}" for key, value in sorted(self.counts["maps"].items()))
        memory = self.counts["memory"]
        return (f"{self.counts['samples']} samples in {len(self.files)} shards ({maps}; memory on {memory.get('on', 0)}"
                f" / off {memory.get('off', 0)} / unknown {memory.get('unknown', 0)}); held out: "
                + (f"map {HELDOUT_MAP} (Wailing Caverns)" if self.heldout_by_map
                   else f"every tenth episode (no sample of map {HELDOUT_MAP} recorded yet)"))


def split_of(batch: dict[str, np.ndarray], heldout_by_map: bool) -> np.ndarray:
    """Each row's split index (SPLITS) by the module's rule."""
    bucket = episode_bucket(batch["episode"])
    if heldout_by_map:
        heldout = batch["map"] == HELDOUT_MAP
        val = ~heldout & (bucket == 1)
    else:
        heldout = bucket == 0
        val = bucket == 1
    out = np.zeros(len(bucket), dtype=np.int64)
    out[val] = 1
    out[heldout] = 2
    return out


class SampleSource:
    """Batches of one split from a ShardList (the module docstring). `batches()` yields numpy batches
    (SAMPLE_FIELDS arrays) for one pass over every shard; `epoch_batches` is how many that is."""

    def __init__(self, shards: ShardList, split: str, batch: int, pool_shards: int = 4, balance: bool = True,
                 seed: int = 1, limit: int = 0):
        if split not in SPLITS:
            raise ValueError(f"split {split!r} is not one of {SPLITS}")
        self.shards = shards
        self.split = SPLITS.index(split)
        self.batch = max(1, int(batch))
        self.pool_shards = max(1, int(pool_shards))
        self.balance = bool(balance)
        self.limit = int(limit)
        self.seed = int(seed)
        self.rng = np.random.default_rng(seed)
        self.shuffle = split == "train"

    def pools(self):
        # The training split's shard order is the epoch's draw; an evaluation split's a fixed shuffle (the shards are
        # written in the run's order, so file order under `limit` would read the run's first samples alone).
        order = (self.rng.permutation(len(self.shards.files)) if self.shuffle
                 else np.random.default_rng(self.seed).permutation(len(self.shards.files)))
        for start in range(0, len(order), self.pool_shards):
            loaded = [read_shard(self.shards.files[index]) for index in order[start:start + self.pool_shards]]
            pool = {name: np.concatenate([shard[name] for shard in loaded]) for name, _, _ in SAMPLE_FIELDS}
            keep = split_of(pool, self.shards.heldout_by_map) == self.split
            if keep.any():
                yield {name: values[keep] for name, values in pool.items()}

    def batches(self):
        drawn = 0
        for pool in self.pools():
            count = len(pool["memory"])
            if self.shuffle and self.balance:
                group = pool["map"].astype(np.int64) * 4 + (pool["memory"].astype(np.int64) + 1)
                _, inverse, sizes = np.unique(group, return_inverse=True, return_counts=True)
                weight = 1.0 / sizes[inverse]
                order = self.rng.choice(count, size=count, replace=True, p=weight / weight.sum())
            elif self.shuffle:
                order = self.rng.permutation(count)
            else:
                order = np.arange(count)
            for start in range(0, count, self.batch):
                rows = order[start:start + self.batch]
                if self.shuffle and len(rows) < self.batch:
                    break
                yield {name: values[rows] for name, values in pool.items()}
                drawn += len(rows)
                if self.limit and drawn >= self.limit:
                    return

    def count(self) -> int:
        """The split's rows over every shard (one pass over the files)."""
        return sum(len(pool["memory"]) for pool in self.pools())


def label_grids(labels: torch.Tensor) -> tuple[list[torch.Tensor], list[torch.Tensor], torch.Tensor]:
    """A batch's label bytes [N, LABEL_BYTES] (uint8) -> per tier the classes [N, H, W] long and the cost-to-go
    [N, H, W] float32, and the six scalars [N, 6]."""
    n = labels.shape[0]
    classes = [labels[:, at:at + CELLS].long().reshape(n, SIDE, SIDE)
               for at in (protocol.LABEL_FINE_CLASS, protocol.LABEL_COARSE_CLASS)]
    costs = [labels[:, at:at + 2 * CELLS].contiguous().view(torch.float16).to(torch.float32).reshape(n, SIDE, SIDE)
             for at in (protocol.LABEL_FINE_COST, protocol.LABEL_COARSE_COST)]
    scalars = labels[:, protocol.LABEL_SCALARS:protocol.LABEL_SCALARS + 4 * protocol.LABEL_SCALAR_COUNT] \
        .contiguous().view(torch.float32)
    return classes, costs, scalars


def direction_targets(cost: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    """A tier's label cost-to-go [N, H, W] -> (`target` [N, 8, H, W] bool, `graded` [N, H, W] bool): the aux pass's
    rule (the module docstring). A cell not graded takes all eight as its target (a finite loss of 0 under its zero
    weight)."""
    label_next = networks.neighbour_stack(cost, protocol.LABEL_UNREACHABLE)
    reach_next = label_next < protocol.LABEL_UNREACHABLE
    lowest = torch.where(reach_next, label_next, torch.full_like(label_next, math.inf)).amin(dim=1)
    reachable = cost < protocol.LABEL_UNREACHABLE
    graded = reachable & (lowest < cost)
    target = (reach_next & (label_next == lowest[:, None])) | ~graded[:, None]
    return target, graded


def walk_cells(cost: torch.Tensor, steps: int = WALK_STEPS) -> torch.Tensor:
    """The cells a steepest descent from the anchor visits over `steps` moves (networks.descend's rule, every step's
    cell marked), [N, H, W] bool."""
    n, height, width = cost.shape
    flat = cost.reshape(n, -1).to(torch.float32)
    offsets = torch.tensor(networks.NEIGHBOURS, dtype=torch.long, device=cost.device)
    row = torch.full((n,), ANCHOR, dtype=torch.long, device=cost.device)
    col = row.clone()
    visited = torch.zeros((n, height * width), dtype=torch.bool, device=cost.device)
    visited.scatter_(1, (row * width + col)[:, None], True)
    for _ in range(int(steps)):
        here = flat.gather(1, (row * width + col)[:, None])[:, 0]
        rows, cols = row[:, None] + offsets[None, :, 0], col[:, None] + offsets[None, :, 1]
        inside = (rows >= 0) & (rows < height) & (cols >= 0) & (cols < width)
        values = flat.gather(1, rows.clamp(0, height - 1) * width + cols.clamp(0, width - 1))
        values = torch.where(inside, values, torch.full_like(values, math.inf))
        best, which = values.min(dim=1)
        move = best < here
        row = torch.where(move, rows.gather(1, which[:, None])[:, 0], row)
        col = torch.where(move, cols.gather(1, which[:, None])[:, 0], col)
        visited.scatter_(1, (row * width + col)[:, None], True)
    return visited.reshape(n, height, width)


def prepare(batch: dict[str, np.ndarray], device) -> dict:
    """A numpy batch (SAMPLE_FIELDS) as the tensors the model, the losses, the metrics and the baselines read (the
    module docstring): "decoded" (per tier decode_map's dict), "goal_plane" (per tier), "goal" (forward, right,
    present), "classes", "cost", "reachable", "known", "target", "graded" (per tier), "body", "body_unseen", "memory"
    [N] float (1 on, 0 off or unknown), "memory_known" [N] bool, "map", "layout", "route", "scalars"."""
    crops = torch.as_tensor(batch["crops"], device=device)
    labels = torch.as_tensor(batch["labels"], device=device)
    n = crops.shape[0]
    decoded = [networks.decode_map(crops[:, tier * CROP_BYTES:(tier + 1) * CROP_BYTES], CROP_SIDE, CROP_SIDE)
               for tier in range(2)]
    classes, costs, scalars = label_grids(labels)
    forward = scalars[:, protocol.LABEL_SCALAR_GOAL_FORWARD]
    right = scalars[:, protocol.LABEL_SCALAR_GOAL_RIGHT]
    present = (forward != 0) | (right != 0)
    # The hierarchy's goal planes (MapEncoder.plan): the coarse clamped to its border, the fine inside the crop only.
    planes = [networks.goal_plane(forward, right, present, SIDE, TIER_CELLS[0], clamp=False),
              networks.goal_plane(forward, right, present, SIDE, TIER_CELLS[1], clamp=True)]
    out = {"decoded": decoded, "goal_plane": planes, "goal": (forward, right, present), "classes": classes,
           "cost": costs, "reachable": [cost < protocol.LABEL_UNREACHABLE for cost in costs],
           "known": [tier["code"] != 0 for tier in decoded], "target": [], "graded": [], "scalars": scalars,
           "memory": torch.as_tensor(batch["memory"] == MEMORY_ON, device=device).to(torch.float32),
           "memory_known": torch.as_tensor(batch["memory"] >= 0, device=device),
           "map": torch.as_tensor(batch["map"].astype(np.int64), device=device),
           "layout": torch.as_tensor(batch["layout"].astype(np.int64), device=device),
           "route": torch.as_tensor(batch["route"], device=device), "count": n}
    for cost in costs:
        target, graded = direction_targets(cost)
        out["target"].append(target)
        out["graded"].append(graded)
    out["body"] = out["graded"][0][:, ANCHOR, ANCHOR]
    walked = walk_cells(costs[0])
    out["body_unseen"] = (walked & ~out["known"][0]).flatten(1).any(dim=1)
    return out


def check_batch(batch: dict[str, np.ndarray]) -> str:
    """A sanity line over a batch: whether the fine label's centre agrees with c_body (the generator's and the sim's
    conventions), and the shares of reachable, known and goal-present rows."""
    labels = torch.as_tensor(batch["labels"])
    classes, costs, scalars = label_grids(labels)
    centre = costs[0][:, ANCHOR:ANCHOR + 2, ANCHOR:ANCHOR + 2].amin(dim=(1, 2))
    body = scalars[:, protocol.LABEL_SCALAR_BODY]
    both = (centre < protocol.LABEL_UNREACHABLE) & (body < protocol.LABEL_UNREACHABLE)
    gap = (centre - body).abs()[both]
    crops = torch.as_tensor(batch["crops"])
    known = (crops[:, :CROP_BYTES].reshape(-1, SIDE, SIDE, networks.MAP_CHANNELS)[..., networks.MAP_CODE] != 0)
    mean, p90 = (float(gap.mean()), float(gap.quantile(0.9))) if len(gap) else (0.0, 0.0)
    return (f"centre four vs c_body: {int(both.sum())} rows comparable, gap mean {mean:.1f} yd, p90 {p90:.1f}; "
            f"fine reachable "
            f"{float((costs[0] < protocol.LABEL_UNREACHABLE).float().mean()):.2f} of cells, crop known "
            f"{float(known.float().mean()):.2f}, goal present "
            f"{float(((scalars[:, 4] != 0) | (scalars[:, 5] != 0)).float().mean()):.2f} of rows")
