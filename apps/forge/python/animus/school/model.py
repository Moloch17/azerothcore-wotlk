"""**The school's planners.** Both read the same inputs a tier (`tier_inputs`): the crop's code as a one-hot
(MAP_CODES), its entity class through a small embedding, decode_map's seven value planes (height, known, visited,
age, frontier, searched, walls), the goal plane and the memory flag as a constant plane -- INPUT_PLANES (18) -- and
on the fine tier two more, the coarse pass's prediction resampled onto the fine crop as the hierarchy resamples it
(`CoarseFrame`, the mirror of networks.MapEncoder.coarse_frame: bilinear at the fine cells' centres and the ring just
past the border; the fine network reads the interior, the VIN holds the ring too): the coarse log cost-to-go and its
reachability. Both give per tier a cost-to-go [N, H, W] in yards, a reachable logit [N, H, W], eight direction
logits [N, 8, H, W] (NEIGHBOURS order) and TRAV_CLASSES class logits [N, C, H, W].

`SchoolPlanner` (the primary): two U-Nets of four levels (48 -> 24 -> 12 -> 6; `widths` channels), the coarse first,
plain Conv2d + SiLU, max-pool down and nearest-neighbour up with skips -- exportable (the one op beyond convolutions
is the hierarchy's grid_sample, kept in CoarseFrame so an export can replace it with a fixed interpolation matrix).
**Its cost head predicts log(1 + cost)**: the cost-to-go runs from 0 to several hundred yards with a long tail, the
walk (networks.descend) only needs the ordering, which log1p keeps, the body's neighbourhood -- the cells the
waypoint is read from -- is where a yard of error matters and where log1p spends its resolution, and the sentinel's
cliff (65504) is the reachable head's job, not the regression's; cost/100 would spend the same gradient on a 300-yd
cell's 10 % as on a 3-yd cell's 1000 %. The metrics convert back with expm1.

`VinPlanner` (the comparison): networks.Planner -- the value iteration network the policy's map encoder carries --
over the same input planes plus a tier one-hot (the tier embedding), `k` iterations, one network for both tiers; the
hierarchy as MapEncoder.plan runs it (the coarse value the fine tier's start and held ring), the goal's cells held
at 0 every iteration in place of the encoder's bound (`AnchoredPlanner`). Its cost
is -value x 100 yards as the encoder's; its direction logits are the cost field's own (-neighbours / cell), its
reachable logit a 1 x 1 convolution over the value and the planes, its classes a two-layer head on the planes.
"""

from __future__ import annotations

import math

import torch
from torch import nn

from ..mappo import networks
from ..mappo.networks import MAP_CODES, MAP_VALUE_PLANES, NEIGHBOURS, Planner
from ..protocol import TRAV_CLASSES
from .data import SIDE, TIER_CELLS

CLASS_EMBED = 4
INPUT_PLANES = MAP_CODES + CLASS_EMBED + MAP_VALUE_PLANES + 2
COARSE_PLANES = 2
#: log1p of the cost the coarse plane is scaled by (a 600-yd route reads 1).
LOG_SCALE = math.log1p(600.0)
MODELS = ("unet", "vin")


class CoarseFrame(nn.Module):
    """The hierarchy's resample (networks.MapEncoder.coarse_frame's grid, built the same way: fine cell i's centre is
    (c - i) x fine yards ahead, c = (side - 1) / 2, which is coarse cell c - (c - i) x fine / coarse; grid_sample's
    normalised coordinates with align_corners, -1 and 1 the coarse crop's first and last cells' centres).
    `forward(coarse [N, H, W]) -> [N, H + 2, W + 2]`: the fine crop's cells and the ring just past its border."""

    def __init__(self, side: int = SIDE, cells: tuple[float, float] = TIER_CELLS):
        super().__init__()
        ratio = cells[0] / cells[1]
        centre = (side - 1) / 2.0
        rows = centre + (torch.arange(-1, side + 1, dtype=torch.float32) - centre) * ratio
        y, x = torch.meshgrid(rows / (side - 1) * 2.0 - 1.0, rows / (side - 1) * 2.0 - 1.0, indexing="ij")
        self.register_buffer("grid", torch.stack([x, y], dim=-1)[None], persistent=False)

    def forward(self, coarse: torch.Tensor) -> torch.Tensor:
        grid = self.grid.expand(coarse.shape[0], -1, -1, -1)
        return nn.functional.grid_sample(coarse.to(torch.float32)[:, None], grid, mode="bilinear",
                                         padding_mode="border", align_corners=True)[:, 0]


