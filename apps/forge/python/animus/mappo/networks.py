"""Layout-aware actor and centralized critic for MAPPO.

One set of weights serves every agent of every layout (parameter sharing). A layout is one kind of agent --
a class/build, say -- with its own observation features and actions. Each network has:

- an input adapter per layout: Linear(layout obs dim -> width), reading only that layout's features;
- a shared trunk: every hidden layer after the first, the same for all layouts;
- (actor) an action head per layout: Linear(width -> layout action count).

So what is learned about moving, threat, healing or interrupts is shared through the trunk, while every layout
keeps its exact observation and action spaces. For a single layout this is exactly the previous plain MLP, and
adapter + trunk + head of one layout is a plain MLP too (see animus.export).

The critic adds the global state: a state encoder Linear(state dim -> width) is summed with the agent's layout
adapter over its own observation, so each agent's value sees the whole env and its own situation
("agent-specific global state").

Agents of a layout are identified by their features, not by a seat index: the same network plays any seat.
"""

from __future__ import annotations

import math
from collections.abc import Sequence

import numpy as np
import torch
from torch import nn
from torch.distributions import Categorical

MASKED_LOGIT = -1e9


def _linear(in_dim: int, out_dim: int, gain: float) -> nn.Linear:
    linear = nn.Linear(in_dim, out_dim)
    nn.init.orthogonal_(linear.weight, gain=gain)
    nn.init.zeros_(linear.bias)
    return linear


