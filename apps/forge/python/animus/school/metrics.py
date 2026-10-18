"""**The school's metrics and baselines**, computed on the same batches for the model and for two classical plans:

- `dijkstra_*`: networks.classical_cost_to_go over the crop's own codes -- what the sim plans today, at the
  learner's 2-yd crop rather than the sim's 1-yd configuration space -- priced by the "map" rule with no costmap
  head (networks.classical_factor with the head's guess fixed at "unknown": unknown 1.5, floor and door 1, wall 100,
  hazard 5 a yard, a known floor off the height band as unknown; CLASSICAL_COSTS, MAP_CODE_RULES), the hierarchy as
  the encoder runs it (the coarse tier first, resampled as the fine tier's ring, model.CoarseFrame), run to
  convergence (CLASSICAL_ITERATIONS at most).
- `oracle_*`: the same over the label's true classes (CLASSICAL_COSTS, unknown CLASSICAL_ORACLE_UNKNOWN): the best
  the rule can do with the truth, the ceiling the school is measured against.

The metrics (`Scores`; every direction one is the share of hits, every cost one yards):

- `dir_acc_body`: at the body -- the anchor cell (data.ANCHOR), where the sim's walk starts -- the direction a plan
  descends to (the highest of its eight direction logits, the first of equals in NEIGHBOURS order) is one of the
  label's downhill neighbours (data.direction_targets); over the rows whose anchor is graded. Split `_seen` /
  `_unseen` by data's `body_unseen` (the label's own 10-step descent from the anchor crosses a cell the fine crop does
  not know: the true route goes into the unknown), `_mem_on` / `_mem_off` by the memory flag (rows whose flag is
  known); `dir_acc_body_coarse` the coarse tier's.
- `dir_acc_cells`: the same over every graded cell of the fine tier; `_seen` / `_unseen` by the cell's own crop code
  (known or not), `_mem_on` / `_mem_off` by the row.
- `cost_mae_known` / `cost_mae_unseen`: the fine cost-to-go's absolute error against the label over the label's
  reachable cells the crop knows / does not know, counting the cells the plan prices under CLASSICAL_OPEN_CEILING
  (a cell above it was priced through a wall; its error is the wall's cost, the trainer's rule); `cost_blocked_known`
  the share of known reachable cells above it.
- `waypoint_regret_yards`: networks.descend's walk of data.WALK_STEPS (10) cells over the plan's fine cost-to-go from
  the anchor (the model's with the cells its reachable head rejects at the sentinel, `walk_field`); the label's
  cost-to-go at the cell reached, less the least label cost-to-go over the cells the label can reach from the anchor
  in 10 steps (a dilation through the label's reachable cells, so a wall is not stepped through); over the rows
  whose anchor the label reaches. A walk that ends on a cell the label cannot reach pays
  REGRET_CAP and counts in `waypoint_unreachable_share`. Split `_seen` / `_unseen` as the body's.
- The model's own heads, where given: `reach_acc` (the reachable logit against the label's reachability over every
  fine cell), `cls_acc_known` / `cls_acc_unseen` (the class head's argmax over labelled cells the crop knows / does
  not).
"""

from __future__ import annotations

import torch

from .. import protocol
from ..mappo import networks
from ..mappo.networks import (CLASSICAL_COSTS, CLASSICAL_ITERATIONS, CLASSICAL_OPEN_CEILING, CLASSICAL_ORACLE_UNKNOWN,
                              CLASSICAL_SENTINEL, classical_cost_to_go, classical_factor, classical_tables)
from .data import ANCHOR, TIER_CELLS, WALK_STEPS
from .model import CoarseFrame, direction_logits

REGRET_CAP = 100.0
BASELINES = ("dijkstra", "oracle")
#: The metrics a report line shows, in order.
HEADLINE = ("dir_acc_body", "dir_acc_body_seen", "dir_acc_body_unseen", "dir_acc_body_mem_on", "dir_acc_body_mem_off",
            "dir_acc_cells", "dir_acc_cells_seen", "dir_acc_cells_unseen", "dir_acc_body_coarse", "cost_mae_known",
            "cost_mae_unseen", "waypoint_regret_yards", "waypoint_unreachable_share")