def tier_inputs(decoded: dict[str, torch.Tensor], goal: torch.Tensor, memory: torch.Tensor,
                class_embed: nn.Embedding) -> torch.Tensor:
    """One tier's input planes [N, INPUT_PLANES, H, W] (the module docstring) from decode_map's dict, the goal plane
    [N, H, W] and the memory flag [N]."""
    code = nn.functional.one_hot(decoded["code"], MAP_CODES).to(torch.float32)
    cls = class_embed(decoded["class"])
    flag = memory.to(torch.float32)[:, None, None].expand(-1, goal.shape[1], goal.shape[2])
    planes = torch.cat([code, cls, decoded["values"].to(torch.float32), goal[..., None], flag[..., None]], dim=-1)
    return planes.permute(0, 3, 1, 2).contiguous()


def direction_logits(cost: torch.Tensor, cell: float) -> torch.Tensor:
    """A cost field's own directions [N, 8, H, W]: minus each neighbour's cost over the cell's yards, a neighbour past
    the border a very low logit (the trainer's rule)."""
    inside = networks.neighbour_stack(torch.ones_like(cost[:1]), 0.0) > 0.0
    logits = -networks.neighbour_stack(cost.to(torch.float32), 0.0) / float(cell)
    return torch.where(inside, logits, torch.full_like(logits, -1e9))


def _block(inputs: int, outputs: int) -> nn.Sequential:
    return nn.Sequential(nn.Conv2d(inputs, outputs, 3, padding=1), nn.SiLU(),
                         nn.Conv2d(outputs, outputs, 3, padding=1), nn.SiLU())