def masked_logits(logits: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    """The logits of allowed actions only, the others MASKED_LOGIT. A row with nothing allowed falls back to action 0."""
    mask = mask.bool()
    # Chosen on the device, not with `if empty.any()`: on a GPU that read waits for everything queued before it.
    empty = ~mask.any(dim=-1, keepdim=True)
    fallback = torch.zeros_like(mask)
    fallback[..., 0] = True
    mask = torch.where(empty, fallback, mask)
    return logits.masked_fill(~mask, MASKED_LOGIT)


def masked_distribution(logits: torch.Tensor, mask: torch.Tensor) -> Categorical:
    """Categorical over allowed actions only. A row with nothing allowed falls back to action 0."""
    return Categorical(logits=masked_logits(logits, mask))


def log_prob_of(logits: torch.Tensor, choice: torch.Tensor) -> torch.Tensor:
    """log softmax(logits)[choice]: Categorical(logits=...).log_prob(choice), without building the distribution."""
    return logits.gather(-1, choice.long()[..., None]).squeeze(-1) - torch.logsumexp(logits, dim=-1)


def _entropy(logits: torch.Tensor) -> torch.Tensor:
    """The entropy of softmax(logits) per row, masked logits included (they carry ~0 probability)."""
    log_p = logits - torch.logsumexp(logits, dim=-1, keepdim=True)
    return -(log_p.exp() * log_p).sum(-1)


def sample_logits(logits: torch.Tensor, deterministic: bool = False) -> tuple[torch.Tensor, torch.Tensor]:
    """(choice, its log probability) from unnormalised logits, as Categorical(logits=...).sample() and log_prob draw
    them, in a handful of kernels instead of the ~30 the distribution's normalising, softmax, multinomial and checks
    take: the rollout's inner loop. Gumbel-max: argmax(logits + G), G = -log(-log U), is an exact draw. U is in
    [0, 1), so U = 0 gives G = -inf, never +inf: a masked action cannot win on a lucky draw."""
    if deterministic:
        choice = logits.argmax(dim=-1)
    else:
        choice = (logits - torch.log(-torch.log(torch.rand_like(logits)))).argmax(dim=-1)
    return choice, log_prob_of(logits, choice)


class RunningNorm(nn.Module):
    """Per-feature mean and variance of the observations the policy has been trained on.

    The features arrive on wildly different scales -- a level in 1..80, yards, fractions of health, gear
    ratings -- and meet `tanh` as the first nonlinearity, which saturates on anything far from zero. Centring
    and scaling them is what lets the first layer see all of them at once.

    It is an affine map with no clipping, so `animus.export` can fold it into the adapter that follows and the
    exported model stays an ordinary MLP. The statistics are buffers: they travel in the checkpoint, and a
    stage that seeds from another carries them across for the blocks it keeps (animus.bootstrap).
    """

    def __init__(self, dim: int, epsilon: float = 1e-5):
        super().__init__()
        self.epsilon = epsilon
        self.register_buffer("mean", torch.zeros(dim))
        self.register_buffer("var", torch.ones(dim))
        self.register_buffer("count", torch.zeros(()))
        # Folded into the layer that reads it (fold_into): the rollout copies pass rows through untouched.
        self.bypass = False

    @torch.no_grad()
    def update(self, rows: torch.Tensor, ranks=None) -> None:
        """Fold a batch of observations into the statistics (Chan's parallel variance). With data-parallel `ranks`
        (animus.parallel) the batch is every rank's rows together, so every rank's statistics stay the same; every
        rank must then call it, with or without rows of its own."""
        if ranks is not None and ranks.active:
            wide = rows.to(torch.float64)
            packed = ranks.sum(torch.cat([wide.new_tensor([float(rows.shape[0])]), wide.sum(dim=0),
                                          (wide * wide).sum(dim=0)]))
            if float(packed[0]) == 0.0:
                return
            width = rows.shape[1]
            batch_count = packed[0].to(torch.float32)
            batch_mean = (packed[1:1 + width] / packed[0]).to(torch.float32)
            batch_var = (packed[1 + width:] / packed[0] - (packed[1:1 + width] / packed[0]) ** 2).clamp(min=0.0)
            batch_var = batch_var.to(torch.float32)
        else:
            if rows.shape[0] == 0:
                return
            batch_count = torch.tensor(float(rows.shape[0]), device=self.count.device)
            batch_mean, batch_var = rows.mean(dim=0), rows.var(dim=0, unbiased=False)
        total = self.count + batch_count
        delta = batch_mean - self.mean
        self.mean += delta * (batch_count / total)
        self.var.copy_((self.var * self.count + batch_var * batch_count
                        + delta.pow(2) * (self.count * batch_count / total)) / total)
        self.count.copy_(total)

    def forward(self, rows: torch.Tensor) -> torch.Tensor:
        if self.bypass:
            return rows
        # Nothing seen yet: the raw features are the best estimate of themselves. Chosen on the device rather than by
        # reading the count back: on a GPU that read waits for every queued kernel, once per layout per forward pass.
        normalised = (rows - self.mean) / torch.sqrt(self.var + self.epsilon)
        return torch.where(self.count > 0.0, normalised, rows)

    @torch.no_grad()
    def scale(self) -> tuple[torch.Tensor, torch.Tensor]:
        """(mean, standard deviation) as the fold in `animus.export` needs them; identity before any update."""
        if float(self.count) == 0.0:
            return torch.zeros_like(self.mean), torch.ones_like(self.var)

        return self.mean.clone(), torch.sqrt(self.var + self.epsilon)


@torch.no_grad()
def fold_into(norm: RunningNorm, linear: nn.Linear) -> None:
    """Fold `norm` into the `linear` that reads its output, as animus.export does for the exported models, and
    bypass it: W' = W / sigma, b' = b - W (mu / sigma). The same map in one matrix product instead of five
    elementwise passes before it, which on the rollout's 128-row batches is most of what normalising cost."""
    mean, std = norm.scale()
    linear.bias.sub_(linear.weight @ (mean / std))
    linear.weight.div_(std[None, :])
    norm.bypass = True


class DenseLayouts:
    """One linear layer per layout (the adapters, the action heads), as one matrix over every layout: each row goes
    through all of them in a single product and keeps its own layout's slice. A layout's input past its width is the
    padding the sim writes as zeros, so its slice is exactly its own layer's output. For a rollout on the GPU, where
    ten small products a network cost more in launches than one ten times the size costs in arithmetic. Built from
    the layers as they are; rebuilt after the weights change (MappoTrainer._sync_rollout).
    """

    @torch.no_grad()
    def __init__(self, linears, in_width: int):
        self.layouts = len(linears)
        self.out = max(linear.out_features for linear in linears)
        first = linears[0].weight
        self.weight = first.new_zeros((in_width, self.layouts * self.out))
        self.bias = first.new_zeros(self.layouts * self.out)
        self.valid = torch.zeros((self.layouts, self.out), dtype=torch.bool, device=first.device)
        for index, linear in enumerate(linears):
            self.valid[index, : linear.out_features] = True
        self.refresh(linears)

    @torch.no_grad()
    def refresh(self, linears) -> None:
        """The layers' current weights, written into this matrix in place: a rollout graph (MappoTrainer) captured
        these tensors' addresses, so the weights after a sync have to land in them rather than in new ones."""
        for index, linear in enumerate(linears):
            columns = slice(index * self.out, index * self.out + linear.out_features)
            self.weight[: linear.in_features, columns] = linear.weight.t()
            self.bias[columns] = linear.bias

    def __call__(self, rows: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """rows [N, in_width], layout [N] -> each row's own layer's output, [N, out] (padded past its width)."""
        every = torch.addmm(self.bias, rows, self.weight).view(rows.shape[0], self.layouts, self.out)
        return every.gather(1, layout.long().view(-1, 1, 1).expand(-1, 1, self.out)).squeeze(1)


class SharedInputDense:
    """Two DenseLayouts that read the same rows (the actor's and the critic's adapters both read the observation) as
    one product: [first | second] stacked, and computed transposed -- W^T x^T -- which is the shape the GPU's
    library runs well at a rollout's ~100 rows (96 x 909 x 2560: 59 us, against 113 us for the two products as
    they were). For the rollout graph (MappoTrainer); refreshed in place after each sync, as DenseLayouts are."""

    @torch.no_grad()
    def __init__(self, first: DenseLayouts, second: DenseLayouts):
        assert (first.layouts, first.out, first.weight.shape[0]) == (second.layouts, second.out, second.weight.shape[0])
        self.layouts, self.out = first.layouts, first.out
        self.weight_t = torch.empty((2 * first.weight.shape[1], first.weight.shape[0]), device=first.weight.device)
        self.bias = torch.empty((2 * first.weight.shape[1], 1), device=first.weight.device)
        self.refresh(first, second)

    @torch.no_grad()
    def refresh(self, first: DenseLayouts, second: DenseLayouts) -> None:
        width = first.weight.shape[1]
        self.weight_t[:width].copy_(first.weight.t())
        self.weight_t[width:].copy_(second.weight.t())
        self.bias[:width, 0].copy_(first.bias)
        self.bias[width:, 0].copy_(second.bias)

    def __call__(self, rows: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """rows [N, in], layout [N] -> (first's output, second's) for each row's own layout, each [N, out]."""
        every = torch.addmm(self.bias, self.weight_t, rows.t()).view(2, self.layouts, self.out, rows.shape[0])
        index = layout.long().view(1, 1, 1, -1).expand(2, 1, self.out, rows.shape[0])
        own = every.gather(1, index).squeeze(1).transpose(1, 2).contiguous()
        return own[0], own[1]


class _Trunk(nn.Module):
    """tanh, then Linear + tanh for every hidden layer after the first."""

    def __init__(self, hidden: Sequence[int]):
        super().__init__()
        self.layers = nn.ModuleList(_linear(a, b, math.sqrt(2)) for a, b in zip(hidden, hidden[1:]))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = torch.tanh(x)
        for layer in self.layers:
            x = torch.tanh(layer(x))
        return x


def skip_distribution_checks() -> None:
    """Stop torch validating every Categorical it builds and every sample it scores.

    The checks (finite logits, a sample inside the support) run on every rollout decision and every minibatch of
    every epoch, and the logits here are built by this module, not by a user. Call it once at start-up.
    """
    torch.distributions.Distribution.set_default_validate_args(False)


@torch.no_grad()
def update_norms(norms: nn.ModuleList, obs: torch.Tensor, layout: torch.Tensor, obs_dims, ranks=None) -> None:
    """Fold a rollout's observations into each layout's statistics, each from its own rows and own features. With
    data-parallel `ranks` every layout is visited on every rank, rows or none: each is a collective."""
    if ranks is not None and ranks.active:
        for index, norm in enumerate(norms):
            norm.update(obs[layout == index, : obs_dims[index]], ranks)
        return
    for index, rows in _per_layout(layout, len(norms)):
        norms[index].update(obs[rows, : obs_dims[index]])


def per_layout(layout: torch.Tensor, count: int) -> list[tuple[int, torch.Tensor]]:
    """(layout, row indices) for every layout present in the flat batch `layout`, for the forward passes to share: the
    grouping is a unique and a nonzero per layout, which with 18 layouts is most of a small batch's forward cost."""
    return _per_layout(layout.reshape(-1), count)


def to_device(tensor: torch.Tensor, device: torch.device) -> torch.Tensor:
    """A small host tensor onto `device` without waiting: from pageable memory a copy to the GPU blocks the host until
    every kernel queued before it has run, which turns each index upload into a pipeline drain."""
    if device.type != "cuda":
        return tensor.to(device)
    return tensor.pin_memory().to(device, non_blocking=True)


def per_layout_host(layout: np.ndarray, count: int, device: torch.device) -> list[tuple[int, torch.Tensor]]:
    """per_layout for a batch whose layouts are still on the host (the rollout buffer's): the grouping is done in
    numpy and only the row indices go to the device, so building it never waits on the GPU."""
    flat = np.asarray(layout).reshape(-1)
    if count == 1:
        return [(0, to_device(torch.arange(flat.shape[0]), device))]
    return [(int(index), to_device(torch.from_numpy(np.flatnonzero(flat == index)), device))
            for index in np.unique(flat)]


def _per_layout(layout: torch.Tensor, count: int) -> list[tuple[int, torch.Tensor]]:
    """(layout, row indices) for every layout present in the batch."""
    if count == 1:
        return [(0, torch.arange(layout.shape[0], device=layout.device))]
    layout = layout.long()
    present = torch.unique(layout).tolist()
    return [(int(index), torch.nonzero(layout == index, as_tuple=True)[0]) for index in present]


def _carry_sequence_loop(cell: nn.GRUCell, size: int, encoded: torch.Tensor, memory: torch.Tensor,
                         dones: torch.Tensor) -> torch.Tensor:
    """_carry_sequence one cell step at a time: what runs on the CPU, and the reference the fused call is tested
    against."""
    carried = memory.reshape(-1, size)
    features = []
    for step in range(encoded.shape[0]):
        carried = cell(encoded[step], carried)
        features.append(carried)
        carried = carried * (~dones[step]).to(carried.dtype)[:, None]
    return torch.stack(features)


def _pieces(dones: torch.Tensor) -> tuple[torch.Tensor, ...]:
    """Cut every row of a [T, N] sequence after each episode end, into pieces with no reset inside them. Returns, on
    the CPU: each piece's row, first step and length, then for every (t, n) the piece it falls in and its position
    there."""
    steps, rows = dones.shape
    ended = dones.detach().to("cpu", torch.bool)
    # A piece starts at step 0 of every row, and after every episode end that has a step after it.
    starts = torch.zeros((steps, rows), dtype=torch.bool)
    starts[0] = True
    starts[1:] = ended[:-1]
    # Numbered row-major (row, then time), so each row's pieces are consecutive.
    flat = starts.t().reshape(-1)
    piece_of = (torch.cumsum(flat.to(torch.int64), 0) - 1).reshape(rows, steps).t().contiguous()     # [T, N]
    first = torch.nonzero(flat, as_tuple=True)[0]
    piece_row = first // steps
    piece_start = first % steps
    # A piece runs to the next piece's start on the same row, else to the end of the sequence.
    same_row = torch.cat([piece_row[1:] == piece_row[:-1], torch.tensor([False])])
    next_start = torch.cat([piece_start[1:], torch.tensor([steps])])
    piece_length = torch.where(same_row, next_start - piece_start, steps - piece_start)
    position_of = torch.arange(steps)[:, None] - piece_start[piece_of]                                  # [T, N]
    return piece_row, piece_start, piece_length, piece_of, position_of


def _carry_sequence(cell: nn.GRUCell, size: int, encoded: torch.Tensor, memory: torch.Tensor,
                    dones: torch.Tensor) -> torch.Tensor:
    """Run a GRU over a replayed sequence: `encoded` [T, N, H], `memory` [N, R] the state its first decision was
    taken with, `dones` [T, N] where an episode ended (the next decision starts cleared) -> [T, N, R]. Only the cell
    is sequential, which is the small part; everything before it runs in one pass. `dones` may be on the host even
    when the rest is on the GPU: the cut into pieces is made there, so passing it from the host saves a read back.

    On the GPU, one fused RNN call per sequence instead of stepping the cell: the rows are cut at episode ends into
    pieces that each start from their own memory (the carried one for a row's first piece, zero after an end), run
    packed through the library's GRU with the cell's own weights, and put back in [T, N] order. Stepping the cell
    was ~210,000 kernel launches per stage4_duel update; this took the update from 1.67 s to 1.29 s. (A hand-written
    persistent kernel was tried and lost to it: at 16 rows a minibatch is one workgroup walking 128 dependent steps.)
    """
    carried = memory.reshape(-1, size)
    steps, rows = encoded.shape[0], encoded.shape[1]
    if not encoded.is_cuda or steps == 0:
        return _carry_sequence_loop(cell, size, encoded, carried, dones)
    # In full precision whatever the update runs in (mappo.update_precision): under autocast the fused call below
    # is not the library's fused GRU but a cell per step, and the memory is what errors would compound through.
    if torch.is_autocast_enabled():
        with torch.autocast("cuda", enabled=False):
            return _carry_sequence(cell, size, encoded.float(), memory.float(), dones)

    piece_row, piece_start, piece_length, piece_of, position_of = _pieces(dones)
    device = encoded.device
    longest = int(piece_length.max())

    # [pieces, longest] gather of each piece's steps; positions past a piece's end are dropped by the packing.
    times = (piece_start[:, None] + torch.arange(longest)[None, :]).clamp(max=steps - 1)
    inputs = encoded[to_device(times, device), to_device(piece_row[:, None].expand_as(times).contiguous(), device)]

    first = torch.nonzero(piece_start == 0, as_tuple=True)[0]
    initial = carried.new_zeros((len(piece_row), size)).index_copy(0, to_device(first, device),
                                                                   carried[to_device(piece_row[first], device)])

    packed = nn.utils.rnn.pack_padded_sequence(inputs, piece_length, batch_first=True, enforce_sorted=False)
    hx = initial.index_select(0, packed.sorted_indices)[None]
    # What nn.GRU.forward calls for a packed sequence, given the cell's parameters rather than a module's own.
    output, _ = torch._VF.gru(packed.data, packed.batch_sizes, hx,
                              [cell.weight_ih, cell.weight_hh, cell.bias_ih, cell.bias_hh], True, 1, 0.0,
                              cell.training, False)
    padded, _ = nn.utils.rnn.pad_packed_sequence(
        nn.utils.rnn.PackedSequence(output, packed.batch_sizes, packed.sorted_indices, packed.unsorted_indices),
        batch_first=True, total_length=longest)
    return padded[to_device(piece_of, device), to_device(position_of, device)]


class GoalHead(nn.Module):
    """**Goals as a kind and a target** (long-horizon plan, Component C): a goal is kind * targets + target, one
    number on the wire and in the buffers. Its logits are the kind's, plus the target's, plus a learned table of the
    pair, over every (kind, target) -- a joint categorical parametrised in parts, so the target's score is shared
    across the kinds that can take it. Masked by the sim's goal block (GoalBlock: which kinds and targets are there
    now, the last block of each layout) and by which targets each kind accepts (stage.json "goals"). With one target
    it is exactly the old goal head over `kinds` goals."""

    def __init__(self, width: int, kinds: int, targets: int, layout_count: int, lookahead: bool = False,
                 slots: int = 1):
        super().__init__()
        self.kinds, self.targets = kinds, max(1, targets)
        # **Two goals and a queue** (next-run plan, Wave 4): with slots > 1 a choice draws the primary, then a
        # secondary held beside it and up to slots - 2 queued behind it, each conditioned on the ones before (a
        # learned bias per slot plus the embedding of what was drawn) and each able to say "none". The kind, target
        # and pair parameters are shared by every slot. The learner keeps the queue; the sim holds the two in front.
        self.slots = max(1, slots)
        # The entropy of each slot after the primary, as a share of the primary's (MappoConfig.goal_slot_entropy_weight).
        self.slot_entropy_weight = 0.1
        if self.slots > 1:
            self.slot_bias = nn.Parameter(torch.zeros(self.slots - 1, width))
            self.drawn = nn.Embedding(kinds * self.targets + 1, width)     # 0 = none, goal g at g + 1
            nn.init.zeros_(self.drawn.weight)
            self.none_bias = nn.Parameter(torch.zeros(self.slots - 1))
        self.kind = _linear(width, kinds, 0.01)
        self.target = _linear(width, self.targets, 0.01) if self.targets > 1 else None
        self.pair = nn.Parameter(torch.zeros(kinds, self.targets))
        # **Goal-level lookahead** (Component P, layer 3): for every candidate goal, the chance it is reached before
        # it ends and how long it would take (a share of a minute), predicted from the same features and trained on
        # what the chosen goals actually did. The choice reads them: the logits add each prediction times a learned
        # weight, so the policy chooses from what it expects of each goal rather than from a bare score.
        self.lookahead = lookahead
        if lookahead:
            self.success = _Factored(width, kinds, self.targets)
            self.duration = _Factored(width, kinds, self.targets)
            self.lookahead_weight = nn.Parameter(torch.tensor([1.0, -0.5]))
        self.register_buffer("accepts", torch.ones(kinds, self.targets, dtype=torch.bool))
        # Per layout, where its goal block starts in the observation; -1 for a layout without one (the director).
        self.register_buffer("block_at", torch.full((max(1, layout_count),), -1, dtype=torch.long))
        self.has_space = False

    @property
    def count(self) -> int:
        return self.kinds * self.targets

    def _load_from_state_dict(self, state_dict, prefix, *args, **kwargs):
        super()._load_from_state_dict(state_dict, prefix, *args, **kwargs)
        # A loaded goal space (a checkpoint's block positions) says itself whether there is one.
        self.has_space = bool((self.block_at >= 0).any())

    @property
    def block_width(self) -> int:
        """The goal block's columns (GoalBlock::Obs): kinds there, targets there, ended, reached, and from the
        next-run format on (slots > 1) the secondary ending, the event, the director's primary and what was
        achieved."""
        base = self.kinds + self.targets + 2
        return base if self.slots <= 1 else base + 3 + 2 * (self.kinds + self.targets)

    # Where the next-run columns sit in the goal block (GoalBlock::Obs; stage.json "goals"."columns").
    @property
    def columns(self) -> dict[str, int]:
        base = self.kinds + self.targets + 2
        return {"secondary_ended": base, "event": base + 1, "from_order": base + 2, "order_kind": base + 3,
                "order_target": base + 3 + self.kinds, "achieved_kind": base + 3 + self.kinds + self.targets,
                "achieved_target": base + 3 + 2 * self.kinds + self.targets}

    def set_space(self, accepts, block_at) -> None:
        """The goal space the sim wrote (stage.json "goals"): accepts [kinds][targets], and per layout the goal
        block's first observation column (-1 without one)."""
        self.accepts.copy_(torch.as_tensor(accepts, dtype=torch.bool, device=self.accepts.device))
        self.block_at.copy_(torch.as_tensor(block_at, dtype=torch.long, device=self.block_at.device))
        # Kept on the host: a rollout graph cannot read the device while it is captured.
        self.has_space = bool(np.any(np.asarray(block_at) >= 0))

    def _block(self, obs: torch.Tensor, layout: torch.Tensor):
        """The goal block's columns of flat rows, and which rows have one."""
        at = self.block_at[layout.long()]
        has = at >= 0
        columns = at.clamp(min=0)[:, None] + torch.arange(self.block_width, device=obs.device)[None, :]
        columns = columns.clamp(max=obs.shape[-1] - 1)
        return obs.gather(1, columns) > 0.5, has

    def signals(self, obs: torch.Tensor, layout: torch.Tensor) -> dict[str, torch.Tensor]:
        """What the goal block says of the goals held, per flat row: the primary ended, the secondary ended, an
        event (choose again now), the director's primary (from_order, order_goal) and what was achieved this decision
        (achieved, -1 for nothing)."""
        obs = obs.reshape(-1, obs.shape[-1])
        block, has = self._block(obs, layout.reshape(-1))
        rows = obs.shape[0]
        k, t = self.kinds, self.targets
        base = k + t
        out = {"ended": block[:, base] & has}
        if self.slots <= 1:
            none = torch.zeros(rows, dtype=torch.bool, device=obs.device)
            out.update(secondary_ended=none, event=none, from_order=none,
                       order_goal=torch.zeros(rows, dtype=torch.long, device=obs.device),
                       achieved=torch.full((rows,), -1, dtype=torch.long, device=obs.device))
            return out
        c = self.columns
        out["secondary_ended"] = block[:, c["secondary_ended"]] & has
        out["event"] = block[:, c["event"]] & has
        out["from_order"] = block[:, c["from_order"]] & has
        order_kind = block[:, c["order_kind"]:c["order_kind"] + k].float().argmax(-1)
        order_target = block[:, c["order_target"]:c["order_target"] + t].float().argmax(-1)
        out["order_goal"] = order_kind * t + order_target
        achieved_kind = block[:, c["achieved_kind"]:c["achieved_kind"] + k]
        achieved_target = block[:, c["achieved_target"]:c["achieved_target"] + t].float().argmax(-1)
        out["achieved"] = torch.where(achieved_kind.any(-1) & has,
                                      achieved_kind.float().argmax(-1) * t + achieved_target,
                                      torch.full_like(achieved_target, -1))
        return out

    def slot_logits(self, features: torch.Tensor, slot: int, drawn: list[torch.Tensor],
                    obs: torch.Tensor | None = None, layout: torch.Tensor | None = None) -> torch.Tensor:
        """Masked logits [rows, count + 1] of a slot after the primary, the last column being none: the shared
        parameters over the features plus the slot's bias and the embedding of what was drawn before it. The
        secondary and the queue are masked by what the goal block says is there, as the primary is: a queued goal
        about something absent was a draw from hundreds of goals that could never be pursued, and its entropy kept
        the whole head near uniform (next-run trial, 2026-09-30)."""
        shifted = features + self.slot_bias[slot - 1].to(features.dtype)
        for goal in drawn:
            shifted = shifted + self.drawn(goal.long() + 1).to(features.dtype)
        joint = self.kind(shifted)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(shifted)[:, None, :]
        allowed = self.accepts[None].expand(features.shape[0], -1, -1)
        if obs is not None and layout is not None:
            block, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            present = block[:, : self.kinds, None] & block[:, self.kinds : self.kinds + self.targets][:, None, :]
            allowed = torch.where(has[:, None, None], allowed & present, allowed)
        allowed = allowed.reshape(features.shape[0], -1)
        none = self.none_bias[slot - 1].to(features.dtype).expand(features.shape[0], 1)
        logits = torch.cat([masked_logits(joint.reshape(features.shape[0], -1), allowed), none], dim=-1)
        # A row whose layout has no goal block (the director) has only none.
        if obs is not None and layout is not None and self.has_space:
            _, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            only_none = torch.zeros_like(logits, dtype=torch.bool)
            only_none[:, -1] = True
            logits = torch.where(has[:, None], logits, logits.masked_fill(~only_none, MASKED_LOGIT))
        return logits

    def draw(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, primary_given: torch.Tensor,
             given: torch.Tensor, deterministic: bool, slots: torch.Tensor | None = None):
        """The slots drawn at a choice, or scored when `slots` [rows, S] is given: (slots [rows, S] with -1 for
        none, log probability, entropy). Where `given` the primary is `primary_given` (the director's order): it
        conditions the later slots but is not drawn, so its log probability and entropy are left out."""
        primary_logits = self.logits(features, obs, layout)
        if slots is None:
            primary, primary_lp = sample_logits(primary_logits, deterministic)
        else:
            primary = slots[:, 0].clamp(min=0)
            primary_lp = log_prob_of(primary_logits, primary)
        log_prob = torch.where(given, torch.zeros_like(primary_lp), primary_lp)
        entropy = torch.where(given, torch.zeros_like(primary_lp), _entropy(primary_logits))
        conditioned = torch.where(given, primary_given, primary)
        drawn, out = [conditioned], [primary]
        none = self.count
        for slot in range(1, self.slots):
            logits = self.slot_logits(features, slot, drawn, obs, layout)
            if slots is None:
                choice, lp = sample_logits(logits, deterministic)
            else:
                choice = torch.where(slots[:, slot] < 0, torch.full_like(slots[:, slot], none), slots[:, slot])
                lp = log_prob_of(logits, choice)
            log_prob = log_prob + lp
            # The primary is the plan; the rest beside it are weighed at slot_entropy_weight, so the entropy bonus
            # does not grow with the number of slots and keep the primary near uniform.
            entropy = entropy + self.slot_entropy_weight * _entropy(logits)
            goal = torch.where(choice == none, torch.full_like(choice, -1), choice)
            drawn.append(goal)
            out.append(goal)
        return torch.stack(out, dim=-1), log_prob, entropy

    def predictions(self, features: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The lookahead: per candidate goal [rows, kinds * targets], the logit of reaching it and the share of a
        minute it would take."""
        return self.success(features), torch.sigmoid(self.duration(features))

    def logits(self, features: torch.Tensor, obs: torch.Tensor | None = None,
               layout: torch.Tensor | None = None) -> torch.Tensor:
        """Masked joint logits [rows, kinds * targets]."""
        joint = self.kind(features)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(features)[:, None, :]
        if self.lookahead:
            # What each goal is expected to do, as the policy weighs it; the predictions learn from outcomes only.
            success, duration = self.predictions(features.detach())
            weight = self.lookahead_weight.to(features.dtype)
            joint = joint + (weight[0] * success.detach() + weight[1] * duration.detach()).reshape(joint.shape)
        allowed = self.accepts[None].expand(features.shape[0], -1, -1)
        if obs is not None and layout is not None and self.targets > 1:
            block, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            present = block[:, : self.kinds, None] & block[:, self.kinds : self.kinds + self.targets][:, None, :]
            # A layout with no goal block (the director) has no goals to choose: only the first, which means none.
            nothing = torch.zeros_like(allowed)
            allowed = torch.where(has[:, None, None], allowed & present, nothing if self.has_space else allowed)
        allowed = allowed.reshape(features.shape[0], -1).clone()
        # Always something to choose: the first goal (Fight about no one in particular) is never masked out.
        allowed[:, 0] = True
        return masked_logits(joint.reshape(features.shape[0], -1), allowed)

    def ended(self, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """Rows whose goal the sim says has just ended (GoalBlock::OBS_ENDED): they choose again now."""
        if self.targets <= 1:
            return torch.zeros(obs.reshape(-1, obs.shape[-1]).shape[0], dtype=torch.bool, device=obs.device)
        block, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
        return block[:, self.kinds + self.targets] & has


class _Factored(nn.Module):
    """A score per (kind, target) pair: the kind's plus the target's plus a table of the pair, as GoalHead's logits
    are built. [rows, kinds * targets]."""

    def __init__(self, width: int, kinds: int, targets: int):
        super().__init__()
        self.kinds, self.targets = kinds, targets
        self.kind = _linear(width, kinds, 0.01)
        self.target = _linear(width, targets, 0.01) if targets > 1 else None
        self.pair = nn.Parameter(torch.zeros(kinds, targets))

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        joint = self.kind(features)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(features)[:, None, :]
        return joint.reshape(features.shape[0], -1)


class DirectorSets(nn.Module):
    """**The director's members and enemies as sets** (long-horizon plan, Component E). Each slot of a set goes through
    one shared encoder; the encodings of the slots present are pooled (mean and max), so the director reads a pair, a
    party and a raid with the same weights and does not care which slot a member landed in. The raw slot columns are
    kept out of the layout's own adapter (column_mask), so nothing learns a slot's position. The actions that name a
    slot -- address this member, heal this member, focus this enemy -- are scored from that slot's encoding against a
    query from the features (pointer logits), so they move with the member rather than with the slot.

    `descriptor` is stage.json's "director" (DirectorLayout::SetDescriptor)."""

    def __init__(self, descriptor: dict, obs_dim: int, width: int, head_width: int = 0, embed: int = 64):
        super().__init__()
        self.sets = {}
        columns = torch.zeros(obs_dim, dtype=torch.bool)
        for name in ("seats", "enemies"):
            spec = descriptor[name]
            first, slots, per = int(spec["first"]), int(spec["slots"]), int(spec["width"])
            self.sets[name] = (first, slots, per, int(spec.get("present", 0)))
            columns[first : first + slots * per] = True
            setattr(self, f"{name}_encoder", nn.Sequential(_linear(per, embed, math.sqrt(2)), nn.Tanh(),
                                                           _linear(embed, embed, math.sqrt(2)), nn.Tanh()))
        self.register_buffer("column_mask", columns)
        self.pool = _linear(4 * embed, width, 1.0)
        self.pointers = [(int(p["first"]), p["over"]) for p in descriptor.get("pointers", ())] if head_width else []
        self.queries = nn.ModuleList(_linear(head_width, embed, 0.01) for _ in self.pointers)

    def encode_set(self, obs: torch.Tensor, name: str) -> tuple[torch.Tensor, torch.Tensor]:
        """[rows, slots, embed] encodings and [rows, slots] which are present."""
        first, slots, per, present = self.sets[name]
        raw = obs[:, first : first + slots * per].reshape(obs.shape[0], slots, per)
        return getattr(self, f"{name}_encoder")(raw), raw[..., present] > 0.5

    def pooled(self, obs: torch.Tensor) -> torch.Tensor:
        """What the sets add to the adapter's output: [rows, width]."""
        parts = []
        for name in ("seats", "enemies"):
            encoded, present = self.encode_set(obs, name)
            weight = present.to(encoded.dtype)[..., None]
            count = weight.sum(dim=1).clamp(min=1.0)
            parts.append((encoded * weight).sum(dim=1) / count)
            parts.append(torch.where(present[..., None], encoded, torch.full_like(encoded, -1.0)).amax(dim=1))
        return self.pool(torch.cat(parts, dim=-1))

    def pointer_logits(self, features: torch.Tensor, obs: torch.Tensor) -> list[tuple[int, torch.Tensor]]:
        """Per pointer action group: (its first action, [rows, slots] logits) from each slot's encoding."""
        encoded = {name: self.encode_set(obs, name)[0] for name in self.sets}
        return [(first, torch.einsum("rse,re->rs", encoded[over], query(features)))
                for (first, over), query in zip(self.pointers, self.queries)]


def _director_extra(sets: "DirectorSets | None", index: int, obs: torch.Tensor, layout: torch.Tensor):
    """What the director rows' sets add to the adapter's output [rows, width] (None without director rows)."""
    if sets is None:
        return None
    rows = layout.reshape(-1) == index
    if not bool(rows.any()):
        return None
    extra = obs.new_zeros(obs.shape[0], sets.pool.out_features)
    extra[rows] = sets.pooled(obs[rows, : sets.column_mask.shape[0]]).to(extra.dtype)
    return extra


def _attach_director(network: nn.Module, director, head_width: int = 0) -> None:
    """Give an actor or critic the director's set encoder (DirectorSets), and keep the director layout's own adapter
    blind to the slot columns: their weights start at zero and their gradient is masked, so they stay zero through
    every update and through the rollout copies' folded normalisation."""
    network.director_index = -1
    network.director_sets = None
    if director is None:
        return
    index, descriptor = director
    network.director_index = int(index)
    network.director_sets = DirectorSets(descriptor, network.obs_dims[index], network.adapters[index].out_features,
                                         head_width)
    keep = (~network.director_sets.column_mask).to(torch.float32)[None, :]
    network.register_buffer("director_keep", keep)
    weight = network.adapters[index].weight
    with torch.no_grad():
        weight.mul_(keep)
    weight.register_hook(lambda grad: grad * network.director_keep)


def attach_blind_columns(network: nn.Module, columns: dict[int, list[int]]) -> None:
    """Keep layout adapters blind to some of their observation columns (layout index -> columns): the weights start at
    zero and their gradient is masked, so they stay zero through every update and the rollout copies' folding. For
    columns the learner reads and the policy must not (the hint block: a suggestion imitated, never copied). Once per
    network; a later call only zeroes again."""
    for index, cols in columns.items():
        if not cols or index >= len(network.adapters):
            continue
        weight = network.adapters[index].weight
        name = f"blind_keep_{index}"
        keep = torch.ones((1, weight.shape[1]), dtype=weight.dtype, device=weight.device)
        keep[0, [c for c in cols if c < weight.shape[1]]] = 0.0
        if hasattr(network, name):
            getattr(network, name).copy_(keep)
        else:
            network.register_buffer(name, keep)
            weight.register_hook(lambda grad, network=network, name=name: grad * getattr(network, name))
        with torch.no_grad():
            weight.mul_(getattr(network, name))


def clear_director_columns(network: nn.Module) -> None:
    """Zero the director adapter's slot columns again (after a seed or a load brought weights from elsewhere)."""
    if getattr(network, "director_sets", None) is None:
        return
    with torch.no_grad():
        network.adapters[network.director_index].weight.mul_(network.director_keep)


class GoalEmbedding(nn.Module):
    """A goal as the features it adds: its kind's embedding plus its target's (zero at the start, so a new goal head
    changes nothing until it is trained).

    With `paired` the goal a seat holds is its pair, primary * (count + 1) + secondary + 1 (goal_pair): the same one
    number in every buffer and call, and the secondary adds its own embedding through a learned gate.

    The actor also scales the features by the goal (condition(): features x (1 + scale) + shift, FiLM). Added alone,
    the goal only moved each action's logit by a constant through the linear heads -- it could not change what the
    seat does in a situation, and swapping it changed the argmax action 13-21% of the time (next-run trials,
    2026-09-30). The scale starts at zero, so a new model acts as the shift alone did until it learns one."""

    def __init__(self, kinds: int, targets: int, width: int, paired: bool = False, scaled: bool = False):
        super().__init__()
        self.targets = max(1, targets)
        self.count = kinds * self.targets
        self.paired = paired
        self.kind = nn.Embedding(kinds, width)
        self.target = nn.Embedding(self.targets, width) if self.targets > 1 else None
        # The actor's scale (condition()); the critic reads the shift alone and has none.
        self.kind_scale = nn.Embedding(kinds, width) if scaled else None
        self.target_scale = nn.Embedding(self.targets, width) if scaled and self.targets > 1 else None
        nn.init.zeros_(self.kind.weight)
        if self.target is not None:
            nn.init.zeros_(self.target.weight)
        for table in (self.kind_scale, self.target_scale):
            if table is not None:
                nn.init.zeros_(table.weight)
        if paired:
            self.gate = nn.Parameter(torch.tensor(0.5))

    def _one(self, goal: torch.Tensor, scale: bool = False) -> torch.Tensor:
        kind, target = (self.kind_scale, self.target_scale) if scale else (self.kind, self.target)
        embedded = kind(torch.div(goal, self.targets, rounding_mode="floor"))
        if target is not None:
            embedded = embedded + target(goal % self.targets)
        return embedded

    def _held(self, goal: torch.Tensor, scale: bool) -> torch.Tensor:
        goal = goal.reshape(-1).long()
        if not self.paired:
            return self._one(goal, scale)
        primary, secondary = split_goal_pair(goal, self.count)
        extra = self._one(secondary.clamp(min=0), scale) * (secondary >= 0)[:, None].to(self.kind.weight.dtype)
        return self._one(primary, scale) + self.gate.to(self.kind.weight.dtype) * extra

    def forward(self, goal: torch.Tensor) -> torch.Tensor:
        """The shift alone (the critic's reading of the goal)."""
        return self._held(goal, False)

    def condition(self, features: torch.Tensor, goal: torch.Tensor) -> torch.Tensor:
        """The actor's features under `goal`: features x (1 + scale) + shift."""
        if self.kind_scale is None:
            return features + self._held(goal, False).to(features.dtype)
        scale = self._held(goal, True).to(features.dtype)
        return features * (1.0 + scale) + self._held(goal, False).to(features.dtype)


# The goal's scale on the actor's features (GoalEmbedding.condition, .amdl 7) starts at zero; an actor saved before it
# had none, and loads as exactly the actor it was.
_GOAL_SCALE_KEYS = ("goal_embedding.kind_scale.weight", "goal_embedding.target_scale.weight")


def load_actor_state(actor: nn.Module, state: dict) -> None:
    """actor.load_state_dict(state), except that an actor saved before the goal scale existed loads with it at zero."""
    missing, unexpected = actor.load_state_dict(state, strict=False)
    wrong = [key for key in missing if key not in _GOAL_SCALE_KEYS]
    if wrong or unexpected:
        raise RuntimeError(f"Error(s) in loading state_dict for {type(actor).__name__}: missing {wrong}, "
                           f"unexpected {list(unexpected)}")
    parameters = dict(actor.named_parameters())
    with torch.no_grad():
        for key in missing:
            parameters[key].zero_()


def goal_pair(primary: torch.Tensor, secondary: torch.Tensor, count: int) -> torch.Tensor:
    """The pair of goals a seat holds as one number: primary * (count + 1) + secondary + 1 (secondary -1: none)."""
    return primary.long() * (count + 1) + secondary.long() + 1


def split_goal_pair(pair: torch.Tensor, count: int) -> tuple[torch.Tensor, torch.Tensor]:
    """(primary, secondary) of goal_pair's number."""
    pair = pair.long()
    return torch.div(pair, count + 1, rounding_mode="floor"), pair % (count + 1) - 1


class LayoutActor(nn.Module):
    def __init__(self, layouts: Sequence[tuple[int, int]], hidden: Sequence[int], foresight_outputs: int = 0,
                 recurrent_size: int = 0, goal_count: int = 0, goal_targets: int = 1, slow_size: int = 0,
                 foresight_feedback: bool = False, lookahead: bool = False, director=None, goal_slots: int = 1):
        """layouts: (obs dim, action count) per layout; hidden: widths, the first being the adapters' output.

        `foresight_outputs` adds a head on the trunk that predicts what happens after this decision (mappo.trainer's
        foresight_*): one output per extra discount horizon and one for how much of the episode is left. Nothing reads
        it at rollout time; it is there to make the trunk the actions are chosen from carry the future, and it is left
        out of exported models.

        `recurrent_size` puts a GRU between the trunk and the heads, carried from decision to decision and cleared when
        an episode ends: the policy's own memory of what it has already done and seen, which no observation of the
        moment can hold (who was crowd-controlled, that the enemy has spent its trinket, where the adds came from).
        """
        super().__init__()
        if not hidden:
            raise ValueError("the actor needs at least one hidden layer")
        self.obs_dims = [obs for obs, _ in layouts]
        self.action_counts = [actions for _, actions in layouts]
        self.max_actions = max(self.action_counts)
        self.norms = nn.ModuleList(RunningNorm(obs) for obs, _ in layouts)
        self.adapters = nn.ModuleList(_linear(obs, hidden[0], math.sqrt(2)) for obs, _ in layouts)
        self.trunk = _Trunk(hidden)
        self.recurrent_size = recurrent_size
        self.memory = nn.GRUCell(hidden[-1], recurrent_size) if recurrent_size else None
        head_width = recurrent_size if recurrent_size else hidden[-1]
        self.head_width = head_width
        self.heads = nn.ModuleList(_linear(head_width, actions, 0.01) for _, actions in layouts)
        # The rollout copy's adapters and heads as DenseLayouts (densify); None everywhere else.
        self.dense_adapters: DenseLayouts | None = None
        self.dense_heads: DenseLayouts | None = None
        self.foresight_outputs = foresight_outputs
        self.foresight = _linear(head_width, foresight_outputs, 1.0) if foresight_outputs else None
        # The goal head (mappo.goal_count): a goal chosen every mappo.goal_every_decisions and kept in between, which
        # the action head is conditioned on. The goal chooser decides on a clock many times slower than the actions,
        # so its own horizon is that many times shorter -- which is where a plan can be learned at all.
        #
        # `goal_count` is the goal *kinds* and `goal_targets` the target space (GoalHead); self.goal_count is the
        # joint count, which is what the buffers, the wire and the stats count.
        #
        # **The two-clock seat** (Component D, `slow_size`): the goal is chosen by a slow loop of its own -- a GRU that
        # steps only when a goal is chosen, over the fast loop's features at that moment, so what it remembers spans
        # minutes of goals rather than seconds of decisions -- with its own value head, credited on its own clock
        # (MappoTrainer's slow goal update). Without it the goal head reads the fast features as before.
        self.slow_size = slow_size if goal_count else 0
        self.slow_memory = nn.GRUCell(head_width, self.slow_size) if self.slow_size else None
        self.slow_value = _linear(self.slow_size, 1, 1.0) if self.slow_size else None
        goal_width = self.slow_size or head_width
        self.goal_head = (GoalHead(goal_width, goal_count, goal_targets, len(layouts), lookahead, goal_slots)
                          if goal_count else None)
        self.goal_count = self.goal_head.count if self.goal_head is not None else 0
        self.goal_slots = self.goal_head.slots if self.goal_head is not None else 1
        self.goal_embedding = (GoalEmbedding(goal_count, goal_targets, head_width, self.goal_slots > 1, scaled=True)
                               if goal_count else None)
        # **Predictions fed back** (Component P, layer 2): the foresight head's outputs, detached, projected onto the
        # features the action head and the slow loop read, so the policy acts on what it expects to happen.
        self.foresight_feedback = bool(foresight_feedback and foresight_outputs)
        self.foresight_proj = _linear(foresight_outputs, head_width, 0.01) if self.foresight_feedback else None
        # The director's members and enemies as sets, with pointer heads (DirectorSets): `director` is (its layout
        # index, stage.json's "director").
        _attach_director(self, director, head_width)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                memory: torch.Tensor | None = None) -> Categorical:
        """obs [..., O], layout [...], mask [..., N] (padded) -> distribution over N actions. `groups` is
        per_layout(layout) when the caller already has it; `memory` [..., recurrent_size] the state carried in."""
        return self._forward(obs, layout, mask, groups, memory)[0]

    def step(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, memory: torch.Tensor | None = None,
             groups=None, goal: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, torch.Tensor]:
        """One decision: (distribution, memory carried out, foresight predictions). Without a GRU the memory out is
        whatever came in (an empty tensor), and without foresight heads the predictions are empty."""
        dist, features, lead = self._forward(obs, layout, mask, groups, memory, goal)
        predictions = (self.foresight(features).reshape(*lead, self.foresight_outputs) if self.foresight is not None
                       else features.new_zeros((*lead, 0)))
        carried = features.reshape(*lead, features.shape[-1]) if self.memory is not None else (
            memory if memory is not None else features.new_zeros((*lead, 0)))
        return dist, carried, predictions

    def fold_normalisation(self) -> None:
        """Fold every layout's RunningNorm into its adapter (fold_into): for the rollout copy only, which is
        overwritten from the trained network at every sync and folded again."""
        for norm, adapter in zip(self.norms, self.adapters):
            fold_into(norm, adapter)

    def densify(self, obs_width: int) -> None:
        """Adapters and heads as DenseLayouts, after fold_normalisation: for a rollout copy on the GPU only. Once
        built they are refreshed in place (DenseLayouts.refresh)."""
        if self.dense_adapters is None:
            self.dense_adapters = DenseLayouts(self.adapters, obs_width)
            self.dense_heads = DenseLayouts(self.heads, self.head_width)
        else:
            self.dense_adapters.refresh(self.adapters)
            self.dense_heads.refresh(self.heads)

    def initial_memory(self, *lead: int, device=None) -> torch.Tensor:
        """A cleared memory for `lead` rows (what an episode starts with)."""
        return torch.zeros((*lead, self.recurrent_size), dtype=torch.float32, device=device)

    def encode(self, obs: torch.Tensor, layout: torch.Tensor, groups=None) -> torch.Tensor:
        """Adapters and trunk for flat rows: everything that depends only on this decision's observation, before the
        GRU. A replayed sequence encodes every step in one pass and then carries the memory through them (carry),
        which is the difference between one large matmul per layer and one per step."""
        extra = _director_extra(self.director_sets, self.director_index, obs, layout)
        if self.dense_adapters is not None:
            hidden = self.dense_adapters(obs, layout)
            return self.trunk(hidden if extra is None else hidden + extra.to(hidden.dtype))
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        width = self.adapters[0].out_features
        hidden = obs.new_zeros(obs.shape[0], width)
        for index, rows in groups:
            # Cast on the way in: under autocast (mappo.update_precision) the adapters answer in half precision.
            hidden[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]])).to(
                hidden.dtype)
        if extra is not None:
            hidden = hidden + extra.to(hidden.dtype)
        return self.trunk(hidden)

    def carry(self, encoded: torch.Tensor, memory: torch.Tensor, dones: torch.Tensor) -> torch.Tensor:
        """Run the GRU over a sequence: `encoded` [T, N, H] from encode(), `memory` [N, R] the state its first
        decision was taken with, `dones` [T, N] where an episode ended (the next decision starts cleared). Returns
        the features each decision was taken with, [T, N, R]. Only the GRU cell is sequential; it is the small part."""
        if self.memory is None:
            return encoded

        return _carry_sequence(self.memory, self.recurrent_size, encoded, memory, dones)

    def features(self, obs: torch.Tensor, layout: torch.Tensor, memory: torch.Tensor | None = None,
                 groups=None) -> torch.Tensor:
        """The trunk's output for flat rows, through the GRU when there is one: what every head reads."""
        return self.features_from(self.encode(obs, layout, groups), memory)

    def features_from(self, hidden: torch.Tensor, memory: torch.Tensor | None = None) -> torch.Tensor:
        """features() from the trunk's output on: the GRU, when there is one."""
        if self.memory is not None:
            carried = (memory.reshape(-1, self.recurrent_size) if memory is not None
                       else hidden.new_zeros(hidden.shape[0], self.recurrent_size))
            hidden = self.memory(hidden, carried)
        return hidden

    def action_distribution(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                            goal: torch.Tensor | None = None, groups=None,
                            obs: torch.Tensor | None = None) -> Categorical:
        """The actions of flat rows whose features are `features`, under `goal` where the actor has goals. `obs`
        lets the director's pointer heads score its per-slot actions (DirectorSets)."""
        return Categorical(logits=self.action_logits(features, layout, mask, goal, groups, obs))

    def with_foresight(self, features: torch.Tensor) -> torch.Tensor:
        """The features with the foresight head's predictions fed back (Component P, layer 2), or as they are."""
        if self.foresight_proj is None:
            return features
        return features + self.foresight_proj(self.foresight(features).detach().to(features.dtype))

    def slow_step(self, features: torch.Tensor, slow: torch.Tensor) -> torch.Tensor:
        """One step of the slow loop over flat rows: the slow memory after this decision's features (detached: the
        slow loop learns on top of the fast one without pulling on it)."""
        return self.slow_memory(self.with_foresight(features).detach(), slow)

    def action_logits(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                      goal: torch.Tensor | None = None, groups=None, obs: torch.Tensor | None = None) -> torch.Tensor:
        """action_distribution's masked logits, unnormalised (for sample_logits)."""
        features = self.with_foresight(features)
        if self.goal_embedding is not None and goal is not None:
            features = self.goal_embedding.condition(features, goal.reshape(-1))

        if self.dense_heads is not None:
            dense = self.dense_heads
            own = torch.where(dense.valid[layout.long()], dense(features, layout), MASKED_LOGIT)
            logits = own if own.shape[-1] == mask.shape[-1] else nn.functional.pad(
                own, (0, mask.shape[-1] - own.shape[-1]), value=MASKED_LOGIT)
            return masked_logits(self._with_pointers(logits, features, layout, obs), mask)

        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        logits = features.new_full((features.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](features[rows]).to(logits.dtype)
        return masked_logits(self._with_pointers(logits, features, layout, obs), mask)

    def _with_pointers(self, logits: torch.Tensor, features: torch.Tensor, layout: torch.Tensor,
                       obs: torch.Tensor | None) -> torch.Tensor:
        """The director rows' per-slot actions, scored by their slots' encodings (DirectorSets.pointer_logits)."""
        if self.director_sets is None or obs is None:
            return logits
        rows = layout.reshape(-1) == self.director_index
        if not bool(rows.any()):
            return logits
        logits = logits.clone()
        picked = torch.nonzero(rows).reshape(-1)
        director_obs = obs.reshape(-1, obs.shape[-1])[picked, : self.director_sets.column_mask.shape[0]]
        # The per-slot actions are scored by their slots alone: the layout head's own logits there would give each
        # slot a score for its position, which is what a set must not have.
        for first, scores in self.director_sets.pointer_logits(features[picked], director_obs):
            logits[picked, first : first + scores.shape[1]] = scores.to(logits.dtype)
        return logits

    def goal_distribution(self, features: torch.Tensor, obs: torch.Tensor | None = None,
                          layout: torch.Tensor | None = None) -> Categorical:
        """Which goal to pursue next, from the same features, masked by what the goal block says is there."""
        return Categorical(logits=self.goal_head.logits(features, obs, layout))

    def goal_logits(self, features: torch.Tensor, obs: torch.Tensor | None = None,
                    layout: torch.Tensor | None = None) -> torch.Tensor:
        return self.goal_head.logits(features, obs, layout)

    def goal_ended(self, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """Rows whose goal just ended (GoalBlock::OBS_ENDED): chosen again at once."""
        return self.goal_head.ended(obs, layout)

    def decide_goals(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, held: torch.Tensor,
                     queue: torch.Tensor, clock: torch.Tensor, deterministic: bool) -> dict[str, torch.Tensor]:
        """**The goal decision of one step** (next-run plan, Wave 4), the same in the rollout, its captured graph,
        the teachers and the module's runtime. Flat rows; `held` the goal (pair) held, `queue` [rows, S - 2] the goals
        waiting (-1 none), `clock` the goal clock's choices. In order:

        - an ended secondary is dropped (the sim dropped it too);
        - an ended primary with a queue is replaced by the queue's head, with no choice made (the plan that queued
          it keeps the credit);
        - a choice is made on the clock, where the primary ended with nothing queued, or on the block's event;
        - the director's order, where there is one, is the primary whatever was held or drawn.

        Returns the pair held now (`goal`), the queue, which rows chose (`chosen`), their log probability (0 where
        none), the slots drawn [rows, S] (what the slow update scores) and the observed signals."""
        head = self.goal_head
        rows = features.shape[0]
        signals = head.signals(obs, layout)
        if head.slots <= 1:
            chosen = clock | signals["ended"]
            logits = head.logits(features, obs, layout)
            drawn, _ = sample_logits(logits, deterministic)
            goal = torch.where(chosen, drawn, held.long())
            return {"goal": goal, "queue": queue, "chosen": chosen,
                    "log_prob": torch.where(chosen, log_prob_of(logits, goal), torch.zeros_like(logits[:, 0])),
                    "slots": goal[:, None], **signals}

        primary, secondary = split_goal_pair(held, head.count)
        secondary = torch.where(signals["secondary_ended"], torch.full_like(secondary, -1), secondary)
        promote = signals["ended"] & (queue[:, 0] >= 0)
        primary = torch.where(promote, queue[:, 0], primary)
        shifted = torch.cat([queue[:, 1:], torch.full_like(queue[:, :1], -1)], dim=-1)
        queue = torch.where(promote[:, None], shifted, queue)
        chosen = clock | (signals["ended"] & ~promote) | signals["event"]

        given = signals["from_order"]
        slots, log_prob, _ = head.draw(features, obs, layout, signals["order_goal"], given, deterministic)
        primary = torch.where(chosen, slots[:, 0], primary)
        secondary = torch.where(chosen, slots[:, 1], secondary)
        queue = torch.where(chosen[:, None], slots[:, 2:], queue)
        primary = torch.where(given, signals["order_goal"], primary)
        secondary = torch.where(secondary == primary, torch.full_like(secondary, -1), secondary)
        return {"goal": goal_pair(primary, secondary, head.count), "queue": queue, "chosen": chosen,
                "log_prob": torch.where(chosen, log_prob, torch.zeros_like(log_prob)), "slots": slots, **signals}

    def _forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                 memory: torch.Tensor | None = None,
                 goal: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, tuple[int, ...]]:
        lead = obs.shape[:-1]
        obs, layout, mask = obs.reshape(-1, obs.shape[-1]), layout.reshape(-1), mask.reshape(-1, mask.shape[-1])
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))

        hidden = self.features(obs, layout, memory, groups)
        dist = self.action_distribution(hidden, layout, mask, goal, groups, obs)
        if len(lead) != 1:
            dist = Categorical(logits=dist.logits.reshape(*lead, -1))
        return dist, hidden, lead


