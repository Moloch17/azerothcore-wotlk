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
        # Columns kept at mean 0, variance 1 whatever is seen (hide): the camera's image, which the adapters never read
        # and the vision encoder reads raw. Made from the stage, not saved: None until hide() is called.
        self.register_buffer("hidden_columns", None, persistent=False)

    @torch.no_grad()
    def hide(self, columns: Sequence[int]) -> None:
        """Keep `columns` at the identity (mean 0, variance 1) through every update."""
        hidden = torch.zeros(self.mean.shape[0], dtype=torch.bool, device=self.mean.device)
        hidden[[c for c in columns if c < hidden.shape[0]]] = True
        if self.hidden_columns is not None:
            hidden |= self.hidden_columns
        self.hidden_columns = hidden
        self.mean.masked_fill_(hidden, 0.0)
        self.var.masked_fill_(hidden, 1.0)

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
        if self.hidden_columns is not None:
            self.mean.masked_fill_(self.hidden_columns, 0.0)
            self.var.masked_fill_(self.hidden_columns, 1.0)

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
def update_norms(norms: nn.ModuleList, obs: torch.Tensor, layout: torch.Tensor, obs_dims, ranks=None,
                 groups=None) -> None:
    """Fold a rollout's observations into each layout's statistics, each from its own rows and own features. With
    data-parallel `ranks` every layout is visited on every rank, rows or none: each is a collective. `groups` is
    _per_layout(layout) when the caller has it already (the actor's and the critic's norms read the same rows)."""
    if ranks is not None and ranks.active:
        for index, norm in enumerate(norms):
            norm.update(obs[layout == index, : obs_dims[index]], ranks)
        return
    for index, rows in groups if groups is not None else _per_layout(layout, len(norms)):
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
        # Per layout, where its goal block starts in the observation; -1 for a layout without one.
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
        next-run format on (slots > 1) the secondary ending, the event, an order's primary and what was
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
        event (choose again now), an order's primary (from_order, order_goal) and what was achieved this decision
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
        # A row whose layout has no goal block has only none.
        if obs is not None and layout is not None and self.has_space:
            _, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            only_none = torch.zeros_like(logits, dtype=torch.bool)
            only_none[:, -1] = True
            logits = torch.where(has[:, None], logits, logits.masked_fill(~only_none, MASKED_LOGIT))
        return logits

    def draw(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, primary_given: torch.Tensor,
             given: torch.Tensor, deterministic: bool, slots: torch.Tensor | None = None):
        """The slots drawn at a choice, or scored when `slots` [rows, S] is given: (slots [rows, S] with -1 for
        none, log probability, entropy). Where `given` the primary is `primary_given` (an order): it
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
            # A layout with no goal block has no goals to choose: only the first, which means none.
            nothing = torch.zeros_like(allowed)
            allowed = torch.where(has[:, None, None], allowed & present, nothing if self.has_space else allowed)
        allowed = allowed.reshape(features.shape[0], -1).clone()
        # Always something to choose: the first goal (Fight about no one in particular) is never masked out.
        allowed[:, 0] = True
        return masked_logits(joint.reshape(features.shape[0], -1), allowed)


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


def attach_blind_columns(network: nn.Module, columns: dict[int, list[int]], tag: str) -> None:
    """Keep layout adapters blind to some of their observation columns (layout index -> columns): the weights start at
    zero and their gradient is masked, so they stay zero through every update and the rollout copies' folding. For
    columns the policy must not read directly: the camera's. Once per network and tag; a later call only zeroes
    again."""
    for index, cols in columns.items():
        if not cols or index >= len(network.adapters):
            continue
        weight = network.adapters[index].weight
        name = f"{tag}_keep_{index}"
        keep = torch.ones((1, weight.shape[1]), dtype=weight.dtype, device=weight.device)
        keep[0, [c for c in cols if c < weight.shape[1]]] = 0.0
        if hasattr(network, name):
            getattr(network, name).copy_(keep)
        else:
            network.register_buffer(name, keep)
            weight.register_hook(lambda grad, network=network, name=name: grad * getattr(network, name))
        with torch.no_grad():
            weight.mul_(getattr(network, name))


def clear_blind_columns(network: nn.Module) -> None:
    """Zero every seat layout's camera columns (VisionEncoder) again (after a seed or a load brought weights from
    elsewhere)."""
    with torch.no_grad():
        for index in range(len(network.adapters)):
            for tag in ("vision",):
                keep = getattr(network, f"{tag}_keep_{index}", None)
                if keep is not None:
                    network.adapters[index].weight.mul_(keep)


#: The vision block's name in stage.json (BlockId::Vision); its manifest entry carries the camera image's shape.
VISION_BLOCK = "vision"


#: The camera's bytes a pixel (protocol 23, Vision::BYTES_PER_PIXEL): distance, height, normal, the class byte (the
#: semantic class and the objective bit) and the entity slot (perception-goals 1a, vision block revision 5).
IMAGE_BYTES_PER_PIXEL = 5
#: The decoded image channels, as the sim's Vision::DecodePixel lays them out: the class is channel 3 (an index, which
#: the encoder embeds), the objective channel 4. The slot byte is no image channel (perception-goals amendment 10): it
#: only pools the patch features under each listed entity (1c).
IMAGE_CHANNELS = 5
IMAGE_CLASS_CHANNEL = 3
#: The class byte's bits (Camera.h): the class in the low five, the objective in bit 5; byte 4 is the slot.
CLASS_MASK = 0x1F
SLOT_BYTE = 4
#: The classes the wire can carry (5 bits): the size of the class embedding's table, so a class added later needs no
#: new shape.
CLASS_LIMIT = 32
#: The encoder's patch size when the manifest names none (revision 3 and before: 4 x 4 at 64 x 32).
DEFAULT_PATCH = 4
#: The entity list's block in stage.json (BlockId::Entities), which comes with a camera.
ENTITIES_BLOCK = "entities"
#: The sight block in stage.json (BlockId::Sight, dungeon-curriculum I1 and I2): the visible and remembered entities
#: as one list, and the pointer presses on it.
SIGHT_BLOCK = "sight"
#: The mental map's block in stage.json (BlockId::Map, perception-goals REDESIGN §3): its manifest's "map" describes
#: the crop that travels as bytes after the image (protocol 24).
MAP_BLOCK = "map"
#: The crop's byte channels (Vision::CropChannel), and the codes (Vision::MapCode: unknown, floor, wall, door, hazard).
MAP_CHANNELS = 6
MAP_CODES = 5
MAP_CODE, MAP_HEIGHT, MAP_VISITED, MAP_AGE, MAP_CLASS, MAP_FRONTIER = range(MAP_CHANNELS)
MAP_HEIGHT_ZERO = 128


def _map_of(entry: dict, name: str) -> dict | None:
    """A layout's mental map as its map block describes it -- {"first" (its scalar columns), "scalars", "height",
    "width", "cell", "channels", "map_bytes", "codes", "classes"} -- or None without one. Refused: a crop this learner
    does not decode (its channels or codes differ, or its bytes are not height x width x channels)."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == MAP_BLOCK), None)
    if block is None:
        return None
    described = block.get("map")
    if not described:
        raise ValueError(f"{name}: stage.json has a map block without its description")
    out = {key: int(described[key]) for key in ("height", "width", "channels", "map_bytes", "codes", "classes",
                                                 "scalars")}
    out["cell"] = float(described.get("cell", 2.0))
    out["first"] = int(block["obs"][0])
    if (out["channels"], out["codes"]) != (MAP_CHANNELS, MAP_CODES) or not 1 <= out["classes"] <= CLASS_LIMIT \
            or out["map_bytes"] != out["height"] * out["width"] * out["channels"]:
        raise ValueError(f"{name}: a map crop of {out['height']} x {out['width']} x {out['channels']} channels "
                         f"({out['map_bytes']} bytes, {out['codes']} codes, {out['classes']} classes); this learner "
                         f"decodes {MAP_CHANNELS} channels and {MAP_CODES} codes, height x width x channels bytes")
    if int(block["obs"][1]) != out["scalars"]:
        raise ValueError(f"{name}: the map block is {block['obs'][1]} columns, its scalars {out['scalars']}")
    return out