class Scores:
    """Sums of hits and counts by metric name; `value(name)` the ratio, `values()` all of them."""

    def __init__(self):
        self.sums: dict[str, float] = {}
        self.counts: dict[str, float] = {}

    def add(self, name: str, hits: torch.Tensor, count: torch.Tensor | None = None) -> None:
        """`hits` summed over `count` rows / cells (a bool mask of the same shape; every element when None)."""
        hits = hits.to(torch.float32)
        if count is None:
            total, n = float(hits.sum()), float(hits.numel())
        else:
            mask = count.to(torch.float32)
            total, n = float((hits * mask).sum()), float(mask.sum())
        self.sums[name] = self.sums.get(name, 0.0) + total
        self.counts[name] = self.counts.get(name, 0.0) + n

    def value(self, name: str) -> float:
        return self.sums.get(name, 0.0) / max(1.0, self.counts.get(name, 0.0))

    def count(self, name: str) -> float:
        return self.counts.get(name, 0.0)

    def values(self) -> dict[str, float]:
        return {name: self.value(name) for name in self.sums}

    def update(self, batch: dict, costs: list[torch.Tensor], dirs: list[torch.Tensor],
               reach: list[torch.Tensor] | None = None, cls: list[torch.Tensor] | None = None) -> None:
        """One batch (data.prepare's dict): `costs` per tier [N, H, W] yards, `dirs` per tier [N, 8, H, W]; with
        `reach` (the model's reachable logits a tier) the waypoint's walk runs over the cost with every cell the model
        calls unreachable at CLASSICAL_SENTINEL (`walk_field`: a wall the model names is not stepped on, as a
        classical plan's 100 a yard keeps a walk off one), the cost error over the cost as predicted."""
        with torch.no_grad():
            n = batch["count"]
            memory_known = batch["memory_known"]
            mem_on = memory_known & (batch["memory"] > 0.5)
            mem_off = memory_known & (batch["memory"] <= 0.5)
            body, unseen = batch["body"], batch["body_unseen"]
            row_splits = {"": torch.ones_like(body), "_seen": ~unseen, "_unseen": unseen, "_mem_on": mem_on,
                          "_mem_off": mem_off}
            for tier, suffix in ((0, ""), (1, "_coarse")):
                target, graded = batch["target"][tier], batch["graded"][tier]
                chosen = dirs[tier][:, :, ANCHOR, ANCHOR].argmax(dim=1)
                hit = target[:, :, ANCHOR, ANCHOR].gather(1, chosen[:, None])[:, 0]
                body_tier = graded[:, ANCHOR, ANCHOR]
                if tier == 0:
                    for name, rows in row_splits.items():
                        self.add(f"dir_acc_body{name}", hit, body_tier & rows)
                else:
                    self.add(f"dir_acc_body{suffix}", hit, body_tier)
            target, graded, known = batch["target"][0], batch["graded"][0], batch["known"][0]
            hits = target.gather(1, dirs[0].argmax(dim=1, keepdim=True))[:, 0]
            self.add("dir_acc_cells", hits, graded)
            self.add("dir_acc_cells_seen", hits, graded & known)
            self.add("dir_acc_cells_unseen", hits, graded & ~known)
            self.add("dir_acc_cells_mem_on", hits, graded & mem_on[:, None, None])
            self.add("dir_acc_cells_mem_off", hits, graded & mem_off[:, None, None])
            # The cost-to-go against the label on the fine tier.
            label, reachable = batch["cost"][0], batch["reachable"][0]
            cost = costs[0].to(torch.float32)
            open_ground = cost < CLASSICAL_OPEN_CEILING
            error = (cost - label).abs()
            self.add("cost_mae_known", error, reachable & known & open_ground)
            self.add("cost_mae_unseen", error, reachable & ~known & open_ground)
            self.add("cost_blocked_known", ~open_ground, reachable & known)
            # The waypoint's regret.
            row, col = networks.descend(walk_field(cost, None if reach is None else reach[0]), ANCHOR, WALK_STEPS)
            index = torch.arange(n, device=cost.device)
            at_reached = label[index, row, col]
            best = best_within(label, reachable, WALK_STEPS)
            anchored = reachable[:, ANCHOR, ANCHOR]
            landed = at_reached < protocol.LABEL_UNREACHABLE
            regret = torch.where(landed, (at_reached - best).clamp(min=0.0), torch.full_like(best, REGRET_CAP))
            for name, rows in (("", torch.ones_like(anchored)), ("_seen", ~unseen), ("_unseen", unseen),
                               ("_mem_on", mem_on), ("_mem_off", mem_off)):
                self.add(f"waypoint_regret_yards{name}", regret, anchored & rows)
            self.add("waypoint_unreachable_share", ~landed, anchored)
            if reach is not None:
                self.add("reach_acc", (reach[0] > 0.0) == reachable)
            if cls is not None:
                labelled = batch["classes"][0] != 0
                right = cls[0].argmax(dim=1) == batch["classes"][0]
                self.add("cls_acc_known", right, labelled & known)
                self.add("cls_acc_unseen", right, labelled & ~known)