class LayoutCritic(nn.Module):
    """V(global state, agent's own observation). Outputs a normalised value when ValueNorm is in use.

    With `recurrent_size` it carries a GRU of its own, exactly as the actor does. It has to: the global state is a
    snapshot -- healths, positions, roles, the clock -- and holds nothing of what has already happened, while a
    recurrent actor's decisions depend on what it remembers. Every part of the return that follows from that memory
    is then invisible to the critic, and lands in the advantage as noise. The two memories are separate (the critic
    sees the whole env, the actor only its own seat) but they are carried and cleared together.
    """

    def __init__(self, state_dim: int, layouts: Sequence[tuple[int, int]], hidden: Sequence[int],
                 goal_count: int = 0, recurrent_size: int = 0, goal_targets: int = 1, director=None,
                 goal_slots: int = 1):
        super().__init__()
        if not hidden:
            raise ValueError("the critic needs at least one hidden layer")
        # The goal the actor is pursuing (mappo.goal_count) is part of what the value depends on: the same situation
        # is worth different things while resting and while fighting. Without it the critic averages over goals and
        # the advantage says nothing about which goal was the right one, which is what lets goals collapse into one.
        self.goal_count = goal_count * max(1, goal_targets)
        self.goal_embedding = (GoalEmbedding(goal_count, goal_targets, hidden[0], goal_slots > 1) if goal_count
                               else None)
        self.obs_dims = [obs for obs, _ in layouts]
        self.state_norm = RunningNorm(state_dim)
        self.state_encoder = _linear(state_dim, hidden[0], math.sqrt(2))
        self.norms = nn.ModuleList(RunningNorm(obs) for obs, _ in layouts)
        self.adapters = nn.ModuleList(_linear(obs, hidden[0], math.sqrt(2)) for obs, _ in layouts)
        self.trunk = _Trunk(hidden)
        self.recurrent_size = recurrent_size
        self.memory = nn.GRUCell(hidden[-1], recurrent_size) if recurrent_size else None
        self.head = _linear(recurrent_size if recurrent_size else hidden[-1], 1, 1.0)
        self.dense_adapters: DenseLayouts | None = None     # as LayoutActor's
        _attach_director(self, director)

    def encode(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
               goal: torch.Tensor | None = None, groups=None) -> torch.Tensor:
        """Everything that depends only on this decision -- the state, the seat's own observation and its goal --
        for flat rows, before the GRU. A replayed sequence encodes every step in one pass and then carries the memory
        through them (carry)."""
        hidden, own = self.encode_goal_free(state, obs, layout, groups)
        return self.encode_goal(hidden, own, goal)

    def encode_goal_free(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
                         groups=None) -> tuple[torch.Tensor, torch.Tensor]:
        """encode's first half, which does not need the goal: (the state's encoding, the seat's own). The larger
        part of the critic's work, so a rollout decision runs it beside the actor choosing the goal."""
        hidden = self.state_encoder(self.state_norm(state))
        if self.dense_adapters is not None:
            own = self.dense_adapters(obs, layout)
        else:
            own = torch.zeros_like(hidden)
            for index, rows in groups if groups is not None else _per_layout(layout, len(self.adapters)):
                own[rows] = self.adapters[index](self.norms[index](obs[rows, : self.obs_dims[index]])).to(
                    own.dtype)
        extra = _director_extra(self.director_sets, self.director_index, obs, layout)
        if extra is not None:
            own = own + extra.to(own.dtype)
        return hidden, own

    def encode_goal(self, hidden: torch.Tensor, own: torch.Tensor, goal: torch.Tensor | None = None) -> torch.Tensor:
        """encode's second half: the goal, then the trunk."""
        if self.goal_embedding is not None and goal is not None:
            own = own + self.goal_embedding(goal.reshape(-1))
        return self.trunk(hidden + own)

    def fold_normalisation(self) -> None:
        """As LayoutActor.fold_normalisation, and the state's normaliser into the state encoder."""
        for norm, adapter in zip(self.norms, self.adapters):
            fold_into(norm, adapter)
        fold_into(self.state_norm, self.state_encoder)

    def densify(self, obs_width: int) -> None:
        """As LayoutActor.densify."""
        if self.dense_adapters is None:
            self.dense_adapters = DenseLayouts(self.adapters, obs_width)
        else:
            self.dense_adapters.refresh(self.adapters)

    def carry(self, encoded: torch.Tensor, memory: torch.Tensor, dones: torch.Tensor) -> torch.Tensor:
        """The critic's GRU over a replayed sequence, as LayoutActor.carry is for the actor's."""
        if self.memory is None:
            return encoded
        return _carry_sequence(self.memory, self.recurrent_size, encoded, memory, dones)

    def values_of(self, features: torch.Tensor) -> torch.Tensor:
        """The value of features already carried through the GRU (a replayed sequence)."""
        return self.head(features).squeeze(-1)

    def initial_memory(self, *lead: int, device=None) -> torch.Tensor:
        """A cleared memory for `lead` rows (what an episode starts with)."""
        return torch.zeros((*lead, self.recurrent_size), dtype=torch.float32, device=device)

    def step(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
             groups=None, memory: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """One decision: (value [...], the memory carried out). Without a GRU the memory out is whatever came in."""
        lead = obs.shape[:-1]
        state, obs, layout = state.reshape(-1, state.shape[-1]), obs.reshape(-1, obs.shape[-1]), layout.reshape(-1)
        return self.step_encoded(self.encode(state, obs, layout, goal, groups), lead, memory)

    def step_encoded(self, encoded: torch.Tensor, lead, memory: torch.Tensor | None = None):
        """step() from the encoding on: the GRU and the value head."""
        if self.memory is None:
            return self.head(encoded).reshape(lead), (memory if memory is not None else encoded.new_zeros((*lead, 0)))

        carried = self.memory(encoded, memory.reshape(-1, self.recurrent_size) if memory is not None
                              else encoded.new_zeros(encoded.shape[0], self.recurrent_size))
        return self.head(carried).reshape(lead), carried.reshape(*lead, self.recurrent_size)

    def forward(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
                groups=None, memory: torch.Tensor | None = None) -> torch.Tensor:
        """state [..., S], obs [..., O], layout [...], goal [...] (with a goal head) -> value [...]. `groups` as for
        LayoutActor.forward; `memory` [..., recurrent_size] the state carried in."""
        return self.step(state, obs, layout, goal, groups, memory)[0]