def _entities_of(entry: dict, name: str) -> dict | None:
    """A layout's entity list as its entities block describes it (perception-goals 1b): {"first", "slots", "width",
    "present", "class_column", "type_column", "object_column", "classes", "type_buckets"}, or None without one."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == ENTITIES_BLOCK), None)
    if block is None:
        return None
    described = block.get("entities")
    if not described:
        raise ValueError(f"{name}: stage.json has an entities block without its description")
    features = list(described.get("features", ()))
    out = {key: int(described[key]) for key in ("first", "slots", "width", "present", "class_column", "type_column",
                                                 "classes", "type_buckets")}
    out["object_column"] = features.index("object") if "object" in features else -1
    out["memory_column"] = features.index("memory") if "memory" in features else -1
    first, count = int(block["obs"][0]), int(block["obs"][1])
    if out["first"] != first or out["slots"] * out["width"] != count:
        raise ValueError(f"{name}: the entities block spans {count} columns from {first}, its description "
                         f"{out['slots']} slots of {out['width']} from {out['first']}")
    return out


def _sight_of(entry: dict, name: str, entities: dict | None) -> dict | None:
    """A layout's sight list as its sight block describes it (dungeon-curriculum I1, I2): {"first", "slots",
    "visible_slots", "width", "present", "class_column", "type_column", "object_column", "memory_column",
    "visible_column", "memory_ids", "type_buckets", "presses", "pointer_first"} -- `pointer_first` each press's first
    action in the layout -- or None without one. Refused: a list without the camera's entity list, or whose visible
    half or leading columns are not that list's (its encoder is the list's own)."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == SIGHT_BLOCK), None)
    if block is None:
        return None
    described = block.get("sight")
    if not described:
        raise ValueError(f"{name}: stage.json has a sight block without its description")
    out = {key: int(described[key]) for key in ("first", "slots", "visible_slots", "width", "present", "class_column",
                                                 "type_column", "object_column", "memory_column", "visible_column",
                                                 "memory_ids", "type_buckets")}
    pointers = list(described.get("pointers", ()))
    out["presses"] = tuple(str(p["press"]) for p in pointers)
    out["pointer_first"] = tuple(int(p["first"]) for p in pointers)
    # The named row (revision 2, M3 interact): what the goal names, after the slots -- the entity list's columns,
    # then the task one-hot. Absent (revision 1): none.
    named = described.get("named")
    out["named_offset"] = int(named["offset"]) if named else -1
    out["named_width"] = int(named["width"]) if named else 0
    out["named_entity_width"] = int(named["entity_width"]) if named else 0
    first, count = int(block["obs"][0]), int(block["obs"][1])
    if out["first"] != first or out["slots"] * out["width"] + out["named_width"] != count:
        raise ValueError(f"{name}: the sight block spans {count} columns from {first}, its description "
                         f"{out['slots']} slots of {out['width']} and a named row of {out['named_width']} from "
                         f"{out['first']}")
    if named and (out["named_offset"] != out["slots"] * out["width"]
                  or out["named_entity_width"] >= out["named_width"]):
        raise ValueError(f"{name}: the sight block's named row ({named}) does not follow its slots")
    if any(int(p["count"]) != out["slots"] for p in pointers):
        raise ValueError(f"{name}: a sight press names {[int(p['count']) for p in pointers]} slots, the list has "
                         f"{out['slots']}")
    if entities is None:
        raise ValueError(f"{name}: a sight block without the camera's entity list, whose encoder it shares")
    if (out["visible_slots"] != entities["slots"] or out["width"] <= entities["width"]
            or any(out[key] != entities[key] for key in ("class_column", "type_column", "object_column",
                                                         "memory_column", "type_buckets"))):
        raise ValueError(f"{name}: the sight list ({out}) does not lead with the entity list's slots and columns "
                         f"({entities}): it reads them with that list's encoder")
    return out


def vision_of(stage: dict | None, layout_names: Sequence[str]) -> list[dict | None] | None:
    """Per layout, in `layout_names` order, its camera as the vision block of stage.json describes it -- {"first": the
    block's first observation column (its scalars), "height", "width", "channels", "classes", "class_channel",
    "scalars", "bytes_per_pixel", "patch", "render_sizes", "look", "look_names", "image_bytes", "entities"} -- or None
    for a layout without one; None altogether when no layout has a camera (no encoder, nothing new in the networks).
    The block's first column differs per layout (the core block before it is as wide as the class's spells), so every
    layout keeps its own; so does its entity list ("entities", _entities_of: the entities block after the camera).

    The image travels as bytes beside the observation (protocol 21, Spec.image_bytes): the block's columns are its
    scalars alone. Revision 4 (camera-vision.FREELOOK.md) added the encoder's "patch" (4 when absent), the
    "render_sizes" the sim draws from (scaled up to the canonical height x width it sends) and the free look's heads,
    `"look": {"heads": [7, 5, 5], "names": [...]}` (() when absent: no look head). Revision 5 (perception-goals P2):
    five bytes a pixel, the semantic class in place of the kind and the entity slot. Refused: a vision block without
    its image, or from before revision 5 (its kinds, four bytes a pixel), layouts whose images differ (one encoder
    reads them all), and a block whose width is not its scalars."""
    if stage is None:
        return None
    layouts = stage.get("layouts") or {}
    out: list[dict | None] = []
    shape = None
    for name in layout_names:
        entry = layouts.get(name) or {}
        block = next((b for b in entry.get("blocks", ()) if b.get("name") == VISION_BLOCK), None)
        if block is None:
            out.append(None)
            continue
        image = block.get("image")
        if not image:
            raise ValueError(f"{name}: stage.json has a vision block without its image (the sim's manifest is older "
                             f"than the learner's camera); its columns would reach the adapters raw")
        if image.get("transport") != "bytes":
            raise ValueError(f"{name}: the vision block's image is not sent as bytes (transport "
                             f"{image.get('transport')!r}): a sim from before protocol 21, which this learner does "
                             f"not read")
        if "classes" not in image:
            raise ValueError(f"{name}: the vision block's image has no class table (revision "
                             f"{block.get('revision')}, {image.get('bytes_per_pixel')} bytes a pixel): a sim from "
                             f"before revision 5 (protocol 23), which this learner does not read")
        described = {key: int(image[key]) for key in ("height", "width", "channels", "classes", "class_channel",
                                                      "scalars", "bytes_per_pixel")}
        described["patch"] = int(image.get("patch", DEFAULT_PATCH))
        described["class_limit"] = int(image.get("class_limit", CLASS_LIMIT))
        described["render_sizes"] = tuple(tuple(int(v) for v in size) for size in image.get("render_sizes", ()))
        look = block.get("look") or image.get("look") or {}
        heads = tuple(int(head) for head in look.get("heads", ()))
        look_names = tuple(str(n) for n in look.get("names", ()))
        if any(head < 1 for head in heads) or (look_names and len(look_names) != len(heads)):
            raise ValueError(f"{name}: the vision block's look heads {list(heads)} (names {list(look_names)}): every "
                             f"head needs a choice, and a name if any has one")
        described["look"], described["look_names"] = heads, look_names
        if described["patch"] < 1:
            raise ValueError(f"{name}: the vision block's patch is {described['patch']}")
        for width, height in described["render_sizes"]:
            if not (1 <= width <= described["width"] and 1 <= height <= described["height"]):
                raise ValueError(f"{name}: render size {width}x{height} is not within the canonical "
                                 f"{described['width']}x{described['height']} image")
        if (described["bytes_per_pixel"], described["channels"], described["class_channel"],
                described["class_limit"]) != (IMAGE_BYTES_PER_PIXEL, IMAGE_CHANNELS, IMAGE_CLASS_CHANNEL,
                                              CLASS_LIMIT) or not 1 <= described["classes"] <= CLASS_LIMIT:
            raise ValueError(f"{name}: a vision image of {described['bytes_per_pixel']} bytes a pixel decoded to "
                             f"{described['channels']} channels (class {described['class_channel']}, "
                             f"{described['classes']} of {described['class_limit']} classes); this learner decodes "
                             f"{IMAGE_BYTES_PER_PIXEL} bytes to {IMAGE_CHANNELS} channels, the class channel "
                             f"{IMAGE_CLASS_CHANNEL}, at most {CLASS_LIMIT} classes")
        if shape is None:
            shape = described
        elif described != shape:
            raise ValueError(f"{name}: vision image {described}, another layout's {shape}; one encoder needs one "
                             f"shape")
        first, count = int(block["obs"][0]), int(block["obs"][1])
        if count != described["scalars"]:
            raise ValueError(f"{name}: the vision block is {count} columns, its scalars {described['scalars']} (the "
                             f"image travels as bytes beside the observation)")
        obs_dim = entry.get("obs_dim")
        if obs_dim is not None and first + count > int(obs_dim):
            raise ValueError(f"{name}: the vision block ends at {first + count}, past the layout's {obs_dim} columns")
        image_bytes = described["height"] * described["width"] * described["bytes_per_pixel"]
        crop = _map_of(entry, name)
        listed = _entities_of(entry, name)
        described_layout = {"first": first, **described, "image_bytes": image_bytes, "entities": listed, "map": crop,
                            "camera_bytes": image_bytes + (crop["map_bytes"] if crop else 0)}
        sight = _sight_of(entry, name, listed)
        if sight is not None:
            described_layout["sight"] = sight
        out.append(described_layout)
    maps = [entry["map"] for entry in out if entry is not None]
    if any(crop is None for crop in maps) and any(crop is not None for crop in maps):
        raise ValueError("some layouts with a camera have a mental map and some do not: one encoder reads them all")
    return out if any(entry is not None for entry in out) else None


def vision_image_bytes(vision: list[dict | None] | None) -> int:
    """The bytes of an agent's camera row stage.json's camera makes, 0 without one: its image (height x width x bytes
    a pixel, the sim's Spec.image_bytes) and, with a map block, its map crop after it (Spec.map_bytes) -- the row the
    buffer keeps and the encoder reads (Spec.camera_bytes)."""
    return next((entry.get("camera_bytes", entry["image_bytes"]) for entry in vision or () if entry is not None), 0)


def vision_look_heads(vision: list[dict | None] | None) -> tuple[int, ...]:
    """The free look's heads stage.json's camera has (revision 4: (7, 5, 5)), () without a camera or a look."""
    return next((tuple(entry.get("look", ())) for entry in vision or () if entry is not None), ())


def check_look_heads(vision: list[dict | None] | None, spec_look_heads: int) -> None:
    """Refuse a sim and a stage.json that disagree about the look (protocol 22): SPEC's LookHeads against the number
    of heads in the vision block's "look"."""
    heads = vision_look_heads(vision)
    if len(heads) != int(spec_look_heads):
        raise ValueError(f"the sim's SPEC announces {spec_look_heads} look heads, stage.json's vision block has "
                         f"{len(heads)} ({list(heads)})")