def walk_field(cost: torch.Tensor, reach: torch.Tensor | None) -> torch.Tensor:
    """The field a walk descends: `cost` [N, H, W], every cell whose reachable logit `reach` is at or below 0 at
    CLASSICAL_SENTINEL (the cost as it is without `reach`)."""
    if reach is None:
        return cost
    return torch.where(reach > 0.0, cost, torch.full_like(cost, CLASSICAL_SENTINEL))


def best_within(label: torch.Tensor, reachable: torch.Tensor, steps: int) -> torch.Tensor:
    """The least label cost-to-go [N] over the cells reachable from the anchor in `steps` 8-neighbour moves through
    the label's reachable cells (a 3 x 3 dilation a step, masked)."""
    n = label.shape[0]
    region = torch.zeros_like(reachable)
    region[:, ANCHOR, ANCHOR] = True
    region &= reachable
    for _ in range(int(steps)):
        grown = torch.nn.functional.max_pool2d(region.to(torch.float32)[:, None], 3, stride=1, padding=1)[:, 0] > 0
        region = grown & reachable
    return torch.where(region, label, torch.full_like(label, protocol.LABEL_UNREACHABLE)).reshape(n, -1).amin(dim=1)


class Baselines:
    """The two classical plans of a batch (the module docstring): `plan(batch, which)` -> per-tier costs [N, H, W]
    yards (fine first) and their direction logits."""

    def __init__(self, device):
        self.tables = classical_tables(device)
        oracle = torch.tensor(CLASSICAL_COSTS, dtype=torch.float32, device=device)
        oracle[0] = CLASSICAL_ORACLE_UNKNOWN
        self.oracle_table = oracle
        self.frame = CoarseFrame().to(device)

    @torch.no_grad()
    def plan(self, batch: dict, which: str) -> tuple[list[torch.Tensor], list[torch.Tensor]]:
        if which not in BASELINES:
            raise ValueError(f"baseline {which!r} is not one of {BASELINES}")
        planned: list[torch.Tensor | None] = [None, None]
        for tier in (1, 0):
            decoded = batch["decoded"][tier]
            if which == "dijkstra":
                code = decoded["code"]
                guess = torch.zeros((code.shape[0], protocol.TRAV_CLASSES, code.shape[1], code.shape[2]),
                                    dtype=torch.float32, device=code.device)
                guess[:, 0] = 1.0
                factor = classical_factor(guess, self.tables, code, decoded["height"])
            else:
                factor = self.oracle_table[batch["classes"][tier]]
            ring = self.frame(planned[1]) if tier == 0 else None
            planned[tier], _ = classical_cost_to_go(factor, batch["goal_plane"][tier], TIER_CELLS[tier],
                                                    CLASSICAL_ITERATIONS[tier], ring)
        costs = [cost for cost in planned]
        return costs, [direction_logits(cost, cell) for cost, cell in zip(costs, TIER_CELLS)]


def pass_mark(model: Scores, dijkstra: Scores) -> tuple[bool, str]:
    """The school's pass mark on the held-out split: the model's dir_acc_body_unseen at least 10 points over
    Dijkstra's and its dir_acc_body_seen no more than 2 points under. Returns (passed, the line)."""
    unseen = model.value("dir_acc_body_unseen") - dijkstra.value("dir_acc_body_unseen")
    seen = model.value("dir_acc_body_seen") - dijkstra.value("dir_acc_body_seen")
    passed = unseen >= 0.10 and seen >= -0.02 and model.count("dir_acc_body_unseen") > 0
    return passed, (f"Pass mark (held out): dir_acc_body_unseen {model.value('dir_acc_body_unseen'):.3f} vs "
                    f"dijkstra {dijkstra.value('dir_acc_body_unseen'):.3f} ({unseen * 100:+.1f} points, needs >= +10; "
                    f"{model.count('dir_acc_body_unseen'):.0f} rows); dir_acc_body_seen "
                    f"{model.value('dir_acc_body_seen'):.3f} vs {dijkstra.value('dir_acc_body_seen'):.3f} "
                    f"({seen * 100:+.1f} points, needs >= -2; {model.count('dir_acc_body_seen'):.0f} rows): "
                    + ("PASSED" if passed else "not passed"))
