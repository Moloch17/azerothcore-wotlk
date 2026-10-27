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
import os
from collections.abc import Sequence

import numpy as np
import torch
from torch import nn
from torch.distributions import Categorical

from ..protocol import HEADING, TRAV_CLASS_NAMES, TRAV_CLASSES, continuous_of

MASKED_LOGIT = -1e9

#: The goal block's place slots (m2-goals; stage.json goals.place_slots): the values a slot has (sin and cos of the
#: bearing, distance, coverage, glimpse age) and the pointer's hidden width.
PLACE_FEATURES = 5
POINTER_HIDDEN = 16


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


def uniform_logits(logits: torch.Tensor) -> torch.Tensor:
    """Equal logits for every goal that is allowed, the masked ones still masked."""
    return torch.where(logits > MASKED_LOGIT / 2, torch.zeros_like(logits), logits)


def none_only_logits(logits: torch.Tensor) -> torch.Tensor:
    """Only the last column (none) allowed: a slot that holds nothing."""
    out = torch.full_like(logits, MASKED_LOGIT)
    out[:, -1] = 0.0
    return out


def log_prob_of(logits: torch.Tensor, choice: torch.Tensor) -> torch.Tensor:
    """log softmax(logits)[choice]: Categorical(logits=...).log_prob(choice), without building the distribution."""
    return logits.gather(-1, choice.long()[..., None]).squeeze(-1) - torch.logsumexp(logits, dim=-1)


def _entropy(logits: torch.Tensor) -> torch.Tensor:
    """The entropy of softmax(logits) per row, masked logits included (they carry ~0 probability)."""
    log_p = logits - torch.logsumexp(logits, dim=-1, keepdim=True)
    return -(log_p.exp() * log_p).sum(-1)