def check_image_bytes(vision: list[dict | None] | None, spec_image_bytes: int, spec_map_bytes: int = 0) -> None:
    """Refuse a sim and a stage.json that disagree about the camera: SPEC's image bytes per agent against stage.json's
    height x width x bytes a pixel, or one side with a camera and the other without; and about the mental map's crop
    (protocol 24): SPEC's map bytes against the map block's."""
    crop = next((entry.get("map") for entry in vision or () if entry is not None), None)
    expected_map = crop["map_bytes"] if crop else 0
    if expected_map != int(spec_map_bytes):
        raise ValueError(f"the sim sends {spec_map_bytes} map bytes an agent, stage.json's map block makes "
                         f"{expected_map}")
    expected = next((entry["image_bytes"] for entry in vision or () if entry is not None), 0)
    if expected == int(spec_image_bytes):
        return
    if not expected:
        raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, but stage.json has no camera (no "
                         f"vision block with an image)")
    if not spec_image_bytes:
        raise ValueError(f"stage.json has a camera ({expected} bytes an agent), but the sim's SPEC sends no image")
    raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, stage.json's camera makes {expected} "
                     f"(height x width x bytes a pixel)")


def decode_image(image: torch.Tensor, height: int, width: int) -> torch.Tensor:
    """The camera's bytes [N, H x W x 5] uint8 as its five float image channels [N, H, W, 5] -- distance, height,
    normal, class, objective -- exactly as Vision::DecodePixel: 255 -> 1.0 (sky) else b / 254; (b - 128) / 125;
    b / 255; b & 31; (b >> 5) & 1. The slot byte is not among them (decode_slots). On the device: the bytes are what
    the rollout keeps."""
    pixels = image.reshape(-1, height, width, IMAGE_BYTES_PER_PIXEL)
    distance, rise, normal, flags = pixels[..., 0], pixels[..., 1], pixels[..., 2], pixels[..., 3]
    as_float = lambda value: value.to(torch.float32)
    return torch.stack([
        torch.where(distance == 255, torch.ones_like(as_float(distance)), as_float(distance) / 254.0),
        (as_float(rise) - 128.0) / 125.0,
        as_float(normal) / 255.0,
        as_float(flags & CLASS_MASK),
        as_float((flags >> 5) & 1),
    ], dim=-1)


def decode_slots(image: torch.Tensor, height: int, width: int) -> torch.Tensor:
    """Each pixel's entity slot, [N, H, W] long: 0 none, else s, the entity list's s-th entry (Vision::DecodePixel's
    sixth value)."""
    return image.reshape(-1, height, width, IMAGE_BYTES_PER_PIXEL)[..., SLOT_BYTE].long()


def decode_map(crops: torch.Tensor, height: int, width: int) -> dict[str, torch.Tensor]:
    """The mental map's crop bytes [N, H x W x 6] uint8 as the encoder reads them (Vision::DecodeCropCell): "code" and
    "class" [N, H, W] long (indexes it embeds), and "values" [N, H, W, 5] float -- the floor's height over the feet
    ((b - 128) / 127, 0 with none), whether a floor is known, visited, the newest look's age (b / 255, 1 never seen)
    and the frontier."""
    cells = crops.reshape(-1, height, width, MAP_CHANNELS)
    as_float = lambda value: value.to(torch.float32)
    rise = cells[..., MAP_HEIGHT]
    known = rise != 0
    return {
        "code": cells[..., MAP_CODE].long().clamp(0, MAP_CODES - 1),
        "class": (cells[..., MAP_CLASS] & CLASS_MASK).long(),
        "values": torch.stack([
            torch.where(known, (as_float(rise) - MAP_HEIGHT_ZERO) / 127.0, torch.zeros_like(as_float(rise))),
            as_float(known),
            as_float(cells[..., MAP_VISITED] != 0),
            as_float(cells[..., MAP_AGE]) / 255.0,
            as_float(cells[..., MAP_FRONTIER] != 0),
        ], dim=-1),
    }


def _spatial_softmax(features: torch.Tensor, grid_x: torch.Tensor, grid_y: torch.Tensor) -> torch.Tensor:
    """Per channel of [N, C, H, W] the expected (x, y) of its softmax over the grid, [N, 2C]."""
    flat = features.flatten(2)
    weights = torch.softmax(flat, dim=-1)
    x = (weights * grid_x.to(weights.dtype)).sum(-1)
    y = (weights * grid_y.to(weights.dtype)).sum(-1)
    return torch.cat([x, y], dim=-1)