class UNet(nn.Module):
    """Four levels over 48 x 48: `widths` channels a level, two 3 x 3 convolutions a block, max-pool down, nearest
    up, skip concatenations; a 1 x 1 head to `outputs` planes."""

    def __init__(self, inputs: int, outputs: int, widths: tuple[int, ...] = (32, 64, 96, 128)):
        super().__init__()
        self.down = nn.ModuleList()
        last = inputs
        for width in widths:
            self.down.append(_block(last, width))
            last = width
        self.up = nn.ModuleList()
        for width in reversed(widths[:-1]):
            self.up.append(_block(last + width, width))
            last = width
        self.head = nn.Conv2d(last, outputs, 1)
        nn.init.zeros_(self.head.bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        skips = []
        for index, block in enumerate(self.down):
            x = block(x)
            if index < len(self.down) - 1:
                skips.append(x)
                x = nn.functional.max_pool2d(x, 2)
        for block in self.up:
            skip = skips.pop()
            x = nn.functional.interpolate(x, size=skip.shape[-2:], mode="nearest")
            x = block(torch.cat([x, skip], dim=1))
        return self.head(x)


#: The heads' planes in a U-Net's output: cost (log1p), reachable logit, eight directions, the classes.
HEAD_COST, HEAD_REACH, HEAD_DIR, HEAD_CLS = 0, 1, 2, 2 + len(NEIGHBOURS)
HEAD_PLANES = HEAD_CLS + TRAV_CLASSES


class SchoolPlanner(nn.Module):
    """The two-tier U-Net planner (the module docstring). `forward(decoded, goal_planes, memory)` -> per-tier lists
    (fine first) "cost" (yards), "cost_log" (log1p), "reach" (logit), "dir" [N, 8, H, W], "cls" [N, C, H, W]."""

    cost_space = "log1p"

    def __init__(self, widths: tuple[int, ...] = (32, 64, 96, 128), class_embed: int = CLASS_EMBED):
        super().__init__()
        self.class_embed = nn.Embedding(networks.CLASS_LIMIT, class_embed)
        planes = MAP_CODES + class_embed + MAP_VALUE_PLANES + 2
        self.coarse = UNet(planes, HEAD_PLANES, tuple(widths))
        self.fine = UNet(planes + COARSE_PLANES, HEAD_PLANES, tuple(widths))
        self.frame = CoarseFrame()

    def forward(self, decoded: list[dict[str, torch.Tensor]], goal_planes: list[torch.Tensor],
                memory: torch.Tensor) -> dict[str, list[torch.Tensor]]:
        coarse_in = tier_inputs(decoded[1], goal_planes[1], memory, self.class_embed)
        coarse_out = self.coarse(coarse_in)
        coarse_log = coarse_out[:, HEAD_COST].clamp(min=0.0)
        coarse_reach = torch.sigmoid(coarse_out[:, HEAD_REACH])
        # The coarse pass onto the fine crop (the ring's interior): its log cost, scaled, and its reachability.
        frame_log = self.frame(coarse_log)[:, 1:-1, 1:-1] / LOG_SCALE
        frame_reach = self.frame(coarse_reach)[:, 1:-1, 1:-1]
        fine_in = torch.cat([tier_inputs(decoded[0], goal_planes[0], memory, self.class_embed),
                             frame_log[:, None], frame_reach[:, None]], dim=1)
        fine_out = self.fine(fine_in)
        outs = (fine_out, coarse_out)
        logs = [out[:, HEAD_COST].clamp(min=0.0) for out in outs]
        return {"cost": [torch.expm1(log) for log in logs], "cost_log": logs,
                "reach": [out[:, HEAD_REACH] for out in outs],
                "dir": [out[:, HEAD_DIR:HEAD_CLS] for out in outs],
                "cls": [out[:, HEAD_CLS:HEAD_PLANES] for out in outs]}


class AnchoredPlanner(Planner):
    """networks.Planner with the goal held: every iteration the cells of `hold` [N, H, W] (the goal plane) are put
    back to 0 and the rest left unclamped -- the Bellman anchor the encoder's VIN has to learn through the goal
    plane's weight under its bound (value at or below 0). The smoke's VIN under that bound never moved: at its start
    the value is 0 nearly everywhere, and a value held at the clamp has no gradient. `forward(planes, hold, init,
    ring)` is the parent's with the anchor carried through the checkpointed segments."""

    def _anchored(self, value: torch.Tensor, fixed: torch.Tensor, hold: torch.Tensor, count: int,
                  ring: torch.Tensor | None) -> torch.Tensor:
        keep = 1.0 - hold
        for _ in range(count):
            if ring is None:
                value = (self.recur(value) + fixed).amax(dim=1, keepdim=True)
            else:
                padded = nn.functional.pad(value, (1, 1, 1, 1)) + ring
                value = (nn.functional.conv2d(padded, self.recur.weight) + fixed).amax(dim=1, keepdim=True)
            value = value * keep
        return value

    def forward(self, planes: torch.Tensor, hold: torch.Tensor | None = None, init: torch.Tensor | None = None,
                ring: torch.Tensor | None = None, **_) -> torch.Tensor:
        if hold is None:
            raise ValueError("AnchoredPlanner needs the goal plane to hold")
        fixed = self.fixed(planes.to(self.fixed.weight.dtype))
        hold = hold.to(fixed.dtype)[:, None]
        if init is None:
            value = fixed.new_zeros((planes.shape[0], 1, planes.shape[-2], planes.shape[-1]))
        else:
            value = init[:, None].to(fixed.dtype)
        if ring is not None:
            inside = torch.ones_like(ring[:1, 1:-1, 1:-1])
            ring = (ring * (1.0 - nn.functional.pad(inside, (1, 1, 1, 1))))[:, None].to(fixed.dtype)
        total, done = self.iterations, 0
        while done < total:
            count = min(self.SEGMENT, total - done)
            if torch.is_grad_enabled() and fixed.requires_grad:
                from torch.utils.checkpoint import checkpoint
                value = checkpoint(self._anchored, value, fixed, hold, count, ring, use_reentrant=False)
            else:
                value = self._anchored(value, fixed, hold, count, ring)
            done += count
        return value[:, 0]


class VinPlanner(nn.Module):
    """The value iteration comparison (the module docstring); the same outputs as SchoolPlanner."""

    cost_space = "yards"

    def __init__(self, k: int = 40, class_embed: int = CLASS_EMBED, channels: int = 32):
        super().__init__()
        self.class_embed = nn.Embedding(networks.CLASS_LIMIT, class_embed)
        planes = MAP_CODES + class_embed + MAP_VALUE_PLANES + 2 + 2   # + the tier one-hot
        self.planner = AnchoredPlanner(planes, k)
        # Every iteration starts a step below 0 (networks.Planner's own init leaves the value at 0), so the value
        # falls with the distance from the held goal from the first update.
        with torch.no_grad():
            self.planner.fixed.bias.fill_(-0.02)
        self.reach = nn.Conv2d(planes + 1, 1, 1)
        self.classes = nn.Sequential(nn.Conv2d(planes, channels, 3, padding=1), nn.SiLU(),
                                     nn.Conv2d(channels, TRAV_CLASSES, 3, padding=1))
        self.frame = CoarseFrame()

    def _planes(self, decoded, goal, memory, tier: int) -> torch.Tensor:
        planes = tier_inputs(decoded, goal, memory, self.class_embed)
        one_hot = planes.new_zeros((planes.shape[0], 2, planes.shape[-2], planes.shape[-1]))
        one_hot[:, tier] = 1.0
        return torch.cat([planes, one_hot], dim=1)

    def forward(self, decoded: list[dict[str, torch.Tensor]], goal_planes: list[torch.Tensor],
                memory: torch.Tensor) -> dict[str, list[torch.Tensor]]:
        coarse_planes = self._planes(decoded[1], goal_planes[1], memory, 1)
        coarse_value = self.planner(coarse_planes, goal_planes[1])
        ring = self.frame(coarse_value)
        fine_planes = self._planes(decoded[0], goal_planes[0], memory, 0)
        fine_value = self.planner(fine_planes, goal_planes[0], init=ring[:, 1:-1, 1:-1], ring=ring)
        values, planes = (fine_value, coarse_value), (fine_planes, coarse_planes)
        costs = [(-value * 100.0).to(torch.float32) for value in values]
        return {"cost": costs, "cost_log": [torch.log1p(cost.clamp(min=0.0)) for cost in costs],
                "reach": [self.reach(torch.cat([value[:, None], plane], dim=1))[:, 0]
                          for value, plane in zip(values, planes)],
                "dir": [direction_logits(cost, cell) for cost, cell in zip(costs, TIER_CELLS)],
                "cls": [self.classes(plane) for plane in planes]}


def build(name: str, **kwargs) -> nn.Module:
    """A planner by its config name (MODELS): "unet" SchoolPlanner(widths), "vin" VinPlanner(k)."""
    if name == "unet":
        return SchoolPlanner(widths=tuple(kwargs.get("widths", (32, 64, 96, 128))))
    if name == "vin":
        return VinPlanner(k=int(kwargs.get("k", 40)))
    raise ValueError(f"school model {name!r} is not one of {MODELS}")


def parameter_count(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters())