def sample_logits(logits: torch.Tensor, deterministic: bool = False,
                  greedy: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
    """(choice, its log probability) from unnormalised logits, as Categorical(logits=...).sample() and log_prob draw
    them, in a handful of kernels instead of the ~30 the distribution's normalising, softmax, multinomial and checks
    take: the rollout's inner loop. Gumbel-max: argmax(logits + G), G = -log(-log U), is an exact draw. U is in
    [0, 1), so U = 0 gives G = -inf, never +inf: a masked action cannot win on a lucky draw.

    `greedy` (bool [N], exploration v3, mappo.greedy_env_fraction): the rows that take the argmax instead of the draw.
    The draw is computed for every row either way and the rows are selected with a where, so the device generator
    consumes the same numbers whatever the mask and the sampled rows' draws do not depend on it; the stored log
    probability of a greedy row is log pi(argmax), which the update does not use as an on-policy sample (it masks
    those rows out of the policy gradient). Ignored with `deterministic`."""
    if deterministic:
        choice = logits.argmax(dim=-1)
    else:
        choice = (logits - torch.log(-torch.log(torch.rand_like(logits)))).argmax(dim=-1)
        if greedy is not None:
            choice = torch.where(greedy.reshape(-1), logits.argmax(dim=-1), choice)
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


#: **The cell pointer** (free-choice-goals, stage.json goals.cells): a goal over the mental map's crop. The crop (48 x 48
#: cells of 2 yd, heading-up) is pooled CELL_POOL x CELL_POOL into a grid of CELL_GRID x CELL_GRID blocks; the head scores
#: every block and the draw is a block index. These are the contract's defaults; the manifest's values are what a run
#: uses (GoalHead.set_space).
CELL_GRID = 24
CELL_POOL = 2
CELL_FEATURES = 22
CELL_HIDDEN = 32
CELL_CHAIN_HIDDEN = 16
#: A block is choosable when it has at least this many floor-like (or unknown) cells; CELL_RISE_UNITS (4 yd) is no
#: longer a limit (cell_valid, I8) but still the scale of the block's mean-height feature (cell_features).
CELL_MIN_FLOOR = 2
CELL_RISE_UNITS = 16


def rise_scale(rise: int) -> float:
    """The height units the cell features are scaled by: `rise`, or CELL_RISE_UNITS where the manifest says 0 (the
    opened cell grid, I8: no height limit). 0 divided the features (sarah's and the leader's nav1 runs, 2026-10-14:
    NaN cell features, a NaN slow goal loss, every slow parameter non-finite from the first update)."""
    return float(rise) if rise > 0 else float(CELL_RISE_UNITS)
#: The block offsets are scaled by this (the grid of the contract's pool 2).
CELL_OFFSET_SCALE = 24.0
#: A cell word is (ticket << CELL_BITS) | (cell + 1); tickets are 1..2^TICKET_BITS - 1.
CELL_BITS = 12
TICKET_BITS = 11
TICKET_COUNT = (1 << TICKET_BITS) - 1


def _block_sum(x: torch.Tensor, grid: int, pool: int) -> torch.Tensor:
    """[N, side, side] -> [N, grid, grid]: the sum over each pool x pool block."""
    return x.reshape(x.shape[0], grid, pool, grid, pool).sum(dim=(2, 4))


def cell_valid(crops: torch.Tensor, grid: int = CELL_GRID, pool: int = CELL_POOL, rise: int = CELL_RISE_UNITS,
               min_floor: int = CELL_MIN_FLOOR) -> torch.Tensor:
    """Which pooled blocks of the mental map's crop a cell goal may name, bool [N, grid * grid] (row-major, block (r, c)
    at r * grid + c) from the crop bytes [N, side x side x 7] (or [N, side, side, 7]) alone -- the contract's one
    definition (free-choice-goals CONTRACT 1.3, opened by the vision movement curriculum's I8, CONTRACT.md sec 3.3),
    which the sim's CellGrid::Choosable implements too: no cell of the block is a Wall or a Hazard, and at least
    `min_floor` are floor-like (Floor or Door) OR unknown; no height limit (`rise` is kept for the callers and the
    export, and read by nothing). A storey above, or a direction through unknown space, is pressable; a bad pick is
    priced by the Cell* costs, never masked. A pure function of the bytes, with no branch on data: the rollout graph
    captures it."""
    side = grid * pool
    cells = crops.reshape(-1, side, side, MAP_CHANNELS)
    code = cells[..., MAP_CODE]
    like = (code == MAP_FLOOR) | (code == MAP_DOOR) | (code == 0)
    barred = (code == MAP_WALL) | (code == MAP_HAZARD)
    count = lambda mask: _block_sum(mask.to(torch.int32), grid, pool)
    return ((count(barred) == 0) & (count(like) >= min_floor)).reshape(cells.shape[0], -1)


def cell_features(crops: torch.Tensor, grid: int = CELL_GRID, pool: int = CELL_POOL, rise: int = CELL_RISE_UNITS,
                  min_floor: int = CELL_MIN_FLOOR) -> tuple[torch.Tensor, torch.Tensor]:
    """The cell pointer's inputs from the crop bytes [N, side x side x 7]: (features [N, grid * grid, CELL_FEATURES],
    choosable bool [N, grid * grid]). Per block, over its pool x pool cells: the shares of Floor, Door, Wall, Hazard and
    Unknown; the share visited and the share on the frontier; whether any cell holds an entity; the age of the newest
    look at any seen cell (b / 255) and whether none was seen; the mean floor height over the feet ((b - 128) / 16,
    clamped to [-1, 1]); the 3 x 3 average over neighbouring blocks of the floor, wall, unknown and frontier shares;
    the block's centre as forward, right and distance in the crop's yards over 48; and (exploration v3, appended so the
    first 18 keep their index) the block's searched share (the share of its 1-yd cells looked at this episode) and that
    share averaged over the 3 x 3 and the 7 x 7 blocks round it (a 28 yd neighbourhood: how much of the region is still
    unlooked-at; an unknown cell reads 0, so unexplored space and unsearched floor both show, and the block's own
    unknown / floor / frontier features say which); and (general search sec 3, appended so the first 21 keep their
    index) the frontier share averaged over the 7 x 7 blocks round it (`frontier_7x7`: the frontier mass of the 28-yd
    neighbourhood, the graph-safe stand-in for the size of the frontier cluster the block belongs to -- true
    components are data-dependent and not capturable). All from the crop, which is the mental map's own (never the
    navmesh)."""
    side = grid * pool
    cells = crops.reshape(-1, side, side, MAP_CHANNELS)
    n = cells.shape[0]
    code = cells[..., MAP_CODE]
    height = cells[..., MAP_HEIGHT].to(torch.int16)
    area = float(pool * pool)
    share = lambda mask: _block_sum(mask.to(torch.float32), grid, pool) / area
    floor, door = share(code == MAP_FLOOR), share(code == MAP_DOOR)
    wall, hazard = share(code == MAP_WALL), share(code == MAP_HAZARD)
    unknown = share(code == 0)
    visited = share(cells[..., MAP_VISITED] != 0)
    frontier = share(cells[..., MAP_FRONTIER] != 0)
    entity = (_block_sum(((cells[..., MAP_CLASS] & CLASS_MASK) != 0).to(torch.float32), grid, pool) > 0).to(
        torch.float32)
    seen = code != 0
    age = torch.where(seen, cells[..., MAP_AGE], torch.full_like(cells[..., MAP_AGE], 255)).to(torch.float32)
    newest = age.reshape(n, grid, pool, grid, pool).amin(dim=(2, 4)) / 255.0
    never = (_block_sum(seen.to(torch.float32), grid, pool) == 0).to(torch.float32)
    like = (code == MAP_FLOOR) | (code == MAP_DOOR)
    rise_over = ((height - MAP_HEIGHT_ZERO).to(torch.float32) / rise_scale(rise)) * like.to(torch.float32)
    like_count = _block_sum(like.to(torch.float32), grid, pool)
    mean_height = (_block_sum(rise_over, grid, pool) / like_count.clamp(min=1.0)).clamp(-1.0, 1.0)
    around = nn.functional.avg_pool2d(torch.stack([floor, wall, unknown, frontier], dim=1), 3, stride=1, padding=1,
                                      count_include_pad=False)
    forward, right, distance = cell_geometry(grid, pool, cells.device)
    geometry = torch.stack([forward, right, distance], dim=0)[None].expand(n, -1, -1, -1)
    searched = _block_sum(cells[..., MAP_SEARCHED].to(torch.float32), grid, pool) / float(
        MAP_SEARCHED_MAX * pool * pool)
    searched_around = torch.cat([
        nn.functional.avg_pool2d(searched[:, None], 3, stride=1, padding=1, count_include_pad=False),
        nn.functional.avg_pool2d(searched[:, None], 7, stride=1, padding=3, count_include_pad=False)], dim=1)
    frontier_7x7 = nn.functional.avg_pool2d(frontier[:, None], 7, stride=1, padding=3, count_include_pad=False)
    features = torch.cat([torch.stack([floor, door, wall, hazard, unknown, visited, frontier, entity, newest, never,
                                       mean_height], dim=1), around, geometry, searched[:, None], searched_around,
                          frontier_7x7], dim=1)
    return features.reshape(n, CELL_FEATURES, grid * grid).transpose(1, 2), cell_valid(crops, grid, pool, rise,
                                                                                               min_floor)


def cell_geometry(grid: int, pool: int, device) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Each block's centre from the body in yards (the crop's cells are 2 yd, heading-up, row 0 furthest ahead),
    [grid, grid] each: forward = side - pool * (2r + 1) and right = pool * (2c + 1) - side, over 48, and the distance
    over 48. For pool 2 on a 48-cell crop: forward = 46 - 4r, right = 4c - 46."""
    side = grid * pool
    rows = torch.arange(grid, device=device, dtype=torch.float32)
    forward = (side - pool * (2.0 * rows + 1.0)) / 48.0
    right = (pool * (2.0 * rows + 1.0) - side) / 48.0
    forward = forward[:, None].expand(grid, grid)
    right = right[None, :].expand(grid, grid)
    return forward, right, torch.sqrt(forward * forward + right * right)


class CellPointer(nn.Module):
    """The cell pointer's parameters (GoalHead.cell, `goal_head.cell.*` in a checkpoint): a block's own features
    (`local`, CELL_FEATURES -> CELL_HIDDEN, SiLU in the head), the slot's query (`query`, the head's feature width ->
    CELL_HIDDEN), the score (`score`, CELL_HIDDEN -> 1) and the chain feature (`chain`, the block's offset from the
    previous plan cell -> a score). The score and the chain's last layer start at zero: a fresh head draws uniformly over
    the choosable blocks."""

    def __init__(self, width: int):
        super().__init__()
        self.local = _linear(CELL_FEATURES, CELL_HIDDEN, math.sqrt(2))
        self.query = _linear(width, CELL_HIDDEN, 1.0)
        self.score = nn.Linear(CELL_HIDDEN, 1)
        nn.init.zeros_(self.score.weight)
        nn.init.zeros_(self.score.bias)
        self.chain = nn.Sequential(nn.Linear(3, CELL_CHAIN_HIDDEN), nn.Tanh(), nn.Linear(CELL_CHAIN_HIDDEN, 1))
        nn.init.orthogonal_(self.chain[0].weight, gain=1.0)
        nn.init.zeros_(self.chain[0].bias)
        nn.init.zeros_(self.chain[2].weight)
        nn.init.zeros_(self.chain[2].bias)
        # The planner's bonus (vision movement curriculum, CONTRACT.md sec 5): the value of each block under the VIN
        # (MapEncoder.plan's pool) times this scalar, added to the score. Zero at the start, so a fresh head draws as it
        # did; a checkpoint from before the planner loads without it (load_actor_state tolerates goal_head.cell.*).
        self.planner_weight = nn.Parameter(torch.zeros(1))


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
        # **The pointer over place slots** (m2-goals W3a): the goal block (revision 3) carries per place slot k its
        # bearing, distance, coverage and glimpse age (PLACE_FEATURES values), and the target logit of place k is
        # raised by pointer(slot k's features) -- an MLP shared by the slots, so the head chooses over rooms by what it
        # knows of each, not by a slot id (slots are bound in discovery order, so an id means nothing across episodes).
        # The last layer starts at zero: a seeded head chooses exactly as it did.
        self.pointer = nn.Sequential(nn.Linear(PLACE_FEATURES, POINTER_HIDDEN), nn.Tanh(),
                                     nn.Linear(POINTER_HIDDEN, 1)) if self.targets > 1 else None
        if self.pointer is not None:
            nn.init.orthogonal_(self.pointer[0].weight, gain=1.0)
            nn.init.zeros_(self.pointer[0].bias)
            nn.init.zeros_(self.pointer[2].weight)
            nn.init.zeros_(self.pointer[2].bias)
        # Where the features are (stage.json "goals": columns.place_features and place_slots), as four numbers: the
        # first feature column inside the goal block, the first place target, how many slots and how many features a
        # slot; -1 throughout for no pointer (a manifest of goal block revision 2). A buffer, so a checkpoint plays the
        # same way where it is loaded without a stage (evaluate, export); the host copy is for the captured graphs.
        self.register_buffer("place_spec", torch.full((4,), -1, dtype=torch.long))
        self.place_at, self.place_first, self.place_count = -1, -1, 0
        # **The pointer over the map's cells** (free-choice-goals): a goal that is a block of the mental map's crop, scored
        # by `cell` (CellPointer) -- parameters of every goal head with targets, unused (and at zero) where the stage has
        # no cells. Where the crop sits is cell_spec, as four numbers: the byte the crop starts at in a row's image
        # bytes (after the camera's pixels), the grid, the pool and the joint goal id of a cell goal; -1 throughout for
        # no cell head (a manifest before goal block revision 4). A buffer, like place_spec; the host copy is for the
        # captured graphs.
        self.cell = CellPointer(width) if self.targets > 1 else None
        self.register_buffer("cell_spec", torch.full((4,), -1, dtype=torch.long))
        # The weight of the cell's entropy in the head's (MappoConfig.goal_cell_entropy_weight).
        self.cell_entropy_weight = 0.3
        self.cell_at, self.cell_grid, self.cell_pool, self.cell_joint = -1, CELL_GRID, CELL_POOL, -1
        self.cell_rise, self.cell_min_floor = CELL_RISE_UNITS, CELL_MIN_FLOOR
        # The goal block's column (from the block's start) of goal_from_present, -1 without: the planner's hindsight.
        self.cell_from = -1

    @property
    def count(self) -> int:
        return self.kinds * self.targets

    def _load_from_state_dict(self, state_dict, prefix, *args, **kwargs):
        super()._load_from_state_dict(state_dict, prefix, *args, **kwargs)
        # A loaded goal space (a checkpoint's block positions) says itself whether there is one.
        self.has_space = bool((self.block_at >= 0).any())
        self._mirror_place_spec()
        self._mirror_cell_spec()

    @property
    def block_width(self) -> int:
        """The goal block's columns (GoalBlock::Obs): kinds there, targets there, ended, reached, and from the
        next-run format on (slots > 1) the secondary ending, the event and what was achieved (kind and target
        one-hots; goal block revision 2: the order columns left it)."""
        base = self.kinds + self.targets + 2
        return base if self.slots <= 1 else base + 2 + (self.kinds + self.targets)

    # Where the next-run columns sit in the goal block (GoalBlock::Obs; stage.json "goals"."columns").
    @property
    def columns(self) -> dict[str, int]:
        base = self.kinds + self.targets + 2
        return {"secondary_ended": base, "event": base + 1, "achieved_kind": base + 2,
                "achieved_target": base + 2 + self.kinds}

    def set_space(self, accepts, block_at, goals: dict | None = None, image_offset: int = -1,
                  map_side: int = 0) -> str:
        """The goal space the sim wrote (stage.json "goals"): accepts [kinds][targets], and per layout the goal
        block's first observation column (-1 without one). `goals` is the manifest's "goals" object, which says where
        the place slots' features sit (columns.place_features, place_slots) and, from goal block revision 4, the cell
        goals (cells); without them the pointer, or the cell head, is off. `image_offset` is the byte the mental map's
        crop starts at in a row's image bytes (the camera's pixels before it; -1 without a map) and `map_side` the
        crop's cells a side. Returns a line about the pointers for the log."""
        self.accepts.copy_(torch.as_tensor(accepts, dtype=torch.bool, device=self.accepts.device))
        self.block_at.copy_(torch.as_tensor(block_at, dtype=torch.long, device=self.block_at.device))
        # Kept on the host: a rollout graph cannot read the device while it is captured.
        self.has_space = bool(np.any(np.asarray(block_at) >= 0))
        spec, note = self._place_spec_of(goals)
        self.place_spec.copy_(torch.as_tensor(spec, dtype=torch.long, device=self.place_spec.device))
        self._mirror_place_spec()
        cell_spec, cell_note = self._cell_spec_of(goals, image_offset, map_side)
        self.cell_spec.copy_(torch.as_tensor(cell_spec, dtype=torch.long, device=self.cell_spec.device))
        self._mirror_cell_spec()
        return f"{note}; {cell_note}"

    def _cell_spec_of(self, goals: dict | None, image_offset: int, map_side: int) -> tuple[list[int], str]:
        """The cell head's cell_spec from a manifest's "goals" (see __init__) and a line saying what it found. Sets the
        host constants the block features and the validity rule read (rise, floor cells, the hindsight column). Refused:
        a manifest whose cell constants this learner does not implement (codes, channels, field widths)."""
        off = [-1, -1, -1, -1]
        self.cell_rise, self.cell_min_floor, self.cell_from = CELL_RISE_UNITS, CELL_MIN_FLOOR, -1
        cells = (goals or {}).get("cells")
        if cells is None:
            return off, "no goals.cells in the manifest (goal block before revision 4): no cell head"
        if self.cell is None or self.slots <= 1:
            return off, "goals.cells in the manifest, but the goal head has one slot or no targets: no cell head"
        if image_offset < 0 or map_side <= 0:
            return off, "goals.cells in the manifest, but the stage's layouts have no mental map: no cell head"
        grid, pool, crop = int(cells["grid"]), int(cells["pool"]), int(cells["crop"])
        joint, target = int(cells["joint"]), int(cells["target"])
        if crop != grid * pool or crop != map_side:
            raise ValueError(f"goals.cells: a {grid} x {grid} grid of pool {pool} is not the {crop}-cell crop, or the "
                             f"map's crop is {map_side} cells a side")
        if int(cells.get("channels", MAP_CHANNELS)) != MAP_CHANNELS or \
                (int(cells["code_channel"]), int(cells["height_channel"])) != (MAP_CODE, MAP_HEIGHT) or \
                (int(cells["floor_code"]), int(cells["wall_code"]), int(cells["door_code"]),
                 int(cells["hazard_code"])) != (MAP_FLOOR, MAP_WALL, MAP_DOOR, MAP_HAZARD) or \
                int(cells["height_zero"]) != MAP_HEIGHT_ZERO:
            raise ValueError(f"goals.cells describes a crop this learner does not decode: {cells}")
        if int(cells["ticket_bits"]) != TICKET_BITS or int(cells["cell_bits"]) != CELL_BITS \
                or int(cells["positions"]) != 4 or grid * grid >= (1 << CELL_BITS):
            raise ValueError(f"goals.cells: cell words of {cells['ticket_bits']} + {cells['cell_bits']} bits over "
                             f"{cells['positions']} positions, this learner writes {TICKET_BITS} + {CELL_BITS} over 4")
        if not 0 <= joint < self.count or joint % self.targets != target or not 0 <= target < self.targets:
            raise ValueError(f"goals.cells: joint {joint} / target {target} is not in the {self.kinds} x "
                             f"{self.targets} goal space")
        # 0 from a manifest of the opened grid (no height limit): the features keep the default scale (rise_scale).
        self.cell_rise = int(cells.get("rise_units", CELL_RISE_UNITS)) or CELL_RISE_UNITS
        self.cell_min_floor = int(cells.get("min_floor_cells", CELL_MIN_FLOOR))
        columns = (goals or {}).get("columns") or {}
        width = int(columns.get("width", 0))
        if "from" in columns and int(columns["from"]) + 3 <= width:
            self.cell_from = int(columns["from"])
        kinds = (goals or {}).get("kinds") or ()
        kind = kinds[joint // self.targets] if 0 <= joint // self.targets < len(kinds) else "?"
        return ([int(image_offset), grid, pool, joint],
                f"cell head over {grid} x {grid} blocks (pool {pool}) of the map crop at image byte {image_offset}, "
                f"cell goals are joint {joint} ({kind} about target {target}), source {cells.get('source', '?')}"
                + ("" if self.cell_from >= 0 else "; no goals.columns.from: no planner hindsight"))

    def _mirror_cell_spec(self) -> None:
        """The host copy of cell_spec (a rollout graph cannot read the device while it is captured)."""
        at, grid, pool, joint = (int(v) for v in self.cell_spec.tolist())
        on = self.cell is not None and at >= 0 and joint >= 0 and grid > 0 and pool > 0
        self.cell_at, self.cell_grid, self.cell_pool, self.cell_joint = (at, grid, pool, joint) if on \
            else (-1, CELL_GRID, CELL_POOL, -1)

    @property
    def cell_on(self) -> bool:
        return self.cell_joint >= 0 and self.slots > 1

    def _place_spec_of(self, goals: dict | None) -> tuple[list[int], str]:
        """The pointer's place_spec from a manifest's "goals" (see __init__) and a line saying what it found."""
        off = [-1, -1, -1, -1]
        columns = (goals or {}).get("columns") or {}
        slots = (goals or {}).get("place_slots")
        if self.pointer is None or not slots or "place_features" not in columns:
            return off, "no place slots in the manifest (goal block before revision 3): no pointer term"
        first, count, features = int(slots["first"]), int(slots["count"]), int(slots["features"])
        at = int(columns["place_features"])
        if features != PLACE_FEATURES:
            raise ValueError(f"goals.place_slots.features is {features}, the pointer reads {PLACE_FEATURES} "
                             f"({', '.join(slots.get('feature_names') or ())})")
        if first < 0 or count < 1 or first + count > self.targets:
            raise ValueError(f"goals.place_slots {first}..{first + count - 1} is not inside the {self.targets} goal "
                             f"targets")
        width = int(columns.get("width", at + count * features))
        if at + count * features > width:
            raise ValueError(f"goals.columns.place_features {at} + {count} x {features} reaches past the goal "
                             f"block's width {width}")
        return ([at, first, count, features],
                f"pointer over {count} place slots (targets {first}..{first + count - 1}) x {features} features "
                f"at goal block column {at}")

    def _mirror_place_spec(self) -> None:
        """The host copy of place_spec (a rollout graph cannot read the device while it is captured)."""
        at, first, count, features = (int(v) for v in self.place_spec.tolist())
        on = self.pointer is not None and at >= 0 and count > 0 and features == PLACE_FEATURES
        self.place_at, self.place_first, self.place_count = (at, first, count) if on else (-1, -1, 0)

    @property
    def pointer_on(self) -> bool:
        return self.place_count > 0

    def place_scores(self, obs: torch.Tensor, layout: torch.Tensor, dtype: torch.dtype) -> torch.Tensor | None:
        """The pointer's score per place slot, [rows, slots]: the slot's features read off the goal block, zero for a
        row whose layout has no goal block. None without a pointer. No branch on data: the row is a mask, so the
        rollout graph captures it."""
        if not self.pointer_on:
            return None
        obs = obs.reshape(-1, obs.shape[-1])
        at = self.block_at[layout.reshape(-1).long()]
        has = at >= 0
        span = self.place_count * PLACE_FEATURES
        columns = (at.clamp(min=0) + self.place_at)[:, None] + torch.arange(span, device=obs.device)[None, :]
        columns = columns.clamp(max=obs.shape[-1] - 1)
        values = obs.gather(1, columns).reshape(-1, self.place_count, PLACE_FEATURES)
        values = values * has[:, None, None].to(values.dtype)
        weight = self.pointer[0].weight
        return self.pointer(values.to(weight.dtype)).squeeze(-1).to(dtype)

    def _with_places(self, joint: torch.Tensor, obs: torch.Tensor | None, layout: torch.Tensor | None) -> torch.Tensor:
        """`joint` [rows, kinds, targets] with the pointer's score added to each place target's logit. The score is the
        target's, so it is added to every kind alike; the kinds that cannot take a place (stage.json goals.accepts) are
        masked out of the choice, which leaves the pointer on the two that can: search (the seek stage's room and cell
        goals, goal block revision 5) and travel_to (a dungeon's places)."""
        if obs is None or layout is None:
            return joint
        scores = self.place_scores(obs, layout, joint.dtype)
        if scores is None:
            return joint
        padded = nn.functional.pad(scores, (self.place_first, self.targets - self.place_first - self.place_count))
        return joint + padded[:, None, :]

    def _block(self, obs: torch.Tensor, layout: torch.Tensor):
        """The goal block's columns of flat rows, and which rows have one."""
        at = self.block_at[layout.long()]
        has = at >= 0
        columns = at.clamp(min=0)[:, None] + torch.arange(self.block_width, device=obs.device)[None, :]
        columns = columns.clamp(max=obs.shape[-1] - 1)
        return obs.gather(1, columns) > 0.5, has

    def signals(self, obs: torch.Tensor, layout: torch.Tensor) -> dict[str, torch.Tensor]:
        """What the goal block says of the goals held, per flat row: the primary ended, the secondary ended, an
        event (choose again now) and what was achieved this decision (achieved, -1 for nothing)."""
        obs = obs.reshape(-1, obs.shape[-1])
        block, has = self._block(obs, layout.reshape(-1))
        rows = obs.shape[0]
        k, t = self.kinds, self.targets
        base = k + t
        out = {"ended": block[:, base] & has}
        if self.slots <= 1:
            none = torch.zeros(rows, dtype=torch.bool, device=obs.device)
            out.update(secondary_ended=none, event=none,
                       achieved=torch.full((rows,), -1, dtype=torch.long, device=obs.device))
            return out
        c = self.columns
        out["secondary_ended"] = block[:, c["secondary_ended"]] & has
        out["event"] = block[:, c["event"]] & has
        achieved_kind = block[:, c["achieved_kind"]:c["achieved_kind"] + k]
        achieved_target = block[:, c["achieved_target"]:c["achieved_target"] + t].float().argmax(-1)
        out["achieved"] = torch.where(achieved_kind.any(-1) & has,
                                      achieved_kind.float().argmax(-1) * t + achieved_target,
                                      torch.full_like(achieved_target, -1))
        return out

    def _shifted(self, features: torch.Tensor, slot: int, drawn: list[torch.Tensor]) -> torch.Tensor:
        """The vector a slot after the primary scores from: the features plus the slot's bias and the embedding of
        each goal drawn before it."""
        shifted = features + self.slot_bias[slot - 1].to(features.dtype)
        for goal in drawn:
            shifted = shifted + self.drawn(goal.long() + 1).to(features.dtype)
        return shifted

    def slot_logits(self, features: torch.Tensor, slot: int, drawn: list[torch.Tensor],
                    obs: torch.Tensor | None = None, layout: torch.Tensor | None = None,
                    cell_ok: torch.Tensor | None = None) -> torch.Tensor:
        """Masked logits [rows, count + 1] of a slot after the primary, the last column being none: the shared
        parameters over the features plus the slot's bias and the embedding of what was drawn before it. The
        secondary and the queue are masked by what the goal block says is there, as the primary is: a queued goal
        about something absent was a draw from hundreds of goals that could never be pursued, and its entropy kept
        the whole head near uniform (next-run trial, 2026-09-30). `cell_ok` [rows] (with a cell head): whether a block
        is left for a cell goal after the cells drawn before this slot (the draw's own de-duplication); the cell goal
        is withdrawn where none is, so a plan never holds a cell the sim would find invalid."""
        shifted = self._shifted(features, slot, drawn)
        joint = self.kind(shifted)[:, :, None] + self.pair[None].to(features.dtype)
        if self.target is not None:
            joint = joint + self.target(shifted)[:, None, :]
        joint = self._with_places(joint, obs, layout)
        allowed = self.accepts[None].expand(features.shape[0], -1, -1)
        if obs is not None and layout is not None:
            block, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            present = block[:, : self.kinds, None] & block[:, self.kinds : self.kinds + self.targets][:, None, :]
            allowed = torch.where(has[:, None, None], allowed & present, allowed)
        allowed = allowed.reshape(features.shape[0], -1)
        goals = torch.arange(allowed.shape[-1], device=allowed.device)[None, :]
        if slot >= 2:
            # A queued goal is not one already drawn at this choice (the primary, the secondary, the queue so far): a
            # repeat of the primary ended lost on promotion, and a repeat in the queue was a wasted place. Not the cell
            # goal, which every cell shares (the cells' own de-duplication is the draw's, by block).
            for goal in drawn:
                repeat = (goal.long()[:, None] == goals) & (goal[:, None] >= 0)
                if self.cell_joint >= 0:
                    repeat = repeat & (goals != self.cell_joint)
                allowed = allowed & ~repeat
        if cell_ok is not None and self.cell_joint >= 0:
            allowed = allowed & ~((goals == self.cell_joint) & ~cell_ok[:, None])
        none = self.none_bias[slot - 1].to(features.dtype).expand(features.shape[0], 1)
        logits = torch.cat([masked_logits(joint.reshape(features.shape[0], -1), allowed), none], dim=-1)
        # A row whose layout has no goal block has only none.
        if obs is not None and layout is not None and self.has_space:
            _, has = self._block(obs.reshape(-1, obs.shape[-1]), layout.reshape(-1))
            only_none = torch.zeros_like(logits, dtype=torch.bool)
            only_none[:, -1] = True
            logits = torch.where(has[:, None], logits, logits.masked_fill(~only_none, MASKED_LOGIT))
        return logits

    def cell_inputs(self, image: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """What every slot's cell scores share, once per row, from the image bytes [rows, I] (the camera's pixels, then
        the mental map's crop -- its fine tier first, where the map has two): (the blocks' hidden features [rows, B,
        CELL_HIDDEN], the choosable mask [rows, B]), B = grid x grid."""
        side = self.cell_grid * self.cell_pool
        crops = image.reshape(image.shape[0], -1)[:, self.cell_at:self.cell_at + side * side * MAP_CHANNELS]
        x, valid = cell_features(crops, self.cell_grid, self.cell_pool, self.cell_rise, self.cell_min_floor)
        weight = self.cell.local.weight
        return nn.functional.silu(self.cell.local(x.to(weight.dtype))), valid

    def cell_scores(self, hidden: torch.Tensor, shifted: torch.Tensor, prev_row: torch.Tensor,
                    prev_col: torch.Tensor, bonus: torch.Tensor | None = None) -> torch.Tensor:
        """A slot's score per block [rows, B]: the score of tanh(the block's hidden features + the slot's query), plus
        the chain feature -- the block's offset (rows, columns, distance, over CELL_OFFSET_SCALE blocks) from the
        previous cell of the plan, `prev_row`, `prev_col` [rows] floats (the seat's own block at the start) -- plus,
        with the planner (`bonus` [rows, B]: each block's pooled value under the VIN, MapEncoder.plan), that value
        times the pointer's planner_weight. A bonus of another grid is ignored."""
        weight = self.cell.query.weight
        query = self.cell.query(shifted.to(weight.dtype))
        score = self.cell.score(torch.tanh(hidden + query[:, None, :])).squeeze(-1)
        grid = self.cell_grid
        index = torch.arange(grid * grid, device=hidden.device)
        rows = torch.div(index, grid, rounding_mode="floor").to(hidden.dtype)
        columns = (index % grid).to(hidden.dtype)
        d_row = (rows[None, :] - prev_row[:, None].to(hidden.dtype)) / CELL_OFFSET_SCALE
        d_col = (columns[None, :] - prev_col[:, None].to(hidden.dtype)) / CELL_OFFSET_SCALE
        offset = torch.stack([d_row, d_col, torch.sqrt(d_row * d_row + d_col * d_col)], dim=-1)
        score = score + self.cell.chain(offset).squeeze(-1)
        if bonus is not None and bonus.shape[-1] == grid * grid:
            score = score + self.cell.planner_weight.to(score.dtype) * bonus.detach().to(score.dtype)
        return score

    def cell_primary(self, features: torch.Tensor, image: torch.Tensor, block: torch.Tensor,
                     bonus: torch.Tensor | None = None):
        """The primary slot's cell distribution scored again (the planner's hindsight and the update's statistics):
        (log probability of `block` [rows], whether that block is choosable, the distribution's entropy, whether any
        block is choosable). The slot is the first, so it scores from `features` as it was drawn, from the seat's own
        block, with every choosable block allowed. `bonus` as cell_scores'."""
        hidden, valid = self.cell_inputs(image)
        seat = torch.full((features.shape[0],), (self.cell_grid - 1) / 2.0, dtype=hidden.dtype, device=hidden.device)
        logits = masked_logits(self.cell_scores(hidden, features, seat, seat, bonus).to(features.dtype), valid)
        block = block.long().clamp(0, valid.shape[-1] - 1)
        return (log_prob_of(logits, block), valid.gather(1, block[:, None])[:, 0], _entropy(logits),
                valid.any(dim=-1))

    def cell_primary_many(self, features: torch.Tensor, image: torch.Tensor, blocks: torch.Tensor,
                          bonus: torch.Tensor | None = None):
        """cell_primary for several blocks per row (the planner's lookback hindsight): `blocks` [rows, M] scored
        against ONE pass of each row's cell distribution. (log probabilities [rows, M], whether each is choosable
        [rows, M], the entropy [rows], whether any block is choosable [rows])."""
        hidden, valid = self.cell_inputs(image)
        seat = torch.full((features.shape[0],), (self.cell_grid - 1) / 2.0, dtype=hidden.dtype, device=hidden.device)
        logits = masked_logits(self.cell_scores(hidden, features, seat, seat, bonus).to(features.dtype), valid)
        blocks = blocks.long().clamp(0, valid.shape[-1] - 1)
        log_probs = logits.gather(1, blocks) - torch.logsumexp(logits, dim=-1, keepdim=True)
        return log_probs, valid.gather(1, blocks), _entropy(logits), valid.any(dim=-1)

    def draw(self, features: torch.Tensor, obs: torch.Tensor, layout: torch.Tensor, deterministic: bool,
             slots: torch.Tensor | None = None, uniform: bool = False, cells: torch.Tensor | None = None,
             image: torch.Tensor | None = None, uniform_cells: bool = False, single: bool = False,
             bonus: torch.Tensor | None = None):
        """The slots drawn at a choice, or scored when `slots` [rows, S] (and `cells`) is given: (slots [rows, S] with -1
        for none, log probability, entropy, cells [rows, S] -- the block each slot named, -1 where its goal is not the
        cell goal). `uniform` (the eval arm random_goal): the primary is drawn uniformly over the goals on offer and the
        cell uniformly over the choosable blocks, and nothing is held beside it or queued. `uniform_cells`
        (random_cell): the cells are drawn uniformly, the goals from the head. `single` (no_plan): nothing is held
        beside the primary or queued, the primary from the head. `bonus` [rows, B]: the planner's pooled value per
        block (cell_scores), with a planner.

        With a cell head (and the image bytes): a slot whose goal is the cell goal also draws a block, from the blocks
        the crop says are choosable (cell_valid) less every block within one of a cell drawn before it in this choice,
        scored by the slot's query and by the offset from the previous cell of the plan (CellPointer); the joint and the
        block both count in the log probability, and the block in the entropy as `cell_entropy_weight` times its
        entropy where the cell goal is drawn."""
        cell_on = self.cell_on and image is not None
        primary_logits = self.logits(features, obs, layout)
        if uniform:
            primary_logits = uniform_logits(primary_logits)
        if slots is None:
            primary, primary_lp = sample_logits(primary_logits, deterministic)
        else:
            primary = slots[:, 0].clamp(min=0)
            primary_lp = log_prob_of(primary_logits, primary)
        log_prob = primary_lp
        entropy = _entropy(primary_logits)
        drawn, out = [primary], [primary]
        none = self.count
        rows = features.shape[0]
        out_cells = [torch.full_like(primary, -1)]

        if cell_on:
            hidden, valid = self.cell_inputs(image)
            grid = self.cell_grid
            index = torch.arange(grid * grid, device=features.device)
            block_row, block_col = torch.div(index, grid, rounding_mode="floor"), index % grid
            seat = (grid - 1) / 2.0
            previous = [torch.full((rows,), seat, dtype=features.dtype, device=features.device),
                        torch.full((rows,), seat, dtype=features.dtype, device=features.device)]
            earlier: list[tuple[torch.Tensor, torch.Tensor, torch.Tensor]] = []

            def allowed_blocks() -> torch.Tensor:
                allowed = valid
                for row, column, is_cell in earlier:
                    near = ((block_row[None, :] - row[:, None]).abs() <= 1) \
                        & ((block_col[None, :] - column[:, None]).abs() <= 1) & is_cell[:, None]
                    allowed = allowed & ~near
                return allowed

            def draw_cell(shifted: torch.Tensor, allowed: torch.Tensor, goal: torch.Tensor, slot: int):
                """(the block a cell goal names, -1 for another goal; its log probability; its entropy)."""
                logits = masked_logits(self.cell_scores(hidden, shifted, previous[0], previous[1],
                                                        bonus).to(features.dtype), allowed)
                if uniform or uniform_cells:
                    logits = uniform_logits(logits)
                if cells is None:
                    block, lp = sample_logits(logits, deterministic)
                else:
                    block = cells[:, slot].clamp(min=0)
                    lp = log_prob_of(logits, block)
                is_cell = goal == self.cell_joint
                row = torch.div(block, grid, rounding_mode="floor")
                column = block - row * grid
                earlier.append((row, column, is_cell))
                previous[0] = torch.where(is_cell, row.to(features.dtype), previous[0])
                previous[1] = torch.where(is_cell, column.to(features.dtype), previous[1])
                return (torch.where(is_cell, block, torch.full_like(block, -1)),
                        torch.where(is_cell, lp, torch.zeros_like(lp)), _entropy(logits))

            block, lp, cell_entropy = draw_cell(features, allowed_blocks(), primary, 0)
            out_cells[0] = block
            log_prob = log_prob + lp
            share = torch.softmax(primary_logits, dim=-1)[:, self.cell_joint]
            entropy = entropy + share * self.cell_entropy_weight * cell_entropy

        for slot in range(1, self.slots):
            if cell_on:
                allowed = allowed_blocks()
                logits = self.slot_logits(features, slot, drawn, obs, layout, allowed.any(dim=-1))
            else:
                logits = self.slot_logits(features, slot, drawn, obs, layout)
            if uniform or single:
                logits = none_only_logits(logits)
            if slots is None:
                choice, lp = sample_logits(logits, deterministic)
            else:
                choice = torch.where(slots[:, slot] < 0, torch.full_like(slots[:, slot], none), slots[:, slot])
                lp = log_prob_of(logits, choice)
            log_prob = log_prob + lp
            # The primary is the plan; the rest beside it are weighed at slot_entropy_weight, so the entropy bonus
            # does not grow with the number of slots and keep the primary near uniform.
            slot_entropy = _entropy(logits)
            goal = torch.where(choice == none, torch.full_like(choice, -1), choice)
            if cell_on:
                block, lp_cell, cell_entropy = draw_cell(self._shifted(features, slot, drawn), allowed, goal, slot)
                out_cells.append(block)
                log_prob = log_prob + lp_cell
                slot_entropy = slot_entropy + torch.softmax(logits, dim=-1)[:, self.cell_joint] \
                    * self.cell_entropy_weight * cell_entropy
            else:
                out_cells.append(torch.full_like(goal, -1))
            entropy = entropy + self.slot_entropy_weight * slot_entropy
            drawn.append(goal)
            out.append(goal)
        return torch.stack(out, dim=-1), log_prob, entropy, torch.stack(out_cells, dim=-1)

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
        joint = self._with_places(joint, obs, layout)
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
        # Always something to choose: the first goal (Fight about no one in particular) is allowed when nothing else
        # is on offer. A stage whose goal block always offers it (every stage but M2's room goals) is unchanged; M2
        # withdraws it once a place exists, so the head has to choose a place.
        allowed[:, 0] = allowed[:, 0] | ~allowed.any(dim=-1)
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
    """Zero every seat layout's camera columns (VisionEncoder), the director's set columns and the teacher's hint
    columns again (after a seed or a load brought weights from elsewhere)."""
    with torch.no_grad():
        for index in range(len(network.adapters)):
            for tag in ("vision", "director", "hint"):
                keep = getattr(network, f"{tag}_keep_{index}", None)
                if keep is not None:
                    network.adapters[index].weight.mul_(keep)


#: The vision block's name in stage.json (BlockId::Vision); its manifest entry carries the camera image's shape.
VISION_BLOCK = "vision"


#: The camera's bytes a pixel (protocol 26, Vision::BYTES_PER_PIXEL): distance, height, normal and the class byte (the
#: semantic class and the objective bit), of the static world alone (vision block revision 6: units and objects are
#: in the entity list, not the image).
IMAGE_BYTES_PER_PIXEL = 4
#: The decoded image channels, as the sim's Vision::DecodePixel lays them out: the class is channel 3 (an index, which
#: the encoder reads as one-hot planes over the manifest's pixel_classes), the objective channel 4.
IMAGE_CHANNELS = 5
IMAGE_CLASS_CHANNEL = 3
#: The class byte's bits (Camera.h): the class in the low five, the objective in bit 5.
CLASS_MASK = 0x1F
#: The classes the wire can carry (5 bits): the size of the class embedding's table, so a class added later needs no
#: new shape.
CLASS_LIMIT = 32
#: The encoder's patch size when the manifest names none (revision 3 and before: 4 x 4 at 64 x 32).
DEFAULT_PATCH = 4
#: The camera's geometry as the sim casts it (Camera.h, VisionCaster.cpp), read by the camera splat (CameraSplat):
#: the distance byte b is ln(max(d, NEAR) / NEAR) / ln(REFERENCE / NEAR) x 254 (255 sky), so d = NEAR x (REFERENCE /
#: NEAR) ^ (b / 254); the field of view when the manifest's "camera" names none (Settings::FovH, FovV, degrees); the
#: vision block's scalars the pose is read from (Vision::Scalar: the yaw offset's sin and cos, the pitch / (pi / 2), the
#: boom -- the camera's distance behind the pivot after the pull-in -- / ZOOM_SCALE, the cast width / the canonical
#: width); and ZOOM_SCALE itself.
IMAGE_NEAR = 0.25
IMAGE_DISTANCE_REFERENCE = 1000.0
IMAGE_FOV_H = 120.0
IMAGE_FOV_V = 60.0
VISION_SCALAR_YAW_SIN = 0
VISION_SCALAR_YAW_COS = 1
VISION_SCALAR_PITCH = 2
VISION_SCALAR_BOOM = 4
VISION_SCALAR_RENDER_WIDTH = 10
VISION_ZOOM_SCALE = 12.0
#: The entity list's block in stage.json (BlockId::Entities), which comes with a camera.
ENTITIES_BLOCK = "entities"
#: The sight block in stage.json (BlockId::Sight, dungeon-curriculum I1 and I2): the visible and remembered entities
#: as one list, and the pointer presses on it.
SIGHT_BLOCK = "sight"
#: The remembered entities in stage.json (BlockId::MemoryEntities, object memory 2026-10-18): the seat's object memory's
#: most relevant entries not in view, each slot the entity list's columns then the memory's (MemoryEntities).
MEMORY_ENTITIES_BLOCK = "memory_entities"
#: The mental map's block in stage.json (BlockId::Map, perception-goals REDESIGN §3): its manifest's "map" describes
#: the crop that travels as bytes after the image (protocol 24).
MAP_BLOCK = "map"
#: The coarse coverage block in stage.json (BlockId::Coverage, general search sec 5): a heading-up, egocentric grid of
#: map-tile counters (known, visited, searched cells of each 32-yd tile, each over the manifest's scale), read raw by
#: CoverageEncoder; its manifest's "coverage" describes the grid.
COVERAGE_BLOCK = "coverage"
COVERAGE_LAYOUT = "row_col_channel"
#: The crop's byte channels (Vision::CropChannel), and the codes (Vision::MapCode: unknown, floor, wall, door, hazard).
#: Exploration v3 added the seventh, MAP_SEARCHED: how many of the crop cell's four 1-yd cells were looked at THIS
#: episode (0..MAP_SEARCHED_MAX), where the other channels outlive an episode on a kept map. Map block revision 2.
#: Map block revision 6 (2026-10-17, the memory's sealed corridors) added the eighth, MAP_WALLS: how many of the crop
#: cell's four 1-yd cells are wall (0..MAP_WALLS_MAX on the fine tier; the coarse tier's count scaled to the same
#: range), since the code byte now calls a cell WALL only by majority (3 or 4 of its 1-yd cells) -- the old any-wall
#: rule read a corridor's edge cells as wall and sealed it, so the map's route ran 2-3 x the true one.
MAP_CHANNELS = 8
MAP_CODES = 5
MAP_CODE, MAP_HEIGHT, MAP_VISITED, MAP_AGE, MAP_CLASS, MAP_FRONTIER, MAP_SEARCHED, MAP_WALLS = range(MAP_CHANNELS)
MAP_SEARCHED_MAX = 4
MAP_WALLS_MAX = 4
#: The first map block revision whose crop has the walls channel and whose route scalars are the seventeen of
#: ROUTE_COLUMNS: an older manifest is refused, as a re-laid block is (revision 6 carried eight route scalars, which
#: this reader cannot lay over the seventeen).
MAP_BLOCK_REVISION = 7
#: The planes decode_map adds to the code and class embeddings: height, known, visited, age, frontier, searched, walls.
MAP_VALUE_PLANES = 7
#: **The sim's route** (mappo.planner_waypoint_source "sim"; map block revision 7): the sim plans the route on the
#: bot's own map at 1 yd every decision and appends these seventeen scalars to the map block's, in this order:
#: the fine path's points at 5, 10, 20 and 30 yd along the 1-yd plan's descent (bearing sin = -right / d, cos =
#: forward / d each; a later point repeats the walk's end where it ended early), `route_fine_reach` (the yards walked
#: / 30) and `route_fine_valid`; the coarse waypoint's four as revision 6 had them (sin, cos, distance over its reach,
#: valid); `route_dist` (min(the own-map cost-to-go at the body, 300) / 300) and `route_dist_valid`; `route_stall`
#: (seconds since that cost-to-go last set a new episode minimum, / 30, clamped to 1). All 0 where there is no route.
#: Located by name (route_of): the map block's obs_names, else its manifest's scalar_names at the block's columns; they
#: have to be the block's last seventeen, which the map's embedding does not read (MapEncoder).
ROUTE_COLUMNS = ("route_p5_sin", "route_p5_cos", "route_p10_sin", "route_p10_cos", "route_p20_sin", "route_p20_cos",
                 "route_p30_sin", "route_p30_cos", "route_fine_reach", "route_fine_valid",
                 "route_coarse_sin", "route_coarse_cos", "route_coarse_dist", "route_coarse_valid",
                 "route_dist", "route_dist_valid", "route_stall")
#: ROUTE_COLUMNS by group, as MapEncoder.sim_waypoints scales them: (the columns, the valid flag that gates them; None
#: for a column read as it is).
ROUTE_GROUPS = ((tuple(range(0, 10)), 9), (tuple(range(10, 14)), 13), ((14,), 15), ((15,), None), ((16,), None))
#: **The school's goal** (mappo.planner_waypoint_source "school"; MapEncoder.school_route; planner school stage 2):
#: two map-block scalars the sim writes immediately before the route's seventeen -- the objective's forward and right
#: yards in the body frame, the labels' goal_forward / goal_right convention (forward = dx cos yaw + dy sin yaw, right
#: = dx sin yaw - dy cos yaw; 0 and 0 with no objective or one withheld, so a goal is present where either is not 0).
#: They are the school planner's goal plane and nothing else's: the map's embedding never reads them (embed_scalars
#: leaves them out as it does the route's, whatever the source), and the VIN's goal plane reads the compass alone
#: (MapEncoder.goal) -- on a nav stage the straight line to the goal reaches the policy only as the school's route.
#: Located by name (goal_of); optional: a block without them refuses the school source and nothing else.
GOAL_COLUMNS = ("goal_forward_yards", "goal_right_yards")
#: The map block's kept scalar (MapBlock OBS_KEPT: 1 when this episode's map was kept from the one before), by name:
#: the memory flag the school planner reads at act time (the recorder's `kept` field; the shard's `memory` label
#: prefers the episode's shared_memory column, which no observation carries). -1 without it (the flag is then 0).
KEPT_COLUMN = "kept"
#: **The school's route rule** (MapEncoder.school_route, route_from_fields; mappo.planner_waypoint_source "school"):
#: the seventeen of ROUTE_COLUMNS computed from the school planner's fields on the 2-yd crop as the sim computes them
#: on its 1-yd plan. The fine walk: networks.descend's rule from the anchor over the fine cost-to-go, every cell the
#: reachable head rejects (logit at or below 0) at CLASSICAL_SENTINEL (school.metrics.walk_field), ROUTE_FINE_YARDS /
#: cell = 15 moves at most (a move is a cell, a diagonal one counts one, as the sim's 1-yd steps do), starting from
#: the one of the centre four cells (the body lies on their corner) with the least field value, the first in
#: row-major order of equals (the anchor itself when the four tie) -- the sim starts on the body's own 1-yd cell, or
#: the least round it when that is blocked; the point at Y yards is the walk's polyline of cell centres at Y / cell
#: moves -- 2.5, 5, 10 and 15: the 5-yd point is the midpoint of the cells after the second and third moves, the
#: others cells reached -- and a walk that stopped earlier repeats its end cell; sin = -right / d, cos = forward / d
#: from the body (the crop's centre, 1 yd off any cell centre); reach = moves made x cell / ROUTE_FINE_YARDS; valid
#: where it moved and a goal is present. The coarse waypoint: ROUTE_COARSE_STEPS moves from the anchor on the coarse
#: field (gated by its reachable head too; the sim's CoarseWaypoint starts there), dist over steps x 8 yd, valid where
#: it moved. route_dist: the least fine cost-to-go over the centre four cells the head calls reachable (the walk's
#: start's), min(., ROUTE_DIST_YARDS) / ROUTE_DIST_YARDS, valid where one of the four is reachable and a goal is
#: present. route_stall is the sim's column as it is (the bot's own clock; the school cannot know it). Each group
#: times its valid flag as sim_waypoints gates the sim's.
ROUTE_POINT_YARDS = (5.0, 10.0, 20.0, 30.0)
ROUTE_FINE_YARDS = 30.0
ROUTE_COARSE_STEPS = 2
ROUTE_DIST_YARDS = 300.0
MAP_HEIGHT_ZERO = 128
#: The height byte's step (Vision::CROP_HEIGHT_STEP): the floor nearest the feet is at (byte - MAP_HEIGHT_ZERO) x
#: MAP_HEIGHT_STEP yards over them, byte 0 no floor known; so the byte reaches +/- 31.75 yd and clamps there.
MAP_HEIGHT_STEP = 0.25
MAP_FLOOR, MAP_WALL, MAP_DOOR, MAP_HAZARD = 1, 2, 3, 4      # the crop codes (Vision::MapCode; 0 unknown)


def _map_of(entry: dict, name: str) -> dict | None:
    """A layout's mental map as its map block describes it -- {"first" (its scalar columns), "scalars", "height",
    "width", "cell", "channels", "map_bytes", "codes", "classes", "route" (route_of: the sim's waypoint columns, or
    None), "embed_scalars" (the scalars the map's embedding reads: all of them but the route's)} -- or None without
    one. Refused: a crop this learner does not decode (its channels or codes differ, or its bytes are not height x
    width x channels), or a map block before MAP_BLOCK_REVISION (the walls channel re-laid the crop's cells: the
    manifest and the learner are from different trees, as a re-laid block is refused)."""
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
    revision = int(block.get("revision", 0))
    if revision < MAP_BLOCK_REVISION:
        raise ValueError(f"{name}: the map block is revision {revision}, this learner reads revision "
                         f"{MAP_BLOCK_REVISION} on (the crop's walls channel and the route scalars; the manifest and "
                         f"the learner are from different trees: rebuild the sim at this learner's tree)")
    # The crop's tiers (map block revision 5, vision movement curriculum CONTRACT.md sec 3.2): the manifest's
    # `tiers: [{cell: 2}, {cell: 8}]` says the crop section is [fine][coarse], each height x width x channels bytes
    # (`crop_bytes`); without the key there is one tier. `map_bytes` is the whole section (what the SPEC announces
    # and the camera's row carries), whichever the manifest wrote.
    tiers = list(described.get("tiers") or ())
    out["tier_cells"] = tuple(float(tier.get("cell", out["cell"])) if isinstance(tier, dict) else float(tier)
                              for tier in tiers) or (out["cell"],)
    out["tiers"] = len(out["tier_cells"])
    out["crop_bytes"] = out["height"] * out["width"] * out["channels"]
    if (out["channels"], out["codes"]) != (MAP_CHANNELS, MAP_CODES) or not 1 <= out["classes"] <= CLASS_LIMIT \
            or out["map_bytes"] not in (out["crop_bytes"], out["crop_bytes"] * out["tiers"]):
        raise ValueError(f"{name}: a map crop of {out['height']} x {out['width']} x {out['channels']} channels "
                         f"({out['map_bytes']} bytes, {out['codes']} codes, {out['classes']} classes, {out['tiers']} "
                         f"tier(s)); this learner decodes {MAP_CHANNELS} channels and {MAP_CODES} codes, height x "
                         f"width x channels bytes a tier")
    out["map_bytes"] = out["crop_bytes"] * out["tiers"]
    # Exploration v3 (map block revision 2): the manifest names the searched channel; one without the key (a sim of the
    # six-channel map) is refused above on its channel count, so the key is only cross-checked when it is there.
    if "searched_channel" in described and int(described["searched_channel"]) != MAP_SEARCHED:
        raise ValueError(f"{name}: the map's searched channel is {described['searched_channel']}, this learner reads "
                         f"channel {MAP_SEARCHED} (the manifest and the learner are from different trees)")
    # Map block revision 6: the walls channel, likewise cross-checked when named.
    if "walls_channel" in described and int(described["walls_channel"]) != MAP_WALLS:
        raise ValueError(f"{name}: the map's walls channel is {described['walls_channel']}, this learner reads "
                         f"channel {MAP_WALLS} (the manifest and the learner are from different trees)")
    if int(described.get("walls_max", MAP_WALLS_MAX)) != MAP_WALLS_MAX:
        raise ValueError(f"{name}: the map's walls count runs to {described['walls_max']}, this learner scales by "
                         f"{MAP_WALLS_MAX}")
    if int(block["obs"][1]) != out["scalars"]:
        raise ValueError(f"{name}: the map block is {block['obs'][1]} columns, its scalars {out['scalars']}")
    # The sim's route (ROUTE_COLUMNS): the block's last seventeen columns, which the map's embedding leaves out (they
    # feed the policy through the waypoint's join alone, under mappo.planner_waypoint_source "sim").
    out["route"] = route_of(block, name)
    # The school's goal (GOAL_COLUMNS): the two columns before the route's, likewise left out of the embedding
    # whatever the source; and the kept scalar's column (KEPT_COLUMN), the school's memory flag.
    out["goal"] = goal_of(block, name, out["route"])
    out["kept"] = kept_of(block)
    out["embed_scalars"] = out["scalars"] - (len(ROUTE_COLUMNS) if out["route"] is not None else 0) \
        - (len(GOAL_COLUMNS) if out["goal"] is not None else 0)
    return out


def _block_names(block: dict) -> list[str]:
    """A map block's column names: its obs_names, else its manifest's scalar_names."""
    return list(block.get("obs_names") or (block.get("map") or {}).get("scalar_names") or ())


def goal_of(block: dict, name: str, route: dict | None) -> dict | None:
    """The school's goal columns of a map block (GOAL_COLUMNS) -- {"columns": [forward, right], their absolute
    observation columns} -- or None when the block names neither. Refused: one of the two without the other, the two
    without the route's seventeen, or the two not being the pair immediately before the route's first (the embedding
    reads the block's prefix, embed_scalars: the goal and the route have to be its tail, in that order)."""
    names = _block_names(block)
    found = [column for column in GOAL_COLUMNS if column in names]
    if not found:
        return None
    if len(found) != len(GOAL_COLUMNS):
        raise ValueError(f"{name}: the map block names {found} of the goal columns {list(GOAL_COLUMNS)}")
    if route is None:
        raise ValueError(f"{name}: the map block names the goal columns {list(GOAL_COLUMNS)} but not the route's "
                         f"{list(ROUTE_COLUMNS)}")
    first = int(block["obs"][0])
    columns = [first + names.index(column) for column in GOAL_COLUMNS]
    expected = [route["first"] - len(GOAL_COLUMNS), route["first"] - 1]
    if columns != expected:
        raise ValueError(f"{name}: the goal columns are at {columns}, not the two before the route's first "
                         f"({expected}) in {list(GOAL_COLUMNS)} order")
    return {"columns": columns}


def kept_of(block: dict) -> int:
    """The absolute observation column of a map block's kept scalar (KEPT_COLUMN), -1 without one."""
    names = _block_names(block)
    return int(block["obs"][0]) + names.index(KEPT_COLUMN) if KEPT_COLUMN in names else -1


def route_of(block: dict, name: str) -> dict | None:
    """The sim's route columns of a map block (ROUTE_COLUMNS; map block revision 7) -- {"columns": their absolute
    observation columns in ROUTE_COLUMNS order, "first": the first of them} -- or None when the block names none of
    them (an older manifest, refused by _map_of on its revision anyway). Named by the block's obs_names, else by its
    manifest's scalar_names at the block's columns. Refused: some of the names but not all (revision 6's eight among
    them), or the seventeen not being the block's last seventeen in order (the embedding reads the block's prefix; the
    exported model's runtime reads the seventeen from the first one on)."""
    names = list(block.get("obs_names") or (block.get("map") or {}).get("scalar_names") or ())
    found = [column for column in ROUTE_COLUMNS if column in names]
    if not found:
        return None
    if len(found) != len(ROUTE_COLUMNS):
        raise ValueError(f"{name}: the map block names {found} of the route columns {list(ROUTE_COLUMNS)}")
    first = int(block["obs"][0])
    columns = [first + names.index(column) for column in ROUTE_COLUMNS]
    width = int(block["obs"][1])
    if columns != list(range(first + width - len(ROUTE_COLUMNS), first + width)):
        raise ValueError(f"{name}: the route columns are at {columns}, not the map block's last {len(ROUTE_COLUMNS)} "
                         f"({first} + {width - len(ROUTE_COLUMNS)} on) in ROUTE_COLUMNS order")
    return {"columns": columns, "first": columns[0]}


#: The compass block in stage.json (BlockId::Compass): the objective's bearing and distance off the body, M1's minimap
#: marker (vision movement curriculum, CONTRACT.md sec 0: the planner's goal plane reads it where a layout has it).
#: The nav stages have no compass block since 2026-10-17 (the route on the bot's own map is the goal's only trace in
#: the observation, ROUTE_COLUMNS): every reader of it -- compass_of, MapEncoder.goal, the export's header, the eval
#: trace's objective fields -- takes its absence as "no goal in the observation", never refuses a layout without it.
COMPASS_BLOCK = "compass"
COMPASS_COLUMNS = ("objective", "objective_bearing_sin", "objective_bearing_cos", "objective_distance")
COMPASS_SCALE = 500.0


def compass_of(entry: dict, name: str) -> dict | None:
    """A layout's compass block as stage.json describes it -- {"first": its first observation column, "present",
    "sin", "cos", "dist": the absolute columns of the objective's presence, bearing sin and cos and distance over
    "scale" yards} -- or None without one (or without the obs_names that locate the columns)."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == COMPASS_BLOCK), None)
    if block is None:
        return None
    names = list(block.get("obs_names") or ())
    if any(column not in names for column in COMPASS_COLUMNS):
        return None
    first = int(block["obs"][0])
    present, sin, cos, dist = (first + names.index(column) for column in COMPASS_COLUMNS)
    return {"first": first, "present": present, "sin": sin, "cos": cos, "dist": dist,
            "scale": float(block.get("objective_scale", COMPASS_SCALE))}


def coverage_of(entry: dict, name: str) -> dict | None:
    """A layout's coarse coverage block as stage.json describes it (general search sec 5) -- {"first": its first
    observation column, "grid", "channels", "width"} -- or None without one. The block's columns are the grid's cells
    in [row][col][channel] order (the manifest's "layout"), each channel a tile counter over its scale, read raw by
    CoverageEncoder; the map block's revision 3 scalars are the map's own. Refused: a width that is not
    grid x grid x channels, or a layout other than row_col_channel; every other manifest key is optional."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == COVERAGE_BLOCK), None)
    if block is None:
        return None
    described = block.get("coverage") or {}
    width = int(block["obs"][1])
    grid, channels = int(described.get("grid", 0)), int(described.get("channels", 0))
    if grid < 1 or channels < 1 or width != grid * grid * channels:
        raise ValueError(f"{name}: the coverage block is {width} columns, its manifest a {grid} x {grid} grid of "
                         f"{channels} channels (stage.json coverage.grid / channels)")
    if described.get("layout", COVERAGE_LAYOUT) != COVERAGE_LAYOUT:
        raise ValueError(f"{name}: the coverage block's columns are laid out {described['layout']!r}; this learner "
                         f"reads {COVERAGE_LAYOUT}")
    return {"first": int(block["obs"][0]), "grid": grid, "channels": channels, "width": width}


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


def _memory_entities_of(entry: dict, name: str, entities: dict | None) -> dict | None:
    """A layout's remembered entities as its memory_entities block describes them (object memory): {"first", "slots",
    "width", "base_width", "present", "class_column", "type_column", "object_column", "memory_column", "type_buckets",
    "features"} -- or None without one. Refused: the block without the camera's entity list, or whose leading
    base_width columns are not that list's (its tokens are read with that list's encoder)."""
    block = next((b for b in entry.get("blocks", ()) if b.get("name") == MEMORY_ENTITIES_BLOCK), None)
    if block is None:
        return None
    described = block.get(MEMORY_ENTITIES_BLOCK)
    if not described:
        raise ValueError(f"{name}: stage.json has a memory_entities block without its description")
    out = {key: int(described[key]) for key in ("first", "slots", "width", "base_width", "present", "class_column",
                                                 "type_column", "object_column", "memory_column", "type_buckets")}
    out["features"] = tuple(str(f) for f in described.get("features", ()))
    first, count = int(block["obs"][0]), int(block["obs"][1])
    if out["first"] != first or out["slots"] * out["width"] != count:
        raise ValueError(f"{name}: the memory_entities block spans {count} columns from {first}, its description "
                         f"{out['slots']} slots of {out['width']} from {out['first']}")
    if entities is None:
        raise ValueError(f"{name}: a memory_entities block without the camera's entity list, whose encoder it shares")
    if (out["base_width"] != entities["width"] or out["width"] <= out["base_width"]
            or any(out[key] != entities[key] for key in ("class_column", "type_column", "object_column",
                                                         "memory_column", "type_buckets"))):
        raise ValueError(f"{name}: the remembered entities ({out}) do not lead with the entity list's columns "
                         f"({entities}): they are read with that list's encoder")
    return out


def vision_of(stage: dict | None, layout_names: Sequence[str]) -> list[dict | None] | None:
    """Per layout, in `layout_names` order, its camera as the vision block of stage.json describes it -- {"first": the
    block's first observation column (its scalars), "height", "width", "channels", "classes", "class_channel",
    "scalars", "bytes_per_pixel", "pixel_classes", "patch", "render_sizes", "look", "look_names", "image_bytes",
    "entities"} -- or None
    for a layout without one; None altogether when no layout has a camera (no encoder, nothing new in the networks).
    The block's first column differs per layout (the core block before it is as wide as the class's spells), so every
    layout keeps its own; so does its entity list ("entities", _entities_of: the entities block after the camera).

    The image travels as bytes beside the observation (protocol 21, Spec.image_bytes): the block's columns are its
    scalars alone. Revision 4 (camera-vision.FREELOOK.md) added the encoder's "patch" (4 when absent), the
    "render_sizes" the sim draws from (scaled up to the canonical height x width it sends) and the free look's heads,
    `"look": {"heads": [7, 5, 5], "names": [...]}` (() when absent: no look head). Revision 5 (perception-goals P2):
    the semantic class in place of the kind, and an entity slot as a fifth byte. Revision 6 (entity sensing): four
    bytes a pixel again, of the static world alone, and `"pixel_classes"`, the classes a pixel can carry (the
    encoder's one-hot planes, in that order). Refused: a vision block without its image, or from before revision 6
    (no pixel classes, five bytes a pixel), layouts whose images differ (one encoder reads them all), and a block
    whose width is not its scalars.

    **The camera switched off** (StageDefinition::Camera, 2026-10-22): the image object carries `"camera": false`
    (true or absent: rendered as before). The sim casts no pixels, so the STEP has no image section (Spec.image_bytes
    0) and the camera's row is the map crop alone; the block's scalars, its look (the view cone still turns), the
    entity list, map, sight and remembered entities are all as before. Recorded as "camera": False with "image_bytes"
    0; the image's keys are still read and checked (they are still written), only nothing is cut from them."""
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
        if "classes" not in image or "pixel_classes" not in image:
            raise ValueError(f"{name}: the vision block's image has no class table or no pixel classes (revision "
                             f"{block.get('revision')}, {image.get('bytes_per_pixel')} bytes a pixel): a sim from "
                             f"before revision 6 (protocol 26), which this learner does not read")
        described = {key: int(image[key]) for key in ("height", "width", "channels", "classes", "class_channel",
                                                      "scalars", "bytes_per_pixel")}
        described["patch"] = int(image.get("patch", DEFAULT_PATCH))
        described["class_limit"] = int(image.get("class_limit", CLASS_LIMIT))
        described["pixel_classes"] = tuple(int(value) for value in image["pixel_classes"])
        # Rendering on (true, or a manifest from before the switch) or off (no pixels on the wire; 2026-10-22).
        described["camera"] = image.get("camera", True) is not False
        if not described["pixel_classes"] or len(set(described["pixel_classes"])) != len(described["pixel_classes"]) \
                or any(not 0 <= value < described["class_limit"] for value in described["pixel_classes"]):
            raise ValueError(f"{name}: the vision block's pixel classes {list(described['pixel_classes'])} are not "
                             f"distinct classes of the {described['class_limit']} the wire carries")
        described["render_sizes"] = tuple(tuple(int(v) for v in size) for size in image.get("render_sizes", ()))
        # The camera it was cast with (the block's informational "camera"): the field of view and the distance
        # channel's reference, which the camera splat projects the pixels with (CameraSplat).
        rig = block.get("camera") or {}
        described["fov"] = (float(rig.get("fov_h", IMAGE_FOV_H)), float(rig.get("fov_v", IMAGE_FOV_V)))
        described["distance_reference"] = float(rig.get("distance_reference", IMAGE_DISTANCE_REFERENCE))
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
        # No pixels travel with the camera off: the row is the map crop alone (Spec.image_bytes 0).
        image_bytes = (described["height"] * described["width"] * described["bytes_per_pixel"]
                       if described["camera"] else 0)
        crop = _map_of(entry, name)
        listed = _entities_of(entry, name)
        described_layout = {"first": first, **described, "image_bytes": image_bytes, "entities": listed, "map": crop,
                            "camera_bytes": image_bytes + (crop["map_bytes"] if crop else 0),
                            "coverage": coverage_of(entry, name), "compass": compass_of(entry, name)}
        sight = _sight_of(entry, name, listed)
        if sight is not None:
            described_layout["sight"] = sight
        remembered = _memory_entities_of(entry, name, listed)
        if remembered is not None:
            described_layout["memory_entities"] = remembered
        out.append(described_layout)
    maps = [entry["map"] for entry in out if entry is not None]
    if any(crop is None for crop in maps) and any(crop is not None for crop in maps):
        raise ValueError("some layouts with a camera have a mental map and some do not: one encoder reads them all")
    grids = [entry["coverage"] for entry in out if entry is not None]
    if any(grid is None for grid in grids) and any(grid is not None for grid in grids):
        raise ValueError("some layouts with a camera have a coverage block and some do not: one encoder reads them all")
    if any(grid is not None for grid in grids) and any(crop is None for crop in maps):
        raise ValueError("a coverage block needs the mental map (the sim's Problem() rule); the manifest has one "
                         "without")
    return out if any(entry is not None for entry in out) else None


def vision_image_bytes(vision: list[dict | None] | None) -> int:
    """The bytes of an agent's camera row stage.json's camera makes, 0 without one: its image (height x width x bytes
    a pixel, the sim's Spec.image_bytes) and, with a map block, its map crop after it (Spec.map_bytes) -- the row the
    buffer keeps and the encoder reads (Spec.camera_bytes)."""
    return next((entry.get("camera_bytes", entry["image_bytes"]) for entry in vision or () if entry is not None), 0)


def vision_renders(vision: list[dict | None] | None) -> bool:
    """Whether stage.json's camera casts pixels: False without a vision block, or with its camera switched off
    (image.camera false, 2026-10-22: the block's scalars, look, entities and map without an image)."""
    return next((bool(entry.get("camera", True)) for entry in vision or () if entry is not None), False)


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


def heading_of(stage: dict | None, layout_names: Sequence[str]) -> list[bool] | None:
    """Which layouts steer by a continuous heading (HeadingHead; heading.CONTRACT.md): where stage.json's top-level
    "continuous" list names the heading, every layout with a move block. None when no layout does (no heading head)."""
    if HEADING not in continuous_of(stage):
        return None
    layouts = (stage or {}).get("layouts") or {}
    has = [any(block.get("name") == "move" for block in ((layouts.get(name) or {}).get("blocks") or ()))
           for name in layout_names]
    return has if any(has) else None


def check_image_bytes(vision: list[dict | None] | None, spec_image_bytes: int, spec_map_bytes: int = 0) -> None:
    """Refuse a sim and a stage.json that disagree about the camera: SPEC's image bytes per agent against stage.json's
    height x width x bytes a pixel, or one side with a camera and the other without; and about the mental map's crop
    (protocol 24): SPEC's map bytes against the map block's. A vision block with the camera off (image.camera false)
    expects no image bytes at all."""
    crop = next((entry.get("map") for entry in vision or () if entry is not None), None)
    expected_map = crop["map_bytes"] if crop else 0
    if expected_map != int(spec_map_bytes):
        raise ValueError(f"the sim sends {spec_map_bytes} map bytes an agent, stage.json's map block makes "
                         f"{expected_map}")
    expected = next((entry["image_bytes"] for entry in vision or () if entry is not None), 0)
    if expected == int(spec_image_bytes):
        return
    if any(entry is not None and not entry.get("camera", True) for entry in vision or ()):
        raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, but stage.json's vision block has "
                         f"the camera off (image.camera false: no pixels)")
    if not expected:
        raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, but stage.json has no camera (no "
                         f"vision block with an image)")
    if not spec_image_bytes:
        raise ValueError(f"stage.json has a camera ({expected} bytes an agent), but the sim's SPEC sends no image")
    raise ValueError(f"the sim sends {spec_image_bytes} image bytes an agent, stage.json's camera makes {expected} "
                     f"(height x width x bytes a pixel)")


def decode_image(image: torch.Tensor, height: int, width: int) -> torch.Tensor:
    """The camera's bytes [N, H x W x 4] uint8 as its five float image channels [N, H, W, 5] -- distance, height,
    normal, class, objective -- exactly as Vision::DecodePixel: 255 -> 1.0 (sky) else b / 254; (b - 128) / 125;
    b / 255; b & 31; (b >> 5) & 1. On the device: the bytes are what the rollout keeps."""
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


def decode_map(crops: torch.Tensor, height: int, width: int) -> dict[str, torch.Tensor]:
    """The mental map's crop bytes [N, H x W x 8] uint8 as the encoder reads them (Vision::DecodeCropCell): "code" and
    "class" [N, H, W] long (indexes it embeds), "height" [N, H, W] the height byte as sent, and "values" [N, H, W, 7]
    float -- the floor's height over the feet
    ((b - 128) / 127, 0 with none), whether a floor is known, visited, the newest look's age (b / 255, 1 never seen),
    the frontier, (exploration v3) the searched share of the cell's four 1-yd cells this episode (b / 4) and (map
    block revision 6) the wall share of them (b / 4: the code byte calls the cell wall by majority alone, so the
    count is where a corridor's edge shows)."""
    cells = crops.reshape(-1, height, width, MAP_CHANNELS)
    as_float = lambda value: value.to(torch.float32)
    rise = cells[..., MAP_HEIGHT]
    known = rise != 0
    return {
        "code": cells[..., MAP_CODE].long().clamp(0, MAP_CODES - 1),
        "class": (cells[..., MAP_CLASS] & CLASS_MASK).long(),
        # The height byte as sent (0 no floor; the classical planner's height band reads it, classical_factor).
        "height": rise,
        "values": torch.stack([
            torch.where(known, (as_float(rise) - MAP_HEIGHT_ZERO) / 127.0, torch.zeros_like(as_float(rise))),
            as_float(known),
            as_float(cells[..., MAP_VISITED] != 0),
            as_float(cells[..., MAP_AGE]) / 255.0,
            as_float(cells[..., MAP_FRONTIER] != 0),
            as_float(cells[..., MAP_SEARCHED]) / float(MAP_SEARCHED_MAX),
            as_float(cells[..., MAP_WALLS]) / float(MAP_WALLS_MAX),
        ], dim=-1),
    }


def decode_map_tiers(crops: torch.Tensor, height: int, width: int, tiers: int) -> list[dict[str, torch.Tensor]]:
    """decode_map of each tier of a crop section [N, tiers x H x W x 8] (map block revision 5: [fine][coarse]), in
    order; one entry for a one-tier map."""
    per = height * width * MAP_CHANNELS
    rows = crops.reshape(crops.shape[0], -1)
    return [decode_map(rows[:, tier * per:(tier + 1) * per], height, width) for tier in range(tiers)]


def goal_plane(forward: torch.Tensor, right: torch.Tensor, present: torch.Tensor, side: int,
               cell: float, clamp: bool = True) -> torch.Tensor:
    """The planner's goal plane [N, side, side]: a one-hot of the goal's cell in a heading-up crop of `side` cells of
    `cell` yards (row 0 furthest ahead, column 0 furthest left, the body at the centre), from the goal's forward and
    right yards in the body frame; the border cell where the line from the body to the goal leaves the crop when it
    lies outside; all zero where no goal is present. Without `clamp` (the hierarchy's fine tier,
    mappo.planner_hierarchical: the coarse plan stands in for a goal past the crop) a goal outside the crop -- its
    unclamped cell past rows or columns 0..side - 1 -- is all zero too; a goal inside is the same one-hot either way."""
    centre = (side - 1) / 2.0
    f = forward.to(torch.float32) / cell
    r = right.to(torch.float32) / cell
    reach = torch.maximum(f.abs(), r.abs()).clamp(min=1e-6)
    t = (centre / reach).clamp(max=1.0)
    row = torch.round(centre - t * f).clamp(0, side - 1).long()
    col = torch.round(centre + t * r).clamp(0, side - 1).long()
    weight = present.to(torch.float32)
    if not clamp:
        free_row, free_col = torch.round(centre - f), torch.round(centre + r)
        inside = (free_row >= 0) & (free_row <= side - 1) & (free_col >= 0) & (free_col <= side - 1)
        weight = weight * inside.to(torch.float32)
    plane = forward.new_zeros((forward.shape[0], side * side), dtype=torch.float32)
    plane.scatter_(1, (row * side + col)[:, None], weight[:, None])
    return plane.reshape(-1, side, side)


#: A cell's eight neighbours as (row, column) offsets, in the one order the planner's direction metric, its direction
#: loss, the waypoint's descent and the export use: row -1 is ahead (row 0 furthest ahead), column -1 to the left.
NEIGHBOURS = ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1))