class MapEncoder(nn.Module):
    """**The mental map's encoder** (perception-goals REDESIGN §3, the Change): the one heading-up crop (48 x 48 cells
    of 2 yd, protocol 24's bytes after the camera's image), shared by every layout as the camera is. Per cell, the code
    embedded (CODE_EMBED wide), the entity's class through the camera's own class embedding, and five values
    (decode_map): CODE_EMBED + the class embedding + 5 planes (15). **Patches**, as the camera's: PATCH x PATCH cells
    (8 yd square at 2 yd; a 12 x 12 grid at 48 x 48) through Linear -> 64 + SiLU and Linear 64 -> 64 + SiLU, each
    channel's spatial-softmax keypoint over the grid, the channels' mean, the block's scalars (its own columns, read
    raw), and Linear -> EMBED. A convolution stack at the crop's own resolution was measured first: 3.6 times the
    camera's update cost on the host's card, the patches a fraction of it.

    **The join** (`join`, orthogonal): EMBED -> the camera's embedding width, after a SiLU, added to the camera's
    embedding before its own SiLU, so both networks read the map through their own camera join. Seeding from a
    checkpoint without a map zeroes it (bootstrap._seed_map): the seeded policy starts as it was, and the map comes in
    as the join learns."""

    CODE_EMBED = 4
    PATCH = 4
    WIDTHS = (64, 64)
    EMBED = 128

    def __init__(self, crop: dict, class_embedding: nn.Embedding, out_width: int, starts: Sequence[int]):
        super().__init__()
        self.height, self.width = crop["height"], crop["width"]
        if self.height % self.PATCH or self.width % self.PATCH:
            raise ValueError(f"the map's crop is {self.width} x {self.height}; its encoder cuts it into {self.PATCH} x "
                             f"{self.PATCH} patches")
        self.map_bytes, self.scalars = crop["map_bytes"], crop["scalars"]
        self.code_embed = nn.Embedding(MAP_CODES, self.CODE_EMBED)
        # The camera's class embedding, held by reference: one table for the pixels, the list and the map.
        self.__dict__["class_embedding"] = class_embedding
        self.planes_per_cell = self.CODE_EMBED + class_embedding.embedding_dim + 5
        self.grid = (self.height // self.PATCH, self.width // self.PATCH)
        self.patch = nn.Linear(self.PATCH * self.PATCH * self.planes_per_cell, self.WIDTHS[0])
        self.mix = nn.Linear(self.WIDTHS[0], self.WIDTHS[1])
        grid_y, grid_x = torch.meshgrid(torch.linspace(-1.0, 1.0, self.grid[0]),
                                        torch.linspace(-1.0, 1.0, self.grid[1]), indexing="ij")
        self.register_buffer("grid_x", grid_x.reshape(-1), persistent=False)
        self.register_buffer("grid_y", grid_y.reshape(-1), persistent=False)
        self.embed = nn.Linear(3 * self.WIDTHS[1] + self.scalars, self.EMBED)
        self.join = _linear(self.EMBED, out_width, math.sqrt(2))
        # Each layout's first map column (the scalars), -1 for a layout without the map.
        self.register_buffer("start", torch.tensor(list(starts), dtype=torch.long), persistent=False)
        self.register_buffer("offsets", torch.arange(self.scalars, dtype=torch.long), persistent=False)

    def planes(self, crops: torch.Tensor) -> torch.Tensor:
        """[N, M] bytes -> [N, H, W, P] planes: the code's embedding, the class's, the five values."""
        decoded = decode_map(crops, self.height, self.width)
        dtype = self.embed.weight.dtype
        # The embeddings' backward (a scatter of 2,304 cells a row onto a few rows) is about two thirds of the
        # encoder's update on the host's card; one-hot products instead measured no faster (the code's) or slower
        # (the class's: a 32-wide plane a cell), so they stay lookups.
        return torch.cat([self.code_embed(decoded["code"]).to(dtype), self.class_embedding(decoded["class"]).to(dtype),
                          decoded["values"].to(dtype)], dim=-1)

    def patches(self, planes: torch.Tensor) -> torch.Tensor:
        """[N, H, W, P] -> [N, patches, PATCH x PATCH x P], row by row as the camera's."""
        rows, cols = self.grid
        cut = planes.reshape(planes.shape[0], rows, self.PATCH, cols, self.PATCH, self.planes_per_cell)
        return cut.permute(0, 1, 3, 2, 4, 5).reshape(planes.shape[0], rows * cols, -1)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, crops: torch.Tensor) -> torch.Tensor:
        """What the map adds to the camera's embedding (before its SiLU), [N, out_width]."""
        silu = nn.functional.silu
        layout = layout.reshape(-1).long()
        columns = (self.start[layout].clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        scalars = obs.gather(1, columns).to(self.embed.weight.dtype)
        features = silu(self.mix(silu(self.patch(self.patches(self.planes(crops))))))     # [N, patches, C]
        grid = features.transpose(1, 2).reshape(features.shape[0], -1, *self.grid)
        pooled = torch.cat([_spatial_softmax(grid, self.grid_x, self.grid_y), features.mean(dim=1), scalars], dim=-1)
        return self.join(silu(self.embed(pooled)))


class VisibleEntities(nn.Module):
    """**The entity list** (perception-goals 1b and 1c): the entities block's slots read as a set -- one encoder shared
    by every slot and layout, the present slots pooled (mean and max) -- with two changes to how a slot is encoded:
    - the class and the type are embedded, never one-hot (amendment 10): the class through the camera's own class
      embedding (one table for the pixels and the list), the type -- the creature or game object entry, raw in its
      column -- hashed into TYPE_EMBED-wide buckets, (entry x 2 + is object) mod the manifest's type_buckets; the
      slot's other columns go in as they are;
    - **the link** (1c): each slot's token adds a projection of the mean patch feature under its own pixels (the
      slots' masks pooled at patch resolution: `linked`), so a token knows what its shape looks like.
    The pool (Linear onto the camera's embedding width) is added to the camera's embedding; a layout without a list
    adds zero. The sight list (SightEntities) shares the encoder and points at the slots."""

    TYPE_EMBED = 8

    def __init__(self, descriptors: Sequence[dict | None], class_embedding: nn.Embedding, feature_width: int,
                 out_width: int, embed: int = 64):
        super().__init__()
        entries = [entry for entry in descriptors if entry is not None]
        spec = entries[0]
        self.embed = embed
        self.slots, self.width_ = spec["slots"], spec["width"]
        self.class_column, self.type_column = spec["class_column"], spec["type_column"]
        self.object_column, self.buckets = spec["object_column"], spec["type_buckets"]
        for entry in entries:
            if any(entry[key] != spec[key] for key in ("slots", "width", "class_column", "type_column",
                                                       "object_column", "type_buckets")):
                raise ValueError(f"the entity lists of the layouts differ ({entry} against {spec}): one encoder "
                                 f"reads them all")
        # The camera's class embedding, held by reference (it is the camera's module, not a second copy).
        self.__dict__["class_embedding"] = class_embedding
        kept = [c for c in range(self.width_) if c not in (self.class_column, self.type_column)]
        inputs = len(kept) + class_embedding.embedding_dim + self.TYPE_EMBED
        self.encoders = nn.ModuleDict({"visible": nn.Sequential(_linear(inputs, embed, math.sqrt(2)), nn.Tanh(),
                                                                _linear(embed, embed, math.sqrt(2)), nn.Tanh())})
        self.pool = _linear(2 * embed, out_width, 1.0)
        self.type_embed = nn.Embedding(self.buckets, self.TYPE_EMBED)
        self.link = _linear(feature_width, embed, 1.0)
        # Per layout: [layouts, slots * width] observation columns and [layouts, slots] present columns (-1: the layout
        # has no list). Derived from stage.json, not learned: kept out of the state dict.
        layouts = len(descriptors)
        columns = torch.zeros(layouts, self.slots * self.width_, dtype=torch.long)
        present = torch.full((layouts, self.slots), -1, dtype=torch.long)
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            start = int(entry["first"])
            columns[index] = torch.arange(start, start + self.slots * self.width_)
            present[index] = start + torch.arange(self.slots) * self.width_ + int(entry["present"])
        self.register_buffer("columns_visible", columns, persistent=False)
        self.register_buffer("present_visible", present, persistent=False)
        self.register_buffer("has_sets", torch.tensor([entry is not None for entry in descriptors]), persistent=False)
        self.register_buffer("kept_columns", torch.tensor(kept, dtype=torch.long), persistent=False)
        # The memory id (entity memory, dungeon-curriculum I2) is a label, not a quantity: it reads 0 here, as it did
        # before memory existed, and the sight list embeds it instead (SightEntities).
        self.memory_column = int(spec.get("memory_column", -1))
        keep = torch.ones(self.width_)
        if self.memory_column >= 0:
            keep[self.memory_column] = 0.0
        self.register_buffer("column_keep", keep, persistent=False)

    def token(self, raw: torch.Tensor) -> torch.Tensor:
        """Slots' tokens [..., embed] from their entity-list columns [..., width] alone (no pixels): the class and the
        type embedded, the memory id read as 0, the rest as they are. The sight list reads its slots so too."""
        dtype = self.pool.weight.dtype
        raw = raw * self.column_keep.to(raw.dtype)
        classes = raw[..., self.class_column].round().clamp(0, self.class_embedding.num_embeddings - 1).long()
        entry = raw[..., self.type_column].round().clamp(min=0).long()
        is_object = raw[..., self.object_column].round().long() if self.object_column >= 0 else torch.zeros_like(entry)
        bucket = (entry * 2 + is_object) % self.buckets
        x = torch.cat([raw.index_select(-1, self.kept_columns).to(dtype), self.class_embedding(classes).to(dtype),
                       self.type_embed(bucket).to(dtype)], dim=-1)
        return self.encoders["visible"](x)

    def encode_linked(self, obs: torch.Tensor, layout: torch.Tensor,
                      linked: torch.Tensor) -> dict[str, tuple[torch.Tensor, torch.Tensor]]:
        """The slots' tokens [N, slots, embed] (with the patch features under each, `linked` [N, slots, F]) and which
        are present."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns_visible[layout]).reshape(rows, self.slots, self.width_)
        dtype = self.pool.weight.dtype
        codes = self.token(raw) + self.link(linked.to(dtype))
        present_at = self.present_visible[layout]
        present = (present_at >= 0) & (obs.gather(1, present_at.clamp(min=0)) > 0.5)
        return {"visible": (codes, present)}

    def pooled(self, obs: torch.Tensor, layout: torch.Tensor,
               encoded: dict[str, tuple[torch.Tensor, torch.Tensor]]) -> torch.Tensor:
        """What the list adds to the camera's embedding: [rows, out], zero for a layout without it."""
        codes, present = encoded["visible"]
        weight = present.to(codes.dtype)[..., None]
        mean = (codes * weight).sum(dim=1) / weight.sum(dim=1).clamp(min=1.0)
        peak = torch.where(present[..., None], codes, torch.full_like(codes, -1.0)).amax(dim=1)
        peak = torch.where(present.any(dim=1, keepdim=True), peak, torch.zeros_like(peak))
        pooled = self.pool(torch.cat([mean, peak], dim=-1))
        return pooled * self.has_sets[layout.reshape(-1).long()][:, None].to(pooled.dtype)


class SightEntities(nn.Module):
    """**What the seat sees and remembers, as one list** (dungeon-curriculum I1 and I2; the sight block): its slots --
    the camera's visible entities first, in the entity list's order, then the most relevant remembered ones -- read by
    the entity list's own encoder (VisibleEntities.token: the same weights, shared by reference, not a copy), each
    token plus:
    - the patch features under its pixels (the list's link), for the visible half, when the image is there;
    - a projection of the memory's columns (visible now, age, dead, open, used, heading, course, selected, focused);
    - an embedding of its memory id (a label stable while the entity is remembered, ids past `memory_ids` folded onto
      the table; 0 none).
    The present slots are pooled (mean and max) onto the camera's embedding width, as the list's are: the pool is what
    seeding zeroes when the checkpoint has no sight list, so a seeded policy starts as it was.

    **The named row** (sight block revision 2, M3 interact), where the layouts have one: what the goal names -- a kind,
    never a place -- read by the same encoder (its class, type and object columns, the rest 0) plus a projection of the
    task, as one token. It looks over the slots (a match score of each slot's token against a query of the named
    token, softmaxed over the present slots), and the named token and the slots it matched are pooled onto the
    embedding (named_pool: zeroed by seeding as the pool is), so the trunk knows what it is after and where what it
    sees of it is; and each press's pointer scores gain the match times a learned weight (named_gain, from 0), so "the
    one named" is a slot the presses can point at from their first update.

    **The pointer heads** read the same tokens without the pixels (an action's logits are taken from the observation
    alone): one query per press, from the actor's features (SightPointers), scored against every slot, so a press
    follows the entity, not the slot -- one head over the combined list of visible and remembered."""

    def __init__(self, descriptors: Sequence[dict | None], visible: "VisibleEntities", out_width: int,
                 embed: int = 64):
        super().__init__()
        spec = next(entry for entry in descriptors if entry is not None)
        for entry in descriptors:
            if entry is not None and any(entry[key] != spec[key] for key in (
                    "slots", "visible_slots", "width", "presses", "memory_ids", "visible_column")):
                raise ValueError(f"the sight lists of the layouts differ ({entry} against {spec}): one encoder reads "
                                 f"them all")
        if spec["visible_slots"] != visible.slots or spec["width"] <= visible.width_:
            raise ValueError(f"the sight list's visible half ({spec['visible_slots']} slots) is not the entity list's "
                             f"({visible.slots})")
        self.slots, self.width_, self.visible_slots = spec["slots"], spec["width"], spec["visible_slots"]
        self.base = visible.width_
        self.memory_column, self.memory_ids = spec["memory_column"], spec["memory_ids"]
        self.presses = tuple(spec["presses"])
        self.named_width = int(spec.get("named_width", 0))
        for entry in descriptors:
            if entry is not None and int(entry.get("named_width", 0)) != self.named_width:
                raise ValueError("the sight lists' named rows differ: one encoder reads them all")
        if self.named_width:
            if int(spec["named_entity_width"]) != self.base:
                raise ValueError(f"the sight list's named row leads with {spec['named_entity_width']} columns, the "
                                 f"entity list's tokens read {self.base}")
            self.named_task = _linear(self.named_width - self.base, embed, 1.0)
            self.named_query = _linear(embed, embed, 1.0)
            self.named_pool = _linear(2 * embed, out_width, 1.0)
            self.named_gain = nn.Parameter(torch.zeros(max(1, len(self.presses))))
        # The entity list's encoder, held by reference (it is the list's module).
        self.__dict__["shared"] = visible
        self.extra = _linear(self.width_ - self.base, embed, 1.0)
        self.memory_embed = nn.Embedding(self.memory_ids + 1, embed, padding_idx=0)
        with torch.no_grad():
            self.memory_embed.weight.normal_(0.0, 0.1)
            self.memory_embed.weight[0].zero_()
        self.pool = _linear(2 * embed, out_width, 1.0)
        layouts = len(descriptors)
        columns = torch.zeros(layouts, self.slots * self.width_, dtype=torch.long)
        present = torch.full((layouts, self.slots), -1, dtype=torch.long)
        first = torch.full((layouts, max(1, len(self.presses))), -1, dtype=torch.long)
        self.blind = {}
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            start = entry["first"]
            columns[index] = torch.arange(start, start + self.slots * self.width_)
            present[index] = start + torch.arange(self.slots) * self.width_ + entry["present"]
            for at, action in enumerate(entry["pointer_first"]):
                first[index, at] = action
            self.blind[index] = list(range(start, start + self.slots * self.width_))
        # Derived from stage.json, not learned: kept out of the state dict.
        self.register_buffer("columns", columns, persistent=False)
        self.register_buffer("present_at", present, persistent=False)
        self.register_buffer("pointer_first", first, persistent=False)
        self.register_buffer("has_sight", present[:, 0] >= 0, persistent=False)
        named = torch.zeros(layouts, max(1, self.named_width), dtype=torch.long)
        has_named = torch.zeros(layouts, dtype=torch.bool)
        for index, entry in enumerate(descriptors):
            if entry is None or not self.named_width:
                continue
            start = entry["first"] + entry["named_offset"]
            named[index] = torch.arange(start, start + self.named_width)
            has_named[index] = True
            self.blind[index] += list(range(start, start + self.named_width))
        self.register_buffer("named_columns", named, persistent=False)
        self.register_buffer("has_named", has_named, persistent=False)

    def tokens(self, obs: torch.Tensor, layout: torch.Tensor,
               linked: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """The slots' tokens [N, slots, embed] and which are present [N, slots]; `linked` [N, visible_slots, F] the
        patch features under the visible half's pixels (None: without them, as the pointers read the list)."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns[layout]).reshape(rows, self.slots, self.width_)
        dtype = self.pool.weight.dtype
        codes = self.shared.token(raw[..., : self.base])
        if linked is not None:
            link = self.shared.link(linked.to(dtype))
            codes = codes + nn.functional.pad(link, (0, 0, 0, self.slots - self.visible_slots))
        codes = codes + self.extra(raw[..., self.base:].to(dtype))
        ids = raw[..., self.memory_column].round().clamp(min=0).long()
        ids = torch.where(ids > 0, (ids - 1) % self.memory_ids + 1, ids)
        codes = codes + self.memory_embed(ids).to(dtype)
        present_at = self.present_at[layout]
        present = (present_at >= 0) & (obs.gather(1, present_at.clamp(min=0)) > 0.5)
        return codes, present

    def named(self, obs: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The named row's token [N, embed] and whether a row names anything [N] (its present column; a layout
        without the row names nothing)."""
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.named_columns[layout])
        dtype = self.pool.weight.dtype
        token = self.shared.token(raw[:, : self.base]) + self.named_task(raw[:, self.base:].to(dtype))
        present = self.has_named[layout] & (raw[:, 0] > 0.5)
        return token, present

    def match(self, codes: torch.Tensor, token: torch.Tensor) -> torch.Tensor:
        """Each slot's match with the named token [N, slots]: its token against the named token's query, scaled."""
        query = self.named_query(token).to(codes.dtype)
        return torch.einsum("rse,re->rs", codes, query) / math.sqrt(codes.shape[-1])

    def pooled(self, codes: torch.Tensor, present: torch.Tensor, layout: torch.Tensor,
               obs: torch.Tensor | None = None) -> torch.Tensor:
        """What the list adds to the camera's embedding: [N, out], zero for a layout without it; with the named row
        (and `obs`), the named token and the slots that match it too."""
        weight = present.to(codes.dtype)[..., None]
        mean = (codes * weight).sum(dim=1) / weight.sum(dim=1).clamp(min=1.0)
        peak = torch.where(present[..., None], codes, torch.full_like(codes, -1.0)).amax(dim=1)
        peak = torch.where(present.any(dim=1, keepdim=True), peak, torch.zeros_like(peak))
        out = self.pool(torch.cat([mean, peak], dim=-1))
        if self.named_width and obs is not None:
            token, named = self.named(obs, layout)
            scores = self.match(codes, token.to(codes.dtype)).masked_fill(~present, -1e4)
            weights = torch.softmax(scores.float(), dim=-1).to(codes.dtype) * present.to(codes.dtype)
            attended = (weights[..., None] * codes).sum(dim=1)
            extra = self.named_pool(torch.cat([token.to(codes.dtype), attended], dim=-1))
            out = out + extra * named[:, None].to(extra.dtype)
        return out * self.has_sight[layout.reshape(-1).long()][:, None].to(out.dtype)

    def with_pointers(self, logits: torch.Tensor, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
                      queries: "SightPointers") -> torch.Tensor:
        """`logits` with every press's slots scored by their tokens against the press's query, in the rows whose
        layout has the list; the other rows' as they were (nothing is read back)."""
        layout = layout.reshape(-1).long()
        codes, _ = self.tokens(obs, layout)
        steps = torch.arange(self.slots, device=logits.device)[None, :]
        matched = None
        if self.named_width:
            token, named = self.named(obs, layout)
            matched = self.match(codes, token.to(codes.dtype)) * named[:, None].to(codes.dtype)
        for at, press in enumerate(self.presses):
            first = self.pointer_first[layout, at]
            scores = torch.einsum("rse,re->rs", codes, queries.queries[press](features).to(codes.dtype))
            if matched is not None:
                scores = scores + self.named_gain[at].to(codes.dtype) * matched
            valid = first >= 0
            columns = (first.clamp(min=0)[:, None] + steps).clamp(max=logits.shape[-1] - 1)
            kept = logits.gather(1, columns)
            # Out of place: the update differentiates through `logits`, which the gather above has just read.
            logits = logits.scatter(1, columns, torch.where(valid[:, None], scores.to(logits.dtype), kept))
        return logits


class SightPointers(nn.Module):
    """The sight list's pointer queries (SightEntities.with_pointers): one per press, a Linear from the actor's
    features to the token width, initialised small as the other pointer queries are. The actor's own (the critic
    acts on nothing)."""

    def __init__(self, presses: Sequence[str], head_width: int, embed: int = 64):
        super().__init__()
        self.queries = nn.ModuleDict({press: _linear(head_width, embed, 0.01) for press in presses})


class VisionEncoder(nn.Module):
    """**The camera** (camera-vision): a seat's H x W image of what its camera's rays hit, encoded by one small
    network shared by every layout, as VisibleEntities shares its encoder.

    The image arrives as bytes beside the observation (protocol 23: [N, H x W x 5] uint8, [row][col][byte]) and is
    decoded on the device to its five channels, channels last (decode_image); the block's scalars are gathered from
    each row's own layout's columns (a table by layout id: the vision block starts at a different column in every
    class). **The class is embedded** (perception-goals amendment 10): a learned CLASS_EMBED-wide vector a class, from
    a table of CLASS_LIMIT (the wire's 5 bits), so 4 + CLASS_EMBED planes a pixel (10). **Patches**, not
    convolutions: the image is cut into PATCH x PATCH squares (128 x 64 with patch 8 -> a 16 x 8 grid), each square's
    pixels and planes (8 x 8 x 10 = 640 features, ordered row in the patch, column in the patch, plane) go through
    one shared Linear -> 64 + SiLU and Linear 64 -> 64 + SiLU, a spatial softmax per channel over the patch grid gives
    its expected (x, y) in [-1, 1] (128 keypoints), the block's scalars join them, and Linear -> 256: the camera's
    embedding, after SiLU. PATCH is the manifest's "patch" (4 when it names none). Each network reads it through a
    `vision_join` of its own, a Linear onto its adapters' output width (VisionJoin).

    **The entity list** (perception-goals 1b, 1c; VisibleEntities): where the layouts have one, its slots are read as
    a set, each slot's token joined with the mean patch feature under its own pixels -- the slot byte, pooled at patch
    resolution (link_features) -- and the set's pool is added to the embedding before the SiLU. The slot byte is
    used for nothing else: it is not an image channel.

    **One encoder for the actor and the critic** (MappoTrainer): the actor owns it -- it is in the actor's state dict,
    so a policy alone (export, distillation, the league) carries its camera -- and the critic holds a reference that
    is not one of its modules, so neither its parameters, its optimiser nor its state dict has a second copy. Both
    losses' gradients reach it, summed, and its own optimiser steps it once per minibatch (MappoTrainer.vision_opt).

    No normalisation: the channels are 0-1 or -1-1 already, and the list's columns are scaled by the sim. Every
    shape is fixed and nothing is read back, so a rollout graph captures it."""

    PATCH = DEFAULT_PATCH       # the class's default; an encoder's own is its manifest's "patch" (revision 4: 8)
    WIDTHS = (64, 64)
    EMBED = 256
    CLASS_EMBED = 6

    @classmethod
    def update_bytes_per_row(cls, image: dict) -> int:
        """Roughly the device memory one row's camera (and map) keeps for the update's backward pass, in bytes, from the
        stage's vision descriptor: float32 tensors the encoder saves per pixel (the decoded channels, the planes
        without the class, the class embedding, the concatenated planes and the patch copy of them) and per map
        cell (the code and class embeddings, the values, the planes and their patch copy, and the int64 decode).
        For the 128 x 64 camera that is 1.15 MB a row against the 0.98 MB measured (5.6 GiB over 6,144 rows,
        move1_controls.yaml); the map adds 0.45 MB."""
        channels = int(image["channels"])
        planes = channels - 1 + cls.CLASS_EMBED
        pixels = int(image["height"]) * int(image["width"])
        total = 4 * pixels * (channels + (channels - 1) + cls.CLASS_EMBED + 2 * planes)
        crop = image.get("map")
        if crop is not None:
            cells = int(crop["height"]) * int(crop["width"])
            per_cell = MapEncoder.CODE_EMBED + cls.CLASS_EMBED + 5
            total += cells * (4 * 3 * per_cell + 16)
        return total

    def __init__(self, descriptors: Sequence[dict | None]):
        super().__init__()
        image = next(entry for entry in descriptors if entry is not None)
        self.height, self.width = image["height"], image["width"]
        self.channels, self.classes = image["channels"], image["classes"]
        self.class_channel, self.scalars = image["class_channel"], image["scalars"]
        self.class_limit = int(image.get("class_limit", CLASS_LIMIT))
        self.PATCH = int(image.get("patch", DEFAULT_PATCH))
        if self.height % self.PATCH or self.width % self.PATCH:
            raise ValueError(f"the camera's image is {self.width} x {self.height}; the vision encoder cuts it into "
                             f"{self.PATCH} x {self.PATCH} patches, so both must be multiples of {self.PATCH} "
                             f"(AnimusForge.Vision.Width, Height)")
        self.image_bytes = image["image_bytes"]
        self.span = self.scalars
        self.class_embed = nn.Embedding(self.class_limit, self.CLASS_EMBED)
        self.planes_per_pixel = self.channels - 1 + self.CLASS_EMBED
        self.grid = (self.height // self.PATCH, self.width // self.PATCH)
        features = self.PATCH * self.PATCH * self.planes_per_pixel
        self.patch = nn.Linear(features, self.WIDTHS[0])
        self.mix = nn.Linear(self.WIDTHS[0], self.WIDTHS[1])
        self.embed = nn.Linear(2 * self.WIDTHS[1] + self.scalars, self.EMBED)
        self.feature_shape = (self.WIDTHS[1], *self.grid)
        # Derived from stage.json, not learned: kept out of the state dict.
        self.register_buffer("start", torch.tensor([entry["first"] if entry is not None else -1
                                                    for entry in descriptors], dtype=torch.long), persistent=False)
        self.register_buffer("has_vision", self.start >= 0, persistent=False)
        self.register_buffer("offsets", torch.arange(self.span, dtype=torch.long), persistent=False)
        grid_y, grid_x = torch.meshgrid(torch.linspace(-1.0, 1.0, self.grid[0]),
                                        torch.linspace(-1.0, 1.0, self.grid[1]), indexing="ij")
        self.register_buffer("grid_x", grid_x.reshape(-1), persistent=False)
        self.register_buffer("grid_y", grid_y.reshape(-1), persistent=False)
        # Each pixel's patch (row-major over the grid, as `patches` orders them), for the link's pooling.
        rows = torch.arange(self.height)[:, None] // self.PATCH
        cols = torch.arange(self.width)[None, :] // self.PATCH
        self.register_buffer("patch_of", (rows * self.grid[1] + cols).reshape(-1), persistent=False)
        # The entity list, where the layouts have one (every revision 5 stage does).
        lists = [entry.get("entities") if entry is not None else None for entry in descriptors]
        self.entities = None
        self.slots = 0
        if any(entry is not None for entry in lists):
            self.entities = VisibleEntities(lists, self.class_embed, self.WIDTHS[1], self.EMBED)
            self.slots = self.entities.slots
        # The mental map, where the layouts have one (perception-goals REDESIGN §3): its crop follows the image in
        # each row's bytes, and its encoder adds to the embedding.
        crops = [entry.get("map") if entry is not None else None for entry in descriptors]
        self.map = None
        self.map_bytes = 0
        if any(crop is not None for crop in crops):
            crop = next(crop for crop in crops if crop is not None)
            self.map = MapEncoder(crop, self.class_embed, self.EMBED,
                                  [c["first"] if c is not None else -1 for c in crops])
            self.map_bytes = crop["map_bytes"]
        # The seen and remembered list (dungeon-curriculum I1, I2), where the layouts have a sight block: read with
        # the entity list's encoder, pooled onto the embedding, and pointed at by the actor's presses.
        sights = [entry.get("sight") if entry is not None else None for entry in descriptors]
        self.sight = None
        if any(sight is not None for sight in sights):
            if self.entities is None:
                raise ValueError("a sight block needs the camera's entity list, whose encoder it shares")
            self.sight = SightEntities(sights, self.entities, self.EMBED)
        #: Per layout with the camera, the columns its adapter and normaliser do not read: the block's scalars, which
        #: the encoder reads raw, its entity list's, which the list's encoder reads, its map's scalars, and its sight
        #: list's.
        self.blind = {}
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            columns = list(range(entry["first"], entry["first"] + self.span))
            if lists[index] is not None:
                columns += list(range(lists[index]["first"],
                                      lists[index]["first"] + lists[index]["slots"] * lists[index]["width"]))
            if crops[index] is not None:
                columns += list(range(crops[index]["first"], crops[index]["first"] + crops[index]["scalars"]))
            if self.sight is not None:
                columns += self.sight.blind.get(index, [])
            self.blind[index] = columns

    def gather(self, obs: torch.Tensor, layout: torch.Tensor,
               image: torch.Tensor | None) -> tuple[torch.Tensor, torch.Tensor]:
        """Each row's camera: (its image decoded, [N, H, W, C], and its own layout's scalar columns, [N, S], raw).
        Rows of a layout without the camera read whatever is at column 0 on (their output is zeroed by the join)."""
        if image is None:
            raise ValueError("this network has a camera: its image bytes have to come with the observation")
        layout = layout.reshape(-1).long()
        columns = (self.start[layout].clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        scalars = obs.gather(1, columns).to(self.embed.weight.dtype)
        decoded = decode_image(self.pixels(image), self.height, self.width)
        return decoded.to(self.embed.weight.dtype), scalars

    def pixels(self, image: torch.Tensor) -> torch.Tensor:
        """The image's bytes of each row [N, I]: the row's first image_bytes (its map crop follows, protocol 24)."""
        rows = image.reshape(-1, self.image_bytes + self.map_bytes)
        return rows[:, :self.image_bytes] if self.map_bytes else rows

    def planes(self, image: torch.Tensor) -> torch.Tensor:
        """[N, H, W, C] -> [N, H, W, C - 1 + CLASS_EMBED]: every channel but the class as it is, then the class's
        embedding (the class rounded and clamped to the table)."""
        classes = image[..., self.class_channel].round().clamp(0, self.class_limit - 1).long()
        rest = torch.cat([image[..., : self.class_channel], image[..., self.class_channel + 1:]], dim=-1)
        return torch.cat([rest, self.class_embed(classes).to(rest.dtype)], dim=-1)

    def patches(self, planes: torch.Tensor) -> torch.Tensor:
        """[N, H, W, P] -> [N, patches, PATCH x PATCH x P]: the grid's patches row by row, each its pixels row by
        row and every pixel's planes."""
        rows, cols = self.grid
        n = planes.shape[0]
        cut = planes.reshape(n, rows, self.PATCH, cols, self.PATCH, self.planes_per_pixel).permute(0, 1, 3, 2, 4, 5)
        return cut.reshape(n, rows * cols, -1)

    def features(self, patches: torch.Tensor) -> torch.Tensor:
        """Each patch's features, [N, patches, 64]."""
        silu = nn.functional.silu
        return silu(self.mix(silu(self.patch(patches))))

    def keypoints(self, features: torch.Tensor) -> torch.Tensor:
        """Spatial softmax: per channel the expected (x, y) of its softmax over the patch grid, [N, 2 x channels]."""
        weights = torch.softmax(features.transpose(1, 2), dim=-1)
        x = (weights * self.grid_x.to(weights.dtype)).sum(-1)
        y = (weights * self.grid_y.to(weights.dtype)).sum(-1)
        return torch.cat([x, y], dim=-1)

    def link_features(self, features: torch.Tensor, slots: torch.Tensor) -> torch.Tensor:
        """**The link** (perception-goals 1c): per listed slot, the mean of the patch features under its pixels, each
        patch weighted by how many of its pixels are the slot's -- [N, slots, F] from the features [N, patches, F]
        and the pixels' slots [N, H, W] (0 none). A slot with no pixel reads zeros. A histogram at patch resolution
        (scatter_add), never a one-hot of the image."""
        n, patches = features.shape[0], features.shape[1]
        bins = self.slots + 1
        index = self.patch_of[None, :] * bins + slots.reshape(n, -1).clamp(0, self.slots)
        counts = torch.zeros(n, patches * bins, dtype=torch.float32, device=features.device)
        counts.scatter_add_(1, index, torch.ones(1, 1, dtype=torch.float32, device=features.device).expand_as(index))
        weights = counts.view(n, patches, bins)[:, :, 1:].transpose(1, 2)
        pooled = torch.bmm(weights.to(features.dtype), features)
        return pooled / weights.sum(-1, keepdim=True).clamp(min=1.0).to(features.dtype)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, image: torch.Tensor | None) -> torch.Tensor:
        """The camera's embedding of rows `obs` [N, O] with their images `image` [N, I] uint8, [N, 256] (a layout
        without the camera's rows are zeroed by the join)."""
        decoded, scalars = self.gather(obs, layout, image)
        features = self.features(self.patches(self.planes(decoded)))
        out = self.embed(torch.cat([self.keypoints(features), scalars], dim=-1))
        if self.map is not None:
            crops = image.reshape(-1, self.image_bytes + self.map_bytes)[:, self.image_bytes:]
            out = out + self.map(obs, layout, crops).to(out.dtype)
        if self.entities is not None:
            slots = decode_slots(self.pixels(image), self.height, self.width)
            linked = self.link_features(features, slots)
            encoded = self.entities.encode_linked(obs, layout, linked)
            out = out + self.entities.pooled(obs, layout, encoded).to(out.dtype)
            if self.sight is not None:
                codes, present = self.sight.tokens(obs, layout, linked)
                out = out + self.sight.pooled(codes, present, layout, obs).to(out.dtype)
        return nn.functional.silu(out)


class VisionJoin(nn.Module):
    """A network's own reading of the shared camera embedding (VisionEncoder): a Linear onto its adapters' output
    width, initialised as the adapters are, so a network trained from scratch sees with the camera from its first
    update (M1, the user, 2026-10-06). Seeding from a checkpoint without the camera zeroes it instead
    (bootstrap._seed_vision), so the seeded policy starts as it was. A layout without the camera adds zero."""

    def __init__(self, layouts_with: torch.Tensor, width: int):
        super().__init__()
        self.linear = _linear(VisionEncoder.EMBED, width, math.sqrt(2))
        self.register_buffer("has_vision", layouts_with.clone(), persistent=False)

    def forward(self, embedding: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """What the camera adds to the adapter's output: [N, width], zero for a layout without it."""
        joined = self.linear(embedding)
        return joined * self.has_vision[layout.reshape(-1).long()][:, None].to(joined.dtype)


#: The look head's initial lean toward the choice that changes nothing (camera-vision.FREELOOK.md, amendments): that
#: choice's logit starts this much above the others', so early turning of the camera is rarer -- at 2.0 a fresh head
#: holds the yaw rate at 0 about 55% of the time, against 14% for an even 7-way draw. Every choice stays possible.
LOOK_HOLD_BIAS = 2.0


def look_hold_indices(heads: Sequence[int]) -> tuple[int, ...]:
    """Per look head, its choice that changes nothing, by position: every head but the last is a rate, whose middle
    choice is rate 0; the last is the camera's command head (zoom, recentre, face), whose choice 0 is "hold". Not by
    size: the command head's 5 choices (with "face") are odd too. Revision 4's (7, 5, 5) gives (3, 2, 0),
    protocol.LOOK_HOLD."""
    return tuple(head // 2 for head in heads[:-1]) + ((0,) if heads else ())


class LookHead(nn.Module):
    """**The free look** (camera-vision.FREELOOK.md D): a second policy head over the features the movement head reads,
    Linear(features -> sum(heads)) split into one categorical per head -- yaw rate, pitch rate, zoom -- chosen every
    decision, independently of the movement action and of each other. Initialised as the other policy outputs are
    (small gain), its bias leaning toward each head's hold choice (LOOK_HOLD_BIAS).

    Only rows whose layout has the camera look: for the others the choice is 0 (what the wire carries, and the sim
    ignores), and its log probability and entropy are 0 with no gradient. Nothing is masked: every choice is allowed.
    Fixed shapes and no reads back, so a rollout graph captures it."""

    def __init__(self, width: int, heads: Sequence[int], layouts_with: torch.Tensor):
        super().__init__()
        self.heads = tuple(int(head) for head in heads)
        self.linear = _linear(width, sum(self.heads), 0.01)
        with torch.no_grad():
            offset = 0
            for head, hold in zip(self.heads, look_hold_indices(self.heads)):
                self.linear.bias[offset + hold] = LOOK_HOLD_BIAS
                offset += head
        self.register_buffer("has_vision", layouts_with.clone(), persistent=False)

    def logits(self, features: torch.Tensor) -> list[torch.Tensor]:
        """Each head's logits for flat rows, [[N, head], ...]."""
        return list(torch.split(self.linear(features), self.heads, dim=-1))

    def _seeing(self, layout: torch.Tensor) -> torch.Tensor:
        return self.has_vision[layout.reshape(-1).long()]

    def sample(self, features: torch.Tensor, layout: torch.Tensor,
               deterministic: bool) -> tuple[torch.Tensor, torch.Tensor]:
        """(choice [N, heads] long, its log probability [N], the heads' summed) for flat rows; 0 and 0 for a row
        without the camera. Deterministic is each head's argmax."""
        seeing = self._seeing(layout)
        choices, log_prob = [], None
        for logits in self.logits(features):
            choice, chosen = sample_logits(logits.float(), deterministic)
            choices.append(choice)
            log_prob = chosen if log_prob is None else log_prob + chosen
        choice = torch.where(seeing[:, None], torch.stack(choices, dim=-1), torch.zeros_like(choices[0])[:, None])
        return choice, torch.where(seeing, log_prob, torch.zeros_like(log_prob))

    def evaluate(self, features: torch.Tensor, layout: torch.Tensor,
                 choice: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """(log probability [N], entropy [N]) of the choices `choice` [N, heads] taken, each the heads' sum; 0 with no
        gradient for a row without the camera."""
        seeing = self._seeing(layout)
        choice = choice.reshape(-1, len(self.heads)).long()
        log_prob = entropy = None
        for index, logits in enumerate(self.logits(features)):
            logits = logits.float()
            part = log_prob_of(logits, choice[:, index].clamp(0, self.heads[index] - 1))
            spread = _entropy(logits)
            log_prob = part if log_prob is None else log_prob + part
            entropy = spread if entropy is None else entropy + spread
        zero = torch.zeros_like(log_prob)
        return torch.where(seeing, log_prob, zero), torch.where(seeing, entropy, zero)


def _attach_vision(network: nn.Module, vision, encoder: "VisionEncoder | None" = None) -> None:
    """Give an actor or critic the camera: the encoder (VisionEncoder) and its own join (VisionJoin), and keep the
    camera's columns -- image and scalars -- out of every layout's adapter (attach_blind_columns, tag "vision") and
    normaliser (RunningNorm.hide). `vision` is vision_of()'s per-layout image, or None (no camera: nothing changes).
    With `encoder` (another network's: the trainer's critic gets the actor's), the network reads that one through a
    reference that is not one of its modules -- not in its parameters, its state dict or its .to() -- else it builds
    and owns its own."""
    network.vision = None
    network.vision_join = None
    if vision is None:
        return
    if encoder is None:
        network.vision = VisionEncoder(vision)
    else:
        share_vision(network, encoder)
    network.vision_join = VisionJoin(network.vision.has_vision, network.adapters[0].out_features)
    attach_blind_columns(network, network.vision.blind, tag="vision")
    for index, columns in network.vision.blind.items():
        network.norms[index].hide(columns)


def share_vision(network: nn.Module, encoder: "VisionEncoder") -> None:
    """Point `network` at another network's camera encoder without making it one of its modules (no second copy in
    its parameters or state dict)."""
    network._modules.pop("vision", None)
    network.__dict__["vision"] = encoder


def vision_term(network: nn.Module, obs: torch.Tensor, layout: torch.Tensor,
                embedding: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
    """What the camera adds to `network`'s adapter output: its join over the encoder's embedding of the rows' images
    `image` [N, I] uint8 (computed here unless the caller has it: the rollout graph and the update encode once for
    both networks)."""
    if embedding is None:
        embedding = network.vision(obs, layout, image)
    return network.vision_join(embedding, layout)


def _vision_extra(network: nn.Module, obs: torch.Tensor, layout: torch.Tensor,
                  embedding: torch.Tensor | None = None, image: torch.Tensor | None = None):
    """What the camera adds to the adapter's output, or None for a network without one."""
    if getattr(network, "vision", None) is None:
        return None
    return vision_term(network, obs, layout, embedding, image)


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


#: The blind-column masks' buffer names (attach_blind_columns' tags): "blind_keep_" (a checkpoint from before the hint
#: block went still carries it, and it is dropped on load), the camera's.
BLIND_KEEP_PREFIXES = ("blind_keep_", "vision_keep_")


def without_blind_columns(state: dict) -> dict:
    """A saved network's state without its blind-column masks (attach_blind_columns): they are made again from the
    stage's layouts after loading (the camera), and a network not yet given them refused them."""
    return {key: value for key, value in state.items()
            if not key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)}


def load_actor_state(actor: nn.Module, state: dict) -> None:
    """actor.load_state_dict(state), except that an actor saved before the goal scale existed loads with it at zero."""
    missing, unexpected = actor.load_state_dict(without_blind_columns(state), strict=False)
    # The blind-column masks are never taken from a checkpoint: an actor built with seat sets has its own already (the
    # stage's), which a resume keeps.
    wrong = [key for key in missing
             if key not in _GOAL_SCALE_KEYS and not key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)]
    if wrong or unexpected:
        raise RuntimeError(f"Error(s) in loading state_dict for {type(actor).__name__}: missing {wrong}, "
                           f"unexpected {list(unexpected)}")
    parameters = dict(actor.named_parameters())
    with torch.no_grad():
        for key in missing:
            if key in _GOAL_SCALE_KEYS:
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
                 foresight_feedback: bool = False, lookahead: bool = False, goal_slots: int = 1,
                 vision=None):
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
        # The camera (VisionEncoder; vision_of(stage.json), None = no layout has one).
        _attach_vision(self, vision)
        # The sight list's pointer heads (SightPointers), where the layouts have a sight block: the actor's own.
        sight = self.vision.sight if self.vision is not None else None
        self.sight_pointers = SightPointers(sight.presses, head_width) if sight is not None else None
        # Its free look (LookHead), where the vision block has look heads (revision 4): the actor's own, not shared.
        look_heads = vision_look_heads(vision)
        self.look_head = LookHead(head_width, look_heads, self.vision.has_vision) if look_heads else None

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, groups=None,
                memory: torch.Tensor | None = None, image: torch.Tensor | None = None) -> Categorical:
        """obs [..., O], layout [...], mask [..., N] (padded) -> distribution over N actions. `groups` is
        per_layout(layout) when the caller already has it; `memory` [..., recurrent_size] the state carried in;
        `image` [..., I] uint8 the camera's bytes (protocol 21), with a camera."""
        return self._forward(obs, layout, mask, groups, memory, image=image)[0]

    def step(self, obs: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor, memory: torch.Tensor | None = None,
             groups=None, goal: torch.Tensor | None = None,
             image: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, torch.Tensor]:
        """One decision: (distribution, memory carried out, foresight predictions). Without a GRU the memory out is
        whatever came in (an empty tensor), and without foresight heads the predictions are empty."""
        dist, features, lead = self._forward(obs, layout, mask, groups, memory, goal, image)
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

    def encode(self, obs: torch.Tensor, layout: torch.Tensor, groups=None,
               vision_embedding: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
        """Adapters and trunk for flat rows: everything that depends only on this decision's observation, before the
        GRU. A replayed sequence encodes every step in one pass and then carries the memory through them (carry),
        which is the difference between one large matmul per layer and one per step. `vision_embedding` is the
        camera's embedding of these rows when the caller has it (the update encodes once for the actor and the
        critic); else it is computed here, from the rows' camera bytes `image` [N, I]."""
        extra = _vision_extra(self, obs, layout, vision_embedding, image)
        if self.dense_adapters is not None:
            hidden = self.dense_adapters(obs, layout)
            return self.trunk(hidden if extra is None else hidden + extra.to(hidden.dtype))
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        width = self.adapters[0].out_features
        hidden = obs.new_zeros(obs.shape[0], width)
        for index, rows in groups:
            # Cast on the way in: the rows' dtype is the adapters', the zeros' the observation's.
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
                 groups=None, image: torch.Tensor | None = None,
                 vision_embedding: torch.Tensor | None = None) -> torch.Tensor:
        """The trunk's output for flat rows, through the GRU when there is one: what every head reads.
        `vision_embedding`: the camera's embedding of these rows when the caller has it (as for encode)."""
        return self.features_from(self.encode(obs, layout, groups, vision_embedding, image), memory)

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
        lets the sight list's pointer heads score their per-slot actions."""
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

    def policy_features(self, features: torch.Tensor, goal: torch.Tensor | None = None) -> torch.Tensor:
        """The final policy features the action head reads -- the predictions fed back, then the goal's conditioning
        -- and the look head with it."""
        features = self.with_foresight(features)
        if self.goal_embedding is not None and goal is not None:
            features = self.goal_embedding.condition(features, goal.reshape(-1))
        return features

    def look(self, features: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
             deterministic: bool = False) -> tuple[torch.Tensor, torch.Tensor]:
        """The free look's choice for flat rows (LookHead.sample): (choice [N, heads], log probability [N])."""
        return self.look_head.sample(self.policy_features(features, goal), layout, deterministic)

    def look_terms(self, features: torch.Tensor, layout: torch.Tensor, choice: torch.Tensor,
                   goal: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """The free look's (log probability [N], entropy [N]) of choices taken (LookHead.evaluate)."""
        return self.look_head.evaluate(self.policy_features(features, goal), layout, choice)

    def action_logits(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                      goal: torch.Tensor | None = None, groups=None, obs: torch.Tensor | None = None) -> torch.Tensor:
        """action_distribution's masked logits, unnormalised (for sample_logits)."""
        features = self.policy_features(features, goal)

        if self.dense_heads is not None:
            dense = self.dense_heads
            own = torch.where(dense.valid[layout.long()], dense(features, layout), MASKED_LOGIT)
            logits = own if own.shape[-1] == mask.shape[-1] else nn.functional.pad(
                own, (0, mask.shape[-1] - own.shape[-1]), value=MASKED_LOGIT)
            return masked_logits(self._with_sight_pointers(logits, features, layout, obs), mask)

        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        logits = features.new_full((features.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](features[rows]).to(logits.dtype)
        return masked_logits(self._with_sight_pointers(logits, features, layout, obs), mask)

    def _with_sight_pointers(self, logits: torch.Tensor, features: torch.Tensor, layout: torch.Tensor,
                             obs: torch.Tensor | None) -> torch.Tensor:
        """The sight list's presses scored by their slots' tokens (SightEntities.with_pointers)."""
        if self.sight_pointers is None or obs is None:
            return logits
        return self.vision.sight.with_pointers(logits, features, obs.reshape(-1, obs.shape[-1]), layout,
                                               self.sight_pointers)

    def goal_distribution(self, features: torch.Tensor, obs: torch.Tensor | None = None,
                          layout: torch.Tensor | None = None) -> Categorical:
        """Which goal to pursue next, from the same features, masked by what the goal block says is there."""
        return Categorical(logits=self.goal_head.logits(features, obs, layout))

    def decide_goals(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, held: torch.Tensor,
                     queue: torch.Tensor, clock: torch.Tensor, deterministic: bool) -> dict[str, torch.Tensor]:
        """**The goal decision of one step** (next-run plan, Wave 4), the same in the rollout, its captured graph,
        the teachers and the module's runtime. Flat rows; `held` the goal (pair) held, `queue` [rows, S - 2] the goals
        waiting (-1 none), `clock` the goal clock's choices. In order:

        - an ended secondary is dropped (the sim dropped it too);
        - an ended primary with a queue is replaced by the queue's head, with no choice made (the plan that queued
          it keeps the credit);
        - a choice is made on the clock, where the primary ended with nothing queued, or on the block's event;
        - an order set in the goal block, where there is one, is the primary whatever was held or drawn.

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
                 memory: torch.Tensor | None = None, goal: torch.Tensor | None = None,
                 image: torch.Tensor | None = None) -> tuple[Categorical, torch.Tensor, tuple[int, ...]]:
        lead = obs.shape[:-1]
        obs, layout, mask = obs.reshape(-1, obs.shape[-1]), layout.reshape(-1), mask.reshape(-1, mask.shape[-1])
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))

        hidden = self.features(obs, layout, memory, groups, image)
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
                 goal_count: int = 0, recurrent_size: int = 0, goal_targets: int = 1,
                 goal_slots: int = 1, vision=None,
                 vision_encoder: "VisionEncoder | None" = None):
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
        # The camera: the actor's encoder when given one (`vision_encoder`, MappoTrainer: one encoder, both losses'
        # gradients), else its own; the join onto its adapters' output is always its own.
        _attach_vision(self, vision, vision_encoder)

    def encode(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
               goal: torch.Tensor | None = None, groups=None,
               vision_embedding: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
        """Everything that depends only on this decision -- the state, the seat's own observation and its goal --
        for flat rows, before the GRU. A replayed sequence encodes every step in one pass and then carries the memory
        through them (carry). `vision_embedding` as for LayoutActor.encode."""
        hidden, own = self.encode_goal_free(state, obs, layout, groups, vision_embedding, image)
        return self.encode_goal(hidden, own, goal)

    def encode_goal_free(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor,
                         groups=None, vision_embedding: torch.Tensor | None = None,
                         image: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
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
        if self.vision is not None:
            own = own + vision_term(self, obs, layout, vision_embedding, image).to(own.dtype)
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
             groups=None, memory: torch.Tensor | None = None, image: torch.Tensor | None = None,
             vision_embedding: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """One decision: (value [...], the memory carried out). Without a GRU the memory out is whatever came in.
        `vision_embedding`: the camera's embedding of these rows when the caller has it (the actor's, in a rollout
        decision), else it is computed from `image`."""
        lead = obs.shape[:-1]
        state, obs, layout = state.reshape(-1, state.shape[-1]), obs.reshape(-1, obs.shape[-1]), layout.reshape(-1)
        return self.step_encoded(self.encode(state, obs, layout, goal, groups, vision_embedding=vision_embedding,
                                             image=image), lead, memory)

    def step_encoded(self, encoded: torch.Tensor, lead, memory: torch.Tensor | None = None):
        """step() from the encoding on: the GRU and the value head."""
        if self.memory is None:
            return self.head(encoded).reshape(lead), (memory if memory is not None else encoded.new_zeros((*lead, 0)))

        carried = self.memory(encoded, memory.reshape(-1, self.recurrent_size) if memory is not None
                              else encoded.new_zeros(encoded.shape[0], self.recurrent_size))
        return self.head(carried).reshape(lead), carried.reshape(*lead, self.recurrent_size)

    def forward(self, state: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
                groups=None, memory: torch.Tensor | None = None, image: torch.Tensor | None = None) -> torch.Tensor:
        """state [..., S], obs [..., O], layout [...], goal [...] (with a goal head) -> value [...]. `groups` as for
        LayoutActor.forward; `memory` [..., recurrent_size] the state carried in; `image` as for LayoutActor."""
        return self.step(state, obs, layout, goal, groups, memory, image)[0]