def neighbour_stack(grid: torch.Tensor, fill: float) -> torch.Tensor:
    """[N, H, W] -> [N, 8, H, W]: each cell's eight neighbours' values in NEIGHBOURS order, `fill` past the border."""
    height, width = grid.shape[-2], grid.shape[-1]
    padded = nn.functional.pad(grid[:, None], (1, 1, 1, 1), value=fill)[:, 0]
    return torch.stack([padded[:, 1 + dr:1 + dr + height, 1 + dc:1 + dc + width] for dr, dc in NEIGHBOURS], dim=1)


def neighbours_within_45() -> torch.Tensor:
    """[8, 8] bool: whether neighbour j's direction is within 45 degrees of neighbour i's (itself and the two beside it
    round the ring)."""
    angles = torch.tensor([math.atan2(-dr, dc) for dr, dc in NEIGHBOURS], dtype=torch.float64)
    gap = (angles[:, None] - angles[None, :]).abs()
    gap = torch.minimum(gap, 2.0 * math.pi - gap)
    return gap <= math.pi / 4.0 + 1e-6


#: **The classical planner's step costs** (MappoTrainer._aux_pass's classical_* and oracle_* statistics; a measurement
#: only): what a yard of step on a cell of each class (TRAV_CLASS_NAMES order) costs, as a multiple of walking it.
#: A step pays the mean of its two cells' multiples (classical_cost_to_go). The bake's own factors where it has one
#: (TravBake.h: walk, step and a safe drop 1, swim and jump TRAV_SWIM_FACTOR / TRAV_JUMP_FACTOR 1.5), a hurting drop
#: a flat 3 (the bake adds its damage at dropHurtYards a health bar), magma or slime 5, and wall, a lethal drop and a
#: slope too steep to walk 100: blocked in effect, but finite, so every cell still has a cost-to-go. Unknown -- the
#: head's guess that it cannot tell -- a modest optimism of 1.5. The oracle reads the label's classes, whose unknown
#: is outside the bake (no route): blocked there instead.
CLASSICAL_COSTS = (1.5, 1.0, 100.0, 1.5, 5.0, 1.0, 1.5, 1.0, 3.0, 100.0, 100.0)
CLASSICAL_ORACLE_UNKNOWN = 100.0
#: The classical planner's iterations at most, the fine tier's and the coarse's (enough to round a detour across
#: the 48-cell crop), the convergence check's cadence and tolerance (yards), and the cost-to-go of a cell no goal
#: reaches (finite, so the hierarchy's bilinear resample of the coarse value never meets inf x 0).
CLASSICAL_ITERATIONS = (160, 100)
CLASSICAL_CHECK_EVERY = 8
CLASSICAL_TOLERANCE = 1e-3
CLASSICAL_SENTINEL = 1e6
#: The classical waypoint's iterations, the fine tier's and the coarse's (mappo.planner_waypoint_source "classical";
#: MapEncoder.classical_plan): a fixed count, run without the convergence check's host syncs. The walk reads only the
#: cells round the body, which settle well before the whole crop (live the crop converges in 56-80 fine, 40-56 coarse):
#: on 192 synthetic room maps (2026-10-16) the fine waypoint was final by 24 iterations at p90 and 48 at most, the
#: coarse by 24 at p90, 49 at p99 and 72 at most -- 64 each leaves one map in 192 short on the coarse tier. 64 a tier
#: cost the cluster a third of its steps live (2.7k -> 1.8k steps/s, 2026-10-16: the rollout's replan); 40 keeps every
#: fine waypoint at p90 x 1.7 and the coarse past its p90, for about 40% less.
CLASSICAL_WAYPOINT_ITERATIONS = (40, 40)
#: Where the planned waypoint can come from (mappo.planner_waypoint_source; MapEncoder.waypoints): the learned
#: planner's cost-to-go, the learner's classical planner's, the sim's own route on the bot's map (ROUTE_COLUMNS:
#: planned at 1 yd every decision and read off the observation), or the school planner's route (the seventeen
#: computed from the frozen school U-Net's fields on the crop, MapEncoder.school_route; mappo.school_checkpoint); the
#: index is the `waypoint_source` buffer's value.
WAYPOINT_SOURCES = ("learned", "classical", "sim", "school")
#: The sources whose waypoint is the seventeen route scalars' layout (ROUTE_COLUMNS; MapEncoder.sim_waypoint): the
#: school's reads the sim's columns too (route_stall, and the no_school arm's sixteen).
ROUTE_SOURCES = ("sim", "school")
#: **The classical planner over the map's own codes** (mappo.planner_waypoint_costs; MapEncoder.classical_plan,
#: classical_factor; 2026-10-16): what the step cost of a cell is priced on. "head": the costmap head's softmax under
#: CLASSICAL_COSTS everywhere (the rule as it was). "map": the crop's own code where the crop knows the cell, the
#: head's guess only where it does not -- measured in the aux pass, the classical plan over the label's true classes
#: points the right way 0.68 / 0.84 of the time (fine / coarse) against 0.53 / 0.79 over the head's, whose known-cell
#: IoU is 0.58 on walls and 0.65 on free ground: the crop's codes are what the body's rays hit, the head's classes a
#: guess at them. Per cell of MapCode k (MentalMap.h: unknown, floor, wall, door, hazard; MAP_CODE_RULES,
#: MAP_CODE_COSTS): unknown -> the head's expected cost under CLASSICAL_COSTS; wall -> 100 and hazard (magma, slime)
#: -> 5, fixed; floor and door -> the head's expected cost under CLASSICAL_FLOOR_COSTS, CLASSICAL_COSTS with every
#: blocking class (wall, a lethal drop, a slope too steep: CLASSICAL_BLOCKED) and unknown re-priced as floor. The
#: crop decides passability (a floor hit is a surface the body can stand on; the head's wall mass on such a cell is
#: exactly the error the rule removes, so max(crop, head) would keep it), the head the grade of passable ground --
#: water (1.5), a hurting drop (3), a jump -- which the crop's five codes cannot name. The coarse tier's code is the
#: majority of the cell's 1-yd cells (MentalMap.cpp Fold), so a corridor's thin wall never makes a coarse cell a wall.
WAYPOINT_COST_RULES = ("head", "map")
CLASSICAL_BLOCKED = 100.0
#: **The height band** (classical_factor, the "map" rule; 2026-10-17): a known floor or door cell whose floor (the
#: crop's height byte: the layer nearest the feet) lies more than this many yards above or below the feet is priced
#: as unknown -- the head's expected cost under CLASSICAL_COSTS -- not as floor at 1. The map stores every floor it
#: has seen, at every height: a deck 6 yd up, a ceiling meshed as a second floor, the Deadmines' phantom storey
#: 300 yd up (the byte clamps at 31.75), the sea's surface in the far tier -- none of them where the body walks, all
#: of them Floor to the code channel, so the height-blind rule planned the straight line across them (the shared
#: memory's flooded coarse crop). A band rather than a wall: a ramp climbs ~3 yd across the 20 yd the crop shows of
#: it, and its upper part priced as unknown (1.5 against the deck's 1) is still the route when nothing else is, where
#: blocked (100) would cut every storey change off the plan; nav3 teaches the ramps. A cell with no height byte keeps
#: its code's rule (the coarse tier's far cells carry a code but a mean height, which the band reads as any other).
CLASSICAL_HEIGHT_BAND = 3.0
CLASSICAL_FLOOR_COSTS = tuple(1.0 if index == 0 or cost >= CLASSICAL_BLOCKED else cost
                              for index, cost in enumerate(CLASSICAL_COSTS))
#: Per MapCode (MAP_CODES of them), how classical_factor prices a cell under the "map" rule: 0 the head's expected
#: cost under CLASSICAL_COSTS, 1 the head's under CLASSICAL_FLOOR_COSTS, 2 the fixed cost in MAP_CODE_COSTS.
MAP_CODE_RULES = (0, 1, 2, 1, 2)
MAP_CODE_COSTS = (0.0, 0.0, CLASSICAL_BLOCKED, 0.0, 5.0)
#: The cost-to-go (yards of cost) no route over free ground can reach from inside the two crops (MappoTrainer.
#: _classical_directions): a cell above it was priced through blockers -- three or more wall cells at 100, or a long
#: way through hazard -- and its error against the label is the wall's cost, not the plan's accuracy. Walking the
#: fine crop's diagonal and the coarse one's at 1 a yard is 68 + 272 = 340 yd; swum at 1.5, 510; the ceiling leaves
#: room above that. A cell behind one or two predicted wall cells (180-360 yd of cost on top of a route that may
#: itself be 300 yd) cannot be told from an open one by any ceiling and still counts in the MAE with that error.
CLASSICAL_OPEN_CEILING = 600.0


def classical_tables(device=None) -> dict[str, torch.Tensor]:
    """The classical planner's constant tables as tensors on `device`: "costs" [TRAV_CLASSES] (CLASSICAL_COSTS),
    "floor_costs" [TRAV_CLASSES] (CLASSICAL_FLOOR_COSTS), "code_rules" [MAP_CODES] long and "code_costs" [MAP_CODES]
    (MAP_CODE_RULES, MAP_CODE_COSTS)."""
    return {"costs": torch.tensor(CLASSICAL_COSTS, dtype=torch.float32, device=device),
            "floor_costs": torch.tensor(CLASSICAL_FLOOR_COSTS, dtype=torch.float32, device=device),
            "code_rules": torch.tensor(MAP_CODE_RULES, dtype=torch.long, device=device),
            "code_costs": torch.tensor(MAP_CODE_COSTS, dtype=torch.float32, device=device)}


@torch.no_grad()
def classical_factor(probabilities: torch.Tensor, tables: dict[str, torch.Tensor],
                     codes: torch.Tensor | None = None, heights: torch.Tensor | None = None) -> torch.Tensor:
    """Each cell's step cost multiple [N, H, W] for the classical planner (classical_cost_to_go) from the costmap
    head's class probabilities `probabilities` [N, C, H, W] (a softmax; a one-hot for the oracle) and, under the "map"
    rule (mappo.planner_waypoint_costs; WAYPOINT_COST_RULES), the crop's codes `codes` [N, H, W] (decode_map's
    "code"): None is the "head" rule, the expected cost under `tables["costs"]` everywhere. With codes, per cell by
    MAP_CODE_RULES: the head's expected cost under "costs" (unknown), under "floor_costs" (floor, door) or the fixed
    "code_costs" (wall, hazard); with `heights` too (the crop's height bytes [N, H, W], decode_map's "height"), a
    floor or door cell whose floor is more than CLASSICAL_HEIGHT_BAND yards off the feet is priced as unknown (the
    height band above). Float32, without a gradient."""
    probabilities = probabilities.to(torch.float32)
    head = torch.einsum("nchw,c->nhw", probabilities, tables["costs"])
    if codes is None:
        return head
    codes = codes.long().clamp(0, MAP_CODES - 1)
    floor = torch.einsum("nchw,c->nhw", probabilities, tables["floor_costs"])
    rule = tables["code_rules"][codes]
    fixed = tables["code_costs"][codes]
    if heights is not None:
        byte = heights.to(torch.float32)
        off_band = (byte != 0) & ((byte - MAP_HEIGHT_ZERO).abs() * MAP_HEIGHT_STEP > CLASSICAL_HEIGHT_BAND)
        rule = torch.where((rule == 1) & off_band, torch.zeros_like(rule), rule)
    return torch.where(rule == 0, head, torch.where(rule == 1, floor, fixed))


@torch.no_grad()
def classical_cost_to_go(factor: torch.Tensor, goal: torch.Tensor, cell: float, iterations: int,
                         ring: torch.Tensor | None = None, check: bool = True) -> tuple[torch.Tensor, int]:
    """**The classical planner** (a measurement beside the learned one; CLASSICAL_COSTS): fixed-weight value iteration
    -- min-plus Bellman, V(s) = min over the eight neighbours n (NEIGHBOURS) of V(n) + step yards (`cell`, x sqrt 2
    diagonally) x (factor(s) + factor(n)) / 2 -- over `factor` [N, H, W] (each cell's step cost multiple) from the
    goal cells (`goal` [N, H, W] > 0, held at 0), every other cell starting at CLASSICAL_SENTINEL. The mean of the
    two cells rather than the one entered: a wall's own cell then holds a high cost-to-go too, so the hierarchy's
    bilinear resample of the coarse value (below) does not leak a wall's far side onto its near one (measured on the
    synthetic wall map: 3.9 against 8.2 yd of error). `ring` [N, H + 2, W + 2] (the hierarchy's boundary: the coarse
    tier's classical cost-to-go resampled, MapEncoder.coarse_frame; its interior is ignored) holds the value just
    past the border, a step onto it priced at the border cell's own factor; without one nothing lies past the
    border. Runs until no cell moved more than CLASSICAL_TOLERANCE yards over CLASSICAL_CHECK_EVERY iterations,
    `iterations` at most; without `check` exactly `iterations`, with no host sync (the classical waypoint's fixed
    count, CLASSICAL_WAYPOINT_ITERATIONS: the same iterations, so the same numbers as far as they go). Returns the
    cost-to-go [N, H, W] (yards; the sentinel where nothing reached) and the iterations run. Without a gradient; reads
    no random state.

    The iteration (2026-10-16): two padded buffers, each the ring around the value, read and written in turn. On a
    GPU a step is two kernels -- the nine candidates (the eight neighbours' sums and the cell's own value) in one add
    of a strided window over the buffer and the edges stacked once, then their minimum -- where it was nineteen: the
    classical waypoint's 80 iterations over 192 rows took a median 43 ms beside training on dev's card (12 at best:
    the small kernels queue behind the update's), now 10 (9). On the host, where launches are cheap and the nine
    candidates measured slower at 192 rows, the eight sums and minimums run in place in preallocated buffers instead
    (classical_plan at 192 rows 39 -> 29 ms). Either gives the same bits as the plain form (pad, add the ring, the
    eight minimums, clamp at the sentinel): the minimum is exact in any order, a value plus the ring's zero is the
    value, and the clamp never bit (each step's minimum includes the cell's own value, never above the sentinel)."""
    n, height, width = factor.shape
    factor = factor.to(torch.float32)
    sentinel = CLASSICAL_SENTINEL
    padded_factor = nn.functional.pad(factor[:, None], (1, 1, 1, 1), mode="replicate")[:, 0]
    scales = [0.5 * cell * math.hypot(dr, dc) for dr, dc in NEIGHBOURS]
    # The two buffers: the ring held (the sentinel without one), the value inside; a step reads one, writes the other.
    padded = torch.full((n, height + 2, width + 2), sentinel, device=factor.device) if ring is None \
        else ring.to(torch.float32).clamp(max=sentinel).clone()
    buffers = (padded, padded.clone())
    inner = tuple(buffer[:, 1:-1, 1:-1] for buffer in buffers)
    inner[0].copy_(torch.where(goal > 0, torch.zeros_like(factor), torch.full_like(factor, sentinel)))
    if factor.device.type != "cpu":
        # [N, 3, 3, H, W]: the step onto the neighbour at (dr, dc) at [:, 1 + dr, 1 + dc], 0 for the cell itself, and
        # a window of each buffer laid out alike (no copy).
        edges = factor.new_zeros((n, 3, 3, height, width))
        for (dr, dc), scale in zip(NEIGHBOURS, scales):
            neighbour = padded_factor[:, 1 + dr:1 + dr + height, 1 + dc:1 + dc + width]
            edges[:, 1 + dr, 1 + dc] = (neighbour + factor) * scale
        windows = tuple(buffer.as_strided((n, 3, 3, height, width), (buffer.stride(0), buffer.stride(1),
                                                                      buffer.stride(2), buffer.stride(1),
                                                                      buffer.stride(2)))
                        for buffer in buffers)
        candidates = torch.empty_like(edges)

        def step(source: int) -> None:
            torch.add(windows[source], edges, out=candidates)
            torch.amin(candidates.view(n, 9, height, width), dim=1, out=inner[1 - source])
    else:
        edges = [(padded_factor[:, 1 + dr:1 + dr + height, 1 + dc:1 + dc + width] + factor) * scale
                 for (dr, dc), scale in zip(NEIGHBOURS, scales)]
        candidate = torch.empty_like(factor)

        def step(source: int) -> None:
            target = inner[1 - source]
            target.copy_(inner[source])
            for (dr, dc), edge in zip(NEIGHBOURS, edges):
                torch.add(buffers[source][:, 1 + dr:1 + dr + height, 1 + dc:1 + dc + width], edge, out=candidate)
                torch.minimum(target, candidate, out=target)
    source = 0
    done = 0
    while done < int(iterations):
        previous = inner[source].clone() if check else None
        for _ in range(min(CLASSICAL_CHECK_EVERY, int(iterations) - done)):
            step(source)
            source = 1 - source
            done += 1
        if check and float((previous - inner[source]).abs().max()) <= CLASSICAL_TOLERANCE:
            break
    return inner[source].contiguous(), done


@torch.no_grad()
def descend(cost: torch.Tensor, start: int, steps: int) -> tuple[torch.Tensor, torch.Tensor]:
    """**The planned waypoint's walk** (mappo.planner_waypoint): steepest descent over `cost` [N, H, W] (the planner's
    cost-to-go, lower is nearer the goal) from cell (start, start), `steps` moves at most, each to the neighbour of
    the lowest cost (the first of equals in NEIGHBOURS order) and only when it is strictly lower than the cell it
    leaves, so a cell with no lower neighbour ends the walk there (a walk never revisits a cell). Deterministic and
    without a gradient. Returns the cell reached, (row, column) [N] long each."""
    n, height, width = cost.shape
    flat = cost.reshape(n, -1).to(torch.float32)
    offsets = torch.tensor(NEIGHBOURS, dtype=torch.long, device=cost.device)
    row = torch.full((n,), int(start), dtype=torch.long, device=cost.device)
    col = row.clone()
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
    return row, col


@torch.no_grad()
def descend_path(cost: torch.Tensor, start_row: torch.Tensor, start_col: torch.Tensor,
                 steps: int) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """descend's walk from each row's own start cell ([N] long each) with every cell it visits: (rows, columns)
    [N, steps + 1] long, the start first and, after a move that did not happen, the cell it stayed on (a walk that
    stopped repeats its end), and the moves made [N] long (the index of the last cell that differs from the one
    before it; 0 where it never moved)."""
    n, height, width = cost.shape
    flat = cost.reshape(n, -1).to(torch.float32)
    offsets = torch.tensor(NEIGHBOURS, dtype=torch.long, device=cost.device)
    row = start_row.reshape(n).long().clone()
    col = start_col.reshape(n).long().clone()
    rows_out, cols_out = [row], [col]
    moved = torch.zeros((n,), dtype=torch.long, device=cost.device)
    for step in range(1, int(steps) + 1):
        here = flat.gather(1, (row * width + col)[:, None])[:, 0]
        rows, cols = row[:, None] + offsets[None, :, 0], col[:, None] + offsets[None, :, 1]
        inside = (rows >= 0) & (rows < height) & (cols >= 0) & (cols < width)
        values = flat.gather(1, rows.clamp(0, height - 1) * width + cols.clamp(0, width - 1))
        values = torch.where(inside, values, torch.full_like(values, math.inf))
        best, which = values.min(dim=1)
        move = best < here
        row = torch.where(move, rows.gather(1, which[:, None])[:, 0], row)
        col = torch.where(move, cols.gather(1, which[:, None])[:, 0], col)
        moved = torch.where(move, torch.full_like(moved, step), moved)
        rows_out.append(row)
        cols_out.append(col)
    return torch.stack(rows_out, dim=1), torch.stack(cols_out, dim=1), moved


@torch.no_grad()
def route_from_fields(fine_cost: torch.Tensor, fine_reach: torch.Tensor, coarse_cost: torch.Tensor,
                      coarse_reach: torch.Tensor, present: torch.Tensor, stall: torch.Tensor,
                      cells: Sequence[float], anchor: int) -> torch.Tensor:
    """**The school's route** (ROUTE_POINT_YARDS and the rule there): ROUTE_COLUMNS' seventeen [N, 17] float32 from
    the fine and coarse cost-to-go [N, H, W] (yards; lower is nearer) and reachable logits [N, H, W] of a planner on
    the crop of `cells` yards a tier, whether a goal is present [N] bool, the sim's route_stall [N] (passed through)
    and the walk's start cell. Every group times its valid flag; all 0 but the stall where no goal is present."""
    sentinel = CLASSICAL_SENTINEL
    n, side = fine_cost.shape[0], fine_cost.shape[-1]
    centre = (side - 1) / 2.0
    fine = torch.where(fine_reach > 0.0, fine_cost.to(torch.float32), torch.full_like(fine_cost, sentinel,
                                                                                         dtype=torch.float32))
    coarse = torch.where(coarse_reach > 0.0, coarse_cost.to(torch.float32),
                         torch.full_like(coarse_cost, sentinel, dtype=torch.float32))
    present = present.to(torch.bool)
    # The fine walk from the least of the centre four (row-major first of equals) and its points.
    c = int(math.floor(centre))
    four = fine[:, c:c + 2, c:c + 2].reshape(n, -1)
    body, which = four.min(dim=1)
    steps = int(round(ROUTE_FINE_YARDS / float(cells[0])))
    rows, cols, moved = descend_path(fine, c + which // 2, c + which % 2, steps)
    forward = (centre - rows.to(torch.float32)) * float(cells[0])          # [N, steps + 1]
    right = (cols.to(torch.float32) - centre) * float(cells[0])
    fine_valid = ((moved > 0) & present).to(torch.float32)
    columns = []
    for yards in ROUTE_POINT_YARDS:
        at = float(yards) / float(cells[0])
        low = min(int(math.floor(at)), steps)
        high = min(low + 1, steps)
        frac = at - low
        f = forward[:, low] * (1.0 - frac) + forward[:, high] * frac
        r = right[:, low] * (1.0 - frac) + right[:, high] * frac
        d = torch.sqrt(f * f + r * r).clamp(min=1e-6)
        columns += [-r / d * fine_valid, f / d * fine_valid]
    columns += [moved.to(torch.float32) * float(cells[0]) / ROUTE_FINE_YARDS * fine_valid, fine_valid]
    # The coarse waypoint.
    row, col = descend(coarse, anchor, ROUTE_COARSE_STEPS)
    coarse_valid = (((row != anchor) | (col != anchor)) & present).to(torch.float32)
    f = (centre - row.to(torch.float32)) * float(cells[1])
    r = (col.to(torch.float32) - centre) * float(cells[1])
    d = torch.sqrt(f * f + r * r).clamp(min=1e-6)
    columns += [-r / d * coarse_valid, f / d * coarse_valid, d / (ROUTE_COARSE_STEPS * float(cells[1])) * coarse_valid,
                coarse_valid]
    # The own-map distance: the least reachable cost-to-go over the centre four (the walk's start's).
    dist_valid = ((body < sentinel) & present).to(torch.float32)
    columns += [body.clamp(max=ROUTE_DIST_YARDS) / ROUTE_DIST_YARDS * dist_valid, dist_valid,
                stall.to(torch.float32)]
    return torch.stack(columns, dim=-1)


class CameraSplat(nn.Module):
    """**The camera splat** (lift, splat; mappo.camera_splat): the camera's patch features laid on the crop's cells,
    so the costmap head reads what each part of the image shows *where it is*, not only the whole image's embedding
    broadcast over every cell.

    **Lift.** Every pixel's ray is fixed by the camera's geometry, exactly as the sim casts it (Vision::PlaceCamera,
    PixelAngles, PixelDirection): the pivot over the body, the camera BOOM yards behind it along the view (the boom the
    vision scalars carry, after the pull-in against walls), the view at the body's facing plus the free look's yaw
    offset and at the pitch the scalars carry, and the pixel's own angles -- equiangular, yaw right = ((c + 0.5) / w x
    FovH - FovH / 2), pitch up = (FovV / 2 - (r + 0.5) / h x FovV) -- of the pixel it was cast as: a frame cast at w x h
    is scaled up to the canonical W x H by nearest pixel (Vision::Upscale), so canonical column c is cast column
    floor(c w / W), w the scalars' cast width (h = H w / W: every render size is the canonical's aspect). The distance
    is the depth channel's byte decoded (d = NEAR (REFERENCE / NEAR) ^ (b / 254); sky, 255, lands nowhere). The hit's
    place in the body's heading-up frame, yards forward f and right r, needs neither the body's height nor the pivot's:
    f = -BOOM cos(pitch) cos(yaw) + d cos(e) cos(a), left = -BOOM cos(pitch) sin(yaw) + d cos(e) sin(a) (e and a the
    ray's elevation and azimuth from the facing; WoW's yaw turns left), the crop's own frame (MentalMap::Crop, the same
    pose: the body's position and facing of the same decision): cell row floor(side / 2 - f / cell), column floor(side
    / 2 + r / cell).

    **Splat.** Every STRIDE-th pixel of every STRIDE-th row (rows and columns STRIDE / 2, STRIDE / 2 + STRIDE, ...:
    2,048 samples at 128 x 64, 16 a patch of 8) carries its patch's features, projected Linear(features -> CHANNELS),
    to the cell it lands in; a cell's value is the mean over the samples that landed in it, and one more plane says
    whether any did ("covered"): [N, CHANNELS + 1, side, side] a tier, zeros and covered 0 where nothing landed. Both
    tiers from the same rays (2 and 8 yd cells). The geometry is computed without a gradient (indices); the features
    keep theirs, so the costmap's loss teaches the camera's patches what the ground they show is like. Fixed shapes
    and no read-back (scatter_add over a clamped index with a zero weight for a sample that lands nowhere): a rollout
    graph captures it."""

    CHANNELS = 8
    STRIDE = 2

    def __init__(self, image: dict, features: int):
        super().__init__()
        self.height, self.width, self.patch = int(image["height"]), int(image["width"]), int(image["patch"])
        fov_h, fov_v = image.get("fov", (IMAGE_FOV_H, IMAGE_FOV_V))
        self.fov_h, self.fov_v = math.radians(float(fov_h)), math.radians(float(fov_v))
        self.log_range = math.log(float(image.get("distance_reference", IMAGE_DISTANCE_REFERENCE)) / IMAGE_NEAR)
        self.project = nn.Linear(features, self.CHANNELS)
        rows, cols = torch.meshgrid(torch.arange(self.STRIDE // 2, self.height, self.STRIDE),
                                    torch.arange(self.STRIDE // 2, self.width, self.STRIDE), indexing="ij")
        rows, cols = rows.reshape(-1), cols.reshape(-1)
        # Derived from the image's shape, not learned: each sample's canonical row and column, its distance byte's
        # offset in the image's bytes ([row][col][4]) and its patch (VisionEncoder.patches' order, row by row).
        self.register_buffer("sample_row", rows.to(torch.float32), persistent=False)
        self.register_buffer("sample_col", cols.to(torch.float32), persistent=False)
        self.register_buffer("sample_byte", (rows * self.width + cols) * IMAGE_BYTES_PER_PIXEL, persistent=False)
        self.register_buffer("sample_patch", (rows // self.patch) * (self.width // self.patch) + cols // self.patch,
                             persistent=False)

    @property
    def samples(self) -> int:
        return int(self.sample_byte.shape[0])

    def ground(self, pixels: torch.Tensor, scalars: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """Where each sample's ray ends in the body's heading-up frame: (forward yards, right yards, hit) [N, samples]
        each, from the image's bytes [N, image bytes] and the vision block's scalars [N, S] (raw). Float32, no grad."""
        with torch.no_grad():
            scalars = scalars.to(torch.float32)
            distance = pixels[:, self.sample_byte].to(torch.float32)
            hit = distance < 255.0
            d = IMAGE_NEAR * torch.exp(distance / 254.0 * self.log_range)
            yaw_sin = scalars[:, VISION_SCALAR_YAW_SIN, None]
            yaw_cos = scalars[:, VISION_SCALAR_YAW_COS, None]
            pitch = scalars[:, VISION_SCALAR_PITCH, None] * (math.pi / 2.0)
            boom = scalars[:, VISION_SCALAR_BOOM, None] * VISION_ZOOM_SCALE
            # The size the frame was cast at (the canonical where the scalar says nothing).
            share = scalars[:, VISION_SCALAR_RENDER_WIDTH, None]
            cast_w = torch.where(share > 0.0, torch.round(share * self.width), torch.full_like(share, self.width))
            cast_w = cast_w.clamp(1.0, float(self.width))
            cast_h = torch.round(cast_w * self.height / self.width).clamp(1.0, float(self.height))
            cast_col = torch.floor(self.sample_col * cast_w / self.width)
            cast_row = torch.floor(self.sample_row * cast_h / self.height)
            yaw_right = (cast_col + 0.5) / cast_w * self.fov_h - self.fov_h / 2.0
            pitch_up = self.fov_v / 2.0 - (cast_row + 0.5) / cast_h * self.fov_v
            # The ray's azimuth from the facing, a = yaw - yaw_right, by its sine and cosine.
            cos_a = yaw_cos * torch.cos(yaw_right) + yaw_sin * torch.sin(yaw_right)
            sin_a = yaw_sin * torch.cos(yaw_right) - yaw_cos * torch.sin(yaw_right)
            level = d * torch.cos(pitch + pitch_up)
            back = boom * torch.cos(pitch)
            forward = level * cos_a - back * yaw_cos
            right = -(level * sin_a - back * yaw_sin)
        return forward, right, hit

    def forward(self, features: torch.Tensor, pixels: torch.Tensor, scalars: torch.Tensor, side: int,
                cells: Sequence[float]) -> list[torch.Tensor]:
        """`features` [N, patches, F] (VisionEncoder.features), `pixels` [N, image bytes] uint8, `scalars` [N, S] ->
        per tier of `cells` yards a cell, [N, CHANNELS + 1, side, side]: the mean of the samples' projected features
        over the cell, then covered (1 where a sample landed)."""
        n = features.shape[0]
        projected = self.project(features.to(self.project.weight.dtype))                     # [N, patches, C]
        values = projected.gather(1, self.sample_patch[None, :, None].expand(n, -1, self.CHANNELS))
        forward, right, hit = self.ground(pixels, scalars)
        out = []
        for cell in cells:
            with torch.no_grad():
                row = torch.floor(side / 2.0 - forward / cell)
                col = torch.floor(side / 2.0 + right / cell)
                inside = hit & (row >= 0) & (row < side) & (col >= 0) & (col < side)
                index = torch.where(inside, row * side + col, torch.zeros_like(row)).long()
                weight = inside.to(values.dtype)
            sums = values.new_zeros((n, side * side, self.CHANNELS)).scatter_add(
                1, index[:, :, None].expand(-1, -1, self.CHANNELS), values * weight[:, :, None])
            counts = weight.new_zeros((n, side * side)).scatter_add(1, index, weight)
            mean = sums / counts.clamp(min=1.0)[:, :, None]
            plane = torch.cat([mean, (counts > 0).to(mean.dtype)[:, :, None]], dim=-1)
            out.append(plane.transpose(1, 2).reshape(n, self.CHANNELS + 1, side, side))
        return out


class CostmapHead(nn.Module):
    """**The costmap head** (vision movement curriculum, CONTRACT.md sec 5 (b)): from a tier's decoded map planes
    (MapEncoder.planes, [N, H, W, P]) and the camera's embedding broadcast over the crop (Linear EMBED -> VISION, plus a
    learned bias per tier, the same head on both tiers), two 3 x 3 convolutions of HIDDEN channels with SiLU and a 1 x 1
    to TRAV_CLASSES logits a cell: the egocentric traversability the policy plans on, trained by cross-entropy against
    the sim's bake (trainer aux.costmap_coef) and never by the return.

    With the camera splat (mappo.camera_splat, `splat` its planes a cell: CameraSplat.CHANNELS + 1) conv1 also reads
    the tier's splat planes, after the broadcast ones: [planes, vision, splat features, covered]. A head without it is
    exactly what it was."""

    VISION = 8
    HIDDEN = 32

    def __init__(self, planes: int, vision_width: int, tiers: int, splat: int = 0):
        super().__init__()
        self.splat = int(splat)
        self.vision = _linear(vision_width, self.VISION, 1.0)
        self.tier_bias = nn.Parameter(torch.zeros(tiers, self.VISION))
        self.conv1 = nn.Conv2d(planes + self.VISION + self.splat, self.HIDDEN, 3, padding=1)
        self.conv2 = nn.Conv2d(self.HIDDEN, self.HIDDEN, 3, padding=1)
        self.out = nn.Conv2d(self.HIDDEN, TRAV_CLASSES, 1)

    def forward(self, planes: torch.Tensor, camera: torch.Tensor, tier: int,
                splat: torch.Tensor | None = None) -> torch.Tensor:
        """`planes` [N, P, H, W], `camera` [N, vision_width], `splat` [N, splat, H, W] (CameraSplat; required exactly
        when the head has splat planes) -> logits [N, TRAV_CLASSES, H, W]."""
        silu = nn.functional.silu
        if (splat is None) != (self.splat == 0):
            raise ValueError(f"the costmap head reads {self.splat} camera splat planes; "
                             f"{'none were' if splat is None else 'some were'} given")
        seen = self.vision(camera.to(self.vision.weight.dtype)) + self.tier_bias[tier].to(self.vision.weight.dtype)
        seen = seen[:, :, None, None].expand(-1, -1, planes.shape[-2], planes.shape[-1])
        inputs = [planes.to(seen.dtype), seen] + ([splat.to(seen.dtype)] if splat is not None else [])
        x = silu(self.conv1(torch.cat(inputs, dim=1)))
        return self.out(silu(self.conv2(x)))


class Planner(nn.Module):
    """**The planner** (CONTRACT.md sec 5 (c)): a value iteration network over a tier's planes -- the costmap's softmax
    (TRAV_CLASSES), the decoded map planes and the goal plane -- `K` iterations of value = max over CHANNELS of
    conv3x3(cat(value, planes)), the planes' convolution taken once (`fixed`, the per-channel step costs) and the
    value's each iteration (`recur`, no bias), the two summing to the one conv3x3 of the contract. One Planner for both
    tiers. Initialised as Bellman's own rule: channel c of `recur` copies neighbour c of the 3 x 3 (the eight
    neighbours), `fixed` is small, so the value starts at 0 everywhere and propagates as the step costs are learned.
    Under a gradient the iterations are checkpointed SEGMENT at a time (torch.utils.checkpoint), so the update keeps
    SEGMENT iterations' activations a row rather than K; the rollout runs it without one.

    **K per tier** (mappo.planner_k, planner_k_fine): `k` the iterations of every tier, or of the coarse alone where
    `k_fine` is given (the fine tier's, a buffer of its own built only then, so a planner without it has exactly the
    state it had). Both are buffers -- the checkpoint and the export carry them -- but the configured counts stand: a
    checkpoint that ran other counts loads with this network's own, and says so (a resume that changes K changes it).

    **The boundary** (the hierarchy, mappo.planner_hierarchical; MapEncoder.plan): forward's `init` is the value the
    iterations start from instead of 0 and `ring` [N, H + 2, W + 2] the value held just past the crop's border every
    iteration instead of recur's zero padding (its interior is ignored), and `bound` holds the value at or below 0 (a
    cost-to-go is never below 0 yards: the goal saturates at 0 rather than feeding back on itself -- nav2's trained
    `recur` has a channel of gain 1.97, so a positive value doubled every iteration, harmless at K 20 and 1e6 yd of
    error at 48). Without them the iteration is exactly what it was."""

    K = 20
    CHANNELS = 8
    SEGMENT = 8

    def __init__(self, planes: int, k: int = K, k_fine: int | None = None):
        super().__init__()
        self.planes = planes
        # The iterations (mappo.planner_k): a buffer, so a checkpoint and the export carry it; the host copy is what
        # forward reads (a captured graph cannot read the device).
        self.iterations = max(1, int(k))
        self.register_buffer("k", torch.tensor(self.iterations, dtype=torch.long))
        # The fine tier's own (mappo.planner_k_fine): built only when configured.
        self.fine_iterations = self.iterations
        if k_fine is not None:
            self.fine_iterations = max(1, int(k_fine))
            self.register_buffer("k_fine", torch.tensor(self.fine_iterations, dtype=torch.long))
        self.fixed = nn.Conv2d(planes, self.CHANNELS, 3, padding=1)
        self.recur = nn.Conv2d(1, self.CHANNELS, 3, padding=1, bias=False)
        with torch.no_grad():
            nn.init.orthogonal_(self.fixed.weight, gain=0.1)
            nn.init.zeros_(self.fixed.bias)
            self.recur.weight.zero_()
            neighbours = [(r, c) for r in range(3) for c in range(3) if (r, c) != (1, 1)]
            for channel in range(self.CHANNELS):
                r, c = neighbours[channel % len(neighbours)]
                self.recur.weight[channel, 0, r, c] = 1.0

    def tier_iterations(self, tier: int) -> int:
        """The iterations tier `tier` runs (0 the fine tier)."""
        return self.fine_iterations if tier == 0 else self.iterations

    def _load_from_state_dict(self, state_dict, prefix, *args, **kwargs):
        configured = {"k": self.iterations}
        if hasattr(self, "k_fine"):
            configured["k_fine"] = self.fine_iterations
        super()._load_from_state_dict(state_dict, prefix, *args, **kwargs)
        # The configured counts stand (a checkpoint from before a buffer leaves it as built): the buffers are put back,
        # so the state this network saves, its rollout copy and the export all say what it runs.
        for name, count in configured.items():
            buffer = getattr(self, name)
            if int(buffer) != count:
                print(f"Load: the checkpoint's planner ran {name} = {int(buffer)} iterations; this network's "
                      f"configured {count} stand (mappo.planner_k{'_fine' if name == 'k_fine' else ''})", flush=True)
                buffer.fill_(count)

    def _iterate(self, value: torch.Tensor, fixed: torch.Tensor, count: int,
                 ring: torch.Tensor | None = None, bound: bool = False) -> torch.Tensor:
        for _ in range(count):
            if ring is None:
                value = (self.recur(value) + fixed).amax(dim=1, keepdim=True)
            else:
                # The held border: the value padded with the ring (zero inside it) in place of recur's zeros.
                padded = nn.functional.pad(value, (1, 1, 1, 1)) + ring
                value = (nn.functional.conv2d(padded, self.recur.weight) + fixed).amax(dim=1, keepdim=True)
            if bound:
                value = value.clamp(max=0.0)
        return value

    def forward(self, planes: torch.Tensor, iterations: int | None = None, init: torch.Tensor | None = None,
                ring: torch.Tensor | None = None, bound: bool = False) -> torch.Tensor:
        """`planes` [N, P, H, W] -> the value [N, H, W] after `iterations` (K when None) iterations (unitless; -value x
        100 is yards of cost-to-go once trained, MapEncoder.plan), started from `init` [N, H, W] (0 when None) with
        the border held at `ring` [N, H + 2, W + 2] (recur's zero padding when None; the class docstring). `bound`
        holds the value at or below 0 after every iteration (and the start): a cost-to-go is never below 0 yards."""
        fixed = self.fixed(planes.to(self.fixed.weight.dtype))
        if init is None:
            value = fixed.new_zeros((planes.shape[0], 1, planes.shape[-2], planes.shape[-1]))
        else:
            value = init[:, None].to(fixed.dtype)
            if bound:
                value = value.clamp(max=0.0)
        if ring is not None:
            inside = torch.ones_like(ring[:1, 1:-1, 1:-1])
            ring = (ring * (1.0 - nn.functional.pad(inside, (1, 1, 1, 1))))[:, None].to(fixed.dtype)
            if bound:
                ring = ring.clamp(max=0.0)
        total = self.iterations if iterations is None else max(1, int(iterations))
        done = 0
        while done < total:
            count = min(self.SEGMENT, total - done)
            if torch.is_grad_enabled() and fixed.requires_grad:
                from torch.utils.checkpoint import checkpoint
                value = checkpoint(self._iterate, value, fixed, count, ring, bound, use_reentrant=False)
            else:
                value = self._iterate(value, fixed, count, ring, bound)
            done += count
        return value[:, 0]


def _spatial_softmax(features: torch.Tensor, grid_x: torch.Tensor, grid_y: torch.Tensor) -> torch.Tensor:
    """Per channel of [N, C, H, W] the expected (x, y) of its softmax over the grid, [N, 2C]."""
    flat = features.flatten(2)
    weights = torch.softmax(flat, dim=-1)
    x = (weights * grid_x.to(weights.dtype)).sum(-1)
    y = (weights * grid_y.to(weights.dtype)).sum(-1)
    return torch.cat([x, y], dim=-1)


#: The school planner's keys in an actor's state (MapEncoder.school; mappo.school_checkpoint): frozen, saved with the
#: policy, tolerated missing (the file's weights stand) and dropped into an actor without a school (load_actor_state,
#: bootstrap._seed_map).
SCHOOL_PREFIX = "vision.map.school."


def load_school(path: str, height: int, width: int, tier_cells: Sequence[float]) -> nn.Module:
    """The planner school's U-Net (school.model.SchoolPlanner) from a checkpoint animus.school.train wrote
    (mappo.school_checkpoint), frozen: eval mode, no parameter requires a gradient. Refused: no path or no file; a
    checkpoint of another model (the VIN comparison is not a policy's planner), of another protocol (the crop's bytes
    or the labels' frame changed under it), or of another crop than this encoder's (the school is built for
    school.data.SIDE cells of TIER_CELLS yards a tier)."""
    from ..school import model as school_model
    from ..school.data import SIDE, TIER_CELLS
    from .. import protocol
    if not path:
        raise ValueError("mappo.planner_waypoint_source school needs mappo.school_checkpoint (a best.pt or latest.pt "
                         "animus.school.train wrote)")
    if not os.path.isfile(path):
        raise FileNotFoundError(f"mappo.school_checkpoint: no file at {path}")
    state = torch.load(path, map_location="cpu", weights_only=False)
    config = dict(state.get("config") or {})
    saved = dict(state.get("format") or {})
    model_name = str(config.get("model", "unet"))
    if model_name != "unet":
        raise ValueError(f"mappo.school_checkpoint {path}: a {model_name!r} school; the policy's planner is the U-Net "
                         f"(school.model.SchoolPlanner, config model \"unet\")")
    if int(saved.get("protocol", -1)) != protocol.PROTOCOL_VERSION:
        raise ValueError(f"mappo.school_checkpoint {path}: trained under protocol {saved.get('protocol')}, this "
                         f"learner speaks {protocol.PROTOCOL_VERSION} (the crop or the labels changed: retrain the "
                         f"school on this tree's shards)")
    if (height, width) != (SIDE, SIDE) or tuple(float(c) for c in tier_cells) != tuple(TIER_CELLS):
        raise ValueError(f"mappo.school_checkpoint: the school plans {SIDE} x {SIDE} cells of {TIER_CELLS} yards a "
                         f"tier, this map is {height} x {width} of {tuple(tier_cells)}")
    widths = tuple(int(w) for w in config.get("widths", (32, 64, 96, 128)))
    school = school_model.SchoolPlanner(widths=widths)
    school.load_state_dict(state["model"])
    school.eval()
    for parameter in school.parameters():
        parameter.requires_grad_(False)
    return school


class MapEncoder(nn.Module):
    """**The mental map's encoder** (perception-goals REDESIGN §3, the Change): the one heading-up crop (48 x 48 cells
    of 2 yd, protocol 24's bytes after the camera's image), shared by every layout as the camera is. Per cell, the code
    embedded (CODE_EMBED wide), the entity's class through the camera's own class embedding, and seven values
    (decode_map): CODE_EMBED + the class embedding + 7 planes (17; 16 before the walls plane, map block revision 6).
    **Patches**, as the camera's: PATCH x PATCH cells
    (8 yd square at 2 yd; a 12 x 12 grid at 48 x 48) through Linear -> 64 + SiLU and Linear 64 -> 64 + SiLU, each
    channel's spatial-softmax keypoint over the grid, the channels' mean, the block's scalars (its own columns, read
    raw), and Linear -> EMBED. A convolution stack at the crop's own resolution was measured first: 3.6 times the
    camera's update cost on the host's card, the patches a fraction of it.

    **Two tiers** (map block revision 5, vision movement curriculum CONTRACT.md sec 3.2; `crop["tiers"]` 2): the crop
    section is [fine 48 x 48 at 2 yd][coarse 48 x 48 at 8 yd], each tier through the same patch and mix weights plus a
    learned tier embedding on its patch features, the two tiers' keypoints and means concatenated before `embed`. A
    one-tier map's parameters are exactly what they were. With two tiers the encoder also carries the **costmap head**
    (CostmapHead) and the **planner** (Planner, `plan`): per tier the head's TRAV_CLASSES logits a cell from the tier's
    planes and the camera's embedding, and the VIN's value over softmax(logits) + the planes + the goal plane (the
    compass block's bearing and distance, goal_plane); its readouts -- the value at the centre 3 x 3 of each tier --
    join the embedding through `planner_join` (detached: the planner learns from its own labels, the trainer's aux
    losses, and the policy reads it), and the fine tier's value pooled over the cell pointer's blocks is the pointer's
    bonus (CellPointer.planner_weight). `planner_off` (the eval arm no_planner) zeroes both.

    **The planned waypoint** (mappo.planner_waypoint, `waypoint`; the global planner -> local controller split): per
    tier, steepest descent over the planner's cost-to-go (descend) from the body's cell -- the readouts' centre, cell
    (WAYPOINT_ANCHOR, WAYPOINT_ANCHOR), the cell ahead-left of the body, which lies on the corner of the centre four --
    WAYPOINT_STEPS moves at most (5 of 2 yd on the fine tier, about 10 yd; 2 of 8 yd on the coarse, which still plans
    when the goal lies past the fine crop and the fine goal plane is clamped to its border), and the cell reached as
    WAYPOINT_FEATURES numbers a tier, measured from the body itself (the crop's centre, (side - 1) / 2 in cells): the
    bearing's sin and cos as the compass block has them (a positive bearing is to the left: sin = -right / distance,
    cos = forward / distance), the distance over the walk's reach (steps x the tier's cell yards: the compass's 500 yd
    would leave a waypoint's 10 yd at 0.02), and a valid flag; all four 0 where the walk did not move or no goal is
    present. They are appended to the readouts (readout_width; so the acting state caches them with the readouts
    between replans), computed without a gradient, zeroed by planner_off, and join the embedding through their own
    `waypoint_join`, which starts at zero: a policy loaded from a checkpoint without it computes exactly what it did.
    The critic reads the actor's encoder, so it sees the waypoint as it sees the readouts.

    **The classical waypoint** (mappo.planner_waypoint_source "classical", `waypoint_source`; 2026-10-16): the walk
    over the classical planner's cost-to-go (classical_plan: classical_cost_to_go over the costmap head's softmax
    priced by CLASSICAL_COSTS, the coarse tier first and, with the hierarchy, its cost-to-go resampled as the fine
    tier's ring, coarse_frame; from the same goal planes; CLASSICAL_WAYPOINT_ITERATIONS fixed iterations) instead of
    the learned value's -- measured live it points the right way far more often (direction accuracy coarse 66 % against
    37 %, fine 46 % against 32 %). The same walk, numbers and scaling, so waypoint_join reads what it read; the learned
    readouts and the pointer's bonus are exactly as they were. Without a gradient; zeroed by planner_off as before. A
    flag buffer (`waypoint_source`, the export reads it) built only then; no parameter, so a checkpoint resumes.

    **The classical waypoint over the map's codes** (mappo.planner_waypoint_costs "map", `waypoint_costs`;
    2026-10-16): the cells' step costs priced on the crop's own codes where the crop knows a cell and on the head's
    guess only where it does not (WAYPOINT_COST_RULES, classical_factor: the rule and why). The same walk, iterations
    and scaling; another flag buffer (`waypoint_costs`, the export reads it: .amdl version 17) built only then; no
    parameter, so a checkpoint resumes and nav1's seeds it by name as before.

    **The sim's route** (mappo.planner_waypoint_source "sim", `sim_waypoint`; 2026-10-17, map block revision 7): the
    sim plans the route on the bot's own map at 1 yd (never the 2-yd crop, whose cells sealed corridors) every decision
    and writes seventeen scalars of it at the end of the map block (ROUTE_COLUMNS: the fine path's points at 5, 10, 20
    and 30 yd as bearing sin and cos, the yards walked over 30 and a valid flag; the coarse waypoint's sin, cos,
    distance over its reach and valid flag; the own-map cost-to-go over 300 yd and its valid flag; the stall clock over
    30 s). The waypoint's columns of the readout are those scalars read off the observation (sim_waypoints: by each
    layout's `route` columns, each group times its valid flag, ROUTE_GROUPS), refreshed EVERY decision -- plan() reads
    them when it runs, and forward() overwrites the cached plan's waypoint slice with the decision's own observation,
    in the rollout, the eval and the update's replay alike, so the cache never stales them between replans. No value
    iteration is run for the waypoint (the aux pass still measures the classical planner). The join (waypoint_join) is
    Linear(17 -> out_width), a shape of its own: a checkpoint with the walk's eight-wide join (nav2 before revision 7)
    or none (nav1) seeds it fresh at zero (bootstrap._seed_map, load_actor_state), so the seeded policy computes what
    it did until the join learns. Zeroed by planner_off. The `waypoint_source` buffer reads 2 (the export writes it;
    the runtime computes the seventeen itself). The seventeen are not among the scalars the map's embedding reads
    (`embed_scalars`: the block's prefix), whatever the source: under "classical" or "learned" the policy never sees
    the sim's route, so the sources compare cleanly.

    **No compass in the observation** (`goal_blind`; the nav stages since 2026-10-17, which carry no compass block and
    no straight-line bearing to the goal anywhere in the observation): the goal plane the learned planner and the
    classical measurement plan from comes from the LABELS in the trainer's aux pass (`label_goal`, plan()'s override:
    the labels' goal_forward / goal_right yards, protocol 33), so the costmap head and the VIN keep learning and being
    measured against the route to the real goal. At act time nothing carries the goal, so under the sim's route with
    no layout that has a compass the VIN is not run: the readouts' value slice and the pointer's pool are zero and the
    seventeen route scalars are the plan (the exported runtime has to do the same: waypoint source 2 with every compass
    column none means zero readouts and a zero pointer bonus, never a value iteration over an empty goal plane). A
    layout WITH a compass (M1) plans at act time off it as before; a withheld compass (its presence column 0) is a zero
    goal plane as before.

    **The school's route** (mappo.planner_waypoint_source "school", `school_waypoint`; planner school stage 2,
    2026-10-17): the seventeen route scalars computed by the learner from the planner school's U-Net
    (school.model.SchoolPlanner, trained offline on the recorder's samples; mappo.school_checkpoint, loaded as the
    frozen submodule `school`: no gradient, left out of every optimizer, saved in the policy's checkpoint and
    exported with it) instead of read off the sim's Dijkstra -- the same seventeen columns, join and gating, so the
    policy's input does not change (ROUTE_POINT_YARDS: the rule on the 2-yd crop; route_from_fields). Its goal plane
    is the goal's forward and right yards from the map block's two goal columns (GOAL_COLUMNS, `goal_columns`; a
    layout without them refuses the source), its memory flag the block's kept scalar (`kept_column`), its inputs the
    crops the embedding decodes anyway. `school_route` runs it and `route_scalars` chooses: the school's seventeen,
    or the sim's columns as they are while `school_off` (the eval arm no_school: the same seeds on the sim's route)
    -- and always the sim's route_stall. It runs at ACT time alone, in MapEncoder.plan (MappoTrainer._replan calls
    it for every seat every mappo.school_every decisions, eager and outside the rollout graph), and its seventeen are
    the cached plan's waypoint slice, which forward() reads AS IT IS under the school -- the rollout, the evaluation
    and the update's replay all read the slice the decision was made with (the rollout buffer records it) and the
    U-Net is never run again on the update's rows; under "sim" forward() keeps overwriting the slice with the
    observation's.
    The sim's seventeen are left in plan_out's "sim_route" either way. In the aux pass (plan with `label_goal`) the
    waypoint slice is the sim's, not the school's: the losses never read it. 1.18 GFLOP a row (both tiers, widths
    32-128): 143 ms per 64 rows on the dev container's CPU at 8 threads, 532 ms per 192 (2026-10-17).

    **The hierarchy** (mappo.planner_hierarchical, `hierarchical`; 2026-10-16): the goal of a nav trip is mostly past
    the fine crop (96 yd across), where the fine goal plane was clamped to a border cell -- the wrong point -- and K
    iterations carried value only K cells. With the hierarchy the coarse tier plans first, as before; the fine tier's
    goal plane is the goal's one-hot only when the goal lies inside the fine crop (goal_plane without clamp) and the
    coarse value, detached, resampled bilinearly at the fine cells' centres and at the ring just past the fine border
    (coarse_frame), is the fine planner's boundary: its iterations start from it over the whole crop and hold the ring
    at it every iteration (Planner.forward's `init` and `ring`) in place of 0 and the zero padding. The fine value is
    then a K-step refinement of the coarse estimate at 2 yd -- the cost through the crop to the border plus the coarse
    plan beyond, where the goal is out of the crop; the goal cell itself where it is in -- the same quantity as its
    label (the route to the real goal), by construction rather than learned through another input plane. A boundary
    only at the ring would not do: an interior started at 0 out-ranks any boundary below -K x the step cost. Both
    tiers' values are bounded at 0 (Planner.forward's `bound`), so the coarse plan never hands the fine one a negative
    cost-to-go and neither tier runs away with more iterations. The coarse plan is detached: it has its own labels at
    every cell, and the shared planner's weights learn from the fine loss through the fine pass anyway. No parameter: a
    checkpoint without it loads as it is.

    **The join** (`join`, orthogonal): EMBED -> the camera's embedding width, after a SiLU, added to the camera's
    embedding before its own SiLU, so both networks read the map through their own camera join. Seeding from a
    checkpoint without a map zeroes it (bootstrap._seed_map): the seeded policy starts as it was, and the map comes in
    as the join learns."""

    CODE_EMBED = 4
    PATCH = 4
    WIDTHS = (64, 64)
    EMBED = 128
    #: The planner's readouts a tier: the value at the centre 3 x 3 cells (rows and columns READOUT_FIRST on).
    READOUT = 9
    READOUT_FIRST = 22
    #: The planned waypoint (mappo.planner_waypoint; the learned and classical walks): the walk's first cell (the
    #: readouts' centre), its moves a tier (fine, coarse) and its numbers a tier (bearing sin, cos, distance over the
    #: reach, valid). The sim's route is ROUTE_COLUMNS wide instead (waypoint_width).
    WAYPOINT_ANCHOR = READOUT_FIRST + 1
    WAYPOINT_STEPS = (5, 2)
    WAYPOINT_FEATURES = 4

    def __init__(self, crop: dict, class_embedding: nn.Embedding, out_width: int, starts: Sequence[int],
                 compass: Sequence[dict | None] = (), planner_k: int = Planner.K, splat: dict | None = None,
                 splat_features: int = 0, waypoint: bool = False, planner_k_fine: int | None = None,
                 hierarchical: bool = False, waypoint_source: str = "learned", waypoint_costs: str = "head",
                 school_checkpoint: str = ""):
        super().__init__()
        if waypoint_source not in WAYPOINT_SOURCES:
            raise ValueError(f"mappo.planner_waypoint_source: expected one of {list(WAYPOINT_SOURCES)}, got "
                             f"{waypoint_source!r}")
        if waypoint_costs not in WAYPOINT_COST_RULES:
            raise ValueError(f"mappo.planner_waypoint_costs: expected one of {list(WAYPOINT_COST_RULES)}, got "
                             f"{waypoint_costs!r}")
        self.height, self.width = crop["height"], crop["width"]
        if self.height % self.PATCH or self.width % self.PATCH:
            raise ValueError(f"the map's crop is {self.width} x {self.height}; its encoder cuts it into {self.PATCH} x "
                             f"{self.PATCH} patches")
        self.tiers = int(crop.get("tiers", 1))
        self.tier_cells = tuple(float(cell) for cell in crop.get("tier_cells", (crop.get("cell", 2.0),)))
        self.crop_bytes = self.height * self.width * MAP_CHANNELS
        # The block's scalars the embedding reads: all of them but the sim's route (ROUTE_COLUMNS, the block's last
        # seventeen; `embed_scalars`), which feeds the policy through the waypoint's join alone.
        self.map_bytes = crop["map_bytes"]
        self.scalars = int(crop.get("embed_scalars", crop["scalars"]))
        if self.map_bytes != self.crop_bytes * self.tiers or len(self.tier_cells) != self.tiers:
            raise ValueError(f"a map of {self.map_bytes} bytes is not {self.tiers} tier(s) of {self.crop_bytes}")
        self.code_embed = nn.Embedding(MAP_CODES, self.CODE_EMBED)
        # The camera's class embedding, held by reference: one table for the pixels, the list and the map.
        self.__dict__["class_embedding"] = class_embedding
        self.planes_per_cell = self.CODE_EMBED + class_embedding.embedding_dim + MAP_VALUE_PLANES
        self.grid = (self.height // self.PATCH, self.width // self.PATCH)
        self.patch = nn.Linear(self.PATCH * self.PATCH * self.planes_per_cell, self.WIDTHS[0])
        self.mix = nn.Linear(self.WIDTHS[0], self.WIDTHS[1])
        grid_y, grid_x = torch.meshgrid(torch.linspace(-1.0, 1.0, self.grid[0]),
                                        torch.linspace(-1.0, 1.0, self.grid[1]), indexing="ij")
        self.register_buffer("grid_x", grid_x.reshape(-1), persistent=False)
        self.register_buffer("grid_y", grid_y.reshape(-1), persistent=False)
        self.embed = nn.Linear(self.tiers * 3 * self.WIDTHS[1] + self.scalars, self.EMBED)
        self.join = _linear(self.EMBED, out_width, math.sqrt(2))
        # Each layout's first map column (the scalars), -1 for a layout without the map.
        self.register_buffer("start", torch.tensor(list(starts), dtype=torch.long), persistent=False)
        self.register_buffer("offsets", torch.arange(self.scalars, dtype=torch.long), persistent=False)
        # The two-tier map's extras (none, and no new parameter, on a one-tier map: a checkpoint of one loads as it is).
        self.planner_off = False
        self.compass_scale = COMPASS_SCALE
        if self.tiers > 1:
            self.tier_embed = nn.Parameter(torch.zeros(self.tiers, self.WIDTHS[1]))
            splat_planes = CameraSplat.CHANNELS + 1 if splat is not None else 0
            self.costmap = CostmapHead(self.planes_per_cell, out_width, self.tiers, splat_planes)
            self.planner = Planner(TRAV_CLASSES + self.planes_per_cell + 1, planner_k, planner_k_fine)
            self.planner_join = _linear(self.tiers * self.READOUT, out_width, math.sqrt(2))
            # Per layout the compass block's columns (present, bearing sin, cos, distance; compass_of), -1 without:
            # the goal plane reads them. Derived from stage.json, not learned.
            columns = [[c["present"], c["sin"], c["cos"], c["dist"]] if c else [-1, -1, -1, -1] for c in compass]
            self.register_buffer("compass", torch.tensor(columns or [[-1] * 4], dtype=torch.long), persistent=False)
            scales = [c["scale"] for c in compass if c]
            self.compass_scale = float(scales[0]) if scales else COMPASS_SCALE
        # The camera splat (mappo.camera_splat; `splat` the camera's image descriptor, vision_of): the camera's patch
        # features laid on both tiers' cells for the costmap head. Built last and only when asked, so a map without it
        # has exactly the parameters and the random draws it had. Nothing reads it on a one-tier map (no costmap head).
        self.splat = CameraSplat(splat, splat_features) if splat is not None and self.tiers > 1 else None
        # The planned waypoint (mappo.planner_waypoint): its join, at zero, built last and only when asked, so a map
        # without it has exactly the parameters and the random draws it had. Its width is the walk's (tiers x
        # WAYPOINT_FEATURES) or, under the sim's route, ROUTE_COLUMNS' (waypoint_width): the sources' checks and the
        # flag come first, the join after them.
        self.waypoint = bool(waypoint) and self.tiers > 1
        # The seventeen route scalars' layout (ROUTE_SOURCES: the sim's route, and the school's, which reads the sim's
        # columns too); the school's own flag after it.
        self.sim_waypoint = self.waypoint and waypoint_source in ROUTE_SOURCES
        self.school_waypoint = self.waypoint and waypoint_source == "school"
        self.waypoint_join = None
        if self.waypoint:
            self.waypoint_join = nn.Linear(self.waypoint_width, out_width)
            nn.init.zeros_(self.waypoint_join.weight)
            nn.init.zeros_(self.waypoint_join.bias)
        # The hierarchy (mappo.planner_hierarchical; plan): no parameter, a flag buffer (the export reads it) built only
        # when asked, and where each fine cell of the crop and of the ring just past it lies on the coarse tier
        # (grid_sample's normalised x = column, y = row, align_corners: -1 and 1 the coarse crop's first and last
        # cells' centres). Both tiers are heading-up and centred on the body: fine cell i's centre is (c - i) x fine
        # yards ahead, c = (side - 1) / 2, which is coarse cell c - (c - i) x fine / coarse.
        self.hierarchical = bool(hierarchical) and self.tiers == 2
        if self.hierarchical:
            self.register_buffer("hierarchy", torch.tensor(1, dtype=torch.long))
            ratio = self.tier_cells[0] / self.tier_cells[1]
            rows = (self.height - 1) / 2.0 + (torch.arange(-1, self.height + 1, dtype=torch.float32)
                                              - (self.height - 1) / 2.0) * ratio
            cols = (self.width - 1) / 2.0 + (torch.arange(-1, self.width + 1, dtype=torch.float32)
                                             - (self.width - 1) / 2.0) * ratio
            y, x = torch.meshgrid(rows / (self.height - 1) * 2.0 - 1.0, cols / (self.width - 1) * 2.0 - 1.0,
                                  indexing="ij")
            self.register_buffer("coarse_grid", torch.stack([x, y], dim=-1)[None], persistent=False)
        # The classical waypoint (mappo.planner_waypoint_source; classical_plan): a flag buffer (the export reads it)
        # and the step costs' table, built only when asked, so a map without it is exactly what it was.
        self.classical_waypoint = self.waypoint and waypoint_source == "classical"
        # ... and over the map's codes (mappo.planner_waypoint_costs "map"; classical_factor): its flag buffer and
        # the rule's tables, likewise built only when asked.
        self.map_costs = self.classical_waypoint and waypoint_costs == "map"
        if self.classical_waypoint:
            self.register_buffer("waypoint_source", torch.tensor(WAYPOINT_SOURCES.index("classical"), dtype=torch.long))
            for name, table in classical_tables().items():
                self.register_buffer(f"classical_{name}", table, persistent=False)
        if self.map_costs:
            self.register_buffer("waypoint_costs", torch.tensor(WAYPOINT_COST_RULES.index("map"), dtype=torch.long))
        # The sim's route (mappo.planner_waypoint_source "sim"; sim_waypoints): its flag buffer and, per layout, the
        # route columns (ROUTE_COLUMNS order; -1 for a layout without them), derived from stage.json, not learned, and
        # each column's valid gate (ROUTE_GROUPS: its own index, or the column itself where it is read as it is).
        self.goal_blind = False
        if self.sim_waypoint:
            routes = list(crop.get("routes") or ())
            if not any(route is not None for route in routes):
                raise ValueError("mappo.planner_waypoint_source sim: no layout's map block names the route columns "
                                 f"{list(ROUTE_COLUMNS)} (map block revision {MAP_BLOCK_REVISION}: the sim is older)")
            columns = [route["columns"] if route is not None else [-1] * len(ROUTE_COLUMNS) for route in routes]
            self.register_buffer("route", torch.tensor(columns, dtype=torch.long), persistent=False)
            gates, gated = [0] * len(ROUTE_COLUMNS), [False] * len(ROUTE_COLUMNS)
            for group, gate in ROUTE_GROUPS:
                for column in group:
                    gates[column], gated[column] = (column, False) if gate is None else (gate, True)
            self.register_buffer("route_gate", torch.tensor(gates, dtype=torch.long), persistent=False)
            self.register_buffer("route_gated", torch.tensor(gated, dtype=torch.bool), persistent=False)
            self.register_buffer("waypoint_source", torch.tensor(WAYPOINT_SOURCES.index(waypoint_source),
                                                                 dtype=torch.long))
            # No layout carries a compass: the VIN has no goal at act time (the class docstring), so it is not run
            # there; the aux pass plans from the labels' goal.
            self.goal_blind = self.tiers > 1 and not any(c for c in compass)
        # The school's route (mappo.planner_waypoint_source "school"; school_route): per layout the goal columns
        # (GOAL_COLUMNS order; -1 without) and the kept scalar's column (-1 without), derived from stage.json, and the
        # school planner itself, loaded frozen from mappo.school_checkpoint (the class docstring).
        self.school = None
        self.school_off = False
        if self.school_waypoint:
            goals = list(crop.get("goals") or ())
            if not any(goal is not None for goal in goals):
                raise ValueError("mappo.planner_waypoint_source school: no layout's map block names the goal columns "
                                 f"{list(GOAL_COLUMNS)} (the objective's forward and right yards in the body frame, "
                                 f"the two scalars before the route's {len(ROUTE_COLUMNS)}: the sim has to write "
                                 f"them for the school planner's goal plane)")
            columns = [goal["columns"] if goal is not None else [-1] * len(GOAL_COLUMNS) for goal in goals]
            self.register_buffer("goal_columns", torch.tensor(columns, dtype=torch.long), persistent=False)
            kept = [int(column) for column in (crop.get("kept") or [-1] * len(columns))]
            self.register_buffer("kept_column", torch.tensor(kept, dtype=torch.long), persistent=False)
            self.school = load_school(school_checkpoint, self.height, self.width, self.tier_cells)

    @property
    def school_widths(self) -> tuple[int, ...]:
        """The school U-Net's level widths (the export writes them), () without a school."""
        return tuple(self.school.widths) if self.school is not None else ()

    @property
    def classical_rule_tables(self) -> dict[str, torch.Tensor]:
        """The classical planner's tables (classical_tables()) as this encoder's buffers, on its device."""
        return {name: getattr(self, f"classical_{name}") for name in ("costs", "floor_costs", "code_rules",
                                                                       "code_costs")}

    @property
    def planner_on(self) -> bool:
        return self.tiers > 1

    @property
    def waypoint_width(self) -> int:
        """The waypoint's numbers a row: ROUTE_COLUMNS' seventeen under the sim's route, else the walk's tiers x
        WAYPOINT_FEATURES."""
        return len(ROUTE_COLUMNS) if self.sim_waypoint else self.tiers * self.WAYPOINT_FEATURES

    def waypoints(self, cost_to_go: list[torch.Tensor], present: torch.Tensor) -> torch.Tensor:
        """The planned waypoint of every tier (the class docstring), [N, tiers x WAYPOINT_FEATURES], from each tier's
        cost-to-go [N, H, W] (yards) and whether the row has a goal [N]. Without a gradient."""
        with torch.no_grad():
            centre = (self.height - 1) / 2.0
            features = []
            for tier, (cost, cell) in enumerate(zip(cost_to_go, self.tier_cells)):
                steps = self.WAYPOINT_STEPS[min(tier, len(self.WAYPOINT_STEPS) - 1)]
                row, col = descend(cost.detach(), self.WAYPOINT_ANCHOR, steps)
                moved = ((row != self.WAYPOINT_ANCHOR) | (col != self.WAYPOINT_ANCHOR)) & present
                forward = (centre - row.to(torch.float32)) * cell
                right = (col.to(torch.float32) - centre) * cell
                distance = torch.sqrt(forward * forward + right * right).clamp(min=1e-6)
                valid = moved.to(torch.float32)
                features.append(torch.stack([-right / distance * valid, forward / distance * valid,
                                             distance / (steps * cell) * valid, valid], dim=-1))
            return torch.cat(features, dim=-1)

    def sim_waypoints(self, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """The sim's route (mappo.planner_waypoint_source "sim"; the class docstring), [N, len(ROUTE_COLUMNS)] float32,
        read off each row's observation at its layout's route columns (ROUTE_COLUMNS order), each group times its
        valid flag (ROUTE_GROUPS: the fine ten by route_fine_valid, the coarse four by route_coarse_valid, route_dist by
        route_dist_valid; route_dist_valid and route_stall as they are -- the sim sends 0 where there is no route, the
        product keeps a stray value out of the join); 0 for a layout without the columns. Without a gradient."""
        with torch.no_grad():
            layout = layout.reshape(-1).long()
            columns = self.route[layout.clamp(max=self.route.shape[0] - 1)]
            has = (columns[:, :1] >= 0).to(torch.float32)
            picked = obs.gather(1, columns.clamp(min=0).clamp(max=obs.shape[-1] - 1)).to(torch.float32)
            valid = ((picked[:, self.route_gate] > 0.5) | ~self.route_gated[None, :]).to(torch.float32)
            return picked * valid * has

    def route_scalars(self, obs: torch.Tensor, layout: torch.Tensor,
                      decoded: list[dict[str, torch.Tensor]]) -> tuple[torch.Tensor, torch.Tensor]:
        """The waypoint's seventeen route scalars and the sim's (the class docstring's "The school's route"), [N, 17]
        float32 each: under the school (`school_waypoint`, and not `school_off`) the school's computed from the crops
        (school_route), else the sim's columns (sim_waypoints) twice over."""
        sim_route = self.sim_waypoints(obs, layout)
        if not self.school_waypoint or self.school_off:
            return sim_route, sim_route
        return self.school_route(obs, layout, decoded, sim_route), sim_route

    def school_route(self, obs: torch.Tensor, layout: torch.Tensor, decoded: list[dict[str, torch.Tensor]],
                     sim_route: torch.Tensor) -> torch.Tensor:
        """The school's seventeen (route_from_fields over the frozen school U-Net's fields; ROUTE_POINT_YARDS, the
        rule), [N, 17] float32. The goal: each row's goal columns (GOAL_COLUMNS; present where either is not 0 and the
        layout has them), as the hierarchy's goal planes (goal_plane: the fine unclamped, the coarse clamped to its
        border -- school.data.prepare's); the memory flag the kept column (0 without one); `sim_route` for
        route_stall. Without a gradient and outside autocast (the fields in float32, as the school was measured)."""
        with torch.no_grad(), torch.autocast(device_type=obs.device.type, enabled=False):
            layout = layout.reshape(-1).long()
            columns = self.goal_columns[layout.clamp(max=self.goal_columns.shape[0] - 1)]
            has = columns[:, 0] >= 0
            picked = obs.gather(1, columns.clamp(min=0).clamp(max=obs.shape[-1] - 1)).to(torch.float32)
            forward = torch.where(has, picked[:, 0], torch.zeros_like(picked[:, 0]))
            right = torch.where(has, picked[:, 1], torch.zeros_like(picked[:, 1]))
            present = has & ((forward != 0.0) | (right != 0.0))
            kept = self.kept_column[layout.clamp(max=self.kept_column.shape[0] - 1)]
            flag = obs.gather(1, kept.clamp(min=0).clamp(max=obs.shape[-1] - 1)[:, None])[:, 0].to(torch.float32)
            memory = torch.where(kept >= 0, (flag > 0.5).to(torch.float32), torch.zeros_like(flag))
            side = self.height
            planes = [goal_plane(forward, right, present, side, self.tier_cells[0], clamp=False),
                      goal_plane(forward, right, present, side, self.tier_cells[1], clamp=True)]
            fields = self.school(decoded, planes, memory)
            return route_from_fields(fields["cost"][0], fields["reach"][0], fields["cost"][1], fields["reach"][1],
                                     present, sim_route[:, ROUTE_COLUMNS.index("route_stall")], self.tier_cells,
                                     self.WAYPOINT_ANCHOR)

    def classical_plan(self, logits: list[torch.Tensor], goals: list[torch.Tensor],
                       codes: list[torch.Tensor] | None = None,
                       heights: list[torch.Tensor] | None = None) -> list[torch.Tensor]:
        """The classical waypoint's cost-to-go of every tier (mappo.planner_waypoint_source "classical"), [N, H, W]
        yards a tier (CLASSICAL_SENTINEL where nothing reached): classical_cost_to_go over each tier's expected step
        cost under the costmap head's softmax (`logits` [N, C, H, W] a tier, priced by CLASSICAL_COSTS) from the plan's
        goal planes (`goals` [N, H, W] a tier), CLASSICAL_WAYPOINT_ITERATIONS fixed iterations (no host sync); with the
        hierarchy the coarse tier first and its cost-to-go resampled (coarse_frame) as the fine tier's ring. With
        mappo.planner_waypoint_costs "map" (`map_costs`) the step costs are priced on the crop's own codes (`codes`
        [N, H, W] a tier, decode_map's; classical_factor) where the crop knows a cell, a known floor off the height
        band (`heights` [N, H, W] a tier, the crop's height bytes; CLASSICAL_HEIGHT_BAND) as unknown. Without a
        gradient; reads no random state."""
        with torch.no_grad():
            tier_count = len(logits)
            planned: list[torch.Tensor | None] = [None] * tier_count
            tables = self.classical_rule_tables
            if self.map_costs and codes is None:
                raise ValueError("the classical waypoint prices the map's codes (mappo.planner_waypoint_costs map): "
                                 "classical_plan needs each tier's codes")
            for tier in (reversed(range(tier_count)) if self.hierarchical else range(tier_count)):
                probabilities = torch.softmax(logits[tier].detach().to(torch.float32), dim=1)
                factor = classical_factor(probabilities, tables, codes[tier] if self.map_costs else None,
                                          heights[tier] if self.map_costs and heights is not None else None)
                boundary = self.coarse_frame(planned[1]) if self.hierarchical and tier == 0 else None
                planned[tier], _ = classical_cost_to_go(
                    factor, goals[tier].detach(), self.tier_cells[tier],
                    CLASSICAL_WAYPOINT_ITERATIONS[min(tier, len(CLASSICAL_WAYPOINT_ITERATIONS) - 1)], boundary,
                    check=False)
            return planned

    def coarse_frame(self, coarse: torch.Tensor) -> torch.Tensor:
        """The hierarchy's boundary (mappo.planner_hierarchical): the coarse tier's value [N, H, W], detached,
        resampled bilinearly at the centres of the fine tier's cells and of the ring of cells just past its border
        (coarse_grid), [N, H + 2, W + 2] float32: the fine planner starts from its interior and holds its ring."""
        n = coarse.shape[0]
        grid = self.coarse_grid.expand(n, -1, -1, -1)
        return nn.functional.grid_sample(coarse.detach().to(torch.float32)[:, None], grid, mode="bilinear",
                                         padding_mode="border", align_corners=True)[:, 0]

    def join_plan(self, readout: torch.Tensor) -> torch.Tensor:
        """The cached plan [N, readout_width] into the embedding: the readouts through planner_join and, with the
        waypoint, its numbers (the last waypoint_width) through waypoint_join."""
        width = self.tiers * self.READOUT
        dtype = self.planner_join.weight.dtype
        out = self.planner_join(readout[:, :width].to(dtype))
        if self.waypoint_join is not None:
            out = out + self.waypoint_join(readout[:, width:].to(dtype))
        return out

    def planes(self, crops: torch.Tensor) -> torch.Tensor:
        """[N, M] bytes of one tier -> [N, H, W, P] planes: the code's embedding, the class's, the seven values."""
        return self.planes_of(decode_map(crops, self.height, self.width))

    def planes_of(self, decoded: dict[str, torch.Tensor]) -> torch.Tensor:
        """planes() from a tier's decode_map."""
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

    def goal(self, obs: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """The goal in the body frame off each row's compass block: (forward yards, right yards, present) [N] each;
        0 and absent for a layout without a compass (the nav stages: the aux pass plans from the labels' goal instead,
        plan()'s `label_goal`) or a withheld one (its presence column 0). WoW's yaw turns left, so a positive bearing is
        to the left: right = -distance x sin(bearing)."""
        layout = layout.reshape(-1).long()
        columns = self.compass[layout.clamp(max=self.compass.shape[0] - 1)]
        has = columns[:, 0] >= 0
        picked = obs.gather(1, columns.clamp(min=0).clamp(max=obs.shape[-1] - 1)).to(torch.float32)
        present = has & (picked[:, 0] > 0.5)
        distance = picked[:, 3] * self.compass_scale
        forward = torch.where(present, distance * picked[:, 2], torch.zeros_like(distance))
        right = torch.where(present, -distance * picked[:, 1], torch.zeros_like(distance))
        return forward, right, present

    def plan(self, obs: torch.Tensor, layout: torch.Tensor, decoded: list[dict[str, torch.Tensor]],
             planes: list[torch.Tensor], camera: torch.Tensor, plan_out: dict | None = None,
             splat: tuple[torch.Tensor, torch.Tensor, torch.Tensor] | None = None,
             classical: bool = True,
             label_goal: tuple[torch.Tensor, torch.Tensor, torch.Tensor] | None = None) -> torch.Tensor:
        """The costmap head and the planner over both tiers: the readouts [N, readout_width], detached (the caller
        joins them; forward). With `plan_out` (a dict) the parts are left in it: "costmap" (logits
        [N, C, H, W] a tier), "cost_to_go" (-value x 100, yards, [N, H, W] a tier), "value", "goal_plane", "readout"
        [N, readout_width], with mappo.planner_waypoint "waypoint" [N, waypoint_width] (the readout's last columns,
        before planner_off; with mappo.planner_waypoint_source "classical" walked over "classical_cost_to_go",
        classical_plan's, a tier; with "sim" the observation's route columns, sim_waypoints), and "pool"
        [N, (H / CELL_POOL) x (W / CELL_POOL)], the fine tier's value
        averaged over the cell pointer's blocks (its bonus). Computed under the caller's grad mode: the trainer's aux
        pass keeps the graph, every other call runs it without one (the readouts are detached either way). `splat` (the
        camera's
        patch features [N, patches, F], its image bytes [N, image bytes] and its scalars [N, S]; VisionEncoder.forward)
        feeds the camera splat where the map has one (CameraSplat), its planes in plan_out's "splat" a tier. Without
        `classical` the classical waypoint (mappo.planner_waypoint_source "classical") is not planned and its columns
        are zero: the aux pass (forward's `grad`), whose losses read the costmap and the value and never the readouts,
        skips its value iteration. `label_goal` ((forward yards, right yards, present) [N] each, the labels' goal; the
        aux pass) plans from that goal instead of the compass's. Without one on a `goal_blind` map (the class
        docstring) nothing is planned: the readouts' value slice and the pool are zero, the waypoint the sim's, and
        plan_out's per-tier lists are empty."""
        forward, right, present = label_goal if label_goal is not None else self.goal(obs, layout)
        side = self.height
        c = self.READOUT_FIRST
        out = {"costmap": [], "cost_to_go": [], "value": [], "goal_plane": []}
        if self.goal_blind and label_goal is None:
            waypoint, sim_route = self.route_scalars(obs, layout, decoded)
            readout = torch.cat([obs.new_zeros((obs.shape[0], self.tiers * self.READOUT), dtype=torch.float32),
                                 waypoint], dim=-1)
            pool = obs.new_zeros((obs.shape[0], self.pool_width), dtype=torch.float32)
            if self.planner_off:
                readout = torch.zeros_like(readout)
            out.update(waypoint=readout[:, self.tiers * self.READOUT:], readout=readout, pool=pool,
                       sim_route=sim_route)
            if plan_out is not None:
                plan_out.update(out)
            return out["readout"]
        splats: list[torch.Tensor | None] = [None] * len(planes)
        if self.splat is not None:
            if splat is None:
                raise ValueError("the map's costmap head reads the camera splat: the camera's patch features, image "
                                 "and scalars have to come with the crop (VisionEncoder.forward)")
            splats = self.splat(*splat, side, self.tier_cells)
            out["splat"] = splats
        tiers = list(zip(planes, self.tier_cells))
        parts: list[tuple | None] = [None] * len(tiers)
        # The hierarchy (mappo.planner_hierarchical): the coarse tier first, its plan the fine tier's boundary.
        for tier in (reversed(range(len(tiers))) if self.hierarchical else range(len(tiers))):
            planes_tier, cell = tiers[tier]
            nchw = planes_tier.permute(0, 3, 1, 2)
            logits = self.costmap(nchw, camera, tier, splats[tier])
            fine = self.hierarchical and tier == 0
            plane = goal_plane(forward, right, present, side, cell, clamp=not fine)
            inputs = torch.cat([torch.softmax(logits.to(torch.float32), dim=1), nchw.to(torch.float32),
                                plane[:, None]], dim=1)
            init = ring = None
            if fine:
                ring = self.coarse_frame(parts[1][1])
                init = ring[:, 1:-1, 1:-1]
            value = self.planner(inputs, self.planner.tier_iterations(tier), init, ring,
                                 bound=self.hierarchical).to(torch.float32)
            parts[tier] = (logits, value, plane)
        readouts = []
        for logits, value, plane in parts:
            readouts.append(value[:, c:c + 3, c:c + 3].reshape(value.shape[0], -1))
            out["costmap"].append(logits)
            out["cost_to_go"].append(-value * 100.0)
            out["value"].append(value)
            out["goal_plane"].append(plane)
        if self.waypoint:
            # The planned waypoint (mappo.planner_waypoint): after the readouts, cached and zeroed with them; walked
            # over the classical planner's cost-to-go with mappo.planner_waypoint_source "classical" (lower is nearer,
            # as the learned -value x 100 is: descend reads either as it is).
            if self.sim_waypoint:
                # The aux pass (no `classical`: the losses never read the waypoint) keeps the sim's columns under
                # the school too, so the school U-Net never runs on the update's rows.
                out["waypoint"], out["sim_route"] = (self.route_scalars(obs, layout, decoded) if classical
                                                     else (self.sim_waypoints(obs, layout),) * 2)
            elif not self.classical_waypoint:
                out["waypoint"] = self.waypoints(out["cost_to_go"], present)
            elif classical:
                out["classical_cost_to_go"] = self.classical_plan(out["costmap"], out["goal_plane"],
                                                                  [tier["code"] for tier in decoded],
                                                                  [tier["height"] for tier in decoded])
                out["waypoint"] = self.waypoints(out["classical_cost_to_go"], present)
            else:
                out["waypoint"] = present.new_zeros((present.shape[0], self.waypoint_width), dtype=torch.float32)
            readouts.append(out["waypoint"])
        readout = torch.cat(readouts, dim=-1)
        pool = nn.functional.avg_pool2d(out["value"][0][:, None], CELL_POOL)[:, 0].reshape(readout.shape[0], -1)
        if self.planner_off:
            readout = torch.zeros_like(readout)
            pool = torch.zeros_like(pool)
        out["readout"], out["pool"] = readout.detach(), pool.detach()
        if plan_out is not None:
            plan_out.update(out)
        return out["readout"]

    @property
    def readout_width(self) -> int:
        """The planner's readouts a row (tiers x READOUT, and the planned waypoint's waypoint_width after them with
        mappo.planner_waypoint), 0 without a planner."""
        if not self.planner_on:
            return 0
        return self.tiers * self.READOUT + (self.waypoint_width if self.waypoint else 0)

    @property
    def pool_width(self) -> int:
        """The pointer's bonus a row (the fine tier pooled CELL_POOL x CELL_POOL), 0 without a planner."""
        return (self.height // CELL_POOL) * (self.width // CELL_POOL) if self.planner_on else 0

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, crops: torch.Tensor,
                camera: torch.Tensor | None = None, plan_out: dict | None = None, grad: bool = False,
                planner: dict | None = None,
                splat: tuple[torch.Tensor, torch.Tensor, torch.Tensor] | None = None,
                label_goal: tuple[torch.Tensor, torch.Tensor, torch.Tensor] | None = None) -> torch.Tensor:
        """What the map adds to the camera's embedding (before its SiLU), [N, out_width]. `camera` [N, out_width] is
        the camera's own embedding (the costmap head reads it; None skips the planner), `plan_out` and `grad` as
        plan()'s: the planner runs without a graph unless `grad` (the aux pass, which also skips the classical
        waypoint: plan's `classical`). `planner` ({"readout" [N, readout_width], "pool"
        [N, pool_width]}: the rows' cached plan, MappoTrainer's acting state) is read instead of running it -- the
        planner runs every mappo.planner_every decisions and its readouts are reused in between. `splat`: the camera
        splat's inputs (plan()). `label_goal`: the labels' goal the plan is made from (plan(); the aux pass)."""
        silu = nn.functional.silu
        layout = layout.reshape(-1).long()
        columns = (self.start[layout].clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        scalars = obs.gather(1, columns).to(self.embed.weight.dtype)
        decoded = decode_map_tiers(crops, self.height, self.width, self.tiers)
        planes = [self.planes_of(tier) for tier in decoded]
        pooled = []
        for tier, tier_planes in enumerate(planes):
            features = silu(self.mix(silu(self.patch(self.patches(tier_planes)))))     # [N, patches, C]
            if self.tiers > 1:
                features = features + self.tier_embed[tier].to(features.dtype)
            grid = features.transpose(1, 2).reshape(features.shape[0], -1, *self.grid)
            pooled += [_spatial_softmax(grid, self.grid_x, self.grid_y), features.mean(dim=1)]
        out = self.join(silu(self.embed(torch.cat([*pooled, scalars], dim=-1))))
        if self.planner_on and planner is not None:
            readout = planner["readout"].to(torch.float32)
            sim_route = self.sim_waypoints(obs, layout) if self.sim_waypoint else None
            if self.sim_waypoint and not self.school_waypoint:
                # The sim's waypoint is this decision's observation's, never the cache's (replanned every
                # planner_every decisions): the cached slice is overwritten here, in the rollout and the replay alike.
                # Under the school the cached slice IS this decision's (plan() ran for every seat, mappo.school_every)
                # and the replay reads the slice recorded with the decision (the class docstring).
                readout = torch.cat([readout[:, :self.tiers * self.READOUT], sim_route], dim=-1)
            if self.planner_off:
                readout = torch.zeros_like(readout)
            if plan_out is not None:
                pool = planner["pool"].to(torch.float32)
                plan_out.update(readout=readout, pool=torch.zeros_like(pool) if self.planner_off else pool)
                if sim_route is not None:
                    plan_out["sim_route"] = sim_route
            out = out + self.join_plan(readout).to(out.dtype)
        elif self.planner_on and camera is not None:
            # The planner without a graph unless asked (the aux pass): its readouts are detached either way, and the
            # join learns from the return outside the no_grad.
            with torch.set_grad_enabled(grad and torch.is_grad_enabled()):
                readout = self.plan(obs, layout, decoded, planes, camera, plan_out, splat, classical=not grad,
                                    label_goal=label_goal)
            out = out + self.join_plan(readout).to(out.dtype)
        return out


class CoverageEncoder(nn.Module):
    """**The coarse coverage grid's encoder** (general search sec 5; a module on VisionEncoder as the map's is): the
    coverage block's columns, read raw by each layout's `first` (the MapEncoder gather pattern), reshaped to
    [N, channels, grid, grid] from the block's [row][col][channel] order; Conv2d(channels -> 16, 3, padding 1) SiLU,
    Conv2d(16 -> 16, 3, stride 2, padding 1) SiLU (a 12 x 12 grid becomes 6 x 6), flattened and Linear -> 64 SiLU, then
    `join` (orthogonal, gain sqrt 2) onto the camera's embedding width, added to it before its SiLU exactly as the
    map's join is. Seeding from a checkpoint without the block zeroes the join (bootstrap._seed_coverage): the seeded
    policy starts as it was, and the grid comes in as the join learns. A layout without the block has `first` -1 and
    contributes nothing (its term is multiplied by 0). Every shape is fixed and nothing branches on data, so a rollout
    graph captures it."""

    CONV = 16
    HIDDEN = 64

    def __init__(self, grid: dict, out_width: int, starts: Sequence[int]):
        super().__init__()
        self.grid, self.channels, self.width = int(grid["grid"]), int(grid["channels"]), int(grid["width"])
        self.conv1 = nn.Conv2d(self.channels, self.CONV, 3, padding=1)
        self.conv2 = nn.Conv2d(self.CONV, self.CONV, 3, stride=2, padding=1)
        pooled = (self.grid + 1) // 2
        self.embed = nn.Linear(self.CONV * pooled * pooled, self.HIDDEN)
        self.join = _linear(self.HIDDEN, out_width, math.sqrt(2))
        # Each layout's first coverage column, -1 for a layout without the block.
        self.register_buffer("start", torch.tensor(list(starts), dtype=torch.long), persistent=False)
        self.register_buffer("offsets", torch.arange(self.width, dtype=torch.long), persistent=False)

    def forward(self, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """What the coverage grid adds to the camera's embedding (before its SiLU), [N, out_width]."""
        silu = nn.functional.silu
        layout = layout.reshape(-1).long()
        start = self.start[layout]
        columns = (start.clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        cells = obs.gather(1, columns).to(self.embed.weight.dtype)
        image = cells.reshape(-1, self.grid, self.grid, self.channels).permute(0, 3, 1, 2)
        features = silu(self.conv2(silu(self.conv1(image)))).flatten(1)
        out = self.join(silu(self.embed(features)))
        return out * (start >= 0).to(out.dtype)[:, None]


class VisibleEntities(nn.Module):
    """**The entity list** (perception-goals 1b; entity sensing): the entities block's slots read as a set -- one
    encoder shared by every slot and layout, the present slots pooled (mean and max) -- with a change to how a slot is
    encoded: the class and the type are embedded, never one-hot (amendment 10): the class through the camera's own
    class embedding (one table for the list and the map), the type -- the creature or game object entry, raw in its
    column -- hashed into TYPE_EMBED-wide buckets, (entry x 2 + is object) mod the manifest's type_buckets; the
    slot's other columns go in as they are, among them the line of sight (los) and the angular width and height the
    sensor measured (revision 2). The list is geometric, so a slot reads the same at every render size.
    The pool (Linear onto the camera's embedding width) is added to the camera's embedding; a layout without a list
    adds zero. The sight list (SightEntities) shares the encoder and points at the slots."""

    TYPE_EMBED = 8

    def __init__(self, descriptors: Sequence[dict | None], class_embedding: nn.Embedding, out_width: int,
                 embed: int = 64):
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
        """Slots' tokens [..., embed] from their entity-list columns [..., width] alone: the class and the
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

    def encode(self, obs: torch.Tensor, layout: torch.Tensor) -> dict[str, tuple[torch.Tensor, torch.Tensor]]:
        """The slots' tokens [N, slots, embed] and which are present."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns_visible[layout]).reshape(rows, self.slots, self.width_)
        codes = self.token(raw)
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

    def tokens(self, obs: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The slots' tokens [N, slots, embed] and which are present [N, slots], from the observation alone (the
        pointers read the list so too)."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns[layout]).reshape(rows, self.slots, self.width_)
        dtype = self.pool.weight.dtype
        codes = self.shared.token(raw[..., : self.base])
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


class MemoryEntities(nn.Module):
    """**What the seat remembers and does not see now** (object memory, 2026-10-18; the memory_entities block): its
    slots -- the object memory's most relevant entries not in this frame's list -- read by the entity list's own
    encoder (VisibleEntities.token, shared by reference: a remembered door is tokenised as a visible one, its columns
    the entity list's from the frame's camera to where it was, los 0), each token plus a projection of the memory's
    columns (age, sightings, absent, kind, open / used / dead, the body-frame bearing and distance, height, trace
    span). The present slots are pooled (mean and max) onto the camera's embedding width; seeding zeroes the pool when
    the checkpoint has none (bootstrap._seed_memory_entities), so a seeded policy acts exactly as it did and the memory
    comes in as the pool learns. A layout without the block adds zero; a stage without it builds no module."""

    def __init__(self, descriptors: Sequence[dict | None], visible: "VisibleEntities", out_width: int,
                 embed: int = 64):
        super().__init__()
        spec = next(entry for entry in descriptors if entry is not None)
        for entry in descriptors:
            if entry is not None and any(entry[key] != spec[key] for key in ("slots", "width", "base_width")):
                raise ValueError(f"the remembered entities of the layouts differ ({entry} against {spec}): one "
                                 f"encoder reads them all")
        if spec["base_width"] != visible.width_:
            raise ValueError(f"the remembered entities lead with {spec['base_width']} columns, the entity list's "
                             f"tokens read {visible.width_}")
        self.slots, self.width_, self.base = spec["slots"], spec["width"], spec["base_width"]
        self.__dict__["shared"] = visible
        self.extra = _linear(self.width_ - self.base, embed, 1.0)
        self.pool = _linear(2 * embed, out_width, 1.0)
        layouts = len(descriptors)
        columns = torch.zeros(layouts, self.slots * self.width_, dtype=torch.long)
        present = torch.full((layouts, self.slots), -1, dtype=torch.long)
        self.blind = {}
        for index, entry in enumerate(descriptors):
            if entry is None:
                continue
            start = entry["first"]
            columns[index] = torch.arange(start, start + self.slots * self.width_)
            present[index] = start + torch.arange(self.slots) * self.width_ + entry["present"]
            self.blind[index] = list(range(start, start + self.slots * self.width_))
        # Derived from stage.json, not learned: kept out of the state dict.
        self.register_buffer("columns", columns, persistent=False)
        self.register_buffer("present_at", present, persistent=False)
        self.register_buffer("has_memory", present[:, 0] >= 0, persistent=False)

    def tokens(self, obs: torch.Tensor, layout: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """The slots' tokens [N, slots, embed] and which are present [N, slots]."""
        rows = obs.shape[0]
        layout = layout.reshape(-1).long()
        raw = obs.gather(1, self.columns[layout]).reshape(rows, self.slots, self.width_)
        dtype = self.pool.weight.dtype
        codes = self.shared.token(raw[..., : self.base]) + self.extra(raw[..., self.base:].to(dtype))
        present_at = self.present_at[layout]
        present = (present_at >= 0) & (obs.gather(1, present_at.clamp(min=0)) > 0.5)
        return codes, present

    def forward(self, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor:
        """What the remembered entities add to the camera's embedding: [N, out], zero for a layout without them and for
        a row with none present."""
        codes, present = self.tokens(obs, layout)
        weight = present.to(codes.dtype)[..., None]
        mean = (codes * weight).sum(dim=1) / weight.sum(dim=1).clamp(min=1.0)
        peak = torch.where(present[..., None], codes, torch.full_like(codes, -1.0)).amax(dim=1)
        any_present = present.any(dim=1, keepdim=True)
        peak = torch.where(any_present, peak, torch.zeros_like(peak))
        out = self.pool(torch.cat([mean, peak], dim=-1)) * any_present.to(codes.dtype)
        return out * self.has_memory[layout.reshape(-1).long()][:, None].to(out.dtype)


class SightPointers(nn.Module):
    """The sight list's pointer queries (SightEntities.with_pointers): one per press, a Linear from the actor's
    features to the token width, initialised small as the other pointer queries are. The actor's own (the critic
    acts on nothing)."""

    def __init__(self, presses: Sequence[str], head_width: int, embed: int = 64):
        super().__init__()
        self.queries = nn.ModuleDict({press: _linear(head_width, embed, 0.01) for press in presses})


class VisionEncoder(nn.Module):
    """**The camera** (camera-vision): a seat's H x W image of the static world its camera's rays hit (terrain,
    models, closed doors, liquids, ground hazards: units and objects are the entity list's), encoded by one small
    network shared by every layout, as VisibleEntities shares its encoder.

    The image arrives as bytes beside the observation (protocol 26: [N, H x W x 4] uint8, [row][col][byte]) and is
    decoded on the device to its five channels, channels last (decode_image); the block's scalars are gathered from
    each row's own layout's columns (a table by layout id: the vision block starts at a different column in every
    class). **The class is a one-hot** over the manifest's pixel_classes (7: sky, terrain, model, door, water, deadly,
    ground hazard), a float data plane compared against the class channel, so 4 + 7 planes a pixel (11) and no
    learned embedding on the pixel path (an embedding's scatter backward was a third of the CPU update). The class
    table `class_embed` stays for the entity tokens and the map. **Patches**, not
    convolutions: the image is cut into PATCH x PATCH squares (128 x 64 with patch 8 -> a 16 x 8 grid), each square's
    pixels and planes (8 x 8 x 11 = 704 features, ordered row in the patch, column in the patch, plane) go through
    one shared Linear -> 64 + SiLU and Linear 64 -> 64 + SiLU, a spatial softmax per channel over the patch grid gives
    its expected (x, y) in [-1, 1] (128 keypoints), the block's scalars join them, and Linear -> 256: the camera's
    embedding, after SiLU. PATCH is the manifest's "patch" (4 when it names none). Each network reads it through a
    `vision_join` of its own, a Linear onto its adapters' output width (VisionJoin).

    **The entity list** (perception-goals 1b; VisibleEntities): where the layouts have one, its slots are read as a
    set and the set's pool is added to the embedding before the SiLU. The image carries no entities, so a token does
    not look at pixels: what it knows of where and how big it is, is its columns (direction, distance, angular size,
    line of sight).

    **One encoder for the actor and the critic** (MappoTrainer): the actor owns it -- it is in the actor's state dict,
    so a policy alone (export, distillation, the league) carries its camera -- and the critic holds a reference that
    is not one of its modules, so neither its parameters, its optimiser nor its state dict has a second copy. Both
    losses' gradients reach it, summed, and its own optimiser steps it once per minibatch (MappoTrainer.vision_opt).

    **The camera switched off** (image.camera false, StageDefinition::Camera; 2026-10-22): no pixels arrive, so there
    is no patch stem, no spatial softmax and no camera splat; the embedding is Linear(scalars -> 256) of the block's
    scalars (the view cone's pose: the look still turns it), plus the entity list, the map, the coverage grid, the
    sight list and the remembered entities as before -- the same 256 wide, so every VisionJoin reads it unchanged.
    Each row's bytes (Step.image) are then its map crop alone, or nothing (None) without a map block.

    No normalisation: the channels are 0-1 or -1-1 already, and the list's columns are scaled by the sim. Every
    shape is fixed and nothing is read back, so a rollout graph captures it."""

    PATCH = DEFAULT_PATCH       # the class's default; an encoder's own is its manifest's "patch" (revision 4: 8)
    WIDTHS = (64, 64)
    EMBED = 256
    CLASS_EMBED = 6

    @classmethod
    def update_bytes_per_row(cls, image: dict, splat: bool = False) -> int:
        """Roughly the device memory one row's camera (and map) keeps for the update's backward pass, in bytes, from the
        stage's vision descriptor: float32 tensors the encoder saves per pixel (the decoded channels, the planes
        without the class, the concatenated planes with the class one-hot and the patch copy of them) and per map
        cell (the code and class embeddings, the values, the planes and their patch copy, and the int64 decode).
        The pixel class is a one-hot (no embedding output kept), so a row of the 128 x 64 camera is 1.0 MB against the
        embedded design's 1.15 MB (0.98 MB measured then, 5.6 GiB over 6,144 rows, move1_controls.yaml); not
        re-measured. The map adds 0.45 MB; the camera splat (`splat`, mappo.camera_splat) its samples' features, its
        geometry and its planes a tier, about 0.3 MB more. With the camera off (image.camera false) there are no pixels
        and no splat: the map's part alone."""
        channels = int(image["channels"])
        planes = channels - 1 + len(image["pixel_classes"])
        renders = bool(image.get("camera", True))
        pixels = int(image["height"]) * int(image["width"]) if renders else 0
        splat = splat and renders
        total = 4 * pixels * (channels + (channels - 1) + 2 * planes)
        crop = image.get("map")
        if crop is not None:
            tiers = int(crop.get("tiers", 1))
            cells = int(crop["height"]) * int(crop["width"]) * tiers
            per_cell = MapEncoder.CODE_EMBED + cls.CLASS_EMBED + 5
            total += cells * (4 * 3 * per_cell + 16)
            if tiers > 1:
                # The planner's transients a tier in the no-grad pass (MapEncoder.plan): the costmap's hidden planes
                # and logits, their softmax, the VIN's input planes and the fixed step costs, float32 a cell.
                planes = CostmapHead.HIDDEN * 2 + 2 * TRAV_CLASSES + (TRAV_CLASSES + per_cell + 1) + Planner.CHANNELS
                total += int(crop["height"]) * int(crop["width"]) * 4 * planes * tiers
                if splat:
                    # The camera splat (CameraSplat): each sample's projected features (and their gradient's copy),
                    # its float32 geometry (about eight arrays) and int64 index a tier, and its planes a tier.
                    samples = pixels // (CameraSplat.STRIDE * CameraSplat.STRIDE)
                    total += samples * 4 * (2 * CameraSplat.CHANNELS + 8 + 4 * tiers)
                    total += int(crop["height"]) * int(crop["width"]) * 4 * 2 * (CameraSplat.CHANNELS + 2) * tiers
        return total

    @property
    def planner_on(self) -> bool:
        """Whether the map's encoder carries the costmap head and the planner (a two-tier map)."""
        return self.map is not None and self.map.planner_on

    @property
    def planner_widths(self) -> tuple[int, int]:
        """(readout width, pool width) of the cached plan a row (MapEncoder), (0, 0) without a planner."""
        return (self.map.readout_width, self.map.pool_width) if self.planner_on else (0, 0)

    #: The camera-side inputs a trainer may leave unread (mappo.drop_inputs): "map", the mental map's crop and its
    #: scalars (no MapEncoder: the crop's bytes still travel and are skipped), and "coverage", the coarse coverage
    #: grid (no CoverageEncoder). "map" takes "coverage" with it (the sim's rule: a coverage block needs the map).
    #: The dropped columns stay blind to the adapters, so a network with and without reads the same checkpoint.
    DROPPABLE = ("map", "coverage")

    def __init__(self, descriptors: Sequence[dict | None], drop: Sequence[str] = (), planner_k: int = Planner.K,
                 splat: bool = False, waypoint: bool = False, planner_k_fine: int | None = None,
                 hierarchical: bool = False, waypoint_source: str = "learned", waypoint_costs: str = "head",
                 school_checkpoint: str = ""):
        super().__init__()
        unknown = sorted(set(drop) - set(self.DROPPABLE))
        if unknown:
            raise ValueError(f"mappo.drop_inputs: unknown {unknown}; expected some of {list(self.DROPPABLE)}")
        self.dropped = frozenset(drop) | (frozenset(["coverage"]) if "map" in drop else frozenset())
        image = next(entry for entry in descriptors if entry is not None)
        self.height, self.width = image["height"], image["width"]
        self.channels, self.classes = image["channels"], image["classes"]
        self.class_channel, self.scalars = image["class_channel"], image["scalars"]
        self.class_limit = int(image.get("class_limit", CLASS_LIMIT))
        self.pixel_classes = tuple(image["pixel_classes"])
        self.PATCH = int(image.get("patch", DEFAULT_PATCH))
        # Whether pixels arrive at all (vision_of's "camera"; image.camera false switches the rendering off).
        self.camera = bool(image.get("camera", True))
        if self.camera and (self.height % self.PATCH or self.width % self.PATCH):
            raise ValueError(f"the camera's image is {self.width} x {self.height}; the vision encoder cuts it into "
                             f"{self.PATCH} x {self.PATCH} patches, so both must be multiples of {self.PATCH} "
                             f"(AnimusForge.Vision.Width, Height)")
        self.image_bytes = image["image_bytes"] if self.camera else 0
        self.span = self.scalars
        self.class_embed = nn.Embedding(self.class_limit, self.CLASS_EMBED)
        self.planes_per_pixel = self.channels - 1 + len(self.pixel_classes)
        # The patch stem and its spatial softmax, with the camera on; with it off the scalars alone make the
        # embedding's own part (no patch, no mix: nothing in the state dict for pixels that never come).
        self.patch = self.mix = None
        self.grid = (0, 0)
        self.feature_shape = None
        if self.camera:
            self.grid = (self.height // self.PATCH, self.width // self.PATCH)
            features = self.PATCH * self.PATCH * self.planes_per_pixel
            self.patch = nn.Linear(features, self.WIDTHS[0])
            self.mix = nn.Linear(self.WIDTHS[0], self.WIDTHS[1])
            self.feature_shape = (self.WIDTHS[1], *self.grid)
        self.embed = nn.Linear((2 * self.WIDTHS[1] if self.camera else 0) + self.scalars, self.EMBED)
        # Derived from stage.json, not learned: kept out of the state dict.
        self.register_buffer("start", torch.tensor([entry["first"] if entry is not None else -1
                                                    for entry in descriptors], dtype=torch.long), persistent=False)
        self.register_buffer("has_vision", self.start >= 0, persistent=False)
        self.register_buffer("offsets", torch.arange(self.span, dtype=torch.long), persistent=False)
        grid_y, grid_x = torch.meshgrid(torch.linspace(-1.0, 1.0, self.grid[0]),
                                        torch.linspace(-1.0, 1.0, self.grid[1]), indexing="ij")
        self.register_buffer("grid_x", grid_x.reshape(-1), persistent=False)
        self.register_buffer("grid_y", grid_y.reshape(-1), persistent=False)
        # The classes a pixel can carry, in plane order: the one-hot compares the class channel against them. Derived
        # from the manifest, not learned.
        self.register_buffer("pixel_class_ids", torch.tensor(self.pixel_classes, dtype=torch.long), persistent=False)
        # The entity list, where the layouts have one (every revision 5 stage does).
        lists = [entry.get("entities") if entry is not None else None for entry in descriptors]
        self.entities = None
        self.slots = 0
        if any(entry is not None for entry in lists):
            self.entities = VisibleEntities(lists, self.class_embed, self.EMBED)
            self.slots = self.entities.slots
        # The mental map, where the layouts have one (perception-goals REDESIGN §3): its crop follows the image in
        # each row's bytes, and its encoder adds to the embedding.
        crops = [entry.get("map") if entry is not None else None for entry in descriptors]
        self.map = None
        self.map_bytes = 0
        if any(crop is not None for crop in crops):
            crop = next(crop for crop in crops if crop is not None)
            # The crop's bytes follow the image in every row whether or not they are read (pixels() skips them).
            self.map_bytes = crop["map_bytes"]
            # Per layout the sim's route waypoint columns (route_of; mappo.planner_waypoint_source "sim"), the
            # school's goal columns and the kept column (goal_of, kept_of; "school").
            crop = {**crop, "routes": [c["route"] if c is not None else None for c in crops],
                    "goals": [c["goal"] if c is not None else None for c in crops],
                    "kept": [c["kept"] if c is not None else -1 for c in crops]}
            if "map" not in self.dropped:
                self.map = MapEncoder(crop, self.class_embed, self.EMBED,
                                      [c["first"] if c is not None else -1 for c in crops],
                                      [entry.get("compass") if entry is not None else None for entry in descriptors],
                                      # The splat projects the camera's pixels: none with the camera off.
                                      planner_k, image if splat and self.camera else None, self.WIDTHS[1], waypoint,
                                      planner_k_fine, hierarchical, waypoint_source, waypoint_costs,
                                      school_checkpoint)
        # The coarse coverage grid (general search sec 5), where the layouts have the block: its columns are read raw
        # by its own small encoder, whose join adds to the embedding as the map's does.
        grids = [entry.get("coverage") if entry is not None else None for entry in descriptors]
        self.coverage = None
        if any(grid is not None for grid in grids):
            if not self.map_bytes:
                raise ValueError("a coverage block needs the mental map's block (the sim's Problem() rule)")
            grid = next(grid for grid in grids if grid is not None)
            if "coverage" not in self.dropped:
                self.coverage = CoverageEncoder(grid, self.EMBED, [g["first"] if g is not None else -1 for g in grids])
        # The seen and remembered list (dungeon-curriculum I1, I2), where the layouts have a sight block: read with
        # the entity list's encoder, pooled onto the embedding, and pointed at by the actor's presses.
        sights = [entry.get("sight") if entry is not None else None for entry in descriptors]
        self.sight = None
        if any(sight is not None for sight in sights):
            if self.entities is None:
                raise ValueError("a sight block needs the camera's entity list, whose encoder it shares")
            self.sight = SightEntities(sights, self.entities, self.EMBED)
        # The remembered entities (object memory), where the layouts have the block: the entity list's tokens plus the
        # memory's columns, pooled onto the embedding.
        remembered = [entry.get("memory_entities") if entry is not None else None for entry in descriptors]
        self.memory_entities = None
        if any(entry is not None for entry in remembered):
            if self.entities is None:
                raise ValueError("a memory_entities block needs the camera's entity list, whose encoder it shares")
            self.memory_entities = MemoryEntities(remembered, self.entities, self.EMBED)
        #: Per layout with the camera, the columns its adapter and normaliser do not read: the block's scalars, which
        #: the encoder reads raw, its entity list's, which the list's encoder reads, its map's scalars, its coverage
        #: grid's, and its sight list's.
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
            if grids[index] is not None:
                columns += list(range(grids[index]["first"], grids[index]["first"] + grids[index]["width"]))
            if self.sight is not None:
                columns += self.sight.blind.get(index, [])
            if self.memory_entities is not None:
                columns += self.memory_entities.blind.get(index, [])
            self.blind[index] = columns

    def gather(self, obs: torch.Tensor, layout: torch.Tensor,
               image: torch.Tensor | None) -> tuple[torch.Tensor | None, torch.Tensor]:
        """Each row's camera: (its image decoded, [N, H, W, C], and its own layout's scalar columns, [N, S], raw).
        Rows of a layout without the camera read whatever is at column 0 on (their output is zeroed by the join).
        With the camera off the image is None: there are no pixels to decode."""
        if image is None and self.camera:
            raise ValueError("this network has a camera: its image bytes have to come with the observation")
        layout = layout.reshape(-1).long()
        columns = (self.start[layout].clamp(min=0)[:, None] + self.offsets[None, :]).clamp(max=obs.shape[-1] - 1)
        scalars = obs.gather(1, columns).to(self.embed.weight.dtype)
        if not self.camera:
            return None, scalars
        decoded = decode_image(self.pixels(image), self.height, self.width)
        return decoded.to(self.embed.weight.dtype), scalars

    def pixels(self, image: torch.Tensor) -> torch.Tensor:
        """The image's bytes of each row [N, I]: the row's first image_bytes (its map crop follows, protocol 24);
        [N, 0] with the camera off."""
        rows = image.reshape(-1, self.image_bytes + self.map_bytes)
        return rows[:, :self.image_bytes] if self.map_bytes else rows

    def planes(self, image: torch.Tensor) -> torch.Tensor:
        """[N, H, W, C] -> [N, H, W, C - 1 + P]: every channel but the class as it is, then the class as a one-hot over
        the P pixel classes (the class rounded; a class past the list is all zeros). Data planes: nothing here needs a
        gradient, so there is no embedding to scatter into."""
        classes = image[..., self.class_channel].round().long()
        rest = torch.cat([image[..., : self.class_channel], image[..., self.class_channel + 1:]], dim=-1)
        onehot = classes[..., None] == self.pixel_class_ids
        return torch.cat([rest, onehot.to(rest.dtype)], dim=-1)

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

    def forward(self, obs: torch.Tensor, layout: torch.Tensor, image: torch.Tensor | None,
                plan_out: dict | None = None, grad: bool = False, planner: dict | None = None,
                label_goal: tuple[torch.Tensor, torch.Tensor, torch.Tensor] | None = None) -> torch.Tensor:
        """The camera's embedding of rows `obs` [N, O] with their images `image` [N, I] uint8, [N, 256] (a layout
        without the camera's rows are zeroed by the join). `plan_out` and `grad`: the planner's parts, left in the dict
        (MapEncoder.plan), and whether the planner keeps a graph (the trainer's aux pass); `planner`: the rows' cached
        plan read instead of running it (MapEncoder.forward); `label_goal`: the labels' goal the plan is made from
        (MapEncoder.plan; the aux pass, where the observation carries no compass). With the camera off `image` is
        the rows' map crops [N, M] (None without a map block) and the scalars alone go through embed."""
        decoded, scalars = self.gather(obs, layout, image)
        features = None
        if self.camera:
            features = self.features(self.patches(self.planes(decoded)))
            out = self.embed(torch.cat([self.keypoints(features), scalars], dim=-1))
        else:
            out = self.embed(scalars)
        if self.map is not None:
            if image is None:
                raise ValueError("this network reads the mental map: its crop bytes (Step.image) have to come with "
                                 "the observation")
            crops = image.reshape(-1, self.image_bytes + self.map_bytes)[:, self.image_bytes:]
            # The camera splat's inputs (mappo.camera_splat): the patch features, the image's bytes (its depth) and
            # the scalars (the camera's pose), raw.
            splat = (features, self.pixels(image), scalars) if self.map.splat is not None else None
            out = out + self.map(obs, layout, crops, out, plan_out, grad, planner, splat, label_goal).to(out.dtype)
        if self.coverage is not None:
            out = out + self.coverage(obs, layout).to(out.dtype)
        if self.entities is not None:
            encoded = self.entities.encode(obs, layout)
            out = out + self.entities.pooled(obs, layout, encoded).to(out.dtype)
            if self.sight is not None:
                codes, present = self.sight.tokens(obs, layout)
                out = out + self.sight.pooled(codes, present, layout, obs).to(out.dtype)
            if self.memory_entities is not None:
                out = out + self.memory_entities(obs, layout).to(out.dtype)
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

    def sample(self, features: torch.Tensor, layout: torch.Tensor, deterministic: bool,
               greedy: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """(choice [N, heads] long, its log probability [N], the heads' summed) for flat rows; 0 and 0 for a row
        without the camera. Deterministic is each head's argmax; `greedy` [N] bool takes it for those rows alone."""
        seeing = self._seeing(layout)
        choices, log_prob = [], None
        for logits in self.logits(features):
            choice, chosen = sample_logits(logits.float(), deterministic, greedy)
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


#: **The heading** (heading.CONTRACT.md sec 5, HeadingHead): a mixture of HEADING_COMPONENTS von Mises over the target
#: heading relative to the body's facing, each component's concentration clamped to [KAPPA_MIN, KAPPA_MAX] and its
#: weight floored at HEADING_WEIGHT_FLOOR; HEADING_PROPOSALS Best-Fisher proposals a draw.
HEADING_COMPONENTS = 3
HEADING_KAPPA_MIN = 0.5
HEADING_KAPPA_MAX = 200.0
HEADING_WEIGHT_FLOOR = 0.02
HEADING_PROPOSALS = 8
#: As initialised: component k's mean at 2 pi k / K (0, +120, -120 degrees) with concentration HEADING_INIT_KAPPA, and
#: component 0 -- theta 0, the heading held -- narrower (HEADING_HOLD_KAPPA, about +-20 degrees) and favoured by
#: HEADING_HOLD_BIAS on its mixture logit (weight 0.56 of 1), as the look's hold choice is (LOOK_HOLD_BIAS): a fresh
#: policy mostly keeps roughly its heading rather than spinning, and still turns about a third of the time.
HEADING_INIT_KAPPA = 2.0
HEADING_HOLD_KAPPA = 8.0
HEADING_HOLD_BIAS = 1.0
_HEADING_NORM_EPSILON = 1e-6
_LOG_TWO_PI = math.log(2.0 * math.pi)


class HeadingHead(nn.Module):
    """**The heading** (heading.CONTRACT.md sec 5): the target heading theta in (-pi, pi] relative to the body's facing
    at the decision (0 holds it), as a mixture of K von Mises over the policy features the action head reads -- one head
    for every layout, as the shared movement is. One Linear(features -> 4K), its outputs in this order (the .amdl's and
    the realm's too):

    - [0, 2K): component k's mean direction (x, y) at 2k, 2k + 1, normalised to a unit vector: mu_k = atan2(y, x);
    - [2K, 3K): kappa_k = clamp(softplus(.) + KAPPA_MIN, KAPPA_MIN, KAPPA_MAX);
    - [3K, 4K): the mixture logits: w = floor + (1 - K floor) softmax(.), every component at least the floor.

    A draw picks the component by Gumbel-max over log w and its offset from the mean by Best-Fisher's rejection sampler
    with HEADING_PROPOSALS proposals at once, the first accepted kept and the mean where none is (P < 0.35^8): fixed
    shapes, no read back, so a rollout graph captures it. Deterministic (evaluation, the export) is the heaviest
    component's mean. Only rows whose layout has a move block steer: elsewhere theta is 0 (held) and its log
    probability, entropy and concentration 0 with no gradient. Nothing is masked."""

    def __init__(self, width: int, layouts_with: torch.Tensor, components: int = HEADING_COMPONENTS):
        super().__init__()
        self.components = int(components)
        k = self.components
        self.linear = _linear(width, 4 * k, 0.01)
        with torch.no_grad():
            for index in range(k):
                angle = 2.0 * math.pi * index / k
                self.linear.bias[2 * index] = math.cos(angle)
                self.linear.bias[2 * index + 1] = math.sin(angle)
            # softplus^-1(kappa - KAPPA_MIN)
            self.linear.bias[2 * k:3 * k] = math.log(math.expm1(HEADING_INIT_KAPPA - HEADING_KAPPA_MIN))
            self.linear.bias[2 * k] = math.log(math.expm1(HEADING_HOLD_KAPPA - HEADING_KAPPA_MIN))
            self.linear.bias[3 * k] = HEADING_HOLD_BIAS
        self.register_buffer("has_heading", layouts_with.clone(), persistent=False)

    def _steering(self, layout: torch.Tensor) -> torch.Tensor:
        return self.has_heading[layout.reshape(-1).long()]

    def mixture(self, features: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
        """(mu [N, K, 2] unit vectors (cos, sin), kappa [N, K], log w [N, K], the mixture logits [N, K]) for flat rows,
        in float32."""
        k = self.components
        raw = self.linear(features).float()
        xy = raw[:, :2 * k].reshape(-1, k, 2)
        mu = xy / xy.norm(dim=-1, keepdim=True).clamp(min=_HEADING_NORM_EPSILON)
        kappa = (nn.functional.softplus(raw[:, 2 * k:3 * k]) + HEADING_KAPPA_MIN).clamp(HEADING_KAPPA_MIN,
                                                                                         HEADING_KAPPA_MAX)
        logits = raw[:, 3 * k:]
        weight = HEADING_WEIGHT_FLOOR + (1.0 - k * HEADING_WEIGHT_FLOOR) * torch.softmax(logits, dim=-1)
        return mu, kappa, weight.log(), logits

    @staticmethod
    def mode_of(mu: torch.Tensor, logits: torch.Tensor) -> torch.Tensor:
        """The heaviest component's mean angle [N]: the component with the largest mixture logit (the first of equals;
        the weights are monotone in the logits, and the realm reads the logits)."""
        heaviest = logits.argmax(dim=-1)
        chosen = mu.gather(1, heaviest[:, None, None].expand(-1, 1, 2))[:, 0]
        return torch.atan2(chosen[:, 1], chosen[:, 0])

    @staticmethod
    def log_prob_of(mu: torch.Tensor, kappa: torch.Tensor, log_w: torch.Tensor, theta: torch.Tensor) -> torch.Tensor:
        """log p(theta) [N] under the mixture: logsumexp_k [log w_k + kappa_k cos(theta - mu_k) - log 2 pi - log I0(
        kappa_k)], with cos(theta - mu) = cos theta mu_x + sin theta mu_y and log I0(kappa) = log i0e(kappa) + kappa."""
        theta = theta.float()[:, None]
        cosine = torch.cos(theta) * mu[..., 0] + torch.sin(theta) * mu[..., 1]
        log_i0 = torch.log(torch.special.i0e(kappa)) + kappa
        return torch.logsumexp(log_w + kappa * cosine - _LOG_TWO_PI - log_i0, dim=-1)

    @staticmethod
    def entropy_of(kappa: torch.Tensor, log_w: torch.Tensor) -> torch.Tensor:
        """The entropy surrogate [N] (an upper bound on the mixture's): sum_k w_k H(vM(kappa_k)) + H(w), with
        H(vM(kappa)) = log(2 pi I0(kappa)) - kappa I1(kappa) / I0(kappa)."""
        i0e = torch.special.i0e(kappa)
        component = _LOG_TWO_PI + torch.log(i0e) + kappa - kappa * torch.special.i1e(kappa) / i0e
        weight = log_w.exp()
        return (weight * component).sum(-1) - (weight * log_w).sum(-1)

    @staticmethod
    def offsets(kappa: torch.Tensor, proposals: int = HEADING_PROPOSALS) -> torch.Tensor:
        """Draws [N] from von Mises(0, kappa [N]) by Best and Fisher's (1979) rejection sampler, `proposals` proposals a
        row at once, in double precision: the first accepted, 0 (the mean) where none is."""
        kappa = kappa.detach().double()
        tau = 1.0 + torch.sqrt(1.0 + 4.0 * kappa * kappa)
        rho = (tau - torch.sqrt(2.0 * tau)) / (2.0 * kappa)
        r = ((1.0 + rho * rho) / (2.0 * rho))[:, None]
        uniform = torch.rand((kappa.shape[0], proposals, 3), dtype=torch.float64, device=kappa.device)
        u1, u2, u3 = uniform.unbind(dim=-1)
        z = torch.cos(math.pi * u1)
        f = ((1.0 + r * z) / (r + z)).clamp(-1.0, 1.0)
        c = kappa[:, None] * (r - f)
        accept = (c * (2.0 - c) - u2 > 0.0) | (torch.log(c / u2) + 1.0 - c >= 0.0)
        drawn = ((u3 >= 0.5).double() * 2.0 - 1.0) * torch.acos(f)
        # The first accepted proposal: the largest of (proposals - index) over the accepted, unique where any is.
        rank = accept.to(torch.int64) * torch.arange(proposals, 0, -1, device=kappa.device)
        first = rank.argmax(dim=-1, keepdim=True)
        return torch.where(accept.any(dim=-1), drawn.gather(1, first)[:, 0], torch.zeros_like(kappa))

    def sample(self, features: torch.Tensor, layout: torch.Tensor, deterministic: bool,
               greedy: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """(theta [N] float32 in (-pi, pi], its log probability [N]) for flat rows; 0 and 0 for a row without a move
        block. Deterministic is the mode (the heaviest component's mean); `greedy` [N] bool takes it for those rows
        alone (the draw is made for every row either way, as sample_logits does)."""
        steering = self._steering(layout)
        mu, kappa, log_w, logits = self.mixture(features)
        mode = self.mode_of(mu, logits)
        if deterministic:
            theta = mode
        else:
            gumbel = -torch.log(-torch.log(torch.rand_like(log_w)))
            component = (log_w + gumbel).argmax(dim=-1, keepdim=True)
            chosen = mu.gather(1, component[:, :, None].expand(-1, 1, 2))[:, 0]
            angle = torch.atan2(chosen[:, 1], chosen[:, 0]) + self.offsets(kappa.gather(1, component)[:, 0]).float()
            theta = torch.atan2(torch.sin(angle), torch.cos(angle))
            if greedy is not None:
                theta = torch.where(greedy.reshape(-1), mode, theta)
        log_prob = self.log_prob_of(mu, kappa, log_w, theta)
        zero = torch.zeros_like(theta)
        return torch.where(steering, theta, zero), torch.where(steering, log_prob, zero)

    def evaluate(self, features: torch.Tensor, layout: torch.Tensor,
                 theta: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """(log probability [N], entropy surrogate [N], mean concentration sum_k w_k kappa_k [N]) of the headings
        `theta` [N] taken; 0 with no gradient for a row without a move block."""
        steering = self._steering(layout)
        mu, kappa, log_w, _ = self.mixture(features)
        log_prob = self.log_prob_of(mu, kappa, log_w, theta.reshape(-1))
        entropy = self.entropy_of(kappa, log_w)
        concentration = (log_w.exp() * kappa).sum(-1).detach()
        zero = torch.zeros_like(log_prob)
        return (torch.where(steering, log_prob, zero), torch.where(steering, entropy, zero),
                torch.where(steering, concentration, zero))

    def mode(self, features: torch.Tensor) -> torch.Tensor:
        """The deterministic heading [N] for flat rows, whatever their layout (the golden vectors' oracle)."""
        mu, _, _, logits = self.mixture(features)
        return self.mode_of(mu, logits)


def _attach_vision(network: nn.Module, vision, encoder: "VisionEncoder | None" = None,
                   drop: Sequence[str] = (), planner_k: int = Planner.K, splat: bool = False,
                   waypoint: bool = False, planner_k_fine: int | None = None, hierarchical: bool = False,
                   waypoint_source: str = "learned", waypoint_costs: str = "head",
                   school_checkpoint: str = "") -> None:
    """Give an actor or critic the camera: the encoder (VisionEncoder) and its own join (VisionJoin), and keep the
    camera's columns -- image and scalars -- out of every layout's adapter (attach_blind_columns, tag "vision") and
    normaliser (RunningNorm.hide). `vision` is vision_of()'s per-layout image, or None (no camera: nothing changes).
    With `encoder` (another network's: the trainer's critic gets the actor's), the network reads that one through a
    reference that is not one of its modules -- not in its parameters, its state dict or its .to() -- else it builds
    and owns its own. `drop` names the camera-side inputs an owned encoder leaves unread (VisionEncoder.DROPPABLE,
    mappo.drop_inputs); `splat` whether its map's costmap head reads the camera splat (CameraSplat,
    mappo.camera_splat); `waypoint` whether its map feeds the planned waypoint (MapEncoder, mappo.planner_waypoint);
    `planner_k_fine` the fine tier's own iterations (None: planner_k's; Planner), `hierarchical` the coarse -> fine
    hierarchy (MapEncoder, mappo.planner_hierarchical), `waypoint_source` what the waypoint walks over (MapEncoder,
    mappo.planner_waypoint_source) and `waypoint_costs` what the classical one prices its cells on (MapEncoder,
    mappo.planner_waypoint_costs); `school_checkpoint` the school planner's weights under the "school" source
    (MapEncoder.school, mappo.school_checkpoint)."""
    network.vision = None
    network.vision_join = None
    if vision is None:
        return
    if encoder is None:
        network.vision = VisionEncoder(vision, drop, planner_k, splat, waypoint, planner_k_fine, hierarchical,
                                       waypoint_source, waypoint_costs, school_checkpoint)
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


#: The director layout's name (Curriculum::DirectorLayout::Name) and stage.json's key for its sets.
DIRECTOR_LAYOUT = "director"


class DirectorSets(nn.Module):
    """**The director's party, enemies and objects as sets** (the director, restored 2026-10-26). Each slot of a set
    goes through one encoder shared across the set's slots; the encodings of the slots present are pooled (mean and
    max), so the director reads its party whatever slot a member landed in. The raw slot columns are kept out of the
    layout's own adapter (attach_blind_columns), so nothing learns a slot's position. The actions that name a slot --
    address this member, heal or follow this member, fight or pull this enemy, loot or use this object -- are scored
    from that slot's encoding against a query from the features (pointer logits), so they move with the thing rather
    than with the slot. `descriptor` is stage.json's "director" (DirectorLayout::SetDescriptor).

    No row picks on the host: the pooled term is computed for every row and multiplied by the layout mask, and the
    pointer scores are written through torch.where, so a captured rollout graph stays valid."""

    SETS = ("seats", "enemies", "objects")

    def __init__(self, descriptor: dict, obs_dim: int, width: int, head_width: int = 0, embed: int = 64):
        super().__init__()
        self.sets = {}
        columns = torch.zeros(obs_dim, dtype=torch.bool)
        for name in self.SETS:
            spec = descriptor[name]
            first, slots, per = int(spec["first"]), int(spec["slots"]), int(spec["width"])
            self.sets[name] = (first, slots, per, int(spec.get("present", 0)))
            columns[first : first + slots * per] = True
            setattr(self, f"{name}_encoder", nn.Sequential(_linear(per, embed, math.sqrt(2)), nn.Tanh(),
                                                           _linear(embed, embed, math.sqrt(2)), nn.Tanh()))
        self.register_buffer("column_mask", columns, persistent=False)
        self.obs_dim = obs_dim
        self.pool = _linear(2 * embed * len(self.SETS), width, 1.0)
        self.pointers = ([(int(p["first"]), str(p["over"]), int(p.get("count", self.sets[str(p["over"])][1])))
                          for p in descriptor.get("pointers", ())] if head_width else [])
        self.queries = nn.ModuleList(_linear(head_width, embed, 0.01) for _ in self.pointers)

    def blind_columns(self) -> list[int]:
        return torch.nonzero(self.column_mask).reshape(-1).tolist()

    def encode_set(self, obs: torch.Tensor, name: str) -> tuple[torch.Tensor, torch.Tensor]:
        """[rows, slots, embed] encodings and [rows, slots] which are present."""
        first, slots, per, present = self.sets[name]
        raw = obs[:, first : first + slots * per].reshape(obs.shape[0], slots, per)
        dtype = self.pool.weight.dtype
        return getattr(self, f"{name}_encoder")(raw.to(dtype)), raw[..., present] > 0.5

    def pooled(self, obs: torch.Tensor) -> torch.Tensor:
        """What the sets add to the adapter's output: [rows, width]."""
        parts = []
        for name in self.SETS:
            encoded, present = self.encode_set(obs, name)
            weight = present.to(encoded.dtype)[..., None]
            count = weight.sum(dim=1).clamp(min=1.0)
            parts.append((encoded * weight).sum(dim=1) / count)
            peak = torch.where(present[..., None], encoded, torch.full_like(encoded, -1.0)).amax(dim=1)
            parts.append(torch.where(present.any(dim=1, keepdim=True), peak, torch.zeros_like(peak)))
        return self.pool(torch.cat(parts, dim=-1))

    def pointer_logits(self, features: torch.Tensor, obs: torch.Tensor) -> list[tuple[int, torch.Tensor]]:
        """Per pointer action group: (its first action, [rows, count] logits) from each slot's encoding."""
        encoded = {name: self.encode_set(obs, name)[0] for name in self.SETS}
        out = []
        for (first, over, count), query in zip(self.pointers, self.queries):
            scores = torch.einsum("rse,re->rs", encoded[over], query(features).to(encoded[over].dtype))
            out.append((first, scores[:, :count]))
        return out


def attach_director(network: nn.Module, director, head_width: int = 0) -> None:
    """Give an actor or critic the director's set encoder (DirectorSets), and keep the director layout's own adapter
    blind to the slot columns (attach_blind_columns, tag "director"). `director` is (its layout index, stage.json's
    "director"); None for a run without one."""
    network.director_index = -1
    network.director_sets = None
    if director is None:
        return
    index, descriptor = director
    network.director_index = int(index)
    network.director_sets = DirectorSets(descriptor, network.obs_dims[index], network.adapters[index].out_features,
                                         head_width)
    attach_blind_columns(network, {int(index): network.director_sets.blind_columns()}, "director")


def director_term(network: nn.Module, obs: torch.Tensor, layout: torch.Tensor) -> torch.Tensor | None:
    """What the director rows' sets add to the adapter's output [rows, width] (zero on the other rows; None without a
    director). Computed for every row and masked rather than gathered: no host branch, so the rollout graph stays."""
    sets = getattr(network, "director_sets", None)
    if sets is None:
        return None
    rows = (layout.reshape(-1) == network.director_index).to(sets.pool.weight.dtype)[:, None]
    return sets.pooled(obs[:, : sets.obs_dim]) * rows


def with_director_pointers(network: nn.Module, logits: torch.Tensor, features: torch.Tensor, layout: torch.Tensor,
                           obs: torch.Tensor | None) -> torch.Tensor:
    """The director rows' per-slot actions, scored by their slots' encodings (DirectorSets.pointer_logits); the
    other rows' logits as they were. The layout head's own logits there would give each slot a score for its
    position, which is what a set must not have."""
    sets = getattr(network, "director_sets", None)
    if sets is None or obs is None:
        return logits
    flat = obs.reshape(-1, obs.shape[-1])[:, : sets.obs_dim]
    rows = (layout.reshape(-1) == network.director_index)[:, None]
    for first, scores in sets.pointer_logits(features, flat):
        count = scores.shape[1]
        kept = logits[:, first : first + count]
        logits = torch.cat([logits[:, :first], torch.where(rows, scores.to(logits.dtype), kept),
                            logits[:, first + count:]], dim=1)
    return logits


def director_of(stage: dict | None, layout_names: Sequence[str]):
    """(the director layout's index, stage.json's "director") when the stage has one and the run fields it, else
    None."""
    if not stage or "director" not in stage or DIRECTOR_LAYOUT not in layout_names:
        return None
    return list(layout_names).index(DIRECTOR_LAYOUT), stage["director"]


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
#: The goal head's pointer over place slots (GoalHead.pointer, m2-goals W3a) and where its features sit
#: (GoalHead.place_spec): an actor saved before them loads with the pointer as initialised -- its last layer at zero, so
#: it adds nothing -- and no place slots until the stage's manifest says (MappoTrainer.set_goal_space).
#: The same for the cell head (GoalHead.cell, free-choice-goals): an actor or critic saved before it loads with
#: goal_head.cell.* as initialised (the score at zero: a uniform draw over the choosable blocks) and no cells until the
#: manifest says (cell_spec).
_GOAL_POINTER_PREFIXES = ("goal_head.pointer.", "goal_head.place_spec", "goal_head.cell.", "goal_head.cell_spec",
                          "vision.map.planner.k")
#: The heading head's weights (HeadingHead): an actor saved without one loads with the head as initialised (it holds the
#: heading, mostly); one saved with it into an actor without a heading (a stage with no move block) drops them.
HEADING_PREFIX = "heading_head."


#: The blind-column masks' buffer names (attach_blind_columns' tags): "blind_keep_" (a checkpoint from before the hint
#: block went still carries it, and it is dropped on load), the camera's, and the teacher's hint block's (2026-10-27:
#: made again from the stage's layouts after loading, by MappoTrainer.set_hint_space).
BLIND_KEEP_PREFIXES = ("blind_keep_", "vision_keep_", "hint_keep_")


def without_blind_columns(state: dict) -> dict:
    """A saved network's state without its blind-column masks (attach_blind_columns): they are made again from the
    stage's layouts after loading (the camera), and a network not yet given them refused them."""
    return {key: value for key, value in state.items()
            if not key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)}


#: The tensors of a goal head and its embeddings that have a row per goal kind -- or, drawn, per joint goal -- and so
#: grow when the sim's goal space does. Kinds are only ever appended (search-kind, goal block revision 5: 9 -> 10), so
#: a joint id kind * targets + target keeps its row and the new kinds' rows follow the old ones (drawn's row 0 is
#: "none", goal g at g + 1). The critic has goal_embedding.kind.weight alone.
GOAL_KIND_ROW_KEYS = (
    "goal_head.kind.weight", "goal_head.kind.bias", "goal_head.pair", "goal_head.drawn.weight", "goal_head.accepts",
    "goal_head.success.kind.weight", "goal_head.success.kind.bias", "goal_head.success.pair",
    "goal_head.duration.kind.weight", "goal_head.duration.kind.bias", "goal_head.duration.pair",
    "goal_embedding.kind.weight", "goal_embedding.kind_scale.weight",
)


def grow_goal_rows(state: dict, current: dict) -> tuple[dict, list[str]]:
    """`state` (a saved network's) with its goal tensors of fewer kinds (GOAL_KIND_ROW_KEYS) padded to the shapes of
    `current` (the network's own state): the old rows stay where they were and the new ones are `current`'s, as
    initialised. Returns (the state, the keys grown); a state of the same goal space is returned as it is. Refused
    (ValueError): a state with more kinds than `current` -- a newer checkpoint into a sim with fewer goal kinds -- or
    one whose rows differ in width or targets, which padding cannot fix."""
    grown, names = dict(state), []
    for key in GOAL_KIND_ROW_KEYS:
        if key not in state or key not in current or state[key].shape == current[key].shape:
            continue
        saved, now = state[key], current[key]
        if saved.shape[0] > now.shape[0]:
            raise ValueError(f"{key}: the checkpoint has {saved.shape[0]} rows, this network {now.shape[0]}: the "
                             f"checkpoint's goal space has more kinds than this stage's (a newer checkpoint cannot be "
                             f"loaded into a sim with fewer goal kinds)")
        if saved.shape[1:] != now.shape[1:]:
            raise ValueError(f"{key}: {tuple(saved.shape)} in the checkpoint does not grow into {tuple(now.shape)}")
        grown[key] = torch.cat([saved, now[saved.shape[0]:].to(saved.device, saved.dtype)], dim=0)
        names.append(key)
    return grown, names


#: The actor's movement shared across class layouts (mappo.shared_blocks, LayoutActor's sharing): one input Linear over
#: the shared blocks' columns, one output Linear over their actions. The index buffers say where each layout has them;
#: they are always the network's own (the stage's), never a checkpoint's.
SHARED_PARAMETER_KEYS = ("shared_adapter.weight", "shared_head.weight", "shared_head.bias")
#: The sharing's per-layout flag (LayoutActor.shared_on): saved with the state, dropped with the index keys.
SHARED_ON_KEY = "shared_on"
SHARED_INDEX_KEYS = ("shared_columns", "shared_slots", "shared_on")


def sharing_of(stage: dict | None, layout_names: Sequence[str], blocks: Sequence[str],
               init_layouts: Sequence[str] = ()) -> dict | None:
    """Where every layout has the blocks `blocks` (mappo.shared_blocks), from stage.json: {"blocks", "obs" (per layout
    its columns of those blocks, in block order), "actions" (its action indices), "action_names", "init" (the layout
    indices the shared weights are first averaged from: mappo.shared_init_layouts, every layout when empty)}. None
    without blocks. A block NO layout has (the compass on the nav stages, which carry no compass block since
    2026-10-17 while the nav yamls still inherit `[move, compass, goal]`) is left out with a printed line; None when
    none is left. Refused (ValueError): a layout without one of the blocks another layout has, or a block whose width,
    column names or action names differ between layouts -- the columns and actions must mean the same thing in every
    class."""
    blocks = [str(block) for block in blocks or ()]
    if not blocks:
        return None
    if not stage or not stage.get("layouts"):
        raise ValueError("mappo.shared_blocks: the stage has no stage.json layouts to find the blocks in")
    names = list(layout_names)
    absent = [block for block in blocks
              if not any(any(b.get("name") == block for b in (stage["layouts"].get(name) or {}).get("blocks", ()))
                         for name in names)]
    if absent:
        blocks = [block for block in blocks if block not in absent]
        print(f"mappo.shared_blocks: no layout of the stage has {absent}; shared over {blocks}", flush=True)
        if not blocks:
            return None
    unknown = [name for name in init_layouts if name not in names]
    if unknown:
        raise ValueError(f"mappo.shared_init_layouts: {unknown} are not layouts of this stage ({names})")
    obs, actions, reference, on = [], [], {}, []
    for name in names:
        entry = stage["layouts"].get(name)
        if entry is None:
            raise ValueError(f"mappo.shared_blocks: stage.json has no layout {name}")
        by_name = {block["name"]: block for block in entry.get("blocks", ())}
        # The director (DIRECTOR_LAYOUT) plays no character and has none of the blocks: left out of the sharing (its
        # entries placeholders, "on" false), not refused.
        if name == DIRECTOR_LAYOUT and not any(block in by_name for block in blocks):
            obs.append(None)
            actions.append(None)
            on.append(False)
            continue
        on.append(True)
        action_names = list(entry.get("action_names") or ())
        columns, slots = [], []
        for block in blocks:
            described = by_name.get(block)
            if described is None:
                raise ValueError(f"mappo.shared_blocks: layout {name} has no block {block}")
            (obs_first, obs_count), (action_first, action_count) = described["obs"], described["actions"]
            seen = (int(obs_count), described.get("obs_names"), action_names[action_first:action_first + action_count])
            if len(seen[2]) != action_count:
                raise ValueError(f"mappo.shared_blocks: layout {name}'s block {block} has {action_count} actions, "
                                 f"its action_names {len(seen[2])}")
            if reference.setdefault(block, seen) != seen:
                raise ValueError(f"mappo.shared_blocks: block {block} of layout {name} is not the same as in "
                                 f"{names[0]} (width, column names or action names differ): it cannot be shared")
            columns += range(obs_first, obs_first + obs_count)
            slots += range(action_first, action_first + action_count)
        obs.append(columns)
        actions.append(slots)
    if not any(on):
        return None
    width = next(len(columns) for columns in obs if columns is not None)
    count = next(len(slots) for slots in actions if slots is not None)
    obs = [columns if columns is not None else [0] * width for columns in obs]
    actions = [slots if slots is not None else [0] * count for slots in actions]
    return {"blocks": blocks, "obs": obs, "actions": actions, "on": on,
            "action_names": [action for block in blocks for action in reference[block][2]],
            "init": [names.index(name) for name in init_layouts]
                    or [index for index, shared in enumerate(on) if shared]}


def materialised_state(state: dict, drop: bool = False) -> dict:
    """A saved actor's state with its shared weights (SHARED_PARAMETER_KEYS) written into every layout's own adapter
    columns and head rows (shared_columns, shared_slots): each layout's adapter and head are then the whole map the
    sharing actor computes, as an actor without sharing reads them -- the export, a seed, a teacher. With `drop` the
    shared keys are left out. A state without sharing is returned as it is."""
    if "shared_columns" not in state:
        return state
    out = dict(state)
    columns, slots = state["shared_columns"].long(), state["shared_slots"].long()
    on = state.get("shared_on")
    for index in range(columns.shape[0]):
        if on is not None and not bool(on[index]):
            continue
        if "shared_adapter.weight" in state and columns.shape[1]:
            weight = out[f"adapters.{index}.weight"].clone()
            weight[:, columns[index].to(weight.device)] = state["shared_adapter.weight"].to(weight.device, weight.dtype)
            out[f"adapters.{index}.weight"] = weight
        if "shared_head.weight" in state and slots.shape[1]:
            for part in ("weight", "bias"):
                own = out[f"heads.{index}.{part}"].clone()
                own[slots[index].to(own.device)] = state[f"shared_head.{part}"].to(own.device, own.dtype)
                out[f"heads.{index}.{part}"] = own
    if drop:
        out = {key: value for key, value in out.items()
               if key not in SHARED_PARAMETER_KEYS and key not in SHARED_INDEX_KEYS}
    return out


#: The camera splat's parameters (CameraSplat, mappo.camera_splat) and the costmap head's first convolution, whose
#: input channels grow by the splat's planes when it is on.
SPLAT_PREFIX = "vision.map.splat."
COSTMAP_CONV1 = "vision.map.costmap.conv1.weight"
#: The planned waypoint's join (MapEncoder.waypoint_join, mappo.planner_waypoint): zero at the start, so a checkpoint
#: without it loads with it as initialised and the policy computes what it did.
WAYPOINT_PREFIX = "vision.map.waypoint_join."
#: The planner's schedule beyond its one K (mappo.planner_k_fine, planner_hierarchical), the waypoint's source
#: (mappo.planner_waypoint_source) and what it prices (mappo.planner_waypoint_costs): buffers built from the config
#: alone (Planner, MapEncoder), so a checkpoint without them loads with this network's, and one with them into a
#: network without drops them; none is a parameter.
PLANNER_SCHEDULE_KEYS = ("vision.map.planner.k_fine", "vision.map.hierarchy", "vision.map.waypoint_source",
                         "vision.map.waypoint_costs")


def grow_costmap_inputs(state: dict, current: dict) -> tuple[dict, list[str]]:
    """A saved actor's state for an actor whose costmap head reads more input channels than the checkpoint's (the
    camera splat switched on over a checkpoint without it, mappo.camera_splat): conv1's weight with the checkpoint's
    channels first, as they were, and the new ones -- the splat's planes, after the map's and the broadcast camera's --
    at zero, so the costmap head starts exactly as it was and the splat comes in as conv1 learns to read it. Returns the
    state and the keys grown (none for a checkpoint of the same shape, or of a shape this does not fit: the load then
    reports it as before)."""
    old, new = state.get(COSTMAP_CONV1), current.get(COSTMAP_CONV1)
    if old is None or new is None or old.shape == new.shape:
        return state, []
    if old.dim() != 4 or new.dim() != 4 or old.shape[0] != new.shape[0] or old.shape[2:] != new.shape[2:] \
            or old.shape[1] > new.shape[1]:
        return state, []
    grown = torch.zeros(new.shape, dtype=old.dtype, device=old.device)
    grown[:, :old.shape[1]] = old
    return {**state, COSTMAP_CONV1: grown}, [COSTMAP_CONV1]


#: The map encoder's tensors whose input grows with the planes a crop cell decodes to (MapEncoder.planes_per_cell;
#: map block revision 6 added the walls plane): the patch's Linear (P fastest within each of its PATCH x PATCH cells),
#: the costmap head's conv1 (the planes before the broadcast camera and the splat) and the planner's fixed conv (the
#: planes after the costmap's classes, before the goal plane).
MAP_PATCH = "vision.map.patch.weight"
PLANNER_FIXED = "vision.map.planner.fixed.weight"


def grow_map_planes(state: dict, current: dict) -> tuple[dict, list[str]]:
    """A saved actor's state for an actor whose map decodes more planes a cell than the checkpoint's (the walls plane,
    map block revision 6, over a seven-channel checkpoint: nav1's into nav2): the patch's weight, the costmap head's
    conv1 and the planner's fixed conv with the checkpoint's columns where they were and the new planes' at zero --
    inserted where the new planes sit (the end of each patch cell's group, after the old planes in conv1, after the
    costmap's classes and the old planes in fixed), so every network computes exactly what it did and the new plane
    comes in as the weights learn to read it. Run BEFORE grow_costmap_inputs, whose tail padding is the splat's.
    Returns the state and the keys grown (none for a checkpoint of the same width, or of one this does not fit: the
    load then reports it as before)."""
    old, new = state.get(MAP_PATCH), current.get(MAP_PATCH)
    if old is None or new is None or old.shape == new.shape or old.dim() != 2 or new.dim() != 2 \
            or old.shape[0] != new.shape[0]:
        return state, []
    cells = MapEncoder.PATCH * MapEncoder.PATCH
    if old.shape[1] % cells or new.shape[1] % cells or old.shape[1] > new.shape[1]:
        return state, []
    planes_old, planes_new = old.shape[1] // cells, new.shape[1] // cells
    added = planes_new - planes_old
    grown = dict(state)
    patch = torch.zeros((old.shape[0], cells, planes_new), dtype=old.dtype, device=old.device)
    patch[:, :, :planes_old] = old.reshape(old.shape[0], cells, planes_old)
    grown[MAP_PATCH] = patch.reshape(old.shape[0], -1)
    names = [MAP_PATCH]
    # conv1 [hidden, planes + VISION + splat, 3, 3] and fixed [channels, classes + planes + 1, 3, 3]: the new planes'
    # channels inserted after the old planes' (at planes_old, and at classes + planes_old).
    for key, at in ((COSTMAP_CONV1, planes_old), (PLANNER_FIXED, TRAV_CLASSES + planes_old)):
        saved, now = state.get(key), current.get(key)
        if saved is None or now is None or saved.dim() != 4 or now.dim() != 4 or saved.shape[0] != now.shape[0] \
                or saved.shape[2:] != now.shape[2:] or saved.shape[1] + added > now.shape[1] or at > saved.shape[1]:
            continue
        pad = torch.zeros((saved.shape[0], added, *saved.shape[2:]), dtype=saved.dtype, device=saved.device)
        grown[key] = torch.cat([saved[:, :at], pad, saved[:, at:]], dim=1)
        names.append(key)
    return grown, names


def load_actor_state(actor: nn.Module, state: dict) -> list[str]:
    """actor.load_state_dict(state), except that an actor saved before the goal scale existed loads with it at zero,
    one saved before the goal head's pointer existed loads with the pointer as initialised (a no-op), and one saved with
    fewer goal kinds loads with the new kinds' rows as initialised (grow_goal_rows). Returns the goal tensors grown
    (empty for a same-shape checkpoint): a caller that restores optimiser moments must not, then.

    The shared movement (mappo.shared_blocks): an actor without sharing reads a sharing checkpoint materialised
    (materialised_state: the same map, in every layout's own weights); a sharing actor reads one saved without sharing
    with its shared weights averaged from the layouts' own (LayoutActor.share_from_layouts), and a sharing checkpoint
    as it is. The index buffers are always the actor's own.

    The camera splat (mappo.camera_splat): an actor with it reads a checkpoint without it with the splat's projection as
    initialised and the costmap head's conv1 grown (grow_costmap_inputs: the new channels at zero), and says so; conv1
    is returned with the grown goal tensors (its optimiser moments have the old shape).

    The planned waypoint (mappo.planner_waypoint): an actor with it reads a checkpoint without it with its join as
    initialised (zero: the policy computes exactly what it did), and says so; one whose join is of another width (the
    walk's eight numbers against the sim's seventeen, ROUTE_COLUMNS) likewise, the checkpoint's join dropped. Nothing
    is grown: the camera's optimiser moments are carried by name past the new tensors (MappoTrainer.
    _load_vision_optimizer; a dropped join's moment no longer fits its parameter, so the moments start fresh).

    The planner's schedule (PLANNER_SCHEDULE_KEYS: mappo.planner_k_fine, planner_hierarchical, and the waypoint's
    source, planner_waypoint_source): the configured one stands either way -- missing from the checkpoint, the actor's
    own; in the checkpoint and not the actor's, dropped -- and says so. Nothing to grow: they are buffers.

    The school planner (SCHOOL_PREFIX; mappo.school_checkpoint): an actor with one reads a checkpoint without it with
    the file's weights (mappo.school_checkpoint's) standing, and says so; a checkpoint with one loading into an actor
    without is read with the school's tensors dropped, and says so. Frozen: no optimiser moment to carry."""
    sharing = getattr(actor, "shared_columns", None) is not None
    # Materialised either way, so the layouts' own weights are whole whatever the checkpoint shared.
    state = materialised_state(state, drop=not sharing)
    state = {key: value for key, value in state.items() if key not in SHARED_INDEX_KEYS}
    current = actor.state_dict()
    state, grown = grow_goal_rows(without_blind_columns(state), current)
    saved_planes = int(state[MAP_PATCH].shape[1]) // (MapEncoder.PATCH ** 2) if MAP_PATCH in state else 0
    state, deepened = grow_map_planes(state, current)
    if deepened:
        print(f"Load: the checkpoint's map decodes {saved_planes} planes a cell, this actor's "
              f"{current[MAP_PATCH].shape[1] // (MapEncoder.PATCH ** 2)} (the walls plane, map block revision "
              f"{MAP_BLOCK_REVISION}): {', '.join(deepened)} carried with the new planes' inputs at zero", flush=True)
        grown = [*grown, *deepened]
    saved_inputs = int(state[COSTMAP_CONV1].shape[1]) if COSTMAP_CONV1 in state else 0
    state, widened = grow_costmap_inputs(state, current)
    if widened:
        print(f"Load: the checkpoint's costmap head reads {saved_inputs} input channels, this actor's "
              f"{current[COSTMAP_CONV1].shape[1]} (the camera splat, mappo.camera_splat): conv1's "
              f"{current[COSTMAP_CONV1].shape[1] - saved_inputs} new channels start at zero, the splat's projection "
              f"as initialised", flush=True)
        grown = [*grown, *widened]
    if getattr(actor, "heading_head", None) is None:
        dropped = [key for key in state if key.startswith(HEADING_PREFIX)]
        if dropped:
            print(f"Load: the checkpoint's heading head ({len(dropped)} tensors) is not this actor's (no layout of the "
                  f"stage steers by a heading); dropped", flush=True)
            state = {key: value for key, value in state.items() if not key.startswith(HEADING_PREFIX)}
    rewidened = [key for key in state if key.startswith(WAYPOINT_PREFIX) and key in current
                 and state[key].shape != current[key].shape]
    if rewidened:
        saved_width, width = int(state[rewidened[0]].shape[-1]), int(current[rewidened[0]].shape[-1])
        what = "the sim's route, networks.ROUTE_COLUMNS" if width == len(ROUTE_COLUMNS) else "the walk's"
        print(f"Load: the checkpoint's waypoint join reads {saved_width} numbers, this actor's {width} ({what}); the "
              f"join starts fresh at zero, so the policy computes what it did", flush=True)
        state = {key: value for key, value in state.items() if key not in rewidened}
    school_dropped = [key for key in state if key.startswith(SCHOOL_PREFIX) and key not in current]
    if school_dropped:
        print(f"Load: the checkpoint's school planner ({len(school_dropped)} tensors) is not this actor's "
              f"(mappo.planner_waypoint_source is not school); dropped", flush=True)
        state = {key: value for key, value in state.items() if key not in school_dropped}
    if any(key.startswith(SCHOOL_PREFIX) for key in current) \
            and not any(key.startswith(SCHOOL_PREFIX) for key in state):
        print("Load: the checkpoint has no school planner; this actor's (mappo.school_checkpoint) stands", flush=True)
    schedule_dropped = [key for key in PLANNER_SCHEDULE_KEYS if key in state and key not in current]
    if schedule_dropped:
        print(f"Load: the checkpoint's planner schedule ({', '.join(schedule_dropped)}) is not this actor's "
              f"(mappo.planner_k_fine, planner_hierarchical off, planner_waypoint_source learned, "
              f"planner_waypoint_costs head); dropped", flush=True)
        state = {key: value for key, value in state.items() if key not in schedule_dropped}
    # ... and a schedule buffer both have reads the configured value, never the checkpoint's (a sim-route checkpoint
    # resumed under the school source is the school's; the docstring's rule, which the copy broke before 2026-10-17).
    schedule_kept = [key for key in PLANNER_SCHEDULE_KEYS if key in state and key in current
                     and not torch.equal(state[key].to(current[key].device, current[key].dtype), current[key])]
    if schedule_kept:
        print(f"Load: the checkpoint's planner schedule ({', '.join(schedule_kept)}) differs from this actor's "
              f"configured one, which stands", flush=True)
        state = {key: value for key, value in state.items() if key not in schedule_kept}
    missing, unexpected = actor.load_state_dict(state, strict=False)
    # The blind-column masks are never taken from a checkpoint: an actor built with seat sets has its own already (the
    # stage's), which a resume keeps.
    wrong = [key for key in missing
             if key not in _GOAL_SCALE_KEYS and not key.startswith(_GOAL_POINTER_PREFIXES)
             and not key.split(".")[-1].startswith(BLIND_KEEP_PREFIXES)
             and key not in SHARED_PARAMETER_KEYS and key not in SHARED_INDEX_KEYS
             and not key.startswith(HEADING_PREFIX) and not (widened and key.startswith(SPLAT_PREFIX))
             and not key.startswith(WAYPOINT_PREFIX) and key not in PLANNER_SCHEDULE_KEYS
             and not key.startswith(SCHOOL_PREFIX)]
    if wrong or unexpected:
        raise RuntimeError(f"Error(s) in loading state_dict for {type(actor).__name__}: missing {wrong}, "
                           f"unexpected {list(unexpected)}")
    if any(key.startswith(HEADING_PREFIX) for key in missing):
        print("Load: the checkpoint has no heading head; the actor's starts as initialised", flush=True)
    schedule_new = [key for key in PLANNER_SCHEDULE_KEYS if key in missing and key not in schedule_kept]
    if schedule_new:
        planner = actor.vision.map.planner if getattr(actor, "vision", None) is not None else None
        hierarchical = bool(getattr(actor.vision.map, "hierarchical", False)) if planner is not None else False
        source = ("classical" if planner is not None and getattr(actor.vision.map, "classical_waypoint", False)
                  else "school" if planner is not None and getattr(actor.vision.map, "school_waypoint", False)
                  else "sim" if planner is not None and getattr(actor.vision.map, "sim_waypoint", False)
                  else "learned")
        costs = "map" if planner is not None and getattr(actor.vision.map, "map_costs", False) else "head"
        print(f"Load: the checkpoint has no planner schedule ({', '.join(schedule_new)}); this actor's stands: fine K "
              f"{planner.fine_iterations if planner is not None else '-'}, coarse K "
              f"{planner.iterations if planner is not None else '-'}, hierarchical {hierarchical}, waypoint source "
              f"{source}, waypoint costs {costs} (mappo.planner_k_fine, planner_hierarchical, "
              f"planner_waypoint_source, planner_waypoint_costs)", flush=True)
    if any(key.startswith(WAYPOINT_PREFIX) for key in missing) and not rewidened:
        print("Load: the checkpoint has no planned waypoint (mappo.planner_waypoint); its join starts at zero, so the "
              "policy computes what it did", flush=True)
    parameters = dict(actor.named_parameters())
    with torch.no_grad():
        for key in missing:
            if key in _GOAL_SCALE_KEYS:
                parameters[key].zero_()
    if sharing:
        if any(key in missing for key in SHARED_PARAMETER_KEYS):
            actor.share_from_layouts()
        else:
            actor.clear_shared_slices()
    return grown


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
                 vision=None, drop_inputs: Sequence[str] = (), planner_k: int = Planner.K, sharing=None,
                 heading: Sequence[bool] | None = None, camera_splat: bool = False, planner_waypoint: bool = False,
                 planner_k_fine: int | None = None, planner_hierarchical: bool = False,
                 planner_waypoint_source: str = "learned", planner_waypoint_costs: str = "head",
                 school_checkpoint: str = "", director=None):
        """layouts: (obs dim, action count) per layout; hidden: widths, the first being the adapters' output.
        `director`: (the director layout's index, stage.json's "director") -- its party, enemies and objects are read
        as sets and its per-slot actions scored by them (DirectorSets); None without one.

        `foresight_outputs` adds a head on the trunk that predicts what happens after this decision (mappo.trainer's
        foresight_*): one output per extra discount horizon and one for how much of the episode is left. Nothing reads
        it at rollout time; it is there to make the trunk the actions are chosen from carry the future, and it is left
        out of exported models.

        `recurrent_size` puts a GRU between the trunk and the heads, carried from decision to decision and cleared when
        an episode ends: the policy's own memory of what it has already done and seen, which no observation of the
        moment can hold (who was crowd-controlled, that the enemy has spent its trinket, where the adds came from).

        `sharing` (sharing_of: mappo.shared_blocks) shares the movement across the class layouts: see _attach_sharing.
        None: every layout's adapter and head are its own alone, exactly as before.

        `heading` (heading_of: stage.json's continuous outputs) is per layout whether it steers by a continuous heading
        (HeadingHead, one for all of them); None: no heading head.

        `camera_splat` (mappo.camera_splat) lays the camera's patch features on the costmap head's cells (CameraSplat);
        off, the network is exactly what it was. `planner_waypoint` (mappo.planner_waypoint) feeds the planner's
        planned waypoint into the embedding (MapEncoder); off, likewise. `planner_k_fine` (mappo.planner_k_fine; None:
        planner_k's) the fine tier's iterations and `planner_hierarchical` (mappo.planner_hierarchical) the coarse ->
        fine hierarchy (MapEncoder); `planner_waypoint_source` (mappo.planner_waypoint_source) what the waypoint walks
        over, the learned value or the classical planner's cost-to-go (MapEncoder.classical_plan), and
        `planner_waypoint_costs` (mappo.planner_waypoint_costs) what the classical one prices its cells on, the head's
        classes or the map's own codes; none adds a parameter.
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
        # The camera (VisionEncoder; vision_of(stage.json), None = no layout has one), less the inputs dropped
        # (mappo.drop_inputs).
        _attach_vision(self, vision, drop=drop_inputs, planner_k=planner_k, splat=camera_splat,
                       waypoint=planner_waypoint, planner_k_fine=planner_k_fine, hierarchical=planner_hierarchical,
                       waypoint_source=planner_waypoint_source, waypoint_costs=planner_waypoint_costs,
                       school_checkpoint=school_checkpoint)
        # The sight list's pointer heads (SightPointers), where the layouts have a sight block: the actor's own.
        sight = self.vision.sight if self.vision is not None else None
        self.sight_pointers = SightPointers(sight.presses, head_width) if sight is not None else None
        # The director's sets, with pointer heads (DirectorSets).
        attach_director(self, director, head_width)
        # Its free look (LookHead), where the vision block has look heads (revision 4): the actor's own, not shared.
        look_heads = vision_look_heads(vision)
        self.look_head = LookHead(head_width, look_heads, self.vision.has_vision) if look_heads else None
        # The heading (HeadingHead, heading.CONTRACT.md), where stage.json's continuous outputs name it: one for every
        # layout with a move block. Registered after every other head and before the shared movement, so that without
        # it the parameters and the random draws are an actor's without, and the shared parameters stay last (an
        # optimiser saved without them still lines up). It reads the policy features the action head does, past the
        # GRU, so there is nothing of it to fold or densify in a rollout copy.
        self.heading_head = (HeadingHead(head_width, torch.tensor([bool(h) for h in heading], dtype=torch.bool))
                             if heading is not None and any(heading) else None)
        # The movement shared across the layouts (mappo.shared_blocks): registered last, so that without it the
        # parameters, their order and the random draws are exactly an actor's without, and with it the shared
        # parameters follow every other (an optimiser saved without them still lines up: MappoTrainer.load_state_dict).
        self.shared_adapter: nn.Linear | None = None
        self.shared_head: nn.Linear | None = None
        self.register_buffer("shared_columns", None)
        self.register_buffer("shared_slots", None)
        self.shared_init: list[int] = []
        # The rollout copy's: the shared weights written into the layouts' own (materialise_shared), not added again.
        self.shared_folded = False
        self.shared_on_list: list[bool] = []
        if sharing is not None:
            self._attach_sharing(sharing, hidden[0], head_width)

    def _attach_sharing(self, sharing: dict, width: int, head_width: int) -> None:
        """**The shared movement** (mappo.shared_blocks): the blocks every class has identically -- the movement
        block's columns and its 25 controls, the compass, the goal block -- read and written by one set of weights
        for every layout, so what one class learns about moving reaches all of them.

        - `shared_adapter`, Linear(shared columns -> width, no bias), reads the shared blocks' normalised columns of
          every layout (shared_columns [L, C], their place in each layout's row; the columns blind to the adapters --
          the camera's -- left out), summed with the layout's own adapter, which reads its row with those columns
          zeroed: its weights there take no gradient and do nothing.
        - `shared_head`, Linear(head width -> shared actions), writes the shared actions' logits over the layout's own
          head's at each layout's slots (shared_slots [L, A]): the layout's rows there take no gradient either.

        The normalisers stay per layout. The layouts' own weights at the shared places are kept at zero
        (clear_shared_slices) though nothing reads them. A rollout copy and the export write the shared weights into
        the layouts' own (materialise_shared, materialised_state), so the dense rollout, the graphs and the .amdl
        are the plain per-layout maps they always were."""
        layouts = len(self.adapters)
        blind = {index: set(columns) for index, columns in (self.vision.blind.items() if self.vision is not None
                                                            else ())}
        obs = [list(columns) for columns in sharing["obs"]]
        actions = [list(slots) for slots in sharing["actions"]]
        if len(obs) != layouts or len(actions) != layouts:
            raise ValueError(f"mappo.shared_blocks: sharing for {len(obs)} layouts, the actor has {layouts}")
        # A column is shared where no layout's adapter is blind to it (the camera's are read by the encoders).
        keep = [offset for offset in range(len(obs[0]))
                if not any(obs[index][offset] in blind.get(index, ()) for index in range(layouts))]
        for index in range(layouts):
            if any(column >= self.obs_dims[index] for column in obs[index]) or \
                    any(slot >= self.action_counts[index] for slot in actions[index]):
                raise ValueError(f"mappo.shared_blocks: layout {index}'s shared columns or actions are past its "
                                 f"{self.obs_dims[index]} columns or {self.action_counts[index]} actions")
        device = self.adapters[0].weight.device
        self.register_buffer("shared_columns", torch.tensor([[obs[index][offset] for offset in keep]
                                                             for index in range(layouts)], dtype=torch.long,
                                                            device=device).reshape(layouts, len(keep)))
        self.register_buffer("shared_slots", torch.tensor(actions, dtype=torch.long,
                                                          device=device).reshape(layouts, -1))
        # Which layouts share (sharing_of's "on": every layout but the director); saved with the state, so a saved
        # state's materialisation (materialised_state) leaves the others' weights alone.
        on = [bool(flag) for flag in sharing.get("on") or [True] * layouts]
        self.shared_on_list = on
        self.register_buffer("shared_on", torch.tensor(on, dtype=torch.bool, device=device))
        self.shared_init = [int(index) for index in sharing.get("init") or range(layouts) if on[int(index)]]
        if keep:
            self.shared_adapter = nn.Linear(len(keep), width, bias=False, device=device)
            nn.init.orthogonal_(self.shared_adapter.weight, gain=math.sqrt(2))
        if self.shared_slots.shape[1]:
            self.shared_head = _linear(head_width, self.shared_slots.shape[1], 0.01).to(device)
        self.clear_shared_slices()

    @torch.no_grad()
    def clear_shared_slices(self) -> None:
        """Zero every layout's own adapter columns and head rows at the shared places (_attach_sharing)."""
        if self.shared_columns is None:
            return
        for index in range(len(self.adapters)):
            if not self.shared_on_list[index]:
                continue
            if self.shared_adapter is not None:
                self.adapters[index].weight[:, self.shared_columns[index]] = 0.0
            if self.shared_head is not None:
                self.heads[index].weight[self.shared_slots[index]] = 0.0
                self.heads[index].bias[self.shared_slots[index]] = 0.0

    @torch.no_grad()
    def share_from_layouts(self) -> None:
        """The shared weights as the mean of the layouts' own at the shared places, over the layouts shared_init names
        (mappo.shared_init_layouts) -- the head's rows matched by action name, the adapter's by column -- and those
        places then cleared: an actor trained without sharing carries on with it (a resume, a seed). Computed in
        double precision on the host, so every rank resuming from the same checkpoint starts from the same weights."""
        if self.shared_columns is None:
            return
        init = self.shared_init or list(range(len(self.adapters)))
        if self.shared_adapter is not None:
            mean = torch.stack([self.adapters[index].weight[:, self.shared_columns[index]].detach().double().cpu()
                                for index in init]).mean(dim=0)
            self.shared_adapter.weight.copy_(mean.to(self.shared_adapter.weight))
        if self.shared_head is not None:
            for part in ("weight", "bias"):
                mean = torch.stack([getattr(self.heads[index], part)[self.shared_slots[index]].detach().double().cpu()
                                    for index in init]).mean(dim=0)
                getattr(self.shared_head, part).copy_(mean.to(getattr(self.shared_head, part)))
        self.clear_shared_slices()

    @torch.no_grad()
    def materialise_shared(self) -> None:
        """Write the shared weights into every layout's own adapter columns and head rows and stop adding them
        (shared_folded): for the rollout copy only, before its normalisers are folded, every sync (the copy brings the
        layouts' cleared places back each time)."""
        if self.shared_columns is None:
            return
        for index in range(len(self.adapters)):
            if not self.shared_on_list[index]:
                continue
            if self.shared_adapter is not None:
                self.adapters[index].weight[:, self.shared_columns[index]] = self.shared_adapter.weight
            if self.shared_head is not None:
                self.heads[index].weight[self.shared_slots[index]] = self.shared_head.weight
                self.heads[index].bias[self.shared_slots[index]] = self.shared_head.bias
        self.shared_folded = True

    def _sharing_live(self) -> bool:
        """Whether the forward adds the shared weights (an actor with sharing, not a materialised rollout copy)."""
        return self.shared_columns is not None and not self.shared_folded

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
        overwritten from the trained network at every sync and folded again. The shared movement is written into the
        layouts' own weights first (materialise_shared): the normaliser folds into the whole adapter."""
        self.materialise_shared()
        for norm, adapter in zip(self.norms, self.adapters):
            fold_into(norm, adapter)

    def densify(self, obs_width: int) -> None:
        """Adapters and heads as DenseLayouts, after fold_normalisation: for a rollout copy on the GPU only. Once
        built they are refreshed in place (DenseLayouts.refresh)."""
        if self._sharing_live():
            raise RuntimeError("densify: the shared movement is not materialised (fold_normalisation first)")
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
        director = director_term(self, obs, layout)
        if director is not None:
            extra = director if extra is None else extra + director.to(extra.dtype)
        if self.dense_adapters is not None:
            hidden = self.dense_adapters(obs, layout)
            return self.trunk(hidden if extra is None else hidden + extra.to(hidden.dtype))
        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        width = self.adapters[0].out_features
        hidden = obs.new_zeros(obs.shape[0], width)
        shared = self.shared_adapter if self._sharing_live() else None
        for index, rows in groups:
            normalised = self.norms[index](obs[rows, : self.obs_dims[index]])
            if shared is None or not self.shared_on_list[index]:
                own = self.adapters[index](normalised)
            else:
                # The shared columns through the shared adapter, the rest through the layout's own (_attach_sharing).
                columns = self.shared_columns[index]
                own = self.adapters[index](normalised.index_fill(1, columns, 0.0)) + shared(
                    normalised.index_select(1, columns))
            # Cast on the way in: the rows' dtype is the adapters', the zeros' the observation's.
            hidden[rows] = own.to(hidden.dtype)
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
             deterministic: bool = False, greedy: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """The free look's choice for flat rows (LookHead.sample): (choice [N, heads], log probability [N]). `greedy`
        [N] bool: the rows whose camera takes the argmax too (a greedy env's whole decision is the argmax)."""
        return self.look_head.sample(self.policy_features(features, goal), layout, deterministic, greedy)

    def look_terms(self, features: torch.Tensor, layout: torch.Tensor, choice: torch.Tensor,
                   goal: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """The free look's (log probability [N], entropy [N]) of choices taken (LookHead.evaluate)."""
        return self.look_head.evaluate(self.policy_features(features, goal), layout, choice)

    def heading(self, features: torch.Tensor, layout: torch.Tensor, goal: torch.Tensor | None = None,
                deterministic: bool = False, greedy: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor]:
        """The heading for flat rows (HeadingHead.sample): (theta [N] float32, log probability [N]). `greedy` [N] bool:
        the rows that take the mode (a greedy env's whole decision is the argmax)."""
        return self.heading_head.sample(self.policy_features(features, goal), layout, deterministic, greedy)

    def heading_terms(self, features: torch.Tensor, layout: torch.Tensor, theta: torch.Tensor,
                      goal: torch.Tensor | None = None) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """The heading's (log probability [N], entropy surrogate [N], mean concentration [N]) of headings taken
        (HeadingHead.evaluate)."""
        return self.heading_head.evaluate(self.policy_features(features, goal), layout, theta)

    def action_logits(self, features: torch.Tensor, layout: torch.Tensor, mask: torch.Tensor,
                      goal: torch.Tensor | None = None, groups=None, obs: torch.Tensor | None = None) -> torch.Tensor:
        """action_distribution's masked logits, unnormalised (for sample_logits)."""
        features = self.policy_features(features, goal)

        if self.dense_heads is not None:
            dense = self.dense_heads
            own = torch.where(dense.valid[layout.long()], dense(features, layout), MASKED_LOGIT)
            logits = own if own.shape[-1] == mask.shape[-1] else nn.functional.pad(
                own, (0, mask.shape[-1] - own.shape[-1]), value=MASKED_LOGIT)
            return masked_logits(with_director_pointers(self, self._with_sight_pointers(logits, features, layout, obs),
                                                        features, layout, obs), mask)

        groups = groups if groups is not None else _per_layout(layout, len(self.adapters))
        logits = features.new_full((features.shape[0], mask.shape[-1]), MASKED_LOGIT)
        for index, rows in groups:
            logits[rows, : self.action_counts[index]] = self.heads[index](features[rows]).to(logits.dtype)
        if self.shared_head is not None and self._sharing_live():
            # The shared actions' logits over the layouts' own at each row's slots (_attach_sharing).
            flat = layout.reshape(-1).long()
            slots = self.shared_slots[flat]
            logits = torch.where(self.shared_on[flat][:, None],
                                 logits.scatter(1, slots, self.shared_head(features).to(logits.dtype)), logits)
        return masked_logits(with_director_pointers(self, self._with_sight_pointers(logits, features, layout, obs),
                                                    features, layout, obs), mask)

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
                     queue: torch.Tensor, clock: torch.Tensor, deterministic: bool,
                     uniform: bool = False, image: torch.Tensor | None = None, plan: torch.Tensor | None = None,
                     serial: torch.Tensor | None = None, uniform_cells: bool = False,
                     single: bool = False, bonus: torch.Tensor | None = None) -> dict[str, torch.Tensor]:
        """**The goal decision of one step** (next-run plan, Wave 4), the same in the rollout, its captured graph,
        the teachers and the module's runtime. Flat rows; `held` the goal (pair) held, `queue` [rows, S - 2] the goals
        waiting (-1 none), `clock` the goal clock's choices. In order:

        - an ended secondary is dropped (the sim dropped it too);
        - an ended primary with a queue is replaced by the queue's head, with no choice made (the plan that queued
          it keeps the credit);
        - a choice is made on the clock, where the primary ended with nothing queued, or on the block's event.

        Returns the pair held now (`goal`), the queue, which rows chose (`chosen`), their log probability (0 where
        none), the slots drawn [rows, S] (what the slow update scores) and the observed signals. `uniform`: draw the
        primary uniformly over the goals on offer (the eval arm random_goal), a host flag the training graph never
        sets; `uniform_cells` and `single` the eval arms random_cell and no_plan (GoalHead.draw).

        **The plan of cells** (free-choice-goals, with the cell head and `image`, the rows' image bytes): `plan` [rows, 4]
        are the cell words the seat holds at the four positions (0 none; ticket << CELL_BITS | block + 1) and `serial`
        [rows] the choices it has made this episode. The words follow the goals as the goals move -- an ended secondary
        loses its word, a promotion shifts the queue's up -- and a choice writes the words of the cells it drew, each
        with a ticket of its own (the sim latches a cell once, at the ticket's first sight, in the frame of the
        observation it was drawn from). Returned as `cells` [rows, S] (the blocks drawn, -1 none) and `plan`. `bonus`
        [rows, B]: the planner's pooled value per block (MapEncoder.plan's "pool"), the cell pointer's bonus."""
        head = self.goal_head
        rows = features.shape[0]
        signals = head.signals(obs, layout)
        if head.slots <= 1:
            chosen = clock | signals["ended"]
            logits = head.logits(features, obs, layout)
            if uniform:
                logits = uniform_logits(logits)
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

        slots, log_prob, _, cells = head.draw(features, obs, layout, deterministic, uniform=uniform, image=image,
                                              uniform_cells=uniform_cells, single=single, bonus=bonus)
        primary = torch.where(chosen, slots[:, 0], primary)
        secondary = torch.where(chosen, slots[:, 1], secondary)
        queue = torch.where(chosen[:, None], slots[:, 2:], queue)
        # Two cell goals share a joint id (head.cell_joint) and differ by their cell: only another goal repeated is dropped.
        secondary = torch.where((secondary == primary) & (primary != head.cell_joint),
                                torch.full_like(secondary, -1), secondary)
        out = {"goal": goal_pair(primary, secondary, head.count), "queue": queue, "chosen": chosen,
               "log_prob": torch.where(chosen, log_prob, torch.zeros_like(log_prob)), "slots": slots, "cells": cells,
               **signals}
        if plan is not None and serial is not None and head.cell_on and image is not None:
            out["plan"] = self._plan_words(plan, serial, cells, signals["secondary_ended"], promote, chosen)
        return out

    def _plan_words(self, plan: torch.Tensor, serial: torch.Tensor, cells: torch.Tensor,
                    secondary_ended: torch.Tensor, promote: torch.Tensor, chosen: torch.Tensor) -> torch.Tensor:
        """The cell words held at the four plan positions [rows, 4] after a decision (see decide_goals)."""
        first, second, third, fourth = plan.unbind(dim=-1)
        none = torch.zeros_like(first)
        second = torch.where(secondary_ended, none, second)
        first, third, fourth = (torch.where(promote, third, first), torch.where(promote, fourth, third),
                                torch.where(promote, none, fourth))
        held = torch.stack([first, second, third, fourth], dim=-1)
        positions = min(cells.shape[-1], plan.shape[-1])
        fresh = []
        for position in range(plan.shape[-1]):
            if position >= positions:
                fresh.append(none)
                continue
            ticket = ((serial + 1) * plan.shape[-1] + position) % TICKET_COUNT + 1
            block = cells[:, position]
            fresh.append(torch.where(block >= 0, (ticket << CELL_BITS) | (block + 1), none))
        return torch.where(chosen[:, None], torch.stack(fresh, dim=-1), held)

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
                 goal_slots: int = 1, vision=None, director=None,
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
        # The director's sets (DirectorSets): the critic reads them as the actor does, with no pointer heads.
        attach_director(self, director)

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
        director = director_term(self, obs, layout)
        if director is not None:
            own = own + director.to(own.dtype)
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
